cmake_minimum_required(VERSION 3.16)

if (NOT DEFINED GVENDOR_SDK_VERSION)
  set(GVENDOR_SDK_VERSION "0.1.0" CACHE STRING "gvendor SDK version")
endif()

option(GVENDOR_XDMA_DEBUG_LOG "Enable verbose XDMA flow debug logging in gvendor" OFF)

project(gvendor VERSION ${GVENDOR_SDK_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(gvendor SHARED
  src/c_api.cpp
  src/xdma_capture_session.cpp
  src/xdma_capture_session.h
  src/xdma_public.h
)

set_target_properties(gvendor PROPERTIES
  AUTOMOC OFF
  AUTOUIC OFF
  AUTORCC OFF
)

target_include_directories(gvendor
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../gdriver_shared/include
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(gvendor PRIVATE
  GVENDOR_BUILD
  GVENDOR_SDK_VERSION="${GVENDOR_SDK_VERSION}"
)

if (GVENDOR_XDMA_DEBUG_LOG)
  target_compile_definitions(gvendor PRIVATE GVENDOR_XDMA_DEBUG_LOG=1)
endif()

if (WIN32)
  target_link_libraries(gvendor PRIVATE
    setupapi
  )

  if (MSVC)
    target_compile_options(gvendor PRIVATE /utf-8)
  endif()
endif()
