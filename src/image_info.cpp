#include "archive.hpp"
#include "image.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::cerr << "usage: th2-image-info ARCHIVE IMAGE\n";
        return 2;
    }
    try {
        const th2::Archive archive(argv[1]);
        const auto* entry = archive.find(argv[2]);
        if (!entry) {
            throw std::runtime_error("image not found");
        }
        SDL_Surface* surface = th2::load_image(archive.read(*entry), entry->name);
        std::cout << entry->name << ": " << surface->w << 'x' << surface->h
                  << ", " << SDL_GetPixelFormatName(surface->format) << '\n';
        SDL_DestroySurface(surface);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
