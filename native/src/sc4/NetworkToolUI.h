// cSC4NetworkToolUI — the mouse half of the road / rail / pipe / power-line drawing tools. cSC4NetworkTool::GetUITool
// creates one per input control (new(300) + Init(tool, type), 0x00122c58 i386); cSC4ViewInputControlNetworkTool forwards
// the view's mouse and key events to it. It picks the cell under the mouse (ScreenToCell), keeps the drag's anchor
// points, asks the tool to draw and solve the line on every new cell (TestPlacement / UpdatePreview: cost indicator and
// failure text on the cursor) and on mouse-up charges the solution and places it (DoPlacement), or opens the bridge
// chooser when the drag crosses water.
//
// Recovered from the Mac i386 build with symbols (0x004cf3a8..0x004d1d34, vtable 0x00bebc88). The offsets below are the
// i386 ones (this object is not part of any file format, so the layout here is free; the comments keep the map):
//   +0x00 cISC4NetworkToolUI  +0x04 cIGZWinProc  +0x08 cIGZMessageTarget2  +0x0c cRZRefCount (+0x10 count)
//   +0x14 the tool (counted)  +0x18 sound system (counted)  +0x1c the 3D view (not counted)
//   +0x20 time of the last anchor point (ms)  +0x24 inited  +0x25 placing  +0x26 mouse down  +0x27 preview dirty
//   +0x28 major bridge is a land bridge  +0x29 grid shown before the drag  +0x2a multi-anchor drag  +0x2b diagonals allowed
//   +0x2c altitude at mouse down  +0x30 altitude under the mouse  +0x34 cell under the mouse  +0x3c cell at mouse down
//   +0x44 std::vector<SC4Point<u32>> anchor points  +0x50 cRZString failure text on the cursor
//   +0x5c preferred bridge (-1)  +0x60 bridge altitude  +0x64 bridge altitude offset (bridge dialog slider)
//   +0x68 bridge altitude override (-1.0f)  +0x6c std::vector<BridgeEntry>  +0x78 network type
//   +0x7c cISC4NetworkTool::tPlacementParams (GetPlacementParams)  +0x128 the bridge chooser window (counted)
//
// Quirks kept:
//  * OnMouseDownL clears the multi-anchor flag (+0x2a) that Init copies from the placement params (0x004d05c2), so the
//    anchor-point path in OnMouseMove / UpdateAnchorPoint never runs in the shipped game; it is ported anyway.
//  * TestPlacement's pen-width correction compares |dz| with dx * 2 and |dx| with dz * 2 (only one side of each made
//    absolute), as the original does (0x004cfc2c..0x004cfc6e).
//  * LastDragDirection rounds with the 12582912.0f trick: (atan2(dx, -dz) * 4 / pi) + 1.5 * 2^23, low 3 bits.
//
// Not ported yet (named so the gap is visible): AddBridgeEntry (the bridge chooser's button rows, 0x004d0874) and the
// bridge window half of DoWinProcMessage / OnButtonActivated; the host's CreateBridgeChooser answers null until then.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "gz/WinInterfaces.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZString.h"
#include "sc4/NetworkTool.h"
#include "sc4/View3DWin.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4NetworkToolUI = 0x8b54be51;
constexpr std::uint32_t kNetworkToolCursorText = 0xe9c87f80;      // SetCursorText / ClearCursorText id
constexpr std::uint32_t kNetworkToolTickMessage = 0x86ad10ee;     // SL::ATM's tick (registered with mask 0xff)

extern const std::uint32_t kNetworkPlopSound[13];        // _kNetworkPlopSound      0x008501e0
extern const std::uint32_t kNetworkDragErrorSound[13];   // _kNetworkDragErrorSound 0x00850220
extern const std::uint32_t kNetworkDragSound[13];        // _kNetworkDragSound      0x00850260

// cISC4NetworkToolUI, in the original vtable order (i386 offsets).
class cISC4NetworkToolUI : public cIGZUnknown {
public:
    virtual bool Shutdown() = 0;                                          // 0x0c
    virtual std::uint32_t ViewInputControlId() = 0;                       // 0x10
    virtual std::uint32_t CursorId() = 0;                                 // 0x14
    virtual void PenWidth(int& widthX, int& widthZ) = 0;                  // 0x18
    virtual void EnterUI() = 0;                                           // 0x1c
    virtual void LeaveUI() = 0;                                           // 0x20
    virtual bool OnMouseDownL(int x, int y, std::uint32_t modifiers) = 0; // 0x24
    virtual bool OnMouseUpL(int x, int y, std::uint32_t modifiers) = 0;   // 0x28
    virtual bool OnMouseMove(int x, int y, std::uint32_t modifiers) = 0;  // 0x2c
    virtual void CancelInput() = 0;                                       // 0x30
    virtual bool IsActive() = 0;                                          // 0x34
    virtual std::uint32_t DirectionalCursorBase() = 0;                    // 0x38
    virtual int LastDragDirection() = 0;                                  // 0x3c
    virtual void AttachToView(cISC4View3DWin* view) = 0;                  // 0x40
    virtual ~cISC4NetworkToolUI() = default;                              // 0x44 / 0x48
};

// Everything outside the object; one method per original global or call.
class INetworkToolUIHost {
public:
    virtual ~INetworkToolUIHost() = default;
    virtual void PlaySound(std::uint32_t id) = 0;                         // SL::spSS vslot 0x28(1, id, 0, 0, 0)
    virtual void StartTick(cIGZMessageTarget2* target) = 0;               // SL::ATM() vslot 0x14(target, 0xff)
    virtual void StopTick(cIGZMessageTarget2* target) = 0;                // SL::ATM() vslot 0x18(target, 0xff)
    virtual void EnterToolView(std::uint32_t viewId) = 0;                 // SL::ViewManager() vslot 0x10
    virtual void LeaveToolView(std::uint32_t viewId) = 0;                 // SL::ViewManager() vslot 0x14
    virtual bool SetGridShown(bool shown) = 0;                            // SL::TerrainView() vslot 0x28: the previous state
    virtual void ClearTerrainHighlight() = 0;                             // SL::TerrainView() vslot 0x24
    virtual void BeginNetworkDrag() = 0;                                  // SL::spCity vslot 0x1a8
    virtual void EndNetworkDrag() = 0;                                    // SL::spCity vslot 0x1ac
    virtual std::int64_t Funds() = 0;                                     // SL::spBudgetSim vslot 0x18
    virtual bool SpendFunds(std::uint32_t cost) = 0;                      // SL::spBudgetSim vslot 0x20(cost, 0)
    virtual float CellSize() = 0;                                         // SL::spTerrain vslot 0x18
    virtual int CellCountX() = 0;                                         // SL::spTerrain vslot 0x1c
    virtual int CellCountZ() = 0;                                         // SL::spTerrain vslot 0x20
    virtual float SeaLevel() = 0;                                         // SL::spTerrain vslot 0x74
    virtual int ZoomLevel() = 0;                                          // SC4ViewUtilities::GetZoomAndRotation
    // GetViewportPointIntersectionWithPlane(view, x, y, altitude, out): the mouse ray on the plane y = altitude.
    virtual bool IntersectViewportWithPlane(int x, int y, float altitude, float out[3]) = 0;
    // SL::spNeighborsSim vslot 0x14: ask whether to connect the drag to the neighbour city.
    virtual bool ConfirmNeighborConnection(const NeighborConnectionInfo& info) = 0;
    virtual void RefreshCursor() = 0;                                     // RZ::WM() vslot 0x4c
    // SC4CreateWindowFromScript(0xebd0d36c, view window, 0x0c525b9e, 0x5386d516): the bridge chooser, one reference.
    virtual cIGZWin* CreateBridgeChooser(cIGZWin* /*parent*/) { return nullptr; }
    virtual void RemoveWindow(cIGZWin* /*window*/) {}                     // view window vslot 0x40(child)
};
INetworkToolUIHost* NetworkToolUIHost();
void SetNetworkToolUIHost(INetworkToolUIHost* host);

class cSC4NetworkToolUI final : public cISC4NetworkToolUI, public cIGZMessageTarget2 {
public:
    struct BridgeEntry {
        int bridge = 0;          // +0 bridge index for SetPreferredBridge
        float altitude = 0.0f;   // +4
    };

    cSC4NetworkToolUI() = default;                                        // @ 0x004d14f4
    ~cSC4NetworkToolUI() override;                                        // @ 0x004d0742
    cSC4NetworkToolUI(const cSC4NetworkToolUI&) = delete;
    cSC4NetworkToolUI& operator=(const cSC4NetworkToolUI&) = delete;

    bool QueryInterface(std::uint32_t iid, void** out) override;          // @ 0x004cf3c4
    std::uint32_t AddRef() override { return ++refCount_; }               // @ 0x004cf3a8
    std::uint32_t Release() override;                                     // @ 0x004cf3b6
    bool Init(cISC4NetworkTool* tool, std::uint32_t networkType);         // @ 0x004cf414

    bool Shutdown() override;                                             // @ 0x004cf8b4
    std::uint32_t ViewInputControlId() override { return tool_->ViewInputControlId(); }   // @ 0x004cf6b8
    std::uint32_t CursorId() override { return tool_->CursorId(); }       // @ 0x004cf6d0
    void PenWidth(int& widthX, int& widthZ) override { tool_->PenWidth(widthX, widthZ); }   // @ 0x004cf6e8
    void EnterUI() override;                                              // @ 0x004cf700
    void LeaveUI() override;                                              // @ 0x004cf736
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;    // @ 0x004d0580
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;      // @ 0x004d0ece
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;     // @ 0x004d1a42
    void CancelInput() override { AbortPlacement(); }                     // @ 0x004cf8aa
    bool IsActive() override { return placing_; }                         // @ 0x004cf5dc
    std::uint32_t DirectionalCursorBase() override { return tool_->DirectionalCursorBase(); }   // @ 0x004cf76c
    int LastDragDirection() override;                                     // @ 0x004d0368
    void AttachToView(cISC4View3DWin* view) override;                     // @ 0x004cf88c

    bool DoMessage(cIGZMessage2* message) override;                       // @ 0x004d01c4

    void AbortPlacement();                                                // @ 0x004cf784
    void DoPlacement();                                                   // @ 0x004cf8f8
    bool TestPlacement(bool updatePreview);                               // @ 0x004cfbcc
    bool UpdatePreview();                                                 // @ 0x004cfdd2
    void UpdateIndicators(bool affordable);                               // @ 0x004cf496
    void OnButtonActivated(std::uint32_t id);                             // @ 0x004cff88
    bool ScreenToCell(int x, int y, SC4PointU& cell, float* altitude, bool usePlane, float planeAltitude,
                      float minAltitude, float maxAltitude);              // @ 0x004d0210
    bool LiesOnSegment(const SC4PointU& cell, const SC4PointU& from, const SC4PointU& to);   // @ 0x004d0456
    void UpdateAnchorPoint(const SC4PointU& cell);                        // @ 0x004d1864

    // Inspection for the tests / the owner.
    const std::vector<SC4PointU>& Anchors() const { return anchors_; }
    bool MouseDown() const { return mouseDown_; }

private:
    void CloseBridgeChooser();

    std::uint32_t refCount_ = 0;                // +0x10
    cISC4NetworkTool* tool_ = nullptr;          // +0x14 (counted)
    cISC4View3DWin* view_ = nullptr;            // +0x1c
    std::uint32_t anchorTime_ = 0;              // +0x20
    bool inited_ = false;                       // +0x24
    bool placing_ = false;                      // +0x25
    bool mouseDown_ = false;                    // +0x26
    bool previewDirty_ = false;                 // +0x27
    bool majorLandBridge_ = false;              // +0x28
    bool gridWasShown_ = false;                 // +0x29
    bool multiAnchor_ = false;                  // +0x2a
    bool allowDiagonals_ = false;               // +0x2b
    float startAltitude_ = 0.0f;                // +0x2c
    float altitude_ = 0.0f;                     // +0x30
    SC4PointU cell_{};                          // +0x34
    SC4PointU startCell_{};                     // +0x3c
    std::vector<SC4PointU> anchors_;            // +0x44
    cRZString failureText_;                     // +0x50
    int preferredBridge_ = 0;                   // +0x5c
    float bridgeAltitude_ = 0.0f;               // +0x60
    float bridgeAltitudeOffset_ = 0.0f;         // +0x64
    float bridgeAltitudeOverride_ = 0.0f;       // +0x68
    std::vector<BridgeEntry> bridges_;          // +0x6c
    std::uint32_t networkType_ = 0;             // +0x78
    cSC4NetworkPlacementParams params_{};       // +0x7c
    cIGZWin* bridgeChooser_ = nullptr;          // +0x128 (counted)
};

}  // namespace sc4
