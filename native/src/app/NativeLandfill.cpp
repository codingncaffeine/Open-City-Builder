// The landfill developer's and its zones' city services (sc4::ILandfillDeveloperHost / sc4::ILandfillZoneHost): the city's cells,
// the terrain, the pollution simulator's garbage, the sanitation department's budget, the zone manager, the traffic simulator's
// subnetworks, the landfill tile and base texture occupants, the unconnected-zone marker prop and the simulator agent.
// Each method names the original call it stands for. The garbage trucks' generator and attractor are automata controllers, the
// seagull flock a visual effect.
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeEffects.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/DBSegment.h"
#include "sc4/Exemplar.h"
#include "sc4/LandfillDeveloper.h"
#include "sc4/LandfillOccupant.h"
#include "sc4/LandfillZone.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/LotDeveloper.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/VisualEffect.h"
#include "sc4/TrafficSimulator.h"
#include "tools/city_probe_plain.h"
#include "sc4/cSC4City.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
int NativeCurrentViewLevel();   // app/NativeCitySystems.cpp
cIGZUnknown* NativeAddControllerAt(const char* templateName, const float position[3]);   // app/NativeAutomataControllerHost.cpp
void NativeControllerSetFlag(cIGZUnknown* controller, std::uint32_t flag, bool on);
void NativeControllerActivate(cIGZUnknown* controller, bool on);
void NativeRemoveController(cIGZUnknown* controller);

namespace {
using namespace sc4;

constexpr std::uint32_t kLandfillTileExemplarGroup = 0x07bddf1c;   // {0x6534284a, 0x07bddf1c, tile id}
constexpr std::uint32_t kExemplarType = 0x6534284a;
constexpr std::uint32_t kBaseTextureOccupantType = 0x497f6d9d;
constexpr std::uint32_t kIncompatibleOccupantType = 0x74758926;     // what a landfill tile clears off its cell
constexpr std::uint32_t kZotPropType = 0x107a0000;                  // the "not connected" marker
constexpr std::uint32_t kOccupantChangedMessage = 0x99ef1145;

bool Trace() { static const bool on = std::getenv("SC4_LANDFILLDEBUG") != nullptr; return on; }

template <class T> T* Serializable(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4PollutionSimulator* Pollution() { return Serializable<cSC4PollutionSimulator>(CityComponent::PollutionSimulator, kGZCLSID_cSC4PollutionSimulator); }
cSC4BudgetSimulator* Budget() { return Serializable<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator); }
cSC4TrafficSimulator* Traffic() { return Serializable<cSC4TrafficSimulator>(CityComponent::TrafficSimulator, kGZCLSID_cSC4TrafficSimulator); }
cSC4ZoneManager* Zones() { return Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4LotDeveloper* LotDeveloper() { return Serializable<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }
cSC4PropManager* Props() {   // a plain class behind its serializable adapter
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::PropManager) : nullptr;
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4PropManager*>(raw) : nullptr;
}
cSC4TrafficNetworkMap* NetworkMap() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::TrafficNetwork) : nullptr;
    return object ? dynamic_cast<cSC4TrafficNetworkMap*>(object) : nullptr;
}
cSTETerrain* Terrain() {
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
void CellCenter(int x, int z, float& wx, float& wz) {   // city vslot 0x308 (i386 0x184 CellCenterToPosition)
    if (cSC4City* city = HostCity()) city->CellCenterToPosition(x, z, wx, wz);
    else { wx = float(x) * 16.0f + 8.0f; wz = float(z) * 16.0f + 8.0f; }
}

// The services the developer holds as cIGZUnknown: the host owns them, so the reference count is a no-op.
template <class I> class Service : public I {
public:
    bool QueryInterface(std::uint32_t, void** out) override { if (out) *out = nullptr; return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};

class CityApi final : public Service<ILandfillCityApi> {
public:
    int CellCountX() const override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CellCountZ() const override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void CellToWorld(int x, int z, float& outX, float& outZ) override { CellCenter(x, z, outX, outZ); }
} g_cityApi;

class TerrainApi final : public Service<ILandfillTerrainApi> {
public:
    float GetAltitude(float x, float z) const override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float GetSeaLevel() const override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    // terrain i386 0x114 RedisplayTerrain(true, true, rect, 5)
    void RefreshRect(const LandfillRect& rect) override {
        if (cSTETerrain* t = Terrain()) t->RedisplayTerrain(true, true, TerrainRect{rect.x0, rect.z0, rect.x1, rect.z1}, 5);
    }
} g_terrainApi;

class PollutionApi final : public Service<ILandfillPollutionApi> {
public:
    int GetGarbageSupply() override { cSC4PollutionSimulator* p = Pollution(); return p ? p->GetGarbageSupply() : 0; }
    int GetTotalGarbageUncollected() const override { cSC4PollutionSimulator* p = Pollution(); return p ? int(p->GetTotalGarbageUncollected()) : 0; }
    void SetGarbageCapacity(int capacity) override {
        if (cSC4PollutionSimulator* p = Pollution()) p->SetGarbageCapacity(capacity);
        if (Trace()) std::fprintf(stderr, "landfill: garbage capacity %d\n", capacity);
    }
} g_pollutionApi;

class BudgetLine final : public Service<ILandfillBudgetLine> {
public:
    void Set(cSC4LineItem* item) { item_ = item; }
    std::int64_t GetCurrentExpenses() const override { return item_ ? item_->GetCurrentExpenses() : 0; }
    void SetFullExpenses(std::int64_t value) override { if (item_) item_->SetFullExpenses(value); }
private:
    cSC4LineItem* item_ = nullptr;
};

class DepartmentBudget final : public Service<ILandfillDepartmentBudget> {
public:
    void Set(cSC4DepartmentBudget* department) { department_ = department; }
    const float& GetFundingPercentage() const override { static const float full = 1.0f; return department_ ? department_->GetFundingPercentage() : full; }
    ILandfillBudgetLine* GetLineItem(std::uint32_t id) override {
        cSC4LineItem* item = department_ ? department_->GetLineItem(id) : nullptr;
        if (!item) return nullptr;
        line_.Set(item);
        return &line_;
    }
private:
    cSC4DepartmentBudget* department_ = nullptr;
    BudgetLine line_;
};

class BudgetApi final : public Service<ILandfillBudgetApi> {
public:
    ILandfillDepartmentBudget* GetDepartmentBudget(std::uint32_t id) override {
        cSC4BudgetSimulator* budget = Budget();
        cSC4DepartmentBudget* department = budget ? budget->GetDepartmentBudget(id) : nullptr;
        if (Trace()) std::fprintf(stderr, "landfill: department %08x -> %s\n", id, department ? "found" : "none");
        if (!department) return nullptr;
        department_.Set(department);
        return &department_;
    }
private:
    DepartmentBudget department_;
} g_budgetApi;

// A cISC4Occupant the zone found, with the reference the lookup gave.
class OccupantRef : public ILandfillOccupantRef {
public:
    explicit OccupantRef(cISC4Occupant* occupant) : occupant_(occupant) {}
    void Release() override { occupant_->Release(); delete this; }
    // occupant manager vslot 0xa8 (occupant, true, 3)
    void RemoveFromCity() override { if (cISC4OccupantManager* o = Occupants()) o->RemoveOccupant(occupant_, true, 3); }
    // the base texture occupant's vslot 0x20 (GetUsage); 0 for anything else
    int TextureKind() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(kGZIID_cISC4LotBaseTextureOccupant, &raw) || !raw) return 0;
        auto* texture = static_cast<cISC4LotBaseTextureOccupant*>(raw);
        const int usage = texture->GetUsage();
        texture->Release();
        return usage;
    }
protected:
    cISC4Occupant* occupant_;
};

// cSC4LandfillOccupant behind the zone's tile interface.
class Tile final : public ILandfillTile {
public:
    explicit Tile(cSC4LandfillOccupant* tile) : tile_(tile) {}
    void Release() override { tile_->Release(); delete this; }
    void RemoveFromCity() override { if (cISC4OccupantManager* o = Occupants()) o->RemoveOccupant(tile_, true, 3); }
    void GetPosition(float xyz[3]) override { tile_->GetPosition(xyz); }
    void SetPosition(const float xyz[3]) override { tile_->SetPosition(xyz); }
    bool IsEmptyTile() override { return tile_->IsEmptyLandfillTile(); }
    void SetLayer(int layer) override { tile_->SetRemovalFlags(layer); }   // i386 occupant +0x30 SetRemovalFlags
    void SetEmpty(bool empty) override { tile_->SetEmptyLandfillTile(empty); }
    void Init() override { tile_->Init(); }
    cISCPropertyHolder* Properties() override { return tile_->AsPropertyHolder(); }
    cSC4LandfillOccupant* Occupant() const { return tile_; }
private:
    cSC4LandfillOccupant* tile_;
};

// The marker prop over an unconnected zone (a cSC4PropOccupant made by the prop manager).
class ZotProp final : public ILandfillProp {
public:
    explicit ZotProp(cSC4PropOccupant* prop) : prop_(prop) {}
    void AddRef() override { ++count_; }
    void Release() override {
        if (--count_ > 0) return;
        prop_->Release();
        delete this;
    }
    void SetState(int state) override { prop_->SetState(static_cast<std::uint32_t>(state)); }
    cSC4PropOccupant* Prop() const { return prop_; }
private:
    cSC4PropOccupant* prop_;
    int count_ = 1;
};

// A garbage truck controller (cSC4AutomataController) as the zone uses it: SetFlag (vslot 0x78) and Activate (vslot 0xa0).
class ControllerEffect final : public ILandfillEffect {
public:
    explicit ControllerEffect(cIGZUnknown* controller) : controller_(controller) {}
    void AddRef() override { ++refs_; }
    void Release() override { if (--refs_ <= 0) delete this; }
    void SetFlag(std::uint32_t flag, bool on) override { NativeControllerSetFlag(controller_, flag, on); }
    void SetEnabled(bool on) override { NativeControllerActivate(controller_, on); }
    void SetTransform(const float[13]) override {}
    void Start(int) override {}
    void Stop(bool) override {}
    cIGZUnknown* Controller() const { return controller_; }
private:
    cIGZUnknown* controller_;
    int refs_ = 0;
};

// The seagull flock: a visual effect placed by its transform (3x3 rotation, position, scale), started and stopped.
class VisualEffectView final : public ILandfillEffect {
public:
    explicit VisualEffectView(cSC4VisualEffect* effect) : effect_(effect) {}
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ > 0) return;
        effect_->Release();
        delete this;
    }
    void SetFlag(std::uint32_t, bool) override {}
    void SetEnabled(bool) override {}
    void SetTransform(const float matrix[13]) override {
        cS3DTransform transform;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) transform.rotation[r][c] = matrix[r * 3 + c];
        transform.translation[0] = matrix[9];
        transform.translation[1] = matrix[10];
        transform.translation[2] = matrix[11];
        transform.flags |= 1;
        transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 1);
        effect_->SetSourceTransform(transform);
    }
    void Start(int mode) override { effect_->Start(mode); }
    void Stop(bool immediate) override { effect_->Stop(immediate ? 1 : 0); }
    bool IsRunning() override { return effect_->IsStarted(); }
private:
    cSC4VisualEffect* effect_;
    int refs_ = 1;   // CreateAmbientEffect hands the zone one reference (it does not AddRef the flock)
};

class ZoneHost final : public ILandfillZoneHost {
public:
    const LandfillTunables& Tunables() override { return LandfillDeveloperTunables(); }
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    void CellToWorld(int x, int z, float& wx, float& wz) override { CellCenter(x, z, wx, wz); }
    // city i386 0xa0 GetTrafficNetwork -> i386 0x28 FindNetworkOccupant(x, z, 0x1f4f, true): a surface network piece on the cell
    bool HasNetworkAt(int x, int z) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        return map && map->FindNetworkOccupant(x, z, 0x1f4fu, true) != nullptr;
    }
    float AltitudeAt(float wx, float wz) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(wx, wz) : 0.0f; }
    float TerrainHeightAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitudeAtNearestGrid(x, z) : 0.0f; }   // i386 0x4c
    float AdjustedHeight(float, float) override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }   // i386 0x74 GetSeaLevel
    void LevelTerrainRect(const LandfillRect& rect, float height) override {   // i386 0xcc SetAltitude(rect, height, true, 5)
        if (cSTETerrain* t = Terrain()) t->SetAltitude(TerrainRect{rect.x0, rect.z0, rect.x1, rect.z1}, height, true, 5);
    }
    // spBuildingDevelopmentSim GetLotDeveloper -> AttemptAreaLevelling(rect, 0, true, true, FLT_MAX, null)
    void RefreshBuildingDevelopmentRect(const LandfillRect& rect) override {
        if (cSC4LotDeveloper* d = LotDeveloper()) d->AttemptAreaLevelling(LotCellRect{rect.x0, rect.z0, rect.x1, rect.z1}, 0.0f, true, true, FLT_MAX, nullptr);
    }
    int ZoneTypeAt(int x, int z) override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneType(x, z) : 0; }
    // zone manager i386 0x2c PlaceZone(1x1 region, 0, commit, free, no terrain check, mask, no clip, ...)
    void DezoneCell(int x, int z) override {
        cSC4ZoneManager* m = Zones();
        if (!m) return;
        SC4CellRegion<int> region = SC4CellRegion<int>::Make(SC4Rect<int>{x, z, x, z}, true);
        m->PlaceZone(region, 0, true, true, false, true, false, nullptr, nullptr, nullptr);
    }
    int LandfillTileCapacity() override { cSC4PollutionSimulator* p = Pollution(); return p ? int(p->GetLandfillTileCapacity()) : 0; }
    void AddToGarbageSupply(int amount) override { if (cSC4PollutionSimulator* p = Pollution()) p->AddToGarbageSupply(amount); }
    void GarbagePutInLandfill(int amount) override { if (cSC4PollutionSimulator* p = Pollution()) p->GarbagePutInLandfill(amount); }
    void SetActiveLandfill(int x, int z, bool on) override {
        if (cSC4PollutionSimulator* p = Pollution()) p->SetIsActiveLandfill(unsigned(x), unsigned(z), on, std::uint32_t(ZoneTypeAt(x, z)));
    }
    // traffic simulator i386 0xa4 GetSubnetworksInRegion / 0xa8 GetOccupantCountForAllSubnetworks
    std::vector<std::uint32_t> NetworkIdsInRegion(const LandfillCellRegion& region) override {
        std::vector<std::uint32_t> out;
        cSC4TrafficSimulator* traffic = Traffic();
        if (!traffic) return out;
        TrafficCellRegion cells;
        cells.bounds = TrafficCellRect{region.bounds.x0, region.bounds.z0, region.bounds.x1, region.bounds.z1};
        const int w = region.bounds.x1 - region.bounds.x0 + 1, h = region.bounds.z1 - region.bounds.z0 + 1;
        if (w > 0 && h > 0) {
            cells.mask.assign(std::size_t(w), std::vector<bool>(std::size_t(h), false));
            for (int dx = 0; dx < w; ++dx)
                for (int dz = 0; dz < h; ++dz) cells.mask[std::size_t(dx)][std::size_t(dz)] = region.Test(region.bounds.x0 + dx, region.bounds.z0 + dz);
        }
        traffic->GetSubnetworksInRegion(cells, out);
        return out;
    }
    std::vector<int> NetworkFlagsForDeveloperType(std::uint32_t type) override {
        std::vector<int> out;
        cSC4TrafficSimulator* traffic = Traffic();
        if (!traffic) return out;
        std::vector<std::uint32_t> counts;
        traffic->GetOccupantCountForAllSubnetworks(type, counts);
        out.assign(counts.begin(), counts.end());
        return out;
    }
    // occupant manager i386 0x80 GetFirstOccupantByStandardCityCell(type 0x6a11bd2b)
    ILandfillTile* FindLandfillTile(int x, int z) override {
        cISC4OccupantManager* o = Occupants();
        cISC4Occupant* found = nullptr;
        if (!o || !o->GetFirstOccupantByStandardCityCell(found, x, z, kLandfillOccupantType) || !found) return nullptr;
        auto* tile = dynamic_cast<cSC4LandfillOccupant*>(found);
        if (!tile) { found->Release(); return nullptr; }
        return new Tile(tile);   // the lookup's reference goes to the wrapper
    }
    bool HasBaseTexture(int x, int z) override {
        cISC4OccupantManager* o = Occupants();
        cISC4Occupant* found = nullptr;
        if (!o || !o->GetFirstOccupantByStandardCityCell(found, x, z, kBaseTextureOccupantType) || !found) return false;
        found->Release();
        return true;
    }
    std::vector<ILandfillOccupantRef*> ListBaseTextures(int x, int z) override { return List(x, z, kBaseTextureOccupantType); }
    std::vector<ILandfillOccupantRef*> ListIncompatibleOccupants(int x, int z) override { return List(x, z, kIncompatibleOccupantType); }
    // spRM GetResource({0x6534284a, 0x07bddf1c, id}, 0xa52160f5); new cSC4LandfillOccupant(id, orientation), AddRef, its default
    // exemplar (+0xc vslot 0x68 SetDefaultExemplar) and SetFlag(4) (+4 vslot 0x74).
    ILandfillTile* LoadLandfillTile(std::uint32_t tileId, int orientation) override {
        void* raw = nullptr;
        const cGZPersistResourceKey key{kExemplarType, kLandfillTileExemplarGroup, tileId};
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) {
            if (Trace()) std::fprintf(stderr, "landfill: tile %08x has no exemplar\n", tileId);
            return nullptr;
        }
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        auto* tile = new cSC4LandfillOccupant(tileId, orientation);
        tile->AddRef();
        tile->SetDefaultExemplar(exemplar);
        tile->SetFlag(4);
        exemplar->Release();
        return new Tile(tile);
    }
    void AddOccupant(ILandfillTile* tile) override {   // occupant manager i386 0x50 InsertOccupant(occupant, 0)
        cISC4OccupantManager* o = Occupants();
        if (o && tile) o->InsertOccupant(static_cast<Tile*>(tile)->Occupant(), 0);
        if (Trace() && tile) {
            float p[3] = {};
            tile->GetPosition(p);
            std::fprintf(stderr, "landfill: tile %08x added at %.0f,%.0f\n", static_cast<Tile*>(tile)->Occupant()->GetTileID(), p[0], p[2]);
        }
    }
    void RefreshOccupant(ILandfillTile* tile) override {   // i386 0x60 MoveOccupant(occupant, true) + message 0x99ef1145
        cISC4OccupantManager* o = Occupants();
        if (!o || !tile) return;
        cSC4LandfillOccupant* occupant = static_cast<Tile*>(tile)->Occupant();
        o->MoveOccupant(occupant, true);
        occupant->PostOccupantMessage(kOccupantChangedMessage, 0);
    }
    // cSC4LandfillZone::SetEmptyCellTexture (i386 0x0007e850): a base texture occupant of usage 1 with one cell of texture 0x1005,
    // white at the zone display alpha, combine mode 1, frame 0; inserted, or moved and announced when it is already in the city.
    void PlaceEmptyCellTexture(int x, int z) override {
        cISC4OccupantManager* o = Occupants();
        cSC4ZoneManager* zones = Zones();
        if (!o) return;
        LotBaseTextureSpecification spec;
        spec.textureId = 0x1005;
        spec.cellX = static_cast<std::uint8_t>(x);
        spec.cellZ = static_cast<std::uint8_t>(z);
        spec.rotation = 0;
        spec.flip = 0;
        spec.red = spec.green = spec.blue = 0xff;
        spec.alpha = zones ? zones->GetZoneDisplayAlpha() : 0xff;
        spec.combineMode = 1;
        spec.frame = 0;
        auto* texture = new cSC4LotBaseTextureOccupant();
        texture->AddRef();
        cISC4Occupant* occupant = texture->AsOccupant();
        occupant->Init();
        texture->SetUsage(1);
        texture->SetTextureSpecification(&spec, 1, nullptr);
        if (!occupant->IsFlagSet(1)) {
            o->InsertOccupant(occupant, 0);
        } else {
            o->MoveOccupant(occupant, false);
            occupant->PostOccupantMessage(kOccupantChangedMessage, 0);
        }
        texture->Release();
    }
    // prop manager i386 0x64 AddLotProp(0x107a0000, position, 0)
    ILandfillProp* CreateZotProp(const float position[3]) override {
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = props ? props->AddLotProp(kZotPropType, position, 0) : nullptr;
        if (Trace()) std::fprintf(stderr, "landfill: zone not connected, marker at %.0f,%.0f -> %s\n", position[0], position[2], prop ? "made" : "none");
        return prop ? new ZotProp(prop) : nullptr;
    }
    void RemoveProp(ILandfillProp* prop) override {   // city i386 0x8c GetPropManager -> i386 0x74 RemovePropA
        cSC4PropManager* props = Props();
        if (props && prop) props->RemovePropA(static_cast<ZotProp*>(prop)->Prop());
    }
    bool WriteProp(cIGZOStream& stream, ILandfillProp* prop) override {
        return SC4WriteGZSerializable(stream, prop ? static_cast<cISC4Occupant*>(static_cast<ZotProp*>(prop)->Prop()) : static_cast<cISC4Occupant*>(nullptr));
    }
    ILandfillProp* ReadProp(cIGZIStream& stream) override {   // SC4ReadGZSerializable(0x26bb767e)
        void* raw = nullptr;
        if (!SC4ReadGZSerializable(stream, 0x26bb767e, &raw, false) || !raw) return nullptr;
        auto* prop = dynamic_cast<cSC4PropOccupant*>(static_cast<cISC4Occupant*>(raw));
        if (!prop) { static_cast<cISC4Occupant*>(raw)->Release(); return nullptr; }
        return new ZotProp(prop);
    }
    // The garbage trucks' generator and attractor: automata controllers from their templates at the zone's road cell (i386: the
    // city's controller manager, vslot 0x14 AddControllerFromTemplateName(null, name, position)); one reference for the zone's AddRef.
    ILandfillEffect* CreateEffect(const char* name, const float position[3]) override {
        cIGZUnknown* controller = name && *name ? NativeAddControllerAt(name, position) : nullptr;
        if (Trace()) std::fprintf(stderr, "landfill: controller %s at %.0f,%.0f -> %s\n", name ? name : "", position[0], position[2], controller ? "made" : "none");
        return controller ? new ControllerEffect(controller) : nullptr;
    }
    void RemoveEffect(ILandfillEffect* effect) override {   // controller manager vslot 0x20 RemoveController
        if (auto* controller = dynamic_cast<ControllerEffect*>(effect)) NativeRemoveController(controller->Controller());
    }
    // The seagulls: the effects manager's visual effect by name (i386 +0x58 vslot 0x18 CreateVisualEffect); one reference.
    ILandfillEffect* CreateAmbientEffect(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        if (Trace()) std::fprintf(stderr, "landfill: effect %s -> %s\n", name, effect ? "made" : "none");
        return effect ? new VisualEffectView(effect) : nullptr;
    }
    int Rand() override { return std::rand(); }
private:
    std::vector<ILandfillOccupantRef*> List(int x, int z, std::uint32_t type) {   // occupant manager i386 0xa0 by type
        std::vector<ILandfillOccupantRef*> out;
        cISC4OccupantManager* o = Occupants();
        if (!o) return out;
        OccupantList list;
        const int cell[2] = {x, z};
        if (!o->GetOccupantsByStandardCityCells(list, cell, cell, type, 0xffffffffu)) return out;
        for (cISC4Occupant* occupant : list) out.push_back(new OccupantRef(occupant));
        return out;
    }
} g_zoneHost;

class DeveloperHost final : public ILandfillDeveloperHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    ILandfillCityApi* City() override { return &g_cityApi; }
    ILandfillTerrainApi* Terrain() override { return &g_terrainApi; }
    ILandfillPollutionApi* Pollution() override { return &g_pollutionApi; }
    ILandfillBudgetApi* Budget() override { return &g_budgetApi; }
    cISCPropertyHolder* TuningExemplar() override {   // {0x6534284a, 0x67cd5fa1, 0x6a2806af}, kept for the session
        if (!tuning_) {
            void* raw = nullptr;
            if (SL::spRM && SL::spRM->GetResource(kLandfillTuningExemplarKey, kLandfillTuningExemplarIid, &raw, 0, nullptr) && raw)
                tuning_ = static_cast<cISCResExemplar*>(raw);
        }
        return tuning_ ? tuning_->AsISCPropertyHolder() : nullptr;
    }
    ILandfillZoneHost* ZoneHost() override { return &g_zoneHost; }
    void AddSimulatorAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) override {
        AddCitySimulatorAgent(target, interval, name, flags);
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    std::uint32_t ViewLevel() override { return static_cast<std::uint32_t>(NativeCurrentViewLevel()); }
    std::int32_t LandfillTileCount() override { cSC4ZoneManager* m = Zones(); return m ? m->GetZoneCount(kLandfillZoneTypeLandfill) : 0; }
private:
    cISCResExemplar* tuning_ = nullptr;
} g_developerHost;

}  // namespace

// The landfill developer's real host (replaces the generated stub's).
void InstallLandfillDeveloperHost() { SetLandfillDeveloperHost(&g_developerHost); }

// The pollution simulator's view of the landfill developer (vslots 0x50 GetLandfillUsage / 0x58 GetLandfillTileCount).
void NativeLandfillUsage(int& used, int& capacity) {
    used = capacity = 0;
    auto* developer = Serializable<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper, kGZCLSID_cSC4LandfillDeveloper);
    if (!developer) return;
    std::uint32_t u = 0, c = 0;
    developer->GetLandfillUsage(u, c);
    used = static_cast<int>(u);
    capacity = static_cast<int>(c);
}
int NativeLandfillTileCount() {
    auto* developer = Serializable<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper, kGZCLSID_cSC4LandfillDeveloper);
    return developer ? developer->GetLandfillTileCount() : 0;
}

// Kit command `landfill`: the developer's zones (bounds, connection, heaps, garbage / capacity, tiles in the city) and the pollution
// simulator's garbage figures.
void NativeLandfillReport() {
    auto* developer = Serializable<cSC4LandfillDeveloper>(CityComponent::LandfillDeveloper, kGZCLSID_cSC4LandfillDeveloper);
    cSC4PollutionSimulator* p = Pollution();
    if (!developer) { std::printf("landfill: no developer\n"); return; }
    std::uint32_t used = 0, capacity = 0;
    developer->GetLandfillUsage(used, capacity);
    std::printf("landfill: %zu zones, usage %u of %u, zoned tiles %d, maintenance %lld, efficiency %.2f; pollution capacity %d supply %d uncollected %u landfilled %lld\n",
                developer->ZoneCount(), used, capacity, developer->GetLandfillTileCount(), static_cast<long long>(developer->GetMaintenanceCosts()),
                developer->GetEfficiency(), p ? p->GetGarbageCapacity() : -1, p ? p->GetGarbageSupply() : -1, p ? p->GetTotalGarbageUncollected() : 0u,
                p ? static_cast<long long>(p->GetGarbageLandfilledPrevMonth()) : -1LL);
    cISC4OccupantManager* o = Occupants();
    for (cSC4LandfillZone* zone : developer->Zones()) {
        const LandfillCellRegion* r = zone->Region();
        int available = 0, garbage = 0, total = 0, tiles = 0, cells = 0, full = 0;
        zone->GetCapacity(available, garbage, total);
        if (r) {
            for (int x = r->bounds.x0; x <= r->bounds.x1; ++x)
                for (int z = r->bounds.z0; z <= r->bounds.z1; ++z) {
                    if (!r->Test(x, z)) continue;
                    ++cells;
                    cISC4Occupant* found = nullptr;
                    if (o && o->GetFirstOccupantByStandardCityCell(found, x, z, kLandfillOccupantType) && found) {
                        ++tiles;
                        if (auto* tile = dynamic_cast<cSC4LandfillOccupant*>(found); tile && tile->GetTileID() != kLandfillTileEmptyId) ++full;
                        found->Release();
                    }
                }
        }
        int withGarbage = 0;
        for (const cGarbageHeap& heap : zone->Heaps()) withGarbage += heap.hasGarbage ? 1 : 0;
        std::printf("landfill: zone %d,%d..%d,%d cells %d connected %d heaps %zu (%d with garbage) garbage %d capacity %d available %d; tiles %d (%d piled) marker %d\n",
                    r ? r->bounds.x0 : -1, r ? r->bounds.z0 : -1, r ? r->bounds.x1 : -1, r ? r->bounds.z1 : -1, cells, zone->Connected() ? 1 : 0,
                    zone->Heaps().size(), withGarbage, garbage, total, available, tiles, full, zone->Zot() ? 1 : 0);
    }
}

}  // namespace native
