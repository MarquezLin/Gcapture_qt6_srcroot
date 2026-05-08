if (NOT DEFINED GCAPTURE_SDK_VERSION)
  set(GCAPTURE_SDK_VERSION "1.0.0" CACHE STRING "gcapture SDK version")
endif()

project(gcapture VERSION ${GCAPTURE_SDK_VERSION} LANGUAGES CXX)

add_library(gcapture SHARED
    src/core/capture_manager.cpp
    src/core/frame_converter.cpp
    src/core/logging.cpp
    src/core/tiff_analysis.cpp
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
    GCAPTURE_SDK_VERSION="${GCAPTURE_SDK_VERSION}"
)

# Optional FFmpeg recorder support for DShow recording.
# Set FFMPEG_ROOT to a folder containing include/ and lib/.
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
  if (EXISTS "${FFMPEG_INCLUDE_DIR}/libavformat/avformat.h" AND FFMPEG_AVFORMAT_LIB AND FFMPEG_AVCODEC_LIB AND FFMPEG_AVUTIL_LIB)
    target_compile_definitions(gcapture PRIVATE GCAP_ENABLE_FFMPEG=1)
    target_include_directories(gcapture PRIVATE "${FFMPEG_INCLUDE_DIR}")
    target_link_libraries(gcapture PRIVATE ${FFMPEG_AVFORMAT_LIB} ${FFMPEG_AVCODEC_LIB} ${FFMPEG_AVUTIL_LIB})
    if (FFMPEG_SWSCALE_LIB)
      target_link_libraries(gcapture PRIVATE ${FFMPEG_SWSCALE_LIB})
    endif()
    message(STATUS "gcapture: FFmpeg recorder enabled: ${FFMPEG_ROOT}")
  else()
    message(WARNING "gcapture: FFMPEG_ROOT is set, but FFmpeg headers/libs were not found. DShow FFmpeg recording will return Not supported.")
  endif()
else()
  message(STATUS "gcapture: FFmpeg recorder disabled. Set FFMPEG_ROOT to enable DShow FFmpeg MP4 recording.")
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
