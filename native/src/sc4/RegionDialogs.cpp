#include "sc4/RegionDialogs.h"

#include "gz/WinListBox.h"

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

}  // namespace sc4
