project(gcapture LANGUAGES CXX)

# Use Widgets because the original codebase already depends on Qt (e.g., QString)
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_library(gcapture SHARED
    src/core/capture_manager.cpp
    src/core/frame_converter.cpp
    src/core/c_api.cpp
    src/providers/winmf_provider.cpp
    src/providers/mf_recorder.cpp
    src/providers/dshow_provider.cpp
    src/audio/audio_manager.cpp
    src/core/exports.def
)

target_include_directories(gcapture
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (WIN32)
  target_compile_definitions(gcapture PRIVATE GCAP_WIN_MF GCAP_WIN_DSHOW)
  target_compile_definitions(gcapture PRIVATE GCAPTURE_BUILD)

  target_link_libraries(gcapture PRIVATE
      Qt${QT_VERSION_MAJOR}::Widgets
      # Media Foundation
      mfplat mf mfuuid mfreadwrite
      # COM / system
      ole32 oleaut32 propsys uuid user32 advapi32 setupapi
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
