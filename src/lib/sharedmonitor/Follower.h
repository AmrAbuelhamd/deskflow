/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace deskflow::sharedmonitor {

//! Which computer a shared monitor shows, as far as the computer that watches it can tell
enum class Shows
{
  Unknown, //!< no steady answer from the monitor yet
  Here,    //!< this computer's input
  Other    //!< any other input
};

//! Decides what a shared monitor shows from the inputs it reports, steadily enough to act on.
//!
//! A change counts after a few matching answers in a row; a missing answer neither counts nor
//! breaks a run. After a change nothing changes again for a moment, so a monitor that hunts for
//! a signal can't make the computers flip back and forth. It has no timers or system calls of
//! its own, so tests can drive it.
class Follower
{
public:
  using Clock = std::chrono::steady_clock;

  struct Options
  {
    uint16_t hereInput = 0;                                         //!< this computer's input (VCP 0x60 value)
    int confirmations = 2;                                          //!< matching answers before a change counts
    std::chrono::milliseconds settle = std::chrono::seconds(3);     //!< no change sooner than this after one
    std::chrono::milliseconds switchTime = std::chrono::seconds(8); //!< how long a requested switch may take
  };

  explicit Follower(const Options &options);

  //! One answer from the monitor, or none. Returns what it shows when that changed.
  std::optional<Shows> onReading(std::optional<uint16_t> input, Clock::time_point now);

  //! This computer switches the monitor (here: to itself). Counts at once; answers that still
  //! show the old input are ignored while the monitor switches.
  void onSwitch(bool here, Clock::time_point now);

  Shows shows() const
  {
    return m_shows;
  }

private:
  Options m_options;
  Shows m_shows = Shows::Unknown;
  std::optional<uint16_t> m_last;
  int m_count = 0;
  Clock::time_point m_quietUntil{};
  std::optional<Shows> m_expected; // a switch this computer asked for, not seen yet
  Clock::time_point m_expectedUntil{};
};

//! Input values (VCP 0x60) from the MCCS standard
inline constexpr uint16_t kInputDisplayPort1 = 0x0F;
inline constexpr uint16_t kInputDisplayPort2 = 0x10;
inline constexpr uint16_t kInputHdmi1 = 0x11;
inline constexpr uint16_t kInputHdmi2 = 0x12;
inline constexpr uint16_t kInputUsbC = 0x1B; // not in the standard, but common

//! The input values a monitor lists for VCP 0x60 in its DDC/CI capabilities string, in order
std::vector<uint16_t> listedInputs(const std::string &capabilities);

//! Picks this computer's input and the other computer's: the configured ones when not 0;
//! otherwise this computer's is the first DisplayPort input listed (or USB-C), and the other
//! computer's the first other input listed. False when it can't tell.
bool chooseInputs(
    const std::vector<uint16_t> &listed, uint16_t configuredHere, uint16_t configuredOther, uint16_t &here,
    uint16_t &other
);

//! A readable name for an input value, e.g. "DisplayPort-1"
std::string inputName(uint16_t input);

} // namespace deskflow::sharedmonitor
