// The city situation manager's services on the native application (sc4::ICitySituationManagerHost): the advisor system's
// script state, the message server, the application's debug flag and completed-mission preferences, the cheat code manager,
// the city view, the game-data registry, the animation tick manager (SL::ATM(): the manager's DoAnimationTick), the window
// manager, the HTML link handler and the "city situations" advisor (id 0x1d). Also the kit's read-only `situations` verb.
// The automata, occupants and their managers come through the views of app/NativeSituationAutomata.cpp.
//   PARTIAL: the city geometry, lot / zone managers, terrain, traffic network, the landfill developer, the effects manager
//   and the MySim answer null. The situation indicators are the dispatch manager's cSC4DispatchVehicleView
//   (app/NativeDispatchVehicleView.cpp).
#include <cmath>
#include "platform/Unwired.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCheats.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeGameData.h"
#include "app/NativeWinMgr.h"
#include "gz/Buffer.h"
#include "gz/CheatCodeManager.h"
#include "gz/Font.h"
#include "gz/WinFont.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/AutoHelpers.h"
#include "sc4/AuraSimulator.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/CivicBuildingSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ViewInputControl.h"
#include "sc4/ViewInputControlPlayerDrive.h"
#include "sc4/ViewUtilities.h"
#include "sc4/WinLocationSaver.h"
#include "gz/cGZPersistResourceManager.h"
#include "app/NativeUi.h"
#include "sc4/AppPreferences.h"
#include "sc4/CommonIIDs.h"
#include "sc4/CitySituationManager.h"
#include "sc4/ISC4App.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"
#include <memory>
#include <map>
#include "app/NativeZoneDeveloperHost.h"
#include "sc4/Lot.h"
#include "sc4/LandfillZone.h"
#include "sc4/LandfillDeveloper.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/SurfaceWater.h"
#include "sc4/cSTETerrain.h"
#include "app/NativeDemolition.h"   // CityNeighborsSimulator
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"

namespace native {
std::string FontStyleName(std::uint32_t guid);   // linux/game_main_linux.cpp
sc4::ISituationLua* NativeSituationLua();   // NativeCitySituationAdvice.cpp
// app/NativeSituationAutomata.cpp
sc4::ISituationOccupant* SituationOccupantOf(void* engineOccupant);
sc4::ISituationOccupant* NativeNewCoverageTarget(const float position[3]);   // NativeCoverageTarget.cpp
sc4::ISituationOccupant* NativeEscapeConnectionTarget(sc4::ISituationAutomaton* automaton);   // NativeTrafficAutomata.cpp
sc4::ISituationOccupant* SituationViewOfOccupant(void* engineOccupant);                    // NativeSituationAutomata.cpp
sc4::ISituationAutomaton* SituationAutomatonOf(void* engineAutomaton);
int SituationAutomataManagerCount();
sc4::ISituationAutomataManager* SituationAutomataManager(int index);
sc4::ISituationOccupantManager* SituationOccupantManager();
cIGZUnknown* SituationEngineOccupant(sc4::ISituationOccupant* occupant);
sc4::ISituationEffect* NativeCreateSituationEffect(const char* name);   // app/NativeDisasterTools.cpp
bool NativePlayerDriveAutomatonPosition(float out[3]);                 // app/NativePlayerDrive.cpp
int NativeZoneTypeAt(int x, int z);                                      // app/NativeDisasters.cpp
sc4::ISituationMySim* NativeActiveSituationMySim();                      // app/NativeMySim.cpp
sc4::ISituationDispatchView* NativeSituationDispatchView();   // NativeDispatchVehicleView.cpp
void SituationEnterPlayerDriveView(sc4::ISituationAutomaton* automaton, cIGZMessageTarget2* target);
std::uint32_t SituationSourceBuildingGroup(std::uint32_t automataGroup);

namespace {
using namespace sc4;

constexpr int kCitySituationsAdvisor = 0x1d;
constexpr std::size_t kCompletedMissionIds = 0x1334;   // preferences: 64 u32 mission ids, zero terminated (inside SC4TutorialPreferences2)

cISC4Advisor* SituationsAdvisor() {
    cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
    return system ? system->GetAdvisor(kCitySituationsAdvisor) : nullptr;
}

// The situation's cISC4Advice view (the same object: the situation's base view answers it).
cISC4Advice* AdviceOf(cISC4CitySituationAdvice* situation) {
    auto* base = situation ? static_cast<ISituationAdviceBase*>(situation->AsAdvice()) : nullptr;
    cIGZUnknown* outer = base ? base->Outer() : nullptr;
    return outer ? static_cast<cISC4Advice*>(outer) : nullptr;
}

class SituationApp final : public ISituationApp {
public:
    bool DebugFunctionalityEnabled() override { return SL::spSC4 && SL::spSC4->GetDebugFunctionalityEnabled(); }
    std::uint32_t* CompletedMissionIds() override {
        std::uint8_t* preferences = SL::spSC4 ? SL::spSC4->GetPreferences() : nullptr;
        return preferences ? reinterpret_cast<std::uint32_t*>(preferences + kCompletedMissionIds) : nullptr;
    }
    void SavePreferences() override { if (SL::spSC4) SL::spSC4->SavePreferences(); }
} g_app;

class SituationCheats final : public ISituationCheatCodes {
public:
    void RegisterCheatCode(std::uint32_t id, const char* text) override {
        if (cGZCheatCodeManager* m = GameCheatCodeManager()) m->RegisterCheatCode(id, cRZString(text));
    }
    void UnregisterCheatCode(std::uint32_t id) override { if (cGZCheatCodeManager* m = GameCheatCodeManager()) m->UnregisterCheatCode(id); }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t id) override {
        if (cGZCheatCodeManager* m = GameCheatCodeManager()) m->AddNotification2(target, id);
    }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t id) override {
        if (cGZCheatCodeManager* m = GameCheatCodeManager()) m->RemoveNotification2(target, id);
    }
} g_cheats;

// SL::spView3D: the mission status panel (vslots 0x1c0 / 0x1c8) and the U-Drive-It tool's removal (vslots 0xc0 / 0xd0).
class SituationView final : public ISituationView3D {
public:
    void ShowMissionStatusPanel(std::uint32_t panel) override { if (cSC4View3DWin* v = CityView3DWin()) v->ShowMissionStatusPanel(panel); }
    void HideMissionStatusPanel() override { if (cSC4View3DWin* v = CityView3DWin()) v->HideMissionStatusPanel(); }
    void EndPlayerDriveTool() override {
        cSC4View3DWin* v = CityView3DWin();
        cSC4ViewInputControl* current = v ? v->GetCurrentViewInputControl() : nullptr;
        if (current && current->GetID() == kPlayerDriveToolId) v->RemoveCurrentViewInputControl(false);
    }
} g_view;

// The automata side (app/NativeSituationAutomata.cpp).
class SituationBudget final : public ISituationBudgetSim {
public:
    // SL::spBudgetSim vslot 0x38 (i386 0x1c) DepositFunds.
    void AddMoney(long long amount) override { if (cSC4BudgetSimulator* b = Budget()) b->DepositFunds(amount); }
    static cSC4BudgetSimulator* Budget() {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::BudgetSimulator) : nullptr;
        return object ? dynamic_cast<cSC4BudgetSimulator*>(object) : nullptr;
    }
} g_budget;
class SituationAura final : public ISituationAuraSim {
public:
    // SL::spAuraSim vslot 0x30 (i386 0x18) AddTransientEffect(x, z, radius, magnitude).
    void AddAura(float x, float z, float radius, float magnitude) override {
        if (cISC4AuraSimulator* a = Aura()) a->AddTransientEffect(x, z, radius, magnitude);
    }
    static cISC4AuraSimulator* Aura() {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::AuraSimulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4AuraSimulator, &raw) || !raw) return nullptr;
        auto* aura = static_cast<cISC4AuraSimulator*>(raw);
        aura->Release();   // the city keeps it
        return aura;
    }
} g_aura;

// SL::ATM(): the manager joins the animation ticks (AddTickableObject) and gets 0x86ad10ee every frame.
class SituationTicks final : public ISituationAutomatonNotifier {
public:
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t mask) override {
        if (cIGZMessageServer2* atm = AnimationTickServerView()) atm->AddNotification(target, mask);
    }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t mask) override {
        if (cIGZMessageServer2* atm = AnimationTickServerView()) atm->RemoveNotification(target, mask);
    }
} g_ticks;

// ---- the city services the situations read (SL::spCity / spLotManager / spEffectsManager / spZoneManager / spTerrain /
// spSurfaceWater / spTrafficNetworkMap / spLandfillDeveloper) ----------------------------------------------------------------
sc4::cSTETerrain* SituationTerrainObject() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
sc4::cSTESurfaceWater* SituationSurfaceWater() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::SurfaceWater) : nullptr;
    return object ? dynamic_cast<sc4::cSTESurfaceWater*>(object) : nullptr;
}
sc4::cSC4TrafficNetworkMap* SituationTrafficMap() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::TrafficNetwork) : nullptr;
    return object ? dynamic_cast<sc4::cSC4TrafficNetworkMap*>(object) : nullptr;
}
sc4::cSC4LandfillDeveloper* SituationLandfills() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::LandfillDeveloper) : nullptr;
    return object ? dynamic_cast<sc4::cSC4LandfillDeveloper*>(object) : nullptr;
}

struct SituationCityView final : ISituationCity {
    int CellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }                 // 0x2e8
    int CellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }                 // 0x2f0
    float CellSizeXMeters() override { return 16.0f; }                                                   // 0x2d8
    void WorldToCell(float x, float z, int& cellX, int& cellZ) override {                                // 0x2f8
        if (cSC4City* c = HostCity()) c->PositionToCell(x, z, cellX, cellZ);
        else { cellX = static_cast<int>(x / 16.0f); cellZ = static_cast<int>(z / 16.0f); }
    }
    void CellToWorld(int cellX, int cellZ, float xyz[3]) override {                                      // 0x308: the cell's centre
        if (cSC4City* c = HostCity()) c->CellCenterToPosition(cellX, cellZ, xyz[0], xyz[2]);
        else { xyz[0] = cellX * 16.0f + 8.0f; xyz[2] = cellZ * 16.0f + 8.0f; }
        sc4::cSTETerrain* terrain = SituationTerrainObject();
        xyz[1] = terrain ? terrain->GetAltitude(xyz[0], xyz[2]) : 0.0f;
    }
} g_situationCity;

// lot manager vslot 0x70 GetOccupantLot: the lot under the occupant (the zone developer's lot map at its cell).
struct SituationLotView final : ISituationLot {
    sc4::cSC4Lot* lot = nullptr;
    void Bounds(int& x0, int& z0, int& x1, int& z1) override {
        std::int32_t r[4] = {0, 0, 0, 0};
        if (lot) lot->GetBoundingRect(r);
        x0 = r[0]; z0 = r[1]; x1 = r[2]; z1 = r[3];
    }
};
struct SituationLotsView final : ISituationLotManager {
    ISituationLot* LotOfOccupant(ISituationOccupant* occupant) override {
        if (!occupant) return nullptr;
        float p[3] = {0, 0, 0};
        occupant->Position(p);
        int x = 0, z = 0;
        g_situationCity.WorldToCell(p[0], p[2], x, z);
        sc4::cSC4Lot* lot = NativeLotAtCell(x, z, false);
        if (!lot) return nullptr;
        std::unique_ptr<SituationLotView>& view = views_[lot];
        if (!view) view = std::make_unique<SituationLotView>();
        view->lot = lot;
        return view.get();
    }
private:
    std::map<sc4::cSC4Lot*, std::unique_ptr<SituationLotView>> views_;
} g_situationLots;

struct SituationEffectsView final : ISituationEffectsManager {
    bool CreateEffect(const char* name, ISituationEffect*& out) override {   // effects manager vslot 0x18 by name
        if (out) {
            out->Release();
            out = nullptr;
        }
        out = NativeCreateSituationEffect(name);
        return out != nullptr;
    }
} g_situationEffects;

struct SituationDriveView final : ISituationDriveView {
    void Position(float xyz[3]) override { NativePlayerDriveAutomatonPosition(xyz); }   // the automaton's vslot 0x1c
} g_situationDrive;

struct SituationZonesView final : ISituationZoneManager {
    int ZoneTypeAt(int x, int z) override { return NativeZoneTypeAt(x, z); }
} g_situationZones;

struct SituationTerrainView final : ISituationTerrain {
    bool IsWaterAt(int x, int z) override {                                    // terrain i386 vslot 0x88 CellHasWater
        sc4::cSTETerrain* t = SituationTerrainObject();
        return t && x >= 0 && z >= 0 && t->CellHasWater(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z));
    }
    float HeightAt(float x, float z) override { sc4::cSTETerrain* t = SituationTerrainObject(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float SurfaceWaterDepthAt(int x, int z) override {                         // spSurfaceWater vslot 0x30 GetWaterDepthForCell
        sc4::cSTESurfaceWater* w = SituationSurfaceWater();
        return w ? w->GetWaterDepthForCell(x, z) : 0.0f;
    }
    bool IsCellWaterZoneCandidate(int x, int z) override {                     // spSurfaceWater vslot 0x38 IsNavigable(x, z, 2, null)
        sc4::cSTESurfaceWater* w = SituationSurfaceWater();
        return w && w->IsNavigable(x, z, 2, nullptr);
    }
} g_situationTerrain;

struct SituationTrafficView final : ISituationTrafficNetwork {
    bool HasNetworkAt(int x, int z) override {                                 // map vslot 0x70 (i386 0x38) SafeGetTileInfoAtCell
        sc4::cSC4TrafficNetworkMap* m = SituationTrafficMap();
        return m && m->SafeGetTileInfoAtCell(x, z) != nullptr;
    }
    bool CellHasNetworkWithFlags(int x, int z, std::uint32_t flags) override {   // vslot 0x68 (i386 0x34): the cell's tiles
        sc4::cSC4TrafficNetworkMap* m = SituationTrafficMap();
        for (const sc4::TrafficTileInfo* tile = m ? m->SafeGetTileInfoAtCell(x, z) : nullptr; tile; tile = tile->next)
            if (tile->occupant && tile->occupant->HasAnyNetworkFlag(flags)) return true;
        return false;
    }
} g_situationTraffic;

// landfill developer vslot 0x60 GetLandfillZoneFromCell: the zone (counted) whose region bounds the situation reads.
class SituationLandfillLot final : public ISituationLandfillLot {
public:
    explicit SituationLandfillLot(sc4::cSC4LandfillZone* zone) : zone_(zone) { zone_->AddRef(); }
    bool QueryInterface(std::uint32_t, void** out) override { *out = nullptr; return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        zone_->Release();
        delete this;
        return 0;
    }
    const int* Rect() override {
        const sc4::LandfillCellRegion* region = zone_->Region();
        if (!region) return nullptr;
        rect_[0] = region->bounds.x0; rect_[1] = region->bounds.z0; rect_[2] = region->bounds.x1; rect_[3] = region->bounds.z1;
        return rect_;
    }
private:
    sc4::cSC4LandfillZone* zone_;
    int rect_[4] = {0, 0, 0, 0};
    std::uint32_t refs_ = 1;
};
struct SituationLandfillView final : ISituationLandfillDeveloper {
    ISituationLandfillLot* LandfillLotAt(int x, int z) override {
        sc4::cSC4LandfillDeveloper* d = SituationLandfills();
        sc4::cSC4LandfillZone* zone = d ? d->GetLandfillZoneFromCell(x, z) : nullptr;
        return zone ? new SituationLandfillLot(zone) : nullptr;
    }
} g_situationLandfill;

class CitySituationManagerHost final : public ICitySituationManagerHost {
public:
    ISituationLua* Lua() override { return NativeSituationLua(); }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    ISituationApp* App() override { return SL::spSC4 ? &g_app : nullptr; }
    ISituationCheatCodes* CheatCodes() override { return GameCheatCodeManager() ? &g_cheats : nullptr; }
    ISituationView3D* View3D() override { return &g_view; }
    // SL::spDispatchManager vslot 0xb0 GetDispatchVehicleView: the indicators of the dispatch manager's view.
    ISituationDispatchView* DispatchView() override { return NativeSituationDispatchView(); }
    ISituationCity* City() override { return HostCity() ? &g_situationCity : nullptr; }                 // SL::spCity
    ISituationLotManager* LotManager() override { return HostCity() ? &g_situationLots : nullptr; }         // SL::spLotManager
    ISituationEffectsManager* EffectsManager() override { return HostCity() ? &g_situationEffects : nullptr; }   // SL::spEffectsManager
    int AutomataManagerCount() override { return SituationAutomataManagerCount(); }
    ISituationAutomataManager* AutomataManager(int index) override { return SituationAutomataManager(index); }
    ISituationAutomataManager* VehicleManager() override { return SituationAutomataManager(0); }
    // cSC4AutomataManager::spPlayerDriveView +0x30 (i386), the driven automaton; null while nothing is driven.
    ISituationDriveView* PlayerDriveView() override {
        float position[3];
        return NativePlayerDriveAutomatonPosition(position) ? &g_situationDrive : nullptr;
    }
    ISituationMissionActor* MySimActor() override { return nullptr; }   // nothing asks for it (port-only slot)
    ISituationMySim* ActiveMySim() override { return NativeActiveSituationMySim(); }
    ISituationOccupantManager* OccupantManager() override { return SituationOccupantManager(); }
    ISituationZoneManager* ZoneManager() override { return HostCity() ? &g_situationZones : nullptr; }
    ISituationTerrain* Terrain() override { return SituationTerrainObject() ? &g_situationTerrain : nullptr; }
    ISituationTrafficNetwork* TrafficNetwork() override { return SituationTrafficMap() ? &g_situationTraffic : nullptr; }
    ISituationAuraSim* AuraSim() override { return SituationAura::Aura() ? &g_aura : nullptr; }
    ISituationBudgetSim* BudgetSim() override { return SituationBudget::Budget() ? &g_budget : nullptr; }
    ISituationLandfillDeveloper* LandfillDeveloper() override { return SituationLandfills() ? &g_situationLandfill : nullptr; }
    void EnterPlayerDriveView(ISituationAutomaton* automaton, cIGZMessageTarget2* target) override { SituationEnterPlayerDriveView(automaton, target); }
    cIGZWinMgr* WindowManager() override { return ActiveWinMgr(); }
    bool DoURL(const void* text) override { return text && NativeHtmlLinkDoURL(*static_cast<const cIGZString*>(text)); }
    // The advisor's current advices (vslot 0x58), each asked for the situation interface (0x6beb1e8a).
    std::vector<cISC4CitySituationAdvice*> AdvisorSituations() override {
        std::vector<cISC4CitySituationAdvice*> out;
        cISC4Advisor* advisor = SituationsAdvisor();
        if (!advisor) return out;
        for (const auto& entry : advisor->GetCurrentAdvices()) {
            cISC4CitySituationAdvice* situation = nullptr;
            if (entry.first && entry.first->QueryInterface(kGZIID_cISC4CitySituationAdvice, reinterpret_cast<void**>(&situation)) && situation) {
                out.push_back(situation);
                situation->Release();   // the advisor keeps the advice
            }
        }
        return out;
    }
    // The advisor's RetireAdvice (vslot 0xa0) with the situation's advice view.
    void RetireAdvisorSituation(cISC4CitySituationAdvice* situation) override {
        cISC4Advisor* advisor = SituationsAdvisor();
        if (cISC4Advice* advice = AdviceOf(situation); advisor && advice) advisor->RetireAdvice(advice);
    }
    sc4::cISC4GameDataRegistry* GameDataRegistry() override { return NativeGameDataRegistry(); }
    ISituationAutomatonNotifier* AutomataNotifier() override { return &g_ticks; }
    // The messages carry the engine objects (a cISC4Occupant from the occupant manager, the cSC4Automaton that calls back).
    ISituationOccupant* OccupantOfMessage(void* occupant) override { return SituationOccupantOf(occupant); }
    ISituationAutomaton* AutomatonOfMessage(void* automaton) override { return SituationAutomatonOf(automaton); }
    // The advisor's GetAdviceByGUID (vslot 0x60), asked for the situation interface.
    cISC4CitySituationAdvice* AdvisorSituationByGuid(std::uint32_t guid) override {
        cISC4Advisor* advisor = SituationsAdvisor();
        cISC4Advice* advice = advisor ? advisor->GetAdviceByGUID(static_cast<int>(guid)) : nullptr;
        cISC4CitySituationAdvice* situation = nullptr;
        if (!advice || !advice->QueryInterface(kGZIID_cISC4CitySituationAdvice, reinterpret_cast<void**>(&situation))) return nullptr;
        return situation;
    }
    std::uint32_t SourceBuildingGroup(std::uint32_t group) override { return SituationSourceBuildingGroup(group); }
    ISituationOccupant* NewCoverageTarget(const float position[3]) override { return NativeNewCoverageTarget(position); }
    ISituationOccupant* EscapeConnectionTarget(ISituationAutomaton* automaton) override { return NativeEscapeConnectionTarget(automaton); }
    ISituationOccupant* ViewOfEngineOccupant(void* engineOccupant) override { return SituationViewOfOccupant(engineOccupant); }
    bool NeighborConnectionCells(std::vector<std::pair<int, int>>& out) override {
        sc4::cSC4NeighborsSimulator* neighbors = CityNeighborsSimulator();
        if (!neighbors) return false;
        sc4::ConnectionList connections;
        neighbors->GetAllConnections(connections);
        for (const auto& connection : connections) {
            if (!connection) continue;
            int x = 0, z = 0;
            connection->GetConnectionCell(x, z);
            out.emplace_back(x, z);
        }
        return true;
    }
    bool ViewFrustumBox(float boxMin[3], float boxMax[3]) override {
        SC4ViewUtilities* view = SL::spViewUtilities ? static_cast<SC4ViewUtilities*>(SL::spViewUtilities) : nullptr;
        if (!view) return false;
        view->GetViewFrustumBoundingBox(boxMin, boxMax);
        return true;
    }
    void RegisterTargetOccupant(ISituationOccupant* occupant, bool add) override {
        sc4::cISC4GameDataRegistry* registry = NativeGameDataRegistry();
        cIGZUnknown* engine = SituationEngineOccupant(occupant);
        if (!registry || !engine) return;
        if (add) {
            registry->AddChildEntry(engine, "", registry->GetRootNodeEntry(), 0, nullptr);
        } else {
            registry->RemoveChildEntry(engine);
        }
    }
    bool CreateSituationWindow(std::uint32_t instance, std::uint32_t windowId, cIGZWin** out) override {
        *out = nullptr;
        WinMgr* manager = ActiveWinMgr();
        const cGZPersistResourceKey key{0, 0x96a006b0, instance};
        cIGZWin* window = CreateWindowFromScript(key, manager ? manager->GetMainWindow() : nullptr, windowId);
        std::printf("situations: popup script %08x -> %s\n", instance, window ? "created" : "FAILED");
        *out = window;
        return window != nullptr;
    }
    // SL::AEMS() vslot 0x30(0); cSC4AutoGamePause(false, true, true, true).
    void BeginPopupPause() override { SC4GamePause(true, false, true, true, true); }
    void EndPopupPause() override { SC4GamePause(false, false, true, true, true); }
    void RestoreWindowLocation(cIGZWin* window) override {
        if (auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver)) saver->RestoreWindowLocation(window, true);
    }
    void SaveWindowLocation(cIGZWin* window) override {
        if (auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver)) saver->SaveWindowLocation(window);
    }
    void FocusView3D() override {
        WinMgr* manager = ActiveWinMgr();
        cSC4View3DWin* view = CityView3DWin();
        if (manager && view) manager->GZSetFocus(view->AsIGZWin());
    }
    bool DashboardMissionPanel(std::uint32_t dashboard, std::uint32_t& panel) override {
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0x690f693f, dashboard}, 0xa52160f5, &raw, 0, nullptr) || !raw) return false;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        const bool found = cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0xec1b2361, panel);
        exemplar->Release();
        return found;
    }
    bool SystemFont(std::uint32_t fontId, SituationDialogFont& out) override {
        gz::IWinFontSource* fonts = gz::WinFontSource();
        gz::IWinFont* style = fonts ? fonts->Font(fontId) : nullptr;
        cIGZFont* font = style ? style->AsFont() : nullptr;
        if (!font) return false;
        // The style's own name: the native HTML font system resolves it to the style's face and parameters.
        const std::string styleName = FontStyleName(fontId);
        if (!styleName.empty()) out.name = cRZString(styleName.c_str());
        out.bold = font->GetStyle(1);
        out.size = font->GetSize();
        return true;
    }
    bool RewardStatus(std::uint32_t guid, SituationRewardStatus& out) override {
        sc4::cSC4CivicBuildingSimulator* civic = CivicSimulator();
        if (!civic) return false;
        const auto* record = civic->GetConditionalBuildingStatus(guid);
        out.hasRecord = record != nullptr;
        out.state = record ? record->state : 0;
        out.advisorHasAdvice = false;
        if (cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance()) {
            if (AdvisorMap* advisors = system->GetAdvisors()) {
                for (auto& entry : *advisors) {
                    cISC4Advice* advice = entry.second ? entry.second->GetAdviceByGUID(static_cast<int>(guid)) : nullptr;
                    if (!advice) continue;
                    (void)advice->GetTrigger();   // called by the original, its answer unused
                    out.advisorHasAdvice = true;
                    break;
                }
            }
        }
        return true;
    }
    cIGZBuffer* SituationPicture(std::uint32_t imageId) override {
        return static_cast<cIGZBuffer*>(LoadBitmapResource(0x46a006b0, imageId));
    }

private:
    static sc4::cSC4CivicBuildingSimulator* CivicSimulator() {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::CivicBuildingSimulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        auto* civic = serializable->GetGZCLSID() == kGZCLSID_cSC4CivicBuildingSimulator
                          ? static_cast<sc4::cSC4CivicBuildingSimulator*>(serializable) : nullptr;
        serializable->Release();
        return civic;
    }
} g_host;

cSC4CitySituationManager* Manager() { return HostCitySituations(); }

}  // namespace

void InstallCitySituationManager() { SetCitySituationManagerHost(&g_host); }

// The kit's `situations` verb: the manager's state as the query windows read it (read-only).
void NativeSituationsReport() {
    cSC4CitySituationManager* m = Manager();
    if (!m) {
        std::printf("situations: no manager\n");
        return;
    }
    std::printf("situations: init %d host %s csi %d max %u status %u tracked %u targets %u/%u time %.1f/%.1f countdown %d\n",
                m->IsInitialized() ? 1 : 0, sc4::CitySituationManagerHost() ? "real" : "none", m->GetVisible() ? 1 : 0, m->GetMaxCSI(),
                m->GetCurrentStatus(), m->GetTrackedAutomataCount(), m->GetCurrentTargetIndex(), m->GetNumTargets(),
                m->GetCurrentSituationTime(), m->GetCurrentTimeLimitSeconds(), m->CountdownIsInProgress() ? 1 : 0);
    for (const SituationTrack& track : m->GetSituationTracks()) {
        cISC4Advice* advice = AdviceOf(track.advice.get());
        std::printf("  track advice 0x%08x \"%s\" automaton %s indicator %u\n", advice ? static_cast<std::uint32_t>(advice->GetGUID()) : 0u,
                    advice ? advice->GetTextTitle().ToChar() : "", track.automaton ? "yes" : "no", track.trackId);
    }
    if (const cISC4CitySituationAdvice* active = m->GetActiveSituation()) {
        cISC4Advice* advice = AdviceOf(const_cast<cISC4CitySituationAdvice*>(active));
        std::printf("  active 0x%08x condition %u flags 0x%x success distance %.1f radius %.1f timeout %.1f\n",
                    advice ? static_cast<std::uint32_t>(advice->GetGUID()) : 0u, active->GetSuccessCondition(), active->GetSituationFlags(),
                    active->GetSuccessDistance(), active->GetActiveRadius(), active->GetSuccessTimeout());
    }
    float a[3] = {0, 0, 0}, t[3] = {0, 0, 0};
    if (ISituationAutomaton* automaton = m->GetActiveAuto()) {
        automaton->Position(a);
        std::printf("  automaton (%.1f, %.1f, %.1f) behavior 0x%x\n", a[0], a[1], a[2], automaton->GetBehavior());
    }
    if (ISituationOccupant* target = m->GetCurrentTarget()) {
        target->Position(t);
        std::printf("  target type 0x%08x (%.1f, %.1f, %.1f) flat distance %.1f\n", target->OccupantType(), t[0], t[1], t[2],
                    std::sqrt((t[0] - a[0]) * (t[0] - a[0]) + (t[2] - a[2]) * (t[2] - a[2])));
    }
    std::printf("  popup %s, successes of the active advice %u\n", m->PopupDialog() ? "open" : "none",
                m->GetActiveSituation() ? m->GetMissionSuccessCount(static_cast<std::uint32_t>(m->GetActiveSituation()->GetAdviceId())) : 0u);
}

// The city situation manager as the automata managers' group budget (SL::spCity vslot 0x188, i386 0xc4
// GetCitySituationManager). IsSituationAvailable(group) (i386 0x004a3924): one of the situations advisor's current advices is
// a situation (QI 0x6beb1e8a) whose automata groups (vslot 0x30) hold the group.
bool NativeSituationUsesAutomataGroup(std::uint32_t group) {
    cISC4Advisor* advisor = SituationsAdvisor();
    if (!advisor) return false;
    for (const auto& entry : advisor->GetCurrentAdvices()) {
        cISC4Advice* advice = entry.first.get();
        cISC4CitySituationAdvice* situation = nullptr;
        if (!advice || !advice->QueryInterface(kGZIID_cISC4CitySituationAdvice, reinterpret_cast<void**>(&situation)) || !situation) continue;
        const std::vector<std::uint32_t>& groups = situation->GetAutomataGroups();
        const bool found = std::find(groups.begin(), groups.end(), group) != groups.end();
        situation->Release();
        if (found) return true;
    }
    return false;
}
std::uint32_t NativeSituationTrackedAutomataCount() {   // vslot 0x70 GetTrackedAutomataCount
    sc4::cSC4CitySituationManager* manager = HostCitySituations();
    return manager ? manager->GetTrackedAutomataCount() : 0;
}
std::uint8_t NativeSituationMaxCSI() {   // vslot 0x6c GetMaxCSI
    sc4::cSC4CitySituationManager* manager = HostCitySituations();
    return manager ? manager->GetMaxCSI() : 0;
}
void NativeSituationResetTutorials() {   // vslot 0x84 ResetTutorials
    if (sc4::cSC4CitySituationManager* manager = HostCitySituations()) manager->ResetTutorials();
}
bool NativeHasCitySituations() { return HostCitySituations() != nullptr; }

}  // namespace native

namespace native {
namespace {
bool g_acceptPending = false;
std::string g_acceptShot;
}  // namespace

// The kit's `situations initiate GUID`: what the "cISC4CitySituationManager <guid>" cheat posts (0xac1045ef with the
// guid, 1 = for the automaton's occupant, 0 = the advice's own groups); `situations accept [PNG]`: the next situation
// dialog is accepted through the manager's window procedure (button 6) while it runs (after its picture is written).
void NativeSituationsCommand(const std::string& line) {
    std::istringstream words(line);
    std::string verb;
    words >> verb;
    if (verb.empty()) {
        NativeSituationsReport();
        return;
    }
    if (verb == "initiate") {
        std::string guidText;
        words >> guidText;
        const std::uint32_t guid = static_cast<std::uint32_t>(std::strtoul(guidText.c_str(), nullptr, 16));
        if (!SL::spMS2 || guid == 0) {
            std::printf("situations initiate: no message server or guid\n");
            return;
        }
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kSituationMsgActivateAdvice);
        message->SetData1(guid);
        message->SetData2(1);
        message->SetData3(0);
        SL::spMS2->MessagePost(message, false);
        message->Release();
        std::printf("situations initiate: posted 0xac1045ef guid 0x%08x\n", guid);
        return;
    }
    if (verb == "accept") {
        g_acceptPending = true;
        g_acceptShot.clear();
        words >> g_acceptShot;
        std::printf("situations accept: armed\n");
        return;
    }
    std::printf("situations: unknown verb %s\n", verb.c_str());
}

// The modal loop's per-frame hook (game_main_linux's modal pump): true when a situation dialog is open and an accept is
// armed; the caller writes the picture first, then calls NativeSituationsAccept.
bool NativeSituationsAcceptDue(std::string& shot) {
    cSC4CitySituationManager* m = Manager();
    if (!g_acceptPending || !m || !m->PopupDialog()) return false;
    shot = g_acceptShot;
    return true;
}
void NativeSituationsAccept() {
    cSC4CitySituationManager* m = Manager();
    g_acceptPending = false;
    if (!m || !m->PopupDialog()) return;
    cGZMessage message;
    message.type = kGZMsgCommand;
    message.param1 = kSituationButtonSender;
    message.param2 = kSituationButtonAccept;
    const bool handled = m->DoWinProcMessage(m->PopupDialog(), message);
    std::printf("situations accept: DoWinProcMessage(button 6) -> %d, status %u\n", handled ? 1 : 0, m->GetCurrentStatus());
}

}  // namespace native
