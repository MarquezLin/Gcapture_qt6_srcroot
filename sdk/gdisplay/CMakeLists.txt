project(gdisplay LANGUAGES CXX)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_library(gdisplay SHARED
    display_info.cpp
    edid_reader.cpp
    display_info.h
    edid_reader.h
)

# Make GDISPLAY_API => __declspec(dllexport)
target_compile_definitions(gdisplay PRIVATE GDISPLAY_BUILD)

target_include_directories(gdisplay
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gdisplay
    PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        dxgi
)

if (MSVC)
  target_compile_options(gdisplay PRIVATE /utf-8)
endif()

# Optional NVAPI
if (EXISTS "${NVAPI_ROOT}/include" AND EXISTS "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_include_directories(gdisplay PRIVATE "${NVAPI_ROOT}/include")
  target_link_libraries(gdisplay PRIVATE "${NVAPI_ROOT}/lib/x64/nvapi64.lib")
  target_compile_definitions(gdisplay PRIVATE GCAP_ENABLE_NVAPI)
endif()
