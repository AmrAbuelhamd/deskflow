/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileListTests.h"

#include "deskflow/Clipboard.h"
#include "filetransfer/FileList.h"

using deskflow::filetransfer::FileItem;
using deskflow::filetransfer::FileList;

namespace {

FileList sampleList()
{
  FileList list;
  list.offerId = "4f1c2a9e0b7d4e21";
  list.sourceOs = "windows";
  list.sourceHost = "MyPC";
  list.items.push_back(
      {.name = "Win11 25H2 (English) \xE2\x80\x94 x64.iso",
       .size = 7736125440ULL,
       .modified = 1769650000,
       .shareName = "share",
       .sharePath = "Downloads/Win11 25H2 (English) \xE2\x80\x94 x64.iso"}
  );
  list.items.push_back({.name = "Photos", .isDirectory = true, .modified = 1760000000});
  return list;
}

// big-endian, as on the wire
std::string uint32Bytes(uint32_t value)
{
  return {static_cast<char>(value >> 24), static_cast<char>(value >> 16), static_cast<char>(value >> 8),
          static_cast<char>(value)};
}

std::string withItemName(const std::string &name)
{
  return R"({"v":1,"offer":"abc","os":"macos","host":"mac","items":[{"name":")" + name +
         R"(","dir":false,"size":1,"mtime":0}]})";
}

} // namespace

void FileListTests::jsonRoundTrip()
{
  const auto list = sampleList();
  const auto parsed = FileList::fromJson(list.toJson());
  QVERIFY(parsed.has_value());
  QVERIFY(*parsed == list);
  QCOMPARE(parsed->items[0].size, 7736125440ULL);
}

void FileListTests::rejectsUnsafeNames()
{
  QVERIFY(FileList::fromJson(withItemName("ok.txt")).has_value());
  QVERIFY(!FileList::fromJson(withItemName("")).has_value());
  QVERIFY(!FileList::fromJson(withItemName(".")).has_value());
  QVERIFY(!FileList::fromJson(withItemName("..")).has_value());
  QVERIFY(!FileList::fromJson(withItemName("a/b")).has_value());
  QVERIFY(!FileList::fromJson(withItemName(R"(a\\b)")).has_value());
  QVERIFY(!FileList::fromJson(withItemName(R"(a\u0001b)")).has_value());
}

void FileListTests::rejectsMalformedLists()
{
  QVERIFY(!FileList::fromJson("not json").has_value());
  QVERIFY(!FileList::fromJson("{}").has_value());
  QVERIFY(!FileList::fromJson(R"({"v":1,"offer":"abc","items":[]})").has_value());
  QVERIFY(!FileList::fromJson(R"({"v":0,"offer":"abc","items":[{"name":"a"}]})").has_value());
  QVERIFY(!FileList::fromJson(R"({"v":1,"offer":"","items":[{"name":"a"}]})").has_value());
  // the Mac names a folder after the offer id, so it can't be a path
  QVERIFY(!FileList::fromJson(R"({"v":1,"offer":"../../Documents","items":[{"name":"a"}]})").has_value());
  QVERIFY(!FileList::fromJson(R"({"v":1,"offer":"abc/def","items":[{"name":"a"}]})").has_value());
  QVERIFY(!FileList::fromJson(R"({"v":1,"offer":"abc","items":[{"name":"a","size":-1}]})").has_value());
}

void FileListTests::dropsUnsafeShareHint()
{
  auto list = sampleList();
  list.items[0].sharePath = "../Windows/System32/config/SAM";
  const auto parsed = FileList::fromJson(list.toJson());
  QVERIFY(parsed.has_value());
  QVERIFY(parsed->items[0].shareName.empty());
  QVERIFY(parsed->items[0].sharePath.empty());
  QCOMPARE(parsed->items[0].name, list.items[0].name);
}

void FileListTests::safeRelativePaths()
{
  QVERIFY(FileList::isSafeRelativePath("a"));
  QVERIFY(FileList::isSafeRelativePath("a/b/c.txt"));
  QVERIFY(!FileList::isSafeRelativePath(""));
  QVERIFY(!FileList::isSafeRelativePath("/a"));
  QVERIFY(!FileList::isSafeRelativePath("a//b"));
  QVERIFY(!FileList::isSafeRelativePath("a/"));
  QVERIFY(!FileList::isSafeRelativePath("a/../b"));
  QVERIFY(!FileList::isSafeRelativePath("a/./b"));
  QVERIFY(!FileList::isSafeRelativePath(R"(a\b)"));
}

void FileListTests::fileListWireId()
{
  const auto json = sampleList().toJson();
  Clipboard clipboard;
  clipboard.open(0);
  clipboard.add(IClipboard::Format::FileList, json);
  clipboard.close();

  // 4 bytes count, then the format id: "DFFL", far from the sequential ids
  const auto data = clipboard.marshall();
  QCOMPARE(data.substr(4, 4), std::string("DFFL"));
  QCOMPARE(data.substr(12), json);
}

void FileListTests::unmarshallFileList()
{
  const auto json = sampleList().toJson();
  const auto data = uint32Bytes(1) + "DFFL" + uint32Bytes(static_cast<uint32_t>(json.size())) + json;

  Clipboard clipboard;
  clipboard.unmarshall(data, 0);
  clipboard.open(0);
  QVERIFY(clipboard.has(IClipboard::Format::FileList));
  QCOMPARE(clipboard.get(IClipboard::Format::FileList), json);
  clipboard.close();
}

void FileListTests::unknownFormatIdsAreSkipped()
{
  // id 3 is the next sequential id, which upstream may use for another format
  const std::string payload = "GIF89a";
  const std::string text = "hello";
  const auto data = uint32Bytes(2) + uint32Bytes(3) + uint32Bytes(static_cast<uint32_t>(payload.size())) + payload +
                    uint32Bytes(0) + uint32Bytes(static_cast<uint32_t>(text.size())) + text;

  Clipboard clipboard;
  clipboard.unmarshall(data, 0);
  clipboard.open(0);
  QVERIFY(!clipboard.has(IClipboard::Format::FileList));
  QCOMPARE(clipboard.get(IClipboard::Format::Text), text);
  clipboard.close();
}

QTEST_MAIN(FileListTests)
