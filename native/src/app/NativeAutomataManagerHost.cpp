// The automata managers' city services (sc4::IVehicleManagerHost, which is also the base IAutomataManagerHost and
// ITrafficAutomataManagerHost every manager reaches through AutomataManagerHost()). Replaces the generated stub
// (app/hosts/StubAutomataManager.cpp) for the native game: the framework (messages, the animation ticks), the city (script
// groups, the 24 hour clock, cells, terrain), resources (tuning, exemplars), creation of automata by class id and the
// managers' views of loaded automata.
//   PARTIAL: the debug view, path views, the group budget, the network manager and its cell masks, the
//   traffic simulator's edge counts and the response occupants answer neutrally. The cheats reach the game's cheat manager.
#include <cstdio>
#include "platform/Unwired.h"
#include <cstdlib>
#include <list>
#include <vector>

#include "app/NativeCheats.h"
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
#include "sc4/AutomataControllerManager.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/Exemplar.h"
#include "sc4/OccupantGroupTemplate.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficAutomataManager.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/VehicleManager.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/Lot.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/Automaton.h"
#include "sc4/OccupantManager.h"

namespace native {
int NativeResidentialPopulation();              // app/NativeSimHosts.cpp
float NativeAttractionCommotion(const float* xyz);   // app/NativeAutomataScripts.cpp
}

namespace native { int NativeViewZoom(); }
namespace native {   // NativeAutomataControllerHost.cpp
bool NativeAddControllersFromOccupantGroup(const char* group, sc4::cISC4Occupant* occupant, const float* position);
void NativeRemoveOccupantControllers(sc4::cISC4Occupant* occupant);
}
namespace native {
sc4::IAutomataPlayerDriveControl* NativeCreatePlayerDriveControl();   // app/NativePlayerDrive.cpp
void NativeSetPlayerDriveControl(sc4::IAutomataPlayerDriveControl* control);
sc4::IAutomataPlayerDriveControl* NativePlayerDriveControl();
void NativeAddPlayerDriveControl(sc4::IAutomataPlayerDriveControl* control, int operation);
bool NativeCurrentInputControlIsPlayerDrive();
bool NativeDispatchViewPresent();   // app/NativeDispatchVehicleView.cpp
void NativeDispatchViewShowAllIndicators(int type, bool show);
sc4::IAutomataCameraControl* NativeAutomataCameraControl();
sc4::ITrafficNetworkMapLot* NativeTrafficLotAt(int cellX, int cellZ);   // app/NativeTrafficNetworkMap.cpp
}
namespace native {
sc4::ITrafficSimulatorApi* NativeTrafficSimulatorApi();          // app/NativeTrafficAutomata.cpp
sc4::ITrafficNetworkManagerApi* NativeTrafficNetworkApi();
void NativeSetTrafficVisibleCells(int x0, int z0, int x1, int z1);
bool NativeSituationUsesAutomataGroup(std::uint32_t group);   // app/NativeCitySituations.cpp
std::uint32_t NativeSituationTrackedAutomataCount();
std::uint8_t NativeSituationMaxCSI();
void NativeSituationResetTutorials();
bool NativeHasCitySituations();
namespace {
// The city's traffic network map (SL::spCity GetTrafficNetwork), or null.
sc4::cSC4TrafficNetworkMap* TrafficMap() {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::TrafficNetwork) : nullptr;
    return object ? dynamic_cast<sc4::cSC4TrafficNetworkMap*>(object) : nullptr;
}
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
    // +0x3f4: the visible terrain area (min x, min z, max x, max z) that cSC43DRender::UpdateVisibleTerrainMetrics (i386
    // 0x00504d28) keeps in the view utilities on every camera change. The automata managers keep an automaton while it stands
    // in this area (+48).
    void ViewRect(float out[4]) override {
        SC4ViewUtilities* v = View();
        for (int i = 0; i < 4; ++i) out[i] = v ? v->VisibleTerrainRect()[i] : 0.0f;
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

// cSC4DispatchVehicleView as the automata managers reach it.
class DispatchViewForward final : public IAutomataDispatchView {
public:
    void ShowAllIndicators(int type, bool show) override { NativeDispatchViewShowAllIndicators(type, show); }
};

class Host final : public IVehicleManagerHost {
public:
    // ---- framework
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    // The view filter at the front of the information (cSC4ViewFilterInformation) asked about the automaton itself.
    bool ViewFilterIncludes(const void* filterInformation, IAutomataAutomaton* automaton) override {
        if (!filterInformation) return true;
        cIGZUnknown* stored = *static_cast<cIGZUnknown* const*>(filterInformation);
        sc4::cSC4Automaton* occupant = AutomatonOf(automaton);
        if (!stored || !occupant) return true;
        return static_cast<sc4::cISC4OccupantFilter*>(stored)->IsOccupantIncluded(occupant);
    }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, std::uint32_t(channel));
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, std::uint32_t(channel));
    }
    bool CheatManagerAvailable() override { return native::GameCheatCodeManager() != nullptr; }
    bool DebugFunctionalityEnabled() override { return native::GameDebugMode(); }
    void RegisterCheat(std::uint32_t id, const char* name) override { native::RegisterGameCheat(id, name); }
    void UnregisterCheat(std::uint32_t id) override { native::UnregisterGameCheat(id); }
    void AddCheatListener(cIGZMessageTarget* target) override { native::AddGameCheatListener(target, 0); }
    void RemoveCheatListener(cIGZMessageTarget* target) override { native::RemoveGameCheatListener(target, 0); }
    void PostMessage(cIGZMessage2* message, bool front) override { if (SL::spMS2 && message) SL::spMS2->MessagePost(message, front); }
    // ---- city
    // SL::spCity vslot 0x140 (i386 0xa0 GetTrafficNetwork): the traffic network map, as the automata see it.
    IAutomataNetworkManager* NetworkManager() override {
        struct MapView final : IAutomataNetworkManager {
            bool QueryInterface(std::uint32_t, void**) override { return false; }
            std::uint32_t AddRef() override { return 1; }
            std::uint32_t Release() override { return 1; }
            // vslot 0x70 (i386 0x38) SafeGetTileInfoAtCell: the tile's road / occupancy / building masks
            bool CellMasks(int cellX, int cellZ, std::uint32_t masks[3]) override {
                sc4::cSC4TrafficNetworkMap* map = TrafficMap();
                const sc4::TrafficTileInfo* tile = map ? map->SafeGetTileInfoAtCell(cellX, cellZ) : nullptr;
                if (!tile) return false;
                masks[0] = tile->roadMask;
                masks[1] = tile->occupancyMask;
                masks[2] = tile->buildingMask;
                return true;
            }
            float Height(float x, float z) override {   // vslot 0x90 (i386 0x48) GetRoadAltitude
                sc4::cSC4TrafficNetworkMap* map = TrafficMap();
                return map ? map->GetRoadAltitude(x, z) : 0.0f;
            }
        };
        static MapView view;
        return TrafficMap() ? &view : nullptr;
    }
    IAutomataScriptGroups* ScriptGroups() override { return &g_scriptGroups; }
    IAutomataClock* CityClock() override { return &g_clock; }
    IAutomataKindManager* KindManager(int) override { return nullptr; }
    // SL::spCity vslot 0x188 (i386 0xc4 GetCitySituationManager): situations reserve automata of their groups up to the max CSI.
    IAutomataGroupBudget* GroupBudget() override {
        struct Budget final : IAutomataGroupBudget {
            bool HandlesGroup(std::uint32_t group) override { return NativeSituationUsesAutomataGroup(group); }   // i386 vslot 0x50
            std::uint32_t CurrentCount() override { return NativeSituationTrackedAutomataCount(); }               // i386 vslot 0x70
            std::uint8_t MaxCount() override { return NativeSituationMaxCSI(); }                                   // i386 vslot 0x6c
            void ClearTrackedAutomata() override { NativeSituationResetTutorials(); }                              // i386 vslot 0x84
        };
        static Budget budget;
        return NativeHasCitySituations() ? &budget : nullptr;
    }
    bool CellToWorld(int cellX, int cellZ, float& x, float& z) override { x = float(cellX) * 16.0f; z = float(cellZ) * 16.0f; return true; }
    bool WorldToCell(float x, float z, int& cellX, int& cellZ) override { cellX = int(x / 16.0f); cellZ = int(z / 16.0f); return true; }
    float CellSize() override { return 16.0f; }
    int CityCellCount() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    // i386 0x000becfc: the building's cells (occupant vslot 0x2c), then the lot under them answers with its own rect (lot manager
    // GetLot at the rect's x0 and z1, as the original asks; lot GetBoundingRect), so a route to a building ends beside its lot.
    bool OccupantCells(IAutomataOccupant* occupant, tCellRect& rect) override {
        if (!occupant) return false;
        occupant->GetBoundingCityCells(rect);
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::ZoneDeveloper) : nullptr;
        auto* lots = object ? dynamic_cast<sc4::cSC4ZoneDeveloper*>(object) : nullptr;
        if (sc4::cSC4Lot* lot = lots ? lots->GetLot(rect.x0, rect.z1, false) : nullptr) {
            std::int32_t r[4] = {0, 0, 0, 0};
            lot->GetBoundingRect(r);
            rect.x0 = r[0]; rect.z0 = r[1]; rect.x1 = r[2]; rect.z1 = r[3];
        }
        return true;
    }
    // ChooseRandomExemplar (i386 0x000bfeba: the manager's traffic network map +0xa8c vslot 0x38 SafeGetTileInfoAtCell): the tile's
    // masks (i386 +0x24/+0x28/+0x2c); a cell without a tile answers all ones, so the filter still runs.
    bool LotCellMasks(int cellX, int cellZ, std::uint32_t masks[3]) override {
        sc4::cSC4TrafficNetworkMap* map = TrafficMap();
        const sc4::TrafficTileInfo* tile = map ? map->SafeGetTileInfoAtCell(cellX, cellZ) : nullptr;
        masks[0] = tile ? tile->roadMask : 0xffffffffu;
        masks[1] = tile ? tile->occupancyMask : 0xffffffffu;
        masks[2] = tile ? tile->buildingMask : 0xffffffffu;
        return true;
    }
    // ---- occupants
    cIGZUnknown* CreateOccupantGroupFilter(const std::vector<std::uint32_t>&) override { return nullptr; }
    bool EnumerateOccupants(cIGZUnknown*, std::list<IAutomataOccupant*>&) override { return false; }
    void ReleaseOccupantList(std::list<IAutomataOccupant*>& list) override { for (IAutomataOccupant* o : list) if (o) o->Release(); list.clear(); }
    // ---- views / rendering
    IAutomataViewUtilities* ViewUtilities() override { return CityRenderer() ? &g_viewUtilities : nullptr; }
    IAutomataPlayerDriveControl* ActiveInputControl() override { return nullptr; }
    IAutomataPlayerDriveControl* PlayerDriveControl() override { return NativePlayerDriveControl(); }
    void SetPlayerDriveControl(IAutomataPlayerDriveControl* control) override { NativeSetPlayerDriveControl(control); }
    IAutomataPlayerDriveControl* CreatePlayerDriveControl() override { return NativeCreatePlayerDriveControl(); }
    void AddViewInputControl(IAutomataPlayerDriveControl* control, int operation) override { NativeAddPlayerDriveControl(control, operation); }
    // SL::spVehicleManager vslot 0xb8 SetMaxCount: the count limit while the player drives, and the tuning's one after.
    void SetVehicleManagerSpeed(float count) override {
        if (auto* m = Concrete<cSC4AutomataManager>(CityComponent::VehicleManager)) m->SetMaxCount(static_cast<int>(count));
    }
    void SetGhostModels(bool enabled) override { sc4::cSC4Automaton::sGhostModelsEnabled = enabled; }   // cSC4Automaton::SetGhostModelsEnabled
    bool CurrentInputControlIsPlayerDrive() override { return NativeCurrentInputControlIsPlayerDrive(); }
    IAutomataCameraControl* CameraControl() override { return NativeAutomataCameraControl(); }
    bool CameraFollowFast() override { return false; }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    bool TerrainShadowEnabled() override { return false; }
    float TerrainShadowHeight(float x, float z) override { return TerrainHeight(x, z); }
    IAutomataPathView* CreatePathView() override { return nullptr; }
    void AddRendererLayer(IAutomataPathView*) override {}
    void RemoveRendererLayer(IAutomataPathView*) override {}
    // SL::spDispatchManager vslot 0xb0 GetDispatchVehicleView (the cheat hides the situation indicators through it).
    IAutomataDispatchView* DispatchView() override {
        static DispatchViewForward view;
        return NativeDispatchViewPresent() ? &view : nullptr;
    }
    void ReleaseObject(cIGZUnknown* object) override { if (object) object->Release(); }
    // ---- simulator / misc
    IAutomataSimulator* Simulator() override {
        static SimulatorView view;
        return CitySimulatorObject() ? &view : nullptr;
    }
    bool VehicleManagerPaused() override { return NativeCurrentInputControlIsPlayerDrive(); }   // the vehicle manager's IsDrivingMode
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
    bool AddControllersFromOccupantGroup(const char* group, cISC4Occupant* occupant, const float* position) override {
        return native::NativeAddControllersFromOccupantGroup(group, occupant, position);
    }
    void RemoveOccupantControllers(cISC4Occupant* occupant) override { native::NativeRemoveOccupantControllers(occupant); }
    IAutomataOccupant* AsAutomataOccupant(cISC4Occupant* occupant) override { return CityOccupantView(occupant); }   // a response's road and building
    ITrafficNetworkOccupant* AsTrafficNetworkOccupant(IAutomataNetworkOccupant*) override { return nullptr; }
    // GetRandomCreationPoint's network path: the lot at a cell and the traffic network map's surrounding roads.
    bool LotCellRectAt(int cellX, int cellZ, SC4RectInt32& rect) override {
        ITrafficNetworkMapLot* lot = NativeTrafficLotAt(cellX, cellZ);
        if (!lot) return false;
        std::int32_t r[4] = {0, 0, 0, 0};
        lot->GetBoundingRect(r);
        rect.x0 = r[0]; rect.z0 = r[1]; rect.x1 = r[2]; rect.z1 = r[3];
        return true;
    }
    bool SurroundingRoads(std::vector<ITrafficNetworkMapOccupant*>& out, const SC4RectInt32& rect, int maxCells, int minCells,
                          std::uint32_t networkFlags) override {
        auto* map = Concrete<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
        const bool found = map && map->GetSurroundingRoads(out, rect, maxCells, minCells, networkFlags);
        if (std::getenv("SC4_UDIDEBUG"))
            std::fprintf(stderr, "udi: roads around (%d, %d)-(%d, %d) within %d cells, flags 0x%x: %zu (map %s)\n", rect.x0, rect.z0, rect.x1, rect.z1,
                         maxCells, networkFlags, out.size(), map ? "yes" : "no");
        return found;
    }
    // ---- IVehicleManagerHost
    bool HasRenderProperties() const override { return false; }
    float AutomataDensityFactor() override { return 1.0f; }
    int AutomataMinZoom() override { return 1; }
    int TimeOfDay() override { const int h = g_clock.Hour(); return (h >= 6 && h < 20) ? 0 : 1; }
    void ShowVehicleHeadlights(IAutomataAutomaton*, bool, bool) override {}
    IAutomataAutomaton* CreateVehicleAutomaton() override { return CreateAutomatonByClassId(kVehicleFallbackGroup); }
    void NotifyKindManagersTuningChanged() override {}
    // cSC4VehicleManager::HandleFundsChanged (i386 0x000a1fd0): the ambulance generator ("ambulance_out") of every hospital and the
    // four school bus generators of every school run while the building is locally funded above zero (its health 0xea56549e /
    // education 0x4a5654ba item), and stop otherwise (controller vslot 0x48 Activate).
    void HandleFundsChangedSweep() override {
        cSC4City* city = HostCity();
        if (!city) return;
        auto* residential = dynamic_cast<sc4::cSC4ResidentialSimulator*>(city->GetComponent(sc4::CityComponent::ResidentialSimulator));
        auto* budget = dynamic_cast<sc4::cSC4BudgetSimulator*>(city->GetComponent(sc4::CityComponent::BudgetSimulator));
        if (!residential || !budget) return;
        auto* controllers = dynamic_cast<sc4::cSC4AutomataControllerManager*>(city->GetComponent(sc4::CityComponent::AutomataControllerManager));
        auto* scripts = dynamic_cast<sc4::cSC4AutomataScriptSystem*>(city->GetComponent(sc4::CityComponent::AutomataScriptSystem));
        const auto funded = [budget](sc4::IResidentialOccupant* building, std::uint32_t department, std::uint32_t item) {
            sc4::cSC4DepartmentBudget* d = budget->GetDepartmentBudget(department);   // vslot 0x6c
            cISCPropertyHolder* holder = building->PropertyHolder();
            return d && holder && d->IsLocallyFundedObjectInDepartment(holder, item) && 0.0f < d->GetLocalFundingPercent(holder, item);
        };
        const auto occupantOf = [](sc4::IResidentialOccupant* building) -> sc4::cISC4Occupant* {
            cIGZUnknown* object = building->Object();
            return object ? dynamic_cast<sc4::cISC4Occupant*>(object) : nullptr;
        };
        if (tControllerTemplate* ambulance = scripts ? scripts->FindControllerTemplate(cRZString("ambulance_out")) : nullptr) {
            sc4::cSC4ResidentialSimulator::OccupantList hospitals;
            residential->GetHospitalBuildings(hospitals, nullptr);
            for (sc4::IResidentialOccupant* hospital : hospitals) {
                const bool on = funded(hospital, 0xaa538cb3, 0xea56549e);
                sc4::cISC4Occupant* occupant = occupantOf(hospital);
                if (sc4::cSC4AutomataController* c = controllers && occupant ? controllers->GetController(occupant, ambulance) : nullptr) c->Activate(on);
                hospital->Release();
            }
        }
        static const char* const kSchoolBusTemplates[] = {"hs_bus_in", "hs_bus_out", "k6_bus_in", "k6_bus_out"};
        sc4::cSC4ResidentialSimulator::OccupantList schools;
        residential->GetSchoolBuildings(schools, nullptr);
        for (sc4::IResidentialOccupant* school : schools) {
            const bool on = funded(school, 0x4a538cc6, 0x4a5654ba);
            sc4::cISC4Occupant* occupant = occupantOf(school);
            for (const char* name : kSchoolBusTemplates)
                if (sc4::cSC4AutomataController* c = controllers && occupant ? controllers->GetController(occupant, cRZString(name)) : nullptr) c->Activate(on);
            school->Release();
        }
    }
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
// pedestrians, aircraft, watercraft; city vslots 0x158 / 0x160 / 0x168 / 0x170) whose vslot 0xf0 takes the group.
sc4::cSC4AutomataManager* NativeAutomataManagerForGroup(std::uint32_t group) {
    for (CityComponent component : {CityComponent::VehicleManager, CityComponent::PedestrianManager, CityComponent::AircraftManager,
                                    CityComponent::WatercraftManager}) {
        cSC4AutomataManager* manager = Concrete<cSC4AutomataManager>(component);
        if (manager && manager->CanCreateGroup(group)) return manager;
    }
    return nullptr;
}
// SL::spVehicleManager / spPedestrianManager / spAircraftManager / spWatercraftManager by index (the automata scripts' source
// managers).
// The default group of the vehicle, aircraft or watercraft exemplar whose key instance is a catalog item (the earned-vehicle
// branch of cSC4View3DWin::InvokeCatalogItemCommand searches the managers' GetExemplarData records in that order); 0 for none.
std::uint32_t NativeVehicleGroupForCatalogItem(std::uint32_t item);
sc4::cSC4AutomataManager* NativeAutomataManagerAt(int index) {
    static constexpr CityComponent kManagers[4] = {CityComponent::VehicleManager, CityComponent::PedestrianManager, CityComponent::AircraftManager,
                                                   CityComponent::WatercraftManager};
    return index >= 0 && index < 4 ? Concrete<cSC4AutomataManager>(kManagers[index]) : nullptr;
}
bool NativeAutomataModelExemplars(int index, std::vector<sc4::cISCResExemplar*>* out) {
    sc4::cSC4AutomataManager* manager = NativeAutomataManagerAt(index);
    if (out) {
        out->clear();
        if (manager)
            for (const sc4::tExemplarData& record : manager->GetExemplarData()) out->push_back(record.exemplar);
    }
    return manager != nullptr;
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

namespace native {
std::uint32_t NativeVehicleGroupForCatalogItem(std::uint32_t item) {
    for (int index : {0, 2, 3}) {   // vehicles, aircraft, watercraft
        sc4::cSC4AutomataManager* manager = native::NativeAutomataManagerAt(index);
        if (!manager) continue;
        for (const sc4::tExemplarData& record : manager->GetExemplarData()) {
            cGZPersistResourceKey key;
            if (record.exemplar && record.exemplar->GetKey(key) && key.instance == item) return record.defaultGroup;
        }
    }
    return 0;
}

// The query tool's shift-click on an automaton: spCity vslot 0xac GetVehicleManager -> vslot 0x88 SetCameraFollow.
void NativeFollowAutomaton(cIGZUnknown* automaton) {
    if (sc4::cSC4AutomataManager* vehicles = NativeAutomataManagerAt(0)) vehicles->SetCameraFollow(AutomatonView(automaton));
}
}  // namespace native
