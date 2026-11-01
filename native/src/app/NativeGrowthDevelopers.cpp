// cSC4GrowthDeveloper behind plain functions: GrowthDeveloper.h (through TractDeveloper.h) and BuildingDevelopmentSimulator.h
// define the same names (TractOutcome, ConstructionProfile), so the building development host reaches the class from this TU.
#include "app/NativeGrowthDevelopers.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sc4/GrowthDeveloper.h"

namespace native {

namespace {
std::vector<sc4::cSC4GrowthDeveloper*> g_developers;   // every developer made (for the debug summary)
}

void* NewGrowthDeveloper() {
    auto* developer = new sc4::cSC4GrowthDeveloper();
    developer->AddRef();
    g_developers.push_back(developer);
    return developer;
}

std::uint32_t GrowthDeveloperSubType(void* developer) {
    return developer ? static_cast<std::uint32_t>(static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GetSubType()) : 0;
}
bool GrowthDeveloperListsValid(void* developer) {
    return developer && static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GrowthAndDeclineListsAreValid();
}
void GrowthDeveloperGrowthList(void* developer, std::vector<std::array<std::int32_t, 3>>& out) {
    out.clear();
    if (!developer) return;
    for (const auto& item : static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GetGrowthList()) out.push_back({item.x, item.z, item.style});
}
float GrowthDeveloperActiveDemand(void* developer) {
    sc4::IGrowthDemandApi* demand = developer ? static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GetSourceDemand() : nullptr;
    return demand ? demand->GetActiveValue() : 0.0f;
}
sc4::cSC4GrowthDeveloper* GrowthDeveloperOfType(std::uint32_t type) {
    for (sc4::cSC4GrowthDeveloper* d : g_developers) if (static_cast<std::uint32_t>(d->GetSubType()) == type) return d;
    return nullptr;
}

std::string GrowthDevelopersSummary() {
    std::string out;
    for (sc4::cSC4GrowthDeveloper* d : g_developers) {
        char line[96];
        sc4::IGrowthDemandApi* demand = d->GetSourceDemand();
        std::snprintf(line, sizeof line, " %x:%s%zu/%zu d%.0f", d->GetSubType(), d->GrowthAndDeclineListsAreValid() ? "" : "!",
                      d->GetGrowthList().size(), d->GetDeclineList().size(), demand ? demand->GetActiveValue() : -1.0f);
        out += line;
    }
    return out;
}
bool GrowthDeveloperInit(void* developer, sc4::cISCPropertyHolder* exemplar) {
    return developer && static_cast<sc4::cSC4GrowthDeveloper*>(developer)->Init(exemplar);
}
void GrowthDeveloperStartMapUpdate(void* developer) {
    if (developer) static_cast<sc4::cSC4GrowthDeveloper*>(developer)->StartMapUpdate();
}
bool GrowthDeveloperMapUpdateFinished(void* developer) {
    return !developer || static_cast<sc4::cSC4GrowthDeveloper*>(developer)->IsMapUpdateFinished();
}

}  // namespace native

// ==================================================================================================== IGrowthDeveloperHost
// The growth developers' host (notes/GROWTH_PLAN.md layer 2; replaces the generated neutral stub, under which Init failed on
// its first service so the map update never left stage 1 and the building development simulator's month waited forever).
// Each service answers from the city's component when the port carries what the developer reads; a map the port does not expose
// (the residential wealth maps, the traffic density and congestion) is null and the map update skips that stage.

#include <chrono>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "rz/cRZRandom.h"
#include "sc4/AuraSimulator.h"
#include "sc4/CommercialSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/DBSegment.h"
#include "sc4/cSC4Demand.h"
#include "tools/city_probe_plain.h"

namespace native {
namespace {
using namespace sc4;

// A city component as its class: the COM classes through their serializable view's class id, the plain classes through the
// adapter's plain view.
template <class T> T* Component(sc4::CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    if (!object) return nullptr;
    void* raw = nullptr;
    if (object->QueryInterface(kGZIID_PlainObject, &raw) && raw) return static_cast<T*>(raw);   // borrowed, no reference
    if (!object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

struct PlainService final : IGrowthDeveloperService {} g_occupantManager;   // stored by Init, never used again (growth.md)

struct CityApi final : IGrowthCityApi {
    IGrowthDeveloperService* OccupantManager() override { return &g_occupantManager; }
    void RemoveDeveloper(cIGZMessageTarget2*) override {}
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { if (cSC4City* c = HostCity()) c->PositionToCell(x, z, cellX, cellZ); }
} g_city;

struct BuildingDevelopmentApi final : IGrowthBuildingDevelopmentApi {
    const BuildingProfile& GetBuildingProfile(std::uint32_t type) override {
        static const BuildingProfile none{};
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? *profile : none;
    }
    void GetSpecialBuildings(std::list<cRZAutoRefCount<cISC4Occupant>>&) override {}   // the civic list (+0x88) is not exposed yet
    float GetMaxCapacity(int zoneType, int purpose, int wealth) override {
        return NativeMaxCapacity(zoneType, static_cast<std::uint32_t>(purpose), static_cast<std::uint32_t>(wealth));
    }
    float GetStagePopulationMultiplier() override { return 1.0f; }   // PARTIAL: the simulator's +0x178 is not modelled
    void UpdateTractPopulation(cISC4GrowthDeveloper*, int, int) override {}   // the tract developer is layer 3
} g_buildingDevelopment;

struct LandValueApi final : IGrowthLandValueApi {
    cSC4SimGrid<std::uint8_t>* TotalLandValueMap() override { return NativeTotalLandValueMap(); }
    cSC4SimGrid<std::uint8_t>* IntrinsicLandValueMap() override { return NativeIntrinsicLandValueMap(); }
    cSC4SimGrid<float>* SlopeMap() override { return NativeSlopeMap(); }
} g_landValue;

struct TrafficApi final : IGrowthTrafficApi {   // the port's traffic simulator exposes neither map: those stages are skipped
    IGrowthTrafficDensityApi* Density() override { return nullptr; }
    cSC4SimGrid<std::uint8_t>* CongestionMap() override { return nullptr; }
} g_traffic;

struct ZoneManagerApi final : IGrowthZoneManagerApi {
    int ZoneTypeAt(int x, int z) override {
        auto* m = Component<cSC4ZoneManager>(sc4::CityComponent::ZoneManager, kZoneManagerClassId);
        return m ? m->GetZoneType(x, z) : 0;
    }
} g_zoneManager;

struct ResidentialApi final : IGrowthResidentialApi {   // its wealth / EQ maps are not exposed by the port's class
    cSC4SimGrid<std::uint8_t>* WealthMap(int) override { return nullptr; }
    cSC4SimGrid<float>* Map138() override { return nullptr; }
    cSC4SimGrid<float>* Map140() override { return nullptr; }
} g_residential;

struct CommercialApi final : IGrowthCommercialApi {
    cSC4SimGrid<std::uint8_t>* DeveloperMap(std::uint32_t type) override {
        auto* s = Component<cSC4CommercialSimulator>(sc4::CityComponent::CommercialSimulator, kGZCLSID_cSC4CommercialSimulator);
        return s ? s->GetProximityMap(type) : nullptr;
    }
} g_commercial;

struct PollutionApi final : IGrowthPollutionApi {
    cSC4SimGrid<std::int16_t>* TypeMap(int type) override {
        auto* s = Component<cSC4PollutionSimulator>(sc4::CityComponent::PollutionSimulator, kGZCLSID_cSC4PollutionSimulator);
        return s && type >= 0 && type < 4 ? s->GetPollutionGrid(static_cast<PollutionType>(type)) : nullptr;
    }
} g_pollution;

struct AuraApi final : IGrowthAuraApi {
    static cSC4AuraSimulator* Sim() {
        cSC4City* city = HostCity();
        return city ? dynamic_cast<cSC4AuraSimulator*>(city->GetComponent(sc4::CityComponent::AuraSimulator)) : nullptr;
    }
    cSC4SimGrid<std::int8_t>* Int8Map() override { auto* s = Sim(); return s ? s->GetTransientAuraGrid() : nullptr; }   // vslot 0x68
    cSC4SimGrid<std::int16_t>* Int16MapA() override { auto* s = Sim(); return s ? s->GetParkMap() : nullptr; }           // 0x70
    cSC4SimGrid<std::int16_t>* Int16MapB() override { auto* s = Sim(); return s ? s->GetLandmarkMap() : nullptr; }       // 0x78
} g_aura;

struct CrimeApi final : IGrowthCrimeApi {   // the crime simulator's grid is const in the port; not handed out as writable
    cSC4SimGrid<std::uint8_t>* CrimeMap() override { return nullptr; }
} g_crime;

// One demand channel: vslot 0x40 / 0x88 of cSC4Demand.
struct DemandChannel final : IGrowthDemandApi {
    cSC4Demand* demand = nullptr;
    float GetValue() const override { return demand ? demand->QuerySupplyValue() : 0.0f; }
    float GetActiveValue() const override { return demand ? demand->QueryActiveDemandValue() : 0.0f; }
    void AddToSupply(float amount) override { if (demand) demand->AddToSupplyValue(amount); }
};
struct DemandSimulatorApi final : IGrowthDemandSimulatorApi {
    std::map<std::uint32_t, DemandChannel> channels;
    IGrowthDemandApi* GetChannel(std::uint32_t type, std::uint32_t flags) override;
} g_demand;

struct NeighborsApi final : IGrowthNeighborsApi {
    int ConnectedCityCount() override { return 0; }
    int AllCityCount() override { return 0; }   // 0 = the demand is local
} g_neighbors;

struct GrowthHost final : IGrowthDeveloperHost {
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IGrowthCityApi* City() override { return HostCity() ? &g_city : nullptr; }
    IGrowthBuildingDevelopmentApi* BuildingDevelopment() override { return &g_buildingDevelopment; }
    IGrowthLandValueApi* LandValue() override { return &g_landValue; }
    IGrowthTrafficApi* Traffic() override { return &g_traffic; }
    IGrowthZoneManagerApi* ZoneManager() override { return &g_zoneManager; }
    IGrowthResidentialApi* Residential() override { return &g_residential; }
    IGrowthCommercialApi* Commercial() override { return &g_commercial; }
    IGrowthPollutionApi* Pollution() override { return &g_pollution; }
    IGrowthAuraApi* Aura() override { return &g_aura; }
    IGrowthCrimeApi* Crime() override { return &g_crime; }
    IGrowthDemandSimulatorApi* DemandSimulator() override { return &g_demand; }
    IGrowthNeighborsApi* Neighbors() override { return &g_neighbors; }
    std::int32_t SimDateNumber() override {
        auto* s = Component<cSC4Simulator>(sc4::CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return s ? static_cast<std::int32_t>(s->GetSimDateNumber()) : 0;
    }
    bool SimulatorPresent() override { return CitySimulatorPresent(); }
    void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {
        AddCitySimulatorAgent(target, interval, name, flags);
    }
    bool BuildingFitsInZone(std::uint32_t purpose, std::uint32_t zoneType) override { return NativeBuildingFitsInZone(purpose, zoneType); }
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override { return NowMicros() + static_cast<std::uint64_t>(milliseconds) * 1000; }
    bool SliceExpired(std::uint64_t slice) override { return NowMicros() >= slice; }
    std::uint32_t RandomUint32Raw() override { static cRZRandom random(0x6a0du); return random.RandomUint32Uniform(); }
    cSC4Lot* MessageLot(cIGZMessage2* message) override {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        cIGZUnknown* unknown = standard ? standard->GetIGZUnknown() : nullptr;
        void* raw = nullptr;
        if (unknown && unknown->QueryInterface(kGZIID_PlainObject, &raw) && raw) return static_cast<cSC4Lot*>(raw);
        return nullptr;
    }
    void WriteGrid(cIGZOStream& stream, cIGZSerializable* grid) override { SC4WriteGZSerializable(stream, grid); }
    cIGZSerializable* ReadGrid(cIGZIStream& stream) override {
        void* out = nullptr;
        SC4ReadGZSerializable(stream, 0xc9bd939a, &out, false);
        return static_cast<cIGZSerializable*>(out);
    }
    static std::uint64_t NowMicros() {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
} g_host;

IGrowthDemandApi* DemandSimulatorApi::GetChannel(std::uint32_t type, std::uint32_t flags) {
    cSC4Demand* demand = NativeDemandChannel(type, flags);   // SL::spDemandSim vslot 0x30
    if (!demand) return nullptr;
    DemandChannel& channel = channels[type + flags];
    channel.demand = demand;
    return &channel;
}

}  // namespace

// Replaces the generated neutral host; installed before the city's components Init.
void InstallGrowthDeveloperHost() { sc4::SetGrowthDeveloperHost(&g_host); }

}  // namespace native

// ---- the demand simulator's view of the growth developers (NativeDemandHost.cpp)
namespace native {
std::uint32_t GrowthDeveloperKind(void* developer) {
    return developer ? static_cast<std::uint32_t>(static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GetType()) : 0;
}
sc4::cSC4Demand* GrowthDeveloperDemandChannel(void* developer) {
    if (!developer) return nullptr;
    auto* channel = dynamic_cast<DemandChannel*>(static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GetSourceDemand());
    return channel ? channel->demand : nullptr;
}
std::int16_t GrowthDeveloperDesirabilityAt(void* developer, unsigned x, unsigned z) {
    sc4::cSC4SimGrid<std::int16_t>* map = developer ? static_cast<sc4::cSC4GrowthDeveloper*>(developer)->GetDesirabilityMap() : nullptr;
    return map ? map->GetCellValue(static_cast<std::int32_t>(x), static_cast<std::int32_t>(z)) : 0;
}
sc4::cSC4SimGrid<std::int16_t>* GrowthDeveloperDesirabilityMap(std::uint32_t type) {
    sc4::cSC4GrowthDeveloper* d = GrowthDeveloperOfType(type);
    return d ? d->GetDesirabilityMap() : nullptr;
}
std::vector<void*> AllGrowthDevelopers() {
    std::vector<void*> out;
    for (sc4::cSC4GrowthDeveloper* d : g_developers) out.push_back(d);
    return out;
}
}  // namespace native
