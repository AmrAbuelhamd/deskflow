/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsFileClipboard.h"

#include "base/Log.h"
#include "common/Settings.h"
#include "filetransfer/FileService.h"
#include "filetransfer/Offers.h"
#include "platform/MSWindowsVirtualFiles.h"

#include <Windows.h>

#include <shellapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;
using namespace deskflow::filetransfer;

namespace {

std::wstring widen(const std::string &text)
{
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
  std::wstring result(size, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), size);
  return result;
}

std::wstring lowered(std::wstring text)
{
  if (!text.empty()) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
  }
  return text;
}

struct Share
{
  std::string name;
  std::wstring path; // lower case, without a trailing separator
};

// The folders this computer shares, from the Server service's registry key.
// Hidden administrative shares (C$ etc.) are left out.
std::vector<Share> sharedFolders()
{
  std::vector<Share> shares;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\LanmanServer\\Shares", 0, KEY_READ, &key) !=
      ERROR_SUCCESS) {
    return shares;
  }

  for (DWORD index = 0;; ++index) {
    wchar_t name[256];
    DWORD nameSize = 256;
    DWORD type = 0;
    std::vector<wchar_t> data(4096);
    DWORD dataSize = static_cast<DWORD>(data.size() * sizeof(wchar_t));
    const auto result = RegEnumValueW(
        key, index, name, &nameSize, nullptr, &type, reinterpret_cast<BYTE *>(data.data()), &dataSize
    );
    if (result == ERROR_NO_MORE_ITEMS) {
      break;
    }
    if (result != ERROR_SUCCESS || type != REG_MULTI_SZ || nameSize == 0 || name[nameSize - 1] == L'$') {
      continue;
    }

    // a list of "Key=value" strings, one of which is the shared folder
    const auto end = data.data() + dataSize / sizeof(wchar_t);
    for (const wchar_t *line = data.data(); line < end && *line != L'\0'; line += wcslen(line) + 1) {
      if (_wcsnicmp(line, L"Path=", 5) == 0) {
        std::wstring path = lowered(line + 5);
        while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
          path.pop_back();
        }
        const std::wstring shareName(name, nameSize);
        const int size =
            WideCharToMultiByte(CP_UTF8, 0, shareName.data(), (int)shareName.size(), nullptr, 0, nullptr, nullptr);
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, shareName.data(), (int)shareName.size(), utf8.data(), size, nullptr, nullptr);
        shares.push_back({utf8, path});
        break;
      }
    }
  }
  RegCloseKey(key);
  return shares;
}

// Fills in the shared folder a copied file lives in, so a computer with that share mounted
// can open it directly; the longest matching share wins
void addShareHint(const std::vector<Share> &shares, const fs::path &file, FileItem &item)
{
  const auto path = lowered(fs::path(file).make_preferred().lexically_normal().wstring());
  const Share *best = nullptr;
  for (const auto &share : shares) {
    const auto &root = share.path;
    if (path.size() > root.size() && path.compare(0, root.size(), root) == 0 &&
        (path[root.size()] == L'\\' || root.back() == L'\\') && (!best || root.size() > best->path.size())) {
      best = &share;
    }
  }
  if (best == nullptr) {
    return;
  }

  // the original spelling, not the lower-case one
  const auto original = fs::path(file).make_preferred().lexically_normal().wstring();
  auto relative = original.substr(best->path.size() + (best->path.back() == L'\\' ? 0 : 1));
  std::replace(relative.begin(), relative.end(), L'\\', L'/');
  const auto relativeUtf8 = toUtf8(fs::path(relative));
  if (FileList::isSafeRelativePath(relativeUtf8)) {
    item.shareName = best->name;
    item.sharePath = relativeUtf8;
  }
}

// Deskflow usually runs as SYSTEM; files are read for the other computer as the signed-in user,
// so nothing can be fetched that the user couldn't open
void installAccessGuard()
{
  static std::once_flag once;
  std::call_once(once, [] {
    FileService::instance().setAccessGuard([]() -> std::shared_ptr<void> {
      HANDLE token = nullptr;
      // only works for SYSTEM; when running as the user there's nothing to do
      if (!WTSQueryUserToken(WTSGetActiveConsoleSessionId(), &token)) {
        return nullptr;
      }
      if (!ImpersonateLoggedOnUser(token)) {
        LOG_WARN("files: can't act as the signed-in user, error=%lu", GetLastError());
        CloseHandle(token);
        return nullptr;
      }
      return std::shared_ptr<void>(token, [](void *handle) {
        RevertToSelf();
        CloseHandle(handle);
      });
    });
  });
}

//! Reads one remote file for Explorer
class RemoteReader : public VirtualReader
{
public:
  explicit RemoteReader(std::unique_ptr<RemoteFile> file) : m_file(std::move(file))
  {
  }

  bool read(char *buffer, size_t size, size_t &count) override
  {
    if (!m_file->read(buffer, size, count)) {
      LOG_WARN("files: %s", m_file->error().c_str());
      return false;
    }
    return true;
  }

private:
  std::unique_ptr<RemoteFile> m_file;
};

//! Files on the other computer, listed and fetched when the user pastes
class RemoteSource : public VirtualFileSource
{
public:
  explicit RemoteSource(FileList list) : m_list(std::move(list))
  {
  }

  bool describe(std::vector<VirtualEntry> &entries, std::string &error) override
  {
    for (size_t item = 0; item < m_list.items.size(); ++item) {
      std::vector<Entry> remote;
      if (!FileService::instance().list(m_list, item, remote, error)) {
        return false;
      }
      const auto root = MSWindowsVirtualFiles::windowsName(widen(m_list.items[item].name));
      for (const auto &entry : remote) {
        std::wstring path = root;
        size_t start = 0;
        while (!entry.relativePath.empty() && start <= entry.relativePath.size()) {
          const auto slash = entry.relativePath.find('/', start);
          const auto part = entry.relativePath.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
          path += L'\\' + MSWindowsVirtualFiles::windowsName(widen(part));
          if (slash == std::string::npos) {
            break;
          }
          start = slash + 1;
        }
        // a paste can't hold longer paths
        if (path.size() >= MAX_PATH) {
          LOG_WARN("files: skipping %s, its path is too long", (m_list.items[item].name + "/" + entry.relativePath).c_str());
          continue;
        }
        entries.push_back({path, entry.isDirectory, entry.size, entry.modified});
        m_refs.push_back({item, entry.relativePath});
      }
    }
    return true;
  }

  std::unique_ptr<VirtualReader> open(size_t index, uint64_t offset, std::string &error) override
  {
    if (index >= m_refs.size()) {
      error = "no such file";
      return nullptr;
    }
    auto file = FileService::instance().open(m_list, m_refs[index].item, m_refs[index].relativePath, offset, error);
    if (file == nullptr) {
      return nullptr;
    }
    return std::make_unique<RemoteReader>(std::move(file));
  }

private:
  struct Ref
  {
    size_t item;
    std::string relativePath;
  };

  FileList m_list;
  std::vector<Ref> m_refs;
};

} // namespace

namespace MSWindowsFileClipboard {

bool hasCopiedFiles()
{
  return IsClipboardFormatAvailable(CF_HDROP) != 0;
}

std::string copiedFiles()
{
  const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
  if (drop == nullptr) {
    return {};
  }

  std::vector<fs::path> files;
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < count; ++i) {
    const UINT length = DragQueryFileW(drop, i, nullptr, 0);
    std::wstring path(length + 1, L'\0');
    DragQueryFileW(drop, i, path.data(), length + 1);
    path.resize(length);
    files.emplace_back(path);
  }
  if (files.empty()) {
    return {};
  }

  installAccessGuard();
  const auto shares = sharedFolders();
  // the other computer knows this one by its Deskflow name
  const auto host = Settings::value(Settings::Core::ComputerName).toString().toStdString();
  const auto list = Offers::instance().publish(files, "windows", host, [&shares](const fs::path &file, FileItem &item) {
    addShareHint(shares, file, item);
  });
  LOG_DEBUG("copied files: %zu, offer %s", list.items.size(), list.offerId.c_str());
  return list.items.empty() ? std::string() : list.toJson();
}

bool offerRemoteFiles(const std::string &fileListJson, const std::string &text)
{
  auto list = FileList::fromJson(fileListJson);
  if (!list) {
    LOG_WARN("files: ignoring a bad file list from the server");
    return false;
  }
  LOG_INFO("files: %zu copied on %s, ready to paste", list->items.size(), list->sourceHost.c_str());
  return MSWindowsVirtualFiles::instance().setFiles(std::make_shared<RemoteSource>(std::move(*list)), widen(text));
}

} // namespace MSWindowsFileClipboard
