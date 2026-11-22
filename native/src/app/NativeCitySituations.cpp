// The city situation manager's services on the native application (sc4::ICitySituationManagerHost): the advisor system's
// script state, the message server, the application's debug flag and completed-mission preferences, the cheat code manager,
// the city view, the game-data registry, the animation tick manager (SL::ATM(): the manager's DoAnimationTick), the window
// manager, the HTML link handler and the "city situations" advisor (id 0x1d). Also the kit's read-only `situations` verb.
// The automata, occupants and their managers come through the views of app/NativeSituationAutomata.cpp.
//   PARTIAL: the city geometry, lot / zone managers, terrain, traffic network, the landfill developer, the effects manager
//   and the MySim answer null; cSC4DispatchVehicleView (the situation indicators) is not recovered (NativeDispatch.cpp
//   CreateDispatchVehicleView), so DispatchView answers null as the dispatch manager does.
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
#include "gz/CheatCodeManager.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/AutoHelpers.h"
#include "sc4/AuraSimulator.h"
#include "sc4/BudgetSimulator.h"
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
#include "app/NativeDemolition.h"   // CityNeighborsSimulator
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"

namespace native {
sc4::ISituationLua* NativeSituationLua();   // NativeCitySituationAdvice.cpp
// app/NativeSituationAutomata.cpp
sc4::ISituationOccupant* SituationOccupantOf(void* engineOccupant);
sc4::ISituationAutomaton* SituationAutomatonOf(void* engineAutomaton);
int SituationAutomataManagerCount();
sc4::ISituationAutomataManager* SituationAutomataManager(int index);
sc4::ISituationOccupantManager* SituationOccupantManager();
cIGZUnknown* SituationEngineOccupant(sc4::ISituationOccupant* occupant);
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

class CitySituationManagerHost final : public ICitySituationManagerHost {
public:
    ISituationLua* Lua() override { return NativeSituationLua(); }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    ISituationApp* App() override { return SL::spSC4 ? &g_app : nullptr; }
    ISituationCheatCodes* CheatCodes() override { return GameCheatCodeManager() ? &g_cheats : nullptr; }
    ISituationView3D* View3D() override { return &g_view; }
    ISituationDispatchView* DispatchView() override {
        SC4_UNWIRED("ICitySituationManagerHost::DispatchView: cSC4DispatchVehicleView (the situation indicators) is not recovered");
        return nullptr;
    }
    ISituationCity* City() override {   // PARTIAL: the city geometry seam (escape / coverage missions, landfill targets)
        SC4_UNWIRED("ICitySituationManagerHost::City: the city geometry seam (escape / coverage missions, landfill targets) is not wired");
        return nullptr;
    }
    ISituationLotManager* LotManager() override {
        SC4_UNWIRED("ICitySituationManagerHost::LotManager: the lot manager is not handed to the situations");
        return nullptr;
    }
    ISituationEffectsManager* EffectsManager() override {
        SC4_UNWIRED("ICitySituationManagerHost::EffectsManager: the effects manager is not handed to the situations");
        return nullptr;
    }
    int AutomataManagerCount() override { return SituationAutomataManagerCount(); }
    ISituationAutomataManager* AutomataManager(int index) override { return SituationAutomataManager(index); }
    ISituationAutomataManager* VehicleManager() override { return SituationAutomataManager(0); }
    ISituationDriveView* PlayerDriveView() override {
        SC4_UNWIRED("ICitySituationManagerHost::PlayerDriveView: the player-drive view is not handed to the situations");
        return nullptr;
    }
    ISituationMissionActor* MySimActor() override {
        SC4_UNWIRED("ICitySituationManagerHost::MySimActor: the MySim is not handed to the situations");
        return nullptr;
    }
    ISituationOccupantManager* OccupantManager() override { return SituationOccupantManager(); }
    ISituationZoneManager* ZoneManager() override {
        SC4_UNWIRED("ICitySituationManagerHost::ZoneManager: the zone manager is not handed to the situations");
        return nullptr;
    }
    ISituationTerrain* Terrain() override {
        SC4_UNWIRED("ICitySituationManagerHost::Terrain: the terrain is not handed to the situations");
        return nullptr;
    }
    ISituationTrafficNetwork* TrafficNetwork() override {
        SC4_UNWIRED("ICitySituationManagerHost::TrafficNetwork: the traffic network is not handed to the situations");
        return nullptr;
    }
    ISituationAuraSim* AuraSim() override { return SituationAura::Aura() ? &g_aura : nullptr; }
    ISituationBudgetSim* BudgetSim() override { return SituationBudget::Budget() ? &g_budget : nullptr; }
    ISituationLandfillDeveloper* LandfillDeveloper() override {
        SC4_UNWIRED("ICitySituationManagerHost::LandfillDeveloper: the landfill developer is not handed to the situations");
        return nullptr;
    }
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
} g_host;

// The city component (a COM wrapper whose plain-object view is the manager, or the manager itself).
cSC4CitySituationManager* Manager() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::CitySituationManager) : nullptr;
    if (!object) return nullptr;
    if (auto* manager = dynamic_cast<cSC4CitySituationManager*>(object)) return manager;
    void* raw = nullptr;
    if (!object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<cSC4CitySituationManager*>(raw);
}

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
