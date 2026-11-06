#include "app/NativeCityUi.h"

#include "app/NativeGameData.h"
#include "app/NativeUi.h"
#include "gz/WinInterfaces.h"
#include "sc4/GraphsViewWinProc.h"
#include "sc4/MySimWinProc.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"

namespace native {
namespace {

// The MySim panel's services. The MySim agent simulator, the game-data registry, the vehicle / pedestrian managers and
// the window-location saver are not up in the native city yet, so those answer null and the panel runs without them.
class MySimHost final : public sc4::IMySimWinProcHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    sc4::cISC4GameDataRegistry* GameDataRegistry() override { return NativeGameDataRegistry(); }
    sc4::IMySimAgentSimulator* MySimAgentSimulator() override { return nullptr; }
    sc4::IModelManager* VehicleManager() override { return nullptr; }
    sc4::IModelManager* PedestrianManager() override { return nullptr; }
    void RestoreWindowLocation(cIGZWin*) override {}
    void SaveWindowLocation(cIGZWin*) override {}
    bool CityName(cIGZString&) override { return false; }
    sc4::IPickSimWinProc* CreatePickSimWinProc(std::uint32_t) override { return nullptr; }
    cIGZWinMgr* WindowManager() override { return window_ ? window_->GetWindowManager() : nullptr; }
    bool CreateTransportDialog(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        cIGZWin* window = CreateWindowFromScript(key, parent, id);
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out);
        window->Release();
        return ok;
    }
    bool CreateChangeHouseTool(sc4::cISC4MySim*) override { return false; }
    bool CreateChangeJobTool(sc4::cISC4MySim*) override { return false; }
    void PostMessage2(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, cIGZUnknown*) override {}
    bool ExemplarString(cRZString&, sc4::cISCPropertyHolder*, std::uint32_t) override { return false; }

    cIGZWin* window_ = nullptr;   // any window of the UI, for its window manager
};

// cSC4View3DWin keeps the MySim procedure as IMySimWindow; the recovered class is a plain window procedure, so this
// forwards to it (one reference on the procedure, released with the adapter).
class MySimWindow final : public sc4::IMySimWindow {
public:
    explicit MySimWindow(sc4::cSC4MySimWinProc* proc) : proc_(proc) { proc_->AddRef(); }
    ~MySimWindow() { proc_->Shutdown(); proc_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return proc_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override { return proc_->DoWinProcMessage(window, message); }
    void Init() override { proc_->Init(); }
    void Show() override { proc_->Show(); }
    void Hide() override { proc_->Hide(); }

private:
    sc4::cSC4MySimWinProc* proc_;
    std::uint32_t refs_ = 0;
};

class GraphsViewWindow final : public sc4::IGraphsViewWindow {
public:
    explicit GraphsViewWindow(sc4::cSC4GraphsViewWinProc* proc) : proc_(proc) { proc_->AddRef(); }
    ~GraphsViewWindow() { proc_->Shutdown(); proc_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return proc_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override { return proc_->DoWinProcMessage(window, message); }
    bool Init() override { return proc_->Init(); }
    void ShowWindow() override { proc_->ShowWindow(); }
    void HideWindow() override { proc_->HideWindow(); }
    bool IsVisible() override { return proc_->IsVisible(); }
    void ShowGraphType(std::uint32_t option) override { proc_->ShowGraphType(option); }

private:
    sc4::cSC4GraphsViewWinProc* proc_;
    std::uint32_t refs_ = 0;
};

}  // namespace

sc4::IMySimWindow* CreateMySimWindow(gz::cIGZWinGen* panel) {
    static MySimHost host;
    host.window_ = panel ? panel->AsIGZWin() : nullptr;
    sc4::SetMySimWinProcHost(&host);
    auto* window = new MySimWindow(new sc4::cSC4MySimWinProc(panel));
    window->AddRef();
    return window;
}

sc4::IGraphsViewWindow* CreateGraphsViewWindow(cIGZWin* graphs) {
    auto* window = new GraphsViewWindow(new sc4::cSC4GraphsViewWinProc(graphs));
    window->AddRef();
    return window;
}

}  // namespace native
