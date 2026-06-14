#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

namespace th2 {

class VideoPlayer {
public:
    VideoPlayer(SDL_Renderer* renderer, std::span<const std::uint8_t> bytes,
                SDL_FRect destination);
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    void update();
    void draw() const;
    bool finished() const;
    void set_speed(double speed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace th2
