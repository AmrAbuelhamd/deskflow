/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "filetransfer/FileList.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace deskflow::filetransfer {

namespace {

constexpr size_t kMaxNameBytes = 255;
constexpr size_t kMaxPathBytes = 4096;
constexpr size_t kMaxIdBytes = 64;

QString qs(const std::string &text)
{
  return QString::fromStdString(text);
}

bool hasControlCharacters(const std::string &text)
{
  for (const unsigned char c : text) {
    if (c < 0x20 || c == 0x7f) {
      return true;
    }
  }
  return false;
}

} // namespace

bool FileList::isSafeName(const std::string &name)
{
  if (name.empty() || name.size() > kMaxNameBytes || name == "." || name == "..") {
    return false;
  }
  if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
    return false;
  }
  return !hasControlCharacters(name);
}

bool FileList::isSafeRelativePath(const std::string &path)
{
  if (path.empty() || path.size() > kMaxPathBytes || path.front() == '/') {
    return false;
  }
  size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find('/', start);
    const auto part = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!isSafeName(part)) {
      return false;
    }
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  return true;
}

std::string FileList::toJson() const
{
  QJsonArray jsonItems;
  for (const auto &item : items) {
    QJsonObject jsonItem{
        {"name", qs(item.name)},
        {"dir", item.isDirectory},
        {"size", static_cast<qint64>(item.size)},
        {"mtime", static_cast<qint64>(item.modified)}
    };
    if (!item.shareName.empty()) {
      jsonItem["share"] = qs(item.shareName);
      jsonItem["sharePath"] = qs(item.sharePath);
    }
    jsonItems.append(jsonItem);
  }

  const QJsonObject json{
      {"v", kVersion}, {"offer", qs(offerId)}, {"os", qs(sourceOs)}, {"host", qs(sourceHost)}, {"items", jsonItems}
  };
  return QJsonDocument(json).toJson(QJsonDocument::Compact).toStdString();
}

std::optional<FileList> FileList::fromJson(const std::string &json)
{
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(json), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return std::nullopt;
  }

  // newer versions may add fields; the ones read here keep their meaning
  const auto root = document.object();
  if (root.value("v").toInt(0) < 1) {
    return std::nullopt;
  }

  FileList list;
  list.offerId = root.value("offer").toString().toStdString();
  list.sourceOs = root.value("os").toString().toStdString();
  list.sourceHost = root.value("host").toString().toStdString();
  if (list.offerId.empty() || list.offerId.size() > kMaxIdBytes || hasControlCharacters(list.offerId) ||
      list.sourceOs.size() > kMaxIdBytes || hasControlCharacters(list.sourceOs) ||
      list.sourceHost.size() > kMaxNameBytes || hasControlCharacters(list.sourceHost)) {
    return std::nullopt;
  }

  const auto jsonItems = root.value("items").toArray();
  if (jsonItems.isEmpty() || static_cast<size_t>(jsonItems.size()) > kMaxItems) {
    return std::nullopt;
  }

  for (const auto &value : jsonItems) {
    const auto jsonItem = value.toObject();
    FileItem item;
    item.name = jsonItem.value("name").toString().toStdString();
    item.isDirectory = jsonItem.value("dir").toBool(false);
    const auto size = jsonItem.value("size").toInteger(-1);
    item.modified = jsonItem.value("mtime").toInteger(0);
    item.shareName = jsonItem.value("share").toString().toStdString();
    item.sharePath = jsonItem.value("sharePath").toString().toStdString();

    if (!isSafeName(item.name) || size < 0) {
      return std::nullopt;
    }
    item.size = static_cast<uint64_t>(size);

    // a share reference is only a hint; drop it rather than reject the list
    if (!item.shareName.empty() && (!isSafeName(item.shareName) || !isSafeRelativePath(item.sharePath))) {
      item.shareName.clear();
      item.sharePath.clear();
    }

    list.items.push_back(std::move(item));
  }

  return list;
}

} // namespace deskflow::filetransfer
