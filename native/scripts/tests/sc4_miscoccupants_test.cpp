// Tests the small occupant classes: foundation, landfill, power line and toxic waste.
#include <cmath>
#include <cstdio>

#include "rz/PortMemoryStreams.h"
#include "sc4/FoundationOccupant.h"
#include "sc4/LandfillOccupant.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/ToxicOccupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace {

template <class T>
T* RoundTrip(T* source) {
    PortMemoryOStream os;
    CHECK(source->Write(os));
    const auto& bytes = os.bytes();
    PortMemoryIStream is(bytes.data(), bytes.size());
    T* copy = new T();
    copy->AddRef();
    CHECK(copy->Read(is));
    CHECK(is.RemainingBytes() == 0);
    return copy;
}

class Pole final : public cISC4PowerPoleOccupant {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    std::uint32_t refs = 0;
};

class Effect final : public IToxicEffect {
public:
    void Release() override { ++releases; }
    void Start(int) override { ++starts; }
    void Stop(int) override { ++stops; }
    void SetTransform(const cS3DTransform& t) override { last = t; ++transforms; }
    int releases = 0, starts = 0, stops = 0, transforms = 0;
    cS3DTransform last;
};

class ToxicHostImpl final : public IToxicHost {
public:
    IToxicEffect* CreateEffect(const char* name) override { requested = name; return &effect; }
    void CellToWorld(int cx, int cz, float& x, float& z) override { x = cx * 16.0f + 8.0f; z = cz * 16.0f + 8.0f; }
    float TerrainHeight(float x, float z) override { return x + z * 0.5f; }
    Effect effect;
    const char* requested = nullptr;
};

}  // namespace

int main() {
    // ---- foundation ----
    {
        auto* f = new cSC4FoundationOccupant();
        f->AddRef();
        CHECK(f->GetType() == kOccupantTypeFoundation && f->GetGZCLSID() == 0x49c05c8f);
        const float pos[3] = {100, 20, 50}, size[3] = {16, 6, 32};
        f->SetPosition(pos);
        CHECK(f->SetSize(size));
        float lo[3], hi[3];
        f->GetBoundingBox(lo, hi);
        CHECK(lo[0] == 92 && hi[0] == 108 && lo[1] == 14 && hi[1] == 20 && lo[2] == 34 && hi[2] == 66);
        f->SetOrientation(-4); CHECK(f->GetOrientation() == 0);
        f->SetOrientation(2); CHECK(f->GetOrientation() == 2);
        f->SetOrientation(9); CHECK(f->GetOrientation() == 3);
        f->SetFoundationType(77); f->SetMaxHeight(4.5f); f->SetUsage(-3);
        CHECK(f->GetFoundationType() == 77 && f->GetMaxHeight() == 4.5f && f->GetUsage() == -3);
        auto* g = RoundTrip(f);
        float p2[3];
        g->GetPosition(p2);
        CHECK(p2[0] == 100 && p2[1] == 20 && p2[2] == 50);
        g->GetBoundingBox(lo, hi);
        CHECK(lo[2] == 34 && hi[2] == 66 && g->GetOrientation() == 3 && g->GetUsage() == -3);
        CHECK(g->GetFoundationType() == 77 && g->GetMaxHeight() == 4.5f);
        void* iface = nullptr;
        CHECK(g->QueryInterface(kGZIID_cISC4FoundationOccupant, &iface) && iface == g);
        g->Release(); g->Release();   // the query above took one
        // wrong version
        const std::uint8_t bad[] = {1, 0};
        PortMemoryIStream is(bad, sizeof bad);
        cSC4FoundationOccupant h;
        h.AddRef();
        CHECK(!h.Read(is));
        f->Release();
    }

    // ---- landfill ----
    {
        auto* l = new cSC4LandfillOccupant(12, 3);
        l->AddRef();
        CHECK(l->GetType() == kOccupantTypeLandfill && l->GetTileID() == 12 && l->GetOrientation() == 3 && !l->IsEmptyLandfillTile());
        CHECK(l->IsFlagSet(kOccupantFlagVisible));
        const float pos[3] = {160, 3, 96};
        l->SetPosition(pos);
        float lo[3], hi[3];
        l->GetBoundingBox(lo, hi);
        CHECK(lo[0] == 152 && lo[1] == 3 && lo[2] == 88 && hi[0] == 168 && hi[1] == 19 && hi[2] == 104);
        CHECK(l->SetTileID(20) == 12 && l->GetTileID() == 20 && l->SetTileID(20) == 20);
        l->SetEmptyLandfillTile(true);
        l->SetOrientation(1);
        PortMemoryOStream os;
        CHECK(l->Write(os));
        const auto& bytes = os.bytes();
        PortMemoryIStream is(bytes.data(), bytes.size());
        auto* r = new cSC4LandfillOccupant();
        r->AddRef();
        CHECK(r->Read(is));
        r->GetBoundingBox(lo, hi);
        CHECK(r->GetTileID() == 20 && r->GetOrientation() == 1 && r->IsEmptyLandfillTile() && lo[0] == 152 && hi[1] == 19);
        void* iface = nullptr;
        CHECK(r->QueryInterface(kGZIID_cISC4LandfillOccupant, &iface) && iface == r);
        CHECK(!r->QueryInterface(0x12345678, &iface));
        CHECK(r->QueryInterface(kGZIID_cISC4Occupant, &iface) && iface == static_cast<cISC4Occupant*>(r));
        r->Release(); r->Release(); r->Release();
        l->Release();
    }

    // ---- power line ----
    {
        auto* w = new cSC4PowerLineOccupant();
        w->AddRef();
        CHECK(w->GetType() == kOccupantTypePowerLine && w->GetGZCLSID() == 0xc9c05c5d && !w->IsPreview());
        const float pos[3] = {170, 9, 35};
        w->SetPosition(pos);
        std::uint32_t cx = 0, cz = 0;
        w->GetCell(cx, cz);
        CHECK(cx == 10 && cz == 2);
        float lo[3], hi[3];
        w->GetBoundingBox(lo, hi);
        CHECK(lo[0] == 170 && hi[0] == 170 && lo[1] == 9 && hi[2] == 35);
        Pole a, b;
        a.refs = b.refs = 1;
        w->SetConnectedPoles(&a, &b, true);
        CHECK(a.refs == 2 && b.refs == 2);
        cISC4PowerPoleOccupant *p0 = nullptr, *p1 = nullptr;
        bool connected = false;
        w->GetConnectedPoles(p0, p1, connected);
        CHECK(p0 == &a && p1 == &b && connected && a.refs == 3 && b.refs == 3);
        a.Release(); b.Release();
        Pole c;
        c.refs = 1;
        w->SetConnectedPoles(&c, nullptr, false);     // replaces the old poles (their refs dropped)
        CHECK(a.refs == 1 && b.refs == 1 && c.refs == 2);
        w->Shutdown();
        CHECK(c.refs == 1);
        w->SetPreview(true); CHECK(w->IsPreview());
        void* iface = nullptr;
        CHECK(w->QueryInterface(kGZIID_cISC4PowerLineOccupant, &iface) && iface == w);
        CHECK(w->QueryInterface(0xa9764c8c, &iface) && iface == w);
        w->Release(); w->Release(); w->Release();
    }

    // ---- toxic ----
    {
        ToxicHostImpl host;
        SetToxicHost(&host);
        auto* t = new cSC4ToxicOccupant();
        t->AddRef();
        CHECK(t->GetType() == kOccupantTypeToxic && t->GetGZCLSID() == 0xabf98cb5 && !t->IsActive());
        t->SetHosed(5); CHECK(t->GetHosed() == 5);
        t->SetHosed(-2); CHECK(t->GetHosed() == 3);
        t->SetHosed(-3); CHECK(t->GetHosed() == 0);       // can't go below zero (exact amount empties it)
        t->SetHosed(4); t->SetHosed(-10); CHECK(t->GetHosed() == 0);
        CHECK(t->Init());
        CHECK(std::string(host.requested) == "toxic_disaster_fx" && t->IsActive() && host.effect.starts == 1);
        CHECK(t->IsOccupantGroup(kOccupantGroupToxic));
        t->SetCell(3, 5);
        float pos[3];
        t->GetPosition(pos);
        CHECK(pos[0] == 56 && pos[2] == 88 && pos[1] == 56 + 44);
        CHECK(host.effect.last.flags == 1 && host.effect.last.changeCounter == 1 && host.effect.last.translation[0] == 56 &&
              host.effect.last.translation[2] == 88 && host.effect.last.scale == 1.0f && host.effect.last.rotation[1][1] == 1.0f);
        float lo[3], hi[3];
        t->GetBoundingBox(lo, hi);
        CHECK(lo[0] == 40 && hi[0] == 72 && hi[1] == pos[1] + 8 && lo[2] == 72);
        t->SetHosed(9);
        auto* r = RoundTrip(t);
        CHECK(r->GetHosed() == 9);
        r->Release();
        CHECK(t->Shutdown());
        CHECK(!t->IsActive() && host.effect.stops == 1 && host.effect.releases == 1);
        t->Release();
        SetToxicHost(nullptr);
    }
    std::printf(failures ? "FAILED\n" : "sc4_miscoccupants ok\n");
    return failures ? 1 : 0;
}
