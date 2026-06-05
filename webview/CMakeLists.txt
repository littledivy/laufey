cmake_minimum_required(VERSION 3.19)
project(laufey_webview VERSION 1.0.0 LANGUAGES C CXX)

if(APPLE)
  enable_language(OBJCXX)
  set(PLATFORM_MACOS TRUE)
elseif(UNIX AND NOT APPLE)
  set(PLATFORM_LINUX TRUE)
elseif(WIN32)
  set(PLATFORM_WINDOWS TRUE)
endif()

# Shared native-API implementations (notifications, tray, dock, dialogs, ...).
# Each platform branch links laufey_backend_common below.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../backend-common
                 ${CMAKE_BINARY_DIR}/backend-common)

set(LAUFEY_SRCS_COMMON
  src/webview_value.h
  src/runtime_loader.cc
  src/runtime_loader.h
)

if(PLATFORM_MACOS)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_macos.mm
    src/main_mac.mm
  )
elseif(PLATFORM_LINUX)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_linux.cc
    src/main_linux.cc
  )
elseif(PLATFORM_WINDOWS)
  set(LAUFEY_SRCS_PLATFORM
    src/webview_windows.cc
    src/main_windows.cc
  )
endif()

set(LAUFEY_TARGET "laufey_webview")

if(PLATFORM_MACOS)
  add_executable(${LAUFEY_TARGET} MACOSX_BUNDLE
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
    macos/Info.plist.in
  )

  find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
  find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
  find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)

  target_link_libraries(${LAUFEY_TARGET}
    ${COCOA_FRAMEWORK}
    ${WEBKIT_FRAMEWORK}
    ${USERNOTIFICATIONS_FRAMEWORK}
    laufey_backend_common
  )

  target_compile_options(${LAUFEY_TARGET} PRIVATE -fobjc-arc)

  set_target_properties(${LAUFEY_TARGET} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.deno.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "Deno"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
  )

elseif(PLATFORM_LINUX)
  add_executable(${LAUFEY_TARGET}
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
  )

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKITGTK REQUIRED webkit2gtk-4.1)
  pkg_check_modules(GTK REQUIRED gtk+-3.0)
  pkg_check_modules(APPINDICATOR appindicator3-0.1)
  if(NOT APPINDICATOR_FOUND)
    pkg_check_modules(APPINDICATOR ayatana-appindicator3-0.1)
  endif()

  target_include_directories(${LAUFEY_TARGET} PRIVATE
    ${WEBKITGTK_INCLUDE_DIRS}
    ${GTK_INCLUDE_DIRS}
    ${APPINDICATOR_INCLUDE_DIRS}
  )

  target_link_libraries(${LAUFEY_TARGET}
    ${WEBKITGTK_LIBRARIES}
    ${GTK_LIBRARIES}
    ${APPINDICATOR_LIBRARIES}
    laufey_backend_common
    dl
    pthread
  )

  target_compile_options(${LAUFEY_TARGET} PRIVATE
    ${WEBKITGTK_CFLAGS_OTHER}
    ${GTK_CFLAGS_OTHER}
    ${APPINDICATOR_CFLAGS_OTHER}
  )

  if(APPINDICATOR_FOUND)
    target_compile_definitions(${LAUFEY_TARGET} PRIVATE LAUFEY_HAVE_APPINDICATOR=1)
  else()
    message(STATUS "libappindicator not found — tray icon will be a no-op on this build")
  endif()

elseif(PLATFORM_WINDOWS)
  add_executable(${LAUFEY_TARGET} WIN32
    ${LAUFEY_SRCS_COMMON}
    ${LAUFEY_SRCS_PLATFORM}
  )

  if(NOT DEFINED WEBVIEW2_ROOT)
    message(STATUS "WEBVIEW2_ROOT not set, looking for WebView2Loader.dll in system")
  else()
    target_include_directories(${LAUFEY_TARGET} PRIVATE
      ${WEBVIEW2_ROOT}/build/native/include
    )
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      target_link_directories(${LAUFEY_TARGET} PRIVATE
        ${WEBVIEW2_ROOT}/build/native/x64
      )
    else()
      target_link_directories(${LAUFEY_TARGET} PRIVATE
        ${WEBVIEW2_ROOT}/build/native/x86
      )
    endif()
  endif()

  target_link_libraries(${LAUFEY_TARGET}
    WebView2LoaderStatic
    user32
    ole32
    oleaut32
    shlwapi
    laufey_backend_common
  )
endif()

set(LAUFEY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../capi/include" CACHE PATH "Path to laufey.h header directory")

target_include_directories(${LAUFEY_TARGET} PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${LAUFEY_INCLUDE_DIR}
)

set_target_properties(${LAUFEY_TARGET} PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
)
