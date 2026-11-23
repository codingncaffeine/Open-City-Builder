// cSC4TutorialTask — one entry of the tutorial script table "tutorialtasks[<task number>]" (46 symbols at
// 0x10011d466..0x10011f4a2 plus cTutorialString at 0x10011d60c..0x10011f468, file SC4TutorialTask.cpp).
//
// A task carries three tutorial strings ("instruction_msg", "try_again_msg", "congratulation_msg"), the region it
// highlights, the camera / zoom / window offset it wants and a list of button GUIDs whose windows it outlines. The
// script row also names an action function ("task_action"), whose "function_index" decides what ExecuteTask does.
//
// Original layout (size 0x170, cISC4TutorialTask vptr first, then the cRZUnknown refcount block):
//   +0x00 cISC4TutorialTask vtable, +0x08 cRZUnknown vtable, +0x10 refcount,
//   +0x18 instruction_msg (cTutorialString, 0x30), +0x48 try_again_msg, +0x78 congratulation_msg,
//   +0xa8 std::vector<std::vector<cGridData>> (rows indexed by z, 12 bytes per cell: +0 x, +4 z, +8 border flag),
//   +0xc0 GUID (written by the tutorial system, not by this class), +0xc4 task number, +0xc8 "task data is on the
//   Lua stack" flag, +0xd0 cISCLua* (not owned), +0xd8 decal color (cRZString), +0x100 "task is running" flag,
//   +0x104 whichFunction, +0x108 region returned by GetRegion, +0x118 the same bounds (startX, startZ, endX, endZ)
//   that the grid / fire / decal code uses, +0x128 "csi" flag, +0x129 the CSI visibility saved by ExecuteTask,
//   +0x12c camera X, +0x130 camera Z, +0x134 window offset X, +0x138 window offset Y, +0x13c extra data,
//   +0x140 decal effect (IToolVisualEffect, referenced), +0x148 std::vector<uint32_t> button GUIDs,
//   +0x160 target object GUID, +0x164 region is taller than it is wide, +0x168 zoom level, +0x16c no edge scrolling.
//
// The Lua protocol (all field names are the globals of the original static init at 0x10011fae2):
//   global "tutorialtasks" is an array of task rows; row "task_action" is the action table; its "function_index"
//   must be non-zero for the task to do anything. The three messages hold script text ("text@0x........") whose
//   resource key (type 0x2026960b, group 0xca554b03) cTutorialString parses and cSLString loads; a text that is not
//   in the string database becomes "***_NOT_IN_TEXT_DATABASE_*** <script text>" converted to code page 1252.
//
// Quirks kept (addresses are the original ones):
//  * Init returns true and re-sets the "data is on the stack" flag to 1 even when the whole task was not found, so a
//    task with whichFunction 0 still reports itself valid (0x10011dcd5).
//  * GetNumber clears that flag and returns 0 without popping the value it read when the field is not a number
//    (0x10011e0fa); GetTaskInformation / GetEnableButtonList return early and also leave their table on the stack
//    (0x10011e34f, 0x10011e4ec). Init restores the stack with SetTop at the end.
//  * Init appends to the grid and to the button GUID list instead of clearing them, so a second Init duplicates both
//    (and a second, smaller region leaves short rows that the original's cell indexing can run past).
//  * ExecuteTask only jumps the camera when both cameraX and cameraZ are non-zero; when both are zero it computes the
//    region centre and jumps there only if endX+startX+1 >= 3 (unsigned) and the centre Z is non-zero (0x10011ec16).
//  * EndTask with whichFunction 0 returns false, stops the decal but leaves the "task is running" flag set, so that
//    task cannot be ended again (0x10011eeae).
//  * TriggerFire dereferences the fire disaster manager without a null check (0x10011f0c7); the port skips that call.
//  * GetTargetWindow returns a borrowed window pointer (the lookups of cGZWin answer borrowed references) and chooses
//    its result from the visibility of the last ancestor it visited, not of the button itself (0x10011ea42).
//  * For a city that is not established yet the window walk substitutes the alt window 0xc991eda8 and, for the
//    windows 0xa9ed5617 / 0x69b9324a / 0x4a551a6b, their same-id child (0x10011e890).
//  * The virtual destructors sit at slots 0x30/0x38 of the original vtable; the port lets the compiler place them.
//
// Everything the original reaches through SL::sp* / SL::spCity / SL::spSimulator / SL::spLotManager goes through
// ITutorialTaskHost (the slot / address of every original call is in the comment).
#pragma once
#include <cstdint>
#include <vector>

#include "gz/WinInterfaces.h"
#include "gz/cIGZString.h"
#include "gz/cIGZUnknown.h"
#include "lua/ISCLua.h"
#include "rz/cRZString.h"
#include "rz/cRZUnknown.h"
#include "sc4/SLString.h"

namespace sc4 {

class cISC4BuildingOccupant;
class cISC4DisasterLayer;
class IToolEffectsManager;
class IToolVisualEffect;

constexpr std::uint32_t kGZIID_cISC4TutorialTask = 0x8a09f5f5;
constexpr std::uint32_t kTutorialStringGroup = 0xca554b03;

// cGridData — one cell of the task region grid (original +0x00 x, +0x04 z, +0x08 border flag, 12 bytes).
struct cGridData {
    std::int32_t cellX = 0;   // +0
    std::int32_t cellZ = 0;   // +4
    bool border = false;      // +8
};

// SC4Rect<int>: an inclusive cell rectangle.
struct SC4TutorialRect {
    std::int32_t x0 = 0;   // startX / left
    std::int32_t z0 = 0;   // startZ / top
    std::int32_t x1 = 0;   // endX / right
    std::int32_t z1 = 0;   // endZ / bottom
};

// What cSC4TutorialTask needs from the running game; every method mirrors one original call.
class ITutorialCSIVisibility {
public:
    virtual ~ITutorialCSIVisibility() = default;
    virtual bool GetVisible() = 0;          // cSC4CitySituationManager vslot 0xf0
    virtual void SetVisible(bool visible) = 0;   // cSC4CitySituationManager vslot 0xe8
};

class ITutorialTaskHost {
public:
    virtual ~ITutorialTaskHost() = default;
    virtual bool CityEstablished() = 0;                             // SL::spCity vslot 0xb0 (GetEstablished)
    virtual cIGZWin* View3DMainWindow() = 0;                        // SL::spView3D vslot 0x18 (the view's root window)
    virtual bool CityCellIsInBounds(int cellX, int cellZ) = 0;      // SL::spCity vslot 0x318, false without a city
    virtual bool CameraControlAvailable() = 0;                      // SL::spRenderer != 0 && renderer vslot 0x80 != 0
    virtual float TerrainCellWidth() = 0;                           // SL::spTerrain vslot 0x30
    virtual float TerrainAltitude(float x, float z) = 0;            // SL::spTerrain vslot 0x60
    // cSC4CameraControl::SetViewTargetPosition @ 0x10076bf2a, then SL::spRenderer vslot 0x88 (update view utilities).
    virtual void SetCameraTargetPosition(float x, float y, float z) = 0;
    virtual void HiddenPauseSimulator() = 0;                        // SL::spSimulator vslot 0x50
    virtual void HiddenResumeSimulator() = 0;                       // SL::spSimulator vslot 0x68
    virtual cISC4DisasterLayer* DisasterLayer() = 0;                // SL::spCity vslot 0x240
    virtual IToolEffectsManager* EffectsManager() = 0;              // SL::spCity vslot 0x2a8
    // SL::spCity vslot 0x188 (GetCitySituationManager); null when the city has no situation manager.
    virtual ITutorialCSIVisibility* CSIVisibility() = 0;
    // SL::spLotManager vslot 0x28(cellX, cellZ, 0), then the lot's vslot 0x180 (GetBuilding); borrowed.
    virtual cISC4BuildingOccupant* BuildingAtCell(int cellX, int cellZ) = 0;
};
ITutorialTaskHost* TutorialTaskHost();
void SetTutorialTaskHost(ITutorialTaskHost* host);

// cTutorialString (0x10011d60c..0x10011f468): a cSLString whose key is parsed out of script text ("text@0x%08x",
// "text@%08x", "text@%u" or "text@%i", tried in that order; the group is 0xca554b03). Original layout is the
// cSLString one (0x30 bytes, key at +0x24).
class cTutorialString : public cSLString {
public:
    cTutorialString() = default;                                             // (the cSLString default constructor)
    ~cTutorialString() override = default;                                   // @ 0x10011d60c / 0x10011f442
    cIGZString& AssignFromIGZString(const cIGZString& other) override;       // @ 0x10011f468
    // Two virtual slots after cRZString::Sprintf (original vslots 0xf0 / 0xf8).
    virtual void FromScriptText(const char* text);                           // @ 0x10011e012
    virtual void FromScriptText(const char* text, unsigned int length);      // @ 0x10011f438
    void init(const char* text, int length);                                 // @ 0x10011f384 (length is unused)
};

// cISC4TutorialTask — the interface (slots of the original vtable in the comments).
class cISC4TutorialTask : public cIGZUnknown {
public:
    virtual bool Init(cISCLua* lua, int taskNumber) = 0;                       // 0x18
    virtual bool Shutdown() = 0;                                               // 0x20
    virtual const SC4TutorialRect& GetRegion() const = 0;                      // 0x28
    virtual std::uint32_t GetGUID() const = 0;                                 // 0x40
    virtual bool HasButtonGUID(std::uint32_t guid) = 0;                        // 0x48
    virtual const cIGZString& GetInstructionMsg() const = 0;                   // 0x50
    virtual const cIGZString& GetTryAgainMsg() const = 0;                      // 0x58
    virtual const cIGZString& GetCongratulationMsg() const = 0;                // 0x60
    virtual std::uint32_t GetTargetGUID() const = 0;                           // 0x68
    virtual cIGZWin* GetTargetWindow() = 0;                                    // 0x70
    virtual bool DoLink(const cIGZString& link) = 0;                           // 0x78
    virtual bool ExecuteTask() = 0;                                            // 0x80
    virtual void ShowHintsForUsers() = 0;                                      // 0x88
    virtual bool EndTask() = 0;                                                // 0x90
    virtual bool drawDecals(int x0, int z0, int x1, int z1) = 0;               // 0x98
    virtual std::uint32_t GetExtraData() const = 0;                            // 0xa0
    virtual std::uint32_t GetCatalogButtonItemID() const = 0;                  // 0xa8
    virtual int GetWhichFunction() const = 0;                                  // 0xb0
    virtual bool handleButtons(std::uint32_t guid) = 0;                        // 0xb8
    virtual int GetZoomLevel() const = 0;                                      // 0xc0
    virtual int GetWindowOffsetX() const = 0;                                  // 0xc8
    virtual int GetWindowOffsetY() const = 0;                                  // 0xd0
    virtual bool IsEdgeScrollingAllowed() const = 0;                           // 0xd8
    virtual bool TriggerFire(const SC4TutorialRect& area) = 0;                 // 0xe0
};

class cSC4TutorialTask final : public cISC4TutorialTask, public cRZUnknown {
public:
    cSC4TutorialTask();                                                        // @ 0x10011d466 (0x10011d622 forwards)
    ~cSC4TutorialTask() override;                                              // @ 0x10011d62c (deleting 0x10011d77a)

    bool QueryInterface(std::uint32_t iid, void** out) override;               // @ 0x10011e52e (thunk 0x10011e550)
    std::uint32_t AddRef() override;                                           // @ 0x10011e576 (thunk 0x10011e584)
    std::uint32_t Release() override;                                          // @ 0x10011e58e (thunk 0x10011e59c)

    // cISC4TutorialTask
    bool Init(cISCLua* lua, int taskNumber) override;                          // @ 0x10011d7b6
    bool Shutdown() override;                                                  // @ 0x10011e702 (always true)
    const SC4TutorialRect& GetRegion() const override;                         // @ 0x10011e7b2
    std::uint32_t GetGUID() const override;                                    // @ 0x10011e716
    bool HasButtonGUID(std::uint32_t guid) override;                           // @ 0x10011e72e
    const cIGZString& GetInstructionMsg() const override;                      // @ 0x10011e79e
    const cIGZString& GetTryAgainMsg() const override;                         // @ 0x10011e7a8
    const cIGZString& GetCongratulationMsg() const override;                   // @ 0x10011e794
    std::uint32_t GetTargetGUID() const override;                              // @ 0x10011e70a
    cIGZWin* GetTargetWindow() override;                                       // @ 0x10011e7c0
    bool DoLink(const cIGZString& link) override;                              // @ 0x10011e526 (always true)
    bool ExecuteTask() override;                                               // @ 0x10011eb7e
    void ShowHintsForUsers() override;                                         // @ 0x10011f1a4
    bool EndTask() override;                                                   // @ 0x10011edfe
    bool drawDecals(int x0, int z0, int x1, int z1) override;                  // @ 0x10011f1e2
    std::uint32_t GetExtraData() const override;                               // @ 0x10011e760
    std::uint32_t GetCatalogButtonItemID() const override;                     // @ 0x10011e778
    int GetWhichFunction() const override;                                     // @ 0x10011e76c
    bool handleButtons(std::uint32_t guid) override;                           // @ 0x10011eeba
    int GetZoomLevel() const override;                                         // @ 0x10011e722
    int GetWindowOffsetX() const override;                                     // @ 0x10011f472
    int GetWindowOffsetY() const override;                                     // @ 0x10011f47e
    bool IsEdgeScrollingAllowed() const override;                              // @ 0x10011f48a
    bool TriggerFire(const SC4TutorialRect& area) override;                    // @ 0x10011f04c

    // Called by cSC4TutorialUIWinProc / cSC4TutorialSystem (not in the vtable): mark one cell of the highlighted
    // region as reached and answer whether every cell has been reached. True for tasks that do not use a grid.
    bool checkGridDataJobFinished(int cellX, int cellZ);                       // @ 0x10011e5a6

    // Accessors for the fields the tutorial system / the tests inspect (no counterparts in the original; the
    // tutorial system writes the GUID through the object layout).
    std::uint32_t GUID() const { return guid_; }
    void SetGUID(std::uint32_t guid) { guid_ = guid; }
    int TaskNumber() const { return taskNumber_; }
    bool IsValid() const { return valid_; }
    cISCLua* Lua() const { return lua_; }
    const std::vector<std::uint32_t>& ButtonGUIDs() const { return buttonGuids_; }
    const std::vector<std::vector<cGridData>>& GridData() const { return grid_; }
    bool TallerThanWide() const { return tallerThanWide_; }   // +0x164 (i386 +0xd0): a road task wants north-south pieces

private:
    bool GetString(const cRZString& field, cRZString& out);                        // @ 0x10011df28
    std::int32_t GetNumber(const cRZString& field);                                // @ 0x10011e05a
    bool GetString(const cRZString& field, cRZString& out, const cRZString& def);  // @ 0x10011e10e
    bool GetTaskInformation();                                                     // @ 0x10011e200
    bool GetEnableButtonList(const cRZString& field);                              // @ 0x10011e35c
    std::uint32_t GetTargetObjectGuid(const cRZString& field);                     // @ 0x10011e680
    void JumpToLocation(int cellX, int cellZ);                                     // @ 0x10011ecf2
    cISC4BuildingOccupant* FindNearestBuilding(const SC4TutorialRect& area);       // @ 0x10011eedc

    // +0x18
    cTutorialString instruction_;
    // +0x48
    cTutorialString tryAgainMsg_;
    // +0x78
    cTutorialString congratulationMsg_;
    // +0xa8
    std::vector<std::vector<cGridData>> grid_;
    std::uint32_t guid_ = 0;                 // +0xc0
    int taskNumber_ = 0;                     // +0xc4
    bool valid_ = false;                     // +0xc8
    cISCLua* lua_ = nullptr;                 // +0xd0 (not owned)
    cRZString decalColor_;                   // +0xd8
    bool started_ = false;                   // +0x100
    int whichFunction_ = 0;                  // +0x104
    // The constructor (i386 0x000d4716) sets all eight to -1: a task without a region has x0 < 0 (the zone tool's
    // tutorial gate lets those through).
    SC4TutorialRect region_{-1, -1, -1, -1}; // +0x108
    std::int32_t startX_ = -1;               // +0x118
    std::int32_t startZ_ = -1;               // +0x11c
    std::int32_t endX_ = -1;                 // +0x120
    std::int32_t endZ_ = -1;                 // +0x124
    bool csi_ = false;                       // +0x128
    bool csiVisibleSaved_ = false;           // +0x129
    std::int32_t cameraX_ = 0;               // +0x12c
    std::int32_t cameraZ_ = 0;               // +0x130
    std::int32_t windowOffsetX_ = 0;         // +0x134
    std::int32_t windowOffsetY_ = 0;         // +0x138
    std::int32_t extraData_ = 0;             // +0x13c
    IToolVisualEffect* decal_ = nullptr;     // +0x140 (referenced)
    std::vector<std::uint32_t> buttonGuids_; // +0x148
    std::uint32_t targetGUID_ = 0;           // +0x160
    bool tallerThanWide_ = false;            // +0x164
    std::int32_t zoomLevel_ = 0;             // +0x168
    bool noEdgeScroll_ = false;              // +0x16c
};

}  // namespace sc4
