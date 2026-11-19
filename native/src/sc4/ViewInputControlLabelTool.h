// cSC4ViewInputControlLabelTool — the view tool that paints labels (text decals) on the ground: Mac i386 0x0049110c..0x00492224
// (the x64 build's constructor is 0x1006c869c), spec OpenSC4 view-input-controls.md section 4. Tool id 0x8b3ce416, cursor
// 0xeb72fef4. The object has a second base at +0x28, the edit dialog's message filter (cIGZWinMessageFilter).
// Layout: +0x2c state (0 idle, 1 pressed on a label, 2 dragging it), +0x30 selected label (counted), +0x34 offset from the
// press point to the label's position, +0x40 the orientation new labels take (1).
// Quirks kept:
//  * The press offset (+0x34) is computed and stored but a drag places the label at the raw terrain hit.
//  * A label made by a click whose edit dialog is cancelled or left empty is removed again; a click on an existing label
//    whose dialog fails removes that label too.
//  * Home and End turn the orientation even with nothing to re-place (they need a selection) and answer false.
//  * The edit dialog's text field takes at most 0x20 characters.
#pragma once
#include <cstdint>

#include "gz/WinInterfaces.h"
#include "rz/cRZString.h"
#include "sc4/Occupant.h"
#include "sc4/TextDecalOccupant.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

constexpr std::uint32_t kLabelToolId = 0x8b3ce416;
constexpr std::uint32_t kLabelToolCursor = 0xeb72fef4;
constexpr std::uint32_t kLabelDialogScriptGroup = 0x96a006b0, kLabelDialogScriptInstance = 0x6b704690;
constexpr std::uint32_t kLabelDialogWindow = 0x8a8dfcf5;
constexpr std::uint32_t kLabelDialogTextField = 0xaa4149bf;
constexpr std::uint32_t kLabelDialogOk = 0x8a8dfd4e, kLabelDialogCancel = 0x8a8dfd4a, kLabelDialogClose = 0x6ba18178;
constexpr std::uint32_t kLabelToolSoundGroup = 0xea451903, kLabelToolSoundGrab = 0x2c16da45, kLabelToolSoundDrop = 0x2c16da29;

// A network piece as the label tool reads it (cISC4NetworkOccupant through the traffic network map): borrowed.
class ILabelNetworkPiece {
public:
    virtual ~ILabelNetworkPiece() = default;
    virtual bool HasAllNetworkFlags(std::uint32_t flags) = 0;     // slot 0x20 (i386)
    virtual bool HasAnyNetworkFlag(std::uint32_t flags) = 0;      // slot 0x24
    virtual bool IsOfType(std::uint32_t networkType) = 0;         // slot 0x58
    virtual std::uint32_t EdgeWord(std::uint32_t networkType) = 0;   // slot 0x98: W N E S bytes
};

// The services behind SL::spRenderer, spCity, spTerrain, spTrafficNetworkMap, spOccupantManager, spPropManager,
// spPropDeveloper, spSS, spView3DWin and RZ::WM().
class ILabelToolHost {
public:
    virtual ~ILabelToolHost() = default;
    virtual bool HasRenderer() = 0;
    virtual bool PickTerrain(int x, int y, float out[3]) = 0;                         // renderer slot 0xf4 (x, y, out, 0, 1)
    virtual int ViewRotation() = 0;                                                   // renderer slot 0x28 -> +0x18
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;       // city slot 0x17c
    virtual void CellCenter(int cellX, int cellZ, float& x, float& z) = 0;           // city slot 0x184
    virtual bool CellHasWater(int cellX, int cellZ) = 0;                             // terrain slot 0x88
    virtual ILabelNetworkPiece* NetworkAtCell(int cellX, int cellZ, std::uint32_t mask) = 0;          // map slot 0x28 (.., 0)
    virtual ILabelNetworkPiece* NetworkAtPosition(const float* position, std::uint32_t mask) = 0;    // map slot 0x2c (.., 1)
    // the occupant manager's first occupant at (x, z) through cSC4DecalFilter, as a label (one reference) or null
    virtual cISC4TextDecalOccupant* LabelAt(float x, float z) = 0;
    virtual cISC4TextDecalOccupant* CreateLabel() = 0;                                // new cSC4TextDecalOccupant, one reference
    virtual void AddProp(cISC4Occupant* occupant) = 0;                               // prop manager slot 0x50
    virtual void RemoveProp(cISC4Occupant* occupant) = 0;                            // prop manager slot 0x74
    virtual void ShowSignsAndLabels() = 0;                                           // prop developer slot 0x20 (1)
    virtual void PlaySound(std::uint32_t group, std::uint32_t id) = 0;               // spSS slot 0x28
    virtual cIGZWin* View3DWindow() = 0;                                             // spView3DWin
    // SC4CreateWindowFromScript(key, parent, id): one reference, or null
    virtual cIGZWin* CreateWindowFromScript(std::uint32_t group, std::uint32_t instance, cIGZWin* parent, std::uint32_t id) = 0;
    virtual cIGZWinMgr* WindowManager() = 0;                                         // RZ::WM()
};
ILabelToolHost* LabelToolHost();
void SetLabelToolHost(ILabelToolHost* host);

class cSC4ViewInputControlLabelTool : public cSC4ViewInputControl, public cIGZWinMessageFilter {
public:
    enum State : int { kIdle = 0, kPressed = 1, kDragging = 2 };

    cSC4ViewInputControlLabelTool();                                        // 0x00491d78
    ~cSC4ViewInputControlLabelTool() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cSC4ViewInputControl::AddRef(); }
    std::uint32_t Release() override { return cSC4ViewInputControl::Release(); }

    bool Init() override;                                                   // 0x0049110c
    bool Shutdown() override;                                               // 0x004912c8
    bool OnKeyDown(std::uint32_t key, std::uint32_t mods) override;         // 0x00491dc8
    bool OnMouseDownL(int x, int y, std::uint32_t mods) override;           // 0x00491edc
    bool OnMouseUpL(int x, int y, std::uint32_t mods) override;             // 0x0049212a
    bool OnMouseMove(int x, int y, std::uint32_t mods) override;            // 0x00492224
    bool DoMessage(cIGZWin* window, cGZMessage& message) override;          // 0x00491154: the edit dialog's filter

    void SetSelection(cISC4TextDecalOccupant* label);                       // 0x0049126e
    cISC4TextDecalOccupant* GetDecalOccupantAt(const float* position);      // 0x004912fc (no reference: the manager keeps it)
    void FindAvenueCenterAndOrientation(float* position, int& orientation); // 0x004913f0
    void FindRoadCenterAndOrientation(float* position, int& orientation);   // 0x004916cc
    void UpdateTextDecal(float* position, cISC4TextDecalOccupant* label);   // 0x00491972
    bool DoEditLabelUI(cISC4TextDecalOccupant* label);                      // 0x00491b3e

    State GetState() const { return static_cast<State>(state_); }
    cISC4TextDecalOccupant* Selection() const { return selection_; }
    int Orientation() const { return orientation_; }

private:
    int state_ = kIdle;                              // +0x2c
    cISC4TextDecalOccupant* selection_ = nullptr;    // +0x30
    float pressOffset_[3] = {0, 0, 0};               // +0x34
    int orientation_ = 1;                            // +0x40
};

}  // namespace sc4
