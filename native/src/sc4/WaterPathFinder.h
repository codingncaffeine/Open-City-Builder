// cSC4WaterPathFinder — the boat route finder the traffic simulator owns (made in cSC4TrafficSimulator::Init, kept at
// i386 +0x118; Mac i386 0x004d384c..0x004d4b3a, decompile scratch/s85/ghidra/ferry.c). An A* search over the city's water
// cells: every cell carries a depth byte (the depth map, a cSC4SimGrid<uint8_t> with tract size 1 and instance id
// 0xcbdf6bf8), the cost of a step is 1 + (40 - depth) * 0.1, the estimate is the straight-line distance, and a route
// is found only between two cells of the same body of water. The finished route is the compressed form the boats read
// (cSC4CompressedRouteReader): 0x00, x, z, then runs of 0x40, count, the steps packed two bits each (0 = x-1, 1 = z-1,
// 2 = x+1, 3 = z+1; at most 255 steps a run).
//
// Layout kept from the binary (i386): +4 initialized, +8 refcount, +0xc the node hash map (3,079 buckets, hash
// key * 0x9e3779b1 % 3079, nodes from 4,096-node blocks), +0x3040 the open list head (sorted by estimate, a node goes
// behind every node of equal estimate), +0x3044 the depth map, +0x3048..+0x3054 the dirty cell rect (min x, min z,
// max x, max z), +0x3058 dirty.
//
// Messages (Init subscribes, Shutdown unsubscribes): 0x26d31ec1 post city init (the whole city dirty), 0x278e6003 and
// 0x678e6e3a terrain changed (data1 = x0 << 16 | x1, data2 = z0 << 16 | z1; the rect grows by one cell on the low side),
// 0x6a3b0afd the end of a long terrain operation (rebuild now when dirty), 0x99ef1142 / 0x99ef1143 occupant inserted /
// removed (a network piece, type 0xc772bf98, whose first cell is under water: a bridge changes what boats clear).
#pragma once
#include <cstdint>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {

constexpr std::uint32_t kWaterPathDepthMapId = 0xcbdf6bf8;
constexpr std::uint32_t kWaterPathMsgPostCityInit = 0x26d31ec1;
constexpr std::uint32_t kWaterPathMsgTerrainChanged = 0x278e6003;
constexpr std::uint32_t kWaterPathMsgTerrainChanged2 = 0x678e6e3a;
constexpr std::uint32_t kWaterPathMsgTerrainOperationEnd = 0x6a3b0afd;
constexpr std::uint32_t kWaterPathMsgOccupantInserted = 0x99ef1142;
constexpr std::uint32_t kWaterPathMsgOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kWaterPathNetworkOccupantType = 0xc772bf98;

// What the path finder reads from the city.
class IWaterPathFinderHost {
public:
    virtual ~IWaterPathFinderHost() = default;
    virtual void CityCellCount(int& x, int& z) = 0;                   // SL::spCity i386 +0x174 / +0x178
    virtual bool IsNavigable(int x, int z) = 0;                       // SL::spSurfaceWater i386 +0x1c (x, z, 0, null)
    virtual float WaterDepthForCell(int x, int z) = 0;                // SL::spSurfaceWater i386 +0x18
    virtual std::uint16_t BodyOfWater(int x, int z) = 0;              // spSurfaceWater i386 +0x24 GetBodyOfWaterMap -> +0x1c
    virtual bool CellHasWater(int x, int z) = 0;                      // SL::spTerrain i386 +0x88
    virtual bool OccupantCells(void* occupant, int& x0, int& z0, int& x1, int& z1) = 0;   // occupant i386 +0x2c GetBoundingCityCells
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;      // SL::spMS2 i386 +0x14
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) = 0;   // SL::spMS2 i386 +0x18
};

class cSC4WaterPathFinder : public cIGZMessageTarget2 {
public:
    explicit cSC4WaterPathFinder(IWaterPathFinderHost* host);
    ~cSC4WaterPathFinder();

    bool QueryInterface(std::uint32_t iid, void** out) override;     // only cIGZUnknown (1)
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;

    bool Init();
    bool Shutdown();

    // From (x0, z0) to (x1, z1). False when the start is on no body of water, the two cells are on different bodies, or
    // no route exists. `forward` gets the route from the start, `backward` from the end; `cellCount` = cells on it.
    bool FindPath(int x0, int z0, int x1, int z1, std::int32_t& cellCount, std::vector<std::uint8_t>* forward,
                  std::vector<std::uint8_t>* backward);

    void UpdateDepthMap();
    cSC4SimGrid<std::uint8_t>* DepthMap() const { return depthMap_; }
    bool Dirty() const { return dirty_; }

private:
    struct WaterNode {                     // 0x1c bytes
        std::uint16_t key = 0;             // +0x00 x << 8 | z
        WaterNode* parent = nullptr;       // +0x04
        float cost = 0.0f;                 // +0x08 from the start
        float estimate = 0.0f;             // +0x0c cost + distance to the end
        bool open = false;                 // +0x10
        bool closed = false;               // +0x11
        WaterNode* nextOpen = nullptr;     // +0x14
        WaterNode* nextInBucket = nullptr; // +0x18
    };
    class WaterNodeHashMap {
    public:
        static constexpr std::uint32_t kBuckets = 0xc07;
        static constexpr std::size_t kBlockNodes = 0x1c000 / 0x1c;
        WaterNodeHashMap();
        void Clear();
        WaterNode* GetNode(std::uint16_t key);
    private:
        WaterNode* buckets_[kBuckets] = {};
        std::vector<std::vector<WaterNode>> blocks_;   // +0x301c
        std::size_t blockUsed_ = 0;                    // +0x3028 / +0x302c as an index into the current block
        std::size_t block_ = 0;                        // +0x3030
    };

    void InsertOpen(WaterNode* node);      // behind every node whose estimate is not above its own
    void MakeFinishedPath(WaterNode* end, std::vector<std::uint8_t>& out, std::int32_t& cellCount, bool fromEnd);
    void GrowDirtyRect(int x0, int z0, int x1, int z1);

    IWaterPathFinderHost* host_;
    bool initialized_ = false;             // +4
    std::uint32_t refCount_ = 0;           // +8
    WaterNodeHashMap nodes_;               // +0xc
    WaterNode* open_ = nullptr;            // +0x3040
    cSC4SimGrid<std::uint8_t>* depthMap_ = nullptr;   // +0x3044
    int dirtyX0_ = 0, dirtyZ0_ = 0, dirtyX1_ = 0, dirtyZ1_ = 0;   // +0x3048..+0x3054
    bool dirty_ = false;                   // +0x3058
};

}  // namespace sc4
