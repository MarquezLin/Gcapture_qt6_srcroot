cmake_minimum_required(VERSION 3.16)

if (NOT DEFINED QT6_VIEWER_VERSION)
  set(QT6_VIEWER_VERSION "1.0.0" CACHE STRING "Qt viewer application version")
endif()

project(qt6_viewer VERSION ${QT6_VIEWER_VERSION} LANGUAGES CXX)

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
  tiff_analyzer.h
  tiff_analyzer.cpp
  tiffanalysisdialog.h
  tiffanalysisdialog.cpp
)

if (WIN32 AND BUILD_GVFG_SDK)
  list(APPEND VIEWER_SOURCES
    gvfg/gvfg_source.h
    gvfg/gvfg_source.cpp
  )
endif()

if (QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(qt6_viewer MANUAL_FINALIZATION ${VIEWER_SOURCES}
    resources.qrc
    app.rc
    tiffpreviewwidget.h tiffpreviewwidget.cpp
    previewwindow.h previewwindow.cpp previewwindow.ui
  )
else()
  add_executable(qt6_viewer ${VIEWER_SOURCES})
endif()

if (WIN32 AND BUILD_GVFG_SDK)
  target_compile_definitions(qt6_viewer PRIVATE QT6_VIEWER_ENABLE_GVFG_BACKEND)
endif()

# Includes
# - gcapture public headers
# - gdisplay public headers (display_info.h, edid_reader.h)
target_include_directories(qt6_viewer PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/info
  ${CMAKE_SOURCE_DIR}/sdk/gcapture/include
)

if (WIN32 AND BUILD_GVFG_SDK)
  target_link_libraries(qt6_viewer PRIVATE gvfg)
  if (TARGET gvfg_preview)
    target_link_libraries(qt6_viewer PRIVATE gvfg_preview)
  endif()
endif()

target_compile_definitions(qt6_viewer PRIVATE
  QT6_VIEWER_VERSION="${QT6_VIEWER_VERSION}"
)

# Link
# NOTE: do NOT use $<TARGET_LINKER_FILE:...> here; link targets directly.
target_link_libraries(qt6_viewer PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  gcapture
  gdisplay
  dxgi d3d11 d3dcompiler
)

# ---- NVAPI: shared root variable from top-level ----
if (EXISTS "${NVAPI_ROOT}/include" AND EXISTS "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_include_directories(qt6_viewer PRIVATE "${NVAPI_ROOT}/include")
  target_link_libraries(qt6_viewer PRIVATE "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_compile_definitions(qt6_viewer PRIVATE GCAP_ENABLE_NVAPI)
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
if (WIN32 AND BUILD_GVFG_SDK)
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

# Optional: copy external edid-decode.exe next to qt6_viewer.exe
option(EDID_DECODE_EXE "Path to a prebuilt edid-decode.exe" "")
if (EDID_DECODE_EXE)
  add_custom_command(TARGET qt6_viewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${EDID_DECODE_EXE}"
      $<TARGET_FILE_DIR:qt6_viewer>
  )
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
