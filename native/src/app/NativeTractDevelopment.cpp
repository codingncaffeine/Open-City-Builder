// The tract developer on the native application (notes/GROWTH_PLAN.md layer 3). Replaces the generated neutral ITractDeveloperHost:
// the city, zone manager, lots (through the zone developer, the port's lot map), lot configurations, building catalogue,
// construction resources and network occupants are the city's, so cSC4TractDeveloper::Grow searches real lots and real lot
// configurations and picks real buildings. Its last step, the lot developer's StartConstruction (layer 4, not recovered), is
// reported, not performed.
//
// Neutral here (each named, PARTIAL): the tunables (the class's defaults stand); the abandon / reoccupy calls; the services the
// port does not run natively answer "provided" so that the growth-stage caps do not hold growth back: power (2), water, fire
// protection, park effect, the land value (0: inside every range test the profile skips) and the residential trip-length map
// (the lot's own road access stands in); the lot developer's NeedsAnchorBuilding (false); DemolishLots (an empty lot has nothing
// to demolish; a built one is reported).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <unordered_set>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeGrowthDevelopers.h"
#include "platform/Unwired.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZRandom.h"
#include "sc4/CommonIIDs.h"
#include "sc4/GrowthDeveloper.h"
#include "sc4/Exemplar.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
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
    bool NeedsAnchorBuilding(const std::int32_t*, std::int32_t) override { return false; }   // PARTIAL: cSC4LotDeveloper vslot 0x120
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
} g_lotManager;

// SL::spLotDeveloper: layer 4. StartConstruction is where a building would begin.
struct LotDeveloperApi final : ITractLotDeveloperApi {
    void StartConstruction(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t buildingType, bool flag) override {
        ++constructions;
        if (std::getenv("SC4_SIMDEBUG")) {
            int x = 0, z = 0;
            if (lot) lot->GetLocation(x, z);
            std::fprintf(stderr, "tract developer: StartConstruction lot (%d,%d) configuration %08x building %08x%s\n", x, z,
                         configuration ? configuration->GetID() : 0, buildingType, flag ? " (keep)" : "");
        }
        SC4_UNWIRED("ITractLotDeveloperApi::StartConstruction (cSC4LotDeveloper construction, growth layer 4)");
    }
    void AbandonBuilding(cSC4Lot*, int) override { SC4_UNWIRED("ITractLotDeveloperApi::AbandonBuilding (layer 4)"); }
    void ReoccupyBuilding(cSC4Lot*, std::uint32_t) override { SC4_UNWIRED("ITractLotDeveloperApi::ReoccupyBuilding (layer 4)"); }
    float GetAreaSlope(const std::int32_t rect[4], bool includeWater) override { return NativeAreaSlope(rect, includeWater); }
    int constructions = 0;
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
    void ReadTunables(TractTunables&) override {}   // PARTIAL: the class's defaults stand (the tunable exemplar is not read here)
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
    std::uint8_t LandValueAt(int, int) override { return 0; }          // PARTIAL: the land value simulator is not run natively
    bool IsCellWatered(int, int) override { return true; }             // PARTIAL: no plumbing simulator natively
    std::uint8_t PoweredAt(int, int) override { return 2; }            // PARTIAL: no power simulator natively
    bool FireProtectedAt(int, int) override { return true; }           // PARTIAL: no fire protection simulator natively
    std::int16_t ParkEffectAt(int, int) override { return 1; }         // PARTIAL: no aura simulator natively
    bool HasTripLengthAt(int x, int z) override {                      // PARTIAL: no traffic simulator; the lot's road access stands in
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        cSC4Lot* lot = d ? d->GetLot(x, z, false) : nullptr;
        return lot && lot->HasRoadAccess();
    }
    void DemolishLots(const std::list<cRZAutoRefCount<cSC4Lot>>& lots, std::uint32_t) override {
        for (const auto& lot : lots)
            if (lot && lot->GetBuildingType(false) != 0) SC4_UNWIRED("ITractDeveloperHost::DemolishLots (built lot; no demolition simulator natively)");
    }
    void BuildingExemplarBool(const cGZPersistResourceKey& key, std::uint32_t property, bool& value) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), property, value);
        exemplar->Release();
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

bool NativeLotConfigurationsByFilter(std::vector<sc4::cSC4LotConfiguration*>& out, sc4::cISC4LotConfigurationFilter* filter) {
    sc4::cSC4LotConfigurationManager* manager = LotConfigurations();
    if (!manager) return false;
    sc4::cSC4LotConfigurationManager::ConfigurationSet set;
    manager->GetLotConfigurationsByFilter(set, filter);
    out.assign(set.begin(), set.end());
    return !out.empty();
}

void InstallTractDeveloperHost() { sc4::SetTractDeveloperHost(&g_host); }

}  // namespace native
