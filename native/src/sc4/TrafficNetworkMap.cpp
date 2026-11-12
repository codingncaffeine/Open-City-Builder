// cSC4TrafficNetworkMap — hand-written from the x86_64 decompilation (see the layout notes in
// TrafficNetworkMap.h). Addresses in the comments are the original functions.
#include "sc4/TrafficNetworkMap.h"
#include "gz/StreamOps.h"
#include "sc4/DBSegment.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cfloat>


namespace sc4 {

namespace {
constexpr std::uint32_t kMaxStreamedGrid = 1u << 24;
constexpr std::uint32_t kMaxStreamedList = 1u << 22;
}  // namespace
namespace {

// The original converts a world coordinate to a cell with the float bit-pattern trick used all over
// this class: f' = f * 0.0625f; f' += -0.5f; f' += 12582912.0f; n = bits(f') - 0x4b400000;
// the +1 correction makes exact cell boundaries round up, i.e. the result is floor(f * 0.0625).
int CellFromWorld(float world) {
    const float scaled = world * 0.0625f;
    float adjusted = scaled + -0.5f;
    adjusted = adjusted + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &adjusted, sizeof bits);
    int n = static_cast<int>(bits - 0x4b400000u);
    const int next = n + 1;
    const float nextFloat = static_cast<float>(next);
    if (!(scaled != nextFloat)) n = next;   // ties on an exact cell boundary round up; NaN keeps n
    return n;
}

// The ushort saturation the occupancy accumulation uses: clamp(round_to_nearest(value), 0, 0xffff).
std::uint16_t SaturateUShort(float value) {
    float f = value + 12582912.0f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof bits);
    std::uint32_t result = bits + 0xb4c00000u;
    if (result > 0xfffeu) result = 0xffffu;
    return static_cast<std::uint16_t>(result);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------
// construction / object life cycle
// ---------------------------------------------------------------------------------------------------

// 0x100298a0e
namespace {
ITrafficNetworkMapHost* g_defaultHost = nullptr;
}  // namespace
ITrafficNetworkMapOccupant* ITrafficNetworkMapHost::AsMapOccupant(cISC4Occupant* occupant) {
    void* raw = nullptr;
    if (!occupant || !occupant->QueryInterface(0xa821ef94, &raw) || !raw) return nullptr;
    return static_cast<ITrafficNetworkMapOccupant*>(raw);
}

ITrafficNetworkMapHost* DefaultTrafficNetworkMapHost() { return g_defaultHost; }
void SetDefaultTrafficNetworkMapHost(ITrafficNetworkMapHost* host) { g_defaultHost = host; }

cSC4TrafficNetworkMap::cSC4TrafficNetworkMap(ITrafficNetworkMapHost* host, const TrafficNetworkMapTuning* tuning)
    : host_(host ? host : g_defaultHost), tuning_(tuning ? tuning : &ownedTuning_) {}

// 0x100298b9c
cSC4TrafficNetworkMap::~cSC4TrafficNetworkMap() {
    Shutdown();
}

// 0x100298c72
bool cSC4TrafficNetworkMap::Init() {
    if (initialized_) return true;
    initialized_ = true;
    cellsX_ = host_->CityCellsX();
    cellsZ_ = host_->CityCellsZ();
    shift_ = 1;
    for (std::uint32_t bit = 1; bit < 32; ++bit) {
        if ((1u << bit) == static_cast<std::uint32_t>(cellsX_)) break;
        shift_ = static_cast<int>(bit) + 1;
    }
    if (grid_.empty()) {
        const std::size_t count = static_cast<std::size_t>(cellsX_) * static_cast<std::size_t>(cellsZ_);
        grid_.assign(count, nullptr);
    }
    for (std::uint32_t message : {kTrafficMessageScrollStart, kTrafficMessageScrollEnd,
                                  kTrafficMessageOccupantInserted, kTrafficMessageNetworkUsableChanged,
                                  kTrafficMessageOccupantRemoved, kTrafficMessageTileOccupancyDirty,
                                  kTrafficMessageIntersectionsChanged, kTrafficMessageScrollActive,
                                  kTrafficMessageScrollInactive}) {
        host_->SubscribeMessage(message);
    }
    host_->SubscribeMessage(0xc0);
    if (tunnelPaths_) InitializeTunnelPaths();
    return true;
}

// 0x100298f2a
void cSC4TrafficNetworkMap::InitializeTunnelPaths() {
    for (TrafficTileInfo*& head : grid_) {
        for (TrafficTileInfo* tile = head; tile != nullptr; tile = tile->next) {
            ITrafficNetworkMapOccupant* occupant = tile->occupant;
            if (!occupant) continue;
            if (!occupant->HasNetworkFlag(kTrafficNetworkFlagTunnel)) continue;
            ITrafficNetworkMapTunnel* tunnel = nullptr;
            void* raw = nullptr;
            if (!occupant->QueryInterface(0x8a4bd52b, &raw)) continue;
            tunnel = static_cast<ITrafficNetworkMapTunnel*>(raw);
            ITrafficNetworkMapPathInfo* pathInfo = occupant->GetPathInfo();
            ITrafficNetworkMapOccupant* other = tunnel->GetOtherEndOccupant();
            if (pathInfo && other) host_->MakeTunnelPaths(pathInfo, occupant, other);
            tunnel->Release();
        }
    }
}

// 0x100299006
bool cSC4TrafficNetworkMap::Shutdown() {
    if (!initialized_) return true;
    ClearNetworkTileInfo();
    ClearIntersectionMap();
    ClearTrainJunctionMap();
    ClearUnusableRoadsMap();
    host_->UnsubscribeMessage(0xc0);
    for (std::uint32_t message : {kTrafficMessageScrollStart, kTrafficMessageScrollEnd,
                                  kTrafficMessageOccupantInserted, kTrafficMessageNetworkUsableChanged,
                                  kTrafficMessageOccupantRemoved, kTrafficMessageTileOccupancyDirty,
                                  kTrafficMessageIntersectionsChanged, kTrafficMessageScrollActive,
                                  kTrafficMessageScrollInactive}) {
        host_->UnsubscribeMessage(message);
    }
    initialized_ = false;
    return true;
}

// 0x10029917e
void cSC4TrafficNetworkMap::ClearNetworkTileInfo() {
    for (TrafficTileInfo*& head : grid_) {
        TrafficTileInfo* tile = head;
        while (tile != nullptr) {
            TrafficTileInfo* next = tile->next;
            ReleaseTileInfo(tile);
            delete tile;
            tile = next;
        }
        head = nullptr;
    }
    grid_.clear();
}

// 0x100299218
void cSC4TrafficNetworkMap::ClearIntersectionMap() {
    for (auto& entry : intersections_) {
        TrafficIntersectionArray& array = entry.second;
        for (ITrafficNetworkMapProp* prop : array.props) {
            if (prop) prop->Release();
        }
        array.props.clear();
        for (ITrafficNetworkMapAutomaton* automaton : array.automata) {
            if (automaton) automaton->Release();
        }
        array.automata.clear();
        entry.first->Release();
    }
    intersections_.clear();
}

// 0x100299288
void cSC4TrafficNetworkMap::ClearTrainJunctionMap() {
    for (auto& entry : trainJunctions_) {
        if (entry.first) entry.first->Release();
    }
    trainJunctions_.clear();
}

// 0x1002992c2
void cSC4TrafficNetworkMap::ClearUnusableRoadsMap() {
    for (auto& entry : unusableRoads_) {
        for (ITrafficNetworkMapOccupant* occupant : entry.second) {
            if (occupant) occupant->Release();
        }
    }
    unusableRoads_.clear();
}

// 0x100299312
bool cSC4TrafficNetworkMap::DoMessage(::cGZMessage&) {
    return false;
}

// @ i386 0x001ecbd6. Network pieces inserted / removed reach the tiles (a subway or tunnel piece as usable-changed), the
// city's post-init builds the intersections, 0x69247dc7 marks the tile occupancy dirty and the camera's move refreshes the
// intersections in view.
//   PARTIAL: 0x86ad10ee (vslot 0xe4), 0x89d3e9f3 (usable-changed through the network filter), a removed train leaving the
//   intersections' train lists and the stoplight props' update on a prop insert are not ported; the scroll granularity test
//   is reduced to "not while scrolling".
bool cSC4TrafficNetworkMap::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    switch (type) {
    case kTrafficMessageIntersectionsChanged: HandlePostCityInit(); break;
    case kTrafficMessageTileOccupancyDirty: tileOccupancyDirty_ = true; break;
    case kTrafficMessageOccupantInserted:
    case kTrafficMessageOccupantRemoved: {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        ITrafficNetworkMapOccupant* network = occupant && host_ ? host_->AsMapOccupant(occupant) : nullptr;
        if (!network) break;
        if (type == kTrafficMessageOccupantInserted) {
            bool special = network->IsOfType(7);   // subway
            if (!special) {
                void* raw = nullptr;
                special = network->QueryInterface(0x8a4bd52b, &raw) && raw;   // a tunnel piece
                if (raw) static_cast<cIGZUnknown*>(raw)->Release();
            }
            if (special) HandleNetworkOccupantUsable(network, true);
            else HandleInsertNetworkOccupant(network);
        } else {
            HandleRemoveNetworkOccupant(network);
        }
        network->Release();
        break;
    }
    case kTrafficMessageScrollEnd:
        if (!scrollActive_) HandleViewChanged();
        break;
    default: return DoMessage2(type, 0, 0.0f);
    }
    return true;
}

// 0x1002a034c — four sub-objects: the map (0xca0f81b9) at +0, the message target (iid 1) at +8, the
// message target 2 (0x452294aa) at +0x10 and the serializable (0xe4fda3d4) at +0x18. The class id
// 0x6a0f82b2 is not answered (as in the original).
bool cSC4TrafficNetworkMap::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZIID_cISC4TrafficNetworkMap) *out = static_cast<cISC4TrafficNetworkMap*>(this);
    else if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 1) *out = static_cast<cIGZMessageTarget*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x1002a03c0
std::uint32_t cSC4TrafficNetworkMap::AddRef() {
    return ++refCount_;
}

// 0x1002a03f8
std::uint32_t cSC4TrafficNetworkMap::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// ---------------------------------------------------------------------------------------------------
// tile storage
// ---------------------------------------------------------------------------------------------------

// 0x1002a04ea
TrafficTileInfo* cSC4TrafficNetworkMap::GetTileInfoAtCell(int cellX, int cellZ) const {
    const std::size_t index = static_cast<std::size_t>((cellZ << shift_) + cellX);
    return grid_[index];
}

// 0x10029d7a6
TrafficTileInfo* cSC4TrafficNetworkMap::SafeGetTileInfoAtCell(int cellX, int cellZ) const {
    if (cellX < 0 || cellZ < 0 || cellX >= cellsX_ || cellZ >= cellsZ_) return nullptr;
    return GetTileInfoAtCell(cellX, cellZ);
}

// 0x1002a0502 / 0x1002a0510 are inline in the header.

// 0x10029b414
void cSC4TrafficNetworkMap::ReleaseTileInfo(TrafficTileInfo* tile) {
    if (!tile) return;
    if (tile->occupant) {
        tile->occupant->Release();
        tile->occupant = nullptr;
    }
    for (ITrafficNetworkMapAutomaton* automaton : tile->automata) {
        if (automaton) automaton->Release();
    }
    tile->automata.clear();
}

// 0x10029a228 — inserts the occupant into the unusable-roads map keyed by its top-left cell.
bool cSC4TrafficNetworkMap::HandleInsertNetworkOccupant(ITrafficNetworkMapOccupant* occupant) {
    if (!occupant) return false;
    SC4CellRect rect{};
    occupant->AsOccupant()->GetBoundingCityCells(rect);
    const std::uint32_t key = (static_cast<std::uint32_t>(rect.x0) << 16) | static_cast<std::uint32_t>(rect.z0);
    std::vector<ITrafficNetworkMapOccupant*>& list = unusableRoads_[key];
    for (ITrafficNetworkMapOccupant* existing : list) {
        if (existing == occupant) return false;
    }
    occupant->AddRef();
    list.push_back(occupant);
    return true;
}

// 0x10029a5ea — creates the per-cell TileInfo nodes for the occupant's bounding rectangle.
bool cSC4TrafficNetworkMap::HandleNetworkOccupantUsable(ITrafficNetworkMapOccupant* occupant, bool unusable) {
    if (!occupant) return true;
    occupant->AddRef();
    SC4CellRect rect{};
    occupant->AsOccupant()->GetBoundingCityCells(rect);
    for (int z = rect.z0; z <= rect.z1; ++z) {
        for (int x = rect.x0; x <= rect.x1; ++x) {
            const std::size_t index = static_cast<std::size_t>((z << shift_) + x);
            if (index >= grid_.size()) continue;
            TrafficTileInfo* tile = new TrafficTileInfo();
            tile->occupant = occupant;
            occupant->AddRef();
            tile->next = grid_[index];
            grid_[index] = tile;
        }
    }
    const SC4RectInt32 cells{rect.x0, rect.z0, rect.x1, rect.z1};
    if (unusable) RemoveUnusableNetworkOccupant(occupant, &cells);
    // The two occupant filters are both traffic filters, so every usable network occupant becomes an
    // intersection / train-junction candidate; the map decides in AddIntersection/AddTrainJunction.
    AddIntersection(occupant);
    AddTrainJunction(occupant);
    occupant->Release();
    return true;
}

// 0x10029a790
void cSC4TrafficNetworkMap::RemoveUnusableNetworkOccupant(ITrafficNetworkMapOccupant* occupant,
                                                          const SC4RectInt32* rect) {
    if (!occupant) return;
    std::uint32_t key = 0;
    if (!rect) {
        std::uint32_t x = 0, z = 0;
        occupant->GetOccupiedCell(x, z);
        key = (x << 16) | z;
    } else {
        key = (static_cast<std::uint32_t>(rect->x0) << 16) | static_cast<std::uint32_t>(rect->z0);
    }
    auto it = unusableRoads_.find(key);
    if (it == unusableRoads_.end()) return;
    std::vector<ITrafficNetworkMapOccupant*>& list = it->second;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (list[i] != occupant) continue;
        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
        occupant->Release();
        break;
    }
    if (list.empty()) unusableRoads_.erase(it);
}

// 0x10029b202
bool cSC4TrafficNetworkMap::HandleRemoveNetworkOccupant(ITrafficNetworkMapOccupant* occupant) {
    if (!occupant) return false;
    RemoveIntersection(occupant);
    RemoveTrainJunction(occupant);
    SC4CellRect rect{};
    occupant->AsOccupant()->GetBoundingCityCells(rect);
    const std::size_t x0 = static_cast<std::size_t>(rect.x0);
    const std::size_t x1 = static_cast<std::size_t>(rect.x1);
    for (std::size_t z = static_cast<std::size_t>(rect.z0); z <= static_cast<std::size_t>(rect.z1); ++z) {
        for (std::size_t x = x0; x <= x1; ++x) {
            const std::size_t index = (z << shift_) + x;
            if (index >= grid_.size()) continue;
            TrafficTileInfo** link = &grid_[index];
            while (*link) {
                TrafficTileInfo* tile = *link;
                if (tile->occupant == occupant) {
                    *link = tile->next;
                    ReleaseTileInfo(tile);
                    delete tile;
                } else {
                    link = &tile->next;
                }
            }
        }
    }
    SC4RectInt32 cells{rect.x0, rect.z0, rect.x1, rect.z1};
    RemoveUnusableNetworkOccupant(occupant, &cells);
    return true;
}

// 0x10029d9b0
TrafficTileInfo* cSC4TrafficNetworkMap::GetTileInfoByNetwork(ITrafficNetworkMapOccupant* occupant) const {
    if (!occupant) return nullptr;
    std::uint32_t x = 0, z = 0;
    occupant->GetOccupiedCell(x, z);
    TrafficTileInfo* tile = SafeGetTileInfoAtCell(static_cast<int>(x), static_cast<int>(z));
    while (tile) {
        if (tile->occupant == occupant) return tile;
        tile = tile->next;
    }
    return nullptr;
}

// 0x10029d7ca
TrafficTileInfo* cSC4TrafficNetworkMap::GetTileInfoAtNeighborEdge(ITrafficNetworkMapOccupant* occupant,
                                                                  std::uint8_t edge, bool secondDirection,
                                                                  std::uint32_t pathType) const {
    if (!occupant) return nullptr;
    std::uint32_t x = 0, z = 0;
    occupant->GetOccupiedCell(x, z);
    const int e = edge & 3u;
    TrafficTileInfo* tile = SafeGetTileInfoAtCell(static_cast<int>(x) + kTrafficEdgeDeltaX[e],
                                                  static_cast<int>(z) + kTrafficEdgeDeltaZ[e]);
    if (!tile) {
        // A tunnel reaches through to the tile on its far side.
        void* raw = nullptr;
        if (!occupant->QueryInterface(0x8a4bd52b, &raw)) return nullptr;
        ITrafficNetworkMapTunnel* tunnel = static_cast<ITrafficNetworkMapTunnel*>(raw);
        ITrafficNetworkMapOccupant* other = tunnel->GetOtherEndOccupant();
        if (other) {
            std::uint32_t ox = 0, oz = 0;
            other->GetOccupiedCell(ox, oz);
            bool farSide = false;
            switch (e) {
                case 0: farSide = x > ox; break;
                case 1: farSide = oz < z; break;
                case 2: farSide = x < ox; break;
                case 3: farSide = z < oz; break;
            }
            if (farSide) tile = SafeGetTileInfoAtCell(static_cast<int>(ox), static_cast<int>(oz));
        }
        tunnel->Release();
    }
    if (!secondDirection) {
        if (pathType == 0) return tile;
        for (; tile != nullptr; tile = tile->next) {
            ITrafficNetworkMapOccupant* occupantAtTile = tile->occupant;
            if (occupantAtTile && occupantAtTile->IsUsable() &&
                occupantAtTile->PathConnectionExists(pathType, 0xff, 0xff))
                return tile;
        }
        return nullptr;
    }
    for (; tile != nullptr; tile = tile->next) {
        ITrafficNetworkMapOccupant* occupantAtTile = tile->occupant;
        if (occupantAtTile && occupantAtTile->IsUsable() &&
            occupant->PathConnectionExists(0, 0xff, edge) &&
            occupantAtTile->PathConnectionExists(pathType, edge, 0xff))
            return tile;
    }
    return nullptr;
}

// 0x10029da12
ITrafficNetworkMapOccupant* cSC4TrafficNetworkMap::FindNetworkOccupant(int cellX, int cellZ) const {
    TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ);
    return tile ? tile->occupant : nullptr;
}

// 0x10029da2a
ITrafficNetworkMapOccupant* cSC4TrafficNetworkMap::FindNetworkOccupant(int cellX, int cellZ, float y0,
                                                                        float y1) const {
    TrafficTileInfo* tile = FindNetworkTile(cellX, cellZ, y0, y1);
    return tile ? tile->occupant : nullptr;
}

// 0x10029da42
ITrafficNetworkMapOccupant* cSC4TrafficNetworkMap::FindNetworkOccupant(const float* position) const {
    if (!position) return nullptr;
    return FindNetworkOccupant(CellFromWorld(position[0]), CellFromWorld(position[2]));
}

// 0x10029dad6
ITrafficNetworkMapOccupant* cSC4TrafficNetworkMap::FindNetworkOccupant(int cellX, int cellZ,
                                                                       std::uint32_t flags, bool unusable) const {
    for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ); tile != nullptr; tile = tile->next) {
        if (tile->occupant && tile->occupant->HasAnyNetworkFlag(flags)) return tile->occupant;
    }
    if (!unusable) return nullptr;
    const std::uint32_t key = (static_cast<std::uint32_t>(cellX) << 16) | static_cast<std::uint32_t>(cellZ);
    auto it = unusableRoads_.find(key);
    if (it == unusableRoads_.end()) return nullptr;
    for (ITrafficNetworkMapOccupant* occupant : it->second) {
        if (occupant && occupant->HasAnyNetworkFlag(flags)) return occupant;
    }
    return nullptr;
}

// 0x10029db98
ITrafficNetworkMapOccupant* cSC4TrafficNetworkMap::FindNetworkOccupant(const float* position,
                                                                       std::uint32_t flags, bool unusable) const {
    return FindNetworkOccupant(CellFromWorld(position[0]), CellFromWorld(position[2]), flags, unusable);
}

// 0x10029dbd0
TrafficTileInfo* cSC4TrafficNetworkMap::FindNetworkTile(int cellX, int cellZ, float y0, float y1) const {
    if (!initialized_) return nullptr;
    float position[3] = {0.0f, 0.0f, 0.0f};
    for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ); tile != nullptr; tile = tile->next) {
        if (!tile->occupant) continue;
        tile->occupant->AsOccupant()->GetPosition(position);
        if (y0 <= position[1] && position[1] <= y1) return tile;
    }
    return nullptr;
}

// 0x10029d70a
bool cSC4TrafficNetworkMap::ConnectionExists(int cellX, int cellZ, std::uint8_t entry, std::uint8_t exit,
                                             std::uint32_t pathType) const {
    for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ); tile != nullptr; tile = tile->next) {
        if (ConnectionExists(tile->occupant, entry, exit, pathType)) return true;
    }
    return false;
}

// 0x10029d77c
bool cSC4TrafficNetworkMap::ConnectionExists(ITrafficNetworkMapOccupant* occupant, std::uint8_t entry,
                                             std::uint8_t exit, std::uint32_t pathType) {
    if (!occupant) return false;
    return occupant->PathConnectionExists(pathType, entry, exit);
}

std::size_t cSC4TrafficNetworkMap::TileCount() const {
    std::size_t count = 0;
    for (TrafficTileInfo* head : grid_) {
        for (TrafficTileInfo* tile = head; tile != nullptr; tile = tile->next) ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------------------------------
// volume / congestion accumulation
// ---------------------------------------------------------------------------------------------------

// 0x100299ae0 — scans west, east and south for adjacent buildings (the original never scans north).
void cSC4TrafficNetworkMap::UpdateRoadNeighborInfo(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile) {
    if (!tile) return;
    tile->roadMask = 0;
    tile->occupancyMask = 0;
    tile->buildingMask = 0;
    std::memset(tile->edgeOccupancy, 0, sizeof tile->edgeOccupancy);
    if (!occupant) return;
    SC4CellRect rect{};
    occupant->AsOccupant()->GetBoundingCityCells(rect);
    // west (edge 0): two cells outwards from x0
    for (int z = rect.z0; z <= rect.z1; ++z) {
        int x = rect.x0;
        for (int step = 0; step < 2; ++step) {
            --x;
            if (x < rect.x0 - 2 || !CheckCellOccupancy(x, z, 0, tile)) break;
        }
    }
    // east (edge 2): two cells outwards from x1
    for (int z = rect.z0; z <= rect.z1; ++z) {
        int x = rect.x1;
        for (int step = 0; step < 2; ++step) {
            ++x;
            if (x > rect.x1 + 2 || !CheckCellOccupancy(x, z, 2, tile)) break;
        }
    }
    // south (edge 3): two cells outwards from z1
    for (int x = rect.x0; x <= rect.x1; ++x) {
        int z = rect.z1;
        for (int step = 0; step < 2; ++step) {
            ++z;
            if (z > rect.z1 + 2 || !CheckCellOccupancy(x, z, 3, tile)) break;
        }
    }
}

// 0x10029cc22
bool cSC4TrafficNetworkMap::CheckCellOccupancy(int cellX, int cellZ, std::uint8_t edge, TrafficTileInfo* tile) {
    if (!tile) return false;
    if (cellX < 0 || cellZ < 0 || cellX >= cellsX_ || cellZ >= cellsZ_) return false;
    ITrafficNetworkMapLot* lot = host_->LotAtCell(cellX, cellZ);
    if (!lot) return false;
    if (!lot->IsLivable()) return false;
    ITrafficNetworkMapBuilding* building = host_->LotBuilding(lot);
    if (!building) return false;
    const std::uint32_t purpose = building->Purpose();   // BuildingProfile +0
    const std::uint32_t wealth = building->Wealth();     // BuildingProfile +4
    if (wealth - 1u < 3u) tile->occupancyMask |= kTrafficFootprintMaskZ[wealth - 1u];
    if (purpose - 1u < 8u) {
        tile->buildingMask |= kTrafficFootprintMaskX[purpose - 1u];
        tile->roadMask |= kTrafficBuildingTypeMask[purpose - 1u];
    }
    if (purpose == 0) return true;
    if ((tile->buildingMask & tuning_->footprintMaskB) == 0) return true;
    if ((tile->roadMask & tuning_->footprintMaskA) == 0) return true;
    const int activeEdges = host_->ActiveEdgesForBuilding(purpose, lot->OccupantWealth());
    const std::uint16_t value = lot->TravelPopulation(static_cast<std::uint32_t>(activeEdges));
    tile->edgeOccupancy[edge & 3u] = SaturateUShort(static_cast<float>(value));
    return true;
}

// 0x10029cdbc
void cSC4TrafficNetworkMap::WalkNeighborOccupancy(ITrafficNetworkMapOccupant* occupant, std::uint8_t edge,
                                                   std::uint8_t depth, std::uint16_t& occupancy) {
    if (!occupant) return;
    std::uint32_t x = 0, z = 0;
    occupant->GetOccupiedCell(x, z);
    TrafficTileInfo* tile = GetTileInfoAtCell(static_cast<int>(x), static_cast<int>(z));
    if (!tile) return;
    const float decay = static_cast<float>(static_cast<int>(depth & 0xff) - 1) /
                        static_cast<float>(tuning_->neighborDivisor);
    const std::uint8_t opposite = static_cast<std::uint8_t>((edge + 2) & 3);
    for (std::uint32_t e = 0; e < 4; ++e) {
        if (GetTileInfoAtNeighborEdge(occupant, static_cast<std::uint8_t>(e), false, 0)) continue;
        const float product = static_cast<float>(tile->edgeOccupancy[e]) * decay;
        const float biased = product + 12582912.0f;
        std::uint32_t bits = 0;
        std::memcpy(&bits, &biased, sizeof bits);
        const std::int32_t delta = static_cast<std::int32_t>(bits - 0x4b400000u);
        const std::uint32_t value = static_cast<std::uint32_t>(occupancy) + static_cast<std::uint32_t>(delta);
        occupancy = static_cast<std::uint16_t>(value > 0xffffu ? 0xffffu : value);
    }
    if (depth <= 1) return;
    std::uint8_t candidate = edge;
    do {
        TrafficTileInfo* neighbor = GetTileInfoAtNeighborEdge(occupant, candidate, false, 0);
        if (neighbor && occupant->PathConnectionExists(0, opposite, candidate)) {
            if (!neighbor->occupant) return;
            WalkNeighborOccupancy(neighbor->occupant, candidate, static_cast<std::uint8_t>(depth - 1), occupancy);
            return;
        }
        candidate = static_cast<std::uint8_t>((candidate + 1) & 3);
    } while (candidate != edge);
}

// 0x100299c4c
void cSC4TrafficNetworkMap::UpdateNetworkOccupancyInfo(ITrafficNetworkMapOccupant* occupant, TrafficTileInfo* tile) {
    if (!tile) return;
    for (std::uint32_t e = 0; e < 4; ++e) {
        TrafficTileInfo* neighbor = GetTileInfoAtNeighborEdge(occupant, static_cast<std::uint8_t>(e), false, 0);
        if (neighbor && neighbor->occupant) {
            WalkNeighborOccupancy(neighbor->occupant, static_cast<std::uint8_t>(e), tuning_->neighborDivisor,
                                  tile->edgeOccupancy[e]);
        }
    }
    std::memset(tile->distribution, 0, sizeof tile->distribution);
    std::uint32_t totals[4] = {tile->edgeOccupancy[0], tile->edgeOccupancy[1], tile->edgeOccupancy[2],
                               tile->edgeOccupancy[3]};
    const std::uint32_t total = totals[0] + totals[1] + totals[2] + totals[3];
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (tile->edgeOccupancy[i] == 0) continue;
        for (std::uint32_t j = 0; j < 4; ++j) {
            if (i == j) continue;
            const std::uint32_t product = static_cast<std::uint32_t>(tile->edgeOccupancy[i]) *
                                          static_cast<std::uint32_t>(tile->edgeOccupancy[j]);
            const std::uint32_t quotient = total ? product / total : 0;
            const double random = random_.RandomDoubleRangeUniform(tuning_->occupancyJitterLow,
                                                                   tuning_->occupancyJitterHigh);
            const float value = static_cast<float>(static_cast<double>(quotient) * random);
            tile->distribution[i][j] = SaturateUShort(value);
        }
    }
}

std::size_t cSC4TrafficNetworkMap::PathTypeIndex(std::uint32_t pathType) {
    // Quirk: every path type outside 1..12 maps to slot 0.
    return (pathType - 1u < 12u) ? pathType - 1u : 0u;
}

// 0x10029dc54
std::uint32_t* cSC4TrafficNetworkMap::GetLaneTick(ITrafficNetworkMapOccupant* occupant, std::uint32_t layer,
                                                  std::uint32_t pathType, std::uint8_t a, std::uint8_t b) {
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) {
        static std::uint32_t badResult = 0;
        return &badResult;
    }
    const std::size_t index = PathTypeIndex(pathType);
    const std::uint32_t key = static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(a) << 8) |
                              (pathType << 24);
    (void)layer;
    if (tile->laneTicks.size() <= index) tile->laneTicks.resize(index + 1);
    std::vector<TrafficLaneTick>& entries = tile->laneTicks[index];
    for (TrafficLaneTick& entry : entries) {
        if (entry.key == key) return &entry.laneTick;
    }
    entries.push_back(TrafficLaneTick{key, 0, 0, 0});
    return &entries.back().laneTick;
}

// 0x10029de12
std::uint32_t* cSC4TrafficNetworkMap::GetAutosPerSecond(ITrafficNetworkMapOccupant* occupant,
                                                        std::uint32_t layer, std::uint32_t pathType,
                                                        std::uint8_t a, std::uint8_t b) {
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) {
        static std::uint32_t badResult = 0;
        return &badResult;
    }
    const std::size_t index = PathTypeIndex(pathType);
    const std::uint32_t key = static_cast<std::uint32_t>(b) | (static_cast<std::uint32_t>(a) << 8) |
                              (pathType << 24);
    (void)layer;
    if (tile->laneTicks.size() <= index) tile->laneTicks.resize(index + 1);
    std::vector<TrafficLaneTick>& entries = tile->laneTicks[index];
    for (TrafficLaneTick& entry : entries) {
        if (entry.key == key) return &entry.autosPerSecond;
    }
    entries.push_back(TrafficLaneTick{key, 0, 0, 0});
    return &entries.back().autosPerSecond;
}

// 0x10029df44 — a forwarder: key = path type << 24 | a << 8 | b, then the frequency-table lookup.
std::uint32_t* cSC4TrafficNetworkMap::GetAutomataCount(ITrafficNetworkMapOccupant* occupant,
                                                       std::uint32_t layer, std::uint32_t pathType,
                                                       std::uint8_t a, std::uint8_t b) {
    const std::uint32_t key = (pathType << 24) | (static_cast<std::uint32_t>(a) << 8) | b;
    return GetAutomataCount(occupant, layer, key);
}

// 0x10029df60
std::uint32_t* cSC4TrafficNetworkMap::GetAutomataCount(ITrafficNetworkMapOccupant* occupant,
                                                       std::uint32_t layer, std::uint32_t key) {
    (void)layer;
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) {
        static std::uint32_t badResult = 0;
        return &badResult;
    }
    for (TrafficFrequency& entry : tile->frequencies) {
        if (entry.key == key) return &entry.automataCount;
    }
    tile->frequencies.push_back(TrafficFrequency{key, 0, 0});
    return &tile->frequencies.back().automataCount;
}

// 0x10029e082
std::uint32_t cSC4TrafficNetworkMap::GetAutomataCountForLane(ITrafficNetworkMapOccupant* occupant,
                                                             std::uint32_t lane) {
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) return 0;
    std::uint32_t count = 0;
    for (ITrafficNetworkMapAutomaton* automaton : tile->automata) {
        if (!automaton) continue;
        if (!automaton->IsRemoved() && automaton->GetLane() == lane) ++count;
    }
    return count;
}

// 0x10029e104
std::uint32_t* cSC4TrafficNetworkMap::GetAutomataCount(ITrafficNetworkMapOccupant* occupant,
                                                       std::uint32_t pathType) {
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) {
        static std::uint32_t badResult = 0;
        return &badResult;
    }
    for (TrafficFrequency& entry : tile->frequencies) {
        if (entry.key == pathType) return &entry.automataCount;
    }
    tile->frequencies.push_back(TrafficFrequency{pathType, 0, 0});
    return &tile->frequencies.back().automataCount;
}

// 0x10029e1a8
std::uint32_t* cSC4TrafficNetworkMap::GetAutosPerSecond(ITrafficNetworkMapOccupant* occupant,
                                                        std::uint32_t pathType) {
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) {
        static std::uint32_t badResult = 0;
        return &badResult;
    }
    for (TrafficFrequency& entry : tile->frequencies) {
        if (entry.key == pathType) return &entry.autosPerSecond;
    }
    tile->frequencies.push_back(TrafficFrequency{pathType, 0, 0});
    return &tile->frequencies.back().autosPerSecond;
}

// 0x10029e24c
int cSC4TrafficNetworkMap::ChooseWeightedEdge(const float* weights, std::uint8_t* edges) {
    float pick[4] = {weights[0], weights[1], weights[2], weights[3]};
    float total = 0.0f;
    int nonzero = 0;
    for (int i = 0; i < 4; ++i) {
        total = total + weights[i];
        if (1e-05f < weights[i]) ++nonzero;
    }
    std::uint8_t remaining = static_cast<std::uint8_t>(4 - nonzero);
    for (int round = 0; round < 4; ++round) {
        float cumulative = total;
        std::uint8_t left = remaining;
        for (std::uint32_t i = 0; i < 4; ++i) {
            const float weight = pick[i];
            if (!(0.0f <= weight)) continue;
            const float probability = (cumulative <= 1e-05f) ? 1.0f / static_cast<float>(left)
                                                             : weight / cumulative;
            const bool significant = 1e-05f < weight;
            const double random = random_.RandomDoubleUniform();
            if (random <= static_cast<double>(probability)) {
                total = significant ? total - weight : total;
                remaining = static_cast<std::uint8_t>(remaining + (significant ? 0 : -1));
                edges[round] = static_cast<std::uint8_t>(i);
                pick[i] = -1.0f;
                break;
            }
            cumulative = significant ? cumulative - weight : cumulative;
            if (weight <= 1e-05f) --left;
        }
        if (round == 3) return nonzero;
    }
    return nonzero;
}

// 0x10029e74a
bool cSC4TrafficNetworkMap::EnterNetwork(ITrafficNetworkMapAutomaton* automaton,
                                         ITrafficNetworkMapOccupant* occupant) {
    if (!automaton || !occupant) return false;
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) return false;
    for (ITrafficNetworkMapAutomaton* existing : tile->automata) {
        if (existing == automaton) return false;
    }
    automaton->AddRef();
    // In front (Mac i386 0x001ea0da): the list runs from the newest arrival to the oldest, and FindLeaderInTile scans
    // from an automaton towards the end, i.e. over the ones that entered the tile before it and are ahead of it.
    tile->automata.push_front(automaton);
    const std::uint32_t lane = automaton->GetLane();
    std::uint32_t* aggregate = GetAutomataCount(occupant, automaton->PathTypeWord(), lane);
    *aggregate += 1;
    std::uint32_t* byType = GetAutomataCount(occupant, (lane >> 24) & 0xf);
    *byType += 1;
    return true;
}

// 0x10029e862
bool cSC4TrafficNetworkMap::ExitNetwork(ITrafficNetworkMapAutomaton* automaton,
                                        ITrafficNetworkMapOccupant* occupant, std::uint32_t lane) {
    if (!automaton || !occupant) return false;
    TrafficTileInfo* tile = GetTileInfoByNetwork(occupant);
    if (!tile) return false;
    bool removed = false;
    for (auto it = tile->automata.begin(); it != tile->automata.end(); ++it) {
        if (*it != automaton) continue;
        tile->automata.erase(it);
        std::uint32_t* aggregate = GetAutomataCount(occupant, automaton->PathTypeWord(), lane);
        if (*aggregate != 0) *aggregate -= 1;
        std::uint32_t* byType = GetAutomataCount(occupant, (lane >> 24) & 0xf);
        if (*byType != 0) *byType -= 1;
        automaton->Release();
        removed = true;
        break;
    }
    return removed;
}

// ---------------------------------------------------------------------------------------------------
// intersections and train junctions
// ---------------------------------------------------------------------------------------------------

TrafficIntersectionArray* cSC4TrafficNetworkMap::FindIntersection(ITrafficNetworkMapOccupant* occupant) {
    auto it = intersections_.find(occupant);
    return it == intersections_.end() ? nullptr : &it->second;
}

const TrafficIntersectionArray* cSC4TrafficNetworkMap::FindIntersection(
    ITrafficNetworkMapOccupant* occupant) const {
    auto it = intersections_.find(occupant);
    return it == intersections_.end() ? nullptr : &it->second;
}

TrafficTrainJunctionArray* cSC4TrafficNetworkMap::FindTrainJunction(ITrafficNetworkMapOccupant* occupant) {
    auto it = trainJunctions_.find(occupant);
    return it == trainJunctions_.end() ? nullptr : &it->second;
}

const TrafficTrainJunctionArray* cSC4TrafficNetworkMap::FindTrainJunction(
    ITrafficNetworkMapOccupant* occupant) const {
    auto it = trainJunctions_.find(occupant);
    return it == trainJunctions_.end() ? nullptr : &it->second;
}

// 0x10029c42a
void cSC4TrafficNetworkMap::InitializeIntersectionEdge(ITrafficNetworkMapOccupant* occupant,
                                                       ITrafficNetworkMapOccupant* neighbor,
                                                       TrafficIntersectionArray& intersections,
                                                       TrafficIntersectionInfo& info, std::uint8_t edge) {
    // A tile that is neither the common road types nor rail has no approach.
    bool noApproach = false;
    if (!occupant->IsOfType(0) && !occupant->IsOfType(3) && !occupant->IsOfType(1) &&
        !occupant->IsOfType(7) && !occupant->IsOfType(8)) {
        noApproach = !occupant->IsOfType(6);
    }
    info = TrafficIntersectionInfo{};
    info.green = 1;
    ITrafficNetworkMapPathInfo* pathInfo = occupant->GetPathInfo();
    if (!pathInfo) return;
    const int stopPoints = pathInfo->GetStopPointCount(0, edge, 0xff, 0xff);
    if (noApproach) return;
    if (stopPoints == 0) return;
    const bool isRoadOrRail = occupant->IsOfType(1) || occupant->IsOfType(8);
    if (!isRoadOrRail) {
        if (!neighbor || !neighbor->IsOfType(3)) {
            info.type = 2;
            if (occupant->IsOfType(3)) {
                info.otherDuration = tuning_->junctionTimeLeft;
            } else if (occupant->IsOfType(0)) {
                info.otherDuration = tuning_->stoplightPeriodA;
            } else if (occupant->IsOfType(6)) {
                info.otherDuration = tuning_->stoplightPeriodB;
            }
            float stateDuration = tuning_->stoplightPeriodA;
            if (neighbor && !neighbor->IsOfType(0) && !neighbor->IsOfType(6)) {
                info.stateDuration = 0.0f;
            } else {
                info.stateDuration = stateDuration;
            }
        } else {
            info.type = 1;
            info.otherDuration = tuning_->junctionDuration;
            info.stateDuration = tuning_->junctionTimeLeft;
        }
    } else {
        info.type = 3;
        info.otherDuration = 0.0f;
        info.stateDuration = 0.0f;
    }
    switch (info.type) {
        case 1:
            info.green = 0;
            info.timeLeft = info.otherDuration;
            break;
        case 3:
            info.green = intersections.automata.empty() ? 1 : 0;
            info.timeLeft = 0.0f;
            break;
        default: {
            const bool southOrNorth = ((edge | 2) == 3);
            if (occupant->IsOfType(0)) {
                info.green = static_cast<std::uint8_t>(stoplightGroupA_ == southOrNorth);
                info.timeLeft = tuning_->stoplightPeriodA - stoplightPhaseA_;
            } else if (occupant->IsOfType(6)) {
                info.green = static_cast<std::uint8_t>(stoplightGroupB_ == southOrNorth);
                info.timeLeft = tuning_->stoplightPeriodB - stoplightPhaseB_;
            } else {
                return;
            }
            break;
        }
    }
}

// 0x10029a186
void cSC4TrafficNetworkMap::InitializeIntersection(ITrafficNetworkMapOccupant* occupant,
                                                    TrafficIntersectionArray& intersections) {
    if (!occupant) return;
    if (!occupant->GetPathInfo()) return;
    intersections.flag50 = 0;
    intersections.dirty = 1;
    for (std::uint32_t edge = 0; edge < 4; ++edge) {
        TrafficTileInfo* tile = GetTileInfoAtNeighborEdge(occupant, static_cast<std::uint8_t>(edge), false, 0);
        ITrafficNetworkMapOccupant* neighbor = tile ? tile->occupant : nullptr;
        InitializeIntersectionEdge(occupant, neighbor, intersections, intersections.approaches[edge],
                                   static_cast<std::uint8_t>(edge));
    }
    UpdateIntersectionProps(occupant, intersections, true);
    intersectionsDirty_ = true;
}

// 0x10029979c
void cSC4TrafficNetworkMap::UpdateIntersectionProps(ITrafficNetworkMapOccupant* occupant,
                                                    TrafficIntersectionArray& intersections, bool flag) {
    (void)occupant;
    bool allRed = false;
    for (const TrafficIntersectionInfo& approach : intersections.approaches) {
        if (approach.type == 3 && approach.green == 0) {
            allRed = true;
            break;
        }
    }
    for (ITrafficNetworkMapProp* prop : intersections.props) {
        if (!prop) continue;
        const bool approachLight = prop->IsOccupantGroup(0x5002) && prop->HasProperty(0x4a70d491);
        if (!approachLight) {
            if (!prop->IsOccupantGroup(0x5003)) continue;
            const int state = prop->GetState();
            if (allRed) {
                if (state != 0 && state != 3) prop->SetState(flag ? 0 : 3);
            } else if (static_cast<unsigned>(state - 1) > 1u) {
                prop->SetState(flag ? 2 : 1);
            }
            continue;
        }
        int index = prop->GetOrientation();
        if (!prop->IsFlagSet(0x80)) index = (index + 2) & 3;
        if (static_cast<unsigned>(index) >= 4u) continue;
        TrafficIntersectionInfo& approach = intersections.approaches[index];
        if (approach.type != 2) continue;
        if (approach.green == 0) {
            prop->SetState(2);
        } else {
            prop->SetState(approach.timeLeft <= 0.75f ? 1 : 0);
        }
    }
    intersections.dirty = 0;
}

// 0x10029b628
void cSC4TrafficNetworkMap::InitIntersectionProps(ITrafficNetworkMapOccupant* occupant,
                                                  TrafficIntersectionArray& intersections) {
    std::uint32_t cellX = 0, cellZ = 0;
    occupant->GetOccupiedCell(cellX, cellZ);
    for (ITrafficNetworkMapProp* prop : intersections.props) {
        if (prop) prop->Release();
    }
    intersections.props.clear();
    std::vector<ITrafficNetworkMapProp*> props = host_->PropsAtCell(static_cast<int>(cellX),
                                                                   static_cast<int>(cellZ));
    for (ITrafficNetworkMapProp* prop : props) {
        if (prop) intersections.props.push_back(prop);   // ownership of the counted reference moves here
    }
}

// 0x10029a872
void cSC4TrafficNetworkMap::AddIntersection(ITrafficNetworkMapOccupant* occupant) {
    if (!occupant) return;
    ITrafficNetworkMapPathInfo* pathInfo = occupant->GetPathInfo();
    if (!pathInfo) return;
    if (!pathInfo->IsIntersection(1) && !pathInfo->IsIntersection(3)) return;
    auto it = intersections_.find(occupant);
    bool inserted = false;
    if (it == intersections_.end()) {
        it = intersections_.emplace(occupant, TrafficIntersectionArray{}).first;
        inserted = true;
    }
    if (inserted) {
        occupant->AddRef();
        InitializeIntersection(occupant, it->second);
    }
    std::uint32_t cellX = 0, cellZ = 0;
    occupant->GetOccupiedCell(cellX, cellZ);
    for (std::uint32_t edge = 0; edge < 4; ++edge) {
        const int nx = static_cast<int>(cellX) + kTrafficEdgeDeltaX[edge];
        const int nz = static_cast<int>(cellZ) + kTrafficEdgeDeltaZ[edge];
        for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(nx, nz); tile != nullptr; tile = tile->next) {
            if (!tile->occupant) continue;
            TrafficIntersectionArray* neighbor = FindIntersection(tile->occupant);
            if (neighbor) InitializeIntersection(tile->occupant, *neighbor);
        }
    }
}

// 0x10029b31a
void cSC4TrafficNetworkMap::RemoveIntersection(ITrafficNetworkMapOccupant* occupant) {
    auto it = intersections_.find(occupant);
    if (it == intersections_.end()) return;
    for (ITrafficNetworkMapProp* prop : it->second.props) {
        if (prop) prop->Release();
    }
    it->second.props.clear();
    for (ITrafficNetworkMapAutomaton* automaton : it->second.automata) {
        if (automaton) automaton->Release();
    }
    it->second.automata.clear();
    occupant->Release();
    intersections_.erase(it);
}

// 0x10029ad1e
void cSC4TrafficNetworkMap::AddTrainJunction(ITrafficNetworkMapOccupant* occupant) {
    if (!occupant) return;
    occupant->AddRef();
    ITrafficNetworkMapPathInfo* pathInfo = occupant->GetPathInfo();
    if (!pathInfo) {
        occupant->Release();
        return;
    }
    auto existing = trainJunctions_.find(occupant);
    if (existing != trainJunctions_.end()) {
        existing->first->Release();
        trainJunctions_.erase(existing);
    }
    TrafficTrainJunctionArray array;
    bool allEmpty = true;
    for (std::uint32_t pathType = 0; pathType < 4; ++pathType) {
        if (pathInfo->GetJunctionKeys(3, static_cast<std::uint8_t>(pathType), array.keys[pathType]) ||
            pathInfo->GetJunctionKeys(7, static_cast<std::uint8_t>(pathType), array.keys[pathType])) {
            array.cursor[pathType] = 0;
            allEmpty = false;
        }
    }
    if (!allEmpty) {
        trainJunctions_.emplace(occupant, std::move(array));
        occupant->AddRef();
    }
    occupant->Release();
}

// 0x10029b3ca
void cSC4TrafficNetworkMap::RemoveTrainJunction(ITrafficNetworkMapOccupant* occupant) {
    auto it = trainJunctions_.find(occupant);
    if (it == trainJunctions_.end()) return;
    occupant->Release();
    trainJunctions_.erase(it);
}

// 0x10029c2a0 / i386 0x001e7cf2: the path info's IsIntersection of path type 0 only.
bool cSC4TrafficNetworkMap::IsIntersection(ITrafficNetworkMapOccupant* occupant) {
    if (!occupant) return false;
    ITrafficNetworkMapPathInfo* pathInfo = occupant->GetPathInfo();
    if (!pathInfo) return false;
    return pathInfo->IsIntersection(0);
}

// 0x10029c2cc
int cSC4TrafficNetworkMap::CheckIntersection(ITrafficNetworkMapOccupant* occupant,
                                             ITrafficNetworkMapAutomaton* automaton, std::uint32_t edge,
                                             float& time) {
    time = 3.4028235e+38f;
    if (!occupant) return 0;
    std::uint32_t cellX = 0, cellZ = 0;
    occupant->GetOccupiedCell(cellX, cellZ);
    if (static_cast<int>(cellX) < visible_.x0 || static_cast<int>(cellZ) < visible_.z0) return 0;
    if (visible_.x1 < static_cast<int>(cellX) || visible_.z1 < static_cast<int>(cellZ)) return 0;
    if ((edge & 0xffu) >= 4u) return 0;
    TrafficTileInfo* tile = GetTileInfoAtNeighborEdge(occupant, static_cast<std::uint8_t>(edge), false, 0);
    if (!tile) return 0;
    TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
    if (!intersections) return 0;
    const std::uint32_t index = (static_cast<std::uint8_t>(edge) + 2u) & 3u;
    TrafficIntersectionInfo& approach = intersections->approaches[index];
    if (approach.type == 0) return 0;
    if (approach.type == 3) {
        if (intersections->automata.empty()) return 0;
        return intersections->automata.front() != automaton ? 3 : 0;
    }
    int result = 0;
    if (approach.green == 0) result = approach.type;
    if (approach.type != 2) return result;
    if (approach.green == 0) {
        time = approach.otherDuration - approach.timeLeft;
    } else {
        time = approach.stateDuration - approach.timeLeft;
    }
    return 2;
}

// 0x10029c256
void cSC4TrafficNetworkMap::UpdateIntersection(ITrafficNetworkMapOccupant* occupant) {
    TrafficIntersectionArray* intersections = FindIntersection(occupant);
    if (!intersections) return;
    InitIntersectionProps(occupant, *intersections);
}

// 0x10029b9e0
void cSC4TrafficNetworkMap::UpdateIntersectionMap() {
    for (auto& entry : intersections_) {
        InitIntersectionProps(entry.first, entry.second);
    }
}

// 0x10029b7ae
bool cSC4TrafficNetworkMap::HandleTuningParametersChanged() {
    for (auto& entry : intersections_) {
        InitializeIntersection(entry.first, entry.second);
    }
    return true;
}

// 0x10029ba20
void cSC4TrafficNetworkMap::TickSingleIntersection(ITrafficNetworkMapOccupant* occupant,
                                                   TrafficIntersectionArray& intersections, float seconds) {
    for (TrafficIntersectionInfo& approach : intersections.approaches) {
        if (approach.type != 2) continue;
        approach.timeLeft = approach.timeLeft - seconds;
        if (approach.timeLeft <= 0.0f) {
            approach.green = static_cast<std::uint8_t>(approach.green ^ 1);
            approach.nearSwitch = 0;
            intersections.dirty = 1;
            approach.timeLeft = approach.green != 0 ? approach.stateDuration : approach.otherDuration;
        } else if (approach.timeLeft < 0.75f && approach.nearSwitch == 0) {
            approach.nearSwitch = 1;
            intersections.dirty = 1;
        }
    }
    if (intersections.dirty) UpdateIntersectionProps(occupant, intersections, false);
}

// 0x10029b7dc
void cSC4TrafficNetworkMap::TickIntersections(float milliseconds) {
    const float seconds = milliseconds / 1000.0f;
    stoplightPhaseA_ = stoplightPhaseA_ + seconds;
    if (tuning_->stoplightPeriodA <= stoplightPhaseA_) {
        stoplightPhaseA_ = 0.0f;
        stoplightGroupA_ = !stoplightGroupA_;
    }
    stoplightPhaseB_ = stoplightPhaseB_ + seconds;
    if (tuning_->stoplightPeriodB <= stoplightPhaseB_) {
        stoplightPhaseB_ = 0.0f;
        stoplightGroupB_ = !stoplightGroupB_;
    }
    if (intersectionsDirty_) {
        UpdateIntersectionMap();
        intersectionsDirty_ = false;
    }
    for (int z = visible_.z0; z <= visible_.z1; ++z) {
        for (int x = visible_.x0; x <= visible_.x1; ++x) {
            for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(x, z); tile != nullptr; tile = tile->next) {
                if (!tile->occupant) continue;
                TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
                if (intersections) TickSingleIntersection(tile->occupant, *intersections, seconds);
            }
        }
    }
    int visited = 0;
    std::uint32_t guard = 0;
    do {
        ++cursorX_;
        if (cursorX_ < cellsX_) {
            // keep the cursor
        } else {
            cursorX_ = 0;
            ++cursorZ_;
            if (cellsZ_ <= cursorZ_) cursorZ_ = 0;
        }
        for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(cursorX_, cursorZ_); tile != nullptr; tile = tile->next) {
            if (!tile->occupant) continue;
            TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
            if (intersections) UpdateIntersectionTiming(tile->occupant, *intersections);
        }
        if (SafeGetTileInfoAtCell(cursorX_, cursorZ_)) ++visited;
        ++guard;
    } while (guard < 0x3f && visited < 4);
}

// 0x10029baaa — PARTIAL: the 1.9 KB graph pass that rebalances the neighbouring intersections' green
// durations (it builds a std::set of intersections to visit and rewrites their approach timings).
void cSC4TrafficNetworkMap::UpdateIntersectionTiming(ITrafficNetworkMapOccupant*, TrafficIntersectionArray&) {
    // Not recovered yet; see TRAFFICNETWORKMAP-2.
}

// 0x10029cb16
void cSC4TrafficNetworkMap::SwitchStoplightStates(ITrafficNetworkMapProp* prop) {
    if (!prop) return;
    float position[3] = {0.0f, 0.0f, 0.0f};
    prop->GetPosition(position);
    for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(static_cast<int>(position[0] * 0.0625f),
                                                       static_cast<int>(position[2] * 0.0625f));
         tile != nullptr; tile = tile->next) {
        TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
        if (!intersections) continue;
        bool changed = false;
        for (TrafficIntersectionInfo& approach : intersections->approaches) {
            if (approach.type == 3) {
                approach.green = 0;
                approach.timeLeft = 5.0f;   // 0x40a00000
                changed = true;
            } else if (approach.type == 2) {
                approach.green = static_cast<std::uint8_t>(approach.green ^ 1);
                approach.timeLeft = approach.green != 0 ? approach.stateDuration : approach.otherDuration;
                changed = true;
            }
        }
        if (changed) UpdateIntersectionProps(tile->occupant, *intersections, false);
    }
}

// 0x10029c786
void cSC4TrafficNetworkMap::MarkTrainInCell(ITrafficNetworkMapAutomaton* automaton, int cellX, int cellZ) {
    if (!automaton) return;
    for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ); tile != nullptr; tile = tile->next) {
        TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
        if (!intersections) continue;
        for (ITrafficNetworkMapAutomaton* existing : intersections->automata) {
            if (existing == automaton) return;
        }
        const std::uint32_t lane = automaton->GetLane();
        automaton->AddRef();
        if (lane == 0x10) {
            intersections->automata.push_back(automaton);
        } else {
            intersections->automata.push_front(automaton);
        }
        for (TrafficIntersectionInfo& approach : intersections->approaches) {
            if (approach.green != 0) intersections->dirty = 1;
            approach.green = 0;
        }
        if (intersections->dirty) UpdateIntersectionProps(tile->occupant, *intersections, false);
        return;
    }
}

// 0x10029c8e2
void cSC4TrafficNetworkMap::UnmarkTrainInCell(ITrafficNetworkMapAutomaton* automaton, int cellX, int cellZ) {
    if (!automaton) return;
    for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ); tile != nullptr; tile = tile->next) {
        TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
        if (!intersections) continue;
        for (auto it = intersections->automata.begin(); it != intersections->automata.end(); ++it) {
            if (*it != automaton) continue;
            intersections->automata.erase(it);
            automaton->Release();
            break;
        }
        if (!intersections->automata.empty()) return;
        for (TrafficIntersectionInfo& approach : intersections->approaches) {
            if (approach.green == 0) intersections->dirty = 1;
            approach.green = 1;
        }
        if (intersections->dirty) UpdateIntersectionProps(tile->occupant, *intersections, false);
    }
}

// 0x10029c9d2
std::uint32_t cSC4TrafficNetworkMap::GetTrainJunctionPath(ITrafficNetworkMapOccupant* occupant,
                                                          std::uint8_t pathType) const {
    TrafficTrainJunctionArray* junction = const_cast<cSC4TrafficNetworkMap*>(this)->FindTrainJunction(occupant);
    if (!junction) return 0;
    const std::uint32_t index = pathType & 3u;
    const std::vector<std::uint32_t>& keys = junction->keys[index];
    if (keys.empty()) return 0;
    return keys[junction->cursor[index]];
}

// 0x10029ca1e
std::size_t cSC4TrafficNetworkMap::GetTrainJunctionKeys(ITrafficNetworkMapOccupant* occupant,
                                                        std::uint8_t pathType, std::vector<std::uint32_t>& keys,
                                                        std::uint16_t& cursor) const {
    keys.clear();
    cursor = 0;
    TrafficTrainJunctionArray* junction = const_cast<cSC4TrafficNetworkMap*>(this)->FindTrainJunction(occupant);
    if (!junction) return 0;
    const std::uint32_t index = pathType & 3u;
    keys = junction->keys[index];
    cursor = junction->cursor[index];
    return keys.size();
}

// 0x10029cab0
bool cSC4TrafficNetworkMap::ToggleTrainJunction(ITrafficNetworkMapOccupant* occupant) {
    TrafficTrainJunctionArray* junction = FindTrainJunction(occupant);
    if (!junction) return false;
    for (std::uint32_t pathType = 0; pathType < 4; ++pathType) {
        const std::size_t count = junction->keys[pathType].size();
        if (count == 0) continue;
        junction->cursor[pathType] = static_cast<std::uint16_t>((junction->cursor[pathType] + 1u) % count);
    }
    return true;
}


// ---------------------------------------------------------------------------------------------------
// geometry and surrounding roads
// ---------------------------------------------------------------------------------------------------

// 0x10029cf42
bool cSC4TrafficNetworkMap::ConstrainPointToNetworkTile(cS3DVector3& point, std::uint32_t flags) const {
    const int cellX = static_cast<int>(point.x * 0.0625f);
    const int cellZ = static_cast<int>(point.z * 0.0625f);
    TrafficTileInfo* tile = SafeGetTileInfoAtCell(cellX, cellZ);
    if (tile && tile->occupant && tile->occupant->HasAnyNetworkFlag(flags)) return true;
    TrafficTileInfo* best = nullptr;
    float bestDistance = 3.4028235e+38f;
    float bestX = 0.0f, bestZ = 0.0f;
    for (int z = cellZ - 2; z <= cellZ + 2; ++z) {
        const float lowZ = static_cast<float>(z * 16);
        const float highZ = static_cast<float>(z * 16 + 16);
        for (int x = cellX - 2; x <= cellX + 2; ++x) {
            tile = SafeGetTileInfoAtCell(x, z);
            if (!tile || !tile->occupant || !tile->occupant->HasAnyNetworkFlag(flags)) continue;
            const float lowX = static_cast<float>(x * 16);
            const float highX = static_cast<float>(x * 16 + 16);
            float clampedX = point.x < lowX ? lowX : (point.x <= highX ? point.x : highX);
            float clampedZ = point.z < lowZ ? lowZ : (point.z <= highZ ? point.z : highZ);
            const float dx = point.x - clampedX;
            const float dz = point.z - clampedZ;
            const float distance = std::sqrt(dz * dz + dx * dx);
            if (distance < bestDistance) {
                best = tile;
                bestDistance = distance;
                bestX = clampedX;
                bestZ = clampedZ;
            }
        }
    }
    if (!best) return false;
    point.x = bestX;
    point.z = bestZ;
    point.y = GetRoadAltitude(point.x, point.z);
    return true;
}

// 0x10029d174 (Mac i386 0x001e9154): keep a step from `from` to `point` on tiles of the wanted networks. A point already on
// one stays. Otherwise the part of the step that crosses an edge of the tile `from` stands on is turned along that edge: one
// candidate per crossed edge (x first), and the first candidate on a wanted tile becomes the point (true). With no such
// candidate the point is clamped into the starting tile (false). A step that crosses no edge, or one of no length, falls
// back to the nearest wanted tile around the point. The point's height follows the road.
bool cSC4TrafficNetworkMap::ConstrainVectorToNetworkTile(const cS3DVector3& from, cS3DVector3& point,
                                                         std::uint32_t flags) const {
    const auto wanted = [&](float x, float z) {
        TrafficTileInfo* tile = SafeGetTileInfoAtCell(static_cast<int>(0.0625f * x), static_cast<int>(z * 0.0625f));
        return tile && tile->occupant && tile->occupant->HasAnyNetworkFlag(flags);
    };
    if (wanted(point.x, point.z)) return true;
    const float dx = point.x - from.x;
    const float dy = point.y - from.y;
    const float dz = point.z - from.z;
    if (std::sqrt(dx * dx + dy * dy + dz * dz) <= 1e-05f) return ConstrainPointToNetworkTile(point, flags);
    constexpr float kInside = 0.01f;   // 0x00a706d0: how far inside the far edge a point is put
    const float lowX = static_cast<float>(static_cast<int>(from.x * 0.0625f) * 0x10);
    const float lowZ = static_cast<float>(static_cast<int>(0.0625f * from.z) * 0x10);
    const float highX = lowX + 16.0f, highZ = lowZ + 16.0f;
    const float signX = 0.0f <= dx ? 1.0f : -1.0f;
    const float signZ = 0.0f <= dz ? 1.0f : -1.0f;
    cS3DVector3 candidates[2];
    int count = 0;
    const float px = point.x, pz = point.z;
    if (px < lowX) {
        candidates[count++] = cS3DVector3{lowX, point.y, signZ * (lowX - px) + pz};
    } else if (highX <= px) {
        candidates[count++] = cS3DVector3{highX - kInside, point.y, signZ * (px - highX) + pz};
    }
    if (pz < lowZ) {
        candidates[count++] = cS3DVector3{signX * (lowZ - pz) + px, point.y, lowZ};
    } else if (highZ <= pz) {
        candidates[count++] = cS3DVector3{signX * (pz - highZ) + px, point.y, highZ - kInside};
    }
    if (count == 0) return ConstrainPointToNetworkTile(point, flags);
    bool onNetwork = false;
    for (int i = 0; i < count && !onNetwork; ++i) {
        if (!wanted(candidates[i].x, candidates[i].z)) continue;
        point = candidates[i];
        onNetwork = true;
    }
    if (!onNetwork) {
        point.x = px < lowX ? lowX : (px <= highX - kInside ? px : highX - kInside);
        point.z = pz < lowZ ? lowZ : (pz <= highZ - kInside ? pz : highZ - kInside);
    }
    point.y = GetRoadAltitude(point.x, point.z);
    return onNetwork;
}

// 0x10029d538
float cSC4TrafficNetworkMap::GetRoadAltitude(float x, float z) const {
    if (!host_->HasTerrain()) return 0.0f;
    const int cellX = CellFromWorld(x);
    const int cellZ = CellFromWorld(z);
    ITrafficNetworkMapOccupant* occupant = FindNetworkOccupant(cellX, cellZ, 0x1f4f, true);
    if (!occupant) return host_->TerrainHeightAt(x, z);
    if (!occupant->HasNetworkFlag(0x304)) return occupant->GetAltitude(x, z);
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->AsOccupant()->GetPosition(position);
    position[0] = x;
    position[2] = z;
    ITrafficNetworkMapPathInfo* pathInfo = occupant->GetPathInfo();
    cS3DVector3 point{position[0], position[1], position[2]};
    if (pathInfo && pathInfo->SnapPointToAnyPath(point, 0, nullptr) != -1) return point.y;
    return host_->TerrainHeightAt(x, z);
}

// 0x10029e40e
bool cSC4TrafficNetworkMap::GetSurroundingRoads(std::vector<ITrafficNetworkMapOccupant*>& out,
                                                const SC4RectInt32& rect, int margin, int flag,
                                                std::uint32_t networkFlags) {
    const float boxMin[3] = {static_cast<float>((rect.x0 - margin) * 16), -3.4028235e+38f,
                             static_cast<float>((rect.z0 - margin) * 16)};
    const float boxMax[3] = {static_cast<float>((margin + 1 + rect.x1) * 16), 3.4028235e+38f,
                             static_cast<float>((margin + 1 + rect.z1) * 16)};
    out.clear();
    std::vector<cISC4Occupant*> candidates = host_->NetworkOccupantsInBox(boxMin, boxMax);
    for (cISC4Occupant* candidate : candidates) {
        if (!candidate) continue;
        ITrafficNetworkMapOccupant* occupant = host_->AsMapOccupant(candidate);   // the original: QI 0xa821ef94 (see the host)
        if (!occupant) continue;
        bool outskirts = true;
        if (flag >= 1) {
            SC4CellRect cells{};
            candidate->GetBoundingCityCells(cells);
            if (cells.x0 > rect.x0 - margin && cells.z0 > rect.z0 - margin &&
                cells.x0 < rect.x1 + margin && cells.z0 < rect.z1 + margin) {
                outskirts = false;
            }
        }
        if (outskirts && occupant->HasAnyNetworkFlag(networkFlags) &&
            occupant->HasNetworkFlag(kTrafficNetworkFlagUsable)) {
            occupant->AddRef();
            out.push_back(occupant);
        }
        occupant->Release();
    }
    host_->ReleaseOccupants(candidates);
    return !out.empty();
}

// 0x10029e684
bool cSC4TrafficNetworkMap::GetSurroundingRoads(std::vector<ITrafficNetworkMapOccupant*>& out,
                                                cISC4Occupant* occupant, int margin, int flag,
                                                std::uint32_t networkFlags) {
    if (!occupant) return false;
    std::int32_t cells[4] = {0, 0, 0, 0};
    ITrafficNetworkMapLot* lot = host_->LotOf(occupant);
    if (lot) {
        lot->GetBoundingRect(cells);
    } else {
        SC4CellRect rect{};
        occupant->GetBoundingCityCells(rect);
        cells[0] = rect.x0;
        cells[1] = rect.z0;
        cells[2] = rect.x1;
        cells[3] = rect.z1;
    }
    return GetSurroundingRoads(out, SC4RectInt32{cells[0], cells[1], cells[2], cells[3]}, margin, flag,
                               networkFlags);
}

// ---------------------------------------------------------------------------------------------------
// ticks, view and city-init hooks
// ---------------------------------------------------------------------------------------------------

// 0x100299982
void cSC4TrafficNetworkMap::DoAnimationTick(std::uint32_t frame, float milliseconds) {
    (void)frame;
    if (host_->SimulationActive()) TickIntersections(milliseconds);
    if (tileOccupancyDirty_) {
        tileOccupancyDirty_ = false;
        for (int z = visible_.z0; z <= visible_.z1; ++z) {
            for (int x = visible_.x0; x <= visible_.x1; ++x) {
                for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(x, z); tile != nullptr; tile = tile->next) {
                    if (!tile->occupant) continue;
                    UpdateRoadNeighborInfo(tile->occupant, tile);
                    UpdateNetworkOccupancyInfo(tile->occupant, tile);
                }
            }
        }
    }
    gcTimer_ = milliseconds + gcTimer_;
    if (gcTimer_ < 1000.0f) return;
    gcTimer_ = 0.0f;
    GarbageCollectPathInfo();
}

// 0x100299dc0
void cSC4TrafficNetworkMap::GarbageCollectPathInfo() {
    if (gcX_ < 0 || cellsX_ <= gcX_) gcX_ = 0;
    if (gcZ_ < 0 || cellsZ_ <= gcZ_) gcZ_ = 0;
    const bool active = host_->SimulationActive();
    cRZTimer timer(cRZTimer::kMilliseconds);
    timer.Start();
    for (int x = gcX_; x < cellsX_;) {
        const int z = gcZ_;
        if (cellsZ_ <= z) break;
        const bool insideVisible = active && visible_.x0 <= x && visible_.z0 <= z && x <= visible_.x1 &&
                                   z <= visible_.z1;
        if (!insideVisible) {
            for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(x, z); tile != nullptr; tile = tile->next) {
                if (tile->occupant && !tile->occupant->HasNetworkFlag(kTrafficNetworkFlagTunnel)) {
                    tile->occupant->ReleasePathInfo();
                }
            }
        }
        ++gcX_;
        if (cellsX_ <= gcX_) {
            gcX_ = 0;
            ++gcZ_;
        }
        float elapsed = 0.0f;
        timer.GetElapsedTimeFloat(elapsed);
        if (5.0f <= elapsed || cellsX_ <= gcX_ || cellsZ_ <= gcZ_) break;
        x = gcX_;
    }
    timer.Stop();
}

// 0x100299f86
bool cSC4TrafficNetworkMap::HandleViewChanged() {
    const int oldX0 = visible_.x0, oldZ0 = visible_.z0, oldX1 = visible_.x1, oldZ1 = visible_.z1;
    float viewX = 0.0f, viewY = 0.0f, viewZ = 0.0f;
    float targetX = 0.0f, targetY = 0.0f, targetZ = 0.0f;
    host_->GetViewPosition(viewX, viewY, viewZ);
    host_->GetViewTarget(targetX, targetY, targetZ);
    int cellX = 0, cellZ = 0, targetCellX = 0, targetCellZ = 0;
    host_->PositionToCell(viewX, viewZ, cellX, cellZ);
    host_->PositionToCell(targetX, targetZ, targetCellX, targetCellZ);
    visible_.x0 = std::min(std::max(cellX - 5, 0), cellsX_ - 1);
    visible_.x1 = std::min(std::max(targetCellX + 5, 0), cellsX_ - 1);
    visible_.z0 = std::min(std::max(cellZ - 5, 0), cellsZ_ - 1);
    visible_.z1 = std::min(std::max(targetCellZ + 5, 0), cellsZ_ - 1);
    for (int z = visible_.z0; z <= visible_.z1; ++z) {
        for (int x = visible_.x0; x <= visible_.x1; ++x) {
            const bool insideOld = oldX0 <= x && oldZ0 <= z && x <= oldX1 && z <= oldZ1;
            if (insideOld) continue;
            for (TrafficTileInfo* tile = SafeGetTileInfoAtCell(x, z); tile != nullptr; tile = tile->next) {
                if (!tile->occupant) continue;
                TrafficIntersectionArray* intersections = FindIntersection(tile->occupant);
                if (intersections) InitializeIntersection(tile->occupant, *intersections);
            }
        }
    }
    return true;
}

// 0x10029b464
bool cSC4TrafficNetworkMap::HandlePostCityInit() {
    if (std::getenv("SC4_TRAFFICMAPDEBUG")) {
        int tiles = 0, withOccupant = 0;
        for (TrafficTileInfo* head : grid_)
            for (TrafficTileInfo* tile = head; tile != nullptr; tile = tile->next) {
                ++tiles;
                withOccupant += tile->occupant != nullptr;
            }
        std::fprintf(stderr, "traffic map: post city init, %d tiles, %d with a network piece, %zu unusable roads\n", tiles, withOccupant,
                     unusableRoads_.size());
    }
    if (!tunnelPaths_) return true;
    for (auto& entry : unusableRoads_) {
        std::vector<ITrafficNetworkMapOccupant*> list = entry.second;
        for (ITrafficNetworkMapOccupant* occupant : list) {
            occupant->ClearNetworkFlag(0x4000);
            occupant->SetNetworkFlag(kTrafficNetworkFlagUsable);
            occupant->Release();
            HandleNetworkOccupantUsable(occupant, false);
        }
    }
    unusableRoads_.clear();
    for (auto& entry : intersections_) {
        InitIntersectionProps(entry.first, entry.second);
    }
    std::vector<ITrafficNetworkMapOccupant*> removals;
    for (TrafficTileInfo* head : grid_) {
        for (TrafficTileInfo* tile = head; tile != nullptr; tile = tile->next) {
            if (!tile->occupant) continue;
            AddTrainJunction(tile->occupant);
            if (tile->occupant->HasAnyNetworkFlag(0x30)) {
                tile->occupant->AddRef();
                removals.push_back(tile->occupant);
            }
        }
    }
    for (ITrafficNetworkMapOccupant* occupant : removals) {
        HandleRemoveNetworkOccupant(occupant);
        occupant->Release();
    }
    return true;
}

// 0x100299322 — PARTIAL: the ten message ids are known (see Init), but the cIGZMessage2 accessors
// themselves are not recovered; the three state-only ids are handled here.
bool cSC4TrafficNetworkMap::DoMessage2(std::uint32_t messageId, std::uint32_t dataA, float dataB) {
    (void)dataA;
    (void)dataB;
    switch (messageId) {
        case kTrafficMessageIntersectionsChanged:
            HandlePostCityInit();
            return true;
        case kTrafficMessageTileOccupancyDirty:
            tileOccupancyDirty_ = true;
            return true;
        case kTrafficMessageScrollActive:
            scrollActive_ = true;
            return true;
        case kTrafficMessageScrollInactive:   // i386 0x001ecbd6: the flag is cleared and the visible cells are worked out again
            scrollActive_ = false;
            HandleViewChanged();
            return true;
        case 0xa6b79602:   // the camera moved (zoom, rotation): the visible cells are worked out again
            HandleViewChanged();
            return true;
        default:
            return true;
    }
}

// ---------------------------------------------------------------------------------------------------
// persistence
// ---------------------------------------------------------------------------------------------------

// 0x10029e972 — the version-7 record. PARTIAL: Write is complete, but Read (0x10029eeaa, 4.5 KB) is not
// hand-written yet, so no round trip is verified.
bool cSC4TrafficNetworkMap::Write(cIGZOStream& stream) const {
    stream.SetUint16(7);
    const std::uint32_t cellCount = static_cast<std::uint32_t>(cellsX_) * static_cast<std::uint32_t>(cellsZ_);
    stream.SetUint32(cellCount);
    if (cellCount == 0) {
        stream.SetUint32(0);
    } else {
        std::uint32_t tileCount = 0;
        for (TrafficTileInfo* head : grid_) {
            for (TrafficTileInfo* tile = head; tile != nullptr; tile = tile->next) ++tileCount;
        }
        stream.SetUint32(tileCount);
        for (std::uint32_t cell = 0; cell < cellCount; ++cell) {
            for (TrafficTileInfo* tile = grid_[cell]; tile != nullptr; tile = tile->next) {
                stream.SetUint32(cell);
                // SC4WriteGZSerializable(stream, tile->occupant): a reference (null when the tile has no piece), as Read takes it
                SC4WriteGZSerializable(stream, tile->occupant ? static_cast<const cIGZUnknown*>(tile->occupant->AsOccupant()) : nullptr);
                stream.SetUint32(static_cast<std::uint32_t>(tile->laneTicks.size()));
                for (std::uint32_t type = 0; type < tile->laneTicks.size(); ++type) {
                    stream.SetUint32(type);
                    const std::vector<TrafficLaneTick>& entries = tile->laneTicks[type];
                    stream.SetUint32(static_cast<std::uint32_t>(entries.size()));
                    for (const TrafficLaneTick& entry : entries) {
                        stream.SetUint32(entry.key);
                        stream.SetFloat32(static_cast<float>(entry.laneTick));
                    }
                }
                // PARTIAL: the original writes a reference per automaton on the tile; the port writes none (the cars re-enter the
                // tiles as they drive), so the count says none.
                stream.SetUint32(0);
                stream.SetUint8(0);
                stream.SetUint32(tile->roadMask);
                stream.SetUint32(tile->occupancyMask);
                stream.SetUint32(tile->buildingMask);
                for (std::uint32_t i = 0; i < 4; ++i) {
                    stream.SetFloat32(static_cast<float>(tile->edgeOccupancy[i]));
                    for (std::uint32_t j = 0; j < 4; ++j) stream.SetUint16(tile->distribution[i][j]);
                }
                stream.SetUint16(0);
                stream.SetUint32(static_cast<std::uint32_t>(tile->frequencies.size()));
                for (const TrafficFrequency& entry : tile->frequencies) {
                    stream.SetUint32(entry.key);
                    stream.SetFloat32(static_cast<float>(entry.automataCount));
                    stream.SetUint16(static_cast<std::uint16_t>(entry.autosPerSecond));
                }
            }
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(intersections_.size()));
    for (const auto& entry : intersections_) {
        stream.SetUint32(0);   // occupant reference placeholder (SC4WriteGZSerializable in the original)
        stream.SetUint8(entry.second.dirty != 0);
        stream.SetUint32(static_cast<std::uint32_t>(entry.second.automata.size()));
        for (const TrafficIntersectionInfo& approach : entry.second.approaches) {
            stream.SetUint32(static_cast<std::uint32_t>(approach.type));
            stream.SetUint8(approach.green);
            stream.SetFloat32(approach.stateDuration);
            stream.SetFloat32(approach.otherDuration);
            stream.SetFloat32(approach.timeLeft);
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(unusableRoads_.size()));
    for (const auto& entry : unusableRoads_) {
        stream.SetUint32(entry.first);
        stream.SetUint32(0);   // the occupant references of the cell are written by SC4WriteGZSerializable in the original
    }
    stream.SetSint32(visible_.x0);
    stream.SetSint32(visible_.z0);
    stream.SetSint32(visible_.x1);
    stream.SetSint32(visible_.z1);
    stream.SetFloat32(stoplightPhaseA_);
    stream.SetUint8(stoplightGroupA_ ? 1 : 0);
    stream.SetFloat32(stoplightPhaseB_);
    stream.SetUint8(stoplightGroupB_ ? 1 : 0);
    stream.SetUint32(static_cast<std::uint32_t>(trainJunctions_.size()));
    for (const auto& entry : trainJunctions_) {
        stream.SetUint32(0);   // occupant reference placeholder
        for (std::uint32_t type = 0; type < 4; ++type) {
            stream.SetUint32(static_cast<std::uint32_t>(entry.second.keys[type].size()));
            for (std::uint32_t key : entry.second.keys[type]) stream.SetUint32(key);
            stream.SetUint16(entry.second.cursor[type]);
        }
    }
    return !stream.GetError();
}

// 0x10029eeaa. The stream is read completely. Every occupant and automaton reference of the file is read (and released): the port keeps
// the traffic data keyed by its own occupant interface, which the native city does not hand out yet (see the host), so the per-occupant maps
// (intersections, unusable roads, train junctions) are consumed but not rebuilt here; the tile data and the stoplight state are kept.
bool cSC4TrafficNetworkMap::Read(cIGZIStream& stream) {
    const auto skipReference = [&stream]() {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, false);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();
    };
    const auto clampToUint16 = [](float value) {
        const long rounded = std::lround(value);
        return static_cast<std::uint16_t>(rounded < 0 ? 0 : (rounded > 0xffff ? 0xffff : rounded));
    };
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 3) return false;
    std::uint32_t gridSize = 0;
    stream.GetUint32(gridSize);
    if (gridSize > kMaxStreamedGrid) return false;
    grid_.assign(gridSize, nullptr);
    std::uint32_t tileCount = 0;
    stream.GetUint32(tileCount);
    for (std::uint32_t t = 0; t < tileCount && !stream.GetError(); ++t) {
        std::uint32_t index = 0;
        stream.GetUint32(index);
        auto* tile = new TrafficTileInfo();
        if (index < grid_.size()) {   // appended at the end of the cell's chain, as the original walks to the last link
            TrafficTileInfo** link = &grid_[index];
            while (*link) link = &(*link)->next;
            *link = tile;
        }
        {   // the network occupant of the tile (the original reads it as 0xa821ef94; the port converts it through the host)
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cIGZSerializable, &raw, false);
            if (raw) {
                auto* object = static_cast<cIGZUnknown*>(raw);
                void* occupant = nullptr;
                if (host_ && object->QueryInterface(0x26bb767e, &occupant) && occupant) {   // cISC4Occupant
                    tile->occupant = host_->AsMapOccupant(static_cast<cISC4Occupant*>(occupant));   // one reference, the tile's
                    static_cast<cISC4Occupant*>(occupant)->Release();
                }
                object->Release();
            }
        }
        std::uint32_t lanes = 0;
        stream.GetUint32(lanes);
        if (lanes > kMaxStreamedList) return false;
        tile->laneTicks.resize(lanes);
        for (std::uint32_t lane = 0; lane < lanes; ++lane) {
            std::uint32_t unused = 0, entries = 0;
            stream.GetUint32(unused);
            stream.GetUint32(entries);
            if (entries > kMaxStreamedList) return false;
            for (std::uint32_t e = 0; e < entries; ++e) {
                TrafficLaneTick tick;
                float laneTick = 0.0f;
                stream.GetUint32(tick.key);
                stream.GetFloat32(laneTick);
                tick.laneTick = static_cast<std::uint32_t>(laneTick);
                tile->laneTicks[lane].push_back(tick);
            }
        }
        std::uint32_t automata = 0;
        stream.GetUint32(automata);
        if (automata > kMaxStreamedList) return false;
        for (std::uint32_t a = 0; a < automata; ++a) skipReference();   // the traffic automata on the tile
        bool unused = false;
        stream >> unused;
        stream.GetUint32(tile->roadMask);
        stream.GetUint32(tile->occupancyMask);
        stream.GetUint32(tile->buildingMask);
        for (int i = 0; i < 4; ++i) {
            float edge = 0.0f;
            stream.GetFloat32(edge);
            tile->edgeOccupancy[i] = clampToUint16(edge);
            for (int j = 0; j < 4; ++j) {
                if (version < 4) {
                    float value = 0.0f;
                    stream.GetFloat32(value);
                    tile->distribution[i][j] = clampToUint16(value);
                } else {
                    stream.GetUint16(tile->distribution[i][j]);
                }
            }
        }
        if (version < 4) {
            float discarded = 0.0f;
            stream.GetFloat32(discarded);
        } else {
            std::uint16_t discarded = 0;
            stream.GetUint16(discarded);
        }
        if (version > 4) {
            std::uint32_t frequencies = 0;
            stream.GetUint32(frequencies);
            if (frequencies > kMaxStreamedList) return false;
            tile->frequencies.reserve(frequencies);
            for (std::uint32_t f = 0; f < frequencies; ++f) {
                TrafficFrequency entry;
                float count = 0.0f;
                std::uint16_t perSecond = 0;
                stream.GetUint32(entry.key);
                stream.GetFloat32(count);
                stream.GetUint16(perSecond);
                entry.automataCount = static_cast<std::uint32_t>(count);
                entry.autosPerSecond = perSecond;
                tile->frequencies.push_back(entry);
            }
        }
    }
    // the intersections: occupant, (version 7) dirty flag and automata, then the four approaches
    std::uint32_t count = 0;
    stream.GetUint32(count);
    for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) {
        skipReference();
        if (version >= 7) {
            bool dirty = false;
            stream >> dirty;
            std::uint32_t automata = 0;
            stream.GetUint32(automata);
            if (automata > kMaxStreamedList) return false;
            for (std::uint32_t a = 0; a < automata; ++a) skipReference();
        }
        for (int approach = 0; approach < 4; ++approach) {
            std::uint32_t type = 0;
            bool green = false;
            float stateDuration = 0, otherDuration = 0, timeLeft = 0;
            stream.GetUint32(type);
            stream >> green;
            stream.GetFloat32(stateDuration);
            stream.GetFloat32(otherDuration);
            stream.GetFloat32(timeLeft);
        }
    }
    // the unusable roads: version 1 is a plain list of occupants, later versions a list of cells with their occupants
    stream.GetUint32(count);
    if (version < 2) {
        for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) skipReference();
    } else {
        for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) {
            std::uint32_t key = 0, occupants = 0;
            stream.GetUint32(key);
            stream.GetUint32(occupants);
            if (occupants > kMaxStreamedList) return false;
            for (std::uint32_t o = 0; o < occupants; ++o) skipReference();
        }
    }
    stream.GetSint32(visible_.x0);
    stream.GetSint32(visible_.z0);
    stream.GetSint32(visible_.x1);
    stream.GetSint32(visible_.z1);
    float phase = 0.0f;
    stream.GetFloat32(phase);
    stoplightPhaseA_ = phase;
    bool group = false;
    stream >> group;
    stoplightGroupA_ = group;
    stream.GetFloat32(phase);
    stoplightPhaseB_ = phase;
    stream >> group;
    stoplightGroupB_ = group;
    if (version > 5) {   // the train junctions: occupant, then four key lists with their cursor
        stream.GetUint32(count);
        for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) {
            skipReference();
            for (int type = 0; type < 4; ++type) {
                std::uint32_t keys = 0;
                stream.GetUint32(keys);
                if (keys > kMaxStreamedList) return false;
                for (std::uint32_t k = 0; k < keys; ++k) { std::uint32_t key = 0; stream.GetUint32(key); }
                std::uint16_t cursor = 0;
                stream.GetUint16(cursor);
            }
        }
    }
    return !stream.GetError();
}

}  // namespace sc4
