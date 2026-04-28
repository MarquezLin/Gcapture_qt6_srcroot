cmake_minimum_required(VERSION 3.16)

project(win_capture_sdk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

# SDK / DLLs
add_subdirectory(sdk/gcapture)
add_subdirectory(sdk/gdisplay)

# Apps / demos
add_subdirectory(apps/qt6_viewer)
