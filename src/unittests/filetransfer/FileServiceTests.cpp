/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FileServiceTests.h"

#include "filetransfer/FileService.h"
#include "filetransfer/Offers.h"
#include "filetransfer/Wire.h"
#include "net/FingerprintDatabase.h"
#include "net/SecureUtils.h"

#include <openssl/pem.h>
#include <openssl/x509.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>

#include <filesystem>
#include <functional>
#include <thread>

using namespace deskflow::filetransfer;
namespace fs = std::filesystem;

namespace {

Fingerprint fingerprintOf(const QString &pemPath)
{
  Fingerprint result;
  FILE *file = fopen(pemPath.toLocal8Bit().constData(), "rb");
  if (file == nullptr) {
    return result;
  }
  X509 *certificate = PEM_read_X509(file, nullptr, nullptr, nullptr);
  fclose(file);
  if (certificate != nullptr) {
    result = deskflow::sslCertFingerprint(certificate, QCryptographicHash::Sha256);
    X509_free(certificate);
  }
  return result;
}

void writeFile(const QString &path, const QByteArray &content)
{
  QFile file(path);
  QVERIFY(file.open(QIODevice::WriteOnly));
  file.write(content);
}

std::string readAll(RemoteFile &file)
{
  std::string content;
  char buffer[7]; // small on purpose, to cross chunk boundaries
  size_t count = 0;
  while (file.read(buffer, sizeof(buffer), count) && count > 0) {
    content.append(buffer, count);
  }
  return content;
}

bool waitFor(const std::function<bool()> &condition)
{
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > 5000) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return true;
}

} // namespace

QString FileServiceTests::path(const QString &name) const
{
  return m_dir.filePath(name);
}

void FileServiceTests::initTestCase()
{
  QVERIFY(m_dir.isValid());

  for (const auto *name : {"server.pem", "client.pem", "stranger.pem"}) {
    deskflow::generatePemSelfSignedCert(path(name), 2048);
  }

  // the server trusts the client, the client trusts the server; nobody trusts the stranger
  FingerprintDatabase trustedClients;
  trustedClients.addTrusted(fingerprintOf(path("client.pem")));
  QVERIFY(trustedClients.write(path("trusted-clients")));
  FingerprintDatabase trustedServers;
  trustedServers.addTrusted(fingerprintOf(path("server.pem")));
  QVERIFY(trustedServers.write(path("trusted-servers")));

  m_serverTls = {path("server.pem").toStdString(), path("trusted-clients").toStdString(), true};
  m_clientTls = {path("client.pem").toStdString(), path("trusted-servers").toStdString(), true};
  m_strangerTls = {path("stranger.pem").toStdString(), path("trusted-servers").toStdString(), true};

  // files to offer: a file, and a folder with a file two levels down
  QVERIFY(QDir(m_dir.path()).mkpath("offered/folder/sub"));
  writeFile(path("offered/hello.txt"), "hello from the other computer");
  QByteArray big(3 * 1024 * 1024 + 17, 'x'); // more than one data chunk
  big[0] = 'a';
  big[big.size() - 1] = 'z';
  writeFile(path("offered/folder/sub/big.bin"), big);
  writeFile(path("secret.txt"), "not offered");
}

void FileServiceTests::wireRoundTrip()
{
  const auto data = wire::Writer().u8(7).u16(65535).u32(0xdeadbeef).u64(1ULL << 40).i64(-5).str("hi").data();
  wire::Reader reader(data);
  uint8_t a = 0;
  uint16_t b = 0;
  uint32_t c = 0;
  uint64_t d = 0;
  int64_t e = 0;
  std::string f;
  QVERIFY(reader.u8(a) && reader.u16(b) && reader.u32(c) && reader.u64(d) && reader.i64(e) && reader.str(f));
  QVERIFY(reader.atEnd());
  QCOMPARE(a, uint8_t(7));
  QCOMPARE(b, uint16_t(65535));
  QCOMPARE(c, uint32_t(0xdeadbeef));
  QCOMPARE(d, uint64_t(1) << 40);
  QCOMPARE(e, int64_t(-5));
  QCOMPARE(f, std::string("hi"));
  QVERIFY(!reader.u8(a)); // nothing left
}

void FileServiceTests::offersResolveStaysInside()
{
  const auto folder = fromUtf8(path("offered/folder").toStdString());
  const auto list = Offers::instance().publish({folder}, "macos", "server");
  QCOMPARE(list.items.size(), size_t(1));

  QVERIFY(Offers::instance().resolve(list.offerId, 0, "sub/big.bin").has_value());
  QVERIFY(!Offers::instance().resolve(list.offerId, 0, "../hello.txt").has_value());
  QVERIFY(!Offers::instance().resolve(list.offerId, 0, "/etc/passwd").has_value());
  QVERIFY(!Offers::instance().resolve(list.offerId, 1, "").has_value());
  QVERIFY(!Offers::instance().resolve("not-an-offer", 0, "").has_value());

  // a link inside an offered folder can't lead out of it
  std::error_code error;
  fs::create_symlink(fromUtf8(path("secret.txt").toStdString()), folder / "link.txt", error);
  if (!error) {
    QVERIFY(!Offers::instance().resolve(list.offerId, 0, "link.txt").has_value());
    fs::remove(folder / "link.txt");
  }
}

void FileServiceTests::offersListParentsFirst()
{
  const auto folder = fromUtf8(path("offered/folder").toStdString());
  const auto list = Offers::instance().publish({folder}, "macos", "server");

  // a link inside the folder isn't followed out of it, nor listed
  std::error_code linkError;
  fs::create_directory_symlink(fromUtf8(m_dir.path().toStdString()), folder / "outside", linkError);

  std::vector<std::string> paths;
  std::string error;
  QVERIFY(Offers::instance().list(
      list.offerId, 0,
      [&paths](const Entry &entry) {
        paths.push_back(entry.relativePath);
        return true;
      },
      error
  ));
  QCOMPARE(paths, (std::vector<std::string>{"", "sub", "sub/big.bin"}));
  if (!linkError) {
    QVERIFY(!Offers::instance().resolve(list.offerId, 0, "outside/secret.txt").has_value());
    fs::remove(folder / "outside");
  }
}

void FileServiceTests::clientFetchesFromServer()
{
  FileService server;
  FileService client;
  std::string error;
  QVERIFY2(server.startServer(m_serverTls, 0, error), error.c_str());
  client.startClient(m_clientTls, "127.0.0.1", server.port(), server.token(), "client");

  const auto list = Offers::instance().publish(
      {fromUtf8(path("offered/hello.txt").toStdString()), fromUtf8(path("offered/folder").toStdString())}, "macos",
      "server"
  );

  std::vector<Entry> entries;
  QVERIFY2(client.list(list, 0, entries, error), error.c_str());
  QCOMPARE(entries.size(), size_t(1));
  QCOMPARE(entries[0].size, uint64_t(29));

  auto file = client.open(list, 0, "", 0, error);
  QVERIFY2(file != nullptr, error.c_str());
  QCOMPARE(readAll(*file), std::string("hello from the other computer"));

  QVERIFY2(client.list(list, 1, entries, error), error.c_str());
  QCOMPARE(entries.size(), size_t(3));
  QVERIFY(entries[1].isDirectory);

  // big enough for several data chunks, read through a reused connection
  file = client.open(list, 1, "sub/big.bin", 0, error);
  QVERIFY2(file != nullptr, error.c_str());
  const auto big = readAll(*file);
  QCOMPARE(big.size(), size_t(3 * 1024 * 1024 + 17));
  QCOMPARE(big.front(), 'a');
  QCOMPARE(big.back(), 'z');

  // resuming part-way
  file = client.open(list, 0, "", 6, error);
  QVERIFY2(file != nullptr, error.c_str());
  QCOMPARE(readAll(*file), std::string("from the other computer"));
}

void FileServiceTests::serverFetchesFromClient()
{
  FileService server;
  FileService client;
  std::string error;
  QVERIFY2(server.startServer(m_serverTls, 0, error), error.c_str());
  client.startClient(m_clientTls, "127.0.0.1", server.port(), server.token(), "client");
  QVERIFY(waitFor([&server] { return server.isConnected("client"); }));

  // the files are on the client this time; the server asks it to connect back
  const auto list = Offers::instance().publish({fromUtf8(path("offered/hello.txt").toStdString())}, "windows", "client");
  std::vector<Entry> entries;
  QVERIFY2(server.list(list, 0, entries, error), error.c_str());
  QCOMPARE(entries.size(), size_t(1));

  auto file = server.open(list, 0, "", 0, error);
  QVERIFY2(file != nullptr, error.c_str());
  QCOMPARE(readAll(*file), std::string("hello from the other computer"));
}

void FileServiceTests::unofferedFilesAreRefused()
{
  FileService server;
  FileService client;
  std::string error;
  QVERIFY2(server.startServer(m_serverTls, 0, error), error.c_str());
  client.startClient(m_clientTls, "127.0.0.1", server.port(), server.token(), "client");

  FileList list;
  list.offerId = "made-up";
  list.sourceHost = "server";
  list.items.push_back({.name = "secret.txt"});
  std::vector<Entry> entries;
  QVERIFY(!client.list(list, 0, entries, error));

  auto file = client.open(list, 0, "", 0, error);
  QVERIFY(file != nullptr); // the refusal arrives with the first read
  size_t count = 0;
  char buffer[16];
  QVERIFY(!file->read(buffer, sizeof(buffer), count));
}

void FileServiceTests::untrustedComputerIsRefused()
{
  FileService server;
  FileService stranger;
  std::string error;
  QVERIFY2(server.startServer(m_serverTls, 0, error), error.c_str());
  stranger.startClient(m_strangerTls, "127.0.0.1", server.port(), server.token(), "stranger");

  const auto list = Offers::instance().publish({fromUtf8(path("offered/hello.txt").toStdString())}, "macos", "server");
  std::vector<Entry> entries;
  QVERIFY(!stranger.list(list, 0, entries, error));
  QVERIFY(!server.isConnected("stranger"));
}

void FileServiceTests::wrongSessionIsRefused()
{
  FileService server;
  FileService client;
  std::string error;
  QVERIFY2(server.startServer(m_serverTls, 0, error), error.c_str());
  client.startClient(m_clientTls, "127.0.0.1", server.port(), {1, 2, 3, 4}, "client");

  const auto list = Offers::instance().publish({fromUtf8(path("offered/hello.txt").toStdString())}, "macos", "server");
  std::vector<Entry> entries;
  QVERIFY(!client.list(list, 0, entries, error));
  QCOMPARE(error, std::string("wrong session"));
}

QTEST_MAIN(FileServiceTests)
