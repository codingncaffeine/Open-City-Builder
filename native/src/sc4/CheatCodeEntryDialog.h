// cSC4CheatCodeEntryDialog — the cheat code box (PC 1.1.641 0x0079bab0..0x0079c550, 16 functions + the std::sort of its name
// list; window vtable 0x00ab7078, cGZWinGenDlg view 0x00ab7020, message filter view 0x00ab7010). cWinSC4App opens it modally
// on its command 0x2a9496c9 ("Open Cheat Code Dialog", Ctrl+X in the general key table) and destroys it afterwards.
// The dialog is a one-line text edit (312 x 28, the edit at 4,6..308,26, flags 0x17c1 so Return notifies 0x9b1552d5, font
// 0x68963c4c, black text) on a magenta-keyed transparent private buffer. Return runs the line: a registered cheat through the
// cheat code manager (DoDefaultCheatCodeProcessing), anything else through the framework command server; a line that ran
// enters the session history and closes the box (Ctrl or Shift held: the line clears and the box stays open for more). A
// line nobody knows leaves the box open with the text. Esc, Alt+F4 and an empty Return close it. Up / Down walk the history
// (shared by every dialog for the whole session; one step past the newest line gives an empty line). F1 or a right click
// lists every cheat name, sorted by byte value, when the application's debug flag is on.
// Quirks kept: the accept / error sounds are never set (both ids stay 0), so the box is silent; a line the command server
// does not reject (any status but 2) counts as run.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/WinGenDlg.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZString.h"

class cGZCheatCodeManager;

namespace sc4 {

constexpr std::uint32_t kCheatDialogFilterIid = 0xc6ae7085;   // cIGZWinMessageFilter
constexpr std::uint32_t kCheatDialogFont = 0x68963c4c;

// What the dialog reaches outside itself.
class ICheatCodeEntryDialogHost {
public:
    virtual ~ICheatCodeEntryDialogHost() = default;
    virtual cGZCheatCodeManager* CheatCodeManager() = 0;               // global 0x00b43cac (SL::spCCM)
    // The command server (global 0x00b43cb0) slot 19 ExecuteCommand(text, length, null): the status, 2 = not a command.
    virtual std::uint32_t ExecuteCommandText(const cIGZString& text) = 0;
    virtual bool DebugFlag() = 0;                                      // cISC4App slot 21
    virtual cIGZWin* MainWindow() = 0;                                 // global 0x00b43dc8
    // SC4DoPopupStringListMenu (0x005f9f10): the index picked from `items` at x, y in `owner`, or a value >= items.size().
    virtual std::uint32_t PopupStringListMenu(cIGZWin* owner, int x, int y, const std::vector<cRZString>& items,
                                              cRZString& choice) = 0;
};
ICheatCodeEntryDialogHost* CheatCodeEntryDialogHost();
void SetCheatCodeEntryDialogHost(ICheatCodeEntryDialogHost* host);

class cSC4CheatCodeEntryDialog final : public gz::cGZWinGenDlg, public cIGZWinMessageFilter {
public:
    explicit cSC4CheatCodeEntryDialog(cIGZWinMgr* mgr);   // @ 0x0079bd00
    ~cSC4CheatCodeEntryDialog() override;                  // @ 0x0079bd80

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x0079bab0
    std::uint32_t AddRef() override { return cGZWinGenDlg::AddRef(); }
    std::uint32_t Release() override { return cGZWinGenDlg::Release(); }

    bool Init() override;                                  // @ 0x0079bdc0
    bool Shutdown() override;                              // @ 0x0079baf0
    bool DoMessage(cGZMessage& message) override;          // @ 0x0079bfc0
    bool GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;     // @ 0x0079c400
    bool GZOnMouseDownR(int x, int y, std::uint32_t modifiers) override;       // @ 0x0079c550
    // The filter on the edit window: its key-downs and right clicks reach the dialog first.   @ 0x0079bc70
    bool DoMessage(cIGZWin* window, cGZMessage& message) override;

    // The session's cheat line history (the global vector at 0x00b4dfdc).
    static std::vector<cRZString>& History();
    gz::cIGZWinTextEdit* Edit() const { return edit_; }

private:
    void EndDialog(std::uint32_t result);                  // @ 0x0079bcd0
    // @ 0x0079bb70: plays sound id +0x114 (which 0, accepted) or +0x118 (1, refused) through RZ::SS. No code writes
    // either id, so the call never plays anything (quirk kept: the box is silent).
    static bool PlaySound(int which) { (void)which; return false; }
    bool DoPopupStringListMenu();                          // @ 0x0079c1d0
    void ShowHistoryLine(std::uint32_t index);
    bool ControlOrShiftDown();

    gz::cIGZWinTextEdit* edit_ = nullptr;   // +0x110 (counted)
    std::uint32_t historyIndex_ = 0;        // +0x11c
};

}  // namespace sc4
