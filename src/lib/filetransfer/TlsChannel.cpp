/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "filetransfer/TlsChannel.h"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "base/Log.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <mutex>

namespace deskflow::filetransfer {

namespace {

constexpr int kHandshakeTimeoutMs = 15000;
constexpr uint8_t kBackgroundTrafficClass = 0x20; // DSCP CS1, "lower effort than best effort"

#if defined(_WIN32)
using SocketHandle = SOCKET;
const SocketHandle kInvalidSocket = INVALID_SOCKET;

void closeSocket(SocketHandle socket)
{
  closesocket(socket);
}

void ensureNetwork()
{
  // Deskflow starts Winsock itself; this covers tools and tests that use the file connection alone
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}
#else
using SocketHandle = int;
const SocketHandle kInvalidSocket = -1;

void closeSocket(SocketHandle socket)
{
  ::close(socket);
}

void ensureNetwork()
{
}
#endif

SocketHandle handle(intptr_t socket)
{
  return static_cast<SocketHandle>(socket);
}

std::string openSslError()
{
  const unsigned long code = ERR_get_error();
  ERR_clear_error();
  if (code == 0) {
    return "connection closed";
  }
  char text[256];
  ERR_error_string_n(code, text, sizeof(text));
  return text;
}

// Certificates are self-signed; the peer is checked by fingerprint after the handshake
int acceptAnyCertificate(int, X509_STORE_CTX *)
{
  return 1;
}

void setSocketTimeout(SocketHandle socket, int option, int milliseconds)
{
#if defined(_WIN32)
  const DWORD value = static_cast<DWORD>(milliseconds);
  setsockopt(socket, SOL_SOCKET, option, reinterpret_cast<const char *>(&value), sizeof(value));
#else
  timeval value{};
  value.tv_sec = milliseconds / 1000;
  value.tv_usec = (milliseconds % 1000) * 1000;
  setsockopt(socket, SOL_SOCKET, option, &value, sizeof(value));
#endif
}

void configureSocket(SocketHandle socket)
{
  int on = 1;
  setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
#if defined(SO_NOSIGPIPE)
  setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
  setSocketTimeout(socket, SO_RCVTIMEO, kHandshakeTimeoutMs);
  setSocketTimeout(socket, SO_SNDTIMEO, kHandshakeTimeoutMs);
}

std::string addressText(const sockaddr *address, socklen_t length)
{
  char host[NI_MAXHOST] = {};
  char port[NI_MAXSERV] = {};
  if (getnameinfo(address, length, host, sizeof(host), port, sizeof(port), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "unknown";
  }
  return std::string(host) + " port " + port;
}

SSL_CTX *createContext(bool server, const TlsSettings &settings, std::string &error)
{
  SSL_CTX *context = SSL_CTX_new(server ? TLS_server_method() : TLS_client_method());
  if (context == nullptr) {
    error = "can't create a TLS context: " + openSslError();
    return nullptr;
  }
  SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
  const auto &path = settings.certificatePath;
  if (SSL_CTX_use_certificate_file(context, path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_use_PrivateKey_file(context, path.c_str(), SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_check_private_key(context) != 1) {
    error = "can't load the certificate " + path + ": " + openSslError();
    SSL_CTX_free(context);
    return nullptr;
  }
  SSL_CTX_set_verify(context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, acceptAnyCertificate);
  return context;
}

bool peerIsTrusted(SSL *ssl, const TlsSettings &settings, std::string &error)
{
  X509 *certificate = SSL_get1_peer_certificate(ssl);
  if (certificate == nullptr) {
    error = "the peer sent no certificate";
    return false;
  }
  const auto fingerprint = sslCertFingerprint(certificate, QCryptographicHash::Sha256);
  X509_free(certificate);

  if (!settings.checkPeers) {
    return true;
  }

  FingerprintDatabase trusted;
  trusted.read(QString::fromStdString(settings.trustedPeersPath));
  if (!trusted.isTrusted(fingerprint)) {
    error = "the peer's certificate isn't trusted (" + formatSSLFingerprint(fingerprint.data).toStdString() + ")";
    return false;
  }
  return true;
}

} // namespace

//
// TlsChannel
//

TlsChannel::TlsChannel(intptr_t socket, SSL_CTX *context, SSL *ssl, std::string peerName)
    : m_socket(socket),
      m_context(context),
      m_ssl(ssl),
      m_peerName(std::move(peerName))
{
}

TlsChannel::~TlsChannel()
{
  if (m_ssl != nullptr) {
    if (!m_broken) {
      SSL_shutdown(m_ssl);
    }
    SSL_free(m_ssl);
  }
  if (m_context != nullptr) {
    SSL_CTX_free(m_context);
  }
  if (!m_socketClosed.exchange(true)) {
    closeSocket(handle(m_socket));
  }
}

std::unique_ptr<TlsChannel> TlsChannel::handshake(
    intptr_t socket, bool server, const TlsSettings &settings, std::string peerName, std::string &error
)
{
  SSL_CTX *context = createContext(server, settings, error);
  if (context == nullptr) {
    closeSocket(handle(socket));
    return nullptr;
  }

  SSL *ssl = SSL_new(context);
  // OpenSSL takes socket handles as int, Windows ones included
  SSL_set_fd(ssl, static_cast<int>(socket));
  auto channel = std::unique_ptr<TlsChannel>(new TlsChannel(socket, context, ssl, std::move(peerName)));

  const int result = server ? SSL_accept(ssl) : SSL_connect(ssl);
  if (result != 1) {
    error = "TLS handshake with " + channel->peerName() + " failed: " + openSslError();
    channel->m_broken = true;
    return nullptr;
  }
  if (!peerIsTrusted(ssl, settings, error)) {
    error = channel->peerName() + ": " + error;
    return nullptr;
  }
  return channel;
}

std::unique_ptr<TlsChannel>
TlsChannel::connect(const std::string &host, uint16_t port, const TlsSettings &settings, std::string &error)
{
  ensureNetwork();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo *addresses = nullptr;
  const auto service = std::to_string(port);
  if (getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses) != 0 || addresses == nullptr) {
    error = "can't resolve " + host;
    return nullptr;
  }

  SocketHandle socket = kInvalidSocket;
  std::string peerName;
  for (const addrinfo *address = addresses; address != nullptr; address = address->ai_next) {
    socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (socket == kInvalidSocket) {
      continue;
    }
    configureSocket(socket);
    const auto length = static_cast<socklen_t>(address->ai_addrlen);
    if (::connect(socket, address->ai_addr, length) == 0) {
      peerName = addressText(address->ai_addr, length);
      break;
    }
    closeSocket(socket);
    socket = kInvalidSocket;
  }
  freeaddrinfo(addresses);

  if (socket == kInvalidSocket) {
    error = "can't connect to " + host + " port " + service;
    return nullptr;
  }
  return handshake(static_cast<intptr_t>(socket), false, settings, peerName, error);
}

std::unique_ptr<TlsChannel> TlsChannel::accept(intptr_t socket, const TlsSettings &settings, std::string &error)
{
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  std::string peerName = "unknown";
  if (getpeername(handle(socket), reinterpret_cast<sockaddr *>(&address), &length) == 0) {
    peerName = addressText(reinterpret_cast<sockaddr *>(&address), length);
  }
  configureSocket(handle(socket));
  return handshake(socket, true, settings, peerName, error);
}

void TlsChannel::setTimeouts(int receiveMilliseconds, int sendMilliseconds)
{
  setSocketTimeout(handle(m_socket), SO_RCVTIMEO, receiveMilliseconds);
  setSocketTimeout(handle(m_socket), SO_SNDTIMEO, sendMilliseconds);
}

void TlsChannel::setBulk()
{
  sockaddr_storage address{};
  socklen_t length = sizeof(address);
  if (getsockname(handle(m_socket), reinterpret_cast<sockaddr *>(&address), &length) != 0) {
    return;
  }
  const int value = kBackgroundTrafficClass;
  if (address.ss_family == AF_INET6) {
    setsockopt(handle(m_socket), IPPROTO_IPV6, IPV6_TCLASS, reinterpret_cast<const char *>(&value), sizeof(value));
  } else {
    setsockopt(handle(m_socket), IPPROTO_IP, IP_TOS, reinterpret_cast<const char *>(&value), sizeof(value));
  }
}

void TlsChannel::interrupt()
{
  m_broken = true;
#if defined(_WIN32)
  // On Windows, shutdown() doesn't wake a thread blocked in recv() on the socket; closing it does.
  // The owning thread then only sees failed reads, and the destructor doesn't close it again.
  if (!m_socketClosed.exchange(true)) {
    shutdown(handle(m_socket), SD_BOTH);
    closeSocket(handle(m_socket));
  }
#else
  shutdown(handle(m_socket), SHUT_RDWR);
#endif
}

bool TlsChannel::send(wire::Type type, const std::string &payload)
{
  return send(type, payload.data(), payload.size());
}

bool TlsChannel::send(wire::Type type, const char *payload, size_t size)
{
  if (size + 1 > wire::kMaxFrameSize) {
    return false;
  }
  wire::Writer frame;
  frame.u32(static_cast<uint32_t>(size + 1)).u8(static_cast<uint8_t>(type));

  // small frames go out in one record
  if (size < 16 * 1024) {
    frame.raw(payload, size);
    return writeAll(frame.data().data(), frame.data().size());
  }
  return writeAll(frame.data().data(), frame.data().size()) && writeAll(payload, size);
}

std::optional<Frame> TlsChannel::receive()
{
  char header[5];
  if (!readExact(header, sizeof(header))) {
    return std::nullopt;
  }
  const auto byte = [&header](int i) { return static_cast<uint32_t>(static_cast<uint8_t>(header[i])); };
  const uint32_t length = (byte(0) << 24) | (byte(1) << 16) | (byte(2) << 8) | byte(3);
  if (length < 1 || length > wire::kMaxFrameSize) {
    LOG_WARN("file connection: bad frame from %s", m_peerName.c_str());
    m_broken = true;
    return std::nullopt;
  }

  Frame frame{static_cast<wire::Type>(static_cast<uint8_t>(header[4])), std::string(length - 1, '\0')};
  if (length > 1 && !readExact(frame.payload.data(), frame.payload.size())) {
    return std::nullopt;
  }
  return frame;
}

bool TlsChannel::readExact(char *data, size_t size)
{
  while (size > 0) {
    if (m_broken) {
      return false;
    }
    const int read = SSL_read(m_ssl, data, static_cast<int>(std::min<size_t>(size, 1 << 30)));
    if (read <= 0) {
      // closed by the peer, timed out or failed: the stream can't be resumed either way
      m_broken = true;
      ERR_clear_error();
      return false;
    }
    data += read;
    size -= static_cast<size_t>(read);
  }
  return true;
}

bool TlsChannel::writeAll(const char *data, size_t size)
{
  while (size > 0) {
    if (m_broken) {
      return false;
    }
    const int written = SSL_write(m_ssl, data, static_cast<int>(std::min<size_t>(size, 1 << 30)));
    if (written <= 0) {
      m_broken = true;
      ERR_clear_error();
      return false;
    }
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

//
// TlsListener
//

TlsListener::TlsListener(intptr_t socket, uint16_t port) : m_socket(socket), m_port(port)
{
}

TlsListener::~TlsListener()
{
  closeSocket(handle(m_socket));
}

std::unique_ptr<TlsListener> TlsListener::listen(uint16_t port, std::string &error)
{
  ensureNetwork();

  // one dual-stack socket takes both IPv6 and IPv4; plain IPv4 where IPv6 isn't available
  SocketHandle socket = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  const bool ipv6 = socket != kInvalidSocket;
  if (!ipv6) {
    socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  }
  if (socket == kInvalidSocket) {
    error = "can't create a socket";
    return nullptr;
  }

  int on = 1;
  int off = 0;
#if defined(_WIN32)
  setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&on), sizeof(on));
#else
  setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

  int bound = -1;
  if (ipv6) {
    setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char *>(&off), sizeof(off));
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_addr = in6addr_any;
    address.sin6_port = htons(port);
    bound = bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  } else {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    bound = bind(socket, reinterpret_cast<const sockaddr *>(&address), sizeof(address));
  }

  if (bound != 0 || ::listen(socket, 8) != 0) {
    error = "can't listen on port " + std::to_string(port);
    closeSocket(socket);
    return nullptr;
  }

  sockaddr_storage local{};
  socklen_t length = sizeof(local);
  uint16_t actualPort = port;
  if (getsockname(socket, reinterpret_cast<sockaddr *>(&local), &length) == 0) {
    actualPort = ntohs(
        local.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6 *>(&local)->sin6_port
                                    : reinterpret_cast<sockaddr_in *>(&local)->sin_port
    );
  }
  return std::unique_ptr<TlsListener>(new TlsListener(static_cast<intptr_t>(socket), actualPort));
}

void TlsListener::discard(intptr_t socket)
{
  closeSocket(handle(socket));
}

intptr_t TlsListener::acceptSocket(int timeoutMilliseconds)
{
  fd_set readable;
  FD_ZERO(&readable);
  FD_SET(handle(m_socket), &readable);
  timeval timeout{};
  timeout.tv_sec = timeoutMilliseconds / 1000;
  timeout.tv_usec = (timeoutMilliseconds % 1000) * 1000;
  // the first argument is ignored on Windows
  const int ready = select(static_cast<int>(handle(m_socket)) + 1, &readable, nullptr, nullptr, &timeout);
  if (ready <= 0) {
    return -1;
  }

  const SocketHandle socket = ::accept(handle(m_socket), nullptr, nullptr);
  if (socket == kInvalidSocket) {
    return -1;
  }
  return static_cast<intptr_t>(socket);
}

} // namespace deskflow::filetransfer
