// The underground network hosts on the city: what cSC4UndergroundNetworkTool (the pipe tool) and cSC4UndergroundOccupant
// (pipes and subway pieces) reach through engine globals in the original. The tool's hosts are the terrain, the occupant
// manager and the resource manager; the occupant's are the city's cell mapping and the cell queries.
//
// The occupant's custom junction models, mesh access and renderer ChangeModel are answered; PrepareRenderer and the network
// occupant statics are empty. PARTIAL: the base InsertRangeConstraintsForCell the tool's drag reaches (i386 0x1001b37e4) is
// not recovered, so dragging pipes is not wired; the tool serves the network manager's demolition and re-solve of pipes.
#include "app/NativeUnderground.h"
#include "platform/Unwired.h"

#include <cstdio>
#include <cstdlib>
#include <list>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeDemolition.h"
#include "app/NativeLighting.h"
#include "app/NativeModelMaker.h"
#include "sc4/Render3D.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/ComplexViewOccupantFilter.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/UndergroundCustomView.h"
#include "sc4/OccupantManager.h"
#include "sc4/PathInfo.h"
#include "sc4/PipeOccupant.h"
#include "sc4/SubwayConnectionOccupant.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/SubwayOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/UndergroundConnectionOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/NetworkManager.h"
#include "sc4/UndergroundNetworkTool.h"
#include "sc4/UndergroundOccupant.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "s3d/MeshAndMaterial.h"
#include "sc4/DrawContext.h"
#include "sc4/LightingManager.h"
#include "sc4/ViewUtilities.h"
#include "s3d/ModelInstance.h"

namespace native {
sc4::cSC4NetworkTool* NativeManagerNetworkTool(std::uint32_t type);   // NativeNetworkManager.cpp

namespace {

using namespace sc4;

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

// SL::spOccupantManager GetOccupantsByStandardCityCells(list, cell, cell, no filter, all): one reference each.
void CellOccupants(int x, int z, std::list<cISC4Occupant*>& out) {
    cISC4OccupantManager* occupants = Occupants();
    if (!occupants) return;
    const int cell[2] = {x, z};
    OccupantList list;
    if (!occupants->GetOccupantsByStandardCityCells(list, cell, cell, 0u, 0xffffffffu)) return;
    out.splice(out.end(), list);
}

class UndergroundToolHost final : public IUndergroundNetworkToolHost {
public:
    float TerrainHeight(std::uint32_t vertexId) override {   // SL::spTerrain i386 vslot 0x34
        cSTETerrain* terrain = Terrain();
        return terrain ? terrain->GetAltitude(static_cast<std::int32_t>(vertexId)) : 0.0f;
    }
    void TerrainCellHeights(int x, int z, float* heights) override {   // corners (x, z), (x, z+1), (x+1, z+1), (x+1, z)
        cSTETerrain* terrain = Terrain();
        if (!terrain) {
            for (int i = 0; i < 4; ++i) heights[i] = 0.0f;
            return;
        }
        heights[0] = terrain->GetAltitudeAtVertex(x, z);
        heights[1] = terrain->GetAltitudeAtVertex(x, z + 1);
        heights[2] = terrain->GetAltitudeAtVertex(x + 1, z + 1);
        heights[3] = terrain->GetAltitudeAtVertex(x + 1, z);
    }
    void QueryCellOccupants(int x, int z, std::list<cISC4Occupant*>& out) override { CellOccupants(x, z, out); }
    std::uint32_t AddOccupantToManager(cISC4Occupant* occupant, int param458) override {
        cISC4OccupantManager* occupants = Occupants();
        return occupants && occupant && occupants->InsertOccupant(occupant, static_cast<std::uint32_t>(param458)) ? 1u : 0u;
    }
    void RemoveOccupantFromManager(cISC4Occupant* occupant, bool keep, int param458) override {
        if (cISC4OccupantManager* occupants = Occupants(); occupants && occupant)
            occupants->RemoveOccupant(occupant, keep, static_cast<std::uint32_t>(param458));
    }
    cISCPropertyHolder* AcquireNetworkTuningProperties(std::uint32_t exemplarId) override {
        const cGZPersistResourceKey key{kUndergroundTuningType, kUndergroundTuningGroup, exemplarId};
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, kUndergroundTuningProperty, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        if (holder) holder->AddRef();
        exemplar->Release();
        return holder;
    }
    // i386 0x1001b37e4 (base drag constraints): not recovered; only placement reaches it.
    void InsertRangeConstraintsForCellBase(std::uint32_t) override {
        SC4_UNWIRED("cSC4UndergroundNetworkTool: the base InsertRangeConstraintsForCell (i386 0x1001b37e4) is not recovered (pipe drag constraints)");
    }
    // SL::NetworkManager() vslot 0x18 DestroyNetworkOccupants, then the occupant manager releases the list (vslot 0xc0).
    void DestroyNetworkOccupants(std::list<cISC4Occupant*>& occupants) override {
        if (cSC4NetworkManager* manager = CityNetworkManager()) manager->DestroyNetworkOccupants(occupants);
        for (cISC4Occupant* occupant : occupants) occupant->Release();
        occupants.clear();
    }
};

// The connection piece as the occupant's walls ask for it: only its release.
class ConnectionRef final : public IUndergroundConnectionOccupant {
public:
    explicit ConnectionRef(cSC4UndergroundConnectionOccupant* connection) : connection_(connection) {}
    void Release() override {
        connection_->Release();
        delete this;
    }

private:
    cSC4UndergroundConnectionOccupant* connection_;
};

// cS3DTextureBinding as the pieces use it: the first word is the texture id (here the renderer's FSH texture).
class TextureBinding final : public IUndergroundTextureBinding {
public:
    explicit TextureBinding(std::uint32_t id) : id_(id) {}
    const std::uint32_t* TextureIds() override { return &id_; }
    void Release() override { delete this; }

private:
    std::uint32_t id_;
};

class UndergroundOccupantHost final : public IUndergroundOccupantHost {
public:
    void InitNetworkOccupantStatics() override {}
    // The texture binding factory (service 0xfac0e219) with key {0x7ab50e44, group, instance}: null when the game data
    // has no such texture.
    IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) override {
        const std::uint32_t id = instance ? FshTextureId(group, instance) : 0;
        return id ? new TextureBinding(id) : nullptr;
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {   // SL::spCity vslot 0x2f8
        if (cSC4City* city = HostCity()) {
            city->PositionToCell(x, z, cellX, cellZ);
        } else {
            cellX = static_cast<int>(x * 0.0625f);
            cellZ = static_cast<int>(z * 0.0625f);
        }
    }
    bool IsCellInCity(int cellX, int cellZ) override {   // SL::spCity vslot 0x318
        cSC4City* city = HostCity();
        return city && city->CellIsInBounds(cellX, cellZ);
    }
    void PrepareRenderer() override {}
    // SL::spRenderer ChangeModel(model, change) for every model of the piece's placeable object (the original's piece is
    // its one model instance).
    void ChangeModelInRenderer(cSC4UndergroundOccupant* piece, s3d::cIS3DModelInstance* instance, int change) override {
        if (!CityRenderer()) return;
        if (instance) CityRendererChangeModel(instance, change);
        cISC43DPlaceableObject* placeable = piece ? static_cast<cISC4Occupant*>(piece)->GetPlaceableObject() : nullptr;
        if (!placeable) return;
        std::uint32_t count = static_cast<std::uint32_t>(placeable->GetModelCount());
        if (count == 0) return;
        std::vector<sc4::cIS3DModelInstance*> models(count, nullptr);
        placeable->GetModels(models.data(), count);   // borrowed
        for (std::uint32_t i = 0; i < count; ++i)
            if (models[i]) CityRendererChangeModel(reinterpret_cast<s3d::cIS3DModelInstance*>(models[i]), change);
    }
    // A junction piece's model: SL::spRM GetResource({0x5ad0e817, 0xbadb57f1, network | piece}, cIS3DResModel 0x9ad0e7e3) and its
    // CreateModelInstance(0, &model); one reference for the caller.
    s3d::cIS3DModelInstance* CreateCustomModel(std::uint32_t instance) override {
        void* resource = nullptr;
        const cGZPersistResourceKey key{0x5ad0e817, 0xbadb57f1, instance};
        if (!SL::spRM || !SL::spRM->GetResource(key, 0x9ad0e7e3, &resource, 0, nullptr) || !resource) return nullptr;
        auto* model = static_cast<s3d::cIS3DResModel*>(resource);
        s3d::cIS3DModelInstance* made = nullptr;
        const bool ok = model->CreateModelInstance(0, &made);
        model->Release();
        static const bool trace = std::getenv("SC4_NETDEBUG") != nullptr;
        if (trace) std::fprintf(stderr, "underground: junction model %08x -> %s\n", instance, ok && made ? "made" : "none");
        return ok ? made : nullptr;
    }
    // cSC4UndergroundNetworkTool::FindUndergroundOccupant: the last occupant of the cell answering `gziid`.
    cSC4UndergroundOccupant* FindUndergroundOccupant(int cellX, int cellZ, std::uint32_t gziid) override {
        std::list<cISC4Occupant*> occupants;
        CellOccupants(cellX, cellZ, occupants);
        cSC4UndergroundOccupant* found = nullptr;
        for (cISC4Occupant* occupant : occupants) {
            void* raw = nullptr;
            if (occupant->QueryInterface(gziid, &raw) && raw) {
                if (found) found->Release();
                found = static_cast<cSC4UndergroundOccupant*>(raw);
            }
            occupant->Release();
        }
        return found;
    }
    // cSC4UndergroundNetworkTool::FindUndergroundConnectionOccupant: the connection piece whose network is `gziid`.
    bool FindUndergroundConnectionOccupant(int cellX, int cellZ, std::uint32_t gziid,
                                           IUndergroundConnectionOccupant*& out) override {
        std::list<cISC4Occupant*> occupants;
        CellOccupants(cellX, cellZ, occupants);
        cSC4UndergroundConnectionOccupant* found = nullptr;
        for (cISC4Occupant* occupant : occupants) {
            void* raw = nullptr;
            if (occupant->QueryInterface(kGZIID_cISC4UndergroundConnectionOccupant, &raw) && raw) {
                auto* connection = static_cast<cSC4UndergroundConnectionOccupant*>(raw);
                if (connection->GetNetworkOccupantGZIID() == gziid) {
                    if (found) found->Release();
                    found = connection;
                } else {
                    connection->Release();
                }
            }
            occupant->Release();
        }
        out = found ? new ConnectionRef(found) : nullptr;
        return found != nullptr;
    }
    // The model's cIS3DMeshAndMaterial view (QueryInterface 0x26a68448): its meshes and materials, one reference until Release.
    IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance* instance) override {
        void* raw = nullptr;
        if (!instance || !instance->QueryInterface(s3d::kGZIID_cIS3DMeshAndMaterial, &raw) || !raw) return nullptr;
        auto* list = static_cast<s3d::cS3DMeshAndMaterial*>(raw);
        static const bool trace = std::getenv("SC4_NETDEBUG") != nullptr;
        static int shown = 0;
        if (trace && shown < 12) {
            ++shown;
            const auto* material = list->Materials().empty() ? nullptr : reinterpret_cast<const s3d::cS3DMaterial*>(list->Materials()[0].data());
            std::fprintf(stderr, "underground: junction draw, %zu meshes %zu materials, first texture %p\n", list->Meshes().size(), list->Materials().size(),
                         material ? reinterpret_cast<const void*>(material->layers[0].texture) : nullptr);
        }
        return new Meshes(list);
    }
private:
    class Meshes final : public IUndergroundModelMeshes {
    public:
        explicit Meshes(s3d::cS3DMeshAndMaterial* list) : list_(list) {}
        int Count() override { return static_cast<int>(list_->Meshes().size()); }
        const SC4Mesh* Mesh(int index) override { return reinterpret_cast<const SC4Mesh*>(list_->Meshes()[std::size_t(index)].data()); }
        s3d::cS3DMaterial* Material(int index) override { return reinterpret_cast<s3d::cS3DMaterial*>(list_->Materials()[std::size_t(index)].data()); }
        void Release() override {
            list_->Release();
            delete this;
        }
    private:
        s3d::cS3DMeshAndMaterial* list_;
    };
};

// The connection pieces' globals: the city's cell geometry, the terrain, the cell queries the underground tool makes, the
// network manager's tool of the connection's network, the occupant manager and the renderer.
class UndergroundConnectionHost final : public IUndergroundConnectionHost, public IUndergroundConnectionViewHost {
public:
    // the drawing: the same services the underground pieces use
    void PrepareRenderer() override { occupantHost_.PrepareRenderer(); }
    IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) override {
        return occupantHost_.CreateTextureBinding(group, instance);
    }
    s3d::cIS3DModelInstance* CreateModel(std::uint32_t instance) override {
        return instance ? occupantHost_.CreateCustomModel(instance) : nullptr;
    }
    IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance* instance) override { return occupantHost_.ModelMeshes(instance); }
    void GetZoomAndRotation(int& zoom, int& rotation) override {
        zoom = rotation = 0;
        cSC43DRender* render = CityRenderer();
        if (SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) view->GetZoomAndRotation(zoom, rotation);
    }
    void GetCameraViewAngles(float& yaw, float& pitch, float& screenPitch) override {
        yaw = pitch = screenPitch = 0.0f;
        cSC43DRender* render = CityRenderer();
        if (SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) view->GetCameraViewAngles(yaw, pitch, screenPitch);
    }
    cS3DVector3 LightColor(const cS3DVector3& normal) override {
        cSC4LightingManager* lighting = LightingManager();
        return lighting ? lighting->GetColor(normal) : cS3DVector3{1.0f, 1.0f, 1.0f};
    }
    cISC4Occupant* FindBuildingOccupant(int cellX, int cellZ) override {
        std::list<cISC4Occupant*> occupants;
        CellOccupants(cellX, cellZ, occupants);
        cISC4Occupant* found = nullptr;
        for (cISC4Occupant* occupant : occupants) {
            if (occupant->GetType() == 0x278128a0u) found = occupant;
            occupant->Release();
        }
        return found;   // borrowed: the occupant manager keeps it
    }

    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {   // spCity vslot 0x17c
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        else cellX = static_cast<int>(x * 0.0625f), cellZ = static_cast<int>(z * 0.0625f);
    }
    void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) override {   // spCity vslot 0x180
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(cellX, cellZ, x, z);
        else x = static_cast<float>(cellX) * 16.0f, z = static_cast<float>(cellZ) * 16.0f;
    }
    float CellWidthX() override { cSC4City* city = HostCity(); return city ? city->CellWidthX() : 16.0f; }
    float CellWidthZ() override { cSC4City* city = HostCity(); return city ? city->CellWidthZ() : 16.0f; }
    void CellCornerHeights(int x, int z, float* heights) override {
        cSTETerrain* terrain = Terrain();
        if (!terrain) {
            for (int i = 0; i < 4; ++i) heights[i] = 0.0f;
            return;
        }
        heights[0] = terrain->GetAltitudeAtVertex(x, z);
        heights[1] = terrain->GetAltitudeAtVertex(x, z + 1);
        heights[2] = terrain->GetAltitudeAtVertex(x + 1, z + 1);
        heights[3] = terrain->GetAltitudeAtVertex(x + 1, z);
    }
    cSC4UndergroundOccupant* FindUndergroundOccupant(int cellX, int cellZ, std::uint32_t gziid) override {
        return occupantHost_.FindUndergroundOccupant(cellX, cellZ, gziid);
    }
    cSC4UndergroundConnectionOccupant* FindUndergroundConnectionOccupant(int cellX, int cellZ, std::uint32_t gziid) override {
        std::list<cISC4Occupant*> occupants;
        CellOccupants(cellX, cellZ, occupants);
        cSC4UndergroundConnectionOccupant* found = nullptr;
        for (cISC4Occupant* occupant : occupants) {
            void* raw = nullptr;
            if (occupant->QueryInterface(kGZIID_cISC4UndergroundConnectionOccupant, &raw) && raw) {
                auto* connection = static_cast<cSC4UndergroundConnectionOccupant*>(raw);
                if (connection->GetNetworkOccupantGZIID() == gziid && !found) found = connection;
                else connection->Release();
            }
            occupant->Release();
        }
        return found;
    }
    void PlaceNetworkOccupants(std::uint32_t networkType, int x0, int z0, int x1, int z1) override {
        sc4::cSC4NetworkTool* tool = NativeManagerNetworkTool(networkType);
        if (!tool) return;
        tool->AddRef();
        tool->Init();
        tool->PlaceNetworkOccupants(x0, z0, x1, z1, 0, nullptr, true, -1);
        tool->Shutdown();
        tool->Release();
    }
    void RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t data) override {
        if (cISC4OccupantManager* occupants = Occupants(); occupants && occupant) occupants->RemoveOccupant(occupant, shutdown, data);
    }
    void ChangeModelInRenderer(cSC4UndergroundConnectionOccupant* connection, int change) override {
        if (CityRenderer() && connection) CityRendererChangeModel(static_cast<s3d::cIS3DModelInstance*>(connection), change);
    }

private:
    UndergroundOccupantHost occupantHost_;
};

// The data views' filter asks for the state of the lot under a lot texture (spLotManager GetLot(x, z, false), vslot 0x84).
class ViewFilterHost final : public IComplexViewFilterHost {
public:
    int LotStateAt(int x, int z) override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::ZoneDeveloper) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return -1;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        auto* lots = serializable->GetGZCLSID() == cSC4ZoneDeveloper::kZoneDeveloperClassId ? dynamic_cast<cSC4ZoneDeveloper*>(serializable) : nullptr;
        serializable->Release();
        cSC4Lot* lot = lots ? lots->GetLot(x, z, false) : nullptr;
        return lot ? static_cast<int>(lot->GetState()) : -1;
    }
};

// The pipe view's custom view: the city's size, its cells' occupants and the message server.
class CustomViewHost final : public IUndergroundCustomViewHost {
public:
    int CityCellCountX() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CityCellCountZ() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    void CellOccupants(int x, int z, std::list<cISC4Occupant*>& out) override { native::CellOccupants(x, z, out); }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
};

}  // namespace

void NativeUndergroundCellsReport(int x, int z) {
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            std::list<cISC4Occupant*> occupants;
            CellOccupants(x + dx, z + dz, occupants);
            int pipes = 0, others = 0;
            std::uint32_t edges = 0, flags = 0;
            bool h0 = false, h1 = false, h2 = false;
            for (cISC4Occupant* occupant : occupants) {
                void* raw = nullptr;
                if (occupant->QueryInterface(kGZCLSID_cSC4PipeOccupant, &raw) && raw) {
                    auto* pipe = static_cast<cSC4PipeOccupant*>(raw);
                    ++pipes;
                    edges = pipe->GetEdgeTypesFlag(static_cast<std::uint8_t>(kPipeNetworkType));
                    flags = pipe->GetNetworkFlag();
                    pipe->GetHiddenTriangles(h0, h1, h2);
                    pipe->Release();
                } else if (occupant->GetType() == 0xc772bf98u) {
                    ++others;
                }
                occupant->Release();
            }
            const cSTETerrain* terrain = Terrain();
            const std::uint32_t cx = std::uint32_t(x + dx), cz = std::uint32_t(z + dz);
            std::printf("pipe cells: %d,%d pipes %d edges %08x flags %08x hidden %d%d%d, other network pieces %d; terrain even %d odd %d flipped %d\n",
                        x + dx, z + dz, pipes, edges, flags, int(h0), int(h1), int(h2), others,
                        terrain ? int(terrain->IsEvenTriangleHidden(cx, cz)) : -1, terrain ? int(terrain->IsOddTriangleHidden(cx, cz)) : -1,
                        terrain ? int(terrain->IsTriangulationFlipped(cx, cz)) : -1);
        }
    }
}

void InstallUndergroundHosts() {
    static UndergroundToolHost tool;
    static UndergroundOccupantHost occupant;
    SetUndergroundNetworkToolHost(&tool);
    SetUndergroundOccupantHost(&occupant);
    static UndergroundConnectionHost connection;
    SetUndergroundConnectionHost(&connection);
    SetUndergroundConnectionViewHost(&connection);
    static ViewFilterHost filter;
    static CustomViewHost customView;
    SetComplexViewFilterHost(&filter);
    SetUndergroundCustomViewHost(&customView);
}

}  // namespace native

namespace native {

void NativeSubwayReport(int limit, bool paths) {
    cSTETerrain* terrain = Terrain();
    if (!terrain) {
        std::printf("subways: no city\n");
        return;
    }
    const int countX = static_cast<int>(terrain->CellCountX()), countZ = static_cast<int>(terrain->CellCountZ());
    int pieces = 0, modeled = 0, connections = 0;
    for (int z = 0; z < countZ; ++z) {
        for (int x = 0; x < countX; ++x) {
            std::list<cISC4Occupant*> occupants;
            CellOccupants(x, z, occupants);
            for (cISC4Occupant* occupant : occupants) {
                void* raw = nullptr;
                if (occupant->QueryInterface(kGZCLSID_cSC4SubwayOccupant, &raw) && raw) {
                    auto* subway = static_cast<cSC4SubwayOccupant*>(raw);
                    const bool model = !subway->PieceGeometry().empty() || subway->CustomModel() != nullptr;
                    if (model) ++modeled;
                    if (pieces++ < limit) {
                        float position[3] = {};
                        subway->GetPosition(position);
                        std::printf("subway: cell %d,%d piece %05x rotation %u edges %08x flags %08x y %.2f model %s (%zu vertices%s)\n", x, z,
                                    subway->PieceId(), subway->GetRotationAndFlip(),
                                    subway->GetEdgeTypesFlag(static_cast<std::uint8_t>(kSubwayNetworkType)), subway->GetNetworkFlag(),
                                    position[1], model ? "yes" : "no", subway->PieceGeometry().size(), subway->CustomModel() ? ", custom" : "");
                        if (paths) {
                            // the train paths as the traffic simulator reads them (path type 4)
                            cISC4PathInfo* info = subway->GetPathInfo();
                            int trainPaths = 0;
                            float low = 0.0f, high = 0.0f;
                            bool any = false;
                            if (info && info->GetPathMap())
                                for (const auto& [key, path] : *info->GetPathMap()) {
                                    if (((key >> 24) & 0xf) != 4) continue;
                                    for (const cS3DVector3& p : path.points) {
                                        low = !any || p.y < low ? p.y : low;
                                        high = !any || p.y > high ? p.y : high;
                                        any = true;
                                    }
                                    ++trainPaths;
                                }
                            std::printf("subway paths: cell %d,%d path info %s, %d train paths, heights %.2f..%.2f, connections %u, floor y %.2f %.2f %.2f %.2f\n", x, z,
                                        info ? "yes" : "no", trainPaths, low, high, unsigned(subway->PathConnectionExists(4, 0xff, 0xff)),
                                        subway->PathBottomVertex(0).y, subway->PathBottomVertex(1).y, subway->PathBottomVertex(2).y,
                                        subway->PathBottomVertex(3).y);
                            // the traffic simulator's subway travel (type 6) connections of the cell and its walkers' switches
                            cIGZUnknown* object = HostCity() ? HostCity()->GetComponent(CityComponent::TrafficSimulator) : nullptr;
                            if (auto* simulator = object ? dynamic_cast<cSC4TrafficSimulator*>(object) : nullptr)
                                std::printf("subway traffic: cell %d,%d subway mask %04x walk mask %04x walk switches %04x\n", x, z,
                                            simulator->CellConnections(x, z).masks[6], simulator->CellConnections(x, z).masks[0],
                                            simulator->CellConnections(x, z).switches[0]);
                        }
                    }
                    subway->Release();
                } else if (occupant->QueryInterface(kGZCLSID_cSC4SubwayConnectionOccupant, &raw) && raw) {
                    auto* connection = static_cast<cSC4SubwayConnectionOccupant*>(raw);
                    if (connections++ < limit) {
                        float position[3] = {};
                        connection->GetPosition(position);
                        std::printf("subway connection: cell %d,%d position %.1f,%.1f,%.1f network piece %s building %s\n", x, z, position[0],
                                    position[1], position[2], connection->NetworkOccupant() ? "yes" : "no",
                                    connection->BuildingOccupant() ? "yes" : "no");
                    }
                    connection->Release();
                }
                occupant->Release();
            }
        }
    }
    std::printf("subways: %d pieces (%d with a model), %d connections\n", pieces, modeled, connections);
}

}  // namespace native
