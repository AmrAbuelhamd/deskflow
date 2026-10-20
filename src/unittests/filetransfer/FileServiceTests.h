/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "base/Log.h"
#include "filetransfer/TlsChannel.h"

#include <QTemporaryDir>
#include <QTest>

class FileServiceTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void initTestCase();
  void wireRoundTrip();
  void offersResolveStaysInside();
  void offersListParentsFirst();
  void clientFetchesFromServer();
  void serverFetchesFromClient();
  void unofferedFilesAreRefused();
  void untrustedComputerIsRefused();
  void wrongSessionIsRefused();

private:
  QString path(const QString &name) const;

  Log m_log;
  QTemporaryDir m_dir;
  deskflow::filetransfer::TlsSettings m_serverTls;
  deskflow::filetransfer::TlsSettings m_clientTls;
  deskflow::filetransfer::TlsSettings m_strangerTls;
};
