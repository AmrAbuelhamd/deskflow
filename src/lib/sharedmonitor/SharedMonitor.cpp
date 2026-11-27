/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "sharedmonitor/SharedMonitor.h"

#include "base/Log.h"
#include "filetransfer/FileService.h"
#include "sharedmonitor/Follower.h"
#include "sharedmonitor/Platform.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace deskflow::sharedmonitor {

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using filetransfer::FileService;
namespace wire = filetransfer::wire;

constexpr auto kPollInterval = 1s;
constexpr auto kLookAgain = 5s; // while the monitor doesn't say which inputs it has

const char *const kStateHere = "here";
const char *const kStateOther = "other";
const char *const kStateNone = "none";

bool sameOptions(const Options &a, const Options &b)
{
  return a.enabled == b.enabled && a.hereInput == b.hereInput && a.otherInput == b.otherInput &&
         a.recoveryFile == b.recoveryFile;
}

class Service
{
public:
  static Service &instance()
  {
    static Service service;
    return service;
  }

  void start(const Options &options, bool server);
  void stop(bool giveBack);
  void requestSwitch(bool here);
  std::string state() const;
  void setListener(std::function<void(const std::string &)> listener);

private:
  struct Watched
  {
    Follower follower;
    uint16_t here = 0;
    uint16_t other = 0;
    bool off = false; // this computer stopped using the monitor
  };

  void watch(const Options &options);
  void follow(const Options &options);
  void showHere(Watched &watched, const std::string &recoveryFile, bool switchIt);
  void showOther(Watched &watched, uint16_t input, const std::string &recoveryFile, bool switchIt);
  void catchUp(Watched &watched, const std::string &recoveryFile);
  bool waitForScreens();
  bool givesBack() const;
  bool wait(Clock::duration time);
  std::optional<bool> takeSwitch();
  void report(const char *state);

  mutable std::mutex m_mutex;
  std::condition_variable m_wake;
  std::thread m_thread;
  Options m_options;
  bool m_running = false;
  bool m_server = false;
  bool m_stopping = false;
  bool m_giveBack = true;                   // what stopping does: use the monitor as usual again, or leave it (a sleep)
  std::optional<bool> m_switch;             // to this computer (true) or the other one
  std::optional<wire::MonitorState> m_told; // client: what the server said last
  std::string m_state = kStateNone;
  std::function<void(const std::string &)> m_listener;
};

void Service::start(const Options &options, bool server)
{
  {
    std::scoped_lock lock(m_mutex);
    if (m_running && m_server == server && sameOptions(m_options, options)) {
      return; // a client gets the server's options again whenever they change
    }
  }
  stop(true);
  if (!options.enabled) {
    return;
  }
  if (server && !platform::canWatch()) {
    LOG_INFO("shared monitor: this computer can't ask a monitor what it shows");
    return;
  }
  if (server && !FileService::instance().isServer()) {
    LOG_INFO("shared monitor: the other computer won't be told what it shows, as file copying is off");
  }

  std::scoped_lock lock(m_mutex);
  m_options = options;
  m_running = true;
  m_server = server;
  m_stopping = false;
  m_switch.reset();
  m_told.reset();
  if (server) {
    FileService::instance().setMonitorSwitchHandler([this](wire::MonitorTarget target, std::string &) {
      std::scoped_lock guard(m_mutex);
      m_switch = target == wire::MonitorTarget::Server;
      m_wake.notify_all();
      return true;
    });
    m_thread = std::thread([this, options] { watch(options); });
  } else {
    FileService::instance().setMonitorStateHandler([this](wire::MonitorState told) {
      std::scoped_lock guard(m_mutex);
      m_told = told;
      m_wake.notify_all();
    });
    m_thread = std::thread([this, options] { follow(options); });
  }
}

void Service::stop(bool giveBack)
{
  std::thread thread;
  {
    std::scoped_lock lock(m_mutex);
    if (!m_running) {
      return;
    }
    m_stopping = true;
    m_giveBack = giveBack;
    thread = std::move(m_thread);
  }
  m_wake.notify_all();
  FileService::instance().setMonitorSwitchHandler(nullptr);
  FileService::instance().setMonitorStateHandler(nullptr);
  if (thread.joinable()) {
    thread.join(); // it gives the monitor back on its way out, unless this is a sleep
  }
  std::scoped_lock lock(m_mutex);
  m_running = false;
  m_stopping = false;
  m_giveBack = true;
}

void Service::requestSwitch(bool here)
{
  std::scoped_lock lock(m_mutex);
  if (!m_running || m_stopping) {
    LOG_WARN("shared monitor: can't switch the monitor, it isn't watched");
    return;
  }
  m_switch = here;
  m_wake.notify_all();
}

std::string Service::state() const
{
  std::scoped_lock lock(m_mutex);
  return m_state;
}

void Service::setListener(std::function<void(const std::string &)> listener)
{
  std::scoped_lock lock(m_mutex);
  m_listener = std::move(listener);
}

bool Service::givesBack() const
{
  std::scoped_lock lock(m_mutex);
  return m_giveBack;
}

// Waits until this computer's screens are awake; false when it's stopped first
bool Service::waitForScreens()
{
  while (platform::screensAsleep()) {
    if (!wait(kPollInterval)) {
      return false;
    }
    takeSwitch(); // nothing is watched yet
  }
  return true;
}

bool Service::wait(Clock::duration time)
{
  std::unique_lock lock(m_mutex);
  m_wake.wait_for(lock, time, [this] { return m_stopping || m_switch.has_value(); });
  return !m_stopping;
}

std::optional<bool> Service::takeSwitch()
{
  std::scoped_lock lock(m_mutex);
  const auto request = m_switch;
  m_switch.reset();
  return request;
}

void Service::report(const char *state)
{
  std::function<void(const std::string &)> listener;
  {
    std::scoped_lock lock(m_mutex);
    if (m_state == state) {
      return;
    }
    m_state = state;
    listener = m_listener;
  }
  if (listener) {
    listener(state);
  }
}

// The server: asks the monitor what it shows every second, stops using it while it shows
// another input, and tells the clients
void Service::watch(const Options &options)
{
  const auto &file = options.recoveryFile;
  // after a wake the screens may still sleep (a dark wake), and macOS can't change them then
  if (!waitForScreens()) {
    return;
  }
  platform::recover(file);

  uint16_t here = 0;
  uint16_t other = 0;
  for (bool logged = false;;) {
    const auto listed = listedInputs(platform::capabilities());
    if (chooseInputs(listed, options.hereInput, options.otherInput, here, other)) {
      break;
    }
    if (!logged && listed.empty()) {
      LOG_INFO("shared monitor: waiting for a monitor that answers over DDC/CI");
    } else if (!logged) {
      LOG_WARN("shared monitor: can't tell which of the monitor's inputs is this computer's");
    }
    logged = true;
    if (!wait(kLookAgain)) {
      return;
    }
    takeSwitch(); // nothing to switch yet
  }
  LOG_INFO(
      "shared monitor: watching it, this computer is on %s and the other one on %s", inputName(here).c_str(),
      inputName(other).c_str()
  );

  // after a sleep this computer may still have the monitor off, which stays right until it shows
  // this computer
  Watched watched{Follower({.hereInput = here}), here, other, platform::stoppedUsingMonitor()};
  bool asleep = false;
  while (wait(kPollInterval)) {
    // while the screens sleep the monitor stays as it is for macOS; once they wake, catch up
    if (const bool nowAsleep = platform::screensAsleep(); nowAsleep != asleep) {
      asleep = nowAsleep;
      if (!asleep) {
        catchUp(watched, file);
      }
    }
    if (const auto request = takeSwitch(); request) {
      if (*request) {
        showHere(watched, file, true);
      } else {
        showOther(watched, other, file, true);
      }
      continue;
    }
    const auto input = platform::readInput();
    if (const auto shows = watched.follower.onReading(input, Clock::now()); shows) {
      if (*shows == Shows::Here) {
        showHere(watched, file, false);
      } else {
        showOther(watched, *input, file, false);
      }
    }
  }

  if (watched.off && givesBack()) {
    platform::useMonitor(true, file);
  }
  FileService::instance().setMonitorState(wire::MonitorState::Released);
  report(kStateNone);
}

void Service::catchUp(Watched &watched, const std::string &recoveryFile)
{
  const auto shows = watched.follower.shows();
  if (shows == Shows::Here && watched.off && platform::useMonitor(true, recoveryFile)) {
    watched.off = false;
  } else if (shows == Shows::Other && !watched.off && platform::useMonitor(false, recoveryFile)) {
    watched.off = true;
  }
}

void Service::showHere(Watched &watched, const std::string &recoveryFile, bool switchIt)
{
  if (switchIt) {
    if (!platform::writeInput(watched.here)) {
      LOG_WARN("shared monitor: couldn't switch the monitor");
      return;
    }
    watched.follower.onSwitch(true, Clock::now());
  }
  // on before the monitor gets there, so it finds a picture
  if (watched.off && platform::useMonitor(true, recoveryFile)) {
    watched.off = false;
  }
  FileService::instance().setMonitorState(wire::MonitorState::ShowsOther);
  report(kStateHere);
  LOG_INFO("shared monitor: shows this computer");
}

void Service::showOther(Watched &watched, uint16_t input, const std::string &recoveryFile, bool switchIt)
{
  if (switchIt) {
    if (!platform::writeInput(watched.other)) {
      LOG_WARN("shared monitor: couldn't switch the monitor");
      return;
    }
    watched.follower.onSwitch(false, Clock::now());
  }
  // the other computer first, so both switch at the same time
  const bool theOther = input == watched.other;
  FileService::instance().setMonitorState(theOther ? wire::MonitorState::ShowsYou : wire::MonitorState::ShowsOther);
  report(kStateOther);
  LOG_INFO("shared monitor: shows %s", theOther ? "the other computer" : inputName(input).c_str());
  if (!watched.off && platform::useMonitor(false, recoveryFile)) {
    watched.off = true;
  }
}

// A client: does what the server says the monitor shows, and passes on switches from the menu
void Service::follow(const Options &options)
{
  const auto &file = options.recoveryFile;
  platform::recover(file);

  bool off = platform::stoppedUsingMonitor();
  for (;;) {
    std::optional<wire::MonitorState> told;
    std::optional<bool> request;
    {
      std::unique_lock lock(m_mutex);
      m_wake.wait(lock, [this] { return m_stopping || m_told.has_value() || m_switch.has_value(); });
      if (m_stopping) {
        break;
      }
      told = m_told;
      m_told.reset();
      request = m_switch;
      m_switch.reset();
    }

    if (request) {
      std::string error;
      const auto target = *request ? wire::MonitorTarget::Client : wire::MonitorTarget::Server;
      if (!FileService::instance().requestMonitorSwitch(target, error)) {
        LOG_WARN("shared monitor: couldn't switch the monitor: %s", error.c_str());
      }
    }

    if (!told) {
      continue;
    }
    switch (*told) {
    case wire::MonitorState::ShowsOther:
      if (!off && platform::useMonitor(false, file)) {
        off = true;
      }
      report(kStateOther);
      break;
    case wire::MonitorState::ShowsYou:
      if (off && platform::useMonitor(true, file)) {
        off = false;
      }
      report(kStateHere);
      break;
    case wire::MonitorState::Released:
      if (off && platform::useMonitor(true, file)) {
        off = false;
      }
      report(kStateNone);
      break;
    }
  }

  if (off && givesBack()) {
    platform::useMonitor(true, file);
  }
  report(kStateNone);
}

} // namespace

void startServer(const Options &options)
{
  Service::instance().start(options, true);
}

void startClient(const Options &options)
{
  Service::instance().start(options, false);
}

void stop()
{
  Service::instance().stop(true);
}

void pause()
{
  Service::instance().stop(false);
}

void requestSwitch(bool here)
{
  Service::instance().requestSwitch(here);
}

std::string state()
{
  return Service::instance().state();
}

void setStateListener(std::function<void(const std::string &state)> listener)
{
  Service::instance().setListener(std::move(listener));
}

} // namespace deskflow::sharedmonitor
