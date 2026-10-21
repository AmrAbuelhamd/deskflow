/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "filetransfer/Wire.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

using SSL = struct ssl_st;
using SSL_CTX = struct ssl_ctx_st;

namespace deskflow::filetransfer {

//! Who this computer is and whom it trusts, as for the connection that carries input
struct TlsSettings
{
  std::string certificatePath;  //!< PEM file holding this computer's certificate and private key
  std::string trustedPeersPath; //!< fingerprints of the computers allowed on the other end
  bool checkPeers = true;       //!< when false, any certificate is accepted (still encrypted)
};

struct Frame
{
  wire::Type type;
  std::string payload;
};

//! One TLS connection with blocking reads and writes, used by one thread at a time.
//! Another thread may only call interrupt(), to unblock it.
class TlsChannel
{
public:
  ~TlsChannel();
  TlsChannel(TlsChannel const &) = delete;
  TlsChannel &operator=(TlsChannel const &) = delete;

  //! Connects, completes the TLS handshake and checks the peer's fingerprint
  static std::unique_ptr<TlsChannel>
  connect(const std::string &host, uint16_t port, const TlsSettings &settings, std::string &error);

  //! Completes the TLS handshake on a socket from TlsListener::acceptSocket() and checks the
  //! peer's fingerprint. Takes ownership of the socket, even on failure.
  static std::unique_ptr<TlsChannel> accept(intptr_t socket, const TlsSettings &settings, std::string &error);

  bool send(wire::Type type, const std::string &payload = {});
  bool send(wire::Type type, const char *payload, size_t size);

  //! Nothing once the connection is closed, broken, or the timeout passed
  std::optional<Frame> receive();

  void setTimeouts(int receiveMilliseconds, int sendMilliseconds);

  //! Marks the traffic as background, so input on other connections goes first when the link is busy
  void setBulk();

  //! Unblocks a receive() waiting on another thread; the connection can't be used afterwards
  void interrupt();

  const std::string &peerName() const
  {
    return m_peerName;
  }

private:
  friend class TlsListener;
  TlsChannel(intptr_t socket, SSL_CTX *context, SSL *ssl, std::string peerName);

  static std::unique_ptr<TlsChannel>
  handshake(intptr_t socket, bool server, const TlsSettings &settings, std::string peerName, std::string &error);

  bool readExact(char *data, size_t size);
  bool writeAll(const char *data, size_t size);

  intptr_t m_socket;
  SSL_CTX *m_context;
  SSL *m_ssl;
  std::string m_peerName;
  std::atomic<bool> m_broken{false};
  std::atomic<bool> m_socketClosed{false};
};

//! Accepts connections on a port
class TlsListener
{
public:
  ~TlsListener();
  TlsListener(TlsListener const &) = delete;
  TlsListener &operator=(TlsListener const &) = delete;

  //! Listens on every address; port 0 picks a free port
  static std::unique_ptr<TlsListener> listen(uint16_t port, std::string &error);

  uint16_t port() const
  {
    return m_port;
  }

  //! Waits up to the timeout for a connection and returns its socket, or -1.
  //! The handshake is left to TlsChannel::accept(), so a slow peer can't hold up the listener.
  intptr_t acceptSocket(int timeoutMilliseconds);

  //! Closes a socket from acceptSocket() without using it
  static void discard(intptr_t socket);

private:
  TlsListener(intptr_t socket, uint16_t port);

  intptr_t m_socket;
  uint16_t m_port;
};

} // namespace deskflow::filetransfer
