/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

//! What each operating system does for a shared monitor. Every call may block for a moment
//! (talking to the monitor takes tens of milliseconds), so none runs on Deskflow's event thread.
namespace deskflow::sharedmonitor::platform {

//! Whether this computer can ask the monitor which input it shows (DDC/CI)
bool canWatch();

//! The monitor's DDC/CI capabilities string; empty when it doesn't answer
std::string capabilities();

//! The input the monitor shows (VCP 0x60); nothing when it doesn't answer
std::optional<uint16_t> readInput();

//! Switches the monitor to an input
bool writeInput(uint16_t input);

//! Stops using the monitor on this computer, or starts again. A stop is noted in recoveryFile
//! until it's undone, so recover() can undo it after a crash. Refused while the screens sleep.
bool useMonitor(bool use, const std::string &recoveryFile);

//! Whether this process stopped using the monitor and hasn't started again
bool stoppedUsingMonitor();

//! Undoes a stop that a crash or a kill left behind. A stop this process made itself (before a
//! sleep) stays: it's still what the monitor shows.
void recover(const std::string &recoveryFile);

//! Whether this computer's screens sleep, so its displays can't be changed now
bool screensAsleep();

} // namespace deskflow::sharedmonitor::platform
