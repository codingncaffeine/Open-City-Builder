// cSC4ViewInputControlNetworkTool — the 3D view's input control for every network drawing tool (roads, rail, highways,
// pipes, power lines, subways, ...). It owns a fresh cSC4NetworkTool from the network manager (GetNetworkTool(type, true))
// and that tool's cSC4NetworkToolUI, forwards the mouse to the UI, sets the cursor the UI asks for (the directional
// cursor while dragging, turned with the camera) and shows the "local_tile_outline" pen and the local grid under the
// mouse when not dragging. Message 0x09c8b8ea (another input control became current) cancels the drag.
//
// Recovered from the Mac i386 build with symbols (16 functions, 0x002ae150..0x002aebb6, vtable 0x00bc9f88; the x86_64
// twin is 0x10038dea0..0x10038e8b6). i386 layout: +0x28 cIGZMessageTarget2, +0x2c terrain view (counted), +0x30 cell
// size, +0x34 last picked position (snapped to the cell centre, starts at -1), +0x40 the outline effect (counted),
// +0x44 cSC4LocalGridHelper, +0x48 the tool (counted), +0x4c its UI (counted), +0x50 network type, +0x54 button down.
#pragma once
#include <cstdint>

#include "gz/cIGZMessage2.h"
#include "sc4/LocalGridHelper.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

class cISC4NetworkTool;
class cISC4NetworkToolUI;

constexpr std::uint32_t kNetworkIntxToolId = 0x48e2a1f8;   // the intersection tool keeps the drag alive

class INetworkInputToolTerrainView : public cIGZUnknown {
public:
    virtual bool GridShown() = 0;                                    // vslot 0x50
};

// Everything outside the object; one method per original global.
class INetworkInputToolHost {
public:
    virtual ~INetworkInputToolHost() = default;
    virtual cISC4View3DWin* View() = 0;                              // the control's view (+0x1c)
    // SL::NetworkManager() vslot 0x14(type, true): a new tool, one reference.
    virtual cISC4NetworkTool* CreateNetworkTool(std::uint32_t type) = 0;
    virtual cIGZMessageServer2* MessageServer() = 0;                 // SL::spMS2
    virtual float CellSize() = 0;                                    // SL::spTerrain vslot 0x18
    virtual INetworkInputToolTerrainView* TerrainView() = 0;         // SL::spTerrain vslot 0x108 (one reference)
    virtual int ViewRotation() = 0;                                  // view renderer -> camera +0x10c
};
INetworkInputToolHost* NetworkInputToolHost();
void SetNetworkInputToolHost(INetworkInputToolHost* host);

class cSC4ViewInputControlNetworkTool final : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    explicit cSC4ViewInputControlNetworkTool(std::uint32_t networkType);   // @ 0x002aeab4
    ~cSC4ViewInputControlNetworkTool() override;                          // @ 0x002aeb50

    bool QueryInterface(std::uint32_t iid, void** out) override;          // @ 0x002ae21a
    std::uint32_t AddRef() override { return cSC4ViewInputControl::AddRef(); }
    std::uint32_t Release() override { return cSC4ViewInputControl::Release(); }
    bool DoMessage(cIGZMessage2* message) override;                       // @ 0x002ae6fe

    bool Init() override;                                                 // @ 0x002ae5a4
    bool Shutdown() override;                                             // @ 0x002ae2e0
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;  // @ 0x002ae54e
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;    // @ 0x002ae410
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;      // @ 0x002ae4ba
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;     // @ 0x002ae76c
    bool OnMouseExit() override;                                          // @ 0x002ae25a

    cISC4NetworkTool* Tool() const { return tool_; }
    cISC4NetworkToolUI* UI() const { return ui_; }

private:
    cISC4View3DWin* View() const;
    void StopOutline();

    INetworkInputToolTerrainView* terrainView_ = nullptr;   // +0x2c
    float cellSize_ = 0.0f;                                 // +0x30
    cS3DVector3 lastPosition_{-1.0f, -1.0f, -1.0f};         // +0x34
    ILocalGridEffect* outline_ = nullptr;                   // +0x40
    cSC4LocalGridHelper localGrid_;                         // +0x44
    cISC4NetworkTool* tool_ = nullptr;                      // +0x48
    cISC4NetworkToolUI* ui_ = nullptr;                      // +0x4c
    std::uint32_t networkType_ = 0;                         // +0x50
    bool mouseDown_ = false;                                // +0x54
};

}  // namespace sc4
