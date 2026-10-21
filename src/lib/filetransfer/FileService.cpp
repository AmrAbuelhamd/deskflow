/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "filetransfer/FileService.h"

#include "base/Log.h"

#include <QRandomGenerator>

#include <algorithm>
#include <chrono>
#include <fstream>

using namespace std::chrono_literals;

namespace deskflow::filetransfer {

namespace {

constexpr int kAcceptPollMs = 500;
constexpr int kMaxWorkers = 32;
constexpr auto kPingInterval = 15s;
constexpr int kPongTimeoutMs = 10000;
constexpr int kControlReceiveMs = 45000; // three missed pings
constexpr int kIoTimeoutMs = 60000;
constexpr int kServeIdleMs = 60000;
constexpr auto kDialTimeout = 10s;
constexpr size_t kMaxIdlePerPeer = 4;
constexpr auto kMaxIdleAge = 30s; // well under the serving side's idle timeout
constexpr size_t kMaxListEntries = 200000;
constexpr size_t kMaxListBytes = 64 * 1024 * 1024;

std::string failure(const Frame &frame)
{
  wire::Reader reader(frame.payload);
  std::string reason;
  reader.str(reason);
  return reason.empty() ? "the other computer couldn't do it" : reason;
}

} // namespace

//
// RemoteFile
//

RemoteFile::RemoteFile(FileService &service, std::string peer, std::unique_ptr<TlsChannel> channel)
    : m_service(service),
      m_peer(std::move(peer)),
      m_channel(std::move(channel))
{
  m_service.track(m_channel.get());
}

RemoteFile::~RemoteFile()
{
  // a channel left mid-file can't be reused; it closes with this object
  letGo(false);
}

// Gives the channel back for reuse, or closes it
void RemoteFile::letGo(bool reusable)
{
  if (m_channel == nullptr) {
    return;
  }
  if (reusable) {
    m_service.release(m_peer, std::move(m_channel));
  } else {
    m_service.untrack(m_channel.get());
    m_channel.reset();
  }
}

// Sends the request and waits for the reply to start, so that a reused connection the other side
// has closed shows up now, while another one can still be tried
bool RemoteFile::start(const std::string &request)
{
  if (!m_channel->send(wire::Type::Read, request)) {
    letGo(false);
    return false;
  }
  auto frame = m_channel->receive();
  if (!frame) {
    letGo(false);
    return false;
  }
  take(std::move(*frame)); // a refusal is reported by read()
  return true;
}

// Handles a frame of the reply; false if the read failed
bool RemoteFile::take(Frame frame)
{
  switch (frame.type) {
  case wire::Type::Data:
    m_chunk = std::move(frame.payload);
    m_chunkOffset = 0;
    return true;
  case wire::Type::ReadEnd:
    m_done = true;
    letGo(true);
    return true;
  case wire::Type::Failed:
    m_error = failure(frame);
    letGo(true);
    return false;
  default:
    m_error = "unexpected reply";
    letGo(false);
    return false;
  }
}

bool RemoteFile::read(char *buffer, size_t size, size_t &count)
{
  count = 0;
  while (count < size) {
    if (m_chunkOffset < m_chunk.size()) {
      const auto part = std::min(size - count, m_chunk.size() - m_chunkOffset);
      std::copy_n(m_chunk.data() + m_chunkOffset, part, buffer + count);
      m_chunkOffset += part;
      count += part;
      continue;
    }
    if (m_done) {
      break;
    }
    if (!m_error.empty() || m_channel == nullptr) {
      return false;
    }

    auto frame = m_channel->receive();
    if (!frame) {
      m_error = "the connection was lost";
      letGo(false);
      return false;
    }
    if (!take(std::move(*frame))) {
      return false;
    }
  }
  return true;
}

//
// FileService
//

FileService &FileService::instance()
{
  // never destroyed: detached serving threads may still use it while the process exits
  static auto *s_instance = new FileService();
  return *s_instance;
}

FileService::~FileService()
{
  stop();
  // serving threads end once their connections close; stop() interrupted them all
  for (int i = 0; i < 500 && m_workers > 0; ++i) {
    std::this_thread::sleep_for(10ms);
  }
}

bool FileService::isServer() const
{
  std::scoped_lock lock(m_mutex);
  return m_server;
}

bool FileService::isConnected(const std::string &peer) const
{
  std::scoped_lock lock(m_mutex);
  if (m_server) {
    return m_controls.find(peer) != m_controls.end();
  }
  return m_clientControl != nullptr;
}

uint16_t FileService::port() const
{
  std::scoped_lock lock(m_mutex);
  return m_port;
}

wire::Token FileService::token() const
{
  std::scoped_lock lock(m_mutex);
  return m_token;
}

void FileService::setAccessGuard(AccessGuard guard)
{
  std::scoped_lock lock(m_mutex);
  m_guard = std::move(guard);
}

bool FileService::startServer(const TlsSettings &tls, uint16_t port, std::string &error)
{
  stop();

  auto listener = TlsListener::listen(port, error);
  if (!listener && port != 0) {
    LOG_WARN("file connection: %s, using any free port", error.c_str());
    listener = TlsListener::listen(0, error);
  }
  if (!listener) {
    return false;
  }

  {
    std::scoped_lock lock(m_mutex);
    m_stopping = false;
    m_server = true;
    m_tls = tls;
    m_port = listener->port();
    // The token travels as option values, which a client may misread as option ids
    // (4 ASCII characters); a high bit in every word keeps it from looking like one.
    for (auto &word : m_token) {
      word = QRandomGenerator::system()->generate() | 0x80000000u;
    }
    m_listener = std::move(listener);
  }
  m_acceptThread = std::thread([this] { acceptLoop(); });
  LOG_INFO("file connection: listening on port %u", static_cast<unsigned>(this->port()));
  return true;
}

void FileService::startClient(
    const TlsSettings &tls, const std::string &host, uint16_t port, const wire::Token &token,
    const std::string &computerName
)
{
  {
    std::scoped_lock lock(m_mutex);
    if (m_client && m_host == host && m_port == port && m_token == token) {
      return; // already connected to this server
    }
  }
  stop();

  uint64_t generation = 0;
  {
    std::scoped_lock lock(m_mutex);
    m_stopping = false;
    m_client = true;
    m_tls = tls;
    m_host = host;
    m_port = port;
    m_token = token;
    m_name = computerName;
    generation = ++m_generation;
  }
  // not joined: stopping must never wait for a connection attempt, as it runs on Deskflow's event thread
  ++m_workers;
  std::thread([this, generation] {
    clientControlLoop(generation);
    --m_workers;
  }).detach();
}

bool FileService::isCurrent(uint64_t generation) const
{
  return !m_stopping && m_generation == generation;
}

void FileService::stop()
{
  std::vector<std::shared_ptr<Control>> controls;
  {
    std::scoped_lock lock(m_mutex);
    if (!m_server && !m_client) {
      return;
    }
    m_stopping = true;
    ++m_generation;
    if (m_clientControl != nullptr) {
      m_clientControl->interrupt();
    }
    for (auto &[peer, control] : m_controls) {
      controls.push_back(control);
    }
    for (auto *channel : m_serving) {
      channel->interrupt();
    }
    for (auto *channel : m_fetching) {
      channel->interrupt();
    }
    m_idle.clear();
  }
  m_stopWake.notify_all();
  for (const auto &control : controls) {
    std::scoped_lock lock(control->mutex);
    control->closed = true;
    if (control->channel != nullptr) {
      control->channel->interrupt();
    }
    control->wake.notify_all();
  }

  // returns within one poll interval
  if (m_acceptThread.joinable()) {
    m_acceptThread.join();
  }

  std::scoped_lock lock(m_mutex);
  m_listener.reset();
  m_controls.clear();
  m_pendingDials.clear();
  m_server = false;
  m_client = false;
  m_port = 0;
}

//
// server side
//

void FileService::acceptLoop()
{
  while (!m_stopping) {
    const auto socket = m_listener->acceptSocket(kAcceptPollMs);
    if (socket < 0) {
      continue;
    }
    if (m_workers >= kMaxWorkers) {
      LOG_WARN("file connection: too many connections, refusing one");
      TlsListener::discard(socket);
      continue;
    }
    ++m_workers;
    std::thread([this, socket] {
      handleIncoming(socket);
      --m_workers;
    }).detach();
  }
}

void FileService::handleIncoming(intptr_t socket)
{
  TlsSettings tls;
  wire::Token token;
  {
    std::scoped_lock lock(m_mutex);
    tls = m_tls;
    token = m_token;
  }

  std::string error;
  auto channel = TlsChannel::accept(socket, tls, error);
  if (!channel) {
    LOG_WARN("file connection refused: %s", error.c_str());
    return;
  }

  auto frame = channel->receive();
  if (!frame || frame->type != wire::Type::Hello) {
    LOG_WARN("file connection from %s: no greeting", channel->peerName().c_str());
    return;
  }

  wire::Reader reader(frame->payload);
  uint16_t version = 0;
  uint8_t role = 0;
  wire::Token theirs{};
  uint32_t requestId = 0;
  std::string peer;
  bool ok = reader.u16(version) && reader.u8(role);
  for (auto &word : theirs) {
    ok = ok && reader.u32(word);
  }
  ok = ok && reader.u32(requestId) && reader.str(peer);
  if (!ok || version < 1 || theirs != token) {
    LOG_WARN("file connection from %s: wrong session", channel->peerName().c_str());
    channel->send(wire::Type::Refused, wire::Writer().str("wrong session").data());
    return;
  }
  if (!channel->send(wire::Type::Welcome)) {
    return;
  }

  switch (static_cast<wire::Role>(role)) {
  case wire::Role::Control:
    runServerControl(peer, std::move(channel));
    break;
  case wire::Role::Requester:
    channel->setBulk();
    channel->setTimeouts(kServeIdleMs, kIoTimeoutMs);
    serve(*channel);
    break;
  case wire::Role::ServeRequest:
    channel->setBulk();
    channel->setTimeouts(kIoTimeoutMs, kIoTimeoutMs);
    deliverDial(requestId, std::move(channel));
    break;
  default:
    LOG_WARN("file connection from %s: unknown role %u", peer.c_str(), static_cast<unsigned>(role));
  }
}

void FileService::runServerControl(const std::string &peer, std::unique_ptr<TlsChannel> channel)
{
  auto control = std::make_shared<Control>();
  control->channel = channel.get();
  {
    std::scoped_lock lock(m_mutex);
    if (const auto old = m_controls.find(peer); old != m_controls.end()) {
      std::scoped_lock oldLock(old->second->mutex);
      old->second->closed = true;
      old->second->wake.notify_all();
    }
    m_controls[peer] = control;
  }
  channel->setTimeouts(kPongTimeoutMs, kPongTimeoutMs);
  LOG_INFO("file connection: %s connected", peer.c_str());

  while (!m_stopping) {
    std::unique_lock lock(control->mutex);
    control->wake.wait_for(lock, kPingInterval, [&control, this] {
      return control->closed || m_stopping || !control->dials.empty();
    });
    if (control->closed || m_stopping) {
      break;
    }
    if (!control->dials.empty()) {
      const auto id = control->dials.front();
      control->dials.pop_front();
      lock.unlock();
      if (!channel->send(wire::Type::Dial, wire::Writer().u32(id).data())) {
        break;
      }
      continue;
    }
    lock.unlock();

    if (!channel->send(wire::Type::Ping)) {
      break;
    }
    const auto pong = channel->receive();
    if (!pong || pong->type != wire::Type::Pong) {
      break;
    }
  }

  {
    std::scoped_lock lock(m_mutex, control->mutex);
    control->closed = true;
    control->channel = nullptr;
    if (const auto it = m_controls.find(peer); it != m_controls.end() && it->second == control) {
      m_controls.erase(it);
    }
  }
  LOG_INFO("file connection: %s disconnected", peer.c_str());
}

void FileService::deliverDial(uint32_t id, std::unique_ptr<TlsChannel> channel)
{
  std::scoped_lock lock(m_mutex);
  const auto it = m_pendingDials.find(id);
  if (it == m_pendingDials.end()) {
    return; // the request gave up waiting
  }
  it->second.set_value(std::move(channel));
  m_pendingDials.erase(it);
}

std::unique_ptr<TlsChannel> FileService::dial(const std::string &peer, std::string &error)
{
  std::shared_ptr<Control> control;
  uint32_t id = 0;
  std::future<std::unique_ptr<TlsChannel>> answer;
  {
    std::scoped_lock lock(m_mutex);
    const auto it = m_controls.find(peer);
    if (it == m_controls.end()) {
      error = peer + " isn't connected for files";
      return nullptr;
    }
    control = it->second;
    id = m_nextDial++;
    answer = m_pendingDials[id].get_future();
  }
  {
    std::scoped_lock lock(control->mutex);
    control->dials.push_back(id);
  }
  control->wake.notify_all();

  if (answer.wait_for(kDialTimeout) != std::future_status::ready) {
    std::scoped_lock lock(m_mutex);
    m_pendingDials.erase(id);
    error = peer + " didn't answer";
    return nullptr;
  }
  return answer.get();
}

//
// client side
//

bool FileService::hello(TlsChannel &channel, wire::Role role, uint32_t requestId, std::string &error) const
{
  wire::Writer writer;
  {
    std::scoped_lock lock(m_mutex);
    writer.u16(wire::kVersion).u8(static_cast<uint8_t>(role));
    for (const auto word : m_token) {
      writer.u32(word);
    }
    writer.u32(requestId).str(m_name);
  }
  if (!channel.send(wire::Type::Hello, writer.data())) {
    error = "the connection was lost";
    return false;
  }
  const auto reply = channel.receive();
  if (!reply) {
    error = "no answer";
    return false;
  }
  if (reply->type != wire::Type::Welcome) {
    error = reply->type == wire::Type::Refused ? failure(*reply) : "unexpected answer";
    return false;
  }
  return true;
}

void FileService::clientControlLoop(uint64_t generation)
{
  auto wait = 1s;
  std::string lastError;
  while (isCurrent(generation)) {
    TlsSettings tls;
    std::string host;
    uint16_t port = 0;
    {
      std::scoped_lock lock(m_mutex);
      tls = m_tls;
      host = m_host;
      port = m_port;
    }

    std::string error;
    auto channel = TlsChannel::connect(host, port, tls, error);
    if (!isCurrent(generation)) {
      return; // stopped or restarted while connecting
    }
    if (channel && hello(*channel, wire::Role::Control, 0, error)) {
      wait = 1s;
      lastError.clear();
      LOG_INFO("file connection: connected to %s", channel->peerName().c_str());
      channel->setTimeouts(kControlReceiveMs, kPongTimeoutMs);
      {
        std::scoped_lock lock(m_mutex);
        if (!isCurrent(generation)) {
          return;
        }
        m_clientControl = channel.get();
      }

      while (isCurrent(generation)) {
        const auto frame = channel->receive();
        if (!frame) {
          break;
        }
        if (frame->type == wire::Type::Ping) {
          if (!channel->send(wire::Type::Pong)) {
            break;
          }
        } else if (frame->type == wire::Type::Dial) {
          wire::Reader reader(frame->payload);
          uint32_t id = 0;
          if (reader.u32(id)) {
            ++m_workers;
            std::thread([this, id] {
              answerDial(id);
              --m_workers;
            }).detach();
          }
        } else {
          break;
        }
      }

      {
        std::scoped_lock lock(m_mutex);
        if (m_clientControl == channel.get()) {
          m_clientControl = nullptr;
        }
      }
      if (isCurrent(generation)) {
        LOG_INFO("file connection: lost, reconnecting");
      }
    } else if (error != lastError) {
      LOG_WARN("file connection: %s", error.c_str());
      lastError = error;
    }

    std::unique_lock lock(m_mutex);
    m_stopWake.wait_for(lock, wait, [this, generation] { return !isCurrent(generation); });
    wait = std::min(wait * 2, std::chrono::seconds(30));
  }
}

void FileService::answerDial(uint32_t id)
{
  TlsSettings tls;
  std::string host;
  uint16_t port = 0;
  {
    std::scoped_lock lock(m_mutex);
    tls = m_tls;
    host = m_host;
    port = m_port;
  }

  std::string error;
  auto channel = TlsChannel::connect(host, port, tls, error);
  if (!channel || !hello(*channel, wire::Role::ServeRequest, id, error)) {
    LOG_WARN("file connection: can't answer a request: %s", error.c_str());
    return;
  }
  channel->setBulk();
  channel->setTimeouts(kServeIdleMs, kIoTimeoutMs);
  serve(*channel);
}

//
// serving files to the other computer
//

void FileService::serve(TlsChannel &channel)
{
  {
    std::scoped_lock lock(m_mutex);
    if (m_stopping) {
      return;
    }
    m_serving.insert(&channel);
  }
  const auto unregister = std::shared_ptr<void>(nullptr, [this, &channel](void *) {
    std::scoped_lock lock(m_mutex);
    m_serving.erase(&channel);
  });

  while (!m_stopping) {
    const auto frame = channel.receive();
    if (!frame) {
      return;
    }

    wire::Reader reader(frame->payload);
    std::string offerId;
    std::string path;
    uint32_t item = 0;
    uint64_t offset = 0;
    bool ok = true;

    switch (frame->type) {
    case wire::Type::List:
      ok = reader.str(offerId) && reader.u32(item) && serveList(channel, offerId, item);
      break;
    case wire::Type::Read:
      ok = reader.str(offerId) && reader.u32(item) && reader.str(path) && reader.u64(offset) &&
           serveRead(channel, offerId, item, path, offset);
      break;
    case wire::Type::Ping:
      ok = channel.send(wire::Type::Pong);
      break;
    default:
      ok = false;
    }
    if (!ok) {
      return;
    }
  }
}

bool FileService::serveList(TlsChannel &channel, const std::string &offerId, uint32_t item)
{
  // folders are listed with the same rights as files are read with
  const auto access = actAsUser();
  bool sent = true;
  std::string error;
  const bool listed = Offers::instance().list(
      offerId, item,
      [&channel, &sent](const Entry &entry) {
        wire::Writer writer;
        writer.str(entry.relativePath).u8(entry.isDirectory ? 1 : 0).u64(entry.size).i64(entry.modified);
        sent = channel.send(wire::Type::Entry, writer.data());
        return sent;
      },
      error
  );
  if (!sent) {
    return false;
  }
  if (!listed) {
    return channel.send(wire::Type::Failed, wire::Writer().str(error).data());
  }
  return channel.send(wire::Type::ListEnd);
}

bool FileService::serveRead(
    TlsChannel &channel, const std::string &offerId, uint32_t item, const std::string &path, uint64_t offset
)
{
  const auto access = actAsUser();
  const auto local = Offers::instance().resolve(offerId, item, path);
  if (!local) {
    return channel.send(wire::Type::Failed, wire::Writer().str("not offered").data());
  }

  std::ifstream file(*local, std::ios::binary);
  if (!file) {
    return channel.send(wire::Type::Failed, wire::Writer().str("can't open " + toUtf8(local->filename())).data());
  }
  if (offset > 0) {
    file.seekg(static_cast<std::streamoff>(offset));
  }

  std::vector<char> buffer(wire::kDataChunkSize);
  uint64_t total = 0;
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<size_t>(file.gcount());
    if (count == 0) {
      break;
    }
    if (!channel.send(wire::Type::Data, buffer.data(), count)) {
      return false;
    }
    total += count;
  }
  if (file.bad()) {
    return channel.send(wire::Type::Failed, wire::Writer().str("can't read " + toUtf8(local->filename())).data());
  }
  return channel.send(wire::Type::ReadEnd, wire::Writer().u64(total).data());
}

std::shared_ptr<void> FileService::actAsUser() const
{
  AccessGuard guard;
  {
    std::scoped_lock lock(m_mutex);
    guard = m_guard;
  }
  return guard ? guard() : nullptr;
}

//
// fetching from the other computer
//

std::unique_ptr<TlsChannel> FileService::acquire(const std::string &peer, bool &reused, std::string &error)
{
  bool server = false;
  {
    std::scoped_lock lock(m_mutex);
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_idle.find(peer); it != m_idle.end() && it->first == peer;) {
      auto idle = std::move(it->second);
      it = m_idle.erase(it);
      if (now - idle.since < kMaxIdleAge) {
        reused = true;
        return std::move(idle.channel);
      }
    }
    if (!m_server && !m_client) {
      error = "file copying isn't connected";
      return nullptr;
    }
    server = m_server;
  }
  reused = false;

  if (server) {
    return dial(peer, error);
  }

  TlsSettings tls;
  std::string host;
  uint16_t port = 0;
  {
    std::scoped_lock lock(m_mutex);
    tls = m_tls;
    host = m_host;
    port = m_port;
  }
  auto channel = TlsChannel::connect(host, port, tls, error);
  if (!channel || !hello(*channel, wire::Role::Requester, 0, error)) {
    return nullptr;
  }
  channel->setBulk();
  channel->setTimeouts(kIoTimeoutMs, kIoTimeoutMs);
  return channel;
}

void FileService::release(const std::string &peer, std::unique_ptr<TlsChannel> channel)
{
  if (channel == nullptr) {
    return;
  }
  std::scoped_lock lock(m_mutex);
  m_fetching.erase(channel.get());
  if (!m_stopping && !channel->broken() && m_idle.count(peer) < kMaxIdlePerPeer) {
    m_idle.emplace(peer, Idle{std::move(channel), std::chrono::steady_clock::now()});
  }
}

void FileService::track(TlsChannel *channel)
{
  std::scoped_lock lock(m_mutex);
  m_fetching.insert(channel);
  if (m_stopping) {
    channel->interrupt(); // stopped while it was being set up
  }
}

void FileService::untrack(TlsChannel *channel)
{
  std::scoped_lock lock(m_mutex);
  m_fetching.erase(channel);
}

bool FileService::list(const FileList &list, size_t item, std::vector<Entry> &entries, std::string &error)
{
  const auto request = wire::Writer().str(list.offerId).u32(static_cast<uint32_t>(item)).data();

  // a reused connection may have been closed by the other side while idle: retry once on a new one
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool reused = false;
    auto channel = acquire(list.sourceHost, reused, error);
    if (!channel) {
      return false;
    }
    // stop() may interrupt it until it's given back, or closed below
    track(channel.get());
    const auto untrackLater = std::shared_ptr<void>(nullptr, [this, &channel](void *) {
      if (channel != nullptr) {
        untrack(channel.get());
      }
    });

    entries.clear();
    size_t listedBytes = 0;
    bool lost = !channel->send(wire::Type::List, request);
    while (!lost) {
      const auto frame = channel->receive();
      if (!frame) {
        lost = true;
        break;
      }
      if (frame->type == wire::Type::Entry) {
        wire::Reader reader(frame->payload);
        Entry entry;
        uint8_t isDirectory = 0;
        if (!reader.str(entry.relativePath) || !reader.u8(isDirectory) || !reader.u64(entry.size) ||
            !reader.i64(entry.modified) ||
            (!entry.relativePath.empty() && !FileList::isSafeRelativePath(entry.relativePath))) {
          error = "the other computer sent a bad file list";
          return false;
        }
        entry.isDirectory = isDirectory != 0;
        listedBytes += entry.relativePath.size();
        if (entries.size() >= kMaxListEntries || listedBytes > kMaxListBytes) {
          error = "too many files to copy";
          return false;
        }
        entries.push_back(std::move(entry));
      } else if (frame->type == wire::Type::ListEnd) {
        release(list.sourceHost, std::move(channel));
        return true;
      } else if (frame->type == wire::Type::Failed) {
        error = failure(*frame);
        release(list.sourceHost, std::move(channel));
        return false;
      } else {
        error = "unexpected reply";
        return false;
      }
    }
    if (!(reused && entries.empty())) {
      error = "the connection was lost";
      return false;
    }
  }
  error = "the connection was lost";
  return false;
}

std::unique_ptr<RemoteFile> FileService::open(
    const FileList &list, size_t item, const std::string &relativePath, uint64_t offset, std::string &error
)
{
  if (!relativePath.empty() && !FileList::isSafeRelativePath(relativePath)) {
    error = "bad path";
    return nullptr;
  }
  const auto request = wire::Writer()
                           .str(list.offerId)
                           .u32(static_cast<uint32_t>(item))
                           .str(relativePath)
                           .u64(offset)
                           .data();

  for (int attempt = 0; attempt < 2; ++attempt) {
    bool reused = false;
    auto channel = acquire(list.sourceHost, reused, error);
    if (!channel) {
      return nullptr;
    }
    std::unique_ptr<RemoteFile> file(new RemoteFile(*this, list.sourceHost, std::move(channel)));
    if (file->start(request)) {
      return file;
    }
    if (!reused) {
      break;
    }
  }
  error = "the connection was lost";
  return nullptr;
}

} // namespace deskflow::filetransfer
