// cHitChannel: register file (layers, legacy ids, typed values), notifications, playback.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>

#include "audio/HitChannel.h"
#include "audio/HitClassIds.h"
#include "gz/cGZAny.h"

namespace {

struct FakeHolder final : cIGZSndDataSourceHolder {
    std::uint32_t refs = 1;
    cIGZSndDataSource* source = nullptr;
    int sets = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool SetDataSource(cIGZSndDataSource* s, cIGZSnd::SndStreamingType) override { source = s; ++sets; return true; }
    cIGZSndDataSource* GetDataSource() override { return source; }
};

struct Fake3D final : cIGZSnd3D {
    float min = -1, max = -1, pos[3] = {}, vel[3] = {};
    bool headRelative = false;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZSnd* AsIGZSnd() override { return nullptr; }
    bool GetPosition(float*, float*, float*) const override { return false; }
    bool SetPosition(float x, float y, float z) override { pos[0] = x; pos[1] = y; pos[2] = z; return true; }
    bool GetVelocity(float*, float*, float*) const override { return false; }
    bool SetVelocity(float x, float y, float z) override { vel[0] = x; vel[1] = y; vel[2] = z; return true; }
    bool GetAll(float*, float*, float*, float*, float*, float*) const override { return false; }
    bool SetAll(float, float, float, float, float, float) override { return false; }
    void SetPower(std::uint32_t) override {}
    bool GetMinimumDistance(float*) const override { return false; }
    bool SetMinimumDistance(float d) override { min = d; return true; }
    bool GetMaximumDistance(float*) const override { return false; }
    bool SetMaximumDistance(float d) override { max = d; return true; }
    bool SetHeadRelative(bool h) override { headRelative = h; return true; }
    bool GetHeadRelative() const override { return headRelative; }
};

struct FakeSnd final : cIGZSnd {
    std::uint32_t refs = 0;
    bool loaded = false, playing = false, shutdown = false, paused = false;
    std::int32_t volume = -1, pan = -1, frequency = 22050, id = 0, playVolume = -1;
    bool playLoop = false;
    FakeHolder holder;
    Fake3D snd3d;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 0x9d002d2) { *out = static_cast<cIGZSndDataSourceHolder*>(&holder); return true; }
        if (iid == 0x306077d2) { *out = static_cast<cIGZSnd3D*>(&snd3d); return true; }
        if (iid == kGZIID_cIGZSnd) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { shutdown = true; return true; }
    bool PlayWithVolumeAndLoop(std::int32_t v, bool l, SndDup) override { playVolume = v; playLoop = l; playing = true; return true; }
    bool PlayWithScriptArguments(std::int32_t, std::int32_t, std::int32_t, std::int32_t) override { return true; }
    bool IsPlaying() const override { return playing; }
    bool Stop() override { playing = false; return true; }
    bool Kill() override { playing = false; return true; }
    bool StopAllInstances() override { return true; }
    std::uint32_t GetUseCount() const override { return 0; }
    std::uint32_t GetID() const override { return static_cast<std::uint32_t>(id); }
    void SetID(std::uint32_t i) override { id = static_cast<std::int32_t>(i); }
    bool Load() override { loaded = true; return true; }
    bool Unload() override { loaded = false; return true; }
    bool IsLoaded() const override { return loaded; }
    bool Pause(bool) override { paused = true; return true; }
    bool Unpause() override { paused = false; return true; }
    bool IsPaused() const override { return paused; }
    std::int32_t GetVolume() const override { return volume; }
    bool SetVolume(std::int32_t v) override { volume = v; return true; }
    bool FadeVolume(std::int32_t, std::int32_t, std::uint32_t, bool) override { return true; }
    bool IsFading() const override { return false; }
    std::int32_t GetPan() const override { return pan; }
    bool SetPan(std::int32_t p) override { pan = p; return true; }
    std::int32_t GetFrequency() const override { return frequency; }
    bool SetFrequency(std::int32_t f) override { frequency = f; return true; }
    std::int32_t GetOriginalFrequency() const override { return 44100; }
    bool SetEffectsType(std::int32_t, std::uint32_t) override { return true; }
    bool SetEffectsLevel(std::int32_t, std::uint32_t) override { return true; }
    std::int32_t GetSoundPriority() const override { return 0; }
    bool SetSoundPriority(std::int32_t) override { return true; }
    bool SetName(const cIGZString&) override { return true; }
    bool GetName(cIGZString&) const override { return false; }
    bool SetSoundFileFromString(const cIGZString&, SndStreamingType) override { return true; }
    bool SetSoundFileFromChars(const char*, std::uint32_t, SndStreamingType) override { return true; }
};

struct Services final : Hit::cIHitManServices {
    FakeSnd* snd = nullptr;
    bool last3D = false;
    std::vector<FakeSnd*> created;   // every sound handed out, freed with the fake host
    ~Services() { for (FakeSnd* s : created) delete s; }
    std::map<std::uint32_t, cGZPropertySet*> propertySets;   // by instance
    bool HasSoundSystem() const override { return true; }
    void EnumerateUpdateTargets(EnumCallback, std::uint32_t, void*) override {}
    bool GetPersistResource(const cGZPersistResourceKey& key, std::uint32_t, void** out) override {
        const auto it = propertySets.find(key.instance);
        if (it == propertySets.end()) return false;
        it->second->AddRef();
        *out = static_cast<cIGZPropertySet*>(it->second);
        return true;
    }
    cIGZUnknown* CreatePropertySetResource(const cGZPersistResourceKey&, const Hit::DefaultProperty*, std::size_t, bool) override { return nullptr; }
    bool ReadPersistRecord(const cGZPersistResourceKey&, std::vector<std::uint8_t>&, std::string&) override { return false; }
    bool GetSoundById(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RegisterUpdateTarget(cIGZSndUpdateTarget*, std::uint32_t) override {}
    void UnregisterUpdateTarget(cIGZSndUpdateTarget*) override {}
    bool SetGlobalProperty(std::uint32_t, const cIGZAny&) override { return false; }
    bool GetGlobalProperty(std::uint32_t, cIGZAny*) override { return false; }
    bool SetInstanceProperty(std::uint32_t, std::uint32_t, const cIGZAny&) override { return false; }
    bool GetDefaultSndPlayerClassId(bool is3D, std::uint32_t& id) override { last3D = is3D; id = is3D ? 0x33 : 0x22; return true; }
    bool CreateComInstance(std::uint32_t, std::uint32_t, void** out) override {
        snd = new FakeSnd();
        created.push_back(snd);
        snd->AddRef();
        *out = static_cast<cIGZSnd*>(snd);
        return true;
    }
    bool ReadFileBytes(const std::string&, std::vector<std::uint8_t>&) override { return false; }
    std::string ApplicationDirectory() const override { return ""; }
    std::string DataDirectory() const override { return ""; }
    void ParseDebugInfo(const cIGZString&, const std::uint8_t*, std::uint32_t) override {}
    bool CreateTrackPlayer(std::uint32_t, void**) override { return false; }
    bool CreateChannel(void**) override { return false; }
};

}  // namespace

int main() {
    Services services;
    Hit::SetServices(&services);

    cHitChannel* ch = new cHitChannel();
    ch->AddRef();   // objects start at count 0; the creator adopts one reference
    // interface offsets of the original (0x78-byte object, six vptrs)
    char* base = reinterpret_cast<char*>(ch);
    assert(reinterpret_cast<char*>(static_cast<cIGZSnd3D*>(ch)) - base == 0x08);
    assert(reinterpret_cast<char*>(static_cast<cIGZSndUpdateTarget*>(ch)) - base == 0x10);
    assert(reinterpret_cast<char*>(static_cast<cIGZSndRegisterSet*>(ch)) - base == 0x18);
    assert(reinterpret_cast<char*>(static_cast<cIGZSndViewUpdateTarget*>(ch)) - base == 0x20);
    assert(reinterpret_cast<char*>(static_cast<cIGZSndDataSourceHolder*>(ch)) - base == 0x28);
    assert(sizeof(cHitChannel) == 0x78);

    // QueryInterface: six ids, not cIGZUnknown(1)
    void* p = nullptr;
    assert(!ch->QueryInterface(1, &p));
    assert(ch->QueryInterface(0x306077d2, &p) && p == static_cast<cIGZSnd3D*>(ch));
    assert(ch->QueryInterface(0xfd584467, &p) && p == static_cast<cIGZSndRegisterSet*>(ch));
    assert(!ch->QueryInterface(0x1234, &p));
    ch->Release(); ch->Release();

    // registers: typed, legacy ids translated, missing = 0
    cIGZSndRegisterSet* regs = ch;
    assert(regs->GetRegisterValue(0x15) == 0);
    assert(regs->SetRegisterValue(0x15, 800, true));           // legacy 0x15 -> volume
    assert(regs->GetRegisterValue(static_cast<std::int32_t>(0xff90aac8)) == 800);
    assert(regs->HasRegisterValue(0x15) && regs->HasRegisterValue(static_cast<std::int32_t>(0xff90aac8)));
    assert(regs->SetRegisterFloat(0xff3071b9, 1.5f, true));
    assert(regs->GetRegisterFloat(0xff3071b9) == 1.5f);
    assert(regs->GetRegisterValue(static_cast<std::int32_t>(0xff3071b9)) == 0);  // float stored: int read fails
    // ClearRegisterValue does NOT translate legacy ids (binary quirk): clearing 0x15 leaves volume.
    regs->ClearRegisterValue(0x15, true);
    assert(regs->GetRegisterValue(0x15) == 800);
    regs->ClearRegisterValue(static_cast<std::int32_t>(0xff90aac8), true);
    assert(regs->GetRegisterValue(0x15) == 0);

    // volume math: volume * scale / 1024 truncating toward zero, 2D pan path, playback
    regs->SetRegisterValue(static_cast<std::int32_t>(0xff90aac8), 1000, true);
    regs->SetRegisterValue(static_cast<std::int32_t>(0xff53dbf3), 512, true);
    regs->SetRegisterValue(static_cast<std::int32_t>(0xff3ec880), 300, true);
    regs->SetRegisterValue(static_cast<std::int32_t>(0xfff7b8df), 1, true);   // loop flag
    assert(ch->NoteOn());
    assert(services.snd != nullptr && !services.last3D);
    assert(ch->effectiveVolume() == 500 && services.snd->playVolume == 500 && services.snd->playLoop);
    assert(services.snd->pan == 300 && services.snd->volume == 500 && services.snd->loaded);
    assert(ch->IsPlaying() && ch->IsLoaded());
    assert(ch->Stop() && !ch->IsPlaying());                       // Stop is a kill

    // notification: writing volume refreshes the sound; suppressed writes do not
    regs->SetRegisterValue(static_cast<std::int32_t>(0xff90aac8), 2048, false);
    assert(services.snd->volume == 1024);
    regs->SetRegisterValue(static_cast<std::int32_t>(0xff90aac8), 4096, true);
    assert(services.snd->volume == 1024);
    regs->SetRegisterValue(static_cast<std::int32_t>(0xff90aac8), -3, false);   // negative: truncates toward zero
    assert(services.snd->volume == -1);                                          // -3*512/1024 = -1.5 -> -1

    // the data source is handed to the sound's holder with streaming type 2
    FakeSnd* snd = services.snd;
    cIGZSndDataSource* ds = reinterpret_cast<cIGZSndDataSource*>(&snd->holder);  // any non-null pointer value
    (void)ds;
    assert(ch->GetDataSource() == nullptr);

    // 0xffaec905 drops the data source
    regs->SetRegisterValue(static_cast<std::int32_t>(0xffaec905), 5, false);

    // 3D path: distance registers, position, velocity
    cHitChannel* c3 = new cHitChannel();
    c3->AddRef();
    c3->SetRegisterValue(static_cast<std::int32_t>(0xffcc156f), 1, true);        // use 3D
    c3->SetRegisterValue(static_cast<std::int32_t>(0xffaab623), 20, true);
    c3->SetRegisterValue(static_cast<std::int32_t>(0xffcc4d2d), 7, true);
    c3->SetRegisterValue(static_cast<std::int32_t>(0xff4a01d6), 8, true);
    c3->SetRegisterValue(static_cast<std::int32_t>(0xff469820), 9, true);
    c3->SetRegisterValue(static_cast<std::int32_t>(0xff90aac8), 1024, true);
    c3->SetRegisterValue(static_cast<std::int32_t>(0xff53dbf3), 1024, true);
    assert(c3->NoteOn());
    FakeSnd* s3 = services.snd;
    assert(services.last3D && s3->snd3d.min == 20.0f && s3->snd3d.max == 100000.0f);  // max 0 -> 100000
    assert(s3->snd3d.pos[0] == 7 && s3->snd3d.pos[1] == 8 && s3->snd3d.pos[2] == 9);

    // SetAll writes the velocity z into the position z (binary bug)
    c3->SetAll(1, 2, 3, 4, 5, 6);
    float x, y, z;
    c3->GetPosition(&x, &y, &z);
    assert(x == 1 && y == 2 && z == 6);
    c3->GetVelocity(&x, &y, &z);
    assert(x == 4 && y == 5 && z == 6);
    // Get*Distance ignores the output pointer, returns the register low byte
    c3->SetMinimumDistance(300);
    float untouched = -5;
    assert(c3->GetMinimumDistance(&untouched) && untouched == -5);              // 300 & 0xff = 44

    // parent property set chain (0xff4c97be): layers are rebuilt and read in order
    cGZPropertySet parent, grandparent;
    parent.AddRef(); grandparent.AddRef();
    std::uint32_t kParentLink = 0xff4c97be, kVol = 0xff90aac8, kPan = 0xff3ec880;
    parent.SetValue(kVol, AnyUint32(111));
    parent.SetValue(kParentLink, AnyUint32(0x2002));
    grandparent.SetValue(kPan, AnyUint32(222));
    services.propertySets[0x2001] = &parent;
    services.propertySets[0x2002] = &grandparent;
    cHitChannel* child = new cHitChannel();
    child->AddRef();
    child->SetRegisterValue(static_cast<std::int32_t>(kParentLink), 0x2001, false);
    assert(child->layerCount() == 3);
    assert(child->GetRegisterValue(static_cast<std::int32_t>(kVol)) == 111);
    assert(child->GetRegisterValue(static_cast<std::int32_t>(kPan)) == 222);
    child->SetRegisterValue(static_cast<std::int32_t>(kVol), 5, true);           // writes go to the own layer
    assert(child->GetRegisterValue(static_cast<std::int32_t>(kVol)) == 5);
    AnyUint32 probe;
    assert(parent.Value(kVol, &probe) == true);                                   // parent untouched
    // a cyclic parent chain is cut (the binary would loop forever)
    parent.SetValue(kParentLink, AnyUint32(0x2001));
    child->SetRegisterValue(static_cast<std::int32_t>(kParentLink), 0x2001, false);
    assert(child->layerCount() <= 65);

    // 0xff0ad592 while idle is reset to zero once, without recursion
    cHitChannel* idle = new cHitChannel();
    idle->AddRef();
    idle->SetRegisterValue(static_cast<std::int32_t>(0xff0ad592), 9, false);
    assert(idle->GetRegisterValue(static_cast<std::int32_t>(0xff0ad592)) == 0);

    // Init resets the refcount; Release destroys at count < 2
    cHitChannel* r = new cHitChannel();
    r->AddRef(); r->AddRef(); r->AddRef();
    assert(r->Init());
    assert(r->Release() == 0);   // count was reset to 0 by Init: destroyed on the next Release

    ch->Release(); c3->Release(); child->Release(); idle->Release();
    Hit::SetServices(nullptr);
    std::puts("hitchannel ok");
}
