#include <cassert>
#include <cstdio>
#include <vector>

#include "audio/AudioServices.h"
#include "audio/GZSndMgr.h"
#include "rz/cRZString.h"
#include "rz/nRZHash.h"
#include "sc4audio_fakes.h"

namespace {

struct Sys final : fakes::FakeSndSys {
    std::uint32_t renderers = 1;
    std::uint32_t intervalRequested = 0;
    std::uint32_t NumRenderers() override { return renderers; }
    bool RegisterTarget(cIGZSndUpdateTarget* t, SndCallbackInterval interval) override {
        intervalRequested = interval;
        return FakeSndSys::RegisterTarget(t, interval);
    }
};

struct Creator final : cIGZSoundCreator {
    int calls = 0;
    int refs = 0;
    fakes::FakeSnd* made = nullptr;
    std::uint32_t acceptId = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZSoundCreator) return false;
        *out = static_cast<cIGZSoundCreator*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool CreateSound(std::uint32_t id, std::uint32_t, cIGZVariant*, std::uint32_t iid, void** out) override {
        ++calls;
        if (id != acceptId) return false;
        delete made;   // a later call replaces the previous sound
        made = new fakes::FakeSnd();
        made->refs = 1;
        return made->QueryInterface(iid, out);
    }
    ~Creator() { delete made; }
};

struct DsCreator final : cIGZSndDataSourceCreator {
    int calls = 0, refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZSndDataSourceCreator) return false;
        *out = static_cast<cIGZSndDataSourceCreator*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool CreateSndDataSource(std::uint32_t id, std::uint32_t, cIGZVariant*, std::uint32_t, void**) override { ++calls; return id == 5; }
    bool AddSndDataSourceDef(std::uint32_t, cIGZString&) override { return true; }
    void RemoveSndDataSourceDef(std::uint32_t) override {}
};

struct Persist final : cIGZPersistRecordService {
    int getResources = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool CreateResource(const cGZPersistResourceKey&, std::uint32_t, void**, std::uint32_t, std::uint32_t) override { return false; }
    bool RegisterResource(cIGZUnknown*) override { return false; }
    bool RemoveResource(const cGZPersistResourceKey&) override { return false; }
    bool HasResource(const cGZPersistResourceKey&) override { return false; }
    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t, void**, std::uint32_t, std::uint32_t) override {
        ++getResources;
        return key.type == 7;
    }
    bool FindRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord**) override { return false; }
    bool OpenRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord**, std::uint32_t) override { return false; }
};

}  // namespace

int main() {
    Sys sys;
    Persist persist;
    Audio::SetSoundSystem(&sys);
    Audio::SetPersistService(&persist);

    cGZSndMgr* mgr = cGZSndMgr::Create();
    mgr->AddRef();
    assert(mgr->GetServiceID() == 0x1af85bfb && mgr->GetServicePriority() == 0x7a11e);
    assert(mgr->GetSystemSndVol() == 0x400);

    // QI
    void* p = nullptr;
    assert(mgr->QueryInterface(0x3af85bb7, &p) && p == static_cast<cIGZSndMgr*>(mgr));
    assert(mgr->QueryInterface(0x287fb697, &p));
    assert(!mgr->QueryInterface(0x1234, &p));
    mgr->Release(); mgr->Release();                 // the two QI references

    // Init registers a 250 ms update target and is idempotent
    assert(mgr->Init() && mgr->initialised());
    assert(sys.targets.size() == 1 && sys.intervalRequested == 0xfa);
    assert(mgr->Init() && sys.targets.size() == 1);
    cGZPersistResourceKey key{7, 0, 0};
    assert(mgr->GetResource(key, 1, nullptr) && persist.getResources == 1);

    // defaults and system sound volume
    std::uint32_t id = 123;
    assert(mgr->SetDefaultSndDataSourcePlayer2DClassId(0xaa) && mgr->SetDefaultSndDataSourcePlayer3DClassId(0xbb));
    assert(mgr->GetDefaultSndDataSourcePlayer2DClassId(id) && id == 0xaa);
    assert(mgr->GetDefaultSndDataSourcePlayer3DClassId(id) && id == 0xbb);
    mgr->SetSystemSndVol(300);
    assert(mgr->GetSystemSndVol() == 300);

    // system sounds
    assert(mgr->SystemSndID(2) == 0);
    assert(mgr->SetSystemSndID(2, 4000) && mgr->SystemSndID(2) == 4000);
    assert(mgr->SetSystemSndID(2, 4001) && mgr->SystemSndID(2) == 4001);

    // registered sounds
    fakes::FakeSnd a, b;
    a.refs = 1; b.refs = 1;
    mgr->RegisterSnd(&a, 10);
    assert(a.refs == 2 && mgr->registeredSoundCount() == 1);
    mgr->RegisterSnd(&b, 10);                       // first registration wins
    assert(b.refs == 1 && mgr->registeredSoundCount() == 1);
    void* out = nullptr;
    assert(mgr->GetSoundById(10, 0, nullptr, 0x206077d2, &out) && out == static_cast<cIGZSnd*>(&a));
    assert(a.refs == 3);
    a.Release();
    // no renderer -> nothing
    sys.renderers = 0;
    assert(!mgr->GetSoundById(10, 0, nullptr, 0x206077d2, &out));
    sys.renderers = 1;

    // creators: newest first; one-shot ids come from the creator
    Creator c1, c2;
    c1.acceptId = 77;
    c2.acceptId = 78;
    assert(mgr->RegisterSndCreator(&c1) && mgr->RegisterSndCreator(&c2) && mgr->soundCreatorCount() == 2);
    assert(mgr->RegisterSndCreator(&c1) && mgr->soundCreatorCount() == 2);    // replaced
    // order after re-registering c1: c1 (newest) then c2
    assert(mgr->GetSoundById(77, 0, nullptr, 0x206077d2, &out) && c1.calls == 1 && c2.calls == 0);
    assert(mgr->GetSoundById(78, 0, nullptr, 0x206077d2, &out) && c1.calls == 2 && c2.calls == 1);
    assert(!mgr->GetSoundById(79, 0, nullptr, 0x206077d2, &out));
    assert(mgr->RemoveSndCreator(&c1) && !mgr->RemoveSndCreator(&c1) && mgr->soundCreatorCount() == 1);
    // name lookup: CRC24(name) | 0xff000000
    cRZString name("boom");
    c2.acceptId = nRZHash::CRC24("boom", false) | 0xff000000u;
    assert(mgr->GetSoundByName(name, 0, nullptr, 0x206077d2, &out));

    // autoplay
    a.playing = false;
    assert(mgr->SndAutoPlayWithVolume(10, 512));
    assert(a.plays == 1 && a.autoVolume == 512 && mgr->autoPlayCount() == 1);
    // update retires the front entry once it stopped playing
    a.playing = true;
    mgr->Update(250);
    assert(mgr->autoPlayCount() == 1);
    a.playing = false;
    mgr->Update(250);
    assert(mgr->autoPlayCount() == 0);
    // system autoplay maps the index; unmapped plays id 0 (not found -> false)
    mgr->RegisterSnd(&b, 4001);
    assert(mgr->SystemSndAutoPlay(2) && b.plays == 1);
    assert(!mgr->SystemSndAutoPlay(99));
    assert(mgr->SndAutoPlay(10) && a.plays >= 2);

    // unregister
    mgr->UnregisterSndByPointer(&a);
    assert(mgr->registeredSoundCount() == 1);       // only 4001 left
    mgr->UnregisterSndById(4001);
    assert(mgr->registeredSoundCount() == 0);
    a.playing = false;
    mgr->Update(0);

    // data source creators / data sources
    DsCreator d1, d2;
    assert(mgr->RegisterSndDataSourceCreator(&d1, 1) && mgr->RegisterSndDataSourceCreator(&d2, 2));
    assert(mgr->dataSourceCreatorCount() == 2);
    assert(mgr->RegisterSndDataSourceCreator(&d2, 1) && mgr->dataSourceCreatorCount() == 2);   // replaced under id 1
    void* got = nullptr;
    assert(mgr->GetSndDataSourceCreator(2, kGZIID_cIGZSndDataSourceCreator, &got) && got == static_cast<cIGZSndDataSourceCreator*>(&d2));
    assert(!mgr->GetSndDataSourceCreator(9, 1, &got));
    assert(mgr->GetSndDataSource(5, 0, nullptr, 1, &got));       // asks the creators
    assert(!mgr->GetSndDataSource(6, 0, nullptr, 1, &got));
    assert(mgr->RemoveSndDataSourceCreatorById(2) && !mgr->RemoveSndDataSourceCreatorById(2));
    assert(mgr->RemoveSndDataSourceCreatorByPointer(&d2) && mgr->dataSourceCreatorCount() == 0);
    assert(!mgr->RemoveSndDataSourceCreatorByPointer(&d2));

    // shutdown keeps data sources
    struct Ds final : cIGZSndDataSource {
        int refs = 1;
        bool QueryInterface(std::uint32_t iid, void** out) override { if (iid != 0x3afd825f) return false; *out = this; ++refs; return true; }
        std::uint32_t AddRef() override { return ++refs; }
        std::uint32_t Release() override { return --refs; }
        bool SetPath(const char*) override { return false; }
        bool GetPath(cIGZString&) override { return false; }
        bool SetKey(const cGZPersistResourceKey&) override { return true; }
        bool GetKey(cGZPersistResourceKey&) override { return true; }
        bool Open() override { return true; }
        bool Close() override { return true; }
        bool IsOpen() const override { return false; }
        std::uint32_t GetDataSize() const override { return 0; }
        bool Lock(std::uint8_t**, std::uint32_t*) override { return false; }
        bool Unlock() override { return true; }
        bool IsLocked() const override { return false; }
        std::int32_t ReadPart(std::uint8_t*, std::int32_t) override { return 0; }
        void ReadReset() override {}
        const GZSndWaveFormat* GetWaveFormat() const override { return nullptr; }
        bool DataIsValid() override { return true; }
        bool SetDataIsValid(bool) override { return true; }
        std::uint32_t GetPosition() const override { return 0; }
        bool SetPosition(std::uint32_t) override { return true; }
    } ds;
    mgr->RegisterSndDataSource(&ds, 40);
    mgr->RegisterSndDataSource(&ds, 40);
    assert(mgr->dataSourceCount() == 1 && ds.refs == 2);
    assert(mgr->GetSndDataSource(40, 0, nullptr, 0x3afd825f, &got) && got == &ds);
    ds.Release();
    mgr->RegisterSnd(&a, 1);
    mgr->AddSndToAutoPlayList(&a);
    assert(mgr->Shutdown());
    assert(mgr->registeredSoundCount() == 0 && mgr->autoPlayCount() == 0 && mgr->dataSourceCount() == 1);
    mgr->UnregisterSndDataSourceByPointer(&ds);
    assert(mgr->dataSourceCount() == 0 && ds.refs == 1);
    mgr->RegisterSndDataSource(&ds, 41);
    mgr->UnregisterSndDataSourceById(41);
    assert(mgr->dataSourceCount() == 0 && ds.refs == 1);

    // re-init works after shutdown
    assert(mgr->Init() && sys.targets.size() == 2);
    assert(mgr->Release() == 0);                    // count 1: shuts down and destroys
    Audio::SetSoundSystem(nullptr);
    Audio::SetPersistService(nullptr);
    std::puts("gzsndmgr ok");
}
