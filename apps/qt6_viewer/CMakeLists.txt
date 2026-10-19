cmake_minimum_required(VERSION 3.16)

set(_QT6_VIEWER_VERSION "${PROJECT_VERSION}")
project(qt6_viewer VERSION ${_QT6_VIEWER_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(GVFG_VIEWER_ENABLE_CONSOLE
  "Show a Windows console for the viewer's existing Qt logs" ON)

find_package(QT NAMES Qt6 REQUIRED COMPONENTS Widgets Concurrent)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Concurrent)
# DICOM support
find_package(DCMTK CONFIG REQUIRED)

set(VIEWER_SOURCES
  app/main.cpp
  app/mainwindow.cpp
  app/mainwindow_capture.cpp
  app/mainwindow_info.cpp
  backend/capture_backend_catalog.h
  backend/capture_backend.h
  backend/capture_source.h
  backend/dshow_backend_catalog.cpp
  dshow/directshow_source.h
  dshow/directshow_source.cpp
  state/raw_snapshot_buffer.h
  state/raw_snapshot_buffer.cpp
  state/capture_runtime_tracker.h
  state/capture_runtime_tracker.cpp
  app/mainwindow_snapshot.cpp
  app/mainwindow.h
  app/mainwindow.ui
  dialogs/inputinfo/inputinfodialog.h
  dialogs/inputinfo/inputinfodialog.cpp
  dialogs/inputinfo/inputinfodialog.ui
  dialogs/displayinfo/displayinfodialog.h
  dialogs/displayinfo/displayinfodialog.cpp
  dialogs/displayinfo/displayinfodialog.ui
  dialogs/procamp/procamp.h
  dialogs/procamp/procamp.cpp
  dialogs/procamp/procamp.ui
  info/display_output_info.h
  info/display_output_info.cpp
  info/capture_device_info.h
  info/capture_device_info.cpp
  raw/rawframe.h
  raw/rawframe.cpp
  raw/rawpreviewwidget.h
  raw/rawpreviewwidget.cpp
  raw/rawinspectordialog.h
  raw/rawinspectordialog.cpp
  dicom/dicomframe.h
  dicom/dicomframe.cpp
  dicom/dicomseries.h
  dicom/dicomseries.cpp
  dicom/dicomviewerdialog.h
  dicom/dicomviewerdialog.cpp
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
    app/mainwindow_dshow.cpp
    app/mainwindow_gvfg.cpp
    backend/gvfg_backend_catalog.cpp
    gvfg/gvfg_source.h
    gvfg/gvfg_source.cpp
    gvfg/gvfg_source_recording.cpp
    dialogs/register/registerdialog.h
    dialogs/register/registerdialog.cpp
    dialogs/register/registerdialog.ui
    dialogs/osd/osddialog.h
    dialogs/osd/osddialog.cpp
  )
endif()

if (QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(qt6_viewer MANUAL_FINALIZATION ${VIEWER_SOURCES}
    resources.qrc
    app.rc
    preview/previewwindow.h
    preview/previewwindow.cpp
    preview/previewwindow.ui
  )
else()
  add_executable(qt6_viewer ${VIEWER_SOURCES})
endif()

if (WIN32)
  target_compile_definitions(qt6_viewer PRIVATE
    QT6_VIEWER_ENABLE_GVFG_BACKEND
  )
endif()

# Includes
# - gcapture public headers
# - gdisplay public headers (display_info.h, edid_reader.h)
target_include_directories(qt6_viewer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/app
  ${CMAKE_CURRENT_SOURCE_DIR}/backend
  ${CMAKE_CURRENT_SOURCE_DIR}/dshow
  ${CMAKE_CURRENT_SOURCE_DIR}/state
  ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/displayinfo
  ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/inputinfo
  ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/procamp
  ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/register
  ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/osd
  ${CMAKE_CURRENT_SOURCE_DIR}/info
  ${CMAKE_CURRENT_SOURCE_DIR}/preview
  ${CMAKE_CURRENT_SOURCE_DIR}/raw
  ${CMAKE_CURRENT_SOURCE_DIR}/dicom
  ${CMAKE_SOURCE_DIR}/sdk/gcapture/include
)

if (WIN32)
  target_link_libraries(qt6_viewer PRIVATE gvfg)
  if (TARGET gvfg_preview)
    target_link_libraries(qt6_viewer PRIVATE gvfg_preview)
  endif()
  if (TARGET gvfg_audio_playback)
    target_link_libraries(qt6_viewer PRIVATE gvfg_audio_playback)
  endif()
  target_link_libraries(qt6_viewer PRIVATE capture_ffmpeg_recorder)
endif()

target_compile_definitions(qt6_viewer PRIVATE
  QT6_VIEWER_VERSION="${PROJECT_VERSION}"
)

# Link
# NOTE: do NOT use $<TARGET_LINKER_FILE:...> here; link targets directly.
target_link_libraries(qt6_viewer PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  Qt${QT_VERSION_MAJOR}::Concurrent
  gcapture
  gdisplay
  DCMTK::DCMTK
  dxgi d3d11 d3dcompiler dwmapi windowscodecs ole32
)

# ---- NVAPI: shared root variable from top-level ----
if (EXISTS "${NVAPI_ROOT}/include" AND EXISTS "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_include_directories(qt6_viewer PRIVATE "${NVAPI_ROOT}/include")
  target_link_libraries(qt6_viewer PRIVATE "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_compile_definitions(qt6_viewer PRIVATE GCAP_ENABLE_NVAPI)
endif()

# The recording helper owns FFmpeg headers and libraries. The viewer sees only
# its small C++ interface and the feature flag used by GvfgSource.
if (WIN32)
  target_compile_definitions(qt6_viewer PRIVATE QT6_VIEWER_ENABLE_GVFG_RECORDING)
  message(STATUS "qt6_viewer: GVFG recording uses capture_ffmpeg_recorder")
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
  if (TARGET gvfg_audio_playback)
    add_custom_command(TARGET qt6_viewer POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:gvfg_audio_playback>
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

# Bundle / subsystem settings (Windows GUI by default).
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

if (WIN32 AND GVFG_VIEWER_ENABLE_CONSOLE)
  # Equivalent to qmake CONFIG += console. Keep the existing main() and
  # message handler: Qt logs already flow to stdout/stderr and the log file.
  set_target_properties(qt6_viewer PROPERTIES WIN32_EXECUTABLE FALSE)
  target_compile_definitions(qt6_viewer PRIVATE QT6_VIEWER_ENABLE_DELIVERY_DIAGNOSTICS)
endif()

include(GNUInstallDirs)
install(TARGETS qt6_viewer
  BUNDLE DESTINATION .
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if (QT_VERSION_MAJOR EQUAL 6)
  qt_finalize_executable(qt6_viewer)
endif()
