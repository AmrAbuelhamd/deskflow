# SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
# SPDX-License-Identifier: MIT

create_test(
  NAME FollowerTests
  DEPENDS sharedmonitor
  SOURCE FollowerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/sharedmonitor"
)
