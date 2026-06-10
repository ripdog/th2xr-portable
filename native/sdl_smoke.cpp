#include <SDL3/SDL.h>
#include "archive.hpp"
#include "image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <vector>

int main(int argc, char** argv)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << "SDL_Init: " << SDL_GetError() << '\n';
        return 1;
    }

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer(
            "ToHeart2 XRATED native port", 800, 600,
            SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        std::cerr << "SDL_CreateWindowAndRenderer: " << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }

    SDL_SetRenderLogicalPresentation(
        renderer, 800, 600, SDL_LOGICAL_PRESENTATION_LETTERBOX);

    const std::filesystem::path data = argc > 1 ? argv[1] : "game-data";
    const th2::Archive graphics(data / "GRP.PAK");
    const std::array<const char*, 3> names{"t0000.tga", "t0001.tga", "t0010.tga"};
    std::vector<SDL_Texture*> textures;
    for (const char* name : names) {
        const auto* entry = graphics.find(name);
        if (!entry) {
            std::cerr << "missing title asset: " << name << '\n';
            return 1;
        }
        const auto bytes = graphics.read(*entry);
        SDL_Surface* surface = th2::load_tga(bytes);
        textures.push_back(SDL_CreateTextureFromSurface(renderer, surface));
        SDL_DestroySurface(surface);
    }

    bool running = true;
    std::size_t selected = 0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (event.type == SDL_EVENT_KEY_DOWN) {
                if (event.key.key == SDLK_ESCAPE) {
                    running = false;
                } else if (!textures.empty()) {
                    selected = (selected + 1) % textures.size();
                }
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && !textures.empty()) {
                selected = (selected + 1) % textures.size();
            }
        }

        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (!textures.empty()) {
            SDL_RenderTexture(renderer, textures[selected], nullptr, nullptr);
        }
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    for (auto* texture : textures) {
        SDL_DestroyTexture(texture);
    }
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
