// cSC4MySimWinProc — the window procedure of the MySim panel (28 functions at 0x100315330..0x10031b5c2 plus the two
// game-data callbacks s_sim_select_car / s_sim_select_pedestrian at 0x100315dda/0x100315e46). cSC4View3DWin::AddGUI
// creates it for the script window {0, 0x96a006b0, 0xca1f1f??} (the "mysims_ui" panel), hands it the cIGZWinGen view of
// that window and attaches it with cIGZWinGen::SetWinProc.
//
// The panel drives the five MySim slots the player owns. Its children live in the panel's *parent* window (the GUI root):
// the five name buttons, their portraits (0x22220000..+4) and the two button variants 0xca9df380.. and 0x8a8f410c.., the
// small/large/stat sub-panels and the advice list. Selecting a slot tells the MySim agent simulator
// (cISC4MySimAgentSimulator, SL::spCity vslot 0x238) which sim the UI tracks and refreshes the transport button images
// (car / pedestrian). The large/small panels show the sim's advice list (cSC4WinAdviceList, iid 0xca1492a2); the stat
// panel shows name, wealth (§ per level), zodiac, age, home, job, EQ/HQ and the bio. The transport dialog is a separate
// script window whose buttons list the vehicle or pedestrian model exemplars, page by page (6 page buttons, 0x1c icons
// per page for vehicles and 0xe for pedestrians), filtered by wealth (vehicles) or sex (pedestrians) and sorted by the
// exemplar's sort-order property; its result sets the sim's car or pedestrian model.
//
// Original layout (0xd8 bytes, allocated by AddGUI; the first three vtables come from the three interfaces):
//   +0x00 cIGZWinProc vptr        +0x08 cIGZMessageTarget2 vptr   +0x10 cRZUnknown (refcount +0x18)
//   +0x20 cIGZWinGen* panel (referenced)
//   +0x28 cIGZUnknown* (referenced and released only; the original never stores anything in it)
//   +0x30 cIGZWinAdviceList* advice list control (referenced; the large/small body)
//   +0x38..+0x58 cISC4MySim* sims[5] (referenced)
//   +0x60 cSC4PickSimWinProc* pick-sim dialog (referenced; transient, see AddSim)
//   +0x68 cISC4MySimAgentSimulator* agent simulator (reference taken lazily by GetCurrentSims)
//   +0x70 std::list<cRZAutoRefCount<cISC4Advice>> shown advices
//   +0x88 cSC4Advisor::AdviceMap advice -> index of the current sim
//   +0xa0 u32 current sim (0..4)   +0xa4 u32 number of sims found
//   +0xa8 u32 panel mode (0 none, 1 small/large, 2 stats)   +0xac u32 0 small / 1 large
//   +0xb0 bool shown   +0xb4 u32 number of model icons   +0xb8 u32 icons per page
//   +0xbc i32 icon page   +0xc0 i32 first page button's page   +0xc4 u32 selected icon button
//   +0xc8 u32 wealth filter index (-1 = all? see ShowChooseTransportDialog)   +0xcc bool pedestrian filter
//   +0xd0 cISCResExemplar* exemplar of the selected icon button (referenced)
//
// Quirks kept (addresses are the original ones):
//  * Every command from the panel first ends a pending move-in on the agent simulator (0x1003162f2: vslot 0x50), even
//    commands that have nothing to do with moving in.
//  * DoWinProcMessage answers false for every command coming from the panel itself and true only for the commands of the
//    transport dialog (0x10031678e vs 0x100316791); the panel's commands therefore reach the other window procedures too.
//  * The 0x4bba8aa1 command reads the current sim without checking it exists (0x1003163e4): with no sim in the slot it
//    dereferences null. The port keeps the call but lets a null sim skip it (see the .cpp).
//  * DisplayVehicleIcons divides by the icons-per-page value (+0xb8) without guarding against zero (0x100316a0d); only
//    ShowChooseTransportDialog writes it, so the division can only be zero if the helper is called some other way.
//  * DisplayVehicleIcons inserts one map node per matching exemplar keyed by the sort property, so two exemplars with the
//    same sort key collapse into one entry (the original's inlined tree insert does not test for equality; the port uses
//    std::map semantics, i.e. the last exemplar with a duplicate key wins).
//  * ShowChooseTransportDialog keeps the reference cIGZWin::GetParam took on the button's exemplar variant (0x1003186b2
//    has no Release); the port releases it.
//  * The pick-sim dialog (AddSim, 0x10031b06a) is created, Init'ed and immediately Shutdown and Released: Init is the
//    modal "pick a sim" loop, the sim is chosen by the time it returns.
//  * Shutdown ends with two Release calls (0x100315f41 and 0x100315fd8): it takes a reference on entry and drops it plus
//    the caller's, so the object frees itself.
//  * Show() sets the dialog state 0xca1f1d9c (main) but leaves the "main panel visible" child hidden (0xea1f1e4e, which
//    SetMySimModeWindow is what shows); the two calls look swapped but are what the binary does.
//
// Every global the original reaches (SL::spMS2, SL::spCity, SL::spSC4, SL::spWinLocationSaver, SL::spVehicleManager,
// SL::spPedestrianManager, RZ::WM(), RZGetFramework's game-data registry and SC4PostMessage2) goes through
// IMySimWinProcHost; the unrecovered neighbours (cISC4MySimAgentSimulator, cSC4WinAdviceList, cSC4PickSimWinProc and
// the vehicle/pedestrian model lists) are the small I* seams below.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include "gz/WinBtn.h"
#include "gz/WinGen.h"
#include "gz/WinInterfaces.h"
#include "gz/WinProc.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZVariant.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "rz/cRZUnknown.h"
#include "sc4/Advice.h"
#include "sc4/Advisor.h"
#include "sc4/AutoHelpers.h"
#include "sc4/Exemplar.h"
#include "sc4/MySim.h"
#include "sc4/SLBuffer.h"

namespace sc4 {

class cISC4GameDataRegistry;

// The panel's three control-id families and the script windows it drives (all values are the original ids).
constexpr std::uint32_t kMySimPanelWindow = 0x698894d3;       // the panel frame shown by Show, hidden by Hide
constexpr std::uint32_t kMySimMainWindow = 0xea1f1e4e;        // the panel frame shown by SetMySimModeWindow
constexpr std::uint32_t kMySimDialogMain = 0xca1f1d9c;        // SetDialogState: main page
constexpr std::uint32_t kMySimDialogSmall = 0xea1f1e4d;       // small page
constexpr std::uint32_t kMySimDialogLarge = 0xaa1f1ec5;       // large page
constexpr std::uint32_t kMySimDialogStat = 0xea1f1e5e;        // stats page
constexpr std::uint32_t kMySimDialogStatAlt = 0x6a61e29f;     // stats page (the second id SetMySimStatWindow uses)
constexpr std::uint32_t kMySimDialogTransport = 0xabbaa2d3;   // transport dialog page
constexpr std::uint32_t kMySimBodyLarge = 0x6a1f1f4a;         // advice list in the large page
constexpr std::uint32_t kMySimBodySmall = 0xaa1f1eb5;         // advice list in the small page
constexpr std::uint32_t kMySimPortrait = 0x22220055;          // the portrait bitmap of the stat page
constexpr std::uint32_t kMySimNameBitmapBase = 0x22220000;    // + slot: portrait of each of the five slots
constexpr std::uint32_t kMySimNameCaption = 0xaa1f1e6a;       // four captions refreshed from the personal name
constexpr std::uint32_t kMySimNameCaption2 = 0xaa1f1e7b;
constexpr std::uint32_t kMySimNameCaption3 = 0x8a1f1efa;
constexpr std::uint32_t kMySimNameCaption4 = 0x8a1f1eff;
constexpr std::uint32_t kMySimTransportCaption = 0x3a1f1efb;
constexpr std::uint32_t kMySimTransportCaption2 = 0xabf32693;
constexpr std::uint32_t kMySimStatWealth = 0x8a1f1f00;
constexpr std::uint32_t kMySimStatSign = 0x8a1f1f01;
constexpr std::uint32_t kMySimStatAge = 0x8a1f1f02;
constexpr std::uint32_t kMySimStatHome = 0x8a1f1f03;
constexpr std::uint32_t kMySimStatJobTitle = 0x8a1f1f04;
constexpr std::uint32_t kMySimStatJob = 0x8a1f1f05;
constexpr std::uint32_t kMySimStatEQ = 0x8a1f1f06;
constexpr std::uint32_t kMySimStatHQ = 0x8a1f1f07;
constexpr std::uint32_t kMySimStatBio = 0x8a243f6e;
constexpr std::uint32_t kMySimTransportCarBmp = 0x2bbaaa69;   // + 8 when the pedestrian image is wanted
constexpr std::uint32_t kMySimWealthButtonBase = 0x2c201cb0;  // three wealth-filter buttons (+0..+2)
constexpr std::uint32_t kMySimIconButtonBase = 0x34560000;    // model icon buttons of the current page
constexpr std::uint32_t kMySimIconBitmapBase = 0x23450000;    // their bitmap windows
constexpr std::uint32_t kMySimPageButtonBase = 0xcc07ea2c;    // six page buttons (+1..+6)
constexpr std::uint32_t kMySimTransportDialogId = 0xcbf32603;
constexpr std::uint32_t kMySimTransportWindowVehicles = 0x4bf325e8;
constexpr std::uint32_t kMySimTransportWindowPedestrians = 0xabfaef15;
constexpr std::uint32_t kMySimIconsPerPageVehicles = 0x1c;
constexpr std::uint32_t kMySimIconsPerPagePedestrians = 0xe;
constexpr std::uint32_t kMySimIconImageGroup = 0x4c06f888;     // the icon button bitmaps
constexpr std::uint32_t kMySimPortraitImageGroup = 0x46a006b0; // the portrait bitmaps
constexpr std::uint32_t kMySimPlaceholderPortrait = 0x13f1525c; // image shown for an empty slot
constexpr std::uint32_t kMySimAdviceImage = 0x14416264;        // background image of the advice list
constexpr std::uint32_t kMySimExemplarParam = 0x8c0855d4;      // cIGZWin::SetParam key of an icon button
constexpr std::uint32_t kMySimPropertyWealth = 0xebfc5e26;     // int, the model's wealth level
constexpr std::uint32_t kMySimPropertyMale = 0x2c0c922e;       // int, 0 = male pedestrian model
constexpr std::uint32_t kMySimPropertySortOrder = 0xebfc5fba;  // int, model sort order
constexpr std::uint32_t kMySimPropertyName = 0x899afbad;       // string, model name
constexpr std::uint32_t kMySimPropertyNameAlt = 0x8a416a99;    // exemplar string id used when that is absent
constexpr std::uint32_t kMySimPropertyNameAlt2 = 0x20;         // generic name property
constexpr std::uint32_t kMySimPropertyBitmap = 0xebfc5e5e;     // int, the model's bitmap id
constexpr std::uint32_t kMySimTransportExemplarIid = 0xa52160f5;   // cISCResExemplar iid
constexpr std::uint32_t kMySimAdviceIid = 0x8a09f5f5;          // iid of a cISC4Advice in a message payload

// id of the cIGZWinAdviceList view of cSC4WinAdviceList (QueryInterface @ 0x1005c79e2).
constexpr std::uint32_t kGZIID_cIGZWinAdviceList = 0xca1492a2;

// Messages the panel subscribes to (Init) and handles (DoMessage).
constexpr std::uint32_t kMySimMsgAdviceAdded = 0x8a09fa64;
constexpr std::uint32_t kMySimMsgAdviceChanged = 0x8a09fa65;
constexpr std::uint32_t kMySimMsgAdviceRemoved = 0xca09fa6c;
constexpr std::uint32_t kMySimMsgCarChosen = 0xac08746e;
constexpr std::uint32_t kMySimMsgPedestrianChosen = 0xac08746f;
constexpr std::uint32_t kMySimMsgForwardToAdviceList = 0xe4a85d91;   // type of the message forwarded to the body list
constexpr std::uint32_t kMySimCommandSender = 0x287259f6;             // cIGZWinBtn's command sender class

// The zodiac sign string instances (mSign @ 0x100a1d230, group 0x6a231eaa used by SetMySimStatWindow).
constexpr std::uint32_t kMySimSignGroup = 0x6a231eaa;
constexpr std::uint32_t kMySimSigns[12] = {0xea822e90, 0xca822e93, 0xca822e97, 0xaa822e9b, 0xca822ea4, 0xaa822ea8,
                                           0xca822eac, 0xca822eaf, 0xca822eb3, 0x6a822eb6, 0x6a822eb9, 0xea822ebd};

// The per-slot control ids (mNameBitmapsID / mNameButtonsID / mNameButtonsIDN / mNameButtonsIDE / mNameTextID,
// 0x100a1d1b0..0x100a1d260).
constexpr std::uint32_t kMySimNameBitmaps[5] = {0x8a1f1df3, 0x8a1f1e09, 0xea22edcc, 0x2a22eddb, 0x8a22edec};
constexpr std::uint32_t kMySimNameButtons[5] = {0x8a1f1dc3, 0x8a1f1dcc, 0xaa1f1dd6, 0x6a1f1de0, 0x4a1f1dea};
constexpr std::uint32_t kMySimNameButtonsNew[5] = {0xca9df384, 0xca9df383, 0xca9df382, 0xca9df381, 0xca9df380};
constexpr std::uint32_t kMySimNameButtonsEmpty[5] = {0x8a8f410c, 0x8a8f410d, 0x8a8f410e, 0x8a8f410f, 0x8a8f4120};
constexpr std::uint32_t kMySimNameTexts[5] = {0x2a1f1dfe, 0x0a1f1e12, 0x6a22edd3, 0x0a22ede4, 0xea22edf5};

// cSC4WinAdviceList's cIGZWinAdviceList view (iid 0xca1492a2). Only the five calls the panel makes are modelled; the
// slot of each one is the original vtable offset (cSC4WinAdviceList::AddEntry @ 0x1005c806e, RebuildHTML @ 0x1005c82a0,
// SetbgImage @ 0x1005c828a, ClearList @ 0x1005c8828).
class IWinAdviceList : public cIGZUnknown {
public:
    virtual void AddEntry(const cIGZString& a, const cIGZString& b, const cIGZString& c, const cIGZString& d,
                          bool enabled, cISC4Advice* advice, const cIGZString& e, bool f, std::uint32_t image) = 0;  // 0x20
    virtual void RebuildHTML(bool force) = 0;             // 0x28
    virtual void SetbgImage(std::uint32_t image) = 0;     // 0x30
    virtual void ClearList() = 0;                         // 0x38
    virtual void ScrollToAnchor(const cIGZString& anchor) = 0;   // 0x40
    virtual void DisableVerticalScrollbar() = 0;          // 0x48
    virtual cIGZWin* AsIGZWin() = 0;                      // 0x18 (first method of the interface)
};

// cISC4MySimAgentSimulator (the object SL::spCity vslot 0x238 answers; cSC4MySimAgentSimulator). Only the five calls the
// panel makes are modelled: vslot 0x28 GetActiveMySims @ 0x1007ad03e, 0x38 TrackLocation @ 0x1007ad24c,
// 0x40 HideLocation @ 0x1007ad2ce, 0x50 CancelMoveIn @ 0x1007ad37a, 0x58 SetActiveUIPanelSim @ 0x1007ad144.
class IMySimAgentSimulator : public cIGZUnknown {
public:
    virtual void GetActiveMySims(std::vector<cRZAutoRefCount<cISC4MySim>>& out) = 0;   // 0x28
    virtual void TrackLocation(cISC4MySim* sim) = 0;                                   // 0x38
    virtual void HideLocation() = 0;                                                   // 0x40
    virtual bool MySimLivesHere(cISC4BuildingOccupant* building) = 0;                  // 0x48
    virtual void CancelMoveIn() = 0;                                                   // 0x50
    virtual void SetActiveUIPanelSim(int index) = 0;                                   // 0x58
    virtual void GetActiveUIPanelSim(int& index) = 0;                                  // 0x60
};

// cSC4PickSimWinProc (ctor @ 0x1003549dc, object size 0xa8, Init @ 0x100354b88 — the modal pick loop — and
// Shutdown @ 0x10035735c). Not recovered yet; the panel only creates, Init's, Shutdown's and releases it.
class IPickSimWinProc : public cIGZUnknown {
public:
    virtual bool Init() = 0;       // 0x100354b88
    virtual bool Shutdown() = 0;   // 0x10035735c
};

// cSC4VehicleManager / cSC4PedestrianManager as far as DisplayVehicleIcons needs them: the model exemplars of the
// manager's automata-model list (vslot 0xe0 answers a std::list whose 0x88-byte nodes keep a counted cISCResExemplar at
// +0x48). The pointers stay owned by the manager; the panel reads the exemplar's property holder (AsISCPropertyHolder,
// vslot 0x30) and puts the exemplar itself into each icon button's param key 0x8c0855d4 (as a cRZVariant).
class IModelManager : public cIGZUnknown {
public:
    virtual void ModelExemplars(std::vector<cISCResExemplar*>& out) = 0;   // vslot 0xe0
};

// What the panel needs from the running game (the original global / call chain is in the comment).
class IMySimWinProcHost {
public:
    virtual ~IMySimWinProcHost() = default;
    virtual cIGZMessageServer2* MessageServer() = 0;                     // SL::spMS2
    virtual cISC4GameDataRegistry* GameDataRegistry() = 0;               // RZGetFramework 0xebe0e860 / 0xe898ed03
    virtual IMySimAgentSimulator* MySimAgentSimulator() = 0;             // SL::spCity vslot 0x238 (borrowed)
    virtual IModelManager* VehicleManager() = 0;                         // SL::spVehicleManager (borrowed)
    virtual IModelManager* PedestrianManager() = 0;                      // SL::spPedestrianManager (borrowed)
    virtual void RestoreWindowLocation(cIGZWin* window) = 0;             // SL::spWinLocationSaver vslot 0x48
    virtual void SaveWindowLocation(cIGZWin* window) = 0;                // SL::spWinLocationSaver vslot 0x28
    virtual bool MySimDirectory(cIGZString& out) = 0;                    // SL::spSC4 vslot 0x1d8 (i386 0xec) GetMySimDirectory
    virtual IPickSimWinProc* CreatePickSimWinProc(std::uint32_t index) = 0;
        // cSC4PickSimWinProc ctor @ 0x1003549dc: a fresh object with no reference yet (like cRZUnknown's ctor); AddSim
        // takes the reference and releases it after the modal Init loop.
    virtual cIGZWinMgr* WindowManager() = 0;                             // RZ::WM()
    // SC4CreateWindowFromScript(key, parent, id, iid, out) @ 0x100039bf7 (the UI script service); a cIGZWinGen* here.
    virtual bool CreateTransportDialog(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                       std::uint32_t iid, void** out) = 0;
    // The two tool wrappers of the concrete cSC4MySim (the original takes the cISC4MySim view back to +0x148):
    // cSC4MySim::CreateChangeHouseTool @ 0x1007aaab4 and CreateChangeJobTool @ 0x1007aab9e.
    virtual bool CreateChangeHouseTool(cISC4MySim* sim) = 0;
    virtual bool CreateChangeJobTool(cISC4MySim* sim) = 0;
    virtual void PostMessage2(std::uint32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3,
                              std::uint32_t data4, cIGZUnknown* unknown) = 0;   // SC4PostMessage2 @ 0x10003a5ba
    virtual bool ExemplarString(cRZString& out, cISCPropertyHolder* holder, std::uint32_t property) = 0;
        // SC4LoadExemplarString @ 0x100121132 (the loader the occupant code installs with SetExemplarStringLoader)
};
IMySimWinProcHost* MySimWinProcHost();
void SetMySimWinProcHost(IMySimWinProcHost* host);

// The two game-data ("pop_select_*_dialog") value callbacks registered by Init. Both answer true forever and, on a read
// (write == false) with a context, post the matching "model chosen" message and set the variant to true.
bool s_sim_select_car(void* handle, cIGZVariant& value, bool write, cISC4GameDataRegistry* registry);        // 0x100315dda
bool s_sim_select_pedestrian(void* handle, cIGZVariant& value, bool write, cISC4GameDataRegistry* registry); // 0x100315e46

class cSC4MySimWinProc final : public gz::cIGZWinProc, public cIGZMessageTarget2, public cRZUnknown {
public:
    explicit cSC4MySimWinProc(gz::cIGZWinGen* panel);   // @ 0x100315330
    ~cSC4MySimWinProc() override;                       // @ 0x100315536

    // cIGZUnknown — one reference count behind the three views (QI 0x22e85d8e / 0x452294aa; everything else: unknown).
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1003160f6
    std::uint32_t AddRef() override { return cRZUnknown::AddRef(); }
    std::uint32_t Release() override { return cRZUnknown::Release(); }

    // gz::cIGZWinProc
    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x100316192

    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;                         // @ 0x1003184ac

    void Init();                                         // @ 0x10031571e
    bool Shutdown();                                     // @ 0x100315eb2 (releases the final reference)
    void SetDialogState(std::uint32_t state);            // @ 0x100315b20

    void DisplayVehicleIcons(cIGZWin* dialog);           // @ 0x100316900
    void SetMySimMainWindow();                            // @ 0x100317168
    void SetMySimModeWindow();                            // @ 0x100317338 (Ghidra models this as taking the pointer)
    void SetMySimLargeWindow();                           // @ 0x1003176de
    void SetMySimSmallWindow();                           // @ 0x1003177ec
    void BootSim();                                       // @ 0x1003178f4
    void SetMySimStatWindow();                            // @ 0x100317956
    void ShowChooseTransportDialog(bool pedestrian);      // @ 0x100317eb8
    void SetCurrentSim(int index, bool showMode, bool addIfMissing);   // @ 0x1003183e0
    void GetCurrentSims();                                // @ 0x10031873a
    void updateBodyTextCtrl();                            // @ 0x100318e14
    void RemoveMessage(cISC4Advice* advice);              // @ 0x10031af44
    void AddMessage(cISC4Advice* advice);                 // @ 0x10031afda
    void AddSim(std::uint32_t index);                     // @ 0x10031b06a
    void SetTransportButtonImage(std::uint32_t bitmapId, bool pedestrian);   // @ 0x10031b158
    void GetMessageList(cISC4Advisor* advisor);                            // @ 0x10031b22a
    void Hide();                                          // @ 0x10031b4d8
    void Show();                                          // @ 0x10031b54a

    // State (for the test and for the sibling panels).
    gz::cIGZWinGen* Panel() const { return panel_; }
    IWinAdviceList* AdviceList() const { return adviceList_; }
    cISC4MySim* Sim(int index) const { return (index >= 0 && index < 5) ? sims_[index] : nullptr; }
    std::uint32_t CurrentSim() const { return currentSim_; }
    std::uint32_t SimCount() const { return simCount_; }
    std::uint32_t Mode() const { return mode_; }
    std::uint32_t Large() const { return large_; }
    bool Shown() const { return shown_; }
    bool PedestrianFilter() const { return pedestrianFilter_; }
    const std::list<cRZAutoRefCount<cISC4Advice>>& Messages() const { return messages_; }
    const AdviceMap& MessageMap() const { return messageMap_; }
    std::uint32_t IconsPerPage() const { return iconsPerPage_; }
    std::int32_t IconPage() const { return iconPage_; }
    std::uint32_t TotalIcons() const { return totalIcons_; }
    std::uint32_t SelectedIconButton() const { return selectedIconButton_; }
    std::uint32_t WealthFilter() const { return wealthFilter_; }
    void SetPanelForTest(gz::cIGZWinGen* panel);

private:
    // The control container: the panel window's parent (the original calls GetParentWin() before every lookup).
    cIGZWin* Controls() const;
    gz::cIGZWinGen* panel_ = nullptr;         // +0x20 (referenced)
    cIGZUnknown* unknown28_ = nullptr;        // +0x28 (referenced; never used)
    IWinAdviceList* adviceList_ = nullptr;    // +0x30 (referenced)
    cISC4MySim* sims_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};   // +0x38..+0x58 (referenced)
    IPickSimWinProc* pickSim_ = nullptr;      // +0x60 (referenced)
    IMySimAgentSimulator* agentSim_ = nullptr; // +0x68 (referenced)
    std::list<cRZAutoRefCount<cISC4Advice>> messages_;   // +0x70
    AdviceMap messageMap_;                                // +0x88
    std::uint32_t currentSim_ = 0;            // +0xa0
    std::uint32_t simCount_ = 0;              // +0xa4
    std::uint32_t mode_ = 1;                  // +0xa8
    std::uint32_t large_ = 0;                 // +0xac
    bool shown_ = false;                      // +0xb0
    std::uint32_t totalIcons_ = 0;            // +0xb4
    std::uint32_t iconsPerPage_ = kMySimIconsPerPageVehicles;   // +0xb8
    std::int32_t iconPage_ = 0;               // +0xbc
    std::int32_t iconPageOffset_ = 0;         // +0xc0
    std::uint32_t selectedIconButton_ = kMySimIconButtonBase;   // +0xc4
    std::uint32_t wealthFilter_ = 0;          // +0xc8
    bool pedestrianFilter_ = false;           // +0xcc
    cISCResExemplar* transportExemplar_ = nullptr;   // +0xd0 (referenced)
};

}  // namespace sc4
