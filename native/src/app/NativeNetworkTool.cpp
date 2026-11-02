#include "app/NativeNetworkTool.h"

#include <cmath>
#include <cstdio>
#include <vector>

#include "app/NativeAudio.h"
#include "app/NativeCityView.h"
#include "app/NativeZoneTool.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/NetworkTool.h"
#include "sc4/NetworkToolUI.h"
#include "sc4/NetworkPreview.h"
#include "app/NativeModelMaker.h"
#include "sc4/NetworkWorldCache.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/TileRules.h"
#include "sc4/Exemplar.h"
#include "sc4/FloraOccupant.h"
#include "gz/cGZPersistResourceManager.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceKey.h"
#include <string>
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlNetworkTool.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/OccupantManager.h"

namespace native {
namespace {

sc4::cSC4City* g_city = nullptr;
sc4::cSC4View3DWin* g_view = nullptr;
bool g_gridShown = false;

sc4::cSTETerrain* Terrain() {
    cIGZUnknown* object = g_city ? g_city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
sc4::cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = g_city ? g_city->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
sc4::cSC4BudgetSimulator* Budget() {
    cIGZUnknown* object = g_city ? g_city->GetComponent(sc4::CityComponent::BudgetSimulator) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    auto* out = serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4BudgetSimulator ? static_cast<sc4::cSC4BudgetSimulator*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
SC4ViewUtilities* ViewUtilities() {
    sc4::cSC43DRender* render = CityRenderer();
    return render ? render->GetViewUtilities() : nullptr;
}

// sNetworkTypeInfo (i386 0x00c1fcc0, 13 records of 0x114 bytes) as cSC4NetworkTool::ReloadNetworkTypeInfo fills it
// (i386 0x0012320a): the ids, cursors and pen widths it writes directly. The exemplar-loaded parts (LoadPlacementParams,
// LoadSupports, LoadBridgeTypes, LoadEvaluator) are not ported yet: the placement block stays zero and there are no
// bridge or support types.
struct TypeRecord {
    std::uint32_t vic, exemplar, toolView, cursor, directionalCursor;
    int penX, penZ;
};
constexpr TypeRecord kTypeRecords[13] = {
    {0xa766c101, 0x2821ed93, 0x00, 0xe17bb266, 0x49e13260, 1, 1},   // 0 road
    {0xc8433ecb, 0xe8347989, 0x00, 0x217bb308, 0x00000000, 1, 1},   // 1 rail
    {0x08433f47, 0xa8434037, 0x00, 0x8bf30a8b, 0x00000000, 2, 2},   // 2 elevated highway
    {0x092a047d, 0xa92a02ea, 0x00, 0x2a920bf9, 0x00000000, 1, 1},   // 3 street
    {0x09511847, 0xa951171c, 0xf0, 0x82c2ff49, 0x00000000, 1, 1},   // 4 pipe
    {0xa88e1592, 0x088e1962, 0x00, 0xa16f1463, 0x69e13770, 1, 2},   // 5 power line
    {0x0b730a26, 0xcb730fac, 0x00, 0x6b730b83, 0x00000000, 2, 2},   // 6 avenue
    {0x8a15ee13, 0x8a15f3f2, 0xf1, 0xc17bb329, 0x00000000, 1, 1},   // 7 subway
    {0x0b79e2e1, 0x2b79dffb, 0x00, 0xab79e761, 0x00000000, 1, 1},   // 8 light rail
    {0xabe08433, 0xebe084c2, 0x00, 0xebe091b6, 0x00000000, 1, 1},   // 9 monorail
    {0x2be08438, 0xcbe084cb, 0x00, 0xabe091a8, 0x00000000, 1, 1},   // 10 one-way road
    {0x4be08afb, 0x6be08658, 0x00, 0x4be091b2, 0x00000000, 1, 1},   // 11 dirt road
    {0x6be0843b, 0xebe084d1, 0x00, 0x6be091b4, 0x00000000, 2, 2},   // 12 ground highway
};

// sNetworkTypeInfo +0: 1 = simple edge subtypes, 2 = complex (ReloadNetworkTypeInfo).
constexpr int kTypeKinds[13] = {1, 2, 0, 1, 1, 0, 0, 1, 2, 2, 1, 1, 0};

class TypeInfo final : public sc4::INetworkTypeInfo {
public:
    explicit TypeInfo(std::uint32_t type) : type_(type) {
        // LoadPlacementParams(kNetworkPlacementExemplarIDs[type]) (the power line has none).
        const std::uint32_t id = sc4::kNetworkPlacementExemplarIDs[type];
        void* raw = nullptr;
        if (id && SL::spRM && SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, sc4::kNetworkPlacementExemplarGroup, id}, 0xa52160f5, &raw, 0, nullptr) && raw) {
            auto* exemplar = static_cast<sc4::cISCResExemplar*>(raw);
            sc4::LoadNetworkPlacementParams(exemplar->AsISCPropertyHolder(), params_);
            exemplar->Release();
        }
    }
    std::uint32_t ViewInputControlId() const override { return kTypeRecords[type_].vic; }
    std::uint32_t ToolViewId() const override { return kTypeRecords[type_].toolView; }
    std::uint32_t CursorId() const override { return kTypeRecords[type_].cursor; }
    std::uint32_t DirectionalCursorBase() const override { return kTypeRecords[type_].directionalCursor; }
    std::uint32_t DefaultExemplarId() const override { return kTypeRecords[type_].exemplar; }
    int PenWidthX() const override { return kTypeRecords[type_].penX; }
    int PenWidthZ() const override { return kTypeRecords[type_].penZ; }
    const std::vector<sc4::cSC4NetworkBridgeTypeInfo>& BridgeTypes() const override { return bridges_; }
    const sc4::cSC4NetworkPlacementParams& PlacementParams() const override { return params_; }
    const sc4::cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t) const override { return nullptr; }
    std::uint32_t PlacementExemplarId() const override { return sc4::kNetworkPlacementExemplarIDs[type_]; }

private:
    std::uint32_t type_;
    std::vector<sc4::cSC4NetworkBridgeTypeInfo> bridges_;
    sc4::cSC4NetworkPlacementParams params_{};
};

// cSC4NetworkPreview's terrain and textures.
class PreviewHost final : public sc4::INetworkPreviewHost {
public:
    float VertexAltitude(int x, int z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetAltitudeAtVertex(x, z) : 0.0f; }
    float WaterDepth(int x, int z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetWaterDepthAtVertex(x, z) : 0.0f; }
    bool WaterShown() override { sc4::cSTETerrain* t = Terrain(); return t && t->GetWaterSurfaceDisplayState(); }
    bool TriangulationFlipped(int x, int z) override {
        sc4::cSTETerrain* t = Terrain();
        return t && t->IsTriangulationFlipped(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
    }
    int Texture(const cGZPersistResourceKey& key) override { return static_cast<int>(FshTextureId(key.group, key.instance)); }
};
PreviewHost g_previewHost;

class ToolHost final : public sc4::INetworkToolHost {
public:
    void RegisterShutdownCallback(bool (*)(void*), void*) override {}
    cIGZUnknown* AcquireTerrain() override { return nullptr; }   // the solver reaches the terrain through this host
    float TerrainCellSize() override { return 16.0f; }
    float TerrainVertexHeight(std::int32_t vertex) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetAltitude(vertex) : 0.0f; }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override { sc4::cSTETerrain* t = Terrain(); return t && t->CellHasWater(x, z); }
    int CityWidth() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CityHeight() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    cIGZUnknown* AcquireOccupantManager() override { return nullptr; }
    cIGZUnknown* AcquireCityViewModel() override { return nullptr; }
    // SL::spRenderer vslot 0x100 / 0x108 (layer 5, priority 1000).
    void AddRenderLayer(sc4::INetworkPreview* layer, std::uint32_t list, std::int32_t priority) override {
        if (auto* view = dynamic_cast<sc4::cSC4NetworkPreview*>(layer); view && CityRenderer())
            CityRenderer()->AddViewObject(view, static_cast<int>(list), static_cast<std::uint32_t>(priority));
    }
    void RemoveRenderLayer(sc4::INetworkPreview* layer) override {
        if (auto* view = dynamic_cast<sc4::cSC4NetworkPreview*>(layer); view && CityRenderer()) CityRenderer()->RemoveViewObject(view);
    }
    std::uint32_t RendererAltitudeScale() override { return 0; }
    void RegisterMessage(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->AddNotification(target, type); }
    void UnregisterMessage(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, type); }
    cIGZUnknown* CreateNetworkOrFilter(std::uint32_t) override { return nullptr; }
    sc4::INetworkPreview* CreatePreview() override { return new sc4::cSC4NetworkPreview(); }
    sc4::INetworkWorldCache* CreateWorldCache() override { return new sc4::cSC4NetworkWorldCache(); }
    sc4::INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return nullptr; }
    const sc4::INetworkTypeInfo* NetworkTypeInfo(std::uint32_t type) override {
        // Never destroyed: the input controls shut down from the window manager's destructor at exit, after function statics.
        static auto* infos = [] {
            auto* v = new std::vector<TypeInfo>();
            for (std::uint32_t t = 0; t < 13; ++t) v->emplace_back(t);
            return v;
        }();
        return type < infos->size() ? &(*infos)[type] : nullptr;
    }
    sc4::cSC4NetworkCellInfo* GetCellInfo(std::uint32_t) override { return nullptr; }
    cIGZUnknown* GetTuningProperty(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t) override { return nullptr; }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    sc4::cSC4NetworkOccupant* NetworkOccupantAt(std::uint32_t x, std::uint32_t z, std::uint32_t) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        sc4::cISC4Occupant* occupant = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByStandardCityCell(occupant, static_cast<int>(x), static_cast<int>(z), 0xc772bf98u) || !occupant) return nullptr;
        // The original QIs 0xa821ef94; the port's network occupant does not answer that iid (other seams cast the
        // result to their own interfaces), so the object is taken directly and keeps the lookup's reference.
        auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        if (!network) occupant->Release();
        return network;
    }
    float WaterSurfaceAtVertex(std::int32_t x, std::int32_t z) override {
        sc4::cSTETerrain* t = Terrain();
        if (!t || !t->IsWaterVertex(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z))) return -3.4e38f;
        return t->GetWaterLevel(static_cast<std::int32_t>(z * (static_cast<std::int32_t>(t->CellCountX()) + 1) + x));
    }
    int NetworkTypeKind(std::uint32_t type) override { return type < 13 ? kTypeKinds[type] : 0; }
    int InsertNetworkOccupant(sc4::cSC4NetworkOccupant* occupant, std::uint32_t data) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        return occupants && occupants->InsertOccupant(static_cast<sc4::cISC4Occupant*>(occupant), data) ? 1 : 0;
    }
    // The network occupants of the cell leave the manager. NOT PORTED: the flora on the cell (spDemolition).
    void ClearCellForNetwork(std::uint32_t x, std::uint32_t z, std::uint32_t, std::uint32_t) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        for (int guard = 0; occupants && guard < 8; ++guard) {
            sc4::cISC4Occupant* occupant = nullptr;
            if (!occupants->GetFirstOccupantByStandardCityCell(occupant, static_cast<int>(x), static_cast<int>(z), 0xc772bf98u) || !occupant) break;
            occupants->RemoveOccupant(occupant, true, 0);
            occupant->Release();
        }
    }
    bool HasPrebuiltModelExemplar(std::uint32_t type, std::uint32_t piece) override {
        std::vector<std::uint8_t> bytes;
        return type < 13 && LoadResourceBytes(0x6534284a, kTypeRecords[type].exemplar, piece, bytes);
    }
    // The demolition a road makes. INTERIM: cSC4Demolition has no native host yet, so only the flora on the cells goes
    // (free, as the original's flora demolition); lots and buildings under a drag are not handled.
    bool DemolishForNetwork(bool commit, const std::vector<sc4::SC4PointU>& cells, std::int64_t& cost) override {
        cost = 0;
        if (!commit) return true;
        sc4::cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return true;
        for (const sc4::SC4PointU& cell : cells) {
            for (int guard = 0; guard < 16; ++guard) {
                sc4::cISC4Occupant* occupant = nullptr;
                if (!occupants->GetFirstOccupantByStandardCityCell(occupant, static_cast<int>(cell.x), static_cast<int>(cell.z), sc4::kOccupantTypeFlora) || !occupant) break;
                occupants->RemoveOccupant(occupant, true, 0);
                occupant->Release();
            }
        }
        return true;
    }
    std::uint32_t PieceTexture(std::uint32_t piece, int&) override {
        int zoom = 0, rotation = 0;
        if (SC4ViewUtilities* v = ViewUtilities()) v->GetZoomAndRotation(zoom, rotation);
        return piece + ((piece & 0xf) == 0 ? static_cast<std::uint32_t>(zoom) : 0u);   // as cSC4NetworkOccupantModel::GetTexture
    }
    int CurrentViewLevel() override { return g_view ? static_cast<int>(g_view->GetCurrentViewLevel()) : 1; }

};

class UIHost final : public sc4::INetworkToolUIHost {
public:
    void PlaySound(std::uint32_t id) override { PlayGameSound(id); }
    void StartTick(cIGZMessageTarget2* target) override { StartToolTick(target, 0xff); }
    void StopTick(cIGZMessageTarget2* target) override { StopToolTick(target); }
    void EnterToolView(std::uint32_t) override {}   // the pipe / subway views (SL::ViewManager) are not ported
    void LeaveToolView(std::uint32_t) override {}
    bool SetGridShown(bool shown) override { const bool old = g_gridShown; g_gridShown = shown; return old; }
    void ClearTerrainHighlight() override {}
    void BeginNetworkDrag() override {}
    void EndNetworkDrag() override {}
    std::int64_t Funds() override { sc4::cSC4BudgetSimulator* b = Budget(); return b ? b->GetTotalFunds() : 0; }
    bool SpendFunds(std::uint32_t cost) override { sc4::cSC4BudgetSimulator* b = Budget(); return b && b->WithdrawFunds(static_cast<std::int64_t>(cost)); }
    float CellSize() override { return 16.0f; }
    int CellCountX() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) : 0; }
    int CellCountZ() override { sc4::cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountZ()) : 0; }
    float SeaLevel() override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    int ZoomLevel() override { int zoom = 0, rotation = 0; if (SC4ViewUtilities* v = ViewUtilities()) v->GetZoomAndRotation(zoom, rotation); return zoom; }
    bool IntersectViewportWithPlane(int x, int y, float altitude, float out[3]) override {
        SC4ViewUtilities* v = ViewUtilities();
        float origin[3], direction[3];
        if (!v || !v->ViewportPositionToRay(x, y, origin, direction) || direction[1] == 0.0f) return false;
        const float t = (altitude - origin[1]) / direction[1];
        out[0] = origin[0] + direction[0] * t;
        out[1] = altitude;
        out[2] = origin[2] + direction[2] * t;
        return true;
    }
    bool ConfirmNeighborConnection(const sc4::NeighborConnectionInfo&) override { return false; }
    void RefreshCursor() override {}
};

class TerrainViewAdapter final : public sc4::INetworkInputToolTerrainView {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    bool GridShown() override { return g_gridShown; }
};

class InputHost final : public sc4::INetworkInputToolHost {
public:
    sc4::cISC4View3DWin* View() override { return g_view; }
    sc4::cISC4NetworkTool* CreateNetworkTool(std::uint32_t type) override {
        auto* tool = new sc4::cSC4NetworkTool(type);
        tool->AddRef();
        return tool;
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    float CellSize() override { return 16.0f; }
    sc4::INetworkInputToolTerrainView* TerrainView() override { return &terrainView_; }
    int ViewRotation() override { int zoom = 0, rotation = 0; if (SC4ViewUtilities* v = ViewUtilities()) v->GetZoomAndRotation(zoom, rotation); return rotation; }

private:
    TerrainViewAdapter terrainView_;
};

ToolHost g_toolHost;
UIHost g_uiHost;
InputHost g_inputHost;

}  // namespace

void InstallNetworkTool(sc4::cSC4View3DWin& view, cISC4City* city) {
    g_view = &view;
    g_city = dynamic_cast<sc4::cSC4City*>(city);
    sc4::SetNetworkToolHost(&g_toolHost);
    // The tile rules (RUL files) the solver matches drags against, once.
    static bool rulesLoaded = false;
    if (!rulesLoaded) {
        rulesLoaded = true;
        sc4::SetTileRuleResourceLoader([](const cGZPersistResourceKey& key, std::string& out) {
            std::vector<std::uint8_t> bytes;
            if (!LoadResourceBytes(key.type, key.group, key.instance, bytes)) return false;
            out.assign(bytes.begin(), bytes.end());
            return true;
        });
        std::printf("network tool: %d tile rule files loaded\n", sc4::LoadNetworkTileEvaluators());
        sc4::cSC4NetworkTool::ReloadRules();   // the multitype rules need the loader too
    }
    sc4::SetNetworkToolUIHost(&g_uiHost);
    sc4::SetNetworkInputToolHost(&g_inputHost);
    sc4::SetNetworkPreviewHost(&g_previewHost);
}

sc4::cSC4ViewInputControl* CreateNetworkTool(int type) {
    if (type < 0 || type >= 13) return nullptr;
    auto* tool = new sc4::cSC4ViewInputControlNetworkTool(static_cast<std::uint32_t>(type));
    tool->AddRef();
    return tool;
}

}  // namespace native
