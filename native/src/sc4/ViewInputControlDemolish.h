// cSC4ViewInputControlDemolish — the bulldozer (Mac i386 0x002e346c..0x002e493c, 14 functions; bulldozer-tool.md). A click
// takes the lot, the occupant or the cell under the mouse; a drag takes the dragged cells, grown to whole lots for the
// developed lots that reach out of it. While the mouse moves the selection is priced by a dry run of the demolition utility
// and drawn as marked cells, green when it can go and red when it cannot; the button's release demolishes it and pays.
//
// Original layout (i386): +0x28 budget, +0x2c city, +0x30 demolition, +0x34 lot developer, +0x38 lot manager, +0x3c the
// demolishable-occupant filter, +0x40 occupant manager, +0x44 dragging, +0x48 the clicked lot / occupant rect, +0x58 the press
// cell, +0x60 the current cell, +0x68 the selected region, +0x6c the dry run's verdict, +0x70 the picked occupant,
// +0x74 the highlighted occupant (+0x78 its old highlight), +0x94 the cost, +0x9c the marked-cell view, +0xa0 the picked
// occupant is a signpost, +0xa4..+0xe0 four colours (highlight colours 5, 6, 9, 10).
//
// Everything outside the tool goes through IDemolishToolHost.
#pragma once
#include <cstdint>
#include <memory>

#include "sc4/Demolition.h"
#include "sc4/ViewInputControl.h"

namespace sc4 {

class cISC4Occupant;
class cSC4Lot;

constexpr std::uint32_t kDemolishToolId = 0x46ddb5f1;
constexpr std::uint32_t kDemolishToolCursor = 0xe1855adc;

class IDemolishToolHost {
public:
    virtual ~IDemolishToolHost() = default;
    // SL::spView3D vslot 0x90 PickTerrain(x, y, out, terrain vslot 0x14c "use the water surface") and city vslot 0x17c
    // PositionToCell.
    virtual bool PickCell(int x, int y, int& cellX, int& cellZ, float position[3]) = 0;
    // SL::spRenderer vslot 0x100 with the demolishable-occupant filter: the occupant under the mouse (one reference) and
    // whether it is a signpost (interface 0x4b44fbe2).
    virtual cISC4Occupant* PickOccupant(int x, int y, bool& signpost) = 0;
    virtual bool OccupantCells(cISC4Occupant* occupant, DemolitionCellRect& out) = 0;   // pole cell (0xa8c86142) or vslot 0x2c
    virtual void OccupantPosition(cISC4Occupant* occupant, float out[3]) = 0;          // occupant vslot 0x20
    virtual cSC4Lot* LotAt(int x, int z) = 0;                                            // lot manager vslot 0x14 (x, z, false)
    virtual cISC4Occupant* LotBuilding(cSC4Lot* lot) = 0;                                // lot vslot 0xc0 -> 0xc (one reference)
    // occupant manager vslot 0xa0 (the cell, type 0xc772bf98) -> the first one the demolishable filter takes (one reference)
    virtual cISC4Occupant* NetworkOccupantAt(int x, int z) = 0;
    virtual std::uint32_t Highlight(cISC4Occupant* occupant) = 0;                       // occupant vslot 0x40
    virtual void SetHighlight(cISC4Occupant* occupant, std::uint32_t highlight) = 0;   // vslot 0x44 (highlight, false)
    // spDemolition vslot 0x18 DemolishRegion(commit, region, 1, 1, false, null, cost, null, clicked, x, y) and vslot 0x20
    // DemolishOccupant(commit, occupant, 1, 1, cost, false, null).
    virtual bool DemolishRegion(bool commit, const DemolitionRegion& region, std::int64_t* cost, cISC4Occupant* clicked, int x, int y) = 0;
    virtual bool DemolishOccupant(bool commit, cISC4Occupant* occupant, std::int64_t* cost) = 0;
    virtual void PayFromFunds(std::int64_t cost) = 0;                                  // budget vslot 0x14 (total - cost)
    virtual void SetCostIndicator(std::int64_t cost) = 0;                              // spView3D vslot 0xac (cost, 0)
    virtual void ClearCostIndicator(bool force) = 0;                                   // vslot 0xb0
    // The marked-cell view (SC4MarkedCellView, layer 5 priority 1000): the region in the cell colour, or nothing.
    virtual void MarkCells(const DemolitionRegion& region, bool valid) = 0;
    virtual void ClearMarkedCells() = 0;
};
IDemolishToolHost* DemolishToolHost();
void SetDemolishToolHost(IDemolishToolHost* host);

class cSC4ViewInputControlDemolish final : public cSC4ViewInputControl {
public:
    cSC4ViewInputControlDemolish();                                       // 0x002e4850
    ~cSC4ViewInputControlDemolish() override;

    bool Init() override;                                                 // 0x002e3a8c
    bool Shutdown() override;                                             // 0x002e3724
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;  // 0x002e346c
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;    // 0x002e493c
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;      // 0x002e4564
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;     // 0x002e4748
    bool OnMouseExit() override;                                          // 0x002e3654
    void EndInput() override;                                             // 0x002e3614

    bool Dragging() const { return dragging_; }
    bool Verdict() const { return verdict_; }
    std::int64_t Cost() const { return cost_; }
    const DemolitionRegion* Selection() const { return selection_.get(); }

private:
    bool PickDemolishableOccupant(int x, int y);                          // 0x002e34a6
    void ClearSelectedRegion();                                           // 0x002e3598
    bool DoCellPick(int x, int y, int& cellX, int& cellZ);                // 0x002e3684
    bool DoLotPick(int x, int y, DemolitionCellRect& rect);               // 0x002e386e
    void UpdateSelectedRegion();                                          // 0x002e3d62
    void SetPicked(cISC4Occupant* occupant);
    void SetHighlighted(cISC4Occupant* occupant);

    bool dragging_ = false;                     // +0x44
    DemolitionCellRect clickRect_;              // +0x48
    int pressX_ = 0, pressZ_ = 0;               // +0x58
    int currentX_ = 0, currentZ_ = 0;           // +0x60
    std::unique_ptr<DemolitionRegion> selection_;   // +0x68
    bool verdict_ = false;                      // +0x6c
    cISC4Occupant* picked_ = nullptr;           // +0x70 (one reference)
    cISC4Occupant* highlighted_ = nullptr;      // +0x74 (one reference)
    std::uint32_t oldHighlight_ = 0;            // +0x78
    std::int64_t cost_ = 0;                     // +0x94
    bool pickedSignpost_ = false;               // +0xa0
};

}  // namespace sc4
