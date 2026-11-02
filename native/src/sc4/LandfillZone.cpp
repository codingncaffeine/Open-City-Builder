#include "sc4/LandfillZone.h"
#include "sc4/DBSegment.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "gz/StreamOps.h"
#include "sc4/DisasterManagers.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SimStreamOps.h"

namespace sc4 {

namespace {

// DAT_100afef74: tile id and stored orientation for each 4-bit neighbour mask (stride 12; the third column is unused).
struct TileShape { std::uint32_t id; int orientation; };
TileShape g_tileTable[16] = {
    {0x02f10000, 0}, {0x034e0000, 3}, {0x034e0000, 2}, {0x02f90000, 3}, {0x034e0000, 1}, {0x034f0000, 0},
    {0x02f90000, 2}, {0x02fb0000, 3}, {0x034e0000, 0}, {0x02f90000, 0}, {0x034f0000, 2}, {0x02fb0000, 0},
    {0x02f90000, 1}, {0x02fb0000, 1}, {0x02fb0000, 2}, {0x02fa0000, 0},
};

// DAT_100a169c0: developer types whose networks make the zone "connected".
constexpr std::uint32_t kDeveloperTypes[12] = {0x1010, 0x1020, 0x1030, 0x3110, 0x3120, 0x3130,
                                               0x3320, 0x3330, 0x4200, 0x4300, 0x4100, 0x4400};

constexpr const char* kSeagullEffectName = "flock_of_seagulls";

int RandomOrientation(int r) {
    // (r - ((((uint)(r >> 31) >> 30) + r) & 0x7ffffffc)) * 2
    const std::uint32_t bias = static_cast<std::uint32_t>(r >> 31) >> 30;
    const std::uint32_t masked = (bias + static_cast<std::uint32_t>(r)) & 0x7ffffffcu;
    return static_cast<int>(static_cast<std::uint32_t>(r) - masked) * 2;
}

}  // namespace

int RoundFloatToEvenInt(float v) {
    const float biased = v + 12582912.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &biased, sizeof(bits));
    return static_cast<int>(bits - 0x4b400000u);
}

// ---- SC4CellRegion<int> ----------------------------------------------------------------------------------------

void LandfillCellRegion::ResetMap(std::uint32_t rowsN, std::uint32_t bitsN, bool fill) {
    rowCount = rowsN;
    bitCount = bitsN;
    wordCount = (bitsN >> 5) + 1 - ((bitsN & 0x1f) == 0 ? 1u : 0u);
    rows.assign(rowsN, std::vector<std::uint32_t>(wordCount, fill ? 0xffffffffu : 0u));
}

LandfillCellRegion LandfillCellRegion::Make(int x0, int z0, int x1, int z1, bool fill) {
    LandfillCellRegion r;
    r.bounds = {x0, z0, x1, z1};
    r.ResetMap(static_cast<std::uint32_t>(x1 + 1 - x0), static_cast<std::uint32_t>(z1 + 1 - z0), fill);
    return r;
}

bool LandfillCellRegion::Test(int x, int z) const {
    const std::uint32_t dx = static_cast<std::uint32_t>(x - bounds.x0);
    const std::uint32_t dz = static_cast<std::uint32_t>(z - bounds.z0);
    if (dx >= rowCount || dz >= bitCount || dx >= rows.size()) return false;
    return ((rows[dx][dz >> 5] >> (dz & 0x1f)) & 1u) != 0;
}

void LandfillCellRegion::SetBit(int x, int z, bool on) {
    const std::uint32_t dx = static_cast<std::uint32_t>(x - bounds.x0);
    const std::uint32_t dz = static_cast<std::uint32_t>(z - bounds.z0);
    if (dx >= rows.size() || (dz >> 5) >= rows[dx].size()) return;   // the original does not check
    if (on) rows[dx][dz >> 5] |= 1u << (dz & 0x1f);
    else rows[dx][dz >> 5] &= ~(1u << (dz & 0x1f));
}

void LandfillCellRegion::Inflate(int neighborType) {
    const int nx0 = bounds.x0 - 1, nz0 = bounds.z0 - 1, nx1 = bounds.x1 + 1, nz1 = bounds.z1 + 1;
    const std::uint32_t newRows = static_cast<std::uint32_t>(nx1 - nx0 + 1);
    const std::uint32_t newBits = static_cast<std::uint32_t>((nz1 - nz0) + 1);
    LandfillCellRegion grown;
    grown.ResetMap(newRows, newBits, false);
    if (bounds.x0 <= bounds.x1 && bounds.z0 <= bounds.z1) {
        auto set = [&](std::uint32_t row, std::uint32_t bit) {
            if (row < grown.rows.size() && (bit >> 5) < grown.wordCount) grown.rows[row][bit >> 5] |= 1u << (bit & 0x1f);
        };
        for (std::uint32_t i = 0; i <= static_cast<std::uint32_t>(bounds.x1 - bounds.x0); ++i) {
            for (std::uint32_t j = 0; j <= static_cast<std::uint32_t>(bounds.z1 - bounds.z0); ++j) {
                if (i >= rows.size() || (j >> 5) >= rows[i].size()) continue;
                if (((rows[i][j >> 5] >> (j & 0x1f)) & 1u) == 0) continue;
                // the source cell lands at (i + 1, j + 1) of the grown map
                set(i + 1, j + 1);
                set(i, j + 1);
                set(i + 2, j + 1);
                set(i + 1, j);
                set(i + 1, j + 2);
                if (neighborType == 1) {
                    set(i, j);
                    set(i, j + 2);
                    set(i + 2, j);
                    set(i + 2, j + 2);
                }
            }
        }
    }
    bounds = {nx0, nz0, nx1, nz1};
    rowCount = grown.rowCount;
    bitCount = grown.bitCount;
    wordCount = grown.wordCount;
    rows = std::move(grown.rows);
}

// ---- cGarbageHeap ----------------------------------------------------------------------------------------------

std::uint32_t cGarbageHeap::DecayGarbage(const LandfillTunables& t) {
    const std::uint32_t g = garbage;
    if (g == 0) return 0;
    const float scaled = static_cast<float>(g) * t.decayRate;
    std::uint32_t decay = static_cast<std::uint32_t>(RoundFloatToEvenInt(scaled));
    if (decay == 0) decay = 1;
    else if (t.maxGarbageToDecayPerMonthPerZone < decay) decay = t.maxGarbageToDecayPerMonthPerZone;
    if (g < decay) decay = g;
    garbage = g - decay;
    return decay;
}

bool cGarbageHeap::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    stream.SetUint32(static_cast<std::uint32_t>(x));
    stream.SetUint32(static_cast<std::uint32_t>(z));
    stream.SetUint16(valid ? 1 : 0);
    stream.SetUint16(hasGarbage ? 1 : 0);
    stream.SetUint32(garbage);
    stream.SetUint32(capacity);
    return !stream.GetError();
}

bool cGarbageHeap::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version == 2) {
        std::uint32_t v = 0;
        stream.GetUint32(v);
        x = static_cast<int>(v);
        stream.GetUint32(v);
        z = static_cast<int>(v);
        std::uint16_t flag = 0;
        stream.GetUint16(flag);
        valid = flag == 1;
        stream.GetUint16(flag);
        hasGarbage = flag == 1;
        stream.GetUint32(garbage);
        stream.GetUint32(capacity);
    }
    return version == 2;
}

// ---- construction ----------------------------------------------------------------------------------------------

cSC4LandfillZone::cSC4LandfillZone(ILandfillZoneHost* host) : host_(host) {}

cSC4LandfillZone::cSC4LandfillZone(ILandfillZoneHost* host, const LandfillCellRegion& region) : host_(host) {
    region_ = new LandfillCellRegion(region);
    columns_ = region_->bounds.x1 - region_->bounds.x0;
    rows_ = region_->bounds.z1 - region_->bounds.z0;
    heapCount_ = rows_ * columns_;
    heaps_.clear();
    if (heapCount_ > 0) BuildHeaps();
}

void cSC4LandfillZone::BuildHeaps() {
    for (int z = 0; z < rows_; ++z) {
        for (int x = 0; x < columns_; ++x) {
            cGarbageHeap heap;
            heap.x = x;
            heap.z = z;
            heap.id = z;
            heaps_.push_back(heap);
        }
    }
}

cSC4LandfillZone::~cSC4LandfillZone() {
    if (zot_) zot_->Release();
}

std::uint32_t cSC4LandfillZone::AddRef() { return ++refCount_; }

std::uint32_t cSC4LandfillZone::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4LandfillZone::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 1 || iid == kGZIID_cISC4LandfillZoneMessageTarget) {
        *out = this;
        AddRef();
        return true;
    }
    return false;
}

bool cSC4LandfillZone::DoMessage(void*) { return true; }

bool cSC4LandfillZone::Init() {
    if (!initialised_) {
        initialised_ = true;
        // the original now takes AddRef'd pointers to the city, occupant manager, terrain, pollution simulator,
        // effects managers and zone manager; the host owns them here
        InitZoneEffects();
        if (levelHeight_ == 0.0f && region_) {
            float wx, wz;
            host_->CellToWorld((region_->bounds.x1 + region_->bounds.x0) / 2, (region_->bounds.z1 + region_->bounds.z0) / 2, wx, wz);
            levelHeight_ = host_->AltitudeAt(wx, wz);
        }
    }
    return true;
}

void cSC4LandfillZone::InitZoneEffects() {
    GetFirstRoadTileNeighbor(roadX_, roadZ_);
    float wx, wz;
    host_->CellToWorld(roadX_, roadZ_, wx, wz);
    const float alt = host_->AltitudeAt(wx, wz);
    const float pos[3] = {wx, alt, wz};
    const LandfillTunables& t = host_->Tunables();
    if (!generator_) {
        generator_ = host_->CreateEffect(t.garbageTruckGeneratorName, pos);
        if (generator_) {
            generator_->AddRef();
            generator_->SetFlag(8, true);
            generator_->SetFlag(0x100, true);
        }
    }
    if (!attractor_) {
        attractor_ = host_->CreateEffect(t.garbageTruckAttractorName, pos);
        if (attractor_) {
            attractor_->AddRef();
            attractor_->SetFlag(8, true);
        }
    }
    if (!seagulls_) {
        ILandfillEffect* flock = host_->CreateAmbientEffect(kSeagullEffectName);
        if (flock) {
            seagulls_ = flock;
            float cx, cz;
            host_->CellToWorld((region_->bounds.x1 + region_->bounds.x0) / 2, (region_->bounds.z1 + region_->bounds.z0) / 2, cx, cz);
            const float height = host_->AltitudeAt(cx, cz) + 64.0f;
            const float matrix[13] = {1, 0, 0, 0, 1, 0, 0, 0, 1, cx, height, cz, 1.0f};
            seagulls_->SetTransform(matrix);
            seagulls_->Start(0);
        }
    }
}

bool cSC4LandfillZone::Shutdown() {
    ShutdownZoneEffects();
    if (!initialised_) return true;
    initialised_ = false;
    RemoveZot();
    delete region_;
    region_ = nullptr;
    return true;
}

void cSC4LandfillZone::ShutdownZoneEffects() {
    if (seagulls_) {
        seagulls_->Stop(true);
        seagulls_->Release();
        seagulls_ = nullptr;
    }
    if (generator_) {
        generator_->SetEnabled(false);
        host_->RemoveEffect(generator_);
        generator_->Release();
        generator_ = nullptr;
    }
    if (attractor_) {
        attractor_->SetEnabled(false);
        host_->RemoveEffect(attractor_);
        attractor_->Release();
        attractor_ = nullptr;
    }
}

void cSC4LandfillZone::RemoveZot() {
    if (!zot_) return;
    host_->RemoveProp(zot_);
    ILandfillProp* prop = zot_;
    zot_ = nullptr;
    prop->Release();
}

void cSC4LandfillZone::AddZot() {
    if (zot_ || !region_) return;
    const LandfillRect& b = region_->bounds;
    const int centerX = (b.x1 + b.x0) / 2, centerZ = (b.z1 + b.z0) / 2;
    SpiralIterator spiral;
    spiral.x = centerX;
    spiral.z = centerZ;
    spiral.targetX = centerX + 1;
    spiral.targetZ = centerZ;
    spiral.minX = b.x0;
    spiral.minZ = b.z0;
    spiral.maxX = std::max(b.x1, b.x0);
    spiral.maxZ = std::max(b.z1, b.z0);
    spiral.mode = 1;
    spiral.direction = 1;
    spiral.state = 0;
    spiral.onZ = 1;
    spiral.edgesHit = 0;
    spiral.turns = 0;
    if (centerX < b.x0 || centerZ < b.z0 || spiral.maxX < centerX || spiral.maxZ < centerZ) {
        spiral.state = 2;
        spiral.edgesHit = 0xf;
    }
    int cellX = centerX, cellZ = centerZ;
    while (!region_->Test(cellX, cellZ)) {
        if (!spiral.GetNext(cellX, cellZ)) {
            cellX = centerX;
            cellZ = centerZ;
            break;
        }
    }
    float wx, wz;
    host_->CellToWorld(cellX, cellZ, wx, wz);
    const float height = host_->TerrainHeightAt(wx, wz);
    const float pos[3] = {wx, height + 3.0f, wz};
    zot_ = host_->CreateZotProp(pos);
    if (zot_) zot_->SetState(3);   // the original calls through a null prop when creation fails
}

// ---- streams ---------------------------------------------------------------------------------------------------

bool cSC4LandfillZone::Write(cIGZOStream& stream) const {
    stream.SetUint16(static_cast<std::uint16_t>(kLandfillZoneStreamVersion));
    const LandfillRect& b = region_->bounds;
    stream.SetSint32(b.x0);
    stream.SetSint32(b.z0);
    stream.SetSint32(b.x1);
    stream.SetSint32(b.z1);
    for (int x = b.x0; x < b.x1; ++x) {          // exclusive upper bounds (quirk)
        for (int z = b.z0; z < b.z1; ++z) stream.SetUint8(region_->Test(x, z) ? 1 : 0);
    }
    stream.SetSint32(columns_);
    stream.SetSint32(rows_);
    for (const cGarbageHeap& heap : heaps_) heap.Write(stream);
    stream << connected_;
    // SC4WriteGZSerializable(zot): without the zone host (a zone loaded before one is installed) the record still holds the
    // reference, written as null
    if (host_) host_->WriteProp(stream, zot_);
    else SC4WriteGZSerializable(stream, nullptr);
    return !stream.GetError();
}

bool cSC4LandfillZone::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    std::int32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    stream.GetSint32(x0);
    stream.GetSint32(z0);
    stream.GetSint32(x1);
    stream.GetSint32(z1);
    delete region_;   // the original overwrites the pointer
    region_ = new LandfillCellRegion(LandfillCellRegion::Make(x0, z0, x1, z1, true));
    for (int x = x0; x < x1; ++x) {
        for (int z = z0; z < z1; ++z) {
            std::uint8_t bit = 0;
            stream.GetUint8(bit);
            region_->SetBit(x, z, bit != 0);
        }
    }
    stream.GetSint32(columns_);
    stream.GetSint32(rows_);
    heapCount_ = rows_ * columns_;
    for (int i = 0; i < heapCount_; ++i) {
        cGarbageHeap heap;
        heap.Read(stream);
        heap.id = i;
        heaps_.push_back(heap);
    }
    if (version == 3) {
        std::uint8_t flag = 0;
        stream.GetUint8(flag);
        connected_ = flag != 0;
    }
    if (version > 3) {
        stream >> connected_;
        if (zot_) {
            ILandfillProp* old = zot_;
            zot_ = nullptr;
            old->Release();
        }
        if (host_) {
            zot_ = host_->ReadProp(stream);
        } else {   // no zone host: the reference is consumed
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, 0x26bb767e, &raw, false);
            if (raw) static_cast<cIGZUnknown*>(raw)->Release();
        }
    }
    return !stream.GetError();
}

// ---- capacity --------------------------------------------------------------------------------------------------

int cSC4LandfillZone::GetNumValidHeaps() const {
    int n = 0;
    for (int i = 0; i < heapCount_ && i < static_cast<int>(heaps_.size()); ++i) n += heaps_[i].valid ? 1 : 0;
    return n;
}

int cSC4LandfillZone::GetCellCount() {
    if (!cellCountDirty_) return cellCount_;
    cellCount_ = 0;
    int count = 0;
    const LandfillRect& b = region_->bounds;
    for (int x = b.x0; x <= b.x1; ++x) {
        for (int z = b.z0; z <= b.z1; ++z) {
            if (region_->Test(x, z)) {
                ++count;
                cellCount_ = count;
            }
        }
    }
    cellCountDirty_ = false;
    return count;
}

int cSC4LandfillZone::CalculateGarbageHeapCapacity(int validHeaps) {
    if (validHeaps < 1) return 0;
    const int cells = GetCellCount();
    const int perTile = host_->LandfillTileCapacity();
    const float total = static_cast<float>(static_cast<std::uint32_t>(perTile * cells));
    const float share = total / static_cast<float>(validHeaps);
    return RoundFloatToEvenInt(share);
}

int cSC4LandfillZone::RecalculateCapacity() {
    int valid = 0;
    for (int i = 0; i < heapCount_ && i < static_cast<int>(heaps_.size()); ++i) valid += heaps_[i].valid ? 1 : 0;
    const std::uint32_t perHeap = static_cast<std::uint32_t>(CalculateGarbageHeapCapacity(valid));
    for (cGarbageHeap& heap : heaps_) {
        heap.capacity = heap.valid ? perHeap : 0;
        if (heap.capacity < heap.garbage) {
            host_->AddToGarbageSupply(static_cast<int>(heap.garbage - heap.capacity));
            heap.garbage = heap.capacity;
        }
    }
    totalCapacity_ = static_cast<std::int32_t>(perHeap * static_cast<std::uint32_t>(valid));
    return totalCapacity_;
}

void cSC4LandfillZone::RedistributeGarbage() {
    if (heaps_.empty()) return;
    int totalCapacity = 0, totalGarbage = 0;
    for (const cGarbageHeap& heap : heaps_) {
        totalCapacity += static_cast<int>(heap.capacity);
        totalGarbage += static_cast<int>(heap.garbage);
    }
    if (totalGarbage <= 0 || heapCount_ <= 0) return;
    int valid = 0;
    for (int i = 0; i < heapCount_ && i < static_cast<int>(heaps_.size()); ++i) valid += heaps_[i].valid ? 1 : 0;
    if (valid <= 0) return;
    for (cGarbageHeap& heap : heaps_) {
        if (!heap.valid) continue;
        int share = totalCapacity / valid;
        if (totalGarbage <= share) share = totalGarbage;
        totalGarbage -= share;
        heap.garbage = static_cast<std::uint32_t>(share);
    }
}

void cSC4LandfillZone::GetCapacity(int& available, int& garbage, int& capacity) const {
    garbage = 0;
    capacity = 0;
    available = 0;
    for (const cGarbageHeap& heap : heaps_) {
        capacity += static_cast<int>(heap.capacity);
        garbage += static_cast<int>(heap.garbage);
        available += static_cast<int>(heap.capacity) - static_cast<int>(heap.garbage);
    }
}

int cSC4LandfillZone::GetAvailableCapacity() const {
    int available = 0;
    for (const cGarbageHeap& heap : heaps_) available = (available + static_cast<int>(heap.capacity)) - static_cast<int>(heap.garbage);
    return available;
}

// ---- costs -----------------------------------------------------------------------------------------------------

std::int64_t cSC4LandfillZone::GetLandfillTileCost(int x, int z) {
    std::int64_t cost = host_->Tunables().defaultLandfillTileCost;
    ILandfillTile* tile = host_->FindLandfillTile(x, z);
    if (tile) {
        std::int64_t value = cost;
        if (cSCPropertyHelp::GetPropertyValue(tile->Properties(), kLandfillTileCostProperty, value)) cost = value;
        tile->Release();
    }
    return cost;
}

std::int64_t cSC4LandfillZone::GetMaintenanceCost() {
    std::int64_t total = 0;
    const LandfillRect& b = region_->bounds;
    if (b.x1 < b.x0) return 0;
    for (int x = b.x0; x <= b.x1; ++x)
        for (int z = b.z0; z <= b.z1; ++z)
            if (region_->Test(x, z)) total += GetLandfillTileCost(x, z);
    return total;
}

// ---- monthly update --------------------------------------------------------------------------------------------

void cSC4LandfillZone::UpdateZone(int& garbageAvailable) {
    connected_ = IsZoneConnected();
    if (!connected_) AddZot();
    else RemoveZot();
    const int cells = GetCellCount();
    const LandfillTunables& t = host_->Tunables();
    if (!heaps_.empty()) {
        std::int32_t budget = cells * t.maxGarbageToTakePerMonthPerTile;
        if (garbageAvailable <= budget) budget = garbageAvailable;
        for (std::size_t i = 0; i < heaps_.size(); ++i) {
            cGarbageHeap& heap = heaps_[i];
            if (!heap.valid) continue;
            heap.DecayGarbage(t);
            std::uint32_t taken = 0;
            const std::uint32_t room = heap.capacity - heap.garbage;
            if (room != 0 && static_cast<std::int32_t>(budget) > 0 && connected_) {
                taken = static_cast<std::uint32_t>(budget) <= room ? static_cast<std::uint32_t>(budget) : room;
                heap.garbage += taken;
                host_->AddToGarbageSupply(-static_cast<int>(taken));
                host_->GarbagePutInLandfill(static_cast<int>(taken));
            }
            garbageAvailable -= static_cast<int>(taken);
            budget -= static_cast<std::int32_t>(taken);
            if (heap.garbage == 0) {
                if (heap.hasGarbage) {
                    heap.hasGarbage = false;
                    UpdateSurroundingTiles(heap);
                }
            } else if (!heap.hasGarbage) {
                heap.hasGarbage = true;
                UpdateSurroundingTiles(heap);
            }
        }
    }
    RedistributeGarbage();
}

bool cSC4LandfillZone::IsZoneConnected() {
    LandfillCellRegion inflated = *region_;
    inflated.Inflate(0);
    const std::vector<std::uint32_t> networks = host_->NetworkIdsInRegion(inflated);
    for (std::uint32_t type : kDeveloperTypes) {
        const std::vector<int> flags = host_->NetworkFlagsForDeveloperType(type);
        for (std::uint32_t id : networks) {
            if (id < flags.size() && flags[id] != 0) return true;
        }
    }
    return false;
}

// ---- tiles -----------------------------------------------------------------------------------------------------

bool cSC4LandfillZone::HeapHasGarbage(int x, int z) const {
    if (x < 0 || z < 0 || x >= columns_ || z >= rows_) return false;
    const std::size_t index = static_cast<std::size_t>(columns_) * static_cast<std::size_t>(z) + static_cast<std::size_t>(x);
    return index < heaps_.size() && heaps_[index].hasGarbage;
}

void cSC4LandfillZone::UpdateSurroundingTiles(const cGarbageHeap& heap) {
    // The heap at (hx, hz) is a vertex; the four cells around it each pick a tile from the garbage state of the four
    // heaps that touch the cell. Mask bits run around the cell: A (hx,hz) = (hx-1,hz-1),(hx-1,hz),(hx,hz),(hx,hz-1);
    // B (hx+1,hz) = (hx,hz-1),(hx,hz),(hx+1,hz),(hx+1,hz-1); C (hx,hz+1) = (hx-1,hz),(hx-1,hz+1),(hx,hz+1),(hx,hz);
    // D (hx+1,hz+1) = (hx,hz),(hx,hz+1),(hx+1,hz+1),(hx+1,hz).
    const int hx = heap.x, hz = heap.z;
    const unsigned self = heap.hasGarbage ? 1u : 0u;
    auto g = [&](int x, int z) -> unsigned { return HeapHasGarbage(x, z) ? 1u : 0u; };
    const unsigned maskA = g(hx - 1, hz - 1) | (g(hx - 1, hz) << 1) | (self << 2) | (g(hx, hz - 1) << 3);
    const unsigned maskB = g(hx, hz - 1) | (self << 1) | (g(hx + 1, hz) << 2) | (g(hx + 1, hz - 1) << 3);
    const unsigned maskC = g(hx - 1, hz) | (g(hx - 1, hz + 1) << 1) | (g(hx, hz + 1) << 2) | (self << 3);
    const unsigned maskD = self | (g(hx, hz + 1) << 1) | (g(hx + 1, hz + 1) << 2) | (g(hx + 1, hz) << 3);
    const unsigned masks[4] = {maskA, maskB, maskC, maskD};
    const int dx[4] = {0, 1, 0, 1}, dz[4] = {0, 0, 1, 1};
    // (the original also tests `table[0].id == 0x2fa0000` first; entry 0 is 0x2f10000 so that never fires)
    for (int i = 0; i < 4; ++i) {
        const int cellX = hx + region_->bounds.x0 + dx[i];
        const int cellZ = hz + region_->bounds.z0 + dz[i];
        TileShape& shape = g_tileTable[masks[i]];
        const std::uint32_t id = shape.id;
        int orientation;
        if (id == kLandfillTileRandomOrientationId) {
            orientation = RandomOrientation(host_->Rand());
            shape.orientation = orientation;
        } else {
            orientation = shape.orientation;
        }
        RemoveLandfillTile(cellX, cellZ);
        CreateLandfillOccupant(cellX, cellZ, id, orientation);
    }
}

void cSC4LandfillZone::InvalidateSurroundingHeaps(int x, int z) {
    const int hx = x - region_->bounds.x0;
    if (hx < 0) return;
    const int hz = z - region_->bounds.z0;
    auto invalidate = [&](int ix, int iz) {
        const std::size_t index = static_cast<std::size_t>(columns_) * static_cast<std::size_t>(iz) + static_cast<std::size_t>(ix);
        if (index < heaps_.size() && heaps_[index].valid) heaps_[index].valid = false;
    };
    if (hz >= 0 && hx < columns_ && hz < rows_) invalidate(hx, hz);
    if (hz > 0 && hx < columns_ && hz <= rows_) invalidate(hx, hz - 1);
    if (hx > 0) {
        if (hz >= 0 && hx <= columns_ && hz < rows_) invalidate(hx - 1, hz);
        if (hz > 0 && hx <= columns_ && hz <= rows_) invalidate(hx - 1, hz - 1);
    }
}

void cSC4LandfillZone::RemoveLandfillTile(int x, int z) {
    ILandfillTile* tile = host_->FindLandfillTile(x, z);
    if (tile) {
        tile->RemoveFromCity();
        tile->Release();
    }
    host_->SetActiveLandfill(x, z, false);
}

bool cSC4LandfillZone::CreateLandfillOccupant(int x, int z, std::uint32_t tileId, int orientation) {
    RemoveIncompatibleOccupants(x, z);
    if (tileId == g_tileTable[0].id) {
        if (!host_->HasBaseTexture(x, z)) SetEmptyCellTexture(x, z);
        return true;
    }
    ILandfillTile* tile = host_->LoadLandfillTile(tileId, orientation);
    if (!tile) return false;   // the original dereferences null here
    // (the original also has an "empty tile" arm after the id test, which cannot be reached)
    tile->SetLayer(2);
    host_->SetActiveLandfill(x, z, true);
    tile->SetEmpty(false);
    float wx, wz;
    host_->CellToWorld(x, z, wx, wz);
    const float pos[3] = {wx, host_->AltitudeAt(wx, wz), wz};
    tile->SetPosition(pos);
    tile->Init();
    host_->AddOccupant(tile);
    tile->Release();
    return true;
}

void cSC4LandfillZone::SetEmptyCellTexture(int x, int z) { host_->PlaceEmptyCellTexture(x, z); }

void cSC4LandfillZone::RemoveIncompatibleOccupants(int x, int z) {
    std::vector<ILandfillOccupantRef*> list = host_->ListIncompatibleOccupants(x, z);
    for (ILandfillOccupantRef* occupant : list) occupant->RemoveFromCity();
    for (ILandfillOccupantRef* occupant : list) occupant->Release();
}

void cSC4LandfillZone::RemoveBaseTexture(int x, int z) {
    std::vector<ILandfillOccupantRef*> list = host_->ListBaseTextures(x, z);
    for (ILandfillOccupantRef* occupant : list) {
        if (occupant->TextureKind() == 1) occupant->RemoveFromCity();
        occupant->Release();
    }
}

bool cSC4LandfillZone::GetFirstRoadTileNeighbor(int& outX, int& outZ) {
    const LandfillRect& b = region_->bounds;
    const int cityX = host_->CityCellsX(), cityZ = host_->CityCellsZ();
    int xLow = b.x0 - 1;
    if (b.x0 < 1) xLow = 0;
    int xHigh = b.x1 + 1;
    if (cityX - 1 <= b.x1) xHigh = cityX - 1;
    const int zBelow = b.z0 - 1;
    int zHigh = b.z1 + 1;
    if (cityZ - 1 <= b.z1) zHigh = cityZ - 1;
    int zLow = 0;
    if (0 < b.z0) {   // top edge
        zLow = zBelow;
        for (int x = xLow; x < xHigh; ++x) {
            if (host_->HasNetworkAt(x, zBelow)) { outX = x; outZ = zBelow; return true; }
        }
    }
    if (b.z1 < zHigh) {   // bottom edge
        for (int x = xLow; x < xHigh; ++x) {
            if (host_->HasNetworkAt(x, zHigh)) { outX = x; outZ = zHigh; return true; }
        }
    }
    if (xLow < b.x0) {   // left edge
        for (int z = zLow; z < zHigh; ++z) {
            if (host_->HasNetworkAt(xLow, z)) { outX = xLow; outZ = z; return true; }
        }
    }
    if (b.x1 < xHigh) {   // right edge
        for (int z = zLow; z < zHigh; ++z) {
            if (host_->HasNetworkAt(xHigh, z)) { outX = xHigh; outZ = z; return true; }
        }
    }
    outX = b.x0;
    outZ = b.z0;
    return false;
}

// ---- zone editing ----------------------------------------------------------------------------------------------

void cSC4LandfillZone::DezoneCell(int x, int z, bool updateZoneManager, bool removeTile) {
    RemoveZot();
    if (removeTile) {
        RemoveLandfillTile(x, z);
        RemoveBaseTexture(x, z);
    }
    region_->SetBit(x, z, false);
    InvalidateSurroundingHeaps(x, z);
    if (updateZoneManager) host_->DezoneCell(x, z);
    cellCountDirty_ = true;
}

void cSC4LandfillZone::HandleTerrainChange(const LandfillRect& rect) {
    const LandfillRect& b = region_->bounds;
    // SC4Rect::IntersectRect (inclusive bounds)
    const int x0 = std::max(rect.x0, b.x0), z0 = std::max(rect.z0, b.z0);
    const int x1 = std::min(rect.x1, b.x1), z1 = std::min(rect.z1, b.z1);
    if (x0 > x1 || z0 > z1) return;
    for (int x = x0; x <= x1; ++x)
        for (int z = z0; z <= z1; ++z)
            if (region_->Test(x, z)) HandleCellTerrainChange(x, z);
}

void cSC4LandfillZone::HandleCellTerrainChange(int x, int z) {
    if (!region_->Test(x, z)) return;
    ILandfillTile* tile = host_->FindLandfillTile(x, z);
    if (!tile) {
        DezoneCell(x, z, true, true);
        return;
    }
    float pos[3];
    tile->GetPosition(pos);
    const float terrain = host_->TerrainHeightAt(pos[0], pos[2]);
    const float adjusted = host_->AdjustedHeight(terrain, pos[1]);
    const float difference = std::fabs(pos[1] - terrain);
    if (!(difference < 0.25f)) {
        if (!tile->IsEmptyTile()) {
            if (host_->Tunables().moveWithTerrain) {
                pos[1] = terrain > adjusted ? terrain : adjusted;
                tile->SetPosition(pos);
                host_->RefreshOccupant(tile);
            }
        } else {
            DezoneCell(x, z, true, true);
        }
    }
    tile->Release();
}

bool cSC4LandfillZone::IsCompletelyDezoned() {
    if (dezonedCache_) return true;
    dezonedCache_ = true;
    const LandfillRect& b = region_->bounds;
    for (int x = b.x0; x <= b.x1; ++x) {
        for (int z = b.z0; z <= b.z1; ++z) {
            if (region_->Test(x, z)) {
                dezonedCache_ = false;
                return false;
            }
        }
    }
    return true;
}

void cSC4LandfillZone::LevelRect(const LandfillRect& rect, float height, bool useTerrain) {
    levelHeight_ = height;
    LandfillRect clamped;
    clamped.x0 = rect.x0 < 0 ? 0 : rect.x0;
    clamped.z0 = rect.z0 < 0 ? 0 : rect.z0;
    clamped.x1 = rect.x1 < host_->CityCellsX() ? rect.x1 : host_->CityCellsX();
    clamped.z1 = rect.z1 < host_->CityCellsZ() ? rect.z1 : host_->CityCellsZ();
    if (useTerrain) host_->LevelTerrainRect(clamped, height);
    else host_->RefreshBuildingDevelopmentRect(rect);
}

void cSC4LandfillZone::PopulateZone() {
    const LandfillRect b = region_->bounds;
    for (int x = b.x0; x <= b.x1; ++x) {
        for (int z = b.z0; z <= b.z1; ++z) {
            if (region_->Test(x, z)) {
                if (host_->ZoneTypeAt(x, z) == 0xe && !host_->HasNetworkAt(x, z)) {
                    ILandfillTile* tile = host_->FindLandfillTile(x, z);
                    if (tile) {
                        tile->Release();
                    } else if (!host_->HasBaseTexture(x, z)) {
                        RemoveIncompatibleOccupants(x, z);
                        SetEmptyCellTexture(x, z);
                    }
                    if (!region_->Test(x, z)) InvalidateSurroundingHeaps(x, z);
                } else {
                    region_->SetBit(x, z, false);
                }
            } else if (!region_->Test(x, z)) {
                InvalidateSurroundingHeaps(x, z);
            }
        }
    }
}

void cSC4LandfillZone::AddRegionToZone(const LandfillCellRegion& added) {
    RemoveZot();
    const std::size_t oldCount = static_cast<std::size_t>(heapCount_ > 0 ? heapCount_ : 0);
    const std::vector<cGarbageHeap> oldHeaps = heaps_;
    const LandfillRect& cur = region_->bounds;
    const int nx0 = std::min(cur.x0, added.bounds.x0), nz0 = std::min(cur.z0, added.bounds.z0);
    const int nx1 = std::max(cur.x1, added.bounds.x1), nz1 = std::max(cur.z1, added.bounds.z1);
    LandfillCellRegion merged = LandfillCellRegion::Make(nx0, nz0, nx1, nz1, false);
    for (int x = cur.x0; x <= cur.x1; ++x)
        for (int z = cur.z0; z <= cur.z1; ++z)
            if (region_->Test(x, z)) merged.SetBit(x, z, true);
    for (int x = added.bounds.x0; x <= added.bounds.x1; ++x) {
        for (int z = added.bounds.z0; z <= added.bounds.z1; ++z) {
            if (added.Test(x, z)) {
                merged.SetBit(x, z, true);
                LevelRect({x, z, x + 1, z + 1}, levelHeight_, false);
            }
        }
    }
    delete region_;
    region_ = new LandfillCellRegion(merged);
    columns_ = region_->bounds.x1 - region_->bounds.x0;
    rows_ = region_->bounds.z1 - region_->bounds.z0;
    heapCount_ = rows_ * columns_;
    heaps_.clear();
    BuildHeaps();
    // old heaps go back by their zone-relative x/z (not shifted for a new origin: quirk)
    for (std::size_t i = 0; i < oldCount && i < oldHeaps.size(); ++i) {
        const cGarbageHeap& old = oldHeaps[i];
        const long index = static_cast<long>(old.z) * columns_ + old.x;
        if (index >= 0 && static_cast<std::size_t>(index) < heaps_.size()) heaps_[static_cast<std::size_t>(index)] = old;
    }
    const LandfillRect& b = region_->bounds;
    for (int x = b.x0; x <= b.x1; ++x)
        for (int z = b.z0; z <= b.z1; ++z)
            if (!region_->Test(x, z)) InvalidateSurroundingHeaps(x, z);
    cellCountDirty_ = true;
}

}  // namespace sc4
