// The lot developer on the native application (notes/GROWTH_PLAN.md layer 4): its terrain host (IBaseLotDeveloperHost, which ran
// on the city probe's neutral binding, so every slope and altitude read 0) and the services StartConstruction reaches outside the
// class (ILotConstructionServices): the lot configuration references lots keep, the building development simulator's placement,
// the city's cell corners, the terrain, and the flora a new building clears.
//
// PARTIAL (each named here and where it is called): the terrain leveller (LevelVertices changes nothing, so a steep lot keeps its
// slope) and levelled-vertex flags. The construction occupant is NativeConstruction.cpp.
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeConstruction.h"
#include "app/NativeFlora.h"
#include "app/NativeLotConstruction.h"
#include "app/NativePlumbing.h"
#include "app/NativePower.h"
#include "app/NativeTraffic.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeCityView.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/SimHostRegistry.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZRandom.h"
#include "sc4/cSC4Demand.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/Simulator.h"
#include "sc4/GrowthDeveloper.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/FloraOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/FloraSimulator.h"
#include "sc4/PortDeveloper.h"
#include "sc4/FoundationOccupant.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/LotDeveloper.h"
#include "sc4/OccupantFilters.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/AnimationTickManager.h"
#include "rz/cRZAutoRefCount.h"

namespace native {
void NativeAddRetainingWallOccupant(sc4::cSC4Lot* lot);           // app/NativeLotWalls.cpp
void NativeUpdateSurroundingRetainingWalls(sc4::cSC4Lot* lot);
bool NativeAddNewFoundationOccupant(std::uint32_t type, int usage, float position[3], float width, float depth, int rotation, float bottom);
}  // namespace native

namespace native {
namespace {
using namespace sc4;

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
cSTETerrain* Terrain() {
    cIGZUnknown* object = Component(CityComponent::Terrain);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSC4LotConfigurationManager* LotConfigurations() {
    cIGZUnknown* object = Component(CityComponent::LotConfigurationManager);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4LotConfigurationManager*>(raw) : nullptr;
}

cSC4PropManager* Props() {
    cIGZUnknown* object = Component(CityComponent::PropManager);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4PropManager*>(raw) : nullptr;
}

// The manager's COM reference to one lot configuration (cISC4LotConfiguration, the id is all a lot asks of it). One per id, kept
// for the process like the manager's own table.
// A COM city component through its serializable view's class id.
template <class T> T* CityComponent(sc4::CityComponent c, std::uint32_t clsid) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
template <class T> bool CellInBounds(const cSC4SimGrid<T>& map, int x, int z) {
    const int shift = map.GetTractShift();
    return x >= 0 && z >= 0 && map.TractIsInBounds(x >> shift, z >> shift);
}

// The reference a lot keeps for its configuration; the port developers ask it the configuration's growth stage (vslot 0x58) and
// zone compatibility (vslot 0x78) too.
class ConfigurationRef final : public IPortLotConfiguration {
public:
    explicit ConfigurationRef(std::uint32_t id) : id_(id) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ ? --refs_ : 0; }
    std::uint32_t GetID() const override { return id_; }
    int GetGrowthStage() const override {
        cSC4LotConfigurationManager* manager = LotConfigurations();
        cSC4LotConfiguration* configuration = manager ? manager->GetLotConfiguration(id_) : nullptr;
        return configuration ? configuration->GetGrowthStage() : 0;
    }
    bool IsCompatibleWithZoneType(int zoneType) const override {
        cSC4LotConfigurationManager* manager = LotConfigurations();
        cSC4LotConfiguration* configuration = manager ? manager->GetLotConfiguration(id_) : nullptr;
        return configuration && configuration->IsCompatibleWithZoneType(zoneType);
    }
private:
    std::uint32_t id_;
    std::uint32_t refs_ = 1;
};
std::map<std::uint32_t, std::unique_ptr<ConfigurationRef>> g_references;

// cSC4LotDeveloper's terrain (+0x70), leveller (+0x38) and city (+0x30) as IBaseLotDeveloperHost.
class LotDeveloperHost final : public IBaseLotDeveloperHost {
public:
    float VertexHeight(int index) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(index) : 0.0f; }   // vslot 0x68
    float VertexWaterHeight(int index) override {                                     // vslot 0xf0: the water surface over the vertex
        cSTETerrain* t = Terrain();
        if (!t) return -FLT_MAX;
        const float depth = t->GetWaterDepthAtVertex(index);
        return depth > 0.0f ? t->GetAltitude(index) + depth : -FLT_MAX;
    }
    float HeightAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }   // vslot 0x60
    float WaterHeightAt(float x, float z) override {                                  // vslot 0xf8
        cSTETerrain* t = Terrain();
        if (!t) return -FLT_MAX;
        const int vx = static_cast<int>(x / 16.0f + 0.5f), vz = static_cast<int>(z / 16.0f + 0.5f);
        const float depth = t->GetWaterDepthAtVertex(vx, vz);
        return depth > 0.0f ? t->GetAltitudeAtVertex(vx, vz) + depth : -FLT_MAX;
    }
    bool VertexIsLevelled(int, int) override { return false; }                        // PARTIAL: vslot 0x128 (the levelled flag)
    int VerticesPerRow() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) + 1 : 0; }
    float CityWidthMetres() override { cSTETerrain* t = Terrain(); return t ? static_cast<float>(t->CellCountX()) * 16.0f : 0.0f; }
    float CityDepthMetres() override { cSTETerrain* t = Terrain(); return t ? static_cast<float>(t->CellCountZ()) * 16.0f : 0.0f; }
    // PARTIAL: the terrain leveller (vslot 0x48) is not ported; no vertex changes, so the slope tests see the land as it is.
    int LevelVertices(float, bool, bool, int, int, int, int, void*, int, int) override { return 0; }
    void CellOfPosition(float x, float z, int& cellX, int& cellZ) override {         // city vslot 0x2f8
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        else { cellX = static_cast<int>(x / 16.0f); cellZ = static_cast<int>(z / 16.0f); }
    }
    void OccupantBounds(void* occupant, float lo[3], float hi[3]) override {           // occupant vslot 0x50
        if (occupant) static_cast<cISC4Occupant*>(occupant)->GetBoundingBox(lo, hi);
    }
} g_host;

class ConstructionServices final : public ILotConstructionServices {
public:
    cISC4LotConfigurationRef* ConfigurationRef(cSC4LotConfiguration* configuration) override {
        return configuration ? NativeLotConfigurationRef(configuration->GetID()) : nullptr;
    }
    cSC4LotConfiguration* Configuration(cISC4LotConfigurationRef* reference) override {
        cSC4LotConfigurationManager* manager = LotConfigurations();
        return reference && manager ? manager->GetLotConfiguration(reference->GetID()) : nullptr;
    }
    std::int32_t BuildingPurpose(std::uint32_t type) override {
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? static_cast<std::int32_t>(profile->purpose) : 0;
    }
    std::uint32_t BuildingWealth(std::uint32_t type) override {
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? profile->wealth : 0;
    }
    cSC4Lot* LotAt(int x, int z) override { return NativeLotAtCell(x, z, true); }
    std::uint32_t RandomBelow(std::uint32_t range) override { static cRZRandom random(0x10fu); return range ? random.RandomUint32Uniform(range) : 0; }
    bool LotTextureExists(std::uint32_t instance) override {
        if (!SL::spRM) return false;
        cGZPersistResourceKey key{0x7ab50e44, 0x0986135e, instance};
        return SL::spRM->TestForKey(key);
    }
    void ObjectPropTypes(const cSC4LotConfigurationObject& object, std::vector<std::uint32_t>& out) override {
        if (cSC4LotConfigurationManager* manager = LotConfigurations()) manager->GetObjectIDArray(&object, out);
    }
    cISC4Occupant* AddLotProp(std::uint32_t type, const float position[3], std::int32_t rotation) override {
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = props ? props->AddLotProp(type, position, rotation) : nullptr;
        if (!prop) return nullptr;
        cISC4Occupant* occupant = prop;
        occupant->Release();   // the occupant manager keeps it (the original returns the manager's borrowed pointer)
        return occupant;
    }
    bool FloraTypeValid(std::uint32_t type) override { cSC4FloraSimulator* f = CityFloraSimulator(); return f && f->IsFloraTypeValid(type); }
    void FloraTypesInFamily(std::vector<std::uint32_t>& out, std::uint32_t family) override {
        cSC4FloraSimulator* f = CityFloraSimulator();
        if (!f) return;
        std::unordered_set<std::uint32_t> types;
        f->GetFloraTypesByFamily(types, family);
        out.assign(types.begin(), types.end());
    }
    cISC4Occupant* AddFlora(std::uint32_t type, float position[3]) override {
        cSC4FloraSimulator* f = CityFloraSimulator();
        return f ? f->AddNewFloraOccupant(type, position, false) : nullptr;
    }
    cISC4BuildingOccupant* AddNewBuildingOccupant(std::uint32_t type, float position[3], int rotation) override {
        return NativeAddNewBuildingOccupant(type, position, rotation);
    }
    bool BuildingTypeValid(std::uint32_t type) override { return NativeIsBuildingType(type); }
    void BuildingTypesInFamily(std::vector<std::uint32_t>& out, std::uint32_t family) override { NativeBuildingsInFamily(family, out); }
    void CellCornerToPosition(int x, int z, float& px, float& pz) override {
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(x, z, px, pz);
        else { px = static_cast<float>(x) * 16.0f; pz = static_cast<float>(z) * 16.0f; }
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    // The lot developer's blocking-lot filter (+0x58, made by its Init) takes flora only (cSC4BlockingLotOccupantFilter); cSC4Demolition's
    // flora case removes the occupant from the manager at no cost (the network tool's interim demolition does the same). PORT GUARD:
    // without the filter (a developer that was never initialized) nothing is taken, where the original would query unfiltered.
    void DemolishBlockingOccupants(const float minimum[3], const float maximum[3], cISC4OccupantFilter* blocking) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants || !blocking) return;
        std::vector<cISC4Occupant*> found;
        occupants->IterateOccupantsByBBox(
            [](cISC4Occupant* occupant, void* out) {
                occupant->AddRef();
                static_cast<std::vector<cISC4Occupant*>*>(out)->push_back(occupant);
                return true;
            },
            &found, minimum, maximum, static_cast<cISC4OccupantFilter*>(blocking));
        for (cISC4Occupant* occupant : found) {
            occupants->RemoveOccupant(occupant, true, 0);
            occupant->Release();
        }
    }
    void StartConstructionOccupant(cSC4Lot* lot) override { StartNativeConstructionOccupant(lot); }
    cISC4Occupant* FirstBuildingOnLot(cSC4Lot* lot) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return nullptr;
        std::int32_t rect[4] = {};
        lot->GetBoundingRect(rect);
        const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
        OccupantList found;
        cISC4Occupant* first = nullptr;
        if (occupants->GetOccupantsByStandardCityCells(found, a, b, kOccupantTypeBuilding, 1) && !found.empty()) first = found.front();
        occupants->ReleaseOccupantList(found);   // the occupant manager keeps the building: the original returns it borrowed
        return first;
    }
    void RemoveBuildingProps(cISC4Occupant* building) override { if (cSC4PropManager* p = Props()) p->RemoveBuildingProps(building); }
    void RemoveLotProps(cSC4Lot* lot) override { if (cSC4PropManager* p = Props()) p->RemoveLotProps(lot); }
    double RandomGaussianRange(double lo, double hi) override {
        static cRZRandom random(0x2f1u);   // the original draws from the global generator (::_sRandom)
        return random.RandomDoubleRangeGaussian(lo, hi);
    }
    void AddRubbleProp(cSC4Lot* lot, std::uint32_t type, std::uint32_t rotation, const float position[3], const float* boxMin,
                       const float* boxMax) override {
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = nullptr;
        if (!props || !props->CreateProp(type, prop) || !prop) return;
        cISC4Occupant* occupant = prop;
        if (occupant->Init()) {
            occupant->SetFlag(0x40);
            prop->SetOrientation(static_cast<std::int32_t>(rotation));
            float p[3] = {position[0], position[1], position[2]};
            occupant->SetPosition(p);
            float lo[3] = {}, hi[3] = {};
            occupant->GetBoundingBox(lo, hi);
            if (lo[0] < boxMin[0]) p[0] += boxMin[0] - lo[0];
            else if (boxMax[0] < hi[0]) p[0] += boxMax[0] - hi[0];
            if (lo[2] < boxMin[2]) p[2] += boxMin[2] - lo[2];
            else if (boxMax[2] < hi[2]) p[2] += boxMax[2] - hi[2];
            occupant->SetPosition(p);
            props->AddLotProp(lot, occupant);
        }
        prop->Release();
    }
    void RemoveLotFlora(cSC4Lot* lot) override {   // DemolishFloraInCellArea(rect, 0): every flora occupant on the lot's cells
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return;
        std::int32_t rect[4] = {};
        lot->GetBoundingRect(rect);
        const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
        OccupantList found;
        if (occupants->GetOccupantsByStandardCityCells(found, a, b, kOccupantTypeFlora, 0xffffffffu))
            for (cISC4Occupant* flora : found) occupants->RemoveOccupant(flora, true, 0);
        occupants->ReleaseOccupantList(found);
    }
    void RemoveOccupant(cISC4Occupant* occupant, std::uint32_t flags) override {
        if (cISC4OccupantManager* occupants = Occupants()) occupants->RemoveOccupant(occupant, true, flags);
    }
    void DeleteLot(cSC4Lot* lot) override { NativeDeleteLot(lot); }
    // ---- foundations
    bool AddNewFoundationOccupant(std::uint32_t type, int usage, float position[3], float width, float depth, int rotation,
                                  float bottom) override {
        const bool added = NativeAddNewFoundationOccupant(type, usage, position, width, depth, rotation, bottom);   // app/NativeBuildingDevelopment.cpp
        static const bool trace = std::getenv("SC4_WALLDEBUG") != nullptr;
        if (trace) std::fprintf(stderr, "foundations: new %08x usage %d at %.0f,%.1f,%.0f %.0fx%.0f bottom %.1f -> %d\n", type, usage, position[0],
                                position[1], position[2], width, depth, bottom, added ? 1 : 0);
        return added;
    }
    cISC4Occupant* FoundationAt(int usage, int x, int z) override {   // GetFoundationByStandardCityCell: type 0x68fd0c69 of a usage
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return nullptr;
        const int cell[2] = {x, z};
        OccupantList list;
        cISC4Occupant* found = nullptr;
        if (occupants->GetOccupantsByStandardCityCells(list, cell, cell, sc4::kOccupantTypeFoundation, 0xffffffffu)) {
            for (cISC4Occupant* occupant : list) {
                auto* foundation = dynamic_cast<sc4::cSC4FoundationOccupant*>(occupant);
                if (!found && foundation && foundation->GetUsage() == usage) found = occupant;
            }
        }
        occupants->ReleaseOccupantList(list);   // the city keeps the occupant: the answer is borrowed
        return found;
    }
    std::uint32_t FoundationTypeOf(cISC4Occupant* foundation) override {
        auto* f = dynamic_cast<sc4::cSC4FoundationOccupant*>(foundation);
        return f ? f->GetFoundationType() : 0;
    }
    void RemoveFoundation(cISC4Occupant* foundation) override { if (cISC4OccupantManager* o = Occupants()) o->RemoveOccupant(foundation, true, 0); }
    void LotsAround(cSC4Lot* lot, std::vector<cSC4Lot*>& out) override {
        cSC4City* city = HostCity();
        auto* lots = city ? dynamic_cast<sc4::cSC4ZoneDeveloper*>(city->GetComponent(sc4::CityComponent::ZoneDeveloper)) : nullptr;
        std::list<cRZAutoRefCount<cSC4Lot>> around;
        if (!lots || !lot || lots->GetLotsSurroundingLot(lot, around, 0xf) <= 0) return;
        for (auto& other : around) out.push_back(other.get());   // the lot manager keeps them: borrowed
    }
    bool CellHasWater(int x, int z) override { cSTETerrain* t = Terrain(); return t && t->CellHasWater(std::uint32_t(x), std::uint32_t(z)); }
    float WaterLevelAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetWaterLevel(x, z) : 0.0f; }
    void CellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
        else { x = float(cellX) * 16.0f + 8.0f; z = float(cellZ) * 16.0f + 8.0f; }
    }
    void AddRetainingWallOccupant(cSC4Lot* lot) override { NativeAddRetainingWallOccupant(lot); }   // app/NativeLotWalls.cpp
    void UpdateSurroundingRetainingWalls(cSC4Lot* lot) override { NativeUpdateSurroundingRetainingWalls(lot); }
    void MoveOccupant(cISC4Occupant* occupant, bool post) override {
        if (cISC4OccupantManager* occupants = Occupants()) occupants->MoveOccupant(occupant, post);
        static const bool trace = std::getenv("SC4_TERRAINLOTDEBUG") != nullptr;
        if (trace && occupant) {
            float p[3] = {};
            occupant->GetPosition(p);
            std::fprintf(stderr, "terrain lots: occupant %08x moved to %.1f,%.2f,%.1f\n", occupant->GetType(), p[0], p[1], p[2]);
        }
    }
} g_services;

// The building profiles the lot developer's tests read (BDS vslot 0x9c).
class GateServices final : public ILotGateServices {
public:
    const BuildingProfile* ProfileOfBuilding(std::uint32_t type) override { return NativeBuildingProfile(type); }
    std::uint32_t WealthOfDeveloperType(std::uint32_t type) override { return NativeDeveloperWealth(type); }
} g_gates;

// The daily pass's city (lot-developer.md §4.2, §5).
class DailyServices final : public ILotDailyServices {
public:
    bool DeveloperDesirability(std::uint32_t purpose, std::uint32_t wealth, int x, int z, std::int32_t& developerPurpose,
                               std::int16_t& desirability) override {
        cSC4GrowthDeveloper* developer = GrowthDeveloperOfType(NativeDeveloperType(purpose, static_cast<std::int32_t>(wealth)));
        cSC4SimGrid<std::int16_t>* map = developer ? developer->GetDesirabilityMap() : nullptr;
        if (!developer) return false;
        developerPurpose = developer->GetPurpose();
        desirability = map && CellInBounds(*map, x, z) ? map->GetCellValue(x, z) : 0;
        return true;
    }
    bool DeveloperWaterSupplyCap(std::uint32_t purpose, std::uint32_t wealth, std::uint8_t& cap) override {
        cSC4GrowthDeveloper* developer = GrowthDeveloperOfType(NativeDeveloperType(purpose, static_cast<std::int32_t>(wealth)));
        if (!developer) return false;
        cap = developer->GetWaterSupplyCap();
        return true;
    }
    bool LotGrowthStage(const cSC4Lot& lot, std::uint8_t& stage) override {
        cSC4LotConfiguration* configuration = g_services.Configuration(lot.GetLotConfiguration());
        if (!configuration) return false;
        stage = configuration->GetGrowthStage();
        return true;
    }
    bool CheatIgnorePower() override { return NativeBuildingCheat(1); }
    bool CheatIgnorePlumbing() override { return NativeBuildingCheat(0); }
    bool IsPowered(int x, int z) override { return NativeCellPowered(x, z); }
    bool IsCellWatered(int x, int z) override { return NativeCellWatered(x, z); }
    std::uint8_t TripLengthAt(int x, int z) override {
        cSC4SimGrid<std::uint8_t>* map = NativeTrafficTripLengthMap();
        if (!map) return 1;   // no traffic simulator: the commute is not judged
        return CellInBounds(*map, x, z) ? map->GetCellValue(x, z) : 0;
    }
    void CellWidths(float& x, float& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellWidthX() : 16.0f;
        z = city ? city->CellWidthZ() : 16.0f;
    }
    std::int32_t SimDateNumber() override {
        cSC4Simulator* simulator = CityComponent<cSC4Simulator>(sc4::CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator ? static_cast<std::int32_t>(simulator->GetSimDateNumber()) : 0;
    }
    bool CityHasJobs() override {
        cSC4Demand* jobs = NativeDemandChannel(0x2000, 0x20000);
        return jobs && jobs->QueryDemandValue() > 0.0f;
    }
    bool BuildingBounds(const cSC4Lot& lot, float lo[3], float hi[3]) override {
        cIGZUnknown* building = lot.GetBuilding();
        void* raw = nullptr;
        if (!building || !building->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return false;
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        occupant->GetBoundingBox(lo, hi);
        occupant->Release();
        return true;
    }
    cISC4Occupant* AddZotProp(std::uint32_t type, const float position[3]) override {
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = props ? props->AddLotProp(type, position, CityViewRotation()) : nullptr;
        if (!prop) return nullptr;
        cISC4Occupant* occupant = prop;
        occupant->Release();   // the occupant manager keeps it
        return occupant;
    }
    bool RemoveLotPropsWithZot(cSC4Lot& lot, std::uint32_t zot) override {
        cSC4PropManager* props = Props();
        cISC4OccupantManager* occupants = Occupants();
        if (!props || !occupants) return false;
        OccupantList found;
        bool removed = false;
        if (props->GetLotProps(found, &lot)) {
            for (cISC4Occupant* prop : found) {
                std::uint32_t value = 0;
                cISCPropertyHolder* holder = prop ? prop->AsPropertyHolder() : nullptr;
                if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropPropZot, value) && value == zot) {
                    props->RemovePropA(prop);   // prop manager slot 29
                    removed = true;
                }
            }
        }
        occupants->ReleaseOccupantList(found);
        return removed;
    }
    void PostLotNotification(cSC4Lot& lot, std::uint32_t data1) override {
        if (!SL::spMS2) return;
        // the lot as the message object: its plain view (the message keeps the lot alive while it is queued)
        struct LotObject final : cIGZUnknown {
            explicit LotObject(cSC4Lot& lot) : lot_(&lot) { lot_->AddRef(); }
            ~LotObject() { lot_->Release(); }
            bool QueryInterface(std::uint32_t iid, void** out) override {
                if (iid != kGZIID_PlainObject || !out) return false;
                *out = lot_;   // borrowed
                return true;
            }
            std::uint32_t AddRef() override { return ++refs_; }
            std::uint32_t Release() override {
                if (refs_ < 2) { delete this; return 0; }
                return --refs_;
            }
            cSC4Lot* lot_;
            std::uint32_t refs_ = 0;
        };
        auto* object = new LotObject(lot);
        object->AddRef();
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0x4c04de69);
        message->SetData1(data1);
        message->SetIGZUnknown(object);
        SL::spMS2->MessagePost(message, false);
        message->Release();
        object->Release();
    }
} g_daily;

// What cSC4LotDeveloper::Init, Shutdown and its message handlers reach in the city (lot-developer.md §1.3, §3.2, §8, §9): the
// services the original takes from its globals are the current city's components; the lot manager is the zone developer.
class CityServices final : public ILotDeveloperCityServices {
public:
    const char* MissingService() override {
        if (!HostCity()) return "city";
        static const std::pair<sc4::CityComponent, const char*> kServices[] = {
            {sc4::CityComponent::ZoneDeveloper, "lot manager"}, {sc4::CityComponent::OccupantManager, "occupant manager"},
            {sc4::CityComponent::PropManager, "prop manager"}, {sc4::CityComponent::BuildingDevelopmentSimulator, "building development"},
            {sc4::CityComponent::PowerSimulator, "power"}, {sc4::CityComponent::PlumbingSimulator, "plumbing"},
            {sc4::CityComponent::FloraSimulator, "flora"}, {sc4::CityComponent::Terrain, "terrain"},
            {sc4::CityComponent::TrafficSimulator, "traffic"}};
        for (const auto& [component, name] : kServices)
            if (!Component(component)) return name;
        return nullptr;
    }
    void CityCellCounts(int& x, int& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 0;
        z = city ? city->CellCountZ() : 0;
    }
    ILotGateServices* GateServices() override { return &g_gates; }
    const cISCPropertyHolder* AcquireTunables() override {
        if (!SL::spRM) return nullptr;
        const cGZPersistResourceKey key{kLotDeveloperTunablesKeyType, kLotDeveloperTunablesKeyGroup, kLotDeveloperTunablesKeyInstance};
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        const cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        held_[holder] = exemplar;
        return holder;
    }
    void ReleaseTunables(const cISCPropertyHolder* holder) override {
        auto it = held_.find(holder);
        if (it == held_.end()) return;
        it->second->Release();
        held_.erase(it);
        if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "lot developer: tunables read\n");
    }
    bool AddSimulatorAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) override {
        return AddCitySimulatorAgent(target, level, name, flags);
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    bool MessageServerPresent() override { return SL::spMS2 != nullptr; }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->AddNotification(target, type); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, type); }
    void PostMessage(std::uint32_t type) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    int VisualTimeMode() override {
        cIGZUnknown* object = Component(sc4::CityComponent::Clock24Hour);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return 0;
        return static_cast<int>(static_cast<cSC424HourClock*>(raw)->GetVisualTimeMode());
    }
    cSC4Lot* LotAt(int x, int z, bool exact) override { return NativeLotAtCell(x, z, exact); }
    void LotsInCellRect(int x1, int z1, int x2, int z2, std::vector<cSC4Lot*>& out) override {
        auto* lots = CityComponent<cSC4ZoneDeveloper>(sc4::CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId);
        if (!lots) return;
        std::list<cRZAutoRefCount<cSC4Lot>> found;
        lots->GetLotsInCellRect(found, x1, z1, x2, z2, true);
        for (const cRZAutoRefCount<cSC4Lot>& lot : found)
            if (lot) out.push_back(lot.get());   // the lot manager keeps them
    }
    cISC4OccupantManager* OccupantManager() override { return Occupants(); }
private:
    std::map<const cISCPropertyHolder*, cISCResExemplar*> held_;
} g_cityServices;

}  // namespace

cISC4LotConfigurationRef* NativeLotConfigurationRef(std::uint32_t id) { return NativePortLotConfiguration(id); }
sc4::IPortLotConfiguration* NativePortLotConfiguration(std::uint32_t id) {
    if (id == 0) return nullptr;
    auto it = g_references.find(id);
    if (it == g_references.end()) it = g_references.emplace(id, std::make_unique<ConfigurationRef>(id)).first;
    return it->second.get();
}

void InstallLotConstructionHosts() {
    gzcom::GetSimHosts().lotDeveloper = &g_host;
    cSC4LotDeveloper::SetConstructionServices(&g_services);
    cSC4LotDeveloper::SetDailyServices(&g_daily);
    cSC4LotDeveloper::SetCityServices(&g_cityServices);
    InstallConstructionOccupantHost();
}

sc4::cSC4LotConfiguration* NativeLotConfiguration(sc4::cISC4LotConfigurationRef* reference) {
    sc4::cSC4LotConfigurationManager* manager = LotConfigurations();
    return reference && manager ? manager->GetLotConfiguration(reference->GetID()) : nullptr;
}

}  // namespace native

namespace native {
// Kit verb "lotdev": the city's lot developer as its life cycle left it (initialized, the messages its agents and subscriptions
// delivered by type, the night-light deque and the lots its power handler visited). Read-only.
void NativeLotDeveloperReport() {
    cSC4LotDeveloper* developer = CityComponent<cSC4LotDeveloper>(sc4::CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper);
    if (!developer) {
        std::printf("lotdev: no lot developer\n");
        return;
    }
    std::printf("lotdev: initialized %d, night deque %zu (mode %u, cursor %u, lit %d), power-changed lots %u, tunables batch %u\n",
                int(developer->IsInitialized()), developer->NightBuildingCount(), developer->NightMode(), developer->NightCursor(),
                int(developer->LitFlag()), developer->PowerChangedLots(), developer->Tunables().nightBatch);
    std::printf("lotdev: messages");
    for (const auto& [type, count] : developer->MessageCounts()) std::printf(" %08x:%u", type, count);
    std::printf("\n");
}
}  // namespace native
