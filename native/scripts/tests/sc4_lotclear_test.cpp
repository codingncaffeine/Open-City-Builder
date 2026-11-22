// The lot developer's clearing paths and what they take with them: ClearLot (Mac i386 0x000f385c) removes the lot's retaining
// wall occupant and its foundation and updates the neighbors' foundations and walls.
#include <cstdio>
#include <vector>

#include "sc4/Lot.h"
#include "sc4/LotDeveloper.h"
#include "sc4/Occupant.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {
class LotHostFake final : public ILotHost {
public:
    std::uint8_t ZoneTypeAt(int, int) override { return 1; }
    std::int32_t SimulatorDay() override { return 100; }
    float TerrainAltitude(float, float) override { return 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t) override { return nullptr; }
    float SlopeOf(const cSC4Lot&) override { return 0.0f; }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override { return purpose * 0x100 + wealth; }
    void OnDeveloperLotAbandoned(std::uint16_t) override {}
    std::uint8_t LandValueAt(int, int) override { return 0; }
    std::uint32_t RandomPercent() override { return 0; }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t) override { return nullptr; }
    void PostLotMessage(std::uint32_t, const cSC4Lot&, std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) override {}
    void SetLotHighlight(int, int, bool, float) override {}
    std::int32_t TrafficJobs(const cSC4Lot&, int) override { return 0; }
    void SetTrafficJobs(const cSC4Lot&, int, std::int32_t) override {}
    float ResidentialTravelFactor(int, int) override { return 1.0f; }
    float TravelVolumeFactor() override { return 1.0f; }
    void TravelDistribution(int, float out[3]) override { out[0] = out[1] = out[2] = 0; }
    void JobsPerOccupant(float out[4], int) override { out[0] = out[1] = out[2] = out[3] = 0; }
    void ExemplarJobs(const cSC4Lot&, float*) override {}
    bool ExemplarProperty(const cSC4Lot&, std::uint32_t, std::int32_t&) override { return false; }
    bool PowerRequirementExceeded(const cSC4Lot&, const LotBuildingProfile&, bool& available) override { available = false; return false; }
    bool TrafficHasJobAccess(const cSC4Lot&, int) override { return false; }
    void NeighborLots(const cSC4Lot&, int, std::vector<cSC4Lot*>&) override {}
    std::vector<cIGZUnknown*> lotOccupants;
    void GetLotOccupants(const cSC4Lot&, std::vector<cIGZUnknown*>& out) override { out = lotOccupants; }
    bool RoadAccessCells(const cSC4Lot&, int) override { return true; }
    bool IsFerryTerminal(const cSC4Lot&) override { return false; }
    bool FerryTerminalRoadAccess(const cSC4Lot&) override { return false; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot&) override { return nullptr; }
    void WriteSerializable(cIGZOStream&, cIGZUnknown*) override {}
    cIGZUnknown* ReadSerializable(cIGZIStream&, std::uint32_t) override { return nullptr; }
};

// An occupant of a given type standing at a position.
class Standing final : public cSC4Occupant {
public:
    Standing(std::uint32_t type, float x, float y, float z) : at_{x, y, z} { type_ = type; }
    void GetPosition(float* xyz) override { for (int i = 0; i < 3; ++i) xyz[i] = at_[i]; }
    void SetPosition(const float* xyz) override { for (int i = 0; i < 3; ++i) at_[i] = xyz[i]; }
private:
    float at_[3];
};

class Construction final : public ILotConstructionServices {
public:
    cISC4LotConfigurationRef* ConfigurationRef(cSC4LotConfiguration*) override { return nullptr; }
    cSC4LotConfiguration* Configuration(cISC4LotConfigurationRef*) override { return nullptr; }
    std::int32_t BuildingPurpose(std::uint32_t) override { return 1; }
    std::uint32_t BuildingWealth(std::uint32_t) override { return 1; }
    cISC4BuildingOccupant* AddNewBuildingOccupant(std::uint32_t, float[3], int) override { return nullptr; }
    void CellCornerToPosition(int, int, float&, float&) override {}
    float TerrainAltitude(float, float) override { return 0.0f; }
    void DemolishBlockingOccupants(const float[3], const float[3], cISC4OccupantFilter*) override {}
    void StartConstructionOccupant(cSC4Lot*) override {}
    cSC4Lot* lot = nullptr;   // the one lot of the city
    cSC4Lot* LotAt(int, int) override { return lot; }
    std::uint32_t RandomBelow(std::uint32_t) override { return 0; }
    bool LotTextureExists(std::uint32_t) override { return false; }
    void ObjectPropTypes(const cSC4LotConfigurationObject&, std::vector<std::uint32_t>&) override {}
    cISC4Occupant* AddLotProp(std::uint32_t, const float[3], std::int32_t) override { return nullptr; }
    bool FloraTypeValid(std::uint32_t) override { return false; }
    void FloraTypesInFamily(std::vector<std::uint32_t>&, std::uint32_t) override {}
    cISC4Occupant* AddFlora(std::uint32_t, float[3]) override { return nullptr; }
    // the foundations: one lot foundation (usage 2) at a cell
    cISC4Occupant* lotFoundation = nullptr;
    int lotFoundationX = -1, lotFoundationZ = -1;
    std::vector<cISC4Occupant*> removedFoundations;
    cISC4Occupant* FoundationAt(int usage, int x, int z) override {
        return usage == 2 && x == lotFoundationX && z == lotFoundationZ ? lotFoundation : nullptr;
    }
    void RemoveFoundation(cISC4Occupant* foundation) override {
        removedFoundations.push_back(foundation);
        if (foundation == lotFoundation) lotFoundation = nullptr;
    }
    // the retaining walls
    int wallsAdded = 0, wallsRemoved = 0, surroundingWallUpdates = 0, lotsAroundAsked = 0;
    void AddRetainingWallOccupant(cSC4Lot* l, bool create) override {
        if (create) { ++wallsAdded; return; }
        ++wallsRemoved;
        l->SetFlags(static_cast<std::uint8_t>(l->GetFlags() & ~4u));
    }
    void UpdateSurroundingRetainingWalls(cSC4Lot*) override { ++surroundingWallUpdates; }
    void LotsAround(cSC4Lot*, std::vector<cSC4Lot*>&) override { ++lotsAroundAsked; }
};

class DeveloperHost final : public IBaseLotDeveloperHost {
public:
    float VertexHeight(int) override { return 0; }
    float VertexWaterHeight(int) override { return 0; }
    float HeightAt(float, float) override { return 0; }
    float WaterHeightAt(float, float) override { return 0; }
    bool VertexIsLevelled(int, int) override { return false; }
    int VerticesPerRow() override { return 65; }
    float CityWidthMetres() override { return 1024; }
    float CityDepthMetres() override { return 1024; }
    int LevelVertices(float, bool, bool, int, int, int, int, void*, int, int) override { return 0; }
    void CellOfPosition(float, float, int&, int&) override {}
    void OccupantBounds(void*, float[3], float[3]) override {}
};
}  // namespace

int main() {
    LotHostFake host;
    SetLotHost(&host);
    Construction construction;
    cSC4LotDeveloper::SetConstructionServices(&construction);
    DeveloperHost devHost;
    cSC4LotDeveloper dev(&devHost);

    {   // ClearLot: the wall occupant goes (create false), the lot foundation at the lot's cell goes and the lot is no longer
        // fixed (flags bits 0..1), the neighbors' foundations and walls are updated, and the lot is an empty state-1 lot.
        auto* lot = new cSC4Lot(6, 9, 2, 2, 0);
        lot->AddRef();
        lot->Init();
        lot->SetBuildingType(0x1234u);
        lot->SetState(3, 0, 0);
        lot->SetFlags(7);
        lot->SetVerticesAreLocked(true);
        construction.lot = lot;
        auto* foundation = new Standing(0x68fd0c69u, 112.0f, 10.0f, 160.0f);
        foundation->AddRef();
        construction.lotFoundation = foundation;
        lot->GetLocation(construction.lotFoundationX, construction.lotFoundationZ);   // the lot's anchor cell
        dev.ClearLot(lot);
        CHECK(construction.wallsRemoved == 1 && construction.wallsAdded == 0);
        CHECK(construction.removedFoundations.size() == 1 && construction.removedFoundations[0] == foundation);
        CHECK((lot->GetFlags() & 7u) == 0u);
        CHECK(construction.lotsAroundAsked >= 1 && construction.surroundingWallUpdates == 1);
        CHECK(lot->GetState() == 1 && lot->GetBuildingType(false) == 0);
        construction.lot = nullptr;
        foundation->Release();
        lot->Release();
    }

    std::printf(failures ? "FAILED\n" : "sc4_lotclear: all passed\n");
    return failures ? 1 : 0;
}
