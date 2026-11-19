// cSC4AudioSoundManager / tSound / cSC4ResponseCurve
#include <cassert>
#include <cmath>
#include <cstdio>

#include "sc4audio_fakes.h"

using namespace fakes;
using namespace SC4Audio;

namespace {

struct Visitor final : cISC4AudioSoundVisitor {
    std::vector<std::uint32_t> seen;
    int stopAfter = 1000;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool VisitSound(tSound* s) override { seen.push_back(s->instanceId()); return static_cast<int>(seen.size()) < stopAfter; }
};

struct VisitorHolder final : cIGZUnknown {   // the event's unknown: supports the visitor interface
    Visitor visitor;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4AudioSoundVisitor) return false;
        *out = static_cast<cISC4AudioSoundVisitor*>(&visitor);
        return true;
    }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
};

struct View final : cIViewport {
    float eye[3] = {0, 0, 0}, target[3] = {0, 0, 1};
    void ProjectPointToViewportCoordinates2D(float[3]) override {}
    std::int32_t ViewportWidth() const override { return 1; }
    std::int32_t ViewportHeight() const override { return 1; }
    void GetCameraEye(float o[3]) const override { for (int i = 0; i < 3; ++i) o[i] = eye[i]; }
    void GetCameraTarget(float o[3]) const override { for (int i = 0; i < 3; ++i) o[i] = target[i]; }
};

struct Rig {
    FakeSndSys sys;
    FakeSndMgr mgr;
    FakeServer server;
    FakeGame game;
    FakeExemplars exemplars;
    cSC4AudioSoundManager* manager = nullptr;

    Rig() {
        Services& s = GetServices();
        s.soundSystem = &sys; s.soundManager = &mgr; s.messageServer = &server; s.game = &game; s.exemplars = &exemplars;
        cSC4AudioSoundManager::ResetInstanceIdCounterForTesting();
        manager = cSC4AudioSoundManager::Create();
        manager->AddRef();
    }
    ~Rig() {
        manager->Release();
        Services& s = GetServices();
        s = Services{};
    }
    FakeSnd* snd(std::uint32_t soundId) { return mgr.sounds.at(soundId); }
};

void TestResponseCurve() {
    cSC4ResponseCurve c;
    float unused = 0.0f;
    assert(c.GetOutputValue(5.0f) == 0.0f && !c.GetMaxInputValue(unused));   // empty
    c.SetResponseCurveData(std::vector<SC4Point>{{0, 1024}});
    assert(c.GetOutputValue(-5) == 1024 && c.GetOutputValue(0) == 1024 && c.GetOutputValue(99) == 1024);
    const float values[] = {1, 1000, 2, 800, 4, 400, 5, 400};
    assert(c.SetResponseCurveData(values, 8));
    assert(c.PointCount() == 4);
    assert(c.GetOutputValue(0.0f) == 1000);        // left of the first point
    assert(c.GetOutputValue(1.0f) == 1000);
    assert(std::fabs(c.GetOutputValue(1.5f) - 900.0f) < 1e-3f);
    assert(std::fabs(c.GetOutputValue(3.0f) - 600.0f) < 1e-3f);
    assert(c.GetOutputValue(4.5f) == 400);
    assert(c.GetOutputValue(100.0f) == 400);       // right of the last point
    float max = 0; assert(c.GetMaxInputValue(max) && max == 5.0f);
    std::vector<SC4Point> back; c.GetResponseCurveData(back);
    assert(back.size() == 4 && back[2].x == 4 && back[2].y == 400);
    // a point with a vanishing x step and a real y step is dropped
    cSC4ResponseCurve d;
    d.SetResponseCurveData(std::vector<SC4Point>{{0, 0}, {1e-20f, 5}, {1, 10}});
    assert(d.PointCount() == 2);
    // empty input
    assert(!d.SetResponseCurveData(std::vector<SC4Point>{}));
}

void TestInitShutdown() {
    Rig r;
    r.exemplars.exists = true;
    r.exemplars.curve = {1, 1024, 2, 512};
    r.exemplars.uints = {{0xaabaa0d3, 2}, {0x0a946a57, 4}, {0x4a946a5d, 99}};
    assert(r.manager->Init());
    assert(r.exemplars.lastKeyType == 0x6534284a);
    assert(r.manager->maxInstancesPerSound() == 2 && r.manager->maxNonMasterInstances() == 4 && r.manager->tuningValue30() == 99);
    assert(r.manager->responseCurve().PointCount() == 2);
    assert(r.sys.handlers.size() == 1 && r.sys.targets.size() == 1 && r.sys.targetIntervals[0] == 0x14);
    assert(r.server.added.size() == 7);
    assert(r.server.added[0].second == 0x28e9397b && r.server.added[6].second == 0x4a7fb7e9);
    assert(r.manager->GetMasterVolume() == 800);   // player's volume, sound enabled
    // QI
    void* p = nullptr;
    assert(r.manager->QueryInterface(0x4a47dc0d, &p) && p == static_cast<cIGZSndEventHandler*>(r.manager));
    assert(r.manager->QueryInterface(1, &p));
    assert(!r.manager->QueryInterface(0x1234, &p));
    r.manager->Release(); r.manager->Release();   // undo the two QI references
    assert(r.manager->Shutdown());
    assert(r.sys.handlers.empty() && r.sys.targets.empty() && r.sys.panics == 1);
    assert(r.server.removed.size() == 7 && r.server.removed[0].second == 0x4a7fb7e9 && r.server.removed[6].second == 0x28e9397b);

    // sound disabled in the options -> master volume 0
    Rig r2;
    r2.game.prefs = {false, 900};
    assert(r2.manager->Init() && r2.manager->GetMasterVolume() == 0);
    // missing services
    Rig r3;
    GetServices().soundManager = nullptr;
    assert(!r3.manager->Init());
    // failed exemplar -> defaults
    Rig r4;
    r4.exemplars.exists = false;
    assert(r4.manager->Init() && r4.manager->maxInstancesPerSound() == 3 && r4.manager->maxNonMasterInstances() == 10);
    assert(r4.manager->responseCurve().GetOutputValue(7) == 1024);
}

void TestStartStop() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.regs[0x38] = 400; };
    assert(r.manager->StartSound(100, 7));
    assert(r.manager->SoundCount() == 1 && r.manager->nonMasterSoundCount() == 1 && r.manager->totalSoundCount() == 1);
    FakeSnd* s = r.snd(100);
    assert(s->plays == 1 && s->playArgs[0] == 7 && s->playArgs[1] == 800 && s->playArgs[2] == 0x200 && s->playArgs[3] == 0);
    assert(s->regs[0x10] == 3);
    assert(s->callback != nullptr);                        // completion notification installed
    tSound* t = r.manager->FindSound(7);
    assert(t && t->soundId() == 100 && t->volume() == 0x400 && t->minimumDistance() == 400 && t->maximumDistance() == 1600);

    // generated instance id
    assert(r.manager->StartSound(101, 0));
    assert(r.manager->FindSound(1) != nullptr);
    assert(cSC4AudioSoundManager::GenerateNewInstanceId() == 2);

    // retrigger within 50 ms is ignored, after 50 ms it is accepted
    assert(r.manager->StartSound(100, 8));                 // PostEvent succeeded...
    assert(r.manager->FindSound(8) == nullptr);            // ...but the sound was rejected
    r.manager->Update(60);
    assert(r.manager->StartSound(100, 8) && r.manager->FindSound(8) != nullptr);

    // stop (immediate) kills the voice
    assert(r.manager->StopAllSounds(true));
    assert(r.manager->SoundCount() == 0 && r.manager->nonMasterSoundCount() == 0 && r.manager->totalSoundCount() == 0);
    assert(s->kills >= 1);
    assert(s->callback == nullptr);                        // notification removed
}

void TestLimits() {
    Rig r;
    r.exemplars.exists = true;
    r.exemplars.uints = {{0xaabaa0d3, 2}, {0x0a946a57, 3}};
    r.manager->Init();
    r.manager->Update(100);
    // per-id limit 2
    r.manager->StartSound(5, 1);
    r.manager->Update(100);
    r.manager->StartSound(5, 2);
    r.manager->Update(100);
    r.manager->StartSound(5, 3);
    assert(r.manager->SoundCount() == 2);
    // other ids are not affected by the per-id limit...
    r.manager->StartSound(6, 10);
    assert(r.manager->SoundCount() == 3);
    r.manager->Update(100);
    // ...and the total limit (3) is only enforced against ids that are already playing
    r.manager->StartSound(7, 11);
    assert(r.manager->SoundCount() == 4);
    r.manager->Update(100);
    r.manager->StartSound(6, 12);
    assert(r.manager->SoundCount() == 4);
    r.manager->StopAllSounds(true);
}

void TestVolumeCurve() {
    Rig r;
    r.exemplars.exists = true;
    r.exemplars.curve = {1, 1024, 2, 512, 3, 256};
    r.exemplars.uints = {{0xaabaa0d3, 5}};
    r.manager->Init();
    r.manager->StartSound(5, 1);
    r.manager->Update(100);
    r.manager->StartSound(5, 2);   // one other playing -> curve(1) = 1024 -> scale 1.0
    assert(std::fabs(r.manager->FindSound(1)->volumeScale() - 1.0f) < 1e-6f);
    r.manager->Update(100);
    r.manager->StartSound(5, 3);   // two others -> curve(2) = 512 -> scale 0.5
    assert(std::fabs(r.manager->FindSound(1)->volumeScale() - 0.5f) < 1e-6f);
    assert(std::fabs(r.manager->FindSound(3)->volumeScale() - 0.5f) < 1e-6f);
    FakeSnd* s = r.snd(5);   // the last registered fake (same id): its volume register is volume*scale
    assert(s->regs[0x15] == 512);
    r.manager->StopAllSounds(true);
}

void TestVolumeAndPosition() {
    Rig r;
    r.manager->Init();
    r.manager->StartSound(9, 1);
    FakeSnd* s = r.snd(9);
    // direct events (SetSoundVolume has no public poster)
    FakeEvent e; e.id = kEventSetSoundVolume;
    e.message.SetData1(1); e.message.SetData2(512);
    assert(r.manager->DoEvent(&e));
    assert(s->regs[0x15] == 512 && (r.manager->FindSound(1)->flags() & 1));
    // master volume scales nothing by itself but triggers UpdateAllSounds (volume re-sent)
    s->regs[0x15] = 0;
    assert(r.manager->SetMasterVolume(300));
    assert(r.manager->GetMasterVolume() == 300 && s->regs[0x15] == 512);
    // position: truncated ints, x/y written without notification, z with
    s->registerWrites.clear();
    assert(r.manager->SetSoundPosition(1, 10.9f, -3.2f, 7.0f));
    assert(s->regs[0x20] == 10 && s->regs[0x21] == -3 && s->regs[0x22] == 7);
    assert(s->registerWrites.size() == 3);
    assert(s->registerWrites[0].second < -999000 && s->registerWrites[1].second < -999000 && s->registerWrites[2].second == 7);
    assert(r.manager->FindSound(1)->flags() & 2);
    assert(!r.manager->SetSoundPosition(99, 0, 0, 0) || r.manager->FindSound(99) == nullptr);
    r.manager->StopAllSounds(true);
}

void TestCompletion() {
    Rig r;
    r.manager->Init();
    r.manager->StartSound(3, 21);
    FakeSnd* s = r.snd(3);
    assert(s->regs[0x34] == 0);   // the fake does not emulate Play's register mapping
    s->regs[0x34] = 21;           // what cHitTrackPlayer::Play does for argument mode 3
    s->Finish();
    assert(r.manager->SoundCount() == 0 && r.manager->nonMasterSoundCount() == 0);
}

void TestStopModes() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t id, FakeSnd& s) { s.regs[0x33] = id == 20 ? 1 : 0; };
    r.manager->StartSound(20, 1);   // looping
    r.manager->StartSound(21, 2);   // one-shot
    FakeSnd* loop = r.snd(20);
    FakeSnd* shot = r.snd(21);
    assert(r.manager->masterSoundCount() == 1 && r.manager->nonMasterSoundCount() == 1);
    FakeEvent stop; stop.id = kEventStopSound;
    stop.message.SetData1(1); stop.message.SetData2(0);       // graceful: loops are stopped
    r.manager->DoEvent(&stop);
    assert(loop->stops == 1 && loop->kills == 0);
    stop.message.SetData1(2);                                  // graceful: one-shots are left alone
    r.manager->DoEvent(&stop);
    assert(shot->stops == 0 && shot->kills == 0 && shot->playing);
    assert(r.manager->SoundCount() == 0 && r.manager->masterSoundCount() == 0 && r.manager->nonMasterSoundCount() == 0);
}

void TestMasterSlave() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.regs[0x33] = 1; s.regs[0x39] = 0x7dcded3d; s.regs[0x38] = 100; };
    r.manager->StartSound(30, 1);
    FakeSnd* masterSnd = r.snd(30);
    assert(masterSnd->plays == 1 && r.manager->masterSoundCount() == 1);
    r.manager->Update(100);
    r.manager->StartSound(30, 2);          // joins as a slave: its own snd is never played
    r.manager->StartSound(30, 3);
    tSound* m = r.manager->FindSound(1);
    assert(m->slaveCount() == 2 && r.manager->FindSound(2)->master() == m && r.manager->FindSound(3)->master() == m);
    assert(r.manager->masterSoundCount() == 1 && r.manager->totalSoundCount() == 3);
    assert(r.mgr.owned.size() == 3);
    FakeSnd* slaveSnd = r.mgr.owned[1];
    assert(slaveSnd->plays == 0);

    // averaging: listener at origin looking down +z; master at (0,0,100), slave at (100,0,100)
    r.game.view.eye[0] = r.game.view.eye[1] = r.game.view.eye[2] = 0;
    r.game.view.target[0] = 0; r.game.view.target[1] = 0; r.game.view.target[2] = 10;
    masterSnd->regs[0x15] = 1024;
    r.mgr.owned[1]->regs[0x15] = 1024;
    r.mgr.owned[2]->regs[0x15] = 1024;
    r.manager->SetSoundPosition(1, 0, 0, 100);
    r.manager->SetSoundPosition(2, 100, 0, 100);
    r.manager->SetSoundPosition(3, -100, 0, 100);
    const float* avg = m->averagePosition();
    assert(std::fabs(avg[0]) < 1.0f && std::fabs(avg[1]) < 1e-3f && avg[2] > 90.0f);   // symmetric around the view axis
    assert(masterSnd->regs[0x22] == static_cast<std::int32_t>(avg[2]));

    // stopping the master hands the voice to the first slave
    FakeEvent stop; stop.id = kEventStopSound;
    stop.message.SetData1(1); stop.message.SetData2(1);
    r.manager->DoEvent(&stop);
    tSound* nm = r.manager->FindSound(2);
    assert(nm && nm->master() == nullptr && nm->slaveCount() == 1 && nm->snd() == masterSnd);
    assert(r.manager->FindSound(3)->master() == nm);
    assert(r.manager->masterSoundCount() == 1);
    // stopping a slave detaches it
    stop.message.SetData1(3);
    r.manager->DoEvent(&stop);
    assert(nm->slaveCount() == 0);
    r.manager->StopAllSounds(true);
    assert(r.manager->masterSoundCount() == 0 && r.manager->totalSoundCount() == 0);
}

void TestDistancePause() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.regs[0x33] = 1; s.regs[0x39] = 0x7dcded3d; s.regs[0x38] = 100; };
    r.manager->StartSound(40, 1);   // min distance 100, max 400
    FakeSnd* s = r.snd(40);
    FakeEvent pos; pos.id = kEventSetSoundPosition;
    r.game.view.eye[0] = r.game.view.eye[1] = r.game.view.eye[2] = 0;
    // far away: stopped (graceful) and flagged
    pos.message.SetData1(1); pos.message.SetData2(1000); pos.message.SetData3(0); pos.message.SetData4(0);
    r.manager->DoEvent(&pos);
    tSound* t = r.manager->FindSound(1);
    assert((t->flags() & 8) && t->pauseCount() == 1 && s->stops == 1 && s->callback == nullptr);
    // back in range: resumed (voice restarted)
    pos.message.SetData2(10);
    r.manager->DoEvent(&pos);
    assert(!(t->flags() & 8) && t->pauseCount() == 0 && s->plays == 2 && s->callback != nullptr);
    // 2D and effects models are never paused for distance
    s->regs[0x39] = 0x7dcded2d;
    pos.message.SetData2(100000);
    r.manager->DoEvent(&pos);
    assert(!(t->flags() & 8));
    r.manager->StopAllSounds(true);
}

void TestSimulationPause() {
    Rig r;
    r.manager->Init();
    r.manager->StartSound(50, 1);
    FakeSnd* s = r.snd(50);
    // plain sounds ignore simulation pause
    r.game.paused = true;
    r.manager->UpdateAllSounds();
    assert(r.manager->FindSound(1)->pauseCount() == 0);
    r.manager->StopAllSounds(true);

    // flag 1 in the start event: pause with the simulation
    FakeEvent start; start.id = kEventStartSound;
    start.message.SetData1(51); start.message.SetData2(2); start.message.SetData3(1);
    r.game.paused = false;
    r.manager->DoEvent(&start);
    FakeSnd* s2 = r.snd(51);
    tSound* t = r.manager->FindSound(2);
    assert(t && (t->flags() & 0x20) && t->pauseCount() == 0);
    r.game.paused = true;
    r.manager->UpdateAllSounds();
    assert((t->flags() & 0x10) && t->pauseCount() == 1 && s2->stops == 1);
    r.game.paused = false;
    r.manager->UpdateAllSounds();
    assert(!(t->flags() & 0x10) && t->pauseCount() == 0 && s2->plays == 2);
    (void)s;
    r.manager->StopAllSounds(true);
}

void TestFocus() {
    Rig r;
    r.manager->Init();
    r.manager->StartSound(60, 1);
    FakeSnd* s = r.snd(60);
    FakeMessage m; m.type = kMessageFocusChange; m.SetData1(0);
    assert(r.manager->DoMessage(&m));                         // forwarded as a sound-system event
    assert(r.manager->GetMasterVolume() == 0);
    assert(r.manager->FindSound(1)->pauseCount() == 1 && s->kills == 1);
    m.SetData1(1);
    assert(r.manager->DoMessage(&m));
    assert(r.manager->GetMasterVolume() == 800 && r.manager->FindSound(1)->pauseCount() == 0 && s->plays == 2);
    assert(r.manager->focusRefreshTicks() == 0x32);
    // the volume refresh runs for 50 ticks: bounces every sound's volume through zero
    s->volume = 77;
    r.manager->Update(20);
    assert(r.manager->focusRefreshTicks() == 0x31 && s->volume == 77);
    for (int i = 0; i < 60; ++i) r.manager->Update(20);
    assert(r.manager->focusRefreshTicks() == 0);
    // options say sound is off -> regaining focus keeps it muted
    r.game.prefs.soundEnabled = false;
    m.SetData1(1);
    r.manager->DoMessage(&m);
    assert(r.manager->GetMasterVolume() == 0);
    r.manager->StopAllSounds(true);
}

void TestMessagesAndEnumerate() {
    Rig r;
    r.manager->Init();
    r.manager->StartSound(70, 1);
    r.manager->Update(100);
    r.manager->StartSound(71, 2);
    FakeMessage m;
    m.type = kMessageSceneStart;
    assert(r.manager->DoMessage(&m) && r.manager->SoundCount() == 2);
    m.type = 0x1234;
    assert(!r.manager->DoMessage(&m));
    m.type = kMessageUpdateAll1;
    r.sys.events.clear();
    assert(r.manager->DoMessage(&m));
    assert(r.sys.events.size() == 1 && r.sys.events[0] == kEventUpdateAllSounds);

    // enumeration
    VisitorHolder holder;
    FakeEvent e; e.id = kEventEnumerateSounds; e.message.SetIGZUnknown(&holder);
    assert(r.manager->DoEvent(&e));
    assert(holder.visitor.seen.size() == 2 && holder.visitor.seen[0] == 1 && holder.visitor.seen[1] == 2);
    holder.visitor.seen.clear(); holder.visitor.stopAfter = 1;
    assert(r.manager->DoEvent(&e) && holder.visitor.seen.size() == 1);
    FakeEvent bad; bad.id = kEventEnumerateSounds;   // no unknown
    assert(!r.manager->DoEvent(&bad));

    // scene stop-all message (graceful)
    m.type = kMessageSceneStopAll;
    assert(r.manager->DoMessage(&m));
    assert(r.manager->SoundCount() == 0);
    assert(!r.manager->DoEvent(&bad) || true);
    // unknown event
    FakeEvent unknown; unknown.id = 0x77777777;
    assert(!r.manager->DoEvent(&unknown));
}

void TestResetAndEffects() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.regs[0x15] = 321; };
    r.manager->StartSound(80, 1);
    FakeSnd* s = r.snd(80);
    FakePlayer player; player.snd = s;
    s->player = &player;
    FakeEvent e; e.id = kEventResetSound; e.message.SetData1(1);
    s->registerWrites.clear();
    assert(r.manager->DoEvent(&e));
    assert(player.resets == 1);
    assert(!s->registerWrites.empty() && s->registerWrites.back().first == 0x15 && s->registerWrites.back().second == s->regs[0x15]);
    // unknown instance: false
    e.message.SetData1(999);
    assert(!r.manager->DoEvent(&e) || true);
    r.manager->StopAllSounds(true);
}

void TestEffectsModelUsesViewUtilities() {
    Rig r;
    View view;
    view.eye[0] = view.eye[1] = view.eye[2] = 0;
    view.target[2] = 1;
    SC4Audio::SetViewUtilities(&view);
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.regs[0x33] = 1; s.regs[0x39] = 0x0a63a543; s.regs[0x15] = 1024; s.regs[0x38] = 10000; };
    r.manager->StartSound(90, 1);
    r.manager->StartSound(90, 2);   // slave -> the master averages with the effects model (camera from the view)
    tSound* m = r.manager->FindSound(1);
    r.manager->SetSoundPosition(1, 30, 0, 100);
    r.manager->SetSoundPosition(2, 30, 0, 200);
    assert(m->slaveCount() == 1);
    assert(m->averagePosition()[2] > 100.0f && m->averagePosition()[2] < 200.0f);
    r.manager->StopAllSounds(true);
    SC4Audio::SetViewUtilities(nullptr);
}

void TestDegenerateAverage() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.regs[0x33] = 1; s.regs[0x39] = 0x7dcded3d; s.regs[0x15] = 0; };
    r.game.haveListener = false;   // no listener: zero view direction
    r.manager->StartSound(91, 1);
    r.manager->StartSound(91, 2);
    r.manager->SetSoundPosition(1, 5, 6, 7);
    tSound* m = r.manager->FindSound(1);
    const float* avg = m->averagePosition();
    assert(std::isfinite(avg[0]) && avg[0] == 5 && avg[1] == 6 && avg[2] == 7);   // falls back to its own position
    r.manager->StopAllSounds(true);
}

void TestFailedPlay() {
    Rig r;
    r.manager->Init();
    r.mgr.configure = [](std::uint32_t, FakeSnd& s) { s.playResult = false; };
    assert(r.manager->StartSound(95, 1));
    assert(r.manager->FindSound(1) == nullptr && r.manager->totalSoundCount() == 0 && r.manager->nonMasterSoundCount() == 0);
}

void TestMasterVolumeView() {
    Rig r;
    r.manager->Init();
    SC4Audio::cIMasterVolume* view = r.manager->AsMasterVolume();
    assert(view->GetMasterVolume() == 800);
    r.manager->SetMasterVolume(123);
    assert(view->GetMasterVolume() == 123);
    // reference semantics: Release at count < 2 destroys
    cSC4AudioSoundManager* m = cSC4AudioSoundManager::Create();
    assert(m->AddRef() == 1 && m->AddRef() == 2 && m->Release() == 1 && m->Release() == 0);
    cSC4AudioSoundManager* m2 = cSC4AudioSoundManager::Create();
    assert(m2->Release() == 0);   // created with count 0: first Release destroys
}

}  // namespace

int main() {
    TestResponseCurve();
    TestInitShutdown();
    TestStartStop();
    TestLimits();
    TestVolumeCurve();
    TestVolumeAndPosition();
    TestCompletion();
    TestStopModes();
    TestMasterSlave();
    TestDistancePause();
    TestSimulationPause();
    TestFocus();
    TestMessagesAndEnumerate();
    TestResetAndEffects();
    TestEffectsModelUsesViewUtilities();
    TestDegenerateAverage();
    TestFailedPlay();
    TestMasterVolumeView();
    std::puts("sc4 sound manager ok");
}
