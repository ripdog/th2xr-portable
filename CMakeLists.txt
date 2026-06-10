cmake_minimum_required(VERSION 3.20)
project(toheart2_linux LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)

add_library(th2_archive
    native/archive.cpp
)
target_include_directories(th2_archive PUBLIC native)
target_compile_options(th2_archive PRIVATE -Wall -Wextra -Wpedantic)

add_library(th2_scenario
    native/scenario.cpp
)
target_include_directories(th2_scenario PUBLIC native)
target_compile_options(th2_scenario PRIVATE -Wall -Wextra -Wpedantic)

add_library(th2_bytecode
    native/bytecode.cpp
    native/event.cpp
    native/event_metadata.c
    native/expression.cpp
    native/vm.cpp
)
target_include_directories(th2_bytecode PUBLIC native)
target_compile_options(th2_bytecode PRIVATE -Wall -Wextra -Wpedantic)

add_library(th2_runtime
    native/script_runtime.cpp
)
target_include_directories(th2_runtime PUBLIC native)
target_compile_options(th2_runtime PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(th2_runtime PUBLIC th2_archive th2_scenario th2_bytecode)

add_executable(th2-archive-info
    native/archive_info.cpp
)
target_link_libraries(th2-archive-info PRIVATE th2_archive)

add_executable(th2-scenario-info
    native/scenario_info.cpp
)
target_link_libraries(th2-scenario-info PRIVATE th2_archive th2_scenario)

add_executable(th2-bytecode-info
    native/bytecode_info.cpp
)
target_link_libraries(th2-bytecode-info PRIVATE th2_archive th2_scenario th2_bytecode)

add_executable(th2-vm-info
    native/vm_info.cpp
)
target_link_libraries(th2-vm-info PRIVATE th2_archive th2_scenario th2_bytecode)

add_executable(th2-script-info
    native/script_info.cpp
)
target_link_libraries(th2-script-info PRIVATE th2_runtime)

if(BUILD_TESTING)
    add_executable(th2-scenario-test
        native/scenario_test.cpp
    )
    target_link_libraries(th2-scenario-test PRIVATE th2_scenario)
    add_test(NAME scenario-parser COMMAND th2-scenario-test)

    add_executable(th2-bytecode-test
        native/bytecode_test.cpp
    )
    target_link_libraries(th2-bytecode-test PRIVATE th2_bytecode)
    add_test(NAME bytecode-decoder COMMAND th2-bytecode-test)

    add_executable(th2-vm-test
        native/vm_test.cpp
    )
    target_link_libraries(th2-vm-test PRIVATE th2_bytecode th2_scenario)
    add_test(NAME scenario-vm COMMAND th2-vm-test)

    add_executable(th2-event-test
        native/event_test.cpp
    )
    target_link_libraries(th2-event-test PRIVATE th2_bytecode)
    add_test(NAME event-decoder COMMAND th2-event-test)

    add_executable(th2-expression-test
        native/expression_test.cpp
    )
    target_link_libraries(th2-expression-test PRIVATE th2_bytecode)
    add_test(NAME expression-evaluator COMMAND th2-expression-test)

endif()

add_executable(th2-sdl-smoke
    native/sdl_smoke.cpp
    native/image.cpp
)
target_link_libraries(th2-sdl-smoke PRIVATE SDL3::SDL3 th2_archive)

add_executable(th2-image-info
    native/image_info.cpp
    native/image.cpp
)
target_link_libraries(th2-image-info PRIVATE SDL3::SDL3 th2_archive)

add_executable(toheart2
    native/font.cpp
    native/game.cpp
    native/image.cpp
    native/message.cpp
)
target_link_libraries(toheart2 PRIVATE SDL3::SDL3 th2_runtime)

if(BUILD_TESTING)
    add_executable(th2-message-test
        native/message.cpp
        native/message_test.cpp
    )
    target_include_directories(th2-message-test PRIVATE native)
    add_test(NAME message-pages COMMAND th2-message-test)
endif()
