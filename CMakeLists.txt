cmake_minimum_required(VERSION 3.20)
project(toheart2_linux LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)

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

add_library(th2_audio
    native/audio.cpp
)
target_include_directories(th2_audio PUBLIC native)
target_compile_options(th2_audio PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(th2_audio PUBLIC SDL3::SDL3 PkgConfig::SNDFILE)

add_library(th2_character
    native/character.cpp
)
target_include_directories(th2_character PUBLIC native)
target_compile_options(th2_character PRIVATE -Wall -Wextra -Wpedantic)

add_library(th2_config
    native/config.cpp
    native/player_name.cpp
)
target_include_directories(th2_config PUBLIC native)
target_compile_options(th2_config PRIVATE -Wall -Wextra -Wpedantic)

add_library(imgui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_options(imgui PRIVATE -w)

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
    native/imgui_layer.cpp
    native/message.cpp
    native/video.cpp
)
target_link_libraries(toheart2 PRIVATE SDL3::SDL3 th2_runtime)
target_link_libraries(toheart2 PRIVATE th2_audio th2_character th2_config imgui)
target_link_libraries(toheart2 PRIVATE PkgConfig::FFMPEG)

add_executable(th2-video-test
    native/video_test.cpp
    native/video.cpp
)
target_link_libraries(th2-video-test PRIVATE
    th2_archive SDL3::SDL3 PkgConfig::FFMPEG)

if(BUILD_TESTING)
    add_executable(th2-character-test
        native/character_test.cpp
    )
    target_link_libraries(th2-character-test PRIVATE th2_character)
    add_test(NAME character-state COMMAND th2-character-test)

    add_executable(th2-message-test
        native/message.cpp
        native/message_test.cpp
    )
    target_include_directories(th2-message-test PRIVATE native)
    add_test(NAME message-pages COMMAND th2-message-test)

    add_executable(th2-audio-test
        native/audio_test.cpp
    )
    target_link_libraries(th2-audio-test PRIVATE th2_audio th2_archive)
    add_test(NAME audio-decoder COMMAND th2-audio-test)

    add_executable(th2-choice-test
        native/choice_test.cpp
    )
    target_link_libraries(th2-choice-test PRIVATE th2_runtime)
    add_test(NAME choice-events COMMAND th2-choice-test)

    add_executable(th2-save-test
        native/message.cpp
        native/save_test.cpp
    )
    target_include_directories(th2-save-test PRIVATE native)
    target_link_libraries(th2-save-test PRIVATE th2_runtime th2_character)
    add_test(NAME save-roundtrip COMMAND th2-save-test)

    add_executable(th2-config-test
        native/config_test.cpp
    )
    target_link_libraries(th2-config-test PRIVATE th2_config)
    add_test(NAME config-roundtrip COMMAND th2-config-test)

    add_executable(th2-player-name-test
        native/player_name_test.cpp
    )
    target_link_libraries(th2-player-name-test PRIVATE th2_config)
    add_test(NAME player-name-substitution COMMAND th2-player-name-test)
endif()
