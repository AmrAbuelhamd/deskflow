/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsVirtualFilesTests.h"

#include "platform/MSWindowsVirtualFiles.h"

#include <Windows.h>

#include <ole2.h>
#include <shlobj.h>

#include <string>

namespace {

const std::string kSmall = "hello";
const std::string kLarge = [] {
  std::string data(70000, 'x');
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<char>('a' + i % 26);
  }
  return data;
}();

IDataObject *twoFiles(const std::wstring &text = {})
{
  return MSWindowsVirtualFiles::createDataObject(
      MSWindowsVirtualFiles::memorySource({{L"one.txt", kSmall}, {L"two.bin", kLarge}}), text
  );
}

FORMATETC format(const wchar_t *name, DWORD tymed, LONG index = -1)
{
  return {static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)), nullptr, DVASPECT_CONTENT, index, tymed};
}

std::string readAll(IStream *stream)
{
  std::string content;
  char buffer[4096];
  ULONG read = 0;
  while (SUCCEEDED(stream->Read(buffer, sizeof(buffer), &read)) && read > 0) {
    content.append(buffer, read);
  }
  return content;
}

} // namespace

void MSWindowsVirtualFilesTests::initTestCase()
{
  QVERIFY(SUCCEEDED(OleInitialize(nullptr)));
}

void MSWindowsVirtualFilesTests::cleanupTestCase()
{
  OleUninitialize();
}

void MSWindowsVirtualFilesTests::windowsNames()
{
  using V = MSWindowsVirtualFiles;
  QVERIFY(V::windowsName(L"report.pdf") == L"report.pdf");
  QVERIFY(V::windowsName(L"a:b?c*.txt") == L"a\xFF1A" L"b\xFF1F" L"c\xFF0A.txt");
  QVERIFY(V::windowsName(L"ends with dot.") == L"ends with dot_");
  QVERIFY(V::windowsName(L"CON.txt") == L"CON_.txt");
  QVERIFY(V::windowsName(L"con") == L"con_");
  QVERIFY(V::windowsName(L"console.txt") == L"console.txt");
  QVERIFY(V::windowsName(L"") == L"_");
}

void MSWindowsVirtualFilesTests::describesFiles()
{
  IDataObject *object = twoFiles();
  auto descriptor = format(L"FileGroupDescriptorW", TYMED_HGLOBAL);
  QCOMPARE(object->QueryGetData(&descriptor), S_OK);

  STGMEDIUM medium{};
  QCOMPARE(object->GetData(&descriptor, &medium), S_OK);
  QCOMPARE(medium.tymed, static_cast<DWORD>(TYMED_HGLOBAL));
  const auto *group = static_cast<const FILEGROUPDESCRIPTORW *>(GlobalLock(medium.hGlobal));
  QCOMPARE(group->cItems, 2u);
  QVERIFY(std::wstring(group->fgd[0].cFileName) == L"one.txt");
  QCOMPARE(group->fgd[0].nFileSizeLow, static_cast<DWORD>(kSmall.size()));
  QCOMPARE(group->fgd[1].nFileSizeLow, static_cast<DWORD>(kLarge.size()));
  QVERIFY(group->fgd[1].dwFlags & FD_FILESIZE);
  QVERIFY(group->fgd[1].dwFlags & FD_PROGRESSUI);
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);
  object->Release();
}

void MSWindowsVirtualFilesTests::streamsContent()
{
  IDataObject *object = twoFiles();
  auto contents = format(L"FileContents", TYMED_ISTREAM | TYMED_HGLOBAL, 1);
  STGMEDIUM medium{};
  QCOMPARE(object->GetData(&contents, &medium), S_OK);
  QCOMPARE(medium.tymed, static_cast<DWORD>(TYMED_ISTREAM));

  STATSTG stat{};
  QCOMPARE(medium.pstm->Stat(&stat, STATFLAG_DEFAULT), S_OK);
  QCOMPARE(stat.cbSize.QuadPart, static_cast<ULONGLONG>(kLarge.size()));
  QVERIFY(std::wstring(stat.pwcsName) == L"two.bin");
  CoTaskMemFree(stat.pwcsName);

  QVERIFY(readAll(medium.pstm) == kLarge);
  ReleaseStgMedium(&medium);

  // the content of an index that doesn't exist isn't offered
  contents.lindex = 2;
  QCOMPARE(object->GetData(&contents, &medium), DV_E_LINDEX);
  object->Release();
}

void MSWindowsVirtualFilesTests::seeksByReopening()
{
  IDataObject *object = twoFiles();
  auto contents = format(L"FileContents", TYMED_ISTREAM, 1);
  STGMEDIUM medium{};
  QCOMPARE(object->GetData(&contents, &medium), S_OK);

  char buffer[3];
  ULONG read = 0;
  QCOMPARE(medium.pstm->Read(buffer, 3, &read), S_OK);
  QCOMPARE(std::string(buffer, read), kLarge.substr(0, 3));

  LARGE_INTEGER move{};
  move.QuadPart = 1000;
  ULARGE_INTEGER position{};
  QCOMPARE(medium.pstm->Seek(move, STREAM_SEEK_SET, &position), S_OK);
  QCOMPARE(position.QuadPart, 1000ull);
  QCOMPARE(medium.pstm->Read(buffer, 3, &read), S_OK);
  QCOMPARE(std::string(buffer, read), kLarge.substr(1000, 3));

  // reading past the end returns what's left, then nothing
  move.QuadPart = -2;
  QCOMPARE(medium.pstm->Seek(move, STREAM_SEEK_END, &position), S_OK);
  QCOMPARE(medium.pstm->Read(buffer, 3, &read), S_FALSE);
  QCOMPARE(read, 2ul);

  ReleaseStgMedium(&medium);
  object->Release();
}

void MSWindowsVirtualFilesTests::offersOwnershipAndOptOuts()
{
  IDataObject *object = twoFiles();

  // Deskflow's marker, so its own clipboard isn't sent back to the server
  auto ownership = format(L"Deskflow Ownership", TYMED_HGLOBAL);
  QCOMPARE(object->QueryGetData(&ownership), S_OK);

  // kept out of Windows clipboard history and the cloud clipboard
  for (const auto *name : {L"CanIncludeInClipboardHistory", L"CanUploadToCloudClipboard"}) {
    auto optOut = format(name, TYMED_HGLOBAL);
    STGMEDIUM medium{};
    QCOMPARE(object->GetData(&optOut, &medium), S_OK);
    QCOMPARE(*static_cast<const DWORD *>(GlobalLock(medium.hGlobal)), 0ul);
    GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
  }

  // a copy, never a move
  auto effect = format(L"Preferred DropEffect", TYMED_HGLOBAL);
  STGMEDIUM medium{};
  QCOMPARE(object->GetData(&effect, &medium), S_OK);
  QCOMPARE(*static_cast<const DWORD *>(GlobalLock(medium.hGlobal)), static_cast<DWORD>(DROPEFFECT_COPY));
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);
  object->Release();
}

void MSWindowsVirtualFilesTests::offersText()
{
  IDataObject *object = twoFiles(L"one.txt\r\ntwo.bin");
  FORMATETC text{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  QCOMPARE(object->GetData(&text, &medium), S_OK);
  QVERIFY(std::wstring(static_cast<const wchar_t *>(GlobalLock(medium.hGlobal))) == L"one.txt\r\ntwo.bin");
  GlobalUnlock(medium.hGlobal);
  ReleaseStgMedium(&medium);
  object->Release();

  // no text, no text format
  object = twoFiles();
  QCOMPARE(object->QueryGetData(&text), DV_E_FORMATETC);
  object->Release();
}

void MSWindowsVirtualFilesTests::refusesUnknownFormats()
{
  IDataObject *object = twoFiles();
  FORMATETC bitmap{CF_DIB, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM medium{};
  QCOMPARE(object->QueryGetData(&bitmap), DV_E_FORMATETC);
  QCOMPARE(object->GetData(&bitmap, &medium), DV_E_FORMATETC);
  object->Release();
}

QTEST_MAIN(MSWindowsVirtualFilesTests)
