/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/OSXFileClipboard.h"

#include "base/Log.h"
#include "filetransfer/FileService.h"
#include "filetransfer/Offers.h"

#import <AppKit/AppKit.h>

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <vector>

namespace fs = std::filesystem;
using namespace deskflow::filetransfer;

namespace {

// Finder waits for a fetched item and macOS gives up after 120 s, so only fetch what fits
constexpr uint64_t kMaxFetchBytes = 10ULL * 1000 * 1000 * 1000;
constexpr auto kFetchDeadline = std::chrono::seconds(110);

NSString *const kOwnFilesType = @"org.deskflow.file-list";
NSString *const kTransientType = @"org.nspasteboard.TransientType";

// providers of the items on the pasteboard, kept until the pasteboard no longer needs them
NSMutableArray *providers()
{
  static NSMutableArray *s_providers = [[NSMutableArray alloc] init];
  return s_providers;
}

fs::path cacheRoot()
{
  NSArray *folders = NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
  NSString *caches = folders.count > 0 ? folders[0] : NSTemporaryDirectory();
  return fs::path(caches.fileSystemRepresentation) / "org.deskflow.deskflow" / "paste";
}

std::string lowerAscii(std::string text)
{
  for (auto &c : text) {
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  }
  return text;
}

std::string percentDecoded(const std::string &text)
{
  std::string result;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '%' && i + 2 < text.size() && isxdigit(static_cast<unsigned char>(text[i + 1])) &&
        isxdigit(static_cast<unsigned char>(text[i + 2]))) {
      result += static_cast<char>(std::stoi(text.substr(i + 1, 2), nullptr, 16));
      i += 2;
    } else {
      result += text[i];
    }
  }
  return result;
}

// Where an item lives on a share of the source computer that this Mac has mounted, if it does
std::optional<fs::path> pathOnShare(const FileList &list, const FileItem &item)
{
  if (item.shareName.empty()) {
    return std::nullopt;
  }

  struct statfs *mounts = nullptr;
  const int count = getmntinfo(&mounts, MNT_NOWAIT);
  for (int i = 0; i < count; ++i) {
    if (strcmp(mounts[i].f_fstypename, "smbfs") != 0) {
      continue;
    }
    // e.g. //user@MyPC.local/share
    std::string from = mounts[i].f_mntfromname;
    if (from.rfind("//", 0) == 0) {
      from = from.substr(2);
    }
    const auto slash = from.find('/');
    if (slash == std::string::npos) {
      continue;
    }
    auto host = lowerAscii(from.substr(0, slash));
    const auto share = lowerAscii(percentDecoded(from.substr(slash + 1)));
    if (const auto at = host.rfind('@'); at != std::string::npos) {
      host = host.substr(at + 1);
    }
    if (const auto colon = host.find(':'); colon != std::string::npos) {
      host = host.substr(0, colon);
    }
    if (host.size() > 6 && host.compare(host.size() - 6, 6, ".local") == 0) {
      host.resize(host.size() - 6);
    }
    if (host != lowerAscii(list.sourceHost) || share != lowerAscii(item.shareName)) {
      continue;
    }

    const auto candidate = fs::path(mounts[i].f_mntonname) / fromUtf8(item.sharePath);
    struct stat info{};
    if (::stat(candidate.c_str(), &info) == 0) {
      return candidate;
    }
  }
  return std::nullopt;
}

void setModified(const fs::path &path, int64_t modified)
{
  timeval times[2] = {{static_cast<time_t>(modified), 0}, {static_cast<time_t>(modified), 0}};
  utimes(path.c_str(), times);
}

// Fetches an item, and for a folder everything inside it, from the source computer
bool fetch(const FileList &list, size_t item, const fs::path &target, std::string &error)
{
  auto &service = FileService::instance();
  std::vector<Entry> entries;
  if (!service.list(list, item, entries, error)) {
    return false;
  }

  uint64_t total = 0;
  for (const auto &entry : entries) {
    total += entry.size;
  }
  if (total > kMaxFetchBytes) {
    error = "too big to fetch while Finder waits; copy it from a shared folder instead";
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() + kFetchDeadline;
  std::error_code code;
  fs::remove_all(target, code);
  std::vector<char> buffer(1024 * 1024);

  for (const auto &entry : entries) {
    const auto path = entry.relativePath.empty() ? target : target / fromUtf8(entry.relativePath);
    if (entry.isDirectory) {
      fs::create_directories(path, code);
      continue;
    }
    fs::create_directories(path.parent_path(), code);

    auto file = service.open(list, item, entry.relativePath, 0, error);
    if (file == nullptr) {
      return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      error = "can't write " + path.string();
      return false;
    }
    size_t count = 0;
    do {
      if (!file->read(buffer.data(), buffer.size(), count)) {
        error = file->error();
        return false;
      }
      out.write(buffer.data(), static_cast<std::streamsize>(count));
      if (std::chrono::steady_clock::now() > deadline) {
        error = "took too long for Finder to wait";
        return false;
      }
    } while (count > 0);
    out.close();
    setModified(path, entry.modified);
  }

  // folders last, as writing files inside them changes their dates
  for (const auto &entry : entries) {
    if (entry.isDirectory) {
      setModified(entry.relativePath.empty() ? target : target / fromUtf8(entry.relativePath), entry.modified);
    }
  }
  return true;
}

// Keeps the newest two pastes' downloads (one may still be copying), removes older ones
void tidyCache(const std::string &currentOffer)
{
  static std::string s_previousOffer;
  std::error_code code;
  for (const auto &entry : fs::directory_iterator(cacheRoot(), code)) {
    const auto name = entry.path().filename().string();
    if (name != currentOffer && name != s_previousOffer) {
      fs::remove_all(entry.path(), code);
    }
  }
  if (s_previousOffer != currentOffer) {
    s_previousOffer = currentOffer;
  }
}

} // namespace

//! Provides an item's file URL when an app pastes it, fetching the item first
@interface DeskflowFileProvider : NSObject <NSPasteboardItemDataProvider>
{
  std::shared_ptr<FileList> _list;
  size_t _item;
}
- (instancetype)initWithList:(std::shared_ptr<FileList>)list item:(size_t)item;
@end

@implementation DeskflowFileProvider

- (instancetype)initWithList:(std::shared_ptr<FileList>)list item:(size_t)item
{
  if ((self = [super init])) {
    _list = std::move(list);
    _item = item;
  }
  return self;
}

// Called on the main thread while the pasting app waits; Deskflow's input threads carry on
- (void)pasteboard:(NSPasteboard *)pasteboard item:(NSPasteboardItem *)item provideDataForType:(NSPasteboardType)type
{
  const auto &file = _list->items[_item];
  const auto target = cacheRoot() / _list->offerId / fromUtf8(file.name);
  LOG_INFO("files: fetching %s for a paste", file.name.c_str());

  const auto started = std::chrono::steady_clock::now();
  std::string error;
  if (!fetch(*_list, _item, target, error)) {
    LOG_WARN("files: can't paste %s: %s", file.name.c_str(), error.c_str());
    return; // the pasting app gets nothing for this item
  }
  const std::chrono::duration<double> took = std::chrono::steady_clock::now() - started;
  LOG_INFO("files: fetched %s in %.1f s", file.name.c_str(), took.count());

  NSString *path = [NSString stringWithUTF8String:target.c_str()];
  [item setString:[NSURL fileURLWithPath:path isDirectory:file.isDirectory].absoluteString forType:type];
}

- (void)pasteboardFinishedWithDataProvider:(NSPasteboard *)pasteboard
{
  [providers() removeObject:self];
}

@end

namespace OSXFileClipboard {

void offerRemoteFiles(const std::string &fileListJson, const std::string &text)
{
  auto parsed = FileList::fromJson(fileListJson);
  if (!parsed) {
    LOG_WARN("files: ignoring a bad file list from the other computer");
    return;
  }
  auto list = std::make_shared<FileList>(std::move(*parsed));

  // looked up here rather than on the main thread, as a share may take a moment to answer
  std::vector<std::string> sharedPaths;
  for (const auto &item : list->items) {
    const auto path = pathOnShare(*list, item);
    sharedPaths.push_back(path ? path->string() : std::string());
  }
  LOG_INFO("files: %zu copied on %s, ready to paste", list->items.size(), list->sourceHost.c_str());

  const auto names = text;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      tidyCache(list->offerId);

      NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
      // also keeps Universal Clipboard from fetching the files to other devices
      [pasteboard prepareForNewContentsWithOptions:NSPasteboardContentsCurrentHostOnly];
      [providers() removeAllObjects];

      NSMutableArray *items = [NSMutableArray array];
      for (size_t i = 0; i < list->items.size(); ++i) {
        NSPasteboardItem *item = [[[NSPasteboardItem alloc] init] autorelease];
        if (!sharedPaths[i].empty()) {
          NSString *path = [NSString stringWithUTF8String:sharedPaths[i].c_str()];
          [item setString:[NSURL fileURLWithPath:path isDirectory:list->items[i].isDirectory].absoluteString
                  forType:NSPasteboardTypeFileURL];
        } else {
          DeskflowFileProvider *provider = [[[DeskflowFileProvider alloc] initWithList:list item:i] autorelease];
          [providers() addObject:provider];
          [item setDataProvider:provider forTypes:@[ NSPasteboardTypeFileURL ]];
        }
        // ours, so it isn't read back as a new copy; transient, so clipboard managers leave it alone
        [item setData:[NSData data] forType:kOwnFilesType];
        [item setData:[NSData data] forType:kTransientType];
        if (i == 0 && !names.empty()) {
          [item setString:[NSString stringWithUTF8String:names.c_str()] forType:NSPasteboardTypeString];
        }
        [items addObject:item];
      }

      if (![pasteboard writeObjects:items]) {
        LOG_WARN("files: couldn't write the pasteboard");
      }
    }
  });
}

void clearCache()
{
  std::error_code code;
  fs::remove_all(cacheRoot(), code);
}

} // namespace OSXFileClipboard
