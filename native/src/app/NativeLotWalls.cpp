// The lot retaining walls' city services: the wall occupant's (sc4::ILotRetainingWallHost: its models, the lot manager, the terrain
// vertices, the occupant manager, the wall exemplars, the renderer) and the wall model's (sc4::ILotRetainingWallModelHost: the lots,
// the terrain, the city lighting, the textures). With the generated stubs the walls of a city file loaded but never built a model.
#include <cstdio>
#include <cstdlib>
#include <list>
#include <vector>

#include "sc4/Render3D.h"   // first: it declares a global cISC4OccupantFilter that clashes with sc4's when included later
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeLighting.h"
#include "app/NativeLotConstruction.h"
#include "app/NativeLotSerial.h"
#include "app/NativeModelMaker.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZRandom.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/DBSegment.h"
#include "sc4/Exemplar.h"
#include "sc4/FoundationModel.h"
#include "sc4/LightingManager.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotRetainingWallModel.h"
#include "sc4/LotRetainingWallOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "tools/city_probe_plain.h"

namespace native {
sc4::cSC4BuildingDevelopmentSimulator* NativeBuildingDevelopmentSimulator();   // app/NativeBuildingDevelopment.cpp

namespace {
using namespace sc4;

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
cSC4ZoneDeveloper* Lots() { return Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4BuildingDevelopmentSimulator* BuildingDevelopment() { return NativeBuildingDevelopmentSimulator(); }
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
SC4Rect<int> RectOf(const SC4CellRect& r) { return SC4Rect<int>{r.x0, r.z0, r.x1, r.z1}; }
SC4CellRect CellRectOf(const SC4Rect<int>& r) { return SC4CellRect{r.x0, r.z0, r.x1, r.z1}; }
bool Trace() { static const bool on = std::getenv("SC4_WALLDEBUG") != nullptr; return on; }

// ---- the wall occupant's services -----------------------------------------------------------------------------------------------
class WallHost final : public ILotRetainingWallHost {
public:
    ILotRetainingWallModel* CreateRetainingWallModel() override { return new cSC4LotRetainingWallModel(); }   // reference count 0
    cSC4Lot* LotAtCell(int x, int z) override { cSC4ZoneDeveloper* d = Lots(); return d ? d->GetLot(x, z, false) : nullptr; }   // 0x28
    bool EdgeCellRect(const SC4CellRect& lotRect, int edgeMask, SC4CellRect& out) override {   // lot manager 0x60
        cSC4ZoneDeveloper* d = Lots();
        SC4Rect<int> cells{};
        if (!d || !d->GetNeighborCellsAtEdgeOfLot(RectOf(lotRect), edgeMask, cells)) return false;
        out = CellRectOf(cells);
        return true;
    }
    void EdgeTerrainRect(const SC4CellRect& lotRect, int edgeMask, SC4CellRect& out) override {   // lot manager 0x58
        cSC4ZoneDeveloper* d = Lots();
        SC4Rect<int> vertices{};
        if (d) d->GetVerticesAtEdgeOfLot(RectOf(lotRect), edgeMask, vertices);
        out = CellRectOf(vertices);
    }
    bool LotsAround(cSC4Lot* lot, std::list<cRZAutoRefCount<cSC4Lot>>& out) override {   // lot manager 0x40 (lot, list, 0xf)
        cSC4ZoneDeveloper* d = Lots();
        return d && d->GetLotsSurroundingLot(lot, out, 0xf) > 0;
    }
    float TerrainAltitudeAtVertex(int x, int z) override { cSTETerrain* t = TerrainOf(); return t ? t->GetAltitudeAtVertex(x, z) : 0.0f; }   // 0x70
    bool FirstOccupantAtCell(cISC4Occupant*& out, int x, int z, std::uint32_t type) override {   // occupant manager 0x100
        out = nullptr;
        cISC4OccupantManager* o = Occupants();
        return o && o->GetFirstOccupantByStandardCityCell(out, x, z, type) && out;
    }
    void InsertOccupant(cISC4Occupant* occupant) override { if (cISC4OccupantManager* o = Occupants()) o->InsertOccupant(occupant, 0); }   // 0xa0
    void RemoveOccupant(cISC4Occupant* occupant) override { if (cISC4OccupantManager* o = Occupants()) o->RemoveOccupant(occupant, true, 0); }   // 0xa8
    // cSC4BuildingDevelopmentSimulator::LoadLotRetainingWallProperties (i386 0x000fc712): the wall type's exemplar (its own type when
    // none is given) becomes the occupant's default exemplar.
    bool LoadRetainingWallProperties(cISC4LotRetainingWallOccupant* wall, std::uint32_t type) override {
        cSC4BuildingDevelopmentSimulator* bds = BuildingDevelopment();
        auto* occupant = wall ? dynamic_cast<cSC4LotRetainingWallOccupant*>(wall) : nullptr;
        if (!bds || !occupant || !SL::spRM) return false;
        if (type == 0) type = occupant->GetRetainingWallType();
        cGZPersistResourceKey key;
        if (!bds->GetLotRetainingWallKeyFromType(type, key)) return false;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return false;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        occupant->SetDefaultExemplar(exemplar);
        exemplar->Release();
        return true;
    }
    bool RendererAvailable() override { return CityRenderer() != nullptr; }
    bool RendererHasModel(sc4::cIS3DModelInstance* model) override {   // 0xd8
        cSC43DRender* r = CityRenderer();
        return r && model && r->IsModelInserted(reinterpret_cast<s3d::cIS3DModelInstance*>(model));
    }
    void RendererAddModel(sc4::cIS3DModelInstance* model) override {   // 0xe0
        if (cSC43DRender* r = CityRenderer(); r && model) r->InsertModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model));
    }
    void RendererRemoveModel(sc4::cIS3DModelInstance* model) override {   // 0xe8
        if (cSC43DRender* r = CityRenderer(); r && model) r->RemoveModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model));
    }
    bool LoadTextureExemplar(cISCResExemplar*& out) override {   // spRM GetResource({0x6534284a, 0x88cd66e9, 1}, 0xa52160f5)
        out = nullptr;
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0x88cd66e9, 1}, 0xa52160f5, &raw, 0, nullptr) || !raw) return false;
        out = static_cast<cISCResExemplar*>(raw);
        return true;
    }
    const std::vector<std::uint32_t>& RetainingWallIds(cSC4Lot* lot) override {   // lot configuration 0xe0 GetRetainingWallIDArray
        static std::vector<std::uint32_t> none;
        cSC4LotConfiguration* configuration = lot ? NativeLotConfiguration(lot->GetLotConfiguration()) : nullptr;
        return configuration ? configuration->GetRetainingWallIDArray() : none;
    }
    std::uint32_t RandomBelow(std::uint32_t range) override { static cRZRandom random(0x5eed); return range ? random.RandomUint32Uniform(range) : 0; }
    bool WriteLotReference(cIGZOStream& stream, cSC4Lot* lot) override { return SC4WriteGZSerializable(stream, LotSerializable(lot)); }
    // SC4ReadGZSerializable(stream, 0xe818fced): the lot of the city file; one reference on the plain lot behind it. The plain lot's
    // adapter answers its serializable view (0xe4fda3d4) and the plain object, not the cISC4Lot id, so the read asks for the former.
    cSC4Lot* ReadLotReference(cIGZIStream& stream) override {
        cIGZUnknown* adapter = nullptr;
        SC4ReadGZSerializable(stream, 0xe4fda3d4, reinterpret_cast<void**>(&adapter), false);
        void* plain = nullptr;
        if (!adapter || !adapter->QueryInterface(kGZIID_PlainObject, &plain) || !plain) {
            if (Trace()) std::fprintf(stderr, "walls: lot reference -> %s\n", adapter ? "not a lot" : "none");
            if (adapter) adapter->Release();
            return nullptr;
        }
        auto* lot = static_cast<cSC4Lot*>(plain);
        lot->AddRef();
        adapter->Release();
        return lot;
    }
} g_wallHost;

// ---- the wall model's services ----------------------------------------------------------------------------------------------------
class WallLots final : public ILotRetainingWallLotManager {
public:
    cSC4Lot* LotAtCell(int x, int z) override {
        cSC4ZoneDeveloper* d = Lots();
        cSC4Lot* lot = d ? d->GetLot(x, z, false) : nullptr;
        static int shown = 0;
        if (Trace() && shown < 12) {
            ++shown;
            cSTETerrain* t = TerrainOf();
            std::fprintf(stderr, "walls: model lot at %d,%d -> %s altitude %.2f terrain %.2f\n", x, z, lot ? "found" : "none", lot ? lot->GetAltitude() : 0.0f,
                         t ? t->GetAltitudeAtVertex(x, z) : 0.0f);
        }
        return lot;
    }
} g_wallLots;

class WallTerrain final : public ILotRetainingWallTerrain {
public:
    int CellCountX() const override { cSTETerrain* t = TerrainOf(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CellCountZ() const override { cSTETerrain* t = TerrainOf(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    float AltitudeAtVertex(int x, int z) const override { cSTETerrain* t = TerrainOf(); return t ? t->GetAltitudeAtVertex(x, z) : 0.0f; }
} g_wallTerrain;

// The city lighting manager's colour for a surface facing `normal` (vslot 0xb8); a fixed sun before the city is lit.
class WallLighting final : public ILotRetainingWallLighting {
public:
    void GetColor(const cS3DVector3&, const cS3DVector3& normal, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        if (cSC4LightingManager* lighting = LightingManager()) {
            lighting->GetColor(normal, r, g, b);
            return;
        }
        float dot = normal.x * -0.45f + normal.y * 0.78f + normal.z * 0.43f;
        if (dot < 0.35f) dot = 0.35f;
        if (dot > 1.0f) dot = 1.0f;
        r = g = b = static_cast<std::uint8_t>(dot * 255.0f);
    }
} g_wallLighting;

class WallTexture final : public ILotRetainingWallTextureBinding {
public:
    explicit WallTexture(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }
private:
    std::uint32_t id_;
};

class WallTextures final : public ILotRetainingWallTextures {
public:
    // cIS3DTextureBindingFactory CreateTexture(key): the FSH texture of the key's group / instance; null when it is missing
    ILotRetainingWallTextureBinding* CreateTexture(const SC4TextureBindingKey& key) override {
        const std::uint32_t id = FshTextureId(key.group, key.instance);
        if (Trace()) std::fprintf(stderr, "walls: texture %08x/%08x -> %u\n", key.group, key.instance, id);
        return id ? new WallTexture(id) : nullptr;
    }
} g_wallTextures;

class WallModelHost final : public ILotRetainingWallModelHost {
public:
    ILotRetainingWallLotManager* LotManager() override { return &g_wallLots; }
    ILotRetainingWallTerrain* Terrain() override { return &g_wallTerrain; }
    ILotRetainingWallLighting* Lighting() override { return &g_wallLighting; }
    ILotRetainingWallTextures* Textures() override { return &g_wallTextures; }
    void TouchRendererDrawContext() override {}   // the original asks for the draw context and drops the answer
    void GetVectorForEdge(float* v, int edge) override { SC4ViewUtilities::GetVectorForEdge(v, edge); }
} g_wallModelHost;

// ---- the foundation model's services (sc4::IFoundationModelHost) ------------------------------------------------------------
class FoundationTexture final : public IFoundationTextureBinding {
public:
    explicit FoundationTexture(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }
private:
    std::uint32_t id_;
};

class FoundationModelHost final : public IFoundationModelHost {
public:
    // cS3DTextureBindingFactory::GetBinding(instance): {0x7ab50e44, 0x1abe787d, instance}
    IFoundationTextureBinding* GetBinding(std::uint32_t instance) override {
        const std::uint32_t id = FshTextureId(0x1abe787d, instance);
        if (Trace()) std::fprintf(stderr, "foundations: texture %08x -> %u\n", instance, id);
        return id ? new FoundationTexture(id) : nullptr;
    }
    void VisibleEdges(int& edgeA, int& edgeB) override {
        cSC43DRender* render = CityRenderer();
        if (const SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) view->GetVisibleEdges(edgeA, edgeB);
    }
    void LightColor(const float normal[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        g_wallLighting.GetColor(cS3DVector3{}, cS3DVector3{normal[0], normal[1], normal[2]}, r, g, b);
    }
    int TerrainCellCountX() override { cSTETerrain* t = TerrainOf(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    float TerrainAltitude(int vertex) override { cSTETerrain* t = TerrainOf(); return t ? t->GetAltitude(vertex) : 0.0f; }
    float TerrainWaterLevel(int vertex) override { cSTETerrain* t = TerrainOf(); return t ? t->GetWaterLevel(vertex) : 0.0f; }
} g_foundationModelHost;

}  // namespace

// Kit command `walls`: the retaining wall occupants, the edges they wall and the models their placeable objects hold.
void NativeWallReport() {
    cSC4City* city = HostCity();
    cISC4OccupantManager* o = Occupants();
    if (!city || !o) { std::printf("walls: no city\n"); return; }
    const int lo[2] = {0, 0}, hi[2] = {city->CellCountX() - 1, city->CellCountZ() - 1};
    OccupantList list;
    o->GetOccupantsByStandardCityCells(list, lo, hi, kOccupantTypeLotRetainingWall, 0xffffffffu);
    int withLot = 0, edges = 0, models = 0, shown = 0;
    for (cISC4Occupant* occupant : list) {
        auto* wall = dynamic_cast<cSC4LotRetainingWallOccupant*>(occupant);
        if (wall) {
            if (wall->GetLot()) ++withLot;
            for (int e = 0; e < 4; ++e) edges += (wall->GetEdgeFlags() >> e) & 1;
            cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject();
            const int count = placeable ? placeable->GetModelCount() : 0;
            models += count;
            if (shown++ < 6) {
                float p[3] = {};
                occupant->GetPosition(p);
                std::int32_t r[4] = {-1, -1, -1, -1};
                if (wall->GetLot()) wall->GetLot()->GetBoundingRect(r);
                cSC4ZoneDeveloper* d = Lots();
                cSC4Lot* here = d ? d->GetLot(int(p[0] / 16.0f), int(p[2] / 16.0f), false) : nullptr;
                std::printf("walls: at %.0f,%.0f edges %x models %d lot %d rect %d,%d..%d,%d, lot manager has %s at the cell\n", p[0], p[2],
                            wall->GetEdgeFlags(), count, wall->GetLot() ? 1 : 0, r[0], r[1], r[2], r[3],
                            !here ? "none" : here == wall->GetLot() ? "the same lot" : "another lot");
            }
        }
    }
    std::printf("walls: %zu occupants, %d with a lot, %d walled edges, %d models\n", list.size(), withLot, edges, models);
    o->ReleaseOccupantList(list);
    // the foundations (occupant type 0x68fd0c69) and their models
    OccupantList foundations;
    o->GetOccupantsByStandardCityCells(foundations, lo, hi, 0x68fd0c69u, 0xffffffffu);
    int foundationModels = 0, firstShown = 0;
    for (cISC4Occupant* occupant : foundations) {
        cISC43DPlaceableObject* placeable = occupant->GetPlaceableObject();
        const int count = placeable ? placeable->GetModelCount() : 0;
        foundationModels += count;
        if (firstShown++ < 3) {
            float mn[3] = {}, mx[3] = {};
            occupant->GetBoundingBox(mn, mx);
            std::printf("foundations: box %.0f,%.1f,%.0f..%.0f,%.1f,%.0f models %d\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], count);
        }
    }
    std::printf("foundations: %zu occupants, %d models\n", foundations.size(), foundationModels);
    o->ReleaseOccupantList(foundations);
}

// The lot developer's foundations ask for a lot's retaining walls and for its neighbours' (cSC4LotDeveloper::AddLotFoundation); ClearLot
// removes them (create false).
void NativeAddRetainingWallOccupant(sc4::cSC4Lot* lot, bool create) { cSC4LotRetainingWallOccupant::AddRetainingWallOccupant(lot, create); }
void NativeUpdateRetainingWalls(sc4::cSC4Lot* lot) { cSC4LotRetainingWallOccupant::UpdateRetainingWalls(lot); }
void NativeUpdateSurroundingRetainingWalls(sc4::cSC4Lot* lot) { cSC4LotRetainingWallOccupant::UpdateSurroundingRetainingWalls(lot); }

// The model maker's retaining wall case (cSC4ModelMaker::AddModels, i386 0x0024aee0).
bool NativeAddRetainingWallModels(sc4::cISC4Occupant* occupant, bool create, bool addToRenderer) {
    return cSC4LotRetainingWallOccupant::AddRetainingWallModels(occupant, create, addToRenderer);
}

// The retaining walls' real hosts (replace the generated stubs').
void InstallLotRetainingWallHosts() {
    SetLotRetainingWallHost(&g_wallHost);
    SetLotRetainingWallModelHost(&g_wallModelHost);
    SetFoundationModelHost(&g_foundationModelHost);
}

}  // namespace native
