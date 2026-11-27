/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "sharedmonitor/Platform.h"

#include "base/Log.h"

#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>

namespace deskflow::sharedmonitor::platform {

namespace {

using namespace std::chrono_literals;

// DDC/CI goes over the DisplayPort AUX channel through the display's AV service, and a display
// can be taken away from macOS without unplugging it. Neither has a public API, so both are
// looked up when first needed; a macOS without them has the feature off.
using AVServiceRef = CFTypeRef;
using CreateWithServiceFn = AVServiceRef (*)(CFAllocatorRef, io_service_t);
using I2CFn = IOReturn (*)(AVServiceRef, uint32_t, uint32_t, void *, uint32_t);
using ConfigureEnabledFn = CGError (*)(CGDisplayConfigRef, CGDirectDisplayID, bool);

struct Api
{
  CreateWithServiceFn createWithService = nullptr;
  I2CFn readI2C = nullptr;
  I2CFn writeI2C = nullptr;
  ConfigureEnabledFn configureEnabled = nullptr;
};

const Api &api()
{
  static const Api loaded = [] {
    Api result;
    if (void *iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY)) {
      result.createWithService = reinterpret_cast<CreateWithServiceFn>(dlsym(iokit, "IOAVServiceCreateWithService"));
      result.readI2C = reinterpret_cast<I2CFn>(dlsym(iokit, "IOAVServiceReadI2C"));
      result.writeI2C = reinterpret_cast<I2CFn>(dlsym(iokit, "IOAVServiceWriteI2C"));
    }
    result.configureEnabled = reinterpret_cast<ConfigureEnabledFn>(dlsym(RTLD_DEFAULT, "CGSConfigureDisplayEnabled"));
    return result;
  }();
  return loaded;
}

// DDC/CI (VESA): requests go to I2C address 0x37, starting with the host's address 0x51; the
// checksum is the XOR of every byte including the monitor's write address 0x6E, and a reply's
// starts from the host's virtual address 0x50
constexpr uint32_t kDdcChip = 0x37;
constexpr uint32_t kDdcHost = 0x51;
constexpr uint8_t kDdcRequestChecksum = 0x6E ^ 0x51;
constexpr uint8_t kDdcReplyChecksum = 0x50;
constexpr uint8_t kVcpInput = 0x60;
constexpr auto kDdcReplyDelay = 50ms;
constexpr int kDdcTries = 3;

// how long macOS may take to change the built-in screen's resolution after the monitor goes
constexpr auto kModeChangeWait = 3s;

std::mutex &displayMutex()
{
  static std::mutex mutex;
  return mutex;
}

CGDirectDisplayID s_turnedOff = kCGNullDirectDisplay; // guarded by displayMutex()

uint8_t checksum(uint8_t start, const uint8_t *bytes, size_t count)
{
  for (size_t i = 0; i < count; ++i) {
    start ^= bytes[i];
  }
  return start;
}

// The external display's AV service, or none when there isn't exactly one external display.
// Opened for every use, because macOS replaces it whenever the monitor is plugged in again,
// which it does when the monitor switches back to this computer's input.
AVServiceRef openService()
{
  if (api().createWithService == nullptr || api().readI2C == nullptr || api().writeI2C == nullptr) {
    return nullptr;
  }
  io_iterator_t services = IO_OBJECT_NULL;
  if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("DCPAVServiceProxy"), &services) !=
      KERN_SUCCESS) {
    return nullptr;
  }
  AVServiceRef found = nullptr;
  int externals = 0;
  for (io_service_t service = IOIteratorNext(services); service != IO_OBJECT_NULL; service = IOIteratorNext(services)) {
    if (CFTypeRef location = IORegistryEntryCreateCFProperty(service, CFSTR("Location"), kCFAllocatorDefault, 0)) {
      if (CFGetTypeID(location) == CFStringGetTypeID() &&
          CFStringCompare(static_cast<CFStringRef>(location), CFSTR("External"), 0) == kCFCompareEqualTo) {
        ++externals;
        if (found == nullptr) {
          found = api().createWithService(kCFAllocatorDefault, service);
        }
      }
      CFRelease(location);
    }
    IOObjectRelease(service);
  }
  IOObjectRelease(services);
  if (externals > 1 && found != nullptr) {
    CFRelease(found); // which one is shared can't be told apart; more than one isn't supported
    return nullptr;
  }
  return found;
}

struct Service
{
  AVServiceRef ref = openService();
  ~Service()
  {
    if (ref != nullptr) {
      CFRelease(ref);
    }
  }
  Service() = default;
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;
};

bool send(AVServiceRef service, const uint8_t *request, uint32_t size)
{
  return api().writeI2C(service, kDdcChip, kDdcHost, const_cast<uint8_t *>(request), size) == kIOReturnSuccess;
}

bool receive(AVServiceRef service, uint8_t *reply, uint32_t size)
{
  return api().readI2C(service, kDdcChip, kDdcHost, reply, size) == kIOReturnSuccess;
}

std::optional<uint8_t> readVcpLow(AVServiceRef service, uint8_t code)
{
  uint8_t request[] = {0x82, 0x01, code, 0};
  request[3] = checksum(kDdcRequestChecksum, request, 3);
  for (int attempt = 0; attempt < kDdcTries; ++attempt) {
    if (attempt > 0) {
      std::this_thread::sleep_for(40ms);
    }
    if (!send(service, request, sizeof(request))) {
      continue;
    }
    std::this_thread::sleep_for(kDdcReplyDelay);
    uint8_t reply[11] = {};
    if (!receive(service, reply, sizeof(reply))) {
      continue;
    }
    // source, length, "VCP reply", result (0: supported), code, type, max (2), value (2), checksum
    if (checksum(kDdcReplyChecksum, reply, 10) == reply[10] && reply[2] == 0x02 && reply[3] == 0x00 &&
        reply[4] == code) {
      return reply[9];
    }
  }
  return std::nullopt;
}

CGDirectDisplayID builtinDisplay()
{
  CGDirectDisplayID ids[16];
  uint32_t count = 0;
  if (CGGetOnlineDisplayList(16, ids, &count) != kCGErrorSuccess) {
    return kCGNullDirectDisplay;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (CGDisplayIsBuiltin(ids[i])) {
      return ids[i];
    }
  }
  return kCGNullDirectDisplay;
}

// The one display that isn't built in; none when there's none or more than one
CGDirectDisplayID externalDisplay()
{
  CGDirectDisplayID ids[16];
  uint32_t count = 0;
  if (CGGetOnlineDisplayList(16, ids, &count) != kCGErrorSuccess) {
    return kCGNullDirectDisplay;
  }
  CGDirectDisplayID external = kCGNullDirectDisplay;
  int externals = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!CGDisplayIsBuiltin(ids[i])) {
      external = ids[i];
      ++externals;
    }
  }
  return externals == 1 ? external : kCGNullDirectDisplay;
}

bool isOnline(CGDirectDisplayID display)
{
  CGDirectDisplayID ids[16];
  uint32_t count = 0;
  if (CGGetOnlineDisplayList(16, ids, &count) != kCGErrorSuccess) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    if (ids[i] == display) {
      return true;
    }
  }
  return false;
}

CGError setEnabled(CGDirectDisplayID display, bool enabled)
{
  CGDisplayConfigRef config = nullptr;
  if (const auto error = CGBeginDisplayConfiguration(&config); error != kCGErrorSuccess) {
    return error;
  }
  if (const auto error = api().configureEnabled(config, display, enabled); error != kCGErrorSuccess) {
    CGCancelDisplayConfiguration(config);
    return error;
  }
  return CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
}

struct Mode
{
  size_t width = 0;
  size_t height = 0;
  size_t pixelWidth = 0;
  size_t pixelHeight = 0;
  double refreshRate = 0;

  bool sameSize(const Mode &other) const
  {
    return width == other.width && height == other.height && pixelWidth == other.pixelWidth &&
           pixelHeight == other.pixelHeight;
  }
};

std::optional<Mode> currentMode(CGDirectDisplayID display)
{
  CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
  if (mode == nullptr) {
    return std::nullopt;
  }
  const Mode result{
      CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode), CGDisplayModeGetPixelWidth(mode),
      CGDisplayModeGetPixelHeight(mode), CGDisplayModeGetRefreshRate(mode)
  };
  CGDisplayModeRelease(mode);
  return result;
}

// When the monitor goes, macOS gives the built-in screen the resolution it last had on its own;
// this puts back the one it had with the monitor
void keepMode(CGDirectDisplayID display, const Mode &wanted)
{
  const auto deadline = std::chrono::steady_clock::now() + kModeChangeWait;
  std::optional<Mode> now = currentMode(display);
  while (now && now->sameSize(wanted) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(100ms);
    now = currentMode(display);
  }
  if (!now || now->sameSize(wanted)) {
    return;
  }

  const void *keys[] = {kCGDisplayShowDuplicateLowResolutionModes};
  const void *values[] = {kCFBooleanTrue};
  CFDictionaryRef options =
      CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, options);
  if (options != nullptr) {
    CFRelease(options);
  }
  if (modes == nullptr) {
    return;
  }
  CGDisplayModeRef best = nullptr;
  for (CFIndex i = 0; i < CFArrayGetCount(modes); ++i) {
    auto mode = static_cast<CGDisplayModeRef>(const_cast<void *>(CFArrayGetValueAtIndex(modes, i)));
    const Mode candidate{
        CGDisplayModeGetWidth(mode), CGDisplayModeGetHeight(mode), CGDisplayModeGetPixelWidth(mode),
        CGDisplayModeGetPixelHeight(mode), CGDisplayModeGetRefreshRate(mode)
    };
    if (candidate.sameSize(wanted) && (best == nullptr || candidate.refreshRate == wanted.refreshRate)) {
      best = mode;
    }
  }
  CGDisplayConfigRef config = nullptr;
  if (best != nullptr && CGBeginDisplayConfiguration(&config) == kCGErrorSuccess) {
    CGConfigureDisplayWithDisplayMode(config, display, best, nullptr);
    const auto error = CGCompleteDisplayConfiguration(config, kCGConfigureForSession);
    if (error == kCGErrorSuccess) {
      LOG_DEBUG("shared monitor: kept the built-in screen at %zux%zu", wanted.width, wanted.height);
    } else {
      LOG_WARN("shared monitor: couldn't keep the built-in screen's resolution, error %d", error);
    }
  }
  CFRelease(modes);
}

void note(const std::string &file, CGDirectDisplayID display)
{
  std::ofstream(file, std::ios::trunc) << display << "\n";
}

CGDirectDisplayID noted(const std::string &file)
{
  std::ifstream in(file);
  CGDirectDisplayID display = kCGNullDirectDisplay;
  if (!(in >> display)) {
    return kCGNullDirectDisplay;
  }
  return display;
}

void forget(const std::string &file)
{
  std::remove(file.c_str());
}

// macOS can't change its displays while they sleep: it waits for a reconfiguration that never
// finishes and gives up after about 10 s, and everything waiting on that waits too
bool asleep()
{
  const auto main = CGMainDisplayID();
  return CGDisplayIsAsleep(main) || !CGDisplayIsActive(main);
}

// Turns a display back on; true once it's on
bool turnOn(CGDirectDisplayID display)
{
  if (isOnline(display)) {
    return true;
  }
  if (api().configureEnabled == nullptr) {
    return false;
  }
  if (asleep()) {
    LOG_DEBUG("shared monitor: the screens sleep, the monitor goes back on for macOS once they wake");
    return false;
  }
  if (const auto error = setEnabled(display, true); error != kCGErrorSuccess) {
    LOG_WARN("shared monitor: couldn't turn the monitor back on for macOS, error %d", error);
    return false;
  }
  LOG_INFO("shared monitor: turned the monitor back on for macOS");
  return true;
}

} // namespace

bool canWatch()
{
  const auto &calls = api();
  return calls.createWithService != nullptr && calls.readI2C != nullptr && calls.writeI2C != nullptr &&
         calls.configureEnabled != nullptr;
}

std::string capabilities()
{
  const Service service;
  if (service.ref == nullptr) {
    return {};
  }
  std::string text;
  for (uint16_t offset = 0; offset < 4096;) {
    uint8_t request[] = {0x83, 0xF3, static_cast<uint8_t>(offset >> 8), static_cast<uint8_t>(offset & 0xFF), 0};
    request[4] = checksum(kDdcRequestChecksum, request, 4);
    bool got = false;
    for (int attempt = 0; attempt < kDdcTries && !got; ++attempt) {
      uint8_t reply[40] = {};
      if (!send(service.ref, request, sizeof(request))) {
        std::this_thread::sleep_for(40ms);
        continue;
      }
      std::this_thread::sleep_for(kDdcReplyDelay);
      if (!receive(service.ref, reply, sizeof(reply))) {
        continue;
      }
      // source, length, "capabilities reply", offset (2), up to 32 bytes, checksum
      const size_t length = reply[1] & 0x7F;
      if (reply[2] != 0xE3 || length < 3 || length > 35 ||
          checksum(kDdcReplyChecksum, reply, 2 + length) != reply[2 + length]) {
        continue;
      }
      const size_t bytes = length - 3;
      if (bytes == 0) {
        return text; // the end
      }
      text.append(reinterpret_cast<const char *>(reply + 5), bytes);
      offset = static_cast<uint16_t>(offset + bytes);
      got = true;
    }
    if (!got) {
      break; // a monitor that stops answering halfway leaves what came so far
    }
  }
  return text;
}

std::optional<uint16_t> readInput()
{
  const Service service;
  if (service.ref == nullptr) {
    return std::nullopt;
  }
  // the input is in the value's low byte; some monitors put other bits in the high one
  if (const auto value = readVcpLow(service.ref, kVcpInput); value && *value != 0) {
    return *value;
  }
  return std::nullopt;
}

bool writeInput(uint16_t input)
{
  const Service service;
  if (service.ref == nullptr) {
    return false;
  }
  uint8_t request[] = {0x84, 0x03, kVcpInput, static_cast<uint8_t>(input >> 8), static_cast<uint8_t>(input & 0xFF), 0};
  request[5] = checksum(kDdcRequestChecksum, request, 5);
  return send(service.ref, request, sizeof(request));
}

bool useMonitor(bool use, const std::string &recoveryFile)
{
  std::scoped_lock lock(displayMutex());

  if (use) {
    const auto display = s_turnedOff != kCGNullDirectDisplay ? s_turnedOff : noted(recoveryFile);
    if (display == kCGNullDirectDisplay) {
      return true; // this computer didn't turn it off
    }
    if (!turnOn(display)) {
      return false;
    }
    s_turnedOff = kCGNullDirectDisplay;
    forget(recoveryFile);
    return true;
  }

  if (s_turnedOff != kCGNullDirectDisplay) {
    return true;
  }
  if (api().configureEnabled == nullptr) {
    LOG_WARN("shared monitor: this version of macOS can't turn a display off");
    return false;
  }
  if (asleep()) {
    LOG_DEBUG("shared monitor: the screens sleep, the monitor goes off for macOS once they wake");
    return false;
  }
  const auto external = externalDisplay();
  if (external == kCGNullDirectDisplay) {
    LOG_WARN("shared monitor: no single external display to turn off");
    return false;
  }
  const auto builtin = builtinDisplay();
  const auto mode = builtin != kCGNullDirectDisplay ? currentMode(builtin) : std::nullopt;

  note(recoveryFile, external); // first, so it's undone even after a crash right after
  if (const auto error = setEnabled(external, false); error != kCGErrorSuccess) {
    forget(recoveryFile);
    LOG_WARN("shared monitor: couldn't turn the monitor off for macOS, error %d", error);
    return false;
  }
  s_turnedOff = external;
  LOG_INFO("shared monitor: turned the monitor off for macOS");
  if (mode) {
    keepMode(builtin, *mode);
  }
  return true;
}

bool stoppedUsingMonitor()
{
  std::scoped_lock lock(displayMutex());
  return s_turnedOff != kCGNullDirectDisplay;
}

void recover(const std::string &recoveryFile)
{
  std::scoped_lock lock(displayMutex());
  if (s_turnedOff != kCGNullDirectDisplay) {
    return; // this process turned it off before a sleep, and the watcher carries on from there
  }
  const auto display = noted(recoveryFile);
  if (display == kCGNullDirectDisplay) {
    return;
  }
  LOG_INFO("shared monitor: the monitor was left off for macOS, turning it back on");
  if (turnOn(display)) {
    forget(recoveryFile);
  }
}

bool screensAsleep()
{
  return asleep();
}

} // namespace deskflow::sharedmonitor::platform
