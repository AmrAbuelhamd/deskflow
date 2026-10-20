/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"

#include <QTest>

class FileListTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void jsonRoundTrip();
  void rejectsUnsafeNames();
  void rejectsMalformedLists();
  void dropsUnsafeShareHint();
  void safeRelativePaths();
  void fileListWireId();
  void unmarshallFileList();
  void unknownFormatIdsAreSkipped();

private:
  Log m_log;
};
