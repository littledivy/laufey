# Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

cmake_minimum_required(VERSION 3.19)

if(APPLE)
  project(laufey VERSION 1.0.0 LANGUAGES C CXX OBJCXX)
else()
  project(laufey VERSION 1.0.0 LANGUAGES C CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LAUFEY_RENDERER_SRCS
  src/render_process_handler.cc
  src/render_process_handler.h
  src/renderer_app.cc
  src/renderer_app.h
)

if(NOT DEFINED CEF_ROOT)
  message(FATAL_ERROR "CEF_ROOT must be defined")
endif()

set(LAUFEY_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../capi/include" CACHE PATH "Path to laufey.h header directory")

# CEF uses the static CRT (/MT) on Windows; build everything, including
# backend-common, the same way or the link fails with LNK2038.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Shared native-API implementations (notifications, tray, dock, dialogs, ...).
# Each platform branch links laufey_backend_common below.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../backend-common
                 ${CMAKE_BINARY_DIR}/backend-common)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CEF_ROOT}/cmake")

find_package(CEF REQUIRED)

include(cef_macros)
include(cef_variables)

set(CEF_TARGET "laufey")
SET_CEF_TARGET_OUT_DIR()

if(OS_MAC)
  set(CEF_WRAPPER_LIB "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.a")
  if(EXISTS "${CEF_WRAPPER_LIB}" AND NOT TARGET libcef_dll_wrapper)
    add_library(libcef_dll_wrapper STATIC IMPORTED GLOBAL)
    set_target_properties(libcef_dll_wrapper PROPERTIES
      IMPORTED_LOCATION "${CEF_WRAPPER_LIB}"
    )
    target_include_directories(libcef_dll_wrapper INTERFACE
      "${CEF_ROOT}"
      "${CEF_ROOT}/include"
    )
  endif()

  set(LAUFEY_SRCS_COMMON
    src/app.cc
    src/app.h
    src/runtime_loader.cc
    src/runtime_loader_mac.mm
    src/runtime_loader.h
  )

  set(LAUFEY_SRCS_MAC
    src/main_mac.mm
  )

  set(LAUFEY_HELPER_SRCS
    src/helper.cc
    ${LAUFEY_RENDERER_SRCS}
  )

  set(LAUFEY_RESOURCES_SRCS
    macos/Info.plist.in
  )

  set(CEF_HELPER_TARGET "laufey_Helper")
  set(CEF_HELPER_OUTPUT_NAME "laufey Helper")

  option(OPTION_USE_ARC "Build with ARC (automatic Reference Counting) on macOS." ON)

  set(CEF_APP "${CEF_TARGET_OUT_DIR}/${CEF_TARGET}.app")

  set(EXECUTABLE_NAME "${CEF_TARGET}")
  set(PRODUCT_NAME "${CEF_TARGET}")

  add_executable(${CEF_TARGET} MACOSX_BUNDLE ${LAUFEY_RESOURCES_SRCS} ${LAUFEY_SRCS_COMMON} ${LAUFEY_SRCS_MAC})
  SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
  if(OPTION_USE_ARC)
    # CEF's SET_EXECUTABLE_TARGET_PROPERTIES applies CEF_COMPILER_FLAGS only
    # to COMPILE_LANGUAGE:C/CXX, so .mm files miss -fobjc-arc otherwise. Apply
    # it explicitly here — without ARC, ObjC pointers stored in C++ structs
    # like TrayEntry are not retained, leading to use-after-free crashes when
    # the dispatched main-queue blocks dereference them.
    target_compile_options(${CEF_TARGET} PRIVATE
      $<$<COMPILE_LANGUAGE:OBJC,OBJCXX>:-fobjc-arc>
    )
  endif()
  target_link_libraries(${CEF_TARGET} libcef_dll_wrapper ${CEF_STANDARD_LIBS}
    laufey_backend_common
    "-framework UserNotifications")
  target_include_directories(${CEF_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${LAUFEY_INCLUDE_DIR}")
  set_target_properties(${CEF_TARGET} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.deno.desktop"
    MACOSX_BUNDLE_BUNDLE_NAME "Deno"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
  )

  COPY_MAC_FRAMEWORK("${CEF_TARGET}" "${CEF_BINARY_DIR}" "${CEF_APP}")

  foreach(_suffix_list ${CEF_HELPER_APP_SUFFIXES})
    string(REPLACE ":" ";" _suffix_list ${_suffix_list})
    list(GET _suffix_list 0 _name_suffix)
    list(GET _suffix_list 1 _target_suffix)
    list(GET _suffix_list 2 _plist_suffix)

    set(_helper_target "${CEF_HELPER_TARGET}${_target_suffix}")
    set(_helper_output_name "${CEF_HELPER_OUTPUT_NAME}${_name_suffix}")

    set(_helper_info_plist "${CMAKE_CURRENT_BINARY_DIR}/helper-Info${_target_suffix}.plist")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/macos/helper-Info.plist.in" _plist_contents)
    string(REPLACE "\${EXECUTABLE_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
    string(REPLACE "\${PRODUCT_NAME}" "${_helper_output_name}" _plist_contents ${_plist_contents})
    string(REPLACE "\${BUNDLE_ID_SUFFIX}" "${_plist_suffix}" _plist_contents ${_plist_contents})
    file(WRITE ${_helper_info_plist} ${_plist_contents})

    add_executable(${_helper_target} MACOSX_BUNDLE ${LAUFEY_HELPER_SRCS})
    SET_EXECUTABLE_TARGET_PROPERTIES(${_helper_target})
    target_link_libraries(${_helper_target} libcef_dll_wrapper ${CEF_STANDARD_LIBS})
    target_include_directories(${_helper_target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${LAUFEY_INCLUDE_DIR}")
    set_target_properties(${_helper_target} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${_helper_info_plist}
      OUTPUT_NAME ${_helper_output_name}
    )

    add_dependencies(${CEF_TARGET} "${_helper_target}")

    add_custom_command(
      TARGET ${CEF_TARGET}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
              "${CEF_TARGET_OUT_DIR}/${_helper_output_name}.app"
              "${CEF_APP}/Contents/Frameworks/${_helper_output_name}.app"
      VERBATIM
    )
  endforeach()

  set(PREFIXES "macos/")
  COPY_MAC_RESOURCES("${LAUFEY_RESOURCES_SRCS}" "${PREFIXES}" "${CEF_TARGET}" "${CMAKE_CURRENT_SOURCE_DIR}" "${CEF_APP}")

elseif(OS_WINDOWS)
  # Find the wrapper lib (.lib on Windows)
  set(CEF_WRAPPER_LIB "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.lib")
  if(EXISTS "${CEF_WRAPPER_LIB}" AND NOT TARGET libcef_dll_wrapper)
    add_library(libcef_dll_wrapper STATIC IMPORTED GLOBAL)
    set_target_properties(libcef_dll_wrapper PROPERTIES
      IMPORTED_LOCATION "${CEF_WRAPPER_LIB}"
    )
    target_include_directories(libcef_dll_wrapper INTERFACE
      "${CEF_ROOT}"
      "${CEF_ROOT}/include"
    )
  endif()

  set(LAUFEY_SRCS_COMMON
    src/app.cc
    src/app.h
    src/runtime_loader.cc
    src/runtime_loader.h
  )

  set(LAUFEY_SRCS_WIN
    src/main_windows.cc
    ${LAUFEY_RENDERER_SRCS}
  )

  add_executable(${CEF_TARGET} WIN32 ${LAUFEY_SRCS_COMMON} ${LAUFEY_SRCS_WIN})
  SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
  target_link_libraries(${CEF_TARGET} libcef_dll_wrapper ${CEF_LIB_RELEASE} ${CEF_STANDARD_LIBS} laufey_backend_common)
  target_include_directories(${CEF_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src" "${LAUFEY_INCLUDE_DIR}")

  # Copy CEF binaries and resources to the output directory
  add_custom_command(
    TARGET ${CEF_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_BINARY_DIR_RELEASE}"
            "${CEF_TARGET_OUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_RESOURCE_DIR}"
            "${CEF_TARGET_OUT_DIR}"
    VERBATIM
  )

elseif(OS_LINUX)
  set(CEF_WRAPPER_LIB "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.a")
  if(EXISTS "${CEF_WRAPPER_LIB}" AND NOT TARGET libcef_dll_wrapper)
    add_library(libcef_dll_wrapper STATIC IMPORTED GLOBAL)
    set_target_properties(libcef_dll_wrapper PROPERTIES
      IMPORTED_LOCATION "${CEF_WRAPPER_LIB}"
    )
    target_include_directories(libcef_dll_wrapper INTERFACE
      "${CEF_ROOT}"
      "${CEF_ROOT}/include"
    )
  endif()

  # GTK3 (for GDK/X11 headers) and XI2 (for input event monitoring)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
  pkg_check_modules(XI2 REQUIRED xi)
  pkg_check_modules(APPINDICATOR appindicator3-0.1)
  if(NOT APPINDICATOR_FOUND)
    pkg_check_modules(APPINDICATOR ayatana-appindicator3-0.1)
  endif()

  set(LAUFEY_SRCS_COMMON
    src/app.cc
    src/app.h
    src/runtime_loader.cc
    src/runtime_loader.h
  )

  set(LAUFEY_SRCS_LINUX
    src/main_linux.cc
    src/runtime_loader_linux.cc
    ${LAUFEY_RENDERER_SRCS}
  )

  add_executable(${CEF_TARGET} ${LAUFEY_SRCS_COMMON} ${LAUFEY_SRCS_LINUX})
  SET_EXECUTABLE_TARGET_PROPERTIES(${CEF_TARGET})
  # Chromium launches child processes with --change-stack-guard-on-fork=enable
  # (re-randomizes __stack_chk_guard after fork). The laufey binary's main() is
  # not part of Chromium's runtime, so its entry-time canary no longer matches
  # at return, tripping __stack_chk_fail. Disable stack protection to bypass.
  target_compile_options(${CEF_TARGET} PRIVATE -fno-stack-protector)
  target_link_libraries(${CEF_TARGET} libcef_dll_wrapper ${CEF_LIB_RELEASE} ${CEF_STANDARD_LIBS} ${GTK3_LIBRARIES} ${XI2_LIBRARIES} ${APPINDICATOR_LIBRARIES} laufey_backend_common)
  target_link_directories(${CEF_TARGET} PRIVATE ${GTK3_LIBRARY_DIRS} ${XI2_LIBRARY_DIRS} ${APPINDICATOR_LIBRARY_DIRS})
  target_include_directories(${CEF_TARGET} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${LAUFEY_INCLUDE_DIR}"
    ${GTK3_INCLUDE_DIRS}
    ${XI2_INCLUDE_DIRS}
    ${APPINDICATOR_INCLUDE_DIRS}
  )

  if(APPINDICATOR_FOUND)
    target_compile_definitions(${CEF_TARGET} PRIVATE LAUFEY_HAVE_APPINDICATOR=1)
  else()
    message(STATUS "libappindicator not found — tray icon will be a no-op on this build")
  endif()

  # Set RPATH so the executable finds libcef.so next to it
  set_target_properties(${CEF_TARGET} PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
  )

  # Copy CEF binaries and resources to the output directory
  add_custom_command(
    TARGET ${CEF_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_BINARY_DIR_RELEASE}"
            "${CEF_TARGET_OUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_RESOURCE_DIR}"
            "${CEF_TARGET_OUT_DIR}"
    VERBATIM
  )
endif()
