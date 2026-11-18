// IPropDeveloperHost on the native city: the prop developer (cSC4PropDeveloper) switches night lights, time-of-day, seasonal,
// global-effect and power props and places a lot's optional props. Each service is the city component the original reaches
// through its SL:: global; the slot comments are the developer's (64-bit vtable byte offsets = i386 slot x 8, checked
// against re/names/mac/vtables.tsv).
//
// PARTIAL (named): a network lot's props are created by the network lot manager, whose own host is still the neutral one
// (app/hosts/StubNetworkLotManager.cpp: AddStreetProp makes nothing), so the developer places no optional street props yet.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeLighting.h"
#include "app/NativeLotConstruction.h"
#include "app/NativePower.h"
#include "app/NativeZoneDeveloperHost.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"
#include "sc4/LightingManager.h"
#include "sc4/Lot.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotDeveloper.h"
#include "sc4/NetworkLotManager.h"
#include "sc4/OccupantManager.h"
#include "sc4/PowerSimulator.h"
#include "sc4/PropDeveloper.h"
#include "sc4/PropManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace sc4 { void SetCityProbeWealthHost(IWealthHost* host); }   // tools/city_probe_bindings.cpp: the host saved requesters get

namespace native {
namespace {
using namespace sc4;

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
template <class T> T* Plain(CityComponent c) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}
template <class T> T* Serializable(CityComponent c, std::uint32_t clsid) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? dynamic_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4PropManager* Props() { return Plain<cSC4PropManager>(CityComponent::PropManager); }
cSC4NetworkLotManager* NetworkLots() { return dynamic_cast<cSC4NetworkLotManager*>(Component(CityComponent::NetworkLotManager)); }
cSC4LotDeveloper* LotDeveloper() { return Serializable<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4ZoneManager* Zones() { return Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC424HourClock* Clock24() { return Plain<cSC424HourClock>(CityComponent::Clock24Hour); }
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();   // the city keeps the manager
    return manager;
}
cSTETerrain* Terrain() {
    cIGZUnknown* object = Component(CityComponent::Terrain);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}

bool Trace() {
    static const bool on = std::getenv("SC4_PROPDEVDEBUG") != nullptr;
    return on;
}

// One reference per entry moves from the occupant list to the vector.
int Move(OccupantList& from, std::vector<sc4::cISC4Occupant*>& to) {
    for (sc4::cISC4Occupant* occupant : from) to.push_back(occupant);
    from.clear();
    return static_cast<int>(to.size());
}

// ---- the services (one object each for the life of the process; AddRef / Release are the stored pointer's, no-ops here) ----

class LotManagerApi final : public IPropLotManagerApi {
public:
    cSC4Lot* GetLot(int x, int z, bool exactCell) override { return NativeLotAtCell(x, z, exactCell); }   // vslot 0x28 (borrowed)
} g_lots;

class PropManagerApi final : public IPropManagerApi {
public:
    bool IsPropTypeValid(std::uint32_t type) override { cSC4PropManager* p = Props(); return p && p->IsPropTypeValid(type); }
    int GetPropTypesByFamily(std::vector<std::uint32_t>& out, std::uint32_t family) override {
        cSC4PropManager* p = Props();
        return p ? p->GetPropTypesByFamily(out, family) : 0;
    }
    const PropProfile* GetPropProfile(std::uint32_t type) override { cSC4PropManager* p = Props(); return p ? p->GetPropProfilePtr(type) : nullptr; }
    int PropsInLot(std::vector<sc4::cISC4Occupant*>& out, cSC4Lot* lot) override {   // i386 slot 23 GetLotProps(list, lot)
        cSC4PropManager* p = Props();
        OccupantList list;
        if (p) p->GetLotProps(list, lot);
        return Move(list, out);
    }
    void RemoveProp(sc4::cISC4Occupant* occupant) override { if (cSC4PropManager* p = Props()) p->RemovePropA(occupant); }   // slot 29
    int PropsInCell(std::vector<sc4::cISC4Occupant*>& out, int x, int z) override {   // slot 30 GetStreetProps(list, x, z)
        cSC4PropManager* p = Props();
        OccupantList list;
        if (p) p->GetStreetProps(list, x, z);
        return Move(list, out);
    }
    bool PropsInRect(std::vector<sc4::cISC4Occupant*>& out, const SC4Rect<int>& rect) override {   // slot 31 GetStreetProps(list, rect)
        cSC4PropManager* p = Props();
        if (!p) return false;
        OccupantList list;
        const int cells[4] = {rect.x0, rect.z0, rect.x1, rect.z1};
        const bool ok = p->GetStreetProps(list, cells);
        Move(list, out);
        return ok;
    }
} g_props;

class PowerApi final : public IPropPowerApi {
public:
    bool IsPowered(int x, int z) override { cSC4PowerSimulator* power = NativePowerSimulator(); return power && power->IsPowered(x, z); }   // vslot 0x128
} g_power;

class NetworkLotManagerApi final : public IPropNetworkLotManagerApi {
public:
    cSC4NetworkLot* GetNetworkLot(int x, int z) override { cSC4NetworkLotManager* m = NetworkLots(); return m ? m->GetNetworkLot(x, z) : nullptr; }
    sc4::cISC4Occupant* CreateProp(cSC4NetworkLot* lot, int objectIndex, std::uint32_t propType) override {   // vslot 0x40
        cSC4NetworkLotManager* m = NetworkLots();
        INetworkLotPropOccupant* prop = m ? m->CreateProp(lot, static_cast<std::uint32_t>(objectIndex), propType) : nullptr;
        if (Trace()) std::fprintf(stderr, "prop developer: optional street prop %08x for object %d -> %s\n", propType, objectIndex, prop ? "made" : "none (PARTIAL)");
        if (!prop) return nullptr;
        void* raw = nullptr;
        sc4::cISC4Occupant* occupant = prop->QueryInterface(kGZIID_cISC4Occupant, &raw) && raw ? static_cast<sc4::cISC4Occupant*>(raw) : nullptr;
        prop->Release();   // the occupant view holds the reference handed on
        return occupant;
    }
} g_networkLots;

class LotDeveloperApi final : public IPropLotDeveloperApi {
public:
    sc4::cISC4Occupant* CreateLotProp(cSC4Lot* lot, int objectIndex, std::uint32_t propType) override {   // lot developer vslot 0x1f8
        cSC4LotDeveloper* developer = LotDeveloper();
        sc4::cISC4Occupant* prop = developer ? developer->CreateLotProp(lot, static_cast<std::uint32_t>(objectIndex), propType) : nullptr;
        if (Trace()) std::fprintf(stderr, "prop developer: optional prop %08x for object %d -> %s\n", propType, objectIndex, prop ? "made" : "FAILED");
        return prop;
    }
} g_lotDeveloper;

class BuildingDevelopmentApi final : public IPropBuildingDevelopmentApi {
public:
    IPropLotDeveloperApi* GetLotDeveloper() override { return LotDeveloper() ? &g_lotDeveloper : nullptr; }   // vslot 0x120
} g_buildingDevelopment;

class SimulatorApi final : public IPropSimulatorApi {
public:
    cGZDate Date() override { cSC4Simulator* s = CitySimulatorObject(); return s ? s->GetSimDate() : cGZDate(); }
    void RemoveAgent(cIGZMessageTarget2* agent) override { RemoveCitySimulatorAgent(agent); }
} g_simulator;

class ClockApi final : public IPropClockApi {
public:
    int VisualTimeMode() const override { cSC424HourClock* c = Clock24(); return c ? static_cast<int>(c->GetVisualTimeMode()) : 0; }
    std::uint32_t Hour() const override { cSC424HourClock* c = Clock24(); return c ? c->GetHour() : 12; }
    std::uint32_t Minute() const override { cSC424HourClock* c = Clock24(); return c ? c->GetMinute() : 0; }
} g_clock;

class LightingApi final : public IPropLightingApi {
public:
    bool IsNight() override { cSC4LightingManager* l = LightingManager(); return l && l->IsNight(); }
} g_lighting;

class View3DApi final : public IPropView3DApi {
public:
    bool GlobalEffectTriggered(std::uint32_t effectId) override { cSC4View3DWin* v = CityView3DWin(); return v && v->GetGlobalEffectTriggerState(effectId); }
} g_view;

class TerrainApi final : public IPropTerrainApi {
public:
    float Height(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float WaterHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetWaterLevel(x, z) : 0.0f; }
} g_terrain;

class OccupantManagerApi final : public IPropOccupantManagerApi {
public:
    void UpdateOccupant(sc4::cISC4Occupant* occupant, bool placed) override { if (cISC4OccupantManager* m = Occupants()) m->MoveOccupant(occupant, placed); }   // slot 24
} g_occupants;

class CityApi final : public IPropCityApi {
public:
    float CellSizeX() override { cSC4City* c = HostCity(); return c ? c->CellWidthX() : 16.0f; }
    float CellSizeZ() override { cSC4City* c = HostCity(); return c ? c->CellWidthZ() : 16.0f; }
    int CellCountX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CellCountZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* c = HostCity()) c->PositionToCell(x, z, cellX, cellZ);
        else { cellX = static_cast<int>(x / 16.0f); cellZ = static_cast<int>(z / 16.0f); }
    }
} g_city;

// The default wealth requester's view of the city.
class WealthHost final : public IWealthHost {
public:
    bool HasLot(int x, int z) override { return NativeLotAtCell(x, z, false) != nullptr; }   // lot manager GetLot(x, z, 0)
    int LotWealth(int x, int z) override {                                                     // lot vslot 0x148 GetOccupantWealth
        cSC4Lot* lot = NativeLotAtCell(x, z, false);
        return lot ? lot->GetOccupantWealth() : 0;
    }
    int CityCellWealth(int x, int z) override {   // city vslot 0x150 (network lot manager) -> vslot 0x50 GetWealthForNetwork
        cSC4NetworkLotManager* m = NetworkLots();
        return m ? static_cast<int>(m->GetWealthForNetwork(x, z)) : 0;
    }
    unsigned ZoneType(int x, int z) override { cSC4ZoneManager* zones = Zones(); return zones ? static_cast<unsigned>(zones->GetZoneType(x, z)) : 0; }
    int PropWealth(std::uint32_t propId) override {   // the prop record's +0x14 (property 0xe9a316eb)
        cSC4PropManager* p = Props();
        const PropProfile* profile = p ? p->GetPropProfilePtr(propId) : nullptr;
        return profile ? profile->flag : 0;
    }
} g_wealth;

class Host final : public IPropDeveloperHost {
public:
    IPropLotManagerApi* LotManager() override { return &g_lots; }
    IPropManagerApi* PropManager() override { return Props() ? &g_props : nullptr; }
    IPropPowerApi* PowerSimulator() override { return &g_power; }
    IPropNetworkLotManagerApi* NetworkLotManager() override { return NetworkLots() ? &g_networkLots : nullptr; }
    IPropBuildingDevelopmentApi* BuildingDevelopment() override { return &g_buildingDevelopment; }
    IPropSimulatorApi* Simulator() override { return CitySimulatorObject() ? &g_simulator : nullptr; }
    const std::vector<cSC4LotConfigurationObject>* LotConfigurationObjects(cSC4Lot* lot) override {
        cSC4LotConfiguration* configuration = lot ? NativeLotConfiguration(lot->GetLotConfiguration()) : nullptr;
        return configuration ? &configuration->GetLotConfigurationObjectArray() : nullptr;
    }
    // The lot-state message carries a copy of the lot as its object (app/NativeZoneDeveloperHost.cpp PostLotMessage).
    cSC4Lot* MessageLot(cIGZMessage2* message) override {
        auto* standard = message ? static_cast<cIGZMessage2Standard*>(message) : nullptr;
        cIGZUnknown* object = standard ? standard->GetIGZUnknown() : nullptr;
        void* raw = nullptr;
        return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4Lot*>(raw) : nullptr;
    }
    // The record holds the three managers as object references (SC4WriteGZSerializable of the components themselves).
    bool WriteService(cIGZOStream& stream, IPropDeveloperService* service) override {
        return SC4WriteGZSerializable(stream, ComponentOf(service));
    }
    IPropDeveloperService* ReadService(cIGZIStream& stream, std::uint32_t iid) override {
        void* object = nullptr;
        SC4ReadGZSerializable(stream, iid, &object, false);
        if (!object) return nullptr;
        static_cast<cIGZUnknown*>(object)->Release();   // the city keeps the component; the developer stores the service
        switch (iid) {
            case 0x681905bd: return &g_lots;
            case 0x28326f3e: return &g_props;
            case 0x89a5ba3f: return &g_networkLots;
            default: return nullptr;
        }
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IPropClockApi* Clock() override { return Clock24() ? &g_clock : nullptr; }
    IPropLightingApi* Lighting() override { return LightingManager() ? &g_lighting : nullptr; }
    IPropView3DApi* View3D() override { return CityView3DWin() ? &g_view : nullptr; }
    IPropTerrainApi* Terrain() override { return &g_terrain; }
    IPropOccupantManagerApi* OccupantManager() override { return &g_occupants; }
    IPropCityApi* City() override { return &g_city; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override { AnimationTickServerView()->AddNotification(target, static_cast<std::uint32_t>(channel)); }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override { AnimationTickServerView()->RemoveNotification(target, static_cast<std::uint32_t>(channel)); }
    void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {
        const bool added = AddCitySimulatorAgent(target, interval, name, flags);
        if (Trace()) std::fprintf(stderr, "prop developer: agent %s interval %d -> %s\n", name ? name : "", interval, added ? "added" : "FAILED");
    }
    IWealthHost* WealthRequesterHost() override { return &g_wealth; }
    std::uint32_t TimeReferenceJulianDay() override { return kPropTimeReferenceJulianDay; }
    cRZRandom* RequesterRandom() override { static cRZRandom random(0x89c4u); return &random; }

private:
    static cIGZUnknown* ComponentOf(IPropDeveloperService* service) {
        if (service == &g_lots) return Component(CityComponent::LotManager);
        if (service == &g_props) return Component(CityComponent::PropManager);
        if (service == &g_networkLots) return Component(CityComponent::NetworkLotManager);
        return nullptr;
    }
} g_host;

}  // namespace

bool NativeAddPropRequester(cIGZUnknown* requester) {
    cIGZUnknown* object = Component(CityComponent::PropDeveloper);
    void* developerRaw = nullptr;
    void* requesterRaw = nullptr;
    if (!object || !requester || !object->QueryInterface(kGZIID_cISC4PropDeveloper, &developerRaw) || !developerRaw) return false;
    auto* developer = static_cast<cISC4PropDeveloper*>(developerRaw);
    const bool ok = requester->QueryInterface(kGZIID_cISC4PropRequester, &requesterRaw) && requesterRaw;
    if (ok) {
        auto* view = static_cast<cISC4PropRequester*>(requesterRaw);
        developer->AddPropRequester(view);   // takes its own reference
        view->Release();
    }
    developer->Release();
    if (std::getenv("SC4_PROPDEVDEBUG"))
        std::fprintf(stderr, "prop developer: add requester %08x -> %s\n",
                     ok ? static_cast<cISC4PropRequester*>(requesterRaw)->GetRequesterSatisfactionId() : 0u, ok ? "added" : "not a requester");
    return ok;
}

void InstallPropDeveloperHost() {
    sc4::SetPropDeveloperHost(&g_host);
    sc4::SetCityProbeWealthHost(&g_wealth);
}

// The kit's `propdev`: the developer's lists, the night flag, and per list how many props are in state 1 / shown (all four
// timer bits set) / placed in the renderer (occupant flag 2).
void NativePropDeveloperReport() {
    auto* developer = Serializable<cSC4PropDeveloper>(CityComponent::PropDeveloper, kGZCLSID_cSC4PropDeveloper);
    if (!developer) { std::printf("propdev: no developer\n"); return; }
    auto count = [](const auto& list, const char* name) {
        int on = 0, timersOn = 0, shown = 0;
        for (cISC4PropOccupant* prop : list) {
            if (!prop) continue;
            if (prop->GetState() == 1) ++on;
            if ((prop->GetPropTimerBitfield() & 0xf) == 0xf) ++timersOn;
            if (sc4::cISC4Occupant* occupant = prop->AsOccupant(); occupant && occupant->IsFlagSet(2)) ++shown;
        }
        std::printf("propdev: %s %zu, state 1: %d, timers on: %d, shown: %d\n", name, list.size(), on, timersOn, shown);
    };
    std::printf("propdev: hour %d, night %d, signs visible %d, requesters %zu\n", NativeClockHour(), developer->IsNight() ? 1 : 0,
                developer->AreSignsAndLabelsVisible() ? 1 : 0, developer->Requesters().size());
    count(developer->StateProps(), "state");
    {
        int powered = 0;
        for (cISC4PropOccupant* prop : developer->StateProps()) {
            if (!prop) continue;
            float position[3] = {};
            prop->AsOccupant()->GetPosition(position);
            int x = 0, z = 0;
            g_city.PositionToCell(position[0], position[2], x, z);
            if (g_power.IsPowered(x, z)) ++powered;
        }
        int cells = 0, wired = 0;
        if (cSC4PowerSimulator* power = NativePowerSimulator())
            for (int x = 0; x < g_city.CellCountX(); ++x)
                for (int z = 0; z < g_city.CellCountZ(); ++z) { cells += power->IsPowered(x, z) ? 1 : 0; wired += power->IsWired(x, z) ? 1 : 0; }
        std::printf("propdev: state props on powered cells: %d (power simulator %s; city cells powered %d, wired %d)\n", powered,
                    NativePowerSimulator() ? "present" : "absent", cells, wired);
    }
    count(developer->TimeProps(), "time");
    count(developer->DateProps(), "date");
    count(developer->PowerProps(), "power");
    count(developer->SignProps(), "sign");
    const cSC4PropDeveloper::WalkStats& stats = developer->Stats();
    std::printf("propdev: walk lots found %lld, livable %lld, network lots %lld\n", static_cast<long long>(stats.lotsFound),
                static_cast<long long>(stats.lotsLivable), static_cast<long long>(stats.networkLots));
    std::printf("propdev: walk tracts %lld (with lots %lld, no share %lld), slots %lld, kept %lld, removed %lld, create calls %lld, created %lld\n",
                static_cast<long long>(stats.tracts), static_cast<long long>(stats.tractsWithLots),
                static_cast<long long>(stats.noShare), static_cast<long long>(stats.slots),
                static_cast<long long>(stats.kept), static_cast<long long>(stats.removed),
                static_cast<long long>(stats.createCalls), static_cast<long long>(stats.created));
    std::size_t effects = 0;
    for (const auto& entry : developer->EffectProps()) effects += entry.second.size();
    std::printf("propdev: effect ids %zu, effect props %zu\n", developer->EffectProps().size(), effects);
    std::fflush(stdout);
}

}  // namespace native
