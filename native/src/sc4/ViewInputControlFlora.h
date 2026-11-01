// cSC4ViewInputControlFlora — the mayor-mode tree tool: a click (or a drag) plants one tree of the chosen species where the
// cursor meets the terrain, for the species' price (9 functions, Mac i386 0x2d0018..0x2d0576 + the destructors at 0x6f7534).
// Layout (i386): +0x28 flora type, +0x2c the flora simulator, +0x30 the hover spot is valid, +0x31 dragging, +0x34 cost (64-bit),
// +0x3c money spent in this drag (64-bit), +0x44 the last plant failed for lack of funds.
// Quirks kept:
//  * Init writes tool id 0x26d7881c and cursor id 0x66db057f and shows the species' price as the cost indicator.
//  * A press with too little money does nothing (no "no" cursor); a drag sets the funds flag and the cursor follows it.
//  * The press only adds the cost to the drag total when the withdrawal succeeds; a drag adds it regardless.
//  * A tree refused on water switches the cursor to the "no" cursor (0xaa62c9d5) until the next update.
//  * The release shows the unit price again when nothing was spent, else clears the indicator.
#pragma once
#include <cstdint>

#include "sc4/ViewInputControl.h"

namespace sc4 {

class cISC4View3DWin;

// The services the tool reaches through SL::spFloraSim / spBudgetSim / spAuraSim / spSS and its view.
class IFloraToolHost {
public:
    virtual ~IFloraToolHost() = default;
    virtual cISC4View3DWin* View() = 0;
    virtual bool FloraSimulatorPresent() = 0;
    virtual std::int64_t FloraCost(std::uint32_t type) = 0;                                    // static data +0x160
    virtual bool LocationIsOKForNewFlora(std::uint32_t type, float x, float z) = 0;           // flora sim vslot 0x3c
    virtual bool AddNewFloraOccupant(std::uint32_t type, float xyz[3]) = 0;                   // flora sim vslot 0x48(type, xyz, false)
    virtual bool IsWaterTerrain(float x, float z) = 0;                                          // _IsWaterTerrain
    virtual std::int64_t Funds() = 0;                                                           // budget vslot 0x18
    virtual bool WithdrawFunds(std::int64_t amount) = 0;                                        // budget vslot 0x20
    virtual void AddAuraEffect(float x, float z) = 0;                                           // aura vslot 0x1c(x, z, 1)
    virtual void PlayPlantSound(const float xyz[3]) = 0;                                        // spSS 0xea451903 / 0x49dc0c90 + position
};
IFloraToolHost* FloraToolHost();
void SetFloraToolHost(IFloraToolHost* host);

constexpr std::uint32_t kFloraToolId = 0x26d7881c;
constexpr std::uint32_t kFloraToolCursor = 0x66db057f;
constexpr std::uint32_t kCursorNo = 0xaa62c9d5;

class cSC4ViewInputControlFlora : public cSC4ViewInputControl {
public:
    explicit cSC4ViewInputControlFlora(std::uint32_t floraType) : floraType_(floraType) {}   // 0x2d0018

    bool Init() override;                                                   // 0x2d006c
    bool Shutdown() override;                                               // 0x2d00fe
    bool OnKeyDown(std::uint32_t key, std::uint32_t modifiers) override;    // 0x2d014e
    bool OnMouseDownL(int x, int y, std::uint32_t modifiers) override;      // 0x2d0576
    bool OnMouseUpL(int x, int y, std::uint32_t modifiers) override;        // 0x2d01e2
    bool OnMouseMove(int x, int y, std::uint32_t modifiers) override;       // 0x2d024c

private:
    void Plant(const float xyz[3], bool countOnlyWhenPaid);
    void UpdateCursor() { SetCursor(noFunds_ ? kCursorNo : 0u); }

    std::uint32_t floraType_ = 0;     // +0x28
    bool simulator_ = false;          // +0x2c (the flora simulator, referenced while the tool is up)
    bool hoverValid_ = false;         // +0x30
    bool dragging_ = false;           // +0x31
    std::int64_t cost_ = 0;           // +0x34
    std::int64_t spent_ = 0;          // +0x3c
    bool noFunds_ = false;            // +0x44
};

}  // namespace sc4
