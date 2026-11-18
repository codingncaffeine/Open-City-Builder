// cSC4LandfillZone — one landfill zone: a connected set of landfill cells that accepts the city's garbage.
// Recovered from SimCity 4 Deluxe Edition 1.2.1: cGarbageHeap / cSC4LandfillZone / SC4CellRegion<int>::Inflate live in
// 0x1000ac9c0..0x1000b011a (ctor 0x1000acbac, region ctor 0x1000acc32, Init 0x1000acefe ... GetAvailableCapacity
// 0x1000b011a). Message target iids 1 / 0x452294aa; no class id (the zone is owned by the landfill developer).
//
// Layout: +8 refcount, +0xc initialised, +0x10 region (SC4CellRegion*), +0x18 heap count, +0x1c heap rows (z),
// +0x20 heap columns (x), +0x28 vector<cGarbageHeap> (0x28 bytes each), +0x40 "known dezoned" cache, +0x41 connected
// to a road/rail developer network, +0x44 cached cell count, +0x48 cell count dirty, +0x4c total capacity,
// +0x50 level height, +0x54 first road neighbour cell, +0x60 "zot" prop (marks an unconnected zone), +0x68 garbage truck
// generator, +0x70 attractor, +0x78 seagull flock effect, +0x80 effect manager, +0x88 ambient effects, +0x90 city,
// +0x98 terrain, +0xa0 pollution simulator, +0xa8 occupant manager, +0xb0 zone manager.
// A cGarbageHeap is one grid vertex of the zone (+8 x, +0xc z, +0x10 valid, +0x11 holds garbage, +0x18 capacity,
// +0x1c garbage, +0x20 id); the 16 landfill tile shapes around a vertex are chosen from the four heaps around a cell.
//
// Services (city, terrain, pollution/traffic/zone/occupant managers, prop and effect managers, rand) are injected
// through ILandfillZoneHost; the original reaches them through SL::sp* globals and holds AddRef'd pointers (+0x80..+0xb0),
// which the host owns here. Unrecovered callee bodies behind the seam: cSC4LotBaseTextureOccupant creation
// (SetEmptyCellTexture), cSC4LandfillOccupant creation/exemplar load (CreateLandfillOccupant), the effect objects.
//
// Quirks kept: the heap grid is (x1-x0) x (z1-z0) while the region is inclusive, so the last row/column of cells has
// no heap; Write/Read store the cell bits with exclusive upper bounds (the last row/column is reloaded as set);
// AddRegionToZone copies the old heaps back by their zone-relative x/z without shifting for a changed origin;
// CreateLandfillOccupant crashes in the original when the tile exemplar is missing (here it returns false);
// the tile orientation of the all-neighbours tile (id 0x2fa0000) is re-randomised on every use.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/SCProperty.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4LandfillZoneMessageTarget = 0x452294aa;
constexpr std::uint32_t kLandfillOccupantType = 0x6a11bd2b;
constexpr std::uint32_t kLandfillTileCostProperty = 0xea54d286;
constexpr std::uint32_t kLandfillZoneStreamVersion = 4;
constexpr std::uint32_t kLandfillTileEmptyId = 0x02f10000;                // table entry 0 (no neighbouring garbage)
constexpr std::uint32_t kLandfillTileRandomOrientationId = 0x02fa0000;    // table entry 15

struct LandfillRect { int x0 = 0, z0 = 0, x1 = 0, z1 = 0; };   // SC4Rect<int>

// SC4CellRegion<int>: bounds (+0..+0xc, inclusive cells) and a cRZCellMap<bool> (+0x10): rowCount x bitCount bits.
class LandfillCellRegion {
public:
    LandfillRect bounds;
    std::uint32_t rowCount = 0, bitCount = 0, wordCount = 0;      // cell map +0x18, +0x1c, +0x20
    std::vector<std::vector<std::uint32_t>> rows;                  // +0x28

    LandfillCellRegion() = default;
    // cRZCellMap<bool>(rows, bits, fill)
    void ResetMap(std::uint32_t rowsN, std::uint32_t bitsN, bool fill);
    static LandfillCellRegion Make(int x0, int z0, int x1, int z1, bool fill);   // map is (x1+1-x0) x (z1+1-z0)
    bool Test(int x, int z) const;                                 // the bounds-checked bit test used everywhere
    void SetBit(int x, int z, bool on);
    void Inflate(int neighborType);                                // @ 0x1000ae73c; 1 also grows diagonally
};

struct LandfillTunables {   // cSC4LandfillDeveloper statics (0x100afdff0..0x100afe00b)
    float decayRate = 0.0f;                          // 0x100afdff0 kLandfillDecayRate        property 0x4a280510
    float levelToleranceThreshold = 0.0f;            // 0x100afdff4 kLevelToleranceThreshold  property 0x4a280513
    std::uint32_t maxGarbageToTakePerMonth = 0;      // 0x100afdff8 kMaxGarbageToTakePerMonth property 0x4a677e5b
    std::uint32_t maxGarbageToDecayPerMonthPerZone = 0;   // 0x100afe000 (property 0x4a677e5e)
    std::int32_t maxGarbageToTakePerMonthPerTile = 0;     // 0x100afdffc (read as a uint32; property 0x4a677e5c)
    std::int64_t defaultLandfillTileCost = 0;        // 0x100b214f8 kDefaultLandfillTileCost   property 0x4a677e5f
    float costPerCellHeightChange = 0.0f;            // 0x100afe008 kCostPerCellHeightChange  property 0x2aa00fe7
    bool moveWithTerrain = false;                    // 0x100afe004 kbMoveWithTerrain        property 0x4a677e5d
    // Not developer statics: the zone's two automata controller templates (_kGarbageTruckGeneratorName / _kGarbageTruckAttractorName,
    // the strings next to "flock_of_seagulls" in the executable): the garbage trucks leave from and return to the zone's road cell.
    const char* garbageTruckGeneratorName = "garbage_out";
    const char* garbageTruckAttractorName = "garbage_attract";
};

class ILandfillOccupantRef {   // a cISC4Occupant the zone found; Release() drops the reference
public:
    virtual ~ILandfillOccupantRef() = default;
    virtual void Release() = 0;
    virtual void RemoveFromCity() = 0;       // occupant manager vslot 0xa8 (occupant, 1, 3)
    virtual int TextureKind() { return 0; }  // lot base texture occupant (iid 0x97f6d98) vslot 0x20
};
class ILandfillTile : public ILandfillOccupantRef {   // cSC4LandfillOccupant (iid 0x2a15f262)
public:
    virtual void GetPosition(float xyz[3]) = 0;         // occupant vslot 0x40
    virtual void SetPosition(const float xyz[3]) = 0;   // occupant vslot 0x48 (CreateLandfillOccupant: 0x80)
    virtual bool IsEmptyTile() = 0;                     // landfill occupant vslot 0x48
    virtual void SetLayer(int layer) = 0;               // occupant vslot 0x60
    virtual void SetEmpty(bool empty) = 0;              // +0x94
    virtual void Init() = 0;                            // vslot 0x18
    virtual cISCPropertyHolder* Properties() = 0;       // occupant vslot 0x30
};
class ILandfillProp {   // the "zot" prop (class 0x26bb767e)
public:
    virtual ~ILandfillProp() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void SetState(int state) = 0;            // vslot 0x60
};
class ILandfillEffect {
public:
    virtual ~ILandfillEffect() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void SetFlag(std::uint32_t flag, bool on) = 0;   // vslot 0x78
    virtual void SetEnabled(bool on) = 0;                    // vslot 0xa0
    virtual void SetTransform(const float matrix[13]) = 0;   // vslot 0x40: 3x3 rotation, position, scale
    virtual void Start(int mode) = 0;                        // vslot 0x18
    virtual void Stop(bool immediate) = 0;                   // vslot 0x20
    // vslot 0x28: the landfill developer polls it before starting / stopping the seagull flock.
    virtual bool IsRunning() { return false; }
};

class ILandfillZoneHost {
public:
    virtual ~ILandfillZoneHost() = default;
    virtual const LandfillTunables& Tunables() = 0;
    // city
    virtual int CityCellsX() = 0;                                           // city vslot 0x2e8
    virtual int CityCellsZ() = 0;                                           // city vslot 0x2f0
    virtual void CellToWorld(int x, int z, float& wx, float& wz) = 0;       // city vslot 0x308
    virtual bool HasNetworkAt(int x, int z) = 0;                            // city 0x140 -> vslot 0x50 (x, z, 0x1f4f, 1)
    // terrain
    virtual float AltitudeAt(float wx, float wz) = 0;                       // terrain vslot 0x60
    virtual float TerrainHeightAt(float x, float z) = 0;                    // terrain vslot 0x98
    virtual float AdjustedHeight(float terrainHeight, float y) = 0;         // terrain vslot 0xe8 (xmm0 = height, xmm1 = y)
    virtual void LevelTerrainRect(const LandfillRect& rect, float height) = 0;   // terrain vslot 0x198 (.., 1, 5)
    virtual void RefreshBuildingDevelopmentRect(const LandfillRect& rect) = 0;   // building dev sim 0x120 -> 0xa8
    // zone manager
    virtual int ZoneTypeAt(int x, int z) = 0;                               // vslot 0x60
    virtual void DezoneCell(int x, int z) = 0;                              // vslot 0x58 (1x1 full mask)
    // pollution simulator
    virtual int LandfillTileCapacity() = 0;                                 // vslot 0xa8
    virtual void AddToGarbageSupply(int amount) = 0;                        // vslot 0xe8
    virtual void GarbagePutInLandfill(int amount) = 0;                      // vslot 0x198
    virtual void SetActiveLandfill(int x, int z, bool on) = 0;              // vslot 0x190
    // traffic simulator
    virtual std::vector<std::uint32_t> NetworkIdsInRegion(const LandfillCellRegion& region) = 0;   // vslot 0x148
    virtual std::vector<int> NetworkFlagsForDeveloperType(std::uint32_t type) = 0;                 // vslot 0x150
    // occupant manager / resources
    virtual ILandfillTile* FindLandfillTile(int x, int z) = 0;              // vslot 0x100, type 0x6a11bd2b
    virtual bool HasBaseTexture(int x, int z) = 0;                          // vslot 0x100, type 0x497f6d9d
    virtual std::vector<ILandfillOccupantRef*> ListBaseTextures(int x, int z) = 0;          // 0x140, type 0x497f6d9d
    virtual std::vector<ILandfillOccupantRef*> ListIncompatibleOccupants(int x, int z) = 0; // 0x140, type 0x74758926
    virtual ILandfillTile* LoadLandfillTile(std::uint32_t tileId, int orientation) = 0;     // exemplar {0x6534284a, 0x07bddf1c, id}; null if missing
    virtual void AddOccupant(ILandfillTile* tile) = 0;                      // vslot 0xa0 (occupant, 0)
    virtual void RefreshOccupant(ILandfillTile* tile) = 0;                  // vslot 0xc0 (occupant, 1) + message 0x99ef1145
    virtual void PlaceEmptyCellTexture(int x, int z) = 0;                   // cSC4LotBaseTextureOccupant (unrecovered body)
    // props
    virtual ILandfillProp* CreateZotProp(const float position[3]) = 0;      // prop manager vslot 0xc8, id 0x107a0000
    virtual void RemoveProp(ILandfillProp* prop) = 0;                       // city 0x118 -> vslot 0xe8
    virtual bool WriteProp(cIGZOStream& stream, ILandfillProp* prop) = 0;   // SC4WriteGZSerializable
    virtual ILandfillProp* ReadProp(cIGZIStream& stream) = 0;               // SC4ReadGZSerializable(0x26bb767e)
    // effects
    virtual ILandfillEffect* CreateEffect(const char* name, const float position[3]) = 0;   // effect manager vslot 0x28
    virtual void RemoveEffect(ILandfillEffect* effect) = 0;                  // effect manager vslot 0x40
    virtual ILandfillEffect* CreateAmbientEffect(const char* name) = 0;     // ambient effects vslot 0x30; null if unknown
    virtual int Rand() = 0;                                                  // libc rand()
};

// cGarbageHeap (0x28 bytes)
struct cGarbageHeap {
    int x = 0, z = 0;                 // +8, +0xc
    bool valid = true;                // +0x10
    bool hasGarbage = false;          // +0x11
    std::uint32_t capacity = 0;       // +0x18
    std::uint32_t garbage = 0;        // +0x1c
    int id = 0;                       // +0x20
    std::uint32_t DecayGarbage(const LandfillTunables& t);   // @ 0x1000acb56
    bool Write(cIGZOStream& stream) const;                    // @ 0x1000aca32
    bool Read(cIGZIStream& stream);                           // @ 0x1000acaa4
};

int RoundFloatToEvenInt(float v);   // (int)(v + 12582912.0f) - 0x4b400000

class cSC4LandfillZone {
public:
    explicit cSC4LandfillZone(ILandfillZoneHost* host);                           // @ 0x1000acbac
    cSC4LandfillZone(ILandfillZoneHost* host, const LandfillCellRegion& region);  // @ 0x1000acc32
    ~cSC4LandfillZone();                                                          // @ 0x1000ace74

    std::uint32_t AddRef();                                  // @ 0x1000adaaa
    std::uint32_t Release();                                 // @ 0x1000adab8 (destroys at refcount < 2)
    bool QueryInterface(std::uint32_t iid, void** out);      // @ 0x1000adaea
    bool DoMessage(void* message);                           // @ 0x1000adb0c, always true
    bool Init();                                             // @ 0x1000acefe
    bool Shutdown();                                         // @ 0x1000ad2de
    bool Write(cIGZOStream& stream) const;                   // @ 0x1000ad56e
    bool Read(cIGZIStream& stream);                          // @ 0x1000ad66a

    int RecalculateCapacity();                               // @ 0x1000ad8e2
    int GetNumValidHeaps() const;                            // @ 0x1000ad986
    int CalculateGarbageHeapCapacity(int validHeaps);        // @ 0x1000ad9b6
    int GetCellCount();                                      // @ 0x1000ada0a
    std::int64_t GetMaintenanceCost();                       // @ 0x1000adfd4
    std::int64_t GetLandfillTileCost(int x, int z);          // @ 0x1000ae062
    void UpdateZone(int& garbageAvailable);                  // @ 0x1000ae124
    bool IsZoneConnected();                                  // @ 0x1000ae238
    void AddZot();                                           // @ 0x1000ae42a
    void RemoveZot();                                        // @ 0x1000ad510
    void RedistributeGarbage();                              // @ 0x1000ae682
    void GetCapacity(int& available, int& garbage, int& capacity) const;   // @ 0x1000ae6f8
    int GetAvailableCapacity() const;                        // @ 0x1000b011a
    void AddRegionToZone(const LandfillCellRegion& region);  // @ 0x1000ae9fe
    void InvalidateSurroundingHeaps(int x, int z);           // @ 0x1000aee92
    void DezoneCell(int x, int z, bool updateZoneManager, bool removeTile);   // @ 0x1000af886
    void HandleTerrainChange(const LandfillRect& rect);      // @ 0x1000afa7e
    void HandleCellTerrainChange(int x, int z);              // @ 0x1000afb1c
    bool IsCompletelyDezoned();                              // @ 0x1000afd60
    void LevelRect(const LandfillRect& rect, float height, bool useTerrain);   // @ 0x1000afdf8
    void PopulateZone();                                     // @ 0x1000aff38
    void UpdateSurroundingTiles(const cGarbageHeap& heap);   // @ 0x1000adb22
    bool GetFirstRoadTileNeighbor(int& x, int& z);           // @ 0x1000af650

    // accessors for tests / the developer
    const LandfillCellRegion* Region() const { return region_; }
    std::vector<cGarbageHeap>& Heaps() { return heaps_; }
    const std::vector<cGarbageHeap>& Heaps() const { return heaps_; }
    int HeapColumns() const { return columns_; }
    int HeapRows() const { return rows_; }
    bool Connected() const { return connected_; }
    float LevelHeight() const { return levelHeight_; }
    void SetLevelHeight(float h) { levelHeight_ = h; }
    int FirstRoadX() const { return roadX_; }
    int FirstRoadZ() const { return roadZ_; }
    std::int32_t TotalCapacity() const { return totalCapacity_; }
    ILandfillProp* Zot() const { return zot_; }
    ILandfillEffect* Seagulls() const { return seagulls_; }
    ILandfillEffect* TruckGenerator() const { return generator_; }
    ILandfillEffect* TruckAttractor() const { return attractor_; }
    // +0x40 "the zone is known to be completely dezoned" cache. cSC4LandfillDeveloper::DezoneArea raises it
    // directly (0x1000620f6) once the zone has no valid garbage heap left.
    bool KnownDezoned() const { return dezonedCache_; }
    void SetKnownDezoned(bool value) { dezonedCache_ = value; }
    // The landfill developer calls these two directly from DoZoneChange (0x1000617fa / 0x100061852).
    void InitZoneEffects();                                  // @ 0x1000ad096
    void ShutdownZoneEffects();                              // @ 0x1000ad442

private:
    void RemoveLandfillTile(int x, int z);                   // @ 0x1000aef82
    bool CreateLandfillOccupant(int x, int z, std::uint32_t tileId, int orientation);   // @ 0x1000af00c
    void SetEmptyCellTexture(int x, int z);                  // @ 0x1000af28e
    void RemoveIncompatibleOccupants(int x, int z);          // @ 0x1000af418
    void RemoveBaseTexture(int x, int z);                    // @ 0x1000af508
    void BuildHeaps();
    bool HeapHasGarbage(int x, int z) const;

    ILandfillZoneHost* host_;
    std::uint32_t refCount_ = 0;                             // +8
    bool initialised_ = false;                               // +0xc
    LandfillCellRegion* region_ = nullptr;                   // +0x10
    int heapCount_ = 0, rows_ = 0, columns_ = 0;             // +0x18, +0x1c, +0x20
    std::vector<cGarbageHeap> heaps_;                        // +0x28
    bool dezonedCache_ = false;                              // +0x40
    bool connected_ = false;                                 // +0x41
    int cellCount_ = 0;                                      // +0x44
    bool cellCountDirty_ = true;                             // +0x48
    std::int32_t totalCapacity_ = 0;                         // +0x4c
    float levelHeight_ = 0.0f;                               // +0x50
    int roadX_ = 0, roadZ_ = 0;                              // +0x54, +0x58
    ILandfillProp* zot_ = nullptr;                           // +0x60
    ILandfillEffect* generator_ = nullptr;                   // +0x68
    ILandfillEffect* attractor_ = nullptr;                   // +0x70
    ILandfillEffect* seagulls_ = nullptr;                    // +0x78
};

}  // namespace sc4
