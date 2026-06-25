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

set(_DEFAULT_GVFG_STANDALONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../GVFG_Standalone")
if (EXISTS "${_DEFAULT_GVFG_STANDALONE_ROOT}/sdk/gvfg/include/gvfg_capture.h")
  set(_DEFAULT_BUILD_GVFG_SDK ON)
else()
  set(_DEFAULT_BUILD_GVFG_SDK OFF)
endif()

option(BUILD_GVFG_SDK "Enable external GVFG SDK viewer integration" ${_DEFAULT_BUILD_GVFG_SDK})
set(GVFG_STANDALONE_ROOT "${_DEFAULT_GVFG_STANDALONE_ROOT}" CACHE PATH "GVFG standalone SDK source root")
set(GVFG_STANDALONE_BUILD_DIR "" CACHE PATH "GVFG standalone SDK build directory containing bin/gvfg.dll and lib/gvfg.lib")

# SDK / DLLs
add_subdirectory(sdk/gcapture)
add_subdirectory(sdk/gdisplay)

if (WIN32 AND BUILD_GVFG_SDK)
  if (NOT EXISTS "${GVFG_STANDALONE_ROOT}/sdk/gvfg/include/gvfg_capture.h")
    message(FATAL_ERROR "GVFG_STANDALONE_ROOT does not contain sdk/gvfg/include/gvfg_capture.h: ${GVFG_STANDALONE_ROOT}")
  endif()

  set(_gvfg_build_dir "${GVFG_STANDALONE_BUILD_DIR}")
  if (NOT _gvfg_build_dir)
    file(GLOB _gvfg_build_candidates LIST_DIRECTORIES true "${GVFG_STANDALONE_ROOT}/build/*")
    foreach(_candidate IN LISTS _gvfg_build_candidates)
      if (EXISTS "${_candidate}/bin/gvfg.dll" AND EXISTS "${_candidate}/lib/gvfg.lib")
        set(_gvfg_build_dir "${_candidate}")
        break()
      endif()
    endforeach()
  endif()

  if (NOT _gvfg_build_dir)
    message(FATAL_ERROR "Could not find GVFG standalone build output. Set GVFG_STANDALONE_BUILD_DIR to a directory containing bin/gvfg.dll and lib/gvfg.lib.")
  endif()
  if (NOT EXISTS "${_gvfg_build_dir}/bin/gvfg.dll" OR NOT EXISTS "${_gvfg_build_dir}/lib/gvfg.lib")
    message(FATAL_ERROR "GVFG_STANDALONE_BUILD_DIR must contain bin/gvfg.dll and lib/gvfg.lib: ${_gvfg_build_dir}")
  endif()
  if (NOT EXISTS "${_gvfg_build_dir}/bin/gvfg_preview.dll" OR NOT EXISTS "${_gvfg_build_dir}/lib/gvfg_preview.lib")
    message(FATAL_ERROR "GVFG_STANDALONE_BUILD_DIR must contain bin/gvfg_preview.dll and lib/gvfg_preview.lib: ${_gvfg_build_dir}")
  endif()

  add_library(gvfg SHARED IMPORTED GLOBAL)
  set_target_properties(gvfg PROPERTIES
    IMPORTED_LOCATION "${_gvfg_build_dir}/bin/gvfg.dll"
    IMPORTED_IMPLIB "${_gvfg_build_dir}/lib/gvfg.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${GVFG_STANDALONE_ROOT}/sdk/gvfg/include"
    INTERFACE_COMPILE_DEFINITIONS QT6_VIEWER_USE_STANDALONE_GVFG
  )

  add_library(gvfg_preview SHARED IMPORTED GLOBAL)
  set_target_properties(gvfg_preview PROPERTIES
    IMPORTED_LOCATION "${_gvfg_build_dir}/bin/gvfg_preview.dll"
    IMPORTED_IMPLIB "${_gvfg_build_dir}/lib/gvfg_preview.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${GVFG_STANDALONE_ROOT}/helpers/gvfg_preview/include;${GVFG_STANDALONE_ROOT}/sdk/gvfg/include"
  )

  if (EXISTS "${_gvfg_build_dir}/bin/gvfg_convert.dll" AND
      EXISTS "${_gvfg_build_dir}/lib/gvfg_convert.lib" AND
      EXISTS "${GVFG_STANDALONE_ROOT}/helpers/gvfg_convert/include/gvfg_convert.h")
    add_library(gvfg_convert SHARED IMPORTED GLOBAL)
    set_target_properties(gvfg_convert PROPERTIES
      IMPORTED_LOCATION "${_gvfg_build_dir}/bin/gvfg_convert.dll"
      IMPORTED_IMPLIB "${_gvfg_build_dir}/lib/gvfg_convert.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${GVFG_STANDALONE_ROOT}/helpers/gvfg_convert/include;${GVFG_STANDALONE_ROOT}/sdk/gvfg/include"
    )
  else()
    message(WARNING "GVFG convert helper not found; qt6_viewer will use local snapshot conversion fallback.")
  endif()

  message(STATUS "Using external GVFG SDK: ${_gvfg_build_dir}")
endif()

# Apps / demos
add_subdirectory(apps/qt6_viewer)
