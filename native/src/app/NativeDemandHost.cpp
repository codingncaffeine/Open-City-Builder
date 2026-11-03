// IDemandSimulatorHost on the native application (DEMAND-1; replaces the generated neutral stub, under which the demand simulator
// loaded no exemplar: no tunables, no demand channels set up, every R / C / I demand 0 with unset bounds, so growth never drew a
// developer). The exemplars come from the resource manager, the history from the city's history warehouse, the census from the
// zone developer's lots, the building catalogue and developers from the growth chain.
//
// Neutral answers, chosen so they leave the demand formulas unchanged (each a to-do): the ordinance factor 1.0 (the tax modifier
// is curve + ordinance - 1) and the nation modifier 0 (no ordinance / nation data natively); no region city, no neighbour
// connections, no airport / seaport trips; the residential workforce and EQ maps (the port's residential simulator exposes none)
// read 1.0 and 0.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DemandSimulator.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/RegionalCity.h"
#include "app/NativeRegion.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/Lot.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4Demand.h"

namespace native {
namespace {
using namespace sc4;

template <class T> T* Com(sc4::CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

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

// The census's view of a lot (cISC4Lot +0x130 / +0x170 / +0x148 / +0x198).
class LotView final : public IDemandLot {
public:
    cSC4Lot* lot = nullptr;
    bool HasCapacity() const override { return lot && lot->HasCapacity(); }
    std::uint32_t GetBuildingType(bool ignoreFamily) const override { return lot ? lot->GetBuildingType(ignoreFamily) : 0; }
    std::int32_t GetOccupantWealth() const override { return lot ? lot->GetOccupantWealth() : 0; }
    std::uint16_t GetCapacity(std::uint32_t developerType, bool ignorePartial) const override { return lot ? lot->GetCapacity(developerType, ignorePartial) : 0; }
};

// A growth developer as the demand simulator sees it.
class DeveloperView final : public IDemandDeveloper {
public:
    void* developer = nullptr;
    std::uint32_t GetType() const override { return GrowthDeveloperKind(developer); }
    std::uint32_t GetDeveloperType() const override { return GrowthDeveloperSubType(developer); }
    cSC4Demand* GetSourceDemand() const override { return GrowthDeveloperDemandChannel(developer); }
    std::int16_t DesirabilityAt(unsigned x, unsigned z) const override { return GrowthDeveloperDesirabilityAt(developer, x, z); }
};

}  // namespace

// The census's copy of a building's profile (a structural copy of BuildingProfile's first fields), cached per type.
const sc4::DemandBuildingProfile* NativeDemandBuildingProfile(std::uint32_t buildingType) {
    static std::map<std::uint32_t, sc4::DemandBuildingProfile> cache;
    auto it = cache.find(buildingType);
    if (it != cache.end()) return &it->second;
    const sc4::BuildingProfile* profile = NativeBuildingProfile(buildingType);
    if (!profile) return nullptr;
    sc4::DemandBuildingProfile& copy = cache[buildingType];
    copy.purpose = profile->purpose;
    copy.wealth = profile->wealth;
    copy.capacity = profile->capacity;
    copy.growthStage = profile->growthStage;
    copy.capacities = profile->capacities;
    return &copy;
}

namespace {

// A region city as the demand simulator reads it (demand.md §9): place, size, workforce, taxes, populations.
class RegionCityView final : public IDemandRegionCity {
public:
    explicit RegionCityView(cSC4RegionalCity* city) : city_(city) {}
    void GetPosition(int& x, int& z) const override { city_->GetPosition(x, z); }
    void GetCitySize(int& w, int& h) const override { city_->GetCitySize(w, h); }
    float WorkforcePercentage() const override { return city_->GetWorkforcePercentage(); }
    float GetTaxRate(std::uint32_t purpose) const override { return city_->GetTaxRate(purpose); }
    int GetPopulation(std::uint32_t purpose) const override { return city_->GetPopulation(purpose); }
    int GetExtrapolatedPopulation(std::uint32_t purpose) const override { return city_->GetExtrapolatedPopulation(purpose); }
    int GetAllowableExtrapolation(std::uint32_t purpose) const override { return city_->GetAllowableExtrapolation(purpose); }
    void ExtrapolateGrowth(std::uint32_t purpose, float amount) override { city_->ExtrapolateGrowth(purpose, amount); }
private:
    cSC4RegionalCity* city_;
};
std::map<cISC4RegionalCity*, std::unique_ptr<RegionCityView>> g_regionViews;   // the region keeps its cities for the session
IDemandRegionCity* RegionView(cISC4RegionalCity* city) {
    if (!city) return nullptr;
    auto& view = g_regionViews[city];
    if (!view) view = std::make_unique<RegionCityView>(static_cast<cSC4RegionalCity*>(city));
    return view.get();
}
// The neighbours simulator's cities (slots 21 / 22, unestablished ones left out).
void NeighborCities(bool all, std::list<IDemandRegionCity*>& out) {
    out.clear();
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::NeighborsSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* neighbors = serializable->GetGZCLSID() == kGZCLSID_cSC4NeighborsSimulator ? static_cast<cSC4NeighborsSimulator*>(serializable) : nullptr;
    serializable->Release();
    if (!neighbors) return;
    std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
    if (all) neighbors->GetAllConnectedCities(cities, false);
    else neighbors->GetConnectedCities(cities, false);
    for (auto& ref : cities)
        if (IDemandRegionCity* view = RegionView(ref.get())) out.push_back(view);
}

class DemandHost final : public IDemandSimulatorHost {
public:
    bool MessageLot(cIGZUnknown* object, std::uint32_t& buildingType, sc4::cISCPropertyHolder*& building) override {
        buildingType = 0;
        building = nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return false;
        auto* lot = static_cast<cSC4Lot*>(raw);
        buildingType = lot->GetBuildingType(true);
        void* view = nullptr;
        if (cIGZUnknown* occupant = lot->GetBuilding(); occupant && occupant->QueryInterface(kGZIID_cISC4Occupant, &view) && view) {
            building = static_cast<cISC4Occupant*>(view)->AsPropertyHolder();
            static_cast<cISC4Occupant*>(view)->Release();
        }
        return true;
    }
    void AddShutdownCallback(bool (*)(void*), void*) override {}   // the app's shutdown callbacks: the native app tears the city down itself
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    void AddSimulatorAgent(cIGZMessageTarget2* target, int interval) override { AddCitySimulatorAgent(target, interval, "cSC4DemandSimulator", 2); }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    // cISC4Demand::CalculateActiveDemand's announcement (0x426840a0 {id, value})
    void PostMessage(std::uint32_t type, std::uint32_t data1, std::int32_t data2) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetData1(data1);
        message->SetData2(static_cast<std::uint64_t>(static_cast<std::int64_t>(data2)));
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    std::uint32_t CurrentDayNumber() override {
        auto* simulator = Com<cSC4Simulator>(sc4::CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
    }
    std::uint64_t NowCycles() override {   // mach_absolute_time in microseconds, with MillisecondsToCycles to match
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    float MillisecondsToCycles() override { return 1000.0f; }
    void ExemplarKeysOfType(std::uint32_t type, std::vector<cGZPersistResourceKey>& out) override {
        out.clear();
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(type);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        for (std::uint32_t i = 0; i < list->Size(); ++i) out.push_back(*list->GetKey(i));
        list->Release();
    }
    sc4::cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& key) override {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        sc4::cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        held_[holder] = exemplar;
        return holder;
    }
    void ReleaseExemplar(sc4::cISCPropertyHolder* holder) override {
        auto it = held_.find(holder);
        if (it == held_.end()) return;
        it->second->Release();
        held_.erase(it);
    }
    void CityCellCount(int& x, int& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 0;
        z = city ? city->CellCountZ() : 0;
    }
    std::uint32_t AirportTrips() override { return 0; }
    std::uint32_t SeaportTrips() override { return 0; }
    IDemandRegionCity* CurrentRegionCity() override { return RegionView(NativeCurrentRegionalCity()); }
    void HistoryMinMaxAverage(std::uint32_t series, std::uint32_t fromDay, std::uint32_t toDay, float& minimum, float& maximum, float& average) override {
        minimum = maximum = average = 0.0f;
        if (cSC4HistoryWarehouse* history = History()) history->GetMinMaxAverageDays(series, fromDay, toDay, minimum, maximum, average);
    }
    void HistoryRecord(std::uint32_t series, std::int32_t value) override { if (cSC4HistoryWarehouse* history = History()) history->RecordHistoryData(series, value); }
    void HistoryQueueLength(std::uint32_t series, std::uint32_t length) override { if (cSC4HistoryWarehouse* history = History()) history->SetHistoryDataQueueLength(series, length); }
    float NationDemandModifier(std::uint32_t) override { return 0.0f; }
    float OrdinanceFactor(std::uint32_t, std::uint32_t) override { return 1.0f; }
    void AllConnections(std::vector<IDemandConnection*>& out) override { out.clear(); }
    void ConnectionsOfType(std::uint32_t, std::vector<IDemandConnection*>& out) override { out.clear(); }
    void AllConnectedCities(std::list<IDemandRegionCity*>& out) override { NeighborCities(true, out); }
    void ConnectedCities(std::list<IDemandRegionCity*>& out) override { NeighborCities(false, out); }
    std::uint32_t TrafficArrived(sc4::cISCPropertyHolder*, std::uint32_t) override { return 0; }
    const DemandBuildingProfile* BuildingProfile(std::uint32_t buildingType) override { return NativeDemandBuildingProfile(buildingType); }
    std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) override { return NativeDeveloperType(purpose, wealth); }
    IDemandDeveloper* Developer(std::uint32_t developerType) override {
        void* d = GrowthDeveloperOfType(developerType);
        if (!d) return nullptr;
        DeveloperView& view = views_[d];
        view.developer = d;
        return &view;
    }
    void Developers(std::vector<IDemandDeveloper*>& out) override {
        out.clear();
        for (void* d : AllGrowthDevelopers()) {
            DeveloperView& view = views_[d];
            view.developer = d;
            out.push_back(&view);
        }
    }
    void AllBuildingTypes(std::vector<std::uint32_t>& out) override { NativeAllBuildingTypes(out); }
    bool BuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) override { return NativeBuildingKeyFromType(type, key); }
    std::int32_t DayOfLastSuccessfulBuild(std::uint32_t developerType) override { return NativeDayOfLastSuccessfulBuild(developerType); }
    IDemandLot* LotAt(int x, int z, int) override {
        auto* developer = Com<cSC4ZoneDeveloper>(sc4::CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId);
        cSC4Lot* lot = developer ? developer->GetLot(x, z, true) : nullptr;   // the census visits a lot once, at its anchor cell
        if (!lot) return nullptr;
        lot_.lot = lot;
        return &lot_;
    }
    float CellWorkforcePercent(unsigned, unsigned) override { return 1.0f; }
    float CellEQ(unsigned, unsigned) override { return 0.0f; }

private:
    static cSC4HistoryWarehouse* History() { return Com<cSC4HistoryWarehouse>(sc4::CityComponent::HistoryWarehouse, kGZCLSID_cSC4HistoryWarehouse); }
    std::map<sc4::cISCPropertyHolder*, cISCResExemplar*> held_;
    std::map<void*, DeveloperView> views_;
    LotView lot_;
};

}  // namespace

void InstallDemandSimulatorHost() {
    static DemandHost host;
    sc4::SetDemandSimulatorHost(&host);
}

}  // namespace native
