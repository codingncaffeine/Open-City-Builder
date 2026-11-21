// cSC4ViewInputControlNetworkIntxTool — the 3D view's input control of the pre-built intersection pieces ("puzzle pieces":
// interchanges, ramps, the highway crossings of the highway catalog, item ids 0x6a47xxxx). It holds one intersection rule
// id; every mouse move tries that rule and the rest of its rotation ring, at the cell under the cursor and its four
// neighbours (offset by the rule's handle), as a dry run of the network manager's highway tool
// (InsertIsolatedHighwayIntersection(..., false)), shows the rule's preview effect ("_red" when it does not fit or the city
// cannot pay) and the cost, and a click places the fitting rule for real and pays for it. Home / End walk the rotation ring,
// Tab / Shift+Tab the type ring.
//
// Recovered from the Mac i386 build with symbols (0x002aec00..0x002afe12, ctor 0x002afe12). i386 layout: +0x28 terrain view
// (counted), +0x2c cell size, +0x30 last picked position (starts at -1), +0x3c the "local_tile_outline" effect, +0x40 the
// preview effect, +0x44 cSC4LocalGridHelper, +0x48 the last rule tried, +0x4c the rule id, +0x50 / +0x54 the anchor cell,
// +0x58 the cost (64 bits), +0x60 the cursor text, +0x6c the preview effect's name, +0x78 the placement fits, +0x79 a byte
// the move clears and nothing reads.
#pragma once
#include <cstdint>
#include <string>

#include "rz/cRZString.h"
#include "sc4/LocalGridHelper.h"
#include "sc4/ViewInputControl.h"
#include "sc4/ViewInputControlNetworkTool.h"

class cIGZString;

namespace sc4 {

class cISC4View3DWin;
class cSC4NetworkTool;
namespace nSC4Networks {
class cIntRule;
}

constexpr std::uint32_t kNetworkIntxToolCursor = 0x88e70b76;      // Init's cursor, and the cursor while nothing fits
constexpr std::uint32_t kNetworkIntxToolFitCursor = 0x417ee36b;   // the cursor while a rule fits
constexpr std::uint32_t kNetworkIntxToolCursorText = 0xea282ca4;  // SetCursorText / ClearCursorText id
constexpr std::uint32_t kNetworkIntxPlaceSound = 0xea5ec59f;      // a piece placed
constexpr std::uint32_t kNetworkIntxRefuseSound = 0x495a6d86;     // a click where nothing fits
constexpr std::uint32_t kNetworkIntxReleaseSound = 0xaa62c9d5;    // the button released where nothing fits

// Everything outside the object; one method per original global.
class INetworkIntxToolHost {
public:
    virtual ~INetworkIntxToolHost() = default;
    virtual cISC4View3DWin* View() = 0;                                  // the control's view (+0x1c)
    virtual float CellSize() = 0;                                        // SL::spTerrain vslot 0x18
    virtual INetworkInputToolTerrainView* TerrainView() = 0;             // SL::spTerrain vslot 0x108 (one reference)
    virtual int TerrainCellsX() = 0;                                     // SL::spTerrain vslot 0x1c
    virtual int TerrainCellsZ() = 0;                                     // SL::spTerrain vslot 0x20
    virtual float TerrainAltitude(float x, float z) = 0;                 // SL::spTerrain vslot 0x30
    virtual std::uint32_t CityCellsX() = 0;                              // SL::spCity vslot 0x174
    virtual std::uint32_t CityCellsZ() = 0;                              // SL::spCity vslot 0x178
    // SL::NetworkManager() vslot 0x14(2, false): the manager's own highway tool (no reference).
    virtual cSC4NetworkTool* HighwayTool() = 0;
    virtual std::int64_t Funds() = 0;                                    // SL::spCity vslot 0xd0 -> budget vslot 0x18
    virtual void SpendFunds(std::int64_t amount) = 0;                    // budget vslot 0x20 (low, high)
    virtual void PlaySound(std::uint32_t id) = 0;                        // SL::spSS vslot 0x28(1, id, 0, 0, 0)
    virtual bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;   // SC4LoadString
    // The view window's window manager vslot 0x88: the cursor position in the view (false without one).
    virtual bool CursorPosition(int& x, int& y) = 0;
    // SL::spGS vslot 0x3c, then vslot 0x48 on its result: the move's last call (the graphics system is not recovered).
    virtual void GraphicsSystemUpdate() = 0;
};
INetworkIntxToolHost* NetworkIntxToolHost();
void SetNetworkIntxToolHost(INetworkIntxToolHost* host);

class cSC4ViewInputControlNetworkIntxTool final : public cSC4ViewInputControl {
public:
    explicit cSC4ViewInputControlNetworkIntxTool(std::uint32_t ruleId);   // 0x002afe12
    ~cSC4ViewInputControlNetworkIntxTool() override;                      // 0x006f3172

    bool Init() override;                                                 // 0x002aed7e
    bool Shutdown() override;                                             // 0x002af150
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;  // 0x002aee1e
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;    // 0x002aefa6
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;      // 0x002aec00
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;     // 0x002af252
    bool OnMouseExit() override;                                          // 0x002aecae

    // The part of OnMouseMove after the cursor's cell is known: find the rule of the ring that fits at the cell (or a
    // neighbour), the cursor text and cost, the funds check and the preview effect. True when a rule fits.
    bool UpdatePlacement(int cellX, int cellZ);
    // OnMouseDownL after the capture: place the fitting rule and pay for it, or play the refusal sound.
    void PlaceOrRefuse();
    // OnKeyDown's ring steps, without the cursor re-run: Home (prev), End (next) of the rotation ring, Tab / Shift+Tab of the
    // type ring.
    void StepRotationRing(bool forward);
    void StepTypeRing(bool forward);

    std::uint32_t RuleId() const { return ruleId_; }
    bool Fits() const { return fits_; }
    int AnchorX() const { return anchorX_; }
    int AnchorZ() const { return anchorZ_; }
    std::int64_t Cost() const { return cost_; }
    std::string CursorText() const { return cursorText_.ToChar(); }
    const std::string& PreviewEffectName() const { return previewName_; }

private:
    cISC4View3DWin* View() const;
    void StopPreview();
    void UpdatePreviewEffect(const nSC4Networks::cIntRule* rule);
    void RerunAtCursor();

    INetworkInputToolTerrainView* terrainView_ = nullptr;   // +0x28
    float cellSize_ = 0.0f;                                 // +0x2c
    cS3DVector3 lastPosition_{-1.0f, -1.0f, -1.0f};         // +0x30
    ILocalGridEffect* outline_ = nullptr;                   // +0x3c
    ILocalGridEffect* preview_ = nullptr;                   // +0x40
    cSC4LocalGridHelper localGrid_;                         // +0x44
    const nSC4Networks::cIntRule* lastRule_ = nullptr;      // +0x48
    std::uint32_t ruleId_ = 0;                              // +0x4c
    int anchorX_ = 0;                                       // +0x50
    int anchorZ_ = 0;                                       // +0x54
    std::int64_t cost_ = 0;                                 // +0x58
    cRZString cursorText_;                                  // +0x60
    std::string previewName_;                               // +0x6c
    bool fits_ = false;                                     // +0x78
    bool unknown79_ = false;                                // +0x79
};

}  // namespace sc4
