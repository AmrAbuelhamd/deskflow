/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

//! One monitor cabled to two computers: the computer it doesn't show stops using it.
//!
//! Monitors keep every input "connected" while they show one of them, so neither computer
//! notices a switch and both keep the monitor as a screen; on the hidden one, the pointer and
//! windows go where nobody can see them. The server asks the monitor over DDC/CI which input it
//! shows (macOS: it answers on every input) and tells the client over the file connection; the
//! computer that isn't shown stops using the monitor until it is again. Either computer's menu
//! can also switch the monitor.
namespace deskflow::sharedmonitor {

struct Options
{
  bool enabled = false;
  uint16_t hereInput = 0;   //!< this computer's input (VCP 0x60 value); 0: found from the monitor
  uint16_t otherInput = 0;  //!< the other computer's input; 0: found from the monitor
  std::string recoveryFile; //!< notes a change to undo after a crash
};

//! Starts watching the monitor and telling clients what it shows (the server, on macOS)
void startServer(const Options &options);

//! Starts following what the server says the monitor shows (a client)
void startClient(const Options &options);

//! Stops either, and this computer uses the monitor as usual again
void stop();

//! Switches the monitor, asked for from the menu: to this computer (here) or the other one
void requestSwitch(bool here);

//! What the menu can offer: "here" (the monitor shows this computer), "other" (it shows the
//! other one) or "none" (not watched, or not known yet)
std::string state();

//! Called with state() whenever it changes, on any thread
void setStateListener(std::function<void(const std::string &state)> listener);

//! The core runs as SYSTEM on Windows, so it starts itself as the signed-in user with this
//! argument and a Win+P mode to switch the displays
inline constexpr char kDisplayModeArg[] = "--shared-monitor-display";

//! What that copy runs; returns the exit code
int runDisplayModeHelper(const char *mode);

} // namespace deskflow::sharedmonitor
