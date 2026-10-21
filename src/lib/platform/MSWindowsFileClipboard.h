/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

//! Files on the Windows clipboard, in both directions
namespace MSWindowsFileClipboard {

//! True if files copied in Explorer are on the clipboard. The clipboard must be open.
bool hasCopiedFiles();

//! The files copied in Explorer as a file list for the other computer, which may then fetch them.
//! Empty if there are none. The clipboard must be open.
std::string copiedFiles();

//! Puts files from the other computer on the clipboard as virtual files, which Explorer fetches
//! when the user pastes; `text` (e.g. their names) goes along for pasting into text fields
bool offerRemoteFiles(const std::string &fileListJson, const std::string &text);

} // namespace MSWindowsFileClipboard
