/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FollowerTests.h"

#include "sharedmonitor/Follower.h"

using namespace deskflow::sharedmonitor;
using namespace std::chrono_literals;

namespace {

// what a monitor with DisplayPort and HDMI inputs answers
const std::string kCapabilities =
    "(prot(monitor)type(LCD)model(Example)cmds(01 02 03 07 0C E3 F3)vcp(04 10 12 14(05 06 08 0B) 16 18 1A  "
    "60(0F 11) 62 8D CC(01 02 03 04 05 06 07 08 09 0a 0c 0d 0e 14 16 1E) D6(01 02 04 05) E0(00 04 05)) "
    "mswhql(1)asset_eep(40)mccs_ver(2.2))";

const auto kStart = Follower::Clock::time_point{} + 100s;

int shown(std::optional<Shows> shows)
{
  return shows ? static_cast<int>(*shows) : -1;
}

} // namespace

void FollowerTests::listsInputs()
{
  QVERIFY(listedInputs(kCapabilities) == (std::vector<uint16_t>{kInputDisplayPort1, kInputHdmi1}));
  QVERIFY(listedInputs("VCP(10 60( 11 12 0f ) 62)") == (std::vector<uint16_t>{kInputHdmi1, kInputHdmi2, 0x0F}));
  QVERIFY(listedInputs("vcp(10 12 14(05 06))").empty());
  QVERIFY(listedInputs("vcpname(60(Input)) no list").empty());
  QVERIFY(listedInputs("").empty());
}

void FollowerTests::choosesInputs()
{
  uint16_t here = 0;
  uint16_t other = 0;

  // this computer's: the DisplayPort input; the other's: the next one listed
  QVERIFY(chooseInputs(listedInputs(kCapabilities), 0, 0, here, other));
  QCOMPARE(int(here), int(kInputDisplayPort1));
  QCOMPARE(int(other), int(kInputHdmi1));

  QVERIFY(chooseInputs({kInputHdmi1, kInputUsbC}, 0, 0, here, other));
  QCOMPARE(int(here), int(kInputUsbC));
  QCOMPARE(int(other), int(kInputHdmi1));

  // the settings win
  QVERIFY(chooseInputs({kInputHdmi1, kInputHdmi2}, kInputHdmi2, 0, here, other));
  QCOMPARE(int(here), int(kInputHdmi2));
  QCOMPARE(int(other), int(kInputHdmi1));
  QVERIFY(chooseInputs({}, kInputDisplayPort1, kInputHdmi1, here, other));

  // nothing to go by
  QVERIFY(!chooseInputs({kInputHdmi1, kInputHdmi2}, 0, 0, here, other));
  QVERIFY(!chooseInputs({}, 0, 0, here, other));
  QVERIFY(!chooseInputs({kInputDisplayPort1}, 0, 0, here, other));
}

void FollowerTests::namesInputs()
{
  QCOMPARE(inputName(kInputDisplayPort1), std::string("DisplayPort-1"));
  QCOMPARE(inputName(kInputHdmi1), std::string("HDMI-1"));
  QCOMPARE(inputName(0x42), std::string("input 0x42"));
}

void FollowerTests::needsMatchingAnswers()
{
  Follower follower({.hereInput = kInputDisplayPort1});
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart)), -1);
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 1s)), static_cast<int>(Shows::Here));
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 2s)), -1); // no change

  // an answer that differs starts a new run
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 5s)), -1);
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 6s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 7s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 8s)), static_cast<int>(Shows::Other));
  QCOMPARE(static_cast<int>(follower.shows()), static_cast<int>(Shows::Other));
}

void FollowerTests::missingAnswersDontCount()
{
  Follower follower({.hereInput = kInputDisplayPort1});
  QCOMPARE(shown(follower.onReading(std::nullopt, kStart)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 1s)), -1);
  QCOMPARE(shown(follower.onReading(std::nullopt, kStart + 2s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 3s)), static_cast<int>(Shows::Other));
  QCOMPARE(shown(follower.onReading(std::nullopt, kStart + 10s)), -1);
  QCOMPARE(static_cast<int>(follower.shows()), static_cast<int>(Shows::Other));
}

void FollowerTests::settlesAfterAChange()
{
  Follower follower({.hereInput = kInputDisplayPort1});
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart)), -1);
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 1s)), static_cast<int>(Shows::Here));

  // the other input right after: not before 3 s have passed since the last change
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 2s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 3s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 4s)), static_cast<int>(Shows::Other));
}

void FollowerTests::requestedSwitchCountsAtOnce()
{
  Follower follower({.hereInput = kInputDisplayPort1});
  follower.onReading(kInputDisplayPort1, kStart);
  follower.onReading(kInputDisplayPort1, kStart + 1s);

  follower.onSwitch(false, kStart + 10s);
  QCOMPARE(static_cast<int>(follower.shows()), static_cast<int>(Shows::Other));

  // the monitor takes a few seconds: what it showed before doesn't undo the switch
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 11s)), -1);
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 12s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 13s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 14s)), -1);
  QCOMPARE(static_cast<int>(follower.shows()), static_cast<int>(Shows::Other));

  // once it got there, switching back by hand counts as usual
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 20s)), -1);
  QCOMPARE(shown(follower.onReading(kInputDisplayPort1, kStart + 21s)), static_cast<int>(Shows::Here));
}

void FollowerTests::switchThatDoesntHappenIsUndone()
{
  Follower follower({.hereInput = kInputDisplayPort1});
  follower.onReading(kInputHdmi1, kStart);
  follower.onReading(kInputHdmi1, kStart + 1s);

  follower.onSwitch(true, kStart + 10s);
  QCOMPARE(static_cast<int>(follower.shows()), static_cast<int>(Shows::Here));
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 11s)), -1);
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 17s)), -1);

  // the switch time is over and the monitor still shows the other input
  QCOMPARE(shown(follower.onReading(kInputHdmi1, kStart + 19s)), static_cast<int>(Shows::Other));
}

QTEST_MAIN(FollowerTests)
