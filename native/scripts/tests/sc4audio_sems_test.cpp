// SC4 emanation models and the sem manager.
#include <cassert>
#include <cmath>
#include <cstdio>
#include <map>

#include "audio/HitMan.h"
#include "sc4audio/SemManager.h"

namespace {

struct Regs final : cIGZSndRegisterSet {
    std::map<std::int32_t, std::int32_t> v;
    std::map<std::int32_t, bool> lastSuppress;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool SetRegisterValue(std::int32_t id, std::int32_t value, bool suppress) override { v[id] = value; lastSuppress[id] = suppress; return true; }
    std::int32_t GetRegisterValue(std::int32_t id) const override { auto it = v.find(id); return it == v.end() ? 0 : it->second; }
    void ClearRegisterValue(std::int32_t id, bool) override { v.erase(id); }
    bool HasRegisterValue(std::int32_t id) const override { return v.count(id) != 0; }
    bool SetRegisterFloat(std::uint32_t, float, bool) override { return true; }
    float GetRegisterFloat(std::uint32_t) const override { return 0; }
};

struct Master final : SC4Audio::cIMasterVolume {
    std::int32_t volume = 1024;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    std::int32_t GetMasterVolume() override { return volume; }
};

struct View final : SC4Audio::cIViewport {
    void ProjectPointToViewportCoordinates2D(float p[3]) override { p[0] = px; p[1] = py; }
    std::int32_t ViewportWidth() const override { return 1000; }
    std::int32_t ViewportHeight() const override { return 600; }
    void GetCameraEye(float out[3]) const override { out[0] = out[1] = out[2] = 0; }
    void GetCameraTarget(float out[3]) const override { out[0] = out[1] = out[2] = 0; }
    float px = 500, py = 300;
};

}  // namespace

int main() {
    Master master;
    SC4Audio::SetSoundManager(&master);

    // ---- 2D ----
    cSC4AudioEmanationModel2D* m2 = new cSC4AudioEmanationModel2D();
    m2->AddRef();
    assert(m2->GetId() == 0x7dcded2d);
    cRZString name;
    assert(m2->GetName(name) && name.str() == "2D SC4K");
    void* p = nullptr;
    assert(m2->QueryInterface(0x89e51472, &p) && p == static_cast<cIGZInitShutdown*>(m2));
    assert(m2->QueryInterface(0xddcf3bd7, &p) && p == static_cast<cIHitSem*>(m2));
    assert(!m2->QueryInterface(0x1234, &p));
    m2->Release(); m2->Release();       // count 3 -> 1
    assert(m2->Init());
    Regs src, ch;
    src.v = {{0x15, 800}, {0x14, 512}, {0x16, 77}, {0x13, 5}};
    master.volume = 512;
    assert(m2->UpdateVolPan(&src, &ch));
    // (800*512)/1024 = 400; 512*400 >> 10 = 200
    assert(ch.v[0x15] == 200 && ch.lastSuppress[0x15] == false);
    assert(ch.v[0x16] == 77 && ch.lastSuppress[0x16] == true && src.v[0x0c] == 200);
    assert(m2->ConfigureChannel(&src, &ch) && ch.v[0x13] == 5 && ch.lastSuppress[0x13]);
    m2->Shutdown();
    assert(m2->UpdateVolPan(&src, &ch) && ch.v[0x15] == 0);          // no sound manager -> master 0
    m2->Release();

    // ---- 3D ----
    SC4Audio::ZoomDistances()[4] = 50.0f;
    cSC4AudioEmanationModel3D m3;
    m3.AddRef();
    master.volume = 1024;
    m3.Init();
    Regs s3, c3;
    s3.v = {{0x20, 10}, {0x21, 20}, {0x22, 30}, {0x40, 1}, {0x38, 200}, {0x15, 1024}, {0x14, 1024}};
    assert(m3.UpdateVolPan(&s3, &c3));
    assert(c3.v[0x20] == 10 && c3.v[0x41] == 0);
    assert(c3.v[0x1e] == 100 && c3.v[0x1f] == 1600);                // 200% of 50, max = 16x
    assert(c3.v[0x15] == 1024);
    assert(m3.ConfigureChannel(&s3, &c3) && c3.v[0x1c] == 1);

    // ---- 3D effects: on-screen centre = centred pan, full volume ----
    View view;
    SC4Audio::SetViewUtilities(&view);
    cSC4AudioEmanationModel3DEffects fx;
    fx.AddRef();
    fx.Init();
    Regs sf, cf;
    sf.v = {{0x38, 100}, {0x15, 1024}, {0x14, 1024}};
    view.px = 512; view.py = 300;
    assert(fx.UpdateVolPan(&sf, &cf));
    assert(cf.v[0x16] == 512 && cf.v[0x15] > 0);                    // sin(pi/2)*512 = 512
    view.px = 0;
    fx.UpdateVolPan(&sf, &cf);
    assert(cf.v[0x16] == 0);                                         // far left
    view.px = 1024;
    fx.UpdateVolPan(&sf, &cf);
    assert(cf.v[0x16] >= 1023);                                      // far right is mirrored
    SC4Audio::SetViewUtilities(nullptr);
    assert(!fx.UpdateVolPan(&sf, &cf));                              // no viewport -> false
    assert(fx.ConfigureChannel(&sf, &cf) && cf.v[0x1c] == 0);

    // ---- sem manager swaps the defaults in and out ----
    cHitMan* hm = new cHitMan();
    hm->AddRef();
    hm->Init();
    cSC4AudioSemManager* mgr = cSC4AudioSemManager::Create();
    mgr->AddRef();
    cIHitSem* default2d = hm->GetSem(0x7dcded2d);
    assert(mgr->Init(hm));
    assert(hm->GetSem(0x7dcded2d) != default2d && hm->GetSem(0x7dcded3d) != nullptr && hm->GetSem(0x0a63a543) != nullptr);
    cRZString n;
    hm->GetSem(0x7dcded2d)->GetName(n);
    assert(n.str() == "2D SC4K");
    assert(mgr->Shutdown());
    assert(hm->GetSem(0x7dcded2d) == default2d && hm->GetSem(0x0a63a543) == nullptr);
    mgr->Release();
    hm->Release();
    SC4Audio::SetSoundManager(nullptr);
    std::puts("sc4 sems ok");
}
