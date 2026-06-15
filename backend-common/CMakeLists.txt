# Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
#
# backend-common: shared native-API implementations that the CEF and
# webview backends both link against. Each backend pre-parses the
# backend-specific laufey_value_t into the plain structs declared in
# laufey_backend_common.h, then calls into the functions defined here.
#
# Parallels backend-winit-common/, which serves the same role for the
# winit-based backends (winit + servo).

cmake_minimum_required(VERSION 3.19)

# On macOS the parent project may set CMAKE_OSX_DEPLOYMENT_TARGET via CEF's
# cmake config. Honor that target so the static lib matches the executable
# linker target and we don't get "built for newer macOS" warnings.
if(APPLE AND DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(_inherited_target "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()

if(APPLE)
  project(laufey_backend_common VERSION 1.0.0 LANGUAGES C CXX OBJCXX)
else()
  project(laufey_backend_common VERSION 1.0.0 LANGUAGES C CXX)
endif()

if(APPLE AND DEFINED _inherited_target)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "${_inherited_target}" CACHE STRING
      "Minimum OS X deployment version" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAUFEY_COMMON_SRCS
  src/parse_options.cc
  src/keymap_vk.cc
  src/title_badge.cc
  src/laufey_value.cc
)

if(APPLE)
  list(APPEND LAUFEY_COMMON_SRCS
    src/notifications_mac.mm
    src/dialog_mac.mm
    src/permissions_mac.mm
    src/keymap_mac.mm
    src/dock_mac.mm
    src/menu_mac.mm
    src/tray_mac.mm
  )
elseif(UNIX)
  list(APPEND LAUFEY_COMMON_SRCS
    src/notifications_linux.cc
    src/dialog_linux.cc
    src/permissions_stub.cc
    src/keymap_gdk.cc
    src/menu_linux.cc
    src/tray_linux.cc
  )
elseif(WIN32)
  list(APPEND LAUFEY_COMMON_SRCS
    src/notifications_win.cc
    src/dialog_win.cc
    src/permissions_stub.cc
    src/tray_win.cc
  )
endif()

add_library(laufey_backend_common STATIC ${LAUFEY_COMMON_SRCS})

target_include_directories(laufey_backend_common PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../capi/include
)

if(WIN32)
  # WIC PNG decode + Shell_NotifyIcon
  target_link_libraries(laufey_backend_common PUBLIC
    windowscodecs
    ole32
    shell32
    gdi32
    user32
  )
elseif(APPLE)
  find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
  find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  target_link_libraries(laufey_backend_common PUBLIC
    ${USERNOTIFICATIONS_FRAMEWORK}
    ${FOUNDATION_FRAMEWORK}
    ${COCOA_FRAMEWORK}
  )
  target_compile_options(laufey_backend_common PRIVATE
    $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>
  )
elseif(UNIX)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LAUFEY_COMMON_GTK REQUIRED gtk+-3.0)
  pkg_check_modules(LAUFEY_COMMON_APPINDICATOR appindicator3-0.1)
  if(NOT LAUFEY_COMMON_APPINDICATOR_FOUND)
    pkg_check_modules(LAUFEY_COMMON_APPINDICATOR ayatana-appindicator3-0.1)
  endif()
  target_include_directories(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_INCLUDE_DIRS}
    ${LAUFEY_COMMON_APPINDICATOR_INCLUDE_DIRS}
  )
  target_link_libraries(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_LIBRARIES}
    ${LAUFEY_COMMON_APPINDICATOR_LIBRARIES}
  )
  target_compile_options(laufey_backend_common PUBLIC
    ${LAUFEY_COMMON_GTK_CFLAGS_OTHER}
    ${LAUFEY_COMMON_APPINDICATOR_CFLAGS_OTHER}
  )
  if(LAUFEY_COMMON_APPINDICATOR_FOUND)
    target_compile_definitions(laufey_backend_common PUBLIC LAUFEY_HAVE_APPINDICATOR=1)
  endif()
endif()

set_target_properties(laufey_backend_common PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)
