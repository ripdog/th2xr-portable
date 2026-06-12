#include "archive.hpp"
#include "video.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <set>
#include <thread>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: th2-video-test MOV.PAK MOVIE.AVI\n";
        return 2;
    }
    try {
        const th2::Archive archive(argv[1]);
        const auto* entry = archive.find(argv[2]);
        if (!entry) {
            throw std::runtime_error("movie not found");
        }
        const auto bytes = archive.read(*entry);
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
            throw std::runtime_error(SDL_GetError());
        }
        SDL_Window* window = SDL_CreateWindow("video test", 800, 600, 0);
        SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
        if (!window || !renderer) {
            throw std::runtime_error(SDL_GetError());
        }
        th2::VideoPlayer player(
            renderer, bytes, SDL_FRect{0.0f, 0.0f, 800.0f, 600.0f});
        std::set<std::uint64_t> hashes;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline
               && !player.finished()) {
            player.update();
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
            SDL_RenderClear(renderer);
            player.draw();
            SDL_Surface* frame = SDL_RenderReadPixels(renderer, nullptr);
            if (!frame) {
                throw std::runtime_error(SDL_GetError());
            }
            const auto* data = static_cast<const std::uint8_t*>(frame->pixels);
            std::uint64_t hash = 1469598103934665603ULL;
            for (int y = 0; y < frame->h; y += 8) {
                for (int x = 0; x < frame->w * 4; x += 32) {
                    hash = (hash ^ data[y * frame->pitch + x])
                        * 1099511628211ULL;
                }
            }
            hashes.insert(hash);
            SDL_DestroySurface(frame);
            SDL_RenderPresent(renderer);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::cout << "unique frame samples: " << hashes.size() << '\n';
        return hashes.size() >= 10 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
