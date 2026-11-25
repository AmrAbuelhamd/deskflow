/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "sharedmonitor/SharedMonitor.h"

namespace deskflow::sharedmonitor {

//! The shared monitor options in Deskflow's settings
Options optionsFromSettings();

} // namespace deskflow::sharedmonitor
