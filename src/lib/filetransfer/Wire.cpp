/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "filetransfer/Wire.h"

#include <algorithm>
#include <limits>

namespace deskflow::filetransfer::wire {

Writer &Writer::u8(uint8_t value)
{
  m_data.push_back(static_cast<char>(value));
  return *this;
}

Writer &Writer::u16(uint16_t value)
{
  return u8(static_cast<uint8_t>(value >> 8)).u8(static_cast<uint8_t>(value));
}

Writer &Writer::u32(uint32_t value)
{
  return u16(static_cast<uint16_t>(value >> 16)).u16(static_cast<uint16_t>(value));
}

Writer &Writer::u64(uint64_t value)
{
  return u32(static_cast<uint32_t>(value >> 32)).u32(static_cast<uint32_t>(value));
}

Writer &Writer::i64(int64_t value)
{
  return u64(static_cast<uint64_t>(value));
}

Writer &Writer::str(const std::string &value)
{
  const auto size = static_cast<uint16_t>(std::min<size_t>(value.size(), std::numeric_limits<uint16_t>::max()));
  return u16(size).raw(value.data(), size);
}

Writer &Writer::raw(const char *data, size_t size)
{
  m_data.append(data, size);
  return *this;
}

bool Reader::take(size_t size, const char *&out)
{
  if (!m_ok || m_data.size() - m_offset < size) {
    m_ok = false;
    return false;
  }
  out = m_data.data() + m_offset;
  m_offset += size;
  return true;
}

bool Reader::u8(uint8_t &value)
{
  const char *p = nullptr;
  if (!take(1, p)) {
    return false;
  }
  value = static_cast<uint8_t>(p[0]);
  return true;
}

bool Reader::u16(uint16_t &value)
{
  uint8_t high = 0;
  uint8_t low = 0;
  if (!u8(high) || !u8(low)) {
    return false;
  }
  value = static_cast<uint16_t>((high << 8) | low);
  return true;
}

bool Reader::u32(uint32_t &value)
{
  uint16_t high = 0;
  uint16_t low = 0;
  if (!u16(high) || !u16(low)) {
    return false;
  }
  value = (static_cast<uint32_t>(high) << 16) | low;
  return true;
}

bool Reader::u64(uint64_t &value)
{
  uint32_t high = 0;
  uint32_t low = 0;
  if (!u32(high) || !u32(low)) {
    return false;
  }
  value = (static_cast<uint64_t>(high) << 32) | low;
  return true;
}

bool Reader::i64(int64_t &value)
{
  uint64_t bits = 0;
  if (!u64(bits)) {
    return false;
  }
  value = static_cast<int64_t>(bits);
  return true;
}

bool Reader::str(std::string &value)
{
  uint16_t size = 0;
  const char *p = nullptr;
  if (!u16(size) || !take(size, p)) {
    return false;
  }
  value.assign(p, size);
  return true;
}

} // namespace deskflow::filetransfer::wire
