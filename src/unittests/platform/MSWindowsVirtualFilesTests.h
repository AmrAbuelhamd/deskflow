/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class MSWindowsVirtualFilesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void cleanupTestCase();
  void windowsNames();
  void describesFiles();
  void streamsContent();
  void seeksByReopening();
  void offersOwnershipAndOptOuts();
  void offersText();
  void refusesUnknownFormats();

private:
  Log m_log;
};
