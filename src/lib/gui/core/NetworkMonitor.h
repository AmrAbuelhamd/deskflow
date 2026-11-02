/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QHostAddress>
#include <QList>
#include <QObject>

class QTimer;
namespace deskflow::gui {

/**
 * @brief An IP address of this computer and the kind of connection it is on
 */
struct NetworkAddress
{
  enum class Connection
  {
    Other,
    Ethernet,
    WiFi
  };

  QHostAddress ip;
  Connection connection = Connection::Other;
  bool isVirtual = false;

  bool operator==(const NetworkAddress &other) const = default;
};

/**
 * @brief Monitor network activity changes and provide IP address updates
 *
 * The NetworkMonitor class monitors IP address changes
 * It periodically checks network status and emits signals when changes are detected.
 */
class NetworkMonitor : public QObject
{
  Q_OBJECT

public:
  /**
   * @brief Construct a new NetworkMonitor object
   * @param parent Parent QObject
   */
  explicit NetworkMonitor(QObject *parent = nullptr);

  /**
   * @brief Destroy the NetworkMonitor object
   */
  ~NetworkMonitor() override = default;

  /**
   * @brief Start network monitoring
   * @param intervalMs Check interval in milliseconds, default 3000ms (3 seconds)
   */
  void startMonitoring(int intervalMs = 3000);

  /**
   * @brief Stop network monitoring
   */
  void stopMonitoring();

  /**
   * @brief Get all available IP addresses (excluding loopback addresses), best first
   * @return IP addresses with the kind of connection each is on
   */
  static QList<NetworkAddress> addresses();

  /**
   * @brief Get list of all available IP addresses (excluding loopback addresses)
   * @return IP address list
   */
  static QStringList validAddresses();

  /**
   * @brief Sort addresses best first: physical IPv4, virtual IPv4, physical IPv6, then virtual IPv6
   * Physical ones put private before public, link-local next, and Ethernet before Wi-Fi.
   * @param addresses Addresses to sort
   */
  static void sortAddresses(QList<NetworkAddress> &addresses);

  /**
   * @brief Pick the addresses another computer on the network can connect to
   * @param addresses Addresses sorted by sortAddresses
   * @return The physical IPv4 addresses, or the first address when there are none
   */
  static QList<NetworkAddress> suggestedAddresses(const QList<NetworkAddress> &addresses);

  /**
   * @brief Describe an address with its kind of connection, e.g. "192.168.1.2 (Wi-Fi)"
   * @param address The address to describe
   * @return The address followed by its kind of connection, when known
   */
  static QString describe(const NetworkAddress &address);

  /**
   * @brief Check if a network interface is virtual
   * @param interfaceName Network interface name
   * @return true if it's a virtual interface
   */
  static bool isVirtualInterface(const QString &interfaceName);

Q_SIGNALS:
  /**
   * @brief Emitted when IP addresses change
   * @param addresses New IP address list
   */
  void ipAddressesChanged(const QList<deskflow::gui::NetworkAddress> &addresses);

private:
  void setIpAddresses(const QList<NetworkAddress> &newAddresses);

  /**
   * @brief Update current network status
   */
  void updateNetworkState();

  QTimer *m_checkTimer;                  ///< Timer for periodic network checks
  QList<NetworkAddress> m_lastAddresses; ///< Last known IP addresses
  bool m_isMonitoring = false;           ///< Flag indicating if monitoring is active
};

} // namespace deskflow::gui
