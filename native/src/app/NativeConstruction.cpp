// The construction occupant on the native application (cSC4ConstructionOccupant: the scaffold, the corner posts, the planks, the
// tower crane, the ground / roof props and the dirt lot texture that take a new building through its stages). Replaces the lot
// developer's "completed at once" stand-in: StartConstruction now creates the occupant, gives it the lot and inserts it into the
// occupant manager (i386 0x000f4d5c..0x000f4dd0), and the occupant takes the lot to state 3 when the scaffold is down.
//
// PARTIAL (each named where it is answered): the construction crew automata (CreateAutomaton has no automata manager natively,
// so no crew walks the site) and the render detail level (high).
#include <cstdio>
#include <cstdlib>

#include "sc4/Render3D.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeLighting.h"
#include "app/NativeConstruction.h"
#include "app/NativeLotSerial.h"
#include "app/NativeModelMaker.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZRandom.h"
#include "s3d/ModelInstance.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/ConstructionOccupant.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/LightingManager.h"
#include "sc4/LotDeveloper.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "app/NativeAnimationTicks.h"

namespace native {
namespace {
using namespace sc4;

constexpr std::uint32_t kGZIID_cIS3DResModel = 0x9ad0e7e3;

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
cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
cSTETerrain* Terrain() {
    cIGZUnknown* object = Component(CityComponent::Terrain);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSC4PropManager* Props() {
    cIGZUnknown* object = Component(CityComponent::PropManager);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4PropManager*>(raw) : nullptr;
}
cSC4LotDeveloper* LotDeveloper() { return Component<cSC4LotDeveloper>(CityComponent::LotDeveloper, kGZCLSID_cSC4LotDeveloper); }

// cISC4Lot as the occupant sees it (vslots 0x70 / 0x110 / 0x180). One adapter per lot reference the occupant holds; the
// serializable view is the lot's own adapter (NativeLotSerial), so a save writes the lot once.
class ConstructionLot final : public IConstructionLot {
public:
    explicit ConstructionLot(cSC4Lot* lot) : lot_(lot) { lot_->AddRef(); }
    ~ConstructionLot() { lot_->Release(); }
    cSC4Lot* Lot() const { return lot_; }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZSerializable) {
            cIGZUnknown* serial = LotSerializable(lot_);
            return serial && serial->QueryInterface(iid, out);
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    void GetCellRect(std::int32_t rect[4]) override { lot_->GetBoundingRect(rect); }
    void SetState(std::uint32_t state, std::uint32_t wealth, std::uint8_t reason) override { lot_->SetState(state, wealth, reason); }
    cISC4BuildingOccupant* GetBuilding() override { return static_cast<cISC4BuildingOccupant*>(lot_->GetBuilding()); }
private:
    cSC4Lot* lot_;
    std::uint32_t refs_ = 1;
};

// cS3DTextureBinding: the driver texture of one FSH.
class Texture final : public IConstructionTexture {
public:
    explicit Texture(std::int32_t id) : id_(id) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    std::int32_t TextureId() const override { return id_; }
private:
    std::int32_t id_;
    std::uint32_t refs_ = 1;
};

// A prop occupant (cISC4PropOccupant) for the props the occupant places.
class Prop final : public IConstructionProp {
public:
    explicit Prop(cSC4PropOccupant* prop) : prop_(prop) {}   // takes the creator's reference
    ~Prop() { prop_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return prop_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    cISC4Occupant* AsOccupant() override { return prop_; }
    void SetOrientation(std::int32_t orientation) override { prop_->SetOrientation(orientation); }
private:
    cSC4PropOccupant* prop_;
    std::uint32_t refs_ = 1;
};

// A visual-effect prop (cSC4PropOccupantVisualEffect): the dust cloud and the welding sparks.
class Effect final : public IConstructionEffect {
public:
    explicit Effect(cSC4PropOccupantVisualEffect* effect) : effect_(effect) {}   // takes the creator's reference
    ~Effect() { effect_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return effect_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    bool Init() override { return effect_->Init(); }
    void SetPosition(const float* xyz) override { effect_->SetPosition(xyz); }
    void SetEffectType(const char* name) override { effect_->SetEffectType(name); }
    bool StartEffect(bool instant) override { return effect_->StartEffect(instant); }
    bool StopEffect(bool instant) override { return effect_->StopEffect(instant); }
    cISC4Occupant* AsOccupant() override { return effect_->AsOccupant(); }
private:
    cSC4PropOccupantVisualEffect* effect_;
    std::uint32_t refs_ = 1;
};

class Renderer final : public IConstructionRenderer {
public:
    bool IsModelInserted(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->IsModelInserted(model); }
    bool InsertModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->InsertModel(model); }
    bool RemoveModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->RemoveModel(model); }
} g_renderer;

class Host final : public IConstructionOccupantHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override { AnimationTickServerView()->AddNotification(target, static_cast<std::uint32_t>(channel)); }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override { AnimationTickServerView()->RemoveNotification(target, static_cast<std::uint32_t>(channel)); }
    cRZRandom* Random() override { static cRZRandom random(0xc0u); return &random; }
    cISCPropertyHolder* ConstructionSettings() override {
        if (settings_ || !SL::spRM) return settings_ ? settings_->AsISCPropertyHolder() : nullptr;
        const cGZPersistResourceKey key{kConstructionPropertyType, kConstructionPropertyGroup, kConstructionPropertyInstance};
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, kConstructionPropertyIID, &raw, 0, nullptr) || !raw) return nullptr;
        settings_ = static_cast<cISCResExemplar*>(raw);   // kept for the process, like the class statics it fills
        return settings_->AsISCPropertyHolder();
    }
    s3d::cIS3DModelInstance* CreateModelInstance(const cGZPersistResourceKey& key, std::uint32_t) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, kGZIID_cIS3DResModel, &resource, 0, nullptr) || !resource) return nullptr;
        auto* res = static_cast<s3d::cIS3DResModel*>(resource);
        s3d::cIS3DModelInstance* instance = nullptr;
        const bool ok = res->CreateModelInstance(0, &instance);
        res->Release();
        return ok ? instance : nullptr;
    }
    IConstructionTexture* CreateTextureBinding(const cGZPersistResourceKey& key) override {
        return new Texture(static_cast<std::int32_t>(FshTextureId(key.group, key.instance)));
    }
    const sc4::PropProfile* PropProfile(std::uint32_t type) override { cSC4PropManager* p = Props(); return p ? p->GetPropProfilePtr(type) : nullptr; }
    bool CreateProp(std::uint32_t type, IConstructionProp** out) override {
        *out = nullptr;
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = nullptr;
        if (!props || !props->CreateProp(type, prop) || !prop) return false;
        *out = new Prop(prop);
        return true;
    }
    // The lot developer's AddLotProp: the prop is initialised and goes into the occupant manager.
    void AddLotProp(IConstructionLot*, cISC4Occupant* occupant) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupant || !occupants) return;
        if (!occupant->IsInitialized()) occupant->Init();
        occupants->InsertOccupant(occupant, 0);
    }
    void RemoveProp(cISC4Occupant* occupant) override { if (cISC4OccupantManager* o = Occupants(); o && occupant) o->RemoveOccupant(occupant, true, 0); }
    void RemoveAutomaton(IConstructionAutomaton*) override {}
    IConstructionEffect* CreatePropEffect() override {
        auto* effect = new cSC4PropOccupantVisualEffect();
        effect->AddRef();
        effect->Init();
        return new Effect(effect);
    }
    bool CreateLotBaseTextureOccupant(IConstructionLot* lot, cISC4LotBaseTextureOccupant** out) override {
        *out = nullptr;
        cSC4LotDeveloper* developer = LotDeveloper();
        if (!lot || !developer) return false;
        cISC4LotBaseTextureOccupant* tiles = nullptr;
        if (!developer->GetLotBaseTextureOccupant(static_cast<ConstructionLot*>(lot)->Lot(), tiles, true) || !tiles) return false;
        *out = tiles;
        return true;
    }
    cISC4BuildingOccupant* LotBuilding(IConstructionLot* lot) override { return lot ? lot->GetBuilding() : nullptr; }
    IConstructionAutomaton* CreateAutomaton(std::uint32_t, const float[3], std::uint32_t, const cGZPersistResourceKey&, std::uint8_t) override {
        return nullptr;   // PARTIAL: no automata manager natively, so no construction crew
    }
    bool InsertOccupant(cISC4Occupant* occupant, std::uint32_t data) override { cISC4OccupantManager* o = Occupants(); return o && o->InsertOccupant(occupant, data); }
    bool RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t data) override { cISC4OccupantManager* o = Occupants(); return o && o->RemoveOccupant(occupant, shutdown, data); }
    bool MoveOccupant(cISC4Occupant* occupant, bool post) override { cISC4OccupantManager* o = Occupants(); return o && o->MoveOccupant(occupant, post); }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    int TerrainCellCountX() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int TerrainCellCountZ() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    bool TerrainCellHasWater(int x, int z) override {
        cSTETerrain* t = Terrain();
        if (!t) return false;
        for (int dz = 0; dz < 2; ++dz)
            for (int dx = 0; dx < 2; ++dx)
                if (t->GetWaterDepthAtVertex(x + dx, z + dz) > 0.0f) return true;
        return false;
    }
    void CityCellToPosition(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(cellX, cellZ, x, z);
        else { x = static_cast<float>(cellX) * 16.0f; z = static_cast<float>(cellZ) * 16.0f; }
    }
    std::uint32_t SimulatorDay() override {
        cSC4Simulator* simulator = Component<cSC4Simulator>(CityComponent::Simulator, kGZCLSID_cSC4Simulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
    }
    sc4::cISC4OccupantFilter* ViewFilter() override { return nullptr; }   // every occupant is in view (the model maker's filter)
    void ProjectToViewCoordinates(float point[3]) override {
        cSC43DRender* render = CityRenderer();
        if (const SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) view->ProjectPointToViewCoordinates(point);
    }
    void TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box) override { SC4ViewUtilities::TransformBoundingBox(transform, box); }
    int RenderDetailLevel() override { return 3; }   // PARTIAL: the render properties' detail level; high
    // SL::spLightingManager vslot 0xb8; the fixed sun the network pieces are lit with when there is no city lighting yet.
    void LightColourForDirection(const float n[3], std::uint8_t rgb[3]) override {
        if (cSC4LightingManager* lighting = LightingManager()) {
            lighting->GetColor(cS3DVector3{n[0], n[1], n[2]}, rgb[0], rgb[1], rgb[2]);
            return;
        }
        float dot = n[0] * -0.45f + n[1] * 0.78f + n[2] * 0.43f;
        if (dot < 0.35f) dot = 0.35f;
        if (dot > 1.0f) dot = 1.0f;
        rgb[0] = rgb[1] = rgb[2] = static_cast<std::uint8_t>(dot * 255.0f);
    }
    IConstructionRenderer* Renderer() override { return &g_renderer; }
    IConstructionLot* AdoptLot(cIGZUnknown* object) override {
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
        return new ConstructionLot(static_cast<cSC4Lot*>(raw));
    }
private:
    cISCResExemplar* settings_ = nullptr;
} g_host;

}  // namespace

// @ i386 0x000f4d5c (StartConstruction with `construct`): GZCOM creates the occupant (or new), SetLot, then the occupant manager
// inserts its occupant view (+0x28 vslot 0x50, data 0) and the lot developer drops its reference.
void StartNativeConstructionOccupant(sc4::cSC4Lot* lot) {
    if (!lot) return;
    auto* occupant = new cSC4ConstructionOccupant();
    occupant->AddRef();
    auto* constructionLot = new ConstructionLot(lot);
    occupant->SetLot(constructionLot);
    constructionLot->Release();
    if (cISC4OccupantManager* occupants = Occupants()) occupants->InsertOccupant(occupant->AsOccupant(), 0);
    if (std::getenv("SC4_SIMDEBUG")) {
        std::fprintf(stderr, "construction: lot started, building %s, segments %u, crane %d, props %zu+%zu\n", occupant->Building() ? "yes" : "NO",
                     occupant->SegmentCount(), occupant->HasCrane() ? 1 : 0, occupant->GroundProps().size(), occupant->RoofProps().size());
        std::fprintf(stderr, "construction: sides %zu %zu %zu %zu planks %zu girders %zu posts %zu colour %08x height %.2f base %.2f bounds y %.2f..%.2f\n",
                     occupant->SideVertices(0).size(), occupant->SideVertices(1).size(), occupant->SideVertices(2).size(), occupant->SideVertices(3).size(),
                     occupant->Planks().size(), occupant->Girders().size(), occupant->Posts().size(), occupant->Posts().empty() ? 0u : occupant->Posts()[0].colour,
                     occupant->ScaffoldHeight(), occupant->BuildingBaseY(), occupant->Bounds().min[1], occupant->Bounds().max[1]);
    }
    occupant->Release();
}

void InstallConstructionOccupantHost() { sc4::SetConstructionOccupantHost(&g_host); }

}  // namespace native
