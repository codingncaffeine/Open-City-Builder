// cSC4SubBudgetWindow — the department budget dialog the budget panel opens for one budget group (Mac i386
// 0x002613ec..0x00279028; decompile in scratch/s52/ghidra/subbudget.c, notes in scratch/s52/subbudget-notes.md).
// The dialog is built in code: UI art strips stacked by GetBudgetSegment (SetupWindowBig), then one row per department
// line: income rows (name, money) and expense rows (a button, the name, the cost, a funding bar image and a funding slider).
// Moving a slider sets the department's funding and refreshes the rows (UpdateValues); OK keeps the changes, Cancel / Esc
// restores the funding the dialog opened with.
//
// It is a cIGZWinProc on the dialog window (DoWinMsg: init 0xa2bf8ad0, shutdown 0xa2bf8ad1, key 5, command 3).
// The neighbor deal dialog (purpose 2) lists every deal the neighbors simulator proposes or runs, sales as income and
// purchases as expenses, each with a combo box of amounts (none / cancel first); OK keeps the deals, Cancel restores them.
//   PARTIAL (see the notes): the department and ordinance paths' row scrolling is not ported yet.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "gz/WinProc.h"
#include "rz/cRZAutoRefCount.h"
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
class cIGZWinCombo;
}  // namespace gz

namespace sc4 {

class cSC4BudgetSimulator;
class cSC4DepartmentBudget;
class cSC4OrdinanceSimulator;
class cSC4NeighborsSimulator;
class cISC4NeighborDeal;
class cISCPropertyHolder;

// What the original reached through globals: spCity vslot 0xd0 (the budget simulator), RZ::WCM() (controls and fonts),
// RZ::GS() (colours), cSLBuffer (UI art) and the window manager.
class ISubBudgetWindowHost {
public:
    virtual ~ISubBudgetWindowHost() = default;
    virtual cSC4BudgetSimulator* BudgetSimulator() = 0;
    virtual cSC4OrdinanceSimulator* OrdinanceSimulator() { return nullptr; }   // SL::spOrdinanceSim
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
    // spCity vslot 0xf8 / SL::spNeighborsSim: the neighbors simulator (borrowed), or null.
    virtual cSC4NeighborsSimulator* NeighborsSimulator() { return nullptr; }
    // GZCOM()->CreateInstance(0x59b, 0x412ce496): a new cGZWinCombo (one reference), or null.
    virtual gz::cIGZWinCombo* CreateComboBox() { return nullptr; }
    // SL::spRM GetResource({0x6534284a, group, instance}, 0xa52160f5): the exemplar's properties (borrowed), or null.
    virtual cISCPropertyHolder* Exemplar(std::uint32_t group, std::uint32_t instance) { (void)group; (void)instance; return nullptr; }

    // ---- the single-building dialog (budget-window-bodies.md section 5)
    // The department's buildings of one service kind (department slot 0x80), in its order, one reference each.
    virtual void LocallyFundedBuildings(std::uint32_t department, std::uint32_t kind, std::vector<cISCPropertyHolder*>& out) {
        (void)department; (void)kind; out.clear();
    }
    virtual float DepartmentMaxFunding(std::uint32_t department) { (void)department; return 1.0f; }   // department slot 0x30
    // The raw service queries of one building (section 5.3): which ones answered, and what.
    struct BuildingService {
        bool residential = false;   // hospital / school: the residential simulator's query (slot 27 / 26)
        int served = 0, capacity = 0;
        float staffFraction = 0, vehicleFraction = 0;
        std::uint32_t staffCost = 0, vehicleCost = 0;
        bool station = false;       // police / fire: the efficiency, then the station's cost and funding
        float efficiency = 0, funding = 0;
        std::int32_t baseCost = 0;
        int numerator = 0, denominator = 0;   // power: the plant's output; then its funding, then its cost
        bool powerFunding = false, powerCost = false;
        std::int64_t powerBaseCost = 0;
    };
    virtual bool QueryBuildingService(cISCPropertyHolder* holder, std::uint32_t kind, BuildingService& out) {
        (void)holder; (void)kind; (void)out; return false;
    }
    virtual bool BuildingName(cISCPropertyHolder* holder, cRZString& out) { (void)holder; (void)out; return false; }   // QI 0x87dda3a9, slot 19
    virtual bool HasBudgetItem(cISCPropertyHolder* holder, std::uint32_t purpose) { (void)holder; (void)purpose; return false; }   // budget slot 51
    // SetOccupantFundingLevel's simulator calls: the building's (secondary) funding of the kind, as a fraction.
    virtual bool SetBuildingFunding(cISCPropertyHolder* holder, std::uint32_t kind, bool secondary, float fraction) {
        (void)holder; (void)kind; (void)secondary; (void)fraction; return false;
    }
    virtual void MoneyString(std::int64_t amount, cRZString& out) { (void)amount; out = cRZString(); }   // SC4MakeMoneyString
    // SL::spSimulator HiddenPause (i386 vslot 0x28, on_init) / HiddenResume (vslot 0x34, DestroySubBudgetWindow): the simulation
    // holds while the dialog is open.
    virtual void SimulatorHiddenPause(bool pause) { (void)pause; }
    // a building row's name button: the camera looks at the building (QI 0x26bb767e slot 8 GetPosition, renderer slot 0x40
    // camera control SetViewTargetPosition)
    virtual bool LookAtBuilding(cISCPropertyHolder* holder) { (void)holder; return false; }
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
    bool Create(cIGZWin** window, std::uint32_t purpose, std::uint32_t period, std::uint32_t department) override;   // CreateSubBudgetWindow 0x00265358
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
        std::uint32_t moneyText = 0;   // port: the row's money text
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
    void SetupWindow(cIGZWin* window, std::uint32_t incomeRows, std::uint32_t expenseRows);      // 0x002655a4
    void StackSegments(cIGZWin* window, std::uint32_t first, std::uint32_t incomeRows, std::uint32_t expenseRows);
    void InitOrdinanceBudget(cIGZWin* window);                          // initOrdinanceBudget 0x002678fe
    void UpdateOrdinanceBudget(cIGZWin* window);                        // updateOrdinanceBudget 0x002628da (the money texts)
    void AddCheckButton(cIGZWin* parent, std::uint32_t id, int x, int y, std::uint32_t image, bool checked);   // add_image_button1 0x00261e94
    // the two-panel popup of SetEmptyWindow and the ordinance info (i386 0x00265ef4 / on_button_activated 500+n)
    void MakePopupPanels(cIGZWin* window, bool ordinanceStyle);
    void ClosePopup(cIGZWin* window);
    void ShowOrdinanceInfo(cIGZWin* window, std::uint32_t row);
    void SetEmptyWindow(cIGZWin* window, const cIGZString& title, const cIGZString& text);       // 0x00265ef4
    void AddText(cIGZWin* parent, std::uint32_t id, int x, int y, const cIGZString& text, int align, std::uint32_t font,
                 std::uint8_t r, std::uint8_t g, std::uint8_t b);      // add_text 0x002616ca
    void AddSlider(cIGZWin* parent, std::uint32_t id, int x, int y, int width, int direction, int minimum, int maximum, int value);   // 0x002613ec
    void AddImageButton(cIGZWin* parent, std::uint32_t id, int x, int y, std::uint32_t image, bool framed);   // add_image_button2 0x002620f8
    void AddBitmap(std::uint32_t image, int x, int y, cIGZWin* parent, std::uint32_t id);              // add_Bitmap 0x002625d2
    void ReleaseDepartments(std::vector<cSC4DepartmentBudget*>& departments);
    // ---- neighbor deals (purpose 2)
    void InitNeighborBudget(cIGZWin* window);                           // initNeighborBudget 0x0026a2e2
    void UpdateNeighborBudget(cIGZWin* window);                         // updateNeighborBudget 0x0027385e
    void LayoutNeighborRows(cIGZWin* window, bool create);              // the row walk both share
    void AddComboBox(cIGZWin* parent, std::uint32_t id, int x, int y);  // add_comboBox 0x002691ee
    // set_comboBox 0x002639be: the amounts the deal can take; `mode` 0 power, 1 water, 2 garbage; `state` 0 not running,
    // 1 running, 2 the opposite deal with the same city is running (unavailable).
    void SetComboBox(cIGZWin* window, std::uint32_t id, std::uint32_t type, cISC4NeighborDeal* deal, int mode, int state);
    bool OnComboChanged(cIGZWin* window, std::uint32_t id);             // DoWinMsg's 0xe8711255 branch
    void RestoreDeals();                                                // 0x0026a0da
    void ReleaseDeals();                                                // DestroySubBudgetWindow's deal part 0x0026961a
    void Departments(std::vector<cSC4DepartmentBudget*>& out);          // the purpose's group (GetAllGroups + GetDepartmentBudgetsInGroup)
    // ---- the single-building dialog (+0x24 a department id; budget-window-bodies.md section 5)
    void InitIndividualBudget(cIGZWin* window);                         // initIndividualBudget 0x002666ba
    void UpdateIndividualBudget(cIGZWin* window, bool rebuild);         // updateIndividualBudget 0x002646a2
    void SetupWindowIndiv(cIGZWin* window, std::uint32_t rows);         // 0x00265c58
    bool SetOccupantFundingLevel(cISCPropertyHolder* holder, bool secondary, float percent);   // 0x0026364e
    void ReleaseIndividualRestore();
    void DoIndividualBudgetWindow(std::uint32_t department);           // 0x00269f00: the nested modal dialog
    void AddFixedText(cIGZWin* parent, std::uint32_t id, int x, int y, int width, const cIGZString& text, int align,
                      std::uint32_t font, std::uint8_t r, std::uint8_t g, std::uint8_t b);   // add_fixed_text 0x00261d18
    void AddTextButton(cIGZWin* parent, std::uint32_t id, int x, int y, int width, const cIGZString& text, int capStyle,
                       std::uint32_t font, std::uint8_t r, std::uint8_t g, std::uint8_t b);  // add_text_button 0x00261990

    std::uint32_t refs_ = 0;
    bool initialized_ = false;                   // +0x08
    cIGZWin* dialog_ = nullptr;                  // +0x0c (the dialog window, one reference)
    cIGZWin* body_ = nullptr;                    // +0x10 (the inner window gen that holds the rows, one reference)
    cIGZWin* popup_ = nullptr;                   // +0x14 the info popup's outer window (one reference)
    cIGZWin* popupInner_ = nullptr;              // +0x18 its inner window (one reference)
    std::uint32_t purpose_ = 0;                  // +0x20 (the budget group)
    std::uint32_t individual_ = 0;               // +0x24 the department id (non-zero: the single-building dialog)
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
    // the single-building dialog
    struct IndividualRestore { cISCPropertyHolder* holder = nullptr; float primary = 0.0f, secondary = 0.0f; };   // 12 bytes
    std::vector<IndividualRestore> individualRestore_;   // +0x44 (one reference per holder)
    cSC4SubBudgetWindow* child_ = nullptr;      // +0x1c the single-building dialog while it runs
    std::uint32_t kind_ = 0, secondKind_ = 0;    // +0x108 / +0x10c the service kind and its second budget item
    std::uint32_t listCount_ = 0;                // +0x128 (the last refresh's count)
    cRZString individualTitle_, secondColumn_;   // the dialog's title, the second slider column's header (+0xd4)
    std::vector<std::pair<std::uint32_t, cISCPropertyHolder*>> rowHolders_;   // port: the sliders' and names' building
                                                                             // (the original keeps it as window property 0x4bfee088)
    // ordinances (purpose 1): +0xa0 ids, +0xa4 (opening, current) on-pairs, +0x74 check button row -> ordinance index
    std::vector<std::uint32_t> ordinanceIds_;
    std::vector<std::pair<bool, bool>> ordinanceOn_;
    std::vector<std::uint32_t> ordinanceRows_;
    // neighbor deals: +0x50 the combo boxes (id, deal), +0x5c the running deals' (city serial, type), +0x68 every listed
    // deal (type, deal), +0x100 the list the simulator's queries fill
    struct ComboInfo { std::uint32_t id = 0; cRZAutoRefCount<cISC4NeighborDeal> deal; };
    struct NDList { std::uint32_t type = 0; cRZAutoRefCount<cISC4NeighborDeal> deal; };
    struct NDCityList { std::uint32_t city = 0; std::uint32_t type = 0; };
    std::vector<ComboInfo> combos_;
    std::vector<NDCityList> dealCities_;
    std::vector<NDList> dealList_;
    // port: the visible slots' control ids per side (text id of slot 0, bitmap / combo id of slot 0, slots made)
    struct DealSlots { std::uint32_t text = 0, bitmap = 0, combo = 0, total = 0; int count = 0, top = 0; };
    DealSlots incomeSlots_, expenseSlots_;
};

}  // namespace sc4
