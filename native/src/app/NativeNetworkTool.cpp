#include "app/NativeNetworkTool.h"
#include "app/NativeNetworkManager.h"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <cstring>
#include <vector>

#include "app/NativeAudio.h"
#include "app/NativeCityView.h"
#include "app/NativeZoneTool.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/NetworkTool.h"
#include "sc4/SubwayOccupant.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/NetworkToolUI.h"
#include "sc4/NetworkPreview.h"
#include "app/NativeModelMaker.h"
#include "sc4/NetworkWorldCache.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/TileRules.h"
#include "sc4/Exemplar.h"
#include "sc4/FloraOccupant.h"
#include "sc4/Demolition.h"
#include "app/NativeDemolition.h"
#include "gz/cGZPersistResourceManager.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceKey.h"
#include <string>
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/WinMapView.h"
#include "sc4/ViewInputControlNetworkTool.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "gz/RZServices.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/cSTETerrain.h"
#include "sc4/OccupantManager.h"
#include "sc4/PowerLineTool.h"
#include "sc4/NetworkBridgeOccupant.h"
#include "sc4/NetworkManager.h"
#include "sc4/ResourceAsFile.h"
#include "rz/cRZIniFile.h"
#include "sc4/VertexHtConstraintSatisfier.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/Lot.h"

namespace native {
namespace {

// The current city: SL::spCity, as the original asks (a pointer kept from an earlier city outlives it).
sc4::cSC4City* CurrentCity() { return dynamic_cast<sc4::cSC4City*>(SL::spCity); }
sc4::cSC4View3DWin* g_view = nullptr;
bool g_gridShown = false;

sc4::cSTETerrain* Terrain() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
sc4::cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(sc4::kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<sc4::cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
sc4::cSC4BudgetSimulator* Budget() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::BudgetSimulator) : nullptr;
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
// (i386 0x0012320a): the ids, cursors and pen widths it writes directly, then LoadBridgeTypes (the type's section of the
// network ini), LoadSupports (the support structure exemplars of group 0x8974f80f, for the types that have them) and
// LoadPlacementParams. LoadEvaluator is the tile rules' (TileRules.cpp).
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
        // LoadBridgeTypes(ini, kBridgeSections[type], 0x1000 + type, ...) from the network ini {0, 0x8a5971c5, 0x8a5993b9}
        // ReloadRules opens; the power line has no section.
        static constexpr const char* kBridgeSections[13] = {
            "RoadBridgeExemplars", "RailBridgeExemplars", "HighwayBridgeExemplars", "StreetBridgeExemplars",
            "PipeBridgeExemplars", nullptr, "AvenueBridgeExemplars", "SubwayBridgeExemplars", "LightRailBridgeExemplars",
            "MonorailBridgeExemplars", "OneWayRoadBridgeExemplars", "DirtRoadBridgeExemplars", "GroundHighwayBridgeExemplars"};
        if (kBridgeSections[type]) {
            cIGZFile* file = nullptr;
            if (sc4::SC4GetResourceAsFile(cGZPersistResourceKey{sc4::kNetworkManagerIniType, sc4::kNetworkManagerIniGroup, sc4::kNetworkManagerIniInstance}, &file, false) && file) {
                cRZIniFile ini(file);
                sc4::LoadNetworkBridgeTypes(ini, kBridgeSections[type], bridges_);
                file->Release();
            }
        }
        // LoadSupports(0x8974f80f): road, one-way road, dirt road, rail, light rail, monorail and street.
        if (type == 0 || type == 10 || type == 11 || type == 1 || type == 8 || type == 9 || type == 3)
            sc4::LoadNetworkSupportStructures(0x8974f80f, supports_);
        if (!bridges_.empty()) std::printf("network type %u: %zu bridge types, %zu support structures\n", type, bridges_.size(), supports_.size());
        if (std::getenv("SC4_BRIDGE_TYPES"))
            for (const sc4::cSC4NetworkBridgeTypeInfo& b : bridges_)
                std::printf("network type %u bridge %08x \"%s\": span %u..%u, width %u, ramp %u, pylons %u every %u, %s%s, depth %.1f..%.1f, clearance water %.1f land %.1f%s\n", type, b.id,
                            b.name.c_str(), b.minSpan, b.maxSpan, b.width, b.rampLength, b.pylonLength, b.pylonSpacing, b.isWater ? "water" : "",
                            b.isLand ? " land" : "", b.minWaterDepth, b.maxWaterDepth, b.waterClearance, b.landClearance,
                            b.suspensionA && b.suspensionB ? ", suspension" : (b.hasCables ? ", cables" : ""));
        sc4::LoadNetworkSlopeLimits(params_, slopeLimits_);   // the tail of LoadPlacementParams, run with or without the exemplar
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
    const sc4::cSC4NetworkSupportStructInfo* FindSupportStructure(std::uint32_t id) const override {
        auto it = supports_.find(id);
        return it == supports_.end() ? nullptr : &it->second;
    }
    std::uint32_t PlacementExemplarId() const override { return sc4::kNetworkPlacementExemplarIDs[type_]; }
    const sc4::cSC4NetworkSlopeLimits& SlopeLimits() const override { return slopeLimits_; }

private:
    std::uint32_t type_;
    std::vector<sc4::cSC4NetworkBridgeTypeInfo> bridges_;
    std::map<std::uint32_t, sc4::cSC4NetworkSupportStructInfo> supports_;
    sc4::cSC4NetworkPlacementParams params_{};
    sc4::cSC4NetworkSlopeLimits slopeLimits_{};
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

// cSC4DemolishByNetworksOccupantFilter (i386 0x0022ec0a): every occupant that does not answer interface 0xa9764c8c.
struct DemolishByNetworksFilter final : sc4::IDemolishFilter {
    bool IsOccupantIncluded(sc4::IDemolishable* occupant) override {
        sc4::cISC4Occupant* o = native::DemolitionOccupant(occupant);
        void* raw = nullptr;
        if (!o || !o->QueryInterface(0xa9764c8c, &raw) || !raw) return true;
        static_cast<cIGZUnknown*>(raw)->Release();
        return false;
    }
    bool IsOccupantTypeIncluded(std::uint32_t) override { return true; }
} g_byNetworks;

// Every network occupant on cell (x, z) whose network flags meet `typeMask` (what the tool's cSC4NetworkORFilter takes,
// i386 0x002eee88), borrowed for the call.
template <typename Fn>
void ForCellNetworkOccupants(std::uint32_t x, std::uint32_t z, std::uint32_t typeMask, Fn&& fn) {
    sc4::cISC4OccupantManager* occupants = Occupants();
    if (!occupants) return;
    const int cell[2] = {static_cast<int>(x), static_cast<int>(z)};
    sc4::OccupantList list;
    if (!occupants->GetOccupantsByStandardCityCells(list, cell, cell, 0xc772bf98u, 0xffffffffu)) return;
    for (sc4::cISC4Occupant* occupant : list) {
        auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        if (network && (network->GetNetworkFlag() & typeMask) != 0) fn(network);
    }
    occupants->ReleaseOccupantList(list);
}

// The tool's two cSC4VertexHtConstraintSatisfier members (+0x228 / +0x328), reached through the tool's satisfier seam.
class ConstraintSatisfier final : public sc4::INetworkConstraintSatisfier {
public:
    void AddVertexToSolve(int vertexId, float height) override { solver_.AddVertexToSolve(vertexId, height); }
    bool GetHeight(int vertexId, float& out) override { return solver_.GetHeight(vertexId, out); }
    void InsertRangeConstraint(int vertexId, float low, float high) override { solver_.InsertRangeConstraint(vertexId, low, high); }
    void InsertPreferredRangeConstraint(int vertexId, float low, float high) override { solver_.InsertPreferredRangeConstraint(vertexId, low, high); }
    void InsertPreferredValueConstraint(int vertexId, float value) override { solver_.InsertPreferredValueConstraint(vertexId, value); }
    void InsertFixedVertex(int vertexId, float height) override { solver_.InsertFixedVertex(vertexId, height); }
    void InsertEqualityConstraint(std::uint32_t a, std::uint32_t b) override {
        solver_.InsertEqualityConstraint(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b));
    }
    bool Init(float low, float high) override { return solver_.Init(low, high); }
    bool Shutdown() override { return solver_.Shutdown(); }
    bool Solve() override { return solver_.Solve(); }
    bool IsInToSolveSet(int vertexId) override { return solver_.IsInToSolveSet(vertexId); }
    void InsertSlopeConstraint(int a, int b, float maxSlope) override { solver_.InsertSlopeConstraint(a, b, maxSlope); }
    void InsertSmoothnessConstraint(int a, int b, int c, float limit) override { solver_.InsertSmoothnessConstraint(a, b, c, limit); }
    void RewindSolvedVertexList() override { solver_.RewindSolvedVertexList(); }
    bool GetNextSolvedVertexAndItsHeight(int& vertexId, float& height) override {
        std::int32_t id = 0;
        const bool more = solver_.GetNextSolvedVertexAndItsHeight(id, height);
        vertexId = id;
        return more;
    }

private:
    sc4::cSC4VertexHtConstraintSatisfier solver_;
};

// The satisfier's terrain (SL::spTerrain GetVertex): the grid coordinates of a vertex id for its debug dump.
class VertexHtConstraintHost final : public sc4::IVertexHtConstraintHost {
public:
    void GetVertex(std::int32_t index, std::int32_t& x, std::int32_t& z) override {
        x = 0;
        z = 0;
        if (sc4::cSTETerrain* t = Terrain()) t->GetVertex(index, x, z);
    }
};

// The zone developer is the city's lot manager (cISC4LotManager).
sc4::cSC4ZoneDeveloper* LotManager() {
    cIGZUnknown* object = CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::ZoneDeveloper) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    sc4::cSC4ZoneDeveloper* out = serializable->GetGZCLSID() == sc4::cSC4ZoneDeveloper::kZoneDeveloperClassId
                                      ? dynamic_cast<sc4::cSC4ZoneDeveloper*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

class ToolHost final : public sc4::INetworkToolHost {
public:
    void RegisterShutdownCallback(bool (*)(void*), void*) override {}
    // SL::spTerrain, borrowed (the tool counts its own reference); the solver reads the terrain through this host.
    cIGZUnknown* AcquireTerrain() override { return CurrentCity() ? CurrentCity()->GetComponent(sc4::CityComponent::Terrain) : nullptr; }
    float TerrainCellSize() override { return 16.0f; }
    float TerrainVertexHeight(std::int32_t vertex) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetAltitude(vertex) : 0.0f; }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override { sc4::cSTETerrain* t = Terrain(); return t && t->CellHasWater(x, z); }
    // The bridge solve: terrain vslot 0x48 / 0x44 and occupant manager vslot 0xa4 with the tool's cSC4NetworkORFilter.
    float CellMaxSurfaceLevel(std::uint32_t x, std::uint32_t z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetMaxSurfaceLevel(x, z) : 0.0f; }
    void CellCornerAltitudes(std::uint32_t x, std::uint32_t z, float out[4]) override {
        sc4::cSTETerrain* t = Terrain();
        if (t) t->GetCellCornerAltitudes(x, z, out);
        else out[0] = out[1] = out[2] = out[3] = 0.0f;
    }
    bool NetworkOccupantInRect(int x0, int z0, int x1, int z1, std::uint32_t typeMask) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return false;
        const int a[2] = {x0, z0}, b[2] = {x1, z1};
        sc4::OccupantList list;
        if (!occupants->GetOccupantsByStandardCityCells(list, a, b, 0xc772bf98u, 0xffffffffu)) return false;
        bool found = false;
        for (sc4::cISC4Occupant* occupant : list) {
            auto* network = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
            if (network && (network->GetNetworkFlag() & typeMask) != 0) found = true;
        }
        occupants->ReleaseOccupantList(list);
        return found;
    }
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
    sc4::INetworkConstraintSatisfier* CreateConstraintSatisfier() override { return new ConstraintSatisfier(); }
    // The height solve's terrain and lot calls.
    std::int32_t TerrainVertexIndex(std::int32_t x, std::int32_t z) override { sc4::cSTETerrain* t = Terrain(); return t ? t->GetVertIndex(x, z) : -1; }
    float TerrainMaxAllowedHeight() override { return sc4::cSTETerrain::MaxAllowedHeight(); }
    void SetTerrainVertexHeight(std::int32_t vertex, float height) override { if (sc4::cSTETerrain* t = Terrain()) t->SetAltitude(vertex, height); }
    void BeginLongTerrainOperation() override { if (sc4::cSTETerrain* t = Terrain()) t->BeginLongTerrainOperation(); }
    void RedisplayTerrain(std::int32_t minX, std::int32_t minZ, std::int32_t maxX, std::int32_t maxZ, std::uint32_t flags) override {
        if (sc4::cSTETerrain* t = Terrain()) t->RedisplayTerrain(true, true, sc4::TerrainRect{minX, minZ, maxX, maxZ}, flags);
    }
    void EndLongTerrainOperation() override { if (sc4::cSTETerrain* t = Terrain()) t->EndLongTerrainOperation(); }
    void LotVertexAltitudeAdjustmentRange(std::int32_t x, std::int32_t z, float& minAltitude, float& maxAltitude) override {
        if (sc4::cSC4ZoneDeveloper* lots = LotManager()) {
            lots->GetLotVertexAltitudeAdjustmentRange(x, z, minAltitude, maxAltitude);
            return;
        }
        sc4::INetworkToolHost::LotVertexAltitudeAdjustmentRange(x, z, minAltitude, maxAltitude);
    }
    bool LotBoundingRectAt(std::uint32_t x, std::uint32_t z, std::int32_t (&rect)[4]) override {
        sc4::cSC4ZoneDeveloper* lots = LotManager();
        sc4::cSC4Lot* lot = lots ? lots->GetLot(static_cast<int>(x), static_cast<int>(z), false) : nullptr;
        if (!lot) return false;
        lot->GetBoundingRect(rect);
        return true;
    }
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
    // SL::spRM GetResource({type, group, instance}, iid): the power line tool's pole exemplars. One reference, or null.
    cIGZUnknown* GetTuningProperty(std::uint32_t type, std::uint32_t group, std::uint32_t instance, std::uint32_t iid) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{type, group, instance}, iid, &raw, 0, nullptr) || !raw) return nullptr;
        return static_cast<cIGZUnknown*>(raw);
    }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    // GetNetworkOccupant (i386 0x00126b82): the first occupant of the cell the tool's cSC4NetworkORFilter takes (a network
    // occupant with one of the tool's network flags), one reference.
    sc4::cSC4NetworkOccupant* NetworkOccupantAt(std::uint32_t x, std::uint32_t z, std::uint32_t typeMask) override {
        sc4::cSC4NetworkOccupant* found = nullptr;
        ForCellNetworkOccupants(x, z, typeMask, [&found](sc4::cSC4NetworkOccupant* network) {
            if (found) return;
            network->AddRef();
            found = network;
        });
        return found;
    }
    // DestroyOccupantsAndResolve's outside calls (network-tools.md §10).
    void SendOccupantDemolished(sc4::cSC4NetworkOccupant* occupant) override {
        if (!SL::spMS2 || !occupant) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(sc4::kMessageOccupantDemolished);
        message->SetVoid1(static_cast<sc4::cISC4Occupant*>(occupant));
        message->SetIGZUnknown(static_cast<sc4::cISC4Occupant*>(occupant));
        SL::spMS2->MessageSend(message);
        message->Release();
    }
    bool RemoveNetworkOccupant(sc4::cSC4NetworkOccupant* occupant) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        return occupants && occupant && occupants->RemoveOccupant(occupant, false, 0);
    }
    void RemovePreviewOccupant(sc4::cSC4NetworkOccupant* occupant) override {
        if (sc4::cISC4OccupantManager* occupants = Occupants(); occupants && occupant) occupants->RemoveOccupant(occupant, true, 0);
    }
    void GetIntersectionPieces(sc4::cSC4NetworkOccupant* occupant, std::vector<sc4::cSC4NetworkOccupant*>& out) override {
        NetworkIntersectionPieces(occupant, out);
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
    // ClearNetworkOccsFromCell (i386 0x00125444): the occupants of the cell the tool's network filter takes leave the manager;
    // for a surface network (not pipe, rail-underground or subway: types 4, 5, 7) a flora occupant on the cell is demolished
    // (DemolishOccupant(true, flora, privilege 1, 0x2001, null, false, null)).
    void ClearCellForNetwork(std::uint32_t x, std::uint32_t z, std::uint32_t typeMask, std::uint32_t networkType) override {
        sc4::cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return;
        std::vector<sc4::cSC4NetworkOccupant*> matches;
        ForCellNetworkOccupants(x, z, typeMask, [&matches](sc4::cSC4NetworkOccupant* network) {
            network->AddRef();
            matches.push_back(network);
        });
        for (sc4::cSC4NetworkOccupant* network : matches) {
            occupants->RemoveOccupant(network, true, 0);
            network->Release();
        }
        if (networkType == 4 || networkType == 5 || networkType == 7) return;
        sc4::cISC4Occupant* flora = nullptr;
        if (occupants->GetFirstOccupantByStandardCityCell(flora, static_cast<int>(x), static_cast<int>(z), sc4::kOccupantTypeFlora) && flora) {
            native::DemolishOccupant(true, flora, 0x2001, nullptr);
            flora->Release();
        }
    }
    bool HasPrebuiltModelExemplar(std::uint32_t type, std::uint32_t piece) override {
        std::vector<std::uint8_t> bytes;
        return type < 13 && LoadResourceBytes(0x6534284a, kTypeRecords[type].exemplar, piece, bytes);
    }
    // DestroyHilightedOccupants (i386 0x00122e3c) and the pricing: spDemolition DemolishRegion(commit, the cells, privilege 1,
    // 0x2001, false, cSC4DemolishByNetworksOccupantFilter, cost).
    bool DemolishForNetwork(bool commit, const std::vector<sc4::SC4PointU>& cells, std::int64_t& cost) override {
        cost = 0;
        if (cells.empty()) return true;
        sc4::DemolitionRegion region;
        region.bounds = sc4::DemolitionCellRect{static_cast<int>(cells[0].x), static_cast<int>(cells[0].z), static_cast<int>(cells[0].x), static_cast<int>(cells[0].z)};
        for (const sc4::SC4PointU& c : cells) {
            region.bounds.x0 = std::min(region.bounds.x0, static_cast<int>(c.x));
            region.bounds.z0 = std::min(region.bounds.z0, static_cast<int>(c.z));
            region.bounds.x1 = std::max(region.bounds.x1, static_cast<int>(c.x));
            region.bounds.z1 = std::max(region.bounds.z1, static_cast<int>(c.z));
        }
        region.mask.assign(static_cast<std::size_t>(region.bounds.x1 - region.bounds.x0 + 1),
                           std::vector<bool>(static_cast<std::size_t>(region.bounds.z1 - region.bounds.z0 + 1), false));
        for (const sc4::SC4PointU& c : cells) region.mask[c.x - region.bounds.x0][c.z - region.bounds.z0] = true;
        return native::DemolishRegion(commit, region, 1, 0x2001, false, &cost, nullptr, 0, 0, &g_byNetworks);
    }
    std::uint32_t PieceTexture(std::uint32_t piece, int&) override {
        int zoom = 0, rotation = 0;
        if (SC4ViewUtilities* v = ViewUtilities()) v->GetZoomAndRotation(zoom, rotation);
        return piece + ((piece & 0xf) == 0 ? static_cast<std::uint32_t>(zoom) : 0u);   // as cSC4NetworkOccupantModel::GetTexture
    }
    int CurrentViewLevel() override { return g_view ? static_cast<int>(g_view->GetCurrentViewLevel()) : 1; }
    // SL::spNeighborsSim: the neighbour-city connections a drag to the city edge makes
    bool HasNeighborsSimulator() override { return native::CityNeighborsSimulator() != nullptr; }
    const void* NeighborCityAcross(int x, int z) override {
        sc4::cSC4NeighborsSimulator* neighbors = native::CityNeighborsSimulator();
        return neighbors ? static_cast<const void*>(neighbors->FindCityOnOtherSideOfConnection(x, z)) : nullptr;
    }
    bool NeighborConnectionAt(std::uint32_t type, int x, int z) override {
        sc4::cSC4NeighborsSimulator* neighbors = native::CityNeighborsSimulator();
        return neighbors && neighbors->FindConnection(type, x, z) != nullptr;
    }
    void CreateNeighborConnection(std::uint32_t type, int x, int z, bool flag) override {
        if (sc4::cSC4NeighborsSimulator* neighbors = native::CityNeighborsSimulator()) neighbors->CreateConnection(type, x, z, flag);
    }
    void DeleteNeighborConnection(std::uint32_t type, int x, int z) override {
        if (sc4::cSC4NeighborsSimulator* neighbors = native::CityNeighborsSimulator()) neighbors->DeleteConnection(type, x, z);
    }
    // the language manager's GetLanguageUtility(0) -> GetLanguageRoadDrivingSide (non-zero = right-hand traffic)
    // A tunnel portal's other end (QI 0x8a4bd52b, GetOtherEndOccupant), one reference.
    sc4::cSC4NetworkOccupant* TunnelOtherEnd(sc4::cSC4NetworkOccupant* portal) override;
    // InsertTunnelPiece's terrain cut: cSTETerrain SetAltitude(index, height), then the redisplay of the touched vertices
    // bracketed by the texture-update accumulation.
    void StartTerrainTextureUpdates() override { if (sc4::cSTETerrain* t = Terrain()) t->StartAccumulatingTextureUpdates(); }
    void RedisplayTerrain(std::int32_t x0, std::int32_t z0, std::int32_t x1, std::int32_t z1) override {
        if (sc4::cSTETerrain* t = Terrain()) t->RedisplayTerrain(true, true, sc4::TerrainRect{x0, z0, x1, z1}, 6);
    }
    void StopTerrainTextureUpdates() override { if (sc4::cSTETerrain* t = Terrain()) t->StopAccumulatingTextureUpdates(); }
    bool RoadDrivingSideRight() override {
        cIGZLanguageManager* languages = RZ::LM();
        cIGZLanguageUtility* utility = languages ? languages->GetLanguageUtility(0) : nullptr;
        return utility == nullptr || static_cast<std::uint8_t>(utility->GetLanguageRoadDrivingSide()) != 0;
    }

};

// The 3D view's view manager (the map view, iid 0x67c5bede), for one call.
template <typename Fn>
void WithViewManager(Fn&& fn) {
    cIGZUnknown* unknown = g_view ? g_view->GetViewManager() : nullptr;
    if (!unknown) return;
    fn(static_cast<sc4::cISC4ViewManager*>(unknown));
    unknown->Release();
}

class UIHost final : public sc4::INetworkToolUIHost {
public:
    void PlaySound(std::uint32_t id) override { PlayGameSound(id); }
    void StartTick(cIGZMessageTarget2* target) override { StartToolTick(target, 0xff); }
    void StopTick(cIGZMessageTarget2* target) override { StopToolTick(target); }
    // SL::ViewManager() vslot 0x10 / 0x14: the map view's PushView / PopView (the pipe tool's view 0xf0 is the pipe view).
    void EnterToolView(std::uint32_t view) override { WithViewManager([view](sc4::cISC4ViewManager* m) { m->PushView(view); }); }
    void LeaveToolView(std::uint32_t view) override { WithViewManager([view](sc4::cISC4ViewManager* m) { m->PopView(view); }); }
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
    // cSC4NetworkToolUI::DoPlacement: SL::spNeighborsSim vslot 0x14 RequestCreateConnection(type, x, z, flag) asks the player
    bool ConfirmNeighborConnection(const sc4::NeighborConnectionInfo& info) override {
        sc4::cSC4NeighborsSimulator* neighbors = native::CityNeighborsSimulator();
        return neighbors && neighbors->RequestCreateConnection(info.unknown8, static_cast<int>(info.x), static_cast<int>(info.z), info.unknownC != 0);
    }
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
    // GetNetworkTool(type, true): cSC4NetworkTool for the surface types, the pipe tool for type 4 (shared, never destroyed),
    // the power line tool (cSC4PowerLineTool, its own pole solver) for type 5, the subway tool for type 7 (shared like the pipe tool).
    sc4::cISC4NetworkTool* CreateNetworkTool(std::uint32_t type) override {
        sc4::cSC4NetworkTool* tool = type == 4 ? NativePipeTool()
                                   : type == sc4::kSubwayNetworkType ? NativeSubwayTool()
                                   : type == sc4::kNetworkPowerLine ? static_cast<sc4::cSC4NetworkTool*>(new sc4::cSC4PowerLineTool())
                                                                    : new sc4::cSC4NetworkTool(type);
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

void InstallVertexHtConstraintHost() {
    static VertexHtConstraintHost host;
    sc4::SetVertexHtConstraintHost(&host);
}

sc4::cSC4ViewInputControl* CreateNetworkTool(int type) {
    if (type < 0 || type >= 13) return nullptr;
    auto* tool = new sc4::cSC4ViewInputControlNetworkTool(static_cast<std::uint32_t>(type));
    tool->AddRef();
    return tool;
}

}  // namespace native

namespace native {
// Kit verb `netline TYPE X1 Z1 X2 Z2`: a tool of TYPE made as the drag's view input control makes its own (InputHost
// CreateNetworkTool + Init; the network manager's shared tool re-solves the demolished pieces of a placement, so it may not
// be the one placing) lays a line as the drag's release does (cSC4NetworkToolUI::DoPlacement): a dry run with neighbour
// connections on, the possible connection, then the placement with the connection. The player's confirmation dialog is skipped (kits have no input); the neighbour connections of the
// city are counted before and after.
void NativeLayNetworkLine(std::uint32_t type, int x1, int z1, int x2, int z2) {
    if (type >= 13 || type == 4 || type == sc4::kNetworkPowerLine || type == sc4::kNetworkSubway) {
        std::printf("netline: no surface network tool of type %u\n", type);
        return;
    }
    auto* tool = new sc4::cSC4NetworkTool(type);
    tool->AddRef();
    tool->Init();
    sc4::cSC4NeighborsSimulator* neighbors = CityNeighborsSimulator();
    auto count = [&] {
        sc4::ConnectionList list;
        if (neighbors) neighbors->GetAllConnections(list);
        return static_cast<int>(list.size());
    };
    const int before = count();
    const std::vector<sc4::SC4PointU> anchors = {sc4::SC4PointU{static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(z1)},
                                                 sc4::SC4PointU{static_cast<std::uint32_t>(x2), static_cast<std::uint32_t>(z2)}};
    tool->Reset();
    tool->SetAnchorPoints(anchors);
    tool->PlaceAllSegments(false, true, true, type);
    sc4::NeighborConnectionInfo info;
    const bool possible = tool->GetPossibleNeighborConnection(info) && !tool->GetFailureState();
    tool->Reset();
    tool->SetAnchorPoints(anchors);
    tool->PlaceAllSegments(true, true, possible, type);
    const bool failed = tool->GetFailureState();
    if (failed) {
        cRZString reason;
        tool->GetFailureReasonString(reason);
        std::printf("netline: failure reason \"%s\"\n", reason.ToChar());
    }
    for (const sc4::tBridge& bridge : tool->Bridges()) {
        cRZString name;
        tool->GetBridgeName(name, bridge.bridgeType);
        std::printf("netline: bridge %d \"%s\" steps %d..%d, deck %.2f (reference %.2f), %zu pieces\n", bridge.bridgeType, name.ToChar(),
                    bridge.startStep, bridge.endStep, bridge.elevation, bridge.referenceAltitude, bridge.pieces.size());
    }
    tool->ClearCellPreviews();
    tool->Reset();
    std::printf("netline %u %d,%d -> %d,%d: neighbor connection %s", type, x1, z1, x2, z2, possible ? "possible" : "none");
    if (possible) std::printf(" (type %u at %d,%d side %u)", info.unknown8, static_cast<int>(info.x), static_cast<int>(info.z), info.unknownC);
    std::printf(", failure %d, city connections %d -> %d\n", failed ? 1 : 0, before, count());
    if (neighbors && possible) {
        sc4::cISC4NeighborConnection* made = neighbors->FindConnection(info.unknown8, static_cast<int>(info.x), static_cast<int>(info.z));
        std::printf("netline: connection at %d,%d %s\n", static_cast<int>(info.x), static_cast<int>(info.z), made ? "made" : "MISSING");
    }
    tool->Shutdown();
    tool->Release();
}
// Kit verb `bridges`: a water map of the rectangle (the whole city when no rectangle is given; one character per block of
// cells so a row stays within 64 characters: '~' = a cell with water in the block) and every bridge piece in the city.
void NativeBridgeReport(int x0, int z0, int x1, int z1) {
    sc4::cSTETerrain* terrain = Terrain();
    sc4::cISC4OccupantManager* occupants = Occupants();
    if (!terrain || !occupants) {
        std::printf("bridges: no city\n");
        return;
    }
    const int w = static_cast<int>(terrain->CellCountX()), h = static_cast<int>(terrain->CellCountZ());
    if (x0 < 0 || z0 < 0 || x1 < 0 || z1 < 0) {
        x0 = 0;
        z0 = 0;
        x1 = w - 1;
        z1 = h - 1;
    }
    x1 = std::min(x1, w - 1);
    z1 = std::min(z1, h - 1);
    const int block = std::max(1, (std::max(x1 - x0, z1 - z0) + 64) / 64);
    std::printf("bridges: water map x %d..%d z %d..%d, %d cell(s) per character (rows = z, columns = x)\n", x0, x1, z0, z1, block);
    for (int z = z0; z <= z1; z += block) {
        std::string row;
        for (int x = x0; x <= x1; x += block) {
            bool water = false;
            for (int dz = 0; dz < block && !water; ++dz)
                for (int dx = 0; dx < block && !water; ++dx)
                    if (x + dx <= x1 && z + dz <= z1)
                        water = terrain->CellHasWater(static_cast<std::uint32_t>(x + dx), static_cast<std::uint32_t>(z + dz));
            row += water ? '~' : '.';
        }
        std::printf("bridges: z %3d %s\n", z, row.c_str());
    }
    const int a[2] = {0, 0}, b[2] = {w - 1, h - 1};
    sc4::OccupantList list;
    int count = 0;
    if (occupants->GetOccupantsByStandardCityCells(list, a, b, 0xc772bf98u, 0xffffffffu)) {
        for (sc4::cISC4Occupant* occupant : list) {
            auto* piece = dynamic_cast<sc4::cSC4NetworkBridgeOccupant*>(occupant);
            if (!piece) continue;
            ++count;
            std::uint32_t cx = 0, cz = 0;
            piece->GetOccupiedCell(cx, cz);
            float position[3] = {0, 0, 0};
            piece->GetPosition(position);
            std::printf("bridges: piece at %u,%u model %08x rotation %u flags %08x position y %.2f model offset y %.2f water %.2f %s\n", cx, cz,
                        piece->PieceId(), piece->GetRotation(), piece->GetNetworkFlag(), position[1], piece->ModelOffset().y,
                        terrain->GetMaxSurfaceLevel(cx, cz), piece->IsUsable() ? "usable" : "not usable");
        }
        occupants->ReleaseOccupantList(list);
    }
    std::printf("bridges: %d bridge piece(s)\n", count);
}
}  // namespace native

namespace native {
void NativeTerrainHeightsReport(int x0, int z0, int x1, int z1) {
    sc4::cSTETerrain* t = Terrain();
    if (!t) {
        std::printf("terrainheights: no terrain\n");
        return;
    }
    std::printf("terrainheights %d,%d..%d,%d\n", x0, z0, x1, z1);
    for (int z = z0; z <= z1; ++z) {
        std::printf("  z=%3d:", z);
        for (int x = x0; x <= x1; ++x) std::printf(" %7.2f", t->GetAltitudeAtVertex(x, z));
        std::printf("\n");
    }
    // The network pieces on the cells of the rectangle: their four corner heights (vertex order (x,z) (x,z+1) (x+1,z+1) (x+1,z)).
    for (int z = z0; z < z1; ++z) {
        for (int x = x0; x < x1; ++x) {
            ForCellNetworkOccupants(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), 0x1fffu, [x, z](sc4::cSC4NetworkOccupant* network) {
                float h[4] = {};
                network->GetVertexHeights(h);
                std::printf("  piece %d,%d %08x corners %.2f %.2f %.2f %.2f\n", x, z, network->PieceId(), h[0], h[1], h[2], h[3]);
            });
        }
    }
}
}  // namespace native

#include "sc4/NetworkTunnelOccupant.h"
#include "sc4/PathInfo.h"

namespace native {

// Kit verb `tunnelparams TYPE`: the tunnel rules of a network type as its placement exemplar gives them (the trigger and
// end slopes, the portal exemplars, their rotations and clearances).
void NativeTunnelParams(std::uint32_t type) {
    const sc4::INetworkTypeInfo* info = g_toolHost.NetworkTypeInfo(type);
    if (!info) {
        std::printf("tunnelparams: no type %u\n", type);
        return;
    }
    const sc4::cSC4NetworkPlacementParams& p = info->PlacementParams();
    float endSlope = 0.0f;
    std::memcpy(&endSlope, &p.unknown1c, sizeof endSlope);
    std::printf("tunnelparams %u: trigger slope %.2f deg (rise per cell %.3f), end slope %.2f deg, %zu portal exemplars:", type,
                p.maxSlope * 57.29578f, std::tan(p.maxSlope) * 16.0f, endSlope * 57.29578f, p.list60.size());
    for (std::size_t i = 0; i < p.list60.size(); ++i)
        std::printf(" %08x(rot %u, clearance %.2f)", p.list60[i], i < p.list78.size() ? p.list78[i] : 0u, i < p.list90.size() ? p.list90[i] : 0.0f);
    std::printf("\n");
}

// Kit verb `heights X1 Z1 X2 Z2`: the terrain corner heights of the cells along a straight row or column.
void NativeTerrainLine(int x1, int z1, int x2, int z2) {
    sc4::cSTETerrain* terrain = Terrain();
    if (!terrain) return;
    const int dx = x2 > x1 ? 1 : (x2 < x1 ? -1 : 0), dz = z2 > z1 ? 1 : (z2 < z1 ? -1 : 0);
    std::printf("heights %d,%d -> %d,%d:", x1, z1, x2, z2);
    for (int x = x1, z = z1;; x += dx, z += dz) {
        float c[4];
        terrain->GetCellCornerAltitudes(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), c);
        std::printf(" %d,%d=%.1f", x, z, (c[0] + c[1] + c[2] + c[3]) * 0.25f);
        if ((x == x2 && z == z2) || (dx == 0 && dz == 0)) break;
    }
    std::printf("\n");
}

// Kit verb `tunnels`: every tunnel portal occupant of the city: its cell, network flags, portal model, the portal at the
// other end, and its paths (how many end at the other portal's first path points, as MakeTunnelPaths links them).
void NativeTunnelReport() {
    sc4::cISC4OccupantManager* occupants = Occupants();
    sc4::cSTETerrain* terrain = Terrain();
    if (!occupants || !terrain) {
        std::printf("tunnels: no city\n");
        return;
    }
    const int cell0[2] = {0, 0};
    const int cell1[2] = {static_cast<int>(terrain->CellCountX()) - 1, static_cast<int>(terrain->CellCountZ()) - 1};
    sc4::OccupantList list;
    int count = 0;
    if (occupants->GetOccupantsByStandardCityCells(list, cell0, cell1, 0xc772bf98u, 0xffffffffu)) {
        for (sc4::cISC4Occupant* occupant : list) {
            auto* tunnel = dynamic_cast<sc4::cSC4NetworkTunnelOccupant*>(occupant);
            if (!tunnel) continue;
            ++count;
            std::uint32_t x = 0, z = 0;
            tunnel->GetOccupiedCell(x, z);
            std::uint32_t ox = 0, oz = 0;
            sc4::cSC4NetworkOccupant* other = tunnel->GetOtherEndOccupant();
            if (other) other->GetOccupiedCell(ox, oz);
            s3d::cIS3DModelInstance* model = tunnel->PreBuiltModel();
            int paths = 0, linked = 0;
            sc4::cISC4PathInfo* own = tunnel->GetPathInfo();
            sc4::cISC4PathInfo* far = other ? other->GetPathInfo() : nullptr;
            if (own && own->GetPathMap()) {
                for (const auto& [key, path] : *own->GetPathMap()) {
                    ++paths;
                    if (!far || path.points.empty()) continue;
                    const sc4::tPath* match = far->GetPath(key, nullptr);
                    if (!match || match->points.empty()) continue;
                    const cS3DVector3& a = path.points.back();
                    const cS3DVector3& b = match->points.front();
                    if (a.x == b.x && a.y == b.y && a.z == b.z) ++linked;
                }
            }
            float corners[4];
            terrain->GetCellCornerAltitudes(x, z, corners);
            std::printf("tunnel portal %d,%d flags %08x rot %u model %s, other end %s %u,%u, paths %d (%d lead into the other portal), terrain %.1f %.1f %.1f %.1f\n",
                        x, z, tunnel->GetNetworkFlag(), tunnel->GetRotation(), model ? "yes" : "none",
                        other ? "at" : "NONE", ox, oz, paths, linked, corners[0], corners[1], corners[2], corners[3]);
        }
        occupants->ReleaseOccupantList(list);
    }
    std::printf("tunnels: %d portals\n", count);
}

}  // namespace native

namespace native {
namespace {
sc4::cSC4NetworkOccupant* ToolHost::TunnelOtherEnd(sc4::cSC4NetworkOccupant* portal) {
    auto* tunnel = portal ? dynamic_cast<sc4::cSC4NetworkTunnelOccupant*>(portal) : nullptr;
    sc4::cSC4NetworkOccupant* other = tunnel ? tunnel->GetOtherEndOccupant() : nullptr;
    if (other) other->AddRef();
    return other;
}
}  // namespace
}  // namespace native
