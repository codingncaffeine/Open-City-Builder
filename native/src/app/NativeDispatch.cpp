// The emergency dispatch layer on the native application: the fire and police simulators as the dispatch manager's sources
// (protection.md 8), the dispatch manager's host (sc4::IDispatchHost: the city, the vehicle manager, the pylons, the record
// streams) with its views of the automata and the call boxes, and the services the dispatched vehicles reach
// (IEmergencyVehicleHost, IFireTruckHost, IPatrolCarHost). Spec: OpenSC4 dispatch.md; the route itself is
// cSC4TrafficAutomataManager::Dispatch (i386 0x000ce2f6).
#include <cstdio>
#include "platform/Unwired.h"
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "app/NativeAudio.h"
#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "gz/GZCOM.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AutomataManager.h"
#include "sc4/AircraftDispatchSource.h"
#include "sc4/AircraftManager.h"
#include "sc4/Automaton.h"
#include "sc4/DBSegment.h"
#include "sc4/DispatchManager.h"
#include "sc4/EmergencyVehicle.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/FireTruck.h"
#include "sc4/PatrolCar.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PropOccupantInterface.h"
#include "sc4/PylonOccupant.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SpecialVehicles.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace sc4 {
extern IEmergencyVehicleHost* g_loadEmergencyHost;   // tools/city_probe_hostglobals.cpp (read by city_probe_creators8.cpp)
extern IFireTruckHost* g_loadFireTruckHost;
extern IPatrolCarHost* g_loadPatrolCarHost;
}  // namespace sc4

namespace native {
// app/NativeFireCrew.cpp and app/NativeDisasters.cpp: the crews and the fire count of the fire engines' host.
cIGZUnknown* NativeNewFireFilter();
cIGZUnknown* NativeNewSpillFilter();
bool NativeCountOccupantsInBox(const float* min, const float* max, cIGZUnknown* filter, int& count);
cIGZUnknown* NativeNewFireCrew(sc4::cSC4FireTruck& truck);
void NativeSetFireCrewCompetence(cIGZUnknown* crew, float competence);
void NativeRegisterFireCrew(cIGZUnknown* crew);
void NativeSetupFireCrew(cIGZUnknown* crew);
void NativeRemoveFireCrew(cIGZUnknown* crew);
int NativeFireCount();
void InstallFireCrewHost();
sc4::cSC4CrimeSimulator* NativeCrimeSimulator();   // app/NativeCrime.cpp
void NativePedestriansInBox(const float* lo, const float* hi, std::vector<sc4::cISC4Occupant*>& out);   // app/NativeTrafficAutomata.cpp

// app/NativeRiotPolice.cpp: the figures of the patrol cars' host
bool NativeDisasterOngoing();
cIGZUnknown* NativeNewRiotPolice(sc4::cSC4PatrolCar& car);
void NativeSetupRiotPolice(cIGZUnknown* riot, float competence);
void NativeRemoveRiotPolice(cIGZUnknown* riot);
void InstallRiotPoliceHost();
// app/NativeAutomataControllerHost.cpp: the controller manager's add (by template name, riding on the occupant) and remove
cIGZUnknown* NativeAddController(sc4::cISC4Occupant* occupant, const char* templateName);
void NativeRemoveController(cIGZUnknown* controller);
bool NativePointInViewFrustum(const float* xyz);   // app/NativeAutomataManagerHost.cpp
bool NativeProjectToViewport(const float* xyz, float out[3]);
sc4::IFireOccupant* NativeFireStation(sc4::cISC4Occupant* occupant);       // app/NativeSimHosts.cpp
sc4::IPoliceOccupant* NativePoliceStation(sc4::cISC4Occupant* occupant);
sc4::cISC4Occupant* NativeStationOccupant(sc4::IProtectionStation* station);

sc4::cISC4PathFinder* NativeCreatePathFinder();   // app/NativeTrafficAutomata.cpp
namespace {
using namespace sc4;

bool Debug() {
    static const bool on = std::getenv("SC4_DISPATCHDEBUG") != nullptr;
    return on;
}

template <class T> T* Component(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    return object ? dynamic_cast<T*>(object) : nullptr;
}
cSC4DispatchManager* Manager() { return Component<cSC4DispatchManager>(CityComponent::DispatchManager); }
cSC4FireProtectionSimulator* FireSim() { return Component<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator); }
cSC4PoliceSimulator* PoliceSim() { return Component<cSC4PoliceSimulator>(CityComponent::PoliceSimulator); }
cSC4TrafficAutomataManager* Vehicles() { return Component<cSC4TrafficAutomataManager>(CityComponent::VehicleManager); }

cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

cISC4Occupant* AsOccupant(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
    auto* occupant = static_cast<cISC4Occupant*>(raw);
    occupant->Release();   // the caller's object keeps it
    return occupant;
}

// ---- the dispatch manager's view of an automaton (cISC4Automaton slots 6..39 it calls) ------------------------------------
class DispatchAutomatonView final : public IDispatchAutomaton {
public:
    explicit DispatchAutomatonView(cSC4Automaton& automaton) : a_(automaton) {}
    cSC4Automaton& Automaton() const { return a_; }
    bool QueryInterface(std::uint32_t iid, void** out) override { return a_.QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return a_.AddRef(); }
    std::uint32_t Release() override { return a_.Release(); }
    cISC4Occupant* Occupant() override { return &a_; }
    bool Remove(bool force) override { a_.Remove(force); return true; }
    std::uint16_t GetCallNumber() const override { return a_.GetCallNumber(); }
    void SetCallNumber(std::uint16_t number) override { a_.SetCallNumber(number); }
    void SetDispatchState(tDispatchState state) override { a_.SetDispatchState(state); }
    tDispatchState GetDispatchState() const override { return static_cast<tDispatchState>(a_.GetDispatchState()); }
    void SetDispatchInfoID(std::uint32_t id) override { a_.SetDispatchInfoID(id); }
    void ReturnToSourceOccupant() override { a_.ReturnToSourceOccupant(); }
    void AddCallback(cIGZMessageTarget2* target) override { a_.AddCallback(target); }
private:
    cSC4Automaton& a_;
};
std::map<cSC4Automaton*, std::unique_ptr<DispatchAutomatonView>> g_automata;   // per address, like the managers' views

IDispatchAutomaton* DispatchView(cSC4Automaton* automaton) {
    if (!automaton) return nullptr;
    auto& slot = g_automata[automaton];
    if (!slot || &slot->Automaton() != automaton) slot = std::make_unique<DispatchAutomatonView>(*automaton);
    return slot.get();
}
cSC4Automaton* AutomatonOfDispatchView(const cIGZUnknown* object) {
    auto* view = dynamic_cast<const DispatchAutomatonView*>(object);
    return view ? &view->Automaton() : nullptr;
}

// ---- the dispatch manager's view of a call box (cISC4PylonOccupant) -------------------------------------------------------
class PylonView final : public IPylonOccupant {
public:
    explicit PylonView(cSC4PylonOccupant& pylon) : p_(pylon) {}
    cSC4PylonOccupant& Pylon() const { return p_; }
    bool QueryInterface(std::uint32_t iid, void** out) override { return p_.QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return p_.AddRef(); }
    std::uint32_t Release() override { return p_.Release(); }
    cISC4Occupant* AsOccupant() override { return p_.AsOccupant(); }
    bool Init() override { return p_.Init(); }
    bool Shutdown() override { return p_.Shutdown(); }
    void SetUIState(std::uint32_t state) override { p_.SetUIState(static_cast<std::int32_t>(state)); }
    void SetDispatchType(tDispatchType type) override { p_.SetDispatchType(static_cast<std::int32_t>(type)); }
    tDispatchType GetDispatchType() const override { return static_cast<tDispatchType>(p_.GetDispatchType()); }
    void SetDispatchState(tDispatchState state) override { p_.SetDispatchState(static_cast<std::int32_t>(state)); }
    void SetCallNumber(std::uint16_t number) override { p_.SetCallNumber(number); }
    std::uint16_t GetCallNumber() const override { return p_.GetCallNumber(); }
    void SetSourceOccupant(cISC4Occupant* occupant) override { p_.SetSourceOccupant(occupant); }
    cISC4Occupant* GetSourceOccupant() const override { return p_.GetSourceOccupant(); }
private:
    cSC4PylonOccupant& p_;
};
std::map<cSC4PylonOccupant*, std::unique_ptr<PylonView>> g_pylons;

IPylonOccupant* PylonViewOf(cSC4PylonOccupant* pylon) {
    if (!pylon) return nullptr;
    auto& slot = g_pylons[pylon];
    if (!slot || &slot->Pylon() != pylon) slot = std::make_unique<PylonView>(*pylon);
    return slot.get();
}

// ---- the fire and police simulators as dispatch sources (protection.md 8.2, 15.4) ----------------------------------------
class ProtectionSource final : public IDispatchSource {
public:
    explicit ProtectionSource(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }   // the simulator owns its source
    std::uint32_t Release() override { return 1; }

    bool CanHandleDispatchType(std::uint32_t type) const override { return type == type_; }
    bool CanDispatch(float x, float z, bool* allBusy) override {
        cSC4ProtectionSimulator* p = Protection();
        bool busy = false;
        const bool ok = p && p->CanDispatch(x, z, &busy);
        if (allBusy) *allBusy = busy;
        return ok;
    }
    // Slot 6: one reference per station (the caller hands the list to IDispatchCity::ReleaseStationList).
    bool GetStations(std::list<cISC4Occupant*>& out) override {
        out.clear();
        if (cSC4ProtectionSimulator* p = Protection()) {
            for (IProtectionStation* station : p->Stations()) {
                if (cISC4Occupant* occupant = NativeStationOccupant(station)) {
                    occupant->AddRef();
                    out.push_back(occupant);
                }
            }
        }
        return !out.empty();
    }
    bool GetDispatchesAtStation(cISC4Occupant* station, std::uint32_t* mask, std::uint32_t* base, std::uint32_t* count) override {
        if (!station) return false;
        if (type_ == kDispatchTypeFire) return cSC4FireProtectionSimulator::GetDispatchesAtStation(NativeFireStation(station), mask, base, count);
        return cSC4PoliceSimulator::GetDispatchesAtStation(NativePoliceStation(station), mask, base, count);
    }
    // Slot 8: the nearest station with a free unit (the base picks it), then slot 9 for it with id 0.
    cISC4Occupant* DispatchFromNearestStation(float x, float z, std::uint32_t* id) override {
        cSC4ProtectionSimulator* p = Protection();
        if (!p) return nullptr;
        std::uint32_t unused = 0;
        IProtectionStation* best = p->DispatchFromNearestStation(x, z, &unused);
        if (id) *id = 0;
        cISC4Occupant* occupant = best ? NativeStationOccupant(best) : nullptr;
        if (occupant) DispatchFromStation(occupant, id);
        return occupant;
    }
    // Slot 9 (protection.md 8.2): take unit *id - base, or the lowest free one when *id is 0 (none free: index = count, no bit
    // changes, still true). The bit is cleared without checking it was free (quirk).
    bool DispatchFromStation(cISC4Occupant* station, std::uint32_t* id) override {
        std::uint32_t mask = 0, base = 0, count = 0;
        if (!GetDispatchesAtStation(station, &mask, &base, &count)) return false;
        std::uint32_t index = 0;
        if (id && *id > 0) {
            index = *id - base;
            if (index > count) return false;
            mask &= ~(1u << (index & 31));
        } else {
            for (index = 0; index < count && ((mask >> index) & 1u) == 0; ++index) {}
            if (index < count) mask &= ~(1u << index);
        }
        WriteMask(station, mask);
        if (id) *id = base + index;
        return true;
    }
    // Slot 10: false when every unit is already home or the unit's bit is set.
    void RevokeDispatchFromStation(cISC4Occupant* station, std::uint32_t id) override {
        std::uint32_t mask = 0, base = 0, count = 0;
        if (!GetDispatchesAtStation(station, &mask, &base, &count)) return;
        if (static_cast<std::uint32_t>(__builtin_popcount(mask)) >= count) return;
        const std::uint32_t bit = 1u << ((id - base) & 31);
        if (mask & bit) return;
        WriteMask(station, mask | bit);
    }
    // Slot 11: fire ground 0x4202; police 0x4207 during a disaster, else 0x4228; key (0,0,0), flags 0x11882.
    // PARTIAL: the disaster layer's query exists natively (NativeDisasterOngoing) but is not asked here yet.
    bool GetDispatchAutomataProperties(cISC4Occupant*, std::uint32_t& group, cGZPersistResourceKey& key, std::uint32_t& flags,
                                       const float*) override {
        group = type_ == kDispatchTypeFire ? 0x4202u : 0x4228u;
        if (type_ != kDispatchTypeFire && NativeDisasterOngoing())
            SC4_UNWIRED("cSC4PoliceSimulator::GetDispatchAutomataProperties: during a disaster the original dispatches police group 0x4207, the port 0x4228");
        key = cGZPersistResourceKey{};
        flags = 0x11882;
        return true;
    }
private:
    cSC4ProtectionSimulator* Protection() const {
        if (type_ == kDispatchTypeFire) {
            cSC4FireProtectionSimulator* fire = FireSim();
            return fire ? &fire->Protection() : nullptr;
        }
        cSC4PoliceSimulator* police = PoliceSim();
        return police ? &police->Protection() : nullptr;
    }
    void WriteMask(cISC4Occupant* station, std::uint32_t mask) {
        const std::uint32_t property = type_ == kDispatchTypeFire ? kFireStationAvailability : kPoliceStationAvailability;
        cSCPropertyHelp::SetPropertyValue(station->AsPropertyHolder(), property, mask);
    }
    std::uint32_t type_;
};
ProtectionSource g_fireSource(kDispatchTypeFire), g_policeSource(kDispatchTypePolice);

// ---- the aircraft dispatch sources' services (SL::spCity, spMS2, spDispatchManager, the occupant manager's list release) -----
class AircraftSourceHost final : public IAircraftDispatchSourceHost {
public:
    cIGZUnknown* City() override { cSC4City* city = HostCity(); return city ? static_cast<cISC4City*>(city) : nullptr; }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cISC4DispatchManager* DispatchManager() override { return Manager(); }
    void ReleaseOccupantList(std::list<cISC4Occupant*>& occupants) override {
        for (cISC4Occupant* occupant : occupants) if (occupant) occupant->Release();
        occupants.clear();
    }
} g_aircraftSourceHost;

// The fire plane source (type 5) the fire protection simulator holds, the helicopter source (type 6) the police simulator holds.
cSC4AircraftDispatchSource* AircraftSource(std::uint32_t type) {
    if (type == kDispatchTypeFirePlane) {
        cSC4FireProtectionSimulator* fire = FireSim();
        return fire ? fire->PlaneDispatchSource() : nullptr;
    }
    if (type == kDispatchTypePoliceHelicopter) {
        cSC4PoliceSimulator* police = PoliceSim();
        return police ? police->HelicopterDispatchSource() : nullptr;
    }
    return nullptr;
}

// ---- the vehicle manager as the dispatch manager's automata manager ------------------------------------------------------
class VehicleDispatch final : public IAutomataDispatchManager {
public:
    IDispatchAutomaton* Dispatch(std::uint32_t group, const tDispatchLocation& from, const tDispatchLocation& to, std::uint32_t flags,
                                 std::uint8_t type, const cGZPersistResourceKey& key, std::uint8_t argument) override {
        cSC4TrafficAutomataManager* manager = Vehicles();
        if (!manager) return nullptr;
        IAutomataAutomaton* automaton = manager->Dispatch(group, Location(from), Location(to), flags, type, key, argument);
        if (Debug()) {
            cSC4Automaton* object = automaton ? AutomatonOf(automaton) : nullptr;
            std::fprintf(stderr, "dispatch: vehicle group 0x%x to (%.0f, %.0f) -> %s class 0x%08x emergency %d\n", group, to.x, to.z,
                         automaton ? "created" : "no route", object ? object->GetGZCLSID() : 0u,
                         dynamic_cast<cSC4EmergencyVehicle*>(object) ? 1 : 0);
        }
        return automaton ? DispatchView(AutomatonOf(automaton)) : nullptr;
    }
    bool Dispatch(IDispatchAutomaton* automaton, const tDispatchLocation& from, const tDispatchLocation& to, std::uint32_t flags,
                  std::uint8_t type) override {
        cSC4TrafficAutomataManager* manager = Vehicles();
        cSC4Automaton* object = AutomatonOfDispatchView(automaton);
        return manager && object && manager->Dispatch(AutomatonView(object), Location(from), Location(to), flags, type);
    }
private:
    // The dispatch manager passes the station (kind 0; null for a re-target) and a world position (kind 1).
    static tAutomataLocation Location(const tDispatchLocation& in) {
        tAutomataLocation out;
        out.kind = in.type;
        out.occupant = in.occupant ? CityOccupantView(in.occupant) : nullptr;
        out.x = in.x;
        out.y = in.y;
        out.z = in.z;
        return out;
    }
} g_vehicleDispatch;

// ---- the aircraft manager as the dispatch manager's automata manager (the police helicopter and fire plane groups) ------
class AircraftDispatch final : public IAutomataDispatchManager {
public:
    IDispatchAutomaton* Dispatch(std::uint32_t group, const tDispatchLocation& from, const tDispatchLocation& to, std::uint32_t flags,
                                 std::uint8_t type, const cGZPersistResourceKey& key, std::uint8_t argument) override {
        cSC4AircraftManager* manager = Aircraft();
        if (!manager) return nullptr;
        IAutomataAutomaton* automaton = manager->Dispatch(group, Location(from), Location(to), flags, type, key, argument);
        if (Debug()) std::fprintf(stderr, "dispatch: aircraft group 0x%x to (%.0f, %.0f) -> %s\n", group, to.x, to.z, automaton ? "sent" : "none at the station");
        return automaton ? DispatchView(AutomatonOf(automaton)) : nullptr;
    }
    bool Dispatch(IDispatchAutomaton* automaton, const tDispatchLocation& from, const tDispatchLocation& to, std::uint32_t flags,
                  std::uint8_t type) override {
        cSC4AircraftManager* manager = Aircraft();
        cSC4Automaton* object = AutomatonOfDispatchView(automaton);
        return manager && object && manager->Dispatch(AutomatonView(object), Location(from), Location(to), flags, type);
    }
    static cSC4AircraftManager* Aircraft() { return Component<cSC4AircraftManager>(CityComponent::AircraftManager); }
private:
    static tAutomataLocation Location(const tDispatchLocation& in) {
        tAutomataLocation out;
        out.kind = in.type;
        out.occupant = in.occupant ? CityOccupantView(in.occupant) : nullptr;
        out.x = in.x;
        out.y = in.y;
        out.z = in.z;
        return out;
    }
} g_aircraftDispatch;

// ---- the city the dispatch manager holds (cISC4City slots it calls) ------------------------------------------------------
class DispatchCity final : public IDispatchCity {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    void CellToWorld(std::int32_t cellX, std::int32_t cellZ, float& x, float& z) override { x = float(cellX) * 16.0f; z = float(cellZ) * 16.0f; }
    float CellHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    void ReleaseStationList(std::list<cISC4Occupant*>& stations) override {
        for (cISC4Occupant* station : stations) if (station) station->Release();
        stations.clear();
    }
} g_city;

class DispatchHostImpl final : public IDispatchHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IDispatchCity* City() override { return HostCity() ? &g_city : nullptr; }
    // PARTIAL: cSC4DispatchVehicleView (the on-screen vehicle labels) is not recovered natively.
    IDispatchVehicleView* CreateDispatchVehicleView() override {
        SC4_UNWIRED("cSC4DispatchManager::CreateDispatchVehicleView: cSC4DispatchVehicleView (the on-screen vehicle labels) is not recovered");
        return nullptr;
    }
    IPylonOccupant* CreatePylonOccupant() override {
        auto* pylon = new cSC4PylonOccupant();
        pylon->AddRef();
        return PylonViewOf(pylon);
    }
    // cSC4AutomataManager::GetAutomataManagerForGroup: the first manager whose CanCreateGroup takes the group, vehicles before
    // aircraft (the ground groups belong to the vehicle manager, the police helicopter and fire plane groups to the aircraft one).
    IAutomataDispatchManager* GetAutomataManagerForGroup(std::uint32_t group) override {
        cSC4TrafficAutomataManager* manager = Vehicles();
        if (manager && manager->CanCreateGroup(group)) return &g_vehicleDispatch;
        cSC4AircraftManager* aircraft = AircraftDispatch::Aircraft();
        return aircraft && aircraft->CanCreateGroup(group) ? &g_aircraftDispatch : nullptr;
    }
    void AddDispatchViewLayer(IDispatchVehicleView*) override {}
    void RemoveDispatchViewLayer(IDispatchVehicleView*) override {}
    // The record streams hold the objects themselves: the views turn back into them for writing and are made for them on reading.
    bool WriteSerializable(cIGZOStream& stream, const cIGZUnknown* object) override {
        if (cSC4Automaton* automaton = AutomatonOfDispatchView(object)) return SC4WriteGZSerializable(stream, static_cast<cISC4Occupant*>(automaton));
        if (auto* pylon = dynamic_cast<const PylonView*>(object)) return SC4WriteGZSerializable(stream, pylon->Pylon().AsOccupant());
        return SC4WriteGZSerializable(stream, object);
    }
    bool ReadSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override {
        *out = nullptr;
        if (iid == kDispatchAutomatonIid) {
            void* raw = nullptr;
            const bool ok = SC4ReadGZSerializable(stream, iid, &raw, false);
            if (raw) *out = DispatchView(static_cast<cSC4Automaton*>(raw));   // cSC4Automaton answers its primary iid with `this`
            return ok;
        }
        if (iid == kDispatchPylonIid) {
            void* raw = nullptr;
            const bool ok = SC4ReadGZSerializable(stream, iid, &raw, false);
            if (raw) *out = PylonViewOf(dynamic_cast<cSC4PylonOccupant*>(static_cast<cISC4PylonOccupant*>(raw)));
            return ok;
        }
        return SC4ReadGZSerializable(stream, iid, out, false);
    }
    IDispatchAutomaton* AutomatonFromMessage(void* object) override { return DispatchView(static_cast<cSC4Automaton*>(object)); }
} g_host;

// ---- the dispatched vehicles' services -----------------------------------------------------------------------------------
class EmergencyHost final : public IEmergencyVehicleHost {
public:
    // The siren is a sound instance named by the vehicle itself (its address in the original; the low 32 bits here).
    static std::uint32_t SirenId(const cSC4EmergencyVehicle& vehicle) { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(&vehicle)); }
    void SirenStopped(const cSC4EmergencyVehicle& vehicle) override { PostSoundSystemEvent(0x6a451908, SirenId(vehicle)); }
    void SirenMoved(const cSC4EmergencyVehicle& vehicle, int x, int y, int z) override {
        PostSoundSystemEvent(0xca45190f, SirenId(vehicle), static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), static_cast<std::uint32_t>(z));
    }
    // cSC4EmergencyVehicle::ShowEmergencyLights (Mac i386 0x00235862): lights on = an "automata_pullover" attractor riding on the
    // vehicle (the cars around it pull over) and the flashing light model its exemplar names; lights off drops both.
    // PARTIAL: the player-drive audio event (AEMS event 0xb) is not posted.
    void SetEmergencyLights(cSC4EmergencyVehicle& vehicle, bool on) override {
        if (vehicle.GetBehavior() == kStatePlayerDrive)
            SC4_UNWIRED("cSC4EmergencyVehicle::ShowEmergencyLights: the player-drive AEMS event 0xb (siren start / stop) is not posted");
        if (cIGZUnknown* old = vehicle.LightController()) {
            NativeRemoveController(old);
            vehicle.SetLightController(nullptr);
        }
        IAutomatonModel* model = vehicle.LightModel();
        if (on) {
            vehicle.SetLightController(NativeAddController(static_cast<cISC4Occupant*>(&vehicle), "automata_pullover"));
            if (model) {
                vehicle.SetLightModel(nullptr);
                model->Release();
            }
            model = nullptr;
            vehicle.LoadAuxiliaryModel(2, &model);
            vehicle.SetLightModel(model);
            if (std::getenv("SC4_DISPATCHDEBUG"))
                std::fprintf(stderr, "dispatch: lights on: pull-over controller %d, light model %d\n", vehicle.LightController() ? 1 : 0, model ? 1 : 0);
        } else if (model) {
            vehicle.RemoveModelInstance(model);
            vehicle.SetLightModel(nullptr);
            model->Release();
        }
    }
    void SirenStarted(const cSC4EmergencyVehicle& vehicle, std::uint32_t soundId) override { PostSoundSystemEvent(0xea451903, soundId, SirenId(vehicle)); }
    // sTuningParams +0x170..+0x17c (AutomataTuning(): 0x89aec88b..0xc9ee640d).
    const EmergencyVehicleTuning& Tuning() override {
        const AutomataTuningGlobals& t = AutomataTuning();
        tuning_.retrySeconds = t.f219f0;
        tuning_.teleportCells = t.f219f4;
        tuning_.maxRepaths = t.f219f5;
        tuning_.speedKmh = t.f219f8;
        tuning_.surroundingDepth = static_cast<std::int32_t>(t.f219fc);
        return tuning_;
    }
    void PostPathBlocked(cSC4EmergencyVehicle& vehicle, std::uint32_t type, std::uint8_t repaths) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(0xca8dcdb4);
        message->SetData1(type);
        message->SetData2(repaths);
        message->SetIGZUnknown(static_cast<cISC4Occupant*>(&vehicle));
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    ITrafficAutomatonOccupant* NetworkOccupantAt(float x, float z) override {
        auto* map = Component<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
        TrafficTileInfo* tile = map ? map->SafeGetTileInfoAtCell(int(x / 16.0f), int(z / 16.0f)) : nullptr;
        for (; tile; tile = tile->next)
            if (auto* piece = dynamic_cast<ITrafficAutomatonOccupant*>(tile->occupant)) return piece;
        return nullptr;
    }
    // The vehicle manager's re-route (Dispatch(automaton): from its own cell, to the call, minimum route 3).
    bool FindRoute(cSC4EmergencyVehicle& vehicle, const cS3DVector3& destination) override {
        cSC4TrafficAutomataManager* manager = Vehicles();
        if (!manager) return false;
        tAutomataLocation from, to;
        to.kind = kLocationWorldPosition;
        to.x = destination.x;
        to.y = destination.y;
        to.z = destination.z;
        return manager->Dispatch(AutomatonView(&vehicle), from, to, 0, 3);
    }
    // The way home: from the vehicle's position to its station, flags 2, minimum route 2.
    bool FindReturnRoute(cSC4EmergencyVehicle& vehicle, const cS3DVector3& position, cIGZUnknown* source) override {
        cSC4TrafficAutomataManager* manager = Vehicles();
        cISC4Occupant* station = AsOccupant(source);
        if (!manager || !station) return false;
        tAutomataLocation from, to;
        from.kind = kLocationWorldPosition;
        from.x = position.x;
        from.y = position.y;
        from.z = position.z;
        to.kind = kLocationOccupant;
        to.occupant = CityOccupantView(station);
        return manager->Dispatch(AutomatonView(&vehicle), from, to, 2, 2);
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { cellX = int(x / 16.0f); cellZ = int(z / 16.0f); }
    cISC4PathFinder* CreatePathFinder() override { return NativeCreatePathFinder(); }
private:
    EmergencyVehicleTuning tuning_;
} g_emergency;

bool CityCellCount(int& width, int& height) {
    cSC4City* city = HostCity();
    if (!city) return false;
    width = city->CellCountX();
    height = city->CellCountZ();
    return true;
}

// The fire engines' host: the fire counts come from the fire disaster, the crew is a cSC4SimFireCrew (app/NativeFireCrew.cpp).
// PARTIAL: the toxic spill counts (the spill manager has no native host yet) and the player-driven hose aim.
class FireTruckHost final : public IFireTruckHost {
public:
    cIGZUnknown* NewFireFilter() override { return NativeNewFireFilter(); }
    cIGZUnknown* NewSpillFilter() override { return NativeNewSpillFilter(); }
    bool FireCompetence(cIGZUnknown* source, float& competence) override {
        cSC4FireProtectionSimulator* fire = FireSim();
        cISC4Occupant* station = AsOccupant(source);
        if (!fire || !station) return false;
        competence = fire->GetStationEfficiency(NativeFireStation(station));
        return true;
    }
    bool StationRadius(cIGZUnknown* source, float& radius) override {
        cSC4FireProtectionSimulator* fire = FireSim();
        cISC4Occupant* station = AsOccupant(source);
        static const bool trace = std::getenv("SC4_FIREDEBUG") != nullptr;
        if (trace) std::fprintf(stderr, "crew: engine source %p station %p radius %.0f\n", static_cast<void*>(source), static_cast<void*>(station),
                                fire && station ? fire->GetStationDispatchRadius(NativeFireStation(station)) : -1.0f);
        if (!fire || !station) return false;
        radius = fire->GetStationDispatchRadius(NativeFireStation(station));
        return true;
    }
    int FireCount() override { return NativeFireCount(); }
    bool OccupantsInBox(const float* min, const float* max, cIGZUnknown* filter, int& count) override {
        return NativeCountOccupantsInBox(min, max, filter, count);
    }
    int SpillTotal() override {
        SC4_UNWIRED("cSC4FireTruck: the toxic spill counts (spill manager) are not wired (always 0)");
        return 0;
    }
    int SpillsInRect(const int*) override { return 0; }
    cIGZUnknown* NewFireCrew(cSC4FireTruck& truck) override { return NativeNewFireCrew(truck); }
    void SetFireCrewCompetence(cIGZUnknown* crew, float competence) override { NativeSetFireCrewCompetence(crew, competence); }
    void RegisterFireCrew(cIGZUnknown* crew) override { NativeRegisterFireCrew(crew); }
    void SetupFireCrew(cIGZUnknown* crew) override { NativeSetupFireCrew(crew); }
    void RemoveFireCrew(cIGZUnknown* crew) override { NativeRemoveFireCrew(crew); }
    bool PickGroundPoint(float, float, cS3DVector3&) override {
        SC4_UNWIRED("cSC4FireTruck player drive: the hose aim's ground pick is not wired");
        return false;
    }
    bool CityCells(int& width, int& height) override { return CityCellCount(width, height); }
    float MaxStreamDistance() override { return AutomataTuning().f21a18; }
} g_fireTruck;

// A crime prop the car arrests: cISC4PropOccupant::SetState (i386 vslot 0x2c) on the crime simulator's answer.
class ArrestedProp final : public ICrimeParticipant {
public:
    void Set(cISC4PropOccupant* prop) { prop_ = prop; }
    void Arrest(bool on) override { if (prop_) prop_->SetState(on ? 1u : 0u); }
private:
    cISC4PropOccupant* prop_ = nullptr;
};

bool ArrestTrace() { static const bool on = std::getenv("SC4_CRIMEDEBUG") != nullptr; return on; }

class PatrolCarHost final : public IPatrolCarHost {
public:
    bool RiotInProgress() override { return NativeDisasterOngoing(); }
    bool PoliceCompetence(cIGZUnknown* source, float& competence) override {
        cSC4PoliceSimulator* police = PoliceSim();
        cISC4Occupant* station = AsOccupant(source);
        if (!police || !station) return false;
        competence = police->GetStationEfficiency(NativePoliceStation(station));
        return true;
    }
    bool StationRadius(cIGZUnknown* source, float& radius) override {
        cSC4PoliceSimulator* police = PoliceSim();
        cISC4Occupant* station = AsOccupant(source);
        return police && station && police->GetStationDispatchRadius(NativePoliceStation(station), radius);
    }
    // spCrimeSim vslot 0x24 (i386): the nearest crime figure; its cISC4Automaton answer is the automaton's primary pointer.
    cSC4Automaton* FindCrimeAutomaton(const cS3DVector3& position, float radius) override {
        cSC4CrimeSimulator* crime = NativeCrimeSimulator();
        const float at[3] = {position.x, position.y, position.z};
        void* found = crime ? crime->GetNearestCrimeSim(at, radius) : nullptr;
        if (ArrestTrace()) {
            std::fprintf(stderr, "arrest: crime figure near %.0f,%.0f -> %s\n", at[0], at[2], found ? "found" : "none");
            const float lo[3] = {at[0] - 60.0f, -3.4e38f, at[2] - 60.0f}, hi[3] = {at[0] + 60.0f, 3.4e38f, at[2] + 60.0f};
            std::vector<cISC4Occupant*> near;
            NativePedestriansInBox(lo, hi, near);
            for (cISC4Occupant* o : near) {
                float p[3] = {};
                o->GetPosition(p);
                std::fprintf(stderr, "arrest:   pedestrian at %.0f,%.0f,%.0f type %08x group 4102 %d\n", p[0], p[1], p[2], o->GetType(), o->IsOccupantGroup(0x4102));
                o->Release();
            }
        }
        return static_cast<cSC4Automaton*>(found);
    }
    // spCrimeSim vslot 0x28: the nearest idle crime prop.
    ICrimeParticipant* FindCrimeParticipant(const cS3DVector3& position, float radius) override {
        cSC4CrimeSimulator* crime = NativeCrimeSimulator();
        const float at[3] = {position.x, position.y, position.z};
        auto* prop = static_cast<cISC4PropOccupant*>(crime ? crime->GetNearestCrimeProp(at, radius) : nullptr);
        if (ArrestTrace()) std::fprintf(stderr, "arrest: crime prop near %.0f,%.0f -> %s\n", at[0], at[2], prop ? "found" : "none");
        if (!prop) return nullptr;
        arrested_.Set(prop);
        return &arrested_;
    }
    cIGZUnknown* NewRiotPolice(cSC4PatrolCar& car) override { return NativeNewRiotPolice(car); }
    void SetupRiotPolice(cIGZUnknown* riot, float competence) override { NativeSetupRiotPolice(riot, competence); }
    void RemoveRiotPolice(cIGZUnknown* riot) override { NativeRemoveRiotPolice(riot); }
    // cSC4Automaton::ReplaceModelInstance(first model, key), then the new instance's SetPlayMode(1) (i386 0x8c).
    void ReplaceArrestModel(cSC4PatrolCar& car, const cGZPersistResourceKey& key) override {
        car.PlayAnim(key);
        if (!car.Models().empty() && car.Models().front()) car.Models().front()->SetPlayMode(static_cast<s3d::PlayMode>(1));
        if (ArrestTrace()) std::fprintf(stderr, "arrest: patrol car plays %08x\n", key.instance);
    }
private:
    ArrestedProp arrested_;
} g_patrolCar;

}  // namespace

// The tool's and the call boxes' services (app/NativeDispatchTool.cpp) reach the manager, the views and the vehicles here.
cISC4DispatchManager* NativeDispatchManager() { return Manager(); }
IPylonOccupant* NativePylonView(cSC4PylonOccupant* pylon) { return PylonViewOf(pylon); }
float NativeDispatchAltitude(float x, float z) { return g_city.CellHeight(x, z); }
void NativeFollowDispatchedAutomaton(IDispatchAutomaton* automaton) {
    cSC4TrafficAutomataManager* manager = Vehicles();
    if (!manager) return;
    cSC4Automaton* object = automaton ? AutomatonOfDispatchView(automaton) : nullptr;
    manager->SetCameraFollow(object ? AutomatonView(object) : nullptr);
}

// ---- the protection simulators' view of the dispatch manager (IFireHost / IPoliceHost) -----------------------------------
int NativeDispatchCount(std::uint32_t type) {
    cSC4DispatchManager* manager = Manager();
    return manager ? manager->GetDispatchCount(static_cast<tDispatchType>(type)) : 0;
}

std::vector<std::uint32_t> NativeDispatchIds(std::uint32_t type) {
    std::vector<std::uint32_t> ids;
    cSC4DispatchManager* manager = Manager();
    const int count = manager ? manager->GetDispatchCount(static_cast<tDispatchType>(type)) : 0;
    if (count <= 0) return ids;
    ids.resize(std::size_t(count));
    std::uint32_t limit = std::uint32_t(count);
    manager->GetDispatchIDArray(static_cast<tDispatchType>(type), ids.data(), limit);
    return ids;
}

bool NativeDispatchRecord(std::uint32_t id, ProtectionDispatchInfo& out) {
    cSC4DispatchManager* manager = Manager();
    DispatchInfo info;
    if (!manager || !manager->GetDispatchInfo(id, info)) return false;
    out.valid = true;
    out.active = info.active;
    out.targetX = info.x;
    out.targetZ = info.z;
    out.origin = nullptr;
    if (info.station) {
        if (info.type == kDispatchTypeFire) out.origin = NativeFireStation(info.station);
        else if (info.type == kDispatchTypePolice) out.origin = NativePoliceStation(info.station);
    }
    return true;
}

void NativeRegisterDispatchSource(std::uint32_t type, bool add) {
    cSC4DispatchManager* manager = Manager();
    if (!manager) return;
    IDispatchSource* source = type == kDispatchTypeFire ? static_cast<IDispatchSource*>(&g_fireSource) : &g_policeSource;
    if (add) {
        if (manager->GetDispatchSource(type) != source) manager->RegisterDispatchSource(source);
    } else {
        manager->UnregisterDispatchSource(source);
    }
    if (Debug()) std::fprintf(stderr, "dispatch: %s source %s\n", type == kDispatchTypeFire ? "fire" : "police", add ? "registered" : "removed");
}

// The kit's `dispatch TYPE X Z` (game_main_linux.cpp): what the dispatch tool's drop does without a call box.
std::uint32_t NativeDispatchAt(std::uint32_t type, float x, float z) {
    cSC4DispatchManager* manager = Manager();
    if (!manager) return 0;
    IDispatchSource* registered = manager->GetDispatchSource(type);
    if (Debug() && registered) {
        std::list<cISC4Occupant*> stations;
        registered->GetStations(stations);
        for (cISC4Occupant* station : stations) {
            float p[3] = {0, 0, 0};
            station->GetPosition(p);
            std::uint32_t mask = 0, base = 0, count = 0;
            const bool ok = registered->GetDispatchesAtStation(station, &mask, &base, &count);
            std::fprintf(stderr, "dispatch: station at (%.0f, %.0f): units %d mask 0x%x base %u count %u\n", p[0], p[2], ok ? 1 : 0, mask, base, count);
        }
        g_city.ReleaseStationList(stations);
    }
    if (Debug() && (type == kDispatchTypeFire || type == kDispatchTypePolice)) {
        cSC4ProtectionSimulator* p = nullptr;
        if (type == kDispatchTypeFire) { if (auto* f = FireSim()) p = &f->Protection(); }
        else if (auto* police = PoliceSim()) p = &police->Protection();
        std::fprintf(stderr, "dispatch: source registered %d, simulator %d, stations %zu, in the file %zu\n", registered ? 1 : 0,
                     p ? 1 : 0, p ? p->Stations().size() : 0, p ? p->FileStationCount() : 0);
    } else if (Debug()) {
        cSC4AircraftDispatchSource* aircraft = AircraftSource(type);
        std::fprintf(stderr, "dispatch: source registered %d (the simulator's own %d), stations %zu\n", registered ? 1 : 0,
                     aircraft && registered == static_cast<IDispatchSource*>(aircraft) ? 1 : 0, aircraft ? aircraft->Stations().size() : 0);
    }
    const bool can = manager->CanDispatch(static_cast<tDispatchType>(type), x, z);
    if (Debug() && registered) {
        cSTETerrain* t = Terrain();
        bool busy = false;
        const bool sourceCan = registered->CanDispatch(x, z, &busy);
        std::fprintf(stderr, "dispatch: terrain %d altitude %.1f sea %.1f; source can %d busy %d\n", t ? 1 : 0, t ? t->GetAltitude(x, z) : 0.0f,
                     t ? t->GetSeaLevel() : 0.0f, sourceCan ? 1 : 0, busy ? 1 : 0);
    }
    const std::uint32_t id = can ? manager->Dispatch(static_cast<tDispatchType>(type), x, z) : 0;
    if (Debug()) std::fprintf(stderr, "dispatch: type %u at (%.0f, %.0f): can %d -> id %u (records %d)\n", type, x, z, can ? 1 : 0, id,
                              manager->GetDispatchCount(static_cast<tDispatchType>(type)));
    return id;
}

// The kit's `dispatches`: every record with its vehicle's position and dispatch state.
void NativeDispatchReport() {
    cSC4DispatchManager* manager = Manager();
    if (!manager) return;
    for (std::uint32_t type : {0u, 1u, 5u, 6u}) {
        for (std::uint32_t id : NativeDispatchIds(type)) {
            DispatchInfo info;
            if (!manager->GetDispatchInfo(id, info)) continue;
            float p[3] = {0, 0, 0};
            std::uint32_t state = 0;
            float opacity = -1.0f;
            int models = 0, inView = 0;
            if (cSC4Automaton* a = info.automaton ? AutomatonOfDispatchView(info.automaton) : nullptr) {
                a->GetPosition(p);
                state = a->GetDispatchState();
                opacity = a->GetOpacity();
                models = a->GetModelInstance() ? 1 : 0;
                inView = NativePointInViewFrustum(p) ? 1 : 0;
                cS3DTransform t;
                float screen[3] = {0, 0, 0};
                NativeProjectToViewport(p, screen);
                if (a->GetTransform(t))
                    std::printf("dispatches: vehicle flags 0x%x, model %p at (%.0f, %.1f, %.0f), on screen (%.0f, %.0f)\n", static_cast<unsigned>(a->GetFlags()),
                                static_cast<void*>(a->GetModelInstance()), t.translation[0], t.translation[1], t.translation[2], screen[0], screen[1]);
            }
            std::printf("dispatches: type %u id %u call %u active %d target (%.0f, %.0f) vehicle %d at (%.0f, %.1f, %.0f) state %u opacity %.2f model %d in view %d\n",
                        type, id, info.callNumber, info.active ? 1 : 0, info.x, info.z, info.automaton ? 1 : 0, p[0], p[1], p[2], state, opacity, models, inView);
        }
    }
    std::printf("dispatches: %d fire, %d police, %d fire plane, %d police helicopter\n", NativeDispatchCount(1), NativeDispatchCount(0),
                NativeDispatchCount(5), NativeDispatchCount(6));
    // The aircraft sources: the one the dispatch manager answers for the type against the simulator's own, and their stations.
    for (std::uint32_t type : {5u, 6u}) {
        cSC4AircraftDispatchSource* source = AircraftSource(type);
        IDispatchSource* registered = manager->GetDispatchSource(type);
        std::printf("dispatches: source type %u class %08x initialized %d registered %d (the simulator's %d) stations %zu next call %u can %d\n", type,
                    source ? source->GetGZCLSID() : 0u, source && source->IsInitialized() ? 1 : 0, registered ? 1 : 0,
                    source && registered == static_cast<IDispatchSource*>(source) ? 1 : 0, source ? source->Stations().size() : 0,
                    source ? source->NextCallNumber() : 0u, manager->CanDispatch(static_cast<tDispatchType>(type), 0.0f, 0.0f) ? 1 : 0);
        if (!source) continue;
        for (cISC4Occupant* station : source->Stations()) {
            float p[3] = {0, 0, 0};
            station->GetPosition(p);
            std::uint32_t mask = 0, base = 0, count = 0;
            const bool ok = source->GetDispatchesAtStation(station, &mask, &base, &count);
            std::printf("dispatches:   station at (%.0f, %.0f) units %d mask 0x%x base %u count %u\n", p[0], p[2], ok ? 1 : 0, mask, base, count);
        }
    }
    // What the GZCOM creators of the two classes make (the serializer creates a saved source through them).
    for (std::uint32_t classId : {kGZCLSID_cSC4FirePlaneDispatchSource, kGZCLSID_cSC4PoliceHelicopterDispatchSource}) {
        const gzcom::ClassFactoryInfo* info = gzcom::FindClassFactory(classId);
        auto* made = static_cast<cIGZUnknown*>(info && info->create ? info->create() : nullptr);
        cIGZSerializable* serializable = nullptr;
        if (made) {
            made->AddRef();
            void* view = nullptr;
            if (made->QueryInterface(kGZIID_cIGZSerializable, &view)) serializable = static_cast<cIGZSerializable*>(view);
        }
        std::printf("dispatches: creator %s makes %s class %08x\n", info ? info->className : "(none)", made ? "an object of" : "nothing,",
                    serializable ? serializable->GetGZCLSID() : 0u);
        if (serializable) serializable->Release();
        if (made) made->Release();
    }
    // The city's buildings that declare aircraft units (what makes a building a station of either source).
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (object && object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) && raw) {
        auto* occupants = static_cast<cISC4OccupantManager*>(raw);
        const float lo[3] = {0.0f, -1.0e9f, 0.0f}, hi[3] = {city->SizeX(), 1.0e9f, city->SizeZ()};
        OccupantList all;
        int declared[2] = {0, 0};
        if (occupants->GetOccupantsByBBox(all, lo, hi, static_cast<cISC4OccupantFilter*>(nullptr), 0xffffffffu)) {
            for (cISC4Occupant* occupant : all) {
                sc4::cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
                if (!holder) continue;
                const std::uint32_t ids[2] = {kFirePlaneDispatchTraits.countProperty, kPoliceHelicopterDispatchTraits.countProperty};
                for (int i = 0; i < 2; ++i) {
                    std::int32_t units = 0;
                    if (!cSCPropertyHelp::GetPropertyValue(holder, ids[i], units)) continue;
                    ++declared[i];
                    float p[3] = {0, 0, 0};
                    occupant->GetPosition(p);
                    std::printf("dispatches:   building type %08x property %08x = %d at (%.0f, %.0f)\n", occupant->GetType(), ids[i], units, p[0], p[2]);
                }
            }
        }
        const std::size_t total = all.size();
        occupants->ReleaseOccupantList(all);
        occupants->Release();
        std::printf("dispatches: buildings with fire plane units %d, police helicopter units %d (of %zu occupants)\n", declared[0], declared[1], total);
    }
}

bool NativeIsSituationTarget(cIGZUnknown* occupant);                                             // app/NativeAdvisors.cpp
bool NativePoliceAndCrimeAt(int cellX, int cellZ, std::uint8_t& police, std::uint8_t& crime);   // app/NativeCrime.cpp

namespace {
// What a getaway van asks when it appears: is it the mission's target, and do the police outweigh the crime where it stands.
class GetawayVanHost final : public sc4::IGetawayVanHost {
public:
    bool IsSituationTarget(sc4::cISC4Occupant* occupant) override { return NativeIsSituationTarget(occupant); }
    bool PoliceAndCrimeAt(int cellX, int cellZ, std::uint8_t& police, std::uint8_t& crime) override {
        return NativePoliceAndCrimeAt(cellX, cellZ, police, crime);
    }
} g_getawayVanHost;
}  // namespace

void InstallNativeDispatchHost() {
    SetAircraftDispatchSourceHost(&g_aircraftSourceHost);
    sc4::SetGetawayVanHost(&g_getawayVanHost);
    SetDispatchHost(&g_host);
    g_loadEmergencyHost = &g_emergency;
    g_loadFireTruckHost = &g_fireTruck;
    InstallFireCrewHost();
    g_loadPatrolCarHost = &g_patrolCar;
    InstallRiotPoliceHost();
}

}  // namespace native
