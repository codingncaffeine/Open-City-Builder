// Native hosts of the city simulators that ran on the city probe's neutral bindings (src/tools/city_probe_bindings.cpp), under which
// they were left uninitialised (NativeCityComponents.cpp kProbeHosted): no agents, no history, so the opinion polls never moved.
// Each host answers from the city's own components (terrain, zone manager, lot developer, growth developers, history warehouse) and
// registers the simulator's agents with the city simulator, as the original's Init does through SL::spSimulator / SL::spMS2.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeSimHosts.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/SimHostRegistry.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Occupant.h"
#include "sc4/Ordinance.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/Simulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

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

// cSC4MapUtilities' Expand (static, 0x0009de12): every cell at distance `distance` lowers its four neighbours (clamped at the edges)
// to distance + 1; true when a cell changed.
bool Expand(std::vector<std::vector<std::uint8_t>>& grid, int countX, int countZ, std::uint8_t distance) {
    const std::uint8_t next = static_cast<std::uint8_t>(distance + 1);
    bool changed = false;
    const auto lower = [&](int x, int z) {
        std::uint8_t& cell = grid[x][z];
        if (next < cell) {
            cell = next;
            changed = true;
        }
    };
    for (int x = 0; x < countX; ++x) {
        for (int z = 0; z < countZ; ++z) {
            if (grid[x][z] != distance) continue;
            lower(x - 1 < 0 ? 0 : x - 1, z);
            lower(x + 1 >= countX ? countX - 1 : x + 1, z);
            lower(x, z - 1 < 0 ? 0 : z - 1);
            lower(x, z + 1 >= countZ ? countZ - 1 : z + 1);
        }
    }
    return changed;
}

// cSC4MapUtilities::AddDistanceEffect @ 0x0009f556: the cells of `source` equal to `match` are distance 0; every cell nearer than
// `range` gains near + distance * (far - near) / range, clamped to a byte.
void AddDistanceEffect(std::vector<std::vector<std::uint8_t>>& grid, int countX, int countZ, const std::vector<std::vector<std::uint8_t>>& source,
                       std::uint8_t match, float range, float nearValue, float farValue) {
    std::vector<std::vector<std::uint8_t>> distance(countX, std::vector<std::uint8_t>(countZ, 0xff));
    for (int x = 0; x < countX; ++x)
        for (int z = 0; z < countZ; ++z)
            if (source[x][z] == match) distance[x][z] = 0;
    for (int d = 0; d < static_cast<int>(range); ++d) Expand(distance, countX, countZ, static_cast<std::uint8_t>(d));
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
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
private:
    cISC4Occupant* occupant_;
};

class PollutionHost final : public IPollutionHost {
public:
    // PARTIAL: the power simulator is not wired natively; every plant counts as powered.
    bool PowerAvailable() override { return true; }
    bool OccupantPowered(IPollutionOccupant*) override { return true; }
    // The sanitation department (0x6a42a8f5): its funding share; full funding while the department does not exist.
    float SanitationFunding() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0x6a42a8f5) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    // PARTIAL: no landfill developer natively (no landfill zones yet): no free landfill space, no transport load.
    void LandfillRange(int& low, int& high) override { low = high = 0; }
    int LandfillTransportLoad() override { return 0; }
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
    // PARTIAL: the traffic simulator is not running natively, so traffic adds no air pollution yet.
    std::uint8_t TrafficAirAt(int, int) override { return 0; }
    // PARTIAL: no neighbour deals natively (the region's connections are not modelled).
    void GarbageDeals(std::uint32_t, std::vector<IPollutionDeal*>& out) override { out.clear(); }
    // PARTIAL: the sanitation department's line items are not created natively.
    void SetSanitationLineItemCost(int, std::int64_t) override {}
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

private:
    std::map<cISC4Occupant*, std::unique_ptr<PollutionOccupant>> occupants_;
};

PollutionHost g_pollution;

cSC4Simulator* Simulator() { return Component<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator); }

// A city occupant as the fire simulator's station list keeps it.
class FireOccupant final : public IFireOccupant {
public:
    explicit FireOccupant(cISC4Occupant* occupant) : occupant_(occupant) {}
    std::uint32_t OccupantType() override { return occupant_->GetType(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    bool UsesDepartmentBudget() override {
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
    // PARTIAL: the stations' budget line items are not native yet: no per-station funding records.
    bool FundingFraction(IFireOccupant*, std::uint32_t, float&) override { return false; }
    bool FundingInfo(IFireOccupant*, std::uint32_t, int&, float&) override { return false; }
    bool SetFundingFraction(IFireOccupant*, std::uint32_t, const SC4Percentage&) override { return false; }
    float DepartmentBudgetFraction() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0x28f55a9f) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    void* DepartmentBudget() override { return nullptr; }
    void SetStrikerGroup(IFireOccupant*, bool) override {}
    void RegisterFundedStation(IFireOccupant*, std::uint32_t, bool) override {}
    float PoliceCoverageShare() override {
        auto* police = Component<cSC4PoliceSimulator>(CityComponent::PoliceSimulator, kGZCLSID_cSC4PoliceSimulator);
        return police ? police->GetPercentCoverage() : 0.0f;
    }
    // PARTIAL: no dispatch layer natively (no fire trucks yet).
    int AvailableDispatchCount(std::uint32_t) override { return 0; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t) override { return {}; }
    bool GetDispatch(std::uint32_t, ProtectionDispatchInfo&) override { return false; }
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
    std::uint32_t OccupantType() override { return occupant_->GetType(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    bool UsesDepartmentBudget() override {
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
    // PARTIAL: the stations' budget line items are not native yet: no per-station funding records.
    bool FundingFraction(IPoliceOccupant*, std::uint32_t, float&) override { return false; }
    bool FundingInfo(IPoliceOccupant*, std::uint32_t, int&, float&) override { return false; }
    float DepartmentBudgetFraction() override {
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(0xa2963983) : nullptr;
        return department ? department->GetFundingPercentage() : 1.0f;
    }
    void SetStrikerGroup(IPoliceOccupant*, bool) override {}
    void SetJailbreakGroup(IPoliceOccupant*, bool) override {}
    void RegisterFundedStation(IPoliceOccupant*, std::uint32_t, bool) override {}
    // PARTIAL: no dispatch layer natively (no police cars yet).
    int AvailableDispatchCount(std::uint32_t) override { return 0; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t) override { return {}; }
    bool GetDispatch(std::uint32_t, ProtectionDispatchInfo&) override { return false; }
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

void InstallNativeSimHosts() {
    gzcom::SimHosts& hosts = gzcom::GetSimHosts();
    hosts.landValue = &g_landValue;
    hosts.pollution = &g_pollution;
    hosts.fire = &g_fire;
    hosts.police = &g_police;
}

}  // namespace native
