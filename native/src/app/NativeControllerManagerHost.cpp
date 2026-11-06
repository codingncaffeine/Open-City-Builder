// IControllerManagerHost on the native application (was the generated neutral stub, whose null clock and script system crashed
// cSC4AutomataControllerManager::Init once the city started initialising its components). The answers come from the loaded city's
// components (SL::spCity vslots 0x2b0 / 0x180 / 0x158 / 0x160), the message server, the animation tick manager and the city renderer's
// view utilities.
#include <cstdio>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeZoneDeveloperHost.h"
#include "platform/Unwired.h"
#include "rz/cRZRandom.h"
#include "sc4/AutomataAttractor.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"

namespace native {
sc4::cSC4AutomataController* NativeCreateAutomataGenerator();   // app/NativeAutomataGenerator.cpp
void InstallAutomataGenerator();
namespace {
using namespace sc4;

template <class T> T* CityComponentAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}

// The controller manager's view of the city's 24-hour clock (cISC424HourClock vslots 0x50 / 0x68: GetHour / GetMinute).
class ClockView final : public IControllerClock {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ > 0 ? --refs_ : 0; }   // static: never deleted
    int Hour() override { cSC424HourClock* c = CityComponentAs<cSC424HourClock>(CityComponent::Clock24Hour); return c ? static_cast<int>(c->GetHour()) : 0; }
    int Minute() override { cSC424HourClock* c = CityComponentAs<cSC424HourClock>(CityComponent::Clock24Hour); return c ? static_cast<int>(c->GetMinute()) : 0; }
private:
    std::uint32_t refs_ = 1;
};

// A script system for a city that carries none (never deleted, answers nothing): the manager AddRefs whatever it gets.
class NoScripts final : public IControllerScriptSystem {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }
    std::uint32_t Release() override { return 1; }
    const std::list<IOccupantGroupTemplate*>& OccupantGroupTemplates() override { return none_; }
    IOccupantGroupTemplate* FindGroupTemplate(const cIGZString&) override { return nullptr; }
    IOccupantGroupTemplate* FindGroupTemplate(std::uint32_t) override { return nullptr; }
    tControllerTemplate* FindControllerTemplate(const cIGZString&) override { return nullptr; }
private:
    std::list<IOccupantGroupTemplate*> none_;
};

struct NativeControllerManagerHost final : IControllerManagerHost {
    IControllerClock* AcquireClock() override { static ClockView clock; return &clock; }
    IControllerScriptSystem* AcquireScriptSystem() override {
        if (auto* scripts = CityComponentAs<cSC4AutomataScriptSystem>(CityComponent::AutomataScriptSystem)) return scripts;
        SC4_UNWIRED("IControllerManagerHost::AcquireScriptSystem (the city has no automata script system)");
        static NoScripts none;
        return &none;
    }
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    // SL::ATM() vslots 0x28 / 0x30 with the channel as the priority
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override { AnimationTickServerView()->AddNotification(target, static_cast<std::uint32_t>(channel)); }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override { AnimationTickServerView()->RemoveNotification(target, static_cast<std::uint32_t>(channel)); }
    bool CheatManagerAvailable() override { return false; }   // the cheat code manager is not recovered
    bool DebugFunctionalityEnabled() override { return false; }
    void RegisterCheat(std::uint32_t, const char*) override {}
    void UnregisterCheat(std::uint32_t) override {}
    void AddCheatListener(cIGZMessageTarget*) override {}
    void RemoveCheatListener(cIGZMessageTarget*) override {}
    // the vehicle or the pedestrian layer is active (city vslots 0x158 / 0x160, cSC4AutomataManager::IsActive vslot 0x30)
    bool AutomataLayersRunning() override { return CityAutomataLayersActive(); }
    const float* ViewPosition() override { return CityViewEye(); }   // SL::spViewUtilities + 0x44
    int ViewZoom() override { return CityViewZoom(); }               // SL::spViewUtilities + 0x14
    // GZCOM()->CreateInstance(classKey, 0x6a1857f8)
    cSC4AutomataController* CreateController(std::uint32_t classKey) override {
        if (classKey == kControllerTypeAttractor) {
            auto* attractor = new cSC4AutomataAttractor();
            attractor->AddRef();
            return attractor;
        }
        if (classKey == kControllerTypeGenerator) return NativeCreateAutomataGenerator();   // app/NativeAutomataGenerator.cpp
        SC4_UNWIRED("IControllerManagerHost::CreateController (an unknown controller class)");
        return nullptr;
    }
    // the city's lot manager (city vslot 0x108 -> vslot 0x70): the lot the occupant stands on; the answer is only compared
    // for identity (never two controllers of one template on one lot)
    const void* LotOf(cISC4Occupant* occupant) override {
        if (!occupant) return nullptr;
        float p[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(p);
        return NativeLotAtCell(static_cast<int>(p[0] / 16.0f), static_cast<int>(p[2] / 16.0f), false);
    }
    double RandomDouble() override { cRZRandom random(0xffffffffu); return random.RandomDoubleUniform(); }   // a fresh clock-seeded generator per call
};

}  // namespace

void InstallControllerManagerStub() {
    static NativeControllerManagerHost s;
    sc4::SetControllerManagerHost(&s);
    InstallAutomataGenerator();   // saved generators load as real controllers
}

}  // namespace native
