#include "app/NativeModelMaker.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "sc4/Render3D.h"
#include "app/NativeShadowOverlay.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "render/gd/IGZGDriver.h"
#include "s3d/FSH.h"
#include "s3d/MaterialManager.h"
#include "s3d/ModelInstance.h"
#include "s3d/S3DModel.h"
#include "sc4/CellTextureModel.h"
#include "sc4/DrawContext.h"
#include "sc4/LotBaseTextureOccupant.h"
#include "sc4/ModelMaker.h"
#include "sc4/ModelMakerUtility.h"
#include "sc4/NetworkOccupantModel.h"
#include "sc4/OccupantManager.h"
#include "sc4/PlaceableObject.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {

constexpr std::uint32_t kFshType = 0x7ab50e44;
constexpr std::uint32_t kModelTextureGroup = 0x1abe787d;   // the S3D textures of buildings, props and flora
constexpr std::uint32_t kGZIID_cIS3DResModel = 0x9ad0e7e3;
constexpr std::uint32_t kGZCLSID_cS3DResModelFactory = 0x5ad0e81d;

gd::cIGZGDriver* g_driver = nullptr;
int g_modelsLoaded = 0, g_modelsFailed = 0, g_instances = 0, g_inserted = 0;

// ---- texture bindings -------------------------------------------------------------------------------------------
// A material layer points at a texture binding whose first word points at the texture record that starts with the
// driver texture id; one binding per FSH, kept for the life of the process like the binding factory's cache.
struct TextureBinding {
    const std::uint32_t* record = &id;
    std::uint32_t id = 0;
};
const TextureBinding* TextureRecord(std::uint32_t group, std::uint32_t instance) {
    static std::map<std::pair<std::uint32_t, std::uint32_t>, std::unique_ptr<TextureBinding>> cache;
    auto& slot = cache[{group, instance}];
    if (slot) return slot.get();
    slot = std::make_unique<TextureBinding>();
    std::vector<std::uint8_t> bytes;
    if (!g_driver) return slot.get();
    if (!LoadResourceBytes(kFshType, kModelTextureGroup, instance, bytes) && !LoadResourceBytes(kFshType, group, instance, bytes)) {
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "s3d: texture %08x (model group %08x) not found\n", instance, group);
        return slot.get();
    }
    s3d::FshFile file;
    std::vector<std::uint8_t> rgba;
    int w = 0, h = 0;
    if (!file.Open(bytes.data(), bytes.size()) || file.FrameCount() == 0 || !file.DecodeRgba8(0, rgba, w, h)) return slot.get();
    for (std::size_t p = 0; p + 3 < rgba.size(); p += 4) std::swap(rgba[p], rgba[p + 2]);   // RGBA -> BGRA
    const std::uint32_t id = g_driver->CreateTexture(4, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1, 1);
    g_driver->LoadTextureLevel(id, 0, 0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 3, 1, static_cast<std::uint32_t>(w), rgba.data());
    slot->id = id;
    return slot.get();
}

// The lot base textures (group 0x0986135e): every frame of the FSH as a driver texture, with the alpha flag the
// binding factory reports (any frame with translucent pixels).
constexpr std::uint32_t kLotTextureGroup = 0x0986135e;
struct LotTexture {
    std::vector<std::uint32_t> ids;
    sc4::CellTextureBinding binding;
};
const sc4::CellTextureBinding* LotTextureBinding(std::uint32_t instance) {
    static std::map<std::uint32_t, std::unique_ptr<LotTexture>> cache;
    auto& slot = cache[instance];
    if (!slot) {
        slot = std::make_unique<LotTexture>();
        std::vector<std::uint8_t> bytes;
        s3d::FshFile file;
        if (g_driver && LoadResourceBytes(kFshType, kLotTextureGroup, instance, bytes) && file.Open(bytes.data(), bytes.size())) {
            for (std::uint32_t i = 0; i < file.FrameCount(); ++i) {
                std::vector<std::uint8_t> rgba;
                int w = 0, h = 0;
                if (!file.DecodeRgba8(i, rgba, w, h)) continue;
                for (std::size_t p = 0; p + 3 < rgba.size(); p += 4) {
                    std::swap(rgba[p], rgba[p + 2]);
                    if (rgba[p + 3] != 0xff) slot->binding.hasAlpha = true;
                }
                const std::uint32_t id = g_driver->CreateTexture(4, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1, 1);
                g_driver->LoadTextureLevel(id, 0, 0, 0, static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 3, 1, static_cast<std::uint32_t>(w), rgba.data());
                slot->ids.push_back(id);
            }
        }
        slot->binding.frames = slot->ids.empty() ? nullptr : slot->ids.data();
        slot->binding.frameCount = static_cast<std::uint32_t>(slot->ids.size());
    }
    return slot->ids.empty() ? nullptr : &slot->binding;
}

// ---- the S3D model resource ------------------------------------------------------------------------------------
// The parsed model converted to what SC4DrawContext::RenderMesh draws: every vertex group as V3F_C4UB_T2F, the index
// groups, one draw list per primitive group and one cS3DMaterial per material (frame 0 of every mesh).
class ResModel final : public cGZPersistResource, public s3d::cIS3DResModel {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIS3DResModel) {
            *out = static_cast<s3d::cIS3DResModel*>(this);
            AddRef();
            return true;
        }
        return cGZPersistResource::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZPersistResource::AddRef(); }
    std::uint32_t Release() override { return cGZPersistResource::Release(); }

    bool CreateModelInstance(std::uint32_t, s3d::cIS3DModelInstance** out) override {
        *out = nullptr;
        if (meshes_.empty()) return false;
        auto* instance = new s3d::cS3DModelInstance();
        for (std::size_t i = 0; i < meshes_.size(); ++i) {
            s3d::MeshRecord mesh{};
            std::memcpy(mesh.data(), &meshes_[i], sizeof(SC4Mesh));
            instance->Meshes().push_back(mesh);
            s3d::MaterialRecord material{};
            std::memcpy(material.data(), &materials_[i], sizeof(s3d::cS3DMaterial));
            instance->Materials().push_back(material);
        }
        instance->SetModelResource(this);
        ++g_instances;
        instance->AddRef();
        *out = instance;
        return true;
    }
    void ComputeBounds(cS3DBoundingBox& out) override { out = bounds_; }
    int FrameCount() const override { return static_cast<int>(frameMeshes_.size()); }
    int FrameRate() const override { return frameRate_; }
    // Loads frame `frame` into an instance of this model (the instance's mesh / material records are frame 0's layout).
    bool SelectFrame(s3d::cIS3DModelInstance* instance, int frame) override {
        auto* concrete = dynamic_cast<s3d::cS3DModelInstance*>(instance);
        if (!concrete || frame < 0 || frame >= static_cast<int>(frameMeshes_.size())) return false;
        const std::vector<SC4Mesh>& meshes = frameMeshes_[static_cast<std::size_t>(frame)];
        if (concrete->Meshes().size() != meshes.size()) return false;
        for (std::size_t i = 0; i < meshes.size(); ++i) {
            std::memcpy(concrete->Meshes()[i].data(), &meshes[i], sizeof(SC4Mesh));
            std::memcpy(concrete->Materials()[i].data(), &frameMaterials_[static_cast<std::size_t>(frame)][i], sizeof(s3d::cS3DMaterial));
        }
        return true;
    }
    bool GetShadowMeshes(std::vector<s3d::ShadowMeshData>& out) override {
        out.clear();
        for (std::size_t i = 0; i < meshes_.size(); ++i) {
            const SC4MeshVertexGroup* group = meshes_[i].vertices;
            s3d::ShadowMeshData data;
            data.textureLayers = materials_[i].layerCount;
            data.texture = materials_[i].layers[0].texture;
            const auto* bytes = static_cast<const std::uint8_t*>(group->data);
            for (std::uint32_t v = 0; v < group->count; ++v) {
                float f[6];
                std::memcpy(f, bytes + std::size_t(v) * 24, 12);
                std::memcpy(f + 4, bytes + std::size_t(v) * 24 + 16, 8);
                data.points.push_back({f[0], f[1], f[2]});
                data.uvs.push_back({f[4], f[5]});
            }
            out.push_back(std::move(data));
        }
        return !out.empty();
    }

    bool Load(const std::uint8_t* data, std::size_t size) {
        s3d::Model model;
        if (!model.Parse(data, size)) return false;
        for (const s3d::VertexGroup& group : model.vertexGroups) vertexData_.push_back(ConvertVertices(group));
        vertexGroups_.resize(model.vertexGroups.size());
        for (std::size_t i = 0; i < model.vertexGroups.size(); ++i)
            vertexGroups_[i] = SC4MeshVertexGroup{gd::vf::V3F_C4UB_T2F, 0, model.vertexGroups[i].count, vertexData_[i].data()};
        indexData_ = model.indexGroups;
        indexPointers_.resize(indexData_.size());
        for (std::size_t i = 0; i < indexData_.size(); ++i) indexPointers_[i] = indexData_[i].data();
        drawRecords_.resize(model.primitiveGroups.size());
        drawLists_.resize(model.primitiveGroups.size());
        for (std::size_t i = 0; i < model.primitiveGroups.size(); ++i) {
            for (const s3d::Primitive& p : model.primitiveGroups[i].primitives)
                drawRecords_[i].push_back(SC4MeshDrawRecord{p.type, p.first, p.count});
            drawLists_[i].begin = drawRecords_[i].data();
            drawLists_[i].end = drawRecords_[i].data() + drawRecords_[i].size();
        }
        const std::uint32_t group = GetGroup();
        bool haveBounds = false;
        // Every frame of the ANIM chunk: frame f of a mesh is its f-th frame reference (meshes with fewer frames keep their
        // last one). Frame 0 is what static models draw and what the bounds cover.
        std::size_t frames = 1;
        for (const s3d::AnimMesh& mesh : model.meshes) frames = std::max(frames, mesh.frames.size());
        frameMeshes_.resize(frames);
        frameMaterials_.resize(frames);
        frameRate_ = model.frameRate;
        for (std::size_t f = 0; f < frames; ++f) {
            for (const s3d::AnimMesh& mesh : model.meshes) {
                if (mesh.frames.empty()) continue;
                const s3d::FrameRef& frame = mesh.frames[std::min(f, mesh.frames.size() - 1)];
                if (frame.vertexGroup >= vertexGroups_.size() || frame.primitiveGroup >= drawLists_.size()) continue;
                SC4Mesh out;
                out.vertices = &vertexGroups_[frame.vertexGroup];
                out.indexGroup = frame.indexGroup < indexPointers_.size() ? &indexPointers_[frame.indexGroup] : nullptr;
                out.draws = &drawLists_[frame.primitiveGroup];
                frameMeshes_[f].push_back(out);
                frameMaterials_[f].push_back(frame.materialGroup < model.materials.size() ? BuildMaterial(model.materials[frame.materialGroup], group) : s3d::cS3DMaterial{});
                if (f != 0) continue;
                const s3d::VertexGroup& vg = model.vertexGroups[frame.vertexGroup];
                for (int k = 0; k < 3; ++k) {
                    bounds_.min[k] = haveBounds ? std::min(bounds_.min[k], vg.min[k]) : vg.min[k];
                    bounds_.max[k] = haveBounds ? std::max(bounds_.max[k], vg.max[k]) : vg.max[k];
                }
                haveBounds = true;
            }
        }
        meshes_ = frameMeshes_[0];
        materials_ = frameMaterials_[0];
        return !meshes_.empty();
    }

private:
    ~ResModel() override = default;

    // Position first, then (by the stride of the standard formats) a colour and the first uv pair.
    static std::vector<std::uint8_t> ConvertVertices(const s3d::VertexGroup& g) {
        constexpr std::size_t kOut = 24;
        std::vector<std::uint8_t> out(std::size_t(g.count) * kOut);
        int colorAt = -1, uvAt = -1;
        switch (g.stride) {
            case 16: colorAt = 12; break;
            case 20: uvAt = 12; break;
            case 24: colorAt = 12; uvAt = 16; break;
            case 28: uvAt = 12; break;
            case 32: colorAt = 12; uvAt = 16; break;
            default: if (g.stride >= 20) uvAt = 12; break;
        }
        for (std::size_t i = 0; i < g.count; ++i) {
            const std::uint8_t* src = g.data.data() + i * g.stride;
            std::uint8_t* dst = out.data() + i * kOut;
            std::memcpy(dst, src, 12);
            const std::uint32_t white = 0xffffffffu;
            std::memcpy(dst + 12, colorAt >= 0 ? src + colorAt : reinterpret_cast<const std::uint8_t*>(&white), 4);
            if (uvAt >= 0) std::memcpy(dst + 16, src + uvAt, 8);
        }
        return out;
    }

    // What cS3DMaterialManager::CreateMaterial builds from the resource material, with the texture bound.
    static s3d::cS3DMaterial BuildMaterial(const s3d::Material& m, std::uint32_t group) {
        s3d::cS3DMaterial material;
        material.flags = static_cast<std::uint16_t>(m.flags | 0x180);
        material.b6 = m.depthFunc;
        material.b7 = m.alphaFunc;
        material.b8 = static_cast<std::uint8_t>(m.alphaThreshold >> 8);
        material.b9 = m.srcBlend;
        material.b10 = m.dstBlend;
        const std::size_t layers = std::min<std::size_t>(m.textures.size(), 2);
        material.layerCount = static_cast<std::uint8_t>(layers);
        for (std::size_t i = 0; i < layers; ++i) {
            s3d::S3DMaterialLayer& layer = material.layers[i];
            layer.textureId = m.textures[i].instance;
            layer.texture = reinterpret_cast<std::uint64_t>(TextureRecord(group, m.textures[i].instance));
        }
        for (std::size_t i = layers; i < 2; ++i) material.layers[i].used = 0xff;
        if (layers == 0) material.flags &= static_cast<std::uint16_t>(~0x20u);
        material.state = 1;
        material.refCount = 1;
        return material;
    }

    std::vector<std::vector<std::uint8_t>> vertexData_;
    std::vector<SC4MeshVertexGroup> vertexGroups_;
    std::vector<std::vector<std::uint16_t>> indexData_;
    std::vector<const std::uint16_t*> indexPointers_;
    std::vector<std::vector<SC4MeshDrawRecord>> drawRecords_;
    std::vector<SC4MeshDrawList> drawLists_;
    std::vector<SC4Mesh> meshes_;
    std::vector<s3d::cS3DMaterial> materials_;
    std::vector<std::vector<SC4Mesh>> frameMeshes_;              // every animation frame (frame 0 = meshes_)
    std::vector<std::vector<s3d::cS3DMaterial>> frameMaterials_;
    int frameRate_ = 0;
    cS3DBoundingBox bounds_{{0, 0, 0}, {0, 0, 0}};
};

class ResModelFactory final : public cGZPersistResourceFactory {
public:
    using cGZPersistResourceFactory::CreateInstance;
    bool CreateInstance(std::uint32_t type, std::uint32_t iid, void** out) override {
        *out = nullptr;
        if (type != s3d::kResourceTypeModel) return false;
        auto* model = new ResModel();
        if (model->QueryInterface(iid, out)) return true;
        model->AddRef();
        model->Release();
        return false;
    }
    bool SerialRead(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) override {
        void* raw = nullptr;
        if (!resource.QueryInterface(kGZIID_cIS3DResModel, &raw)) return false;
        auto* model = static_cast<ResModel*>(static_cast<s3d::cIS3DResModel*>(raw));
        const std::uint32_t size = record.GetSize() - record.GetPosition();
        std::vector<std::uint8_t> bytes(size);
        const bool ok = size && record.GetFieldVoid(bytes.data(), size) && model->Load(bytes.data(), bytes.size());
        if (!ok && std::getenv("SC4_DEBUG")) std::fprintf(stderr, "s3d: model %08x/%08x failed to load (%u bytes)\n", resource.GetGroup(), resource.GetInstance(), size);
        model->Release();
        ++(ok ? g_modelsLoaded : g_modelsFailed);
        return ok;
    }
    bool SerialWrite(cIGZPersistResource&, cIGZPersistDBSerialRecord&) override { return false; }
};

// ---- hosts ------------------------------------------------------------------------------------------------------
sc4::cSC43DRender* g_render = nullptr;
sc4::cSC4City* g_city = nullptr;

class ModelInstanceHost final : public s3d::IModelInstanceHost {
public:
    void ModelPick(cSC4PickContext*, s3d::cIS3DModelInstance*, s3d::cS3DMeshAndMaterial*) override {}
    void RenderModelInstance(SC4DrawContext* context, s3d::cIS3DModelInstance* instance, s3d::cS3DMeshAndMaterial* meshes,
                             const s3d::SC4DrawAttributes* attributes, bool flag) override {
        static int drawn = 0;
        if (std::getenv("SC4_DEBUG") && (++drawn == 1 || drawn % 20000 == 0)) std::fprintf(stderr, "s3d: model draw #%d\n", drawn);
        if (context) context->RenderModelInstance(instance, meshes, attributes, flag);
    }
    void TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box) override {
        SC4ViewUtilities::TransformBoundingBox(transform, box);
    }
};

class PlaceableHost final : public sc4::IPlaceableHost {
public:
    bool AddModelToRenderer(sc4::cIS3DModelInstance* model) override {
        const bool ok = g_render && g_render->InsertModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model));
        if (ok) ++g_inserted;
        return ok;
    }
    void RemoveModelFromRenderer(sc4::cIS3DModelInstance* model) override {
        if (g_render) g_render->RemoveModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model));
    }
    void RemoveOverlay(std::uint32_t id) override { RemoveShadowOverlay(id); }
    void SetOverlayVisible(std::uint32_t id, bool visible) override { SetShadowOverlayEnabled(id, visible); }
};

sc4::cSTETerrain* Terrain() {
    if (!g_city) return nullptr;
    cIGZUnknown* object = g_city->GetComponent(sc4::CityComponent::Terrain);
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}

// cISC4LightingManager for network pieces: the terrain's shade under a corner (TerrainShade below), a fixed sun for
// the add-on parts.
std::uint8_t TerrainShade(int x, int z);
class NetworkLighting final : public sc4::INetworkOccupantLighting {
public:
    void TerrainLighting(const float p[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = g = b = TerrainShade(static_cast<int>(p[0] / 16.0f + 0.5f), static_cast<int>(p[2] / 16.0f + 0.5f));
    }
    void SurfaceLighting(const float*, const float n[3], std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        float dot = n[0] * -0.45f + n[1] * 0.78f + n[2] * 0.43f;
        if (dot < 0.35f) dot = 0.35f;
        if (dot > 1.0f) dot = 1.0f;
        r = g = b = static_cast<std::uint8_t>(dot * 255.0f);
    }
};

// GetTexture's binding factory: the network textures live in the S3D texture group.
class NetworkModelHost final : public sc4::INetworkModelHost {
public:
    std::uint32_t NetworkTexture(std::uint32_t instance) override { return TextureRecord(kModelTextureGroup, instance)->id; }
};

// The terrain's lit grey at a vertex: the same fixed sun NativeTerrainHost::ApplyTerrainLightingRow lights the terrain with.
std::uint8_t TerrainShade(int x, int z) {
    sc4::cSTETerrain* terrain = native::Terrain();
    if (!terrain) return 0xff;
    const int countX = static_cast<int>(terrain->CellCountX()) + 1, countZ = static_cast<int>(terrain->CellCountZ()) + 1;
    if (x < 0 || z < 0 || x >= countX || z >= countZ) return 0xff;
    const float* n = terrain->Vertices()[static_cast<std::size_t>(z) * countX + x].normal;
    const float dot = n[0] * -0.45f + n[1] * 0.78f + n[2] * 0.43f;
    float lit = 0.80f + 0.45f * (dot > 0.0f ? dot : 0.0f);
    if (lit > 1.0f) lit = 1.0f;
    return static_cast<std::uint8_t>(lit * 255.0f);
}

class CellTextureHost final : public sc4::ICellTextureModelHost {
public:
    bool LockedLotAltitude(int, int, float&) override { return false; }   // no lot manager in the native city yet
    void CellCornerAltitudes(int x, int z, float out[4]) override {
        sc4::cSTETerrain* terrain = native::Terrain();
        if (!terrain) { out[0] = out[1] = out[2] = out[3] = 0; return; }
        out[0] = terrain->GetAltitudeAtVertex(x, z);
        out[1] = terrain->GetAltitudeAtVertex(x + 1, z);
        out[2] = terrain->GetAltitudeAtVertex(x, z + 1);
        out[3] = terrain->GetAltitudeAtVertex(x + 1, z + 1);
    }
    void CellCornerShades(int x, int z, std::uint8_t out[4]) override {
        const int xs[4] = {x, x + 1, x, x + 1}, zs[4] = {z, z, z + 1, z + 1};
        for (int i = 0; i < 4; ++i) out[i] = TerrainShade(xs[i], zs[i]);
    }
    const sc4::CellTextureBinding* TextureBinding(std::uint32_t instance) override { return LotTextureBinding(instance); }
};

class LotBaseTextureHost final : public sc4::ILotBaseTextureHost {
public:
    struct Binding final : sc4::ILotBaseTextureBinding {
        std::uint32_t frames = 1;
        std::uint32_t FrameCount() const override { return frames; }
        void Release() override { delete this; }
    };
    sc4::cSC4Lot* LotAtCell(int, int) override { return nullptr; }
    float TerrainAltitudeAtVertex(int x, int z) override { sc4::cSTETerrain* t = native::Terrain(); return t ? t->GetAltitudeAtVertex(x, z) : 0.0f; }
    sc4::ILotBaseTextureBinding* CreateTextureBinding(std::uint32_t instance) override {
        const sc4::CellTextureBinding* binding = LotTextureBinding(instance);
        if (!binding) return nullptr;
        auto* out = new Binding();
        out->frames = binding->frameCount;
        return out;
    }
    int ViewZoom() override { const SC4ViewUtilities* v = g_render ? g_render->GetViewUtilities() : nullptr; return v ? v->Zoom() : 0; }
    void RendererRemoveModel(sc4::cIS3DModelInstance* model) override { if (g_render) g_render->RemoveModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model)); }
    void RendererMoveModel(sc4::cIS3DModelInstance* model) override { if (g_render) g_render->MoveModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model)); }
    void RendererChangeModel(sc4::cIS3DModelInstance* model, int reason) override { if (g_render) g_render->ChangeModel(reinterpret_cast<s3d::cIS3DModelInstance*>(model), reason); }
    sc4::ILotBaseTextureViewSet* AcquireViewSet() override { return nullptr; }
    void MoveOccupant(sc4::cISC4Occupant*) override {}
    sc4::ILotCellTextureModel* CreateCellTextureModel(const sc4::LotBaseTextureSpecification& spec) override {
        auto* model = new sc4::cSC4CellTextureModel();
        model->AddRef();
        model->Init(spec);
        return model;
    }
};

class ModelMakerHost final : public sc4::IModelMakerHost {
public:
    cIGZUnknown* Renderer() override { return static_cast<cIGZUnknown*>(static_cast<cISC43DRender*>(g_render)); }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cIGZMessageServer2* AnimationTickManager() override { return nullptr; }
    sc4::cISC4OccupantManager* OccupantManager() override {
        cIGZUnknown* object = g_city ? g_city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw)) return nullptr;
        auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
        manager->Release();   // the model maker takes its own reference
        return manager;
    }
    sc4::cISC4LotManager* LotManager() override { return nullptr; }
    cIGZUnknown* BuildingDevelopmentSim() override { return g_city ? g_city->GetComponent(sc4::CityComponent::BuildingDevelopmentSimulator) : nullptr; }
    cIGZUnknown* Terrain() override { return g_city ? g_city->GetComponent(sc4::CityComponent::Terrain) : nullptr; }
    int ShadowRenderQuality() override { const char* q = std::getenv("SC4_SHADOWQ"); return q ? std::atoi(q) : 5; }
    float ShadowMinimumHeight() override { return 0.0f; }
    float ShadowViewScale() override { return 1.0f; }
    sc4::IShadowOverlay* ShadowOverlay(int) override { return native::TheShadowOverlay(); }
    sc4::cISC4OccupantFilter* CurrentViewFilter() override {
        auto* all = new sc4::cSC4OccupantFilter();   // every occupant is in view
        all->AddRef();
        return all;
    }
    void RefreshViewFilter() override {}
    bool RendererHasModelInstance(s3d::cIS3DModelInstance* instance) override { return g_render && g_render->IsModelInserted(instance); }
    void RendererUpdateModelInstance(s3d::cIS3DModelInstance* instance) override { if (g_render) g_render->MoveModel(instance); }
    void RendererAddModelInstance(s3d::cIS3DModelInstance* instance, int) override { if (g_render) g_render->InsertModel(instance); }
    cIGZUnknown* ViewUtilities() override { return nullptr; }
    int ViewZoom() override { const SC4ViewUtilities* v = Utilities(); return v ? v->Zoom() : 0; }
    int ViewRotation() override { const SC4ViewUtilities* v = Utilities(); return v ? v->Rotation() : 0; }
    float ViewRotationAngle() override { const SC4ViewUtilities* v = Utilities(); return v ? v->RotationAngle() : 0.0f; }
    float ViewportUnitsPerWorldUnit() override {
        const SC4ViewUtilities* v = Utilities();
        const float world = v ? v->ViewportUnitsToWorldUnits(1.0f) : 0.0f;
        return world > 0.0f ? 1.0f / world : 1.0f;
    }
    // The whole city is the load rectangle: every model is built up front.
    bool ViewGroundRectangle(float& focusX, float& focusZ, float rect[4]) override {
        const float sizeX = static_cast<float>(TerrainSizeX()) * 16.0f, sizeZ = static_cast<float>(TerrainSizeZ()) * 16.0f;
        focusX = sizeX * 0.5f;
        focusZ = sizeZ * 0.5f;
        rect[0] = 0; rect[1] = 0; rect[2] = sizeX; rect[3] = sizeZ;
        return true;
    }
    float ModelSizeThreshold() override { return 0.0f; }
    float TextureTintAlpha() override { return 0.5f; }
    int RenderDetailLevel() override { return 0; }
    void LightingColour(float out[4]) override { out[0] = out[1] = out[2] = out[3] = 1.0f; }
    int TerrainSizeX() override { sc4::cSTETerrain* t = native::Terrain(); return t ? static_cast<int>(t->CellCountX()) : 64; }
    int TerrainSizeZ() override { sc4::cSTETerrain* t = native::Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 64; }
    std::uint32_t TerrainCellFlags(int, int) override { return 0; }
    void TerrainSetCellFlags(int, int) override {}
    void TerrainRefreshCell(int, int) override {}
    bool TerrainCellHasWater(int, int) override { return false; }
    void TerrainNormalAt(float, float, float out[3]) override { out[0] = 0; out[1] = 1; out[2] = 0; }
    bool NetworkAt(float, float) override { return false; }
    void PositionToCityCell(float x, float z, int& cx, int& cz) override { cx = static_cast<int>(x / 16.0f); cz = static_cast<int>(z / 16.0f); }
    // No background loader thread natively: the model maker loads a new zoom / rotation's models at once.
    bool ReadConfigOption(const char* key, std::string& value) override {
        if (std::string(key) != "backgroundLoad") return false;
        value = "off";
        return true;
    }
    sc4::IModelBackgroundLoader* CreateBackgroundLoader() override { return nullptr; }
    sc4::IModelAnimationCoordinator* CreateAnimationCoordinator() override { return nullptr; }
    sc4::cISC4OccupantFilter* CreateZoomRotationModelChangeOccupantFilter() override { return nullptr; }
    void SetZoomRotationFilter(sc4::cISC4OccupantFilter*, int, int) override {}
    std::uint32_t GetModelInstanceID(sc4::cISC4Occupant* occupant, int zoom, int rotation, int state, bool lit, std::uint32_t* outA, std::uint32_t* outB) override {
        std::vector<std::uint32_t> ids;
        sc4::cSC4ModelMakerUtility::GetAllModelInstanceIDs(ids, nullptr, occupant->AsPropertyHolder(), false, zoom, rotation, state, lit, 0, outA, outB);
        return ids.empty() ? 0 : ids[0];
    }
    std::uint32_t MapShadowTexture(std::uint32_t, std::uint8_t&, std::uint32_t, std::uint32_t) override { return 0; }
    std::uint32_t AddShadowDecal(int, const cGZPersistResourceKey&, float, float, float, float, bool) override { return 0; }

private:
    static const SC4ViewUtilities* Utilities() { return g_render ? g_render->GetViewUtilities() : nullptr; }
};

}  // namespace

void InstallModelResources(gd::cIGZGDriver* driver) {
    g_driver = driver;
    static bool registered = false;
    if (registered || !SL::spRM) return;
    registered = true;
    static ResModelFactory factory;
    factory.AddRef();
    SL::spRM->RegisterObjectFactory(kGZCLSID_cS3DResModelFactory, s3d::kResourceTypeModel, &factory);
}

namespace {
sc4::cSC4ModelMaker* g_maker = nullptr;
}  // namespace

void DetachModelMaker() {
    if (!g_maker) return;
    g_maker->Shutdown();
    g_maker->Release();
    g_maker = nullptr;
}

bool AttachModelMaker(sc4::cSC43DRender& render, sc4::cSC4City& city) {
    DetachModelMaker();
    g_render = &render;
    g_city = &city;
    if (sc4::cSTETerrain* terrain = Terrain()) AttachShadowOverlay(render, *terrain);
    static ModelInstanceHost instanceHost;
    static PlaceableHost placeableHost;
    static ModelMakerHost makerHost;
    static sc4::cSC4ModelMakerUtility utility;
    static CellTextureHost cellTextureHost;
    static NetworkModelHost networkModelHost;
    sc4::SetNetworkModelHost(&networkModelHost);
    sc4::SetNetworkModelFactory([](sc4::cISC4Occupant* occupant) -> s3d::cIS3DModelInstance* {
        auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        if (!network) return nullptr;
        // The lighting manager lights a network piece when it enters the city (cSC4NetworkOccupant::UpdateLighting).
        static NetworkLighting lighting;
        network->UpdateLighting(lighting);
        auto* model = new sc4::cSC4NetworkOccupantModel(*network);
        model->AddRef();
        return model;
    });
    static LotBaseTextureHost lotBaseTextureHost;
    sc4::SetCellTextureModelHost(&cellTextureHost);
    sc4::SetLotBaseTextureHost(&lotBaseTextureHost);
    s3d::SetModelInstanceHost(&instanceHost);
    sc4::SetPlaceableHost(&placeableHost);
    sc4::SetPlaceableObjectFactory([] {
        auto* object = new sc4::cSC43DPlaceableObject();
        object->AddRef();
        return static_cast<sc4::cISC43DPlaceableObject*>(object);
    });
    sc4::SetModelMakerUtility(&utility);
    sc4::SetModelMakerHost(&makerHost);
    auto* maker = new sc4::cSC4ModelMaker();
    maker->AddRef();
    g_maker = maker;
    const bool ok = maker->Init();
    maker->DoPostCityInit();
    std::printf("city view: model maker %s: %d models loaded, %d failed, %d instances, %d inserted\n", ok ? "up" : "FAILED", g_modelsLoaded, g_modelsFailed, g_instances, g_inserted);
    return ok;
}

}  // namespace native

namespace native {
unsigned int FshTextureId(unsigned int group, unsigned int instance) { return TextureRecord(group, instance)->id; }
}  // namespace native
