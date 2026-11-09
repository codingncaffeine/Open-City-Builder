// cSC4SubBudgetWindow — the department budget dialog the budget panel opens for one budget group (Mac i386
// 0x002613ec..0x00279028; decompile in scratch/s52/ghidra/subbudget.c, notes in scratch/s52/subbudget-notes.md).
// The dialog is built in code: UI art strips stacked by GetBudgetSegment (SetupWindowBig), then one row per department
// line: income rows (name, money) and expense rows (a button, the name, the cost, a funding bar image and a funding slider).
// Moving a slider sets the department's funding and refreshes the rows (UpdateValues); OK keeps the changes, Cancel / Esc
// restores the funding the dialog opened with.
//
// It is a cIGZWinProc on the dialog window (DoWinMsg: init 0xa2bf8ad0, shutdown 0xa2bf8ad1, key 5, command 3).
//   PARTIAL (phase A of the port, see the notes): the department path only; the ordinance (purpose 1), neighbor deal
//   (purpose 2) and individual building (+0x24) dialogs, row scrolling (0x451..0x454) and the department buttons'
//   individual budget windows are not ported yet: those dialogs open empty with OK and Cancel.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/WinProc.h"
#include "rz/cRZString.h"
#include "sc4/BudgetUIWinProc.h"

class cIGZBuffer;
class cIGZWinMgr;
namespace gz {
class IWinFont;
class cIGZWinGen;
class cIGZWinText;
class cIGZWinSlider;
class cIGZWinBtn;
}  // namespace gz

namespace sc4 {

class cSC4BudgetSimulator;
class cSC4DepartmentBudget;

// What the original reached through globals: spCity vslot 0xd0 (the budget simulator), RZ::WCM() (controls and fonts),
// RZ::GS() (colours), cSLBuffer (UI art) and the window manager.
class ISubBudgetWindowHost {
public:
    virtual ~ISubBudgetWindowHost() = default;
    virtual cSC4BudgetSimulator* BudgetSimulator() = 0;
    virtual cIGZWinMgr* WindowManager() = 0;
    // GZCOM()->CreateInstance(0x4386d516, 0x5386d516): a new cGZWinGen (one reference), or null.
    virtual gz::cIGZWinGen* CreateWinGen() = 0;
    // cSLBuffer {0x856ddbac, 0x46a006b0, instance}: the UI picture (borrowed), or null.
    virtual cIGZBuffer* UiImage(std::uint32_t instance) = 0;
    // RZ::WCM() vslot 0x14: the font of a style id (borrowed), or null.
    virtual gz::IWinFont* Font(std::uint32_t styleId) = 0;
    // RZ::GS() vslot 0x8c: the native colour of (r, g, b).
    virtual std::uint32_t MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0;
    // cGZWinCtrlMgr::CreateLabel / CreateSlider / CreateButton (Mac i386 0x0016a49a / 0x0016a8be / 0x0016a18c): one reference.
    virtual gz::cIGZWinText* CreateLabel(std::uint32_t id, const cIGZString& caption) = 0;
    virtual gz::cIGZWinSlider* CreateSlider(std::uint32_t id, int direction, int minimum, int maximum, int value) = 0;
    virtual gz::cIGZWinBtn* CreateButton(std::uint32_t style, std::uint32_t id, const cIGZString& caption, cIGZBuffer* image) = 0;
    // A cGZWinBMP showing `image` at (x, y) in its natural size (one reference).
    virtual cIGZWin* CreateBitmap(cIGZBuffer* image, int x, int y) = 0;
};
ISubBudgetWindowHost* SubBudgetWindowHost();
void SetSubBudgetWindowHost(ISubBudgetWindowHost* host);

class cSC4SubBudgetWindow final : public ISubBudgetWindow, public gz::cIGZWinProc {
public:
    cSC4SubBudgetWindow();                                              // i386 0x00269b7c
    ~cSC4SubBudgetWindow();

    bool QueryInterface(std::uint32_t iid, void** out) override;        // 0x00261c16
    std::uint32_t AddRef() override { return ++refs_; }                 // 0x00261c4e
    std::uint32_t Release() override;                                   // 0x00261c60

    // ISubBudgetWindow
    bool Create(cIGZWin** window, std::uint32_t purpose, std::uint32_t period, cIGZWin* parent) override;   // CreateSubBudgetWindow 0x00265358
    void Destroy() override;                                            // DestroySubBudgetWindow 0x0026961a

    // cIGZWinProc
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }   // the base (0x006229ce)
    bool DoWinMsg(cIGZWin* window, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t p3) override;   // 0x00279028

    // ---- inspection (not in the original)
    std::uint32_t Result() const { return result_; }
    std::size_t SliderCount() const { return sliders_.size(); }
    std::uint32_t IncomeRows() const { return incomeRows_; }
    std::uint32_t ExpenseRows() const { return expenseRows_; }

    struct SliderInfo {                                                                               // +0x2c entries
        std::uint32_t id = 0;
        cSC4DepartmentBudget* department = nullptr;
        std::uint32_t moneyText = 0;   // the row's money text (port: UpdateValues re-captions it)
        bool departmentRow = false;    // a department total row (item rows carry a hidden slider)
    };
    struct BudgetRestoreInfo { float funding = 0.0f; cSC4DepartmentBudget* department = nullptr; };   // +0x38 entries

private:
    bool OnInit(cIGZWin* window);                                       // on_init 0x0026e80e (department path)
    bool OnShutdown();                                                  // on_shutdown 0x0026a20e
    bool OnButtonActivated(cIGZWin* window, std::uint32_t id);          // on_button_activated 0x0027825c (OK / Cancel)
    void EndDialog(cIGZWin* window, std::uint32_t result);              // end_dialog 0x00261c86
    void UpdateValues(cIGZWin* window);                                 // 0x00271a46 (department path)
    void RestoreStartSettings();                                        // 0x002638fc
    int GetBudgetSegment(std::uint32_t image, int x, int y, cIGZWin* parent);   // 0x0026279a
    void SetupWindowBig(cIGZWin* window, std::uint32_t incomeRows, std::uint32_t expenseRows);   // 0x002658fe
    void SetEmptyWindow(cIGZWin* window, const cIGZString& title, const cIGZString& text);       // 0x00265ef4
    void AddText(cIGZWin* parent, std::uint32_t id, int x, int y, const cIGZString& text, int align, std::uint32_t font,
                 std::uint8_t r, std::uint8_t g, std::uint8_t b);      // add_text 0x002616ca
    void AddSlider(cIGZWin* parent, std::uint32_t id, int x, int y, int width, int direction, int minimum, int maximum, int value);   // 0x002613ec
    void AddImageButton(cIGZWin* parent, std::uint32_t id, int x, int y, std::uint32_t image, bool framed);   // add_image_button2 0x002620f8
    void AddBitmap(std::uint32_t image, int x, int y, cIGZWin* parent, std::uint32_t id);              // add_Bitmap 0x002625d2
    void ReleaseDepartments(std::vector<cSC4DepartmentBudget*>& departments);
    void Departments(std::vector<cSC4DepartmentBudget*>& out);          // the purpose's group (GetAllGroups + GetDepartmentBudgetsInGroup)

    std::uint32_t refs_ = 0;
    bool initialized_ = false;                   // +0x08
    cIGZWin* dialog_ = nullptr;                  // +0x0c (the dialog window, one reference)
    cIGZWin* body_ = nullptr;                    // +0x10 (the inner window gen that holds the rows, one reference)
    std::uint32_t purpose_ = 0;                  // +0x20 (the budget group)
    cIGZWin* individual_ = nullptr;              // +0x24 (non-null: one building's dialog)
    std::uint32_t period_ = 0;                   // +0x28 (0 monthly, else yearly)
    std::vector<SliderInfo> sliders_;            // +0x2c
    std::vector<BudgetRestoreInfo> restore_;     // +0x38
    int height_ = 0;                             // +0x80 (the stacked strips)
    int width_ = 0;                              // +0x84
    int rowStep_ = 0;                            // +0x88 (half the body strip)
    std::int64_t total_ = 0;                     // +0x8c
    int incomeTop_ = 0, expenseTop_ = 0;         // +0x94 / +0x98
    std::int64_t multiplier_ = 1;                // +0xa8 (1 monthly, 12 yearly)
    cRZString incomeTitle_, incomeHeader_, expenseTitle_, expenseHeader_, totalCaption_;   // +0xb0 .. +0xe0
    std::uint32_t incomeScroll_ = 0, expenseScroll_ = 0;   // +0xec / +0xf0
    std::uint32_t incomeRows_ = 0, expenseRows_ = 0;       // +0xf4 / +0xf8
    bool updating_ = false;                      // +0xfc
    std::uint32_t result_ = 0;                   // the end_dialog code
    std::uint32_t expenseTotalText_ = 0;         // port: the expense total's money text
    struct ItemRow { std::uint32_t moneyText = 0; cSC4DepartmentBudget* department = nullptr; std::uint32_t item = 0; };
    std::vector<ItemRow> itemRows_;              // port: the expense item rows UpdateValues re-captions
};

}  // namespace sc4
