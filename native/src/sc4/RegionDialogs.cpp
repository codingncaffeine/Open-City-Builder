#include "sc4/RegionDialogs.h"

#include "gz/WinListBox.h"
#include "gz/WinBtn.h"
#include "gz/WinTextEdit.h"
#include "rz/PathUtils.h"

#include <cstdio>
#include <cstdlib>
#include "rz/cRZString.h"

namespace sc4 {

namespace {
constexpr std::uint32_t kRegionList = 0x1000;

gz::cIGZWinListBox* RegionList(cIGZWin* dialog) {
    cIGZWin* child = dialog ? dialog->GetChildWindowFromIDRecursive(kRegionList) : nullptr;
    gz::cIGZWinListBox* list = nullptr;
    if (child && child->QueryInterface(gz::kGZIID_cIGZWinListBox, reinterpret_cast<void**>(&list)) && list) return list;
    return nullptr;
}
}  // namespace

// Every region's name with its index as the item id ('#' starts a formatting code in a list item, so a leading '#' is
// doubled), the active region selected, then sorted.
void cSC4LoadRegionDialog::GZOnAttach() {
    gz::cIGZWinListBox* list = RegionList(Window());
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "load region: %zu regions, list %p\n", nation_.names.size(), static_cast<void*>(list));
    if (!list) return;
    for (std::size_t i = 0; i < nation_.names.size(); ++i) {
        cRZString name(nation_.names[i].c_str());
        if (name.Strlen() != 0 && name.ToChar()[0] == '#') name = cRZString(("#" + nation_.names[i]).c_str());
        list->InsertString(name, -2, static_cast<std::uint32_t>(i));
    }
    list->SetSelectionByID(static_cast<std::uint32_t>(nation_.active), true);
    list->Sort();
    list->Release();
}

void cSC4LoadRegionDialog::GZOnOK() {
    gz::cIGZWinListBox* list = RegionList(Window());
    const std::uint32_t id = list ? list->GetSelectionID() : 0xffffffffu;
    if (list) list->Release();
    EndDialog(id);
}

namespace {
class cButtonResultDialog final : public cSC4DialogWinProcBase {
public:
    cButtonResultDialog(std::uint32_t instance, std::uint32_t windowId) : cSC4DialogWinProcBase(instance, windowId, 0) {}
    bool GZOnButtonActivate(std::uint32_t id) override { EndDialog(id); return true; }
};
}  // namespace

std::uint32_t SC4RunScriptedButtonDialog(cIGZWin* parent, std::uint32_t instance, std::uint32_t windowId) {
    auto* dialog = new cButtonResultDialog(instance, windowId);
    dialog->AddRef();
    const std::uint32_t result = dialog->Display(parent);
    dialog->Release();
    return result;
}

int SC4DisplayLoadRegionDialog(cIGZWin* parent, const RegionDialogNation& nation) {
    auto* dialog = new cSC4LoadRegionDialog(nation);
    dialog->AddRef();
    const int result = static_cast<int>(dialog->Display(parent));
    dialog->Release();
    return result;
}

bool SC4DisplayDeleteRegionDialog(cIGZWin* parent) {
    auto* dialog = new cSC4DeleteRegionDialog();
    dialog->AddRef();
    const std::uint32_t result = dialog->Display(parent);
    dialog->Release();
    return result != 0;
}


// ---- new region ----------------------------------------------------------------------------------------------------

bool cSC4NewRegionDialog::NameConflicts(const cIGZString& name) {
    if (!nation_) return false;
    for (const std::string& existing : nation_->RegionNames())
        if (cRZString(existing.c_str()).IsEqualToStringRef(name, false)) return true;
    return false;
}

void cSC4NewRegionDialog::EnableControls() {
    cIGZWin* ok = Window() ? Window()->GetChildWindowFromIDRecursive(0x8a5b9959) : nullptr;
    cIGZWin* name = Window() ? Window()->GetChildWindowFromIDRecursive(0x2000) : nullptr;
    if (!ok || !name) return;
    ok->SetFlag(2, IsFileNameValid(cRZString(name->GetCaption()), nullptr));
}

bool cSC4NewRegionDialog::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (message.type == 0x9b1552d8u) {   // the name changed
        EnableControls();
        return true;
    }
    return cSC4DialogWinProcBase::DoWinProcMessage(window, message);
}

// The script's name, made unique ("<name> 1", "<name> 2", ...), selected for typing; the name field and the two terrain
// buttons notify the dialog.
void cSC4NewRegionDialog::GZOnAttach() {
    cIGZWin* name = Window() ? Window()->GetChildWindowFromIDRecursive(0x2000) : nullptr;
    if (!name) return;
    void* raw = nullptr;
    gz::cIGZWinTextEdit* edit = name->QueryInterface(gz::kGZIID_cIGZWinTextEdit, &raw) && raw ? static_cast<gz::cIGZWinTextEdit*>(raw) : nullptr;
    const cRZString base(name->GetCaption());
    cRZString unique(base);
    for (int n = 1; NameConflicts(unique); ++n) unique.Sprintf("%s %d", base.ToChar(), n);
    name->SetCaption(unique);
    if (edit) edit->SetMaximumTextToAllow(0x80);
    name->SetNotificationTarget(Window());
    if (edit) {
        edit->SetFlag(0x800);
        edit->SetSelection(0, edit->GetTextLength());
    }
    for (std::uint32_t id : {0x1000u, 0x1001u})
        if (cIGZWin* button = Window()->GetChildWindowFromIDRecursive(id)) button->SetNotificationTarget(Window());
    EnableControls();
    if (edit) edit->Release();
}

// The two terrain buttons act as a radio pair.
bool cSC4NewRegionDialog::GZOnButtonActivate(std::uint32_t id) {
    if (id != 0x1000 && id != 0x1001) return cSC4DialogWinProcBase::GZOnButtonActivate(id);
    if (gz::cIGZWinBtn* flat = GetBtnControl(0x1000)) flat->SetChecked(id == 0x1000);
    if (gz::cIGZWinBtn* mountains = GetBtnControl(0x1001)) mountains->SetChecked(id == 0x1001);
    return true;
}

void cSC4NewRegionDialog::GZOnOK() {
    cIGZWin* field = Window() ? Window()->GetChildWindowFromIDRecursive(0x2000) : nullptr;
    if (!field) return;
    const cRZString name(field->GetCaption());
    if (!IsFileNameValid(name, nullptr)) return;
    if (NameConflicts(name)) {
        if (nation_) nation_->NotifyNameInUse();
        return;
    }
    gz::cIGZWinBtn* mountains = GetBtnControl(0x1001);
    const std::uint32_t terrain = mountains && mountains->IsChecked() ? 1u : 0u;
    const int index = nation_ ? nation_->AddNewRegion(name, terrain) : -1;
    EndDialog(static_cast<std::uint32_t>(index));
}

bool SC4DisplayDeleteCityDialog(cIGZWin* parent) {
    auto* dialog = new cSC4DeleteCityDialog();
    dialog->AddRef();
    const std::uint32_t result = dialog->Display(parent);
    dialog->Release();
    return result != 0;
}

int SC4DisplayNewRegionDialog(cIGZWin* parent, INewRegionNation* nation) {
    auto* dialog = new cSC4NewRegionDialog(nation);
    dialog->AddRef();
    const int result = static_cast<int>(dialog->Display(parent));
    dialog->Release();
    return result;
}

}  // namespace sc4
