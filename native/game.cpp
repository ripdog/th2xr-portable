#include "archive.hpp"
#include "font.hpp"
#include "image.hpp"
#include "message.hpp"
#include "script_runtime.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};
using Texture = std::unique_ptr<SDL_Texture, TextureDeleter>;

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

std::vector<std::string> display_lines(std::string_view source)
{
    std::vector<std::string> lines;
    std::string line;
    for (std::size_t position = 0; position < source.size();) {
        if (source[position] == '\n') {
            lines.push_back(line);
            line.clear();
            ++position;
            continue;
        }
        line.push_back(source[position++]);
        if (line.size() >= 58) {
            const auto space = line.find_last_of(' ');
            if (space != std::string::npos && space > 35) {
                lines.push_back(line.substr(0, space));
                line.erase(0, space + 1);
            } else {
                lines.push_back(line);
                line.clear();
            }
        }
    }
    lines.push_back(line);
    return lines;
}

class Game {
public:
    explicit Game(const std::filesystem::path& data)
        : scripts_(data / "SDT.PAK"), graphics_(data / "GRP.PAK"),
          backgrounds_(data / "bak.pak"), fonts_(data / "FNT.PAK"),
          runtime_(scripts_), font_(fonts_)
    {
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            throw std::runtime_error(SDL_GetError());
        }
        if (!SDL_CreateWindowAndRenderer(
                "ToHeart2 XRATED", 800, 600, SDL_WINDOW_RESIZABLE,
                &window_, &renderer_)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_SetRenderLogicalPresentation(
            renderer_, 800, 600, SDL_LOGICAL_PRESENTATION_LETTERBOX);
        runtime_.load("EV_0301MORNING.SDT");
    }

    ~Game()
    {
        for (auto& overlay : overlays_) {
            overlay.reset();
        }
        background_.reset();
        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    int run()
    {
        advance();
        while (running_) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    running_ = false;
                } else if (event.type == SDL_EVENT_KEY_DOWN) {
                    if (event.key.key == SDLK_ESCAPE) {
                        running_ = false;
                    } else if (event.key.key == SDLK_F11) {
                        SDL_SetWindowFullscreen(
                            window_, !(SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN));
                    } else if (event.key.key == SDLK_RETURN
                               || event.key.key == SDLK_SPACE) {
                        advance();
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN
                           && event.button.button == SDL_BUTTON_LEFT) {
                    advance();
                }
            }
            if (wake_time_ && std::chrono::steady_clock::now() >= *wake_time_) {
                wake_time_.reset();
                advance();
            }
            draw();
            SDL_Delay(8);
        }
        return 0;
    }

private:
    th2::Archive scripts_;
    th2::Archive graphics_;
    th2::Archive backgrounds_;
    th2::Archive fonts_;
    th2::ScriptRuntime runtime_;
    th2::GameFont font_;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    Texture background_;
    std::array<Texture, 32> overlays_{};
    th2::Message message_;
    int tone_ = 0;
    int weather_ = 0;
    bool running_ = true;
    bool waiting_for_input_ = false;
    std::optional<std::chrono::steady_clock::time_point> wake_time_;

    void set_background(const th2::Event& event)
    {
        const int scene = number(event, 1) * 10
            + std::max<std::int32_t>(0, number(event, 2));
        const auto name = std::format(
            "B{:03d}{}{}{}.bmp", scene / 10, tone_ % 4, weather_, scene % 10);
        background_ = load_texture(renderer_, backgrounds_, name);
    }

    void handle(const th2::Event& event)
    {
        const auto name = event.instruction.name;
        if (name == "B" || name == "BT" || name == "BC" || name == "BCT") {
            if (number(event, 1) >= 0) {
                set_background(event);
            }
        } else if (name == "BD") {
            background_.reset();
        } else if (name == "SetTimeMode") {
            tone_ = std::max<std::int32_t>(0, number(event, 0));
        } else if (name == "SetWeatherMode") {
            weather_ = std::max<std::int32_t>(0, number(event, 0));
        } else if (name == "SetBmpEx") {
            const auto slot = static_cast<std::size_t>(number(event, 0));
            if (slot < overlays_.size()) {
                const auto& archive = text(event, 6) == "bak" ? backgrounds_ : graphics_;
                overlays_[slot] = load_texture(renderer_, archive, text(event, 2));
            }
        } else if (name == "ResetBmp") {
            const auto slot = static_cast<std::size_t>(number(event, 0));
            if (slot < overlays_.size()) {
                overlays_[slot].reset();
            }
        } else if (name == "ResetBmpAll") {
            for (auto& overlay : overlays_) {
                overlay.reset();
            }
        } else if (name == "SetMessage2") {
            message_.set(text(event, 0));
            waiting_for_input_ = true;
        } else if (name == "AddMessage2") {
            message_.append(text(event, 0));
            waiting_for_input_ = true;
        }
    }

    void advance()
    {
        if (wake_time_) {
            return;
        }
        if (waiting_for_input_ && message_.reveal_next()) {
            return;
        }
        waiting_for_input_ = false;
        while (running_ && !waiting_for_input_) {
            const auto step = runtime_.run();
            if (step.reason == th2::VmYield::ended) {
                running_ = false;
                break;
            }
            if (step.reason == th2::VmYield::wait_frames
                || step.reason == th2::VmYield::wait_time) {
                const auto bytes = step.event.instruction.name.empty()
                    ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>{};
                static_cast<void>(bytes);
                wake_time_ = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(16);
                break;
            }
            if (step.reason == th2::VmYield::frame) {
                break;
            }
            if (step.reason == th2::VmYield::event) {
                try {
                    handle(step.event);
                } catch (const std::exception& error) {
                    std::cerr << step.script_name << ": "
                              << step.event.instruction.name << ": "
                              << error.what() << '\n';
                }
            }
        }
    }

    void draw()
    {
        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        SDL_RenderClear(renderer_);
        if (background_) {
            SDL_RenderTexture(renderer_, background_.get(), nullptr, nullptr);
        }
        for (const auto& overlay : overlays_) {
            if (overlay) {
                SDL_RenderTexture(renderer_, overlay.get(), nullptr, nullptr);
            }
        }
        if (!message_.empty()) {
            SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer_, 0, 0, 16, 150);
            SDL_RenderFillRect(renderer_, nullptr);
            const auto lines = display_lines(message_.visible());
            float y = 72.0f;
            for (const auto& line : lines) {
                font_.draw(renderer_, 54.0f, y + 2.0f, line, 0, 0, 0);
                font_.draw(renderer_, 52.0f, y, line);
                y += 31.0f;
                if (y > 535.0f) {
                    break;
                }
            }
        }
        SDL_RenderPresent(renderer_);
    }
};

}  // namespace

int main(int argc, char** argv)
{
    try {
        return Game(argc > 1 ? argv[1] : "game-data").run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
