/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "NetworkMonitor.h"

#include <QAbstractSocket>
#include <QList>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

namespace deskflow::gui {

bool NetworkMonitor::isVirtualInterface(const QString &interfaceName)
{
  // Common virtual network interface patterns
  static const auto virtualRegEx = QRegularExpression(
      QStringLiteral("^vboxnet|vmnet|docker|virbr|veth|br\\-|tun|utun|awdl|p2p|llw|anpi|tap|vEth"),
      QRegularExpression::CaseInsensitiveOption
  );
  return virtualRegEx.match(interfaceName).hasMatch();
}

NetworkMonitor::NetworkMonitor(QObject *parent) : QObject(parent), m_checkTimer(new QTimer(this))
{
  connect(m_checkTimer, &QTimer::timeout, this, &NetworkMonitor::updateNetworkState);
}

void NetworkMonitor::startMonitoring(int intervalMs)
{
  if (m_isMonitoring) {
    return;
  }

  updateNetworkState();

  m_checkTimer->start(intervalMs);
  m_isMonitoring = true;
}

void NetworkMonitor::stopMonitoring()
{
  if (!m_isMonitoring) {
    return;
  }

  m_checkTimer->stop();
  m_isMonitoring = false;
}

QList<NetworkAddress> NetworkMonitor::addresses()
{
  QList<NetworkAddress> result;
  QSet<QHostAddress> uniqueAddresses;

  const auto allInterfaces = QNetworkInterface::allInterfaces();
  for (const auto &interface : allInterfaces) {
    if (!(interface.flags() & QNetworkInterface::IsUp) || !(interface.flags() & QNetworkInterface::IsRunning) ||
        (interface.flags() & QNetworkInterface::IsLoopBack)) {
      continue;
    }

    const bool isP2P = (interface.flags() & QNetworkInterface::IsPointToPoint);
    const bool isVirtualType = interface.type() == QNetworkInterface::Virtual;
    const bool isVirtual = isVirtualInterface(interface.humanReadableName()) || isP2P || isVirtualType;
    const auto addressEntries = interface.addressEntries();
    // link-local is only worth offering when it's a physical interface's only address, e.g. a direct ethernet cable
    const bool hasNonLinkLocal = std::ranges::any_of(addressEntries, [](const QNetworkAddressEntry &entry) {
      return !entry.ip().isLoopback() && !entry.ip().isLinkLocal();
    });
    const bool skipLinkLocal = isVirtual || hasNonLinkLocal;

    auto connection = NetworkAddress::Connection::Other;
    if (interface.type() == QNetworkInterface::Ethernet)
      connection = NetworkAddress::Connection::Ethernet;
    else if (interface.type() == QNetworkInterface::Wifi)
      connection = NetworkAddress::Connection::WiFi;

    for (const auto &entry : addressEntries) {
      const QHostAddress address = entry.ip();

      if (address.isLoopback() || (skipLinkLocal && address.isLinkLocal()) || uniqueAddresses.contains(address)) {
        continue;
      }

      uniqueAddresses.insert(address);
      result.append({address, connection, isVirtual});
    }
  }

  sortAddresses(result);
  return result;
}

QStringList NetworkMonitor::validAddresses()
{
  QStringList ipList;
  for (const auto &address : addresses()) {
    ipList.append(address.ip.toString());
  }
  return ipList;
}

void NetworkMonitor::sortAddresses(QList<NetworkAddress> &addresses)
{
  const auto group = [](const NetworkAddress &address) {
    const bool isIP6 = address.ip.protocol() == QHostAddress::IPv6Protocol;
    return (isIP6 ? 2 : 0) + (address.isVirtual ? 1 : 0);
  };

  // a cable is faster and steadier than wi-fi, so it is the better one to connect to
  const auto rank = [](NetworkAddress::Connection connection) {
    switch (connection) {
    case NetworkAddress::Connection::Ethernet:
      return 0;
    case NetworkAddress::Connection::WiFi:
      return 1;
    default:
      return 2;
    }
  };

  std::ranges::sort(addresses, [&group, &rank](const NetworkAddress &a, const NetworkAddress &b) {
    if (group(a) != group(b))
      return group(a) < group(b);
    if (!a.isVirtual) {
      if (a.ip.isPrivateUse() != b.ip.isPrivateUse())
        return a.ip.isPrivateUse();
      if (a.ip.isLinkLocal() != b.ip.isLinkLocal())
        return a.ip.isLinkLocal();
      if (rank(a.connection) != rank(b.connection))
        return rank(a.connection) < rank(b.connection);
    }
    if (a.ip.protocol() == QHostAddress::IPv6Protocol)
      return a.ip.toString() < b.ip.toString();
    return a.ip.toIPv4Address() < b.ip.toIPv4Address();
  });
}

QList<NetworkAddress> NetworkMonitor::suggestedAddresses(const QList<NetworkAddress> &addresses)
{
  QList<NetworkAddress> suggested;
  for (const auto &address : addresses) {
    if (!address.isVirtual && address.ip.protocol() == QHostAddress::IPv4Protocol)
      suggested.append(address);
  }

  if (suggested.isEmpty() && !addresses.isEmpty())
    suggested.append(addresses.first());

  return suggested;
}

QString NetworkMonitor::describe(const NetworkAddress &address)
{
  switch (address.connection) {
  case NetworkAddress::Connection::Ethernet:
    return tr("%1 (Ethernet)").arg(address.ip.toString());
  case NetworkAddress::Connection::WiFi:
    return tr("%1 (Wi-Fi)").arg(address.ip.toString());
  default:
    return address.ip.toString();
  }
}

void NetworkMonitor::setIpAddresses(const QList<NetworkAddress> &newAddresses)
{
  if (newAddresses == m_lastAddresses)
    return;
  m_lastAddresses = newAddresses;
  Q_EMIT ipAddressesChanged(m_lastAddresses);
}

void NetworkMonitor::updateNetworkState()
{
  setIpAddresses(addresses());
}

} // namespace deskflow::gui
