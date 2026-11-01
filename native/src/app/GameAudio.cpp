// GameAudio — the game's sound for the native builds: Sound.dat opened through the recovered persistence layer, the radio
// station's music and the UI sounds. The output is the platform backend CreatePlatformAudioBackend() makes
// (GameAudioAV.mm on macOS, linux/GameAudioSDL.cpp on Linux).
#include <algorithm>
#include <random>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <set>

#include "app/GameAudio.h"
#include "audio/AudioServices.h"
#include "audio/SegmentDataSources.h"
#include "audio/platform/AudioPlatform.h"
#include "gz/AudioPersistAdapter.h"
#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cGZPersistResourceKeyList.h"
#include "gz/cGZPersistResourceManager.h"

struct GameAudio::Impl {
    cGZDBSegmentPackedFile* segment = nullptr;
    cGZPersistResourceManager* manager = nullptr;
    AudioPersistAdapter* persist = nullptr;
    cIGZSndDataSourceCreator* creator = nullptr;
    std::unique_ptr<AudioPlatform::IBackend> backend{CreatePlatformAudioBackend()};
    AudioPlatform::IVoice* ambient = nullptr;
    AudioPlatform::IVoice* click = nullptr;
    std::vector<AudioPlatform::IVoice*> voices;   // the effects still playing

    bool Decode(std::uint32_t instance, std::vector<std::uint8_t>& pcm, GZSndWaveFormat& fmt) {
        void* raw = nullptr;
        if (!creator->CreateSndDataSource(instance, 0, nullptr, kGZIID_cIGZSndDataSource, &raw)) return false;
        auto* source = static_cast<cIGZSndDataSource*>(raw);
        bool ok = source->Open();
        if (ok) {
            fmt = *source->GetWaveFormat();
            const std::uint32_t expected = source->GetDataSize();
            pcm.resize(expected);
            std::uint32_t got = 0;
            while (got < expected) {
                const std::int32_t n = source->ReadPart(pcm.data() + got, static_cast<std::int32_t>(expected - got));
                if (n <= 0) break;
                got += static_cast<std::uint32_t>(n);
            }
            pcm.resize(got);
            ok = got != 0;
            source->Close();
        }
        source->Release();
        return ok;
    }
    AudioPlatform::IVoice* MakeVoice(std::uint32_t instance, double* seconds = nullptr) {
        std::vector<std::uint8_t> pcm;
        GZSndWaveFormat fmt{};
        if (!Decode(instance, pcm, fmt)) return nullptr;
        if (seconds) *seconds = double(pcm.size()) / std::max<std::uint32_t>(1, fmt.averageBytesPerSecond);
        return backend->CreatePcmVoice(pcm.data(), static_cast<std::int32_t>(pcm.size()), fmt.channels,
                                      fmt.samplesPerSecond, fmt.bitsPerSample);
    }
};

GameAudio::~GameAudio() { Close(); }

// Stops and frees every voice and the backend; safe to call twice. The main loop calls it before SDL_Quit: the voices
// are SDL audio streams, and the static GameAudio would otherwise free them after SDL shut its audio down (a crash on
// every quit).
void GameAudio::Close() {
    if (impl_) {
        delete impl_->ambient;
        delete impl_->click;
        if (impl_->manager) impl_->manager->Shutdown();
        for (AudioPlatform::IVoice* voice : impl_->voices) delete voice;
        impl_->voices.clear();
        if (impl_->segment) impl_->segment->Close();
        delete impl_;
        impl_ = nullptr;
    }
}

bool GameAudio::Open(const std::string& soundDat, std::string& error) {
    impl_ = new Impl();
    impl_->segment = new cGZDBSegmentPackedFile();
    impl_->segment->AddRef();
    impl_->segment->Init();
    cRZString path(soundDat.c_str());
    impl_->segment->SetPath(path);
    if (!impl_->segment->Open(true, false)) { error = "cannot open " + soundDat; return false; }
    auto* keys = new cGZPersistResourceKeyList();
    keys->AddRef();
    cGZPersistResourceKeyFilterByType filter(kGZResourceTypeSndSegment);
    filter.AddRef();
    impl_->segment->GetResourceKeyList(keys, &filter);
    std::set<std::uint32_t> groups;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bySize;   // instance, bytes
    for (std::uint32_t i = 0; i < keys->Size(); ++i) {
        const auto* k = keys->GetKey(i);
        groups.insert(k->group);
        const std::uint32_t size = impl_->segment->GetRecordSize(*k);
        sounds_.push_back(k->instance);
        bySize.push_back({k->instance, size});
    }
    if (!impl_->backend) { error = "no audio output"; return false; }
    AudioPlatform::SetBackend(impl_->backend.get());
    impl_->manager = new cGZPersistResourceManager();
    impl_->manager->AddRef();
    impl_->manager->RegisterDBSegment(*impl_->segment);
    impl_->persist = new AudioPersistAdapter(impl_->manager);
    Audio::SetPersistService(impl_->persist);
    impl_->creator = cGZSndSegmentDataSourceCreator::Create();
    impl_->creator->AddRef();
    for (std::uint32_t g : groups) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%x", g);
        cRZString def(text);
        impl_->creator->AddSndDataSourceDef(g, def);
    }
    // long sounds make the ambient bed, short ones are the UI clicks; spread the picks over the file
    const std::size_t step = std::max<std::size_t>(1, bySize.size() / 40);
    for (std::size_t i = 0; i < bySize.size(); i += step) {
        if (bySize[i].second > 60000) ambient_.push_back(bySize[i].first);
        else if (bySize[i].second > 1500 && bySize[i].second < 12000) clicks_.push_back(bySize[i].first);
    }
    if (ambient_.empty())
        for (std::size_t i = 0; i < bySize.size() && ambient_.size() < 16; i += step)
            if (bySize[i].second > 20000) ambient_.push_back(bySize[i].first);
    return true;
}

void GameAudio::SetMuted(bool muted) {
    muted_ = muted;
    if (impl_ && impl_->ambient) impl_->ambient->SetVolume(muted ? 0.0f : musicGain_);
}

void GameAudio::NextAmbient() {
    if (!impl_ || ambient_.empty()) return;
    for (std::size_t tries = 0; tries < ambient_.size(); ++tries) {
        const std::uint32_t instance = ambient_[nextAmbient_++ % ambient_.size()];
        double seconds = 0;
        AudioPlatform::IVoice* voice = impl_->MakeVoice(instance, &seconds);
        if (!voice) continue;
        if (impl_->ambient) { impl_->ambient->Stop(); delete impl_->ambient; }
        impl_->ambient = voice;
        voice->SetVolume(muted_ ? 0.0f : 0.6f);
        voice->Play(0);
        char name[64];
        std::snprintf(name, sizeof name, "Sound.dat %08x (%.0fs)", instance, seconds);
        current_ = name;
        return;
    }
}

void GameAudio::PlayClick() {
    if (!impl_ || clicks_.empty() || muted_) return;
    for (std::size_t tries = 0; tries < clicks_.size(); ++tries) {
        AudioPlatform::IVoice* voice = impl_->MakeVoice(clicks_[nextClick_++ % clicks_.size()]);
        if (!voice) continue;
        if (impl_->click) { impl_->click->Stop(); delete impl_->click; }
        impl_->click = voice;
        voice->SetVolume(0.8f);
        voice->Play(0);
        return;
    }
}

bool GameAudio::PlayTrack(std::uint32_t trackId) {
    if (!impl_ || !impl_->segment || muted_ || !soundEnabled_) return false;
    auto found = trackSound_.find(trackId);
    if (found == trackSound_.end()) {
        std::uint32_t sound = 0;
        const cGZPersistResourceKey key{0x0b8d821a, 0xca4d1943, trackId};
        const std::uint32_t size = impl_->segment->GetRecordSizeRaw(key);
        if (size > 12 && size < 4096) {
            std::vector<char> bytes(size + 1, 0);
            std::uint32_t got = size;
            impl_->segment->ReadRecordRaw(key, bytes.data(), got);
            // "2DKT" + size, then "TKDT,2,,<sound>,<property set>,..." : the sound id is the fourth comma field
            const char* text = std::strstr(bytes.data() + 8, "TKDT");
            if (text) {
                int field = 0;
                for (const char* c = text; *c && field < 4; ++c) {
                    if (*c == ',') {
                        ++field;
                        if (field == 3) sound = static_cast<std::uint32_t>(std::strtoul(c + 1, nullptr, 0));
                    }
                }
            }
        }
        found = trackSound_.emplace(trackId, sound).first;
    }
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: track %08x -> sample %08x\n", trackId, found->second);
    if (found->second == 0) return false;
    AudioPlatform::IVoice* voice = impl_->MakeVoice(found->second);
    if (!voice) return false;
    voice->SetVolume(soundGain_);
    voice->Play(0);
    impl_->voices.push_back(voice);
    return true;
}

void GameAudio::SetStation(const std::string& directory) {
    if (directory == stationDirectory_) return;
    stationDirectory_ = directory;
    playlist_.clear();
    nextSong_ = 0;
    if (impl_ && impl_->ambient) { impl_->ambient->Stop(); delete impl_->ambient; impl_->ambient = nullptr; }
    currentSong_.clear();
    if (directory.empty()) return;
    std::error_code listError;
    for (const auto& entry : std::filesystem::directory_iterator(directory, listError)) {
        std::string extension = entry.path().extension().string();
        for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (extension == ".mp3") playlist_.push_back(directory + "/" + entry.path().filename().string());
    }
    std::sort(playlist_.begin(), playlist_.end());   // the listing order varies; the shuffle below picks the order
    Shuffle();
}

// A new random order every launch and every time the list has played through (std::rand was never seeded, so every
// launch played the same order).
void GameAudio::Shuffle() {
    static std::mt19937 random{std::random_device{}()};
    std::shuffle(playlist_.begin(), playlist_.end(), random);
    // Never start a new round with the song that just ended.
    if (playlist_.size() > 1 && playlist_.front() == currentSong_) std::swap(playlist_.front(), playlist_.back());
    nextSong_ = 0;
    if (std::getenv("SC4_DEBUG")) {
        std::fprintf(stderr, "audio: playlist order:");
        for (const std::string& song : playlist_) std::fprintf(stderr, " %s", std::filesystem::path(song).filename().string().c_str());
        std::fprintf(stderr, "\n");
    }
}

void GameAudio::Tick() {
    if (impl_ && !playlist_.empty() && !muted_ && musicEnabled_ && !radioHeld_ && (!impl_->ambient || !impl_->ambient->IsPlaying())) {
        for (std::size_t tries = 0; tries < playlist_.size(); ++tries) {
            if (nextSong_ >= playlist_.size()) Shuffle();
            const std::string path = playlist_[nextSong_++];
            if (songEnabled_ && !songEnabled_(path)) continue;
            if (PlaySong(path)) break;
        }
    }
    if (impl_) {
        for (std::size_t i = 0; i < impl_->voices.size();) {
            if (!impl_->voices[i]->IsPlaying()) { delete impl_->voices[i]; impl_->voices.erase(impl_->voices.begin() + i); }
            else ++i;
        }
    }
}

bool GameAudio::PlaySong(const std::string& path) {
    if (!impl_) return false;
    AudioPlatform::IVoice* voice = impl_->backend->CreateFileVoice(path.c_str());
    if (!voice) return false;
    if (impl_->ambient) { impl_->ambient->Stop(); delete impl_->ambient; }
    impl_->ambient = voice;
    voice->SetVolume(musicGain_);
    voice->Play(0);
    current_ = currentSong_ = path;
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: music %s\n", path.c_str());
    return true;
}

void GameAudio::StopRadio() {
    radioHeld_ = true;
    if (impl_ && impl_->ambient) { impl_->ambient->Stop(); delete impl_->ambient; impl_->ambient = nullptr; }
    currentSong_.clear();
}

void GameAudio::SetMusic(bool enabled, float gain) {
    if (enabled == musicEnabled_ && gain == musicGain_) return;
    musicEnabled_ = enabled;
    musicGain_ = gain;
    if (!impl_ || !impl_->ambient) return;
    if (!enabled) { impl_->ambient->Stop(); delete impl_->ambient; impl_->ambient = nullptr; currentSong_.clear(); }
    else impl_->ambient->SetVolume(gain);
}

void GameAudio::SetSound(bool enabled, float gain) {
    soundEnabled_ = enabled;
    soundGain_ = gain;
    if (!impl_) return;
    for (AudioPlatform::IVoice* voice : impl_->voices) {
        if (enabled) voice->SetVolume(gain);
        else voice->Stop();
    }
}
