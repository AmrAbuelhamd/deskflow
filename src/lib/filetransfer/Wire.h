/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>

//! Messages on the file connection, a TLS connection separate from the one that carries input.
//!
//! Every frame is: u32 length of the rest, u8 type, payload. Integers are big-endian,
//! strings are a u16 length followed by UTF-8 bytes.
namespace deskflow::filetransfer::wire {

constexpr uint16_t kVersion = 1;
constexpr uint32_t kMaxFrameSize = 2 * 1024 * 1024;
constexpr size_t kDataChunkSize = 1024 * 1024;
constexpr size_t kTokenWords = 4;

using Token = std::array<uint32_t, kTokenWords>;

enum class Type : uint8_t
{
  Hello = 1,   //!< u16 version, u8 role, token words, u32 request id, string computer name
  Welcome = 2, //!< empty
  Refused = 3, //!< string reason

  List = 10,    //!< string offer id, u32 item: list the item and, for a folder, everything below it
  Entry = 11,   //!< string relative path, u8 folder, u64 size, i64 modified
  ListEnd = 12, //!< empty

  Read = 20,    //!< string offer id, u32 item, string relative path, u64 offset
  Data = 21,    //!< bytes of the file
  ReadEnd = 22, //!< u64 bytes sent

  Dial = 30, //!< u32 request id: the server asks the client to connect back and serve a request
  Ping = 31, //!< empty
  Pong = 32, //!< empty

  Failed = 40 //!< string reason: the request failed, the connection stays usable
};

//! What the connecting side will do once connected
enum class Role : uint8_t
{
  Control = 1,     //!< kept open by the client; the server sends Dial over it
  Requester = 2,   //!< the connecting side sends requests, the accepting side serves them
  ServeRequest = 3 //!< answer to a Dial: the accepting side sends requests, the connecting side serves them
};

//! Builds a payload
class Writer
{
public:
  Writer &u8(uint8_t value);
  Writer &u16(uint16_t value);
  Writer &u32(uint32_t value);
  Writer &u64(uint64_t value);
  Writer &i64(int64_t value);
  Writer &str(const std::string &value);
  Writer &raw(const char *data, size_t size);

  const std::string &data() const
  {
    return m_data;
  }

private:
  std::string m_data;
};

//! Reads a payload; once anything is missing or too long, every read fails
class Reader
{
public:
  explicit Reader(const std::string &data) : m_data(data)
  {
  }

  bool u8(uint8_t &value);
  bool u16(uint16_t &value);
  bool u32(uint32_t &value);
  bool u64(uint64_t &value);
  bool i64(int64_t &value);
  bool str(std::string &value);

  bool ok() const
  {
    return m_ok;
  }

  bool atEnd() const
  {
    return m_ok && m_offset == m_data.size();
  }

private:
  bool take(size_t size, const char *&out);

  const std::string &m_data;
  size_t m_offset = 0;
  bool m_ok = true;
};

} // namespace deskflow::filetransfer::wire
