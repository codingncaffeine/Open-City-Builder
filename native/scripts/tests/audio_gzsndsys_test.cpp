// cGZSndSys, cGZSndSysMac, caches and the message queue
#include <SDL.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <tuple>
#include <vector>

#include "audio/AudioServices.h"
#include "audio/GZSndCaches.h"
#include "audio/GZSndDefaultEventHandler.h"
#include "audio/GZSndSys.h"
#include "audio/GZSndSysMac.h"
#include "audio/interfaces/cIGZSnd.h"
#include "gz/cGZAny.h"
#include "gz/cGZMessageQueue2.h"
#include "gz/cGZPropertySet.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4audio_fakes.h"

namespace {

// A renderer that records everything and exposes all the optional interfaces.
struct Renderer final : cIGZSndRenderer, cIGZSnd3DListener, cIGZSnd3DListenerEx, cIGZSndEaRendererEffects {
    int refs = 1;
    std::vector<std::int32_t> masterVolumes;
    float position[3] = {};
    float orientation[6] = {};
    float velocity[3] = {};
    float doppler = -1, rolloff = -1, distance = -1;
    int commits = 0;
    int busInits = 0;
    float busLevel = -1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        void* p = nullptr;
        if (iid == 1 || iid == kGZIID_cIGZSndRenderer) p = static_cast<cIGZSndRenderer*>(this);
        else if (iid == 0x406077d2) p = static_cast<cIGZSnd3DListener*>(this);
        else if (iid == 0x5b30e7b3) p = static_cast<cIGZSnd3DListenerEx*>(this);
        else if (iid == kGZIID_cIGZSndEaRendererEffects) p = static_cast<cIGZSndEaRendererEffects*>(this);
        else return false;
        *out = p;
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    // cIGZSndRenderer
    bool SetMasterVolume(std::int32_t v) override { masterVolumes.push_back(v); return true; }
    std::int32_t GetMasterVolume() override { return 0; }
    bool Pause(bool) override { return true; }
    bool Unpause() override { return true; }
    bool IsPaused() const override { return false; }
    // listener
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool CommitDeferredSettings() override { ++commits; return true; }
    bool GetPosition(float*, float*, float*) const override { return false; }
    bool SetPosition(float x, float y, float z) override { position[0] = x; position[1] = y; position[2] = z; return true; }
    bool GetOrientation(float*, float*, float*, float*, float*, float*) const override { return false; }
    bool SetOrientation(float a, float b, float c, float d, float e, float f) override {
        orientation[0] = a; orientation[1] = b; orientation[2] = c; orientation[3] = d; orientation[4] = e; orientation[5] = f;
        return true;
    }
    cIGZSnd3DListenerEx* AsIGZSnd3DListenerEx() override { return this; }
    bool GetVelocity(float*, float*, float*) const override { return false; }
    bool SetVelocity(float x, float y, float z) override { velocity[0] = x; velocity[1] = y; velocity[2] = z; return true; }
    bool GetAll(float*, float*, float*, float*, float*, float*, float*, float*, float*, float*, float*, float*) const override { return false; }
    bool SetAll(float, float, float, float, float, float, float, float, float, float, float, float) override { return false; }
    bool GetDopplerFactor(float*) const override { return false; }
    bool SetDopplerFactor(float f) override { doppler = f; return true; }
    bool GetRolloffFactor(float*) const override { return false; }
    bool SetRolloffFactor(float f) override { rolloff = f; return true; }
    bool GetDistanceFactor(float*) const override { return false; }
    bool SetDistanceFactor(float f) override { distance = f; return true; }
    // effects
    bool InitializeEffectsBus(std::int32_t, float level, std::int32_t, std::int32_t, std::int32_t) override { ++busInits; busLevel = level; return true; }
    bool SetEffectMasterLevel(std::int32_t, float level) override { busLevel = level; return false; }
};

struct Target final : cIGZSndUpdateTarget {
    int refs = 0;
    std::vector<std::int32_t> updates;
    std::function<void()> onUpdate;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Update(std::int32_t ms) override {
        updates.push_back(ms);
        if (onUpdate) onUpdate();
    }
};

struct Handler final : cIGZSndEventHandler {
    int refs = 0;
    std::vector<std::uint32_t> seen;
    bool consume = false;
    std::function<void()> onEvent;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoEvent(cIGZSndEventStd* e) override {
        seen.push_back(e->GetEventId());
        if (onEvent) onEvent();
        return consume;
    }
};

// A property-set resource: cGZPropertySet answering the resource iid as well.
struct Resource final : cIGZUnknown {
    cGZPropertySet* set = [] { auto* s = new cGZPropertySet(); s->AddRef(); return s; }();
    int refs = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZPropertySet) { *out = static_cast<cIGZPropertySet*>(set); set->AddRef(); return true; }
        if (iid == 1 || iid == 0x456b8f1d) { *out = this; ++refs; return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

struct Persist final : cIGZPersistRecordService {
    std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, Resource*> resources;
    std::vector<Resource*> made;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    static auto K(const cGZPersistResourceKey& k) { return std::make_tuple(k.type, k.group, k.instance); }
    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t, std::uint32_t) override {
        const auto it = resources.find(K(key));
        if (it == resources.end()) return false;
        return it->second->QueryInterface(iid, out);
    }
    bool CreateResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t, std::uint32_t) override {
        auto* r = new Resource();
        made.push_back(r);
        pending[reinterpret_cast<cIGZUnknown*>(r)] = key;
        return r->QueryInterface(iid, out);
    }
    std::map<cIGZUnknown*, cGZPersistResourceKey> pending;
    bool RegisterResource(cIGZUnknown* resource) override {
        auto* r = static_cast<Resource*>(resource);
        resources[K(pending[reinterpret_cast<cIGZUnknown*>(r)])] = r;
        return true;
    }
    bool RemoveResource(const cGZPersistResourceKey& key) override { return resources.erase(K(key)) != 0; }
    bool HasResource(const cGZPersistResourceKey& key) override { return resources.count(K(key)) != 0; }
    bool FindRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord**) override { return false; }
    bool OpenRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord**, std::uint32_t) override { return false; }
    ~Persist() { for (auto* r : made) { r->set->Release(); delete r; } }
};

void TestMessageQueue() {
    auto* q = new cGZMessageQueue2();
    q->AddRef();
    const auto make = [](std::uint32_t type) {
        auto* m = new cRZMessage2Standard();
        m->AddRef();                 // owned by the test
        m->SetType(type);
        return m;
    };
    cRZMessage2Standard *a = make(1), *b = make(2), *c = make(1);
    assert(q->PushBack(a) && q->PushBack(b) && q->PushFront(c));       // order: c a b
    assert(q->GetMessageCount() == 3);
    assert(q->PeekFrontType() == 1 && q->PeekBackType() == 2 && q->PeekAnywhereType(2) && !q->PeekAnywhereType(9));
    cIGZMessage2* out = nullptr;
    assert(q->PeekFrontMessage(&out) && out == c);
    out->Release();
    assert(q->RemoveAllFoundType(1) == 2 && q->GetMessageCount() == 1);
    q->PushBack(a);
    assert(q->RemoveFirstFoundType(1) && q->PeekFrontType() == 2);
    assert(q->GetFrontMessage(&out) && out == b);
    out->Release();
    assert(!q->GetFrontMessage(&out) && out == nullptr && q->PeekFrontType() == 0);
    q->PushBack(a); q->PushBack(b);
    assert(q->GetBackMessage(&out) && out == b);
    out->Release();
    assert(q->RemoveAll() == 1 && q->GetMessageCount() == 0);
    q->PushBack(a);
    assert(q->GetFrontMessageBlock(&out) && out == a);       // returns at once when queued
    out->Release();
    // IsEqual compares types
    q->PushBack(a); q->PushBack(b); q->PushBack(c);
    assert(q->RemoveAllFound(*c) == 2 && q->GetMessageCount() == 1);
    assert(q->RemoveLastFound(*b) && !q->RemoveLastFound(*b));
    void* p = nullptr;
    assert(q->QueryInterface(0xa52294cd, &p) && !q->QueryInterface(5, &p));
    q->Release();
    assert(q->Release() == 0);
    assert(a->Release() == 0 && b->Release() == 0 && c->Release() == 0);
}

void TestCaches() {
    Persist persist;
    fakes::FakeSndSys sys;
    Audio::SetPersistService(&persist);
    Audio::SetSoundSystem(&sys);
    auto* pc = cGZSndPropertyCache::Create();
    pc->AddRef();
    assert(pc->AddInstance(5) && pc->HasInstance(5) && !pc->HasInstance(6));
    assert(pc->SetInstanceProperty(5, 0x10, AnyUint32(77)));
    AnyUint32 got;
    assert(pc->GetInstanceProperty(5, 0x10, &got) && got.get() == 77);
    assert(pc->HasInstanceProperty(5, 0x10) && !pc->HasInstanceProperty(5, 0x11));
    assert(pc->SetInstancePropertyUint32(5, 0x11, 9) && pc->HasInstanceProperty(5, 0x11));
    assert(pc->RemoveInstanceProperty(5, 0x10) && !pc->HasInstanceProperty(5, 0x10));
    // global properties live in instance 0xcc3014ee and announce changes
    assert(!pc->SetGlobalProperty(1, AnyUint32(3)));          // no global instance yet
    assert(pc->AddInstance(0xcc3014ee));
    sys.posted.clear();
    assert(pc->SetGlobalProperty(1, AnyUint32(3)) && pc->HasGlobalProperty(1));
    assert(pc->GetGlobalProperty(1, &got) && got.get() == 3);
    assert(pc->SetGlobalPropertyUint32(2, 8) && pc->HasGlobalProperty(2));
    assert(pc->RemoveGlobalProperty(1) && !pc->HasGlobalProperty(1));
    assert(pc->RemoveInstance(5) && !pc->HasInstance(5));
    void* p = nullptr;
    assert(pc->QueryInterface(0x5d4c6b90, &p) && pc->QueryInterface(1, &p) && !pc->QueryInterface(7, &p));
    pc->Release(); pc->Release();
    assert(pc->Release() == 0);                               // decrement-then-destroy

    auto* ic = cGZSndInstanceCache::Create();
    ic->AddRef();
    assert(!ic->HasObject(1) && ic->AddObject(1) && ic->HasObject(1));
    assert(!ic->HasInstance(1, 100));                          // empty list
    assert(ic->AddInstance(1, 100) && ic->AddInstance(1, 200) && ic->AddInstance(1, 100));
    assert(ic->GetObjectInstanceIdList(1)->size() == 2);
    assert(ic->HasInstance(1, 999));                           // any instance counts
    assert(ic->AddInstance(2, 300));                           // creates the object
    std::uint32_t object = 0;
    assert(ic->FindFirstObjectFromInstance(300, object) && object == 2);
    assert(ic->FindFirstObjectFromInstance(100, object) && object == 1);
    assert(!ic->FindFirstObjectFromInstance(7, object));
    assert(ic->RemoveInstance(1, 200));                        // removes the FIRST instance (100)
    assert(ic->GetObjectInstanceIdList(1)->front() == 200);
    assert(!ic->RemoveInstance(9, 1));
    assert(ic->RemoveAllInstances(200) && ic->GetObjectInstanceIdList(1)->size() == 1);
    assert(ic->RemoveObject(1) && !ic->HasObject(1) && ic->GetObjectInstanceIdList(1) == nullptr);
    assert(ic->QueryInterface(0x3df613a1, &p) && !ic->QueryInterface(3, &p));
    ic->Release();
    assert(ic->Release() == 0);
    Audio::SetPersistService(nullptr);
    Audio::SetSoundSystem(nullptr);
}

void TestSystem() {
    g_ticks = 1000;
    fakes::FakeServer server;
    Persist persist;
    Audio::SetMessageServer(&server);
    Audio::SetPersistService(&persist);
    Audio::SetFileSystemDataDirectory("/data/sound");

    cGZSndSys* sys = cGZSndSys::Create();
    sys->AddRef();
    Audio::SetSoundSystem(sys);
    assert(sys->GetServiceID() == 0x23cecba0 && sys->GetServicePriority() == 500000 && sys->IsServiceRunning());
    cRZString* dir = static_cast<cRZString*>(sys->DataDirectory());
    assert(dir->str() == "/data/sound");
    assert(sys->GetMasterVolume() == 0x400 && sys->GetEnabledFlag() && !sys->GetInitFlag() && !sys->GetTestMode());
    float f[12];
    assert(sys->GetRolloffFactor(f) && f[0] == 1.0f && sys->GetDopplerFactor(f) && f[0] == 0.0f);

    // QI
    void* p = nullptr;
    assert(sys->QueryInterface(0x106077d2, &p) && p == static_cast<cIGZSndSys*>(sys));
    assert(sys->QueryInterface(0x287fb697, &p) && p == static_cast<cIGZSystemService*>(sys));
    assert(sys->QueryInterface(0x452294aa, &p) && p == static_cast<cIGZMessageTarget2*>(sys));
    assert(sys->QueryInterface(0x406077d2, &p) && p == static_cast<cIGZSnd3DListener*>(sys));
    assert(sys->QueryInterface(0x5b30e7b3, &p) && p == static_cast<cIGZSnd3DListenerEx*>(sys));
    assert(sys->QueryInterface(0xab79e31c, &p) && p == static_cast<cIGZSndEaRendererEffects*>(sys));
    assert(!sys->QueryInterface(0x1234, &p));
    for (int i = 0; i < 6; ++i) sys->Release();

    // Init: message registration, caches, timers
    assert(sys->Init() && sys->GetInitFlag());
    assert(server.added.size() == 1 && server.added[0].second == 0xdb53383b);
    assert(sys->GetSndPropertyCache() != nullptr && sys->GetSndInstanceCache() != nullptr);
    cIGZSndPropertyCache* pc = nullptr;
    assert(sys->GetSndPropertyCacheByIid(&pc) && pc == sys->GetSndPropertyCache());
    pc->Release();
    assert(g_timerCallback != nullptr && g_timerInterval == 20);

    // renderers and the forwarded settings
    Renderer r;
    assert(sys->NumRenderers() == 0);
    assert(sys->RegisterRenderer(&r) && sys->RegisterRenderer(&r) && sys->NumRenderers() == 1);
    assert(sys->IsRegisteredRenderer(&r));
    assert(sys->SetPosition(1, 2, 3) && r.position[2] == 3);
    sys->SetOrientation(0, 1, 0, 0, 0, 1);
    assert(r.orientation[1] == 1 && r.orientation[5] == 1);
    sys->SetVelocity(4, 5, 6);
    assert(r.velocity[1] == 5);
    sys->SetDopplerFactor(2); sys->SetRolloffFactor(3); sys->SetDistanceFactor(4);
    assert(r.doppler == 2 && r.rolloff == 3 && r.distance == 4);
    assert(sys->GetRolloffFactor(f) && f[0] == 3);
    sys->CommitDeferredSettings();
    assert(r.commits == 1);
    float all[12];
    assert(sys->GetAll(&all[0], &all[1], &all[2], &all[3], &all[4], &all[5], &all[6], &all[7], &all[8], &all[9], &all[10], &all[11]));
    assert(all[0] == 1 && all[3] == 4 && all[6] == 0 && all[11] == 1);
    // SetAll ignores its arguments and re-sends the stored values
    r.position[0] = 99;
    assert(sys->SetAll(7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7) && r.position[0] == 1);
    // master volume argument is ignored
    sys->SetMasterVolume(10);
    assert(r.masterVolumes.size() == 1 && r.masterVolumes[0] == 0x400 && sys->GetMasterVolume() == 0x400);
    assert(sys->InitializeEffectsBus(0, 0.5f, 1, 2, 3) && r.busInits == 1);
    assert(!sys->SetEffectMasterLevel(0, 0.25f) && r.busLevel == 0.25f);   // last renderer's result

    // targets: fast list ticks every Update, slow list after more than 250 ms
    Target fast, slow;
    sys->RegisterTarget(&fast, 20);
    sys->RegisterTarget(&slow, 250);
    assert(sys->fastTargetCount() == 1 && sys->slowTargetCount() == 1);
    sys->RegisterTarget(&fast, 20);                         // re-registering replaces
    assert(sys->fastTargetCount() == 1);
    g_ticks += 20;
    sys->HandleTimerCallback(20);
    assert(fast.updates.size() == 1 && fast.updates[0] == 20 && slow.updates.empty());
    g_ticks += 300;
    sys->HandleTimerCallback(300);
    assert(fast.updates.size() == 2 && slow.updates.size() == 1);
    // registering / unregistering from inside a tick is deferred
    Target late;
    fast.onUpdate = [&] { sys->RegisterTarget(&late, 20); sys->UnregisterTarget(&slow); };
    sys->HandleTimerCallback(20);
    fast.onUpdate = nullptr;
    assert(sys->fastTargetCount() == 2 && sys->slowTargetCount() == 0);
    assert(late.updates.empty());
    sys->HandleTimerCallback(20);
    assert(late.updates.size() == 1);
    // updates can be disabled
    sys->DisableUpdates();
    sys->HandleTimerCallback(20);
    assert(late.updates.size() == 1);
    sys->EnableUpdates();

    // sounds as targets: lookup by id, Panic kills them
    fakes::FakeSnd snd;
    snd.refs = 1;
    struct SndTarget final : cIGZSndUpdateTarget {
        fakes::FakeSnd* snd;
        int refs = 0;
        bool QueryInterface(std::uint32_t iid, void** out) override { return snd->QueryInterface(iid, out); }
        std::uint32_t AddRef() override { return ++refs; }
        std::uint32_t Release() override { return --refs; }
        void Update(std::int32_t) override {}
    } sndTarget;
    sndTarget.snd = &snd;
    sys->RegisterTarget(&sndTarget, 250);
    // FakeSnd::GetID() is 0, so id 0 is "found"
    assert(sys->IsSndPlaying(0) && !sys->IsSndPlaying(5));
    snd.playing = true;
    sys->Panic();
    assert(snd.kills == 1 && !snd.playing);
    sys->UnregisterTarget(&sndTarget);

    // events: handlers see DoEvent; a consuming handler stops the dispatch
    Handler h1, h2;
    sys->RegisterEventHandler(&h1);
    sys->RegisterEventHandler(&h2);
    assert(sys->handlerCount() == 2);
    assert(!sys->DoEventIds(0x42, 1, 2, 3, 4));
    assert(h1.seen.size() == 1 && h2.seen.size() == 1 && h1.seen[0] == 0x42);
    h1.consume = true;
    assert(sys->DoEventIds(0x43, 0, 0, 0, 0) && h2.seen.size() == 1);
    h1.consume = false;
    // handler registered from inside a handler is deferred
    Handler h3;
    h1.onEvent = [&] { sys->RegisterEventHandler(&h3); sys->UnregisterEventHandler(&h2); };
    sys->DoEventIds(0x44, 0, 0, 0, 0);
    h1.onEvent = nullptr;
    assert(sys->handlerCount() == 2);                        // h1, h3
    sys->DoEventIds(0x45, 0, 0, 0, 0);
    assert(h3.seen.size() == 1 && h3.seen[0] == 0x45 && h2.seen.size() == 2);
    // unknown / string / unknown-object variants
    auto* text = new cRZString("hi");
    text->AddRef();                                           // strings are reference counted
    assert(!sys->DoEventString(0x50, text, 9, 9, 9, 9));
    assert(text->Release() == 0);
    assert(!sys->DoEventUnknown(0x51, nullptr, 1, 2, 3, 4));

    // queued events: PostEvent -> Update drains the queue and ticks the targets
    sys->PostEventIds(0x60, 5, 6, 7, 8);
    sys->PostEventIds(0x61, 0, 0, 0, 0);
    assert(sys->queuedEventCount() == 2);
    const std::size_t before = h1.seen.size();
    assert(sys->Update(20));
    assert(sys->queuedEventCount() == 0 && h1.seen.size() == before + 2);
    // events are only executed when a renderer exists
    assert(sys->UnregisterRenderer(&r) && !sys->UnregisterRenderer(&r) && sys->NumRenderers() == 0);
    sys->PostEventIds(0x62, 0, 0, 0, 0);
    sys->Update(20);
    assert(h1.seen.size() == before + 2);

    // DoMessage with a foreign type is ignored
    cRZMessage2Standard msg;
    msg.SetType(0x77);
    assert(!sys->DoMessage(&msg));

    // extension map
    cRZString e1(" .WAV "), e2("ogg"), e3("   ");
    assert(sys->SetExtensionDefaultDataSourceClassId(e1, 11) && sys->SetExtensionDefaultDataSourceClassId(e2, 22));
    assert(!sys->SetExtensionDefaultDataSourceClassId(e3, 33));
    std::uint32_t id = 0;
    cRZString q(".Wav");
    assert(sys->GetExtensionDefaultDataSourceClassId(q, id) && id == 11);
    assert(sys->GetNumExtensionDefaultDataSourceClassIdMappings() == 2);
    assert(sys->GetExtensionDefaultDataSourceClassIdMappingClassId(0, id) && id == 22);   // "ogg" < "wav"
    cRZString name;
    assert(sys->GetExtensionDefaultDataSourceClassIdMappingExtension(1, name) && name.str() == "wav");
    assert(!sys->GetExtensionDefaultDataSourceClassIdMappingClassId(2, id));

    // mainline release list holds references until shutdown
    fakes::FakeSnd holder;
    holder.refs = 1;
    sys->AddToMainlineReleaseList(static_cast<cIGZSnd*>(&holder));
    assert(holder.refs == 2 && sys->mainlineReleaseCount() == 1);

    // shutdown
    assert(sys->Shutdown());
    assert(!sys->GetInitFlag() && sys->shuttingDown() && holder.refs == 1);
    assert(server.removed.size() == 1 && sys->handlerCount() == 0 && sys->fastTargetCount() == 0);
    assert(sys->GetSndPropertyCache() == nullptr);
    // unregistering during shutdown is accepted and does nothing
    assert(sys->UnregisterTarget(&fast));

    Audio::SetSoundSystem(nullptr);
    sys->Release();
    Audio::SetMessageServer(nullptr);
    Audio::SetPersistService(nullptr);
    Audio::SetFileSystemDataDirectory("");
}

void TestSysMac() {
    cGZSndSys* sys = cGZSndSys::Create();
    sys->AddRef();
    Audio::SetSoundSystem(sys);
    cGZSndSysMac* mac = cGZSndSysMac::Create();
    mac->AddRef();
    assert(mac->GetServiceID() == 0xe9d27715 && mac->GetServicePriority() == 500000);
    assert(mac->Init());
    assert(sys->NumRenderers() == 1);
    assert(GZSnd3D::ActiveListener() != nullptr);
    void* p = nullptr;
    assert(mac->QueryInterface(0xc9d25f29, &p) && p == static_cast<cIGZSndRenderer*>(mac));
    mac->Release();
    assert(mac->QueryInterface(0x406077d2, &p) && p == static_cast<cIGZSnd3DListener*>(GZSnd3D::ActiveListener()));
    GZSnd3D::ActiveListener()->Release();
    assert(!mac->QueryInterface(0x1234, &p));
    // pause quirks
    assert(!mac->IsPaused());
    mac->Pause(true);
    assert(mac->IsPaused());
    mac->Unpause();
    assert(mac->IsPaused());                                 // Unpause sets, it does not clear
    assert(!mac->SetMasterVolume(5) && mac->GetMasterVolume() == 0);
    // the system's listener settings reach the platform listener
    sys->SetPosition(1, 2, 3);
    assert(std::fabs(GZSnd3D::ActiveListener()->position()[0] - (-2)) < 1e-6f);   // (-y, z, x)
    assert(mac->Shutdown());
    assert(sys->NumRenderers() == 0 && GZSnd3D::ActiveListener() == nullptr);
    assert(mac->Release() == 0);
    Audio::SetSoundSystem(nullptr);
    sys->Release();
}

void TestDefaultEventHandler() {
    Persist persist;
    fakes::FakeSndMgr mgr;
    cGZSndSys* sys = cGZSndSys::Create();
    sys->AddRef();
    Audio::SetPersistService(&persist);
    Audio::SetSoundSystem(sys);
    Audio::SetSoundManagerService(&mgr);
    g_ticks = 5000;
    Audio::SetMessageServer(nullptr);
    assert(sys->Init());
    auto* h = cGZSndDefaultEventHandler::Create();
    h->AddRef();
    void* p = nullptr;
    assert(h->QueryInterface(0xbb523bb2, &p) && !h->QueryInterface(2, &p));
    h->Release();

    std::vector<fakes::FakeEvent*> events;   // the handler only borrows events; the test owns them
    const auto event = [&events](std::uint32_t id, std::uint32_t d1, std::uint32_t d2 = 0, std::uint32_t d3 = 0) {
        auto* e = new fakes::FakeEvent();
        events.push_back(e);
        e->id = id;
        e->message.SetData1(d1); e->message.SetData2(d2); e->message.SetData3(d3);
        return e;
    };
    // start: plays at 0x400 once; unknown ids are not consumed
    fakes::FakeEvent* e = event(1, 7);
    assert(h->DoEvent(e));
    fakes::FakeSnd* snd = mgr.sounds.at(7);
    assert(snd->plays == 1 && snd->autoVolume == 0x400);
    assert(!h->DoEvent(event(1, 0)));
    // stop / kill
    assert(!h->DoEvent(event(2, 7)) && mgr.sounds.at(7)->stops == 1);    // each lookup builds a new fake
    assert(!h->DoEvent(event(4, 7)) && mgr.sounds.at(7)->kills == 1);
    assert(!h->DoEvent(event(3, 7)));
    // property cache events
    assert(!h->DoEvent(event(0xca3553d6, 21)));
    assert(sys->GetSndPropertyCache()->HasInstance(21));
    assert(!h->DoEvent(event(0xea355407, 21, 0x30, 99)));
    AnyUint32 got;
    assert(sys->GetSndPropertyCache()->GetInstanceProperty(21, 0x30, &got) && got.get() == 99);
    assert(!h->DoEvent(event(0x0a35540c, 21, 0x30)));
    assert(!sys->GetSndPropertyCache()->HasInstanceProperty(21, 0x30));
    assert(!h->DoEvent(event(0xca3553e7, 21)));
    assert(!sys->GetSndPropertyCache()->HasInstance(21));
    assert(sys->GetSndPropertyCache()->AddInstance(0xcc3014ee));
    assert(!h->DoEvent(event(0x0a355411, 5, 6)));
    assert(sys->GetSndPropertyCache()->GetGlobalProperty(5, &got) && got.get() == 6);
    assert(!h->DoEvent(event(0x0a355417, 5)));
    assert(!sys->GetSndPropertyCache()->HasGlobalProperty(5));
    assert(!h->DoEvent(event(0x1234, 5)));
    assert(h->Release() == 0);
    for (fakes::FakeEvent* ev : events) delete ev;
    sys->Shutdown();
    Audio::SetSoundManagerService(nullptr);
    Audio::SetSoundSystem(nullptr);
    Audio::SetPersistService(nullptr);
    sys->Release();
}

}  // namespace

int main() {
    TestMessageQueue();
    TestCaches();
    TestSystem();
    TestSysMac();
    TestDefaultEventHandler();
    std::puts("gzsndsys ok");
}
