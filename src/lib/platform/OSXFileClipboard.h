/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

//! Files from the other computer on the Mac pasteboard
namespace OSXFileClipboard {

//! Puts files from the other computer on the pasteboard, for Finder to paste. A file in a shared
//! folder this Mac has mounted is offered at its real path, so Finder copies it from the share
//! itself; anything else is fetched when Finder asks for it, into a cache Finder then clones from.
//! `text` (their names) goes along for pasting into text fields. Returns at once; the pasteboard
//! is written on the main thread.
void offerRemoteFiles(const std::string &fileListJson, const std::string &text);

//! Removes what earlier pastes left in the cache
void clearCache();

} // namespace OSXFileClipboard
