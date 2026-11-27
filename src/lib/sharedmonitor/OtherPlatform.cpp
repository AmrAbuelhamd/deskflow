/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

// Systems without shared monitor support: nothing to watch and nothing to switch

#include "sharedmonitor/Platform.h"
#include "sharedmonitor/SharedMonitor.h"

namespace deskflow::sharedmonitor {

int runDisplayModeHelper(const char *)
{
  return 2;
}

namespace platform {

bool canWatch()
{
  return false;
}

std::string capabilities()
{
  return {};
}

std::optional<uint16_t> readInput()
{
  return std::nullopt;
}

bool writeInput(uint16_t)
{
  return false;
}

bool useMonitor(bool, const std::string &)
{
  return false;
}

bool stoppedUsingMonitor()
{
  return false;
}

void recover(const std::string &)
{
  // nothing was changed
}

bool screensAsleep()
{
  return false;
}

} // namespace platform

} // namespace deskflow::sharedmonitor
