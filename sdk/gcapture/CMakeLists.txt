set(_GCAPTURE_VERSION "${PROJECT_VERSION}")
project(gcapture VERSION ${_GCAPTURE_VERSION} LANGUAGES CXX)

add_library(gcapture SHARED
    src/core/capture_manager.cpp
    src/core/frame_converter.cpp
    src/core/logging.cpp
    src/core/c_api.cpp
    src/pipeline/shared_scene_pipeline.cpp
    src/providers/winmf_provider.cpp
    src/providers/mf_recorder.cpp
    src/providers/ffmpeg_video_recorder.cpp
    src/providers/dshow_provider.cpp
    src/providers/dshow_signal_probe.cpp
    src/providers/dshow_raw_renderer.cpp
    src/providers/dshow_custom_sink.cpp
    src/audio/audio_manager.cpp
    src/audio/dshow_audio_monitor.cpp
    src/core/exports.def
)

# gcapture is a plain SDK DLL and intentionally does not use Qt.
# The top-level project enables AUTOMOC/AUTOUIC/AUTORCC for Qt apps; disable
# them here to avoid CMake dev warnings after removing the Qt dependency.
set_target_properties(gcapture PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gcapture
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_definitions(gcapture PRIVATE
    GCAPTURE_SDK_VERSION="${PROJECT_VERSION}"
)

# Required LGPL FFmpeg recorder support for DShow recording.
set(FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "FFmpeg root directory")
if (NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
  set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()
if (FFMPEG_ROOT)
  set(FFMPEG_INCLUDE_DIR "${FFMPEG_ROOT}/include")
  find_library(FFMPEG_AVFORMAT_LIB avformat PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(FFMPEG_AVCODEC_LIB avcodec PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(FFMPEG_AVUTIL_LIB avutil PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  find_library(FFMPEG_SWSCALE_LIB swscale PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
  if (EXISTS "${FFMPEG_INCLUDE_DIR}/libavformat/avformat.h" AND EXISTS "${FFMPEG_INCLUDE_DIR}/libswscale/swscale.h" AND FFMPEG_AVFORMAT_LIB AND FFMPEG_AVCODEC_LIB AND FFMPEG_AVUTIL_LIB AND FFMPEG_SWSCALE_LIB)
    target_compile_definitions(gcapture PRIVATE GCAP_ENABLE_FFMPEG=1)
    target_include_directories(gcapture PRIVATE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(gcapture PRIVATE ${FFMPEG_AVFORMAT_LIB} ${FFMPEG_AVCODEC_LIB} ${FFMPEG_AVUTIL_LIB} ${FFMPEG_SWSCALE_LIB})
    message(STATUS "gcapture: FFmpeg recorder enabled: ${FFMPEG_ROOT}")
    if (NOT EXISTS "${FFMPEG_ROOT}/bin/ffmpeg.exe")
      message(FATAL_ERROR "gcapture: Cannot verify FFmpeg licensing because ${FFMPEG_ROOT}/bin/ffmpeg.exe is missing.")
    endif()
    execute_process(
      COMMAND "${FFMPEG_ROOT}/bin/ffmpeg.exe" -version
      RESULT_VARIABLE GCAP_FFMPEG_VERSION_RESULT
      OUTPUT_VARIABLE GCAP_FFMPEG_VERSION_TEXT
      ERROR_VARIABLE GCAP_FFMPEG_VERSION_ERROR
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if (NOT GCAP_FFMPEG_VERSION_RESULT EQUAL 0)
      message(FATAL_ERROR "gcapture: Failed to inspect FFmpeg licensing: ${GCAP_FFMPEG_VERSION_ERROR}")
    endif()
    if (GCAP_FFMPEG_VERSION_TEXT MATCHES "--enable-gpl|--enable-nonfree")
      message(FATAL_ERROR "gcapture: FFmpeg at ${FFMPEG_ROOT} was built with GPL/nonfree options. An LGPL shared FFmpeg build is required.")
    endif()
  else()
    message(FATAL_ERROR "gcapture: Required FFmpeg headers/libs including libswscale were not found under FFMPEG_ROOT: ${FFMPEG_ROOT}")
  endif()
else()
  message(FATAL_ERROR "gcapture: FFMPEG_ROOT is required.")
endif()

if (WIN32)
  target_compile_definitions(gcapture PRIVATE GCAP_WIN_MF GCAP_WIN_DSHOW)
  target_compile_definitions(gcapture PRIVATE GCAPTURE_BUILD)

  target_link_libraries(gcapture PRIVATE
      # Media Foundation
      mfplat mf mfuuid mfreadwrite
      # COM / system
      ole32 oleaut32 propsys uuid user32 advapi32 setupapi windowscodecs
      # Direct3D
      dxgi d3d11 d2d1 dwrite D3DCompiler
      # DirectShow IIDs
      strmiids
  )

  if (MSVC)
    target_compile_options(gcapture PRIVATE /utf-8)
  endif()
endif()

# Optional NVAPI (enabled by consumer; keep available here too)
if (EXISTS "${NVAPI_ROOT}/include" AND EXISTS "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_include_directories(gcapture PRIVATE "${NVAPI_ROOT}/include")
  target_link_libraries(gcapture PRIVATE "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_compile_definitions(gcapture PRIVATE GCAP_ENABLE_NVAPI)
endif()
