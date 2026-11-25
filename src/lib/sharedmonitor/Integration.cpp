/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "sharedmonitor/Integration.h"

#include "common/Settings.h"

namespace deskflow::sharedmonitor {

namespace {

// an input's VCP 0x60 value, written as 15 or 0x0F; 0 (find it from the monitor) when unset
uint16_t inputSetting(const QString &key)
{
  bool ok = false;
  const auto value = Settings::value(key).toString().toUInt(&ok, 0);
  return ok && value <= 0xFF ? static_cast<uint16_t>(value) : 0;
}

} // namespace

Options optionsFromSettings()
{
  Options options;
  options.enabled = Settings::value(Settings::Core::SharedMonitor).toBool();
  options.hereInput = inputSetting(Settings::Core::SharedMonitorInput);
  options.otherInput = inputSetting(Settings::Core::SharedMonitorOtherInput);
  options.recoveryFile = QStringLiteral("%1/shared-monitor.state").arg(Settings::settingsPath()).toStdString();
  return options;
}

} // namespace deskflow::sharedmonitor
