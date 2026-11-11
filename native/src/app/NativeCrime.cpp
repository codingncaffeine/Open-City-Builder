// The crime simulator in a native city. It ran on the city probe's neutral host and was never initialised: no monthly census, no
// crimes, no crime map. Here is its real host (ICrimeHost: lots, buildings, the police and residential simulators, the job
// demands, the crime type exemplars, arson) and its place in the city's lifecycle: the three simulator agents and the four
// notifications of the original's Init (Mac i386 0x00173b20), delivered to its DoMessage.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeLotSerial.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/DemandSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/OccupantManager.h"
#include "sc4/Ordinance.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PropManager.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "tools/city_probe_plain.h"

namespace sc4 { void SetCityProbeCrimeHost(ICrimeHost* host); }   // tools/city_probe_bindings.cpp: the host the creator hands out

namespace native {
void NativeStartArson(sc4::cISC4Occupant* building);   // app/NativeDisasters.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_CRIMEDEBUG") != nullptr;
    return on;
}

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
cSC4CrimeSimulator* Crime() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::CrimeSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<cSC4CrimeSimulator*>(raw);
}

class CrimeBuilding final : public ICrimeBuilding {
public:
    explicit CrimeBuilding(cISC4BuildingOccupant* building) : building_(building) {}
    // the profile's first two words: [0] purpose, [1] wealth (the filters' "subclass" and "class")
    const std::int32_t* ClassAndSubclass() override {
        const BuildingProfile& profile = building_->GetBuildingProfile();
        values_[0] = static_cast<std::int32_t>(profile.purpose);
        values_[1] = static_cast<std::int32_t>(profile.wealth);
        return values_;
    }
    std::uint32_t OccupantId() override { return 0; }
    cISC4BuildingOccupant* Building() const { return building_; }
private:
    cISC4BuildingOccupant* building_;
    std::int32_t values_[2] = {};
};

// A lot as the crime simulator holds it: the references are the lot's own.
class CrimeLot final : public ICrimeLot {
public:
    explicit CrimeLot(cSC4Lot* lot) : lot_(lot) {}
    void AddRef() override { lot_->AddRef(); }
    void Release() override { lot_->Release(); }
    void GetBounds(int& x0, int& z0, int& x1, int& z1) override {
        std::int32_t r[4] = {};
        lot_->GetBoundingRect(r);
        x0 = r[0];
        z0 = r[1];
        x1 = r[2];
        z1 = r[3];
    }
    void GetCell(int& x, int& z) override { lot_->GetLocation(x, z); }
    std::uint32_t Wealth() override { return lot_->GetCondition(); }   // lot vslot 0x8c: the lot's condition
    ICrimeBuilding* Building() override {
        cIGZUnknown* object = lot_->GetBuilding();
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return nullptr;
        auto* building = static_cast<cISC4BuildingOccupant*>(raw);
        building->Release();   // the lot keeps it
        building_ = std::make_unique<CrimeBuilding>(building);
        return building_.get();
    }
    void GetPosition(float& x, float& z) override {
        float center[3] = {};
        lot_->GetCenter(center);
        x = center[0];
        z = center[2];
    }
    cSC4Lot* Lot() const { return lot_; }
private:
    cSC4Lot* lot_;
    std::unique_ptr<CrimeBuilding> building_;
};

class CrimeOccupant final : public ICrimeOccupant {
public:
    explicit CrimeOccupant(cISC4Occupant* occupant) : occupant_(occupant) {}
    std::uint32_t OccupantType() override { return occupant_->GetType(); }
    cISCPropertyHolder* PropertyHolder() override { return occupant_->AsPropertyHolder(); }
    void Position(float xyz[3]) override { occupant_->GetPosition(xyz); }
    bool IsAvailableDepartmentBuilding() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(0xe9793a65, &raw) || !raw) return false;
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    }
    bool IsCrimeResponder() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(0xa9b40f05, &raw) || !raw) return false;
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    }
    bool HasFlags(std::uint32_t flags) override { return occupant_->IsFlagSet(flags); }
    cISC4Occupant* Occupant() const { return occupant_; }
private:
    cISC4Occupant* occupant_;
};

template <class T> class CensusGrid final : public ICrimeCensusGrid {
public:
    void Set(cSC4SimGrid<T>* grid) { grid_ = grid; }
    int CountX() override { return grid_ ? grid_->GetTractCountX() : 0; }
    int CountZ() override { return grid_ ? grid_->GetTractCountZ() : 0; }
    std::uint16_t Population(int x, int z) override { return grid_ ? static_cast<std::uint16_t>(grid_->GetGridData()[x][z]) : 0; }
    float Value(int x, int z) override { return grid_ ? static_cast<float>(grid_->GetGridData()[x][z]) : 0.0f; }
private:
    cSC4SimGrid<T>* grid_ = nullptr;
};

class TypeAndGroupFilter final : public cIGZPersistResourceKeyFilter {
public:
    TypeAndGroupFilter(std::uint32_t type, std::uint32_t group) : type_(type), group_(group) {}
    bool QueryInterface(std::uint32_t, void** out) override { *out = nullptr; return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_ && key.group == group_; }
private:
    std::uint32_t type_, group_;
};

class CrimeHost final : public ICrimeHost {
public:
    ICrimeLot* RandomLot() override {
        auto* lots = CityAs<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);
        return LotView(lots ? lots->GetRandomLot() : nullptr);
    }
    ICrimeLot* LotAt(int x, int z) override {
        auto* lots = CityAs<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper);
        return LotView(lots ? lots->GetLot(x, z, false) : nullptr);
    }
    int ZoneTypeAt(int x, int z) override {
        auto* zones = CityAs<cSC4ZoneManager>(CityComponent::ZoneManager);
        return zones ? zones->GetZoneType(x, z) : 0;
    }
    int PropCrimeTypeId(std::uint32_t propId) override {
        auto* props = CityAs<cSC4PropManager>(CityComponent::PropManager);
        const PropProfile* profile = props ? props->GetPropProfilePtr(propId) : nullptr;
        return profile ? static_cast<int>(profile->valueC) : 0;
    }
    int PoliceJailPopulation() override {
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        return police ? police->GetJailPopulation() : 0;
    }
    int PoliceResetUnjailedCriminals() override {
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        return police ? police->ResetUnjailedCriminals() : 0;
    }
    void PoliceProcessMonthlyCrimeStats() override {
        if (auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator)) police->ProcessMonthlyCrimeStats();
    }
    const cSC4SimGrid<std::uint8_t>* PoliceCoverageGrid() override {
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        return police ? police->Protection().AdjustedGrid() : nullptr;
    }
    // demand simulator vslot 0x18 GetDemand(id, 0): the active demand's value, maximum and minimum
    void DemandValues(int demandId, float& a, float& b, float& c) override {
        a = b = c = 0.0f;
        auto* demands = CityAs<cISC4DemandSimulator>(CityComponent::DemandSimulator);
        cSC4Demand* demand = demands ? demands->GetDemand(static_cast<std::uint32_t>(demandId), 0) : nullptr;
        if (!demand) return;
        a = demand->QueryActiveDemandValue();
        b = demand->QueryActiveDemandMax();
        c = demand->QueryActiveDemandMin();
    }
    float OrdinanceFactor(std::uint32_t ordinanceId) override {
        auto* ordinances = CityAs<cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator);
        return ordinances ? ordinances->GetCumulativeFactor(ordinanceId, 0) : 1.0f;
    }
    // residential simulator vslots 0x64 / 0x5c: the population and the education grids of a wealth
    void ResidentialGrids(int wealth, ICrimeCensusGrid*& population, ICrimeCensusGrid*& value) override {
        population = value = nullptr;
        auto* residential = CityAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
        if (!residential || wealth < 0 || wealth > 2) return;
        populations_[wealth].Set(residential->GetPopulationGrid(wealth));
        values_[wealth].Set(residential->GetEQGrid(wealth));
        population = &populations_[wealth];
        value = &values_[wealth];
    }
    std::vector<cGZPersistResourceKey> ListCrimeTypeExemplars() override {
        std::vector<cGZPersistResourceKey> keys;
        if (!SL::spRM) return keys;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeAndGroupFilter filter(kCrimeTypeExemplarType, kCrimeTypeExemplarGroup);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        return keys;
    }
    cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& key) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        loaded_[holder] = exemplar;
        return holder;
    }
    void ReleaseExemplar(cISCPropertyHolder* exemplar) override {
        auto it = loaded_.find(exemplar);
        if (it == loaded_.end()) return;
        it->second->Release();
        loaded_.erase(it);
    }
    // SC4LoadExemplarString(out, group, instance, property, null): the string the exemplar's property (a resource key) points at.
    // The shipped crime types point at strings the game data does not have, so their names stay empty as in the original.
    bool LoadExemplarString(std::uint32_t group, std::uint32_t instance, std::uint32_t property, std::string& out) override {
        cISCPropertyHolder* exemplar = LoadExemplar(cGZPersistResourceKey{kCrimeTypeExemplarType, group, instance});
        if (!exemplar) return false;
        cRZString text;
        const bool found = native::LoadExemplarString(text, exemplar, property);
        if (found) out = text.str();
        ReleaseExemplar(exemplar);
        return found;
    }
    // message 0x0a159d95: data1 = the crime type, the lot, the description (Mac i386 0x00172cea)
    void PostCrimeEvent(std::uint32_t typeId, ICrimeLot* lot, const std::string& description) override {
        if (Trace()) {
            int x = 0, z = 0;
            if (lot) lot->GetCell(x, z);
            std::fprintf(stderr, "crime: event type %08x at lot cell %d,%d: %s\n", typeId, x, z, description.c_str());
        }
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kCrimeMsgCrimeEvent);
        message->SetData1(typeId);
        auto* view = static_cast<CrimeLot*>(lot);
        message->SetIGZUnknown(view ? LotSerializable(view->Lot()) : nullptr);
        auto* text = new cRZString(description.c_str());
        message->SetString(text);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void PostCrimesUpdated() override {
        if (Trace())
            if (cSC4CrimeSimulator* crime = Crime())
                std::fprintf(stderr, "crime: month done, %d crimes, average %d, %zu events\n", crime->GetMonthlyCrimeCount(),
                             crime->GetAverageCrime(), crime->Events().size());
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kCrimeMsgCrimesUpdated);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    // fire manager: IgniteOccupant(the lot's building), then SetCause(2)
    void StartArson(ICrimeLot*, ICrimeBuilding* building) override {
        auto* view = static_cast<CrimeBuilding*>(building);
        cISC4BuildingOccupant* occupant = view ? view->Building() : nullptr;
        if (occupant) NativeStartArson(occupant->AsOccupant());
    }
    bool WriteLot(cIGZOStream& stream, ICrimeLot* lot) override {
        auto* view = static_cast<CrimeLot*>(lot);
        return SC4WriteGZSerializable(stream, view ? LotSerializable(view->Lot()) : nullptr);
    }
    // The lot comes in its serializable adapter (the reader's reference stays with the adapter table: the lot outlives the event).
    ICrimeLot* ReadLot(cIGZIStream& stream) override {
        cIGZUnknown* adapter = nullptr;
        SC4ReadGZSerializable(stream, 0xe818fced, reinterpret_cast<void**>(&adapter), false);
        void* plain = nullptr;
        if (!adapter || !adapter->QueryInterface(kGZIID_PlainObject, &plain) || !plain) return nullptr;
        auto* lot = static_cast<cSC4Lot*>(plain);
        lot->AddRef();          // the event's reference
        adapter->Release();
        return LotView(lot);
    }
    // city vslot 0x17c PositionToCell: an incident is kept by its cell
    void LotPositionToIncidentXZ(float x, float z, float& outX, float& outZ) override {
        int cellX = 0, cellZ = 0;
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        outX = static_cast<float>(cellX);
        outZ = static_cast<float>(cellZ);
    }
    std::vector<ICrimeOccupant*> OccupantsInBox(const float min[3], const float max[3]) override {
        std::vector<ICrimeOccupant*> out;
        auto* manager = CityAs<cISC4OccupantManager>(CityComponent::OccupantManager);
        if (!manager) return out;
        OccupantList list;
        manager->GetOccupantsByBBox(list, min, max, static_cast<std::uint32_t>(0), 0xffffffffu);
        for (cISC4Occupant* occupant : list) out.push_back(new CrimeOccupant(occupant));   // the list's reference stays with the view
        return out;
    }
    void ReleaseOccupants(std::vector<ICrimeOccupant*>& list) override {
        for (ICrimeOccupant* view : list) {
            auto* occupant = static_cast<CrimeOccupant*>(view);
            occupant->Occupant()->Release();
            delete occupant;
        }
        list.clear();
    }
    double RandomDouble() override { return random_.RandomDoubleUniform(); }
    double RandomDoubleRange(double lo, double hi) override { return random_.RandomDoubleRangeUniform(lo, hi); }
    std::uint32_t RandomUint32(std::uint32_t bound) override { return bound ? random_.RandomUint32Uniform(bound) : 0; }

    void ForgetLots() { lots_.clear(); }

private:
    ICrimeLot* LotView(cSC4Lot* lot) {
        if (!lot) return nullptr;
        auto& slot = lots_[lot];
        if (!slot) slot = std::make_unique<CrimeLot>(lot);
        return slot.get();
    }
    std::map<cSC4Lot*, std::unique_ptr<CrimeLot>> lots_;
    std::map<cISCPropertyHolder*, cISCResExemplar*> loaded_;
    CensusGrid<std::uint16_t> populations_[3];
    CensusGrid<float> values_[3];
    cRZRandom random_{0xc1e5u};
} g_crimeHost;

// The simulator's message target: the daily / monthly / idle agents and the notifications.
class CrimeTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kGZIID_cIGZMessageTarget2) { *out = nullptr; return false; }
        *out = static_cast<cIGZMessageTarget2*>(this);
        return true;
    }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool DoMessage(cIGZMessage2* message) override {
        cSC4CrimeSimulator* crime = Crime();
        if (!crime) return true;
        const std::uint32_t type = message->GetType();
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        if (type == kCrimeMsgOccupantAdded || type == kCrimeMsgOccupantRemoved) {
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            if (!occupant) return true;
            CrimeOccupant view(occupant);
            crime->DoMessage(type, 0, 0, &view);
        } else if (type == kCrimeMsgExemplarReloaded) {
            crime->DoMessage(type, static_cast<std::uint32_t>(standard->GetData1()), static_cast<std::uint32_t>(standard->GetData2()));
        } else {
            crime->DoMessage(type);
        }
        return true;
    }
} g_crimeTarget;

constexpr std::uint32_t kNotifications[4] = {kCrimeMsgOccupantAdded, kCrimeMsgOccupantRemoved, kCrimeMsgOrdinanceChanged,
                                             kCrimeMsgExemplarReloaded};

}  // namespace

void InstallCrimeHost() { sc4::SetCityProbeCrimeHost(&g_crimeHost); }

// The crime layer of the automata (cSC4TrafficAutomataManager::PopulateCrimeLayer): the incidents by cell.
// Off unless SC4_CRIMELAYER is set: two of the figures the layer creates (the carjacked car 0xcbb32b0a and the getaway van)
// have no class yet, so their crime types cannot be shown.
bool NativeCrimeIncidents(std::vector<sc4::ITrafficNetworkManagerApi::CrimeIncidentCell>& out) {
    static const bool enabled = std::getenv("SC4_CRIMELAYER") != nullptr;
    if (!enabled) return false;
    sc4::cSC4CrimeSimulator* crime = Crime();
    std::vector<sc4::CrimeIncident> incidents;
    if (!crime || !crime->GetCrimeIncidentArray(incidents)) return false;
    for (const sc4::CrimeIncident& incident : incidents)
        out.push_back({static_cast<int>(incident.x), static_cast<int>(incident.z), incident.typeId, incident.visualsCreated != 0});
    return true;
}
void NativeSetCrimeVisualsCreated(std::uint32_t index, bool created) {
    if (sc4::cSC4CrimeSimulator* crime = Crime()) crime->SetVisualsCreated(index, created);
}

// The crime simulator's part of the city's component lifecycle (the original's Init takes no argument: it reads its tuning
// exemplar itself and registers with the simulator and the message server).
bool CityComponentInit_cSC4CrimeSimulator(cIGZUnknown* object, bool init) {
    void* raw = nullptr;
    auto* adapter = dynamic_cast<sc4::PlainSerializableAdapter<sc4::cSC4CrimeSimulator>*>(object);
    if (!adapter || !adapter->QueryInterface(sc4::kGZIID_PlainObject, &raw) || !raw) return false;
    auto* crime = static_cast<sc4::cSC4CrimeSimulator*>(raw);
    if (init) {
        const cGZPersistResourceKey key{sc4::kCrimeTypeExemplarType, sc4::kCrimeTunableGroup, sc4::kCrimeTunableInstance};
        sc4::cISCPropertyHolder* tunables = g_crimeHost.LoadExemplar(key);
        const bool started = crime->Init(tunables);
        if (tunables) g_crimeHost.ReleaseExemplar(tunables);
        if (started) {
            for (int interval : {4, 6, 10}) AddCitySimulatorAgent(&g_crimeTarget, interval, "cSC4CrimeSimulator", 2);
            if (SL::spMS2)
                for (std::uint32_t type : kNotifications) SL::spMS2->AddNotification(&g_crimeTarget, type);
        }
        if (Trace()) std::fprintf(stderr, "crime: init %s, tunables %p\n", started ? "ok" : "skipped", static_cast<void*>(tunables));
    } else {
        RemoveCitySimulatorAgent(&g_crimeTarget);
        if (SL::spMS2)
            for (std::uint32_t type : kNotifications) SL::spMS2->RemoveNotification(&g_crimeTarget, type);
        crime->Shutdown();
        g_crimeHost.ForgetLots();
    }
    return true;
}

}  // namespace native
