// The "establish city" dialog: cSC4View3DWin::DoEstablishCityDialog (Mac i386 0x52188c) and the host of its window
// procedure (cSC4EstablishCityWinProc). A new city opens unestablished, in the god-mode panel with the terraform tools;
// the mayor-mode button asks for the city and mayor names and the difficulty, and an OK establishes the city.
#include <cstdint>
#include <cstring>

#include "app/NativeBoot.h"
#include "app/NativeCity.h"
#include "app/NativeUi.h"
#include "app/NativeWinMgr.h"
#include "gz/WinGen.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZString.h"
#include "sc4/App.h"
#include "sc4/AppPreferences.h"
#include "sc4/EstablishCityWinProc.h"
#include "sc4/UIFilters.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {

constexpr std::uint32_t kDialogScript = 0x2a41436b, kDialogWindowId = 0x6a414973, kUIScriptGroup = 0x96a006b0;
constexpr std::uint32_t kCityNameEdit = 0xaa4149bf, kMayorNameEdit = 0xaa4149d2;
constexpr std::uint32_t kNameMaxLength = 0x80;    // the edit boxes take 128 characters
constexpr std::uint32_t kEditTabFlag = 0x800;     // cGZWinTextEdit flag set on both boxes (vslot 0x130)
constexpr std::size_t kNameBufferSize = 0x200;    // SC4LimitStringLength(0x1ff) + the terminator

std::uint8_t* Preferences() {
    cSC4App* app = BootApplication();
    return app ? app->GetPreferences() : nullptr;
}

std::uint32_t PreferredDifficulty() {
    std::uint8_t* prefs = Preferences();
    std::uint32_t level = 0;
    if (prefs) std::memcpy(&level, prefs + sc4::prefs::kDifficultyLevel, sizeof level);
    return level < 3 ? level : 0;
}

struct EstablishCityHost final : sc4::IEstablishCityHost {
    int DifficultyLevel() override {
        sc4::cSC4City* city = HostCity();
        return city ? static_cast<int>(city->GetDifficultyLevel()) : 0;
    }
    cIGZWin* FocusedWindow() override { return ActiveWinMgr() ? ActiveWinMgr()->GZGetFocus() : nullptr; }
    void SetFocus(cIGZWin* window) override { if (ActiveWinMgr()) ActiveWinMgr()->GZSetFocus(window); }
};

// The edit box `id` of the dialog: 128 characters, the tab filter routing Tab to the dialog.
gz::cIGZWinTextEdit* PrepareNameEdit(cIGZWin* dialog, std::uint32_t id) {
    gz::cIGZWinTextEdit* edit = nullptr;
    if (!dialog->GetChildAsRecursive(id, gz::kGZIID_cIGZWinTextEdit, reinterpret_cast<void**>(&edit)) || !edit) return nullptr;
    edit->SetFlag(kEditTabFlag);
    auto* filter = new sc4::cSC4CtabFilter(dialog);
    filter->AddRef();
    edit->AsIGZWin()->AddMessageFilter(filter);
    edit->SetMaximumTextToAllow(kNameMaxLength);
    filter->Release();
    return edit;
}

// The text of the edit box `id`, cut to 0x1ff characters (SC4LimitStringLength).
bool ReadName(cIGZWin* dialog, std::uint32_t id, cRZString& out) {
    gz::cIGZWinTextEdit* edit = nullptr;
    if (!dialog->GetChildAsRecursive(id, gz::kGZIID_cIGZWinTextEdit, reinterpret_cast<void**>(&edit)) || !edit) return false;
    edit->GetText(out);
    edit->Release();
    if (out.Strlen() > kNameBufferSize - 1) out.Resize(kNameBufferSize - 1);
    return true;
}

void StoreName(std::size_t offset, const cRZString& name) {
    std::uint8_t* prefs = Preferences();
    if (!prefs) return;
    char* field = reinterpret_cast<char*>(prefs + offset);
    std::memset(field, 0, kNameBufferSize);
    std::memcpy(field, name.Data(), name.Strlen());
}

}  // namespace

void InstallEstablishCityHost() { static EstablishCityHost host; sc4::SetEstablishCityHost(&host); }

bool RunEstablishCityDialog(cIGZWin* view) {
    sc4::cSC4City* city = HostCity();
    WinMgr* manager = ActiveWinMgr();
    if (!view || !city || !manager) return false;
    cIGZWin* dialog = CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroup, kDialogScript}, view, kDialogWindowId);
    if (!dialog) return false;
    // The dialog's difficulty buttons start at the one chosen last time.
    city->SetDifficultyLevel(static_cast<sc4::cSC4City::DifficultyLevel>(PreferredDifficulty()));
    dialog->PullToFront();
    dialog->GZWinMoveTo((view->GetW() - dialog->GetW()) / 2, (view->GetH() - dialog->GetH()) / 3);

    gz::cIGZWinGen* gen = nullptr;
    sc4::cSC4EstablishCityWinProc* proc = nullptr;
    if (dialog->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen)) && gen) {
        proc = new sc4::cSC4EstablishCityWinProc(gen);
        proc->AddRef();
        gen->SetWinProc(proc);
    }
    if (gz::cIGZWinTextEdit* cityEdit = PrepareNameEdit(dialog, kCityNameEdit)) {
        cityEdit->SetSelection(0, cityEdit->GetTextLength());
        manager->GZSetFocus(cityEdit->AsIGZWin());
        cityEdit->Release();
    }
    if (gz::cIGZWinTextEdit* mayorEdit = PrepareNameEdit(dialog, kMayorNameEdit)) {
        // The mayor name of the last city is offered again.
        std::uint8_t* prefs = Preferences();
        const char* lastMayor = prefs ? reinterpret_cast<const char*>(prefs + sc4::prefs::kMayorName) : "";
        if (*lastMayor) mayorEdit->SetText(cRZString(lastMayor), false);
        mayorEdit->Release();
        if (proc) proc->ValidateCityAndMayorNames();   // the original posts the text-changed message 0x9b1552d8 to the dialog
    }

    const bool ok = manager->RunPopup(dialog) == 0;   // DoModalWin: EndDialog answers 0 for OK
    if (ok && proc) {
        const std::uint32_t difficulty = static_cast<std::uint32_t>(proc->Difficulty());
        city->SetDifficultyLevel(static_cast<sc4::cSC4City::DifficultyLevel>(difficulty));
        if (std::uint8_t* prefs = Preferences()) std::memcpy(prefs + sc4::prefs::kDifficultyLevel, &difficulty, sizeof difficulty);
        cRZString name;
        if (ReadName(dialog, kCityNameEdit, name)) {
            StoreName(sc4::prefs::kLastCityName, name);
            city->SetCityName(name);
            city->SetCityNameChanged(true);
        }
        if (ReadName(dialog, kMayorNameEdit, name)) {
            StoreName(sc4::prefs::kMayorName, name);
            city->SetMayorName(name);
        }
        if (cSC4App* app = BootApplication()) app->SavePreferences();
        city->SetEstablished(true);
    }
    if (proc) {
        proc->Shutdown();
        proc->Release();
    }
    manager->DestroyWindow(dialog);
    manager->GZSetFocus(view);
    if (gen) gen->Release();
    dialog->Release();
    return ok;
}

}  // namespace native
