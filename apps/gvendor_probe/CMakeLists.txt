project(gvendor_probe LANGUAGES CXX)

add_executable(gvendor_probe
  main.cpp
)

target_link_libraries(gvendor_probe PRIVATE gvendor)

if (MSVC)
  target_compile_options(gvendor_probe PRIVATE /utf-8)
endif()
