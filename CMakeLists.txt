cmake_minimum_required(VERSION 3.16)

project(win_capture_sdk VERSION 1.0.0 LANGUAGES CXX)

# Remove obsolete user-facing cache switches from existing build directories.
# These features and versions are now governed by this project configuration.
unset(BUILD_GVFG_SDK CACHE)
unset(QT6_VIEWER_ENABLE_GVFG_RECORDING CACHE)
unset(GCAP_ENFORCE_LGPL_FFMPEG CACHE)
unset(GCAPTURE_SDK_VERSION CACHE)
unset(QT6_VIEWER_VERSION CACHE)

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

set(_DEFAULT_GVFG_STANDALONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../GVFG_Standalone")
set(GVFG_STANDALONE_ROOT "${_DEFAULT_GVFG_STANDALONE_ROOT}" CACHE PATH "GVFG standalone SDK source root")
set(GVFG_STANDALONE_BUILD_DIR "${GVFG_STANDALONE_ROOT}/build" CACHE PATH "GVFG standalone SDK build root or directory containing bin/gvfg.dll and lib/gvfg.lib")

# SDK / DLLs
add_subdirectory(sdk/gcapture)
add_subdirectory(sdk/gdisplay)

if (WIN32)
  if (NOT EXISTS "${GVFG_STANDALONE_ROOT}/sdk/gvfg/include/gvfg_capture.h")
    message(FATAL_ERROR "GVFG_STANDALONE_ROOT does not contain sdk/gvfg/include/gvfg_capture.h: ${GVFG_STANDALONE_ROOT}")
  endif()

  set(_gvfg_build_dir "${GVFG_STANDALONE_BUILD_DIR}")
  if (_gvfg_build_dir AND
      (NOT EXISTS "${_gvfg_build_dir}/bin/gvfg.dll" OR
       NOT EXISTS "${_gvfg_build_dir}/lib/gvfg.lib"))
    file(GLOB _gvfg_build_candidates LIST_DIRECTORIES true "${_gvfg_build_dir}/*")

    # Prefer a build produced by the same compiler and configuration as this
    # viewer.  Taking the first directory can silently link a Release viewer
    # against the much slower Debug GVFG preview helper.
    # Visual Studio is a multi-config generator, so CMAKE_BUILD_TYPE is empty
    # at configure time. Prefer the optimized standalone build in that case;
    # otherwise the fallback below can silently pick an older Debug directory.
    if (CMAKE_BUILD_TYPE)
      set(_gvfg_preferred_config "${CMAKE_BUILD_TYPE}")
    else()
      set(_gvfg_preferred_config "Release")
    endif()
    set(_gvfg_preferred_suffix "-${_gvfg_preferred_config}")
    if (MSVC)
      set(_gvfg_preferred_compiler "MSVC")
    else()
      set(_gvfg_preferred_compiler "${CMAKE_CXX_COMPILER_ID}")
    endif()
    foreach(_candidate IN LISTS _gvfg_build_candidates)
      get_filename_component(_candidate_name "${_candidate}" NAME)
      if (_candidate_name MATCHES "${_gvfg_preferred_compiler}" AND
          _candidate_name MATCHES "${_gvfg_preferred_suffix}$" AND
          EXISTS "${_candidate}/bin/gvfg.dll" AND
          EXISTS "${_candidate}/lib/gvfg.lib" AND
          EXISTS "${_candidate}/bin/gvfg_preview.dll" AND
          EXISTS "${_candidate}/lib/gvfg_preview.lib")
        set(_gvfg_build_dir "${_candidate}")
        break()
      endif()
    endforeach()

    foreach(_candidate IN LISTS _gvfg_build_candidates)
      if ((NOT EXISTS "${_gvfg_build_dir}/bin/gvfg.dll" OR
           NOT EXISTS "${_gvfg_build_dir}/lib/gvfg.lib") AND
          EXISTS "${_candidate}/bin/gvfg.dll" AND
          EXISTS "${_candidate}/lib/gvfg.lib")
        set(_gvfg_build_dir "${_candidate}")
        break()
      endif()
    endforeach()
  endif()

  if (NOT _gvfg_build_dir)
    message(FATAL_ERROR "Could not find GVFG standalone build output under GVFG_STANDALONE_BUILD_DIR.")
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
  )

  add_library(gvfg_preview SHARED IMPORTED GLOBAL)
  set_target_properties(gvfg_preview PROPERTIES
    IMPORTED_LOCATION "${_gvfg_build_dir}/bin/gvfg_preview.dll"
    IMPORTED_IMPLIB "${_gvfg_build_dir}/lib/gvfg_preview.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${GVFG_STANDALONE_ROOT}/helpers/gvfg_preview/include;${GVFG_STANDALONE_ROOT}/sdk/gvfg/include"
  )

  message(STATUS "Using external GVFG SDK: ${_gvfg_build_dir}")
endif()

# Apps / demos
add_subdirectory(apps/qt6_viewer)
