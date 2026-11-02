// The demolition utility's host on the native application (src/sc4/Demolition.h; demolition.md §4). What is not wired is
// marked PARTIAL where the original calls it.
#include "app/NativeDemolition.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/Demolition.h"
#include "sc4/Exemplar.h"
#include "sc4/FloraOccupant.h"
#include "sc4/Lot.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/LotDeveloper.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/NetworkManager.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/VisualEffect.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

constexpr std::uint32_t kIIDOccupant = 0x26bb767e;   // cISC4Occupant

bool Debug() { static const bool on = std::getenv("SC4_DEMOLISHDEBUG") != nullptr; return on; }

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
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
template <class T> T* Plain(CityComponent c) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
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
cSC4PropManager* Props() { return Plain<cSC4PropManager>(CityComponent::PropManager); }
cSC4Demolition* Service() {
    cSC4Demolition* demolition = Plain<cSC4Demolition>(CityComponent::DemolitionUtility);
    if (demolition) demolition->Init();   // the plain wrapper never initialises it; Init is idempotent
    return demolition;
}
cSC4LotDeveloper* LotDeveloper() { return Serializable<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4ZoneDeveloper* ZoneDeveloper() { return Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4ZoneManager* Zones() { return Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4NetworkManager* Networks() { return Serializable<cSC4NetworkManager>(CityComponent::NetworkManager, kGZCLSID_cSC4NetworkManager); }
cSC4NeighborsSimulator* Neighbors() { return Serializable<cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, kGZCLSID_cSC4NeighborsSimulator); }

// ---- the occupants and lots of one call ---------------------------------------------------------------------------------
class Occupant final : public IDemolishable {
public:
    explicit Occupant(cISC4Occupant* occupant) : occupant_(occupant) { occupant_->AddRef(); }
    ~Occupant() override { occupant_->Release(); }
    cISC4Occupant* Get() const { return occupant_; }
    std::uint32_t Type() override { return occupant_->GetType(); }
    bool HasRemovalFlag(std::uint32_t flag) override { return occupant_->IsFlagSet(flag); }   // occupant slot 32
    std::uint32_t Flags() override { return occupant_->GetFlags(); }
    void Remove() override { occupant_->Shutdown(); }                                          // occupant slot 4
    void BoundingBox(float out[6]) override { occupant_->GetBoundingBox(out, out + 3); }
    sc4::cISCPropertyHolder* Properties() override { return occupant_->AsPropertyHolder(); }
    bool TestFlag(std::uint32_t group) override { return occupant_->IsOccupantGroup(group); }
    // PARTIAL: the network occupant's flag query (only the demolition effect's road / rail pick reads it).
    bool NetworkHasFlag(int, bool* isNetwork) override {
        if (isNetwork) *isNetwork = dynamic_cast<cSC4NetworkOccupant*>(occupant_) != nullptr;
        return false;
    }
private:
    cISC4Occupant* occupant_;
};

class Lot final : public IDemolishableLot {
public:
    explicit Lot(cSC4Lot* lot) : lot_(lot) { lot_->AddRef(); }
    ~Lot() override { lot_->Release(); }
    cSC4Lot* Get() const { return lot_; }
    DemolitionCellRect Bounds() override {
        std::int32_t r[4] = {};
        lot_->GetBoundingRect(r);
        return DemolitionCellRect{r[0], r[1], r[2], r[3]};
    }
    bool IsPlopped() override { return lot_->IsInDemolishableState(); }   // lot slot 39 IsInDemolishableState
    std::vector<IDemolishable*> Occupants() override;
    int TypeCode() override { return lot_->GetState(); }                   // slot 33 GetState (1 empty, 6 destroyed)
    int ZoneClass() override { return lot_->GetCondition(); }
    int ZoneType() override { return lot_->GetZoneType(); }
    std::uint8_t Orientation() override { return lot_->GetFacing(); }
    void Size(std::uint8_t& width, std::uint8_t& depth) override { lot_->GetSize(width, depth); }
    IDemolishable* BuildingOccupant() override;
    bool BuildingRemovable(int level) override;
    // The lot configuration (slot 9) and its IsCompatibleWithWater (configuration slot 14): ExplodeLot instead of the plain
    // removal when the lot has no water-compatible configuration and the caller asked for 0x300.
    bool HasBuildingObject() override { return lot_->GetLotConfiguration() != nullptr; }
    bool BuildingPlain() override;
    bool BlocksTerrainChange() override { return lot_->GetVerticesAreLocked(); }
private:
    cSC4Lot* lot_;
};

// One call's adapters: an occupant / lot always maps to the same adapter, and all of them go when the call returns.
struct Arena {
    std::map<cISC4Occupant*, std::unique_ptr<Occupant>> occupants;
    std::map<cSC4Lot*, std::unique_ptr<Lot>> lots;
    IDemolishable* Of(cISC4Occupant* occupant) {
        if (!occupant) return nullptr;
        auto& slot = occupants[occupant];
        if (!slot) slot = std::make_unique<Occupant>(occupant);
        return slot.get();
    }
    IDemolishableLot* Of(cSC4Lot* lot) {
        if (!lot) return nullptr;
        auto& slot = lots[lot];
        if (!slot) slot = std::make_unique<Lot>(lot);
        return slot.get();
    }
} g_arena;
int g_depth = 0;
struct Scope {
    Scope() { ++g_depth; }
    ~Scope() {
        if (--g_depth == 0) {
            g_arena.occupants.clear();
            g_arena.lots.clear();
        }
    }
};
cISC4Occupant* Raw(IDemolishable* o) { return static_cast<Occupant*>(o)->Get(); }
cSC4Lot* Raw(IDemolishableLot* l) { return static_cast<Lot*>(l)->Get(); }

std::vector<IDemolishable*> Lot::Occupants() {   // lot slot 49 GetLotOccupants
    std::vector<cIGZUnknown*> objects;
    lot_->GetLotOccupants(objects);
    std::vector<IDemolishable*> out;
    for (cIGZUnknown* object : objects) {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kIIDOccupant, &raw) || !raw) continue;
        auto* occupant = static_cast<cISC4Occupant*>(raw);
        out.push_back(g_arena.Of(occupant));
        occupant->Release();
    }
    return out;
}
IDemolishable* Lot::BuildingOccupant() {   // lot slot 48 GetBuilding
    cIGZUnknown* building = lot_->GetBuilding();
    void* raw = nullptr;
    if (!building || !building->QueryInterface(kIIDOccupant, &raw) || !raw) return nullptr;
    auto* occupant = static_cast<cISC4Occupant*>(raw);
    IDemolishable* out = g_arena.Of(occupant);
    occupant->Release();
    return out;
}
bool Lot::BuildingRemovable(int level) {
    IDemolishable* building = BuildingOccupant();
    return !building || Raw(building)->CanRemove(level);
}
bool Lot::BuildingPlain() {
    cISC4LotConfigurationRef* reference = lot_->GetLotConfiguration();
    cSC4LotConfigurationManager* manager = Plain<cSC4LotConfigurationManager>(CityComponent::LotConfigurationManager);
    cSC4LotConfiguration* configuration = reference && manager ? manager->GetLotConfiguration(reference->GetID()) : nullptr;
    return configuration && configuration->IsCompatibleWithWater();
}

// The default occupant filter (cSC4DefaultOccupantFilter): not the interfaces 0x4a10afc2 / 0x8a139184 nor the groups 0x4217,
// 0x4208, 0x421d.
bool DefaultFilterIncludes(cISC4Occupant* occupant) {
    for (std::uint32_t iid : {0x4a10afc2u, 0x8a139184u}) {
        void* raw = nullptr;
        if (occupant->QueryInterface(iid, &raw) && raw) {
            static_cast<cIGZUnknown*>(raw)->Release();
            return false;
        }
    }
    for (std::uint32_t group : {0x4217u, 0x4208u, 0x421du})
        if (occupant->IsOccupantGroup(group)) return false;
    return true;
}

// The demolition filter Φ (demolition.md §4.2). PARTIAL: the "realistic occupant" half of step 1 and the foundation
// interface's slot 9 of step 5 (foundations pass); the view level is 1 (no underground view natively).
bool PhiIncludes(cISC4Occupant* occupant, const DemolitionFilterState& state) {
    if (!DefaultFilterIncludes(occupant)) return false;
    IDemolishable* adapter = g_arena.Of(occupant);
    if (state.user && !state.user->IsOccupantIncluded(adapter)) return false;
    if (state.active && !occupant->CanRemove(state.mode)) return false;
    switch (occupant->GetType()) {
    case kDemolishTypeFoundation: return true;
    case kDemolishTypeBuilding: case kDemolishTypePole: case kDemolishTypeSpecial: case kDemolishTypeProp:
    case kDemolishTypeNetwork: case kDemolishTypeFlora: break;
    default: return false;
    }
    if (state.view && !state.view->IsOccupantIncluded(adapter)) return false;
    return true;
}

// ---- the host ------------------------------------------------------------------------------------------------------------
class Host final : public IDemolitionHost {
public:
    void AddNotification(std::uint32_t) override {}      // the plain wrapper is not a message target; the tuning is read at Init
    void RemoveNotification(std::uint32_t) override {}
    void PostOccupantMessage(std::uint32_t message, IDemolishable* occupant, bool) override { Raw(occupant)->PostOccupantMessage(message, 0); }
    sc4::cISCPropertyHolder* TuningExemplar(std::uint32_t group, std::uint32_t instance) override {
        static std::map<std::uint64_t, cISCResExemplar*> cache;   // kept for the process, like the original's tuning reads
        const std::uint64_t key = (static_cast<std::uint64_t>(group) << 32) | instance;
        auto it = cache.find(key);
        if (it == cache.end()) {
            void* raw = nullptr;
            if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, group, instance}, 0xa52160f5, &raw, 0, nullptr)) raw = nullptr;
            it = cache.emplace(key, static_cast<cISCResExemplar*>(raw)).first;
        }
        return it->second ? it->second->AsISCPropertyHolder() : nullptr;
    }
    void CityCells(int& x, int& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 0;
        z = city ? city->CellCountZ() : 0;
    }
    IDemolishFilter* ViewOccupantFilter() override { return nullptr; }   // PARTIAL: no view filters (data views) natively
    void ViewBeginSweep() override {}
    bool OccupantAllowedByFilter(IDemolishable* occupant, const DemolitionFilterState& state) override { return PhiIncludes(Raw(occupant), state); }
    bool LocalFilterIncludes(IDemolishable* occupant) override { return DefaultFilterIncludes(Raw(occupant)); }
    // §4.4 step 4: the occupants Φ accepts whose position lies in the cells (x0 * 16 .. (x1 + 1) * 16, all sides inclusive).
    std::vector<IDemolishable*> OccupantsInRect(const DemolitionCellRect& cells, const DemolitionFilterState& state) override {
        std::vector<IDemolishable*> out;
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return out;
        const int a[2] = {cells.x0, cells.z0}, b[2] = {cells.x1, cells.z1};
        OccupantList found;
        if (occupants->GetOccupantsByStandardCityCells(found, a, b, 0u, 0xffffffffu)) {
            const float x0 = static_cast<float>(cells.x0) * 16.0f, z0 = static_cast<float>(cells.z0) * 16.0f;
            const float x1 = static_cast<float>(cells.x1 + 1) * 16.0f, z1 = static_cast<float>(cells.z1 + 1) * 16.0f;
            for (cISC4Occupant* occupant : found) {
                float p[3] = {};
                occupant->GetPosition(p);
                if (state.rectScan && (p[0] < x0 || x1 < p[0] || p[2] < z0 || z1 < p[2])) continue;
                if (PhiIncludes(occupant, state)) out.push_back(g_arena.Of(occupant));
            }
        }
        occupants->ReleaseOccupantList(found);
        return out;
    }
    std::vector<IDemolishable*> OccupantsForTerrain(const DemolitionCellRect& cells, int) override {
        DemolitionFilterState none;
        return OccupantsInRect(cells, none);   // PARTIAL: the terrain filter (ModifyTerrainHeight has no native caller yet)
    }
    bool RemoveOccupant(IDemolishable* occupant, bool shutdown, bool) override {
        cISC4OccupantManager* occupants = Occupants();
        return occupants && occupants->RemoveOccupant(Raw(occupant), shutdown, 0);
    }
    void ReleaseOccupantList(std::vector<IDemolishable*>&) override {}   // the call's arena owns the adapters
    void OccupantManagerCentre(int x, int z, float& a, float& b) override {
        a = static_cast<float>(x) * 16.0f + 8.0f;
        b = static_cast<float>(z) * 16.0f + 8.0f;
    }
    IDemolishableLot* LotAtCell(int x, int z) override { return g_arena.Of(NativeLotAtCell(x, z, false)); }
    IDemolishableLot* LotOfOccupant(IDemolishable* occupant) override {
        float p[3] = {};
        Raw(occupant)->GetPosition(p);
        return g_arena.Of(NativeLotAtCell(static_cast<int>(p[0] / 16.0f), static_cast<int>(p[2] / 16.0f), false));
    }
    std::vector<IDemolishableLot*> LotsInRect(const DemolitionCellRect& cells) override {
        std::vector<IDemolishableLot*> out;
        std::list<cRZAutoRefCount<cSC4Lot>> lots;
        if (cSC4ZoneDeveloper* developer = ZoneDeveloper(); developer && developer->GetLotsInCellRect(lots, cells.x0, cells.z0, cells.x1, cells.z1, false))
            for (const auto& lot : lots) out.push_back(g_arena.Of(lot.get()));
        return out;
    }
    void RemoveLot(IDemolishableLot* lot) override { NativeDeleteLot(Raw(lot)); }
    void CreateLotAt(int x, int z, std::uint8_t orientation) override {
        cSC4ZoneDeveloper* developer = ZoneDeveloper();
        cSC4Lot* made = nullptr;
        if (developer && developer->CreateLotIfPossible(x, z, 1, 1, orientation, made, false, false, nullptr) && made) made->Release();
    }
    int ZoneTypeAt(int x, int z) override { cSC4ZoneManager* zones = Zones(); return zones ? zones->GetZoneType(x, z) : 0; }
    void ClearZones(const DemolitionCellRect& cells) override {   // zone manager slot 11 PlaceZone(region(rect), 0, 1, 1, 0, 1, 1, 0, 0, 0)
        cSC4ZoneManager* zones = Zones();
        if (!zones) return;
        SC4CellRegion<int> region = SC4CellRegion<int>::Make(SC4Rect<int>{cells.x0, cells.z0, cells.x1, cells.z1}, true);
        zones->PlaceZone(region, 0, true, true, false, true, true, nullptr, nullptr, nullptr);
    }
    void LotPrepareRemoval(IDemolishableLot* lot) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->StartDemolishLot(Raw(lot)); }
    void LotFinishRemoval(IDemolishableLot* lot) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->EndDemolishLot(Raw(lot)); }
    void LotSkipRemoval(IDemolishableLot* lot, bool burnt) override { if (cSC4LotDeveloper* d = LotDeveloper()) d->ExplodeLot(Raw(lot), burnt); }
    void NetworkBatch(bool begin) override { if (cSC4NetworkManager* n = Networks()) n->QueueRemovals(begin); }
    void UpdateNetworkManager(const std::vector<IDemolishable*>& networks) override {   // slot 6 DestroyNetworkOccupants
        cSC4NetworkManager* manager = Networks();
        if (!manager) return;
        ilist<cISC4Occupant*> list;
        for (IDemolishable* network : networks) list.push_back(Raw(network));
        manager->DestroyNetworkOccupants(list);
    }
    void NotifyNeighborsNetworkChange(IDemolishable* network) override { if (cSC4NeighborsSimulator* n = Neighbors()) n->DeleteOccupantConnection(Raw(network)); }
    float TerrainHeight(int x, int z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitudeAtVertex(x, z) : 0.0f; }
    bool TerrainCellSkipped(int, int) override { return false; }
    // PARTIAL: the terrain levelling of ModifyTerrainHeight (no native caller yet).
    void TerrainLevelCell(int, int) override {}
    void TerrainLevelRect(const DemolitionCellRect&, std::uint32_t) override {}
    void TerrainUpdateRect(const DemolitionCellRect&, std::uint32_t) override {}
    float TerrainHeightAtMetres(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool PropManagerAvailable() override { return Props() != nullptr; }
    bool PropManagerRemove(IDemolishable* prop) override { cSC4PropManager* p = Props(); return p && p->RemovePropA(Raw(prop)); }
    bool NetworkCost(IDemolishable* network, std::int64_t& cost) override {   // network occupant slot 63
        auto* occupant = dynamic_cast<cSC4NetworkOccupant*>(Raw(network));
        if (!occupant) return false;
        cost = occupant->GetCostOfDemolition();
        return true;
    }
    bool BuildingCost(IDemolishable* building, std::int64_t& cost) override {   // GetCostOfOccupantDemolition: property 0x099afacd
        cost = 0;
        cSCPropertyHelp::GetPropertyValue(Raw(building)->AsPropertyHolder(), 0x99afacd, cost);
        return true;
    }
    bool FloraCost(IDemolishable*, std::int64_t& cost) override { cost = 0; return true; }   // flora slot 22: always 0
    // PARTIAL: power poles (cISC4PowerPoleOccupant) are not demolishable natively yet.
    bool PoleCost(IDemolishable*, std::int64_t&) override { return false; }
    std::vector<IDemolishable*> PoleConnections(IDemolishable*) override { return {}; }
    void PoleDetach(IDemolishable*) override {}
    bool IsPlainOccupant(IDemolishable*) override { return false; }
    IDemolitionEffect* CreateEffect(std::uint32_t effectSet, std::uint32_t index) override;
    void ReleaseEffect(IDemolitionEffect* effect) override;
    void PlayGroupEffect(std::uint32_t, float, float, float) override {}   // PARTIAL: the group dust cloud
    void PlaySound(std::uint32_t) override {}                              // PARTIAL: the demolition sounds
    float RenderMinimumSize() override { return 0.0f; }
    float ViewScale() override { return 1.0f; }
} g_host;

// A demolition effect (effect set 0x6a3589a0, by index) placed on the occupant it is attached to.
class Effect final : public IDemolitionEffect {
public:
    explicit Effect(cSC4VisualEffect* effect) : effect_(effect) {}
    ~Effect() override { effect_->Release(); }
    bool Attach(IDemolishable* occupant) override {   // PARTIAL: SetModelFromOccupant; the effect starts at the occupant's position
        float p[3] = {};
        Raw(occupant)->GetPosition(p);
        cS3DTransform source;
        source.flags = 1;
        source.changeCounter = 1;
        source.translation[0] = p[0];
        source.translation[1] = p[1];
        source.translation[2] = p[2];
        effect_->SetSourceTransform(source);
        return true;
    }
    void SetViewpoint(int, int) override {}
    void SetGrayTint() override {}
    void Play() override { effect_->Start(0); }
private:
    cSC4VisualEffect* effect_;
};
IDemolitionEffect* Host::CreateEffect(std::uint32_t effectSet, std::uint32_t index) {
    cSC4VisualEffect* effect = CreateKeyedEffect(effectSet, index);
    return effect ? new Effect(effect) : nullptr;
}
void Host::ReleaseEffect(IDemolitionEffect* effect) { delete static_cast<Effect*>(effect); }

}  // namespace

IDemolitionHost* NativeDemolitionHost() { return &g_host; }
cISC4Occupant* DemolitionOccupant(IDemolishable* occupant) { return occupant ? Raw(occupant) : nullptr; }

bool DemolishRegion(bool commit, const DemolitionRegion& region, int privilege, std::uint32_t flags, bool dezone, std::int64_t* cost,
                    cISC4Occupant* clicked, int viewX, int viewY, IDemolishFilter* filter) {
    cSC4Demolition* demolition = Service();
    if (!demolition) return false;
    Scope scope;
    const bool ok = demolition->DemolishRegion(commit, region, privilege, flags, dezone, filter, cost, nullptr, g_arena.Of(clicked), viewX, viewY);
    if (Debug()) {
        std::map<std::uint32_t, std::pair<int, int>> byType;   // type -> seen, gone from the occupant manager (flag 1 cleared)
        for (auto& [occupant, adapter] : g_arena.occupants) {
            auto& tally = byType[occupant->GetType()];
            ++tally.first;
            if (!occupant->IsFlagSet(1)) ++tally.second;
        }
        std::fprintf(stderr, "demolition: %zu lots;", g_arena.lots.size());
        for (auto& [type, tally] : byType) std::fprintf(stderr, " %08x %d/%d", type, tally.second, tally.first);
        std::fprintf(stderr, "\n");
    }
    if (Debug())
        std::fprintf(stderr, "demolition: region %d,%d..%d,%d commit %d privilege %d flags %x -> %d cost %lld\n", region.bounds.x0, region.bounds.z0,
                     region.bounds.x1, region.bounds.z1, commit, privilege, flags, ok, cost ? static_cast<long long>(*cost) : 0LL);
    return ok;
}

bool DemolishOccupant(bool commit, cISC4Occupant* occupant, std::uint32_t flags, std::int64_t* cost) {
    cSC4Demolition* demolition = Service();
    if (!demolition || !occupant) return false;
    Scope scope;
    std::vector<IDemolishable*> released;
    return demolition->DemolishOccupant(commit, g_arena.Of(occupant), flags, cost, false, &released);
}

bool DemolishLots(bool commit, cSC4Lot* const* lots, int count, int privilege, std::uint32_t flags, bool dezone, std::int64_t* cost) {
    cSC4Demolition* demolition = Service();
    if (!demolition) return false;
    Scope scope;
    std::list<IDemolishableLot*> list;
    for (int i = 0; i < count; ++i)
        if (lots[i]) list.push_back(g_arena.Of(lots[i]));
    return demolition->DemolishLots(commit, list, privilege, flags, dezone, nullptr, cost, nullptr);
}

}  // namespace native
