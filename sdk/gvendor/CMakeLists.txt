if (NOT DEFINED GVENDOR_SDK_VERSION)
  set(GVENDOR_SDK_VERSION "0.1.0" CACHE STRING "gvendor SDK version")
endif()

project(gvendor VERSION ${GVENDOR_SDK_VERSION} LANGUAGES CXX)

add_library(gvendor SHARED
  src/c_api.cpp
  src/ks_capture_session.cpp
  src/ks_capture_session.h
  src/ks_device_enumerator.cpp
  src/ks_device_enumerator.h
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

if (WIN32)
  target_link_libraries(gvendor PRIVATE
    setupapi
    ksuser
    ole32
    uuid
  )

  if (MSVC)
    target_compile_options(gvendor PRIVATE /utf-8)
  endif()
endif()
