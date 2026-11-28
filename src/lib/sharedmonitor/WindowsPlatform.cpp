/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "sharedmonitor/Platform.h"
#include "sharedmonitor/SharedMonitor.h"

#include "base/Log.h"

#include <Windows.h>
#include <wtsapi32.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace deskflow::sharedmonitor {

namespace {

// Win+P modes: QueryDisplayConfig reports them with the same values SetDisplayConfig takes
constexpr UINT32 kPcScreenOnly = SDC_TOPOLOGY_INTERNAL;
constexpr ULONGLONG kHelperTimeoutMs = 10000;

using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

bool isMode(UINT32 mode)
{
  return mode == SDC_TOPOLOGY_INTERNAL || mode == SDC_TOPOLOGY_CLONE || mode == SDC_TOPOLOGY_EXTEND ||
         mode == SDC_TOPOLOGY_EXTERNAL;
}

// The Win+P mode now; 0 when Windows doesn't say
UINT32 currentMode()
{
  UINT32 pathCount = 0;
  UINT32 modeCount = 0;
  if (GetDisplayConfigBufferSizes(QDC_DATABASE_CURRENT, &pathCount, &modeCount) != ERROR_SUCCESS) {
    return 0;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
  DISPLAYCONFIG_TOPOLOGY_ID topology = {};
  if (QueryDisplayConfig(QDC_DATABASE_CURRENT, &pathCount, paths.data(), &modeCount, modes.data(), &topology) !=
      ERROR_SUCCESS) {
    return 0;
  }
  return static_cast<UINT32>(topology);
}

// "internal": this computer's own screen only. A mode number: back to that mode, but only from
// the computer's own screen as it was left, since the user may have picked a mode since.
bool applyMode(const char *argument, UINT32 &before, LONG &result)
{
  before = currentMode();
  result = ERROR_SUCCESS;
  if (std::strcmp(argument, "internal") == 0) {
    if (before != kPcScreenOnly) {
      result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_INTERNAL);
    }
    return true;
  }
  const auto wanted = static_cast<UINT32>(std::strtoul(argument, nullptr, 10));
  if (!isMode(wanted)) {
    return false;
  }
  if (before == kPcScreenOnly && wanted != kPcScreenOnly) {
    result = SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | wanted);
  }
  return true;
}

std::wstring widen(const std::string &text)
{
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
  return result;
}

// Runs this program as the signed-in user with these arguments and returns what it printed.
// asUser is false when this process doesn't run as SYSTEM, so it can't and needn't.
std::optional<std::string> runAsSignedInUser(const std::wstring &arguments, bool &asUser)
{
  asUser = false;
  DWORD session = 0;
  HANDLE token = nullptr;
  if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || !WTSQueryUserToken(session, &token)) {
    return std::nullopt;
  }
  asUser = true;
  const Handle tokenHandle(token, &CloseHandle);

  SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
  HANDLE readEnd = nullptr;
  HANDLE writeEnd = nullptr;
  if (!CreatePipe(&readEnd, &writeEnd, &inheritable, 0)) {
    LOG_WARN("shared monitor: can't create a pipe, error=%lu", GetLastError());
    return std::nullopt;
  }
  const Handle readHandle(readEnd, &CloseHandle);
  Handle writeHandle(writeEnd, &CloseHandle);
  SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);

  // the helper inherits the pipe and nothing else, none of the core's connections
  SIZE_T listSize = 0;
  InitializeProcThreadAttributeList(nullptr, 1, 0, &listSize);
  std::vector<char> listBuffer(listSize);
  auto *list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(listBuffer.data());
  if (!InitializeProcThreadAttributeList(list, 1, 0, &listSize)) {
    LOG_WARN("shared monitor: can't prepare the display helper, error=%lu", GetLastError());
    return std::nullopt;
  }
  const std::shared_ptr<void> deleteList(nullptr, [list](void *) { DeleteProcThreadAttributeList(list); });
  if (!UpdateProcThreadAttribute(
          list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &writeEnd, sizeof(writeEnd), nullptr, nullptr
      )) {
    LOG_WARN("shared monitor: can't prepare the display helper, error=%lu", GetLastError());
    return std::nullopt;
  }

  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdOutput = writeEnd;
  startup.StartupInfo.lpDesktop = const_cast<wchar_t *>(L"winsta0\\default");
  startup.lpAttributeList = list;

  wchar_t program[MAX_PATH] = {};
  if (GetModuleFileNameW(nullptr, program, MAX_PATH) == 0) {
    return std::nullopt;
  }
  std::wstring commandLine = L"\"" + std::wstring(program) + L"\" " + arguments;

  PROCESS_INFORMATION process{};
  if (!CreateProcessAsUserW(
          token, program, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
          nullptr, nullptr, &startup.StartupInfo, &process
      )) {
    LOG_WARN("shared monitor: can't switch the displays as the signed-in user, error=%lu", GetLastError());
    return std::nullopt;
  }
  CloseHandle(process.hThread);
  const Handle processHandle(process.hProcess, &CloseHandle);
  writeHandle.reset(); // the helper has its own, so the pipe ends when it exits

  std::string output;
  const auto deadline = GetTickCount64() + kHelperTimeoutMs;
  for (;;) {
    DWORD available = 0;
    if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &available, nullptr)) {
      break; // the helper exited and everything was read
    }
    if (available > 0) {
      const auto start = output.size();
      output.resize(start + available);
      DWORD read = 0;
      if (!ReadFile(readEnd, output.data() + start, available, &read, nullptr)) {
        output.resize(start);
        break;
      }
      output.resize(start + read);
      continue;
    }
    if (GetTickCount64() > deadline) {
      TerminateProcess(process.hProcess, 1);
      LOG_WARN("shared monitor: switching the displays took too long");
      return std::nullopt;
    }
    WaitForSingleObject(process.hProcess, 5);
  }
  return output;
}

// Switches the Win+P mode (as the signed-in user when this runs as SYSTEM) and returns the mode
// it was in, 0 when unknown; nothing when switching failed
std::optional<UINT32> switchMode(const std::string &argument)
{
  UINT32 before = 0;
  LONG result = ERROR_SUCCESS;
  bool asUser = false;
  const auto output = runAsSignedInUser(widen(std::string(kDisplayModeArg) + " " + argument), asUser);
  if (asUser) {
    if (!output) {
      return std::nullopt;
    }
    // "<mode before> <result>"
    char *end = nullptr;
    before = static_cast<UINT32>(std::strtoul(output->c_str(), &end, 10));
    if (end == output->c_str()) {
      LOG_WARN("shared monitor: the display helper didn't answer");
      return std::nullopt;
    }
    result = std::strtol(end, nullptr, 10);
  } else if (!applyMode(argument.c_str(), before, result)) {
    return std::nullopt;
  }
  if (result != ERROR_SUCCESS) {
    LOG_WARN("shared monitor: Windows didn't switch the displays, error=%ld", static_cast<long>(result));
    return std::nullopt;
  }
  return before;
}

std::mutex &modeMutex()
{
  static std::mutex mutex;
  return mutex;
}

UINT32 s_before = 0; // the Win+P mode before this computer stopped using the monitor; guarded by modeMutex()

void note(const std::string &file, UINT32 mode)
{
  std::ofstream(file, std::ios::trunc) << mode << "\n";
}

UINT32 noted(const std::string &file)
{
  std::ifstream in(file);
  UINT32 mode = 0;
  if (!(in >> mode) || !isMode(mode)) {
    return 0;
  }
  return mode;
}

void forget(const std::string &file)
{
  std::remove(file.c_str());
}

} // namespace

int runDisplayModeHelper(const char *argument)
{
  UINT32 before = 0;
  LONG result = ERROR_SUCCESS;
  if (!applyMode(argument, before, result)) {
    return 2;
  }
  std::printf("%u %ld\n", static_cast<unsigned>(before), static_cast<long>(result));
  std::fflush(stdout);
  return result == ERROR_SUCCESS ? 0 : 1;
}

namespace platform {

bool canWatch()
{
  return false; // Windows doesn't reach this monitor over DDC/CI; the server watches it
}

std::string capabilities()
{
  return {};
}

std::optional<uint16_t> readInput()
{
  return std::nullopt;
}

bool writeInput(uint16_t)
{
  return false;
}

bool useMonitor(bool use, const std::string &recoveryFile)
{
  std::scoped_lock lock(modeMutex());

  if (use) {
    const auto before = s_before != 0 ? s_before : noted(recoveryFile);
    if (before == 0) {
      return true; // this computer didn't switch away from it
    }
    if (!switchMode(std::to_string(before))) {
      return false;
    }
    s_before = 0;
    forget(recoveryFile);
    LOG_INFO("shared monitor: using the monitor again");
    return true;
  }

  if (s_before != 0) {
    return true;
  }
  const auto before = switchMode("internal");
  if (!before) {
    return false;
  }
  if (*before == 0 || *before == kPcScreenOnly) {
    LOG_INFO("shared monitor: this computer already shows only on its own screen");
    return true;
  }
  s_before = *before;
  note(recoveryFile, s_before);
  LOG_INFO("shared monitor: switched to this computer's own screen");
  return true;
}

bool stoppedUsingMonitor()
{
  std::scoped_lock lock(modeMutex());
  return s_before != 0;
}

void recover(const std::string &recoveryFile)
{
  std::scoped_lock lock(modeMutex());
  if (s_before != 0) {
    return; // this process switched away from it itself and carries on from there
  }
  if (const auto before = noted(recoveryFile); before != 0 && switchMode(std::to_string(before))) {
    LOG_INFO("shared monitor: back to the displays this computer had before it stopped");
    forget(recoveryFile);
  }
}

bool screensAsleep()
{
  return false; // only macOS is known to stall on a display change while its screens sleep
}

} // namespace platform

} // namespace deskflow::sharedmonitor
