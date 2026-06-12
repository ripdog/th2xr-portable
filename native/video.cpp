#include "video.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

namespace th2 {
namespace {

std::runtime_error ffmpeg_error(std::string_view action, int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buffer, sizeof(buffer));
    return std::runtime_error(std::string(action) + ": " + buffer);
}

struct TextureDeleter {
    void operator()(SDL_Texture* texture) const { SDL_DestroyTexture(texture); }
};

}  // namespace

struct VideoPlayer::Impl {
    struct VideoFrame {
        double time;
        std::vector<std::uint8_t> pixels;
    };

    SDL_Renderer* renderer;
    SDL_FRect destination;
    std::span<const std::uint8_t> bytes;
    std::size_t position = 0;
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext* video_codec = nullptr;
    AVCodecContext* audio_codec = nullptr;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    std::unique_ptr<SDL_Texture, TextureDeleter> texture;
    SDL_AudioStream* audio = nullptr;
    int video_stream = -1;
    int audio_stream = -1;
    bool eof = false;
    bool done = false;
    double shown_time = -1.0;
    double decoded_time = -1.0;
    double frame_duration = 1.0 / 30.0;
    std::size_t decoded_frames = 0;
    std::deque<VideoFrame> video_frames;
    std::chrono::steady_clock::time_point started;

    static int read(void* opaque, std::uint8_t* destination, int size)
    {
        auto& self = *static_cast<Impl*>(opaque);
        const auto available = self.bytes.size() - self.position;
        const auto copied = std::min<std::size_t>(available, size);
        if (copied == 0) {
            return AVERROR_EOF;
        }
        std::copy_n(self.bytes.data() + self.position, copied, destination);
        self.position += copied;
        return static_cast<int>(copied);
    }

    static std::int64_t seek(void* opaque, std::int64_t offset, int whence)
    {
        auto& self = *static_cast<Impl*>(opaque);
        if (whence == AVSEEK_SIZE) {
            return static_cast<std::int64_t>(self.bytes.size());
        }
        const int origin = whence & ~AVSEEK_FORCE;
        std::int64_t base = origin == SEEK_CUR
            ? static_cast<std::int64_t>(self.position)
            : origin == SEEK_END ? static_cast<std::int64_t>(self.bytes.size())
                                 : 0;
        const auto next = base + offset;
        if (next < 0 || next > static_cast<std::int64_t>(self.bytes.size())) {
            return AVERROR(EINVAL);
        }
        self.position = static_cast<std::size_t>(next);
        return next;
    }

    AVCodecContext* open_codec(int stream)
    {
        const auto parameters = format->streams[stream]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(parameters->codec_id);
        if (!codec) {
            throw std::runtime_error("video codec not available");
        }
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (!context) {
            throw std::bad_alloc();
        }
        int result = avcodec_parameters_to_context(context, parameters);
        if (result >= 0) {
            result = avcodec_open2(context, codec, nullptr);
        }
        if (result < 0) {
            avcodec_free_context(&context);
            throw ffmpeg_error("open codec", result);
        }
        return context;
    }

    Impl(SDL_Renderer* renderer_, std::span<const std::uint8_t> bytes_,
         SDL_FRect destination_)
        : renderer(renderer_), destination(destination_), bytes(bytes_)
    {
        if (!frame || !packet) {
            throw std::bad_alloc();
        }
        auto* io_buffer = static_cast<std::uint8_t*>(av_malloc(64 * 1024));
        io = avio_alloc_context(
            io_buffer, 64 * 1024, 0, this, read, nullptr, seek);
        format = avformat_alloc_context();
        if (!io || !format) {
            throw std::bad_alloc();
        }
        format->pb = io;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        int result = avformat_open_input(&format, nullptr, nullptr, nullptr);
        if (result < 0) {
            throw ffmpeg_error("open movie", result);
        }
        result = avformat_find_stream_info(format, nullptr);
        if (result < 0) {
            throw ffmpeg_error("read movie streams", result);
        }
        video_stream = av_find_best_stream(
            format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        audio_stream = av_find_best_stream(
            format, AVMEDIA_TYPE_AUDIO, -1, video_stream, nullptr, 0);
        if (video_stream < 0) {
            throw std::runtime_error("movie has no video stream");
        }
        video_codec = open_codec(video_stream);
        const AVRational frame_rate =
            av_guess_frame_rate(format, format->streams[video_stream], nullptr);
        if (frame_rate.num > 0 && frame_rate.den > 0) {
            frame_duration = av_q2d(av_inv_q(frame_rate));
        }
        texture.reset(SDL_CreateTexture(
            renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
            video_codec->width, video_codec->height));
        if (!texture) {
            throw std::runtime_error(SDL_GetError());
        }
        scaler = sws_getContext(
            video_codec->width, video_codec->height, video_codec->pix_fmt,
            video_codec->width, video_codec->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) {
            throw std::runtime_error("cannot create video scaler");
        }
        if (audio_stream >= 0) {
            audio_codec = open_codec(audio_stream);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            result = swr_alloc_set_opts2(
                &resampler, &stereo, AV_SAMPLE_FMT_FLT, 48000,
                &audio_codec->ch_layout, audio_codec->sample_fmt,
                audio_codec->sample_rate, 0, nullptr);
            if (result < 0 || swr_init(resampler) < 0) {
                throw std::runtime_error("cannot create audio resampler");
            }
            const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, 48000};
            audio = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
            if (!audio || !SDL_ResumeAudioStreamDevice(audio)) {
                throw std::runtime_error(SDL_GetError());
            }
        }
        started = std::chrono::steady_clock::now();
    }

    ~Impl()
    {
        if (audio) SDL_DestroyAudioStream(audio);
        swr_free(&resampler);
        sws_freeContext(scaler);
        avcodec_free_context(&audio_codec);
        avcodec_free_context(&video_codec);
        av_packet_free(&packet);
        av_frame_free(&frame);
        avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    void queue_audio(AVFrame* decoded)
    {
        const int output_samples = av_rescale_rnd(
            swr_get_delay(resampler, audio_codec->sample_rate)
                + decoded->nb_samples,
            48000, audio_codec->sample_rate, AV_ROUND_UP);
        std::vector<float> samples(
            static_cast<std::size_t>(output_samples) * 2);
        std::uint8_t* output[] = {
            reinterpret_cast<std::uint8_t*>(samples.data())};
        const int converted = swr_convert(
            resampler, output, output_samples,
            const_cast<const std::uint8_t**>(decoded->extended_data),
            decoded->nb_samples);
        if (converted > 0) {
            SDL_PutAudioStreamData(
                audio, samples.data(), converted * 2 * sizeof(float));
        }
    }

    double frame_time(const AVFrame* decoded) const
    {
        if (decoded->best_effort_timestamp == AV_NOPTS_VALUE) {
            return decoded_frames * frame_duration;
        }
        const auto* stream = format->streams[video_stream];
        const std::int64_t start = stream->start_time == AV_NOPTS_VALUE
            ? 0 : stream->start_time;
        return (decoded->best_effort_timestamp - start)
            * av_q2d(stream->time_base);
    }

    void queue_video(AVFrame* decoded)
    {
        VideoFrame output_frame{
            frame_time(decoded),
            std::vector<std::uint8_t>(
                static_cast<std::size_t>(video_codec->width)
                * video_codec->height * 4)};
        std::uint8_t* output[] = {output_frame.pixels.data()};
        const int pitches[] = {video_codec->width * 4};
        sws_scale(
            scaler, decoded->data, decoded->linesize, 0, video_codec->height,
            output, pitches);
        decoded_time = output_frame.time;
        ++decoded_frames;
        video_frames.push_back(std::move(output_frame));
    }

    void present_due_frame(double elapsed)
    {
        while (!video_frames.empty()
               && video_frames.front().time <= elapsed + 0.001) {
            const auto& due = video_frames.front();
            if (!SDL_UpdateTexture(
                    texture.get(), nullptr, due.pixels.data(),
                    video_codec->width * 4)) {
                throw std::runtime_error(SDL_GetError());
            }
            shown_time = due.time;
            video_frames.pop_front();
        }
    }

    void decode_packet(AVCodecContext* codec, bool video)
    {
        int result = avcodec_send_packet(codec, packet);
        if (result < 0 && result != AVERROR(EAGAIN)) {
            throw ffmpeg_error("decode movie packet", result);
        }
        while ((result = avcodec_receive_frame(codec, frame)) >= 0) {
            if (video) queue_video(frame);
            else queue_audio(frame);
            av_frame_unref(frame);
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) {
            throw ffmpeg_error("decode movie frame", result);
        }
    }

    void update()
    {
        const double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        while (!eof && (decoded_time < 0.0
                        || decoded_time < elapsed + 0.10)) {
            const int result = av_read_frame(format, packet);
            if (result < 0) {
                eof = true;
                avcodec_send_packet(video_codec, nullptr);
                while (avcodec_receive_frame(video_codec, frame) >= 0) {
                    queue_video(frame);
                    av_frame_unref(frame);
                }
                break;
            }
            if (packet->stream_index == video_stream) {
                decode_packet(video_codec, true);
            } else if (packet->stream_index == audio_stream && audio) {
                decode_packet(audio_codec, false);
            }
            av_packet_unref(packet);
        }
        present_due_frame(elapsed);
        if (eof) {
            const double duration = format->duration > 0
                ? format->duration / static_cast<double>(AV_TIME_BASE)
                : shown_time;
            done = elapsed >= duration && video_frames.empty()
                && (!audio || SDL_GetAudioStreamQueued(audio) == 0);
        }
    }
};

VideoPlayer::VideoPlayer(
    SDL_Renderer* renderer, std::span<const std::uint8_t> bytes,
    SDL_FRect destination)
    : impl_(std::make_unique<Impl>(renderer, bytes, destination))
{
}

VideoPlayer::~VideoPlayer() = default;

void VideoPlayer::update()
{
    impl_->update();
}

void VideoPlayer::draw() const
{
    if (impl_->texture) {
        SDL_RenderTexture(
            impl_->renderer, impl_->texture.get(), nullptr, &impl_->destination);
    }
}

bool VideoPlayer::finished() const
{
    return impl_->done;
}

}  // namespace th2
