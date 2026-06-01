if (NOT DEFINED GVFG_SDK_VERSION)
  set(GVFG_SDK_VERSION "0.1.0" CACHE STRING "GVFG SDK version")
endif()

project(gvfg VERSION ${GVFG_SDK_VERSION} LANGUAGES CXX)

add_library(gvfg SHARED
    include/gvfg_capture.h
    src/gvfg_capture.cpp
    src/gcapture_log_bridge.cpp
    ${CMAKE_SOURCE_DIR}/sdk/gcapture/src/pipeline/shared_scene_pipeline.cpp
)

set_target_properties(gvfg PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_include_directories(gvfg
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_SOURCE_DIR}/sdk/gvendor/include
        ${CMAKE_SOURCE_DIR}/sdk/gdriver_shared/include
        ${CMAKE_SOURCE_DIR}/sdk/gcapture/include
        ${CMAKE_SOURCE_DIR}/sdk/gcapture/src
        ${CMAKE_SOURCE_DIR}/sdk/gcapture/src/pipeline
)

target_compile_definitions(gvfg PRIVATE
    GVFG_BUILD
    GVFG_SDK_VERSION="${GVFG_SDK_VERSION}"
)

target_link_libraries(gvfg PRIVATE
    gvendor
    ole32 user32 windowscodecs
    dxgi d3d11 d2d1 dwrite D3DCompiler
)

if (MSVC)
  target_compile_options(gvfg PRIVATE /utf-8)
endif()
