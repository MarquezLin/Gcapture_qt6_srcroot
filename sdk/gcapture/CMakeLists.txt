set(_GCAPTURE_VERSION "${PROJECT_VERSION}")
project(gcapture VERSION ${_GCAPTURE_VERSION} LANGUAGES CXX)

set(GCAPTURE_CORE_SOURCES
    src/core/capture_manager.cpp
    src/core/frame_converter.cpp
    src/core/logging.cpp
    src/core/c_api.cpp
    src/pipeline/shared_scene_pipeline.cpp
    src/audio/audio_manager.cpp
    src/audio/wasapi_renderer.cpp
    src/core/exports.def
)

set(GCAPTURE_DIRECTSHOW_SOURCES
    src/providers/dshow_provider.cpp
    src/providers/dshow_signal_probe.cpp
    src/providers/dshow_raw_renderer.cpp
    src/providers/dshow_custom_sink.cpp
    src/audio/dshow_audio_monitor.cpp
)

add_library(gcapture SHARED ${GCAPTURE_CORE_SOURCES})
target_link_libraries(gcapture PRIVATE capture_ffmpeg_recorder)

if (WIN32)
  target_sources(gcapture PRIVATE ${GCAPTURE_DIRECTSHOW_SOURCES})
  target_compile_definitions(gcapture PRIVATE GCAP_WIN_DSHOW)
endif()

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

if (WIN32)
  target_compile_definitions(gcapture PRIVATE GCAPTURE_BUILD)

  target_link_libraries(gcapture PRIVATE
      # COM / system
      ole32 oleaut32 propsys uuid user32 advapi32 setupapi windowscodecs
      # Direct3D
      dxgi d3d11 d2d1 dwrite D3DCompiler
  )

  target_link_libraries(gcapture PRIVATE strmiids)

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
