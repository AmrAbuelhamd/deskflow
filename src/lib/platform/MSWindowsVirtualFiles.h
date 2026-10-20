/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>
#include <vector>

//! Puts "virtual files" on the Windows clipboard: files that don't exist on disk and
//! whose content Explorer reads from this process when the user pastes.
//!
//! The clipboard object lives on its own OLE thread, so Explorer's calls into it never
//! wait on the thread that injects input.
class MSWindowsVirtualFiles
{
public:
  struct File
  {
    std::wstring name;
    std::string content;
  };

  //! How COM decides which processes may call the clipboard object.
  //! The core may run as SYSTEM while Explorer runs as the signed-in user.
  enum class ComAccess
  {
    InteractiveUsers, //!< SYSTEM, administrators and interactive users, at medium integrity or above
    Anyone            //!< no access check
  };

  static MSWindowsVirtualFiles &instance();

  //! Replaces the clipboard with the given files.
  //! COM access is fixed by the first call in the process.
  bool setFiles(std::vector<File> files, ComAccess access = ComAccess::InteractiveUsers);

  //! Test hook: returns true if text copied on the server asks this client to offer a test file
  static bool isSelfTestRequest(const std::string &text, ComAccess &access);

  MSWindowsVirtualFiles(MSWindowsVirtualFiles const &) = delete;
  MSWindowsVirtualFiles &operator=(MSWindowsVirtualFiles const &) = delete;

private:
  MSWindowsVirtualFiles() = default;
  ~MSWindowsVirtualFiles() = default;

  bool start();
  void run();

  unsigned long m_threadId = 0;
};
