#pragma once

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace th2 {

struct AudioClip {
    int sample_rate;
    int channels;
    std::vector<float> samples;
};

AudioClip decode_audio(std::span<const std::uint8_t> bytes);

class AudioChannel {
public:
    AudioChannel() = default;
    ~AudioChannel();

    AudioChannel(const AudioChannel&) = delete;
    AudioChannel& operator=(const AudioChannel&) = delete;

    AudioChannel(AudioChannel&& other) noexcept;
    AudioChannel& operator=(AudioChannel&& other) noexcept;

    void play(AudioClip clip, bool loop, float gain);
    void play_intro_loop(AudioClip intro, AudioClip loop, float gain);
    void stop();
    void pause(bool paused);
    void set_gain(float gain);
    void update();
    bool playing() const;

private:
    SDL_AudioStream* stream_ = nullptr;
    AudioClip clip_{};
    AudioClip loop_clip_{};
    bool loop_ = false;
    bool active_ = false;
    std::chrono::steady_clock::time_point playback_end_{};
    std::optional<std::chrono::steady_clock::time_point> paused_at_;

    void queue();
    void set_playback_end();
};

}  // namespace th2
