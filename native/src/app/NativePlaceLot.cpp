// The civic building menus and the place-lot tool's services on the native application.
#include "app/NativePlaceLot.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "gz/Buffer.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CatalogItem.h"
#include "sc4/CivicBuildingSimulator.h"
#include "sc4/CommonIIDs.h"
#include "sc4/Exemplar.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/SC4UI.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SLBuffer.h"
#include "sc4/SLString.h"
#include "sc4/ServiceLocator.h"
#include "sc4/StringDetokenizer.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"
#include "sc4/BudgetSimulator.h"
#include "s3d/ModelInstance.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/Lot.h"
#include "sc4/LotDeveloper.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/OverlayManager.h"
#include "sc4/FloraOccupant.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/VisualEffect.h"
#include "sc4/ViewInputControlPlaceLot.h"
#include "sc4/ZoneDeveloper.h"
#include "app/NativeAudio.h"
#include "app/NativeDecalOverlay.h"
#include "app/NativeEffects.h"
#include "app/NativeModelMaker.h"
#include "gz/cGZPersistResourceKey.h"

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
// A component wrapped around a plain class (no GZCOM interfaces of its own).
template <class T> T* Plain(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}

constexpr std::uint32_t kPropOccupantGroups = 0xaa1dd396;
constexpr std::uint32_t kPropLotConfiguration = 0xea260589;   // the lot the building is plopped on
constexpr std::uint32_t kPropUserVisibleName = 0x8a416a99;
constexpr std::uint32_t kPropItemName = 0x899afbad;
constexpr std::uint32_t kPropItemDescriptionKey = 0xca416ab5;
constexpr std::uint32_t kPropItemDescription = 0x8a2602a9;
constexpr std::uint32_t kPropExemplarName = 0x20;
constexpr std::uint32_t kPropItemCost = 0x49cac341;
constexpr std::uint32_t kPropConditional = 0xea3209f8;        // a reward: shown only while its status allows it
constexpr std::uint32_t kPropItemIcon = 0x8a2602b8;
constexpr std::uint32_t kPropItemOrder = 0x8a2602b9;
constexpr std::uint32_t kPropFerryType = 0x0bd86fd3;
constexpr std::uint32_t kItemTypeLot = 0x6a2b245f;

bool AnyIn(const std::vector<std::uint32_t>& values, const std::set<std::uint32_t>& set) {
    for (std::uint32_t v : values) if (set.count(v)) return true;
    return false;
}

}  // namespace

void AddBuildingsToItemList(const std::set<std::uint32_t>& include, const std::set<std::uint32_t>& exclude, bool rewardsUnlocked,
                            std::multimap<int, cISC4CatalogItem*>& items) {
    auto* civic = Component<cSC4CivicBuildingSimulator>(CityComponent::CivicBuildingSimulator, kGZCLSID_cSC4CivicBuildingSimulator);
    auto* lots = Plain<cSC4LotConfigurationManager>(CityComponent::LotConfigurationManager);
    auto* traffic = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator);
    cIGZPersistResourceManager* manager = SL::RM();
    if (!manager) return;
    cSC4View3DWin* view = CityView3DWin();
    cIGZWin* window = view ? static_cast<cIGZWin*>(static_cast<cGZWin*>(view)) : nullptr;

    auto* detokenizer = new cSCStringDetokenizer();
    detokenizer->AddRef();
    std::vector<std::uint32_t> types;
    NativeAllBuildingTypes(types);
    for (std::uint32_t type : types) {
        const BuildingProfile* profile = NativeBuildingProfile(type);
        if (!profile || (profile->categoryFlags & 7) != 0) continue;   // R / C / I buildings grow, they are not plopped
        cGZPersistResourceKey key{};
        if (!NativeBuildingKeyFromType(type, key)) continue;
        cISCResExemplar* exemplar = nullptr;
        if (!manager->GetResource(key, kGZIID_cISCResExemplar, reinterpret_cast<void**>(&exemplar), 0, nullptr) || !exemplar) continue;
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        // Shown when one occupant group is included and none is excluded.
        std::vector<std::uint32_t> groups;
        const bool listed = cSCPropertyHelp::GetPropertyValue(holder, kPropOccupantGroups, groups) && AnyIn(groups, include) &&
                            !AnyIn(groups, exclude);
        std::uint32_t lot = 0;
        if (listed) cSCPropertyHelp::GetPropertyValue(holder, kPropLotConfiguration, lot);
        if (lot != 0 && (!lots || !lots->GetLotConfiguration(lot))) lot = 0;
        if (lot == 0) {
            exemplar->Release();
            continue;
        }
        cRZString name, description;
        detokenizer->AddUnknownTokenReplacementMethod(&nSC4UI::PropertyTokenReplacementFunction, holder, true);
        if (!nSC4UI::Host()->LoadExemplarString(name, holder, kPropUserVisibleName, detokenizer) &&
            !cSCPropertyHelp::GetPropertyValue(holder, kPropItemName, name))
            cSCPropertyHelp::GetPropertyValue(holder, kPropExemplarName, name);
        if (!nSC4UI::Host()->LoadExemplarString(description, holder, kPropItemDescriptionKey, detokenizer) &&
            !cSCPropertyHelp::GetPropertyValue(holder, kPropItemDescription, description))
            cSCPropertyHelp::GetPropertyValue(holder, kPropExemplarName, description);
        detokenizer->AddUnknownTokenReplacementMethod(&nSC4UI::PropertyTokenReplacementFunction, holder, false);
        if (name.Strlen() == 0 || description.Strlen() == 0) {
            exemplar->Release();
            continue;
        }
        cGZPersistResourceKey own{};
        exemplar->GetKey(own);
        std::uint32_t cost = 0, icon = 0x144161ec, exclusion = 0;
        bool conditional = false;
        cSCPropertyHelp::GetPropertyValue(holder, kPropItemCost, cost);
        cSCPropertyHelp::GetPropertyValue(holder, kPropConditional, conditional);
        cSCPropertyHelp::GetPropertyValue(holder, kPropItemIcon, icon);

        // A unique building that already stands is greyed out with the reason in its tooltip.
        bool enabled = true;
        if (cSCPropertyHelp::GetPropertyValue(holder, kCivicPropertyExclusionId, exclusion) && civic && civic->IsBuildingExcluded(exclusion)) {
            description.AppendChars("\n\n", 2);
            cSLString reason(cGZPersistResourceKey{0x2026960b, 0x6a231eaa, 0x0a765619}, true);
            description.AppendString(reason);
            enabled = false;
        }
        bool shown = true;
        bool ferryCheck = enabled;
        if (conditional && enabled) {
            // A reward: its conditional status (by exemplar instance) says whether it is offered and whether it is hidden.
            cISC4CivicBuildingSimulator::ConditionalBuildingStatus* status = civic ? civic->GetConditionalBuildingStatus(own.instance) : nullptr;
            if (!status) {
                enabled = false;
            } else if (status->state & 2) {
                enabled = true;
            } else {
                description.AppendChars("\n\n", 2);
                description.AppendString(status->text);
                enabled = false;
            }
            shown = !status || (status->state & 1) == 0;
            ferryCheck = enabled;
            if (rewardsUnlocked) shown = ferryCheck = true;
        } else if (conditional) {
            ferryCheck = false;   // an excluded reward is listed, greyed out
        }
        if (ferryCheck) {
            // A ferry terminal is offered while fewer than 16 of its type stand.
            std::uint8_t ferry = 0;
            enabled = !(holder->HasProperty(kPropFerryType) && cSCPropertyHelp::GetPropertyValue(holder, kPropFerryType, ferry) && traffic &&
                        ferry < 3 && traffic->GetFerryTerminalCount(ferry) > 15);
        }
        if (shown) {
            auto* item = new cSC4CatalogItem();
            item->AddRef();
            cSLBuffer bitmap(cGZPersistResourceKey{0x856ddbac, 0x6a386d26, icon}, 1, false);
            if (cIGZBuffer* buffer = bitmap.GetBuffer()) {
                buffer->SetTransparency(window ? window->MakeFillColor(0xff, 0, 0xff) : 0);
                item->SetIcon(buffer);
            }
            std::uint32_t order = 0;
            cSCPropertyHelp::GetPropertyValue(holder, kPropItemOrder, order);
            item->SetName(name);
            item->SetDescription(description);
            item->SetCost(cost);
            item->SetItemID(lot);
            item->SetItemType(kItemTypeLot);
            item->SetEnabled(enabled);
            items.emplace(static_cast<int>(order * 0x10000 + cost), item);
        }
        exemplar->Release();
    }
    detokenizer->Release();
}

// ---- the place-lot tool's host -----------------------------------------------------------------------------------------------
namespace {

cSC4View3DWin* g_view = nullptr;

cSC4ZoneDeveloper* ZoneDeveloper() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4ZoneManager* Zones() { return Component<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4LotDeveloper* LotDeveloper() { return Component<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4BudgetSimulator* Budget() { return Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSTETerrain* TerrainOf() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
bool CellIsWater(int x, int z) {
    cSTETerrain* t = TerrainOf();
    return t && t->IsWaterVertex(x, z) && t->IsWaterVertex(x + 1, z) && t->IsWaterVertex(x, z + 1) && t->IsWaterVertex(x + 1, z + 1);
}
// The building exemplar of a building type (borrowed for the call).
template <class F> bool WithBuildingExemplar(std::uint32_t type, F&& fn) {
    cGZPersistResourceKey key{};
    cIGZPersistResourceManager* manager = SL::RM();
    if (!manager || !NativeBuildingKeyFromType(type, key)) return false;
    cISCResExemplar* exemplar = nullptr;
    if (!manager->GetResource(key, kGZIID_cISCResExemplar, reinterpret_cast<void**>(&exemplar), 0, nullptr) || !exemplar) return false;
    const bool result = fn(exemplar->AsISCPropertyHolder());
    exemplar->Release();
    return result;
}
bool PlaceLotDebug() { static const bool on = std::getenv("SC4_PLOPDEBUG") != nullptr; return on; }

// A visual effect of the effects manager as the tool holds it.
class Effect final : public IPlaceLotEffect {
public:
    explicit Effect(cSC4VisualEffect* effect) : effect_(effect) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        effect_->Release();
        delete this;
        return 0;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    void SetEffectTransform(const cS3DTransform& transform) override {
        // The tool's transform carries the world position: the effect is placed by its source transform (as StartNamedEffect).
        effect_->SetSourceTransform(transform);
    }
    void SetFinishMessage(std::uint32_t message, std::uint32_t data) override { effect_->SetFinishNotifyMessage(message, data); }
    void SetVectorParams(std::uint32_t type, const float* values, int count) override {
        std::vector<cS3DVector3> vectors(static_cast<std::size_t>((count + 2) / 3));
        for (int i = 0; i < count; ++i) {
            cS3DVector3& v = vectors[static_cast<std::size_t>(i / 3)];
            (i % 3 == 0 ? v.x : i % 3 == 1 ? v.y : v.z) = values[i];
        }
        effect_->SetVectorParams(type, vectors.data(), static_cast<int>(vectors.size()));
    }
    void SetVoidParams(std::uint32_t type, void** values, int count) override { effect_->SetVoidParams(type, values, count); }

private:
    cSC4VisualEffect* effect_;
    std::uint32_t refs_ = 1;
};
struct Effects final : IPlaceLotEffects {
    IPlaceLotEffect* Create(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        if (PlaceLotDebug()) std::fprintf(stderr, "place lot: effect %s -> %s\n", name, effect ? "made" : "none");
        if (!effect) return nullptr;
        return new Effect(effect);
    }
} g_effects;

class View final : public IPlaceLotView {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void EndInput(int) override { if (g_view) g_view->RemoveCurrentViewInputControl(false); }
    cSC4ViewInputControl* TopControl() override { return g_view ? g_view->GetCurrentViewInputControl() : nullptr; }
    bool PickTerrain(int x, int y, float out[3], bool flag) override { return g_view && g_view->PickTerrain(x, y, out, flag); }
    cIGZWinKeyAccelerator* KeyAccelerator() override { return g_view ? g_view->GetKeyAccelerator() : nullptr; }
    void SetCursorText(std::uint32_t id, std::uint32_t arg2, std::uint32_t, const cIGZString* text, int flags) override {
        if (g_view) g_view->SetCursorText(id, arg2, nullptr, text, flags);
    }
    void ClearCursorText(std::uint32_t id) override { if (g_view) g_view->ClearCursorText(id); }
    void SetCostIndicator(std::uint32_t cost) override { if (g_view) g_view->SetCostIndicator(cost); }
    void ClearCostIndicator(bool force) override { if (g_view) g_view->ClearCostIndicator(force); }
    IPlaceLotCoverageHelper* CoverageHelper() override { return nullptr; }   // cSC4CoverageRadiusEffectHelper is not recovered
    std::uint32_t LastSelectedBuilding() override { return lastBuilding_; }
    std::uint32_t LastSelectedLot() override { return lastLot_; }
    void SetLastSelectedBuilding(std::uint32_t type) override { lastBuilding_ = type; }
    void SetLastSelectedLot(std::uint32_t id) override { lastLot_ = id; }
    void AddGlobalEffect(std::uint32_t) override {}      // the view manager's data views are not ported
    void RemoveGlobalEffect(std::uint32_t) override {}

private:
    std::uint32_t lastBuilding_ = 0, lastLot_ = 0;
} g_viewAdapter;

struct TerrainQuery final : IPlaceLotTerrainQuery {
    bool IsWater(float x, float z) override { return CellIsWater(static_cast<int>(x / 16.0f), static_cast<int>(z / 16.0f)); }
} g_terrainQuery;
struct Terrain final : IPlaceLotTerrain {
    float CellSize() override { return 16.0f; }
    int CellCountX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CellCountZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    IPlaceLotTerrainQuery* Query() override { return &g_terrainQuery; }
} g_terrain;

// The traffic network map's cell: the network occupant on the cell that carries one of the asked flags.
struct NetworkCell final : IPlaceLotNetworkCell {
    cSC4NetworkOccupant* network = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool HasAnyFlag(std::uint32_t flags) override { return network->HasAnyNetworkFlag(flags); }
    std::uint32_t Flags() override { return network->GetNetworkFlag(); }
    std::uint32_t EdgeTypes(std::uint32_t type) override { return network->GetEdgeTypesFlag(static_cast<std::uint8_t>(type)); }
    std::uint8_t EdgeFlags() override { return 0; }   // vslot 0x140 is only read by the toll-booth path
} g_networkCell;
struct NetworkMap final : IPlaceLotNetworkMap {
    IPlaceLotNetworkCell* GetCell(int x, int z, std::uint32_t mask, int) override {
        cISC4OccupantManager* m = Occupants();
        if (!m) return nullptr;
        OccupantList list;
        const int cell[2] = {x, z};
        if (!m->GetOccupantsByStandardCityCells(list, cell, cell, 0xc772bf98u, 0xffffffffu)) return nullptr;
        cSC4NetworkOccupant* found = nullptr;
        for (sc4::cISC4Occupant* occupant : list) {
            auto* network = dynamic_cast<cSC4NetworkOccupant*>(occupant);
            if (!found && network && network->HasAnyNetworkFlag(mask)) found = network;
        }
        for (sc4::cISC4Occupant* occupant : list) occupant->Release();   // the city keeps the occupant alive
        if (!found) return nullptr;
        g_networkCell.network = found;
        return &g_networkCell;
    }
} g_networkMap;

struct City final : IPlaceLotCity {
    void PositionToCell(float x, float z, int& cx, int& cz) override { if (cSC4City* c = HostCity()) c->PositionToCell(x, z, cx, cz); }
    void CellToPosition(int cx, int cz, float& x, float& z) override { x = static_cast<float>(cx) * 16.0f; z = static_cast<float>(cz) * 16.0f; }
    IPlaceLotNetworkMap* NetworkMap() override { return &g_networkMap; }
    IPlaceLotMission* Mission() override { return nullptr; }   // the tutorial system is not ported
} g_city;

struct LotDeveloperView final : IPlaceLotLotDeveloper {
    std::int64_t LotPrice(const SC4Rect<int>& rect) override {
        cSC4LotDeveloper* d = LotDeveloper();
        return d ? d->GetLotDevelopmentCost(LotCellRect{rect.x0, rect.z0, rect.x1, rect.z1}) : 0;
    }
    bool ConstructDefaultLot(cSC4Lot* lot, cSC4LotConfiguration* configuration, std::uint32_t type, bool construct) override {
        cSC4LotDeveloper* d = LotDeveloper();
        const bool made = d && d->ConstructDefaultLot(lot, configuration, type, construct);
        if (PlaceLotDebug()) std::fprintf(stderr, "place lot: ConstructDefaultLot(%08x, type %08x, construct %d) -> %d\n",
                                          configuration ? configuration->GetID() : 0u, type, construct, made);
        return made;
    }
} g_lotDeveloperView;

// The place-lot preview as the coverage queries see it: a station / school / hospital that is not in the city yet.
class PreviewStation final : public IPoliceOccupant, public IFireOccupant, public IResidentialOccupant {
public:
    PreviewStation(cISCPropertyHolder* holder, std::uint32_t type) : holder_(holder), type_(type) {}
    std::uint32_t OccupantType() override { return kOccupantTypeBuilding; }
    void Position(float xyz[3]) override { xyz[0] = xyz[1] = xyz[2] = 0.0f; }
    cISCPropertyHolder* PropertyHolder() override { return holder_; }
    bool UsesDepartmentBudget() override { return false; }
    bool IsInGroup(std::uint32_t group) override {
        std::vector<std::uint32_t> groups;
        return cSCPropertyHelp::GetPropertyValue(holder_, kPropOccupantGroups, groups) && std::find(groups.begin(), groups.end(), group) != groups.end();
    }
    std::uint32_t BuildingType() override { return type_; }
private:
    cISCPropertyHolder* holder_;
    std::uint32_t type_;
};

struct BuildingDevelopment final : IPlaceLotBuildingDevelopment {
    void GetAllBuildingTypes(std::vector<std::uint32_t>& out) override { NativeAllBuildingTypes(out); }
    bool BuildingTypeName(std::uint32_t type, cIGZString& out) override {
        return WithBuildingExemplar(type, [&](cISCPropertyHolder* h) { return cSCPropertyHelp::GetPropertyValue(h, 0x20, out); });
    }
    std::int64_t BuildingPrice(std::uint32_t type) override {
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? profile->cost : -100000;
    }
    IPlaceLotLotDeveloper* LotDeveloper() override { return &g_lotDeveloperView; }
    bool ResolveBuildingType(const std::vector<std::uint32_t>& ids, std::uint32_t& out) override {
        out = cSC4LotDeveloper::FirstBuildingTypeOf(ids, NativeIsBuildingType,
                                                   [](std::vector<std::uint32_t>& members, std::uint32_t family) { NativeBuildingsInFamily(family, members); });
        return out != 0;
    }
    bool BuildingIsPoliceStation(std::uint32_t type) override {
        return WithBuildingExemplar(type, [](cISCPropertyHolder* h) { return h->HasProperty(kPoliceStationRadius); });
    }
    bool BuildingIsFireStation(std::uint32_t type) override {
        return WithBuildingExemplar(type, [](cISCPropertyHolder* h) { return h->HasProperty(kFireStationRadius); });
    }
    // spPoliceSim / spFireProtectionSim / spResidentialSim GetOccupantCoverage(preview occupant, fraction, &radius, &ringRadius),
    // with the building exemplar standing in for the preview occupant's property holder (its budget item is not locally funded yet:
    // the department's funding applies).
    bool BuildingCoverageRadius(std::uint32_t type, int category, float fraction, float& radius, float& ringRadius) override {
        return WithBuildingExemplar(type, [&](cISCPropertyHolder* holder) {
            PreviewStation station(holder, type);
            const SC4Percentage share(fraction);
            bool ok = false;
            if (category == 0) {
                auto* police = Component<cSC4PoliceSimulator>(CityComponent::PoliceSimulator, kGZCLSID_cSC4PoliceSimulator);
                ok = police && police->GetOccupantCoverage(&station, share, radius, ringRadius);
            } else if (category == 1) {
                auto* fire = Component<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator, kGZCLSID_cSC4FireProtectionSimulator);
                ok = fire && fire->GetOccupantCoverage(&station, share, radius, ringRadius);
            } else {
                auto* residential = Component<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator, kGZCLSID_cSC4ResidentialSimulator);
                ok = residential && residential->GetOccupantCoverage(&station, fraction, radius, ringRadius);
            }
            if (PlaceLotDebug()) std::fprintf(stderr, "place lot: coverage of %08x category %d fraction %.2f -> %d radius %.1f ring %.1f\n", type, category, fraction, ok, radius, ringRadius);
            return ok;
        });
    }
} g_buildingDevelopment;

struct BudgetView final : IPlaceLotBudget {
    std::int64_t Funds() override { cSC4BudgetSimulator* b = Budget(); return b ? b->GetTotalFunds() : 0; }
    void Deposit(std::int64_t amount) override { if (cSC4BudgetSimulator* b = Budget()) b->DepositFunds(amount); }
    void Withdraw(std::int64_t amount) override { if (cSC4BudgetSimulator* b = Budget()) b->WithdrawFunds(amount); }
} g_budget;

struct LotManager final : IPlaceLotLotManager {
    int CanCreatePloppedLot(const SC4Rect<int>& rect, int facing, cSC4LotConfiguration* configuration) override {
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        const int code = d && configuration ? d->CanCreatePloppedLot(rect, facing, configuration) : 0;
        if (PlaceLotDebug()) std::fprintf(stderr, "place lot: CanCreatePloppedLot(%d,%d..%d,%d facing %d) -> %d\n", rect.x0, rect.z0, rect.x1, rect.z1, facing, code);
        return code;
    }
    bool CreateLotIfPossible(int x, int z, int sizeX, int sizeZ, int facing, bool arg7, bool arg8, cSC4LotConfiguration* configuration,
                             cSC4Lot*& out) override {
        out = nullptr;
        cSC4ZoneDeveloper* d = ZoneDeveloper();
        const bool made = d && d->CreateLotIfPossible(x, z, sizeX, sizeZ, facing, out, arg7, arg8, configuration);
        if (PlaceLotDebug()) std::fprintf(stderr, "place lot: CreateLotIfPossible(%d,%d %dx%d facing %d) -> %d\n", x, z, sizeX, sizeZ, facing, made);
        return made;
    }
} g_lotManager;

struct ZoneManager final : IPlaceLotZoneManager {
    bool PlaceZone(const SC4Rect<int>& rect, int zoneType, bool a2, bool a3, bool a4, bool a5, bool a6) override {
        cSC4ZoneManager* m = Zones();
        if (!m) return false;
        SC4CellRegion<int> region = SC4CellRegion<int>::Make(rect, true);
        const bool placed = m->PlaceZone(region, zoneType, a2, a3, a4, a5, a6, nullptr, nullptr, nullptr);
        if (PlaceLotDebug()) std::fprintf(stderr, "place lot: PlaceZone(%d,%d..%d,%d zone %d) -> %d\n", rect.x0, rect.z0, rect.x1, rect.z1, zoneType, placed);
        return placed;
    }
} g_zoneManager;

// SL::spDemolition DemolishRegion. INTERIM: cSC4Demolition has no native port. The dry runs (flags with 0x800) answer yes at no
// cost; the real one removes the lots (their buildings with them) and the flora on the cells.
struct Demolition final : IPlaceLotDemolition {
    bool DemolishRegion(const SC4Rect<int>& rect, std::uint32_t flags, std::int64_t* cost) override {
        if (cost) *cost = 0;
        if (flags & 0x800) return true;
        cSC4ZoneDeveloper* developer = ZoneDeveloper();
        cISC4OccupantManager* occupants = Occupants();
        for (int x = rect.x0; x <= rect.x1; ++x) {
            for (int z = rect.z0; z <= rect.z1; ++z) {
                if (developer)
                    if (cSC4Lot* lot = developer->GetLot(x, z, false)) developer->DeleteLot(lot);
                for (int guard = 0; occupants && guard < 16; ++guard) {
                    sc4::cISC4Occupant* occupant = nullptr;
                    if (!occupants->GetFirstOccupantByStandardCityCell(occupant, x, z, kOccupantTypeFlora) || !occupant) break;
                    occupants->RemoveOccupant(occupant, true, 0);
                    occupant->Release();
                }
            }
        }
        return true;
    }
} g_demolition;

// The preview building (the original's prop manager CreateProp(building profile)): a building occupant that never enters the
// occupant manager, so no simulator sees it; the model maker makes its models and they are drawn with the tool's attributes.
class PreviewBuilding final : public IPlaceLotProp {
public:
    explicit PreviewBuilding(std::uint32_t type) : occupant_(new cSC4BuildingOccupant()) {
        occupant_->AddRef();
        occupant_->SetBuildingType(type);
        cGZPersistResourceKey key{};
        cISCResExemplar* exemplar = nullptr;
        if (SL::RM() && NativeBuildingKeyFromType(type, key) &&
            SL::RM()->GetResource(key, kGZIID_cISCResExemplar, reinterpret_cast<void**>(&exemplar), 0, nullptr) && exemplar) {
            occupant_->SetDefaultExemplar(exemplar);
            std::vector<float> size;
            if (cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x27812810, size) && size.size() >= 3) {
                const float origin[3] = {0.0f, 0.0f, 0.0f};
                occupant_->SetBoundingBox(origin, size.data());
            }
            exemplar->Release();
        }
    }
    ~PreviewBuilding() {
        Hide();
        occupant_->Release();
    }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    int Orientation() override { return occupant_->GetOrientation(); }
    void SetOrientation(int orientation) override { occupant_->SetOrientation(orientation); }
    bool IsPlaced() override { return shown_; }
    bool ShowInPreview() override { return true; }
    void Show(const cS3DVector3& position, const s3d::SC4DrawAttributes& attributes) override {
        // Rebuilt at every change: the model maker places a building's models from the occupant when it makes them.
        Hide();
        const float xyz[3] = {position.x, position.y, position.z};
        occupant_->SetPosition(xyz);
        if (!ShowPreviewOccupant(occupant_, true)) return;
        shown_ = true;
        cISC43DPlaceableObject* placeable = occupant_->GetPlaceableObject();
        if (PlaceLotDebug()) std::fprintf(stderr, "place lot: preview building %08x at %.0f,%.0f: %d models\n", occupant_->GetBuildingType(),
                                          position.x, position.z, placeable ? placeable->GetModelCount() : -1);
        if (placeable) {
            std::uint32_t count = static_cast<std::uint32_t>(placeable->GetModelCount());
            std::vector<cIS3DModelInstance*> models(count);
            placeable->GetModels(models.data(), count);
            for (std::uint32_t i = 0; i < count; ++i)
                reinterpret_cast<s3d::cIS3DModelInstance*>(models[i])->SetDrawAttributes(attributes);
            if (placeable->IsVisible()) {   // the renderer reads the translucency bits when a model goes in
                placeable->SetVisibility(false);
                placeable->SetVisibility(true);
            }
        }
    }
    void Hide() override {
        if (!shown_) return;
        ShowPreviewOccupant(occupant_, false);
        shown_ = false;
    }

private:
    cSC4BuildingOccupant* occupant_;
    std::uint32_t refs_ = 1;
    bool shown_ = false;
};
struct PropManager final : IPlaceLotPropManager {
    bool PropTypeExists(std::uint32_t) override { return false; }   // PARTIAL: the lot's preview props
    IPlaceLotProp* CreateProp(std::uint32_t) override { return nullptr; }
    IPlaceLotProp* CreateBuildingProp(std::uint32_t type) override { return new PreviewBuilding(type); }
} g_propManager;

class Host final : public IPlaceLotHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    IPlaceLotView* View() override { return &g_viewAdapter; }
    IPlaceLotTerrain* Terrain() override { return &g_terrain; }
    IPlaceLotCity* City() override { return &g_city; }
    IPlaceLotNetworkMap* NetworkMap() override { return &g_networkMap; }
    IPlaceLotBuildingDevelopment* BuildingDevelopment() override { return &g_buildingDevelopment; }
    IPlaceLotBudget* Budget() override { return &g_budget; }
    IPlaceLotLotManager* LotManager() override { return &g_lotManager; }
    IPlaceLotZoneManager* ZoneManager() override { return &g_zoneManager; }
    IPlaceLotDemolition* Demolition() override { return &g_demolition; }
    IPlaceLotPropManager* PropManager() override { return &g_propManager; }
    IPlaceLotEffects* Effects() override { return &g_effects; }
    IPlaceLotLocalGrid* CreateLocalGrid() override { return nullptr; }   // cSC4LocalGridHelper is not recovered
    void* CreateFootprintTexture(int size, int marginX, int marginZ) override { return native::CreateFootprintTexture(size, marginX, marginZ); }
    void ReleaseFootprintTexture(void* texture) override { static_cast<sc4::IOverlayTexture*>(texture)->Release(); }
    cSC4LotConfiguration* GetLotConfiguration(std::uint32_t id) override {
        cSC4LotConfigurationManager* m = Plain<cSC4LotConfigurationManager>(CityComponent::LotConfigurationManager);
        return m ? m->GetLotConfiguration(id) : nullptr;
    }
    int GetLotConfigurationIDsByFilter(std::unordered_set<std::uint32_t>&, IPlaceLotFilter*) override { return 0; }   // the LotPlop cheat only
    IPlaceLotFilter* CreatePlopFilter(bool, std::uint32_t) override { return nullptr; }
    void GetObjectIDArray(const cSC4LotConfigurationObject* object, std::vector<std::uint32_t>& out) override {
        cSC4LotConfigurationManager* m = Plain<cSC4LotConfigurationManager>(CityComponent::LotConfigurationManager);
        if (m && object) m->GetObjectIDArray(object, out);
    }
    void LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        cSLString text(cGZPersistResourceKey{0x2026960b, group, instance}, true);
        out.Copy(text);
    }
    void PlaySound(std::uint32_t, std::uint32_t id) override { PlayGameSound(id); }
    void ConnectLotUtilities(cSC4Lot*, const cS3DVector3&) override {}   // PARTIAL: pipe / subway connection occupants
    void MouseMoved() override {}
    bool BuildingPropertyBool(std::uint32_t type, std::uint32_t property) override {
        return WithBuildingExemplar(type, [&](cISCPropertyHolder* h) {
            bool value = false;
            if (cSCPropertyHelp::GetPropertyValue(h, property, value)) return value;
            std::uint32_t number = 0;   // a numeric property (a coverage capacity, an exclusion id) counts when non-zero
            return cSCPropertyHelp::GetPropertyValue(h, property, number) && number != 0;
        });
    }
    bool BuildingPropertyUint32(std::uint32_t type, std::uint32_t property, std::uint32_t& out) override {
        return WithBuildingExemplar(type, [&](cISCPropertyHolder* h) { return cSCPropertyHelp::GetPropertyValue(h, property, out); });
    }
    bool OccupantGroup(std::uint32_t type, std::uint32_t group) override {
        return WithBuildingExemplar(type, [&](cISCPropertyHolder* h) {
            std::vector<std::uint32_t> groups;
            return cSCPropertyHelp::GetPropertyValue(h, 0xaa1dd396, groups) && std::find(groups.begin(), groups.end(), group) != groups.end();
        });
    }
    int RewardNetworkType(std::uint8_t category) override {
        auto* traffic = Component<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator);
        return traffic && category < 3 ? traffic->GetFerryTerminalCount(category) : 0;
    }
    IPlaceLotChooser* Chooser() override { return nullptr; }   // the debug pick dialogs (LotPlop / BuildingPlop cheats)
} g_host;

}  // namespace

void InstallPlaceLotHost(sc4::cSC4View3DWin& view) {
    g_view = &view;
    SetPlaceLotHost(&g_host);
}

}  // namespace native
