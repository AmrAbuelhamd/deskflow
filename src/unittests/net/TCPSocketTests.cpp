/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "TCPSocketTests.h"

#include "arch/ArchException.h"
#include "base/EventQueue.h"
#include "net/SocketMultiplexer.h"
#include "net/TCPSocket.h"

#include <QTest>

#include <atomic>
#include <chrono>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;
using Family = IArchNetwork::AddressFamily;
using Type = IArchNetwork::SocketType;

// Stands in for a SecureSocket after a fatal TLS error: its write ends the connection
class BrokenSocket : public TCPSocket
{
public:
  using TCPSocket::TCPSocket;

protected:
  JobResult doWrite() override
  {
    return JobResult::Break;
  }
};

// Two connected ends over loopback, or nulls
std::pair<ArchSocket, ArchSocket> connectedPair()
{
  auto addresses = ARCH->nameToAddr("127.0.0.1");
  if (addresses.empty()) {
    return {};
  }
  ArchNetAddress address = addresses.front();
  for (size_t i = 1; i < addresses.size(); ++i) {
    ARCH->closeAddr(addresses[i]);
  }

  ArchSocket listener = ARCH->newSocket(Family::INet, Type::Stream);
  bool bound = false;
  for (int port = 41950; port < 42050 && !bound; ++port) {
    ARCH->setAddrPort(address, port);
    try {
      ARCH->bindSocket(listener, address);
      bound = true;
    } catch (const ArchNetworkException &) {
      // taken: try the next one
    }
  }

  ArchSocket mine = nullptr;
  ArchSocket theirs = nullptr;
  if (bound) {
    ARCH->listenOnSocket(listener);
    mine = ARCH->newSocket(Family::INet, Type::Stream);
    ARCH->connectSocket(mine, address); // may still be connecting; accepting waits for it
    for (int tries = 0; tries < 500 && theirs == nullptr; ++tries) {
      theirs = ARCH->acceptSocket(listener, nullptr);
      if (theirs == nullptr) {
        std::this_thread::sleep_for(10ms);
      }
    }
  }
  ARCH->closeSocket(listener);
  ARCH->closeAddr(address);
  return {mine, theirs};
}

} // namespace

void TCPSocketTests::initTestCase()
{
  m_arch.init();
}

// What hung a server going to sleep: its goodbye to a client whose TLS connection had just
// failed waited in flush() forever
void TCPSocketTests::flushReturnsAfterFatalWrite()
{
  EventQueue events;
  SocketMultiplexer multiplexer;
  const auto [mine, theirs] = connectedPair();
  QVERIFY(mine != nullptr);
  QVERIFY(theirs != nullptr);

  BrokenSocket socket(&events, &multiplexer, mine);
  const char goodbye[] = "CBYE";
  socket.write(goodbye, sizeof(goodbye));

  std::atomic<bool> flushed = false;
  std::thread flusher([&socket, &flushed] {
    socket.flush();
    flushed = true;
  });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!flushed && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(10ms);
  }
  const bool flushedInTime = flushed;
  socket.close(); // wakes a flush that still waits, so the thread can end either way
  flusher.join();
  ARCH->closeSocket(theirs);

  QVERIFY(flushedInTime);
}

QTEST_MAIN(TCPSocketTests)
