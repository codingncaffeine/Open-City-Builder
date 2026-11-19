// Cheat codes on the native build: the cheat code manager the application creates (cSC4App::Init), the cheat code box
// (cSC4CheatCodeEntryDialog, opened by Ctrl+X through the app window's command), and the systems' cheat registrations.
#pragma once

#include <cstdint>

class cGZCheatCodeManager;
class cIGZMessageTarget;
class cIGZMessageTarget2;
class cIGZWin;

namespace native {

// The application's cheat code manager (SL::spCCM), or null before the application has made it.
cGZCheatCodeManager* GameCheatCodeManager();
// cISC4App::GetDebugFunctionalityEnabled (SL::spSC4 vslot 0xa8): the debug cheats (LotPlop, Moolah ...) exist only then.
bool GameDebugMode();
// The systems' registrations: the cheat code manager's RegisterCheatCode / UnregisterCheatCode (vslots 0x18 / 0x20) and
// its two listener lists (vslots 0x28 / 0x30 the old cGZMessage targets, 0x38 / 0x40 the cIGZMessage2 targets; id 0 hears
// every cheat). Each does nothing without the manager.
bool RegisterGameCheat(std::uint32_t id, const char* text);
void UnregisterGameCheat(std::uint32_t id);
void AddGameCheatListener(cIGZMessageTarget* target, std::uint32_t id);
void RemoveGameCheatListener(cIGZMessageTarget* target, std::uint32_t id);
void AddGameCheatListener(cIGZMessageTarget2* target, std::uint32_t id);
void RemoveGameCheatListener(cIGZMessageTarget2* target, std::uint32_t id);
// new cSC4CheatCodeEntryDialog on the active window manager, reference count 0 (like `new`); installs the dialog's host.
cIGZWin* CreateCheatCodeEntryDialog();

}  // namespace native
