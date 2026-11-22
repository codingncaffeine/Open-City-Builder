// Native hosts of the city simulators that ran on the city probe's neutral bindings (src/tools/city_probe_bindings.cpp), under which
// they were left uninitialised (NativeCityComponents.cpp kProbeHosted): no agents, no history, so the opinion polls never moved.
// Each host answers from the city's own components (terrain, zone manager, lot developer, growth developers, history warehouse) and
// registers the simulator's agents with the city simulator, as the original's Init does through SL::spSimulator / SL::spMS2.
#include <algorithm>
#include "platform/Unwired.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeSimHosts.h"
#include "app/NativePower.h"
#include "app/NativeTraffic.h"
#include "app/NativeRegion.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/SimHostRegistry.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "app/NativeBuildingDevelopment.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/CommercialSimulator.h"
#include "sc4/MapUtilities.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/Budget.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Lot.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Occupant.h"
#include "sc4/Ordinance.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/FireProtectionSimulator.h"
#include "app/NativeDemolition.h"
#include "app/NativePlumbing.h"
namespace native { void InstallTrafficNetworkMapHost(); }   // app/NativeTrafficNetworkMap.cpp
namespace native { void NativeMarkNeighborDealsDirty(); }   // app/NativeRegion.cpp
namespace sc4 { class ITrafficAutomatonHost; extern ITrafficAutomatonHost* g_loadTrafficHost; }   // tools/city_probe_hostglobals.cpp
namespace sc4 {
class IVehicleHost;
extern IVehicleHost* g_loadVehicleHost;   // tools/city_probe_hostglobals.cpp
class IPedestrianHost;
extern IPedestrianHost* g_loadPedestrianHost;
}
namespace native {
void NativeLandfillUsage(int& used, int& capacity);   // app/NativeLandfill.cpp
int NativeLandfillTileCount();
sc4::ITrafficAutomatonHost* NativeVehicleTrafficHost();   // app/NativeTrafficAutomata.cpp
sc4::IVehicleHost* NativeVehicleHost();
sc4::IPedestrianHost* NativePedestrianHost();
// app/NativeDispatch.cpp: the dispatch manager as the protection simulators see it, and its host.
int NativeDispatchCount(std::uint32_t type);
std::vector<std::uint32_t> NativeDispatchIds(std::uint32_t type);
bool NativeDispatchRecord(std::uint32_t id, sc4::ProtectionDispatchInfo& info);
bool NativeIsOccupantPowered(sc4::cISC4Occupant* occupant);   // app/NativeAnimationCoordinator.cpp
void NativeRegisterDispatchSource(std::uint32_t type, bool add);
void InstallNativeDispatchHost();
}
namespace sc4 { class IFlammabilityHost; }
namespace native { sc4::IFlammabilityHost* NativeFlammabilityHost(); void InstallCrimeHost(); void InstallClockHost(); }
#include "sc4/PoliceSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/Simulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"
#include "sc4/VisualEffect.h"
#include "sc4/ParticlesEffect.h"

namespace native {
namespace {
using namespace sc4;

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4HistoryWarehouse* History() { return Component<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse, kGZCLSID_cSC4HistoryWarehouse); }
cSC4ZoneManager* Zones() { return Component<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

// A simulator tuning exemplar {0x6534284a, 0xe7e2c2db, instance}, held for the city's life (the original keeps it in the simulator).
const cISCPropertyHolder* SimulatorExemplar(std::uint32_t instance) {
    static std::map<std::uint32_t, cISCResExemplar*> held;
    auto it = held.find(instance);
    if (it != held.end()) return it->second ? it->second->AsISCPropertyHolder() : nullptr;
    void* raw = nullptr;
    const cGZPersistResourceKey key{0x6534284a, 0xe7e2c2db, instance};
    cISCResExemplar* exemplar = (SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw) ? static_cast<cISCResExemplar*>(raw) : nullptr;
    held[instance] = exemplar;
    return exemplar ? exemplar->AsISCPropertyHolder() : nullptr;
}

// cSC4DynamicLimitTimer: a deadline in microseconds of the steady clock.
std::uint64_t NowMicroseconds() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void PostCityMessage(std::uint32_t type) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

void AddHistoryValue(std::uint32_t series, std::int64_t value) {
    static const bool trace = std::getenv("SC4_HISTORYDEBUG") != nullptr;
    if (trace) std::fprintf(stderr, "sim history: %08x <- %lld\n", series, static_cast<long long>(value));
    if (cSC4HistoryWarehouse* history = History()) history->RecordHistoryData(series, static_cast<int>(value));
}

// cSC4MapUtilities::AddDistanceEffect @ 0x0009f556: the cells of `source` equal to `match` are distance 0; every cell nearer than
// `range` gains near + distance * (far - near) / range, clamped to a byte.
void AddDistanceEffect(std::vector<std::vector<std::uint8_t>>& grid, int countX, int countZ, const std::vector<std::vector<std::uint8_t>>& source,
                       std::uint8_t match, float range, float nearValue, float farValue) {
    std::vector<std::vector<std::uint8_t>> distance(countX, std::vector<std::uint8_t>(countZ, 0xff));
    for (int x = 0; x < countX; ++x)
        for (int z = 0; z < countZ; ++z)
            if (source[x][z] == match) distance[x][z] = 0;
    for (int d = 0; d < static_cast<int>(range); ++d) cSC4MapUtilities::Expand(distance, countX, countZ, static_cast<std::uint8_t>(d));
    for (int x = 0; x < countX; ++x) {
        for (int z = 0; z < countZ; ++z) {
            const float d = distance[x][z];
            if (!(0.0f < d && d < range)) continue;
            const int value = static_cast<int>((grid[x][z] + nearValue) - d * ((nearValue - farValue) / range));
            grid[x][z] = static_cast<std::uint8_t>(value < 0 ? 0 : value > 0xff ? 0xff : value);
        }
    }
}

class LandValueTerrain final : public ILandValueTerrain {
public:
    float MinAltitude() override { cSTETerrain* t = Terrain(); return t ? t->MinHeight() : 0.0f; }
    float MaxAltitude() override { cSTETerrain* t = Terrain(); return t ? t->MaxHeight() : 0.0f; }
    float AltitudeAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool CellHasLand(int x, int z) override { cSTETerrain* t = Terrain(); return t && t->CellHasLand(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z)); }
};

class LandValueHost final : public ILandValueHost {
public:
    void AddIdleClient(cIGZMessageTarget2* target, int kind, const char* name, int priority) override {
        AddCitySimulatorAgent(target, kind, name, static_cast<std::uint32_t>(priority));
    }
    void RemoveIdleClient(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    void PostMessage(std::uint32_t message) override { PostCityMessage(message); }
    bool AcquireServices() override { return true; }
    const cISCPropertyHolder* SimulatorExemplar() override { return native::SimulatorExemplar(0xe7e2c8d8); }
    void ReleaseServices() override {}
    ILandValueTerrain* Terrain() override { return &terrain_; }
    int CellZoneType(int x, int z) override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneType(x, z) : 0; }
    void CreateHistory(std::uint32_t historyId, int kind) override {
        if (cSC4HistoryWarehouse* history = History()) history->SetHistoryDataQueueLength(historyId, static_cast<std::uint32_t>(kind));
    }
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { AddHistoryValue(historyId, value); }
    // The developer of a type is looked up when its map is wanted (the growth developers may come up after this simulator's Init):
    // the key is a stable token holding the type.
    const void* GrowthDeveloperForType(std::uint32_t developerType) override { return &*types_.insert(developerType).first; }
    cSC4SimGrid<std::int16_t>* DesirabilityMap(const void* developer) override {
        return developer ? GrowthDeveloperDesirabilityMap(*static_cast<const std::uint32_t*>(developer)) : nullptr;
    }
    // spBuildingDevelopmentSim vslot 0x90 (the lot developer) -> vslot 0x74 GetAreaSlope(rect, true)
    float AverageSlope(int x0, int z0, int x1, int z1) override { const std::int32_t rect[4] = {x0, z0, x1, z1}; return NativeAreaSlope(rect, true); }
    // DoIdle stage 2: AddDistanceEffect(grid, x, z, grid, 0, range, near, 0)
    void AddDistanceEffect(std::vector<std::vector<std::uint8_t>>& grid, int countX, int countZ, float range, float nearValue) override {
        const std::vector<std::vector<std::uint8_t>> source = grid;
        native::AddDistanceEffect(grid, countX, countZ, source, 0, range, nearValue, 0.0f);
    }
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override { return NowMicroseconds() + std::uint64_t{milliseconds} * 1000u; }
    bool SliceExpired(std::uint64_t slice) override { return NowMicroseconds() >= slice; }

private:
    LandValueTerrain terrain_;
    std::set<std::uint32_t> types_;
};

LandValueHost g_landValue;


// A city occupant as the pollution simulator's plant lists keep it.
class PollutionOccupant final : public IPollutionOccupant {
public:
    explicit PollutionOccupant(cISC4Occupant* occupant) : occupant_(occupant) {}
    cISC4Occupant* Occupant() const { return occupant_; }
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    cIGZUnknown* Object() override { return occupant_; }   // the record's building reference
private:
    cISC4Occupant* occupant_;
};

// A per-block pollution effect: the effects manager's visual effect, holding the reference it was created with.
class PollutionVisualEffect final : public IPollutionVisualEffect {
public:
    explicit PollutionVisualEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    void SetSourceTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
    void SetFloatParams(std::uint32_t type, const float* values, int count) override { effect_->SetFloatParams(type, values, count); }
    void Release() override {
        effect_->Release();
        delete this;
    }
    const cSC4VisualEffect* Effect() const { return effect_; }
private:
    cSC4VisualEffect* effect_;
};

class PollutionHost final : public IPollutionHost {
public:
    // The city's effects manager (cSC4EffectsManager::CreateVisualEffect(group, type)) and the terrain.
    IPollutionVisualEffect* CreateVisualEffect(std::uint32_t group, std::uint32_t type) override {
        cSC4VisualEffect* effect = CreateKeyedEffect(group, type);
        return effect ? new PollutionVisualEffect(effect) : nullptr;
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool OccupantPowered(IPollutionOccupant* occupant) override {
        return occupant && NativeIsOccupantPowered(static_cast<PollutionOccupant*>(occupant)->Occupant());
    }
    // The sanitation department (0x6a42a8f5): its funding share; full funding while the department does not exist.
    void AddPropRequester(cISC4PropRequester* requester) override { NativeAddPropRequester(requester); }
    float SanitationFunding() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0x6a42a8f5) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    // The landfill developer (app/NativeLandfill.cpp): vslot 0x50 GetLandfillUsage (used, capacity), vslot 0x58 GetLandfillTileCount.
    void LandfillRange(int& low, int& high) override { NativeLandfillUsage(low, high); }
    int LandfillTransportLoad() override { return NativeLandfillTileCount(); }
    void PostMessage(std::uint32_t message, std::uint32_t arg) override {
        if (!SL::spMS2) return;
        auto* m = new cRZMessage2Standard();
        m->AddRef();
        m->SetType(message);
        m->SetData1(arg);
        SL::spMS2->MessagePost(m, false);
        m->Release();
    }
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { AddHistoryValue(historyId, value); }
    float RecyclingCapacitySlots() override { return 0.0f; }
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, interval, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    const cISCPropertyHolder* SimulatorExemplar() override { return native::SimulatorExemplar(0xc911e35b); }
    void CreateHistory(std::uint32_t historyId, std::uint32_t length) override {
        if (cSC4HistoryWarehouse* history = History()) history->SetHistoryDataQueueLength(historyId, length);
    }
    float OrdinanceFactor(std::uint32_t property, std::uint32_t index) override {
        auto* ordinances = Component<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        return ordinances ? ordinances->GetCumulativeFactor(property, index) : 1.0f;
    }
    int ZoneTypeAt(int x, int z) override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneType(x, z) : 0; }
    // The traffic simulator's air-polluting traffic map (cISC4TrafficSimulator slot 8 GetAirPollutingTrafficMap, a uint8 grid at
    // cell resolution): GetCellValue(x, z) of the cell the block walk adds to its air.
    std::uint8_t TrafficAirAt(int x, int z) override {
        auto* traffic = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator);
        const cSC4SimGrid<std::uint8_t>* map = traffic ? traffic->GetAirPollutingTrafficMap() : nullptr;
        if (!map) return 0;
        const int shift = map->GetTractShift();
        return map->TractIsInBounds(x >> shift, z >> shift) ? map->GetCellValue(x, z) : 0;
    }
    // SL::spNeighborsSim vslot 0x7c MarkDealsDirty at the month's end, as the power and plumbing hosts do.
    void RefreshNeighborDeals() override { NativeMarkNeighborDealsDirty(); }
    // PARTIAL: no neighbour deals natively (the region's connections are not modelled).
    void GarbageDeals(std::uint32_t, std::vector<IPollutionDeal*>& out) override {
        SC4_UNWIRED("cSC4PollutionSimulator: the neighbor garbage deals are not read (none)");
        out.clear();
    }
    // department 0x6a42a8f5 GetLineItem(item) -> SetFullExpenses(cost) (spec pollution.md 4.9.2)
    void SetSanitationLineItemCost(int item, std::int64_t cost) override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0x6a42a8f5) : nullptr;
        if (cSC4LineItem* line = department ? department->GetLineItem(static_cast<std::uint32_t>(item)) : nullptr) line->SetFullExpenses(cost);
    }
    void EnsureSanitationDepartment() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        if (!budget || budget->GetDepartmentBudget(0x6a42a8f5)) return;
        cSC4DepartmentBudget* department = budget->CreateDepartmentBudget(0x6a42a8f5, 0x4a357eaf);
        if (!department) return;
        department->SetDepartmentName(0x6a3ff01c, 0xca4938f8);
        department->SetMaxAllowedFundingPercentage(1.2f);
        static constexpr std::pair<std::uint32_t, std::uint32_t> kLines[] = {{10, 0xca57fe96}, {20, 0x4a57fff0}, {30, 0x0a580005}};
        for (const auto& line : kLines)
            if (cSC4LineItem* item = department->CreateLineItem(line.first, false)) item->SetName(0x6a3ff01c, line.second);
    }
    void BookSanitationBuilding(cISCPropertyHolder* holder, bool added) override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        BudgetItem item;
        if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, 0xca58edc7, item)) return;
        if (item.lineItemId == 10) {
            if (added) budget->AddBudgetItemToBudget(holder, static_cast<int>(item.purpose));
            else budget->RemoveBudgetItemFromBudget(holder, static_cast<int>(item.purpose));
        }
        cSCPropertyHelp::SetPropertyValue(holder, 0x2a58d350, item.cost);
    }
    bool OccupantFootprint(void* raw, PollutionOccupantFootprint& out) override {
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        cSC4City* city = HostCity();
        if (!occupant || !city) return false;
        out.holder = occupant->AsPropertyHolder();
        float lo[3] = {}, hi[3] = {};
        occupant->GetBoundingBox(lo, hi);
        city->PositionToCell(lo[0], lo[2], out.cellX0, out.cellZ0);
        city->PositionToCell(hi[0], hi[2], out.cellX1, out.cellZ1);
        occupant->GetPosition(out.position);
        auto& adapter = occupants_[occupant];
        if (!adapter) adapter = std::make_unique<PollutionOccupant>(occupant);
        out.occupant = adapter.get();
        return true;
    }

    // the adapter the simulator lists for `occupant` (the query tool's rows), null when the simulator never saw it
    IPollutionOccupant* Adapter(cISC4Occupant* occupant) {
        auto it = occupants_.find(occupant);
        return it == occupants_.end() ? nullptr : it->second.get();
    }

private:
    std::map<cISC4Occupant*, std::unique_ptr<PollutionOccupant>> occupants_;
};

PollutionHost g_pollution;

cSC4Simulator* Simulator() { return Component<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); }

// GetStationEfficiency's funding (i386 0x00076b12): the budget item of the station's exemplar for the funding purpose, then
// the occupant's own funding when the department funds it locally, else the department's; a missing department is 100%.
bool StationFundingFraction(cISCPropertyHolder* holder, std::uint32_t purpose, float& fraction) {
    auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
    BudgetItem item;
    if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, purpose, item)) return false;
    cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
    if (!department)
        fraction = 1.0f;
    else
        fraction = department->IsLocallyFundedObjectInDepartment(holder, purpose) ? item.localFunding : department->GetFundingPercentage();
    return true;
}

// A protection station or jail joins (leaves) its department's locally funded objects (spec protection.md 4.3 steps 4 and the
// removal, 9.2): its budget item for the funding purpose, then that item's department, when both exist.
void RegisterProtectionFunding(cISCPropertyHolder* holder, std::uint32_t purpose, bool add) {
    auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
    BudgetItem item;
    if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, purpose, item)) return;
    cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
    if (!department) return;
    if (add) department->AddLocallyFundedObject(holder, purpose);
    else department->RemoveLocallyFundedObject(holder, purpose);
}

// The protection simulators' Init: a department the city has no record of (a new city) is created in budget group 0x4a357b40,
// named from table 0x6a3ff01c, capped at the tuned funding, with its line item 1 (locally funded, display flag 1 off).
void EnsureProtectionDepartment(std::uint32_t id, std::uint32_t nameInstance, float maxFunding) {
    auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
    if (!budget || budget->GetDepartmentBudget(id)) return;
    cSC4DepartmentBudget* department = budget->CreateDepartmentBudget(id, 0x4a357b40);
    if (!department) return;
    department->SetDepartmentName(0x6a3ff01c, nameInstance);
    department->SetMaxAllowedFundingPercentage(maxFunding);
    if (cSC4LineItem* item = department->CreateLineItem(1, true)) item->SetDisplayFlag(1, false);
}
// GetStationFunding's record (fire/police i386 0x00237880 / 0x00077324): budget simulator vslot 0xcc GetBudgetItemForPurpose; the
// item's cost (+0x10) and local funding (+0x1c).
bool StationFundingInfo(cISCPropertyHolder* holder, std::uint32_t purpose, int& cost, float& fraction) {
    auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
    BudgetItem item;
    if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, purpose, item)) return false;
    cost = static_cast<int>(item.cost);
    fraction = item.localFunding;
    return true;
}
// SetStationFunding (i386 0x002378fa / 0x0007739e): the item's department (budget vslot 0x6c) SetLocalFundingPercent (vslot 0x60).
bool SetStationFundingFraction(cISCPropertyHolder* holder, std::uint32_t purpose, const SC4Percentage& fraction) {
    auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
    BudgetItem item;
    if (!budget || !holder || !budget->GetBudgetItemForPurpose(holder, purpose, item)) return false;
    cSC4DepartmentBudget* department = budget->GetDepartmentBudget(static_cast<std::uint32_t>(item.departmentId));
    if (!department) return false;
    department->SetLocalFundingPercent(holder, fraction.value, purpose);
    return true;
}

// A city occupant as the fire simulator's station list keeps it.
class FireOccupant final : public IFireOccupant {
public:
    explicit FireOccupant(cISC4Occupant* occupant) : occupant_(occupant) {}
    cISC4Occupant* Occupant() const { return occupant_; }
    cIGZUnknown* Object() override { return occupant_; }   // the record's station reference
    std::uint32_t OccupantType() override { return occupant_->GetType(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    bool IsPropOccupant() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(0xe9793a65, &raw) || !raw) return false;
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    }
private:
    cISC4Occupant* occupant_;
};

class FireHost final : public IFireHost {
public:
    int CurrentDay() override { cSC4Simulator* s = Simulator(); return s ? static_cast<int>(s->GetSimDateNumber()) : 0; }
    void PostMessage(std::uint32_t message, IFireOccupant*) override { PostCityMessage(message); }
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { AddHistoryValue(historyId, value); }
    bool FundingFraction(IFireOccupant* station, std::uint32_t purpose, float& fraction) override {
        return station && StationFundingFraction(station->PropertyHolder(), purpose, fraction);
    }
    bool FundingInfo(IFireOccupant* station, std::uint32_t purpose, int& cost, float& fraction) override {
        return station && StationFundingInfo(station->PropertyHolder(), purpose, cost, fraction);
    }
    bool SetFundingFraction(IFireOccupant* station, std::uint32_t purpose, const SC4Percentage& fraction) override {
        return station && SetStationFundingFraction(station->PropertyHolder(), purpose, fraction);
    }
    float DepartmentBudgetFraction() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0x28f55a9f) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    void* DepartmentBudget() override { return nullptr; }
    void EnsureDepartment(std::uint32_t id, std::uint32_t name, float maxFunding) override { EnsureProtectionDepartment(id, name, maxFunding); }
    void SetStrikerGroup(IFireOccupant*, bool) override {}
    void RegisterFundedStation(IFireOccupant* station, std::uint32_t purpose, bool add) override {
        if (station) RegisterProtectionFunding(station->PropertyHolder(), purpose, add);
    }
    float PoliceCoverageShare() override {
        auto* police = Component<cSC4PoliceSimulator>(CityComponent::PoliceSimulator, kGZCLSID_cSC4PoliceSimulator);
        return police ? police->GetPercentCoverage() : 0.0f;
    }
    // The dispatch manager's records of fire engines (app/NativeDispatch.cpp).
    int AvailableDispatchCount(std::uint32_t purpose) override { return NativeDispatchCount(purpose); }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) override { return NativeDispatchIds(purpose); }
    bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) override { return NativeDispatchRecord(id, info); }
    void RegisterDispatchSource(bool add) override { NativeRegisterDispatchSource(1, add); }
    bool LotAllowsDispatch(IProtectionStation*) override { return true; }
    bool IsAboveWater(float x, float z) override { cSTETerrain* t = Terrain(); return !t || t->GetAltitude(x, z) > t->GetSeaLevel(); }
    bool IsZonedAt(int x, int z) override { cSC4ZoneManager* m = Zones(); return m && m->GetZoneType(x, z) != 0; }
    void TakeStationDispatch(IProtectionStation*) override {}
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, interval, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    const cISCPropertyHolder* SimulatorExemplar() override { return native::SimulatorExemplar(0x68f5524a); }
    IFireOccupant* FireOccupant(void* raw) override {
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        if (!occupant) return nullptr;
        auto& adapter = occupants_[occupant];
        if (!adapter) adapter = std::make_unique<native::FireOccupant>(occupant);
        return adapter.get();
    }
private:
    std::map<cISC4Occupant*, std::unique_ptr<native::FireOccupant>> occupants_;
};

FireHost g_fire;

// A city occupant as the police simulator's station list keeps it.
class PoliceOccupant final : public IPoliceOccupant {
public:
    explicit PoliceOccupant(cISC4Occupant* occupant) : occupant_(occupant) {}
    cISC4Occupant* Occupant() const { return occupant_; }
    cIGZUnknown* Object() override { return occupant_; }   // the record's station reference
    std::uint32_t OccupantType() override { return occupant_->GetType(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    bool IsPropOccupant() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(0xe9793a65, &raw) || !raw) return false;
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    }
private:
    cISC4Occupant* occupant_;
};

class PoliceHost final : public IPoliceHost {
public:
    int CurrentDay() override { cSC4Simulator* s = Simulator(); return s ? static_cast<int>(s->GetSimDateNumber()) : 0; }
    void PostMessage(std::uint32_t message, IPoliceOccupant*) override { PostCityMessage(message); }
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { AddHistoryValue(historyId, value); }
    bool FundingFraction(IPoliceOccupant* station, std::uint32_t purpose, float& fraction) override {
        return station && StationFundingFraction(station->PropertyHolder(), purpose, fraction);
    }
    bool FundingInfo(IPoliceOccupant* station, std::uint32_t purpose, int& cost, float& fraction) override {
        return station && StationFundingInfo(station->PropertyHolder(), purpose, cost, fraction);
    }
    bool SetFundingFraction(IPoliceOccupant* station, std::uint32_t purpose, const SC4Percentage& fraction) override {
        return station && SetStationFundingFraction(station->PropertyHolder(), purpose, fraction);
    }
    float DepartmentBudgetFraction() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0xa2963983) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    void EnsureDepartment(std::uint32_t id, std::uint32_t name, float maxFunding) override { EnsureProtectionDepartment(id, name, maxFunding); }
    void SetStrikerGroup(IPoliceOccupant*, bool) override {}
    void SetJailbreakGroup(IPoliceOccupant*, bool) override {}
    void RegisterFundedStation(IPoliceOccupant* station, std::uint32_t purpose, bool add) override {
        if (station) RegisterProtectionFunding(station->PropertyHolder(), purpose, add);
    }
    // The dispatch manager's records of police cars (app/NativeDispatch.cpp).
    int AvailableDispatchCount(std::uint32_t purpose) override { return NativeDispatchCount(purpose); }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) override { return NativeDispatchIds(purpose); }
    bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) override { return NativeDispatchRecord(id, info); }
    void RegisterDispatchSource(bool add) override { NativeRegisterDispatchSource(0, add); }
    bool LotAllowsDispatch(IProtectionStation*) override { return true; }
    bool IsAboveWater(float x, float z) override { cSTETerrain* t = Terrain(); return !t || t->GetAltitude(x, z) > t->GetSeaLevel(); }
    bool IsZonedAt(int x, int z) override { cSC4ZoneManager* m = Zones(); return m && m->GetZoneType(x, z) != 0; }
    void TakeStationDispatch(IProtectionStation*) override {}
    void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, interval, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    const cISCPropertyHolder* SimulatorExemplar() override { return native::SimulatorExemplar(0x28dfcf89); }
    IPoliceOccupant* PoliceOccupant(void* raw) override {
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        if (!occupant) return nullptr;
        auto& adapter = occupants_[occupant];
        if (!adapter) adapter = std::make_unique<native::PoliceOccupant>(occupant);
        return adapter.get();
    }
private:
    std::map<cISC4Occupant*, std::unique_ptr<native::PoliceOccupant>> occupants_;
};

PoliceHost g_police;

}  // namespace

sc4::IFireOccupant* NativeFireStation(sc4::cISC4Occupant* occupant) { return g_fire.FireOccupant(occupant); }
sc4::IPoliceOccupant* NativePoliceStation(sc4::cISC4Occupant* occupant) { return g_police.PoliceOccupant(occupant); }
sc4::cISC4Occupant* NativeStationOccupant(sc4::IProtectionStation* station) {
    if (auto* fire = dynamic_cast<native::FireOccupant*>(station)) return fire->Occupant();
    if (auto* police = dynamic_cast<native::PoliceOccupant*>(station)) return police->Occupant();
    return nullptr;
}

namespace {

// The buzzards over a sick tract (residential.md 5.5 step 2).
class ResidentialEffect final : public IResidentialEffect {
public:
    explicit ResidentialEffect(const char* name) : name_(name) {}
    void StartAt(float x, float y, float z) override { native::StartNamedEffect(name_, x, y, z); }
private:
    const char* name_;
};

// A school, hospital or health-affecting building as the residential simulator's lists keep it (REH 4.1): one adapter per
// occupant, holding no reference of its own (the lists AddRef the occupant through it).
class ResidentialOccupant final : public IResidentialOccupant {
public:
    explicit ResidentialOccupant(cISC4Occupant* occupant) : occupant_(occupant) {}
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    bool IsInGroup(std::uint32_t group) override { return occupant_->IsOccupantGroup(group); }
    std::uint32_t BuildingType() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return 0;
        auto* building = static_cast<cISC4BuildingOccupant*>(raw);
        const std::uint32_t type = building->GetBuildingType();
        building->Release();
        return type;
    }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    // lot manager GetOccupantLot -> GetCenter; the port's lot map is the zone developer's
    void LotCenter(float xyz[3]) override {
        auto* lots = Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId);
        if (cSC4Lot* lot = lots ? lots->GetOccupantLot(occupant_) : nullptr) lot->GetCenter(xyz);
        else occupant_->GetPosition(xyz);
    }
    void AddRef() override { occupant_->AddRef(); }
    void Release() override { occupant_->Release(); }
    cIGZUnknown* Object() override { return occupant_; }
private:
    cISC4Occupant* occupant_;
};

class ResidentialHost final : public IResidentialHost {
public:
    std::uint32_t CurrentDay() override { cSC4Simulator* s = Simulator(); return s ? s->GetSimDateNumber() : 0; }
    void PostStrikeMessage(std::uint32_t message, IResidentialOccupant*, bool, std::uint32_t) override { PostCityMessage(message); }
    void SetStrikerGroup(bool, IResidentialOccupant*, bool) override {   // PARTIAL: no automata controllers for strikers yet
        SC4_UNWIRED("cSC4ResidentialSimulator strike: the strikers' automata controller on the building is not created");
    }
    bool CityHasOccupantManager() override { cSC4City* c = HostCity(); return c && c->GetComponent(CityComponent::OccupantManager); }
    std::uint8_t CellWorkforce(unsigned, unsigned) override { return 0; }   // only without grids (the class answers itself)
    std::uint8_t GlobalWorkforce() override { return 0; }
    float CellLifeExpectancyBase(unsigned, unsigned) override { return 0.0f; }

    // ---- the education/health half ----
    IResidentialOccupant* Occupant(cIGZUnknown* object) override {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return nullptr;
        static_cast<cIGZUnknown*>(raw)->Release();
        raw = nullptr;
        if (!object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        occupant->Release();
        auto& adapter = occupants_[occupant];
        if (!adapter) adapter = std::make_unique<ResidentialOccupant>(occupant);
        return adapter.get();
    }
    cSC4BudgetSimulator* BudgetSimulator() override { return Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
    void EnactedOrdinances(std::vector<const cISCPropertyHolder*>& out) override {
        auto* ordinances = Component<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, kGZCLSID_cSC4OrdinanceSimulator);
        if (!ordinances) return;
        std::uint32_t count = static_cast<std::uint32_t>(ordinances->GetOrdinanceOnCount());
        std::vector<std::uint32_t> ids(count);
        if (count) ordinances->GetOrdinanceOnArray(ids.data(), count);
        ids.resize(count);
        std::sort(ids.begin(), ids.end());
        for (std::uint32_t id : ids)
            if (cISC4Ordinance* ordinance = ordinances->GetOrdinanceByID(id)) out.push_back(ordinance->GetMiscProperties());
    }

    // ---- the population half ----
    bool AcquireServices() override { return HostCity() != nullptr; }
    void AddAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) override { AddCitySimulatorAgent(target, level, name, flags); }
    void RemoveAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    void CreateHistory(std::uint32_t id, std::uint32_t length) override {
        if (cSC4HistoryWarehouse* history = History()) history->SetHistoryDataQueueLength(id, length);
    }
    void RecordHistory(std::uint32_t id, std::int32_t value) override { AddHistoryValue(id, value); }
    const cISCPropertyHolder* SimulatorExemplar() override { return native::SimulatorExemplar(0x69189c84); }
    cSC4SimGrid<std::int16_t>* DeveloperPopulationMap(int wealth) override {
        return GrowthDeveloperPopulationMap(0x1000u + 0x10u * static_cast<std::uint32_t>(wealth));
    }
    // pollution slot 6: max(0, the source grid's value)
    std::uint16_t PollutionValue(int type, int x, int z) override {
        auto* p = Component<cSC4PollutionSimulator>(CityComponent::PollutionSimulator, kGZCLSID_cSC4PollutionSimulator);
        cSC4SimGrid<std::int16_t>* grid = p ? p->GetPollutionGrid(static_cast<PollutionType>(type)) : nullptr;
        if (!grid) return 0;
        const std::int32_t shift = grid->GetTractShift();
        if (!grid->TractIsInBounds(x >> shift, z >> shift)) return 0;
        const std::int16_t v = grid->GetCellValue(x, z);
        return static_cast<std::uint16_t>(v < 0 ? 0 : v);
    }
    // The building development simulator's building map (the proximity tract's seed, residential.md 5.3).
    const cSC4SimGrid<std::uint8_t>* BuildingMap() override {   // SL::spBuildingDevelopmentSim vslot 0xa4
        cSC4BuildingDevelopmentSimulator* simulator = NativeBuildingDevelopmentSimulator();
        return simulator ? simulator->GetBuildingMap() : nullptr;
    }
    int CurrentMonth() override {
        cSC4Simulator* s = Simulator();
        int month = 0;
        if (s) s->GetSimDate(nullptr, &month, nullptr, nullptr, nullptr);
        return month;
    }
    bool CreateEffect(const char* name, IResidentialEffect** out) override {
        *out = nullptr;
        if (!native::HasNamedEffect(name)) return false;
        static ResidentialEffect buzzards("buzzards");
        *out = &buzzards;
        return true;
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override { return NowMicroseconds() + std::uint64_t{milliseconds} * 1000u; }
    bool SliceExpired(std::uint64_t slice) override { return NowMicroseconds() >= slice; }
private:
    std::map<cISC4Occupant*, std::unique_ptr<ResidentialOccupant>> occupants_;
};

ResidentialHost g_residential;

// cSC4CommercialSimulator's city (i386 0x00170c0a Init, 0x00170976 DoIdle): the simulator agent at kinds 0, 10, 4, 6, the
// network-change notifications, and its proximity maps flood from the building development simulator's building map.
class CommercialHost final : public ICommercialHost {
public:
    void AddIdleClient(cIGZMessageTarget2* target, int kind, const char* name, int priority) override {
        AddCitySimulatorAgent(target, kind, name, static_cast<std::uint32_t>(priority));
    }
    void RemoveIdleClient(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    // The original holds the city, the building development simulator and the city's services 0x3000 / 0x3800; it fails
    // without the city. The simulator is looked up when a map is filled (the city creates its components in turn).
    bool AcquireServices() override { return HostCity() != nullptr; }
    void ReleaseServices() override {}
    // SL::spBuildingDevelopmentSim: FloodFillDistance(grid, GetBuildingMap() (vslot 0xa4), GetBuildingMapKey(3, kind) (vslot 0xac))
    void FloodFillDistance(cSC4SimGrid<std::uint8_t>& grid, int sourceKind) override {
        cSC4BuildingDevelopmentSimulator* simulator = NativeBuildingDevelopmentSimulator();
        if (!simulator) return;
        cSC4MapUtilities::FloodFillDistance(&grid, simulator->GetBuildingMap(),
                                            simulator->GetBuildingMapKey(3u, static_cast<std::uint32_t>(sourceKind)));
    }
};
CommercialHost g_commercial;

cSC4ResidentialSimulator* Residential() {
    return Component<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator, kGZCLSID_cSC4ResidentialSimulator);
}

}  // namespace

cSC4SimGrid<std::uint8_t>* NativeResidentialProximityMap(int wealth) {
    cSC4ResidentialSimulator* r = Residential();
    return r ? r->GetProximityMap(static_cast<std::uint32_t>(wealth)) : nullptr;
}
float NativeResidentialWorkforcePercent(int x, int z) {
    cSC4ResidentialSimulator* r = Residential();
    return r ? r->GetCellWorkforcePercent(SimulatorExemplar(0x69189c84), unsigned(x), unsigned(z)) : 1.0f;
}
// The automata generators' inputs (cSC4AutomataGenerator::TriggerGenerator, i386 0x000b8dd2): the city's population, and the
// pupils / patients a school or hospital serves (GetSchoolQueryData / GetHospitalQueryData).
int NativeResidentialPopulation() {
    cSC4ResidentialSimulator* r = Residential();
    return r ? r->GetPopulation() : 0;
}
bool NativeResidentialServed(cIGZUnknown* building, bool school, std::uint32_t& served) {
    cSC4ResidentialSimulator* r = Residential();
    IResidentialOccupant* occupant = r ? g_residential.Occupant(building) : nullptr;
    ResidentialQueryData data;
    served = 0;
    if (!occupant) return false;
    const bool ok = school ? r->GetSchoolQueryData(occupant, data) : r->GetHospitalQueryData(occupant, data);
    served = data.served;   // the original reads the record whether or not the query succeeded
    return ok;
}
sc4::IResidentialOccupant* NativeResidentialOccupant(cIGZUnknown* building) {
    return Residential() ? g_residential.Occupant(building) : nullptr;
}
sc4::IPollutionOccupant* NativePollutionOccupantFor(sc4::cISC4Occupant* occupant) { return g_pollution.Adapter(occupant); }
cSC4SimGrid<float>* NativeResidentialGradeMap(bool school) {
    cSC4ResidentialSimulator* r = Residential();
    return r ? r->GetGradeMap(school) : nullptr;
}

const sc4::cISCPropertyHolder* NativeSimulatorExemplar(std::uint32_t instance) { return SimulatorExemplar(instance); }
std::uint64_t NativeNowMicroseconds() { return NowMicroseconds(); }

void InstallNativeSimHosts() {
    gzcom::SimHosts& hosts = gzcom::GetSimHosts();
    hosts.landValue = &g_landValue;
    hosts.pollution = &g_pollution;
    hosts.power = NativePowerHost();
    hosts.plumbing = NativePlumbingHost();
    InstallTrafficNetworkMapHost();
    sc4::g_loadTrafficHost = NativeVehicleTrafficHost();   // the loaded and created vehicles drive on the native map
    sc4::g_loadVehicleHost = NativeVehicleHost();
    sc4::g_loadPedestrianHost = NativePedestrianHost();   // and the pedestrians walk on it
    hosts.fire = &g_fire;
    hosts.flammability = NativeFlammabilityHost();   // app/NativeDisasters.cpp
    InstallCrimeHost();                              // app/NativeCrime.cpp: before the city creates its crime simulator
    InstallClockHost();                              // app/NativeLighting.cpp: before the city creates its 24-hour clock
    hosts.police = &g_police;
    hosts.residential = &g_residential;
    hosts.commercial = &g_commercial;
    hosts.demolition = NativeDemolitionHost();
    InstallNativeTrafficHost();
    InstallNeighborHosts();
    InstallNativeDispatchHost();
}

}  // namespace native

namespace native {
int NativeWaterPollutionAt(int x, int z) {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::PollutionSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return 0;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    if (serializable->GetGZCLSID() != sc4::kGZCLSID_cSC4PollutionSimulator) return 0;
    int value = 0;
    if (x >= 0 && z >= 0) static_cast<sc4::cSC4PollutionSimulator*>(serializable)->GetWaterValue(unsigned(x), unsigned(z), value);
    return value;
}
// The city occupant behind a polluter as the pollution simulator lists it (the data views' highlights).
sc4::cISC4Occupant* NativePollutionOccupant(sc4::IPollutionOccupant* polluter) {
    auto* adapter = dynamic_cast<PollutionOccupant*>(polluter);
    return adapter ? adapter->Occupant() : nullptr;
}
}  // namespace native

namespace native {
void NativePollutionEffectReport(int limit) {
    auto* p = Component<sc4::cSC4PollutionSimulator>(sc4::CityComponent::PollutionSimulator, sc4::kGZCLSID_cSC4PollutionSimulator);
    if (!p) { std::printf("pollutionfx: no pollution simulator\n"); return; }
    int x = 0, z = 0, v = 0;
    p->GetWorstAirTract(x, z, v);     std::printf("pollutionfx: worst air cell (%d,%d) value %d\n", x, z, v);
    p->GetWorstWaterTract(x, z, v);   std::printf("pollutionfx: worst water cell (%d,%d) value %d\n", x, z, v);
    p->GetWorstGarbageTract(x, z, v); std::printf("pollutionfx: worst garbage cell (%d,%d) value %d\n", x, z, v);
    static const char* const kKinds[4] = {"water", "air", "radiation", "garbage"};
    const int blocksX = p->EffectBlocksX();
    std::printf("pollutionfx: surface view %d, blocks %d x %zu\n", p->EffectsVisible() ? 1 : 0, blocksX,
                blocksX > 0 ? p->EffectSlots(sc4::cSC4PollutionSimulator::kEffectWater).size() / std::size_t(blocksX) : std::size_t(0));
    for (int kind = 0; kind < 4; ++kind) {
        const auto& slots = p->EffectSlots(static_cast<sc4::cSC4PollutionSimulator::EffectKind>(kind));
        int present = 0, started = 0, shown = 0, components = 0;
        std::uint64_t spawned = 0;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto* slot = static_cast<PollutionVisualEffect*>(slots[i]);
            if (!slot) continue;
            ++present;
            const cSC4VisualEffect* effect = slot->Effect();
            if (effect->IsStarted()) ++started;
            for (const auto& record : effect->Components()) {
                ++components;
                if (const cSC4ParticlesEffect* particles = record.component ? record.component->ParticleSystem() : nullptr)
                    spawned += static_cast<std::uint64_t>(particles->LiveParticleCount());
            }
            if (shown++ >= limit) continue;
            const cS3DTransform& t = effect->SourceTransform();
            std::printf("pollutionfx:   %s block (%d,%d) effect %08x/%u slot %d started %d at (%.1f, %.1f, %.1f)\n", kKinds[kind],
                        int(i % std::size_t(blocksX)), int(i / std::size_t(blocksX)), effect->Group(), effect->Type(), effect->Slot(),
                        effect->IsStarted() ? 1 : 0, t.translation[0], t.translation[1], t.translation[2]);
        }
        std::printf("pollutionfx: %s effects %d (started %d), components %d, live particles %llu\n", kKinds[kind], present, started,
                    components, static_cast<unsigned long long>(spawned));
    }
}
}  // namespace native

namespace native {
void NativeTrafficAirReport() {
    sc4::cSC4City* city = HostCity();
    auto* p = Component<sc4::cSC4PollutionSimulator>(sc4::CityComponent::PollutionSimulator, sc4::kGZCLSID_cSC4PollutionSimulator);
    if (!city || !p) { std::printf("trafficair: no city or pollution simulator\n"); return; }
    int cells = 0, maxX = -1, maxZ = -1, maxValue = 0;
    std::uint64_t sum = 0;
    for (int x = 0; x < city->CellCountX(); ++x)
        for (int z = 0; z < city->CellCountZ(); ++z) {
            const int value = g_pollution.TrafficAirAt(x, z);
            if (!value) continue;
            ++cells;
            sum += static_cast<std::uint64_t>(value);
            if (value > maxValue) { maxValue = value; maxX = x; maxZ = z; }
        }
    int air = 0;
    if (maxX >= 0) p->GetAirValue(unsigned(maxX), unsigned(maxZ), air);
    std::printf("trafficair: %d cells with traffic air, sum %llu, busiest (%d,%d) = %d (its air %d), city average air %d, traffic air factor %g\n",
                cells, static_cast<unsigned long long>(sum), maxX, maxZ, maxValue, air, p->GetAverageAirValue(), p->Tunables().trafficAirFactor);
}
}  // namespace native

namespace native {
// kit command `budget`: every budget group with its departments (funding, cap) and their line items (full / current expenses).
void NativeBudgetReport() {
    auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
    if (!budget) { std::printf("budget: no budget simulator\n"); return; }
    std::vector<cSC4BudgetSimulator::BudgetGroupInfo> groups;
    budget->GetAllGroups(groups);
    for (const auto& group : groups) {
        std::printf("budget group %08x \"%s\" expense %lld income %lld\n", group.id, group.name.c_str(),
                    static_cast<long long>(group.expense), static_cast<long long>(group.income));
        std::vector<cSC4DepartmentBudget*> departments;
        budget->GetDepartmentBudgetsInGroup(group.id, departments);
        for (cSC4DepartmentBudget* department : departments) {
            if (!department) continue;
            cRZString name;
            department->GetDepartmentName(name);
            std::printf("budget department %08x \"%s\" funding %.2f cap %.2f\n", department->GetDepartmentID(), name.ToChar(),
                        department->GetFundingPercentage(), department->GetMaxAllowedFundingPercentage());
            std::vector<cSC4LineItem*> items;
            department->GetAllLineItems(items);
            for (cSC4LineItem* item : items) {
                cRZString itemName;
                item->GetName(itemName);
                std::printf("budget   line %u \"%s\" full %lld current %lld local %d flags %u\n", item->GetID(), itemName.ToChar(),
                            static_cast<long long>(item->GetFullExpenses()), static_cast<long long>(item->GetCurrentExpenses()),
                            item->IsLocalFundingItem() ? 1 : 0, item->GetDisplayFlags());
                item->Release();
            }
        }
    }
}
}  // namespace native
