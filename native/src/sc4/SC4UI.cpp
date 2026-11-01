#include "sc4/SC4UI.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>

#include "gz/Buffer.h"
#include "gz/RZServices.h"
#include "gz/WinGen.h"
#include "gz/WinText.h"
#include "rz/RZStringUtils.h"
#include "sc4/AutoHelpers.h"
#include "sc4/CatalogItem.h"
#include "sc4/DialogWinProcs.h"
#include "sc4/Exemplar.h"
#include "sc4/ISC4App.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SLBuffer.h"
#include "sc4/StringDetokenizer.h"
#include "sc4/WinCatalogView.h"
#include "sc4/WinProcs.h"

namespace {

// ---- the default host: the recovered engine accessors ------------------------------------------------

class DefaultNSC4UIHost final : public nSC4UI::INSC4UIHost {
public:
    bool PopupModalDialogsEnabled() override {
        // @ 0x100038e6b: SL::spSC4 -> vslot 0xb8 (cISC4App::GetPopupDialogsEnabled); false without the app.
        return SL::spSC4 != nullptr && SL::spSC4->GetPopupDialogsEnabled();
    }

    bool LoadExemplarString(cIGZString& out, sc4::cISCPropertyHolder* holder, std::uint32_t property,
                            sc4::cSCStringDetokenizer* detokenizer) override {
        // SC4LoadExemplarString @ 0x100121132 reads a resource-key property and hands it to SC4LoadString
        // @ 0x100121184 with the detokenizer. Neither is recovered yet, so the default host reports "no string"
        // and the catalog builder falls back to the plain properties, as the original does when the property is
        // missing. see CATALOG-STRINGS-1.
        (void)out;
        (void)holder;
        (void)property;
        (void)detokenizer;
        return false;
    }

    void MakeNumberString(std::int64_t value, cIGZString& out) override {
        // RZ::LM()->GetLanguageUtility(0) -> vslot 0x90 (MakeNumberString); the utility is borrowed.
        cIGZLanguageManager* languages = RZ::LM();
        if (!languages) return;
        if (cIGZLanguageUtility* utility = languages->GetLanguageUtility(0)) utility->MakeNumberString(value, out);
    }

    cIGZWinMgr* WindowManager() override { return RZ::WM(); }   // @ 0x100127d34

    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        return sc4::SC4CreateWindowFromScript(key, parent, id);   // @ 0x100039a9f
    }
    bool CreateWindowFromScriptAs(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                  std::uint32_t iid, void** out) override {
        return sc4::SC4CreateWindowFromScript(key, parent, id, iid, out);   // @ 0x100039bf7
    }

    cSLWinLocationSaver* WindowLocationSaver() override {
        // SL::spWinLocationSaver (cSC4App::GetWinLocationSaver, cISC4App vslot 0x158).
        if (!SL::spSC4) return nullptr;
        return static_cast<cSLWinLocationSaver*>(SL::spSC4->GetWinLocationSaver());
    }

    void BeginGamePause() override { sc4::SC4GamePause(true, false, false, true, true); }    // cSC4AutoGamePause ctor
    void EndGamePause() override { sc4::SC4GamePause(false, false, false, true, true); }     // ... and dtor
};

nSC4UI::INSC4UIHost* HostOrDefault() {
    static DefaultNSC4UIHost defaultHost;
    return &defaultHost;
}

}  // namespace

namespace nSC4UI {
INSC4UIHost* DefaultHost() { return HostOrDefault(); }

// ---- palette ramp --------------------------------------------------------------------------------------

// @ 0x10030a7f0. `ramp` is a list of (position, colour) pairs; the entries before the first stop get its
// colour, the ones past the last stop get the last colour, and in between two stops each colour is blended
// channel-pairwise with a 0..256 factor ((position - stop) * 256 / span).
void InterpolateRampToPalette(std::uint32_t* out, int count, const std::uint32_t* ramp, int rampCount) {
    std::uint32_t position = 0;
    if (count > 0) {
        do {
            if (ramp[0] <= position) break;
            out[position] = ramp[1];
            ++position;
        } while (static_cast<std::int64_t>(position) < static_cast<std::int64_t>(count));
    }

    if (static_cast<int>(position) < count) {
        const int limit = rampCount - 2;
        int index = 0;
        bool done = false;
        while (index < limit) {
            while (ramp[index + 2] <= position) {
                index += 2;
                if (index >= limit) {
                    done = true;
                    break;
                }
            }
            if (done) break;
            const std::uint32_t nextColour = ramp[index + 3];
            const std::uint32_t colour = ramp[index + 1];
            const std::uint32_t start = ramp[index];
            const std::uint32_t end = ramp[index + 2];
            const std::uint32_t factor = (static_cast<std::uint32_t>(position - start) << 8) / (end - start);
            const std::uint32_t lowColour = colour & 0xff00ffu;
            const std::uint32_t highColour = colour & 0xff00ff00u;
            const std::uint32_t low = ((((nextColour & 0xff00ffu) - lowColour) * factor + 0x800080u) >> 8) + lowColour;
            const std::uint32_t high =
                ((((nextColour >> 8) & 0xff00ffu) - (highColour >> 8)) * factor + 0x800080u) + highColour;
            out[position] = (high & 0xff00ff00u) | (low & 0xff00ffu);
            ++position;
            if (static_cast<std::int64_t>(position) >= static_cast<std::int64_t>(count)) break;
        }
    }

    if (static_cast<int>(position) < count) {
        std::uint32_t* fill = out + position;
        const std::uint32_t colour = ramp[rampCount - 1];
        for (std::int64_t left = static_cast<std::int64_t>(count) - position; left > 0; --left) *fill++ = colour;
    }
}

// ---- property tokens ------------------------------------------------------------------------------------

// @ 0x10030a156. "#D:xxxxxxxx#" formats the property as a plain number, "#M:xxxxxxxx#" as money; both accept a
// float or a (signed) integer property. The token must be exactly ten characters long and carry ':' at [1].
bool PropertyTokenReplacementFunction(const cIGZString& token, cIGZString& out, void* context) {
    auto* holder = static_cast<sc4::cISCPropertyHolder*>(context);
    if (token.Strlen() != 10) return false;
    const char* text = token.ToChar();
    if (text[1] != ':') return false;
    const char kind = text[0];
    if (kind != 'D' && kind != 'd' && kind != 'M' && kind != 'm') return false;

    const std::uint32_t property = RZAtoUint32HexA(text + 2);
    float floatValue = 0.0f;
    std::int32_t intValue = 0;
    const bool money = kind == 'M' || kind == 'm';
    if (sc4::GetFloatProperty(holder, property, floatValue)) {
        if (money) {
            cRZString format("%.2f");
            sc4::SC4MakeFormattedMoneyString(static_cast<double>(floatValue), out, format);
        } else {
            cRZString formatted;
            formatted.Sprintf("%.2f", static_cast<double>(floatValue));
            out.AssignFromIGZString(formatted);
        }
        return true;
    }
    if (!sc4::GetPropertyAsSint32(holder, property, intValue)) return false;
    if (money) sc4::SC4MakeMoneyString(intValue, out);
    else Host()->MakeNumberString(intValue, out);
    return true;
}

// ---- catalog items --------------------------------------------------------------------------------------

// @ 0x100309e70. The record is built from the exemplar's name / description / icon / id properties; the cost
// [sic] is always stored as 0 and a missing id (0xffffffff) becomes the exemplar's instance (i386 0x00243eb2).
sc4::cSC4CatalogItem* CreateCatalogItemFromExemplar(sc4::cISCResExemplar* exemplar, cIGZWin* window,
                                                    std::uint32_t itemType,
                                                    sc4::cSCStringDetokenizer* detokenizer) {
    if (!exemplar || !detokenizer) return nullptr;   // port guard
    sc4::cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();   // original vslot 0x30

    cRZString name;
    detokenizer->AddUnknownTokenReplacementMethod(&PropertyTokenReplacementFunction, holder, true);
    if (!Host()->LoadExemplarString(name, holder, 0x8a416a99, detokenizer)) {
        if (!sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x899afbad, name) &&
            !sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x20, name))
            return nullptr;
    }

    cRZString description;
    std::uint32_t iconInstance = 0x144161ec;
    std::uint32_t itemId = 0xffffffffu;
    if (!Host()->LoadExemplarString(description, holder, 0xca416ab5, detokenizer))
        sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x8a2602a9, description);
    sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x8a2602b8, iconInstance);
    sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x8a2602bb, itemId);
    detokenizer->AddUnknownTokenReplacementMethod(&PropertyTokenReplacementFunction, holder, false);

    // The icon bitmap is {0x856ddbac, 0x6a386d26, icon instance} in the default colour type.
    cSLBuffer icon(cGZPersistResourceKey{0x856ddbac, 0x6a386d26, iconInstance}, 1, false);

    // An exemplar without the id property is identified by its resource instance (the terrain brushes, 0x6a01fc2a).
    cGZPersistResourceKey key{};
    exemplar->GetKey(key);

    auto* item = new sc4::cSC4CatalogItem();
    item->AddRef();   // original vslot 0x8 (the catalog takes over the reference)
    item->SetItemType(itemType);
    item->SetItemID(itemId == 0xffffffffu ? key.instance : itemId);
    item->SetName(name);
    item->SetDescription(description);
    if (cIGZBuffer* bitmap = icon.GetBuffer()) {
        const std::uint32_t keyColour = window ? window->MakeFillColor(0xff, 0, 0xff) : 0;
        bitmap->SetTransparency(keyColour);   // original vslot 0xc8
        item->SetIcon(bitmap);
    }
    item->SetCost(0);          // quirk kept: this builder never stores the exemplar's cost
    item->SetEnabled(true);    // original vslot 0x50
    return item;
}

// @ 0x10030a37b. Walks the resource manager's available exemplars, keeps those whose type property
// (0x8a2602ba) equals `typeFilter` (and that carry `propertyFilter`, when non-zero) and inserts the built items
// into `items` under their sort property (0x8a2602b9, default 1000). Quirk kept: itemType is never read.
void CreateCatalogItemList(std::multimap<int, sc4::cISC4CatalogItem*>& items, sc4::cISC4WinCatalogView* view,
                           std::uint32_t itemType, std::uint32_t id, std::uint32_t typeFilter,
                           std::uint32_t propertyFilter) {
    (void)itemType;
    auto* detokenizer = new sc4::cSCStringDetokenizer();
    detokenizer->AddRef();

    cIGZPersistResourceManager* manager = SL::RM();   // SL::spRM
    if (manager) {
        cIGZPersistResourceKeyList* keys = nullptr;
        if (manager->GetAvailableResourceList(&keys, nullptr) && keys) {
            const std::uint32_t count = keys->Size();
            for (std::uint32_t i = 0; i < count; ++i) {
                const cGZPersistResourceKey* key = keys->GetKey(i);
                if (!key) continue;
                sc4::cISCResExemplar* exemplar = nullptr;
                if (!manager->GetResource(*key, sc4::kGZIID_cISCResExemplar,
                                          reinterpret_cast<void**>(&exemplar), 0, nullptr))
                    continue;
                if (!exemplar) continue;
                sc4::cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
                std::uint32_t type = 0;
                sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x8a2602ba, type);
                if (type == typeFilter && (propertyFilter == 0 || holder->HasProperty(propertyFilter))) {
                    cIGZWin* window = view ? view->AsIGZWin() : nullptr;
                    sc4::cSC4CatalogItem* item = CreateCatalogItemFromExemplar(exemplar, window, id, detokenizer);
                    if (item) {
                        std::uint32_t sortKey = 1000;
                        sc4::cSCPropertyHelp::GetPropertyValue(holder, 0x8a2602b9, sortKey);
                        items.emplace(static_cast<int>(sortKey), item);
                    }
                }
                exemplar->Release();
            }
            keys->Release();
        }
    }
    detokenizer->Release();
}

// @ 0x10030a6f4. The view takes over every item (AddItem), the map only holds the pointers.
sc4::cSC4WinCatalogView* CreateCatalogView(std::uint32_t itemType, std::uint32_t id, std::uint32_t typeFilter,
                                           std::uint32_t propertyFilter) {
    auto* view = new sc4::cSC4WinCatalogView(Host()->WindowManager());
    std::multimap<int, sc4::cISC4CatalogItem*> items;
    CreateCatalogItemList(items, view, itemType, id, typeFilter, propertyFilter);
    for (auto& entry : items) {
        view->AddItem(entry.second);   // original vslot 0x20
        if (entry.second) entry.second->Release();
    }
    items.clear();   // the original destroys the tree here
    return view;
}

// ---- dialogs -------------------------------------------------------------------------------------------

namespace {

// The two dialog resource instances live in the UI script group 0x96a006b0.
constexpr std::uint32_t kUIScriptGroupGeneric = 0x96a006b0;
constexpr std::uint32_t kNotificationScriptInstance = 0xca8cbf0f;
constexpr std::uint32_t kConfirmationScriptInstance = 0xea8cc3c6;
constexpr std::uint32_t kNotificationWindowId = 0xaa8def97;
constexpr std::uint32_t kConfirmationWindowId = 0x8a8dfcf5;
constexpr std::uint32_t kNotificationTitleId = 0xca8cc4b6;
constexpr std::uint32_t kNotificationTextId = 0x4a8cc4a9;
constexpr std::uint32_t kConfirmationTitleId = 0x8a91e4fd;
constexpr std::uint32_t kConfirmationTextId = 0x6a8cc495;
constexpr std::uint32_t kConfirmationOkId = 0x8a8dfd4e;
constexpr std::uint32_t kConfirmationCancelId = 0x8a8dfd4a;
constexpr std::uint32_t kConfirmationOkResult = 0x8a8dfd4e;   // the id RunPopup answers for "OK"

constexpr std::uint32_t kIIDWinGen = 0x5386d516;

// Sets a cGZWinText child's caption (the original's GetChildAsRecursive + vslot 0x20).
void SetTextCaption(cIGZWin* dialog, std::uint32_t id, const cIGZString& caption) {
    gz::cIGZWinText* text = nullptr;
    if (dialog->GetChildAsRecursive(id, gz::kGZIID_cIGZWinText, reinterpret_cast<void**>(&text)) && text) {
        text->SetCaption(caption);
        text->Release();
    }
}

// cSC4GenericDialogWinProc (WinProcs.h) as the original builds it: one reference, attached to the gen window
// through cIGZWinGen::SetWinProc (which AddRefs; the extra reference is dropped right after).
sc4::cSC4GenericDialogWinProc* AttachGenericWinProc(cIGZWin* dialog, gz::cIGZWinGen** gen) {
    *gen = nullptr;
    if (!dialog->QueryInterface(kIIDWinGen, reinterpret_cast<void**>(gen)) || !*gen) return nullptr;
    auto* proc = new sc4::cSC4GenericDialogWinProc();
    proc->AddRef();
    (*gen)->SetWinProc(proc);   // original vslot 0x38
    return proc;
}

// Positions a freshly created dialog at the centre of the main window horizontally and one third down.
void PlaceDialog(cIGZWin* dialog, cIGZWin* parent) {
    const int parentW = parent ? parent->GetW() : 0;
    const int parentH = parent ? parent->GetH() : 0;
    dialog->GZWinMoveTo((parentW - dialog->GetW()) / 2, (parentH - dialog->GetH()) / 3);
}

}  // namespace

// @ 0x10030b2da.
void GenericNotificationDialog(const cIGZString& title, const cIGZString& text) {
    INSC4UIHost* host = Host();
    if (!host->PopupModalDialogsEnabled()) return;   // SC4GetPopupModalDialogsEnabled
    cIGZWinMgr* manager = host->WindowManager();
    cIGZWin* previousFocus = manager ? manager->GZGetFocus() : nullptr;   // the original saves it up front
    cIGZWin* parent = manager ? manager->GetMainWindow() : nullptr;
    cIGZWin* dialog = host->CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroupGeneric,
                                                                        kNotificationScriptInstance},
                                                   parent, kNotificationWindowId);
    if (!dialog) return;
    // (the original re-queries the framework for the window manager at 0xa417445e/0x5a4 and gets RZ::WM()
    // again; here the same manager is used)
    host->BeginGamePause();
    dialog->PullToFront();
    PlaceDialog(dialog, parent);
    gz::cIGZWinGen* gen = nullptr;
    sc4::cSC4GenericDialogWinProc* proc = AttachGenericWinProc(dialog, &gen);
    SetTextCaption(dialog, kNotificationTitleId, title);
    SetTextCaption(dialog, kNotificationTextId, text);
    cSLWinLocationSaver* saver = host->WindowLocationSaver();
    if (saver) saver->RestoreWindowLocation(dialog, true);
    if (manager) manager->RunPopup(dialog);
    if (saver) saver->SaveWindowLocation(dialog);
    if (gen) gen->SetWinProc(nullptr);
    if (proc) proc->Release();
    if (manager) {
        manager->DestroyWindow(dialog);
        if (previousFocus && manager->IsWindowValid(previousFocus)) manager->GZSetFocus(previousFocus);
    }
    dialog->Release();
    host->EndGamePause();
    if (gen) gen->Release();
}

// @ 0x10030b655. The answer is whether the control that closed the modal dialog was the OK button.
bool GenericConfirmationDialog(const cIGZString& title, const cIGZString& text, const cIGZString* okText,
                               const cIGZString* cancelText) {
    INSC4UIHost* host = Host();
    if (!host->PopupModalDialogsEnabled()) return false;
    cIGZWinMgr* manager = host->WindowManager();
    cIGZWin* previousFocus = manager ? manager->GZGetFocus() : nullptr;
    cIGZWin* parent = manager ? manager->GetMainWindow() : nullptr;
    cIGZWin* dialog = host->CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroupGeneric,
                                                                        kConfirmationScriptInstance},
                                                   parent, kConfirmationWindowId);
    if (!dialog) return false;
    host->BeginGamePause();
    dialog->PullToFront();
    PlaceDialog(dialog, parent);
    gz::cIGZWinGen* gen = nullptr;
    sc4::cSC4GenericDialogWinProc* proc = AttachGenericWinProc(dialog, &gen);
    SetTextCaption(dialog, kConfirmationTitleId, title);
    SetTextCaption(dialog, kConfirmationTextId, text);
    if (okText) {
        gz::cIGZWinBtn* button = nullptr;
        if (dialog->GetChildAsRecursive(kConfirmationOkId, gz::kGZIID_cIGZWinBtn,
                                        reinterpret_cast<void**>(&button)) && button) {
            button->AsIGZWin()->SetCaption(*okText);   // cGZWinBtn::SetCaption @ 0x10027... (vslot 0x188)
            button->Release();
        }
    }
    if (cancelText) {
        gz::cIGZWinBtn* button = nullptr;
        if (dialog->GetChildAsRecursive(kConfirmationCancelId, gz::kGZIID_cIGZWinBtn,
                                        reinterpret_cast<void**>(&button)) && button) {
            button->AsIGZWin()->SetCaption(*cancelText);
            button->Release();
        }
    }
    cSLWinLocationSaver* saver = host->WindowLocationSaver();
    if (saver) saver->RestoreWindowLocation(dialog, true);
    const std::uint32_t result = manager ? static_cast<std::uint32_t>(manager->RunPopup(dialog)) : 0;
    if (saver) saver->SaveWindowLocation(dialog);
    if (gen) gen->SetWinProc(nullptr);
    if (proc) proc->Release();
    if (manager) {
        manager->DestroyWindow(dialog);
        if (previousFocus && manager->IsWindowValid(previousFocus)) manager->GZSetFocus(previousFocus);
    }
    dialog->Release();
    host->EndGamePause();
    if (gen) gen->Release();
    return result == kConfirmationOkResult;
}

// @ 0x10030ba79. Returns the id RunPopup answered (0 when no dialog was built).
std::uint32_t DoScriptedModalDialog(std::uint32_t scriptInstance, std::uint32_t windowId) {
    INSC4UIHost* host = Host();
    if (!host->PopupModalDialogsEnabled()) return 0;
    cIGZWinMgr* manager = host->WindowManager();
    cIGZWin* previousFocus = manager ? manager->GZGetFocus() : nullptr;
    cIGZWin* parent = manager ? manager->GetMainWindow() : nullptr;
    cIGZWin* dialog = nullptr;
    if (!host->CreateWindowFromScriptAs(cGZPersistResourceKey{0, kUIScriptGroupGeneric, scriptInstance}, parent,
                                        windowId, kChildEnumIID, reinterpret_cast<void**>(&dialog)) ||
        !dialog)
        return 0;
    host->BeginGamePause();
    dialog->PullToFront();
    PlaceDialog(dialog, parent);
    gz::cIGZWinGen* gen = nullptr;
    sc4::cSC4GenericDialogWinProc* proc = AttachGenericWinProc(dialog, &gen);
    cSLWinLocationSaver* saver = host->WindowLocationSaver();
    if (saver) saver->RestoreWindowLocation(dialog, true);
    const std::uint32_t result = manager ? static_cast<std::uint32_t>(manager->RunPopup(dialog)) : 0;
    if (saver) saver->SaveWindowLocation(dialog);
    if (gen) gen->SetWinProc(nullptr);
    if (proc) proc->Release();
    if (manager) {
        manager->DestroyWindow(dialog);
        if (previousFocus && manager->IsWindowValid(previousFocus)) manager->GZSetFocus(previousFocus);
    }
    host->EndGamePause();
    dialog->Release();
    if (gen) gen->Release();
    return result;
}

}  // namespace nSC4UI
