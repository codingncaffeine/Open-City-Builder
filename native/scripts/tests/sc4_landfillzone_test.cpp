// cSC4LandfillZone / cGarbageHeap / SC4CellRegion::Inflate with a fake host. Expected values are derived by hand from
// the decompiled formulas (0x1000ac9c0..0x1000b011a).
#include <vector>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "rz/PortMemoryStreams.h"
#include "sc4/LandfillZone.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECKF(a, b) do { if (std::fabs((double)(a) - (double)(b)) > 1e-4) { std::printf("FAIL line %d: %s=%g != %g\n", __LINE__, #a, (double)(a), (double)(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

struct FakeEffect : ILandfillEffect {
    int refs = 1;
    std::map<std::uint32_t, bool> flags;
    bool enabled = true, stopped = false, started = false;
    float matrix[13] = {};
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetFlag(std::uint32_t f, bool on) override { flags[f] = on; }
    void SetEnabled(bool on) override { enabled = on; }
    void SetTransform(const float m[13]) override { for (int i = 0; i < 13; ++i) matrix[i] = m[i]; }
    void Start(int) override { started = true; }
    void Stop(bool) override { stopped = true; }
};
struct FakeProp : ILandfillProp {
    int refs = 1, state = 0;
    float pos[3] = {};
    void AddRef() override { ++refs; }
    void Release() override { --refs; }
    void SetState(int s) override { state = s; }
};
struct FakeTile : ILandfillTile {
    int refs = 1;
    bool removed = false, empty = false, initialised = false;
    float pos[3] = {};
    int layer = -1;
    std::uint32_t id = 0;
    int orientation = 0;
    cSCPropertyHolder props;
    void Release() override { --refs; }
    void RemoveFromCity() override { removed = true; }
    void GetPosition(float xyz[3]) override { for (int i = 0; i < 3; ++i) xyz[i] = pos[i]; }
    void SetPosition(const float xyz[3]) override { for (int i = 0; i < 3; ++i) pos[i] = xyz[i]; }
    bool IsEmptyTile() override { return empty; }
    void SetLayer(int l) override { layer = l; }
    void SetEmpty(bool e) override { empty = e; }
    void Init() override { initialised = true; }
    cISCPropertyHolder* Properties() override { return &props; }
};
struct FakeOcc : ILandfillOccupantRef {
    int kind = 0, refs = 1;
    bool removed = false;
    void Release() override { --refs; }
    void RemoveFromCity() override { removed = true; }
    int TextureKind() override { return kind; }
};

std::vector<FakeProp*> gDeferredProps;   // props freed at the end of main, after the zones that reference them

struct Host : ILandfillZoneHost {
    LandfillTunables tunables;
    int cellsX = 64, cellsZ = 64;
    int tileCapacity = 100;
    std::set<std::pair<int, int>> network, baseTextures;
    std::map<std::pair<int, int>, int> zoneTypes;
    std::map<std::pair<int, int>, FakeTile*> tiles;
    std::vector<std::uint32_t> networkIds;
    std::map<std::uint32_t, std::vector<int>> flagsByType;
    std::vector<std::pair<int, int>> activeOn, activeOff, dezoned, emptyTextures;
    std::vector<std::pair<std::pair<int, int>, std::pair<std::uint32_t, int>>> created;   // (cell, (id, orientation))
    std::vector<FakeTile*> allTiles;
    std::vector<FakeOcc*> incompatible, baseTexList;
    int supply = 0, landfilled = 0, nextRand = 1, rands = 0, refreshes = 0, levelCalls = 0, devCalls = 0;
    LandfillRect lastLevel;
    float lastLevelHeight = 0;
    FakeProp* prop = nullptr;
    FakeEffect generator, attractor, flock;
    bool haveFlock = true;
    std::vector<std::string> effectNames;
    int removedEffects = 0, removedProps = 0;
    float adjust = 0;
    std::pair<int, int> lastTileCell{-1, -1};

    const LandfillTunables& Tunables() override { return tunables; }
    int CityCellsX() override { return cellsX; }
    int CityCellsZ() override { return cellsZ; }
    void CellToWorld(int x, int z, float& wx, float& wz) override { wx = x * 16.0f; wz = z * 16.0f; }
    bool HasNetworkAt(int x, int z) override { return network.count({x, z}) != 0; }
    float AltitudeAt(float wx, float wz) override { return 100.0f + wx * 0.0f + wz * 0.0f; }
    float TerrainHeightAt(float, float) override { return 105.0f; }
    float AdjustedHeight(float, float) override { return adjust; }
    void LevelTerrainRect(const LandfillRect& r, float h) override { lastLevel = r; lastLevelHeight = h; ++levelCalls; }
    void RefreshBuildingDevelopmentRect(const LandfillRect& r) override { lastLevel = r; ++devCalls; }
    int ZoneTypeAt(int x, int z) override { auto it = zoneTypes.find({x, z}); return it == zoneTypes.end() ? 0 : it->second; }
    void DezoneCell(int x, int z) override { dezoned.push_back({x, z}); }
    int LandfillTileCapacity() override { return tileCapacity; }
    void AddToGarbageSupply(int a) override { supply += a; }
    void GarbagePutInLandfill(int a) override { landfilled += a; }
    void SetActiveLandfill(int x, int z, bool on) override { (on ? activeOn : activeOff).push_back({x, z}); }
    std::vector<std::uint32_t> NetworkIdsInRegion(const LandfillCellRegion&) override { return networkIds; }
    std::vector<int> NetworkFlagsForDeveloperType(std::uint32_t t) override { return flagsByType[t]; }
    ILandfillTile* FindLandfillTile(int x, int z) override {
        auto it = tiles.find({x, z});
        if (it == tiles.end()) return nullptr;
        ++it->second->refs;
        return it->second;
    }
    bool HasBaseTexture(int x, int z) override { return baseTextures.count({x, z}) != 0; }
    std::vector<ILandfillOccupantRef*> ListBaseTextures(int, int) override { return {baseTexList.begin(), baseTexList.end()}; }
    std::vector<ILandfillOccupantRef*> ListIncompatibleOccupants(int, int) override { return {incompatible.begin(), incompatible.end()}; }
    ~Host() { for (FakeTile* t : allTiles) delete t; }
    ILandfillTile* LoadLandfillTile(std::uint32_t id, int o) override {
        auto* t = new FakeTile();
        t->id = id;
        t->orientation = o;
        allTiles.push_back(t);
        return t;
    }
    void AddOccupant(ILandfillTile* t) override {
        auto* ft = static_cast<FakeTile*>(t);
        created.push_back({{int(ft->pos[0] / 16), int(ft->pos[2] / 16)}, {ft->id, ft->orientation}});
    }
    void RefreshOccupant(ILandfillTile*) override { ++refreshes; }
    void PlaceEmptyCellTexture(int x, int z) override { emptyTextures.push_back({x, z}); }
    ILandfillProp* CreateZotProp(const float p[3]) override {
        prop = new FakeProp();
        gDeferredProps.push_back(prop);
        for (int i = 0; i < 3; ++i) prop->pos[i] = p[i];
        return prop;
    }
    void RemoveProp(ILandfillProp*) override { ++removedProps; }
    bool WriteProp(cIGZOStream& s, ILandfillProp* p) override { s.SetUint8(p ? 1 : 0); return true; }
    ILandfillProp* ReadProp(cIGZIStream& s) override {
        std::uint8_t present = 0;
        s.GetUint8(present);
        if (!present) return nullptr;
        prop = new FakeProp();
        gDeferredProps.push_back(prop);
        return prop;
    }
    ILandfillEffect* CreateEffect(const char* name, const float p[3]) override {
        effectNames.push_back(name);
        FakeEffect& e = effectNames.size() == 1 ? generator : attractor;
        e.matrix[0] = p[0]; e.matrix[1] = p[1]; e.matrix[2] = p[2];
        return &e;
    }
    void RemoveEffect(ILandfillEffect*) override { ++removedEffects; }
    ILandfillEffect* CreateAmbientEffect(const char* name) override {
        effectNames.push_back(name);
        return haveFlock ? &flock : nullptr;
    }
    int Rand() override { ++rands; return nextRand; }
};

LandfillCellRegion FullRegion(int x0, int z0, int x1, int z1) { return LandfillCellRegion::Make(x0, z0, x1, z1, true); }

// The zone destructor does not free its region; Shutdown does, once the zone was initialised.
struct ZoneRetire {
    cSC4LandfillZone* zone;
    explicit ZoneRetire(cSC4LandfillZone* z) : zone(z) {}
    ~ZoneRetire() { zone->Init(); zone->Shutdown(); }
};

void TestRegion() {
    LandfillCellRegion r = LandfillCellRegion::Make(2, 3, 4, 5, false);
    CHECK(r.rowCount == 3 && r.bitCount == 3 && r.wordCount == 1);
    CHECK(!r.Test(3, 4));
    r.SetBit(3, 4, true);
    CHECK(r.Test(3, 4));
    CHECK(!r.Test(1, 4) && !r.Test(3, 6) && !r.Test(5, 4));
    LandfillCellRegion wide = LandfillCellRegion::Make(0, 0, 0, 31, false);
    CHECK(wide.wordCount == 1);   // 32 bits
    LandfillCellRegion wide2 = LandfillCellRegion::Make(0, 0, 0, 32, false);
    CHECK(wide2.wordCount == 2);  // 33 bits
    // inflate a single cell
    LandfillCellRegion one = LandfillCellRegion::Make(5, 5, 5, 5, true);
    LandfillCellRegion plus = one;
    plus.Inflate(0);
    CHECK(plus.bounds.x0 == 4 && plus.bounds.z0 == 4 && plus.bounds.x1 == 6 && plus.bounds.z1 == 6);
    CHECK(plus.rowCount == 3 && plus.bitCount == 3);
    int count = 0;
    for (int x = 4; x <= 6; ++x) for (int z = 4; z <= 6; ++z) count += plus.Test(x, z) ? 1 : 0;
    CHECK(count == 5);
    CHECK(plus.Test(5, 5) && plus.Test(4, 5) && plus.Test(6, 5) && plus.Test(5, 4) && plus.Test(5, 6));
    CHECK(!plus.Test(4, 4) && !plus.Test(6, 6));
    LandfillCellRegion diag = one;
    diag.Inflate(1);
    count = 0;
    for (int x = 4; x <= 6; ++x) for (int z = 4; z <= 6; ++z) count += diag.Test(x, z) ? 1 : 0;
    CHECK(count == 9);
}

void TestHeapDecay() {
    LandfillTunables t;
    t.decayRate = 0.1f;
    t.maxGarbageToDecayPerMonthPerZone = 5;
    cGarbageHeap h;
    CHECK(h.valid && !h.hasGarbage && h.garbage == 0);
    CHECK(h.DecayGarbage(t) == 0);
    h.garbage = 25;   // 2.5 rounds to even: 2
    CHECK(h.DecayGarbage(t) == 2 && h.garbage == 23);
    h.garbage = 4;    // 0.4 -> 0 -> forced to 1
    CHECK(h.DecayGarbage(t) == 1 && h.garbage == 3);
    h.garbage = 1000;  // 100 -> clamped to 5
    CHECK(h.DecayGarbage(t) == 5 && h.garbage == 995);
    t.decayRate = 1.0f;
    t.maxGarbageToDecayPerMonthPerZone = 100;
    h.garbage = 3;
    CHECK(h.DecayGarbage(t) == 3 && h.garbage == 0);
    CHECK(RoundFloatToEvenInt(3.5f) == 4 && RoundFloatToEvenInt(2.5f) == 2 && RoundFloatToEvenInt(-1.5f) == -2);
}

void TestHeapStream() {
    cGarbageHeap h;
    h.x = 3; h.z = 7; h.valid = false; h.hasGarbage = true; h.garbage = 55; h.capacity = 90;
    PortMemoryOStream out;
    CHECK(h.Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    cGarbageHeap r;
    CHECK(r.Read(in));
    CHECK(r.x == 3 && r.z == 7 && !r.valid && r.hasGarbage && r.garbage == 55 && r.capacity == 90);
    const std::uint8_t bad[2] = {3, 0};
    PortMemoryIStream badIn(bad, 2);
    cGarbageHeap b;
    CHECK(!b.Read(badIn));
}

void TestConstructionAndCapacity() {
    Host host;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 13, 22));   // 4 x 3 cells
    ZoneRetire retire_zone(&zone);
    CHECK(zone.HeapColumns() == 3 && zone.HeapRows() == 2 && zone.Heaps().size() == 6);
    CHECK(zone.Heaps()[4].x == 1 && zone.Heaps()[4].z == 1 && zone.Heaps()[4].id == 1);
    CHECK(zone.GetCellCount() == 12);
    CHECK(zone.GetNumValidHeaps() == 6);
    CHECK(zone.CalculateGarbageHeapCapacity(0) == 0);
    CHECK(zone.CalculateGarbageHeapCapacity(6) == 200);       // 100 * 12 / 6
    CHECK(zone.CalculateGarbageHeapCapacity(5) == 240);       // 1200 / 5
    CHECK(zone.CalculateGarbageHeapCapacity(7) == 171);       // 171.43
    zone.Heaps()[2].valid = false;
    zone.Heaps()[0].garbage = 300;
    zone.Heaps()[1].garbage = 50;
    CHECK(zone.RecalculateCapacity() == 1200);    // 5 valid heaps: 240 each
    CHECK(zone.Heaps()[2].capacity == 0 && zone.Heaps()[3].capacity == 240);
    CHECK(zone.Heaps()[0].garbage == 240 && host.supply == 60);
    CHECK(zone.Heaps()[1].garbage == 50);
    CHECK(zone.TotalCapacity() == 1200);
    int avail, garbage, cap;
    zone.GetCapacity(avail, garbage, cap);
    CHECK(cap == 1200 && garbage == 290 && avail == 910);
    CHECK(zone.GetAvailableCapacity() == 910);
    // redistribution: 290 garbage, 1200 capacity, 5 valid heaps -> share 240 each until exhausted
    zone.RedistributeGarbage();
    CHECK(zone.Heaps()[0].garbage == 240 && zone.Heaps()[1].garbage == 50 && zone.Heaps()[2].garbage == 0);
    CHECK(zone.Heaps()[3].garbage == 0 && zone.Heaps()[4].garbage == 0);
    // Release with refcount 0 destroys the zone: the zone above is on the stack so only use AddRef/QueryInterface
    void* out = nullptr;
    CHECK(zone.QueryInterface(0x452294aa, &out) && out == &zone);
    CHECK(zone.QueryInterface(1, &out));
    CHECK(!zone.QueryInterface(2, &out));
    CHECK(zone.DoMessage(nullptr));
    CHECK(zone.AddRef() == 3);   // two QueryInterface calls added references
    cSC4LandfillZone empty(&host);
    CHECK(empty.Heaps().empty() && empty.GetNumValidHeaps() == 0);
}

void TestUpdateZone() {
    Host host;
    host.tunables.decayRate = 0.0f;
    host.tunables.maxGarbageToDecayPerMonthPerZone = 10;
    host.tunables.maxGarbageToTakePerMonthPerTile = 5;
    host.networkIds = {0, 1};
    host.flagsByType[0x4400] = {0, 1};   // the last developer type answers for network 1
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 12, 22));   // 3x3 cells, 2x2 heaps
    ZoneRetire retire_zone(&zone);
    CHECK(zone.IsZoneConnected());
    host.flagsByType[0x4400] = {0, 0};
    CHECK(!zone.IsZoneConnected());
    host.flagsByType[0x1010] = {1};      // network 0 flagged on the first type
    CHECK(zone.IsZoneConnected());
    host.flagsByType[0x1010] = {};
    host.flagsByType[0x3320] = {0, 0, 1};   // network id 2 is not in the list
    CHECK(!zone.IsZoneConnected());

    // not connected: a zot is added, nothing is taken
    int available = 1000;
    zone.RecalculateCapacity();   // 100*9/4 = 225 each
    CHECK(zone.Heaps()[0].capacity == 225);
    zone.UpdateZone(available);
    CHECK(!zone.Connected() && zone.Zot() != nullptr && host.prop->state == 3);
    CHECK(available == 1000 && host.landfilled == 0);
    // centre of (10..12, 20..22) is (11, 21); height = terrain 105 + 3
    CHECKF(host.prop->pos[0], 11 * 16.0f);
    CHECKF(host.prop->pos[1], 108.0f);
    CHECKF(host.prop->pos[2], 21 * 16.0f);

    // connected: budget = min(1000, 9 cells * 5) = 45; each heap takes min(45 left, 225)
    host.flagsByType[0x4400] = {0, 1};
    host.network.clear();
    available = 1000;
    zone.UpdateZone(available);
    CHECK(zone.Connected() && zone.Zot() == nullptr && host.removedProps == 1);
    CHECK(zone.Heaps()[0].garbage > 0 || zone.Heaps()[1].garbage > 0);
    // the first heap takes all 45 (room 225); the others take 0; then redistribution spreads 45 over 4 valid heaps
    CHECK(available == 955 && host.landfilled == 45 && host.supply == -45);
    // after redistribution: capacity total 900, valid 4 -> share 225; garbage 45 goes to the first heap (min(45, 225))
    CHECK(zone.Heaps()[0].garbage == 45 && zone.Heaps()[1].garbage == 0);
    CHECK(zone.Heaps()[0].hasGarbage && !zone.Heaps()[1].hasGarbage);
    // heap (0,0) now has garbage: UpdateSurroundingTiles created four tiles around the vertex
    CHECK(host.created.size() >= 3);
    // a heap with garbage sets its own bit in all four surrounding masks (4, 2, 8, 1), so no cell gets the empty tile (mask 0)
    CHECK(host.emptyTextures.empty());   // verified against the table at 0x100afef74 and UpdateSurroundingTiles 0x1000adb22
    // a heap with no garbage and no garbage neighbours: all four masks are 0 -> table entry 0 (id 0x2f10000) places the
    // empty cell texture instead of an occupant
    {
        Host hostE;
        cSC4LandfillZone isolated(&hostE, FullRegion(0, 0, 3, 3));
        ZoneRetire retire_isolated(&isolated);
        isolated.UpdateSurroundingTiles(isolated.Heaps()[0]);
        CHECK(hostE.emptyTextures.size() == 4 && hostE.created.empty());
    }

    // budget limited by availability
    available = 3;
    zone.UpdateZone(available);
    CHECK(available == 0 && host.landfilled == 48);
}

void TestSurroundingTiles() {
    Host host;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 13, 22));   // 3x2 heaps
    ZoneRetire retire_zone(&zone);
    zone.Heaps()[1].hasGarbage = true;   // heap (1,0)
    zone.UpdateSurroundingTiles(zone.Heaps()[1]);
    // cells: A (11,20) mask = self<<2 = 4 -> table[4] = 0x34e0000 o1; B (12,20) mask = self<<1 = 2 -> 0x34e0000 o2;
    // C (11,21) mask = self<<3 = 8 -> 0x34e0000 o0; D (12,21) mask = self = 1 -> 0x34e0000 o3
    CHECK(host.created.size() == 4);
    CHECK(host.created[0].first == std::make_pair(11, 20) && host.created[0].second == std::make_pair(0x034e0000u, 1));
    CHECK(host.created[1].first == std::make_pair(12, 20) && host.created[1].second == std::make_pair(0x034e0000u, 2));
    CHECK(host.created[2].first == std::make_pair(11, 21) && host.created[2].second == std::make_pair(0x034e0000u, 0));
    CHECK(host.created[3].first == std::make_pair(12, 21) && host.created[3].second == std::make_pair(0x034e0000u, 3));
    CHECK(host.activeOff.size() == 4 && host.activeOn.size() == 4);
    for (FakeTile* t : host.allTiles) CHECK(t->layer == 2 && !t->empty && t->initialised && t->refs == 0);
    CHECK(host.allTiles[0]->pos[1] == 100.0f);
    // neighbouring heaps with garbage change the masks: heaps (0,0) and (2,0) too -> A sees (0,0) as bit1 etc.
    host.created.clear();
    zone.Heaps()[0].hasGarbage = true;
    zone.Heaps()[2].hasGarbage = true;
    zone.UpdateSurroundingTiles(zone.Heaps()[1]);
    // A (hx=1,hz=0): (0,-1)=0 | (0,0)<<1 = 2 | self<<2 = 4 | (1,-1)<<3 = 0 -> 6 -> 0x2f90000 o2
    // B: (1,-1)=0 | self<<1 = 2 | (2,0)<<2 = 4 | (2,-1)<<3 = 0 -> 6
    // C: (0,0)=1 | (0,1)<<1 = 0 | (1,1)<<2 = 0 | self<<3 = 8 -> 9 -> 0x2f90000 o0
    // D: self=1 | (1,1)<<1 = 0 | (2,1)<<2 = 0 | (2,0)<<3 = 8 -> 9
    CHECK(host.created.size() == 4);
    CHECK(host.created[0].second == std::make_pair(0x02f90000u, 2));
    CHECK(host.created[1].second == std::make_pair(0x02f90000u, 2));
    CHECK(host.created[2].second == std::make_pair(0x02f90000u, 0));
    CHECK(host.created[3].second == std::make_pair(0x02f90000u, 0));
    // all-neighbour tile re-randomises: heap (1,1) with all four heaps lit -> cell D of... use heap (1,0) region
    cSC4LandfillZone big(&host, FullRegion(0, 0, 4, 4));   // 4x4 heaps
    ZoneRetire retire_big(&big);
    for (auto& h : big.Heaps()) h.hasGarbage = true;
    host.created.clear();
    host.nextRand = 7;   // 7 & 3 = 3 -> orientation 6
    big.UpdateSurroundingTiles(big.Heaps()[5]);   // heap (1,1): A (1,1) mask 15
    CHECK(host.created.size() == 4);
    CHECK(host.created[0].second == std::make_pair(0x02fa0000u, 6));
    CHECK(host.rands == 4);
    // an existing tile and incompatible occupants are removed first
    FakeTile existing;
    host.tiles[{1, 1}] = &existing;
    FakeOcc occ;
    host.incompatible.push_back(&occ);
    host.created.clear();
    big.UpdateSurroundingTiles(big.Heaps()[5]);
    CHECK(existing.removed && occ.removed && occ.refs == 1 - 4);
}

void TestEmptyTileAndMissingExemplar() {
    Host host;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 13, 22));
    ZoneRetire retire_zone(&zone);
    // lone heap state false everywhere: cell mask 0 is the empty tile id -> empty texture placed when none exists
    zone.UpdateSurroundingTiles(zone.Heaps()[0]);   // hasGarbage false: masks all 0
    CHECK(host.emptyTextures.size() == 4 && host.created.empty());
    host.baseTextures.insert({10, 20});
    host.emptyTextures.clear();
    zone.UpdateSurroundingTiles(zone.Heaps()[0]);
    CHECK(host.emptyTextures.size() == 3);
}

void TestEdges() {
    Host host;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 13, 22));
    ZoneRetire retire_zone(&zone);
    // invalidation: cell (11, 21) -> heaps (1,1) (1,0) (0,1) (0,0)
    zone.InvalidateSurroundingHeaps(11, 21);
    CHECK(!zone.Heaps()[4].valid && !zone.Heaps()[1].valid && !zone.Heaps()[3].valid && !zone.Heaps()[0].valid);
    CHECK(zone.Heaps()[2].valid && zone.Heaps()[5].valid);
    // the last column (13): hx = 3 = columns -> only (hx-1, ...) heaps
    cSC4LandfillZone z2(&host, FullRegion(10, 20, 13, 22));
    ZoneRetire retire_z2(&z2);
    z2.InvalidateSurroundingHeaps(13, 22);   // hx = 3, hz = 2 = rows: (hx-1, hz-1) = (2,1)
    int invalid = 0;
    for (auto& h : z2.Heaps()) invalid += h.valid ? 0 : 1;
    CHECK(invalid == 1 && !z2.Heaps()[5].valid);
    z2.InvalidateSurroundingHeaps(9, 20);   // hx < 0
    invalid = 0;
    for (auto& h : z2.Heaps()) invalid += h.valid ? 0 : 1;
    CHECK(invalid == 1);

    // first road neighbour: network at (9, 21) left of the zone
    host.network.insert({9, 21});
    int x = 0, z = 0;
    CHECK(zone.GetFirstRoadTileNeighbor(x, z) && x == 9 && z == 21);
    host.network.clear();
    host.network.insert({11, 19});   // top edge row z0-1
    CHECK(zone.GetFirstRoadTileNeighbor(x, z) && x == 11 && z == 19);
    host.network.clear();
    host.network.insert({14, 20});   // right edge: x1+1
    CHECK(zone.GetFirstRoadTileNeighbor(x, z) && x == 14 && z == 20);
    host.network.clear();
    host.network.insert({11, 23});   // bottom edge z1+1
    CHECK(zone.GetFirstRoadTileNeighbor(x, z) && x == 11 && z == 23);
    host.network.clear();
    host.network.insert({14, 19});   // top-right corner: missed by the top loop (x < xHigh) but the right edge loop starts at zLow = z0-1
    CHECK(zone.GetFirstRoadTileNeighbor(x, z) && x == 14 && z == 19);   // verified against 0x1000af650
    host.network.clear();
    host.network.insert({15, 20});   // two cells outside the zone: not scanned; failure returns the region's (x0, z0)
    CHECK(!zone.GetFirstRoadTileNeighbor(x, z) && x == 10 && z == 20);
    // (13, 19) is on the top-edge row and is hit first
    host.network.insert({13, 19});
    CHECK(zone.GetFirstRoadTileNeighbor(x, z) && x == 13 && z == 19);
}

void TestInitShutdown() {
    Host host;
    host.tunables.garbageTruckGeneratorName = "gen";
    host.tunables.garbageTruckAttractorName = "att";
    host.network.insert({9, 21});
    auto* zone = new cSC4LandfillZone(&host, FullRegion(10, 20, 13, 22));
    zone->AddRef();
    CHECK(zone->Init());
    CHECK(zone->FirstRoadX() == 9 && zone->FirstRoadZ() == 21);
    CHECK(host.effectNames.size() == 3 && host.effectNames[0] == "gen" && host.effectNames[1] == "att" &&
          host.effectNames[2] == "flock_of_seagulls");
    CHECK(host.generator.flags[8] && host.generator.flags[0x100] && host.attractor.flags[8]);
    CHECKF(host.generator.matrix[0], 9 * 16.0f);
    CHECKF(host.generator.matrix[1], 100.0f);
    CHECKF(host.generator.matrix[2], 21 * 16.0f);
    CHECK(host.generator.refs == 2 && host.attractor.refs == 2);
    CHECK(host.flock.started);
    // seagull transform: centre cell (11, 21), height altitude + 64
    CHECKF(host.flock.matrix[0], 1); CHECKF(host.flock.matrix[4], 1); CHECKF(host.flock.matrix[8], 1);
    CHECKF(host.flock.matrix[9], 11 * 16.0f); CHECKF(host.flock.matrix[10], 164.0f);
    CHECKF(host.flock.matrix[11], 21 * 16.0f); CHECKF(host.flock.matrix[12], 1);
    CHECKF(zone->LevelHeight(), 100.0f);
    CHECK(zone->Init());   // second call does nothing
    CHECK(host.effectNames.size() == 3);
    // AddZot, then shutdown removes it
    zone->AddZot();
    CHECK(zone->Zot() != nullptr);
    FakeProp* prop = host.prop;
    // Release at refcount 1 shuts down and deletes
    CHECK(zone->Release() == 0);
    CHECK(host.flock.stopped && host.flock.refs == 0);
    CHECK(!host.generator.enabled && !host.attractor.enabled && host.removedEffects == 2);
    CHECK(host.generator.refs == 1 && host.attractor.refs == 1);
    CHECK(host.removedProps == 1 && prop->refs == 0);

    // no seagull effect available
    Host h2;
    h2.haveFlock = false;
    auto* z2 = new cSC4LandfillZone(&h2, FullRegion(0, 0, 3, 3));
    z2->Init();
    CHECK(z2->Seagulls() == nullptr && z2->TruckGenerator() != nullptr);
    z2->Release();
}

void TestZotSpiral() {
    Host host;
    // region where the centre cell is missing: spiral finds the nearest set cell
    LandfillCellRegion r = LandfillCellRegion::Make(10, 20, 14, 24, false);
    r.SetBit(12, 24, true);
    cSC4LandfillZone zone(&host, r);
    ZoneRetire retire_zone(&zone);
    zone.AddZot();
    CHECK(zone.Zot() != nullptr);
    CHECKF(host.prop->pos[0], 12 * 16.0f);
    CHECKF(host.prop->pos[2], 24 * 16.0f);
    // empty region: fall back to the centre
    Host h2;
    LandfillCellRegion e = LandfillCellRegion::Make(10, 20, 14, 24, false);
    cSC4LandfillZone ez(&h2, e);
    ZoneRetire retire_ez(&ez);
    ez.AddZot();
    CHECKF(h2.prop->pos[0], 12 * 16.0f);
    CHECKF(h2.prop->pos[2], 22 * 16.0f);
    // the zones still hold a reference to their props: free the props only after the zones are gone
}

void TestCostsAndTerrain() {
    Host host;
    host.tunables.defaultLandfillTileCost = 7;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 11, 21));   // 4 cells
    ZoneRetire retire_zone(&zone);
    CHECK(zone.GetLandfillTileCost(10, 20) == 7);
    FakeTile tile;
    cSCPropertyHelp::SetPropertyValue(&tile.props, kLandfillTileCostProperty, std::int64_t(50));
    host.tiles[{10, 20}] = &tile;
    CHECK(zone.GetLandfillTileCost(10, 20) == 50 && tile.refs == 1);
    CHECK(zone.GetMaintenanceCost() == 50 + 7 * 3);

    // terrain change: tile at height 100, terrain 105 -> moves when allowed
    tile.pos[0] = 160; tile.pos[1] = 100; tile.pos[2] = 320;
    host.adjust = 104;
    zone.HandleCellTerrainChange(10, 20);
    CHECK(tile.pos[1] == 100.0f && host.refreshes == 0);   // moveWithTerrain off
    host.tunables.moveWithTerrain = true;
    zone.HandleCellTerrainChange(10, 20);
    CHECK(tile.pos[1] == 105.0f && host.refreshes == 1);   // max(terrain 105, adjusted 104)
    host.adjust = 110;
    tile.pos[1] = 100;
    zone.HandleCellTerrainChange(10, 20);
    CHECK(tile.pos[1] == 110.0f);
    tile.pos[1] = 105.1f;   // |diff| < 0.25: untouched
    zone.HandleCellTerrainChange(10, 20);
    CHECK(tile.pos[1] == 105.1f);
    // an "empty" tile gets dezoned
    tile.empty = true;
    tile.pos[1] = 100;
    zone.HandleCellTerrainChange(10, 20);
    CHECK(!zone.Region()->Test(10, 20) && host.dezoned.size() == 1 && tile.removed);
    CHECK(zone.GetCellCount() == 3);
    // a missing tile dezones its cell as well
    zone.HandleTerrainChange({11, 21, 30, 30});
    CHECK(!zone.Region()->Test(11, 21) && host.dezoned.size() == 2);
    CHECK(zone.GetCellCount() == 2);
    CHECK(!zone.IsCompletelyDezoned());
    zone.DezoneCell(10, 21, false, false);
    zone.DezoneCell(11, 20, false, false);
    CHECK(host.dezoned.size() == 2);
    CHECK(zone.GetCellCount() == 0);
    CHECK(zone.IsCompletelyDezoned());
    // base textures: only kind 1 are removed
    FakeOcc a, b;
    a.kind = 1; b.kind = 2;
    host.baseTexList = {&a, &b};
    zone.DezoneCell(10, 20, false, true);
    CHECK(a.removed && !b.removed && a.refs == 0 && b.refs == 0);
}

void TestLevelAndPopulate() {
    Host host;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 12, 22));
    ZoneRetire retire_zone(&zone);
    zone.LevelRect({-3, 5, 100, 100}, 12.5f, true);
    CHECKF(zone.LevelHeight(), 12.5f);
    CHECK(host.lastLevel.x0 == 0 && host.lastLevel.z0 == 5 && host.lastLevel.x1 == 64 && host.lastLevel.z1 == 64);
    CHECKF(host.lastLevelHeight, 12.5f);
    zone.LevelRect({-3, 5, 7, 8}, 1.0f, false);
    CHECK(host.devCalls == 1 && host.lastLevel.x0 == -3 && host.lastLevel.z1 == 8);

    // populate: cells with zone type 0xe and no network keep their bit and get an empty texture; others lose their bit
    host.zoneTypes[{10, 20}] = 0xe;
    host.zoneTypes[{11, 20}] = 0xe;
    host.network.insert({11, 20});
    host.zoneTypes[{10, 21}] = 3;
    FakeTile t;
    host.tiles[{10, 20}] = &t;
    host.baseTextures.insert({12, 22});
    zone.PopulateZone();
    CHECK(zone.Region()->Test(10, 20));          // zone type 0xe, tile present
    CHECK(!zone.Region()->Test(11, 20));         // network present
    CHECK(!zone.Region()->Test(10, 21));         // wrong zone type
    CHECK(!zone.Region()->Test(12, 22));
    CHECK(t.refs == 1);
    CHECK(host.emptyTextures.empty());
    // a cell whose bit was cleared stays cleared (PopulateZone only ever clears bits; verified against 0x1000aff38)
    host.zoneTypes[{12, 21}] = 0xe;
    zone.PopulateZone();
    CHECK(zone.Region()->Test(12, 21) == false);
    CHECK(host.emptyTextures.empty());
    // a set cell with zone type 0xe, no network, no tile and no base texture gets the empty cell texture and keeps its bit
    cSC4LandfillZone single(&host, FullRegion(12, 21, 12, 21));
    ZoneRetire retire_single(&single);
    single.PopulateZone();
    CHECK(single.Region()->Test(12, 21) == true);
    CHECK(host.emptyTextures.size() == 1 && host.emptyTextures[0] == std::make_pair(12, 21));
}

void TestAddRegion() {
    Host host;
    cSC4LandfillZone zone(&host, FullRegion(10, 20, 12, 22));   // 2x2 heaps
    ZoneRetire retire_zone(&zone);
    zone.SetLevelHeight(9.0f);
    zone.Heaps()[3].garbage = 77;
    zone.Heaps()[3].hasGarbage = true;
    zone.Heaps()[3].capacity = 99;
    LandfillCellRegion extra = LandfillCellRegion::Make(13, 20, 14, 21, false);
    extra.SetBit(13, 20, true);
    extra.SetBit(14, 21, true);
    zone.AddRegionToZone(extra);
    const LandfillRect& b = zone.Region()->bounds;
    CHECK(b.x0 == 10 && b.z0 == 20 && b.x1 == 14 && b.z1 == 22);
    CHECK(zone.HeapColumns() == 4 && zone.HeapRows() == 2 && zone.Heaps().size() == 8);
    CHECK(zone.Region()->Test(10, 20) && zone.Region()->Test(13, 20) && zone.Region()->Test(14, 21));
    CHECK(!zone.Region()->Test(14, 20) && !zone.Region()->Test(13, 21));
    // old heap (1,1) is copied by its own x/z
    const cGarbageHeap& moved = zone.Heaps()[1 * 4 + 1];
    CHECK(moved.garbage == 77 && moved.hasGarbage && moved.capacity == 99 && moved.x == 1 && moved.z == 1);
    CHECK(host.devCalls == 2);   // one LevelRect per added cell, using the zone's level height
    CHECK(host.lastLevel.x0 == 14 && host.lastLevel.z0 == 21 && host.lastLevel.x1 == 15 && host.lastLevel.z1 == 22);
    CHECK(zone.GetCellCount() == 9 + 2);
    // cells outside the merged region invalidate surrounding heaps: (14,20) lies outside -> heaps around it
    CHECK(!zone.Heaps()[0 * 4 + 3].valid);
}

void TestStream() {
    Host host;
    host.tunables.garbageTruckGeneratorName = "gen";
    LandfillCellRegion r = FullRegion(10, 20, 13, 22);
    r.SetBit(11, 21, false);
    r.SetBit(13, 22, false);   // last row/column: not stored, reloaded as set
    r.SetBit(10, 22, false);   // last column of the first row: not stored either
    cSC4LandfillZone zone(&host, r);
    ZoneRetire retire_zone(&zone);
    zone.Heaps()[2].garbage = 12;
    zone.Heaps()[2].capacity = 40;
    zone.Heaps()[2].hasGarbage = true;
    zone.Heaps()[5].valid = false;
    int available = 0;
    host.network.clear();
    zone.UpdateZone(available);   // not connected -> zot
    CHECK(zone.Zot() != nullptr);
    // UpdateZone decays and redistributes garbage (verified against 0x1000ae124), so round-trip whatever the heap holds now
    const cGarbageHeap savedHeap2 = zone.Heaps()[2];
    PortMemoryOStream out;
    CHECK(zone.Write(out));
    // layout: u16 version 4, 4 x s32, 3*2 bit bytes, s32 columns, s32 rows, 6 heaps of 2+4+4+2+2+4+4 = 22 bytes, bool, prop byte
    CHECK(out.bytes().size() == 2 + 16 + 6 + 8 + 6 * 22 + 1 + 1);
    CHECK(out.bytes()[0] == 4 && out.bytes()[1] == 0);

    Host host2;
    cSC4LandfillZone loaded(&host2);
    ZoneRetire retire_loaded(&loaded);
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(loaded.Read(in));
    CHECK(loaded.Region()->bounds.x0 == 10 && loaded.Region()->bounds.z1 == 22);
    CHECK(loaded.Region()->Test(10, 20) && !loaded.Region()->Test(11, 21) && loaded.Region()->Test(12, 21));
    CHECK(loaded.Region()->Test(13, 22) && loaded.Region()->Test(10, 22));   // reloaded as set
    CHECK(loaded.HeapColumns() == 3 && loaded.HeapRows() == 2 && loaded.Heaps().size() == 6);
    CHECK(loaded.Heaps()[2].garbage == savedHeap2.garbage && loaded.Heaps()[2].capacity == savedHeap2.capacity && loaded.Heaps()[2].hasGarbage == savedHeap2.hasGarbage);
    CHECK(!loaded.Heaps()[5].valid && loaded.Heaps()[4].valid);
    CHECK(loaded.Heaps()[4].id == 4);
    CHECK(!loaded.Connected() && loaded.Zot() != nullptr);

    // version 3 stream: connected flag byte only
    PortMemoryOStream v3;
    v3.SetUint16(3);
    v3.SetSint32(0); v3.SetSint32(0); v3.SetSint32(1); v3.SetSint32(1);
    v3.SetUint8(1);
    v3.SetSint32(1); v3.SetSint32(1);
    cGarbageHeap h;
    h.Write(v3);
    v3.SetUint8(1);
    PortMemoryIStream v3in(v3.bytes().data(), v3.bytes().size());
    Host host3;
    cSC4LandfillZone old(&host3);
    ZoneRetire retire_old(&old);
    CHECK(old.Read(v3in) && old.Connected() && old.Zot() == nullptr);
}

}  // namespace

int main() {
    TestRegion();
    TestHeapDecay();
    TestHeapStream();
    TestConstructionAndCapacity();
    TestUpdateZone();
    TestSurroundingTiles();
    TestEmptyTileAndMissingExemplar();
    TestEdges();
    TestInitShutdown();
    TestZotSpiral();
    TestCostsAndTerrain();
    TestLevelAndPopulate();
    TestAddRegion();
    TestStream();
    for (FakeProp* p : gDeferredProps) delete p;
    if (failures) { std::printf("%d failures\n", failures); return 1; }
    std::printf("ok\n");
    return 0;
}
