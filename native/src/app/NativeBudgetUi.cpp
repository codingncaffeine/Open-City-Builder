#include "app/NativeBudgetUi.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "app/NativeBoot.h"
#include "app/NativeUi.h"
#include "app/NativeZoneTool.h"
#include "gz/WinInterfaces.h"
#include "sc4/App.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BudgetUIWinProc.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SC4Utilities.h"
#include "sc4/Simulator.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {

cIGZWin* g_view = nullptr;

sc4::cSC4City* LoadedCity() {
    cSC4App* app = BootApplication();
    return app ? dynamic_cast<sc4::cSC4City*>(app->GetCity()) : nullptr;
}

template <class T> T* Component(sc4::CityComponent c, std::uint32_t clsid) {
    sc4::cSC4City* city = LoadedCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}

class BudgetHost final : public sc4::IBudgetUIWinProcHost {
public:
    cIGZWin* ViewWindow() override { return g_view; }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        cIGZWin* window = native::CreateWindowFromScript(key, parent, id);
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out);
        window->Release();
        return ok;
    }
    cIGZWinMgr* WindowManager() override { return g_view ? g_view->GetWindowManager() : nullptr; }
    bool WindowManagerIsModal() override { return false; }
    sc4::cSC4BudgetSimulator* Budget() override { return Component<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator, sc4::kGZCLSID_cSC4BudgetSimulator); }
    sc4::IOrdinanceBudgetApi* Ordinances() override { return nullptr; }
    sc4::INeighborsBudgetApi* Neighbors() override { return nullptr; }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void AddTickNotification(cIGZMessageTarget2* target, std::uint32_t channel) override { StartToolTick(target, channel); }
    void RemoveTickNotification(cIGZMessageTarget2* target) override { StopToolTick(target); }
    void HiddenPause() override { if (auto* s = Component<sc4::cSC4Simulator>(sc4::CityComponent::Simulator, sc4::kGZCLSID_cSC4Simulator)) s->HiddenPause(); }
    void HiddenResume() override { if (auto* s = Component<sc4::cSC4Simulator>(sc4::CityComponent::Simulator, sc4::kGZCLSID_cSC4Simulator)) s->HiddenResume(); }
    bool SaveWindowLocation(cIGZWin*) override { return false; }
    bool RestoreWindowLocation(cIGZWin*, bool) override { return false; }
    void DecimalSeparator(cIGZString& out) override { out.AssignFromCString("."); }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b; }
    bool FilterUICommandMessage(cGZMessage&) override { return true; }
    sc4::ISubBudgetWindow* CreateSubBudgetWindow() override { return nullptr; }   // the department windows are not recovered
};

std::string Group(std::int64_t value) {
    std::string digits = std::to_string(value < 0 ? -value : value), out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

// RZ::LM() -> GetLanguageUtility(0): the English money / number format.
class EnglishFormatter final : public sc4::ILanguageFormatter {
public:
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString& symbol) override {
        out.AssignFromCString(((value < 0 ? "-" : "") + std::string(symbol.ToChar()) + Group(value)).c_str());
    }
    void FormatMoney(double value, cIGZString& out, const cIGZString& format, const cRZString& symbol) override {
        char buffer[64];
        const char* f = format.ToChar();
        std::snprintf(buffer, sizeof buffer, (f && std::strchr(f, '%')) ? f : "%.0f", value < 0 ? -value : value);
        out.AssignFromCString(((value < 0 ? "-" : "") + std::string(symbol.ToChar()) + buffer).c_str());
    }
    void FormatNumber(std::int64_t value, cRZString& out) override { out = cRZString(((value < 0 ? "-" : "") + Group(value)).c_str()); }
};

class BudgetWindow final : public sc4::IBudgetUIWindow {
public:
    explicit BudgetWindow(sc4::cSC4BudgetUIWinProc* proc) : proc_(proc) { proc_->AddRef(); }
    ~BudgetWindow() { proc_->Shutdown(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return proc_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    void Init() override { proc_->Init(); }
    void ShowWindow() override { proc_->ShowWindow(); }
    void HideWindow() override { proc_->HideWindow(); }
    bool IsVisible() override { return proc_->IsVisible(); }
    void Update() { proc_->Update(); }

private:
    sc4::cSC4BudgetUIWinProc* proc_;
    std::uint32_t refs_ = 0;
};

BudgetWindow* g_window = nullptr;

}  // namespace

void SetBudgetViewWindow(cIGZWin* view) { g_view = view; }

void InstallLanguageFormatter() {
    static EnglishFormatter formatter;
    sc4::SetLanguageFormatter(&formatter);
}

sc4::IBudgetUIWindow* CreateBudgetUiWindow() {
    static BudgetHost host;
    InstallLanguageFormatter();
    sc4::SetBudgetUIWinProcHost(&host);
    g_window = new BudgetWindow(new sc4::cSC4BudgetUIWinProc());
    g_window->AddRef();
    return g_window;
}

bool BudgetUiOpen(cIGZUnknown* window) { return window && window == static_cast<cIGZUnknown*>(g_window) && g_window->IsVisible(); }
void BudgetUiRefresh(cIGZUnknown* window) { if (window && window == static_cast<cIGZUnknown*>(g_window)) g_window->Update(); }

}  // namespace native
