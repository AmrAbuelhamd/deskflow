/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "filetransfer/FileList.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace deskflow::filetransfer {

//! A file or folder inside an offered item; the item itself has an empty path
struct Entry
{
  std::string relativePath; //!< '/' separated, UTF-8
  bool isDirectory = false;
  uint64_t size = 0;
  int64_t modified = 0;
};

std::string toUtf8(const std::filesystem::path &path);
std::filesystem::path fromUtf8(const std::string &text);

//! Size, modification time and kind of a local file; nothing if it doesn't exist
std::optional<Entry> statEntry(const std::filesystem::path &path);

//! Files this computer has put on the clipboard, which the other computer may fetch.
//! Only these, and what's inside offered folders, are ever served. Thread-safe.
class Offers
{
public:
  static constexpr size_t kKept = 8;

  static Offers &instance();

  //! Registers copied files and returns the list for the clipboard. The same files give the
  //! same offer id, so reading the clipboard again doesn't look like a new copy.
  FileList publish(
      const std::vector<std::filesystem::path> &paths, const std::string &sourceOs, const std::string &sourceHost
  );

  //! The local path for a request, or nothing if no current offer covers it
  std::optional<std::filesystem::path>
  resolve(const std::string &offerId, size_t item, const std::string &relativePath) const;

  //! Emits the item, then for a folder everything below it, parents first. Links inside
  //! folders are skipped. Stops early if emit returns false.
  bool list(
      const std::string &offerId, size_t item, const std::function<bool(const Entry &)> &emit, std::string &error
  ) const;

  void clear();

private:
  struct Offer
  {
    std::string id;
    std::vector<std::filesystem::path> paths;
  };

  std::optional<std::filesystem::path> itemPath(const std::string &offerId, size_t item) const;

  mutable std::mutex m_mutex;
  std::deque<Offer> m_offers; // newest last
};

} // namespace deskflow::filetransfer
