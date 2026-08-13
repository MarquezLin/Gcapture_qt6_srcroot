cmake_minimum_required(VERSION 3.16)

set(_QT6_VIEWER_VERSION "${PROJECT_VERSION}")
project(qt6_viewer VERSION ${_QT6_VIEWER_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(VIEWER_SOURCES
  main.cpp
  mainwindow.cpp
  mainwindow_capture.cpp
  mainwindow_info.cpp
  mainwindow_frame.cpp
  mainwindow.h
  mainwindow.ui
  inputinfodialog.h
  inputinfodialog.cpp
  inputinfodialog.ui
  displayinfodialog.h
  displayinfodialog.cpp
  displayinfodialog.ui
  procamp.h
  procamp.cpp
  procamp.ui
  info/display_output_info.h
  info/display_output_info.cpp
  info/capture_device_info.h
  info/capture_device_info.cpp
  rawframe.h
  rawframe.cpp
  rawpreviewwidget.h
  rawpreviewwidget.cpp
  rawinspectordialog.h
  rawinspectordialog.cpp
)

set(VIEWER_DESIGN_ASSETS
  styles/gigabyte_dark.qss
  image/signal_pattern.svg
  image/icon_start.svg
  image/icon_stop.svg
  image/icon_record.svg
  image/icon_camera.svg
  image/icon_monitor.svg
  image/icon_chip.svg
  image/icon_image.svg
)

list(APPEND VIEWER_SOURCES ${VIEWER_DESIGN_ASSETS})

if (WIN32)
  list(APPEND VIEWER_SOURCES
    gvfg/gvfg_source.h
    gvfg/gvfg_source.cpp
    registerdialog.h
    registerdialog.cpp
    registerdialog.ui
  )
  set(QT6_VIEWER_GVFG_RECORDING_SOURCE "${CMAKE_SOURCE_DIR}/sdk/gcapture/src/providers/ffmpeg_video_recorder.cpp")
  if (EXISTS "${QT6_VIEWER_GVFG_RECORDING_SOURCE}")
    list(APPEND VIEWER_SOURCES
      ${QT6_VIEWER_GVFG_RECORDING_SOURCE}
    )
  endif()
endif()

if (QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(qt6_viewer MANUAL_FINALIZATION ${VIEWER_SOURCES}
    resources.qrc
    app.rc
    previewwindow.h previewwindow.cpp previewwindow.ui
  )
else()
  add_executable(qt6_viewer ${VIEWER_SOURCES})
endif()

if (WIN32)
  target_compile_definitions(qt6_viewer PRIVATE
    QT6_VIEWER_ENABLE_GVFG_BACKEND
    QT6_VIEWER_ENABLE_GVFG_INTERNAL_TOOLS
  )
endif()

# Includes
# - gcapture public headers
# - gdisplay public headers (display_info.h, edid_reader.h)
target_include_directories(qt6_viewer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/info
  ${CMAKE_SOURCE_DIR}/sdk/gcapture/include
)

if (WIN32)
  target_link_libraries(qt6_viewer PRIVATE gvfg)
  if (TARGET gvfg_preview)
    target_link_libraries(qt6_viewer PRIVATE gvfg_preview)
  endif()
  target_include_directories(qt6_viewer PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk/gcapture/src/providers
  )
endif()

target_compile_definitions(qt6_viewer PRIVATE
  QT6_VIEWER_VERSION="${PROJECT_VERSION}"
)

# Link
# NOTE: do NOT use $<TARGET_LINKER_FILE:...> here; link targets directly.
target_link_libraries(qt6_viewer PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  gcapture
  gdisplay
  dxgi d3d11 d3dcompiler dwmapi windowscodecs ole32
)

# ---- NVAPI: shared root variable from top-level ----
if (EXISTS "${NVAPI_ROOT}/include" AND EXISTS "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_include_directories(qt6_viewer PRIVATE "${NVAPI_ROOT}/include")
  target_link_libraries(qt6_viewer PRIVATE "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_compile_definitions(qt6_viewer PRIVATE GCAP_ENABLE_NVAPI)
endif()

# Required FFmpeg recorder support for GVFG's pull-based frame path.
if (WIN32)
  find_path(QT6_VIEWER_FFMPEG_INCLUDE_DIR libavformat/avformat.h PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
  find_library(QT6_VIEWER_FFMPEG_AVFORMAT_LIB avformat PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(QT6_VIEWER_FFMPEG_AVCODEC_LIB avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(QT6_VIEWER_FFMPEG_AVUTIL_LIB avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(QT6_VIEWER_FFMPEG_SWSCALE_LIB swscale PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  if (QT6_VIEWER_FFMPEG_INCLUDE_DIR AND QT6_VIEWER_FFMPEG_AVFORMAT_LIB AND QT6_VIEWER_FFMPEG_AVCODEC_LIB AND QT6_VIEWER_FFMPEG_AVUTIL_LIB AND QT6_VIEWER_FFMPEG_SWSCALE_LIB)
    target_compile_definitions(qt6_viewer PRIVATE QT6_VIEWER_ENABLE_GVFG_RECORDING GCAP_ENABLE_FFMPEG)
    target_include_directories(qt6_viewer PRIVATE "${QT6_VIEWER_FFMPEG_INCLUDE_DIR}")
    target_link_libraries(qt6_viewer PRIVATE
      ${QT6_VIEWER_FFMPEG_AVFORMAT_LIB}
      ${QT6_VIEWER_FFMPEG_AVCODEC_LIB}
      ${QT6_VIEWER_FFMPEG_AVUTIL_LIB}
      ${QT6_VIEWER_FFMPEG_SWSCALE_LIB}
    )
    message(STATUS "qt6_viewer: GVFG FFmpeg recording enabled: ${FFMPEG_ROOT}")
  else()
    message(FATAL_ERROR "qt6_viewer: Required FFmpeg headers/libs were not found under FFMPEG_ROOT: ${FFMPEG_ROOT}")
  endif()
endif()

if (MSVC)
  target_compile_options(qt6_viewer PRIVATE /utf-8)
endif()

# Copy gcapture.dll next to qt6_viewer.exe
add_custom_command(TARGET qt6_viewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:gcapture>
    $<TARGET_FILE_DIR:qt6_viewer>
)

# Copy gdisplay.dll next to qt6_viewer.exe
add_custom_command(TARGET qt6_viewer POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:gdisplay>
    $<TARGET_FILE_DIR:qt6_viewer>
)

# Copy GVFG SDK DLL next to qt6_viewer.exe
if (WIN32)
  add_custom_command(TARGET qt6_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_FILE:gvfg>
      $<TARGET_FILE_DIR:qt6_viewer>
  )
  if (TARGET gvfg_preview)
    add_custom_command(TARGET qt6_viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:gvfg_preview>
        $<TARGET_FILE_DIR:qt6_viewer>
    )
  endif()
endif()

# Copy FFmpeg runtime DLLs next to qt6_viewer.exe (Windows)
# FFmpeg itself is kept under srcroot/third_party/ffmpeg, but this patch zip does not include third_party.
if (WIN32)
  set(FFMPEG_BIN_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg/bin")
  if (EXISTS "${FFMPEG_BIN_DIR}")
    add_custom_command(TARGET qt6_viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${FFMPEG_BIN_DIR}"
        $<TARGET_FILE_DIR:qt6_viewer>
    )
  else()
    message(WARNING "FFmpeg bin directory not found: ${FFMPEG_BIN_DIR} (skip copy)")
  endif()
endif()

# Copy edid-decode.exe next to qt6_viewer.exe. This is a file path, not a
# boolean option; using option() here stores BOOL=OFF in CMakeCache and skips
# the required runtime helper after a clean configure/build.
set(_QT6_VIEWER_DEFAULT_EDID_DECODE_EXE
  "${CMAKE_SOURCE_DIR}/third_party/edid-decode/vs/x64/Release/edid-decode.exe")
if (NOT DEFINED EDID_DECODE_EXE OR NOT EDID_DECODE_EXE)
  set(EDID_DECODE_EXE "${_QT6_VIEWER_DEFAULT_EDID_DECODE_EXE}" CACHE FILEPATH
      "Path to a prebuilt edid-decode.exe" FORCE)
else()
  set(EDID_DECODE_EXE "${EDID_DECODE_EXE}" CACHE FILEPATH
      "Path to a prebuilt edid-decode.exe")
endif()
if (EXISTS "${EDID_DECODE_EXE}")
  add_custom_command(TARGET qt6_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${EDID_DECODE_EXE}"
      $<TARGET_FILE_DIR:qt6_viewer>
  )
else()
  message(WARNING "edid-decode executable not found: ${EDID_DECODE_EXE}")
endif()

# Bundle / subsystem settings (Windows GUI)
if (QT_VERSION VERSION_LESS 6.1.0)
  set(BUNDLE_ID_OPTION MACOSX_BUNDLE_GUI_IDENTIFIER com.example.qt6_viewer)
endif()

set_target_properties(qt6_viewer PROPERTIES
  ${BUNDLE_ID_OPTION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
  MACOSX_BUNDLE TRUE
  WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS qt6_viewer
  BUNDLE DESTINATION .
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if (QT_VERSION_MAJOR EQUAL 6)
  qt_finalize_executable(qt6_viewer)
endif()
