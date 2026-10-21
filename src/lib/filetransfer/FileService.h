/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "filetransfer/FileList.h"
#include "filetransfer/Offers.h"
#include "filetransfer/TlsChannel.h"
#include "filetransfer/Wire.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace deskflow::filetransfer {

class FileService;

//! A file being read from the other computer
class RemoteFile
{
public:
  ~RemoteFile();
  RemoteFile(RemoteFile const &) = delete;
  RemoteFile &operator=(RemoteFile const &) = delete;

  //! Fills the buffer unless the file ends first; count is 0 at the end. False on failure.
  bool read(char *buffer, size_t size, size_t &count);

  const std::string &error() const
  {
    return m_error;
  }

private:
  friend class FileService;
  RemoteFile(FileService &service, std::string peer, std::unique_ptr<TlsChannel> channel);

  bool start(const std::string &request);
  bool take(Frame frame);
  void letGo(bool reusable);

  FileService &m_service;
  std::string m_peer;
  std::unique_ptr<TlsChannel> m_channel;
  std::string m_chunk;
  size_t m_chunkOffset = 0;
  bool m_done = false;
  std::string m_error;
};

//! Moves file content between two computers on its own TLS connections, so it never waits
//! behind or holds up the connection that carries input.
//!
//! The server listens; the client keeps a control connection open, over which the server asks
//! it to connect back whenever the server needs a file from the client. Every connection is
//! checked with the same certificate fingerprints as the input connection, plus a per-session
//! token the server hands out over that connection.
class FileService
{
public:
  //! The process's service; tests create their own
  static FileService &instance();

  FileService() = default;
  ~FileService();
  FileService(FileService const &) = delete;
  FileService &operator=(FileService const &) = delete;

  //! Server side. Port 0 picks a free port.
  bool startServer(const TlsSettings &tls, uint16_t port, std::string &error);

  //! Client side
  void startClient(
      const TlsSettings &tls, const std::string &host, uint16_t port, const wire::Token &token,
      const std::string &computerName
  );

  void stop();

  bool isServer() const;

  //! Server: whether that client's control connection is up. Client: whether ours is.
  bool isConnected(const std::string &peer = {}) const;

  uint16_t port() const;
  wire::Token token() const;

  //! Lists an item of a list that came from the other computer: the item, then for a folder
  //! everything below it, parents first. Blocking; never call it from Deskflow's event thread.
  bool list(const FileList &list, size_t item, std::vector<Entry> &entries, std::string &error);

  //! Starts reading a file of a list that came from the other computer. Blocking, like list().
  std::unique_ptr<RemoteFile>
  open(const FileList &list, size_t item, const std::string &relativePath, uint64_t offset, std::string &error);

  //! Called on the serving thread before a file is opened or a folder listed for the other
  //! computer, e.g. to act as the signed-in user; what it returns is kept until that's done
  using AccessGuard = std::function<std::shared_ptr<void>()>;
  void setAccessGuard(AccessGuard guard);

private:
  friend class RemoteFile;

  struct Control
  {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<uint32_t> dials;
    TlsChannel *channel = nullptr;
    bool closed = false;
  };

  void acceptLoop();
  void handleIncoming(intptr_t socket);
  void runServerControl(const std::string &peer, std::unique_ptr<TlsChannel> channel);
  void deliverDial(uint32_t id, std::unique_ptr<TlsChannel> channel);
  std::unique_ptr<TlsChannel> dial(const std::string &peer, std::string &error);

  void clientControlLoop(uint64_t generation);
  bool isCurrent(uint64_t generation) const;
  void answerDial(uint32_t id);
  bool hello(TlsChannel &channel, wire::Role role, uint32_t requestId, std::string &error) const;

  void serve(TlsChannel &channel);
  bool serveList(TlsChannel &channel, const std::string &offerId, uint32_t item);
  bool serveRead(TlsChannel &channel, const std::string &offerId, uint32_t item, const std::string &path, uint64_t offset);

  std::shared_ptr<void> actAsUser() const;

  std::unique_ptr<TlsChannel> acquire(const std::string &peer, bool &reused, std::string &error);
  void release(const std::string &peer, std::unique_ptr<TlsChannel> channel);
  void track(TlsChannel *channel);
  void untrack(TlsChannel *channel);

  mutable std::mutex m_mutex;
  bool m_server = false;
  bool m_client = false;
  TlsSettings m_tls;
  wire::Token m_token{};
  std::string m_host;
  uint16_t m_port = 0;
  std::string m_name;
  AccessGuard m_guard;

  std::atomic<bool> m_stopping{false};
  std::condition_variable m_stopWake;
  std::unique_ptr<TlsListener> m_listener;
  std::thread m_acceptThread;
  std::atomic<uint64_t> m_generation{0}; // a stopped client's control thread exits once this moves on
  TlsChannel *m_clientControl = nullptr;
  std::atomic<int> m_workers{0};
  std::set<TlsChannel *> m_serving;
  std::set<TlsChannel *> m_fetching; // waiting on the other computer, so stop() can interrupt them

  std::map<std::string, std::shared_ptr<Control>> m_controls;
  std::map<uint32_t, std::promise<std::unique_ptr<TlsChannel>>> m_pendingDials;
  uint32_t m_nextDial = 1;
  struct Idle
  {
    std::unique_ptr<TlsChannel> channel;
    std::chrono::steady_clock::time_point since;
  };
  std::multimap<std::string, Idle> m_idle;
};

} // namespace deskflow::filetransfer
