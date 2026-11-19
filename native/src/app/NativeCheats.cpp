#include "app/NativeCheats.h"

#include "app/NativeBoot.h"
#include "app/NativeKeyboard.h"
#include "app/NativeWinMgr.h"
#include "gz/CheatCodeManager.h"
#include "gz/CommandServer.h"
#include "sc4/App.h"
#include "sc4/CheatCodeEntryDialog.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinSC4App.h"

namespace native {
namespace {

struct CheatDialogHost final : sc4::ICheatCodeEntryDialogHost {
    cGZCheatCodeManager* CheatCodeManager() override { return GameCheatCodeManager(); }
    std::uint32_t ExecuteCommandText(const cIGZString& text) override {
        cIGZCommandServer* server = GameCommandServer();
        return server ? server->ExecuteCommand(text.Data(), text.Strlen(), nullptr) : 2;
    }
    bool DebugFlag() override { return GameDebugMode(); }
    // The app window (PC global 0x00b43dc8 is spWinSC4AppWin), not the desktop the window manager keeps as its root: the
    // box gives the keys back to it (Ctrl+X works again at once; the city view's keys wait for a click on the view).
    cIGZWin* MainWindow() override {
        WinMgr* manager = ActiveWinMgr();
        cIGZWin* root = manager ? manager->GetMainWindow() : nullptr;
        return root ? root->GetChildWindowFromID(sc4::kWinSC4AppId) : nullptr;
    }
    // SC4DoPopupStringListMenu is not ported: the list (debug builds only) offers nothing to pick.
    std::uint32_t PopupStringListMenu(cIGZWin*, int, int, const std::vector<cRZString>& items, cRZString&) override {
        return static_cast<std::uint32_t>(items.size());
    }
};

}  // namespace

cGZCheatCodeManager* GameCheatCodeManager() {
    if (auto* cheats = dynamic_cast<cGZCheatCodeManager*>(SL::spCCM)) return cheats;
    cSC4App* app = BootApplication();
    return app ? dynamic_cast<cGZCheatCodeManager*>(app->GetCheatCodeManager()) : nullptr;
}

bool GameDebugMode() {
    cSC4App* app = BootApplication();
    return app && app->GetDebugFunctionalityEnabled();
}

bool RegisterGameCheat(std::uint32_t id, const char* text) {
    cGZCheatCodeManager* cheats = GameCheatCodeManager();
    return cheats && text && cheats->RegisterCheatCode(id, cRZString(text));
}
void UnregisterGameCheat(std::uint32_t id) {
    if (cGZCheatCodeManager* cheats = GameCheatCodeManager()) cheats->UnregisterCheatCode(id);
}
void AddGameCheatListener(cIGZMessageTarget* target, std::uint32_t id) {
    if (cGZCheatCodeManager* cheats = GameCheatCodeManager(); cheats && target) cheats->AddNotification(target, id);
}
void RemoveGameCheatListener(cIGZMessageTarget* target, std::uint32_t id) {
    if (cGZCheatCodeManager* cheats = GameCheatCodeManager(); cheats && target) cheats->RemoveNotification(target, id);
}
void AddGameCheatListener(cIGZMessageTarget2* target, std::uint32_t id) {
    if (cGZCheatCodeManager* cheats = GameCheatCodeManager(); cheats && target) cheats->AddNotification2(target, id);
}
void RemoveGameCheatListener(cIGZMessageTarget2* target, std::uint32_t id) {
    if (cGZCheatCodeManager* cheats = GameCheatCodeManager(); cheats && target) cheats->RemoveNotification2(target, id);
}

cIGZWin* CreateCheatCodeEntryDialog() {
    static CheatDialogHost host;
    sc4::SetCheatCodeEntryDialogHost(&host);
    WinMgr* manager = ActiveWinMgr();
    if (!manager) return nullptr;
    return static_cast<cIGZWin*>(static_cast<cGZWin*>(new sc4::cSC4CheatCodeEntryDialog(manager)));
}

}  // namespace native
