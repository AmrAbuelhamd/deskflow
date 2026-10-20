/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

//! Starts and stops the file connection along with the input connection.
//! Call these on Deskflow's event thread, as they read the settings.
namespace deskflow::filetransfer {

//! Server: starts listening for file connections, if TLS is on
void startServer();

//! Server: adds the file connection's port and session token to the options sent to a client.
//! Clients that don't know these options ignore them, and never connect.
void appendOptions(std::vector<uint32_t> &options);

//! Client: connects the file connection if the server's options offer one
void handleOptions(const std::vector<uint32_t> &options, const std::string &serverHost, const std::string &name);

//! Server or client: closes the file connection
void stop();

} // namespace deskflow::filetransfer
