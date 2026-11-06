// cSC4AnimationCoordinator — decides which occupants with ANIMATED models (model class 0xea123be1 / 0xaa123bf9: the
// sprite and mesh animations props use) tick. Recovered from the Mac i386 build (0x00284b64..0x00285dda).
//
// The city is cut into "occupant sets" of 16 x 16 cells (256 world units); every animatable occupant the model maker shows
// (SetOccupantAnimation) is kept in its set's hash set. An occupant animates while the city detail preference is at least 2,
// the view zoom is at least 2, its set lies in the visible terrain rectangle (the view utilities' +0x3ec..+0x3f8, world
// units / 256) and ShouldAnimate says yes (powered when the exemplar asks for power with 0xea5393ed, funded at least the
// percentage property 0x8a5f88e3 = {budget purpose, percent} asks for). Animating = registered with the animation tick
// manager (priority 0xc0) through the occupant's cIGZMessageTarget2, occupant flag 0x20 set; the occupant's own DoMessage
// advances its models (cSC4PropOccupant::DoMessage).
//
// Original layout: +0 cIGZMessageTarget2 / IModelAnimationCoordinator, +4 refcount, +8 initialised, +0xc ATM, +0x10 occupant
// manager, +0x14 cSC4AnimatableOccupantFilter, +0x18 cSC4VisibleAnimatableOccupantFilter, +0x1c power simulator, +0x20 budget
// simulator, +0x24 lot manager, +0x28 view zoom (ctor 4), +0x2c least zoom that animates (2), +0x30..+0x3c visible set rect,
// +0x40 vector<vector<hash_set<occupant ref>>>, +0x4c sets per side, +0x50 occupants held, +0x54 detail allows animation.
// The +0x18 filter is created in Init and released in Shutdown but nothing in the class reads it (left out here); the
// occupant manager (+0x10), likewise only held.
//
// Messages: 0x8a07ae00 (cell block data1..4) refreshes the block's sets, 0x6a5fd3fd (local funding changed: the payload's
// 0x26bb767e object is the occupant) refreshes the occupant's lot, 0x231bbf91 re-reads the preference, 0xa6b79612 (camera
// moved) refreshes the sets entering or leaving the view, 0xa6b79602 (zoom / rotation: void1 old, void2 new {zoom, rotation})
// refreshes everything when the zoom crosses the threshold or the rotation changes above it.
#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "sc4/ModelMaker.h"

namespace sc4 {

class cISC4Occupant;

class IAnimationCoordinatorHost {
public:
    virtual ~IAnimationCoordinatorHost() = default;
    virtual cIGZMessageServer2* Messages() = 0;                                          // SL::spMS2
    virtual bool AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0;    // SL::ATM() vslot 0x14
    virtual bool RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) = 0; // SL::ATM() vslot 0x18
    virtual bool IsOccupantPowered(cISC4Occupant* occupant) = 0;                         // spPowerSim vslot 0x7c
    // spLotManager GetOccupantLot -> lot GetBuilding -> spBudgetSim GetBudgetItemForPurpose(building's holder, purpose):
    // the item's local funding (1.0 = 100 %); false when there is no lot, building or item (the original then uses 0).
    virtual bool OccupantFunding(cISC4Occupant* occupant, std::uint32_t purpose, float& funding) = 0;
    // spLotManager GetOccupantLot -> lot vslot 0x38: the lot's cell rectangle {x0, z0, x1, z1}; false without a lot.
    virtual bool OccupantLotCells(cISC4Occupant* occupant, int out[4]) = 0;
    virtual int ViewZoom() = 0;                                                          // spViewUtilities +0xc
    virtual void VisibleTerrainRect(float out[4]) = 0;                                   // spViewUtilities +0x3ec..+0x3f8
    virtual int CityCellsX() = 0;                                                        // spCity vslot 0x174
    virtual int CityDetail() = 0;                                                        // spPreferences +0xf16; 0 without preferences
    virtual int OccupantModelClass(cISC4Occupant* occupant) = 0;                         // cSC4ModelMakerUtility::GetOccupantModelClass
};

class cSC4AnimationCoordinator final : public IModelAnimationCoordinator, public cIGZMessageTarget2 {
public:
    explicit cSC4AnimationCoordinator(IAnimationCoordinatorHost* host) : host_(host) {}
    ~cSC4AnimationCoordinator();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    void Shutdown() override;
    bool Init() override;
    void SetOccupantAnimation(cISC4Occupant* occupant, bool on) override;
    bool DoMessage(cIGZMessage2* message) override;

    // read by the kit and the tests
    int HeldCount() const { return held_; }
    int SetsPerSide() const { return sets_; }
    bool DetailAllows() const { return detailAllows_; }
    void VisibleSetRect(int out[4]) const { for (int i = 0; i < 4; ++i) out[i] = visible_[i]; }
    static bool IsAnimatableClass(int modelClass);   // cSC4AnimatableOccupantFilter's model class list

private:
    struct OccupantHash { std::size_t operator()(cISC4Occupant* o) const { return reinterpret_cast<std::size_t>(o); } };
    using OccupantSet = std::unordered_set<cISC4Occupant*, OccupantHash>;   // each entry holds a reference

    bool IsIncluded(cISC4Occupant* occupant);
    bool ShouldAnimate(cISC4Occupant* occupant);
    void AddToTickManager(cISC4Occupant* occupant, bool add);
    void GetOccupantSetCell(cISC4Occupant* occupant, int& x, int& z) const;
    bool InVisible(int x, int z) const;
    void UpdateSet(OccupantSet& set, int x, int z);
    void UpdateRect(const int rect[4]);
    void UpdateCellRect(const int cells[4]);
    void UpdateAll();
    void ReadVisibleRect(int out[4]) const;
    void EvaluatePreferences();
    void UpdateVisibleSetRect();

    IAnimationCoordinatorHost* host_;
    std::uint32_t refCount_ = 0;
    bool initialized_ = false;          // +8
    int zoom_ = 4;                      // +0x28
    int leastZoom_ = 2;                 // +0x2c
    int visible_[4] = {0, 0, 0, 0};     // +0x30 x0, z0, x1, z1
    std::vector<std::vector<OccupantSet>> grid_;   // +0x40 [x][z]
    int sets_ = 0;                      // +0x4c
    int held_ = 0;                      // +0x50
    bool detailAllows_ = true;          // +0x54
};

}  // namespace sc4
