# SPDX-FileCopyrightText: (C) 2026 Amr Mohamed
# SPDX-License-Identifier: MIT

# One monitor cabled to two computers: the one it doesn't show stops using it
set(sharedmonitor_sources
  Follower.cpp
  Follower.h
  Integration.cpp
  Integration.h
  Platform.h
  SharedMonitor.cpp
  SharedMonitor.h
)

if(APPLE)
  list(APPEND sharedmonitor_sources MacPlatform.cpp)
elseif(WIN32)
  list(APPEND sharedmonitor_sources WindowsPlatform.cpp)
else()
  list(APPEND sharedmonitor_sources OtherPlatform.cpp)
endif()

add_library(sharedmonitor STATIC ${sharedmonitor_sources})

target_link_libraries(sharedmonitor PUBLIC filetransfer common base Qt6::Core)

if(APPLE)
  target_link_libraries(sharedmonitor PRIVATE "-framework ApplicationServices" "-framework IOKit" "-framework CoreFoundation")
elseif(WIN32)
  target_link_libraries(sharedmonitor PRIVATE user32 wtsapi32 advapi32)
endif()
