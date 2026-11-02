// The Linux audio backend of GameAudio: one SDL3 audio stream per voice on the default playback device. PCM voices (the
// Sound.dat effects) are queued whole; MP3 voices (the radio stations) are decoded by libmpg123 a block at a time from the
// stream's own callback, on SDL's audio thread, so a song never decodes on the frame loop's thread.
#include <cstdlib>
#include <SDL3/SDL.h>
#include <mpg123.h>

#include <atomic>
#include <cstdio>
#include <vector>

#include "app/GameAudio.h"
#include "audio/platform/AudioPlatform.h"

namespace {

SDL_AudioDeviceID gDevice = 0;

// A stream bound to the device. Pan is not modelled (SDL streams have no per-stream pan); the voices are mixed centred.
class SdlVoice : public AudioPlatform::IVoice {
public:
    ~SdlVoice() override {
        if (stream_) SDL_DestroyAudioStream(stream_);   // unbinds; the callback never runs again after this returns
    }
    bool ok() const { return stream_ != nullptr; }
    void Stop() override {
        stopped_ = true;
        if (stream_) SDL_ClearAudioStream(stream_);
    }
    void SetVolume(float v) override { if (stream_) SDL_SetAudioStreamGain(stream_, v); }
    void SetPan(float) override {}

protected:
    bool Open(const SDL_AudioSpec& spec, SDL_AudioStreamCallback refill) {
        stream_ = SDL_CreateAudioStream(&spec, nullptr);
        if (!stream_) return false;
        if (refill) SDL_SetAudioStreamGetCallback(stream_, refill, this);
        if (!SDL_BindAudioStream(gDevice, stream_)) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
            return false;
        }
        return true;
    }
    SDL_AudioStream* stream_ = nullptr;
    std::atomic<bool> stopped_{false};
};

// The whole sample in memory; `loops` follows AVAudioPlayer (0 once, -1 forever, n = n more times).
class PcmVoice final : public SdlVoice {
public:
    PcmVoice(const std::uint8_t* data, std::int32_t size, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits)
        : pcm_(data, data + size) {
        SDL_AudioSpec spec{bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE, int(channels), int(rate)};
        Open(spec, &Refill);
    }
    void Play(std::int32_t loops) override {
        if (!stream_) return;
        SDL_LockAudioStream(stream_);
        SDL_ClearAudioStream(stream_);
        loopsLeft_ = loops;
        stopped_ = false;
        SDL_PutAudioStreamData(stream_, pcm_.data(), int(pcm_.size()));
        SDL_UnlockAudioStream(stream_);
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: voice %zu bytes gain %.2f\n", pcm_.size(), double(SDL_GetAudioStreamGain(stream_)));
    }
    bool IsPlaying() const override { return stream_ && !stopped_ && (SDL_GetAudioStreamQueued(stream_) > 0 || loopsLeft_ != 0); }

private:
    // Queues the sample again when it runs dry and loops remain.
    static void SDLCALL Refill(void* self, SDL_AudioStream* stream, int additional, int) {
        auto* v = static_cast<PcmVoice*>(self);
        if (v->stopped_ || additional <= 0 || v->loopsLeft_ == 0 || SDL_GetAudioStreamQueued(stream) >= additional) return;
        if (v->loopsLeft_ > 0) --v->loopsLeft_;
        SDL_PutAudioStreamData(stream, v->pcm_.data(), int(v->pcm_.size()));
    }
    std::vector<std::uint8_t> pcm_;
    std::atomic<std::int32_t> loopsLeft_{0};
};

// An MP3 file, decoded on demand into signed 16-bit samples at the file's own rate and channel count.
class Mp3Voice final : public SdlVoice {
public:
    explicit Mp3Voice(const char* path) {
        int error = 0;
        decoder_ = mpg123_new(nullptr, &error);
        if (!decoder_ || mpg123_open(decoder_, path) != MPG123_OK) return;
        long rate = 0;
        int channels = 0, encoding = 0;
        if (mpg123_getformat(decoder_, &rate, &channels, &encoding) != MPG123_OK) return;
        mpg123_format_none(decoder_);
        mpg123_format(decoder_, rate, channels, MPG123_ENC_SIGNED_16);
        SDL_AudioSpec spec{SDL_AUDIO_S16, channels, int(rate)};
        Open(spec, &Decode);
    }
    ~Mp3Voice() override {
        if (stream_) { SDL_DestroyAudioStream(stream_); stream_ = nullptr; }   // first: the callback uses the decoder
        if (decoder_) mpg123_delete(decoder_);
    }
    void Play(std::int32_t loops) override {
        if (!stream_) return;
        SDL_LockAudioStream(stream_);
        loops_ = loops;
        ended_ = false;
        stopped_ = false;
        SDL_UnlockAudioStream(stream_);
        SDL_ResumeAudioStreamDevice(stream_);
    }
    bool IsPlaying() const override { return stream_ && !stopped_ && (!ended_ || SDL_GetAudioStreamQueued(stream_) > 0); }

private:
    static void SDLCALL Decode(void* self, SDL_AudioStream* stream, int additional, int) {
        auto* v = static_cast<Mp3Voice*>(self);
        std::uint8_t block[16384];
        while (!v->stopped_ && !v->ended_ && additional > 0) {
            std::size_t got = 0;
            const int rc = mpg123_read(v->decoder_, block, sizeof block, &got);
            if (got > 0) {
                SDL_PutAudioStreamData(stream, block, int(got));
                additional -= int(got);
            }
            if (rc == MPG123_DONE || (rc != MPG123_OK && rc != MPG123_NEW_FORMAT)) {
                if (rc == MPG123_DONE && v->loops_ != 0 && mpg123_seek(v->decoder_, 0, SEEK_SET) >= 0) {
                    if (v->loops_ > 0) --v->loops_;
                    continue;
                }
                v->ended_ = true;
            }
        }
    }
    mpg123_handle* decoder_ = nullptr;
    std::atomic<std::int32_t> loops_{0};
    std::atomic<bool> ended_{true};
};

class SdlBackend final : public AudioPlatform::IBackend {
public:
    AudioPlatform::IVoice* CreatePcmVoice(const std::uint8_t* data, std::int32_t size, std::uint16_t channels,
                                          std::uint32_t rate, std::uint16_t bits) override {
        if (!data || size <= 0 || (bits != 8 && bits != 16)) return nullptr;
        auto* voice = new PcmVoice(data, size, channels, rate, bits);
        if (!voice->ok()) { delete voice; return nullptr; }
        return voice;
    }
    AudioPlatform::IVoice* CreateFileVoice(const char* path) override {
        auto* voice = new Mp3Voice(path);
        if (!voice->ok()) { delete voice; return nullptr; }
        return voice;
    }
    std::uint32_t AddTimer(std::uint32_t, AudioPlatform::TimerCallback, void*) override { return 0; }
    void RemoveTimer(std::uint32_t) override {}
};

}  // namespace

AudioPlatform::IBackend* CreatePlatformAudioBackend() {
    if (!gDevice) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            std::fprintf(stderr, "audio: %s\n", SDL_GetError());
            return nullptr;
        }
        gDevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!gDevice) {
            std::fprintf(stderr, "audio: no playback device (%s)\n", SDL_GetError());
            return nullptr;
        }
        std::printf("audio: %s\n", SDL_GetCurrentAudioDriver());
    }
    return new SdlBackend();
}
