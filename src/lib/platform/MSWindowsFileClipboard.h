/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

//! Files on the Windows clipboard, in both directions
namespace MSWindowsFileClipboard {

//! Started with this argument, the core prints the files copied in Explorer and exits. The core
//! running as SYSTEM starts a copy of itself this way, as the signed-in user, to see them.
inline constexpr char kPrintCopiedFilesArg[] = "--print-copied-files";

//! Prints the paths of the files copied in Explorer, one per line, in UTF-8. Returns the exit code.
int printCopiedFiles();

//! Windows hides files copied in Explorer from the core when it runs as SYSTEM, though not text or
//! pictures. If the clipboard looks empty here, this reads it as the signed-in user, for
//! hasCopiedFiles() and copiedFiles(). The clipboard must not be open.
void prepareCopiedFiles();

//! True if files copied in Explorer are on the clipboard. The clipboard must be open.
bool hasCopiedFiles();

//! The files copied in Explorer as a file list for the other computer, which may then fetch them.
//! Empty if there are none. The clipboard must be open.
std::string copiedFiles();

//! Puts files from the other computer on the clipboard as virtual files, which Explorer fetches
//! when the user pastes; `text` (e.g. their names) goes along for pasting into text fields
bool offerRemoteFiles(const std::string &fileListJson, const std::string &text);

} // namespace MSWindowsFileClipboard
