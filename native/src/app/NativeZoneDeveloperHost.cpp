// IZoneDeveloperHost on the native application (was the generated neutral stub, so cSC4ZoneDeveloper::Init failed on its
// first service and every zone placement ran the dezone branch: no lot was ever made). The services are the loaded city's
// components: the zone manager, the terrain, the occupant manager, the lot developer, the city's cell grid. The lot developer's
// base-texture services (ILotBaseTextureServices) are installed here too, so a new empty lot draws the game's zone texture.
//
// The lots get a native ILotHost here as well (zone type, date, altitude). Still neutral (each a to-do): the building development simulator (GROWTH-1: no growth developers, so DetermineLotSize falls
// back to 2x2 lots and no building is ever proposed), the network tools (ExtendAsStreets / DrawStreet need cSC4NetworkTool,
// TOOL-1), the lot configuration manager queries, and the terrain view's highlight textures (the drag preview stays the native
// overlay's).
#include "app/NativeZoneDeveloperHost.h"

#include <cstdio>
#include <cstdlib>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeLotConstruction.h"
#include "app/NativeLotSerial.h"
#include "app/NativeZoneOverlay.h"
#include "platform/Unwired.h"
#include "rz/cRZRandom.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/LotDeveloper.h"
#include "sc4/NetworkOccupant.h"
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>
#include "sc4/DBSegment.h"
#include "sc4/GrowthDeveloper.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/Simulator.h"
#include "sc4/Lot.h"
#include "sc4/OccupantManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

cSC4City* g_city = nullptr;

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    cIGZUnknown* object = g_city ? g_city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4ZoneManager* Zones() { return Component<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4ZoneDeveloper* Developer() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4LotDeveloper* LotDeveloper() { return Component<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSTETerrain* Terrain() {
    cIGZUnknown* object = g_city ? g_city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = g_city ? g_city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
bool CellIsWater(int x, int z) {
    cSTETerrain* t = Terrain();
    return t && t->IsWaterVertex(x, z) && t->IsWaterVertex(x + 1, z) && t->IsWaterVertex(x, z + 1) && t->IsWaterVertex(x + 1, z + 1);
}

// The services the zone developer AddRefs and Releases: the city keeps them alive, so the counts are not modelled.
struct CityApi final : IZoneDeveloperCityApi {
    int CellCountX() override { return g_city ? g_city->CellCountX() : 0; }
    int CellCountZ() override { return g_city ? g_city->CellCountZ() : 0; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { if (g_city) g_city->PositionToCell(x, z, cellX, cellZ); }
    bool LocationIsInBounds(float x, float z) override { return g_city && g_city->LocationIsInBounds(x, z); }
} g_cityApi;

struct TerrainApi final : IZoneDeveloperTerrainApi {
    bool IsCellWater(int x, int z) override { return CellIsWater(x, z); }
    float Altitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    // terrain vslot 0x70: the altitude at the cell's centre
    float CellAltitude(int x, int z) override { return Altitude((static_cast<float>(x) + 0.5f) * 16.0f, (static_cast<float>(z) + 0.5f) * 16.0f); }
} g_terrainApi;

struct ZoneManagerApi final : IZoneDeveloperZoneManagerApi {
    int ZoneTypeAt(int x, int z) override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneType(x, z) : 0; }
    bool IsZoneTypeValid(int zoneType) override { return cSC4ZoneManager::IsRCIZone(zoneType); }   // vslot 0x68 IsRCIZone
    std::uint32_t ZoneDragColor(int zoneType) override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneDragColor(zoneType) : 0; }
    void PlaceZone(const SC4CellRegion<int>& region, int zoneType, bool a2, bool a3, bool a4, bool a5, bool a6) override {
        cSC4ZoneManager* m = Zones();
        if (!m) return;
        SC4CellRegion<int> copy = region;
        m->PlaceZone(copy, zoneType, a2, a3, a4, a5, a6, nullptr, nullptr, nullptr);
    }
} g_zoneApi;

struct OccupantApi final : IZoneDeveloperOccupantManagerApi {
    // vslot 0x140 GetOccupantsByStandardCityCells over the one cell; every entry carries a reference
    void ListOccupants(std::vector<cISC4Occupant*>& out, int x, int z, std::uint32_t typeId, int maxCount) override {
        cISC4OccupantManager* m = Occupants();
        if (!m) return;
        OccupantList list;
        const int cell[2] = {x, z};
        if (!m->GetOccupantsByStandardCityCells(list, cell, cell, typeId, static_cast<std::uint32_t>(maxCount))) return;
        out.insert(out.end(), list.begin(), list.end());
    }
    void RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t data) override {   // vslot 0xa8
        if (cISC4OccupantManager* m = Occupants()) m->RemoveOccupant(occupant, shutdown, data);
    }
    IZoneDeveloperNetworkOccupant* NetworkView(cISC4Occupant* occupant) override {
        auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        if (!network) return nullptr;
        view_.network = network;   // used before the next lookup only, like the original's released QI pointer
        return &view_;
    }
    struct View final : IZoneDeveloperNetworkOccupant {
        cSC4NetworkOccupant* network = nullptr;
        bool QueryInterface(std::uint32_t, void**) override { return false; }
        std::uint32_t AddRef() override { return 1; }
        std::uint32_t Release() override { return 1; }
        bool HasNetworkFlag(std::uint32_t flags) const override { return network->HasNetworkFlag(flags); }
        bool HasAnyNetworkFlag(std::uint32_t flags) const override { return network->HasAnyNetworkFlag(flags); }
        std::uint32_t GetNetworkFlag() override { return network->GetNetworkFlag(); }
        bool IsImmovable() const override { return network->IsImmovable(); }
        bool IsOfType(std::uint32_t type) const override { return network->IsOfType(type); }
        std::uint32_t GetEdgeTypesFlag(std::uint32_t type) override { return network->GetEdgeTypesFlag(static_cast<std::uint8_t>(type)); }
        bool ConnectsToNeighbor(int edge, std::uint32_t type) const override { return network->ConnectsToNeighbor(edge, static_cast<std::uint8_t>(type)); }
    } view_;
} g_occupantApi;

// SL::spLotDeveloper as the zone developer calls it.
struct LotDeveloperApi final : IZoneDeveloperLotDeveloperApi {
    void RemoveLot(cSC4Lot* lot) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->RemoveLotBaseTexture(lot); }   // PARTIAL: vslot 0x58 also drops props / foundations
    void ConstructDefaultLot(cSC4Lot*, cSC4LotConfiguration*, std::uint32_t, bool) override {
        SC4_UNWIRED("IZoneDeveloperLotDeveloperApi::ConstructDefaultLot (cSC4LotDeveloper construction not recovered)");
    }
    void SetEmptyLotBaseTexture(cSC4Lot* lot) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->SetEmptyLotBaseTexture(lot); }
    bool GetForceEmptyLotBaseTextures() override { cSC4LotDeveloper* d = LotDeveloper(); return d && d->GetForceEmptyLotBaseTextures(); }
    bool GetLotBaseTextureOccupant(cSC4Lot* lot, cISC4Occupant*& out) override {
        out = nullptr;
        cSC4LotDeveloper* d = LotDeveloper();
        cISC4LotBaseTextureOccupant* texture = nullptr;
        if (!d || !d->GetLotBaseTextureOccupant(lot, texture, false) || !texture) return false;
        out = texture->AsOccupant();
        out->AddRef();
        texture->Release();
        return true;
    }
    void ReleaseLotUnit(cSC4Lot*) override { SC4_UNWIRED("IZoneDeveloperLotDeveloperApi::ReleaseLotUnit (lot developer vslot 0x108, business units)"); }
    void AttemptAreaLevelling(const SC4Rect<int>& rect, float altitude, bool a3, bool a4, float a5, cSC4Lot* lot) override {
        if (cSC4LotDeveloper* d = LotDeveloper()) d->AttemptAreaLevelling(LotCellRect{rect.x0, rect.z0, rect.x1, rect.z1}, altitude, a3, a4, a5, lot);
    }
} g_lotDeveloperApi;

// SL::spBuildingDevelopmentSim as DetermineLotSize reads it: the growth developers (their purpose, wealth, growth list,
// desirability map and demand) and the building catalogue. Its vslot 0x120 is the lot developer.
struct GridView final : IZoneDeveloperGridApi {
    cSC4SimGrid<std::int16_t>* grid = nullptr;
    int TractSize() const override { return grid ? grid->GetTractSize() : 0; }
    std::int16_t TractValue(int x, int z) const override { return grid ? grid->GetTractValue(x, z) : 0; }
};
struct DemandView final : IZoneDeveloperDemandApi {
    sc4::IGrowthDemandApi* demand = nullptr;
    float ActiveDemandValue() const override { return demand ? demand->GetActiveValue() : 0.0f; }
};
class GrowthView final : public IZoneDeveloperGrowthApi {
public:
    explicit GrowthView(sc4::cSC4GrowthDeveloper* d) : d_(d) {}
    int Purpose() const override { return d_->GetPurpose(); }
    int Wealth() const override { return d_->GetWealth(); }
    bool GrowthAndDeclineListsAreValid() const override { return d_->GrowthAndDeclineListsAreValid(); }
    const std::vector<ZoneDeveloperGrowthItem>& GrowthList() const override {
        list_.clear();
        for (const sc4::GrowthItem& item : d_->GetGrowthList()) list_.push_back({item.x, item.z, item.style});
        return list_;
    }
    IZoneDeveloperGridApi* DesirabilityMap() const override { grid_.grid = d_->GetDesirabilityMap(); return grid_.grid ? &grid_ : nullptr; }
    IZoneDeveloperDemandApi* SourceDemand() const override { demand_.demand = d_->GetSourceDemand(); return demand_.demand ? &demand_ : nullptr; }
    std::int32_t EstimateDesirability(int x, int z) const override { return d_->EstimateDesirability(x, z); }
private:
    sc4::cSC4GrowthDeveloper* d_;
    mutable std::vector<ZoneDeveloperGrowthItem> list_;
    mutable GridView grid_;
    mutable DemandView demand_;
};
struct BuildingDevelopmentApi final : IZoneDeveloperBuildingDevelopmentApi {
    IZoneDeveloperGrowthApi* GetGrowthDeveloper(std::uint32_t type) override {
        sc4::cSC4GrowthDeveloper* developer = GrowthDeveloperOfType(type);
        if (!developer) return nullptr;
        auto it = views_.find(developer);
        if (it == views_.end()) it = views_.emplace(developer, std::make_unique<GrowthView>(developer)).first;
        return it->second.get();
    }
    void GetAllBuildingTypes(std::vector<std::uint32_t>& out) override { NativeAllBuildingTypes(out); }
    IZoneDeveloperLotDeveloperApi* GetLotDeveloper() override { return &g_lotDeveloperApi; }
    void GetBuildingTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) override {
        std::vector<std::uint32_t> types;
        NativeBuildingsInFamily(family, types);
        out.insert(types.begin(), types.end());
    }
    const BuildingProfile& GetBuildingProfile(std::uint32_t type) override {
        static const BuildingProfile none{};
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? *profile : none;
    }
    std::map<sc4::cSC4GrowthDeveloper*, std::unique_ptr<GrowthView>> views_;
} g_buildingDevelopmentApi;

struct PlainService final : IZoneDeveloperService {} g_renderer, g_trafficSim;   // only AddRef / Release reach these

// The terrain view's highlight textures: the drag preview is the native overlay's (NativeZoneOverlay).
struct TerrainViewApi final : IZoneDeveloperTerrainViewApi {
    void ClearHighlight() override { ClearDragHighlight(); }
    void PlaceHighlightTexture(const sc4::SC4Point<int>& cell, const ZoneHighlightTexture&, std::uint8_t, bool, std::uint32_t zoneColor, int) override {
        SetDragHighlightCell(cell.x, cell.z, 0x90000000u | (zoneColor >> 8));   // RRGGBBAA -> 0xAARRGGBB
    }
} g_terrainViewApi;

struct ZoneDeveloperHost final : IZoneDeveloperHost {
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    int RendererHighlightScale() override { return 0; }
    IZoneDeveloperService* Renderer() override { return &g_renderer; }
    IZoneDeveloperLotDeveloperApi* LotDeveloper() override { return &g_lotDeveloperApi; }
    IZoneDeveloperBuildingDevelopmentApi* BuildingDevelopment() override { return &g_buildingDevelopmentApi; }
    IZoneDeveloperCityApi* City() override { return &g_cityApi; }
    IZoneDeveloperOccupantManagerApi* OccupantManager() override { return &g_occupantApi; }
    IZoneDeveloperTerrainApi* Terrain() override { return &g_terrainApi; }
    IZoneDeveloperTerrainViewApi* TerrainView() override { return &g_terrainViewApi; }
    IZoneDeveloperService* TrafficSim() override { return &g_trafficSim; }
    IZoneDeveloperZoneManagerApi* ZoneManager() override { return &g_zoneApi; }
    IZoneDeveloperNetworkApi* NetworkSubnet(int) override { return nullptr; }       // cSC4NetworkTool per type: TOOL-1
    IZoneDeveloperSafeListApi* SafeListIterator() override { return nullptr; }     // the developer keeps its own lot list
    void LotConfigurationsByFilter(std::vector<cSC4LotConfiguration*>& out, IZoneDeveloperLotFilter* filter) override {   // manager vslot 0x48
        struct Bridge final : sc4::cISC4LotConfigurationFilter {
            IZoneDeveloperLotFilter* filter = nullptr;
            bool Matches(cSC4LotConfiguration* configuration) override { return filter->IsLotConfigurationIncluded(configuration); }
        } bridge;
        bridge.filter = filter;
        if (filter) NativeLotConfigurationsByFilter(out, &bridge);
    }
    void AllLotConfigurations(std::vector<cSC4LotConfiguration*>&) override {}
    void LotConfigurationForBuildingType(cSC4LotConfiguration*& out, std::uint32_t) override { out = nullptr; }
    bool LotConfigurationCompatibleWithZone(cISC4LotConfigurationRef*, int) override { return false; }
    void DemolishRegion(const SC4CellRegion<int>&, int, std::uint32_t) override { SC4_UNWIRED("IZoneDeveloperHost::DemolishRegion (demolition utility)"); }
    bool CheatCodesEnabled() override { return false; }
    void RegisterCheatCode(const char*, std::uint32_t, cIGZMessageTarget2*) override {}
    void UnregisterCheatCode(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t range) override {
        static cRZRandom random(0x5c4u);
        return range ? random.RandomUint32Uniform(range) : 0;
    }
    void GameDelay(int, bool) override {}
    void WriteLot(cIGZOStream& stream, cSC4Lot* lot) override { SC4WriteGZSerializable(stream, LotSerializable(lot)); }
    cSC4Lot* ReadLot(cIGZIStream&) override { return nullptr; }   // the load path reads lots through the serializable factory
    void LotRead(cSC4Lot* lot, cIGZUnknown* serializable) override { RememberLotSerializable(lot, serializable); }
} g_host;

// The lot developer's base-texture services.
struct BaseTextureServices final : ILotBaseTextureServices {
    cSC4Lot* LotAtCell(int x, int z) override { cSC4ZoneDeveloper* d = Developer(); return d ? d->GetLot(x, z, false) : nullptr; }
    cISC4OccupantManager* OccupantManager() override { return Occupants(); }
    std::uint32_t ZoneDragColor(int zoneType) override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneDragColor(zoneType) : 0; }
    std::uint8_t ZoneDisplayAlpha() override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneDisplayAlpha() : 0xff; }
    int CityCellsX() override { return g_city ? g_city->CellCountX() : 0; }
    int CityCellsZ() override { return g_city ? g_city->CellCountZ() : 0; }
    void BoundaryLotTexture(const int sides[4], std::uint32_t& texture, std::uint8_t& rotation, bool& flag) override {
        BoundaryType boundary;
        for (int i = 0; i < 4; ++i) boundary.sides[i] = sides[i];
        texture = 0;
        rotation = 0;
        flag = false;
        if (cSC4ZoneDeveloper* d = Developer()) d->GetBoundaryLotTexture(boundary, texture, rotation, flag);
    }
    cISC4LotBaseTextureOccupant* CreateBaseTextureOccupant() override {
        auto* occupant = new cSC4LotBaseTextureOccupant();
        occupant->AddRef();
        return occupant;
    }
} g_baseTextures;

// ILotHost: the lot's zone type, date, altitude and random come from the city's components. Everything about buildings,
// business units, traffic and power is still the neutral answer of the generated stub (GROWTH-1: the building development
// simulator, the residential / commercial simulators and the traffic simulator are not wired natively).
struct LotHost final : ILotHost {
    std::uint8_t ZoneTypeAt(int x, int z) override { cSC4ZoneManager* m = Zones(); return m ? static_cast<std::uint8_t>(m->GetZoneType(x, z)) : 0; }
    std::int32_t SimulatorDay() override { cSC4Simulator* sim = Component<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); return sim ? static_cast<std::int32_t>(sim->GetSimDateNumber()) : 0; }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    const LotBuildingProfile* BuildingProfile(std::uint32_t buildingType) override { return {}; }
    float SlopeOf(const cSC4Lot& lot) override { return {}; }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override { return {}; }
    void OnDeveloperLotAbandoned(std::uint16_t wealthIndex) override {}
    std::uint8_t LandValueAt(int x, int z) override { return {}; }
    std::uint32_t RandomPercent() override { static cRZRandom random(0x107u); return random.RandomUint32Uniform(100); }
    cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t id) override { return NativeLotConfigurationRef(id); }
    // PARTIAL: the message (0xe8bda2e7 and the condition change) is not posted to SL::spMS2; the lot developer, which turns a built
    // lot's base texture, takes the state change directly.
    void PostLotMessage(std::uint32_t message, const cSC4Lot& snapshot, std::uint32_t oldValue, std::uint32_t newValue, std::uint32_t, std::int32_t) override {
        if (message != sc4::kMessageLotStateChanged) return;
        if (cSC4LotDeveloper* developer = LotDeveloper()) developer->DoLotStateChanged(snapshot, oldValue, newValue);
    }
    void SetLotHighlight(int x, int z, bool on, float altitude) override {}
    std::int32_t TrafficJobs(const cSC4Lot& lot, int kind) override { return {}; }
    void SetTrafficJobs(const cSC4Lot& lot, int kind, std::int32_t jobs) override {}
    float ResidentialTravelFactor(int x, int z) override { return {}; }
    float TravelVolumeFactor() override { return {}; }
    void TravelDistribution(int kind, float out[3]) override {}
    void JobsPerOccupant(float out[4], int developerType) override {}
    void ExemplarJobs(const cSC4Lot& lot, float out[4]) override {}
    bool ExemplarProperty(const cSC4Lot& lot, std::uint32_t id, std::int32_t& value) override { return {}; }
    bool PowerRequirementExceeded(const cSC4Lot& lot, const LotBuildingProfile& profile, bool& available) override { return {}; }
    bool TrafficHasJobAccess(const cSC4Lot& lot, int kind) override { return {}; }
    void UnitLotShutdown(cIGZUnknown* unit, const cSC4Lot& lot) override {}
    void NeighborLots(const cSC4Lot& lot, int direction, std::vector<cSC4Lot*>& out) override {}
    void GetLotOccupants(const cSC4Lot& lot, std::vector<cIGZUnknown*>& out) override {}
    // cSC4Lot::CalculateRoadAccess's scan (0x100181c62): the cells along one edge of the lot (the zone developer's
    // GetNeighborCellsAtEdgeOfLot, edge mask 1 << rotation) hold a network occupant of a road type (mask 0x449: road, street,
    // avenue, one-way road).
    bool RoadAccessCells(const cSC4Lot& lot, int rotation) override {
        cSC4ZoneDeveloper* developer = Developer();
        if (!developer) return false;
        std::int32_t rect[4];
        lot.GetBoundingRect(rect);
        SC4Rect<int> edge;
        const bool hasEdge = developer->GetNeighborCellsAtEdgeOfLot(SC4Rect<int>{rect[0], rect[1], rect[2], rect[3]}, 1 << rotation, edge);
        bool found = false;
        std::uint32_t seen = 0;
        if (hasEdge)
            for (int x = edge.x0; x <= edge.x1 && !found; ++x)
                for (int z = edge.z0; z <= edge.z1 && !found; ++z) {
                    const std::uint32_t types = NativeNetworkTypesAt(x, z);
                    seen |= types;
                    found = (types & 0x449) != 0;
                }
        if (std::getenv("SC4_SIMDEBUG"))
            std::fprintf(stderr, "lot road access: rect (%d,%d)-(%d,%d) rotation %d edge %d (%d,%d)-(%d,%d) types %x -> %d\n", rect[0], rect[1],
                         rect[2], rect[3], rotation, hasEdge, edge.x0, edge.z0, edge.x1, edge.z1, seen, found);
        return found;
    }
    bool IsFerryTerminal(const cSC4Lot& lot) override { return {}; }
    bool FerryTerminalRoadAccess(const cSC4Lot& lot) override { return {}; }
    cIGZUnknown* FindBuildingOccupant(const cSC4Lot& lot) override { return {}; }
    void WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) override { SC4WriteGZSerializable(stream, object); }
    cIGZUnknown* ReadSerializable(cIGZIStream& stream, std::uint32_t clsid) override { void* out = nullptr; SC4ReadGZSerializable(stream, clsid, &out, false); return static_cast<cIGZUnknown*>(out); }
} g_lotHost;

}  // namespace

std::uint32_t NativeNetworkTypesAt(int x, int z) {
    cISC4OccupantManager* occupants = Occupants();
    cISC4Occupant* occupant = nullptr;
    if (!occupants || !occupants->GetFirstOccupantByStandardCityCell(occupant, x, z, 0xc772bf98) || !occupant) return 0;
    std::uint32_t types = 0;
    if (auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant))
        for (std::uint32_t type = 0; type < 13; ++type)
            if (network->IsOfType(type)) types |= 1u << type;
    occupant->Release();
    return types;
}

sc4::cSC4Lot* NativeLotAtCell(int x, int z, bool exactCell) {
    cSC4ZoneDeveloper* developer = Developer();
    return developer ? developer->GetLot(x, z, exactCell) : nullptr;
}

float NativeAreaSlope(const std::int32_t rect[4], bool includeWater) {
    cSC4LotDeveloper* developer = LotDeveloper();
    return developer ? developer->GetAreaSlope(LotCellRect{rect[0], rect[1], rect[2], rect[3]}, includeWater) : 0.0f;
}

bool InstallZoneDeveloperHost(sc4::cSC4City* city) {
    g_city = city;
    sc4::SetZoneDeveloperHost(&g_host);
    sc4::SetLotHost(&g_lotHost);
    sc4::cSC4LotDeveloper::SetBaseTextureServices(&g_baseTextures);
    // The component pass ran Init against the stub host, which failed on its first service; with the real services it completes.
    cSC4ZoneDeveloper* developer = Developer();
    const bool ok = developer && developer->Init();
    std::printf("city view: zone developer %s (lot developer %s)\n", ok ? "up" : "NOT initialised", LotDeveloper() ? "found" : "MISSING");
    return ok;
}

}  // namespace native
