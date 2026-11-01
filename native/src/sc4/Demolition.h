// cSC4Demolition — the demolition service (33 functions with thunks, 0x1000eaf70..0x1000ee64a, class id 0x0990c38a (the type of its
// city-file record; the city keeps it as interface 0x26dde2ea),
// message target 0x452294aa, serializable 0xe4fda3d4). Layout: +0x1c initialized, +0x20/+0x24 city cells x/z,
// +0x28 default occupant filter, +0x30 city, +0x38 building development simulator, +0x40 occupant manager,
// +0x48 terrain, +0x50 current region.
//
// Start up / shut down, tuning exemplars, the one-version save record, the region sweep (DemolishRegion), the lot
// batch entry (DemolishLots), the per-type occupant switch (DemolishOccupant), the network / pole follow-up pass
// (UpdateNetworks), terrain levelling (ModifyTerrainHeight) and the occupant / group effects.
// Quirks kept: DoDemolishGroupEffect receives a group size of 0 from DemolishRegion (the occupant count goes into an
// unused argument); a lot's occupants are all-or-nothing (one filtered-out building or prop vetoes the whole lot).
#pragma once
#include <cstdint>
#include <list>
#include <functional>
#include <set>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4Demolition = 0x0990c38a;
constexpr std::uint32_t kMessageTuningReloaded = 0xc53d10aa, kMessageCityInit = 0x26d31ec1, kMessageDemolishRequest = 0xa9ded179;
constexpr std::uint32_t kMessageOccupantDemolished = 0xaa7a014d;

// Demolition options (the `flags` argument)
constexpr std::uint32_t kDemolishSkipProtected = 0x100;       // occupants with removal flag 0x40 are left alone
constexpr std::uint32_t kDemolishLotsOnly = 0x2000;           // set by DemolishLots: only the lots' own cells count

// Occupant type ids switched on by DemolishOccupant
constexpr std::uint32_t kDemolishTypeBuilding = 0x278128a0, kDemolishTypeProp = 0xa823821e, kDemolishTypeNetwork = 0xc772bf98;
constexpr std::uint32_t kDemolishTypeSpecial = 0x096e763c, kDemolishTypeFoundation = 0x68fd0c69, kDemolishTypeFlora = 0x74758926;
constexpr std::uint32_t kDemolishTypePole = 0x2890d4de;

constexpr std::uint32_t kPropertyModelSmallMedium = 0x6a8a09d2, kPropertyModelMediumLarge = 0x6a8a09d3;
constexpr std::uint32_t kPropertyGroupSmallMedium = 0x6a8a09d4, kPropertyGroupMediumLarge = 0x6a8a09d5;
constexpr std::uint32_t kPropertyDemolishLotTileCost = 0x2ab845e7;
constexpr std::uint32_t kPropertyPropDemolishEffect = 0x8a5e5db8, kPropertyDemolishSound = 0x0a93036f;
constexpr std::uint32_t kEffectSetDemolish = 0x6a3589a0;
constexpr std::uint32_t kDemolishFlagEffectsMask = 0xff, kDemolishFlagKeepLotZone = 0x800, kDemolishFlagNoBuildingRestore = 0x1000;
constexpr std::uint32_t kDemolishFlagAnyLotPolicy = 0x300;

struct DemolitionCellRect { int x0 = 0, z0 = 0, x1 = 0, z1 = 0; };   // inclusive cells

// A set of cells: bounds plus an optional per-cell mask indexed [x - x0][z - z0] (cRZCellMap); no mask = every cell.
struct DemolitionRegion {
    DemolitionCellRect bounds;
    std::vector<std::vector<bool>> mask;
    bool Contains(int x, int z) const {
        if (mask.empty()) return x >= bounds.x0 && x <= bounds.x1 && z >= bounds.z0 && z <= bounds.z1;
        unsigned dx = x - bounds.x0, dz = z - bounds.z0;
        return dx < mask.size() && dz < mask[dx].size() && mask[dx][dz];
    }
};

// The occupant as demolition sees it.
class IDemolishable {
public:
    virtual ~IDemolishable() = default;
    virtual std::uint32_t Type() = 0;                       // vslot 0x38
    virtual bool HasRemovalFlag(std::uint32_t flag) = 0;    // vslot 0x100 (0x40 protected, 1 "has network", ...)
    virtual std::uint32_t Flags() = 0;                      // vslot 0x108 (bit 0: network piece needing an update pass)
    virtual void Remove() = 0;                              // vslot 0x20
    virtual void BoundingBox(float out[6]) = 0;             // vslot 0x50: min x,y,z then max x,y,z (metres)
    virtual cISCPropertyHolder* Properties() = 0;           // vslot 0x30
    virtual bool TestFlag(std::uint32_t flag) = 0;          // vslot 0xb0 (props: 0x5004 = animated)
    virtual bool NetworkHasFlag(int flag, bool* isNetwork) = 0;   // QI 0xa821ef94 then vslot 0xb0 (1 road-like, 2 ...)
};

// The filter object of the 3D view and the demolition filters (cISC4OccupantFilter).
class IDemolishFilter {
public:
    virtual ~IDemolishFilter() = default;
    virtual bool IsOccupantIncluded(IDemolishable* occupant) = 0;      // vslot 0x18
    virtual bool IsOccupantTypeIncluded(std::uint32_t type) = 0;       // vslot 0x20
};

// State of the service's default occupant filter (cSC4DemolitionOccupantFilter at +0x28): the caller's filter at +0x30,
// the view filter at +0x38, active flag +0x28, mode +0x2c (1 = no privilege, 2 = privileged) and the rect scan window.
struct DemolitionFilterState {
    IDemolishFilter* user = nullptr;
    IDemolishFilter* view = nullptr;
    bool active = false;
    int mode = 0;
    bool rectScan = false;
};

// A lot as demolition sees it.
class IDemolishableLot {
public:
    virtual ~IDemolishableLot() = default;
    virtual DemolitionCellRect Bounds() = 0;                // vslot 0x70
    virtual bool IsPlopped() = 0;                           // vslot 0x138
    virtual std::vector<IDemolishable*> Occupants() = 0;    // vslot 0x188
    virtual int TypeCode() = 0;                             // vslot 0x108 (6 = destroyed lot, 1 = empty lot)
    virtual int ZoneClass() = 0;                            // vslot 0x118 (0 / 1 = no tint)
    virtual int ZoneType() = 0;                             // vslot 0xf8
    virtual std::uint8_t Orientation() = 0;                 // vslot 0xb8
    virtual void Size(std::uint8_t& width, std::uint8_t& depth) = 0;   // vslot 0xb0
    virtual IDemolishable* BuildingOccupant() = 0;          // vslot 0x180 -> ->0x18 (null: no building yet)
    virtual bool BuildingRemovable(int level) = 0;          // that occupant's vslot 0x70 with 2
    virtual bool BuildingPlain() = 0;                       // vslot 0x48 object's vslot 0x70 (commit stage), true = plain
    virtual bool HasBuildingObject() = 0;                   // vslot 0x48 non-null
    virtual bool BlocksTerrainChange() = 0;                 // vslot 0x220
};

// A visual effect instance of the effects manager.
class IDemolitionEffect {
public:
    virtual ~IDemolitionEffect() = default;
    virtual bool Attach(IDemolishable* occupant) = 0;       // vslot 0x48
    virtual void SetViewpoint(int x, int y) = 0;            // vslot 0x58 (type 9, from ViewportPositionToRay)
    virtual void SetGrayTint() = 0;                         // vslot 0x60 (4, kColorGray50, 3)
    virtual void Play() = 0;                                // vslot 0x18
};

// Everything the service reaches through the service locator.
class IDemolitionHost {
public:
    virtual ~IDemolitionHost() = default;
    virtual void AddNotification(std::uint32_t message) = 0;
    virtual void RemoveNotification(std::uint32_t message) = 0;
    virtual void PostOccupantMessage(std::uint32_t message, IDemolishable* occupant, bool sendNow) = 0;
    virtual cISCPropertyHolder* TuningExemplar(std::uint32_t group, std::uint32_t instance) = 0;   // type 0x6534284a
    virtual void CityCells(int& x, int& z) = 0;                        // city vslot 0x2e8 / 0x2f0
    // the 3D view: its occupant filter (vslot 0x90, out parameter) and the begin-sweep notification (vslot 0xb0)
    virtual IDemolishFilter* ViewOccupantFilter() = 0;
    virtual void ViewBeginSweep() = 0;
    // default occupant filter (cSC4DemolitionOccupantFilter slot 0x18) and the stack-local cSC4DefaultOccupantFilter
    virtual bool OccupantAllowedByFilter(IDemolishable* occupant, const DemolitionFilterState& state) = 0;
    virtual bool LocalFilterIncludes(IDemolishable* occupant) = 0;
    // occupant manager
    virtual std::vector<IDemolishable*> OccupantsInRect(const DemolitionCellRect& cells, const DemolitionFilterState& state) = 0;   // vslot 0x148
    virtual std::vector<IDemolishable*> OccupantsForTerrain(const DemolitionCellRect& cells, int mode) = 0;   // vslot 0x148 with the terrain filter
    virtual bool RemoveOccupant(IDemolishable* occupant, bool refund, bool deferred) = 0;    // vslot 0xa8
    virtual void ReleaseOccupantList(std::vector<IDemolishable*>& list) = 0;                 // vslot 0x180
    virtual void OccupantManagerCentre(int x, int z, float& a, float& b) = 0;               // vslot 0x90
    // lot manager and lot-related services
    virtual IDemolishableLot* LotAtCell(int x, int z) = 0;             // lot manager vslot 0x28
    virtual IDemolishableLot* LotOfOccupant(IDemolishable* occupant) = 0;   // city metres -> cell -> lot
    virtual std::vector<IDemolishableLot*> LotsInRect(const DemolitionCellRect& cells) = 0;  // vslot 0x38
    virtual void RemoveLot(IDemolishableLot* lot) = 0;                 // vslot 0xc0
    virtual void CreateLotAt(int x, int z, std::uint8_t orientation) = 0;   // vslot 0xb0, 1x1
    virtual int ZoneTypeAt(int x, int z) = 0;                          // zone manager vslot 0x60
    virtual void ClearZones(const DemolitionCellRect& cells) = 0;      // zone manager vslot 0x58 over a full cell map
    virtual void LotPrepareRemoval(IDemolishableLot* lot) = 0;         // building development vslot 0x60
    virtual void LotFinishRemoval(IDemolishableLot* lot) = 0;          // vslot 0x68
    virtual void LotSkipRemoval(IDemolishableLot* lot, bool flag) = 0; // vslot 0x70
    virtual void NetworkBatch(bool begin) = 0;                         // network manager vslot 0x38
    virtual void UpdateNetworkManager(const std::vector<IDemolishable*>& networks) = 0;   // vslot 0x30
    virtual void NotifyNeighborsNetworkChange(IDemolishable* network) = 0;   // neighbors simulator vslot 0x40
    // terrain
    virtual float TerrainHeight(int x, int z) = 0;                     // vslot 0x70
    virtual bool TerrainCellSkipped(int x, int z) = 0;                 // vslot 0x128
    virtual void TerrainLevelCell(int x, int z) = 0;                   // vslot 0x190
    virtual void TerrainLevelRect(const DemolitionCellRect& cells, std::uint32_t reason) = 0;   // vslot 0x198
    virtual void TerrainUpdateRect(const DemolitionCellRect& cells, std::uint32_t reason) = 0;  // vslot 0x228 (1, 1, rect)
    virtual float TerrainHeightAtMetres(float x, float z) = 0;         // vslot 0x60
    // per-type services
    virtual bool PropManagerAvailable() = 0;
    virtual bool PropManagerRemove(IDemolishable* prop) = 0;           // prop manager vslot 0xe8
    virtual bool NetworkCost(IDemolishable* network, std::int64_t& cost) = 0;      // QI 0xa821ef94, vslot 0x1f8
    virtual bool BuildingCost(IDemolishable* building, std::int64_t& cost) = 0;    // city vslot 0x1a8 object, vslot 0x48
    virtual bool FloraCost(IDemolishable* flora, std::int64_t& cost) = 0;          // city vslot 0x1d8 object, vslot 0xb0
    virtual bool PoleCost(IDemolishable* pole, std::int64_t& cost) = 0;            // QI 0xa8c86142, vslot 0x50
    virtual std::vector<IDemolishable*> PoleConnections(IDemolishable* pole) = 0;  // slots 0x20 / 0x38
    virtual void PoleDetach(IDemolishable* pole) = 0;                              // slot 0x30 with 1
    virtual bool IsPlainOccupant(IDemolishable* occupant) = 0;                     // connection's own slot 0x20 == 0
    // effects
    virtual IDemolitionEffect* CreateEffect(std::uint32_t effectSet, std::uint32_t index) = 0;   // effects manager vslot 0x38
    virtual void ReleaseEffect(IDemolitionEffect* effect) = 0;
    virtual void PlayGroupEffect(std::uint32_t index, float x, float height, float z) = 0;
    virtual void PlaySound(std::uint32_t soundId) = 0;                 // sound service vslot 0x50 (1, id)
    virtual float RenderMinimumSize() = 0;                             // render properties +0x278
    virtual float ViewScale() = 0;                                     // view utilities +0x15c
};

class cSC4Demolition {
public:
    explicit cSC4Demolition(IDemolitionHost* host) : host_(host) {}

    bool Init();
    bool Shutdown();
    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4Demolition; }
    void SetDefaultOccupantFilter(void*) {}
    // returns true when `message` was one of ours
    bool DoMessage(std::uint32_t type);

    void ReadProperties();
    float SmallMediumModelThreshold() const { return smallMediumModel_; }
    float MediumLargeModelThreshold() const { return mediumLargeModel_; }
    std::uint32_t SmallMediumGroupThreshold() const { return smallMediumGroup_; }
    std::uint32_t MediumLargeGroupThreshold() const { return mediumLargeGroup_; }
    std::int32_t CostToDemolishDestroyedLotTile() const { return lotTileCost_; }
    int CityCellsX() const { return cellsX_; }
    int CityCellsZ() const { return cellsZ_; }

    // `privilege` 0 = none; `flags` low byte = effect index, 0x2000 lots-only, 0x100 skip protected, 0x300 lot policy,
    // 0x800 keep lot zone, 0x1000 (see the kDemolishFlag* constants).
    bool DemolishRegion(bool commit, const DemolitionRegion& region, int privilege, std::uint32_t flags, bool keepZones,
                        IDemolishFilter* filter, std::int64_t* cost, std::set<IDemolishable*>* demolished,
                        IDemolishable* primary, int viewX, int viewY);
    bool DemolishOccupant(bool commit, IDemolishable* occupant, std::uint32_t flags, std::int64_t* cost, bool skipNetworks,
                          std::vector<IDemolishable*>* released);
    bool DemolishLots(bool commit, const std::list<IDemolishableLot*>& lots, int privilege, std::uint32_t flags, bool keepZones,
                      IDemolishFilter* filter, std::int64_t* cost, std::set<IDemolishable*>* demolished);
    void UpdateNetworks(const std::set<IDemolishable*>& occupants);
    std::vector<IDemolishable*> GetDemolishableLotOccupants(IDemolishableLot* lot);
    std::vector<IDemolishable*> GetDemolishableRectOccupants(const DemolitionCellRect& rect);
    void PostMessage(std::uint32_t message, IDemolishable* occupant, bool sendNow);

    // returns the number of cells that need lowering / raising to `height`
    int ModifyTerrainHeight(bool apply, bool raise, int x0, int z0, int x1, int z1, float height,
                            IDemolishableLot* ignoredLot, std::uint32_t reason);
    void DoDemolishOccupantEffect(IDemolishable* occupant, std::uint32_t flags, IDemolishableLot* lot, bool primary, int viewX,
                                  int viewY);
    void DoDemolishGroupEffect(std::uint32_t flags, int groupSize, int unused, const DemolitionCellRect& bounds);

private:
    IDemolitionHost* host_;
    bool initialized_ = false;
    int cellsX_ = 0, cellsZ_ = 0;
    float smallMediumModel_ = 0, mediumLargeModel_ = 0;
    std::uint32_t smallMediumGroup_ = 0, mediumLargeGroup_ = 0;
    std::int32_t lotTileCost_ = 0;
    DemolitionFilterState filter_;          // +0x28
    const DemolitionRegion* region_ = nullptr;   // +0x50
};

}  // namespace sc4
