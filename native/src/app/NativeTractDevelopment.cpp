// The tract developer on the native application (notes/GROWTH_PLAN.md layer 3). Replaces the generated neutral ITractDeveloperHost:
// the city, zone manager, lots (through the zone developer, the port's lot map), lot configurations, building catalogue,
// construction resources and network occupants are the city's, so cSC4TractDeveloper::Grow searches real lots and real lot
// configurations and picks real buildings. Its last step is the city's lot developer's StartConstruction (layer 4).
//
// The cell services are the city's simulators: the land value simulator's total map, the fire protection grid, the aura
// simulator's park map, the traffic simulator's trip length map, power and water; the tunables are the simulator exemplar's.
// Neutral here (PARTIAL): the lot developer's NeedsAnchorBuilding (false). DemolishLots goes to the demolition utility.
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_set>

#include "app/NativeDemolition.h"
#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativeLotConstruction.h"
#include "app/NativePlumbing.h"
#include "app/NativePower.h"
#include "app/NativeTraffic.h"
#include "platform/Unwired.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZRandom.h"
#include "sc4/AuraSimulator.h"
#include "sc4/CommonIIDs.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/GrowthDeveloper.h"
#include "sc4/Exemplar.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/LotDeveloper.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/cSC4ZoneManager.h"

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
template <class T> T* Plain(sc4::CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}
cSC4ZoneDeveloper* ZoneDeveloper() { return Com<cSC4ZoneDeveloper>(sc4::CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4TractDeveloper* TractDeveloper() { return Com<cSC4TractDeveloper>(sc4::CityComponent::TractDeveloper, kTractDeveloperClassId); }
cSC4LotDeveloper* LotDeveloper() { return Com<cSC4LotDeveloper>(sc4::CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4LotConfigurationManager* LotConfigurations() { return Plain<cSC4LotConfigurationManager>(sc4::CityComponent::LotConfigurationManager); }

// ---- the growth developer as the tract developer sees it (IGrowthDeveloperApi)
template <class T> class ValueMap final : public ITractValueMap {
public:
    explicit ValueMap(cSC4SimGrid<T>* grid) : grid_(grid) {}
    std::int32_t GetTractSize() const override { return grid_ ? grid_->GetTractSize() : 0; }
    std::int32_t GetTractShift() const override { return grid_ ? grid_->GetTractShift() : 0; }
    std::int32_t GetCellValue(int x, int z) const override { return grid_ ? static_cast<std::int32_t>(grid_->GetTractValue(x, z)) : 0; }   // grid vslot 0xb0
    void Reset(cSC4SimGrid<T>* grid) { grid_ = grid; }
private:
    cSC4SimGrid<T>* grid_;
};
class DemandView final : public ITractDemandApi {
public:
    IGrowthDemandApi* demand = nullptr;
    float GetValue() const override { return demand ? demand->GetActiveValue() : 0.0f; }   // demand vslot 0x60
};
class GrowthView final : public IGrowthDeveloperApi {
public:
    explicit GrowthView(cSC4GrowthDeveloper* d)
        : d_(d), desirability_(d->GetDesirabilityMap()), capacity_(d->GetCapacityMap()), population_(d->GetPopulationMap()),
          maxCapacity_(d->GetMaxCapacityMap()) {}
    std::int32_t GetDeveloperType() const override { return d_->GetSubType(); }
    std::int32_t GetPurpose() const override { return d_->GetPurpose(); }
    std::int32_t GetWealth() const override { return d_->GetWealth(); }
    void GetConstructionInfo(ConstructionProfile& out) const override { d_->GetConstructionInfo(out); }
    ITractValueMap* GetDesirabilityMap() const override { desirability_.Reset(d_->GetDesirabilityMap()); return &desirability_; }
    ITractValueMap* GetCapacityMap() const override { capacity_.Reset(d_->GetCapacityMap()); return &capacity_; }
    ITractValueMap* GetPopulationMap() const override { population_.Reset(d_->GetPopulationMap()); return &population_; }
    ITractValueMap* GetMaxCapacityMap() const override { maxCapacity_.Reset(d_->GetMaxCapacityMap()); return &maxCapacity_; }
    ITractDemandApi* GetSourceDemand() const override { demand_.demand = d_->GetSourceDemand(); return &demand_; }
    std::int32_t GetType() const override { return d_->GetType(); }
    std::uint8_t GetPowerSupplyCap() const override { return d_->GetPowerSupplyCap(); }
    std::uint8_t GetWaterSupplyCap() const override { return d_->GetWaterSupplyCap(); }
    std::uint8_t GetFireCoverageCap() const override { return d_->GetFireCoverageCap(); }
    std::uint8_t GetParkCap() const override { return d_->GetParkCap(); }
    std::uint32_t GetMonopolyUse() const override { return d_->GetMonopolyUse(); }
    const std::map<std::uint32_t, SC4Percentage>& GetStylePercentages() const override { return d_->GetStylePercentages(); }
private:
    cSC4GrowthDeveloper* d_;
    mutable ValueMap<std::int16_t> desirability_;
    mutable ValueMap<std::uint16_t> capacity_;
    mutable ValueMap<std::int16_t> population_;
    mutable ValueMap<std::uint16_t> maxCapacity_;
    mutable DemandView demand_;
};
std::map<cSC4GrowthDeveloper*, GrowthView*> g_views;
GrowthView* ViewOf(cSC4GrowthDeveloper* d) {
    if (!d) return nullptr;
    auto it = g_views.find(d);
    if (it != g_views.end()) return it->second;
    return g_views[d] = new GrowthView(d);
}

// ---- the services
struct PlainService final : ITractDeveloperService {} g_landValue, g_plumbing, g_power, g_traffic;

struct CityApi final : ITractCityApi {
    int CellCountX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CellCountZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void RemoveDeveloper(cIGZMessageTarget2*) override {}
} g_city;

struct BuildingDevelopmentApi final : ITractBuildingDevelopmentApi {
    bool CheatCodesEnabled() override { return false; }
    const BuildingProfile& GetBuildingProfile(std::uint32_t type) const override {
        static const BuildingProfile none{};
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? *profile : none;
    }
    std::uint16_t GetFreeConstructionResources() override { return NativeFreeConstructionResources(); }
    bool ClaimConstructionResources(std::uint16_t amount) override { return NativeClaimConstructionResources(amount); }
    void ReleaseConstructionResources(std::uint16_t amount) override { NativeReleaseConstructionResources(amount); }
    bool GetBuildingKeyFromType(std::uint32_t type, cGZPersistResourceKey& key) override { return NativeBuildingKeyFromType(type, key); }
    void GetBuildingTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) override {
        std::vector<std::uint32_t> types;
        NativeBuildingsInFamily(family, types);
        out.insert(types.begin(), types.end());
    }
    bool CheatIgnorePlumbing() override { return NativeBuildingCheat(0); }
    bool CheatIgnorePower() override { return NativeBuildingCheat(1); }
    bool CheatIgnoreCaps() override { return NativeBuildingCheat(2); }
    bool NeedsAnchorBuilding(const std::int32_t* rect, std::int32_t purpose) override {   // BDS vslot 0x120 -> lot developer slot 36
        cSC4LotDeveloper* developer = LotDeveloper();
        return developer && developer->NeedsAnchorBuilding(rect, purpose);
    }
    IGrowthDeveloperApi* GetDeveloper(std::uint32_t purpose, std::int32_t wealth) override {
        return ViewOf(GrowthDeveloperOfType(NativeDeveloperType(purpose, wealth)));
    }
} g_buildingDevelopment;

struct LotManagerApi final : ITractLotManagerApi {   // SL::spLotManager: the port's lot map is the zone developer's
    cSC4Lot* GetLot(int x, int z, bool exactCell) override { cSC4ZoneDeveloper* d = ZoneDeveloper(); return d ? d->GetLot(x, z, exactCell) : nullptr; }
    int GetLotsInCellRect(std::list<cRZAutoRefCount<cSC4Lot>>& out, int x0, int z0, int x1, int z1, bool unique) override {
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        return d ? d->GetLotsInCellRect(out, x0, z0, x1, z1, unique) : 0;
    }
    int CanCreateLot(int x, int z, int sizeX, int sizeZ, bool param5, bool param6, cSC4LotConfiguration* configuration) override {
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        return d ? d->CanCreateLot(x, z, sizeX, sizeZ, param5, param6, configuration) : 0;
    }
    int GetLotsSurroundingLot(cSC4Lot* lot, std::list<cRZAutoRefCount<cSC4Lot>>& out, int edgeMask) override {
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        return d ? d->GetLotsSurroundingLot(lot, out, edgeMask) : 0;
    }
    int GetLotsSurroundingRect(const std::int32_t rect[4], std::list<cRZAutoRefCount<cSC4Lot>>& out, int edgeMask) override {
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        if (!d) return 0;
        SC4Rect<int> r;
        r.x0 = rect[0]; r.z0 = rect[1]; r.x1 = rect[2]; r.z1 = rect[3];
        return d->GetLotsSurroundingRect(r, out, edgeMask);
    }
    bool CreateLotIfPossible(int x, int z, int sizeX, int sizeZ, int facing, cSC4Lot*& out) override {
        out = nullptr;
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        const bool made = d && d->CreateLotIfPossible(x, z, sizeX, sizeZ, facing, out, false, false, nullptr);
        if (std::getenv("SC4_SIMDEBUG"))
            std::fprintf(stderr, "tract developer: CreateLotIfPossible (%d,%d) %dx%d facing %d -> %s\n", x, z, sizeX, sizeZ, facing,
                         out ? "lot" : (made ? "deferred" : "FAILED"));
        return made;
    }
    void DeleteLot(cSC4Lot* lot) override {
        if (std::getenv("SC4_SIMDEBUG") && lot) {
            int x = 0, z = 0;
            lot->GetLocation(x, z);
            std::fprintf(stderr, "tract developer: DeleteLot (%d,%d)\n", x, z);
        }
        if (cSC4ZoneDeveloper* d = ZoneDeveloper()) d->DeleteLot(lot);
    }
} g_lotManager;

// SL::spLotDeveloper: layer 4. StartConstruction is where a building would begin; AbandonBuilding / ReoccupyBuilding are the city's
// cSC4LotDeveloper's (the decline and the take-over of abandoned buildings).
struct LotDeveloperApi final : ITractLotDeveloperApi {
    void StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType, bool flag) override {
        ++constructions;
        if (std::getenv("SC4_SIMDEBUG")) {
            int x = 0, z = 0;
            if (lot) lot->GetLocation(x, z);
            std::fprintf(stderr, "tract developer: StartConstruction lot (%d,%d) configuration %08x building %08x%s\n", x, z,
                         configuration ? configuration->GetID() : 0, buildingType, flag ? " (keep)" : "");
        }
        cSC4LotDeveloper* developer = LotDeveloper();
        const bool started = developer && developer->StartConstruction(lot, configuration, buildingType, flag);   // vslot 0x18
        if (std::getenv("SC4_SIMDEBUG")) std::fprintf(stderr, "tract developer: construction %s\n", started ? "started" : "FAILED");
    }
    void AbandonBuilding(cSC4Lot* lot, int vacateReason) override {   // vslot 0x40
        cSC4LotDeveloper* developer = LotDeveloper();
        if (!developer) return;
        ++abandoned;
        if (std::getenv("SC4_SIMDEBUG")) TraceLot("abandon", lot, vacateReason);
        developer->AbandonBuilding(lot, static_cast<std::uint8_t>(vacateReason));
    }
    void ReoccupyBuilding(cSC4Lot* lot, std::uint32_t developerType) override {   // vslot 0x48
        cSC4LotDeveloper* developer = LotDeveloper();
        if (!developer) return;
        ++reoccupied;
        if (std::getenv("SC4_SIMDEBUG")) TraceLot("reoccupy", lot, static_cast<int>(developerType));
        developer->ReoccupyBuilding(lot, developerType);
    }
    static void TraceLot(const char* what, cSC4Lot* lot, int value) {
        int x = 0, z = 0;
        if (lot) lot->GetLocation(x, z);
        std::fprintf(stderr, "tract developer: %s lot (%d,%d) building %08x state %u (%x)\n", what, x, z, lot ? lot->GetBuildingType(false) : 0,
                     lot ? lot->GetState() : 0, value);
    }
    float GetAreaSlope(const std::int32_t rect[4], bool includeWater) override { return NativeAreaSlope(rect, includeWater); }
    int constructions = 0;
    int abandoned = 0;
    int reoccupied = 0;
} g_lotDeveloper;

struct ZoneManagerApi final : ITractZoneManagerApi {
    int ZoneTypeAt(int x, int z) override {
        auto* m = Com<cSC4ZoneManager>(sc4::CityComponent::ZoneManager, kZoneManagerClassId);
        return m ? m->GetZoneType(x, z) : 0;
    }
} g_zoneManager;

// City vslot 0x140 GetTrafficNetwork -> vslot 0x50: the cell's network types come from its network occupant.
struct NetworkCell final : ITractNetworkCellApi {
    std::uint32_t types = 0;
    bool HasNetworkType(int type) const override { return (types >> type) & 1; }
} g_networkCell;
struct NetworkManagerApi final : ITractNetworkManagerApi {
    ITractNetworkCellApi* GetNetworkAt(int x, int z, std::uint32_t typeMask, int) override {
        g_networkCell.types = NativeNetworkTypesAt(x, z);
        return (g_networkCell.types & typeMask) ? &g_networkCell : nullptr;
    }
} g_networkManager;

// ---- the cell services the growth predicates read (the simulators the original reaches through its SL:: globals)
template <class T> T* Simulator(sc4::CityComponent c) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(c)) : nullptr;
}
template <class T> bool CellOf(const cSC4SimGrid<T>* grid, int x, int z, T& out) {
    if (!grid || x < 0 || z < 0 || (x >> grid->GetTractShift()) >= grid->GetTractCountX() ||
        (z >> grid->GetTractShift()) >= grid->GetTractCountZ()) return false;
    out = grid->GetCellValue(x, z);   // grid vslot 0x38
    return true;
}
cSC4SimGrid<std::uint8_t>* LandValueMap() {   // SL::spLandValueSim (GetLandValue reads its total land value map)
    auto* simulator = Simulator<cSC4LandValueSimulator>(sc4::CityComponent::LandValueSimulator);
    return simulator ? simulator->GetTotalLandValueMap() : nullptr;
}
cSC4SimGrid<std::uint8_t>* FireProtectionMap() {   // SL::spFireProtectionSim vslot 0x28 GetFireProtectionGrid
    auto* simulator = Simulator<cSC4FireProtectionSimulator>(sc4::CityComponent::FireProtectionSimulator);
    return simulator ? simulator->GetFireProtectionGrid() : nullptr;
}
cSC4SimGrid<std::int16_t>* ParkMap() {   // SL::spAuraSim vslot 0x70 GetParkMap
    auto* simulator = Simulator<cSC4AuraSimulator>(sc4::CityComponent::AuraSimulator);
    return simulator ? simulator->GetParkMap() : nullptr;
}

// The tract developer's tunables: the simulator exemplar {0x6534284a, 0xe7e2c2db, 0xe8da7677} (ReadSimulatorTunableValues,
// Mac i386 0x000ebf4c). Each value is reset to its default and replaced by the exemplar's when it has the property; nothing
// changes without the exemplar.
bool ReadTractTunables(TractTunables& io) {
    const cISCPropertyHolder* exemplar = NativeSimulatorExemplar(0xe8da7677);
    if (!exemplar) return false;
    const TractTunables defaults;
    auto single = [exemplar](std::uint32_t id, float& value, float initial) {
        value = initial;
        cSCPropertyHelp::GetPropertyValue(exemplar, id, value);
    };
    auto pair = [exemplar](std::uint32_t id, float& first, float& second, float initialFirst, float initialSecond) {
        first = initialFirst;
        second = initialSecond;
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, values) && values.size() >= 2) {
            first = values[0];
            second = values[1];
        }
    };
    single(0x47e2c500, io.capacityFactor, defaults.capacityFactor);
    pair(0x47e2c501, io.declineFactor, io.buildBias, defaults.declineFactor, defaults.buildBias);
    single(0x47e2c502, io.capacityFactor2, defaults.capacityFactor2);
    pair(0x47e2c510, io.existingCapacityWeight, io.abandonedCapacityWeight, defaults.existingCapacityWeight,
         defaults.abandonedCapacityWeight);
    pair(0x47e2c511, io.aggregationPenalty, io.subdivisionPenalty, defaults.aggregationPenalty, defaults.subdivisionPenalty);
    single(0x47e2c530, io.populationJitter, defaults.populationJitter);
    pair(0x47e2c531, io.declineJitterLow, io.declineJitterHigh, defaults.declineJitterLow, defaults.declineJitterHigh);
    io.allowTakeOver = true;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c540, io.allowTakeOver);
    io.styles.clear();
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c550, io.styles);
    std::int32_t years = 0;
    io.hasYearsBetweenStyleChanges = cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c551, years);
    if (io.hasYearsBetweenStyleChanges) io.yearsBetweenStyleChanges = years;
    return true;
}

sc4::cSTETerrain* CityTerrain() {   // SL::spTerrain: the city's terrain component
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<sc4::cSTETerrain*>(serializable);
}

struct TractHost final : ITractDeveloperHost {
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    bool SimulatorPresent() override { return CitySimulatorPresent(); }
    void RegisterSimulatorCallback(int messageId, cIGZMessageTarget2* target, const char* name) override { AddCitySimulatorAgent(target, messageId, name, 2); }
    ITractCityApi* City() override { return HostCity() ? &g_city : nullptr; }
    ITractBuildingDevelopmentApi* BuildingDevelopment() override { return &g_buildingDevelopment; }
    ITractDeveloperService* LandValue() override { return &g_landValue; }
    cSC4LotConfigurationManager* LotConfigurationManager() override { return LotConfigurations(); }
    ITractLotDeveloperApi* LotDeveloper() override { return &g_lotDeveloper; }
    ITractLotManagerApi* LotManager() override { return &g_lotManager; }
    ITractDeveloperService* Plumbing() override { return &g_plumbing; }
    ITractDeveloperService* Power() override { return &g_power; }
    ITractDeveloperService* Traffic() override { return &g_traffic; }
    ITractZoneManagerApi* ZoneManager() override { return &g_zoneManager; }
    IGrowthDeveloperApi* GrowthDeveloper(std::uint32_t type) override { return ViewOf(GrowthDeveloperOfType(type)); }
    bool LotConfigurationsByFilter(std::vector<cSC4LotConfiguration*>& out, cISC4LotConfigurationFilter* filter) override {
        return NativeLotConfigurationsByFilter(out, filter);
    }
    void RegisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) override { if (auto* m = LotConfigurations()) m->RegisterLotConfigurationFilter(filter); }
    void UnregisterLotConfigurationFilter(cISC4LotConfigurationFilter* filter) override { if (auto* m = LotConfigurations()) m->UnregisterLotConfigurationFilter(filter); }
    bool ReadTunables(TractTunables& io) override { return ReadTractTunables(io); }
    std::int32_t SimulatorDay() override {   // the simulator date (vslot 0x30 -> the date's day number)
        auto* simulator = Com<cSC4Simulator>(sc4::CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator ? static_cast<std::int32_t>(simulator->GetSimDateNumber()) : 0;
    }
    std::uint32_t RandomBelow(std::uint32_t range) override { static cRZRandom random(0x7a3cu); return range ? random.RandomUint32Uniform(range) : 0; }
    void ForEachCellInTract(int tractX, int tractZ, int shift, const std::function<void(int, int)>& fn) override {
        for (int x = tractX << shift; x < ((tractX + 1) << shift); ++x)
            for (int z = tractZ << shift; z < ((tractZ + 1) << shift); ++z) fn(x, z);
    }
    ITractNetworkManagerApi* NetworkManager() override { return &g_networkManager; }
    std::uint8_t LandValueAt(int x, int z) override {                   // land value sim vslot 0x30 GetLandValue
        std::uint8_t value = 0;
        CellOf(LandValueMap(), x, z, value);
        return value;
    }
    bool IsCellWatered(int x, int z) override { return NativeCellWatered(x, z); }
    std::uint8_t PoweredAt(int x, int z) override { return NativePowerSupplyAt(x, z); }
    bool FireProtectedAt(int x, int z) override {                       // fire protection grid at the cell (grid vslot 0x38)
        std::uint8_t value = 0;
        return CellOf(FireProtectionMap(), x, z, value) && value != 0;
    }
    std::int16_t ParkEffectAt(int x, int z) override {                  // park map at the cell (grid vslot 0x38)
        std::int16_t value = 0;
        CellOf(ParkMap(), x, z, value);
        return value;
    }
    bool HasTripLengthAt(int x, int z) override {                       // traffic sim vslot 0x68 GetTripLengthMap at the cell
        std::uint8_t value = 0;
        return CellOf(NativeTrafficTripLengthMap(), x, z, value) && value != 0;
    }
    // spDemolition DemolishLots(true, lots, privilege 0, flags, false, null, null, null) (tract-developer.md: 0x800 keeps the zone).
    void DemolishLots(const std::list<cRZAutoRefCount<cSC4Lot>>& lots, std::uint32_t flags) override {
        std::vector<cSC4Lot*> list;
        for (const auto& lot : lots)
            if (lot) list.push_back(lot.get());
        native::DemolishLots(true, list.data(), static_cast<int>(list.size()), 0, flags, false, nullptr);
    }
    void BuildingExemplarBool(const cGZPersistResourceKey& key, std::uint32_t property, bool& value) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), property, value);
        exemplar->Release();
    }
    bool BuildingExemplarUint32s(const cGZPersistResourceKey& key, std::uint32_t property, std::vector<std::uint32_t>& out) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return false;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        const bool found = cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), property, out);
        exemplar->Release();
        return found;
    }
    void RedisplayTerrain(const std::int32_t rect[4]) override {   // terrain i386 vslot 0x114 (false, false, rect, 0)
        if (sc4::cSTETerrain* t = CityTerrain()) t->RedisplayTerrain(false, false, sc4::TerrainRect{rect[0], rect[1], rect[2], rect[3]}, 0);
    }
} g_host;

}  // namespace

int NativeTractGrow(void* developer, int tractX, int tractZ, std::uint8_t style) {
    cSC4TractDeveloper* tract = TractDeveloper();
    GrowthView* view = ViewOf(static_cast<cSC4GrowthDeveloper*>(developer));
    if (!tract || !view) return 0;
    GrowthProfile profile;
    profile.developer = view;
    profile.tractX = tractX;
    profile.tractZ = tractZ;
    profile.growthStage = style;
    std::list<int> outcomes;
    bool declined = false;
    const int before = g_lotDeveloper.constructions;
    const int result = tract->Grow(profile, outcomes, declined);   // vslot 0x28
    if (std::getenv("SC4_SIMDEBUG")) {
        std::fprintf(stderr, "tract developer: Grow developer %x tract (%d,%d) -> %d, %zu outcomes, %d constructions, %zu lots, %zu configurations;",
                     view->GetDeveloperType(), tractX, tractZ, result, outcomes.size(), g_lotDeveloper.constructions - before,
                     tract->ExistingLots().size(), tract->SuitableConfigurations().size());
        const auto& histograms = tract->Histograms();   // the attempt record's outcome counts, row:outcome=count
        for (std::size_t row = 0; row < histograms.size(); ++row)
            for (std::size_t outcome = 0; outcome < histograms[row].size(); ++outcome)
                if (histograms[row][outcome]) std::fprintf(stderr, " %zu:%zu=%u", row, outcome, histograms[row][outcome]);
        std::fprintf(stderr, "\n");
        static int shown = 0;
        if (shown++ < 2) {
            std::fprintf(stderr, "tract developer: stage %u configurations", style);
            for (cSC4LotConfiguration* configuration : tract->SuitableConfigurations()) {
                std::uint32_t sx = 0, sz = 0;
                configuration->GetSize(sx, sz);
                std::fprintf(stderr, " %08x:%ux%u/s%u", configuration->GetID(), sx, sz, configuration->GetGrowthStage());
            }
            std::fprintf(stderr, "\n");
        }
    }
    return result;
}

// The original's GrowthProfile here carries the tract and the developer; its stage byte is left uninitialised (growth.md), and
// UpdatePopulation does not read it.
void NativeTractUpdatePopulation(void* developer, int tractX, int tractZ) {
    cSC4TractDeveloper* tract = TractDeveloper();
    GrowthView* view = ViewOf(static_cast<cSC4GrowthDeveloper*>(developer));
    if (!tract || !view) return;
    GrowthProfile profile;
    profile.developer = view;
    profile.tractX = tractX;
    profile.tractZ = tractZ;
    tract->UpdatePopulation(profile);   // vslot 0x38
}

static int g_declineOutcomes = 0;          // decline-plan outcomes the tract developer executed
static int g_declinedCapacity = 0;         // the capacity they abandoned

int NativeTractDecline(void* developer, int tractX, int tractZ, std::uint8_t flag) {
    cSC4TractDeveloper* tract = TractDeveloper();
    GrowthView* view = ViewOf(static_cast<cSC4GrowthDeveloper*>(developer));
    if (!tract || !view) return 0;
    GrowthProfile profile;
    profile.developer = view;
    profile.tractX = tractX;
    profile.tractZ = tractZ;
    profile.growthStage = flag;
    const int abandoned = tract->Decline(profile);   // vslot 0x30
    ++g_declineOutcomes;
    g_declinedCapacity += abandoned;
    if (std::getenv("SC4_SIMDEBUG"))
        std::fprintf(stderr, "tract developer: Decline developer %x tract (%d,%d) -> capacity %d abandoned\n", view->GetDeveloperType(), tractX,
                     tractZ, abandoned);
    return abandoned;
}

bool NativeLotConfigurationsByFilter(std::vector<sc4::cSC4LotConfiguration*>& out, sc4::cISC4LotConfigurationFilter* filter) {
    sc4::cSC4LotConfigurationManager* manager = LotConfigurations();
    if (!manager) return false;
    sc4::cSC4LotConfigurationManager::ConfigurationSet set;
    manager->GetLotConfigurationsByFilter(set, filter);
    out.assign(set.begin(), set.end());
    return !out.empty();
}

// The kit verb "decline": the lots of the city by state, and what the tract developer abandoned and reoccupied since the city opened.
void NativeDeclineReport() {
    cSC4ZoneDeveloper* zones = ZoneDeveloper();
    int states[8] = {};
    int abandonedByReason[8] = {};
    std::map<std::uint32_t, std::pair<int, int>> livable;   // developer type -> livable lots, of them past the 59 days Decline waits
    if (zones)
        for (cSC4Lot* lot : zones->Lots()) {
            if (!lot) continue;
            ++states[lot->GetState() & 7];
            if (lot->GetState() == kLotStateAbandoned) ++abandonedByReason[lot->GetVacateReason() & 7];
            if (lot->GetState() == kLotStateLivable) {
                const BuildingProfile* building = NativeBuildingProfile(lot->GetBuildingType(false));
                if (building && building->purpose) {
                    auto& count = livable[NativeDeveloperType(building->purpose, lot->GetOccupantWealth())];
                    ++count.first;
                    if (lot->GetDaysSinceLastStateChange() > 0x3b) ++count.second;
                }
            }
        }
    std::printf("decline: lots unbuilt %d, construction %d, occupied %d, livable %d, abandoned %d (reasons", states[0], states[1], states[2],
                states[3], states[4]);
    for (int reason = 0; reason < 8; ++reason)
        if (abandonedByReason[reason]) std::printf(" %d:%d", reason, abandonedByReason[reason]);
    std::printf("), reserved %d, demolishable %d\n", states[5], states[6]);
    if (zones) {   // where the first abandoned lots are (cells), for kits that look at them
        int shown = 0;
        for (cSC4Lot* lot : zones->Lots()) {
            if (!lot || lot->GetState() != kLotStateAbandoned || shown >= 5) continue;
            int x = 0, z = 0;
            lot->GetLocation(x, z);
            std::printf("decline: abandoned lot at %d,%d condition %u\n", x, z, lot->GetCondition());
            ++shown;
        }
    }
    std::printf("decline: livable lots by developer (type:lots/past 59 days)");
    for (const auto& [type, count] : livable) std::printf(" %x:%d/%d", type, count.first, count.second);
    std::printf("\n");
    std::printf("decline: tract developer abandoned %d, reoccupied %d, constructions %d\n", g_lotDeveloper.abandoned,
                g_lotDeveloper.reoccupied, g_lotDeveloper.constructions);
    std::printf("decline: plan outcomes executed %d, capacity declined %d\n", g_declineOutcomes, g_declinedCapacity);
    // The inputs the growth predicates read: each map, how many of its tracts are non-zero, and the tunables in use.
    auto coverage = [](auto* grid) {
        if (!grid) return std::string("missing");
        int set = 0;
        for (int tx = 0; tx < grid->GetTractCountX(); ++tx)
            for (int tz = 0; tz < grid->GetTractCountZ(); ++tz)
                if (grid->GetCellValue(tx << grid->GetTractShift(), tz << grid->GetTractShift()) != 0) ++set;
        return std::to_string(set) + "/" + std::to_string(grid->GetTractCountX() * grid->GetTractCountZ());
    };
    std::printf("decline: inputs land value %s, fire protection %s, park %s, trip length %s (non-zero tracts)\n",
                coverage(LandValueMap()).c_str(), coverage(FireProtectionMap()).c_str(), coverage(ParkMap()).c_str(),
                coverage(NativeTrafficTripLengthMap()).c_str());
    if (auto* fire = Simulator<cSC4FireProtectionSimulator>(sc4::CityComponent::FireProtectionSimulator))
        std::printf("decline: fire protection stations %zu (file %zu), unadjusted %s\n", fire->Protection().Stations().size(),
                    fire->Protection().FileStationCount(), coverage(fire->Protection().UnadjustedGrid()).c_str());
    TractTunables tunables;
    const bool read = ReadTractTunables(tunables);
    std::printf("decline: tunables %s: capacity %.2f, take-over %.2f/%.2f, weights %.2f/%.2f, penalties %.1f/%.1f, take over %d, "
                "styles %zu, years %d\n", read ? "from the exemplar" : "DEFAULTS (no exemplar)", tunables.capacityFactor,
                tunables.declineFactor, tunables.buildBias, tunables.existingCapacityWeight, tunables.abandonedCapacityWeight,
                tunables.aggregationPenalty, tunables.subdivisionPenalty, tunables.allowTakeOver ? 1 : 0, tunables.styles.size(),
                tunables.hasYearsBetweenStyleChanges ? tunables.yearsBetweenStyleChanges : -1);
    if (cSC4LotDeveloper* lots = LotDeveloper())
        std::printf("decline: lot developer condition threshold %u days, ratios %.2f/%.2f/%.2f\n", lots->Tunables().conditionThreshold,
                    lots->Tunables().ratioA, lots->Tunables().ratioB, lots->Tunables().ratioC);
    std::printf("decline: developers (type:growth/decline lists, demand)%s\n", GrowthDevelopersSummary().c_str());
}

void InstallTractDeveloperHost() { sc4::SetTractDeveloperHost(&g_host); }

}  // namespace native
