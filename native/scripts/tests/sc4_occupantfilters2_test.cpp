#include <cstdio>

#include "fake_occupant.h"
#include "rz/PortMemoryStreams.h"
#include "sc4/OccupantFilters2.h"
#include "sc4/SCProperty.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
struct World final : IOccupantFilterWorld {
    bool isNetwork = true, placed = true; std::uint32_t flags = 0x10; int level = 2;
    bool NetworkFlags(cISC4Occupant*, std::uint32_t& f) override { f = flags; return isNetwork; }
    bool NetworkIsPlaced(cISC4Occupant*) override { return placed; }
    int OccupantViewLevel(cISC4Occupant*) override { return level; }
    int lotState = -1;
    int LotStateAtPosition(float, float) override { return lotState; }
};
struct Inner final : cSC4OccupantFilter {
    bool answer = true;
    bool IsOccupantIncluded(cISC4Occupant*) override { return answer; }
};
}  // namespace

int main() {
    World world;
    SetOccupantFilterWorld(&world);
    FakeOccupant occ;

    cSC4OccupantTypesFilter types;
    types.AddType(5); types.AddType(9);
    CHECK(types.IsOccupantTypeIncluded(9) && !types.IsOccupantTypeIncluded(6));
    PortMemoryOStream os;
    CHECK(types.Write(os));
    cSC4OccupantTypesFilter read;
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    CHECK(read.Read(is) && read.IsOccupantTypeIncluded(5) && read.IsOccupantTypeIncluded(9) && !read.IsOccupantTypeIncluded(1));
    CHECK(types.GetGZCLSID() == 0xe9c75ee8);

    cSC4OccupantTypesFilterHashed hashed;
    hashed.AddType(7); hashed.AddOccupantType(8); hashed.AddOccupantType(8);
    PortMemoryOStream os2;
    CHECK(hashed.Write(os2));
    cSC4OccupantTypesFilterHashed hashedRead;
    PortMemoryIStream is2(os2.bytes().data(), os2.bytes().size());
    CHECK(hashedRead.Read(is2) && hashedRead.IsOccupantTypeIncluded(7) && hashedRead.IsOccupantTypeIncluded(8) && !hashedRead.IsOccupantTypeIncluded(9));

    cSC4ViewLevelOccupantFilter level;
    level.SetViewLevel(2);
    CHECK(level.IsOccupantIncluded(&occ));
    world.level = 3; CHECK(!level.IsOccupantIncluded(&occ));
    PortMemoryOStream os3; CHECK(level.Write(os3));
    cSC4ViewLevelOccupantFilter levelRead;
    PortMemoryIStream is3(os3.bytes().data(), os3.bytes().size());
    CHECK(levelRead.Read(is3));
    world.level = 2; CHECK(levelRead.IsOccupantIncluded(&occ) && levelRead.GetGZCLSID() == 0xaac3c01d);

    cSC4NetworkOccupantFilter network;
    network.SetMask(0x30);
    CHECK(network.IsOccupantIncluded(&occ));          // flags 0x10 overlap the mask
    world.flags = 0x1; CHECK(!network.IsOccupantIncluded(&occ));
    world.isNetwork = false; world.flags = 0x10; CHECK(!network.IsOccupantIncluded(&occ));
    PortMemoryOStream os4; CHECK(network.Write(os4));
    cSC4NetworkOccupantFilter networkRead;
    PortMemoryIStream is4(os4.bytes().data(), os4.bytes().size());
    CHECK(networkRead.Read(is4) && networkRead.GetGZCLSID() == 0xeac3a3d2);

    world.isNetwork = true; world.flags = 0x4;
    auto* surface = new cSC4SurfaceNetworkOccupantFilter();
    surface->AddRef();
    CHECK(surface->IsOccupantIncluded(&occ) && !surface->IsOccupantTypeIncluded(1));
    world.flags = 0x100000; CHECK(!surface->IsOccupantIncluded(&occ));
    void* out = nullptr;
    CHECK(surface->QueryInterface(kGZIID_cISC4OccupantFilter, &out) && out == surface);
    surface->Release();
    CHECK(surface->Release() == 0);

    auto* inner = new Inner();
    inner->AddRef();
    auto* placed = new cSC4PlacedNetworkOccupantFilter(inner);
    placed->AddRef();
    CHECK(placed->IsOccupantIncluded(&occ));
    world.placed = false; CHECK(!placed->IsOccupantIncluded(&occ));
    world.placed = true; inner->answer = false; CHECK(!placed->IsOccupantIncluded(&occ));
    CHECK(!cSC4PlacedNetworkOccupantFilter::IsOccPlaced(nullptr));
    placed->Release();
    inner->Release();
    // cSC4RealisticOccupantFilter (i386 0x000795c8): zots, empty-lot textures and signposts hidden unless their flag is set
    {
        cSC4RealisticOccupantFilter realistic;
        cSCPropertyHolder empty;   // the default filter reads the occupant's properties
        FakeOccupant zot;
        zot.holder = &empty;
        zot.group = 0x5001;
        CHECK(!realistic.IsOccupantIncluded(&zot));
        realistic.showZots = true;
        CHECK(realistic.IsOccupantIncluded(&zot));
        FakeOccupant texture;
        texture.type = 0x497f6d9d;
        texture.holder = &empty;
        texture.iface = 0x097f6d98;
        world.lotState = 1;   // an empty lot
        CHECK(!realistic.IsOccupantIncluded(&texture));
        world.lotState = 3;   // control: a built lot keeps its texture
        CHECK(realistic.IsOccupantIncluded(&texture));
        world.lotState = 1;
        realistic.showEmptyLotTextures = true;
        CHECK(realistic.IsOccupantIncluded(&texture));
        FakeOccupant sign;
        sign.type = 0xa823821e;
        sign.holder = &empty;
        sign.iface = 0x4b44fbe2;
        CHECK(!realistic.IsOccupantIncluded(&sign));
        sign.iface = 0;   // control: another prop shows
        CHECK(realistic.IsOccupantIncluded(&sign));
        PortMemoryOStream out;
        realistic.showSigns = true;
        CHECK(realistic.Write(out));
        cSC4RealisticOccupantFilter back;
        PortMemoryIStream in(out.bytes().data(), out.bytes().size());
        CHECK(back.Read(in) && back.showEmptyLotTextures && back.showZots && back.showSigns);
        CHECK(back.GetGZCLSID() == 0x8a7a1c2c);
    }
    std::printf(failures ? "FAILED\n" : "sc4_occupantfilters2 ok\n");
    return failures != 0;
}
