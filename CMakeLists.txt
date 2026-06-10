cmake_minimum_required(VERSION 3.20)
project(toheart2_linux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)

add_library(th2_archive
    native/archive.cpp
)
target_include_directories(th2_archive PUBLIC native)
target_compile_options(th2_archive PRIVATE -Wall -Wextra -Wpedantic)

add_executable(th2-archive-info
    native/archive_info.cpp
)
target_link_libraries(th2-archive-info PRIVATE th2_archive)

add_executable(th2-sdl-smoke
    native/sdl_smoke.cpp
    native/image.cpp
)
target_link_libraries(th2-sdl-smoke PRIVATE SDL3::SDL3 th2_archive)
