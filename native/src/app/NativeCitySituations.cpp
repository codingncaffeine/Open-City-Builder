// The city situation manager's services on the native application (sc4::ICitySituationManagerHost): the advisor system's
// script state, the message server, the application's debug flag and completed-mission preferences, the cheat code manager,
// the city view, the game-data registry, the animation tick manager (SL::ATM(): the manager's DoAnimationTick), the window
// manager, the HTML link handler and the "city situations" advisor (id 0x1d). Also the kit's read-only `situations` verb.
//   PARTIAL: the automata side (the four cISC4AutomataManager views, the vehicle manager, the occupant / lot / zone managers,
//   terrain, traffic network, aura and budget simulators, the landfill developer, the MySim and EnterPlayerDriveView) answers
//   null until its adapters over the recovered automata exist; cSC4DispatchVehicleView (the situation indicators) is not
//   recovered (NativeDispatch.cpp CreateDispatchVehicleView), so DispatchView answers null as the dispatch manager does.
#include <cstdio>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCheats.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeGameData.h"
#include "app/NativeWinMgr.h"
#include "gz/CheatCodeManager.h"
#include "rz/cRZString.h"
#include "sc4/Advisor.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/AppPreferences.h"
#include "sc4/CommonIIDs.h"
#include "sc4/CitySituationManager.h"
#include "sc4/ISC4App.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"

namespace native {
sc4::ISituationLua* NativeSituationLua();   // NativeCitySituationAdvice.cpp

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

// SL::spView3D vslot 0xe4 (i386; 0x1c8 in the 64-bit build): HideMissionStatusPanel.
class SituationView final : public ISituationView3D {
public:
    void CancelSituationPopup() override { if (cSC4View3DWin* v = CityView3DWin()) v->HideMissionStatusPanel(); }
} g_view;

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
    ISituationDispatchView* DispatchView() override { return nullptr; }
    ISituationCity* City() override { return nullptr; }
    ISituationLotManager* LotManager() override { return nullptr; }
    ISituationEffectsManager* EffectsManager() override { return nullptr; }
    int AutomataManagerCount() override { return 0; }
    ISituationAutomataManager* AutomataManager(int) override { return nullptr; }
    ISituationAutomataManager* VehicleManager() override { return nullptr; }
    ISituationDriveView* PlayerDriveView() override { return nullptr; }
    ISituationMissionActor* MySimActor() override { return nullptr; }
    ISituationOccupantManager* OccupantManager() override { return nullptr; }
    ISituationZoneManager* ZoneManager() override { return nullptr; }
    ISituationTerrain* Terrain() override { return nullptr; }
    ISituationTrafficNetwork* TrafficNetwork() override { return nullptr; }
    ISituationAuraSim* AuraSim() override { return nullptr; }
    ISituationBudgetSim* BudgetSim() override { return nullptr; }
    ISituationLandfillDeveloper* LandfillDeveloper() override { return nullptr; }
    void EnterPlayerDriveView(ISituationAutomaton*, cIGZMessageTarget2*) override {}
    cIGZWinMgr* WindowManager() override { return ActiveWinMgr(); }
    void ReleaseWindowParamObject(cIGZWin*, std::uint32_t) override {}
    cIGZWin* WindowParamWindow(cIGZWin*, std::uint32_t) override { return nullptr; }
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
    // PARTIAL: no situation views of the engine's occupants / automata yet (the automata adapters).
    ISituationOccupant* OccupantOfMessage(void*) override { return nullptr; }
    ISituationAutomaton* AutomatonOfMessage(void*) override { return nullptr; }
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
        std::printf("  active 0x%08x condition %u flags 0x%x\n", advice ? static_cast<std::uint32_t>(advice->GetGUID()) : 0u,
                    active->GetSuccessCondition(), active->GetSituationFlags());
    }
}

}  // namespace native
