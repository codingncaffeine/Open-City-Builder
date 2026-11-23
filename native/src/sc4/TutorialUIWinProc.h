// cSC4TutorialUIWinProc — the tutorial's instruction window and task driver (Mac i386 0x00084e9e..0x00088c51, x64
// 0x1000b697c..0x1000bbb4c; object 0xc0 bytes; window procedure view at +8 iid 0x22e85d8e, message target at +0xc iid
// 0x452294aa). The tutorial system creates it when a tutorial city's view starts.
//
// Init builds the window from UI script {0, 0x96a006b0, 0x0a2dd355} (window 0x4a35b0f2) under the main window, takes the
// tutorial system's Lua, reads every row of the script table "tutorialtasks" into a cSC4TutorialTask, puts an HTML control
// into the window's flat rectangle 0x0a35f767 for the instruction text, loads the two pointing arrows and starts the
// current task. Each task shows its instruction text, moves the camera / zoom, disables every button the task does not
// name and flashes the one it does, then waits for the game event that completes it:
//   1 zone a region, 2 lay a road across a region, 3 plop a building on a lot, 4 press a button, 5 build a lot,
//   0xc wait for a disaster to end, 0xd reach a population, 0x10 point at a window, 0x11 string power lines.
// A wrong attempt shows the task's "try again" text (a misplaced building is bulldozed); a right one moves to the next
// task; after the last one the tutorial ends (message 0xaa3840e6 to the tutorial system).
// Instruction texts may carry "<!-- legacy id=0x... winflag_visible=yes winflag_enabled=no href="..." -->" comments that
// show, enable or link a control of the window (the "next" button's href is sc4://action/next_tutorial).
//
// Quirks kept: finish_tutorial calls the task's EndTask twice; a dispatch event re-shows the old task's instruction text
// after moving on; the arrow index helpers write no bounds of their own.
#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "gz/WinInterfaces.h"
#include "gz/WinProc.h"
#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/TutorialSystem.h"
#include "sc4/TutorialTask.h"

class cISCLua;
class cIGZWinHTML;
namespace gz { class cIGZWinGen; }

namespace sc4 {

class cISC4WinCatalogView;
class cISC4Occupant;

// What the procedure reaches outside the window toolkit. Every method mirrors the original calls named beside it.
class ITutorialUIHost {
public:
    virtual ~ITutorialUIHost() = default;
    virtual cIGZWin* MainWindow() = 0;                                                    // RZ::WM() slot 3
    // SC4CreateWindowFromScript(key, parent, id, iid, out): true with one counted reference in *out.
    virtual bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                        std::uint32_t iid, void** out) = 0;
    virtual cIGZWinHTML* CreateHtmlControl() = 0;                     // GZCOM 0xaa12e5f5 / 0x4a11fd4a, one reference
    virtual cISC4TutorialSystem* TutorialSystem() = 0;                    // SL::spInstance (borrowed, may be null)
    // cISC4App slot 47 GetTutorialRegistry -> GetItem(guid): the item's first task (+8) and task count (+0xc).
    virtual bool TutorialItem(int guid, int& firstTask, int& taskCount) = 0;
    virtual cIGZMessageServer2* Messages() = 0;                           // SL::spMS2
    virtual cIGZWin* ViewWindow() = 0;                                    // SL::spView3D slot 3, null without a view
    virtual void SetViewZoom(int zoom) = 0;                               // cSC4View3DWin::SetZoom
    virtual void EndViewInput() = 0;                                      // RemoveCurrentViewInputControl(false)
    virtual bool PickTerrain(int x, int y, float out[3]) = 0;             // PickTerrain(x, y, out, false)
    virtual bool SimulatorPaused() = 0;                                   // cSC4Simulator::IsPaused
    virtual void ResumeSimulator() = 0;                                   // cSC4Simulator::Resume
    virtual void PauseSimulator() = 0;                                    // cSC4Simulator::Pause
    virtual void SetClockVisualTimeMode(int mode) = 0;                    // cSC424HourClock::SetVisualTimeMode
    virtual void RequestGoToRegionView(bool ask) = 0;                     // cISC4App slot 12
    virtual bool PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;          // cSC4City::PositionToCell
    virtual bool LotRectAtCell(int cellX, int cellZ, SC4TutorialRect& rect) = 0;        // GetLot(x, z, false)->GetBoundingRect
    virtual bool DispatchPosition(std::uint32_t id, float& x, float& z) = 0;            // GetDispatchInfo(id): +0x14 / +0x1c
    virtual int ResidentialPopulation() = 0;                              // cSC4ResidentialSimulator::GetPopulation
    virtual bool DisasterPlaying(std::uint32_t disasterId) = 0;           // GetDisasterManager(id) slot 10
    virtual void DemolishCells(const SC4TutorialRect& rect) = 0;          // cSC4Demolition::DemolishRegion(1, cells, 0, 1, ...)
    virtual bool DoURL(cIGZString* link) = 0;                             // cSC4HTMLLinkHandler::DoURL
    // The occupant a placement message (0x99ef1142, void1) carries:
    //   a finished network piece of `networkType` (flags 0x80000000 and 0x200000 clear): its cell, and whether it
    //   connects on edges 0 and 2 (or 1 and 3 when `tall`). -1 when it is not such a piece, else 0 / 1.
    virtual int NetworkPiece(cISC4Occupant* occupant, std::uint32_t networkType, bool tall, int& x, int& z) = 0;
    //   a power line (not a preview: its cell) or else a power pole (not a preview: its cell); false when neither.
    virtual bool PowerPieceCell(cISC4Occupant* occupant, int& x, int& z) = 0;
    //   a building: its type (QI 0x87dda3a9, slot 4); false when it is not a building.
    virtual bool BuildingType(cISC4Occupant* occupant, std::uint32_t& type) = 0;
    // The lot (QI 0xe818fced) a lot message (0xe8bda2e7, unknown) carries: its bounding rect.
    virtual bool LotBoundingRect(cIGZUnknown* lot, SC4TutorialRect& rect) = 0;
};
ITutorialUIHost* TutorialUIHost();
void SetTutorialUIHost(ITutorialUIHost* host);

constexpr std::uint32_t kTutorialUIWinProcIid = 0x22e85d8e, kTutorialUIMessageTargetIid = 0x452294aa;
constexpr std::uint32_t kTutorialUIScript = 0x0a2dd355, kTutorialUIWindowId = 0x4a35b0f2, kTutorialUIWindowIid = 0x5386d516;
constexpr std::uint32_t kTutorialUIHtmlRect = 0x0a35f767, kTutorialUIExitButton = 0x4a383da6;
constexpr std::uint32_t kTutorialUIExitScript = 0xeaaeec1b, kTutorialUIExitWindow = 0x6aaeec4a;
constexpr std::uint32_t kTutorialUIExitSave = 0x6aaeec41, kTutorialUIExitFinish = 0x6aaeec42, kTutorialUIExitCancel = 0x6aaeec43;
constexpr std::uint32_t kTutorialUITick = 0x4c05a5e6;
constexpr std::uint32_t kTutorialUIMsgZoned = 0x07699455, kTutorialUIMsgPlaced = 0x99ef1142, kTutorialUIMsgDispatch = 0xe94671eb,
                        kTutorialUIMsgDisaster = 0x894671f6, kTutorialUIMsgPopulation = 0x66956816,
                        kTutorialUIMsgPlaced2 = 0x99ef1143, kTutorialUIMsgLot = 0xe8bda2e7;
constexpr std::uint32_t kTutorialUILinkParam = 0x0a8fe6cb;     // a control's link (window param, a string variant)

class cSC4TutorialUIWinProc final : public ITutorialUIWinProc, public gz::cIGZWinProc, public cIGZMessageTarget2 {
public:
    cSC4TutorialUIWinProc();                                       // @ 0x00085f3a
    ~cSC4TutorialUIWinProc() override;                             // @ 0x00086004

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x00084e9e
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    // ITutorialUIWinProc
    void Init() override;                                          // @ 0x0008746c
    void Shutdown() override;                                      // @ 0x0008585e
    void handleButtons(std::uint32_t command) override;            // @ 0x00086e60
    void ShowUIHintsForUsers() override;                           // @ 0x00088920
    void HandleCatalogView(cISC4WinCatalogView* view) override;    // @ 0x000856fc
    cISC4TutorialTask* GetCurrentTask() override;                  // @ 0x00085420
    bool FilterLeftMouseClick(int& x, int& y, int flags) override; // @ 0x00085ac4
    int ResetTutorialNumAndIndex(int guid, int index) override;    // @ 0x000873d2
    void moveToNext() override;                                    // @ 0x00086b62
    bool OnTutorialEvent(std::uint32_t event) override;            // @ 0x00088484

    bool DoWinProcMessage(cIGZWin* window, cGZMessage& message) override;   // @ 0x00086ec4
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x000877d6

    // test access
    std::size_t TaskCount() const { return tasks_.size(); }
    std::uint32_t TaskIndex() const { return taskIndex_; }
    bool LastTask() const { return lastTask_; }

private:
    bool load_task_script();                                       // @ 0x00085dc4
    bool ReStartCurrentTask();                                     // @ 0x00087294
    void setInstructionText(const cRZString& text);                // @ 0x00086b1a
    void DoSpecialDocumentProcessing(const cRZString& text);       // @ 0x000866aa
    void finish_tutorial(bool lastTask);                           // @ 0x00085cdc
    void exit_to_region();                                         // @ 0x000863cc
    bool IsRightBuildingPosition(float x, float z);                // @ 0x00084f10
    void tick_self(std::uint32_t kind, std::int64_t data2, std::int64_t data3, void* window, cIGZUnknown* object);   // @ 0x000852b6
    bool LoadArrow(std::uint32_t instance, std::uint32_t id, cIGZWin*& out);   // @ 0x00085394
    void HideArrow();                                              // @ 0x00085024
    void ShowArrow();                                              // @ 0x0008527e
    void KillArrow();                                              // @ 0x000851e4
    void DrawArrow(int left, int top, int right);                  // @ 0x0008546c
    void DrawArrow(cIGZWin* target);                               // @ 0x0008565a
    void StopFlashing();                                           // the flashing button off (Flash(0, 200)) and dropped
    cIGZWin* Window() const;                                       // the window of +0x10
    void Subscribe(bool on);
    void ShowTryAgain(cISC4TutorialTask* task);

    std::uint32_t refCount_ = 0;
    gz::cIGZWinGen* window_ = nullptr;          // +0x10
    cIGZWinHTML* html_ = nullptr;           // +0x14
    cIGZWin* arrow_ = nullptr;                  // +0x18 the arrow shown (one of the two below)
    cIGZWin* arrowA_ = nullptr;                 // +0x1c
    cIGZWin* arrowB_ = nullptr;                 // +0x20
    cIGZWin* exitDialog_ = nullptr;             // +0x24
    cISC4WinCatalogView* catalog_ = nullptr;    // +0x2c (flashing an item)
    std::uint32_t taskIndex_ = 0;               // +0x30
    cIGZWin* flashing_ = nullptr;               // +0x34 the flashing button
    bool catalogHandled_ = false;               // +0x38
    bool clickPending_ = false;                 // +0x39
    int lastInsideX_ = 0, lastInsideY_ = 0;     // port: the last view point inside the region (FilterLeftMouseClick's release)
    bool dispatchDone_ = false;                 // +0x3a
    bool lastTask_ = false;                     // +0x3b
    int windowX_ = 0, windowY_ = 0;             // +0x3c / +0x40
    cISCLua* lua_ = nullptr;                    // +0x44
    std::vector<cSC4TutorialTask*> tasks_;      // +0x48 (one reference each)
};

}  // namespace sc4
