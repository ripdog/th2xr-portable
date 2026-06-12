#include "audio.hpp"

#include <sndfile.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace th2 {
namespace {

struct MemoryInput {
    std::span<const std::uint8_t> bytes;
    sf_count_t position = 0;
};

sf_count_t file_length(void* user_data)
{
    return static_cast<sf_count_t>(
        static_cast<MemoryInput*>(user_data)->bytes.size());
}

sf_count_t seek(sf_count_t offset, int whence, void* user_data)
{
    auto& input = *static_cast<MemoryInput*>(user_data);
    sf_count_t base = 0;
    if (whence == SEEK_CUR) {
        base = input.position;
    } else if (whence == SEEK_END) {
        base = static_cast<sf_count_t>(input.bytes.size());
    } else if (whence != SEEK_SET) {
        return -1;
    }
    const auto next = base + offset;
    if (next < 0 || next > static_cast<sf_count_t>(input.bytes.size())) {
        return -1;
    }
    input.position = next;
    return next;
}

sf_count_t read(void* destination, sf_count_t count, void* user_data)
{
    auto& input = *static_cast<MemoryInput*>(user_data);
    const auto available = static_cast<sf_count_t>(input.bytes.size()) - input.position;
    const auto copied = std::min(count, available);
    std::memcpy(
        destination, input.bytes.data() + input.position,
        static_cast<std::size_t>(copied));
    input.position += copied;
    return copied;
}

sf_count_t write(const void*, sf_count_t, void*)
{
    return 0;
}

sf_count_t tell(void* user_data)
{
    return static_cast<MemoryInput*>(user_data)->position;
}

SF_VIRTUAL_IO virtual_io{
    file_length,
    seek,
    read,
    write,
    tell,
};

}  // namespace

AudioClip decode_audio(std::span<const std::uint8_t> bytes)
{
    MemoryInput input{bytes};
    SF_INFO info{};
    SNDFILE* file = sf_open_virtual(&virtual_io, SFM_READ, &info, &input);
    if (!file) {
        throw std::runtime_error(sf_strerror(nullptr));
    }
    if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
        sf_close(file);
        throw std::runtime_error("invalid audio stream");
    }

    AudioClip clip{
        info.samplerate,
        info.channels,
        std::vector<float>(
            static_cast<std::size_t>(info.frames) * info.channels),
    };
    const auto frames = sf_readf_float(file, clip.samples.data(), info.frames);
    sf_close(file);
    if (frames != info.frames) {
        throw std::runtime_error("truncated decoded audio");
    }
    return clip;
}

AudioChannel::~AudioChannel()
{
    stop();
}

AudioChannel::AudioChannel(AudioChannel&& other) noexcept
    : stream_(std::exchange(other.stream_, nullptr)),
      clip_(std::move(other.clip_)), loop_clip_(std::move(other.loop_clip_)),
      loop_(other.loop_), active_(other.active_),
      gain_(other.gain_), fade_from_(other.fade_from_),
      fade_to_(other.fade_to_), fade_stop_(other.fade_stop_),
      fade_started_(std::move(other.fade_started_)),
      fade_duration_(other.fade_duration_),
      playback_end_(other.playback_end_),
      paused_at_(std::move(other.paused_at_))
{
    other.active_ = false;
    other.paused_at_.reset();
}

AudioChannel& AudioChannel::operator=(AudioChannel&& other) noexcept
{
    if (this != &other) {
        stop();
        stream_ = std::exchange(other.stream_, nullptr);
        clip_ = std::move(other.clip_);
        loop_clip_ = std::move(other.loop_clip_);
        loop_ = other.loop_;
        active_ = other.active_;
        gain_ = other.gain_;
        fade_from_ = other.fade_from_;
        fade_to_ = other.fade_to_;
        fade_stop_ = other.fade_stop_;
        fade_started_ = std::move(other.fade_started_);
        fade_duration_ = other.fade_duration_;
        playback_end_ = other.playback_end_;
        paused_at_ = std::move(other.paused_at_);
        other.active_ = false;
        other.paused_at_.reset();
    }
    return *this;
}

void AudioChannel::play(AudioClip clip, bool loop, float gain)
{
    stop();
    clip_ = std::move(clip);
    loop_ = loop;
    const SDL_AudioSpec spec{
        SDL_AUDIO_F32,
        clip_.channels,
        clip_.sample_rate,
    };
    stream_ = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        throw std::runtime_error(SDL_GetError());
    }
    if (!SDL_SetAudioStreamGain(stream_, std::clamp(gain, 0.0f, 1.0f))) {
        throw std::runtime_error(SDL_GetError());
    }
    queue();
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
        throw std::runtime_error(SDL_GetError());
    }
    active_ = true;
    gain_ = std::clamp(gain, 0.0f, 1.0f);
    fade_started_.reset();
    fade_stop_ = false;
    paused_at_.reset();
    set_playback_end();
}

void AudioChannel::play_intro_loop(AudioClip intro, AudioClip loop, float gain)
{
    if (intro.sample_rate != loop.sample_rate || intro.channels != loop.channels) {
        throw std::runtime_error("BGM intro and loop formats do not match");
    }
    play(std::move(intro), false, gain);
    loop_clip_ = std::move(loop);
}

void AudioChannel::queue()
{
    if (!stream_ || clip_.samples.empty()) {
        return;
    }
    const auto bytes = clip_.samples.size() * sizeof(float);
    if (!SDL_PutAudioStreamData(stream_, clip_.samples.data(), static_cast<int>(bytes))) {
        throw std::runtime_error(SDL_GetError());
    }
    SDL_FlushAudioStream(stream_);
}

void AudioChannel::stop()
{
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    clip_ = {};
    loop_clip_ = {};
    loop_ = false;
    active_ = false;
    fade_started_.reset();
    fade_stop_ = false;
    paused_at_.reset();
}

void AudioChannel::pause(bool paused)
{
    if (!stream_) {
        return;
    }
    if (paused) {
        SDL_PauseAudioStreamDevice(stream_);
        if (!paused_at_) {
            paused_at_ = std::chrono::steady_clock::now();
        }
    } else {
        SDL_ResumeAudioStreamDevice(stream_);
        if (paused_at_) {
            playback_end_ += std::chrono::steady_clock::now() - *paused_at_;
            paused_at_.reset();
        }
    }
}

void AudioChannel::set_gain(float gain)
{
    gain_ = std::clamp(gain, 0.0f, 1.0f);
    fade_started_.reset();
    fade_stop_ = false;
    if (stream_) {
        SDL_SetAudioStreamGain(stream_, gain_);
    }
}

void AudioChannel::fade_to(
    float gain, std::chrono::milliseconds duration, bool stop_after)
{
    if (!stream_ || duration <= std::chrono::milliseconds::zero()) {
        if (stop_after) {
            stop();
        } else {
            set_gain(gain);
        }
        return;
    }
    fade_from_ = gain_;
    fade_to_ = std::clamp(gain, 0.0f, 1.0f);
    fade_stop_ = stop_after;
    fade_duration_ = duration;
    fade_started_ = std::chrono::steady_clock::now();
}

void AudioChannel::update()
{
    if (stream_ && fade_started_) {
        const auto elapsed = std::chrono::steady_clock::now() - *fade_started_;
        const float progress = std::clamp(
            std::chrono::duration<float>(elapsed).count()
                / std::chrono::duration<float>(fade_duration_).count(),
            0.0f, 1.0f);
        gain_ = fade_from_ + (fade_to_ - fade_from_) * progress;
        SDL_SetAudioStreamGain(stream_, gain_);
        if (progress >= 1.0f) {
            const bool stop_after = fade_stop_;
            fade_started_.reset();
            fade_stop_ = false;
            if (stop_after) {
                stop();
                return;
            }
        }
    }
    if (!stream_ || !active_ || paused_at_
        || std::chrono::steady_clock::now() < playback_end_) {
        return;
    }
    if (!loop_clip_.samples.empty()) {
        clip_ = std::move(loop_clip_);
        loop_ = true;
        queue();
        set_playback_end();
    } else if (loop_) {
        queue();
        set_playback_end();
    } else {
        active_ = false;
    }
}

bool AudioChannel::playing() const
{
    return stream_ && active_;
}

void AudioChannel::set_playback_end()
{
    const auto frames = clip_.channels > 0
        ? clip_.samples.size() / static_cast<std::size_t>(clip_.channels)
        : 0;
    const auto duration = clip_.sample_rate > 0
        ? std::chrono::duration<double>(
              static_cast<double>(frames) / clip_.sample_rate)
        : std::chrono::duration<double>::zero();
    playback_end_ = std::chrono::steady_clock::now()
        + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            duration);
}

}  // namespace th2
