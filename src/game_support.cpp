#include "game.hpp"

#include "image.hpp"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <fstream>
#include <mutex>
#include <stdexcept>

namespace th2app {

void TextureDeleter::operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
void SurfaceDeleter::operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
void WindowDeleter::operator()(SDL_Window* window) const { SDL_DestroyWindow(window); }
void RendererDeleter::operator()(SDL_Renderer* renderer) const { SDL_DestroyRenderer(renderer); }
void IoDeleter::operator()(SDL_IOStream* stream) const
{
    if (stream) {
        SDL_CloseIO(stream);
    }
}

// Directory for writable files (config, saves, logs). On Android the current
// working directory is not writable, so use the app-internal storage path.
// On desktop platforms use the system-preferred user data directory so the
// game works regardless of where the binary is launched from.
std::filesystem::path writable_directory()
{
#ifdef __ANDROID__
    return std::filesystem::path(SDL_GetAndroidInternalStoragePath());
#else
    char* path = SDL_GetPrefPath("ripdog", "ToHeart2XR");
    if (!path) {
        return std::filesystem::path(".");
    }
    std::filesystem::path result(path);
    SDL_free(path);
    return result;
#endif
}

std::filesystem::path profile_directory()
{
    return writable_directory() / "profile";
}

std::filesystem::path app_config_directory()
{
#ifdef __ANDROID__
    return std::filesystem::path(SDL_GetAndroidInternalStoragePath());
#else
    char* path = SDL_GetPrefPath("ripdog", "ToHeart2XR");
    if (!path) {
        return std::filesystem::path(".");
    }
    std::filesystem::path result(path);
    SDL_free(path);
    return result;
#endif
}

std::filesystem::path remembered_data_path_file()
{
    return app_config_directory() / "game-data-path.txt";
}

bool valid_game_data_directory(const std::filesystem::path& path)
{
    return std::filesystem::is_directory(path)
        && std::filesystem::exists(path / "TOHEART2.EXE")
        && std::filesystem::exists(path / "SDT.PAK")
        && std::filesystem::exists(path / "GRP.PAK");
}

std::optional<std::filesystem::path> load_remembered_data_path()
{
    std::ifstream input(remembered_data_path_file());
    std::string line;
    if (!std::getline(input, line) || line.empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(line);
}

void save_remembered_data_path(const std::filesystem::path& path)
{
    std::filesystem::create_directories(app_config_directory());
    std::ofstream output(remembered_data_path_file());
    output << path.string() << '\n';
}

std::optional<std::filesystem::path> pick_game_executable()
{
    struct DialogState {
        std::mutex mutex;
        bool done = false;
        std::optional<std::filesystem::path> path;
    } state;

    SDL_Window* window = SDL_CreateWindow(
        "Select TOHEART2.EXE", 640, 120, SDL_WINDOW_HIDDEN);
    WindowPtr window_holder(window);
    const SDL_DialogFileFilter filters[] = {
        {"ToHeart2 executable", "exe"},
        {"All files", "*"},
    };
    auto callback = [](void* userdata, const char* const* filelist, int) {
        auto* dialog = static_cast<DialogState*>(userdata);
        std::lock_guard lock(dialog->mutex);
        if (filelist && filelist[0]) {
            dialog->path = std::filesystem::path(filelist[0]);
        }
        dialog->done = true;
    };
    SDL_ShowOpenFileDialog(
        callback, &state, window, filters, std::size(filters), nullptr, false);

    for (;;) {
        {
            std::lock_guard lock(state.mutex);
            if (state.done) {
                return state.path;
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                return std::nullopt;
            }
        }
        SDL_Delay(16);
    }
}

std::optional<std::filesystem::path> discover_game_data_path(
    const std::filesystem::path& default_path, bool explicit_path)
{
    if (valid_game_data_directory(default_path)) {
        if (explicit_path) {
            save_remembered_data_path(default_path);
        }
        return default_path;
    }
    if (!explicit_path) {
        if (const auto remembered = load_remembered_data_path();
            remembered && valid_game_data_directory(*remembered)) {
            return *remembered;
        }
        const auto executable = pick_game_executable();
        if (!executable) {
            return std::nullopt;
        }
        const auto directory = executable->parent_path();
        if (valid_game_data_directory(directory)) {
            save_remembered_data_path(directory);
            return directory;
        }
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Selected executable is not in a valid game data directory: %s",
            executable->string().c_str());
    }
    return std::nullopt;
}

// Convert window-coordinate mouse events to the fixed 800x600 logical
// coordinate system used by the core game, applying 4:3 letterboxing.
std::pair<float, float> logical_coordinates(
    float x, float y, int window_width, int window_height)
{
    const float scale = std::min(
        window_width / 800.0f, window_height / 600.0f);
    const float logical_width = 800.0f * scale;
    const float logical_height = 600.0f * scale;
    const float offset_x = (window_width - logical_width) / 2.0f;
    const float offset_y = (window_height - logical_height) / 2.0f;
    return {(x - offset_x) / scale, (y - offset_y) / scale};
}

void convert_event_to_logical_coordinates(
    SDL_Event& event, int window_width, int window_height)
{
    const float scale = std::min(
        window_width / 800.0f, window_height / 600.0f);

    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION: {
        const auto [x, y] = logical_coordinates(
            event.motion.x, event.motion.y, window_width, window_height);
        event.motion.x = x;
        event.motion.y = y;
        event.motion.xrel /= scale;
        event.motion.yrel /= scale;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const auto [x, y] = logical_coordinates(
            event.button.x, event.button.y, window_width, window_height);
        event.button.x = x;
        event.button.y = y;
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        const auto [x, y] = logical_coordinates(
            event.wheel.mouse_x, event.wheel.mouse_y, window_width, window_height);
        event.wheel.mouse_x = x;
        event.wheel.mouse_y = y;
        break;
    }
    default:
        break;
    }
}

std::int32_t number(const th2::Event& event, std::size_t index)
{
    return std::get<std::int32_t>(event.arguments.at(index));
}

const std::string& text(const th2::Event& event, std::size_t index)
{
    return std::get<std::string>(event.arguments.at(index));
}

Texture load_texture(SDL_Renderer* renderer, const th2::Archive& archive,
                     std::string_view name)
{
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("image not found: " + std::string(name));
    }
    SDL_Surface* surface = th2::load_image(archive.read(*entry), entry->name);
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (!texture) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return Texture(texture);
}

Texture load_toned_texture(
    SDL_Renderer* renderer,
    const th2::Archive& image_archive,
    std::string_view image_name,
    const th2::Archive& curve_archive,
    const std::vector<ToneCurveSpec>& curves,
    Surface* pixels)
{
    const auto* image = image_archive.find(image_name);
    if (!image) {
        throw std::runtime_error(
            "image not found: " + std::string(image_name));
    }
    Surface surface(
        th2::load_image(image_archive.read(*image), image->name));
    for (const auto& curve : curves) {
        if (curve.name.empty()) {
            th2::apply_tone_curve(surface.get(), {}, curve.vividness);
            continue;
        }
        const auto* entry = curve_archive.find(curve.name);
        if (!entry) {
            throw std::runtime_error(
                "tone curve not found: " + curve.name);
        }
        th2::apply_tone_curve(
            surface.get(), curve_archive.read(*entry), curve.vividness);
    }
    SDL_Surface* texture_surface = surface.get();
    if (pixels) {
        pixels->reset(
            SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_RGBA32));
        if (!*pixels) {
            throw std::runtime_error(SDL_GetError());
        }
        texture_surface = pixels->get();
    }
    SDL_Texture* texture =
        SDL_CreateTextureFromSurface(renderer, texture_surface);
    if (!texture) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    return Texture(texture);
}

th2::AudioClip load_audio(const th2::Archive& archive, std::string_view name)
{
    const auto* entry = archive.find(name);
    if (!entry) {
        throw std::runtime_error("audio not found: " + std::string(name));
    }
    return th2::decode_audio(archive.read(*entry));
}

int scenario_number(std::string_view name)
{
    int result = 0;
    for (const auto byte : name) {
        if (byte >= '0' && byte <= '9') {
            result = result * 10 + byte - '0';
        } else if (result != 0) {
            break;
        }
    }
    return result;
}

std::vector<std::string> display_lines(
    std::string_view source, std::size_t wrap_columns)
{
    std::vector<std::string> lines;
    std::string line;
    bool just_wrapped = false;
    for (std::size_t position = 0; position < source.size();) {
        if (source[position] == '\n') {
            if (!line.empty() || !just_wrapped) {
                lines.push_back(line);
            }
            line.clear();
            just_wrapped = false;
            ++position;
            continue;
        }
        line.push_back(source[position++]);
        just_wrapped = false;
        // A leading separator after \k is visible immediately. The original
        // renderer does not wrap until the next printable glyph establishes
        // that the line is over width.
        if (line.size() >= wrap_columns && line.back() != ' ') {
            const auto space = line.find_last_of(' ');
            if (space != std::string::npos && space > wrap_columns / 2) {
                lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
            } else {
                lines.push_back(line);
                line.clear();
            }
            just_wrapped = line.empty();
        }
    }
    if (!line.empty() || lines.empty()) {
        lines.push_back(line);
    }
    return lines;
}

std::string interpret_newlines(std::string text)
{
    for (std::size_t position = 0;
         (position = text.find("\\n", position)) != std::string::npos;) {
        text.replace(position, 2, "\n");
        ++position;
    }
    return text;
}

bool clip_texture_source(
    SDL_Texture* texture, SDL_FRect& source, SDL_FRect& destination)
{
    float texture_width = 0.0f;
    float texture_height = 0.0f;
    if (!SDL_GetTextureSize(texture, &texture_width, &texture_height)
        || source.w <= 0.0f || source.h <= 0.0f) {
        return false;
    }
    const SDL_FRect original = source;
    const float left = std::clamp(source.x, 0.0f, texture_width);
    const float top = std::clamp(source.y, 0.0f, texture_height);
    const float right = std::clamp(source.x + source.w, 0.0f, texture_width);
    const float bottom = std::clamp(source.y + source.h, 0.0f, texture_height);
    if (right <= left || bottom <= top) {
        return false;
    }
    destination.x += (left - original.x) / original.w * destination.w;
    destination.y += (top - original.y) / original.h * destination.h;
    destination.w *= (right - left) / original.w;
    destination.h *= (bottom - top) / original.h;
    source = {left, top, right - left, bottom - top};
    return true;
}


}  // namespace th2app
