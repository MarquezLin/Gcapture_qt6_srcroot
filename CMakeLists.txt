cmake_minimum_required(VERSION 3.16)

project(win_capture_sdk VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GCAPTURE_SDK_VERSION "1.0.0" CACHE STRING "gcapture SDK version")
set(QT6_VIEWER_VERSION "1.0.0" CACHE STRING "Qt viewer application version")

# Qt auto tools (moc/uic/rcc) - safe to enable globally
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# ---- NVAPI root: shared by all targets ----
set(NVAPI_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/nvapi")
# ---- FFmpeg root: shared by gcapture / viewer ----
# Expected layout:
#   third_party/ffmpeg/include
#   third_party/ffmpeg/lib
#   third_party/ffmpeg/bin
set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg root directory")

# Output folders (keep build tree tidy)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Optional engineering diagnostics. The product viewer uses gcapture by default;
# gvendor is kept as an opt-in KS probe path while the driver private API is
# still being defined.
option(BUILD_GVENDOR_PROBE "Build the gvendor KS diagnostic probe tool" OFF)
option(BUILD_QT_VIEWER_GVENDOR_BACKEND "Expose experimental GVendor backend in qt6_viewer" OFF)

# SDK / DLLs
add_subdirectory(sdk/gcapture)
add_subdirectory(sdk/gdisplay)

if (WIN32 AND (BUILD_GVENDOR_PROBE OR BUILD_QT_VIEWER_GVENDOR_BACKEND))
  add_subdirectory(sdk/gvendor)
endif()

# Apps / demos
add_subdirectory(apps/qt6_viewer)

if (WIN32 AND BUILD_GVENDOR_PROBE)
  add_subdirectory(apps/gvendor_probe)
endif()
