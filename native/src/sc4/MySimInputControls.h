// The MySim view tools that choose a building: cSC4MySimInputControlMoveIn (a new MySim's home), ...ChangeHouse (a new
// home) and ...ChangeJob (a new workplace). Mac i386 0x00483696..0x00487b70.
//
// Each picks the occupant under the pointer (spView3D vslot 0x94); a finished building that passes the tool's test is
// highlighted (its old highlight kept to put back) and becomes the target; a click while the tool is the view's current
// control acts on the target and ends the tool, Escape ends it. Layout (i386): +0x28 the MySim (the cISC4MySim interface for
// MoveIn, the cSC4MySim for the others), +0x2c the target building, +0x30 its saved highlight, +0x34 the MySim agent
// simulator, +0x38 the callout over the target, +0x3c / +0x40 the portrait windows that follow the pointer, +0x44 / +0x48
// the portrait offset. The callout and portraits are drawn through the host (ShowTarget / ShowNoTarget / HideTarget).
#pragma once

#include <cstdint>

#include "rz/cRZAutoRefCount.h"
#include "sc4/ViewInputControl.h"

struct cS3DVector3;

namespace sc4 {

class cISC4View3DWin;

class cISC4BuildingOccupant;
class cISC4MySim;
class cISC4Occupant;
class cSC4Lot;
class cSC4MySim;
class cSC4MySimAgentSimulator;

constexpr std::uint32_t kMySimToolCursor = 0x43401c71;   // Init's SetCursor (vslot 0x24), all three tools

class IMySimToolHost {
public:
    virtual ~IMySimToolHost() = default;
    virtual cISC4View3DWin* View() = 0;                          // SL::spView3D
    virtual cSC4MySimAgentSimulator* Agents() = 0;               // spCity vslot 0x11c
    virtual cSC4Lot* OccupantLot(cISC4Occupant* occupant) = 0;   // spLotManager vslot 0x38 GetOccupantLot
    virtual cSC4Lot* LotAt(const cS3DVector3& position) = 0;     // spLotManager vslot 0x18 GetLot
    // The portrait at the pointer and the callout over the target (cSC4WinCalloutBox, color by the lot's wealth, the
    // building's name and, for a home, its desirability for the MySim's wealth); without a target, the pointer's portrait.
    virtual void ShowTarget(cISC4BuildingOccupant* building, int wealth, bool home, int x, int y) { (void)building; (void)wealth; (void)home; (void)x; (void)y; }
    virtual void ShowNoTarget(int x, int y) { (void)x; (void)y; }
    virtual void HideTarget() {}
};
IMySimToolHost* MySimToolHost();
void SetMySimToolHost(IMySimToolHost* host);

// The shared half of the three tools (the original repeats it in each).
class cSC4MySimBuildingTool : public cSC4ViewInputControl {
public:
    bool Init() override;                                                   // SetCursor(kMySimToolCursor), base Init
    bool Shutdown() override;
    bool ShouldStack() override { return false; }
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // Escape ends the tool
    bool OnMouseExit() override;                                           // hides the portraits
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;
    cISC4BuildingOccupant* Target() const { return target_.get(); }

protected:
    bool IsCurrentControl();                                               // spView3D vslot 0x60 == this
    // The tool's own test of a finished building (completion 100%) under the pointer; `wealth` is the lot wealth the
    // callout shows.
    virtual bool Accepts(cISC4BuildingOccupant* building, int& wealth) = 0;
    virtual bool ForHome() const { return true; }
    void ClearTarget();                                                    // the saved highlight back, no target

    cRZAutoRefCount<cISC4BuildingOccupant> target_;   // +0x2c
    std::uint32_t savedHighlight_ = 0;                // +0x30
    cSC4MySimAgentSimulator* agents_ = nullptr;       // +0x34 (the city keeps it)
};

class cSC4MySimInputControlMoveIn final : public cSC4MySimBuildingTool {
public:
    using cSC4MySimBuildingTool::Init;
    bool Init(cSC4MySim* sim);                                             // i386 0x0048764e
    bool Shutdown() override;                                              // i386 0x00486ccc
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // i386 0x00486c7e

private:
    bool Accepts(cISC4BuildingOccupant* building, int& wealth) override;
    cISC4MySim* sim_ = nullptr;                                            // +0x28 (counted)
};

class cSC4MySimInputControlChangeHouse final : public cSC4MySimBuildingTool {
public:
    using cSC4MySimBuildingTool::Init;
    bool Init(cSC4MySim* sim);                                             // i386 0x00484198
    bool Shutdown() override;                                              // i386 0x00483814
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // i386 0x00483796

private:
    bool Accepts(cISC4BuildingOccupant* building, int& wealth) override;
    cRZAutoRefCount<cSC4MySim> sim_;                                       // +0x28
};

class cSC4MySimInputControlChangeJob final : public cSC4MySimBuildingTool {
public:
    using cSC4MySimBuildingTool::Init;
    bool Init(cSC4MySim* sim);                                             // i386 0x00484adc
    bool Shutdown() override;                                              // i386 0x00484982
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // i386 0x00484932

private:
    bool Accepts(cISC4BuildingOccupant* building, int& wealth) override;
    bool ForHome() const override { return false; }
    cRZAutoRefCount<cSC4MySim> sim_;                                       // +0x28
};

}  // namespace sc4
