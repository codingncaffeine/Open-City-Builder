// cSC4Demolition: tuning, save record, occupant dispatch per type, lot union mask, network follow-up.
#include <cstdio>
#include <map>
#include <set>

#include "rz/PortMemoryStreams.h"
#include "sc4/Demolition.h"
#include "sc4/SCProperty.h"

using namespace sc4;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Occ final : IDemolishable {
    std::uint32_t type = 0, flags = 0;
    bool protectedFlag = false, hasNet = false, removed = false, plain = true;
    float box[6] = {0, 0, 0, 10, 10, 10};
    std::uint32_t Type() override { return type; }
    bool HasRemovalFlag(std::uint32_t f) override { return f == 0x40 ? protectedFlag : hasNet; }
    std::uint32_t Flags() override { return flags; }
    void Remove() override { removed = true; }
    void BoundingBox(float out[6]) override { for (int i = 0; i < 6; ++i) out[i] = box[i]; }
    cISCPropertyHolder* Properties() override { return nullptr; }
    bool TestFlag(std::uint32_t) override { return false; }
    bool NetworkHasFlag(int, bool* isNetwork) override { if (isNetwork) *isNetwork = false; return false; }
};
struct Lot final : IDemolishableLot {
    DemolitionCellRect r; bool plopped = true; int type = 0, zone = 0;
    std::vector<IDemolishable*> occupants;
    DemolitionCellRect Bounds() override { return r; }
    bool IsPlopped() override { return plopped; }
    std::vector<IDemolishable*> Occupants() override { return occupants; }
    int TypeCode() override { return type; }
    int ZoneClass() override { return 0; }
    int ZoneType() override { return zone; }
    std::uint8_t Orientation() override { return 0; }
    void Size(std::uint8_t& w, std::uint8_t& d) override { w = std::uint8_t(r.x1 - r.x0 + 1); d = std::uint8_t(r.z1 - r.z0 + 1); }
    IDemolishable* BuildingOccupant() override { return nullptr; }
    bool BuildingRemovable(int) override { return true; }
    bool BuildingPlain() override { return true; }
    bool HasBuildingObject() override { return false; }
    bool BlocksTerrainChange() override { return false; }
};
struct Effect final : IDemolitionEffect {
    int played = 0;
    bool Attach(IDemolishable*) override { return true; }
    void SetViewpoint(int, int) override {}
    void SetGrayTint() override {}
    void Play() override { ++played; }
};
struct Host final : IDemolitionHost {
    cSCPropertyHolder thresholds, costs;
    std::vector<std::uint32_t> added, removedNotes;
    std::vector<IDemolishable*> neighborCalls, managerRemoved, networkBatch;
    std::vector<std::uint32_t> posted, effects;
    bool propOk = true, propRemoveOk = true;
    std::vector<Lot*> lots;
    std::vector<std::pair<int, int>> visited;
    std::vector<IDemolishableLot*> prepared;
    std::vector<IDemolishable*> terrainOccupants;
    int levelRects = 0, levelCells = 0, updates = 0, batches = 0;
    Effect effect;
    void AddNotification(std::uint32_t m) override { added.push_back(m); }
    void RemoveNotification(std::uint32_t m) override { removedNotes.push_back(m); }
    void PostOccupantMessage(std::uint32_t m, IDemolishable*, bool) override { posted.push_back(m); }
    cISCPropertyHolder* TuningExemplar(std::uint32_t group, std::uint32_t) override { return group == 0x690f693f ? &thresholds : &costs; }
    void CityCells(int& x, int& z) override { x = 64; z = 32; }
    IDemolishFilter* ViewOccupantFilter() override { return nullptr; }
    void ViewBeginSweep() override {}
    bool OccupantAllowedByFilter(IDemolishable*, const DemolitionFilterState&) override { return true; }
    bool LocalFilterIncludes(IDemolishable*) override { return true; }
    std::vector<IDemolishable*> OccupantsInRect(const DemolitionCellRect&, const DemolitionFilterState&) override { return {}; }
    std::vector<IDemolishable*> OccupantsForTerrain(const DemolitionCellRect&, int mode) override {
        return mode < 0 ? terrainOccupants : std::vector<IDemolishable*>{};
    }
    bool RemoveOccupant(IDemolishable* o, bool, bool) override { managerRemoved.push_back(o); return true; }
    void ReleaseOccupantList(std::vector<IDemolishable*>&) override {}
    void OccupantManagerCentre(int, int, float& a, float& b) override { a = b = 0; }
    IDemolishableLot* LotAtCell(int x, int z) override {
        visited.emplace_back(x, z);
        for (Lot* l : lots)
            if (x >= l->r.x0 && x <= l->r.x1 && z >= l->r.z0 && z <= l->r.z1) return l;
        return nullptr;
    }
    IDemolishableLot* LotOfOccupant(IDemolishable*) override { return nullptr; }
    std::vector<IDemolishableLot*> LotsInRect(const DemolitionCellRect&) override { return {}; }
    void RemoveLot(IDemolishableLot*) override {}
    void CreateLotAt(int, int, std::uint8_t) override {}
    int ZoneTypeAt(int, int) override { return 0; }
    void ClearZones(const DemolitionCellRect&) override {}
    void LotPrepareRemoval(IDemolishableLot* l) override { prepared.push_back(l); }
    void LotFinishRemoval(IDemolishableLot*) override {}
    void LotSkipRemoval(IDemolishableLot*, bool) override {}
    void NetworkBatch(bool) override { ++batches; }
    void UpdateNetworkManager(const std::vector<IDemolishable*>& n) override { networkBatch = n; }
    void NotifyNeighborsNetworkChange(IDemolishable* n) override { neighborCalls.push_back(n); }
    float TerrainHeight(int, int) override { return 0.0f; }
    bool TerrainCellSkipped(int, int) override { return false; }
    void TerrainLevelCell(int, int) override { ++levelCells; }
    void TerrainLevelRect(const DemolitionCellRect&, std::uint32_t) override { ++levelRects; }
    void TerrainUpdateRect(const DemolitionCellRect&, std::uint32_t) override { ++updates; }
    float TerrainHeightAtMetres(float, float) override { return 0.0f; }
    bool PropManagerAvailable() override { return propOk; }
    bool PropManagerRemove(IDemolishable*) override { return propRemoveOk; }
    bool NetworkCost(IDemolishable*, std::int64_t& c) override { c = 25; return true; }
    bool BuildingCost(IDemolishable*, std::int64_t& c) override { c = 100; return true; }
    bool FloraCost(IDemolishable*, std::int64_t&) override { return false; }
    bool PoleCost(IDemolishable*, std::int64_t& c) override { c = 7; return true; }
    std::vector<IDemolishable*> PoleConnections(IDemolishable*) override { return conns; }
    void PoleDetach(IDemolishable*) override {}
    bool IsPlainOccupant(IDemolishable*) override { return true; }
    std::uint32_t missingAbove = 0xffffffffu;   // effect indices above this one are absent from the effect directory
    IDemolitionEffect* CreateEffect(std::uint32_t, std::uint32_t index) override {
        effects.push_back(index);
        return index > missingAbove ? nullptr : &effect;
    }
    void ReleaseEffect(IDemolitionEffect*) override {}
    void PlayGroupEffect(std::uint32_t, float, float, float) override {}
    void PlaySound(std::uint32_t) override {}
    float RenderMinimumSize() override { return 0.0f; }
    float ViewScale() override { return 1.0f; }
    std::vector<IDemolishable*> conns;
};
}  // namespace

int main() {
    Host host;
    cSCPropertyHelp::SetPropertyValue(&host.thresholds, kPropertyModelSmallMedium, 1.5f);
    cSCPropertyHelp::SetPropertyValue(&host.thresholds, kPropertyGroupMediumLarge, 9.0f);
    cSCPropertyHelp::SetPropertyValue(&host.costs, kPropertyDemolishLotTileCost, std::int32_t(12));
    cSC4Demolition d(&host);
    CHECK(d.Init());
    CHECK(host.added.size() == 3);
    CHECK(d.CityCellsX() == 64 && d.CityCellsZ() == 32);
    CHECK(d.SmallMediumModelThreshold() == 1.5f && d.MediumLargeGroupThreshold() == 9.0f && d.CostToDemolishDestroyedLotTile() == 12);
    cSCPropertyHelp::SetPropertyValue(&host.costs, kPropertyDemolishLotTileCost, std::int32_t(20));
    CHECK(d.DoMessage(kMessageTuningReloaded) && d.CostToDemolishDestroyedLotTile() == 20);
    CHECK(!d.DoMessage(0x1234));

    PortMemoryOStream out;
    CHECK(d.Write(out));
    PortMemoryIStream in(out.bytes().data(), out.bytes().size());
    CHECK(d.Read(in));
    PortMemoryOStream bad; bad.SetUint16(2);
    PortMemoryIStream badIn(bad.bytes().data(), bad.bytes().size());
    CHECK(!d.Read(badIn));

    Occ prop; prop.type = kDemolishTypeProp;
    std::int64_t cost = -1;
    CHECK(d.DemolishOccupant(true, &prop, 0, &cost, false, nullptr) && prop.removed && cost == 0);
    Occ prop2; prop2.type = kDemolishTypeProp; host.propRemoveOk = false;
    CHECK(!d.DemolishOccupant(true, &prop2, 0, &cost, false, nullptr) && !prop2.removed);
    Occ prot; prot.type = kDemolishTypeProp; prot.protectedFlag = true;
    CHECK(!d.DemolishOccupant(true, &prot, kDemolishSkipProtected, &cost, false, nullptr));
    Occ bld; bld.type = kDemolishTypeBuilding;
    CHECK(d.DemolishOccupant(true, &bld, 0, &cost, false, nullptr) && cost == 100 && host.managerRemoved.empty());
    Occ net; net.type = kDemolishTypeNetwork; net.flags = 1; net.hasNet = true;
    CHECK(d.DemolishOccupant(true, &net, 0, &cost, false, nullptr) && cost == 25);
    CHECK(host.neighborCalls.size() == 1 && host.networkBatch.size() == 1);
    Occ flora; flora.type = kDemolishTypeFlora;
    CHECK(!d.DemolishOccupant(true, &flora, 0, &cost, false, nullptr) && cost == 0);
    Occ found; found.type = kDemolishTypeFoundation; found.hasNet = false;
    CHECK(d.DemolishOccupant(true, &found, 0, &cost, false, nullptr) && host.managerRemoved.empty());
    found.hasNet = true;
    CHECK(d.DemolishOccupant(true, &found, 0, &cost, false, nullptr) && host.managerRemoved.size() == 1);
    Occ unknown; unknown.type = 5;
    CHECK(!d.DemolishOccupant(true, &unknown, 0, &cost, false, nullptr));

    Occ pole; pole.type = kDemolishTypePole; Occ link; link.type = 1; host.conns = {&link};
    std::vector<IDemolishable*> released;
    host.posted.clear();
    CHECK(d.DemolishOccupant(true, &pole, 0, &cost, false, &released) && cost == 7 && pole.removed);
    CHECK(host.posted.size() == 1 && host.posted[0] == kMessageOccupantDemolished && released.size() == 1);

    // lots: only the plopped lot's cells are visited; its prop is demolished and the destroyed lot is costed per tile
    Occ lotProp; lotProp.type = kDemolishTypeProp;
    Lot a, b; a.r = {2, 3, 3, 4}; a.type = 6; a.occupants = {&lotProp}; b.r = {5, 1, 5, 2}; b.plopped = false;
    host.lots = {&a, &b}; host.propRemoveOk = true;
    std::set<IDemolishable*> demolished;
    cost = -1;
    CHECK(d.DemolishLots(true, {&a, &b}, 0, 0, false, nullptr, &cost, &demolished));
    CHECK(host.visited.size() == 4 && cost == 4 * 20 && lotProp.removed && demolished.count(&lotProp) == 1);
    CHECK(host.prepared.size() == 1 && host.prepared[0] == &a && host.batches == 2);
    for (auto [x, z] : host.visited) CHECK(x >= 2 && x <= 3 && z >= 3 && z <= 4);

    // terrain: lowering a free 3x3 area levels it as one rectangle; raising skips every vertex whose 32 m window
    // (x*16-16+1.5 .. x*16+16-1.5) wholly contains an occupant: the rock at 20..30 m sits in the windows of 1 and 2
    CHECK(d.ModifyTerrainHeight(true, false, 1, 1, 3, 3, 1.0f, nullptr, 0) == 9 && host.levelRects == 1);
    Occ rock; rock.box[0] = 20; rock.box[2] = 20; rock.box[3] = 30; rock.box[5] = 30;
    host.terrainOccupants = {&rock};
    CHECK(d.ModifyTerrainHeight(true, true, 1, 1, 3, 3, 1.0f, nullptr, 0) == 5 && host.levelCells == 5);

    // effects: a 10 m building with effect byte 3 is large (0x312) and plays only that effect, not the smaller ones too
    Occ fx; fx.type = kDemolishTypeBuilding;
    d.DoDemolishOccupantEffect(&fx, 3, nullptr, false, 0, 0);
    CHECK(host.effects.size() == 1 && host.effects[0] == 0x312 && host.effect.played == 1);
    // only when the size's effect is absent the next smaller index is tried, and the first one found is the only one
    host.effects.clear();
    host.missingAbove = 0x310;
    d.DoDemolishOccupantEffect(&fx, 3, nullptr, false, 0, 0);
    CHECK(host.effects.size() == 3 && host.effects[0] == 0x312 && host.effects[1] == 0x311 && host.effects[2] == 0x310 &&
          host.effect.played == 2);
    // none at all down to the nibble 0 key: nothing is played
    host.effects.clear();
    host.missingAbove = 0x30f;
    d.DoDemolishOccupantEffect(&fx, 3, nullptr, false, 0, 0);
    CHECK(host.effects.size() == 3 && host.effect.played == 2);
    CHECK(d.Shutdown() && host.removedNotes.size() == 3);
    std::printf(failures ? "demolition: %d failures\n" : "demolition: all passed\n", failures);
    return failures != 0;
}
