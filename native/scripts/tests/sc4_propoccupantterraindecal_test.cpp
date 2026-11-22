// cSC4PropOccupantTerrainDecal (Mac i386 0x00102b44..0x00106208) and cSC4PropOccupantBurntBuilding (0x00102c24..0x001062d2):
// the decal's overlay life (shown only with flag 2, the land or dynamic land manager by its fade rate), the fade on the
// animation tick with its completion actions, both records (version 3) and the burnt floor's quad.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/OverlayManager.h"
#include "sc4/PropOccupantBurntBuilding.h"
#include "sc4/PropOccupantTerrainDecal.h"

using namespace sc4;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

struct FakeOverlay final : cSTEOverlayManager {
    int index = 0;
    std::uint32_t nextId = 7;
    int adds = 0, removes = 0, enables = 0, disables = 0, updates = 0;
    float alpha = -1.0f;
    cGZPersistResourceKey key{0, 0, 0};
    cDecalInfo info;
    std::uint32_t AddDecal(const cGZPersistResourceKey& k, const cS3DVector2& position, float width, float) override {
        ++adds;
        key = k;
        info.x = position.x;
        info.z = position.y;
        info.width = width;
        return nextId;
    }
    void DecalInfo(std::uint32_t, cDecalInfo* out) const override { *out = info; }
    void UpdateDecalInfo(std::uint32_t, const cDecalInfo& in) override { ++updates; info = in; }
    void RemoveOverlay(std::uint32_t) override { ++removes; }
    void SetOverlayEnabled(std::uint32_t, bool enabled) override { enabled ? ++enables : ++disables; }
    void SetOverlayAlpha(std::uint32_t, float a) override { alpha = a; }
};

struct Host final : ITerrainDecalHost {
    FakeOverlay land, dynamicLand;
    int ticksAdded = 0, ticksRemoved = 0;
    cSTEOverlayManager* OverlayManager(int index) override { return index == 2 ? static_cast<cSTEOverlayManager*>(&land) : &dynamicLand; }
    bool AddTick(cIGZMessageTarget2*) override { ++ticksAdded; return true; }
    void RemoveTick(cIGZMessageTarget2*) override { ++ticksRemoved; }
};

struct BurntHost final : IBurntBuildingHost {
    int inserted = 0, removed = 0;
    bool in = false;
    std::int32_t TextureId(std::uint32_t, std::uint32_t, std::uint32_t instance) override { return instance == kBurntBuildingTextureInstance ? 42 : 0; }
    void ProjectToViewCoordinates(float*) override {}
    bool IsModelInserted(s3d::cIS3DModelInstance*) override { return in; }
    bool InsertModel(s3d::cIS3DModelInstance*) override { ++inserted; in = true; return true; }
    bool RemoveModel(s3d::cIS3DModelInstance*) override { ++removed; in = false; return true; }
};

void Tick(cSC4PropOccupantTerrainDecal& decal, float milliseconds) {
    auto* message = new cRZMessage2Standard();   // reference counted, as the tick manager sends it
    message->AddRef();
    message->SetType(kTerrainDecalTickMessage);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &milliseconds, sizeof bits);
    message->SetData3(bits);
    decal.DoMessage(message);
    message->Release();
}

}  // namespace

int main() {
    Host host;
    SetTerrainDecalHost(&host);
    {   // a static decal: 32 m box until textured; shown by flag 2 into dynamic land (no fade), positioned, sized, hidden
        auto* decal = new cSC4PropOccupantTerrainDecal();
        decal->AddRef();
        cISC4Occupant* occupant = decal;
        occupant->Init();
        float lo[3], hi[3];
        occupant->GetBoundingBox(lo, hi);
        CHECK(lo[0] == -16.0f && hi[2] == 16.0f);
        decal->SetDecalTexture(cGZPersistResourceKey{0x7ab50e44, 0x1abe787d, 0x49c80ec7}, 28.0f);
        occupant->GetBoundingBox(lo, hi);
        CHECK(lo[0] == -14.0f && hi[0] == 14.0f && std::fabs(lo[1] + 0.1f) < 1e-6f && std::fabs(hi[1] - 0.1f) < 1e-6f);
        decal->SetOpacity(0.7f);
        const float position[3] = {936.0f, 260.0f, 1211.0f};
        occupant->SetPosition(position);
        CHECK(decal->OverlayId() == 0xffffffffu && host.dynamicLand.adds == 0);   // not shown yet
        occupant->SetAllFlags(occupant->GetFlags() | 2);
        CHECK(host.dynamicLand.adds == 1 && host.land.adds == 0 && decal->OverlayId() == 7);
        CHECK(host.dynamicLand.key.instance == 0x49c80ec7 && host.dynamicLand.info.width == 28.0f && host.dynamicLand.info.x == 936.0f &&
              host.dynamicLand.info.z == 1211.0f && host.dynamicLand.alpha == 0.7f);
        decal->SetDecalSize(30.0f);
        CHECK(host.dynamicLand.updates == 1 && host.dynamicLand.info.width == 30.0f && host.dynamicLand.info.x == 936.0f);
        occupant->SetAllFlags(occupant->GetFlags() & ~2u);
        CHECK(host.dynamicLand.disables == 1);
        occupant->Shutdown();
        CHECK(host.dynamicLand.removes == 1 && decal->OverlayId() == 0xffffffffu);
        CHECK(decal->Release() == 0);
    }
    {   // a fading decal (rate 0.5 a second >= 0.1): land; the tick moves the fade, action 1 drops the overlay at the end
        auto* decal = new cSC4PropOccupantTerrainDecal();
        decal->AddRef();
        cISC4Occupant* occupant = decal;
        occupant->Init();
        decal->SetDecalTexture(cGZPersistResourceKey{1, 2, 3}, 10.0f);
        decal->SetOpacityFade(2.0f, cISC4PropOccupantTerrainDecal::kRemoveDecal, 1.0f, 0.0f);
        CHECK(host.ticksAdded == 1 && decal->Ticking());
        occupant->SetAllFlags(occupant->GetFlags() | 2);
        CHECK(host.land.adds == 1);
        Tick(*decal, 1000.0f);   // 1 s: 1.0 - 0.5
        CHECK(std::fabs(host.land.alpha - 0.5f) < 1e-5f && std::fabs(decal->Opacity() - 0.5f) < 1e-5f);
        Tick(*decal, 1000.0f);   // reaches 0: the overlay goes
        CHECK(host.land.removes == 1 && decal->OverlayId() == 0xffffffffu);
        occupant->Shutdown();
        CHECK(host.ticksRemoved == 1);
        CHECK(decal->Release() == 0);
    }
    {   // action 2 holds its own reference until the fade ends; the record round-trips version 3
        auto* decal = new cSC4PropOccupantTerrainDecal();
        decal->AddRef();
        cISC4Occupant* occupant = decal;
        occupant->Init();
        decal->SetDecalTexture(cGZPersistResourceKey{4, 5, 6}, 12.0f);
        decal->SetOpacityFade(4.0f, cISC4PropOccupantTerrainDecal::kRemoveOccupant, 0.8f, 0.2f);
        CHECK(decal->AddRef() == 3 && decal->Release() == 2);   // the test's and the fade's
        PortMemoryOStream out;
        CHECK(decal->Write(out));
        auto* back = new cSC4PropOccupantTerrainDecal();
        back->AddRef();
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(back->Read(in));
        CHECK(back->Size() == 12.0f && back->TextureKey().instance == 6 && back->Ticking());
        const std::vector<std::uint8_t> bytes = out.bytes();
        CHECK(bytes.size() > 2 && bytes[0] == 3 && bytes[1] == 0);   // version 3 first (little-endian u16)
        CHECK(back->Release() == 0);
        occupant->Shutdown();   // drops the fade's reference
        CHECK(decal->Release() == 0);
    }
    {   // the burnt floor: the building's box, a quad 0.1 above its floor; flag 2 puts it in the renderer; record version 3
        BurntHost burntHost;
        SetBurntBuildingHost(&burntHost);
        auto* building = new cSC4PropOccupantTerrainDecal();   // any occupant with a box
        building->AddRef();
        const float bmin[3] = {100.0f, 20.0f, 200.0f}, bmax[3] = {116.0f, 40.0f, 230.0f};
        building->SetBoundingBox(bmin, bmax);
        auto* floor = new cSC4PropOccupantBurntBuilding();
        floor->AddRef();
        floor->SetBoundsFromOccupant(building);
        cISC4Occupant* occupant = floor;
        occupant->SetFlag(0x40);
        occupant->Init();
        CHECK(floor->HasFloor() && floor->Texture(2) == 42);
        const auto& quad = floor->Floor();
        CHECK(quad[0].x == 100.0f && quad[0].z == 200.0f && quad[1].x == 116.0f && quad[1].z == 200.0f && quad[2].x == 116.0f && quad[2].z == 230.0f &&
              quad[3].x == 100.0f && quad[3].z == 230.0f && std::fabs(quad[0].y - 20.1f) < 1e-4f);
        occupant->SetAllFlags(occupant->GetFlags() | 2);
        CHECK(burntHost.inserted == 1);
        occupant->SetAllFlags(occupant->GetFlags() & ~2u);
        CHECK(burntHost.removed == 1);
        PortMemoryOStream out;
        CHECK(floor->Write(out));
        auto* back = new cSC4PropOccupantBurntBuilding();
        back->AddRef();
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(back->Read(in) && back->SourceMin()[0] == 100.0f && back->SourceMax()[2] == 230.0f);
        CHECK(back->Release() == 0);
        occupant->Shutdown();
        CHECK(floor->Release() == 0);
        CHECK(building->Release() == 0);
        SetBurntBuildingHost(nullptr);
    }
    SetTerrainDecalHost(nullptr);
    if (failures == 0) std::printf("sc4_propoccupantterraindecal: all passed\n");
    return failures == 0 ? 0 : 1;
}
