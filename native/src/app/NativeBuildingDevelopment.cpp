// The building development simulator on the native application (notes/GROWTH_PLAN.md layer 1). Replaces the generated neutral
// IBuildingDevelopmentSimHost: the building / simulator exemplars come from the resource manager (so the building catalogue and the
// growth developers exist), the simulator becomes a real simulator agent and SL::spMS2 listener through a message-target adapter,
// its construction resources are a real cSC4Demand, and its growth developers are real cSC4GrowthDeveloper objects.
//
// Still neutral (each logs SC4_UNWIRED or is named here): the foundation occupants (layer 4), the
// maintenance cost and localized names, the lot configurations behind the zone capacity map.
#include "app/NativeBuildingDevelopment.h"

#include <cstdio>
#include <cstdlib>
#include <array>
#include <chrono>
#include <map>

#include "rz/cRZRandom.h"

#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "platform/Unwired.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSTETerrain.h"
#include "sc4/DemandSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4Demand.h"

namespace native {
namespace {
using namespace sc4;

// Every key of one resource type.
class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }
private:
    std::uint32_t type_;
};

template <class T> T* CityComponent(sc4::CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

// The simulator's cIGZMessageTarget2 view (+0x10 in the original): the message server and the simulator deliver to it.
class SimulatorTarget final : public cIGZMessageTarget2 {
public:
    explicit SimulatorTarget(cSC4BuildingDevelopmentSimulator* simulator) : simulator_(simulator) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessageTarget2 && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ > 1 ? --refs_ : 1; }   // owned by the host's table
    bool DoMessage(cIGZMessage2* message) override {
        static const bool trace = std::getenv("SC4_SIMDEBUG") != nullptr;
        if (trace && message && (message->GetType() == 0x66956816 || message->GetType() == 0x66956810))
            std::fprintf(stderr, "building development: month %08x, stage %d, %zu developers;%s\n", message->GetType(), simulator_ ? simulator_->GrowthStage() : -1,
                         simulator_ ? simulator_->GetDevelopers().size() : 0, GrowthDevelopersSummary().c_str());
        return simulator_ ? simulator_->DoMessage(message) : true;
    }
private:
    cSC4BuildingDevelopmentSimulator* simulator_;
    std::uint32_t refs_ = 1;
};
std::map<cSC4BuildingDevelopmentSimulator*, SimulatorTarget*> g_targets;
constexpr std::uint32_t kMessages[] = {0xe8bda2e7, 0x278e6003, 0x99ef1142, 0x99ef1143, 0xb99ae81};
constexpr int kAgentIntervals[] = {0, 4, 6, 10};

// cSC4Demand as the construction resources (+0xe0): supply = the month's total, demand = what construction holds.
class ConstructionResources final : public IConstructionResources {
public:
    float Total() const override { return demand_.QuerySupplyValue(); }       // vslot 0x40
    float Used() const override { return demand_.QueryDemandValue(); }        // vslot 0x48
    void Add(float amount) override { demand_.AddToDemandValue(amount); }      // vslot 0x88
    void Update() override { demand_.EndOfCycle(); }                           // vslot 0xf0
    void Init() override { demand_.Init(); }
    void SetTotal(float total) override { demand_.SetSupplyValue(total); }     // vslot 0x90
    void SetUsed(float used) override { demand_.SetDemandValue(used); }        // vslot 0x98
private:
    cSC4Demand demand_;
};

// One cSC4GrowthDeveloper (a COM object: the reference taken at creation is the simulator's).
class GrowthDeveloperRef final : public IGrowthDeveloperRef {
public:
    GrowthDeveloperRef() : developer_(NewGrowthDeveloper()) {}
    void Init(cISCPropertyHolder* properties) override { GrowthDeveloperInit(developer_, properties); }
    void StartMapUpdate() override { GrowthDeveloperStartMapUpdate(developer_); }
    bool IsMapUpdateFinished() const override { return GrowthDeveloperMapUpdateFinished(developer_); }
    std::uint32_t SubType() const override { return GrowthDeveloperSubType(developer_); }
    bool ListsValid() const override { return GrowthDeveloperListsValid(developer_); }
    std::vector<GrowthTract> GrowthList() const override {
        std::vector<std::array<std::int32_t, 3>> items;
        GrowthDeveloperGrowthList(developer_, items);
        std::vector<GrowthTract> out;
        for (const auto& i : items) out.push_back(GrowthTract{i[0], i[1], i[2]});
        return out;
    }
    float ActiveDemand() const override { return GrowthDeveloperActiveDemand(developer_); }
    void* Handle() const { return developer_; }
private:
    void* developer_;
};

// SL::spTractDeveloper: the city's cSC4TractDeveloper, reached through NativeTractDevelopment.cpp.
class TractDeveloperRef final : public ITractDeveloperRef {
public:
    int ExecuteOutcome(const TractOutcome&) override { SC4_UNWIRED("ITractDeveloperRef::ExecuteOutcome (the decline plan, layer 3)"); return 0; }
    int Grow(IGrowthDeveloperRef* developer, int x, int z, std::uint8_t style) override {
        auto* ref = static_cast<GrowthDeveloperRef*>(developer);   // every developer this host made is one
        return ref ? NativeTractGrow(ref->Handle(), x, z, style) : 0;
    }
} g_tract;

sc4::cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
    manager->Release();   // the city keeps the manager
    return manager;
}
sc4::cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<sc4::cSTETerrain*>(serializable);
}

// A building occupant while AddNewBuildingOccupant places it: the slots it calls on the new cSC4BuildingOccupant. The wrapper
// holds the creation reference; ReleaseBuilding drops it (the occupant manager keeps its own once inserted).
class PlacedBuilding final : public IPlacedBuilding {
public:
    PlacedBuilding() : occupant(new sc4::cSC4BuildingOccupant()) { occupant->AddRef(); }   // operator new + vslot 0x4
    cISCPropertyHolder* PropertyHolder() override { return occupant->AsPropertyHolder(); }
    void SetBuildingType(std::uint32_t type) override { occupant->SetBuildingType(type); }                      // 0x14
    void SetBounds(const float minimum[3], const float maximum[3]) override { occupant->SetBoundingBox(minimum, maximum); }   // 0x20
    void SetRotation(int rotation) override { occupant->SetOrientation(rotation); }                              // 0x3c
    void FinishPlacement() override { occupant->Init(); }                                                         // 0x64
    void SetPosition(const float position[3]) override { occupant->SetPosition(position); }                      // 0x70
    void GetBounds(float minimum[3], float maximum[3]) override { occupant->GetBoundingBox(minimum, maximum); }   // 0x74
    sc4::cSC4BuildingOccupant* occupant;
    bool inserted = false;
};

// IBuildingPlacementServices: the growth developers, and the building occupants AddNewBuildingOccupant places.
class PlacementServices final : public IBuildingPlacementServices {
public:
    IPlacedBuilding* CreateBuilding() override { return new PlacedBuilding(); }
    void ReleaseBuilding(IPlacedBuilding* building) override {   // vslot 0x8; the wrapper goes with the last use (NativeAddNewBuildingOccupant)
        auto* placed = static_cast<PlacedBuilding*>(building);
        sc4::cSC4BuildingOccupant* occupant = placed->occupant;
        if (!placed->inserted) delete placed;
        occupant->Release();
    }
    IPlacedFoundation* CreateFoundation() override { SC4_UNWIRED("IBuildingPlacementServices::CreateFoundation (layer 4)"); return nullptr; }
    void ReleaseFoundation(IPlacedFoundation*) override {}
    float FoundationSurfaceHeight(const int*, int) override { return 0.0f; }
    // @ i386 0x000fc808: the building's exemplar becomes the occupant's default exemplar (the 0xac2b5f7 view, vslot 0xc), and its
    // size (0x27812810) the bounding box from the origin.
    void LoadBuildingProperties(IPlacedBuilding& building, std::uint32_t type) override {
        auto& placed = static_cast<PlacedBuilding&>(building);
        if (type == 0) type = placed.occupant->GetBuildingType();
        cGZPersistResourceKey key;
        void* raw = nullptr;
        if (!NativeBuildingKeyFromType(type, key) || !SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        placed.occupant->SetDefaultExemplar(exemplar);
        std::vector<float> size;
        if (cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x27812810, size) && size.size() >= 3) {
            const float origin[3] = {0.0f, 0.0f, 0.0f};
            placed.occupant->SetBoundingBox(origin, size.data());
        }
        exemplar->Release();
    }
    IGrowthDeveloperRef* CreateGrowthDeveloper() override { return new GrowthDeveloperRef(); }
    // PARTIAL: terrain vslot 0x44(rect, 0, 1); the highest terrain point of the footprint's corners and centre stands in.
    float SurfaceHeightForFootprint(const float rect[4]) override {
        sc4::cSTETerrain* terrain = Terrain();
        if (!terrain) return 0.0f;
        float best = terrain->GetAltitude((rect[0] + rect[2]) * 0.5f, (rect[1] + rect[3]) * 0.5f);
        for (int corner = 0; corner < 4; ++corner) {
            const float h = terrain->GetAltitude(rect[(corner & 1) ? 2 : 0], rect[(corner & 2) ? 3 : 1]);
            if (best < h) best = h;
        }
        return best;
    }
    void CellToPosition(int x, int z, float& px, float& pz) override { px = (static_cast<float>(x) + 0.5f) * 16.0f; pz = (static_cast<float>(z) + 0.5f) * 16.0f; }
    void PositionToCell(float x, float z, int& cx, int& cz) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cx, cz);
        else { cx = static_cast<int>(x / 16.0f); cz = static_cast<int>(z / 16.0f); }
    }
    float TerrainHeight(float x, float z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool OccupantAtCell(int cx, int cz, std::uint32_t type) override {   // occupant manager vslot 0x80 GetFirstOccupantByStandardCityCell
        sc4::cISC4OccupantManager* occupants = Occupants();
        sc4::cISC4Occupant* occupant = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByStandardCityCell(occupant, cx, cz, type) || !occupant) return false;
        occupant->Release();
        return true;
    }
    // BDS vslot 0x18 (i386 0x000f6ed8): occupant manager vslot 0x50 InsertOccupant(building, 0).
    bool AddOccupant(IPlacedOccupant* occupant, bool) override {
        auto* placed = static_cast<PlacedBuilding*>(occupant);
        sc4::cISC4OccupantManager* occupants = Occupants();
        placed->inserted = occupants && occupants->InsertOccupant(placed->occupant->AsOccupant(), 0);
        return placed->inserted;
    }
    bool RemoveOccupant(IPlacedOccupant*, bool, bool) override { return false; }
} g_placement;

class BuildingDevelopmentHost final : public IBuildingDevelopmentSimHost {
public:
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(0x6534284a);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) fn(key);
    }
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) override {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        held_[holder] = exemplar;
        return holder;
    }
    void ReleaseExemplar(cISCPropertyHolder* holder) override {
        auto it = held_.find(holder);
        if (it == held_.end()) return;
        it->second->Release();
        held_.erase(it);
    }
    std::int64_t MaintenanceCost(cISCPropertyHolder*) override { return 0; }   // budget simulator vslot 0x190: not wired here
    bool LoadLocalizedName(cISCPropertyHolder*, std::uint32_t, cRZString&) override { return false; }
    void CityCellSize(float& x, float& z) override { x = 16.0f; z = 16.0f; }
    void PostBuildingTypesUpdated() override {}
    std::int32_t SimulatorDay() override {
        cSC4Simulator* simulator = CityComponent<cSC4Simulator>(sc4::CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator ? static_cast<std::int32_t>(simulator->GetSimDateNumber()) : 0;
    }
    void LotConfigurations(std::vector<LotConfigurationInfo>&) override {}   // the zone capacity map stays empty (GROWTH-1)

    void RegisterMessageTarget(cSC4BuildingDevelopmentSimulator* simulator) override {
        if (!simulator || g_targets.count(simulator)) return;
        auto* target = new SimulatorTarget(simulator);
        g_targets[simulator] = target;
        if (SL::spMS2) for (std::uint32_t type : kMessages) SL::spMS2->AddNotification(target, type);
        int agents = 0;
        for (int interval : kAgentIntervals) agents += AddCitySimulatorAgent(target, interval, "cSC4BuildingDevelopmentSimulator", 2) ? 1 : 0;
        if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "building development: %d agents registered\n", agents);
    }
    void UnregisterMessageTarget(cSC4BuildingDevelopmentSimulator* simulator) override {
        auto it = g_targets.find(simulator);
        if (it == g_targets.end()) return;
        if (SL::spMS2) for (std::uint32_t type : kMessages) SL::spMS2->RemoveNotification(it->second, type);
        RemoveCitySimulatorAgent(it->second);
        delete it->second;
        g_targets.erase(it);
    }
    IBuildingPlacementServices* PlacementServices() override { return &g_placement; }
    ITractDeveloperRef* TractDeveloper() override { return &g_tract; }
    std::uint32_t RandomBelow(std::uint32_t range) override { static cRZRandom random(0x5eedu); return range ? random.RandomUint32Uniform(range) : 0; }
    std::uint64_t BeginGrowSlice() override { return NowMicros() + 3000; }      // cSC4DynamicLimitTimer: 3 ms
    bool GrowSliceExpired(std::uint64_t slice) override { return NowMicros() >= slice; }
    static std::uint64_t NowMicros() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    IConstructionResources* CreateConstructionResources() override { return new ConstructionResources(); }
    float DemandChannelSupply(std::uint32_t type) override {
        cSC4DemandSimulator* demand = CityComponent<cSC4DemandSimulator>(sc4::CityComponent::DemandSimulator, kGZCLSID_cSC4DemandSimulator);
        cSC4Demand* channel = demand ? demand->GetDemand(type, 0) : nullptr;
        return channel ? channel->QuerySupplyValue() : 0.0f;
    }

private:
    std::map<cISCPropertyHolder*, cISCResExemplar*> held_;
};

}  // namespace

namespace {
// The city's building development simulator: a plain class held in its serializable adapter (the plain view).
cSC4BuildingDevelopmentSimulator* CityBuildingDevelopment() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::BuildingDevelopmentSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<cSC4BuildingDevelopmentSimulator*>(raw);
}
}  // namespace

// BDS vslot 0x1c AddNewBuildingOccupant(type, position, rotation): the inserted building (borrowed: the occupant manager holds it).
sc4::cISC4BuildingOccupant* NativeAddNewBuildingOccupant(std::uint32_t type, float position[3], int rotation) {
    cSC4BuildingDevelopmentSimulator* simulator = CityBuildingDevelopment();
    IPlacedBuilding* building = simulator ? simulator->AddNewBuildingOccupant(type, position, rotation) : nullptr;
    if (!building) return nullptr;
    auto* placed = static_cast<PlacedBuilding*>(building);
    sc4::cSC4BuildingOccupant* occupant = placed->occupant;
    delete placed;
    return occupant;
}

const sc4::BuildingProfile* NativeBuildingProfile(std::uint32_t buildingType) {
    cSC4BuildingDevelopmentSimulator* simulator = CityBuildingDevelopment();
    return simulator ? simulator->GetBuildingProfilePtr(buildingType) : nullptr;
}
float NativeMaxCapacity(int zoneType, std::uint32_t purpose, std::uint32_t wealth) {
    cSC4BuildingDevelopmentSimulator* simulator = CityBuildingDevelopment();
    return simulator ? simulator->GetMaxCapacity(zoneType, purpose, wealth) : 0.0f;
}
sc4::cSC4Demand* NativeDemandChannel(std::uint32_t type, std::uint32_t keyOffset) {
    cSC4DemandSimulator* demand = CityComponent<cSC4DemandSimulator>(sc4::CityComponent::DemandSimulator, kGZCLSID_cSC4DemandSimulator);
    return demand ? demand->GetDemand(type, keyOffset) : nullptr;
}
std::uint32_t NativeDeveloperType(std::uint32_t purpose, std::int32_t wealth) { return cSC4BuildingDevelopmentSimulator::GetDeveloperType_Static(purpose, wealth); }
void NativeAllBuildingTypes(std::vector<std::uint32_t>& out) {
    out.clear();
    if (cSC4BuildingDevelopmentSimulator* simulator = CityBuildingDevelopment()) simulator->GetAllBuildingTypes(out);
}
bool NativeBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) {
    cSC4BuildingDevelopmentSimulator* simulator = CityBuildingDevelopment();
    return simulator && simulator->GetBuildingKeyFromType(type, key);
}
std::int32_t NativeDayOfLastSuccessfulBuild(std::uint32_t developerType) {
    cSC4BuildingDevelopmentSimulator* simulator = CityBuildingDevelopment();
    return simulator ? simulator->GetDayOfLastSuccessfulBuild(static_cast<std::int32_t>(developerType)) : 0;
}
void NativeEnsureBuildingCatalogue() { cSC4BuildingDevelopmentSimulator::StaticInit(); }
bool NativeIsBuildingType(std::uint32_t id) { auto* s = CityBuildingDevelopment(); return s && s->IsBuildingTypeValid(id); }
bool NativeIsBuildingFamily(std::uint32_t id) { auto* s = CityBuildingDevelopment(); return s && s->IsBuildingFamilyTypeValid(id); }
void NativeBuildingsInFamily(std::uint32_t family, std::vector<std::uint32_t>& out) {
    out.clear();
    if (auto* s = CityBuildingDevelopment()) s->GetBuildingTypesByFamily(out, family);
}
std::uint16_t NativeFreeConstructionResources() { auto* s = CityBuildingDevelopment(); return s ? s->GetFreeConstructionResources() : 0; }
bool NativeClaimConstructionResources(std::uint16_t amount) { auto* s = CityBuildingDevelopment(); return s && s->ClaimConstructionResources(amount); }
void NativeReleaseConstructionResources(std::uint16_t amount) { if (auto* s = CityBuildingDevelopment()) s->ReleaseConstructionResources(amount); }
bool NativeBuildingCheat(int which) {
    auto* s = CityBuildingDevelopment();
    if (!s) return false;
    return which == 0 ? s->CheatIgnorePlumbing() : which == 1 ? s->CheatIgnorePower() : s->CheatIgnoreCaps();
}
bool NativeIsBuildingInFamily(std::uint32_t type, std::uint32_t family) { auto* s = CityBuildingDevelopment(); return s && s->IsBuildingInFamily(type, family); }
bool NativeRetainingWallExists(std::uint32_t id) {
    cGZPersistResourceKey key;
    auto* s = CityBuildingDevelopment();
    return s && s->GetLotRetainingWallKeyFromType(id, key);
}
bool NativeBuildingFitsInZone(std::uint32_t buildingPurpose, std::uint32_t zoneType) {
    return cSC4BuildingDevelopmentSimulator::BuildingFitsInZone(buildingPurpose, zoneType);
}

void InstallBuildingDevelopmentHost() {
    static BuildingDevelopmentHost host;
    sc4::SetBuildingDevelopmentSimHost(&host);
}

}  // namespace native
