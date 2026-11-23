// cSC4ZoneManager — the zone grid and zoning statistics. Recovered from Sim City 4 Deluxe
// Editionsub 1.2.1 (vtable 0x100a7b740, 39 functions; class id 0x298f9b2d, iid 0x67652433).
//
// The zone grid is a tract-size-1 int8 grid (class 0x49b9e603) holding the zone type per city cell.
// Zone types 0..15: 0 none, 1..3 residential (low/medium/high), 4..6 commercial, 7..9 industrial,
// 0xb airport, 0xc seaport, 0xd spaceport, 0xe landfill, 0xf plopped; IsRCIZone is the mask 0x3fe
// (types 1..9). Per type it keeps the cell count, the undeveloped-cell count, minimum/maximum zone
// side, drag colour, display texture instance and the per-zone-type development cost tunables
// (exemplar {type 0x6534284a, group 0xe7e2c2db, instance 0xe9482490}, properties 0xe94825b2..b9).
//
// Not recovered yet (pending in the ledger): PlaceZone (the core zoning transaction) and the
// message handler that calls it for zoned-building removal (0x99ef1142 / 0xc772bf98).
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cGZPersistResourceKey.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZRandom.h"
#include "sc4/CommonIIDs.h"
#include "sc4/SCProperty.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {
class cISC4Occupant;

constexpr std::uint32_t kZoneManagerClassId = 0x298f9b2d;
constexpr std::uint32_t kGZIID_cISC4ZoneManager = 0x67652433;
constexpr int kZoneTypeCount = 16;

enum ZoneType : int {
    kZoneNone = 0, kZoneResidentialLow = 1, kZoneResidentialMedium = 2, kZoneResidentialHigh = 3,
    kZoneCommercialLow = 4, kZoneCommercialMedium = 5, kZoneCommercialHigh = 6, kZoneIndustrialAgriculture = 7,
    kZoneIndustrialMedium = 8, kZoneIndustrialHigh = 9, kZoneMilitary = 10, kZoneAirport = 11,
    kZoneSeaport = 12, kZoneSpaceport = 13, kZoneLandfill = 14, kZonePlopped = 15,
};

class IZoneManagerHost {
public:
    virtual ~IZoneManagerHost() = default;
    virtual int CityCellsX() = 0;
    virtual int CityCellsZ() = 0;
    virtual cISCPropertyHolder* GetTunables(const cGZPersistResourceKey& key) = 0;   // null when absent
    virtual void RegisterNotifications() {}                                           // occupant removed (0x99ef1142)
    // SL::spMS2: the manager listens to kSC4MessageInsertOccupant (0x99ef1142) through it (DoMessage, dezone under new networks).
    virtual cIGZMessageServer2* MessageServer() { return nullptr; }
    // QueryInterface(0xa821ef94) as cISC4NetworkOccupant + slot 9 HasAnyNetworkFlag(flags): -1 when the occupant is no network
    // occupant, else 1 when it has any of `flags`, 0 when none.
    virtual int NetworkOccupantHasAnyFlag(cISC4Occupant* occupant, std::uint32_t flags) { (void)occupant; (void)flags; return -1; }
    virtual void UnregisterNotifications() {}
    // The five services the manager references in its stream, in stream order and with the
    // interface ids it reads them as: city 0x66ba4db5, occupant manager 0xc6bb8334, terrain
    // 0xc70bd37e, pollution simulator 0x877181ec, budget simulator 0x6772bd23.
    virtual cIGZUnknown* GetServiceForStream(int /*index*/) { return nullptr; }
    virtual void OnServiceLoaded(int /*index*/, cIGZUnknown* /*object*/) {}
    // the building development simulator re-lights lots when the zone overlay alpha changes
    virtual void OnZoneDisplayAlphaChanged() {}
};

template <typename T>
class SC4CellRegion;
class cSC4Lot;

// What PlaceZone (0x1001993a8) reaches outside the manager, one method per original call. `this+0x50` is the budget
// simulator, +0x30 the city (occupant manager), +0x38 the prop manager, +0x40 the terrain; the lot manager, the
// demolition simulator, the zone developer and the message server are the SL::sp* globals.
class IZonePlacementHost {
public:
    virtual ~IZonePlacementHost() = default;
    virtual std::int64_t Funds() = 0;                                    // budget vslot 0x30
    virtual bool SpendFunds(std::int64_t cost) = 0;                      // budget vslot 0x40
    virtual bool NetworkAt(int x, int z) = 0;                            // occupant manager vslot 0x50(x, z, 0x1f4f, 1) -> a network occupant
    virtual bool IsWater(int x, int z) = 0;                              // terrain vslot 0x110
    virtual float CellDevelopmentCost(int x, int z) { return 0.0f; }     // building development simulator vslot 0xe8 (the slope check)
    virtual bool PropBlocksCell(int x, int z) { return false; }          // prop manager vslot 0x100(x, z, 0x6a11bd2b) found and not removable
    virtual cSC4Lot* LotAt(int x, int z) = 0;                            // lot manager vslot 0x28(x, z, 0)
    virtual void LotCells(cSC4Lot* lot, int& x0, int& z0, int& x1, int& z1) = 0;      // lot vslot 0xe
    virtual int LotKind(cSC4Lot* lot) = 0;                               // lot vslot 0x1f: kinds 11, 12 and 15 are never rezoned
    virtual bool LotCompatibleWithZone(cSC4Lot* lot, int zoneType) = 0;  // lot configuration vslot 0x78
    virtual bool LotIsProtected(cSC4Lot* lot) { return false; }          // lot vslot 0x30 -> privilege test vslot 0x70(2) fails
    virtual std::int64_t DemolishCost(const std::vector<cSC4Lot*>& lots) = 0;         // demolition vslot 0x38 (test pass)
    virtual void DemolishLots(const std::vector<cSC4Lot*>& lots) = 0;                 // demolition vslot 0x38 (do it)
    virtual std::int64_t LandfillCost(const SC4CellRegion<int>& region) { return 0; } // city vslot 0x270 -> vslot 0x48
    virtual void ApplyZone(const SC4CellRegion<int>& changed, int zoneType, bool flag) = 0;   // zone developer vslot 0x30
    virtual void PostZoneChanged(const SC4CellRegion<int>& changed, int zoneType, bool flag) = 0;   // message 0x7699455
};

// The zone manager's own interface (class id 0x298f9b2d, iid 0x67652433). The recovered port only needs
// the COM identity; the concrete methods are called directly, so the interface carries no extra slots.
class cISC4ZoneManager : public cIGZUnknown {
public:
    virtual ~cISC4ZoneManager() = default;
};

class cSC4ZoneManager : public cISC4ZoneManager, public cIGZMessageTarget2, public cIGZSerializable {
public:
    struct ZoneDevelopmentResult {
        std::int8_t zoneType = 0;
        std::int8_t result = 0;
        friend bool operator<(const ZoneDevelopmentResult& a, const ZoneDevelopmentResult& b) {
            return a.zoneType != b.zoneType ? a.zoneType < b.zoneType : a.result < b.result;
        }
    };

    explicit cSC4ZoneManager(IZoneManagerHost* host);
    ~cSC4ZoneManager() override = default;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;   // i386 0x004DCA70: dezone under every new network occupant (zones.md 5.1)

    bool Init();
    bool Shutdown();

    // 0x1001993a8. Tests (commit == false) or performs a zoning of `region`'s set cells. The region is trimmed to the
    // cells that can take the zone. Returns whether the zoning is possible; `reason` (0 ok, 1 not enough money,
    // 2 landfill on water, 3 too small, 4 too large, 5 blocked) and `cost` are optional outputs, `lots` collects the
    // lots that have to be demolished.
    bool PlaceZone(SC4CellRegion<int>& region, int zoneType, bool commit, bool freeOfCharge, bool checkTerrain,
                   bool hasMask, bool clipToRegion, std::int64_t* cost, int* reason, std::set<cSC4Lot*>* lots);
    void SetPlacementHost(IZonePlacementHost* host) { placement_ = host; }

    cSC4SimGrid<std::int8_t>* GetZoneGrid() const { return zoneGrid_.get(); }
    std::int32_t GetZoneCount(int zoneType) const { return zoneCount_[zoneType]; }
    std::int32_t GetUndevelopedTileCount() const { return undevelopedTotal_; }
    std::int32_t GetUndevelopedTileCount(int zoneType) const { return undevelopedByType_[zoneType]; }
    std::uint32_t GetDevelopmentFailureCount(int zoneType, int result) const;
    std::int32_t GetAbandonedTileCount() const { return abandonedTotal_; }
    int GetZoneType(int x, int z) const { return zoneGrid_->GetTractValue(x, z); }
    static constexpr std::uint32_t kStreamServiceIid[5] = {0x66ba4db5, 0xc6bb8334, 0xc70bd37e, 0x877181ec, 0x6772bd23};
    static bool IsRCIZone(int zoneType) { return static_cast<unsigned>(zoneType) < 0x11 && ((0x3feu >> zoneType) & 1u) != 0; }
    bool GetTextureForZone(int zoneType, cGZPersistResourceKey& key) const;
    std::uint32_t GetZoneDragColor(int zoneType) const { return dragColor_[zoneType]; }
    std::uint8_t GetZoneDisplayAlpha() const { return displayAlpha_; }
    std::uint8_t GetDefaultZoneDisplayAlpha() const { return defaultDisplayAlpha_; }
    void SetZoneDisplayAlpha(std::uint8_t alpha);
    std::int32_t GetMinZoneSize(int zoneType) const { return minSize_[zoneType]; }
    std::int32_t GetMaxZoneSize(int zoneType) const { return maxSize_[zoneType]; }

    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kZoneManagerClassId; }

    // Exposed for tests / callers that adjust statistics when zones change.
    void AdjustZoneCounts(int fromType, int toType, std::int32_t cells) {
        zoneCount_[fromType] -= cells;
        zoneCount_[toType] += cells;
    }
    void RecordDevelopmentFailure(int zoneType, int result) { ++failures_[{static_cast<std::int8_t>(zoneType), static_cast<std::int8_t>(result)}]; }

private:
    void ReadSimulatorTunableValues();

    IZoneManagerHost* host_;
    IZonePlacementHost* placement_ = nullptr;
    std::uint32_t refCount_ = 0;               // COM refcount (shared by the three sub-objects)
    bool initialized_ = false;                 // +0x18
    bool countsLoaded_ = false;                // +0x20 (set by Read: Init keeps the loaded counters)
    cRZRandom random_{0xffffffffu};            // +0x58
    cRZAutoRefCount<cSC4SimGrid<std::int8_t>> zoneGrid_;   // +0x70
    std::int32_t zoneCount_[kZoneTypeCount] = {};          // +0x78
    std::int32_t undevelopedByType_[kZoneTypeCount] = {};  // +0xb8
    std::int32_t secondCounts_[kZoneTypeCount] = {};       // +0xf8 (initialised like the undeveloped counts)
    std::int32_t undevelopedTotal_ = 0;                    // +0x138
    std::int32_t counter13c_ = 0, abandonedTotal_ = 0, counter144_ = 0;   // +0x13c, +0x140, +0x144
    std::int32_t minSize_[kZoneTypeCount] = {};            // +0x148
    std::int32_t maxSize_[kZoneTypeCount] = {};            // +0x188
    float developmentCost_[kZoneTypeCount] = {};           // +0x1c8
    std::int64_t zoneParameterA_[kZoneTypeCount] = {};     // +0x208
    std::int64_t zoneParameterB_[kZoneTypeCount] = {};     // +0x288
    std::uint32_t textureInstance_[kZoneTypeCount] = {};   // +0x308
    std::uint32_t dragColor_[kZoneTypeCount] = {};         // +0x348
    std::uint8_t displayAlpha_ = 0xff;                     // +0x388
    std::uint8_t defaultDisplayAlpha_ = 0xff;              // +0x389
    std::map<ZoneDevelopmentResult, std::uint32_t> failures_;      // +0x390
    std::map<ZoneDevelopmentResult, std::uint32_t> successes_;     // +0x3a8
};

}  // namespace sc4
