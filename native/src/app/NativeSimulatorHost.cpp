// ISimulatorHost on the native application (was the generated neutral stub, which left the simulator without a message server and
// never ticked it: cSC4Simulator::Init sets the hidden pause and waits for "city started" on SL::spMS2, and the framework's AddToTick
// is what runs cSC4Simulator::OnTick, the loop that advances the date and calls every simulator agent).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeBoot.h"
#include "gz/GZFramework.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZSystemService.h"
#include "platform/Unwired.h"
#include "sc4/Exemplar.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "rz/cRZString.h"
#include "sc4/cSC4City.h"
#include "app/NativeCity.h"

namespace native {
namespace {
using namespace sc4;

std::vector<cIGZSystemService*> g_ticked;   // the services the simulator host added to the tick, in order

struct NativeSimulatorHost final : ISimulatorHost {
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    // cRZFramework::AddSystemService + AddToTick (cSC4Simulator::Init). The framework keeps the service registry; the tick is driven by
    // TickSimulatorServices from the frame loop, because the native loop pumps the framework's other services itself.
    void RegisterService(cIGZSystemService* service) override {
        if (!service || std::find(g_ticked.begin(), g_ticked.end(), service) != g_ticked.end()) return;
        if (cGZFramework* framework = BootFramework()) framework->AddSystemService(service);
        service->AddRef();
        g_ticked.push_back(service);
    }
    void UnregisterService(cIGZSystemService* service) override {
        auto it = std::find(g_ticked.begin(), g_ticked.end(), service);
        if (it == g_ticked.end()) return;
        g_ticked.erase(it);
        service->Release();
    }
    // The cheat code manager and the command server are not recovered: the simulator's "FPS" / "SimDate" cheats and the
    // GetSimulationDate command are not reachable.
    void RegisterCheat(std::uint32_t, const char*) override { SC4_UNWIRED("ISimulatorHost::RegisterCheat (cheat code manager)"); }
    void UnregisterCheat(std::uint32_t) override {}
    void AddCheatNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RemoveCheatNotification(cIGZMessageTarget2*, std::uint32_t) override {}
    void RegisterCommand(std::uint32_t, const char*) override { SC4_UNWIRED("ISimulatorHost::RegisterCommand (command server)"); }
    void UnregisterCommand(std::uint32_t) override {}
    bool DebugFunctionalityEnabled() override { return false; }   // the retail build's SL::spSC4 vslot 0xa8
    bool IsDrivingMode() override { return false; }               // no U-Drive-It: the vehicle manager never enters driving mode natively
    cISCPropertyHolder* TuningExemplar() override {
        if (!tuning_ && SL::spRM) {
            void* resource = nullptr;
            if (SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0xe7e2c2db, 0xe9590e52}, 0xa52160f5, &resource, 0, nullptr) && resource)
                tuning_ = static_cast<cISCResExemplar*>(resource)->AsISCPropertyHolder();
        }
        return tuning_;
    }

private:
    cISCPropertyHolder* tuning_ = nullptr;
};

}  // namespace

void InstallSimulatorStub() { static NativeSimulatorHost s; sc4::SetSimulatorHost(&s); }

namespace {
// The city's simulator component through its cISC4Simulator interface (iid 0x8695664e).
cSC4Simulator* CitySimulator() {
    cSC4City* city = HostCity();
    cIGZUnknown* component = city ? city->GetComponent(CityComponent::Simulator) : nullptr;
    void* iface = nullptr;
    if (!component || !component->QueryInterface(0x8695664e, &iface) || !iface) return nullptr;
    auto* simulator = static_cast<cSC4Simulator*>(static_cast<cISC4Simulator*>(iface));
    simulator->Release();   // the city keeps it alive
    return simulator;
}
}  // namespace

bool AddCitySimulatorAgent(cIGZMessageTarget2* agent, int interval, const char* name, std::uint32_t flags) {
    cSC4Simulator* simulator = CitySimulator();
    if (!simulator || !agent) return false;
    return simulator->AddAgent(agent, interval, cRZString(name ? name : ""), flags);
}

bool CitySimulatorPresent() { return CitySimulator() != nullptr; }
sc4::cSC4Simulator* CitySimulatorObject() { return CitySimulator(); }

void RemoveCitySimulatorAgent(cIGZMessageTarget2* agent) {
    if (cSC4Simulator* simulator = CitySimulator()) simulator->RemoveAgent(agent);
}

// One framework tick for the services the simulator host registered (cGZFramework::OnTick over its tick list).
void TickSimulatorServices(std::uint32_t tickNumber) {
    const std::vector<cIGZSystemService*> snapshot = g_ticked;
    for (cIGZSystemService* service : snapshot) service->OnTick(tickNumber);
    static const bool trace = std::getenv("SC4_SIMDEBUG") != nullptr;
    if (trace) {
        static std::uint32_t frames = 0;
        if (++frames % 120 == 0) std::fprintf(stderr, "simulator: %zu ticked services\n", snapshot.size());
    }
}

}  // namespace native
