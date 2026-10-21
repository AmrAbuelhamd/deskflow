/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "filetransfer/Integration.h"

#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/OptionTypes.h"
#include "filetransfer/FileService.h"

namespace deskflow::filetransfer {

namespace {

// option ids are 4 characters packed into an integer, like the ones in OptionTypes.h
constexpr OptionID optionCode(const char (&code)[5])
{
  return (static_cast<OptionID>(static_cast<unsigned char>(code[0])) << 24) |
         (static_cast<OptionID>(static_cast<unsigned char>(code[1])) << 16) |
         (static_cast<OptionID>(static_cast<unsigned char>(code[2])) << 8) |
         static_cast<OptionID>(static_cast<unsigned char>(code[3]));
}

constexpr OptionID kOptionFilePort = optionCode("FXPT");
constexpr OptionID kOptionFileToken[wire::kTokenWords] = {
    optionCode("FXT0"), optionCode("FXT1"), optionCode("FXT2"), optionCode("FXT3")
};

TlsSettings tlsSettings(bool server)
{
  TlsSettings tls;
  tls.certificatePath = Settings::value(Settings::Security::Certificate).toString().toStdString();
  tls.trustedPeersPath =
      (server ? Settings::tlsTrustedClientsDb() : Settings::tlsTrustedServersDb()).toStdString();
  tls.checkPeers = Settings::value(Settings::Security::CheckPeers).toBool();
  return tls;
}

} // namespace

void startServer()
{
  if (!Settings::value(Settings::Security::TlsEnabled).toBool()) {
    LOG_INFO("file copying is off because TLS is off");
    return;
  }

  // next to the input connection's port, or any free port if that one is taken
  const auto inputPort = Settings::value(Settings::Core::Port).toInt();
  const auto port = static_cast<uint16_t>(inputPort > 0 && inputPort < 65535 ? inputPort + 1 : 0);
  std::string error;
  if (!FileService::instance().startServer(tlsSettings(true), port, error)) {
    LOG_WARN("file copying is off: %s", error.c_str());
  }
}

void appendOptions(std::vector<uint32_t> &options)
{
  auto &service = FileService::instance();
  if (!service.isServer()) {
    return;
  }
  options.push_back(kOptionFilePort);
  options.push_back(service.port());
  const auto token = service.token();
  for (size_t i = 0; i < wire::kTokenWords; ++i) {
    options.push_back(kOptionFileToken[i]);
    options.push_back(token[i]);
  }
}

void handleOptions(const std::vector<uint32_t> &options, const std::string &serverHost, const std::string &name)
{
  uint32_t port = 0;
  wire::Token token{};
  size_t tokenWords = 0;
  for (size_t i = 0; i + 1 < options.size(); i += 2) {
    if (options[i] == kOptionFilePort) {
      port = options[i + 1];
      continue;
    }
    for (size_t word = 0; word < wire::kTokenWords; ++word) {
      if (options[i] == kOptionFileToken[word]) {
        token[word] = options[i + 1];
        ++tokenWords;
      }
    }
  }

  if (port == 0 || port > 65535 || tokenWords != wire::kTokenWords) {
    return; // the server doesn't offer file copying
  }
  if (!Settings::value(Settings::Security::TlsEnabled).toBool()) {
    LOG_INFO("file copying is off because TLS is off");
    return;
  }
  FileService::instance().startClient(tlsSettings(false), serverHost, static_cast<uint16_t>(port), token, name);
}

void stop()
{
  FileService::instance().stop();
}

} // namespace deskflow::filetransfer
