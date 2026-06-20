cmake_minimum_required(VERSION 3.20)
project(toheart2_portable LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" TH2_VERSION)
string(STRIP "${TH2_VERSION}" TH2_VERSION)

if(ANDROID)
    set(ENV{PKG_CONFIG_PATH} "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-android/lib/pkgconfig")
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
if(ANDROID)
    # Use a locally-built GNU libiconv: Bionic's iconv does not support
    # CP932/SHIFT_JIS, which the game needs for Japanese text.
    set(LIBICONV_ROOT "${CMAKE_SOURCE_DIR}/third_party/libiconv-android")
    set(LIBICONV_LIBRARY "${LIBICONV_ROOT}/lib/libiconv.a")
    set(LIBICONV_INCLUDE_DIR "${LIBICONV_ROOT}/include")
    if(NOT EXISTS "${LIBICONV_LIBRARY}")
        message(FATAL_ERROR
            "GNU libiconv not found at ${LIBICONV_LIBRARY}. "
            "Run third_party/build-libiconv-android.sh first.")
    endif()
    add_library(Iconv::Iconv STATIC IMPORTED GLOBAL)
    set_target_properties(Iconv::Iconv PROPERTIES
        IMPORTED_LOCATION "${LIBICONV_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBICONV_INCLUDE_DIR}")
else()
    find_package(Iconv REQUIRED)
endif()
if(ANDROID)
    set(SQLITE_ROOT "${CMAKE_SOURCE_DIR}/third_party/sqlite-android")
    set(SQLITE_LIBRARY "${SQLITE_ROOT}/lib/libsqlite3.a")
    set(SQLITE_INCLUDE_DIR "${SQLITE_ROOT}/include")
    if(NOT EXISTS "${SQLITE_LIBRARY}")
        message(FATAL_ERROR
            "SQLite not found at ${SQLITE_LIBRARY}. "
            "Run third_party/build-sqlite-android.sh first.")
    endif()
    add_library(SQLite3::SQLite3 STATIC IMPORTED GLOBAL)
    set_target_properties(SQLite3::SQLite3 PROPERTIES
        IMPORTED_LOCATION "${SQLITE_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SQLITE_INCLUDE_DIR}")
else()
    find_package(SQLite3 REQUIRED)
    if(NOT TARGET SQLite3::SQLite3) # CMake < 4.3
        add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
    endif()
endif()
if(WIN32)
    find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED
        HINTS "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/tools/glslang")
else()
    find_program(GLSLANG_VALIDATOR glslangValidator REQUIRED)
endif()

if(NOT ANDROID)
    pkg_check_modules(SDL3_TTF REQUIRED IMPORTED_TARGET sdl3-ttf)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
else()
    find_package(SDL3_ttf CONFIG REQUIRED)
endif()
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale libswresample)
if(ANDROID)
    set(ZSTD_ROOT "${CMAKE_SOURCE_DIR}/third_party/zstd-android")
    set(ZSTD_LIBRARY "${ZSTD_ROOT}/lib/libzstd.a")
    set(ZSTD_INCLUDE_DIR "${ZSTD_ROOT}/include")
    if(NOT EXISTS "${ZSTD_LIBRARY}")
        message(FATAL_ERROR
            "zstd not found at ${ZSTD_LIBRARY}. "
            "Run third_party/build-zstd-android.sh first.")
    endif()
    add_library(ZSTD::ZSTD STATIC IMPORTED GLOBAL)
    set_target_properties(ZSTD::ZSTD PROPERTIES
        IMPORTED_LOCATION "${ZSTD_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${ZSTD_INCLUDE_DIR}")
else()
    pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
    add_library(ZSTD::ZSTD ALIAS PkgConfig::ZSTD)
endif()
if(ANDROID)
    link_directories(${FFMPEG_LIBRARY_DIRS})
endif()

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
target_link_libraries(th2_audio PUBLIC SDL3::SDL3 PkgConfig::FFMPEG)

add_library(th2_character
    src/character.cpp
)
target_include_directories(th2_character PUBLIC src)
target_compile_options(th2_character PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

add_library(th2_config
    src/config.cpp
    src/persistent_state.cpp
    src/player_name.cpp
)
target_include_directories(th2_config PUBLIC src)
target_link_libraries(th2_config PUBLIC SQLite3::SQLite3)
target_compile_options(th2_config PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

add_library(th2_soak
    src/soak.cpp
)
target_include_directories(th2_soak PUBLIC src)
target_compile_options(th2_soak PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic>)

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

    add_executable(th2-soak-test
        src/soak_test.cpp
    )
    target_link_libraries(th2-soak-test PRIVATE th2_soak)
    add_test(NAME soak-explorer COMMAND th2-soak-test)

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

if(ANDROID)
    add_library(toheart2 SHARED
        src/anime4k.cpp
        src/font.cpp
        src/game.cpp
        src/game_backlog_touch.cpp
        src/game_config_ui.cpp
        src/game_events.cpp
        src/game_flow.cpp
        src/game_map.cpp
        src/game_media.cpp
        src/game_omake_input.cpp
        src/gamepad_input.cpp
        src/game_render_effects.cpp
        src/game_render_main.cpp
        src/game_save.cpp
        src/game_save_bundle.cpp
        src/game_save_ui.cpp
        src/game_state.cpp
        src/game_support.cpp
        src/game_title_draw.cpp
        src/icon.cpp
        src/image.cpp
        src/imgui_layer.cpp
        src/message.cpp
        src/touch_input.cpp
        src/upscaler.cpp
        src/video.cpp
    )
    set_target_properties(toheart2 PROPERTIES OUTPUT_NAME "main")
else()
    add_executable(toheart2
        src/anime4k.cpp
        src/font.cpp
        src/game.cpp
        src/game_backlog_touch.cpp
        src/game_config_ui.cpp
        src/game_events.cpp
        src/game_flow.cpp
        src/game_map.cpp
        src/game_media.cpp
        src/game_omake_input.cpp
        src/gamepad_input.cpp
        src/game_render_effects.cpp
        src/game_render_main.cpp
        src/game_save.cpp
        src/game_save_bundle.cpp
        src/game_save_ui.cpp
        src/game_state.cpp
        src/game_support.cpp
        src/game_title_draw.cpp
        src/icon.cpp
        src/image.cpp
        src/imgui_layer.cpp
        src/message.cpp
        src/touch_input.cpp
        src/upscaler.cpp
        src/video.cpp
    )
endif()
set(ANIME4K_SHADER_DIR "shaders/anime4k")
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
    if(ANDROID)
        set(ANDROID_SHADER_ASSET
            "${CMAKE_CURRENT_SOURCE_DIR}/android/app/src/main/assets/${ANIME4K_SHADER_DIR}/${SHADER}.frag.spv")
        add_custom_command(
            OUTPUT "${ANDROID_SHADER_ASSET}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/android/app/src/main/assets/${ANIME4K_SHADER_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy
                "${CMAKE_CURRENT_BINARY_DIR}/${ANIME4K_SHADER_DIR}/${SHADER}.frag.spv"
                "${ANDROID_SHADER_ASSET}"
            DEPENDS "${ANIME4K_SHADER_DIR}/${SHADER}.frag.spv"
            VERBATIM)
        list(APPEND ANDROID_SHADER_ASSET_OUTPUTS "${ANDROID_SHADER_ASSET}")
    endif()
endforeach()
add_custom_target(anime4k-shaders DEPENDS ${ANIME4K_SHADER_OUTPUTS})
add_dependencies(toheart2 anime4k-shaders)
if(ANDROID)
    add_custom_target(anime4k-shaders-android DEPENDS ${ANDROID_SHADER_ASSET_OUTPUTS})
    add_dependencies(toheart2 anime4k-shaders-android)
endif()
if(ANDROID)
    target_compile_definitions(toheart2 PRIVATE
        TH2_ANIME4K_SHADER_DIR="${ANIME4K_SHADER_DIR}"
        TH2_VERSION="${TH2_VERSION}"
        TH2_ANDROID_FONT_PATH="fonts/LiberationSerif-Regular.ttf"
        TH2_ANDROID_IMGUI_FONT_PATH="fonts/NotoSans-Regular.ttf")
else()
    target_compile_definitions(toheart2 PRIVATE
        TH2_ANIME4K_SHADER_DIR="${ANIME4K_SHADER_DIR}"
        TH2_VERSION="${TH2_VERSION}")
endif()

add_executable(th2-anime4k-test
    src/anime4k.cpp
    src/anime4k_test.cpp
)
add_dependencies(th2-anime4k-test anime4k-shaders)
target_link_libraries(th2-anime4k-test PRIVATE SDL3::SDL3)
if(ANDROID)
    target_link_libraries(toheart2 PRIVATE
        SDL3::SDL3 SDL3_ttf::SDL3_ttf th2_runtime th2_soak)
    # 16 KB page alignment required by Android 15+ (API 35+).
    target_link_options(toheart2 PRIVATE
        "-Wl,-z,max-page-size=16384"
        "-Wl,-z,common-page-size=16384")
else()
    target_link_libraries(toheart2 PRIVATE
        SDL3::SDL3 PkgConfig::SDL3_TTF PkgConfig::FONTCONFIG th2_runtime th2_soak)
endif()
target_link_libraries(toheart2 PRIVATE th2_audio th2_character th2_config imgui)
target_link_libraries(toheart2 PRIVATE PkgConfig::FFMPEG ZSTD::ZSTD)

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

    add_executable(th2-persistent-state-test
        src/persistent_state_test.cpp
    )
    target_link_libraries(th2-persistent-state-test PRIVATE th2_config)
    add_test(NAME persistent-state COMMAND th2-persistent-state-test)

    add_executable(th2-icon-test
        src/icon.cpp
        src/icon_test.cpp
    )
    target_link_libraries(th2-icon-test PRIVATE SDL3::SDL3)
    add_test(
        NAME executable-icon
        COMMAND th2-icon-test
            "${CMAKE_CURRENT_SOURCE_DIR}/legacy/ToHeart2/ScriptEngine/ToHeart2.exe")

    add_executable(th2-player-name-test
        src/player_name_test.cpp
    )
    target_link_libraries(th2-player-name-test PRIVATE th2_config)
    add_test(NAME player-name-substitution COMMAND th2-player-name-test)
endif()
