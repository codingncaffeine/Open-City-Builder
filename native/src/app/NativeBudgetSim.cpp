// The budget simulator's environment and services on the native application (cSC4BudgetSimulator::Init, Mac i386 0x18ff94).
// Before this the simulator had neither: it took no simulator ticks (no monthly budget), heard no messages (an established city
// never got its starting funds) and read no tunables (no starting funds, tax rates or loan terms at all).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "gz/cGZPersistResourceManager.h"
#include "platform/Unwired.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "rz/cRZString.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/Ordinance.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;


// The simulator tuning exemplar the original reads its budget values from (ReadSimulatorTunableValues, 0x18f898).
constexpr cGZPersistResourceKey kBudgetTuningKey{0x6534284a, 0xe7e2c2db, 0x0bff6824};
constexpr std::uint32_t kExemplarIid = 0xa52160f5;
// The simulator agents of the original's Init: every tick (0), the monthly cycle (6) and the daily history (4).
constexpr int kAgentPeriods[] = {0, 6, 4};

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    sc4::cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
sc4::cSC4Simulator* Simulator() { return Component<sc4::cSC4Simulator>(CityComponent::Simulator, sc4::kGZCLSID_cSC4Simulator); }
sc4::cSC4HistoryWarehouse* History() { return Component<sc4::cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse, sc4::kGZCLSID_cSC4HistoryWarehouse); }

// The simulator's cIGZMessageTarget2 view (+4 in the original): the message server and the simulator's agents deliver to it.
class BudgetTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessageTarget2 && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ > 1 ? --refs_ : 1; }   // static: lives as long as the process
    bool DoMessage(cIGZMessage2* message) override {
        if (!simulator || !message) return true;
        sc4::BudgetMessage m;
        m.type = static_cast<std::int32_t>(message->GetType());
        void* standard = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &standard) && standard) {
            auto* s = static_cast<cIGZMessage2Standard*>(standard);
            m.data1 = static_cast<std::int64_t>(s->GetData1());
            cIGZString* text = nullptr;
            if (s->GetString(kGZIID_cIGZString, reinterpret_cast<void**>(&text)) && text) {
                m.text = text->ToChar();
                text->Release();
            }
            s->Release();
        }
        const bool handled = simulator->DoMessage(m);
        if (m.type == 0x26d31ec4 && std::getenv("SC4_DEBUG"))   // the city was established: its starting funds
            std::fprintf(stderr, "budget: city established, funds %lld\n", static_cast<long long>(simulator->GetTotalFunds()));
        return handled;
    }
    cSC4BudgetSimulator* simulator = nullptr;
private:
    std::uint32_t refs_ = 1;
};
BudgetTarget g_target;

struct BudgetServices final : sc4::IBudgetSimServices {
    void RegisterSimulatorNotifications(void* owner) override {
        auto* sim = static_cast<cSC4BudgetSimulator*>(owner);
        g_target.simulator = sim;
        // The original reads the tunables first thing in Init (ReadSimulatorTunableValues): starting funds, tax rates, loans.
        sc4::cISCPropertyHolder* exemplar = nullptr;
        void* resource = nullptr;
        if (SL::spRM && SL::spRM->GetResource(kBudgetTuningKey, kExemplarIid, &resource, 0, nullptr) && resource)
            exemplar = static_cast<cISCResExemplar*>(resource)->AsISCPropertyHolder();
        if (!exemplar) std::fprintf(stderr, "budget: tuning exemplar %08x not found, defaults stand\n", kBudgetTuningKey.instance);
        sim->ReadSimulatorTunableValues(exemplar);
        if (std::getenv("SC4_DEBUG")) {
            std::string funds;
            for (std::int64_t f : sim->GetTunables().startingFunds) funds += " " + std::to_string(f);
            std::fprintf(stderr, "budget: tunables read, starting funds by difficulty:%s\n", funds.c_str());
        }
        for (int period : kAgentPeriods) AddCitySimulatorAgent(&g_target, period, "cSC4BudgetSimulator", 2);
    }
    void UnregisterSimulator(void*) override {
        RemoveCitySimulatorAgent(&g_target);
        g_target.simulator = nullptr;
    }
    void SubscribeMessage(std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(&g_target, message); }
    void UnsubscribeMessage(std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(&g_target, message); }
    void AcquireServices() override {}   // the city, land value, ordinance simulators and MS2 are reached through the city here
    void ReleaseServices() override {}
    void RegisterCheatCodes() override {}   // no cheat code manager natively (see ISimulatorHost::RegisterCheat)
    void UnregisterCheatCodes() override {}
    void RegisterGameDataNodes(cSC4BudgetSimulator*) override {}   // cISC4GameDataRegistry is not recovered (MySim Lua)
    void UnregisterGameDataNodes(cSC4BudgetSimulator*) override {}
    void ResetHistory(std::uint32_t id, std::uint32_t days) override { if (auto* h = History()) h->SetHistoryDataQueueLength(id, days); }
    bool CityCheatsEnabled() override { return false; }
    double RandomDouble() override { static cRZRandom random(0x5eedu); return random.RandomDoubleUniform(); }
    void CityCellSize(float& x, float& z) override {
        sc4::cSC4City* city = HostCity();
        x = city ? static_cast<float>(city->CellCountX()) : 0.0f;
        z = city ? static_cast<float>(city->CellCountZ()) : 0.0f;
    }
    bool SpawnDisasterAt(int, int) override { return false; }
    bool HasDisasterLayer() override { return false; }
    void ShowBudgetDialog(bool) override { SC4_UNWIRED("IBudgetSimServices::ShowBudgetDialog (cSC4BudgetUIExpenseWinProc / IncomeWinProc)"); }
};

struct BudgetEnvironment final : sc4::IBudgetSimEnvironment {
    void SimulatorDate(int& year, int& month) override {
        year = month = 0;
        if (sc4::cSC4Simulator* sim = Simulator()) sim->GetSimDate(&year, &month, nullptr, nullptr, nullptr);
    }
    std::int64_t OrdinanceMonthlyIncome() override {
        auto* o = Component<sc4::cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, sc4::kGZCLSID_cSC4OrdinanceSimulator);
        return o ? o->GetOrdinanceMonthlyIncome() : 0;
    }
    std::int64_t OrdinanceMonthlyExpense() override {
        auto* o = Component<sc4::cSC4OrdinanceSimulator>(CityComponent::OrdinanceSimulator, sc4::kGZCLSID_cSC4OrdinanceSimulator);
        return o ? o->GetOrdinanceMonthlyExpense() : 0;
    }
    std::int64_t NeighborsIncome() override {
        auto* n = Component<sc4::cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, sc4::kGZCLSID_cSC4NeighborsSimulator);
        return n ? n->GetTotalMonthlyNeighborDealIncome() : 0;
    }
    std::int64_t NeighborsExpense() override {
        auto* n = Component<sc4::cSC4NeighborsSimulator>(CityComponent::NeighborsSimulator, sc4::kGZCLSID_cSC4NeighborsSimulator);
        return n ? n->GetTotalMonthlyNeighborDealExpense() : 0;
    }
    int DifficultyLevel() override {
        sc4::cSC4City* city = HostCity();
        return city ? static_cast<int>(city->GetDifficultyLevel()) : 0;
    }
    void ForEachTaxableLot(const std::function<void(const sc4::LotTaxInfo&)>&) override {
        SC4_UNWIRED("IBudgetSimEnvironment::ForEachTaxableLot (lot manager walk: no tax income yet)");
    }
    void PostMessage(std::uint32_t type, std::uint32_t a, std::int64_t b, std::int64_t c) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetData1(a);
        message->SetData2(static_cast<std::uint64_t>(b));
        message->SetData3(static_cast<std::uint64_t>(c));
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void AddHistory(std::uint32_t id, std::int64_t value) override { if (auto* h = History()) h->RecordHistoryData(id, static_cast<int>(value)); }
    void PostBudgetMessage(std::uint32_t type, cSC4DepartmentBudget* department, std::uint32_t id, std::uint32_t extra) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetVoid1(department);
        message->SetData2(id);
        message->SetData3(extra);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    void NotifyLocalFundingChanged(sc4::cISCPropertyHolder*) override {}   // no funded object answers 0x26bb767e natively yet
    std::string LoadLocalizedString(std::uint32_t group, std::uint32_t instance) override {
        cRZString text;
        return native::LoadLocalizedString(group, instance, text) ? std::string(text.ToChar()) : std::string();
    }
    std::string BuildingName(std::uint32_t) override { return {}; }
};

}  // namespace

void InstallBudgetSimHosts() {
    static BudgetServices services;
    static BudgetEnvironment environment;
    cSC4BudgetSimulator::SetServices(&services);
    cSC4BudgetSimulator::SetEnvironment(&environment);
}

}  // namespace native
