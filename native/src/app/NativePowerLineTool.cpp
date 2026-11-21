// The power line tool's and the power poles' engine services (IPowerLineToolHost / IPowerPoleOccupantHost): the city's
// occupant, zone and lot managers, the terrain, the renderer's terrain pick and lighting, the resource manager (the pole
// models, the tuning exemplar, the "PowerPoles" ini), the pole occupants themselves, and the kit verbs that drive the tool
// through its own placement API (`powerline`) and print the poles, wires and the power grid of cells (`poles`).
#include <cstdio>

#include "sc4/Render3D.h"   // before OccupantManager.h: its occupant filter forward declaration is the global one
#include <vector>

#include "app/NativeCityView.h"
#include "app/NativeDemolition.h"
#include "app/NativeEffects.h"
#include "app/NativeLighting.h"
#include "app/NativePower.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZIniFile.h"
#include "sc4/Exemplar.h"
#include "sc4/LightingManager.h"
#include "sc4/ModelMaker.h"
#include "sc4/OccupantManager.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerLineTool.h"
#include "sc4/PowerPoleOccupant.h"
#include "sc4/PowerSimulator.h"
#include "sc4/ResourceAsFile.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/VisualEffect.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {

int NativeZoneTypeAt(int x, int z);                                        // NativeFlammabilityHost.cpp
std::uint32_t NativeFshTexture(std::uint32_t group, std::uint32_t instance);   // NativeModelMaker.cpp
sc4::cSC4NetworkTool* NativeManagerNetworkTool(std::uint32_t type);         // NativeNetworkManager.cpp

namespace {

using namespace sc4;

cSC4City* City() { return dynamic_cast<cSC4City*>(SL::spCity); }

cSTETerrain* Terrain() {
    cIGZUnknown* object = City() ? City()->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}

cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = City() ? City()->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}

// The pole behind an occupant: QueryInterface 0xa8c86142 (cISC4PowerPoleOccupant), one reference.
cSC4PowerPoleOccupant* PoleOf(cIGZUnknown* object) {
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZCLSID_cSC4PowerPoleOccupant, &raw) || !raw) return nullptr;
    auto* pole = dynamic_cast<cSC4PowerPoleOccupant*>(static_cast<cISC4PowerPoleOccupant*>(raw));
    if (!pole) static_cast<cISC4PowerPoleOccupant*>(raw)->Release();
    return pole;
}

// cSC4NetworkRoutines::Get0To3Direction (i386 0x0022eb2c): 2 along z, 0 along x (or the same cell), 1 on the diagonal whose
// steps have the same sign, 3 on the other.
int Get0To3Direction(int x1, int z1, int x2, int z2) {
    const auto ux1 = static_cast<std::uint32_t>(x1), uz1 = static_cast<std::uint32_t>(z1);
    const auto ux2 = static_cast<std::uint32_t>(x2), uz2 = static_cast<std::uint32_t>(z2);
    if (ux1 == ux2) return uz2 != uz1 ? 2 : 0;
    if (uz2 == uz1) return 0;
    const int sx = ux1 < ux2 ? 1 : -1;
    const int sz = uz1 < uz2 ? 1 : -1;
    return sx != sz ? 3 : 1;
}

// The power-line tuning exemplar {0x6534284a, 0xe7e2c2db, 0xc911e35b} (property holder of the resource, kept for the process).
const cISCPropertyHolder* PowerLineTuning() {
    static cISCResExemplar* exemplar = nullptr;
    if (!exemplar && SL::spRM) {
        void* raw = nullptr;
        if (SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0xe7e2c2db, 0xc911e35b}, kGZIID_cISCResExemplar, &raw, 0, nullptr) && raw)
            exemplar = static_cast<cISCResExemplar*>(raw);
    }
    return exemplar ? exemplar->AsISCPropertyHolder() : nullptr;
}

// A wire occupant as the tool sees it (cISC4PowerLineOccupant: GetConnectedPoles).
class LineView final : public IPowerLineOccupant {
public:
    explicit LineView(cSC4PowerLineOccupant* line) : line_(line) { line_->AddRef(); }
    ~LineView() override { line_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    void GetConnectedPoles(IPowerPoleOccupant*& first, IPowerPoleOccupant*& second, bool& connected) override {
        cISC4PowerPoleOccupant* a = nullptr;
        cISC4PowerPoleOccupant* b = nullptr;
        line_->GetConnectedPoles(a, b, connected);
        first = a ? dynamic_cast<IPowerPoleOccupant*>(a) : nullptr;
        second = b ? dynamic_cast<IPowerPoleOccupant*>(b) : nullptr;
        if (a && !first) a->Release();
        if (b && !second) b->Release();
    }

private:
    cSC4PowerLineOccupant* line_;
    std::uint32_t refs_ = 0;
};

// A city occupant as the tool sees it; holds the occupant list's reference.
class OccupantView final : public ICityOccupant {
public:
    explicit OccupantView(cISC4Occupant* occupant) : occupant_(occupant) { occupant_->AddRef(); }
    ~OccupantView() override { occupant_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    std::uint32_t GetType() override { return occupant_->GetType(); }
    void GetPosition(float* xyz) override { occupant_->GetPosition(xyz); }
    IPowerPoleOccupant* QueryPole() override { return PoleOf(occupant_); }
    IPowerLineOccupant* QueryPowerLine() override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(kGZIID_cISC4PowerLineOccupant, &raw) || !raw) return nullptr;
        static_cast<cIGZUnknown*>(raw)->Release();
        auto* line = dynamic_cast<cSC4PowerLineOccupant*>(occupant_);
        if (!line) return nullptr;
        auto* view = new LineView(line);
        view->AddRef();
        return view;
    }
    bool QueriesAs(std::uint32_t iid) override {
        void* raw = nullptr;
        if (!occupant_->QueryInterface(iid, &raw) || !raw) return false;
        static_cast<cIGZUnknown*>(raw)->Release();
        return true;
    }

private:
    cISC4Occupant* occupant_;
    std::uint32_t refs_ = 0;
};

cSC4PowerPoleOccupant* AsPole(IPowerPoleOccupant* pole) { return pole ? dynamic_cast<cSC4PowerPoleOccupant*>(pole) : nullptr; }

class ToolHost final : public IPowerLineToolHost {
public:
    void GetOccupantsInCellRect(std::vector<ICityOccupant*>& out, int x0, int z0, int x1, int z1) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return;
        const int a[2] = {x0, z0}, b[2] = {x1, z1};
        OccupantList list;
        if (occupants->GetOccupantsByStandardCityCells(list, a, b, static_cast<sc4::cISC4OccupantFilter*>(nullptr), 0xffffffffu)) {
            for (cISC4Occupant* occupant : list) {
                auto* view = new OccupantView(occupant);
                view->AddRef();
                out.push_back(view);
            }
        }
        occupants->ReleaseOccupantList(list);
    }
    void PositionToCityCell(float x, float z, int& cx, int& cz) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants) occupants->PositionToStandardCityCell(x, z, cx, cz);
    }
    void InsertPoleOccupant(IPowerPoleOccupant* pole) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && AsPole(pole)) occupants->InsertOccupant(static_cast<cISC4Occupant*>(AsPole(pole)), 0);
    }
    void RemovePoleOccupant(IPowerPoleOccupant* pole, bool shutdown) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && AsPole(pole)) occupants->RemoveOccupant(static_cast<cISC4Occupant*>(AsPole(pole)), shutdown, 0);
    }
    int ZoneAt(int x, int z) override { return NativeZoneTypeAt(x, z); }
    bool HasLotAt(int x, int z) override { return NativeLotAtCell(x, z, false) != nullptr; }
    float TerrainCellSize() override { return 16.0f; }
    bool TerrainAllowsNetwork(std::uint32_t x, std::uint32_t z) override { cSTETerrain* t = Terrain(); return t && t->CellHasWater(x, z); }
    void TerrainCornerHeights(int x, int z, float out[4]) override {
        cSTETerrain* t = Terrain();
        if (!t) return;
        out[0] = t->GetAltitudeAtVertex(x, z);
        out[1] = t->GetAltitudeAtVertex(x, z + 1);
        out[2] = t->GetAltitudeAtVertex(x + 1, z + 1);
        out[3] = t->GetAltitudeAtVertex(x + 1, z);
    }
    float TerrainHeightAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool RendererRayTest(const float* start, const float* direction, float* end) override {
        cSC43DRender* render = CityRenderer();
        return render && render->PickTerrain(start, direction, end, false, false);
    }
    const cISCPropertyHolder* TuningPropertyHolder() override { return PowerLineTuning(); }
    cRZIniFile* OpenPowerPoleIni() override {
        cIGZFile* file = nullptr;
        if (!SC4GetResourceAsFile(cGZPersistResourceKey{0, 0x8a5971c5, 0x8a5993b9}, &file, false) || !file) return nullptr;
        auto* ini = new cRZIniFile(file);
        file->Release();
        return ini;
    }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    int Get0To3Direction(int x1, int z1, int x2, int z2) override { return native::Get0To3Direction(x1, z1, x2, z2); }
    void ClearBaseCellPreviews() override {}
    void DrawNetworkLine(SC4PointU&, SC4PointU&, bool, std::uint32_t) override {}
    bool AttemptNeighborConnections(bool, std::uint32_t) override { return false; }
    void PlaceNeighborConnections() override {}
    IPowerPoleOccupant* CreatePoleOccupant() override { return new cSC4PowerPoleOccupant(); }
    void ConnectPoles(IPowerPoleOccupant* from, IPowerPoleOccupant* to, const std::vector<tLineInfo>* lines) override {
        if (cSC4PowerPoleOccupant* pole = AsPole(from)) pole->AddConnection(AsPole(to), const_cast<std::vector<tLineInfo>*>(lines));
    }
    void RemovePoleConnection(IPowerPoleOccupant* from, IPowerPoleOccupant* to, bool preview) override {
        if (cSC4PowerPoleOccupant* pole = AsPole(from)) pole->RemoveConnection(AsPole(to), preview);
    }
    void DrawPoleConnection(IPowerPoleOccupant* from, IPowerPoleOccupant* to, bool preview) override {
        if (cSC4PowerPoleOccupant* pole = AsPole(from)) pole->DrawConnection(AsPole(to), preview);
    }
};

class PoleHost final : public IPowerPoleOccupantHost {
public:
    float CityCellWidthX() override { return 16.0f; }
    float CityCellWidthZ() override { return 16.0f; }
    float TerrainCellSize() override { return 16.0f; }
    void TerrainCornerHeights(int x, int z, float out[4]) override { tool_.TerrainCornerHeights(x, z, out); }
    bool CellHasWater(int x, int z) override {
        cSTETerrain* t = Terrain();
        return t && x >= 0 && z >= 0 && t->CellHasWater(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
    }
    int Get0To3Direction(int x1, int z1, int x2, int z2) override { return native::Get0To3Direction(x1, z1, x2, z2); }
    int ViewZoom() override { int zoom = 0, rotation = 0; Utilities(zoom, rotation); return zoom; }
    int ViewRotation() override { int zoom = 0, rotation = 0; Utilities(zoom, rotation); return rotation; }
    float CameraYaw() override {
        cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* utilities = render ? render->GetViewUtilities() : nullptr;
        return utilities ? utilities->RotationAngle() : 0.0f;
    }
    std::uint32_t ModelInstanceID(cISC4Occupant* occupant, int zoom, int rotation, int state) override {
        IModelMakerHost* host = ModelMakerHost();
        return host ? host->GetModelInstanceID(occupant, zoom, rotation, state, false, nullptr, nullptr) : 0;
    }
    s3d::cIS3DModelInstance* CreateModelInstance(const cGZPersistResourceKey& key) override {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0x9ad0e7e3, &raw, 0, nullptr) || !raw) return nullptr;
        auto* model = static_cast<s3d::cIS3DResModel*>(static_cast<cIGZUnknown*>(raw));
        s3d::cIS3DModelInstance* instance = nullptr;
        if (!model->CreateModelInstance(0, &instance)) instance = nullptr;
        static_cast<cIGZUnknown*>(raw)->Release();
        return instance;
    }
    int TextureId(std::uint32_t instance) override { return static_cast<int>(NativeFshTexture(0x1abe787d, instance)); }
    cS3DVector3 LightColor(const cS3DVector3& normal, const cS3DVector3& position) override {
        if (cSC4LightingManager* lighting = LightingManager()) return lighting->GetColor(normal, position);
        return cS3DVector3{1.0f, 1.0f, 1.0f};
    }
    void InsertOccupant(cISC4Occupant* occupant) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && occupant) occupants->InsertOccupant(occupant, 0);
    }
    void RemoveOccupant(cISC4Occupant* occupant) override {
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && occupant) occupants->RemoveOccupant(occupant, true, 0);
    }
    const cISCPropertyHolder* TuningPropertyHolder() override { return PowerLineTuning(); }
    cIGZUnknown* CreateVisualEffect(const char* name) override { return native::CreateNamedEffect(name); }
    // the source transform carries the pole's position (translation flag set, change counter bumped), then Start(0)
    void StartVisualEffect(cIGZUnknown* effect, const float* xyz) override {
        auto* visual = static_cast<cSC4VisualEffect*>(effect);
        cS3DTransform source;
        source.translation[0] = xyz[0];
        source.translation[1] = xyz[1];
        source.translation[2] = xyz[2];
        source.flags = 1;
        source.changeCounter = 1;
        visual->SetSourceTransform(source);
        visual->Start(0);
    }
    bool VisualEffectStarted(cIGZUnknown* effect) override { return static_cast<cSC4VisualEffect*>(effect)->IsStarted(); }
    void StopVisualEffect(cIGZUnknown* effect, int transition) override { static_cast<cSC4VisualEffect*>(effect)->Stop(transition); }

private:
    static void Utilities(int& zoom, int& rotation) {
        cSC43DRender* render = CityRenderer();
        if (SC4ViewUtilities* utilities = render ? render->GetViewUtilities() : nullptr) utilities->GetZoomAndRotation(zoom, rotation);
    }
    ToolHost tool_;
};

ToolHost g_toolHost;
PoleHost g_poleHost;

}  // namespace

void InstallPowerLineToolHost() {
    SetPowerLineToolHost(&g_toolHost);
    SetPowerPoleOccupantHost(&g_poleHost);
}

// Kit verb `powerline X1 Z1 X2 Z2`: the network manager's power line tool lays a line between two cells through its own
// placement API (the anchors and PlaceAllSegments, what the drag's release calls), as a script would.
void NativeLayPowerLine(int x1, int z1, int x2, int z2) {
    cSC4NetworkTool* tool = NativeManagerNetworkTool(kNetworkPowerLine);
    auto* powerLine = dynamic_cast<cSC4PowerLineTool*>(tool);
    if (!powerLine) {
        std::printf("powerline: the network manager has no power line tool\n");
        return;
    }
    powerLine->Init();
    const std::vector<SC4PointU> anchors = {SC4PointU{static_cast<std::uint32_t>(x1), static_cast<std::uint32_t>(z1)},
                                            SC4PointU{static_cast<std::uint32_t>(x2), static_cast<std::uint32_t>(z2)}};
    powerLine->SetAnchorPoints(anchors);
    powerLine->PlaceAllSegments(true, true, false, kNetworkPowerLine);
    std::printf("powerline %d,%d -> %d,%d: %d poles, %d wire cells, cost %d, failure 0x%x\n", x1, z1, x2, z2,
                powerLine->poleCount_, powerLine->wireCellCount_, powerLine->GetCostOfSolution(),
                powerLine->GetFailureState() ? 1u : 0u);
}

// Kit verb `poles X0 Z0 X1 Z1`: every pole and wire occupant of the cell rectangle (cell, connections, which end draws the
// wire, model, wire occupants) and the power simulator's grid for the cells (wired / powered).
void NativePowerPoleReport(int x0, int z0, int x1, int z1) {
    cISC4OccupantManager* occupants = Occupants();
    if (!occupants) {
        std::printf("poles: no city\n");
        return;
    }
    const int a[2] = {x0, z0}, b[2] = {x1, z1};
    OccupantList list;
    int poles = 0, wires = 0, distressed = 0;
    if (occupants->GetOccupantsByStandardCityCells(list, a, b, static_cast<sc4::cISC4OccupantFilter*>(nullptr), 0xffffffffu)) {
        for (cISC4Occupant* occupant : list) {
            if (auto* pole = dynamic_cast<cSC4PowerPoleOccupant*>(occupant)) {
                int x = 0, z = 0;
                pole->GetCell(x, z);
                float p[3] = {};
                pole->GetPosition(p);
                int drawn = 0, curves = 0, lines = 0;
                for (const auto& connection : pole->Connections()) {
                    if (connection.drawsWire) ++drawn;
                    curves += static_cast<int>(connection.curvesA.size());
                    lines += static_cast<int>(connection.lines.size());
                }
                cGZPersistResourceKey key{0, 0, 0};
                if (cISCResExemplar* exemplar = pole->GetDefaultExemplar()) exemplar->GetKey(key);
                std::printf("pole %d,%d at %.1f,%.1f,%.1f: mask 0x%x exemplar %08x connections %d (draws %d, curves %d, wire occupants %d) model %s preview %d distressed %d zap %d points %s\n",
                            x, z, p[0], p[1], p[2], pole->Style(), key.instance, pole->GetNumberOfConnections(), drawn, curves, lines,
                            pole->HasModel() ? "yes" : "no", pole->IsPreview() ? 1 : 0, pole->GetDistressed() ? 1 : 0,
                            pole->HasDistressEffect() ? 1 : 0, pole->SavedPointsMatchTable() ? "as-table" : "differ");
                ++poles;
                if (pole->GetDistressed()) ++distressed;
            } else if (auto* line = dynamic_cast<cSC4PowerLineOccupant*>(occupant)) {
                std::uint32_t x = 0, z = 0;
                line->GetCell(x, z);
                std::printf("wire %u,%u preview %d\n", x, z, line->IsPreview() ? 1 : 0);
                ++wires;
            }
        }
    }
    occupants->ReleaseOccupantList(list);
    std::printf("poles: %d poles (%d distressed), %d wire occupants in %d,%d..%d,%d\n", poles, distressed, wires, x0, z0, x1, z1);
    std::vector<float> widths;
    int textures[4] = {};
    cSC4PowerPoleOccupant::DrawStatics(widths, textures);
    std::printf("poles: wire widths");
    for (float w : widths) std::printf(" %.2f", w);
    std::printf(", textures wire %d buoy %d top %d wall %d\n", textures[0], textures[1], textures[2], textures[3]);
    if ((x1 - x0) > 40 || (z1 - z0) > 40) return;   // the grid only for small rectangles
    if (cSC4PowerSimulator* power = NativePowerSimulator()) {
        for (int z = z0; z <= z1; ++z) {
            std::printf("power row %3d:", z);
            for (int x = x0; x <= x1; ++x)
                std::printf(" %c", power->IsPowered(x, z) ? 'P' : (power->IsWired(x, z) ? 'w' : '.'));
            std::printf("\n");
        }
    } else {
        std::printf("poles: no power simulator\n");
    }
}

}  // namespace native
