/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct IDataObject;

//! A file or folder in a paste, path relative to the paste target ('\' separated)
struct VirtualEntry
{
  std::wstring path;
  bool isDirectory = false;
  uint64_t size = 0;
  int64_t modified = 0; //!< seconds since the Unix epoch
};

//! Reads one file's content for Explorer
class VirtualReader
{
public:
  virtual ~VirtualReader() = default;
  //! Fills the buffer unless the file ends first; count is 0 at the end. False on failure.
  virtual bool read(char *buffer, size_t size, size_t &count) = 0;
};

//! Where virtual files come from. Called on the clipboard thread, only once the user pastes.
class VirtualFileSource
{
public:
  virtual ~VirtualFileSource() = default;
  //! Every file and folder in the paste, parents before their contents
  virtual bool describe(std::vector<VirtualEntry> &entries, std::string &error) = 0;
  //! Opens entry `index` of describe()'s list, from `offset`
  virtual std::unique_ptr<VirtualReader> open(size_t index, uint64_t offset, std::string &error) = 0;
};

//! Puts "virtual files" on the Windows clipboard: files that don't exist on disk and whose
//! content Explorer reads from this process when the user pastes, with its usual progress window.
//!
//! The clipboard object lives on its own OLE thread, so Explorer's calls into it never wait on
//! the thread that injects input.
class MSWindowsVirtualFiles
{
public:
  //! How COM decides which processes may call the clipboard object.
  //! The core may run as SYSTEM while Explorer runs as the signed-in user.
  enum class ComAccess
  {
    InteractiveUsers, //!< SYSTEM, administrators and interactive users, at medium integrity or above
    Anyone            //!< no access check
  };

  static MSWindowsVirtualFiles &instance();

  //! Replaces the clipboard with the files from `source`, plus `text` (e.g. their names) if not empty.
  //! Returns at once; the clipboard thread does the rest. COM access is fixed by the first call in the process.
  bool setFiles(
      std::shared_ptr<VirtualFileSource> source, const std::wstring &text,
      ComAccess access = ComAccess::InteractiveUsers
  );

  //! Test hook: returns true if text copied on the server asks this client to offer a test file
  static bool isSelfTestRequest(const std::string &text, ComAccess &access);

  //! A source holding files in memory, for the test hook
  static std::shared_ptr<VirtualFileSource> memorySource(std::vector<std::pair<std::wstring, std::string>> files);

  //! Makes a file or folder name valid on Windows, replacing characters it doesn't allow
  static std::wstring windowsName(const std::wstring &name);

  //! The clipboard object setFiles() puts on the clipboard, for tests; the caller releases it
  static IDataObject *createDataObject(std::shared_ptr<VirtualFileSource> source, const std::wstring &text);

  MSWindowsVirtualFiles(MSWindowsVirtualFiles const &) = delete;
  MSWindowsVirtualFiles &operator=(MSWindowsVirtualFiles const &) = delete;

private:
  MSWindowsVirtualFiles() = default;
  ~MSWindowsVirtualFiles() = default;

  bool start();
  void run();

  unsigned long m_threadId = 0;
};
