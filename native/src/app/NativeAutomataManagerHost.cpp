// The automata managers' city services (sc4::IVehicleManagerHost, which is also the base IAutomataManagerHost and
// ITrafficAutomataManagerHost every manager reaches through AutomataManagerHost()). Replaces the generated stub
// (app/hosts/StubAutomataManager.cpp) for the native game: the framework (messages, the animation ticks), the city (script
// groups, the 24 hour clock, cells, terrain), resources (tuning, exemplars), creation of automata by class id and the
// managers' views of loaded automata.
//   PARTIAL: cheats, the debug view, player drive, the camera and view utilities, path views, the dispatch view, the group
//   budget, the network manager's cell masks, the traffic simulator's edge counts and the response occupants answer
//   neutrally; the population pass therefore spawns nothing yet.
#include <list>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include <algorithm>
#include <cmath>
#include <cstring>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/AutomataManager.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/Exemplar.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/VehicleManager.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
int NativeResidentialPopulation();              // app/NativeSimHosts.cpp
float NativeAttractionCommotion(const float* xyz);   // app/NativeAutomataScripts.cpp
}

namespace native { int NativeViewZoom(); }
namespace native {
sc4::ITrafficSimulatorApi* NativeTrafficSimulatorApi();          // app/NativeTrafficAutomata.cpp
sc4::ITrafficNetworkManagerApi* NativeTrafficNetworkApi();
void NativeSetTrafficVisibleCells(int x0, int z0, int x1, int z1);
namespace {
using namespace sc4;

template <class T> T* Plain(CityComponent c) {   // a component held as its plain view (kGZIID_PlainObject)
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<T*>(raw);
}
template <class T> T* Concrete(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    return object ? dynamic_cast<T*>(object) : nullptr;
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

// The automata script system seen by the managers.
class ScriptGroups final : public IAutomataScriptGroups {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }   // the city keeps the script system
    std::uint32_t Release() override { return 1; }
    IAutomataGroupTemplate* GetAutomataGroup(std::uint32_t id) override {
        cSC4AutomataScriptSystem* s = System();
        return s ? GroupTemplateView(s->GetAutomataGroup(id)) : nullptr;
    }
    IAutomataGroupTemplate* GetOccupantGroup(std::uint32_t id) override {
        cSC4AutomataScriptSystem* s = System();
        return s ? GroupTemplateView(s->GetOccupantGroup(id)) : nullptr;
    }
    const std::list<std::uint32_t>& GetTrackBuildingGroups() override {
        cSC4AutomataScriptSystem* s = System();
        return s ? s->GetTrackBuildingGroups() : empty_;
    }
private:
    static cSC4AutomataScriptSystem* System() {
        if (auto* s = Concrete<cSC4AutomataScriptSystem>(CityComponent::AutomataScriptSystem)) return s;
        return Plain<cSC4AutomataScriptSystem>(CityComponent::AutomataScriptSystem);
    }
    std::list<std::uint32_t> empty_;
} g_scriptGroups;

// The 24 hour clock.
class Clock final : public IAutomataClock {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    int Hour() override { cSC424HourClock* c = Get(); return c ? int(c->GetHour()) : 12; }
    int Minute() override { cSC424HourClock* c = Get(); return c ? int(c->GetMinute()) : 0; }
    float TimeMagnification() {
        float value = 1.0f;
        if (cSC424HourClock* c = Get()) c->GetTimeMagnification(value);
        return value;
    }
private:
    static cSC424HourClock* Get() {
        if (auto* c = Concrete<cSC424HourClock>(CityComponent::Clock24Hour)) return c;
        return Plain<cSC424HourClock>(CityComponent::Clock24Hour);
    }
} g_clock;

// The city renderer's view utilities (SL::spViewUtilities) as the managers read them.
class ViewUtilitiesView final : public IAutomataViewUtilities {
public:
    int ViewLevel() override { return CityViewLevel(); }
    void GetZoomAndRotation(int& zoom, int& rotation) override { if (SC4ViewUtilities* v = View()) v->GetZoomAndRotation(zoom, rotation); else zoom = rotation = 0; }
    void ViewPosition(float out[3]) override {
        SC4ViewUtilities* v = View();
        for (int i = 0; i < 3; ++i) out[i] = v ? (&v->EyePosition().x)[i] : 0.0f;
    }
    // +0x3f4: the visible terrain area, x / z. cSC43DRender::UpdateVisibleTerrainMetrics (i386 0x00504d28) keeps it in the view
    // utilities: the view's centre on the terrain is the mean of the terrain picks at the viewport's centre and 100 pixels to
    // each side of it (the picks that found terrain above 0), its reach is the larger of the x and z distances from there to
    // the terrain under the viewport's bottom right corner, and the area is the square of that reach around the centre. The
    // automata managers keep an automaton while it stands in this area (+48), so it has to cover the whole window: the
    // midpoint rectangle used here before covered a strip down the middle, and on a wide window every automaton made
    // toward the sides left at once.
    //   PARTIAL: when no pick finds terrain (the view is off the map) the original walks the city's boundary; here the
    //   ground plane's block rectangle answers.
    //   The picks are the same until the camera moves, while the traffic population asks once per road tile (hundreds of times
    //   a tick): the answer is kept for the camera's screen offset, zoom, rotation and the viewport size.
    void ViewRect(float out[4]) override {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* v = View();
        if (!render || !v) return;
        std::uint32_t width = 0, height = 0;
        render->GetViewportSize(width, height);
        struct Key {
            cS3DTransform view;
            int zoom = -1;
            std::uint32_t width = 0, height = 0;
            const void* render = nullptr;
        } key;
        s3d::cS3DCamera* camera = render->GetCamera();
        if (camera) key.view = camera->ViewXform();
        key.zoom = NativeViewZoom();
        key.width = width;
        key.height = height;
        key.render = render;
        static Key cachedKey;
        static bool cachedValid = false;
        static float cached[4] = {};
        if (camera && cachedValid && std::memcmp(&key.view, &cachedKey.view, sizeof key.view) == 0 && key.zoom == cachedKey.zoom &&
            key.width == cachedKey.width && key.height == cachedKey.height && key.render == cachedKey.render) {
            for (int i = 0; i < 4; ++i) out[i] = cached[i];
            return;
        }
        ComputeViewRect(render, v, width, height, out);
        cachedKey = key;
        cachedValid = true;
        for (int i = 0; i < 4; ++i) cached[i] = out[i];
    }
    static void ComputeViewRect(cSC43DRender* render, SC4ViewUtilities* v, std::uint32_t width, std::uint32_t height, float out[4]) {
        const int centreX = static_cast<int>(width) / 2, centreY = static_cast<int>(height) / 2;
        static constexpr int kOffsets[5][2] = {{0, 0}, {100, 0}, {0, 100}, {-100, 0}, {0, -100}};
        float sum[3] = {0.0f, 0.0f, 0.0f};
        int hits = 0;
        for (const auto& offset : kOffsets) {
            float point[3] = {0.0f, 0.0f, 0.0f};
            if (!render->PickTerrain(centreX + offset[0], centreY + offset[1], point, true, false) || !(point[1] > 0.0f)) continue;
            for (int i = 0; i < 3; ++i) sum[i] += point[i];
            ++hits;
        }
        float corner[3] = {0.0f, 0.0f, 0.0f};
        if (hits == 0 || !render->PickTerrain(static_cast<int>(width), static_cast<int>(height), corner, true, false)) {
            float midMin[3] = {}, midMax[3] = {}, blockMin[3] = {}, blockMax[3] = {};
            v->GetGroundPlaneVisibleAreaRectangle(midMin, midMax, blockMin, blockMax);
            out[0] = blockMin[0];
            out[1] = blockMin[2];
            out[2] = blockMax[0];
            out[3] = blockMax[2];
            return;
        }
        const float x = sum[0] / static_cast<float>(hits), z = sum[2] / static_cast<float>(hits);
        const float reach = std::max(std::fabs(corner[2] - z), std::fabs(corner[0] - x));
        out[0] = x - reach;
        out[1] = z - reach;
        out[2] = x + reach;
        out[3] = z + reach;
    }
    bool IsPointInViewFrustum(const float point[3]) override { SC4ViewUtilities* v = View(); return v && v->IsPointInViewFrustum(point); }
    void ProjectPointToViewCoordinates(float point[3]) override { if (SC4ViewUtilities* v = View()) v->ProjectPointToViewCoordinates(point); }
    void ProjectPointFromViewCoordinatesToViewportCoordinates(float point[3]) override {
        if (SC4ViewUtilities* v = View()) v->ProjectPointFromViewCoordinatesToViewportCoordinates(point);
    }
    void UnProjectPointFromViewportCoordinatesToWorldCoordinates(float point[3]) override {
        if (SC4ViewUtilities* v = View()) v->UnProjectPointFromViewportCoordinatesToWorldCoordinates(point);
    }
private:
    static SC4ViewUtilities* View() { cSC43DRender* r = CityRenderer(); return r ? r->GetViewUtilities() : nullptr; }
} g_viewUtilities;

// SL::spSimulator and the variable-speed preference as CalculateSimSpeedModifier (i386 0x000bd8a8) reads them; the
// automata move 1x / 2x / 4x at turtle / llama / cheetah (tuning 0x6b42922c) and freeze while the simulator is paused.
class SimulatorView final : public IAutomataSimulator {
public:
    bool IsEmergencyPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsEmergencyPaused(); }
    bool IsPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsPaused(); }
    int SimSpeed() override { cSC4Simulator* s = CitySimulatorObject(); return s ? s->GetSimSpeed() : 0; }
    bool VariableSpeedAutomata() override {
        const std::uint8_t* preferences = SL::spPreferences;
        return preferences && preferences[0xf18] != 0;   // Graphics options "variable speed automata", on by default
    }
};

class Host final : public IVehicleManagerHost {
public:
    // ---- framework
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, std::uint32_t(channel));
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, std::uint32_t(channel));
    }
    bool CheatManagerAvailable() override { return false; }
    bool DebugFunctionalityEnabled() override { return false; }
    void RegisterCheat(std::uint32_t, const char*) override {}
    void UnregisterCheat(std::uint32_t) override {}
    void AddCheatListener(cIGZMessageTarget*) override {}
    void RemoveCheatListener(cIGZMessageTarget*) override {}
    void PostMessage(cIGZMessage2* message, bool front) override { if (SL::spMS2 && message) SL::spMS2->MessagePost(message, front); }
    // ---- city
    IAutomataNetworkManager* NetworkManager() override { return nullptr; }
    IAutomataScriptGroups* ScriptGroups() override { return &g_scriptGroups; }
    IAutomataClock* CityClock() override { return &g_clock; }
    IAutomataKindManager* KindManager(int) override { return nullptr; }
    IAutomataGroupBudget* GroupBudget() override { return nullptr; }
    bool CellToWorld(int cellX, int cellZ, float& x, float& z) override { x = float(cellX) * 16.0f; z = float(cellZ) * 16.0f; return true; }
    bool WorldToCell(float x, float z, int& cellX, int& cellZ) override { cellX = int(x / 16.0f); cellZ = int(z / 16.0f); return true; }
    float CellSize() override { return 16.0f; }
    int CityCellCount() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    bool OccupantCells(IAutomataOccupant* occupant, tCellRect& rect) override {
        if (!occupant) return false;
        occupant->GetBoundingCityCells(rect);
        return true;
    }
    bool LotCellMasks(int, int, std::uint32_t masks[3]) override { masks[0] = masks[1] = masks[2] = 0; return false; }
    // ---- occupants
    cIGZUnknown* CreateOccupantGroupFilter(const std::vector<std::uint32_t>&) override { return nullptr; }
    bool EnumerateOccupants(cIGZUnknown*, std::list<IAutomataOccupant*>&) override { return false; }
    void ReleaseOccupantList(std::list<IAutomataOccupant*>& list) override { for (IAutomataOccupant* o : list) if (o) o->Release(); list.clear(); }
    // ---- views / rendering
    IAutomataViewUtilities* ViewUtilities() override { return CityRenderer() ? &g_viewUtilities : nullptr; }
    IAutomataPlayerDriveControl* ActiveInputControl() override { return nullptr; }
    IAutomataPlayerDriveControl* PlayerDriveControl() override { return nullptr; }
    void SetPlayerDriveControl(IAutomataPlayerDriveControl*) override {}
    IAutomataPlayerDriveControl* CreatePlayerDriveControl() override { return nullptr; }
    void AddViewInputControl(IAutomataPlayerDriveControl*, int) override {}
    void SetVehicleManagerSpeed(float) override {}
    void SetGhostModels(bool) override {}
    bool CurrentInputControlIsPlayerDrive() override { return false; }
    IAutomataCameraControl* CameraControl() override { return nullptr; }
    bool CameraFollowFast() override { return false; }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool TerrainShadowEnabled() override { return false; }
    float TerrainShadowHeight(float x, float z) override { return TerrainHeight(x, z); }
    IAutomataPathView* CreatePathView() override { return nullptr; }
    void AddRendererLayer(IAutomataPathView*) override {}
    void RemoveRendererLayer(IAutomataPathView*) override {}
    IAutomataDispatchView* DispatchView() override { return nullptr; }
    void ReleaseObject(cIGZUnknown* object) override { if (object) object->Release(); }
    // ---- simulator / misc
    IAutomataSimulator* Simulator() override {
        static SimulatorView view;
        return CitySimulatorObject() ? &view : nullptr;
    }
    bool VehicleManagerPaused() override { return false; }   // IsDrivingMode: the native game has no driving mode yet
    bool LandfillDeveloperPresent() override { return false; }
    int LandfillSourceBuildingCount() override { return 0; }
    bool GetTuningProperties(cISCPropertyHolder** out) override {
        *out = nullptr;
        cISCResExemplar* exemplar = nullptr;
        if (!GetExemplar(cGZPersistResourceKey{0x6534284a, 0xa998d30b, 1}, &exemplar) || !exemplar) return false;
        // one reference, which the caller releases (ReadTuningParametersImpl): the exemplar's own
        *out = exemplar->AsISCPropertyHolder();
        if (!*out) exemplar->Release();
        return *out != nullptr;
    }
    bool GetExemplar(const cGZPersistResourceKey& key, cISCResExemplar** out) override {
        *out = nullptr;
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return false;
        *out = static_cast<cISCResExemplar*>(raw);
        return true;
    }
    bool EnumerateResourceKeys(std::uint32_t type, std::vector<cGZPersistResourceKey>& out) override {
        if (!SL::spRM) return false;
        struct Filter final : cIGZPersistResourceKeyFilter {
            explicit Filter(std::uint32_t t) : type(t) {}
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return 1; }
            std::uint32_t Release() override { return 1; }
            bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type; }
            std::uint32_t type;
        } filter(type);
        cIGZPersistResourceKeyList* list = nullptr;
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return false;
        for (std::uint32_t i = 0; i < list->Size(); ++i) out.push_back(*list->GetKey(i));
        list->Release();
        return true;
    }
    IAutomataAutomaton* CreateAutomatonByClassId(std::uint32_t classId) override {
        cIGZSerializable* object = NativeCreateSerializable(classId);
        IAutomataAutomaton* view = object ? native::AutomatonView(static_cast<cIGZUnknown*>(object)) : nullptr;
        if (!view && object) object->Release();
        return view;   // the factory's reference
    }
    void DebugAutomatonRemoved(IAutomataAutomaton*) override {}
    void DebugViewSetEnabled(bool) override {}
    void DebugViewShutdown() override {}
    IAutomataAutomaton* LoadedAutomaton(void* object) override {
        // SC4ReadGZSerializable's cISC4Automaton answer: cSC4Automaton::QueryInterface hands out `this` for it
        return object ? native::AutomatonView(static_cast<cSC4Automaton*>(object)) : nullptr;
    }
    IAutomataOccupant* OccupantView(void* cityOccupant) override { return CityOccupantView(static_cast<cISC4Occupant*>(cityOccupant)); }
    IAutomataBuildingOccupant* BuildingView(void* building) override { return CityBuildingView(static_cast<cIGZUnknown*>(building)); }
    IAutomataAutomaton* AutomatonView(void* automaton) override {   // a QI 0xa9b40f05 answer: the automaton's `this`
        return automaton ? native::AutomatonView(static_cast<cSC4Automaton*>(automaton)) : nullptr;
    }
    // ---- ITrafficAutomataManagerHost
    ITrafficSimulatorApi* TrafficSimulator() override { return NativeTrafficSimulatorApi(); }
    ITrafficNetworkManagerApi* TrafficNetworkManager() override {
        // the population pass walks the view's cells: the view rectangle in cells, clamped to the city
        float rect[4] = {};
        g_viewUtilities.ViewRect(rect);
        const int cells = CityCellCount();
        const auto clamp = [cells](float v) { int c = int(v / 16.0f); return c < 0 ? 0 : (c >= cells ? cells - 1 : c); };
        NativeSetTrafficVisibleCells(clamp(rect[0]), clamp(rect[1]), clamp(rect[2]), clamp(rect[3]));
        return NativeTrafficNetworkApi();
    }
    int ResidentialPopulation() override { return NativeResidentialPopulation(); }
    float ClockTimeMagnification() override { return g_clock.TimeMagnification(); }
    float AttractionCommotion(ITrafficNetworkMapOccupant* occupant) override {
        cISC4Occupant* piece = occupant ? occupant->AsOccupant() : nullptr;
        if (!piece) return 0.0f;
        float p[3] = {0.0f, 0.0f, 0.0f};
        piece->GetPosition(p);
        return NativeAttractionCommotion(p);
    }
    ITrafficAutomataAutomaton* AsTrafficAutomaton(IAutomataAutomaton* automaton) override { return TrafficAutomatonView(automaton); }
    cISC4Occupant* AsCityOccupant(IAutomataOccupant* view) override { return CityOccupantOf(view); }   // a dispatch's source station
    IAutomataOccupant* AsAutomataOccupant(cISC4Occupant*) override { return nullptr; }
    ITrafficNetworkOccupant* AsTrafficNetworkOccupant(IAutomataNetworkOccupant*) override { return nullptr; }
    // ---- IVehicleManagerHost
    bool HasRenderProperties() const override { return false; }
    float AutomataDensityFactor() override { return 1.0f; }
    int AutomataMinZoom() override { return 1; }
    int TimeOfDay() override { const int h = g_clock.Hour(); return (h >= 6 && h < 20) ? 0 : 1; }
    void ShowVehicleHeadlights(IAutomataAutomaton*, bool, bool) override {}
    IAutomataAutomaton* CreateVehicleAutomaton() override { return CreateAutomatonByClassId(kVehicleFallbackGroup); }
    void NotifyKindManagersTuningChanged() override {}
    void HandleFundsChangedSweep() override {}
};
Host g_host;

}  // namespace

void InstallAutomataManagerHost() { sc4::SetAutomataManagerHost(&g_host); }

// The view utilities for hosts that cannot include the renderer's header next to their own.
// IAutomatonHost::FindAutomataGroup: the script system's automata group (its slot 0x38), as the automaton sees it.
sc4::IAutomatonGroupTemplate* NativeFindAutomatonGroup(std::uint32_t id) {
    return AutomatonGroupTemplateView(g_scriptGroups.GetAutomataGroup(id));
}

// cSC4AutomataManager::GetAutomataManagerForGroup (0x100104c7e): the first of the city's automata managers (vehicles,
// pedestrians, aircraft; city vslots 0x158 / 0x160 / 0x168) whose vslot 0xf0 takes the group.
//   PARTIAL: the watercraft manager (0x170) is a class of its own here and is not asked.
sc4::cSC4AutomataManager* NativeAutomataManagerForGroup(std::uint32_t group) {
    for (CityComponent component : {CityComponent::VehicleManager, CityComponent::PedestrianManager, CityComponent::AircraftManager}) {
        cSC4AutomataManager* manager = Concrete<cSC4AutomataManager>(component);
        if (manager && manager->CanCreateGroup(group)) return manager;
    }
    return nullptr;
}
// SL::spVehicleManager / spPedestrianManager / spAircraftManager / spWatercraftManager by index (the automata scripts' source
// managers). PARTIAL: the watercraft manager (3) is a class of its own natively and answers null.
sc4::cSC4AutomataManager* NativeAutomataManagerAt(int index) {
    static constexpr CityComponent kManagers[3] = {CityComponent::VehicleManager, CityComponent::PedestrianManager, CityComponent::AircraftManager};
    return index >= 0 && index < 3 ? Concrete<cSC4AutomataManager>(kManagers[index]) : nullptr;
}
// cSC4TrafficAutomataManager::IsLayerVisible (i386 0x000ca21a) on the vehicle manager: the view level shows that layer.
bool NativeAutomataLayerVisible(std::uint32_t layer) {
    cSC4TrafficAutomataManager* manager = Concrete<cSC4TrafficAutomataManager>(CityComponent::VehicleManager);
    return manager && manager->IsLayerVisible(layer);
}

// Whether a viewport position (NativeProjectToViewport's) lies inside the view's rectangle.
bool NativeViewportHolds(const float* viewport) {
    const std::int32_t* rect = CityRenderer() && CityRenderer()->GetViewUtilities() ? CityRenderer()->GetViewUtilities()->ViewportRect() : nullptr;
    return rect && viewport[0] >= float(rect[0]) && viewport[0] < float(rect[2]) && viewport[1] >= float(rect[1]) && viewport[1] < float(rect[3]);
}
bool NativeViewAvailable() { return CityRenderer() && CityRenderer()->GetViewUtilities(); }
// The view utilities' visible terrain area (+0x3ec..+0x3f8, world units x0, z0, x1, z1) and zoom (+0xc); zeros without a view.
void NativeVisibleTerrainRect(float out[4]) { g_viewUtilities.ViewRect(out); }
int NativeViewZoom() { int zoom = 0, rotation = 0; g_viewUtilities.GetZoomAndRotation(zoom, rotation); return zoom; }
// Where a world point lands in the view (viewport pixels); false without a view.
bool NativeProjectToViewport(const float* xyz, float out[3]) {
    SC4ViewUtilities* view = CityRenderer() ? CityRenderer()->GetViewUtilities() : nullptr;
    if (!view) return false;
    out[0] = xyz[0]; out[1] = xyz[1]; out[2] = xyz[2];
    view->ProjectPointToViewCoordinates(out);
    view->ProjectPointFromViewCoordinatesToViewportCoordinates(out);
    return true;
}
bool NativePointInViewFrustum(const float* xyz) {
    SC4ViewUtilities* view = CityRenderer() ? CityRenderer()->GetViewUtilities() : nullptr;
    return view && view->IsPointInViewFrustum(xyz);
}

}  // namespace native
