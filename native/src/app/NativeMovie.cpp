// The game movies through the system FFmpeg libraries (its "ea" demuxer reads the MADk/MADm/SCHl/SCDl chunk stream, the eamad
// and adpcm_ea_r1 decoders do the rest). Built only when the build script found libavformat & co. (OCB_HAVE_FFMPEG); without
// them every movie fails to open and the callers skip it.
#include "app/NativeMovie.h"

#include <algorithm>
#include <cstring>

#ifdef OCB_HAVE_FFMPEG
#include <SDL3/SDL.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
#endif

namespace native {

#ifdef OCB_HAVE_FFMPEG

struct Movie::Impl {
    std::vector<std::uint8_t> bytes;
    std::size_t position = 0;
    AVIOContext* io = nullptr;
    AVFormatContext* format = nullptr;
    AVCodecContext* video = nullptr;
    AVCodecContext* audio = nullptr;
    int videoStream = -1, audioStream = -1;
    SwsContext* scaler = nullptr;
    SwrContext* resampler = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* pending = nullptr;     // the next picture, not shown yet
    double pendingTime = 0.0;
    bool pendingValid = false, endOfFile = false, videoDone = false;
    std::vector<std::uint8_t> pixels;
    std::vector<std::int16_t> sound;
    int width = 0, height = 0;

    ~Impl() {
        av_frame_free(&pending);
        av_frame_free(&frame);
        av_packet_free(&packet);
        swr_free(&resampler);
        sws_freeContext(scaler);
        avcodec_free_context(&audio);
        avcodec_free_context(&video);
        if (format) avformat_close_input(&format);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
    }

    static int Read(void* opaque, std::uint8_t* out, int size) {
        auto* self = static_cast<Impl*>(opaque);
        const std::size_t left = self->bytes.size() - self->position;
        if (left == 0) return AVERROR_EOF;
        const std::size_t n = std::min<std::size_t>(left, static_cast<std::size_t>(size));
        std::memcpy(out, self->bytes.data() + self->position, n);
        self->position += n;
        return static_cast<int>(n);
    }
    static std::int64_t Seek(void* opaque, std::int64_t offset, int whence) {
        auto* self = static_cast<Impl*>(opaque);
        const auto size = static_cast<std::int64_t>(self->bytes.size());
        if (whence == AVSEEK_SIZE) return size;
        std::int64_t target = offset;
        if ((whence & 3) == SEEK_CUR) target += static_cast<std::int64_t>(self->position);
        else if ((whence & 3) == SEEK_END) target += size;
        if (target < 0 || target > size) return -1;
        self->position = static_cast<std::size_t>(target);
        return target;
    }

    static AVCodecContext* OpenDecoder(AVStream* stream) {
        const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (!codec) return nullptr;
        AVCodecContext* context = avcodec_alloc_context3(codec);
        if (!context || avcodec_parameters_to_context(context, stream->codecpar) < 0 || avcodec_open2(context, codec, nullptr) < 0) {
            avcodec_free_context(&context);
            return nullptr;
        }
        return context;
    }

    bool Open() {
        constexpr int kBufferSize = 64 * 1024;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(kBufferSize));
        io = buffer ? avio_alloc_context(buffer, kBufferSize, 0, this, &Read, nullptr, &Seek) : nullptr;
        if (!io) { av_free(buffer); return false; }
        format = avformat_alloc_context();
        if (!format) return false;
        format->pb = io;
        const AVInputFormat* ea = av_find_input_format("ea");
        if (avformat_open_input(&format, nullptr, ea, nullptr) < 0) { format = nullptr; return false; }
        if (avformat_find_stream_info(format, nullptr) < 0) return false;
        videoStream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoStream < 0 || !(video = OpenDecoder(format->streams[videoStream]))) return false;
        audioStream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (audioStream >= 0 && (audio = OpenDecoder(format->streams[audioStream]))) {
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            if (swr_alloc_set_opts2(&resampler, &stereo, AV_SAMPLE_FMT_S16, 22050, &audio->ch_layout, audio->sample_fmt,
                                    audio->sample_rate, 0, nullptr) < 0 || swr_init(resampler) < 0)
                swr_free(&resampler);
        }
        width = video->width;
        height = video->height;
        if (width <= 0 || height <= 0) return false;
        pixels.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
        for (std::size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xff;
        packet = av_packet_alloc();
        frame = av_frame_alloc();
        pending = av_frame_alloc();
        return packet && frame && pending;
    }

    void TakeSound(AVFrame* decoded) {
        if (!resampler) return;
        const int room = swr_get_out_samples(resampler, decoded->nb_samples);
        if (room <= 0) return;
        const std::size_t at = sound.size();
        sound.resize(at + static_cast<std::size_t>(room) * 2);
        std::uint8_t* out = reinterpret_cast<std::uint8_t*>(sound.data() + at);
        const int got = swr_convert(resampler, &out, room, const_cast<const std::uint8_t**>(decoded->extended_data), decoded->nb_samples);
        sound.resize(at + static_cast<std::size_t>(got > 0 ? got : 0) * 2);
    }

    // Reads on until a picture is pending (or the stream ends).
    void Pull() {
        while (!pendingValid && !videoDone) {
            const int received = avcodec_receive_frame(video, pending);
            if (received == 0) {
                const AVRational base = format->streams[videoStream]->time_base;
                const std::int64_t stamp = pending->best_effort_timestamp != AV_NOPTS_VALUE ? pending->best_effort_timestamp : pending->pts;
                pendingTime = stamp == AV_NOPTS_VALUE ? pendingTime : static_cast<double>(stamp) * av_q2d(base);
                pendingValid = true;
                return;
            }
            if (received == AVERROR_EOF) { videoDone = true; return; }
            if (endOfFile) {
                avcodec_send_packet(video, nullptr);   // drain
                if (avcodec_receive_frame(video, pending) == 0) { pendingValid = true; return; }
                videoDone = true;
                return;
            }
            if (av_read_frame(format, packet) < 0) {
                endOfFile = true;
                avcodec_send_packet(video, nullptr);
                continue;
            }
            if (packet->stream_index == videoStream) {
                avcodec_send_packet(video, packet);
            } else if (audio && packet->stream_index == audioStream && avcodec_send_packet(audio, packet) == 0) {
                while (avcodec_receive_frame(audio, frame) == 0) {
                    TakeSound(frame);
                    av_frame_unref(frame);
                }
            }
            av_packet_unref(packet);
        }
    }

    void Show(AVFrame* picture) {
        scaler = sws_getCachedContext(scaler, picture->width, picture->height, static_cast<AVPixelFormat>(picture->format), width,
                                      height, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) return;
        std::uint8_t* planes[4] = {pixels.data(), nullptr, nullptr, nullptr};
        int strides[4] = {width * 4, 0, 0, 0};
        sws_scale(scaler, picture->data, picture->linesize, 0, picture->height, planes, strides);
    }
};

bool Movie::Available() { return true; }
bool Movie::Open(std::vector<std::uint8_t> bytes) {
    Close();
    impl_ = std::make_unique<Impl>();
    impl_->bytes = std::move(bytes);
    if (impl_->Open()) {
        impl_->Pull();
        return true;
    }
    impl_.reset();
    return false;
}
bool Movie::AdvanceTo(double seconds) {
    if (!impl_) return false;
    bool changed = false;
    while (impl_->pendingValid && impl_->pendingTime <= seconds) {
        impl_->Show(impl_->pending);
        av_frame_unref(impl_->pending);
        impl_->pendingValid = false;
        changed = true;
        impl_->Pull();
    }
    return changed;
}
bool Movie::Finished() const { return !impl_ || (!impl_->pendingValid && impl_->videoDone); }
int Movie::Width() const { return impl_ ? impl_->width : 0; }
int Movie::Height() const { return impl_ ? impl_->height : 0; }
const std::uint8_t* Movie::Pixels() const { return impl_ ? impl_->pixels.data() : nullptr; }
void Movie::TakeAudio(std::vector<std::int16_t>& out) {
    out.clear();
    if (impl_) out.swap(impl_->sound);
}

#else   // no decoder in this build

struct Movie::Impl {};
bool Movie::Available() { return false; }
bool Movie::Open(std::vector<std::uint8_t>) { return false; }
bool Movie::AdvanceTo(double) { return false; }
bool Movie::Finished() const { return true; }
int Movie::Width() const { return 0; }
int Movie::Height() const { return 0; }
const std::uint8_t* Movie::Pixels() const { return nullptr; }
void Movie::TakeAudio(std::vector<std::int16_t>& out) { out.clear(); }

#endif

MovieSound::~MovieSound() { Close(); }
#ifdef OCB_HAVE_FFMPEG
bool MovieSound::Open(float gain) {
    Close();
    if (!SDL_WasInit(SDL_INIT_AUDIO)) return false;   // the game's audio brought the subsystem up (GameAudioSDL)
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, 22050};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) return false;
    SDL_SetAudioStreamGain(stream, gain);
    SDL_ResumeAudioStreamDevice(stream);
    stream_ = stream;
    return true;
}
void MovieSound::Push(const std::vector<std::int16_t>& samples) {
    if (stream_ && !samples.empty())
        SDL_PutAudioStreamData(static_cast<SDL_AudioStream*>(stream_), samples.data(), static_cast<int>(samples.size() * sizeof(std::int16_t)));
}
void MovieSound::Close() {
    if (stream_) SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream_));
    stream_ = nullptr;
}

#else
bool MovieSound::Open(float) { return false; }
void MovieSound::Push(const std::vector<std::int16_t>&) {}
void MovieSound::Close() {}
#endif

Movie::Movie() = default;
Movie::~Movie() = default;
void Movie::Close() { impl_.reset(); }
bool Movie::IsOpen() const { return impl_ != nullptr; }

}  // namespace native
