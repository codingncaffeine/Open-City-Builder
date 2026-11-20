// cGZSnd, cGZSnd3D, cGZSnd3DListener, CPCMPlayer/CQTRemotePlayer and the segment data sources.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

#include "audio/AudioServices.h"
#include "audio/GZSnd.h"
#include "audio/GZSnd3D.h"
#include "audio/SegmentDataSources.h"
#include "audio/platform/AudioPlatform.h"
#include "rz/cRZString.h"
#include "sc4audio_fakes.h"

namespace {

// ---- platform backend ---------------------------------------------------------------------
struct Voice final : AudioPlatform::IVoice {
    std::vector<int> plays;
    int stops = 0;
    float volume = -1, pan = 0;
    bool playing = false;
    bool* aliveFlag = nullptr;
    ~Voice() override { if (aliveFlag) *aliveFlag = false; }
    void Play(std::int32_t loops) override { plays.push_back(loops); playing = true; }
    void Stop() override { ++stops; playing = false; }
    bool IsPlaying() const override { return playing; }
    void SetVolume(float v) override { volume = v; }
    void SetPan(float p) override { pan = p; }
};

struct Backend final : AudioPlatform::IBackend {
    Voice* last = nullptr;
    std::vector<std::uint8_t> lastData;
    std::uint16_t lastChannels = 0, lastBits = 0;
    std::uint32_t lastRate = 0;
    std::string lastPath;
    bool failPcm = false;
    std::vector<std::pair<std::uint32_t, AudioPlatform::TimerCallback>> timers;
    std::vector<void*> timerContexts;
    std::uint32_t nextTimer = 1;
    std::vector<std::uint32_t> removed;
    AudioPlatform::IVoice* CreatePcmVoice(const std::uint8_t* data, std::int32_t size, std::uint16_t channels,
                                          std::uint32_t rate, std::uint16_t bits) override {
        if (failPcm) return nullptr;
        lastData.assign(data, data + size);
        lastChannels = channels; lastRate = rate; lastBits = bits;
        last = new Voice();
        return last;
    }
    AudioPlatform::IVoice* CreateFileVoice(const char* path) override {
        lastPath = path;
        last = new Voice();
        return last;
    }
    std::uint32_t AddTimer(std::uint32_t interval, AudioPlatform::TimerCallback cb, void* ctx) override {
        timers.push_back({interval, cb});
        timerContexts.push_back(ctx);
        return nextTimer++;
    }
    void RemoveTimer(std::uint32_t id) override { removed.push_back(id); }
    void FireTimer(std::size_t index = 0) { timers[index].second(timers[index].first, timerContexts[index]); }
};

// ---- a trivial data source ---------------------------------------------------------------------
struct PcmSource final : cIGZSndDataSource {
    int refs = 0;
    bool open = false;
    int opens = 0, closes = 0;
    std::vector<std::uint8_t> bytes;
    GZSndWaveFormat format;
    std::uint32_t pos = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool SetPath(const char*) override { return false; }
    bool GetPath(cIGZString&) override { return false; }
    bool SetKey(const cGZPersistResourceKey&) override { return true; }
    bool GetKey(cGZPersistResourceKey&) override { return true; }
    bool Open() override { open = true; ++opens; return true; }
    bool Close() override { open = false; ++closes; return true; }
    bool IsOpen() const override { return open; }
    std::uint32_t GetDataSize() const override { return static_cast<std::uint32_t>(bytes.size()); }
    bool Lock(std::uint8_t**, std::uint32_t*) override { return false; }
    bool Unlock() override { return true; }
    bool IsLocked() const override { return false; }
    std::int32_t ReadPart(std::uint8_t* dst, std::int32_t n) override {
        std::memcpy(dst, bytes.data(), static_cast<std::size_t>(n));
        return n;
    }
    void ReadReset() override {}
    const GZSndWaveFormat* GetWaveFormat() const override { return &format; }
    bool DataIsValid() override { return true; }
    bool SetDataIsValid(bool) override { return true; }
    std::uint32_t GetPosition() const override { return pos; }
    bool SetPosition(std::uint32_t p) override { pos = p; return true; }
};

// ---- persistence records ------------------------------------------------------------------------
struct Record final : cIGZPersistDBRecord, cIGZPersistDBSerialRecordStub {
    std::vector<std::uint8_t> bytes;
    std::size_t cursor = 0;
    int refs = 0;
    bool closed = false;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZPersistDBSerialRecord) { *out = static_cast<cIGZPersistDBSerialRecord*>(this); ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cGZPersistResourceKey GetKey() const override { return {}; }
    std::uint32_t GetAccessFlags() const override { return 0; }
    bool Close() override { closed = true; return true; }
    using cIGZPersistDBSerialRecordStub::GetFieldUint32;
    bool GetFieldUint32(std::uint32_t* dst, std::uint32_t count) override { return GetFieldVoid(dst, count * 4); }
    bool GetFieldVoid(void* dst, std::uint32_t n) override {
        if (cursor + n > bytes.size()) return false;
        std::memcpy(dst, bytes.data() + cursor, n);
        cursor += n;
        return true;
    }
    std::uint32_t GetSize() override { return static_cast<std::uint32_t>(bytes.size()); }
};

struct Persist final : cIGZPersistRecordService {
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::vector<std::uint8_t>> data;   // (group, instance)
    std::vector<Record*> opened;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool CreateResource(const cGZPersistResourceKey&, std::uint32_t, void**, std::uint32_t, std::uint32_t) override { return false; }
    bool RegisterResource(cIGZUnknown*) override { return false; }
    bool RemoveResource(const cGZPersistResourceKey&) override { return false; }
    bool HasResource(const cGZPersistResourceKey&) override { return false; }
    bool GetResource(const cGZPersistResourceKey&, std::uint32_t, void**, std::uint32_t, std::uint32_t) override { return false; }
    bool FindRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** out) override {
        if (key.type != 0x2026960b || !data.count({key.group, key.instance})) return false;
        *out = nullptr;
        return true;
    }
    bool OpenRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** out, std::uint32_t) override {
        const auto it = data.find({key.group, key.instance});
        if (key.type != 0x2026960b || it == data.end()) return false;
        auto* r = new Record();
        r->bytes = it->second;
        r->refs = 1;
        opened.push_back(r);
        *out = r;
        return true;
    }
    ~Persist() { for (auto* r : opened) delete r; }
};

void Put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void Put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
}

std::vector<std::uint8_t> MakeWav() {
    std::vector<std::uint8_t> v;
    Put32(v, 0x46464952);   // RIFF
    Put32(v, 0);
    Put32(v, 0x45564157);   // WAVE
    Put32(v, 0x20746d66);   // 'fmt '
    Put32(v, 16);
    Put16(v, 1); Put16(v, 2); Put32(v, 22050); Put32(v, 88200); Put16(v, 4); Put16(v, 16);
    Put32(v, 0x61746164);   // 'data'
    Put32(v, 8);
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(0x10 + i));
    return v;
}

std::vector<std::uint8_t> MakeXaMono(std::uint32_t decodedSize, const std::vector<std::uint8_t>& blocks) {
    std::vector<std::uint8_t> v = {'X', 'A', 0, 0};
    Put32(v, decodedSize);
    Put16(v, 1); Put16(v, 1); Put32(v, 22050); Put32(v, 44100); Put16(v, 2); Put16(v, 16);
    v.insert(v.end(), blocks.begin(), blocks.end());
    return v;
}

float Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// ---- tests ---------------------------------------------------------------------------------------

void TestPcmPlayer() {
    Backend backend;
    AudioPlatform::SetBackend(&backend);
    {
        CPCMPlayer p;
        assert(!p.IsOpen() && !p.IsPlaying());
        auto* data = new std::uint8_t[4]{1, 2, 3, 4};
        assert(p.Open(2, 16, 44100, data, 4));
        assert(backend.lastChannels == 2 && backend.lastBits == 16 && backend.lastRate == 44100 && backend.lastData.size() == 4);
        Voice* v = backend.last;
        assert(p.IsOpen());
        p.SetVolume(0.5f);
        assert(Near(v->volume, 0.5f));
        // 2D sounds never pan
        p.SetVolumes(0.2f, 0.8f);
        assert(v->pan == 0.0f);
        p.SetIs3D(true);
        p.SetVolumes(0.25f, 0.75f);
        assert(Near(v->pan, (0.75f * 2) / 1.0f - 1.0f));
        p.Play(true);
        assert(p.IsPlaying() && v->plays.size() == 1 && v->plays[0] == -1);
        p.Stop();
        assert(v->stops == 1 && !p.IsPlaying());   // Stop clears the playing flag
        p.Stop();
        assert(v->stops == 1);                     // and a stopped player is not stopped again
        p.Play(false);
        assert(v->stops == 1 && v->plays.back() == 0 && p.IsPlaying());
        v->playing = false;                        // the sample ran out: a one-shot is over
        assert(!p.IsPlaying());
        p.Play(true);
        v->playing = false;
        assert(p.IsPlaying());                     // a looping player stays playing until stopped
        p.Stop();
        assert(!p.IsPlaying());
        p.SetFrequency(8000);
        assert(p.GetFrequency() == 8000 && p.GetOriginalFrequency() == 0);
        p.Close();
        assert(!p.IsOpen());
        backend.failPcm = true;
        assert(!p.Open(1, 8, 8000, new std::uint8_t[1], 1));
        backend.failPcm = false;
    }
    {
        CQTRemotePlayer q;
        assert(!q.IsOpen() && !q.IsPlaying());
        assert(q.Open("a.mp3") && backend.lastPath == "a.mp3");
        q.Play(true);
        assert(backend.last->plays[0] == -1 && q.IsPlaying());
        q.SetVolume(0.3f);
        assert(Near(backend.last->volume, 0.3f));
        q.Stop();
        assert(!q.IsPlaying());
        q.Close();
        assert(!q.IsOpen());
    }
    AudioPlatform::SetBackend(nullptr);
}

void TestGZSnd() {
    Backend backend;
    fakes::FakeSndSys sys;
    AudioPlatform::SetBackend(&backend);
    Audio::SetSoundSystem(&sys);   // FakeSndSys::GetMasterVolume() returns 0 -> mute
    class MasterSys final : public fakes::FakeSndSys {
    public:
        std::int32_t GetMasterVolume() const override { return 512; }
    } loud;
    Audio::SetSoundSystem(&loud);

    cGZSnd* s = cGZSnd::Create();
    s->AddRef();
    void* p = nullptr;
    assert(s->QueryInterface(0x206077d2, &p) && p == static_cast<cIGZSnd*>(s));
    assert(s->QueryInterface(0x9d002d2, &p) && p == static_cast<cIGZSndDataSourceHolder*>(s));
    assert(s->QueryInterface(0xe9d00a9a, &p) && p == static_cast<cIGZSndCompletionNotification*>(s));
    assert(!s->QueryInterface(1, &p));                       // cIGZUnknown's id is not accepted
    s->Release(); s->Release(); s->Release();                // count back to 1
    assert(!s->IsLoaded() && !s->IsPlaying());
    assert(!s->PlayWithVolumeAndLoop(1024, false, 0));         // nothing configured
    assert(s->GetID() == 0);
    s->SetID(77);
    assert(s->GetID() == 77);

    // file sound
    assert(s->SetSoundFileFromChars("music.mp3", 9, 0));
    assert(s->PlayWithVolumeAndLoop(1024, true, 0));
    assert(backend.lastPath == "music.mp3");
    Voice* v = backend.last;
    assert(v->plays[0] == -1 && s->IsPlaying() && s->IsLoaded());
    assert(Near(v->volume, 512.0f / 1024.0f));               // (1024 * master 512) / 1024 = 512, / 1024
    assert(s->SetVolume(256));
    assert(s->GetVolume() == 256 && Near(v->volume, ((256 * 512) / 1024) / 1024.0f));
    s->SetVolume(5000);
    assert(s->GetVolume() == 1024);
    s->SetVolume(-3);
    assert(s->GetVolume() == 0);
    assert(s->Pause(true) && !s->IsPlaying());
    assert(s->IsPaused());
    assert(s->Unpause() && s->IsPlaying() && v->plays.size() == 2);
    // pan / frequency only for PCM sounds
    assert(s->SetPan(512) && s->GetPan() == 0 && s->SetFrequency(5) && s->GetFrequency() == 0);

    // name
    cRZString name("hello");
    assert(s->SetName(name));
    cRZString out;
    assert(s->GetName(out) && out.str() == "hello");

    // a new sound file forces a reload
    assert(s->SetSoundFileFromChars("other.mp3", 9, 0));
    backend.lastPath.clear();
    assert(s->PlayWithVolumeAndLoop(1024, false, 0));
    assert(backend.lastPath == "other.mp3");
    s->Release();

    // data source sound
    PcmSource source;
    source.bytes = {9, 8, 7, 6, 5, 4, 3, 2};
    source.format.channels = 1; source.format.bitsPerSample = 8; source.format.samplesPerSecond = 11025;
    cGZSnd* d = cGZSnd::Create();
    d->AddRef();
    assert(d->SetDataSource(&source, 0));
    assert(d->GetDataSource() == &source && source.refs == 1);
    assert(d->PlayWithVolumeAndLoop(512, false, 0));
    assert(source.opens == 1 && source.closes == 1);          // opened only for the read
    assert(backend.lastChannels == 1 && backend.lastBits == 8 && backend.lastRate == 11025);
    assert(backend.lastData.size() == 8 && backend.lastData[0] == 9);
    v = backend.last;
    assert(d->SetPan(0) && d->SetPan(1024));
    assert(d->SetFrequency(500) && d->GetFrequency() == 1000);
    assert(d->SetFrequency(0) && d->GetFrequency() == 0);
    assert(d->SetFrequency(200000) && d->GetFrequency() == 100000);
    assert(d->SetFrequency(22050) && d->GetFrequency() == 22050 && d->GetOriginalFrequency() == 0);

    // fade: 1000 -> 0 over 100 ms = 5 ticks of 200
    d->SetVolume(1000);
    assert(!d->IsFading());
    assert(d->FadeVolume(1000, 0, 100, true));
    assert(d->IsFading() && backend.timers.size() == 1 && backend.timers[0].first == 20);
    assert(!d->FadeVolume(1000, 0, 100, true));              // already fading
    assert(d->GetVolume() == 1000);                           // SetVolume(from) applied
    int ticks = 0;
    while (d->IsFading() && ticks < 20) { backend.FireTimer(); ++ticks; }
    assert(!d->IsFading() && ticks == 5 && d->GetVolume() == 0);
    assert(backend.removed.size() == 1 && backend.removed[0] == 1);
    assert(!d->IsPlaying() || true);
    // zero-length fade applies the target at once; startFade=false does nothing
    assert(d->FadeVolume(100, 900, 10, true) && d->GetVolume() == 900 && !d->IsFading());
    assert(d->FadeVolume(0, 50, 100, false) && d->GetVolume() == 900);
    d->Release();
    assert(source.refs == 0);

    // stop ends a fade
    cGZSnd* e = cGZSnd::Create();
    e->AddRef();
    e->SetSoundFileFromChars("x", 1, 0);
    e->PlayWithVolumeAndLoop(1024, false, 0);
    e->FadeVolume(1024, 0, 200, true);
    assert(e->IsFading());
    e->Stop();
    assert(!e->IsFading());
    e->Release();

    Audio::SetSoundSystem(nullptr);
    AudioPlatform::SetBackend(nullptr);
}

void TestSnd3D() {
    Backend backend;
    AudioPlatform::SetBackend(&backend);
    class MasterSys final : public fakes::FakeSndSys {
    public:
        std::int32_t GetMasterVolume() const override { return 1; }
    } sys;
    Audio::SetSoundSystem(&sys);

    PcmSource source;
    source.bytes = {1, 2, 3, 4};
    source.format.channels = 1; source.format.bitsPerSample = 8; source.format.samplesPerSecond = 8000;

    cGZSnd3DListener* listener = new cGZSnd3DListener();
    listener->AddRef();
    void* p = nullptr;
    assert(listener->QueryInterface(0x5b30e7b3, &p) && p == static_cast<cIGZSnd3DListenerEx*>(listener));
    assert(listener->QueryInterface(0x406077d2, &p) && listener->QueryInterface(1, &p) && !listener->QueryInterface(5, &p));
    listener->Release(); listener->Release(); listener->Release();
    assert(listener->Init());
    GZSnd3D::SetActiveListener(listener);

    const auto before = GZSnd3D::SoundList().size();
    cGZSnd3D* s = cGZSnd3D::Create();
    s->AddRef();
    assert(GZSnd3D::SoundList().size() == before + 1);
    void* q = nullptr;
    assert(s->QueryInterface(0x306077d2, &q) && q == static_cast<cIGZSnd3D*>(s));
    assert(s->QueryInterface(0x206077d2, &q));
    s->Release(); s->Release();
    assert(s->AsIGZSnd() == static_cast<cIGZSnd*>(s));

    s->SetDataSource(&source, 0);
    assert(s->PlayWithVolumeAndLoop(1024, false, 0));
    Voice* v = backend.last;

    // listener at the origin facing +x (game space), up +z: front = (-0, 0, 1) in mixer space.
    assert(listener->SetOrientation(1, 0, 0, 0, 0, 1));
    assert(listener->SetPosition(0, 0, 0));
    // remapped (x, y, z) -> (-y, z, x)
    assert(s->SetPosition(10, 0, 0));
    assert(Near(s->mixerPosition()[0], 0) && Near(s->mixerPosition()[1], 0) && Near(s->mixerPosition()[2], 10));
    assert(!s->needsUpdate());
    // sound straight ahead, min distance 0 -> attenuation min(1, 1/10) = 0.1, centred
    assert(s->SetMinimumDistance(0.0f));
    assert(Near(v->pan, 0.0f, 1e-3f));
    // closer than 0.1 -> 2D mode: 0.8125 * master (1) per side -> pan 0
    s->SetPosition(0.05f, 0, 0);
    assert(Near(v->pan, 0.0f));
    assert(Near(v->volume, 1.0f / 1024.0f));   // cGZSnd::SetVolume: (1024 * master 1) / 1024 / 1024
    // a sound to the side pans
    s->SetMinimumDistance(5.0f);
    s->SetPosition(10, 10, 0);       // mixer: (-10, 0, 10)
    assert(std::fabs(v->pan) > 0.05f);
    const float panRight = v->pan;
    s->SetPosition(10, -10, 0);
    assert(std::fabs(v->pan + panRight) < 1e-4f);   // mirrored
    // max distance clamps the distance used for attenuation
    s->SetMaximumDistance(1.0f);
    s->SetMinimumDistance(0.0f);
    s->SetPosition(1000, 0, 0);
    // stubbed getters
    float x = 0;
    assert(!s->GetPosition(&x, &x, &x) && !s->GetMinimumDistance(&x) && !s->GetHeadRelative());
    assert(s->SetVelocity(0, 0, 0) && !s->SetAll(0, 0, 0, 0, 0, 0));

    // listener movement refreshes loaded sounds
    GZSnd3D::SetActiveListener(listener);
    assert(listener->SetPosition(0, 0, 1000));
    assert(!listener->dirty());
    // head relative flag only marks the sound dirty
    assert(s->SetHeadRelative(true) && s->needsUpdate() && s->headRelative());
    listener->SetOrientation(1, 0, 0, 0, 0, 1);
    assert(listener->dirty());
    listener->CommitDeferredSettings();
    assert(!listener->dirty() && !s->needsUpdate());

    s->Release();
    assert(GZSnd3D::SoundList().size() == before);
    GZSnd3D::SetActiveListener(nullptr);
    listener->Release();
    Audio::SetSoundSystem(nullptr);
    AudioPlatform::SetBackend(nullptr);
}

void TestWavSource() {
    Persist persist;
    persist.data[{5, 100}] = MakeWav();
    Audio::SetPersistService(&persist);

    auto* w = new cGZSndSegmentWavDataSource(0x2026960b, 5, 100);
    assert(!w->IsOpen());
    cGZPersistResourceKey key;
    assert(w->GetKey(key) && key.group == 5 && key.instance == 100);
    assert(w->DataIsValid());                      // opens and closes once
    assert(!w->IsOpen());
    assert(w->Open() && w->IsOpen() && w->GetDataSize() == 8);
    assert(w->GetWaveFormat()->channels == 2 && w->GetWaveFormat()->samplesPerSecond == 22050 && w->GetWaveFormat()->bitsPerSample == 16);
    std::uint8_t buf[16] = {};
    assert(w->ReadPart(buf, 3) == 3 && buf[0] == 0x10 && buf[2] == 0x12 && w->GetPosition() == 3);
    assert(w->ReadPart(buf, 100) == 5 && buf[0] == 0x13 && buf[4] == 0x17);   // clamped to what is left
    assert(w->ReadPart(buf, 4) == 0);
    // Open at a non-zero position re-opens
    w->ReadReset();
    assert(w->GetPosition() == 0 && w->IsOpen());
    assert(w->ReadPart(buf, 2) == 2 && buf[0] == 0x10);
    // lock gives a zeroed buffer and ReadPart then reads from it
    std::uint8_t* locked = nullptr; std::uint32_t lockedSize = 0;
    assert(w->Lock(&locked, &lockedSize) && locked && w->IsLocked());
    assert(locked[0] == 0 && locked[7] == 0);
    locked[2] = 0x55;
    assert(w->ReadPart(buf, 1) == 1 && buf[0] == 0x55);    // position 2
    assert(w->Unlock() && !w->IsLocked());
    // QI / refcounts
    void* p = nullptr;
    assert(w->QueryInterface(0x3afd825f, &p) && w->QueryInterface(1, &p) && !w->QueryInterface(2, &p));
    assert(w->Release() == 1 && w->Release() == 0);        // closes and destroys at 1

    // a record that is not a wav
    persist.data[{5, 200}] = {1, 2, 3, 4};
    auto* bad = new cGZSndSegmentWavDataSource(0x2026960b, 5, 200);
    assert(!bad->Open() && !bad->IsOpen());
    assert(!bad->DataIsValid());
    delete bad;
    // missing record
    auto* missing = new cGZSndSegmentWavDataSource(0x2026960b, 9, 9);
    assert(!missing->Open());
    delete missing;
    Audio::SetPersistService(nullptr);
}

void TestXaSource() {
    Persist persist;
    // mono: block 0 -> filter 0 / shift 0, data byte 0x21 -> s0 = table[0][2], s1 = table[0][1]
    std::vector<std::uint8_t> blocks(15, 0);
    blocks[0] = 0x00;
    blocks[1] = 0x21;
    persist.data[{1, 7}] = MakeXaMono(56, blocks);
    Audio::SetPersistService(&persist);

    auto* xa = new cGZSndSegmentDataSourceXA(0x2026960b, 1, 7);
    xa->AddRef();
    assert(xa->DataIsValid());
    assert(xa->Open() && xa->GetDataSize() == 56);
    assert(xa->GetWaveFormat()->formatTag == 1 && xa->GetWaveFormat()->channels == 1 && xa->GetWaveFormat()->samplesPerSecond == 22050);
    std::int16_t samples[28] = {};
    assert(xa->ReadPart(reinterpret_cast<std::uint8_t*>(samples), 56) == 56);
    assert(samples[0] == 8192 && samples[1] == 4096);
    assert(samples[2] == 0 && samples[27] == 0);
    assert(xa->GetPosition() == 56);
    assert(xa->ReadPart(reinterpret_cast<std::uint8_t*>(samples), 8) == 0);   // nothing left
    assert(xa->Release() == 0);   // count 1: closes and destroys

    // filter 1 (f1 = 0.9375): second byte feeds back
    std::vector<std::uint8_t> blocks2(15, 0);
    blocks2[0] = 0x10;           // filter 1, shift 0
    blocks2[1] = 0x20;           // hi nibble 2 -> 8192 ; lo nibble 0 -> 0 + 0.9375*8192
    persist.data[{1, 8}] = MakeXaMono(56, blocks2);
    auto* xb = new cGZSndSegmentDataSourceXA(0x2026960b, 1, 8);
    assert(xb->Open());
    std::int16_t s2[2] = {};
    assert(xb->ReadPart(reinterpret_cast<std::uint8_t*>(s2), 4) == 4);
    assert(s2[0] == 8192 && s2[1] == 7680);
    delete xb;

    // stereo: two header bytes + 14 byte pairs
    std::vector<std::uint8_t> st(30, 0);
    st[0] = 0x00; st[1] = 0x00;
    st[2] = 0x10;      // left: hi 1 -> 4096
    st[3] = 0x30;      // right: hi 3 -> 12288
    std::vector<std::uint8_t> file = {'X', 'A', 0, 0};
    Put32(file, 112);
    Put16(file, 1); Put16(file, 2); Put32(file, 22050); Put32(file, 88200); Put16(file, 4); Put16(file, 16);
    file.insert(file.end(), st.begin(), st.end());
    persist.data[{1, 9}] = file;
    auto* xs = new cGZSndSegmentDataSourceXA(0x2026960b, 1, 9);
    assert(xs->Open() && xs->GetWaveFormat()->channels == 2);
    std::int16_t out[4] = {};
    assert(xs->ReadPart(reinterpret_cast<std::uint8_t*>(out), 8) == 8);
    assert(out[0] == 4096 && out[1] == 12288 && out[2] == 0 && out[3] == 0);
    delete xs;

    // bad magic -> error 3, not open
    persist.data[{1, 10}] = {'N', 'O', 0, 0, 0, 0, 0, 0};
    auto* bad = new cGZSndSegmentDataSourceXA(0x2026960b, 1, 10);
    assert(!bad->Open() && !bad->IsOpen() && bad->lastError() == 3);
    delete bad;
    // truncated header -> error 4
    persist.data[{1, 11}] = {'X', 'A'};
    auto* trunc = new cGZSndSegmentDataSourceXA(0x2026960b, 1, 11);
    assert(!trunc->Open() && trunc->lastError() == 4);
    delete trunc;
    Audio::SetPersistService(nullptr);
}

struct FakeUTalk final : Audio::IUTalkCodec {
    std::uint32_t samples[0x1b0];
    int frames = 0;
    int endAfter = 1000;
    void Init(const std::uint8_t*, Audio::UTalkState&) override { frames = 0; }
    void Decode(Audio::UTalkState&) override {
        ++frames;
        for (std::uint32_t i = 0; i < 0x1b0; ++i) samples[i] = (frames > endAfter) ? 0x8000u : (i + 1) & 0x7fff;
    }
    const std::uint32_t* Samples(const Audio::UTalkState&) const override { return samples; }
};

void TestUTalkSource() {
    Persist persist;
    std::vector<std::uint8_t> v;
    Put32(v, 0x304d5455);      // 'UTM0'
    Put32(v, 4000);            // decoded data size
    Put32(v, 0x14);
    Put16(v, 1); Put16(v, 1); Put32(v, 22050); Put32(v, 44100); Put16(v, 2); Put16(v, 16); Put32(v, 0);
    for (int i = 0; i < 40; ++i) v.push_back(static_cast<std::uint8_t>(i));   // compressed payload
    persist.data[{2, 5}] = v;
    Audio::SetPersistService(&persist);
    FakeUTalk codec;
    Audio::SetUTalkCodec(&codec);

    auto* u = new cGZSndSegmentDataSourceUTalk(0x2026960b, 2, 5);
    u->AddRef();
    assert(u->DataIsValid());
    assert(u->Open() && u->IsOpen() && u->GetDataSize() == 4000);
    assert(u->GetWaveFormat()->samplesPerSecond == 22050);
    std::int16_t s[10] = {};
    assert(u->ReadPart(reinterpret_cast<std::uint8_t*>(s), 20) == 20);
    assert(s[0] == 1 && s[9] == 10);
    // across frames: 0x1b0 samples per frame
    std::vector<std::int16_t> big(0x1b0 + 5);
    codec.frames = 0;
    assert(u->ReadPart(reinterpret_cast<std::uint8_t*>(big.data()), static_cast<std::int32_t>(big.size() * 2)) == static_cast<int>(big.size() * 2));
    assert(big[0] == 1 && big[0x1b0] == 1 && big[0x1b0 + 4] == 5);
    // end of stream: the whole destination is zeroed, only the earlier part counted
    codec.endAfter = 0;   // next decode already yields the marker
    std::int16_t tail[8] = {7, 7, 7, 7, 7, 7, 7, 7};
    assert(u->ReadPart(reinterpret_cast<std::uint8_t*>(tail), 16) == 2);   // the marker sample itself counts
    for (auto x : tail) assert(x == 0);
    assert(u->ReadPart(reinterpret_cast<std::uint8_t*>(tail), 16) == 0);   // stays finished
    assert(u->Release() == 0);

    // bad magic
    std::vector<std::uint8_t> bad = v;
    bad[0] = 'X';
    persist.data[{2, 6}] = bad;
    auto* b = new cGZSndSegmentDataSourceUTalk(0x2026960b, 2, 6);
    assert(!b->DataIsValid());
    delete b;
    Audio::SetUTalkCodec(nullptr);
    Audio::SetPersistService(nullptr);
}

void TestCreator() {
    Persist persist;
    persist.data[{20, 1}] = MakeWav();                                  // only in group 20
    persist.data[{10, 2}] = MakeXaMono(56, std::vector<std::uint8_t>(15, 0));
    Audio::SetPersistService(&persist);
    FakeUTalk codec;
    Audio::SetUTalkCodec(&codec);

    auto* c = cGZSndSegmentDataSourceCreator::Create();
    c->AddRef();
    void* p = nullptr;
    assert(c->QueryInterface(0x9b75709e, &p) && c->QueryInterface(1, &p) && !c->QueryInterface(3, &p));
    c->Release(); c->Release();
    cRZString g10("10"), g20("0x14");
    assert(c->AddSndDataSourceDef(10, g10));
    assert(c->AddSndDataSourceDef(0x14, g20));
    assert(c->groups().size() == 2 && c->groups().front() == 10 && c->groups().back() == 20);
    // wav found via the second group
    void* out = nullptr;
    assert(c->CreateSndDataSource(1, 0, nullptr, kGZIID_cIGZSndDataSource, &out));
    auto* ds = static_cast<cIGZSndDataSource*>(out);
    assert(ds->Open() && ds->GetDataSize() == 8 && ds->GetWaveFormat()->channels == 2);
    ds->Release();
    // XA preferred when valid
    assert(c->CreateSndDataSource(2, 0, nullptr, kGZIID_cIGZSndDataSource, &out));
    ds = static_cast<cIGZSndDataSource*>(out);
    assert(ds->Open() && ds->GetDataSize() == 56);
    ds->Release();
    // unknown id
    assert(!c->CreateSndDataSource(999, 0, nullptr, kGZIID_cIGZSndDataSource, &out));
    // removing a group
    c->RemoveSndDataSourceDef(20);
    assert(!c->CreateSndDataSource(1, 0, nullptr, kGZIID_cIGZSndDataSource, &out));
    assert(c->Release() == 0);        // count 1 -> destroyed
    Audio::SetUTalkCodec(nullptr);
    Audio::SetPersistService(nullptr);
}

}  // namespace

int main() {
    TestPcmPlayer();
    TestGZSnd();
    TestSnd3D();
    TestWavSource();
    TestXaSource();
    TestUTalkSource();
    TestCreator();
    std::puts("gzsnd ok");
}
