/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NetworkMonitorTests.h"

#include "gui/core/NetworkMonitor.h"

#include <QSignalSpy>

using namespace deskflow::gui;

namespace {

using Connection = NetworkAddress::Connection;

NetworkAddress address(const char *ip, Connection connection = Connection::Other, bool isVirtual = false)
{
  return {QHostAddress(QString::fromLatin1(ip)), connection, isVirtual};
}

QStringList ips(const QList<NetworkAddress> &addresses)
{
  QStringList result;
  for (const auto &a : addresses) {
    result.append(a.ip.toString());
  }
  return result;
}

} // namespace

void NetworkMonitorTests::testVirtualInterface()
{
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vboxnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vboXnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("vmnet0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("docker-bridge")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("virbr0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("veth0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("br-0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("tun0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("utun0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("awdl0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("p2p0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("llw0")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("anpi5")));
  QVERIFY(NetworkMonitor::isVirtualInterface(QStringLiteral("tap1")));

  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("eth0")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("enp0s0f")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("wifi")));
  QVERIFY(!NetworkMonitor::isVirtualInterface(QStringLiteral("wlan0")));
}

void NetworkMonitorTests::testSortAddresses()
{
  QList<NetworkAddress> addresses{
      address("fd00::2", Connection::Other, true),   address("10.211.55.2", Connection::Other, true),
      address("203.0.113.5", Connection::Ethernet),  address("192.168.1.20", Connection::WiFi),
      address("fd00::1", Connection::Ethernet),      address("169.254.10.10", Connection::Ethernet),
      address("192.168.1.30", Connection::Ethernet), address("192.168.1.5", Connection::WiFi),
  };

  NetworkMonitor::sortAddresses(addresses);

  // physical IPv4 first: private, then link-local, then public; Ethernet before Wi-Fi, then by number
  const QStringList expected{
      QStringLiteral("192.168.1.30"),  QStringLiteral("192.168.1.5"), QStringLiteral("192.168.1.20"),
      QStringLiteral("169.254.10.10"), QStringLiteral("203.0.113.5"), QStringLiteral("10.211.55.2"),
      QStringLiteral("fd00::1"),       QStringLiteral("fd00::2"),
  };
  QCOMPARE(ips(addresses), expected);
}

void NetworkMonitorTests::testSuggestedAddresses()
{
  const QList<NetworkAddress> addresses{
      address("192.168.1.30", Connection::Ethernet),
      address("192.168.1.5", Connection::WiFi),
      address("10.211.55.2", Connection::Other, true),
      address("fd00::1", Connection::Ethernet),
  };
  const QStringList expected{QStringLiteral("192.168.1.30"), QStringLiteral("192.168.1.5")};
  QCOMPARE(ips(NetworkMonitor::suggestedAddresses(addresses)), expected);

  // with no physical IPv4 address, the best of the rest
  const QList<NetworkAddress> noIP4{
      address("fd00::1", Connection::Ethernet), address("fd00::2", Connection::Other, true)
  };
  QCOMPARE(ips(NetworkMonitor::suggestedAddresses(noIP4)), QStringList{QStringLiteral("fd00::1")});

  QVERIFY(NetworkMonitor::suggestedAddresses({}).isEmpty());
}

void NetworkMonitorTests::testDescribe()
{
  QCOMPARE(
      NetworkMonitor::describe(address("192.168.1.30", Connection::Ethernet)), QStringLiteral("192.168.1.30 (Ethernet)")
  );
  QCOMPARE(NetworkMonitor::describe(address("192.168.1.5", Connection::WiFi)), QStringLiteral("192.168.1.5 (Wi-Fi)"));
  QCOMPARE(NetworkMonitor::describe(address("10.211.55.2", Connection::Other, true)), QStringLiteral("10.211.55.2"));
}

QTEST_MAIN(NetworkMonitorTests)
