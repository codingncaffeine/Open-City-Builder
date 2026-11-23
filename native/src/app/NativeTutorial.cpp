// Tutorials on the native build: the hosts of the tutorial system (cSC4TutorialSystem), its tasks (cSC4TutorialTask) and the
// instruction window (cSC4TutorialUIWinProc). A tutorial city's saved tutorial GUID starts the tutorial when its view opens:
// the window reads the tutorial's Lua task table and drives the tasks; every game object it consults is reached here.
#include "app/NativeTutorial.h"

#include <cstdio>
#include <cstdlib>

#include "app/NativeBoot.h"
#include "app/NativeCheats.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeRegion.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "gz/GZCOM.h"
#include "gz/GZCOMDirector.h"
#include "app/NativeRegionScreen.h"
#include "sc4/RegionalCity.h"
#include "gz/WinHTML.h"
#include "sc4/App.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CameraControl.h"
#include "sc4/CitySituationManager.h"
#include "sc4/Demolition.h"
#include "sc4/Disaster.h"
#include "sc4/DispatchManager.h"
#include "sc4/ISC4RegionalCity.h"
#include "sc4/Lot.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/PowerLineOccupant.h"
#include "sc4/PowerPoleOccupant.h"
#include "sc4/Region.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TutorialRegistry.h"
#include "sc4/TutorialSystem.h"
#include "sc4/TutorialTask.h"
#include "sc4/TutorialUIWinProc.h"
#include "sc4/View3DWin.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"
#include "sc4/Render3D.h"
#include "s3d/Camera.h"
#include "app/NativeCityView.h"
#include <cmath>

namespace native {
sc4::IToolEffectsManager* NativeToolEffectsManager();   // NativeDisasterTools.cpp
cISCLua* NativeCityScriptingContext();                     // NativeAdvisors.cpp
sc4::cSC4Demolition* NativeDemolitionService();          // NativeDemolition.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_TUTORIALDEBUG") != nullptr;
    return on;
}

// A city component as class T: the object itself, or its serializable view (the port's components answer one or the other).
template <class T> T* Component(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    if (!object) return nullptr;
    if (T* direct = dynamic_cast<T*>(object)) return direct;
    void* raw = nullptr;
    if (!object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}

cSC4TutorialSystem* System() { return Component<cSC4TutorialSystem>(CityComponent::TutorialSystem); }
cSC4ZoneDeveloper* Lots() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper); }

// The city's scripting server is the advisor system (cSC4City vslot 0x288, its slot 0x50 GetScriptingContext).
cISCLua* CityLua() { return NativeCityScriptingContext(); }

struct TutorialCheats final : ITutorialCheatCodes {
    void RegisterCheatCode(std::uint32_t id, const char* text) override { RegisterGameCheat(id, text); }
    void UnregisterCheatCode(std::uint32_t id) override { UnregisterGameCheat(id); }
    void AddNotification(cIGZMessageTarget* target, std::uint32_t id) override { AddGameCheatListener(target, id); }
    void RemoveNotification(cIGZMessageTarget* target, std::uint32_t id) override { RemoveGameCheatListener(target, id); }
};

struct SystemHost final : ITutorialSystemHost {
    TutorialCheats cheats;
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    ITutorialCheatCodes* CheatCodes() override { return GameCheatCodeManager() ? &cheats : nullptr; }
    bool DebugFunctionalityEnabled() override { return GameDebugMode(); }
    bool TutorialRegistrySetActive(int guid, bool active) override {
        sc4::cSC4TutorialRegistry* registry = TutorialRegistry();
        return registry && registry->SetActive(guid, active, nullptr);
    }
    bool TutorialItemActive(int guid) override {
        sc4::cSC4TutorialRegistry* registry = TutorialRegistry();
        sc4::cSC4TutorialRegistry::Item* item = registry ? registry->GetItem(guid) : nullptr;
        if (Trace()) std::fprintf(stderr, "tutorial: item %08x %s\n", static_cast<unsigned>(guid), item ? (item->active ? "active" : "finished") : "unknown");
        return item && item->active;
    }
    int TutorialGUIDFromStoredValue(int value) override {   // sc4::cSC4TutorialRegistry::WhichToGUID
        if (value == 0) return kTutorialGUIDDefault;
        if (value == 1) return kTutorialGUIDSecond;
        return value;
    }
    cISCLua* ScriptingContext() override { return CityLua(); }
    void CloseTertiaryMenu() override { if (cSC4View3DWin* view = CityView3DWin()) view->CloseTertiaryMenu(); }
    void CloseSecondaryMenu() override { if (cSC4View3DWin* view = CityView3DWin()) view->CloseSecondaryMenu(); }
    void ResetTutorialCity(int guid) override {
        cSC4App* app = BootApplication();
        if (cISC4Region* region = app ? app->GetRegion() : nullptr) region->ResetTutorialCity(guid);
    }
    void SetRegionalCityTutorialGUID(int guid) override {
        if (auto* city = dynamic_cast<::cSC4RegionalCity*>(NativeCurrentRegionalCity())) city->SetTutorialGUID(guid);
    }
    void RequestSaveCity() override { if (cSC4App* app = BootApplication()) app->RequestSaveCity(true, false); }
    ITutorialUIWinProc* CreateTutorialUIWinProc() override {
        if (Trace()) std::fprintf(stderr, "tutorial: the instruction window starts\n");
        return new cSC4TutorialUIWinProc();
    }
};

struct TaskHost final : ITutorialTaskHost {
    bool CityEstablished() override { cSC4City* city = HostCity(); return city && city->GetEstablished(); }
    cIGZWin* View3DMainWindow() override {
        cSC4View3DWin* view = CityView3DWin();
        return view ? static_cast<cIGZWin*>(static_cast<cGZWin*>(view)) : nullptr;
    }
    bool CityCellIsInBounds(int cellX, int cellZ) override { cSC4City* city = HostCity(); return city && city->CellIsInBounds(cellX, cellZ); }
    bool CameraControlAvailable() override { return CityView3DWin() && CityView3DWin()->CameraControl(); }
    float TerrainCellWidth() override { return kTerrainCellWidth; }
    float TerrainAltitude(float x, float z) override {
        cSTETerrain* terrain = Component<cSTETerrain>(CityComponent::Terrain);
        return terrain ? terrain->GetAltitude(x, z) : 0.0f;
    }
    void SetCameraTargetPosition(float x, float y, float z) override {
        cSC4View3DWin* view = CityView3DWin();
        if (cSC4CameraControl* camera = view ? view->CameraControl() : nullptr) camera->SetViewTargetPosition(cS3DVector3{x, y, z});
    }
    void HiddenPauseSimulator() override { if (cSC4Simulator* s = CitySimulatorObject()) s->HiddenPause(); }
    void HiddenResumeSimulator() override { if (cSC4Simulator* s = CitySimulatorObject()) s->HiddenResume(); }
    cISC4DisasterLayer* DisasterLayer() override { return Component<cISC4DisasterLayer>(CityComponent::DisasterLayer); }
    IToolEffectsManager* EffectsManager() override { return NativeToolEffectsManager(); }
    ITutorialCSIVisibility* CSIVisibility() override { return Component<cSC4CitySituationManager>(CityComponent::CitySituationManager); }
    cISC4BuildingOccupant* BuildingAtCell(int cellX, int cellZ) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots ? lots->GetLot(cellX, cellZ, false) : nullptr;
        cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
        return building ? dynamic_cast<cISC4BuildingOccupant*>(building) : nullptr;
    }
};

struct UIHost final : ITutorialUIHost {
    cIGZWin* MainWindow() override { WinMgr* manager = ActiveWinMgr(); return manager ? manager->GetMainWindow() : nullptr; }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        *out = nullptr;
        cIGZWin* window = native::CreateWindowFromScript(key, parent, id);
        if (Trace()) std::fprintf(stderr, "tutorial: window %08x of script %08x -> %s\n", id, key.instance, window ? "made" : "FAILED");
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out) && *out;
        window->Release();
        return ok;
    }
    cIGZWinHTML* CreateHtmlControl() override {
        void* out = nullptr;
        if (!GZCOM() || !GZCOM()->CreateInstance(gzcom::kCLSID_cGZWinHTML, kGZIID_cIGZWinHTML, &out)) return nullptr;
        return static_cast<cIGZWinHTML*>(out);
    }
    cISC4TutorialSystem* TutorialSystem() override { return System(); }
    bool TutorialItem(int guid, int& firstTask, int& taskCount) override {
        sc4::cSC4TutorialRegistry* registry = TutorialRegistry();
        sc4::cSC4TutorialRegistry::Item* item = registry ? registry->GetItem(guid) : nullptr;
        if (!item) return false;
        firstTask = item->taskFirst;
        taskCount = item->taskCount;
        return true;
    }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    cIGZWin* ViewWindow() override {
        cSC4View3DWin* view = CityView3DWin();
        return view ? static_cast<cIGZWin*>(static_cast<cGZWin*>(view)) : nullptr;
    }
    void SetViewZoom(int zoom) override { if (cSC4View3DWin* view = CityView3DWin()) view->SetZoom(zoom); }
    void EndViewInput() override { if (cSC4View3DWin* view = CityView3DWin()) view->RemoveCurrentViewInputControl(false); }
    bool PickTerrain(int x, int y, float out[3]) override { cSC4View3DWin* view = CityView3DWin(); return view && view->PickTerrain(x, y, out, false); }
    bool CellViewPoint(int cellX, int cellZ, int& x, int& y) override {
        cSC4City* city = HostCity();
        sc4::cSC43DRender* render = CityRenderer();
        s3d::cS3DCamera* camera = render ? render->GetCamera() : nullptr;
        cSC4View3DWin* view = CityView3DWin();
        if (!city || !camera || !view) return false;
        cIGZUnknown* object = city->GetComponent(CityComponent::Terrain);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return false;
        auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
        const float wx = cellX * 16.0f + 8.0f, wz = cellZ * 16.0f + 8.0f;
        const cS3DVector3 world{wx, terrain->GetAltitude(wx, wz), wz};
        static_cast<cIGZSerializable*>(raw)->Release();
        cS3DVector3 screen;
        if (!camera->Project(world, screen)) return false;
        x = static_cast<int>(std::lround(screen.x));
        y = static_cast<int>(std::lround(screen.y));
        if (x < 0 || y < 0 || x >= view->GetW() || y >= view->GetH()) return false;
        float hit[3] = {};   // the point must land on that cell
        return view->PickTerrain(x, y, hit, false) && static_cast<int>(hit[0] / 16.0f) == cellX && static_cast<int>(hit[2] / 16.0f) == cellZ;
    }
    bool SimulatorPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsPaused(); }
    void ResumeSimulator() override { if (cSC4Simulator* s = CitySimulatorObject()) s->Resume(); }
    void PauseSimulator() override { if (cSC4Simulator* s = CitySimulatorObject()) s->Pause(); }
    void SetClockVisualTimeMode(int mode) override {
        if (auto* clock = Component<cSC424HourClock>(CityComponent::Clock24Hour)) clock->SetVisualTimeMode(static_cast<cSC424HourClock::VisualTimeMode>(mode));
    }
    void RequestGoToRegionView(bool ask) override { if (cSC4App* app = BootApplication()) app->RequestGoToRegionView(ask); }
    bool PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        city->PositionToCell(x, z, cellX, cellZ);
        return true;
    }
    bool LotRectAtCell(int cellX, int cellZ, SC4TutorialRect& rect) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots ? lots->GetLot(cellX, cellZ, false) : nullptr;
        if (!lot) return false;
        std::int32_t r[4] = {};
        lot->GetBoundingRect(r);
        rect = SC4TutorialRect{r[0], r[1], r[2], r[3]};
        return true;
    }
    bool DispatchPosition(std::uint32_t id, float& x, float& z) override {
        auto* dispatch = Component<cSC4DispatchManager>(CityComponent::DispatchManager);
        sc4::DispatchInfo info;
        if (!dispatch || !dispatch->GetDispatchInfo(id, info)) return false;
        x = info.x;
        z = info.z;
        return true;   // the info's station / automaton / pylon are the manager's (borrowed, as GetDispatchInfo copies them)
    }
    int ResidentialPopulation() override {
        auto* residential = Component<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator);
        return residential ? residential->GetPopulation() : 0;
    }
    // GetDisasterManager(id) slot 10 of the interface view: read as "instances still active" (inferred: the only count the
    // manager answers without arguments).
    bool DisasterPlaying(std::uint32_t id) override {
        auto* layer = Component<cISC4DisasterLayer>(CityComponent::DisasterLayer);
        cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(id) : nullptr;
        return manager && manager->GetActiveInstanceCount() != 0;
    }
    void DemolishCells(const SC4TutorialRect& rect) override {
        cSC4Demolition* demolition = NativeDemolitionService();
        if (!demolition) return;
        DemolitionRegion region;
        region.bounds = DemolitionCellRect{rect.x0, rect.z0, rect.x1, rect.z1};
        demolition->DemolishRegion(true, region, 0, 1, false, nullptr, nullptr, nullptr, nullptr, 0, 0);
    }
    bool DoURL(cIGZString* link) override { return link && NativeHtmlLinkDoURL(*link); }
    int NetworkPiece(sc4::cISC4Occupant* occupant, std::uint32_t networkType, bool tall, int& x, int& z) override {
        auto* piece = dynamic_cast<cSC4NetworkOccupant*>(occupant);
        if (!piece || piece->HasNetworkFlag(0x80000000u) || !piece->IsOfType(networkType) || piece->HasNetworkFlag(0x200000u)) return -1;
        std::uint32_t cellX = 0, cellZ = 0;
        piece->GetOccupiedCell(cellX, cellZ);
        x = static_cast<int>(cellX);
        z = static_cast<int>(cellZ);
        const auto type = static_cast<std::uint8_t>(networkType);
        const bool connected = tall ? (piece->ConnectsToNeighbor(1, type) && piece->ConnectsToNeighbor(3, type))
                                    : (piece->ConnectsToNeighbor(0, type) && piece->ConnectsToNeighbor(2, type));
        return connected ? 1 : 0;
    }
    bool PowerPieceCell(sc4::cISC4Occupant* occupant, int& x, int& z) override {
        if (auto* line = dynamic_cast<cSC4PowerLineOccupant*>(occupant); line && !line->IsPreview()) {
            std::uint32_t cx = 0, cz = 0;
            line->GetCell(cx, cz);
            x = static_cast<int>(cx);
            z = static_cast<int>(cz);
            return true;
        }
        if (auto* pole = dynamic_cast<cSC4PowerPoleOccupant*>(occupant); pole && !pole->IsPreview()) {
            pole->GetCell(x, z);
            return true;
        }
        return false;
    }
    bool BuildingType(sc4::cISC4Occupant* occupant, std::uint32_t& type) override {
        void* raw = nullptr;
        if (!occupant || !occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) || !raw) return false;
        auto* building = static_cast<cISC4BuildingOccupant*>(raw);
        type = building->GetBuildingType();
        building->Release();
        return true;
    }
    bool LotBoundingRect(cIGZUnknown* lot, SC4TutorialRect& rect) override {
        auto* concrete = lot ? dynamic_cast<cSC4Lot*>(lot) : nullptr;
        if (!concrete) return false;
        std::int32_t r[4] = {};
        concrete->GetBoundingRect(r);
        rect = SC4TutorialRect{r[0], r[1], r[2], r[3]};
        return true;
    }
};

}  // namespace

bool NativeTutorialFilterUiCommand(cGZMessage& message) {
    sc4::SetTutorialFilterSystem(System());   // cSC4City vslot 0x290, looked up per call (the city changes)
    return sc4::FilterUICommandMessage(message);
}

// The tutorial system as the city view's mouse router: IsTutorial (vslot 0x20), IsEdgeScrollingAllowed (0x24) and
// FilterLeftMouseClick (0x40, flags -1 press / 0 move / 1 release), looked up per call (the city changes).
sc4::ICityMouseRouter* NativeTutorialMouseRouter() {
    struct Router final : sc4::ICityMouseRouter {
        bool IsActive() override { cSC4TutorialSystem* t = System(); return t && t->IsTutorial(); }
        bool AllowsEdgeScroll() override { cSC4TutorialSystem* t = System(); return !t || t->IsEdgeScrollingAllowed(); }
        bool ProcessMouse(int& x, int& y, std::uint32_t flags) override {
            cSC4TutorialSystem* t = System();
            return !t || t->FilterLeftMouseClick(x, y, static_cast<int>(flags));
        }
    };
    static Router router;
    return System() ? &router : nullptr;
}

bool NativeTutorialCheat(const char* text) {
    cSC4TutorialSystem* tutorial = System();
    if (!tutorial) return false;
    cRZString typed(text);
    cGZMessage message;
    message.type = sc4::kTutorialMessageCheatCode;
    message.param1 = sc4::kTutorialCheatId;
    message.param2 = reinterpret_cast<std::uintptr_t>(static_cast<cIGZString*>(&typed));
    tutorial->DoMessage(message);
    return true;
}

bool NativeTutorialAllowsZone(int x0, int z0, int x1, int z1, bool flood) {
    cSC4TutorialSystem* tutorial = System();
    if (!tutorial || !tutorial->IsTutorial()) return true;                       // spTutorialSystem vslot 0x20
    cISC4TutorialTask* task = tutorial->GetCurrentTask();                         // vslot 0x28
    if (!task) return true;
    const sc4::SC4TutorialRect& area = task->GetRegion();                         // task vslot 0x14
    static const bool trace = std::getenv("SC4_ZONEDEBUG") != nullptr;
    if (trace) std::fprintf(stderr, "tutorial zone gate: task %d kind %d region %d,%d..%d,%d, asked %d,%d..%d,%d flood %d\n", tutorial->GetTutorialTaskNumber(), task->GetWhichFunction(),
                            area.x0, area.z0, area.x1, area.z1, x0, z0, x1, z1, flood ? 1 : 0);
    if (area.x0 < 0) return true;
    if (!flood && area.x0 == x0 && area.z0 == z0 && area.x1 == x1 && area.z1 == z1) return true;
    tutorial->OnTutorialEvent(static_cast<cISC4TutorialSystem::eTutorialEvent>(1));   // vslot 0x48 (1)
    return false;
}

void NativeTutorialCatalogOpened(sc4::cISC4WinCatalogView* catalog) {
    cSC4TutorialSystem* tutorial = System();
    if (tutorial && tutorial->IsTutorial()) tutorial->HandleCatalogMenu(catalog);
}

void InstallTutorialHosts() {
    static SystemHost system;
    static TaskHost task;
    static UIHost ui;
    SetTutorialSystemHost(&system);
    SetTutorialTaskHost(&task);
    SetTutorialUIHost(&ui);
}

}  // namespace native
