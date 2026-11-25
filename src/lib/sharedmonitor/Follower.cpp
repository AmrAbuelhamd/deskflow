/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "sharedmonitor/Follower.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <initializer_list>
#include <string>

namespace deskflow::sharedmonitor {

Follower::Follower(const Options &options) : m_options(options)
{
  // do nothing
}

std::optional<Shows> Follower::onReading(std::optional<uint16_t> input, Clock::time_point now)
{
  if (!input) {
    return std::nullopt;
  }
  if (input == m_last) {
    ++m_count;
  } else {
    m_last = input;
    m_count = 1;
  }
  const auto seen = *input == m_options.hereInput ? Shows::Here : Shows::Other;

  if (m_expected) {
    if (seen == *m_expected) {
      m_expected.reset(); // the monitor got there
    } else if (now < m_expectedUntil) {
      return std::nullopt; // still switching
    } else {
      m_expected.reset(); // it didn't switch: go by what it shows
    }
  }

  if (m_count < m_options.confirmations || seen == m_shows || now < m_quietUntil) {
    return std::nullopt;
  }
  m_shows = seen;
  m_quietUntil = now + m_options.settle;
  return m_shows;
}

void Follower::onSwitch(bool here, Clock::time_point now)
{
  m_shows = here ? Shows::Here : Shows::Other;
  m_expected = m_shows;
  m_expectedUntil = now + m_options.switchTime;
  m_quietUntil = now + m_options.settle;
}

std::vector<uint16_t> listedInputs(const std::string &capabilities)
{
  std::string text(capabilities);
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  std::vector<uint16_t> inputs;
  const auto start = text.find("vcp(");
  if (start == std::string::npos) {
    return inputs;
  }

  // vcp(10 12 14(05 06 08) 60(0f 11) ...): codes, some followed by their values in brackets
  int depth = 0;
  long code = -1;
  bool inInputs = false;
  for (size_t i = start + 4; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if (c == '(') {
      ++depth;
      inInputs = depth == 1 && code == 0x60;
      ++i;
    } else if (c == ')') {
      if (depth == 0) {
        break; // the end of vcp(...)
      }
      --depth;
      inInputs = false;
      ++i;
    } else if (std::isxdigit(c)) {
      size_t end = i;
      while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end]))) {
        ++end;
      }
      if (end - i <= 4) {
        const auto value = std::stol(text.substr(i, end - i), nullptr, 16);
        if (depth == 0) {
          code = value;
        } else if (inInputs && depth == 1 && value > 0) {
          inputs.push_back(static_cast<uint16_t>(value));
        }
      }
      i = end;
    } else {
      ++i;
    }
  }
  return inputs;
}

bool chooseInputs(
    const std::vector<uint16_t> &listed, uint16_t configuredHere, uint16_t configuredOther, uint16_t &here,
    uint16_t &other
)
{
  const auto firstOf = [&listed](std::initializer_list<uint16_t> wanted) -> uint16_t {
    for (const auto value : listed) {
      if (std::find(wanted.begin(), wanted.end(), value) != wanted.end()) {
        return value;
      }
    }
    return 0;
  };

  here = configuredHere;
  if (here == 0) {
    here = firstOf({kInputDisplayPort1, kInputDisplayPort2});
  }
  if (here == 0) {
    here = firstOf({kInputUsbC});
  }

  other = configuredOther;
  if (other == 0) {
    for (const auto value : listed) {
      if (value != here) {
        other = value;
        break;
      }
    }
  }
  return here != 0 && other != 0 && here != other;
}

std::string inputName(uint16_t input)
{
  switch (input) {
  case 0x01:
    return "VGA-1";
  case 0x02:
    return "VGA-2";
  case 0x03:
    return "DVI-1";
  case 0x04:
    return "DVI-2";
  case kInputDisplayPort1:
    return "DisplayPort-1";
  case kInputDisplayPort2:
    return "DisplayPort-2";
  case kInputHdmi1:
    return "HDMI-1";
  case kInputHdmi2:
    return "HDMI-2";
  case kInputUsbC:
    return "USB-C";
  default: {
    char name[16];
    std::snprintf(name, sizeof(name), "input 0x%02X", static_cast<unsigned>(input));
    return name;
  }
  }
}

} // namespace deskflow::sharedmonitor
