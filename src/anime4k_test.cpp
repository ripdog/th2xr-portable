#include "anime4k.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: th2-anime4k-test SHADER_DIR\n";
        return 2;
    }
    try {
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Window* window = SDL_CreateWindow(
            "Anime4K test", 1600, 1200, SDL_WINDOW_HIDDEN);
        SDL_PropertiesID properties = SDL_CreateProperties();
        SDL_SetStringProperty(
            properties, SDL_PROP_RENDERER_CREATE_NAME_STRING,
            SDL_GPU_RENDERER);
        SDL_SetPointerProperty(
            properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
        SDL_SetBooleanProperty(
            properties, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN,
            true);
        SDL_Renderer* renderer =
            SDL_CreateRendererWithProperties(properties);
        SDL_DestroyProperties(properties);
        if (!window || !renderer) {
            throw std::runtime_error(SDL_GetError());
        }
        th2::Anime4K anime4k(renderer, argv[1]);
        if (!anime4k.available()) {
            throw std::runtime_error("Anime4K unavailable");
        }
        for (int frame = 0; frame < 10; ++frame) {
            SDL_SetRenderTarget(renderer, anime4k.art_target());
            SDL_SetRenderDrawColor(renderer, 20, 40, 80, 255);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawColor(renderer, 255, 180, 80, 255);
            const SDL_FRect rectangle{
                80.0f + frame * 3.0f, 100.0f, 320.0f, 240.0f};
            SDL_RenderFillRect(renderer, &rectangle);
            SDL_SetRenderTarget(renderer, anime4k.overlay_target());
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            anime4k.present();
            SDL_RenderPresent(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
