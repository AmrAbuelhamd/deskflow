# SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
# SPDX-License-Identifier: MIT

# Copying and pasting files between computers
add_library(filetransfer STATIC
  FileList.cpp
  FileList.h
  FileService.cpp
  FileService.h
  Offers.cpp
  Offers.h
  TlsChannel.cpp
  TlsChannel.h
  Wire.cpp
  Wire.h
)

target_link_libraries(filetransfer PUBLIC net base Qt6::Core)

if(WIN32)
  target_link_libraries(filetransfer PRIVATE ws2_32)
endif()
