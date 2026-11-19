// The tip of the day dialog's host (sc4/TipOfTheDayWinProc.h): the exemplars of the resource manager, the tip texts, the
// preferences, and the city view's "once per run, when tips exist" opener (cSC4View3DWin::DoPostCityInit).
#include "app/NativeTips.h"

#include <cstdio>
#include <cstdlib>
#include <map>

#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "gz/GZCOM.h"
#include "gz/GZCOMDirector.h"
#include "gz/WinHTML.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "sc4/App.h"
#include "sc4/Exemplar.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/TipOfTheDayWinProc.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_TIPDEBUG") != nullptr;
    return on;
}

class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }

private:
    std::uint32_t type_;
};

// One loaded exemplar for the time of a call (resource manager slot 3, interface 0xa52160f5).
struct Exemplar {
    cISCResExemplar* exemplar = nullptr;
    explicit Exemplar(const cGZPersistResourceKey& key) {
        void* raw = nullptr;
        if (SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw) exemplar = static_cast<cISCResExemplar*>(raw);
    }
    ~Exemplar() { if (exemplar) exemplar->Release(); }
    cISCPropertyHolder* Holder() const { return exemplar ? exemplar->AsISCPropertyHolder() : nullptr; }
};

struct Host final : ITipOfTheDayHost {
    cIGZWin* DialogParent() override { WinMgr* manager = ActiveWinMgr(); return manager ? manager->GetMainWindow() : nullptr; }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid, void** out) override {
        *out = nullptr;
        cIGZWin* window = native::CreateWindowFromScript(key, parent, id);
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
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(0x6534284a);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        int tips = 0;
        for (const cGZPersistResourceKey& key : keys) {
            fn(key);
            ++tips;
        }
        if (Trace()) std::fprintf(stderr, "tips: walked %d exemplars\n", tips);
    }
    bool ExemplarType(const cGZPersistResourceKey& key, std::uint32_t& type) override {
        Exemplar exemplar(key);
        cISCPropertyHolder* holder = exemplar.Holder();
        return holder && cSCPropertyHelp::GetPropertyValue(holder, 0x10, type);
    }
    bool TipText(const cGZPersistResourceKey& key, cRZString& out) override {
        Exemplar exemplar(key);
        cISCPropertyHolder* holder = exemplar.Holder();
        return holder && LoadExemplarString(out, holder, kTipTextProperty);
    }
    std::uint8_t* Preferences() override { return SL::spSC4 ? SL::spSC4->GetPreferences() : nullptr; }
};

}  // namespace

void InstallTipOfTheDayHost() {
    static Host host;
    SetTipOfTheDayHost(&host);
}

void ShowTipOfTheDayOnce() {
    if (cSC4TipOfTheDayWinProc::HasBeenShownAlready()) return;
    const bool present = cSC4TipOfTheDayWinProc::EntriesPresent();
    if (Trace()) std::fprintf(stderr, "tips: %s\n", present ? "present, opening the dialog" : "none in the game data");
    if (present) cSC4TipOfTheDayWinProc::DoModelessWin(nullptr);
}

}  // namespace native
