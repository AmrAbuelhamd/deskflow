/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "filetransfer/Offers.h"

#include <QCryptographicHash>
#include <QRandomGenerator>

#include <sys/stat.h>

namespace fs = std::filesystem;

namespace deskflow::filetransfer {

std::string toUtf8(const fs::path &path)
{
  const auto text = path.generic_u8string();
  return std::string(reinterpret_cast<const char *>(text.data()), text.size());
}

fs::path fromUtf8(const std::string &text)
{
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(text.data()), text.size()));
}

std::optional<Entry> statEntry(const fs::path &path)
{
  Entry entry;
#if defined(_WIN32)
  struct _stat64 info{};
  if (_wstat64(path.c_str(), &info) != 0) {
    return std::nullopt;
  }
  entry.isDirectory = (info.st_mode & _S_IFDIR) != 0;
#else
  struct stat info{};
  if (::stat(path.c_str(), &info) != 0) {
    return std::nullopt;
  }
  entry.isDirectory = S_ISDIR(info.st_mode);
#endif
  entry.size = entry.isDirectory ? 0 : static_cast<uint64_t>(info.st_size);
  entry.modified = static_cast<int64_t>(info.st_mtime);
  return entry;
}

namespace {

// Links, and junctions on Windows, inside an offered folder could lead out of it, so only plain
// folders and files are followed
bool isPlain(fs::file_type type)
{
  return type == fs::file_type::directory || type == fs::file_type::regular;
}

} // namespace

Offers &Offers::instance()
{
  static Offers s_instance;
  return s_instance;
}

FileList Offers::publish(
    const std::vector<fs::path> &paths, const std::string &sourceOs, const std::string &sourceHost,
    const Annotate &annotate
)
{
  // per run, so offer ids can't be predicted
  static const auto salt = QByteArray::number(QRandomGenerator::system()->generate64());

  FileList list;
  list.sourceOs = sourceOs;
  list.sourceHost = sourceHost;

  QCryptographicHash hash(QCryptographicHash::Sha256);
  hash.addData(salt);
  std::vector<fs::path> kept;
  for (const auto &path : paths) {
    const auto entry = statEntry(path);
    const auto name = toUtf8(path.filename());
    if (!entry || !FileList::isSafeName(name) || list.items.size() >= FileList::kMaxItems) {
      continue;
    }
    FileItem item;
    item.name = name;
    item.isDirectory = entry->isDirectory;
    item.size = entry->size;
    item.modified = entry->modified;
    if (annotate) {
      annotate(path, item);
    }
    list.items.push_back(item);
    kept.push_back(path);

    hash.addData(QByteArray::fromStdString(toUtf8(path)));
    hash.addData(QByteArray::number(static_cast<qulonglong>(entry->size)));
    hash.addData(QByteArray::number(static_cast<qlonglong>(entry->modified)));
  }
  list.offerId = hash.result().toHex().left(24).toStdString();

  if (kept.empty()) {
    return list;
  }

  std::scoped_lock lock(m_mutex);
  if (m_offers.empty() || m_offers.back().id != list.offerId) {
    m_offers.push_back({list.offerId, std::move(kept)});
    while (m_offers.size() > kKept) {
      m_offers.pop_front();
    }
  }
  return list;
}

std::optional<fs::path> Offers::itemPath(const std::string &offerId, size_t item) const
{
  std::scoped_lock lock(m_mutex);
  for (const auto &offer : m_offers) {
    if (offer.id == offerId) {
      if (item < offer.paths.size()) {
        return offer.paths[item];
      }
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<fs::path>
Offers::resolve(const std::string &offerId, size_t item, const std::string &relativePath) const
{
  auto path = itemPath(offerId, item);
  if (!path || relativePath.empty()) {
    return path;
  }
  if (!FileList::isSafeRelativePath(relativePath)) {
    return std::nullopt;
  }

  // walk down one component at a time, so a link inside the folder can't lead out of it
  std::error_code error;
  auto current = *path;
  for (const auto &part : fromUtf8(relativePath)) {
    current /= part;
    const auto type = fs::symlink_status(current, error).type();
    if (!isPlain(type) && type != fs::file_type::not_found) {
      return std::nullopt;
    }
  }
  return current;
}

bool Offers::list(
    const std::string &offerId, size_t item, const std::function<bool(const Entry &)> &emit, std::string &error
) const
{
  const auto path = itemPath(offerId, item);
  if (!path) {
    error = "not offered";
    return false;
  }
  auto root = statEntry(*path);
  if (!root) {
    error = "no longer exists";
    return false;
  }
  if (!emit(*root) || !root->isDirectory) {
    return true;
  }

  std::error_code code;
  fs::recursive_directory_iterator it(*path, fs::directory_options::skip_permission_denied, code);
  if (code) {
    error = "can't read the folder: " + code.message();
    return false;
  }
  for (const fs::recursive_directory_iterator end; it != end; it.increment(code)) {
    if (code) {
      error = "can't read the folder: " + code.message();
      return false;
    }
    if (!isPlain(it->symlink_status(code).type())) {
      it.disable_recursion_pending();
      continue;
    }
    auto entry = statEntry(it->path());
    if (!entry) {
      continue;
    }
    entry->relativePath = toUtf8(it->path().lexically_relative(*path));
    if (!emit(*entry)) {
      return true;
    }
  }
  return true;
}

void Offers::clear()
{
  std::scoped_lock lock(m_mutex);
  m_offers.clear();
}

} // namespace deskflow::filetransfer
