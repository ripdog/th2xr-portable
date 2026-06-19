#include "game.hpp"

#include "icon.hpp"
#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <imgui.h>
#include <zstd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <exception>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numbers>
#include <ranges>
#include <stdexcept>
#include <unordered_set>

#ifdef _WIN32
#define localtime_r(timep, result) localtime_s(result, timep)
#endif

namespace th2app {

void Game::push_backlog()
{
    if (message_.empty()) return;
    const auto& text = message_.visible();
    if (!backlog_.empty() && backlog_.back().text == text) return;
    backlog_.push_back({text, current_backlog_voices_});
    if (backlog_.size() > 256) backlog_.erase(backlog_.begin());
}

void Game::open_system_menu()
{
    if (choosing_) return;
    play_se(-1, 9104, false, 255);
    save_snapshot_ = capture_frame_pixels();
    begin_transition(1, 12, 128, false);
    ui_mode_ = UiMode::system_menu;
    menu_highlight_ = 4;
}

void Game::reset_play_state()
{
    bgm_.stop();
    bgm_track_ = -1;
    bgm_loop_ = false;
    bgm_volume_ = 255;
    for (auto& channel : transient_se_) {
        channel.stop();
    }
    transient_se_volume_ = {};
    for (auto& channel : se_channels_) {
        channel.stop();
    }
    se_sound_.fill(-1);
    se_loop_ = {};
    se_volume_ = {};
    for (auto& channel : voice_channels_) {
        channel.stop();
    }
    voice_sound_.fill(-1);
    voice_character_ = {};
    voice_scenario_ = {};
    voice_volume_ = {};
    voice_loop_ = {};
    vi_event_voice_no_ = -1;
    vi_event_voice_no_all_ = -1;
    movie_.reset();
    movie_bytes_.clear();
    movie_resume_script_ = false;
    movie_mode_ = -1;

    background_.reset();
    bg_scene_ = -1;
    background_kind_ = BackgroundKind::background;
    background_view_ = {0.0f, 0.0f, 800.0f, 600.0f};
    background_scroll_.reset();
    background_tone_curve_.clear();
    background_brightness_ = {128.0f, 128.0f, 128.0f};
    tone_ = 0;
    tone_back_ = -1;
    tone_char_ = -1;
    weather_ = 0;
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        overlays_[i].reset();
        overlay_pixels_[i].reset();
        overlay_states_[i] = {};
    }
    clear_characters();
    sakura_.reset();

    message_ = th2::Message{};
    message_visible_ = true;
    message_ends_block_ = true;
    waiting_for_input_ = false;
    current_line_key_.clear();
    backlog_.clear();
    current_backlog_voices_.clear();
    pending_backlog_voice_.reset();
    backlog_depth_ = 0;
    choices_.clear();
    choosing_ = false;
    choice_highlight_ = 0;
    choice_selected_ = -1;
    choice_result_register_ = -1;
    choice_ex_ = false;
    map_events_.clear();
    map_characters_.clear();
    map_selected_ = -1;

    wake_time_.reset();
    audio_wait_.reset();
    transition_.reset();
    background_fade_.reset();
    screen_flash_.reset();
    shake_.reset();
    clock_state_.reset();
    calendar_state_.reset();
    skipped_month_ = 0;
    skipped_day_ = 0;
    auto_next_time_.reset();
    auto_mode_ = false;
    skip_mode_ = false;
    demo_mode_ = false;
    replay_mode_ = false;
    demo_delay_frames_ = 0;
}

void Game::initialize_scenario_flags()
{
    runtime_.reset_flags();
    runtime_.set_flag(0, 3);
    runtime_.set_flag(1, 1);
    runtime_.set_flag(2, 0);
    runtime_.set_flag(3, -1);
    runtime_.set_flag(4, 0);
    runtime_.set_flag(
        5, th2::uses_default_voice_name(
               config_.player_name, default_player_name_)
            ? 1 : 0);
    runtime_.set_flag(6, 0);
    runtime_.set_flag(7, 0);
}

void Game::start_new_game()
{
    reset_play_state();
    initialize_scenario_flags();
    direct_scenario_ = false;
    load_script("EV_0301MORNING.SDT");
    ui_mode_ = UiMode::game;
    advance();
}

void Game::open_name_input()
{
    name_input_open_ = true;
    name_error_.clear();
    const auto copy = [](auto& destination, const std::string& source) {
        std::snprintf(
            destination.data(), destination.size(), "%s", source.c_str());
    };
    copy(name_family_, config_.player_name.family);
    copy(name_given_, config_.player_name.given);
    copy(name_family_reading_, config_.player_name.family_reading);
    copy(name_given_reading_, config_.player_name.given_reading);
    copy(name_nickname_, config_.player_name.nickname);
}

void Game::begin_title_exit(bool start_game)
{
    if (title_exit_started_) {
        return;
    }
    title_exit_game_ = start_game;
    title_exit_started_ = std::chrono::steady_clock::now();
}

void Game::begin_title_menu_transition(bool extras)
{
    if (title_menu_transition_started_ || title_extras_ == extras) {
        return;
    }
    title_extras_transition_from_ = title_extras_;
    title_extras_ = extras;
    title_menu_transition_started_ = std::chrono::steady_clock::now();
}

void Game::update_title()
{
    if (title_menu_transition_started_) {
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now()
            - *title_menu_transition_started_).count();
        if (elapsed * 60.0 >= 24.0) {
            title_menu_transition_started_.reset();
        }
    }
    if (!title_exit_started_) {
        return;
    }
    const auto elapsed =
        std::chrono::steady_clock::now() - *title_exit_started_;
    if (elapsed < std::chrono::milliseconds(32 * 1000 / 60)) {
        return;
    }
    const bool start_game = title_exit_game_;
    title_exit_started_.reset();
    if (start_game) {
        open_name_input();
    } else {
        running_ = false;
    }
}

void Game::close_system_menu()
{
    begin_transition(1, 12, 128, false);
    ui_mode_ = UiMode::game;
}

void Game::open_config()
{
    config_open_ = true;
}

void Game::close_config()
{
    config_open_ = false;
    th2::save_config(config_path_, config_);
}

void Game::append_u16(
    std::vector<std::uint8_t>& data, std::uint16_t value)
{
    data.push_back(static_cast<std::uint8_t>(value & 0xff));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void Game::append_u32(
    std::vector<std::uint8_t>& data, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        data.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void Game::append_u64(
    std::vector<std::uint8_t>& data, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        data.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

std::uint16_t Game::read_u16(
    const std::vector<std::uint8_t>& data, std::size_t& offset)
{
    if (offset + 2 > data.size()) {
        throw std::runtime_error("truncated save bundle");
    }
    const auto value = static_cast<std::uint16_t>(
        data[offset] | (data[offset + 1] << 8));
    offset += 2;
    return value;
}

std::uint32_t Game::read_u32(
    const std::vector<std::uint8_t>& data, std::size_t& offset)
{
    if (offset + 4 > data.size()) {
        throw std::runtime_error("truncated save bundle");
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[offset + i]) << (i * 8);
    }
    offset += 4;
    return value;
}

std::uint64_t Game::read_u64(
    const std::vector<std::uint8_t>& data, std::size_t& offset)
{
    if (offset + 8 > data.size()) {
        throw std::runtime_error("truncated save bundle");
    }
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + i]) << (i * 8);
    }
    offset += 8;
    return value;
}

std::vector<std::uint8_t> Game::read_local_file(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("could not read " + path.string());
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void Game::write_local_file(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("could not write " + path.string());
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        throw std::runtime_error("could not finish writing " + path.string());
    }
}

std::vector<std::uint8_t> Game::read_sdl_file(const std::string& path)
{
    IoPtr stream(SDL_IOFromFile(path.c_str(), "rb"));
    if (!stream) {
        throw std::runtime_error(
            "could not open bundle: " + std::string(SDL_GetError()));
    }
    std::vector<std::uint8_t> bytes;
    const Sint64 size = SDL_GetIOSize(stream.get());
    if (size > 0) {
        bytes.resize(static_cast<std::size_t>(size));
        std::size_t read = 0;
        while (read < bytes.size()) {
            const auto n = SDL_ReadIO(
                stream.get(), bytes.data() + read, bytes.size() - read);
            if (n == 0) {
                throw std::runtime_error("could not read complete bundle");
            }
            read += n;
        }
        return bytes;
    }
    std::array<std::uint8_t, 64 * 1024> buffer{};
    for (;;) {
        const auto n = SDL_ReadIO(stream.get(), buffer.data(), buffer.size());
        if (n == 0) {
            break;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + n);
    }
    return bytes;
}

void Game::write_sdl_file(
    const std::string& path, const std::vector<std::uint8_t>& bytes)
{
    IoPtr stream(SDL_IOFromFile(path.c_str(), "wb"));
    if (!stream) {
        throw std::runtime_error(
            "could not create bundle: " + std::string(SDL_GetError()));
    }
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto n = SDL_WriteIO(
            stream.get(), bytes.data() + written, bytes.size() - written);
        if (n == 0) {
            throw std::runtime_error("could not write complete bundle");
        }
        written += n;
    }
}

bool Game::safe_bundle_path(std::string_view path)
{
    if (path.empty() || path.starts_with('/') || path.starts_with('\\')
        || path.find(':') != std::string_view::npos
        || path.find('\\') != std::string_view::npos) {
        return false;
    }
    const std::filesystem::path parsed(path);
    for (const auto& part : parsed) {
        if (part == "." || part == "..") {
            return false;
        }
    }
    if (path.starts_with("save/")) {
        return true;
    }
    return parsed.parent_path().empty() && parsed.extension() == ".ini";
}

std::vector<std::pair<std::string, std::filesystem::path>>
Game::save_bundle_files() const
{
    std::vector<std::pair<std::string, std::filesystem::path>> files;
    std::unordered_set<std::string> seen;
    const auto root = writable_directory();
    const auto add_file =
        [&](std::string relative, const std::filesystem::path& path) {
            if (!std::filesystem::is_regular_file(path)
                || !seen.insert(relative).second) {
                return;
            }
            files.emplace_back(std::move(relative), path);
        };

    if (std::filesystem::is_directory(root)) {
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            if (entry.is_regular_file()
                && entry.path().extension() == ".ini") {
                add_file(entry.path().filename().generic_string(), entry.path());
            }
        }
    }
    add_file(config_path_.filename().generic_string(), config_path_);

    const auto save_dir = root / "save";
    if (std::filesystem::is_directory(save_dir)) {
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(save_dir)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const auto relative =
                std::filesystem::relative(entry.path(), root)
                    .generic_string();
            if (safe_bundle_path(relative)) {
                add_file(relative, entry.path());
            }
        }
    }
    std::ranges::sort(
        files, {},
        &std::pair<std::string, std::filesystem::path>::first);
    return files;
}

std::vector<std::uint8_t> Game::build_save_bundle()
{
    sync_window_config();
    th2::save_config(config_path_, config_);

    const auto files = save_bundle_files();
    std::vector<std::uint8_t> payload;
    append_u32(payload, static_cast<std::uint32_t>(files.size()));
    for (const auto& [relative, path] : files) {
        const auto bytes = read_local_file(path);
        if (relative.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::runtime_error("save bundle path is too long");
        }
        append_u16(payload, static_cast<std::uint16_t>(relative.size()));
        payload.insert(payload.end(), relative.begin(), relative.end());
        append_u64(payload, static_cast<std::uint64_t>(bytes.size()));
        payload.insert(payload.end(), bytes.begin(), bytes.end());
    }

    const auto bound = ZSTD_compressBound(payload.size());
    std::vector<std::uint8_t> compressed(bound);
    const auto compressed_size = ZSTD_compress(
        compressed.data(), compressed.size(),
        payload.data(), payload.size(), 10);
    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error(ZSTD_getErrorName(compressed_size));
    }
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> bundle;
    constexpr std::string_view magic = "TH2SAVES";
    bundle.insert(bundle.end(), magic.begin(), magic.end());
    append_u32(bundle, 1);
    append_u64(bundle, static_cast<std::uint64_t>(payload.size()));
    bundle.insert(bundle.end(), compressed.begin(), compressed.end());
    return bundle;
}

void Game::export_save_bundle(const std::string& selected_path)
{
    std::string path = selected_path;
    if (!path.starts_with("content://")
        && std::filesystem::path(path).extension() != ".th2saves") {
        path += ".th2saves";
    }
    const auto bundle = build_save_bundle();
    write_sdl_file(path, bundle);
    save_bundle_status_ =
        std::format("Exported save bundle ({} bytes).", bundle.size());
}

void Game::import_save_bundle(const std::string& path)
{
    const auto bundle = read_sdl_file(path);
    constexpr std::string_view magic = "TH2SAVES";
    if (bundle.size() < magic.size() + 12
        || !std::equal(magic.begin(), magic.end(), bundle.begin())) {
        throw std::runtime_error("not a ToHeart2 save bundle");
    }
    std::size_t offset = magic.size();
    const auto version = read_u32(bundle, offset);
    if (version != 1) {
        throw std::runtime_error("unsupported save bundle version");
    }
    const auto payload_size = read_u64(bundle, offset);
    if (payload_size > 512ull * 1024ull * 1024ull) {
        throw std::runtime_error("save bundle is too large");
    }
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_size));
    const auto result = ZSTD_decompress(
        payload.data(), payload.size(),
        bundle.data() + offset, bundle.size() - offset);
    if (ZSTD_isError(result) || result != payload.size()) {
        throw std::runtime_error("could not decompress save bundle");
    }

    std::size_t read_offset = 0;
    const auto file_count = read_u32(payload, read_offset);
    if (file_count > 1000) {
        throw std::runtime_error("save bundle contains too many files");
    }
    const auto root = writable_directory();
    for (std::uint32_t i = 0; i < file_count; ++i) {
        const auto path_size = read_u16(payload, read_offset);
        if (read_offset + path_size > payload.size()) {
            throw std::runtime_error("truncated save bundle path");
        }
        const std::string relative(
            reinterpret_cast<const char*>(payload.data() + read_offset),
            path_size);
        read_offset += path_size;
        if (!safe_bundle_path(relative)) {
            throw std::runtime_error("unsafe path in save bundle: " + relative);
        }
        const auto file_size = read_u64(payload, read_offset);
        if (file_size > 64ull * 1024ull * 1024ull
            || read_offset + file_size > payload.size()) {
            throw std::runtime_error("invalid file size in save bundle");
        }
        std::vector<std::uint8_t> bytes(
            payload.begin() + static_cast<std::ptrdiff_t>(read_offset),
            payload.begin()
                + static_cast<std::ptrdiff_t>(read_offset + file_size));
        read_offset += static_cast<std::size_t>(file_size);
        write_local_file(root / relative, bytes);
    }
    if (read_offset != payload.size()) {
        throw std::runtime_error("trailing data in save bundle");
    }

    config_ = th2::load_config(config_path_);
    if (config_.player_name.family.empty()) {
        config_.player_name = default_player_name_;
    }
    for (std::size_t i = 0; i < config_.game_flags.size(); ++i) {
        runtime_.set_game_flag(i, config_.game_flags[i]);
    }
    apply_audio_gains();
    refresh_save_page();
    save_bundle_status_ =
        std::format("Imported {} file{}.", file_count, file_count == 1 ? "" : "s");
}

void Game::save_bundle_dialog_callback(
    void* userdata, const char* const* filelist, int)
{
    auto* dialog = static_cast<SaveBundleDialog*>(userdata);
    std::lock_guard lock(dialog->mutex);
    if (!filelist) {
        dialog->error = SDL_GetError();
    } else if (filelist[0]) {
        dialog->path = filelist[0];
    }
    dialog->done = true;
}

void Game::show_save_bundle_export_dialog()
{
    std::lock_guard lock(save_bundle_dialog_.mutex);
    if (save_bundle_dialog_.active) {
        return;
    }
    save_bundle_dialog_.active = true;
    save_bundle_dialog_.done = false;
    save_bundle_dialog_.path.reset();
    save_bundle_dialog_.error.clear();
    save_bundle_dialog_.action = SaveBundleAction::export_bundle;
    save_bundle_status_ = "Waiting for export location...";
    const SDL_DialogFileFilter filters[] = {
        {"ToHeart2 saves", "th2saves"},
        {"All files", "*"},
    };
    SDL_ShowSaveFileDialog(
        save_bundle_dialog_callback, &save_bundle_dialog_, window_,
        filters, std::size(filters), "toheart2-saves.th2saves");
}

void Game::show_save_bundle_import_dialog()
{
    std::lock_guard lock(save_bundle_dialog_.mutex);
    if (save_bundle_dialog_.active) {
        return;
    }
    save_bundle_dialog_.active = true;
    save_bundle_dialog_.done = false;
    save_bundle_dialog_.path.reset();
    save_bundle_dialog_.error.clear();
    save_bundle_dialog_.action = SaveBundleAction::import_bundle;
    save_bundle_status_ = "Waiting for bundle selection...";
    const SDL_DialogFileFilter filters[] = {
        {"ToHeart2 saves", "th2saves"},
        {"All files", "*"},
    };
    SDL_ShowOpenFileDialog(
        save_bundle_dialog_callback, &save_bundle_dialog_, window_,
        filters, std::size(filters), nullptr, false);
}

void Game::process_save_bundle_dialog()
{
    SaveBundleAction action = SaveBundleAction::none;
    std::optional<std::string> path;
    std::string error;
    {
        std::lock_guard lock(save_bundle_dialog_.mutex);
        if (!save_bundle_dialog_.active || !save_bundle_dialog_.done) {
            return;
        }
        action = save_bundle_dialog_.action;
        path = save_bundle_dialog_.path;
        error = save_bundle_dialog_.error;
        save_bundle_dialog_.action = SaveBundleAction::none;
        save_bundle_dialog_.active = false;
        save_bundle_dialog_.done = false;
        save_bundle_dialog_.path.reset();
        save_bundle_dialog_.error.clear();
    }
    if (!error.empty()) {
        save_bundle_status_ = "File dialog failed: " + error;
        return;
    }
    if (!path) {
        save_bundle_status_ = "Save bundle operation cancelled.";
        return;
    }
    try {
        if (action == SaveBundleAction::export_bundle) {
            export_save_bundle(*path);
        } else if (action == SaveBundleAction::import_bundle) {
            import_save_bundle(*path);
        }
    } catch (const std::exception& exception) {
        save_bundle_status_ =
            std::string("Save bundle failed: ") + exception.what();
    }
}


}  // namespace th2app
