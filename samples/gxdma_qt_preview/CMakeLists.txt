cmake_minimum_required(VERSION 3.16)

project(gxdma_qt_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

if (QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(gxdma_qt_preview
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
  )
else()
  add_executable(gxdma_qt_preview
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
  )
endif()

target_link_libraries(gxdma_qt_preview PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  gxdma
)

target_include_directories(gxdma_qt_preview PRIVATE
  ${CMAKE_SOURCE_DIR}/sdk/gxdma/include
)

if (MSVC)
  target_compile_options(gxdma_qt_preview PRIVATE /utf-8)
endif()

add_custom_command(TARGET gxdma_qt_preview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:gxdma>
    $<TARGET_FILE_DIR:gxdma_qt_preview>
)

add_custom_command(TARGET gxdma_qt_preview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:gvendor>
    $<TARGET_FILE_DIR:gxdma_qt_preview>
)

set_target_properties(gxdma_qt_preview PROPERTIES
  WIN32_EXECUTABLE TRUE
)
