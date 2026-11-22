// cSC4WaterPathFinder — see WaterPathFinder.h. Mac i386 decompile: scratch/s85/ghidra/ferry.c.
#include "sc4/WaterPathFinder.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sc4 {

namespace {
constexpr int kDX[4] = {-1, 0, 1, 0};              // _kDX 0x008419c0
constexpr int kDZ[4] = {0, -1, 0, 1};              // _kDZ 0x008419b0
constexpr float kMaxWaterDepth = 40.0f;            // _kfMaxWaterDepth 0x00a70a44
constexpr float kDepthStep = 0.2f;                 // 0x00a707b8: metres per depth-map unit
constexpr float kStepCost = 1.0f;                  // 0x00a706d8
constexpr float kCostPerDepthMeter = 0.1f;         // __kfCostPerDepthMeter (static initializer: 0x3dcccccd)

// The binary's float-to-int: x + 0.5 + 1.5 * 2^23 rounded to nearest, minus one when x was already whole = ceil.
std::uint8_t DepthUnits(float depth) {
    const float units = (depth <= kMaxWaterDepth ? depth : kMaxWaterDepth) / kDepthStep;
    return static_cast<std::uint8_t>(static_cast<int>(std::ceil(units)));
}
}  // namespace

// ---- WaterNodeHashMap (0x004d3886 Clear, 0x004d3f52 GetNode, 0x004d4046 constructor) ----------------------------------
cSC4WaterPathFinder::WaterNodeHashMap::WaterNodeHashMap() {
    blocks_.emplace_back(kBlockNodes);
}

void cSC4WaterPathFinder::WaterNodeHashMap::Clear() {
    blockUsed_ = 0;
    block_ = 0;
    for (auto& b : buckets_) b = nullptr;
}

cSC4WaterPathFinder::WaterNode* cSC4WaterPathFinder::WaterNodeHashMap::GetNode(std::uint16_t key) {
    const std::uint32_t bucket = (static_cast<std::uint32_t>(key) * 0x9e3779b1u) % kBuckets;
    for (WaterNode* n = buckets_[bucket]; n; n = n->nextInBucket)
        if (n->key == key) return n;
    if (blockUsed_ == kBlockNodes) {               // the block is full: the next one (made the first time it is needed)
        ++block_;
        if (block_ == blocks_.size()) blocks_.emplace_back(kBlockNodes);
        blockUsed_ = 0;
    }
    WaterNode* n = &blocks_[block_][blockUsed_++];
    n->nextInBucket = buckets_[bucket];
    buckets_[bucket] = n;
    n->key = key;
    n->open = false;
    n->closed = false;
    return n;
}

// ---- lifetime (0x004d40d6 constructor, 0x004d3eb6 destructor, 0x004d3f2c Release, 0x004d384c QueryInterface) ----------
cSC4WaterPathFinder::cSC4WaterPathFinder(IWaterPathFinderHost* host) : host_(host) {}

cSC4WaterPathFinder::~cSC4WaterPathFinder() {
    if (depthMap_) depthMap_->Release();
}

bool cSC4WaterPathFinder::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4WaterPathFinder::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

// ---- Init 0x004d4126 / Shutdown 0x004d38e2 ----------------------------------------------------------------------------
bool cSC4WaterPathFinder::Init() {
    if (initialized_) return false;
    auto* grid = new cSC4SimGrid<std::uint8_t>();
    grid->AddRef();
    if (depthMap_) depthMap_->Release();
    depthMap_ = grid;
    depthMap_->Init();
    depthMap_->SetTractSize(1);
    depthMap_->SetInstanceID(kWaterPathDepthMapId);
    for (std::uint32_t m : {kWaterPathMsgPostCityInit, kWaterPathMsgTerrainChanged, kWaterPathMsgTerrainChanged2,
                            kWaterPathMsgTerrainOperationEnd, kWaterPathMsgOccupantInserted, kWaterPathMsgOccupantRemoved})
        host_->AddNotification(this, m);
    initialized_ = true;
    return true;
}

bool cSC4WaterPathFinder::Shutdown() {
    if (!initialized_) return false;
    open_ = nullptr;
    nodes_.Clear();
    for (std::uint32_t m : {kWaterPathMsgPostCityInit, kWaterPathMsgTerrainChanged, kWaterPathMsgTerrainChanged2,
                            kWaterPathMsgTerrainOperationEnd, kWaterPathMsgOccupantInserted, kWaterPathMsgOccupantRemoved})
        host_->RemoveNotification(this, m);
    if (depthMap_) depthMap_->Release();
    depthMap_ = nullptr;
    initialized_ = false;
    return true;
}

// ---- UpdateDepthMap 0x004d39c0: every cell of the dirty rect gets its depth byte (0 = not navigable) --------------------
void cSC4WaterPathFinder::UpdateDepthMap() {
    if (depthMap_) {
        auto& data = depthMap_->GetGridData();
        for (int z = dirtyZ0_; z <= dirtyZ1_; ++z)          // ScanningIterator over the clip rect: x fastest
            for (int x = dirtyX0_; x <= dirtyX1_; ++x) {
                if (x < 0 || z < 0 || static_cast<std::size_t>(x) >= data.size() ||
                    static_cast<std::size_t>(z) >= data[static_cast<std::size_t>(x)].size()) continue;
                std::uint8_t value = 0;
                if (host_->IsNavigable(x, z)) {
                    const float depth = host_->WaterDepthForCell(x, z);
                    if (depth > 0.0f) value = DepthUnits(depth);
                }
                data[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = value;
            }
    }
    dirty_ = false;
}

void cSC4WaterPathFinder::GrowDirtyRect(int x0, int z0, int x1, int z1) {
    if (dirty_) {
        if (x0 > dirtyX0_) x0 = dirtyX0_;
        if (z0 > dirtyZ0_) z0 = dirtyZ0_;
        if (x1 < dirtyX1_) x1 = dirtyX1_;
        if (z1 < dirtyZ1_) z1 = dirtyZ1_;
    }
    dirtyX0_ = x0;
    dirtyZ0_ = z0;
    dirtyX1_ = x1;
    dirtyZ1_ = z1;
    dirty_ = true;
}

// ---- DoMessage 0x004d3bc6 ---------------------------------------------------------------------------------------------
bool cSC4WaterPathFinder::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (type) {
        case kWaterPathMsgPostCityInit: {
            int sx = 0, sz = 0;
            host_->CityCellCount(sx, sz);
            dirtyX0_ = 0;
            dirtyZ0_ = 0;
            dirtyX1_ = sx - 1;
            dirtyZ1_ = sz - 1;
            dirty_ = true;
            return true;
        }
        case kWaterPathMsgTerrainChanged:
        case kWaterPathMsgTerrainChanged2: {
            int sx = 0, sz = 0;
            host_->CityCellCount(sx, sz);
            const auto d1 = static_cast<std::uint32_t>(standard->GetData1());
            const auto d2 = static_cast<std::uint32_t>(standard->GetData2());
            int x0 = static_cast<int>(d1 >> 16) - 1;
            if (x0 < 0) x0 = 0;
            int x1 = static_cast<int>(d1 & 0xffff) < sx ? static_cast<int>(d1 & 0xffff) : sx - 1;
            int z0 = static_cast<int>(d2 >> 16) - 1;
            if (z0 < 0) z0 = 0;
            int z1 = static_cast<int>(d2 & 0xffff) < sz ? static_cast<int>(d2 & 0xffff) : sz - 1;
            GrowDirtyRect(x0, z0, x1, z1);
            return true;
        }
        case kWaterPathMsgTerrainOperationEnd:
            if (dirty_) UpdateDepthMap();
            return true;
        case kWaterPathMsgOccupantInserted:
        case kWaterPathMsgOccupantRemoved: {
            if (static_cast<std::uint32_t>(standard->GetData3()) != kWaterPathNetworkOccupantType) return true;
            int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
            if (!host_->OccupantCells(standard->GetVoid1(), x0, z0, x1, z1)) return true;
            if (!host_->CellHasWater(x0, z0)) return true;
            GrowDirtyRect(x0, z0, x1, z1);
            return true;
        }
        default:
            return true;
    }
}

// ---- the open list: sorted by estimate, a node goes behind the nodes whose estimate is not above its own ---------------
void cSC4WaterPathFinder::InsertOpen(WaterNode* node) {
    WaterNode* head = open_;
    if (!head) {
        open_ = node;
        node->nextOpen = nullptr;
        return;
    }
    if (node->estimate < head->estimate) {
        node->nextOpen = head;
        open_ = node;
        return;
    }
    WaterNode* at = head;
    while (WaterNode* next = at->nextOpen) {
        if (node->estimate < next->estimate) {
            node->nextOpen = next;
            at->nextOpen = node;
            return;
        }
        at = next;
    }
    at->nextOpen = node;
    node->nextOpen = nullptr;
}

// ---- FindPath 0x004d455c ----------------------------------------------------------------------------------------------
bool cSC4WaterPathFinder::FindPath(int x0, int z0, int x1, int z1, std::int32_t& cellCount,
                                   std::vector<std::uint8_t>* forward, std::vector<std::uint8_t>* backward) {
    if (dirty_) UpdateDepthMap();
    const std::uint16_t startBody = host_->BodyOfWater(x0, z0);
    const std::uint16_t endBody = host_->BodyOfWater(x1, z1);
    if (std::getenv("SC4_WATERPATHDEBUG"))
        std::fprintf(stderr, "water path: %d,%d (body %u depth %u) -> %d,%d (body %u depth %u)\n", x0, z0, unsigned(startBody),
                     depthMap_ && depthMap_->TractIsInBounds(x0, z0) ? unsigned(depthMap_->GetTractValue(x0, z0)) : 999u, x1, z1,
                     unsigned(endBody), depthMap_ && depthMap_->TractIsInBounds(x1, z1) ? unsigned(depthMap_->GetTractValue(x1, z1)) : 999u);
    if (startBody == 0 || startBody != endBody) return false;
    if (!depthMap_) return false;
    int sx = 0, sz = 0;
    host_->CityCellCount(sx, sz);
    auto& depth = depthMap_->GetGridData();

    open_ = nullptr;
    nodes_.Clear();
    WaterNode* start = nodes_.GetNode(static_cast<std::uint16_t>((x0 << 8) | (z0 & 0xff)));
    start->cost = 0.0f;
    start->estimate = std::sqrt(static_cast<float>((x0 - x1) * (x0 - x1) + (z0 - z1) * (z0 - z1)));
    start->parent = nullptr;
    InsertOpen(start);                     // the start is never flagged open

    for (;;) {
        WaterNode* node = open_;
        if (!node) return false;
        open_ = node->nextOpen;
        const int nx = node->key >> 8;
        const int nz = node->key & 0xff;
        if (x1 == nx && z1 == nz) {
            if (forward) MakeFinishedPath(node, *forward, cellCount, false);
            if (backward) MakeFinishedPath(node, *backward, cellCount, true);
            return true;
        }
        for (int i = 0; i < 4; ++i) {
            const int cx = nx + kDX[i];
            const int cz = nz + kDZ[i];
            if (cx < 0 || cx > sx - 1 || cz < 0 || cz > sz - 1) continue;
            if (static_cast<std::size_t>(cx) >= depth.size() || static_cast<std::size_t>(cz) >= depth[static_cast<std::size_t>(cx)].size()) continue;
            const std::uint8_t d = depth[static_cast<std::size_t>(cx)][static_cast<std::size_t>(cz)];
            if (d == 0 && !(x1 == cx && z1 == cz)) continue;
            const auto key = static_cast<std::uint16_t>((cx << 8) | (cz & 0xff));
            if (node->parent && key == node->parent->key) continue;
            const float cost = node->cost + kStepCost + (kMaxWaterDepth - static_cast<float>(d) * kDepthStep) * kCostPerDepthMeter;
            WaterNode* next = nodes_.GetNode(key);
            if (!((!next->open && !next->closed) || cost < next->cost)) continue;
            next->closed = false;
            next->cost = cost;
            next->estimate = std::sqrt(static_cast<float>((cx - x1) * (cx - x1) + (cz - z1) * (cz - z1))) + cost;
            next->parent = node;
            if (!next->open) {
                InsertOpen(next);
                next->open = true;
            } else {                       // already open: take it out and put it back by its new estimate
                if (next == open_) {
                    open_ = next->nextOpen;
                } else {
                    for (WaterNode* at = open_; at && at->nextOpen; at = at->nextOpen)
                        if (at->nextOpen == next) {
                            at->nextOpen = next->nextOpen;
                            break;
                        }
                }
                InsertOpen(next);
            }
        }
        node->open = false;
        node->closed = true;
    }
}

// ---- MakeFinishedPath 0x004d4242 --------------------------------------------------------------------------------------
void cSC4WaterPathFinder::MakeFinishedPath(WaterNode* end, std::vector<std::uint8_t>& out, std::int32_t& cellCount, bool fromEnd) {
    out.clear();
    cellCount = 0;
    if (!end) return;
    std::size_t n = 0;
    for (WaterNode* w = end; w; w = w->parent) ++n;
    cellCount = static_cast<std::int32_t>(n);
    std::vector<WaterNode*> cells(n, nullptr);
    if (fromEnd) {
        std::size_t i = 0;
        for (WaterNode* w = end; w; w = w->parent) cells[i++] = w;
    } else {
        std::size_t i = n;
        for (WaterNode* w = end; w; w = w->parent) cells[--i] = w;
    }
    const std::uint16_t first = cells[0]->key;
    out.push_back(0);
    out.push_back(static_cast<std::uint8_t>(first >> 8));
    out.push_back(static_cast<std::uint8_t>(first));
    std::uint8_t packed[256] = {};
    std::uint8_t count = 0;
    unsigned px = first >> 8, pz = first & 0xff;
    auto flush = [&] {
        out.push_back(0x40);
        out.push_back(count);
        const std::size_t bytes = (count >> 2) + ((count & 3) != 0 ? 1 : 0);
        out.insert(out.end(), packed, packed + bytes);
        count = 0;
    };
    for (std::size_t i = 1; i < n; ++i) {
        const unsigned cx = cells[i]->key >> 8, cz = cells[i]->key & 0xff;
        std::uint8_t dir;
        if (cx == px) {
            const auto dz = static_cast<std::uint16_t>(cz - pz);
            if (dz == 0xffff) dir = 1;
            else if (dz == 1) dir = 3;
            else return;                   // not a neighbour: the route ends here, unflushed
        } else {
            if (cz != pz) return;
            const auto dx = static_cast<std::uint16_t>(cx - px);
            if (dx == 0xffff) dir = 0;
            else if (dx == 1) dir = 2;
            else return;
        }
        if (count == 0xff) flush();
        if ((count & 3) == 0) packed[count >> 2] = dir;
        else packed[count >> 2] = static_cast<std::uint8_t>(packed[count >> 2] | (dir << ((count & 3) * 2)));
        ++count;
        px = cx;
        pz = cz;
    }
    if (count != 0) flush();
}

}  // namespace sc4
