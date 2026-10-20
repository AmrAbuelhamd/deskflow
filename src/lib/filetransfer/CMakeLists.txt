# SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
# SPDX-License-Identifier: MIT

# Copying and pasting files between computers
add_library(filetransfer STATIC
  FileList.cpp
  FileList.h
)

target_link_libraries(filetransfer PUBLIC Qt6::Core)
