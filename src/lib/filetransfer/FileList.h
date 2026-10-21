/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace deskflow::filetransfer {

//! A file or folder copied on the source computer, as carried on the clipboard
struct FileItem
{
  std::string name; //!< UTF-8 display name, never a path
  bool isDirectory = false;
  uint64_t size = 0;    //!< bytes, files only
  int64_t modified = 0; //!< seconds since the Unix epoch

  //! Windows only: the shared folder the item is in, and its path inside that share ('/' separated),
  //! so a computer that has the share mounted can read the item directly
  std::string shareName;
  std::string sharePath;

  bool operator==(const FileItem &) const = default;
};

//! What the clipboard carries when files are copied: names and sizes, never content.
//! The content is fetched from the source computer when the user pastes.
struct FileList
{
  static constexpr int kVersion = 1;
  static constexpr size_t kMaxItems = 10000;

  std::string offerId;    //!< identifies this copy on the source computer
  std::string sourceOs;   //!< "macos" or "windows"
  std::string sourceHost; //!< the source computer's name
  std::vector<FileItem> items;

  std::string toJson() const;

  //! Parses and validates; returns nothing if the data isn't a usable file list
  static std::optional<FileList> fromJson(const std::string &json);

  //! True if the name can be used as a single path component on the receiving computer
  static bool isSafeName(const std::string &name);

  //! True if the path is relative, '/' separated, and never leaves its starting folder
  static bool isSafeRelativePath(const std::string &path);

  bool operator==(const FileList &) const = default;
};

} // namespace deskflow::filetransfer
