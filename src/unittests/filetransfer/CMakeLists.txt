# SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs platform version)
endif()

create_test(
  NAME FileListTests
  DEPENDS filetransfer
  LIBS app arch base io ${extra_libs}
  SOURCE FileListTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/filetransfer"
)
