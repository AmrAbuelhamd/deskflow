/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2004 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXClipboard.h"

#include "arch/ArchException.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "filetransfer/Offers.h"
#include "platform/OSXClipboardBMPConverter.h"
#include "platform/OSXClipboardHTMLConverter.h"
#include "platform/OSXClipboardTextConverter.h"
#include "platform/OSXClipboardUTF16Converter.h"
#include "platform/OSXClipboardUTF8Converter.h"

#include <climits>
#include <filesystem>
#include <vector>

namespace {

// Finder copies a file's icon along with its location; that icon isn't a picture the user copied
bool isFileCopy(PasteboardRef pboard, PasteboardItemID item)
{
  PasteboardFlavorFlags flags;
  return PasteboardGetItemFlavorFlags(pboard, item, CFSTR("public.file-url"), &flags) == noErr;
}

// Marks pasteboard items written by Deskflow, so its own writes aren't read back as a new copy
const CFStringRef kOwnFilesFlavor = CFSTR("org.deskflow.file-list");

// The files copied in Finder: one pasteboard item per file, each with a file URL. Items whose URL is
// only promised are skipped; reading them would make the app that copied them produce the file.
std::vector<std::filesystem::path> copiedFiles(PasteboardRef pboard)
{
  std::vector<std::filesystem::path> files;
  ItemCount count = 0;
  if (PasteboardGetItemCount(pboard, &count) != noErr) {
    return files;
  }

  for (ItemCount index = 1; index <= count; ++index) {
    PasteboardItemID item = nullptr;
    PasteboardFlavorFlags flags = 0;
    if (PasteboardGetItemIdentifier(pboard, static_cast<CFIndex>(index), &item) != noErr ||
        PasteboardGetItemFlavorFlags(pboard, item, CFSTR("public.file-url"), &flags) != noErr ||
        (flags & kPasteboardFlavorPromised) != 0) {
      continue;
    }

    CFDataRef data = nullptr;
    if (PasteboardCopyItemFlavorData(pboard, item, CFSTR("public.file-url"), &data) != noErr || data == nullptr) {
      continue;
    }
    // Finder writes file reference URLs (file:///.file/id=...); turn them into paths
    CFURLRef url =
        CFURLCreateWithBytes(nullptr, CFDataGetBytePtr(data), CFDataGetLength(data), kCFStringEncodingUTF8, nullptr);
    CFRelease(data);
    if (url == nullptr) {
      continue;
    }
    CFURLRef pathUrl = CFURLCreateFilePathURL(nullptr, url, nullptr);
    CFRelease(url);
    if (pathUrl == nullptr) {
      continue;
    }
    char path[PATH_MAX];
    if (CFURLGetFileSystemRepresentation(pathUrl, true, reinterpret_cast<UInt8 *>(path), sizeof(path))) {
      files.emplace_back(path);
    }
    CFRelease(pathUrl);
  }
  return files;
}

bool hasOwnFiles(PasteboardRef pboard, PasteboardItemID item)
{
  PasteboardFlavorFlags flags;
  return PasteboardGetItemFlavorFlags(pboard, item, kOwnFilesFlavor, &flags) == noErr;
}

} // namespace

//
// OSXClipboard
//

OSXClipboard::OSXClipboard() : m_time(0), m_pboard(nullptr)
{
  m_converters.push_back(new OSXClipboardHTMLConverter);
  m_converters.push_back(new OSXClipboardBMPConverter);
  m_converters.push_back(new OSXClipboardUTF8Converter);
  m_converters.push_back(new OSXClipboardUTF16Converter);
  m_converters.push_back(new OSXClipboardTextConverter);

  OSStatus createErr = PasteboardCreate(kPasteboardClipboard, &m_pboard);
  if (createErr != noErr) {
    LOG_WARN("failed to create clipboard reference: error %i", createErr);
    LOG_ERR("unable to connect to pasteboard, clipboard sharing disabled", createErr);
    m_pboard = nullptr;
    return;
  }

  OSStatus syncErr = PasteboardSynchronize(m_pboard);
  if (syncErr != noErr) {
    LOG_WARN("failed to syncronize clipboard: error %i", syncErr);
  }
}

OSXClipboard::~OSXClipboard()
{
  clearConverters();
}

bool OSXClipboard::empty()
{
  LOG_DEBUG("emptying clipboard");
  if (m_pboard == nullptr)
    return false;

  OSStatus err = PasteboardClear(m_pboard);
  if (err != noErr) {
    LOG_WARN("failed to clear clipboard: error %i", err);
    return false;
  }

  return true;
}

bool OSXClipboard::synchronize()
{
  if (m_pboard == nullptr)
    return false;

  PasteboardSyncFlags flags = PasteboardSynchronize(m_pboard);
  LOG_VERBOSE("flags: %x", flags);

  if (flags & kPasteboardModified) {
    return true;
  }
  return false;
}

void OSXClipboard::add(Format format, const std::string &data)
{
  if (m_pboard == nullptr)
    return;

  LOG_DEBUG("add %d bytes to clipboard format: %d", data.size(), format);
  if (format == IClipboard::Format::Text) {
    LOG_DEBUG("format of data to be added to clipboard was kText");
  } else if (format == IClipboard::Format::Bitmap) {
    LOG_DEBUG("format of data to be added to clipboard was kBitmap");
  } else if (format == IClipboard::Format::HTML) {
    LOG_DEBUG("format of data to be added to clipboard was kHTML");
  }

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {

    IOSXClipboardConverter *converter = *index;

    // skip converters for other formats
    if (converter->getFormat() == format) {
      std::string osXData = converter->fromIClipboard(data);
      CFStringRef flavorType = converter->getOSXFormat();
      CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (uint8_t *)osXData.data(), osXData.size());
      PasteboardItemID itemID = 0;

      if (dataRef) {
        PasteboardPutItemFlavor(m_pboard, itemID, flavorType, dataRef, kPasteboardFlavorNoFlags);

        CFRelease(dataRef);
        LOG_DEBUG("added %d bytes to clipboard format: %d", data.size(), format);
      }
    }
  }
}

bool OSXClipboard::open(Time time) const
{
  if (m_pboard == nullptr)
    return false;

  LOG_DEBUG("opening clipboard");
  m_time = time;
  return true;
}

void OSXClipboard::close() const
{
  LOG_DEBUG("closing clipboard");
  /* not needed */
}

IClipboard::Time OSXClipboard::getTime() const
{
  return m_time;
}

bool OSXClipboard::has(Format format) const
{
  if (m_pboard == nullptr)
    return false;

  PasteboardItemID item;
  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  if (format == Format::Bitmap && isFileCopy(m_pboard, item)) {
    return false;
  }
  if (format == Format::FileList) {
    return isFileCopy(m_pboard, item) && !hasOwnFiles(m_pboard, item);
  }

  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    IOSXClipboardConverter *converter = *index;
    if (converter->getFormat() == format) {
      PasteboardFlavorFlags flags;
      CFStringRef type = converter->getOSXFormat();

      OSStatus res;

      if ((res = PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags)) == noErr) {
        return true;
      }
    }
  }

  return false;
}

std::string OSXClipboard::get(Format format) const
{
  CFStringRef type;
  PasteboardItemID item;
  std::string result;

  if (m_pboard == nullptr)
    return result;

  PasteboardGetItemIdentifier(m_pboard, (CFIndex)1, &item);

  if (format == Format::Bitmap && isFileCopy(m_pboard, item)) {
    return result;
  }
  if (format == Format::FileList) {
    if (hasOwnFiles(m_pboard, item)) {
      return result;
    }
    const auto files = copiedFiles(m_pboard);
    if (files.empty()) {
      return result;
    }
    // the name other computers know this one by
    const auto host = Settings::value(Settings::Core::ComputerName).toString().toStdString();
    const auto list = deskflow::filetransfer::Offers::instance().publish(files, "macos", host);
    LOG_DEBUG("copied files: %zu, offer %s", list.items.size(), list.offerId.c_str());
    return list.items.empty() ? result : list.toJson();
  }

  // find the converter for the first clipboard format we can handle
  IOSXClipboardConverter *converter = nullptr;
  for (ConverterList::const_iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    converter = *index;

    PasteboardFlavorFlags flags;
    type = converter->getOSXFormat();

    if (converter->getFormat() == format && PasteboardGetItemFlavorFlags(m_pboard, item, type, &flags) == noErr) {
      break;
    }
    converter = nullptr;
  }

  // if no converter then we don't recognize any formats
  if (converter == nullptr) {
    LOG_DEBUG("unable to find converter for data");
    return result;
  }

  // get the clipboard data.
  CFDataRef buffer = nullptr;
  try {
    OSStatus err = PasteboardCopyItemFlavorData(m_pboard, item, type, &buffer);

    if (err != noErr) {
      throw err;
    }

    result = std::string((char *)CFDataGetBytePtr(buffer), CFDataGetLength(buffer));
  } catch (OSStatus err) {
    LOG_DEBUG("exception thrown in OSXClipboard::get MacError (%d)", err);
  } catch (...) {
    LOG_DEBUG("unknown exception in OSXClipboard::get");
    RETHROW_THREADEXCEPTION
  }

  if (buffer != nullptr)
    CFRelease(buffer);

  return converter->toIClipboard(result);
}

void OSXClipboard::clearConverters()
{
  if (m_pboard == nullptr)
    return;

  for (ConverterList::iterator index = m_converters.begin(); index != m_converters.end(); ++index) {
    delete *index;
  }
  m_converters.clear();
}
