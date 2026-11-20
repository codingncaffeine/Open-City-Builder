// The Linux audio backend of GameAudio: one SDL3 audio stream per voice on the default playback device. PCM voices (the
// Sound.dat effects) are queued whole; MP3 voices (the radio stations) are decoded by libmpg123 a block at a time from the
// stream's own callback, on SDL's audio thread, so a song never decodes on the frame loop's thread.
#include <cstdlib>
#include <SDL3/SDL.h>
#include <mpg123.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>

#include "app/GameAudio.h"
#include "audio/platform/AudioPlatform.h"

namespace linuxapp {   // StubClock.cpp
std::uint32_t AddPumpedTimer(std::uint32_t intervalMs, std::uint32_t (*callback)(std::uint32_t, void*), void* context);
void RemovePumpedTimer(std::uint32_t id);
}  // namespace linuxapp

namespace {

SDL_AudioDeviceID gDevice = 0;
class SdlVoice;
std::vector<SdlVoice*> gVoices;   // every live voice (the kit's `audio` report); made and freed on the frame loop's thread

// A stream bound to the device.
class SdlVoice : public AudioPlatform::IVoice {
public:
    SdlVoice() { gVoices.push_back(this); }
    ~SdlVoice() override {
        std::erase(gVoices, this);
        if (stream_) SDL_DestroyAudioStream(stream_);   // unbinds; the callback never runs again after this returns
    }
    void Report() const {
        std::printf("audio:   voice queued %d bytes stopped %d playing %d gain %.2f pan %.2f\n", stream_ ? SDL_GetAudioStreamQueued(stream_) : -1,
                    int(stopped_.load()), int(IsPlaying()), stream_ ? double(SDL_GetAudioStreamGain(stream_)) : 0.0, double(Pan()));
    }
    virtual float Pan() const { return 0.0f; }
    bool ok() const { return stream_ != nullptr; }
    void Stop() override {
        stopped_ = true;
        if (stream_) SDL_ClearAudioStream(stream_);
    }
    void SetVolume(float v) override { if (stream_) SDL_SetAudioStreamGain(stream_, v); }
    void SetPan(float) override {}   // the songs are not placed

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

// The whole sample in memory; `loops` follows AVAudioPlayer (0 once, -1 forever, n = n more times). The stream is fed a block
// at a time from its own callback, as stereo 16-bit frames with the voice's pan applied to each block: a sound moving across
// the view (the 3D sounds' left / right balance) is heard moving. The pan is a balance, as AVAudioPlayer's: -1 leaves only the
// left channel, 1 only the right, 0 both in full.
class PcmVoice final : public SdlVoice {
public:
    PcmVoice(const std::uint8_t* data, std::int32_t size, std::uint16_t channels, std::uint32_t rate, std::uint16_t bits)
        : channels_(channels ? channels : 1), bytesPerSample_(bits == 8 ? 1 : 2) {
        const std::size_t frameBytes = std::size_t(channels_) * bytesPerSample_;
        const std::size_t frames = frameBytes ? std::size_t(size) / frameBytes : 0;
        samples_.resize(frames * channels_);
        for (std::size_t i = 0; i < samples_.size(); ++i) {
            if (bytesPerSample_ == 1) samples_[i] = static_cast<std::int16_t>((int(data[i]) - 128) << 8);
            else samples_[i] = static_cast<std::int16_t>(data[2 * i] | (data[2 * i + 1] << 8));
        }
        frames_ = frames;
        SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(rate)};
        Open(spec, &Feed);
    }
    void Play(std::int32_t loops) override {
        if (!stream_) return;
        SDL_LockAudioStream(stream_);
        SDL_ClearAudioStream(stream_);
        position_ = 0;
        loopsLeft_ = loops;
        fed_ = frames_ == 0;
        stopped_ = false;
        SDL_UnlockAudioStream(stream_);
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: voice %zu frames gain %.2f pan %.2f\n", frames_, double(SDL_GetAudioStreamGain(stream_)), double(pan_.load()));
    }
    void SetPan(float pan) override { pan_ = pan < -1.0f ? -1.0f : pan > 1.0f ? 1.0f : pan; }
    float Pan() const override { return pan_; }
    // Playing until every pass was fed and the device has taken it all.
    bool IsPlaying() const override { return stream_ && !stopped_ && (!fed_ || SDL_GetAudioStreamQueued(stream_) > 0); }

private:
    // On SDL's audio thread, with the stream locked: the frames the device asks for, each pass after the other; after the
    // last pass the stream is flushed (the converter would keep the sample's tail queued and a one-shot would never end).
    static void SDLCALL Feed(void* self, SDL_AudioStream* stream, int additional, int) {
        auto* v = static_cast<PcmVoice*>(self);
        if (v->stopped_ || v->fed_ || additional <= 0) return;
        const float pan = v->pan_;
        const float left = pan > 0.0f ? 1.0f - pan : 1.0f, right = pan < 0.0f ? 1.0f + pan : 1.0f;
        std::size_t wanted = std::size_t(additional) / 4 + 1;   // stereo 16-bit frames
        std::int16_t block[2 * 1024];
        while (wanted > 0 && !v->fed_) {
            const std::size_t count = std::min({wanted, std::size_t(1024), v->frames_ - v->position_});
            for (std::size_t i = 0; i < count; ++i) {
                const std::int16_t* frame = &v->samples_[(v->position_ + i) * v->channels_];
                const float l = frame[0], r = v->channels_ > 1 ? frame[1] : frame[0];
                block[2 * i] = static_cast<std::int16_t>(l * left);
                block[2 * i + 1] = static_cast<std::int16_t>(r * right);
            }
            SDL_PutAudioStreamData(stream, block, int(count * 4));
            v->position_ += count;
            wanted -= count;
            if (v->position_ >= v->frames_) {
                if (v->loopsLeft_ == 0) {
                    v->fed_ = true;
                    SDL_FlushAudioStream(stream);
                } else {
                    if (v->loopsLeft_ > 0) --v->loopsLeft_;
                    v->position_ = 0;
                }
            }
        }
    }
    std::vector<std::int16_t> samples_;   // the sample as signed 16-bit, `channels_` to a frame
    std::size_t frames_ = 0;
    std::uint16_t channels_ = 1, bytesPerSample_ = 2;
    std::size_t position_ = 0;            // the next frame to feed (the audio thread's, under the stream lock)
    std::atomic<std::int32_t> loopsLeft_{0};
    std::atomic<bool> fed_{true};
    std::atomic<float> pan_{0.0f};
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
    // The frame loop's pumped timers (StubClock.cpp): the callback runs on the loop's thread.
    std::uint32_t AddTimer(std::uint32_t intervalMs, AudioPlatform::TimerCallback callback, void* context) override {
        return linuxapp::AddPumpedTimer(intervalMs, callback, context);
    }
    void RemoveTimer(std::uint32_t id) override { linuxapp::RemovePumpedTimer(id); }
};

}  // namespace

namespace linuxapp {
// Kit command `audio` with SC4_AUDIODEBUG: one line per live voice of the sound output.
void AudioVoiceReport() {
    std::printf("audio: %zu voices, device paused %d\n", gVoices.size(), gDevice ? int(SDL_AudioDevicePaused(gDevice)) : -1);
    for (const SdlVoice* voice : gVoices) voice->Report();
}
}  // namespace linuxapp

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
