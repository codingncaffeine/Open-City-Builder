#include "sc4/CheatCodeEntryDialog.h"

#include <algorithm>
#include <cstring>

#include "gz/CheatCodeManager.h"
#include "gz/WinMgrBase.h"
#include "rz/cRZKeyboard.h"

namespace sc4 {
namespace {
ICheatCodeEntryDialogHost* g_host = nullptr;

constexpr std::uint32_t kKeyF1 = 0x70, kKeyF4 = 0x73, kKeyUp = 0x26, kKeyDown = 0x28, kKeyEscape = 0x1b;
constexpr std::uint32_t kKeyControl = 0x11, kKeyShift = 0x10;
constexpr std::uint32_t kModifierAlt = 4;
constexpr std::uint32_t kEditFlags = 0x17c1;   // 0x400: Return sends kTextEditMsgKey to the dialog
}  // namespace

ICheatCodeEntryDialogHost* CheatCodeEntryDialogHost() { return g_host; }
void SetCheatCodeEntryDialogHost(ICheatCodeEntryDialogHost* host) { g_host = host; }

std::vector<cRZString>& cSC4CheatCodeEntryDialog::History() {
    static auto* history = new std::vector<cRZString>();   // the whole session's lines (never destroyed)
    return *history;
}

cSC4CheatCodeEntryDialog::cSC4CheatCodeEntryDialog(cIGZWinMgr* mgr) : gz::cGZWinGenDlg(mgr, gz::kWindowBackgroundBevelled) {
    historyIndex_ = static_cast<std::uint32_t>(History().size());
}

cSC4CheatCodeEntryDialog::~cSC4CheatCodeEntryDialog() { Shutdown(); }

bool cSC4CheatCodeEntryDialog::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kCheatDialogFilterIid) {
        *out = static_cast<cIGZWinMessageFilter*>(this);
        AddRef();
        return true;
    }
    return cGZWinGenDlg::QueryInterface(iid, out);
}

bool cSC4CheatCodeEntryDialog::Init() {
    if (get_init_flag()) return true;
    if (GetW() <= 16 || GetH() <= 16) SetArea(8, 8, 0xf8, 0x1c);
    cGZWinGenDlg::Init();
    PrivateBuffer(true);
    fill_private_buffer_with_transparent_color();
    set_private_buffer_transparency(true);

    const cRZRect area{4, 6, 308, 26};
    cIGZWin* window = AddTextEdit(area, cRZString(""));
    if (window) window->QueryInterface(gz::kGZIID_cIGZWinTextEdit, reinterpret_cast<void**>(&edit_));
    if (edit_) {
        edit_->SetFlags(kEditFlags);
        if (gz::IWinFont* font = ControlFont()) edit_->SetFont(font->AsFont());   // RZ::WCM slot 5 (0x68963c4c)
        edit_->SetFontColor(MakeFillColor(0, 0, 0));
        edit_->AsIGZWin()->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(this));
    }
    if (cIGZWinMgr* manager = GetWindowManager()) manager->GZSetFocus(window);
    SetW(area.right - area.left + 8);
    SetH(area.bottom - area.top + 8);
    SetFillColor(MakeFillColor(255, 0, 255));
    return true;
}

bool cSC4CheatCodeEntryDialog::Shutdown() {
    if (!get_init_flag()) return true;
    if (edit_) {
        edit_->AsIGZWin()->RemoveMessageFilter(static_cast<cIGZWinMessageFilter*>(this));
        edit_->Release();
        edit_ = nullptr;
    }
    SetFlag(kWinVisible, false);
    return cGZWinGenDlg::Shutdown();
}

void cSC4CheatCodeEntryDialog::EndDialog(std::uint32_t result) {
    cIGZWinMgr* manager = GetWindowManager();
    if (!manager) return;
    manager->EndDialog(gz::kWinMgrMsgEndModal, static_cast<cIGZWin*>(static_cast<cGZWin*>(this)), result);
    if (cIGZWin* main = g_host ? g_host->MainWindow() : nullptr) manager->GZSetFocus(main);
}

bool cSC4CheatCodeEntryDialog::ControlOrShiftDown() {
    auto* keyboard = dynamic_cast<cRZKeyboard*>(GetKeyboard());
    return keyboard && (keyboard->IsKeyDown(kKeyControl) || keyboard->IsKeyDown(kKeyShift));
}

bool cSC4CheatCodeEntryDialog::DoMessage(cGZMessage& message) {
    if (message.type == kGZMsgSetFocus) {   // 0x11: the focus goes on to the edit
        if (cIGZWinMgr* manager = GetWindowManager(); manager && edit_) manager->GZSetFocus(edit_->AsIGZWin());
        return cGZWinGenDlg::DoMessage(message);
    }
    if (message.type != gz::kTextEditMsgKey) return cGZWinGenDlg::DoMessage(message);

    cGZCheatCodeManager* cheats = g_host ? g_host->CheatCodeManager() : nullptr;
    if (!cheats) {
        EndDialog(0);
        return false;
    }
    cRZString line;
    if (edit_) edit_->GetText(line);
    if (line.Strlen() == 0) {
        EndDialog(0);
        return true;
    }
    std::uint32_t id = 0;
    cheats->DoDefaultCheatCodeProcessing(line, id);
    if (id == 0) {
        if (g_host->ExecuteCommandText(line) == 2) {   // nobody knows the line: it stays for another try
            PlaySound(1);
            return true;
        }
        id = 0xffffffffu;
    }
    std::vector<cRZString>& history = History();
    history.push_back(line);
    historyIndex_ = static_cast<std::uint32_t>(history.size());
    if (ControlOrShiftDown()) {   // more cheats to come: the line clears and the box stays
        if (edit_) edit_->SetText(cRZString(""), false);
        PlaySound(0);
    } else {
        PlaySound(0);
        EndDialog(1);
    }
    return true;
}

void cSC4CheatCodeEntryDialog::ShowHistoryLine(std::uint32_t index) {
    if (!edit_) return;
    const cRZString& line = History()[index];
    edit_->SetText(line, false);
    edit_->SetInsertionIndex(line.Strlen());
}

bool cSC4CheatCodeEntryDialog::GZOnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    const std::vector<cRZString>& history = History();
    const auto count = static_cast<std::uint32_t>(history.size());
    switch (key) {
    case kKeyF1:
        DoPopupStringListMenu();
        return true;
    case kKeyUp:
        if (count == 0) return true;
        if (historyIndex_ > 0) ShowHistoryLine(--historyIndex_);
        else PlaySound(1);
        return true;
    case kKeyDown:
        if (count == 0) return true;
        if (historyIndex_ >= count) {
            PlaySound(1);
        } else if (++historyIndex_ < count) {
            ShowHistoryLine(historyIndex_);
        } else if (edit_) {
            edit_->SetText(cRZString(""), false);
        }
        return true;
    case kKeyEscape:
        EndDialog(0);
        return true;
    case kKeyF4:
        if (modifiers & kModifierAlt) {
            EndDialog(0);
            return true;
        }
        return false;
    default:
        return false;
    }
}

bool cSC4CheatCodeEntryDialog::GZOnMouseDownR(int, int, std::uint32_t) {
    DoPopupStringListMenu();
    return true;
}

bool cSC4CheatCodeEntryDialog::DoMessage(cIGZWin*, cGZMessage& message) {
    if (message.type == kGZMsgKeyDown)
        return GZOnKeyDown(static_cast<std::uint32_t>(message.param1), static_cast<std::uint32_t>(message.param2));
    if (message.type == kGZMsgMouseDownR)
        return GZOnMouseDownR(static_cast<short>(message.param1), static_cast<short>(message.param2),
                              static_cast<std::uint16_t>(message.param3 >> 16));
    return false;
}

bool cSC4CheatCodeEntryDialog::DoPopupStringListMenu() {
    if (!g_host || !g_host->DebugFlag()) return false;
    cGZCheatCodeManager* cheats = g_host->CheatCodeManager();
    if (!cheats) return false;
    std::uint32_t none = 0;
    const std::uint32_t count = cheats->GetListOfAllCheatStrings(nullptr, none);   // a zero capacity answers the count
    std::vector<const cIGZString*> names(count);
    std::uint32_t got = count;
    if (count) cheats->GetListOfAllCheatStrings(names.data(), got);
    else got = 0;
    std::vector<cRZString> items;
    for (std::uint32_t i = 0; i < got; ++i) items.emplace_back(*names[i]);
    // std::string order: unsigned bytes over the shorter length, the shorter string first on a tie.
    std::sort(items.begin(), items.end(), [](const cRZString& a, const cRZString& b) {
        return std::string(a.Data(), a.Strlen()) < std::string(b.Data(), b.Strlen());
    });
    cIGZWin* owner = GetParentWin();
    int x = 0, y = 0;
    if (cIGZWinMgr* manager = GetWindowManager()) manager->GetCursorPos(owner, x, y);
    cRZString choice;
    const std::uint32_t picked = g_host->PopupStringListMenu(owner, x, y, items, choice);
    if (picked < items.size() && edit_) {
        edit_->SetText(choice, false);
        edit_->SetInsertionIndex(choice.Strlen());
    }
    return true;
}

}  // namespace sc4
