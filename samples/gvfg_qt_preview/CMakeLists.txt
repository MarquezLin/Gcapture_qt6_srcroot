cmake_minimum_required(VERSION 3.16)

project(gvfg_qt_preview LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

if (QT_VERSION_MAJOR GREATER_EQUAL 6)
  qt_add_executable(gvfg_qt_preview
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    previewwindow.cpp
    previewwindow.h
    previewwindow.ui
  )
else()
  add_executable(gvfg_qt_preview
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    previewwindow.cpp
    previewwindow.h
    previewwindow.ui
  )
endif()

target_link_libraries(gvfg_qt_preview PRIVATE
  Qt${QT_VERSION_MAJOR}::Widgets
  gvfg
)

target_include_directories(gvfg_qt_preview PRIVATE
  ${CMAKE_SOURCE_DIR}/sdk/gvfg/include
)

if (MSVC)
  target_compile_options(gvfg_qt_preview PRIVATE /utf-8)
endif()

add_custom_command(TARGET gvfg_qt_preview POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:gvfg>
    $<TARGET_FILE_DIR:gvfg_qt_preview>
)

set_target_properties(gvfg_qt_preview PROPERTIES
  WIN32_EXECUTABLE TRUE
)
