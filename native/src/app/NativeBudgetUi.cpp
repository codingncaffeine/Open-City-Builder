#include "app/NativeBudgetUi.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "app/NativeBoot.h"
#include "app/NativeUi.h"
#include "app/NativeZoneTool.h"
#include "gz/WinInterfaces.h"
#include "sc4/App.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/BudgetUIWinProc.h"
#include "gz/Buffer.h"
#include "gz/WinBMP.h"
#include "gz/WinBtn.h"
#include "gz/WinFont.h"
#include "gz/WinGen.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "gz/GZCOMDirector.h"
#include "gz/WinCtrlFactory.h"
#include "sc4/Ordinance.h"
#include "sc4/SubBudgetWindow.h"
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
    sc4::ISubBudgetWindow* CreateSubBudgetWindow() override { return new sc4::cSC4SubBudgetWindow(); }   // the department dialogs
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
    cIGZWin* BalanceSheetWindow() override { return proc_->Window() ? proc_->Window()->AsIGZWin() : nullptr; }
    void Update() { proc_->Update(); }

private:
    sc4::cSC4BudgetUIWinProc* proc_;
    std::uint32_t refs_ = 0;
};

BudgetWindow* g_window = nullptr;

// The department budget dialog's services (sc4::ISubBudgetWindowHost): the controls the original made through RZ::WCM()
// (cGZWinCtrlMgr::CreateLabel / CreateSlider / CreateButton, Mac i386 0x0016a49a / 0x0016a8be / 0x0016a18c), made here
// directly on the window classes with the same settings.
class SubBudgetHost final : public sc4::ISubBudgetWindowHost {
public:
    sc4::cSC4BudgetSimulator* BudgetSimulator() override {
        return Component<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator, sc4::kGZCLSID_cSC4BudgetSimulator);
    }
    sc4::cSC4OrdinanceSimulator* OrdinanceSimulator() override {
        return Component<sc4::cSC4OrdinanceSimulator>(sc4::CityComponent::OrdinanceSimulator, sc4::kGZCLSID_cSC4OrdinanceSimulator);
    }
    cIGZWinMgr* WindowManager() override { return g_view ? g_view->GetWindowManager() : nullptr; }
    gz::cIGZWinGen* CreateWinGen() override {
        void* raw = nullptr;
        return GZCOM() && GZCOM()->CreateInstance(gz::kGZCLSID_cGZWinGen, gz::kGZIID_cIGZWinGen, &raw) && raw ? static_cast<gz::cIGZWinGen*>(raw) : nullptr;
    }
    cIGZBuffer* UiImage(std::uint32_t instance) override {
        auto cached = images_.find(instance);
        if (cached != images_.end()) return cached->second;
        cIGZBuffer* image = LoadBitmapResource(0x46a006b0, instance);   // the table keeps the reference
        images_[instance] = image;
        return image;
    }
    gz::IWinFont* Font(std::uint32_t styleId) override {
        gz::IWinFontSource* source = gz::WinFontSource();
        return source ? source->Font(styleId) : nullptr;
    }
    std::uint32_t MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t{r} << 16) | (std::uint32_t{g} << 8) | b;
    }
    gz::cIGZWinText* CreateLabel(std::uint32_t id, const cIGZString& caption) override {
        cIGZWinMgr* manager = WindowManager();
        if (!manager) return nullptr;
        auto* label = new gz::cGZWinText(manager);
        auto* view = static_cast<gz::cIGZWinText*>(label);
        view->AddRef();
        cIGZWin* win = view->AsIGZWin();
        win->SetID(id);
        win->SetCaption(caption);
        if (gz::IWinFont* font = Font(gz::kWinDefaultFontStyle)) view->SetFont(font);
        view->SetAlignment(1);
        view->SetBackgroundOpaque(false);   // text vslot 0x40
        view->SetWinTextFlag(2, false);     // text vslot 0x1c
        view->FitWindowToText(false, false);
        return view;
    }
    gz::cIGZWinSlider* CreateSlider(std::uint32_t id, int direction, int minimum, int maximum, int value) override {
        cIGZWinMgr* manager = WindowManager();
        if (!manager) return nullptr;
        auto* slider = new gz::cGZWinSlider(manager);
        auto* view = static_cast<gz::cIGZWinSlider*>(slider);
        view->AddRef();
        view->AsIGZWin()->SetID(id);
        view->SetDirection(static_cast<gz::tSliderWindowDirection>(direction));
        if (gz::IWinCtrlFactory* factory = gz::WinCtrlFactory())
            view->SetImage(factory->Image(direction == 1 ? 0x88963ec1 : 0xa8963ec4), nullptr);
        view->SetMinimumValue(minimum);
        view->SetMaximumValue(maximum);
        view->SetValue(value);
        return view;
    }
    gz::cIGZWinBtn* CreateButton(std::uint32_t style, std::uint32_t id, const cIGZString& caption, cIGZBuffer* image) override {
        cIGZWinMgr* manager = WindowManager();
        if (!manager) return nullptr;
        auto* button = new gz::cGZWinBtn(manager);
        auto* view = static_cast<gz::cIGZWinBtn*>(button);
        view->AddRef();
        view->AsIGZWin()->SetID(id);
        view->AsIGZWin()->SetCaption(caption);
        if (!image) if (gz::IWinCtrlFactory* factory = gz::WinCtrlFactory()) image = factory->Image(0xe8963ef1);
        view->SetStyle(static_cast<int>(style), image, nullptr);
        if (image) view->AsIGZWin()->SetSize(static_cast<int>(image->Width()) / 4, static_cast<int>(image->Height()));
        return view;
    }
    cIGZWin* CreateBitmap(cIGZBuffer* image, int x, int y) override {
        cIGZWinMgr* manager = WindowManager();
        if (!manager || !image) return nullptr;
        auto* bitmap = new gz::cGZWinBMP(manager);
        cIGZWin* win = bitmap;
        win->AddRef();
        win->SetArea(x, y, x + static_cast<int>(image->Width()), y + static_cast<int>(image->Height()));
        static_cast<gz::cIGZWinBMP*>(bitmap)->SetBMP(image);
        return win;
    }

private:
    std::map<std::uint32_t, cIGZBuffer*> images_;
};

}  // namespace

void SetBudgetViewWindow(cIGZWin* view) { g_view = view; }

void InstallLanguageFormatter() {
    static EnglishFormatter formatter;
    sc4::SetLanguageFormatter(&formatter);
}

sc4::IBudgetUIWindow* CreateBudgetUiWindow() {
    static BudgetHost host;
    static SubBudgetHost subHost;
    InstallLanguageFormatter();
    sc4::SetBudgetUIWinProcHost(&host);
    sc4::SetSubBudgetWindowHost(&subHost);
    g_window = new BudgetWindow(new sc4::cSC4BudgetUIWinProc());
    g_window->AddRef();
    return g_window;
}

bool BudgetUiOpen(cIGZUnknown* window) { return window && window == static_cast<cIGZUnknown*>(g_window) && g_window->IsVisible(); }
void BudgetUiRefresh(cIGZUnknown* window) { if (window && window == static_cast<cIGZUnknown*>(g_window)) g_window->Update(); }

}  // namespace native
