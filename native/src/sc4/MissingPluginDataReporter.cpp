#include "sc4/MissingPluginDataReporter.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "gz/RZServices.h"
#include "gz/WinGen.h"
#include "gz/WinListBox.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/cRZString.h"
#include "sc4/DialogWinProcs.h"
#include "sc4/ModelMakerUtility.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SLString.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/WinSC4App.h"
#include "sc4/WinLocationSaver.h"

namespace sc4 {
namespace {
constexpr std::uint32_t kPropertyPluginPack = 0x6a871b82, kPropertyLitModel = 0xe83a081d;
constexpr std::uint32_t kMessagePostCityInit = 0xea8ae29a;
constexpr std::uint32_t kDialogScript = 0xea89b6c3, kDialogWindowId = 0x2a5cfb2c, kPackListId = 0x6a89fb3a;
constexpr std::uint32_t kOkButtonId = 0xaa89faf5, kButtonSender = 0x287259f6;
constexpr std::uint32_t kGZIID_cSC4MissingPluginPacksDialog = 0xc6ae7085, kGZIID_MessageTarget2 = 0x452294aa;
constexpr std::uint32_t kGZIID_cIGZPersistResource = 0x456b8f1d;
}  // namespace

bool SC4GetPopupModalDialogsEnabled() { return SL::spSC4 && SL::spSC4->GetPopupDialogsEnabled(); }

// ---- the filter -------------------------------------------------------------------------------------------------------

bool cSC4MissingPluginPackOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    return holder->HasProperty(kPropertyPluginPack) && AreModelsAvailable(occupant) == 1;
}

int cSC4MissingPluginPackOccupantFilter::AreModelsAvailable(cISC4Occupant* occupant) {
    const bool lit = occupant->AsPropertyHolder()->HasProperty(kPropertyLitModel);
    std::uint32_t orientation = 0;
    int state = 0;
    cSC4ModelMakerUtility utility;
    utility.GetOccupantOrientationAndState(occupant, orientation, state);
    int result = AreModelsAvailableForZoomAndRotation(occupant, zoom, rotation, orientation, state, lit);
    if (result != 2) return result;
    for (int other = 4; other != -1; --other) {
        if (other == zoom) continue;
        const int found = AreModelsAvailableForZoomAndRotation(occupant, other, 0, orientation, state, lit);
        if (found != 2) return found;
    }
    return 2;
}

// The first non-zero model instance decides: its resource {type, group} of the lookup, that instance.
int cSC4MissingPluginPackOccupantFilter::AreModelsAvailableForZoomAndRotation(cISC4Occupant* occupant, int zoomLevel, int rotationIndex,
                                                                             std::uint32_t orientation, int state, bool lit) {
    std::vector<std::uint32_t> ids;
    ids.reserve(4);
    std::uint32_t type = 0, group = 0;
    cSC4ModelMakerUtility utility;
    utility.GetAllModelInstanceIDs(ids, nullptr, occupant, zoomLevel, rotationIndex, state, lit, orientation, &type, &group);
    for (std::uint32_t id : ids) {
        if (id == 0) continue;
        void* resource = nullptr;
        const cGZPersistResourceKey key{type, group, id};
        if (!SL::spRM || !SL::spRM->GetResource(key, kGZIID_cIGZPersistResource, &resource, 0, nullptr)) return 1;
        static_cast<cIGZUnknown*>(resource)->Release();
        return 0;
    }
    return 2;
}

// ---- the dialog -------------------------------------------------------------------------------------------------------

bool cSC4MissingPluginPacksDialog::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cSC4MissingPluginPacksDialog) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4MissingPluginPacksDialog::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4MissingPluginPacksDialog::DoMessage(cIGZWin* window, cGZMessage& message) {
    if (window->GetID() != kDialogWindowId || message.type != kGZMsgCommand || message.param1 != kButtonSender) return false;
    if (static_cast<std::uint32_t>(message.param2) == kOkButtonId)
        if (cIGZWinMgr* windows = window->GetWindowManager()) windows->EndDialog(0xf, window, static_cast<std::int32_t>(kOkButtonId));
    return true;
}

bool cSC4MissingPluginPacksDialog::DoDialog(const std::set<std::uint32_t>& packs) {
    // SL::spWinSC4AppWin: the app window under the window manager's root
    cIGZWinMgr* root = RZ::WM();
    cIGZWin* parent = root && root->GetMainWindow() ? root->GetMainWindow()->GetChildWindowFromID(kWinSC4AppId) : nullptr;
    if (!parent) return false;
    cIGZWin* window = SC4CreateWindowFromScript(cGZPersistResourceKey{0, kUIScriptGroup, kDialogScript}, parent, kDialogWindowId);
    if (!window) return false;
    window->AddMessageFilter(this);                                       // window vslot 0x1d0
    gz::cIGZWinGen* frame = nullptr;
    if (window->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&frame)) && frame) {
        frame->SetWinProc(nullptr);
        frame->Release();
    }
    gz::cIGZWinListBox* list = nullptr;
    if (window->GetChildAsRecursive(kPackListId, gz::kGZIID_cIGZWinListBox, reinterpret_cast<void**>(&list)) && list) {
        const cSLString label(0x6a231eaa, 0x6a89c0d9, true);         // "Plugin Pack"
        cRZString line;
        for (std::uint32_t pack : packs) {
            line.Sprintf("%s %d", label.ToChar(), static_cast<int>(pack));
            list->InsertString(line, -2, 0);
        }
    }
    cIGZWinMgr* windows = window->GetWindowManager();
    auto* saver = static_cast<cSLWinLocationSaver*>(SL::spWinLocationSaver);
    if (saver) saver->RestoreWindowLocation(window, true);
    const int result = windows ? windows->RunPopup(window) : 0;          // WM vslot 0xa4
    if (saver) saver->SaveWindowLocation(window);
    if (windows) windows->DestroyWindow(window);                         // WM vslot 0x5c
    window->Release();
    if (list) list->Release();
    return result == static_cast<std::int32_t>(kOkButtonId);
}

// ---- the reporter -----------------------------------------------------------------------------------------------------

cSC4MissingPluginDataReporter::~cSC4MissingPluginDataReporter() {
    if (initialized_) Shutdown();
}

bool cSC4MissingPluginDataReporter::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_MessageTarget2) return false;
    *out = static_cast<cIGZMessageTarget2*>(this);
    AddRef();
    return true;
}

std::uint32_t cSC4MissingPluginDataReporter::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4MissingPluginDataReporter::Init() {
    if (initialized_) return true;
    if (SL::spMS2) SL::spMS2->AddNotification(this, kMessagePostCityInit);
    filter_ = new cSC4MissingPluginPackOccupantFilter();
    filter_->AddRef();
    initialized_ = true;
    return true;
}

bool cSC4MissingPluginDataReporter::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (filter_) filter_->Release();
    filter_ = nullptr;
    if (SL::spMS2) SL::spMS2->RemoveNotification(this, kMessagePostCityInit);
    return true;
}

bool cSC4MissingPluginDataReporter::DoMessage(cIGZMessage2* message) {
    if (message->GetType() == kMessagePostCityInit) DoPostCityInit();
    return true;
}

bool cSC4MissingPluginDataReporter::DoPostCityInit() {
    std::set<std::uint32_t> packs;
    if (SL::spViewUtilities) {
        filter_->zoom = SL::spViewUtilities->Zoom();
        filter_->rotation = SL::spViewUtilities->Rotation();
    }
    cISC4OccupantManager* manager = nullptr;
    if (!SL::spOccupantManager ||
        !SL::spOccupantManager->QueryInterface(kGZIID_cISC4OccupantManager, reinterpret_cast<void**>(&manager)) || !manager)
        return false;
    OccupantList occupants;
    manager->GetOccupantsByOccupantManagerCells(occupants, nullptr, nullptr, filter_, 0xffffffffu);   // vslot 0x9c: the whole city
    manager->Release();
    for (cISC4Occupant* occupant : occupants) {
        std::uint32_t pack = 0;
        if (cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyPluginPack, pack)) packs.insert(pack);
        occupant->Release();
    }
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "missing plugins: %zu packs\n", packs.size());
    if (packs.empty() || !SC4GetPopupModalDialogsEnabled()) return true;
    auto* dialog = new cSC4MissingPluginPacksDialog();
    dialog->AddRef();
    const bool ok = dialog->DoDialog(packs);
    dialog->Release();
    return ok;
}

}  // namespace sc4
