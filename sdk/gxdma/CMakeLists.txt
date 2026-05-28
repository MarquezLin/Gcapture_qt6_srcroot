if (NOT DEFINED GXDMA_SDK_VERSION)
  set(GXDMA_SDK_VERSION "0.1.0" CACHE STRING "gxdma SDK version")
endif()

project(gxdma VERSION ${GXDMA_SDK_VERSION} LANGUAGES CXX)

add_library(gxdma SHARED
    include/gxdma_capture.h
    src/gxdma_capture.cpp
    src/gcapture_log_bridge.cpp
    ${CMAKE_SOURCE_DIR}/sdk/gcapture/src/pipeline/shared_scene_pipeline.cpp
)

set_target_properties(gxdma PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gxdma
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/sdk/gvendor/include
        ${CMAKE_SOURCE_DIR}/sdk/gdriver_shared/include
        ${CMAKE_SOURCE_DIR}/sdk/gcapture/include
        ${CMAKE_SOURCE_DIR}/sdk/gcapture/src
        ${CMAKE_SOURCE_DIR}/sdk/gcapture/src/pipeline
)

target_compile_definitions(gxdma PRIVATE
    GXDMA_BUILD
    GXDMA_SDK_VERSION="${GXDMA_SDK_VERSION}"
)

target_link_libraries(gxdma PRIVATE
    gvendor
    ole32 user32 windowscodecs
    dxgi d3d11 d2d1 dwrite D3DCompiler
)

if (MSVC)
  target_compile_options(gxdma PRIVATE /utf-8)
endif()
