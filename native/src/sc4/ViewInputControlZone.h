// cSC4ViewInputControlZone — the city-view zoning tool (the zone, de-zone and landfill drag; 29 functions at
// 0x1003c37aa..0x1003c5d62, class id 0xc6bb8345 as every view tool, message target iid 0x452294aa at +0x40).
//
// The user drags a rectangle of cells (or holds still for 750 ms to flood-fill the cells around the start cell that
// are free, or, for de-zoning, zoned); every change of the rectangle re-runs cSC4ZoneManager::PlaceZone in its test
// mode (ComputeAndMarkSelectedArea), which answers whether the zoning is possible, what it costs and which lots it
// would demolish. The tool then paints the answer (highlights, the cost indicator, the "n buildings will be demolished"
// cursor text, the failure reason) and the mouse-up runs PlaceZone again for real.
//
// Object layout of the original (0x100 bytes):
//   +0x10 tool id 0xa766c101   +0x14 cursor id (kZoneCursorId[type])   +0x38 the terrain-view overlay state saved on mouse down
//   +0x40 cIGZMessageTarget2   +0x48 terrain (cell size source, referenced)   +0x50 zone manager   +0x58 lot manager
//   +0x60 sound system   +0x68 zone type   +0x6c dragging   +0x70/+0x74 drag start cell   +0x78/+0x7c drag end cell
//   +0x80 float ms the pointer rested on the end cell   +0x88 flood region (cRZCellMap<bool> with its rect)
//   +0x90..+0x9c selected rectangle (x0, z0, x1, z1)   +0xa0 float cell size   +0xa8 / +0xb0 the cursor text
//   +0xd0 set of the lots the zoning would demolish   +0xe8 cSC4LocalGridHelper   +0xf0 the "local_tile_outline" effect
//
// Everything outside the object goes through IZoneToolHost, one method per original call with its vtable slot in the
// comment (docs/SEAMS.md is generated).
#pragma once
#include <cstdint>
#include <list>
#include <memory>
#include <set>
#include <string>

#include "gz/cIGZMessage2.h"
#include "sc4/LocalGridHelper.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControl.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4ZoneManager.h"

namespace sc4 {

constexpr std::uint32_t kZoneToolId = 0xa766c101;
constexpr std::uint32_t kZoneToolMessageTarget = 0x452294aa;
constexpr std::uint32_t kZoneToolMsgGridChanged = 0x277a9364;   // registered with the message server
constexpr std::uint32_t kZoneToolMsgTick = 0x86ad10ee;          // the animation timer's tick; data3 = elapsed milliseconds
constexpr std::uint32_t kZoneToolTickInterval = 0xc0;           // ATM vslot 0x28(target, 0xc0)
constexpr std::uint32_t kZoneToolCursorText = 0xa9c87cf4;       // SetCursorText / ClearCursorText id
constexpr float kZoneToolFloodDelay = 750.0f;

// The zone-type tables of the binary (0x100a1eae0 / eb20 / eb60 / eba0 / ebe0, 16 entries each).
extern const std::uint32_t kZoneCursorId[16];
extern const std::uint32_t kZoneDragSound[16];
extern const std::uint32_t kZoneDragErrorSound[16];
extern const std::uint32_t kZoneClickSound[16];
extern const std::uint32_t kZonePlopSound[16];

// cSTETerrainView3D's zone-highlight calls (SL::spTerrain vslot 0x210).
class IZoneToolTerrainView : public cIGZUnknown {
public:
    // vslot 0x38: one highlighted cell (the original passes shape 0, 0, layer 4, the zone colour and 0).
    virtual void HighlightCell(int x, int z, std::uint32_t zoneColor) = 0;
    // vslot 0x28(rect, 0, 1): the failed selection.
    virtual void HighlightRegionError(const SC4Rect<int>& rect) = 0;
    virtual void ClearHighlight() = 0;               // vslot 0x48
    virtual bool SetGridShown(bool shown) = 0;       // vslot 0x50: returns the previous state
    virtual bool GridShown() = 0;                    // vslot 0xa0
};

// The tile-outline effect under the cursor (SL::spEffectsManager "local_tile_outline").
class IZoneToolEffect : public cIGZUnknown {
public:
    virtual void Start(int transition) = 0;          // vslot 0x18
    virtual void Stop(int transition) = 0;           // vslot 0x20
    virtual bool IsStarted() = 0;                    // vslot 0x28
    virtual void SetCell(float cellX, float cellZ) = 0;   // vslot 0x40
};

class IZoneToolHost {
public:
    virtual ~IZoneToolHost() = default;
    virtual cISC4View3DWin* View() = 0;                              // SL::spView3D
    virtual float CellSize() = 0;                                    // SL::spTerrain vslot 0x30
    virtual IZoneToolTerrainView* TerrainView() = 0;                 // SL::spTerrain vslot 0x210 (one reference)
    virtual cSC4ZoneManager* ZoneManager() = 0;                      // SL::spZoneManager
    virtual cISC4ZoneDeveloper* ZoneDeveloper() = 0;                 // SL::spZoneDeveloper
    virtual int CityCellsX() = 0;                                    // SL::spCity vslot 0x2e8
    virtual int CityCellsZ() = 0;                                    // SL::spCity vslot 0x2f0
    virtual bool CellIsOpenGround(int x, int z) = 0;                 // SL::spLotManager vslot 0xa0(...) == 8
    // The building of a lot on the demolition list: the highlight (mode 5 = demolish, 0 = off) goes to it. Answers 1
    // when the lot has a building, 2 when it is an empty lot that still counts (lot vslot 0x170), else 0.
    virtual int HighlightLot(cSC4Lot* lot, int mode) = 0;            // lot vslot 0x180 -> occupant vslot 0x18 -> 0x88
    virtual bool TutorialAllowsZone(const SC4CellRegion<int>& region, bool flood) { (void)region; (void)flood; return true; }   // SL::spTutorialSystem (PlaceZone 0x002db59c)
    virtual cIGZMessageServer2* MessageServer() = 0;                 // SL::spMS2
    virtual void StartTick(cIGZMessageTarget2* target, std::uint32_t interval) = 0;   // SL::ATM() vslot 0x28
    virtual void StopTick(cIGZMessageTarget2* target, std::uint32_t interval) = 0;    // SL::ATM() vslot 0x30
    virtual void PlaySound(std::uint32_t id) = 0;                    // SL::spSS vslot 0x50(1, id, 0, 0, 0)
    virtual void LoadString(std::uint32_t group, std::uint32_t instance, std::string& out) = 0;   // cSLString
    virtual IZoneToolEffect* CreateTileOutlineEffect() { return nullptr; }   // SL::spEffectsManager vslot 0x30
};
IZoneToolHost* ZoneToolHost();
void SetZoneToolHost(IZoneToolHost* host);

class cSC4ViewInputControlZone : public cSC4ViewInputControl, public cIGZMessageTarget2 {
public:
    explicit cSC4ViewInputControlZone(int zoneType);   // @ 0x1003c37aa
    ~cSC4ViewInputControlZone() override;

    // cIGZUnknown / cIGZMessageTarget2
    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1003c3b24
    std::uint32_t AddRef() override { return cSC4ViewInputControl::AddRef(); }
    std::uint32_t Release() override { return cSC4ViewInputControl::Release(); }
    bool DoMessage(cIGZMessage2* message) override;                // @ 0x1003c3b6c

    bool Init() override;                                          // @ 0x1003c3906
    bool Shutdown() override;                                      // @ 0x1003c3a0a
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;   // @ 0x1003c4ed6
    bool OnKeyUp(std::uint32_t key, std::uint32_t modifiers) override;     // @ 0x1003c4f54
    void EndInput() override;                                      // @ 0x1003c4fa6
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;     // @ 0x1003c50d6
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;      // @ 0x1003c52ba
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;       // @ 0x1003c54e6
    bool OnMouseExit() override;                                   // @ 0x1003c5a7c

    // Inspection for the tests / the owner.
    int ZoneType() const { return zoneType_; }
    bool Dragging() const { return dragging_; }
    const std::string& CursorText() const { return cursorText_; }

    void DoCursorUpdate();                                         // @ 0x1003c3c10
    std::unique_ptr<SC4CellRegion<int>> FloodFillRegion(const SC4Point<int>& start);   // @ 0x1003c3dba
    bool ComputeAndMarkSelectedArea();                             // @ 0x1003c4116
    static void GetFailureReasonString(int reason, std::string& out);   // @ 0x1003c4b82
    void ClearHighlights();                                        // @ 0x1003c4ce8
    bool PlaceZone(int zoneType, SC4CellRegion<int>& region, bool setRectangle, bool hasMask,
                   std::int64_t* cost, int* reason);               // @ 0x1003c5972
    bool TestCell(const SC4Point<int>& cell, const SC4Point<int>& origin, int distance,
                  const std::list<SC4Point<int>>& visited);        // @ 0x1003c5b08

private:
    void SetOptionsFromModifiers(std::uint32_t modifiers);
    cIGZMessageTarget2* AsTarget() { return static_cast<cIGZMessageTarget2*>(this); }
    cISC4View3DWin* View() const;
    void SetCursorTextIfChanged(const std::string& text);

    int zoneType_ = 0;                          // +0x68
    bool dragging_ = false;                     // +0x6c
    SC4Point<int> start_, end_;                 // +0x70 / +0x78
    float restMs_ = 0.0f;                       // +0x80
    std::unique_ptr<SC4CellRegion<int>> flood_; // +0x88
    SC4Rect<int> selected_;                     // +0x90
    float cellSize_ = 16.0f;                    // +0xa0
    std::string cursorText_;                    // +0xb0
    std::set<cSC4Lot*> demolishLots_;           // +0xd0
    cSC4LocalGridHelper localGrid_;             // +0xe8
    IZoneToolEffect* outline_ = nullptr;        // +0xf0
    IZoneToolTerrainView* terrainView_ = nullptr;   // +0x48
    bool savedGrid_ = false;                    // +0x38
    bool ticking_ = false;
};

}  // namespace sc4
