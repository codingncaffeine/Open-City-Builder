#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/DirectSoundMac.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
static bool Near(float a, float b, float e = 1e-4f) { return std::fabs(a - b) <= e; }

namespace {
struct Api final : IAudioQueueApi {
    int created = 0, started = 0, disposed = 0, enqueued = 0, failNew = 0;
    std::vector<std::vector<std::uint8_t>> storage;
    std::vector<AudioQueueBuffer> bufs;
    std::vector<std::pair<std::uint32_t, float>> params;
    int dummy[64];
    IDirectSoundBuffer_Mac* lastUser = nullptr;
    void* NewOutput(const AudioStreamBasicDescription&, IDirectSoundBuffer_Mac* user) override {
        if (failNew) return nullptr;
        lastUser = user;
        return &dummy[created++];
    }
    void EnableTimePitchBypass(void*) override {}
    bool Start(void*) override { ++started; return true; }
    bool AllocateBuffer(void*, std::uint32_t bytes, AudioQueueBuffer** out) override {
        storage.emplace_back(bytes + 16);
        bufs.emplace_back();
        bufs.back().capacity = bytes;
        bufs.back().data = storage.back().data();
        *out = &bufs.back();
        return true;
    }
    bool Enqueue(void*, AudioQueueBuffer*) override { ++enqueued; return true; }
    void SetParameter(void*, std::uint32_t p, float v) override { params.push_back({p, v}); }
    void Dispose(void*) override { ++disposed; }
};
}  // namespace

int main() {
    Api api;
    SetAudioQueueApi(&api);
    for (int i = 0; i < 16; ++i) IID_IDirectSound3DBuffer.bytes[i] = std::uint8_t(i + 1), IID_IDirectSound3DListener.bytes[i] = std::uint8_t(i + 21), IID_IDirectSoundBuffer8.bytes[i] = std::uint8_t(i + 41);
    IDirectSound_Mac::ResetGlobalsForTest();
    auto* ds = new IDirectSound_Mac();
    ds->AddRef();
    CHECK(IDirectSound_Mac::Instance() == ds && IDirectSound_Mac::MacGetSoundBufferMaxCount() == 256);
    _DSCAPS caps{};
    CHECK(ds->GetCaps(&caps) == 0 && caps.flags == 0xf7f && caps.minSecondarySampleRate == 500 && caps.maxSecondarySampleRate == 48000 &&
          caps.freeHwMixingAllBuffers == 32 && caps.unlockTransferRateHwBuffers == 2);
    void* none = reinterpret_cast<void*>(1);
    CHECK(ds->QueryInterface(IID_IDirectSoundBuffer8, &none) == DSERR_NOINTERFACE && !none);

    tWAVEFORMATEX fmt{1, 2, 22050, 88200, 4, 16, 0};   // 22050 Hz, 16-bit stereo
    _DSBUFFERDESC desc{0x14, 2 /*static*/, 44100, 0, &fmt};
    IDirectSoundBuffer_Mac* buf = nullptr;
    CHECK(ds->CreateSoundBuffer(&desc, &buf, nullptr) == 0 && buf);
    CHECK(buf->kind() == IDirectSoundBuffer_Mac::kStatic && buf->size() == 44100 && IDirectSound_Mac::MacGetSoundBuffer(0) == buf);
    CHECK(buf->MacGetDirectSound() == ds);
    // 22050 / 2 bytes * 2 channels / 1000 * 500 = 11025 ... integer: (22050/2=11025)*2=22050 /1000=22 *500 = 11000
    CHECK(buf->chunkBytes() == 11000 && buf->streamFormat().sampleRate == 22050.0 && buf->streamFormat().bytesPerFrame == 4);

    // Lock / unlock
    void *p1 = nullptr, *p2 = nullptr; std::uint32_t s1 = 0, s2 = 0;
    CHECK(buf->Lock(100, 200, &p1, &s1, &p2, &s2, 0) == 0 && p1 && s1 == 200 && !p2 && s2 == 0);
    CHECK(buf->Lock(44000, 500, &p1, &s1, &p2, &s2, 0) == 0 && s1 == 100 && p2 && s2 == 400);
    CHECK(buf->Lock(0, 0, &p1, &s1, nullptr, nullptr, 2) == 0 && s1 == 44100);
    CHECK(buf->Lock(50000, 1, &p1, &s1, &p2, &s2, 0) == DSERR_INVALIDPARAM && buf->Lock(0, 10, nullptr, &s1, &p2, &s2, 0) == DSERR_INVALIDPARAM);
    for (std::uint32_t i = 0; i < 44100; ++i) static_cast<std::uint8_t*>(p1)[i] = std::uint8_t(i);
    CHECK(buf->Unlock(p1, s1, nullptr, 0) == 0);
    _DSBCAPS bc{}; CHECK(buf->GetCaps(&bc) == 0 && bc.size == 0x14 && bc.bufferBytes == 44100 && buf->GetCaps(nullptr) == DSERR_INVALIDPARAM);
    tWAVEFORMATEX got{};
    CHECK(buf->GetFormat(&got, 0x12, nullptr) == 0 && got.samplesPerSec == 22050 && buf->GetFormat(&got, 0x10, nullptr) == DSERR_INVALIDPARAM);

    // Play: one chunk is queued, the callback refills until the sound ends
    std::uint32_t status = 99;
    CHECK(buf->GetStatus(&status) == 0 && status == 0);   // stopped sounds with data report 0
    CHECK(buf->Play(0, 0, 0) == 0 && buf->playing() && buf->queue() && api.started == 1 && api.enqueued == 1);
    CHECK(buf->GetStatus(&status) == 0 && status == 1);
    AudioQueueBuffer* aq = &api.bufs.back();
    CHECK(aq->size == 11000 && static_cast<std::uint8_t*>(aq->data)[5] == 5);
    // callback 1: next chunk
    AudioBufferCallback(buf, buf->queue(), aq);
    CHECK(aq->size == 11000 && api.enqueued == 2 && static_cast<std::uint8_t*>(aq->data)[0] == std::uint8_t(11000));
    std::uint32_t play = 0, write = 0;
    buf->GetCurrentPosition(&play, &write);
    CHECK(play == 11000);
    AudioBufferCallback(buf, buf->queue(), aq);   // 22000
    AudioBufferCallback(buf, buf->queue(), aq);   // 33000
    CHECK(api.enqueued == 4);
    AudioBufferCallback(buf, buf->queue(), aq);   // 44000 + 11000 >= 44100: short tail of 100 bytes
    CHECK(aq->size == 100 && api.enqueued == 5);
    AudioBufferCallback(buf, buf->queue(), aq);   // position wrapped to 0 without looping: finished
    CHECK(!buf->playing() && api.enqueued == 5);
    CHECK(buf->Stop() == 0 && !buf->queue());

    // looping wraps back to the start
    CHECK(buf->SetCurrentPosition(0) == 0);
    CHECK(buf->Play(0, 0, 1) == 0 && buf->looping() && buf->playing());
    for (int i = 0; i < 4; ++i) AudioBufferCallback(buf, buf->queue(), &api.bufs.back());
    const int before = api.enqueued;
    AudioBufferCallback(buf, buf->queue(), &api.bufs.back());   // tail chunk, sets the wrap flag
    AudioBufferCallback(buf, buf->queue(), &api.bufs.back());   // restarts from 0
    CHECK(api.enqueued == before + 2 && buf->playing());
    CHECK(buf->GetStatus(&status) == 0 && status == 5);
    buf->Stop();

    // volume and pan
    CHECK(buf->Play(0, 0, 0) == 0);
    api.params.clear();
    CHECK(buf->SetVolume(-300) == 0);
    CHECK(api.params.size() == 2 && api.params[0].first == 0xd && api.params[1].first == 1 && Near(api.params[1].second, std::pow(0.8912509f, 3.0f)));
    api.params.clear();
    buf->SetVolume(-10000);
    CHECK(api.params[1].second == 0.0f);
    api.params.clear();
    buf->SetVolume(-50);
    CHECK(api.params[1].second == 1.0f);
    api.params.clear();
    CHECK(buf->SetPan(5000) == 0 && Near(api.params[0].second, 0.5f) && Near(buf->leftMultiplier(), 0.5f) && buf->rightMultiplier() == 1.0f);
    CHECK(buf->SetPan(5000) == 0 && api.params.size() == 2);   // unchanged: nothing re-applied
    buf->SetPan(-2500);
    CHECK(Near(buf->rightMultiplier(), 0.75f) && buf->leftMultiplier() == 1.0f);
    std::int32_t v = 0, pan = 0; std::uint32_t freq = 0;
    CHECK(buf->GetVolume(&v) == 0 && v == -50 && buf->GetPan(&pan) == 0 && pan == -2500 && buf->GetFrequency(&freq) == 0 && freq == 22050);
    buf->Stop();

    // 3D: positions turn into pan / distance gain through the listener
    void* raw = nullptr;
    CHECK(buf->QueryInterface(IID_IDirectSound3DBuffer, &raw) == 0 && raw && buf->MacGet3DBuffer() == raw);
    auto* b3d = static_cast<IDirectSound3DBuffer_Mac*>(raw);
    void* lraw = nullptr;
    CHECK(buf->QueryInterface(IID_IDirectSound3DListener, &lraw) == 0 && lraw);
    auto* listener = static_cast<IDirectSound3DListener_Mac*>(lraw);
    _DS3DBUFFER bp{}; bp.size = 0x40;
    CHECK(b3d->GetAllParameters(&bp) == 0 && bp.minDistance == 1.0f && bp.maxDistance == 1.0e9f && bp.mode == 0);
    CHECK(listener->SetPosition(0, 0, 0, 0) == 0 && listener->SetOrientation(0, 0, 1, 0, 1, 0, 0) == 0);
    CHECK(b3d->SetPosition(0, 0, 0, 0) == 0 && buf->leftMultiplier() == 1.0f && buf->rightMultiplier() == 1.0f && buf->distanceMultiplier() == 1.0f);
    CHECK(b3d->SetPosition(0, 0, 5, 0) == 0 && buf->leftMultiplier() == 1.0f && buf->rightMultiplier() == 1.0f && Near(buf->distanceMultiplier(), 0.2f));
    CHECK(b3d->SetPosition(0, 0, -5, 0) == 0 && buf->leftMultiplier() == 0.3f && buf->rightMultiplier() == 0.3f);
    CHECK(b3d->SetPosition(4, 0, 0, 0) == 0 && buf->leftMultiplier() == 0.4f && buf->rightMultiplier() == 1.0f);
    CHECK(b3d->SetPosition(-4, 0, 0, 0) == 0 && buf->leftMultiplier() == 1.0f && buf->rightMultiplier() == 0.4f);
    CHECK(b3d->SetMinDistance(10, 0) == 0 && b3d->SetMaxDistance(100, 0) == 0);
    CHECK(b3d->SetPosition(0, 0, 50, 0) == 0 && Near(buf->distanceMultiplier(), 0.2f));
    CHECK(b3d->SetPosition(0, 0, 500, 0) == 0 && Near(buf->distanceMultiplier(), 0.1f));   // beyond max: clamped
    CHECK(b3d->SetPosition(1, 0, 1, 1) == 0 && buf->distanceMultiplier() > 0.0f);   // deferred: nothing applied yet
    float gl = 0, gr = 0;
    CHECK(listener->CommitDeferredSettings() == 0);
    gl = buf->leftMultiplier(); gr = buf->rightMultiplier();
    CHECK(gl >= 0.4f && gl <= 1.0f && gr >= 0.4f && gr <= 1.0f);
    CHECK(b3d->SetPosition(1, 2, 3, 2) == DSERR_INVALIDPARAM);
    _D3DVECTOR vec{};
    CHECK(b3d->GetPosition(&vec) == 0 && vec.x == 1 && vec.z == 1 && b3d->GetPosition(nullptr) == DSERR_INVALIDPARAM);
    CHECK(b3d->SetVelocity(1, 2, 3, 1) == 0 && b3d->GetVelocity(&vec) == 0 && vec.y == 2);
    CHECK(b3d->SetConeAngles(10, 20, 1) == 0 && b3d->SetConeOutsideVolume(-500, 1) == 0 && b3d->SetConeOrientation(0, 1, 0, 1) == 0 && b3d->SetMode(2, 1) == 0);
    std::uint32_t a = 0, b = 0, mode = 0; std::int32_t cv = 0;
    CHECK(b3d->GetConeAngles(&a, &b) == 0 && a == 10 && b == 20 && b3d->GetConeOutsideVolume(&cv) == 0 && cv == -500 && b3d->GetMode(&mode) == 0 && mode == 2);
    CHECK(b3d->GetConeOrientation(&vec) == 0 && vec.y == 1);
    gAmbientVolumeLimit = 0.5f;
    // listener accessors
    float f = 0;
    CHECK(listener->SetDistanceFactor(2.5f, 1) == 0 && listener->GetDistanceFactor(&f) == 0 && f == 2.5f);
    CHECK(listener->SetDopplerFactor(1.5f, 1) == 0 && listener->GetDopplerFactor(&f) == 0 && f == 1.5f);
    CHECK(listener->SetRolloffFactor(0.5f, 1) == 0 && listener->GetRolloffFactor(&f) == 0 && f == 0.5f);
    CHECK(listener->SetVelocity(1, 2, 3, 1) == 0 && listener->GetVelocity(&vec) == 0 && vec.z == 3);
    _D3DVECTOR front{}, top{};
    CHECK(listener->GetOrientation(&front, &top) == 0 && front.z == 1 && top.y == 1 && listener->GetOrientation(&front, nullptr) == DSERR_INVALIDPARAM);
    _DS3DLISTENER lp{}; lp.size = 0x40;
    CHECK(listener->GetAllParameters(&lp) == 0 && lp.distanceFactor == 2.5f && listener->SetDistanceFactor(1, 7) == DSERR_INVALIDPARAM);
    lp.position = {3, 4, 5};
    CHECK(listener->SetAllParameters(&lp, 1) == 0 && listener->GetPosition(&vec) == 0 && vec.z == 5);
    lp.size = 1;
    CHECK(listener->SetAllParameters(&lp, 1) == DSERR_INVALIDPARAM);

    // duplicate: only static buffers, copies the data, registers nothing, takes an extra reference on the source
    IDirectSoundBuffer_Mac* dup = nullptr;
    const std::int32_t refsBefore = 0; (void)refsBefore;
    CHECK(ds->DuplicateSoundBuffer(buf, &dup) == 0 && dup && dup != buf && dup->size() == 44100 && dup->kind() == IDirectSoundBuffer_Mac::kStatic);
    void* dp = nullptr; std::uint32_t ds1 = 0;
    CHECK(dup->Lock(0, 0, &dp, &ds1, nullptr, nullptr, 2) == 0 && static_cast<std::uint8_t*>(dp)[300] == std::uint8_t(300));
    CHECK(ds->DuplicateSoundBuffer(nullptr, &dup) == DSERR_INVALIDPARAM);
    // caps count the queues in use
    api.params.clear();
    CHECK(ds->GetCaps(&caps) == 0 && caps.freeHwMixingAllBuffers == 29);   // each Play after a Stop takes a fresh queue: three plays so far

    // the bank: 32 queues at most
    api.failNew = 0;
    std::vector<IDirectSoundBuffer_Mac*> many;
    int played = 0;
    for (int i = 0; i < 40; ++i) {
        IDirectSoundBuffer_Mac* extra = nullptr;
        if (ds->CreateSoundBuffer(&desc, &extra, nullptr) != 0) break;
        many.push_back(extra);
        if (extra->Play(0, 0, 0) == 0) ++played;
        else CHECK(extra->Play(0, 0, 0) == DSERR_NOQUEUE);
    }
    CHECK(played == 29);   // 32 queues minus the three used above; Stop never gives one back
    // primary buffer: no samples, no queue
    _DSBUFFERDESC primary{0x14, 1, 0, 0, nullptr};
    IDirectSoundBuffer_Mac* prim = nullptr;
    for (auto* m : many) m->Release();
    many.clear();
    CHECK(ds->CreateSoundBuffer(&primary, &prim, nullptr) == 0 && prim->kind() == IDirectSoundBuffer_Mac::kPrimary && prim->Play(0, 0, 0) == 0 && !prim->queue());
    prim->Release();

    // GetXZAngle
    CHECK(GetXZAngle({0, 0, 1}) == 0.0f && GetXZAngle({1, 0, 0}) == 0.0f);
    CHECK(Near(GetXZAngle({1, 0, 1}), 0.7853982f + 3.1415927f) && Near(GetXZAngle({-1, 0, -1}), 0.7853982f) && Near(GetXZAngle({-1, 0, 1}), std::atan(-1.0f) + 6.2831855f));

    dup->Release();
    buf->Release();
    ds->Release();
    SetAudioQueueApi(nullptr);
    std::printf(failures ? "FAILED\n" : "audio_directsoundmac ok\n");
    return failures != 0;
}
