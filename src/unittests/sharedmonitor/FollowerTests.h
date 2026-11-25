/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class FollowerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void listsInputs();
  void choosesInputs();
  void namesInputs();
  void needsMatchingAnswers();
  void missingAnswersDontCount();
  void settlesAfterAChange();
  void requestedSwitchCountsAtOnce();
  void switchThatDoesntHappenIsUndone();
};
