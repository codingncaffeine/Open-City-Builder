// The pre-built model network pieces' host (cSC4NetworkOccupantWithPreBuiltModel: tunnel portals and bridge pieces): the
// lighting manager, the view utilities, the terrain, the model maker utility, the model resources, the renderer, the path
// files, and cSC4NetworkOccupantModelInstance (the model instance a piece lays on its tile).
//
// cSC4NetworkOccupantModelInstance (Mac i386 0x004c9c46..0x004ca294) is a cS3DModelInstance with a cS3DModelProcessor:
// Init fills it from the model resource, Deform shears every vertex onto the tile's two triangles (the fill rows), FlipAndLight
// mirrors the triangle order of a flipped piece and lights the model, Clip cuts a bridge pylon at the ground. The port's model
// resources keep their vertices in the resource (one shared V3F_C4UB_T2F array per vertex group), so the instance here keeps
// its own copy of every vertex group, index group and draw list it changes.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "app/NativeCityView.h"
#include "app/NativeDecalOverlay.h"
#include "app/NativeDemolition.h"
#include "app/NativeLighting.h"
#include "app/NativeModelMaker.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cGZPersistResourceKey.h"
#include "sc4/DrawContext.h"
#include "sc4/LightingManager.h"
#include "sc4/OverlayManager.h"
#include "sc4/NetworkOccupantModel.h"
#include "sc4/NetworkOccupantWithPreBuiltModel.h"
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

using namespace sc4;

constexpr std::uint32_t kGZIID_cIS3DResModel = 0x9ad0e7e3;
constexpr std::uint32_t kGZIID_cISC4PathInfo = 0xa9667793;
// The native model resource's vertex record (NativeModelMaker.cpp ResModel: position, colour, one uv pair).
constexpr std::size_t kVertexStride = 24;

cSTETerrain* Terrain() {
    auto* city = dynamic_cast<cSC4City*>(SL::spCity);
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

SC4ViewUtilities* ViewUtilities() {
    cSC43DRender* render = CityRenderer();
    return render ? render->GetViewUtilities() : nullptr;
}

// cSC4NetworkOccupantModelInstance.
class OccupantModelInstance final : public s3d::cS3DModelInstance, public INetworkOccupantModelInstance {
public:
    s3d::cIS3DModelInstance* ModelInstance() override { return this; }

    // i386 0x004c9d7a: the meshes and materials of the resource's instance `param` go into this instance, then the bounds.
    bool Init(s3d::cIS3DResModel* model, std::uint32_t param) override {
        s3d::cIS3DModelInstance* source = nullptr;
        if (!model || !model->CreateModelInstance(param, &source) || !source) return false;
        auto* concrete = dynamic_cast<s3d::cS3DModelInstance*>(source);
        if (!concrete) {
            source->Release();
            return false;
        }
        const std::size_t count = concrete->Meshes().size();
        groups_.assign(count, SC4MeshVertexGroup{});
        vertices_.assign(count, {});
        indices_.assign(count, {});
        indexPointers_.assign(count, nullptr);
        draws_.assign(count, {});
        drawLists_.assign(count, SC4MeshDrawList{});
        Meshes().clear();
        Materials().clear();
        for (std::size_t i = 0; i < count; ++i) {
            SC4Mesh mesh;
            std::memcpy(&mesh, concrete->Meshes()[i].data(), sizeof mesh);
            if (mesh.vertices) {
                groups_[i] = *mesh.vertices;
                const auto* bytes = static_cast<const std::uint8_t*>(mesh.vertices->data);
                if (bytes) vertices_[i].assign(bytes, bytes + std::size_t(mesh.vertices->count) * kVertexStride);
                groups_[i].data = vertices_[i].data();
                mesh.vertices = &groups_[i];
            }
            s3d::MeshRecord record{};
            std::memcpy(record.data(), &mesh, sizeof mesh);
            Meshes().push_back(record);
            Materials().push_back(concrete->Materials()[i]);
        }
        source->Release();
        SetModelResource(model);
        RecalculateBounds();
        cS3DModelInstance::GetBounds(bounds_);
        return true;
    }

    // i386 0x004c9dd2: cS3DModelProcessor::SetModel(this), the lighting pass FlipAndLight finishes. The port's renderer lights
    // the models it draws itself, as it does every other model, so the processor is not used here.
    void Prelight() override {}

    // i386 0x004c9df6: every vertex, turned by the piece's rotation (and mirrored in x when flipped) and moved by `offset`, rises
    // by the fill row of the triangle it lies over (the second row where x - z, or x + z for a diagonal piece, is >= 0).
    void Deform(int rotation, bool flip, const cS3DVector3& fillA, const cS3DVector3& fillB, const cS3DVector3& offset, bool diagonal) override {
        bool any = false;
        for (std::size_t m = 0; m < groups_.size(); ++m) {
            std::uint8_t* bytes = vertices_[m].data();
            for (std::size_t v = 0; v < groups_[m].count && bytes; ++v) {
                float p[3];
                std::memcpy(p, bytes + v * kVertexStride, sizeof p);
                float x = p[0], z = p[2];
                if (rotation == 2) {
                    x = -p[0];
                    z = -p[2];
                } else if (rotation == 3) {
                    x = p[2];
                    z = -p[0];
                } else if (rotation == 1) {
                    x = -p[2];
                    z = p[0];
                }
                if (flip) x = -x;
                x -= offset.x;
                z -= offset.z;
                const float side = diagonal ? x + z : x - z;
                const float rise = !(side < 0.0f) ? x * fillB.x + z * fillB.y + fillB.z : x * fillA.x + z * fillA.y + fillA.z;
                p[1] += rise;
                std::memcpy(bytes + v * kVertexStride + 4, &p[1], sizeof p[1]);
                for (int k = 0; k < 3; ++k) {
                    if (!any || p[k] < bounds_.min[k]) bounds_.min[k] = p[k];
                    if (!any || p[k] > bounds_.max[k]) bounds_.max[k] = p[k];
                }
                any = true;
            }
        }
    }

    // i386 0x004c9fa0: a flipped piece reverses each mesh's triangle order (a reversed copy of the index array and the draw
    // ranges counted from the end; a mesh without indices gets an index array of its vertices in reverse), then the model is
    // lit (see Prelight).
    void FlipAndLight(bool flip) override {
        if (!flip) return;
        for (std::size_t m = 0; m < groups_.size(); ++m) {
            SC4Mesh mesh;
            std::memcpy(&mesh, Meshes()[m].data(), sizeof mesh);
            if (!mesh.draws) continue;
            std::vector<std::uint16_t> reversed;
            std::uint32_t total = groups_[m].count;
            if (mesh.indexGroup && *mesh.indexGroup) {
                // The index count is the end of the furthest draw range (the port's index groups carry no length).
                total = 0;
                for (const SC4MeshDrawRecord* d = mesh.draws->begin; d != mesh.draws->end; ++d) total = std::max(total, d->start + d->count);
                const std::uint16_t* source = *mesh.indexGroup;
                reversed.assign(source, source + total);
                std::reverse(reversed.begin(), reversed.end());
            }
            std::vector<SC4MeshDrawRecord> records(mesh.draws->begin, mesh.draws->end);
            // Quirk kept (as the decompile reads it): the "lowest start" starts at INT_MAX and keeps the larger value, so it never comes down
            // and the reversed vertex index array of a mesh without indices is never built (its ranges are reversed only).
            std::int32_t lo = 0x7fffffff, hi = 0;
            for (SC4MeshDrawRecord& d : records) {
                d.start = total - d.start - d.count;
                lo = std::max(lo, static_cast<std::int32_t>(d.start));
                hi = std::max(hi, static_cast<std::int32_t>(d.start + d.count));
            }
            if (reversed.empty() && lo <= hi) {
                for (SC4MeshDrawRecord& d : records) d.start -= static_cast<std::uint32_t>(lo);
                reversed.resize(static_cast<std::size_t>(hi - lo));
                for (std::uint16_t& index : reversed) index = static_cast<std::uint16_t>(--hi);
            }
            indices_[m] = std::move(reversed);
            indexPointers_[m] = indices_[m].data();
            draws_[m] = std::move(records);
            drawLists_[m].begin = draws_[m].data();
            drawLists_[m].end = draws_[m].data() + draws_[m].size();
            mesh.indexGroup = &indexPointers_[m];
            mesh.draws = &drawLists_[m];
            std::memcpy(Meshes()[m].data(), &mesh, sizeof mesh);
        }
    }

    // i386 0x004ca294: the triangles below `lo` are cut away (a bridge pylon standing in water or on the ground; only the
    // bridge pieces, network flag 0x40000, take this path). NOT PORTED: the pylon clip; the model is drawn whole.
    void Clip(const cS3DVector3&, const cS3DVector3&, bool) override {}

    void GetBounds(cS3DBoundingBox& out) override { out = bounds_; }

private:
    std::vector<SC4MeshVertexGroup> groups_;
    std::vector<std::vector<std::uint8_t>> vertices_;
    std::vector<std::vector<std::uint16_t>> indices_;
    std::vector<const std::uint16_t*> indexPointers_;
    std::vector<std::vector<SC4MeshDrawRecord>> draws_;
    std::vector<SC4MeshDrawList> drawLists_;
    cS3DBoundingBox bounds_{{0, 0, 0}, {0, 0, 0}};
};

// The terrain view's overlay manager `index` (SL::TerrainView vslot 0x48: 0 the dynamic land one, 1 the dynamic water one) as the
// pieces' UpdateShadow uses it: AddShadowDecal and RemoveOverlay.
class ShadowDecalLayer final : public IShadowDecalOverlay {
public:
    explicit ShadowDecalLayer(int index) : index_(index) {}
    std::uint32_t AddShadowDecal(const cGZPersistResourceKey& key, const cS3DVector2& position, float height, float rotation, bool flip) override {
        cSTEOverlayManager* manager = DecalOverlayManager(index_);
        return manager ? manager->AddShadowDecal(key, position, height, rotation, flip) : 0xffffffffu;
    }
    void RemoveOverlay(std::uint32_t id) override {
        if (cSTEOverlayManager* manager = DecalOverlayManager(index_)) manager->RemoveOverlay(id);
    }
private:
    int index_;
};
ShadowDecalLayer g_landShadows(0), g_waterShadows(1);

class PreBuiltModelHost final : public IPreBuiltModelOccupantHost {
public:
    // cSC4NetworkOccupant::Draw: the tile's own quad (cSC4NetworkOccupantModel draws it in the port).
    void DrawOccupant(cSC4NetworkOccupant& occupant, SC4DrawContext* context, bool highlight) override {
        if (!context) return;
        cSC4NetworkOccupantModel model(occupant);
        model.Draw(context, highlight);
    }
    // cSC4NetworkOccupant::GetModelInstances: the occupant's own model instance (its cSC4NetworkOccupantModel stand-in), lit
    // as a piece entering the city is.
    void GetOccupantModelInstances(cSC4NetworkOccupant& occupant, int, int, s3d::cIS3DModelInstance** out, int capacity, int& count) override {
        if (count >= capacity) return;
        occupant.UpdateLighting(NativeNetworkLighting());
        auto* model = new cSC4NetworkOccupantModel(occupant);
        model->AddRef();
        out[count++] = model;
    }
    void UpdateOccupantLighting(cSC4NetworkOccupant& occupant) override { occupant.UpdateLighting(NativeNetworkLighting()); }
    bool IsNight() override { cSC4LightingManager* lighting = LightingManager(); return lighting && lighting->IsNight(); }
    void GetSunDirection(cS3DVector3& out) override {
        cSC4LightingManager* lighting = LightingManager();
        out = lighting ? lighting->SunDirection() : cS3DVector3{0.0f, 1.0f, 0.0f};
    }
    void GetZoomAndRotation(int& zoom, int& rotation) override {
        zoom = rotation = 0;
        if (SC4ViewUtilities* v = ViewUtilities()) v->GetZoomAndRotation(zoom, rotation);
    }
    float GetObjectYRotationAngle(int rotation) override { return SC4ViewUtilities::GetObjectYRotationAngle(rotation); }
    float ViewRotationAngle() override { SC4ViewUtilities* v = ViewUtilities(); return v ? v->RotationAngle() : 0.0f; }
    std::int32_t ShadowViewMode() override { return NativeShadowRenderQuality(); }
    bool CellHasWater(std::uint32_t x, std::uint32_t z) override { cSTETerrain* t = Terrain(); return t && t->CellHasWater(x, z); }
    float TerrainAltitudeAtVertex(int x, int z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitudeAtVertex(x, z) : 0.0f; }
    float WaterLevel(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetWaterLevel(x, z) : 0.0f; }
    bool WaterSurfaceDisplayed() override { cSTETerrain* t = Terrain(); return t && t->GetWaterSurfaceDisplayState(); }
    // spCity vslot 0x150 -> terrain vslot 0x108 -> the terrain view's overlay managers 0 (land) and 1 (water); none without a city view.
    bool GetShadowOverlays(IShadowDecalOverlay** land, IShadowDecalOverlay** water) override {
        const bool present = DecalOverlayManager(0) && DecalOverlayManager(1);
        if (land) *land = present ? &g_landShadows : nullptr;
        if (water) *water = present ? &g_waterShadows : nullptr;
        return present;
    }
    std::uint32_t MapShadowTexture(std::uint32_t instance, std::uint8_t& rotation, std::uint32_t flags, std::uint32_t raised) override {
        return NativeMapShadowTexture(instance, rotation, flags, raised);
    }
    std::uint32_t GetModelInstanceID(cISC4Occupant* occupant, int zoom, int rotation, int rotationAndFlip, bool flag, std::uint32_t* a, std::uint32_t* b) override {
        return NativeModelInstanceID(occupant, zoom, rotation, rotationAndFlip, flag, a, b);
    }
    s3d::cIS3DResModel* CreateModelResource(const cGZPersistResourceKey& key) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, kGZIID_cIS3DResModel, &raw, 0, nullptr) || !raw) return nullptr;
        return static_cast<s3d::cIS3DResModel*>(raw);
    }
    INetworkOccupantModelInstance* CreateOccupantModelInstance() override { return new OccupantModelInstance(); }
    bool HasModelInstance(s3d::cIS3DModelInstance* instance) override { cSC43DRender* r = CityRenderer(); return r && instance && r->IsModelInserted(instance); }
    void AddModelInstanceToRenderer(s3d::cIS3DModelInstance* instance, int change) override {
        if (cSC43DRender* r = CityRenderer(); r && instance) r->ChangeModel(instance, change);
    }
    cISC4PathInfo* GetPathInfoResource(const cGZPersistResourceKey& key) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, kGZIID_cISC4PathInfo, &raw, 0, nullptr) || !raw) return nullptr;
        return static_cast<cISC4PathInfo*>(raw);
    }
    std::uint32_t MapPath(std::uint32_t pathId) override {
        INetworkOccupantPathHost* paths = NetworkOccupantPathHost();   // the network manager's MapPath
        return paths ? paths->MapPath(pathId) : pathId;
    }
};

}  // namespace

int NativePreBuiltModelInstances(sc4::cISC4Occupant* occupant, int zoom, int rotation, s3d::cIS3DModelInstance** out, int capacity) {
    auto* prebuilt = dynamic_cast<sc4::cSC4NetworkOccupantWithPreBuiltModel*>(occupant);
    if (!prebuilt) return -1;
    int count = 0;
    prebuilt->GetModelInstances(zoom, rotation, out, capacity, count);
    return count;
}

void InstallPreBuiltModelOccupantHost() {
    static PreBuiltModelHost host;
    sc4::SetPreBuiltModelOccupantHost(&host);
}

}  // namespace native
