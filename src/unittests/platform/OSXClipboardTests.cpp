/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2011 Nick Bolton
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "OSXClipboardTests.h"

#include "platform/OSXClipboard.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <QDebug>
#include <QTemporaryFile>
#include <QUrl>

#include <string>
#include <utility>
#include <vector>

void OSXClipboardTests::open()
{
  OSXClipboard clipboard;
  QVERIFY(clipboard.open(0));
  QVERIFY(clipboard.empty());
  clipboard.close();
}

void OSXClipboardTests::singleFormat()
{
  using enum IClipboard::Format;

  OSXClipboard clipboard;
  QVERIFY(clipboard.empty());
  clipboard.add(Text, m_testString);
  QVERIFY(clipboard.has(Text));
  QCOMPARE(clipboard.get(Text), m_testString);
}

void OSXClipboardTests::formatConvert_UTF8()
{
  OSXClipboardUTF8Converter converter;
  QCOMPARE(IClipboard::Format::Text, converter.getFormat());
  QCOMPARE(converter.getOSXFormat(), CFSTR("public.utf8-plain-text"));
  QCOMPARE(converter.fromIClipboard("test data\n"), "test data\r");
  QCOMPARE(converter.toIClipboard("test data\r"), "test data\n");
}

namespace {

// Puts one item on the general pasteboard with the given flavors
void putItem(const std::vector<std::pair<CFStringRef, std::string>> &flavors)
{
  PasteboardRef pboard = nullptr;
  QCOMPARE(PasteboardCreate(kPasteboardClipboard, &pboard), OSStatus(noErr));
  QCOMPARE(PasteboardClear(pboard), OSStatus(noErr));
  for (const auto &[type, data] : flavors) {
    CFDataRef ref = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(data.data()), data.size());
    const auto result = PasteboardPutItemFlavor(pboard, (PasteboardItemID)1, type, ref, kPasteboardFlavorNoFlags);
    CFRelease(ref);
    QCOMPARE(result, OSStatus(noErr));
  }
  CFRelease(pboard);
}

// The flavors of the first item on the general pasteboard, for diagnostics
QStringList flavorsOnPasteboard()
{
  QStringList result;
  PasteboardRef pboard = nullptr;
  if (PasteboardCreate(kPasteboardClipboard, &pboard) != noErr) {
    return result;
  }
  PasteboardSynchronize(pboard);
  PasteboardItemID item = nullptr;
  CFArrayRef flavors = nullptr;
  if (PasteboardGetItemIdentifier(pboard, 1, &item) == noErr && PasteboardCopyItemFlavors(pboard, item, &flavors) == noErr) {
    for (CFIndex i = 0; i < CFArrayGetCount(flavors); ++i) {
      result << QString::fromCFString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(flavors, i)));
    }
    CFRelease(flavors);
  }
  CFRelease(pboard);
  return result;
}

// A 1x1 24-bit BMP file
std::string tinyBmp()
{
  const std::vector<unsigned char> bytes = {
      // file header: magic, file size 58, reserved, pixel data offset 54
      'B', 'M', 58, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0,
      // info header: size 40, 1x1, 1 plane, 24 bpp, no compression, 4 bytes of pixels, 2835 px/m
      40, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0x13, 0x0b, 0, 0, 0x13, 0x0b, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0,
      // one white pixel, padded to 4 bytes
      0xff, 0xff, 0xff, 0
  };
  return std::string(bytes.begin(), bytes.end());
}

} // namespace

void OSXClipboardTests::bitmapWithoutFile()
{
  putItem({{CFSTR("com.microsoft.bmp"), tinyBmp()}});

  OSXClipboard clipboard;
  QVERIFY(clipboard.has(IClipboard::Format::Bitmap));
}

void OSXClipboardTests::fileCopySkipsIcon()
{
  // macOS only keeps file URLs on the general pasteboard for files that exist
  QTemporaryFile file;
  QVERIFY(file.open());
  const auto url = QUrl::fromLocalFile(file.fileName()).toString(QUrl::FullyEncoded).toStdString();

  // what Finder offers when a file is copied: its location, its name and its icon
  putItem(
      {{CFSTR("public.file-url"), url},
       {CFSTR("public.utf8-plain-text"), "deskflow-test.txt"},
       {CFSTR("com.microsoft.bmp"), tinyBmp()}}
  );

  OSXClipboard clipboard;
  if (clipboard.has(IClipboard::Format::Bitmap)) {
    qWarning() << "pasteboard flavors:" << flavorsOnPasteboard();
  }
  QVERIFY(!clipboard.has(IClipboard::Format::Bitmap));
  QVERIFY(clipboard.get(IClipboard::Format::Bitmap).empty());
  QVERIFY(clipboard.has(IClipboard::Format::Text));
  QCOMPARE(clipboard.get(IClipboard::Format::Text), std::string("deskflow-test.txt"));
}

QTEST_MAIN(OSXClipboardTests)
