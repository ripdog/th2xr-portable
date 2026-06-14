cmake_minimum_required(VERSION 3.20)
project(toheart2_portable LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(Iconv REQUIRED)
if(DEFINED VCPKG_TARGET_TRIPLET)
    find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED
        HINTS "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/tools/glslang")
else()
    find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
endif()
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
pkg_check_modules(SDL3_TTF REQUIRED IMPORTED_TARGET sdl3-ttf)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)

add_library(th2_archive
    src/archive.cpp
)
target_include_directories(th2_archive PUBLIC src)
target_compile_options(th2_archive PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>>)

add_library(th2_scenario
    src/scenario.cpp
)
target_include_directories(th2_scenario PUBLIC src)
target_compile_options(th2_scenario PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

add_library(th2_bytecode
    src/bytecode.cpp
    src/event.cpp
    src/event_metadata.c
    src/expression.cpp
    src/vm.cpp
)
target_include_directories(th2_bytecode PUBLIC src)
target_compile_options(th2_bytecode PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
target_link_libraries(th2_bytecode PUBLIC Iconv::Iconv)
add_library(th2_runtime
    src/script_runtime.cpp
)
target_include_directories(th2_runtime PUBLIC src)
target_compile_options(th2_runtime PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
target_link_libraries(th2_runtime PUBLIC th2_archive th2_scenario th2_bytecode)

add_library(th2_audio
    src/audio.cpp
)
target_include_directories(th2_audio PUBLIC src)
target_compile_options(th2_audio PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)
target_link_libraries(th2_audio PUBLIC SDL3::SDL3 PkgConfig::SNDFILE)

add_library(th2_character
    src/character.cpp
)
target_include_directories(th2_character PUBLIC src)
target_compile_options(th2_character PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

add_library(th2_config
    src/config.cpp
    src/player_name.cpp
)
target_include_directories(th2_config PUBLIC src)
target_compile_options(th2_config PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

add_library(imgui
    third_party/imgui/imgui.cpp
    third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp
    third_party/imgui/imgui_widgets.cpp
)
target_include_directories(imgui PUBLIC third_party/imgui)
target_compile_options(imgui PRIVATE -w)

add_executable(th2-archive-info
    src/archive_info.cpp
)
target_link_libraries(th2-archive-info PRIVATE th2_archive)

add_executable(th2-scenario-info
    src/scenario_info.cpp
)
target_link_libraries(th2-scenario-info PRIVATE th2_archive th2_scenario)

add_executable(th2-bytecode-info
    src/bytecode_info.cpp
)
target_link_libraries(th2-bytecode-info PRIVATE th2_archive th2_scenario th2_bytecode)

add_executable(th2-vm-info
    src/vm_info.cpp
)
target_link_libraries(th2-vm-info PRIVATE th2_archive th2_scenario th2_bytecode)

add_executable(th2-script-info
    src/script_info.cpp
)
target_link_libraries(th2-script-info PRIVATE th2_runtime)

add_executable(th2-script-audit
    src/script_audit.cpp
)
target_link_libraries(th2-script-audit PRIVATE
    th2_runtime)

add_executable(th2-conformance-generator
    conformance/make_th2_conformance.cpp
)
target_link_libraries(th2-conformance-generator PRIVATE th2_bytecode)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_SOURCE_DIR}/conformance/999999999.SDT"
    COMMAND th2-conformance-generator
        "${CMAKE_CURRENT_SOURCE_DIR}/conformance/999999999.SDT"
    DEPENDS th2-conformance-generator
    VERBATIM)
add_custom_target(th2-conformance ALL
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/conformance/999999999.SDT")

if(BUILD_TESTING)
    add_executable(th2-scenario-test
        src/scenario_test.cpp
    )
    target_link_libraries(th2-scenario-test PRIVATE th2_scenario)
    add_test(NAME scenario-parser COMMAND th2-scenario-test)

    add_executable(th2-bytecode-test
        src/bytecode_test.cpp
    )
    target_link_libraries(th2-bytecode-test PRIVATE th2_bytecode)
    add_test(NAME bytecode-decoder COMMAND th2-bytecode-test)

    add_executable(th2-vm-test
        src/vm_test.cpp
    )
    target_link_libraries(th2-vm-test PRIVATE th2_bytecode th2_scenario)
    add_test(NAME scenario-vm COMMAND th2-vm-test)

    add_executable(th2-event-test
        src/event_test.cpp
    )
    target_link_libraries(th2-event-test PRIVATE th2_bytecode)
    add_test(NAME event-decoder COMMAND th2-event-test)

    add_executable(th2-expression-test
        src/expression_test.cpp
    )
    target_link_libraries(th2-expression-test PRIVATE th2_bytecode)
    add_test(NAME expression-evaluator COMMAND th2-expression-test)

    add_executable(th2-script-runtime-test
        src/script_runtime_test.cpp
    )
    target_link_libraries(th2-script-runtime-test PRIVATE th2_runtime)
    add_test(NAME script-runtime-loose-lookup COMMAND th2-script-runtime-test)

    add_executable(th2-tone-curve-test
        src/tone_curve_test.cpp
        src/image.cpp
    )
    target_link_libraries(th2-tone-curve-test PRIVATE SDL3::SDL3)
    add_test(NAME tone-curve COMMAND th2-tone-curve-test)

endif()

add_executable(th2-sdl-smoke
    src/sdl_smoke.cpp
    src/image.cpp
)
target_link_libraries(th2-sdl-smoke PRIVATE SDL3::SDL3 th2_archive)

add_executable(th2-image-info
    src/image_info.cpp
    src/image.cpp
)
target_link_libraries(th2-image-info PRIVATE SDL3::SDL3 th2_archive)

add_executable(toheart2
    src/anime4k.cpp
    src/font.cpp
    src/game.cpp
    src/image.cpp
    src/imgui_layer.cpp
    src/message.cpp
    src/upscaler.cpp
    src/video.cpp
)
set(ANIME4K_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/anime4k")
set(ANIME4K_SHADERS apply)
foreach(SHADER IN LISTS ANIME4K_SHADERS)
    add_custom_command(
        OUTPUT "${ANIME4K_SHADER_DIR}/${SHADER}.frag.spv"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ANIME4K_SHADER_DIR}"
        COMMAND "${GLSLANG_VALIDATOR}" -V
            "${CMAKE_CURRENT_SOURCE_DIR}/shaders/anime4k/${SHADER}.frag"
            -o "${ANIME4K_SHADER_DIR}/${SHADER}.frag.spv"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/anime4k/${SHADER}.frag"
        VERBATIM)
    list(APPEND ANIME4K_SHADER_OUTPUTS
        "${ANIME4K_SHADER_DIR}/${SHADER}.frag.spv")
endforeach()
add_custom_target(anime4k-shaders DEPENDS ${ANIME4K_SHADER_OUTPUTS})
add_dependencies(toheart2 anime4k-shaders)
target_compile_definitions(toheart2 PRIVATE
    TH2_ANIME4K_SHADER_DIR="${ANIME4K_SHADER_DIR}")

add_executable(th2-anime4k-test
    src/anime4k.cpp
    src/anime4k_test.cpp
)
add_dependencies(th2-anime4k-test anime4k-shaders)
target_link_libraries(th2-anime4k-test PRIVATE SDL3::SDL3)
target_link_libraries(toheart2 PRIVATE
    SDL3::SDL3 PkgConfig::SDL3_TTF PkgConfig::FONTCONFIG th2_runtime)
target_link_libraries(toheart2 PRIVATE th2_audio th2_character th2_config imgui)
target_link_libraries(toheart2 PRIVATE PkgConfig::FFMPEG)

add_executable(th2-video-test
    src/video_test.cpp
    src/video.cpp
)
target_link_libraries(th2-video-test PRIVATE
    th2_archive SDL3::SDL3 PkgConfig::FFMPEG)

if(BUILD_TESTING)
    add_executable(th2-character-test
        src/character_test.cpp
    )
    target_link_libraries(th2-character-test PRIVATE th2_character)
    add_test(NAME character-state COMMAND th2-character-test)

    add_executable(th2-message-test
        src/message.cpp
        src/message_test.cpp
    )
    target_include_directories(th2-message-test PRIVATE src)
    add_test(NAME message-pages COMMAND th2-message-test)

    add_executable(th2-audio-test
        src/audio_test.cpp
    )
    target_link_libraries(th2-audio-test PRIVATE th2_audio th2_archive)
    add_test(NAME audio-decoder COMMAND th2-audio-test)

    add_executable(th2-choice-test
        src/choice_test.cpp
    )
    target_link_libraries(th2-choice-test PRIVATE th2_runtime)
    add_test(NAME choice-events COMMAND th2-choice-test)

    add_executable(th2-save-test
        src/message.cpp
        src/save_test.cpp
    )
    target_include_directories(th2-save-test PRIVATE src)
    target_link_libraries(th2-save-test PRIVATE th2_runtime th2_character SDL3::SDL3)
    add_test(NAME save-roundtrip COMMAND th2-save-test)

    add_executable(th2-config-test
        src/config_test.cpp
    )
    target_link_libraries(th2-config-test PRIVATE th2_config)
    add_test(NAME config-roundtrip COMMAND th2-config-test)

    add_executable(th2-player-name-test
        src/player_name_test.cpp
    )
    target_link_libraries(th2-player-name-test PRIVATE th2_config)
    add_test(NAME player-name-substitution COMMAND th2-player-name-test)
endif()
