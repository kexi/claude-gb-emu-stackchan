cmake_minimum_required(VERSION 3.20)

project(gb_embedded_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(
  gb_embedded_core
  STATIC
  core/apu.cpp
  core/cartridge.cpp
  core/chromatic.cpp
  core/cpu.cpp
  core/gb.cpp
  core/ppu.cpp
)
target_compile_definitions(gb_embedded_core PUBLIC GB_EMBEDDED)
target_include_directories(gb_embedded_core PUBLIC core)

add_executable(verify_embedded tools/verify_host.cpp)
target_link_libraries(verify_embedded PRIVATE gb_embedded_core)
