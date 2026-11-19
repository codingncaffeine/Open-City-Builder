// cSTESurfaceWater — see sc4/SurfaceWater.h.
#include <cstdio>
#include <cstdlib>
#include "sc4/SurfaceWater.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <set>

#include "platform/Unwired.h"
#include "sc4/AutoHelpers.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
// The directions DoMessage looks at around a built network tile: bit i of the tile's connection mask names the cell
// (x + kDeltaX[i], z + kDeltaZ[i]) (the original's UpdateNeighborConnections()::outZ and DoMessage::deltaZ tables, 0x100a29b70 / 0x100a29ac0).
constexpr int kDeltaX[4] = {-1, 0, 1, 0};
constexpr int kDeltaZ[4] = {0, -1, 0, 1};
constexpr std::uint32_t kNetworkOccupantFlagBridge = 0x40000;
constexpr std::uint32_t kNetworkLookupMask = 0x1f4f;
}  // namespace

cSTESurfaceWater::~cSTESurfaceWater() {
    if (bodies_) { bodies_->Release(); bodies_ = nullptr; }
}

bool cSTESurfaceWater::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISTESurfaceWater) *out = static_cast<cIGZSerializable*>(this);
    // (port only) the original answers neither this nor any other serializable id; the port's component factory asks for it first.
    else if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x10072e264: the last reference shuts the object down and deletes it.
std::uint32_t cSTESurfaceWater::Release() {
    if (refCount_ > 1) return --refCount_;
    Shutdown();
    delete this;
    return 0;
}

void cSTESurfaceWater::SyncTerrain() {
    if (!terrain_) return;
    std::vector<float>& depth = terrain_->WaterDepth();
    std::vector<float>& sediment = terrain_->Sediment();
    if (depth.size() != info_.size()) depth.assign(info_.size(), 0.0f);
    if (sediment.size() != info_.size()) sediment.assign(info_.size(), 0.0f);
    for (std::size_t i = 0; i < info_.size(); ++i) {
        depth[i] = info_[i].depth > 0.0f ? info_[i].depth : 0.0f;     // the terrain model keeps land at zero
        sediment[i] = info_[i].sediment;
    }
}

// 0x10072e312.
bool cSTESurfaceWater::Init() {
    if (initialized_) return true;
    terrain_ = host_ ? host_->Terrain() : nullptr;
    if (!terrain_) return false;
    const std::uint32_t vertexCount = terrain_->VertexCount();
    if (std::getenv("SC4_WATERDEBUG")) std::fprintf(stderr, "water %p: Init, terrain %p, %u vertices\n", static_cast<void*>(this), static_cast<void*>(terrain_), vertexCount);
    info_.resize(vertexCount);
    scratchFloats_.resize(vertexCount, 0.0f);
    dirty_ = TerrainRect{0, 0, static_cast<std::int32_t>(terrain_->VertexCountX()) - 1, static_cast<std::int32_t>(terrain_->VertexCountZ()) - 1};
    perVertex_ = false;
    channel_ = 0;
    if (!bodies_) {
        bodies_ = new cSC4SimGrid<std::uint16_t>();
        bodies_->AddRef();
        bodies_->Init();
        bodies_->SetTractSize(1);
        bodies_->SetInstanceID(kBodiesOfWaterGridInstance);
    }
    host_->AddNotifications(*this);
    host_->RegisterTicks(*this);
    ResetWaterToSeaLevel(TerrainRect{0, 0, static_cast<std::int32_t>(terrain_->VertexCountX()) - 1, static_cast<std::int32_t>(terrain_->VertexCountZ()) - 1});
    SyncTerrain();
    initialized_ = true;
    return true;
}

// 0x10072e6d8.
bool cSTESurfaceWater::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (host_) {
        host_->UnregisterTicks(*this);
        host_->RemoveNotifications(*this);
    }
    info_.clear();
    terrain_ = nullptr;
    if (bodies_) {
        bodies_->Shutdown();
        bodies_->Release();
        bodies_ = nullptr;
    }
    flipStrength_.clear();
    return true;
}

// 0x10072e648: depth = sea level - altitude (negative on land), the flow channels and the sediment cleared.
void cSTESurfaceWater::ResetWaterToSeaLevel(const TerrainRect& rect) {
    if (!terrain_) return;
    const std::int32_t width = static_cast<std::int32_t>(terrain_->VertexCountX());
    for (std::int32_t z = rect.top; z <= rect.bottom; ++z) {
        for (std::int32_t x = rect.left; x <= rect.right; ++x) {
            const std::size_t index = static_cast<std::size_t>(width * z + x);
            tWaterInfo& info = info_[index];
            info.depth = terrain_->GetSeaLevel() - terrain_->Vertices()[index].altitude;
            info.flow[0] = 0.0f;
            info.flow[1] = 0.0f;
            info.flow[2] = 0.0f;
            info.sediment = 0.0f;
        }
    }
}

// 0x10072f6dc.
float cSTESurfaceWater::GetWaterDepthAtVertex(int vertex) const { return info_[static_cast<std::size_t>(vertex)].depth; }

// 0x10072f6f2: the shallowest of the four corners of the cell (min with the original's compare order), never below zero.
float cSTESurfaceWater::GetWaterDepthForCell(int x, int z) const {
    const std::int32_t width = static_cast<std::int32_t>(terrain_->VertexCountX());
    const std::size_t index = static_cast<std::size_t>(terrain_->GetVertIndex(x, z));
    const float a = info_[index].depth, b = info_[index + 1].depth;
    const float lower = (a <= b || std::isnan(a) || std::isnan(b)) ? a : b;                 // ucomiss + cmovbe
    const float corner = info_[index + static_cast<std::size_t>(width) + 1].depth;
    const float second = corner < lower ? corner : lower;                                    // minss
    const float below = info_[index + static_cast<std::size_t>(width)].depth;
    const float minimum = below < second ? below : second;                                  // minss
    return minimum > 0.0f ? minimum : 0.0f;                                                  // maxss with zero
}

// 0x10072f76e.
bool cSTESurfaceWater::HasDraftClearance(int x, int z) const { return GetWaterDepthForCell(x, z) > kDraftClearanceDepth; }

// 0x10072f784.
bool cSTESurfaceWater::IsNavigable(const cS3DVector3& position, int radius, const void* lot) const {
    int cellX = 0, cellZ = 0;
    host_->PositionToCell(position.x, position.z, cellX, cellZ);
    return IsNavigable(cellX, cellZ, radius, lot);
}

// 0x10072f7ec: every cell of the spiral around (x, z) must be inside the city and deep enough; with a lot given no other lot may
// stand there; a bridge on the cell decides alone (it must clear the water by 20); a network tile of the occupant group on the
// cell blocks it.
bool cSTESurfaceWater::IsNavigable(int x, int z, int radius, const void* lot) const {
    SpiralIterator spiral;
    spiral.x = x; spiral.z = z;
    spiral.targetX = x + 1; spiral.targetZ = z;
    spiral.minX = x - radius; spiral.minZ = z - radius;
    spiral.maxX = radius + x; spiral.maxZ = radius + z;
    if (spiral.minX > spiral.maxX) spiral.maxX = spiral.minX;
    if (spiral.maxZ < spiral.minZ) spiral.maxZ = spiral.minZ;
    spiral.mode = 1; spiral.direction = 1; spiral.state = 0; spiral.onZ = 1; spiral.edgesHit = 0; spiral.turns = 0;
    int cellX = 0, cellZ = 0;
    if (!spiral.GetNext(cellX, cellZ)) return true;
    do {
        if (!host_->CellInCity(cellX, cellZ)) return false;
        if (!HasDraftClearance(cellX, cellZ)) return false;
        if (lot != nullptr) {
            const void* other = host_->LotAtCell(cellX, cellZ);
            if (other != nullptr && other != lot) return false;
        }
        IWaterNetworkOccupant* network = host_->NetworkOccupantAt(cellX, cellZ, kNetworkLookupMask, true);
        if (network != nullptr && network->HasNetworkFlag(kNetworkOccupantFlagBridge)) {
            float height = 0.0f;
            if (!network->BridgeHeight(height)) return false;
            const std::size_t index = static_cast<std::size_t>(terrain_->GetVertIndex(cellX, cellZ));
            return kBridgeClearance <= height - (terrain_->Vertices()[index].altitude + info_[index].depth);
        }
        std::vector<IWaterCellOccupant*> occupants;
        host_->OccupantsInCell(cellX, cellZ, kWaterOccupantGroup, occupants);
        bool blocked = false;
        for (IWaterCellOccupant* occupant : occupants) {
            int ox = 0, oz = 0;
            occupant->GetCell(ox, oz);
            if (ox == cellX && oz == cellZ) blocked = true;
        }
        if (blocked) return false;
    } while (spiral.GetNext(cellX, cellZ));
    return true;
}

// 0x100730b40: the highest water surface among the vertex and its water-covered neighbours, never below the sea level.
float cSTESurfaceWater::AltitudeAtWhichFloodingWillOccur(int x, int z) const {
    const std::int32_t vertex = terrain_->GetVertIndex(x, z);
    float level = terrain_->GetSeaLevel();
    const float depth = info_[static_cast<std::size_t>(vertex)].depth;
    float surface = terrain_->Vertices()[static_cast<std::size_t>(vertex)].altitude + depth;
    if (surface <= level) surface = level;
    level = depth > 0.0f ? surface : level;
    int vx = 0, vz = 0;
    terrain_->GetVertex(vertex, vx, vz);
    const std::uint8_t neighbors = terrain_->GetNeighbors(vx, vz);
    std::uint8_t bit = 1;
    for (int i = 0; i < 8; ++i, bit = static_cast<std::uint8_t>(bit * 2)) {
        if ((bit & neighbors) == 0) continue;
        const std::size_t other = static_cast<std::size_t>(terrain_->NeighborOffset(i) + vertex);
        const float neighborDepth = info_[other].depth;
        float neighborSurface = neighborDepth + terrain_->Vertices()[other].altitude;
        if (neighborSurface <= level) neighborSurface = level;
        level = neighborDepth > 0.0f ? neighborSurface : level;
    }
    return level;
}

// 0x100730c48.
const cS3DVector3& cSTESurfaceWater::GetWaterVelocity(int x, int z) const {
    return info_[static_cast<std::size_t>(terrain_->GetVertIndex(x, z))].velocity;
}

// 0x100730c72: 0 dry, 3 water at or below the sea level, 2 water above it (lakes).
int cSTESurfaceWater::GetWaterType(int x, int z) const {
    const std::size_t index = static_cast<std::size_t>(terrain_->GetVertIndex(x, z));
    const float depth = info_[index].depth;
    if (!(0.0f < depth)) return 0;
    return 3 - (terrain_->GetSeaLevel() < depth + terrain_->Vertices()[index].altitude ? 1 : 0);
}

// 0x100730cd0: water that has no flow in one of the three velocity components.
bool cSTESurfaceWater::IsFlatWater(int vertex) const {
    const tWaterInfo& info = info_[static_cast<std::size_t>(vertex)];
    if (!(0.0f < info.depth)) return false;
    return info.velocity.x == 0.0f || info.velocity.y == 0.0f || info.velocity.z == 0.0f;
}

// 0x100730d26: the connected cells that are navigable and not yet numbered take the id (breadth first, left, right, up, down).
void cSTESurfaceWater::FloodFillBodyOfWater(int x, int z, std::uint16_t id) {
    std::list<std::pair<int, int>> queue;
    queue.push_back({x, z});
    const int width = bodies_->GetTractCountX() - 1, height = bodies_->GetTractCountZ() - 1;
    for (auto it = queue.begin(); it != queue.end(); ++it) {
        const int cx = it->first, cz = it->second;
        if (bodies_->GetTractValue(cx, cz) != 0) continue;
        if (!IsNavigable(cx, cz, 0, nullptr)) continue;
        bodies_->SetTractValue(cx, cz, id);
        if (cx > 0) queue.push_back({cx - 1, cz});
        if (cx < width) queue.push_back({cx + 1, cz});
        if (cz > 0) queue.push_back({cx, cz - 1});
        if (cz < height) queue.push_back({cx, cz + 1});
    }
}

// 0x10072f50a: clear the map, then number every navigable body from 1.
void cSTESurfaceWater::UpdateBodiesOfWater() {
    const int width = bodies_->GetTractCountX(), height = bodies_->GetTractCountZ();
    for (int x = 0; x < width; ++x)
        for (int z = 0; z < height; ++z) bodies_->SetTractValue(x, z, 0);
    std::uint16_t next = 0;
    for (int x = 0; x < width; ++x) {
        for (int z = 0; z < height; ++z) {
            if (bodies_->GetTractValue(x, z) == 0 && IsNavigable(x, z, 0, nullptr)) {
                ++next;
                FloodFillBodyOfWater(x, z, next);
            }
        }
    }
}

// The directions of the flood step: neighbour i is the vertex at (dx, dz) cells away, in the order of the terrain's neighbour table
// (kFloodDirection @ 0x100a29ae0: x, y, z per direction, y unused).
constexpr float kFloodDirectionX[8] = {-16.0f, -16.0f, 0.0f, 16.0f, 16.0f, 16.0f, 0.0f, -16.0f};
constexpr float kFloodDirectionZ[8] = {0.0f, -16.0f, -16.0f, -16.0f, 0.0f, 16.0f, 16.0f, 16.0f};
constexpr float kSedimentDepositSpeed = 0.1f;          // 0x100a16204: flow slower than this deposits sediment
constexpr float kMaxSedimentToDeposit = 0.1f;          // 0x100a29ad0
constexpr float kFlipDecay = 0.9f;                     // 0x100a1818c
constexpr float kMinimumDepth = 0.001f;                // 0x100a16da0
constexpr float kShallowDepthCutoff = 0.01f;           // 0x100a15928

// 0x10072fb5c. Each cell of the dirty rect that holds water (in the active depth buffer) shares it with the neighbours whose surface lies
// lower: the shares are proportional to the drop, the water carries its sediment along, the flow vector (accumulated drop-weighted
// directions plus a vertical part) is stored as the vertex velocity, and the cell asks the triangulation to follow the flow. The result
// goes to the other depth buffer; the buffers swap, and every other swap the two buffers are averaged into the visible depth.
void cSTESurfaceWater::UpdateFloodWaters() {
    const std::uint32_t channel = channel_;
    const std::uint32_t other = 1 - channel;
    const std::int32_t cellsX = static_cast<std::int32_t>(terrain_->CellCountX());
    const std::int32_t cellsZ = static_cast<std::int32_t>(terrain_->CellCountZ());
    if (flipStrength_.empty()) flipStrength_.assign(static_cast<std::size_t>(cellsZ * cellsX), 0.0f);
    const TerrainRect rect = dirty_;
    const auto vertexOf = [&](std::int32_t x, std::int32_t z) { return static_cast<std::size_t>(terrain_->GetVertIndex(x, z)); };
    auto& vertices = terrain_->Vertices();

    // pass 1: the other buffer starts as the spring, or as an empty (signed-zero) copy where there is water
    for (std::int32_t x = rect.left; x <= rect.right; ++x) {
        for (std::int32_t z = rect.top; z <= rect.bottom; ++z) {
            tWaterInfo& info = info_[vertexOf(x, z)];
            const float spring = info.flow[0];
            float value = spring;
            if (!(spring > 0.0f)) value = (0.0f < info.flow[1 + channel]) ? -0.0f : 0.0f;
            info.flow[1 + other] = value;
        }
    }

    // pass 2: the flow
    for (std::int32_t x = rect.left; x <= rect.right; ++x) {
        for (std::int32_t z = rect.top; z <= rect.bottom; ++z) {
            const std::size_t index = vertexOf(x, z);
            const float depth = info_[index].flow[1 + channel];
            if (!(depth > 0.0f)) continue;
            const float spring = info_[index].flow[0];
            const float level = vertices[index].altitude + depth;
            const std::uint8_t neighbors = terrain_->GetNeighbors(x, z);
            float drop[8];
            float total = depth;
            float sumX = 0.0f, sumZ = 0.0f, negSquares = 0.0f, largest = 0.0f;
            int largestIndex = -1;
            std::uint8_t bit = 1;
            for (int i = 0; i < 8; ++i, bit = static_cast<std::uint8_t>(bit * 2)) {
                float d;
                if ((bit & neighbors) == 0) {
                    if (!(spring <= 0.0f)) { drop[i] = 0.0f; continue; }     // off the map: the water drains unless it is a spring
                    d = depth;
                } else {
                    const std::size_t n = static_cast<std::size_t>(terrain_->NeighborOffset(i)) + index;
                    d = level - (info_[n].flow[1 + channel] + vertices[n].altitude);
                    if (0.0f > d) { drop[i] = 0.0f; continue; }
                }
                drop[i] = d;
                if (d > 0.0f) {
                    total += d;
                    if (d > largest) largestIndex = i;
                    largest = d > largest ? d : largest;
                    sumX += kFloodDirectionX[i] * d;
                    sumZ += kFloodDirectionZ[i] * d;
                    negSquares -= d * d;
                }
            }
            float velocityX, velocityY, velocityZ;
            if (sumX != 0.0f || std::isnan(sumX) || (negSquares < 0.0f && (sumZ != 0.0f || std::isnan(sumZ)))) {
                const float squares = (negSquares * negSquares + sumX * sumX) + sumZ * sumZ;
                velocityX = sumX; velocityY = negSquares; velocityZ = sumZ;
                if (squares > 0.0f) {
                    const float horizontal = std::sqrt(sumZ * sumZ + sumX * sumX);
                    const float scale = std::sqrt(-negSquares / horizontal) / std::sqrt(squares);
                    velocityX = sumX * scale; velocityY = negSquares * scale; velocityZ = sumZ * scale;
                }
            } else {
                velocityX = 0.0f; velocityY = 0.0f; velocityZ = 0.0f;
            }
            info_[index].velocity = cS3DVector3{velocityX, velocityY, velocityZ};

            // the cell the flow points into asks its triangulation to follow
            if (velocityX != 0.0f || std::isnan(velocityX) || velocityZ != 0.0f || std::isnan(velocityZ)) {
                const std::uint32_t flipX = static_cast<std::uint32_t>(x) - (0.0f > velocityX ? 1u : 0u);
                const std::uint32_t flipZ = static_cast<std::uint32_t>(z) - (0.0f > velocityZ ? 1u : 0u);
                if (flipX < terrain_->CellCountX() && flipZ < terrain_->CellCountZ()) {
                    const float strength = (1.0f - depth / total) * depth;
                    const std::size_t cell = static_cast<std::size_t>(static_cast<std::int32_t>(terrain_->CellCountX() * flipZ + flipX));
                    const float current = flipStrength_[cell];
                    if (strength > current) {
                        flipStrength_[cell] = strength;
                        terrain_->FlipInternalTriangulation(flipX, flipZ, 0.0f > velocityX * velocityZ);
                    } else {
                        flipStrength_[cell] = current * kFlipDecay;
                    }
                }
            }

            // a vertex below the surface keeps only its steepest drop
            if (0.0f > depth) {
                if (largestIndex > 0) {
                    const float keep = drop[largestIndex];
                    for (int i = 0; i < 8; ++i)
                        if (i != largestIndex) drop[i] = 0.0f;
                    total = keep + depth;
                }
            }

            // hand the water and its sediment to the lower neighbours
            const float shareOfWater = depth / total;
            const float shareOfSediment = info_[index].sediment / total;
            bit = 1;
            for (int i = 0; i < 8; ++i, bit = static_cast<std::uint8_t>(bit * 2)) {
                if ((bit & neighbors) == 0 || !(drop[i] > 0.0f)) continue;
                tWaterInfo& target = info_[static_cast<std::size_t>(terrain_->NeighborOffset(i)) + index];
                target.flow[1 + other] = shareOfWater * drop[i] + target.flow[1 + other];
                const float carried = drop[i] * shareOfSediment;
                info_[index].sediment = info_[index].sediment - carried;
                target.sediment = carried + target.sediment;
            }
            info_[index].flow[1 + other] = depth * shareOfWater + info_[index].flow[1 + other];

            // slow water drops its sediment onto the ground
            if (info_[index].sediment > 0.0f) {
                const tWaterInfo& info = info_[index];
                const float speed = std::sqrt(info.velocity.z * info.velocity.z + (info.velocity.x * info.velocity.x + info.velocity.y * info.velocity.y));
                if (!(kSedimentDepositSpeed < speed) && !std::isnan(speed)) {
                    const float deposit = info_[index].sediment > kSedimentDepositSpeed ? kMaxSedimentToDeposit : info_[index].sediment;
                    vertices[index].altitude = vertices[index].altitude + deposit;
                    info_[index].sediment = info_[index].sediment - deposit;
                }
            }
        }
    }

    // pass 3: springs keep their level, a dry cell empties, and the sea level floors the water
    for (std::int32_t x = rect.left; x <= rect.right; ++x) {
        for (std::int32_t z = rect.top; z <= rect.bottom; ++z) {
            const std::size_t index = vertexOf(x, z);
            const float altitude = vertices[index].altitude;
            const float spring = info_[index].flow[0];
            float& buffer = info_[index].flow[1 + other];
            if (spring > 0.0f) {
                if (spring > buffer) buffer = spring;
            } else if (kMinimumDepth > buffer) {
                buffer = 0.0f;
            }
            if (terrain_->GetSeaLevel() > altitude) buffer = terrain_->GetSeaLevel() - altitude;
        }
    }

    channel_ = other;
    if (channel_ == 0) {                               // back on buffer 0: publish the average of the two buffers as the depth
        for (std::int32_t x = rect.left; x <= rect.right; ++x) {
            for (std::int32_t z = rect.top; z <= rect.bottom; ++z) {
                tWaterInfo& info = info_[vertexOf(x, z)];
                const float mean = (info.flow[1] + info.flow[2]) * 0.5f;
                float depth = 0.0f;
                if (!(kShallowDepthCutoff > mean)) depth = (1.0f > mean) ? 1.0f : mean;
                info.depth = depth;
            }
        }
    }
    SyncTerrain();
}

// 0x1007303b0: downhill from the vertex until the ground levels out or water begins.
int cSTESurfaceWater::FindLocalMinimumOrWater(int vertex) const {
    if (!(info_[static_cast<std::size_t>(vertex)].depth <= 0.0f)) return vertex;
    const auto& vertices = terrain_->Vertices();
    do {
        int x = 0, z = 0;
        terrain_->GetVertex(vertex, x, z);
        const std::uint8_t neighbors = terrain_->GetNeighbors(x, z);
        std::uint32_t lowest = static_cast<std::uint32_t>(vertex);
        std::uint8_t bit = 1;
        for (int i = 0; i < 8; ++i, bit = static_cast<std::uint8_t>(bit * 2)) {
            if ((bit & neighbors) == 0) continue;
            const std::uint64_t candidate = static_cast<std::uint64_t>(static_cast<std::int64_t>(vertex) + terrain_->NeighborOffset(i));
            const float best = vertices[lowest].altitude, other = vertices[static_cast<std::size_t>(candidate)].altitude;
            if (!(best < other || best == other)) lowest = static_cast<std::uint32_t>(candidate);
        }
        if (lowest == static_cast<std::uint32_t>(vertex)) break;
        vertex = static_cast<int>(lowest);
    } while (info_[static_cast<std::size_t>(vertex)].depth <= 0.0f);
    return vertex;
}

// 0x100730492. Breadth-first over the vertices at or below `limit`: they join the basin; vertices above it join the rim; a higher
// neighbour that is itself a crest (the next vertex beyond it, in the same direction, is lower still) joins the spill list. The seed is
// not in the visited set, so a neighbour that leads back to it adds it again (kept from the original).
void cSTESurfaceWater::GetBasinVertices(int seed, float level, float limit, std::list<int>& basin, std::list<int>& rim, std::list<int>& spill) const {
    (void)level;
    basin.clear();
    spill.clear();
    std::set<int> visited;
    const auto& vertices = terrain_->Vertices();
    if (vertices[static_cast<std::size_t>(seed)].altitude <= limit) basin.push_back(seed);
    for (auto it = basin.begin(); it != basin.end(); ++it) {
        const int vertex = *it;
        const float height = vertices[static_cast<std::size_t>(vertex)].altitude;
        if (!(height <= limit)) {
            rim.push_back(vertex);
            continue;
        }
        int x = 0, z = 0;
        terrain_->GetVertex(vertex, x, z);
        const std::uint8_t neighbors = terrain_->GetNeighbors(x, z);
        std::uint32_t bit = 1;
        for (int i = 0; i < 8; ++i, bit = (bit * 2) & 0x1fe) {
            if ((neighbors & bit) == 0) continue;
            const int neighbor = terrain_->NeighborOffset(i) + vertex;
            const float neighborHeight = vertices[static_cast<std::size_t>(neighbor)].altitude;
            if (height < neighborHeight) {
                int nx = 0, nz = 0;
                terrain_->GetVertex(neighbor, nx, nz);
                const std::uint8_t beyond = terrain_->GetNeighbors(nx, nz);
                if (((beyond >> i) & 1) != 0) {
                    const std::size_t next = static_cast<std::size_t>(neighbor + terrain_->NeighborOffset(i));
                    const float nextHeight = vertices[next].altitude;
                    if (nextHeight <= neighborHeight && neighborHeight != nextHeight) {     // the neighbour is a crest
                        if (std::find(spill.begin(), spill.end(), neighbor) == spill.end()) spill.push_back(neighbor);
                        continue;
                    }
                }
            }
            if (visited.insert(neighbor).second) basin.push_back(neighbor);
        }
    }
}

// 0x100730820: in the per-vertex mode the brush adds the rain to the vertex (AddRain); otherwise the static fill below runs.
bool cSTESurfaceWater::Rain(int vertex, TerrainBrushContext& context, TerrainRect& rect, float amount) {
    if (!perVertex_) {
        RainStatic(vertex, context, rect, amount);
        return false;
    }
    return true;
}

// 0x100730882: rain on a vertex fills the basin it drains into up to the water level plus the rain, within the part of the basin the
// brush texture allows; the vertices that changed are returned as a rect.
void cSTESurfaceWater::RainStatic(int vertex, TerrainBrushContext& context, TerrainRect& rect, float amount) {
    rect.left = INT_MAX; rect.top = INT_MAX; rect.right = INT_MIN; rect.bottom = INT_MIN;
    const int start = FindLocalMinimumOrWater(vertex);
    int x = 0, z = 0;
    terrain_->GetVertex(start, x, z);
    if (!terrain_->GetTextureValue(x, z, context) || context.textureValue == context.component->textureValue) return;
    auto& vertices = terrain_->Vertices();
    const float depth = info_[static_cast<std::size_t>(start)].depth;
    float level = 0.0f;
    if (0.0f <= depth) level = depth;
    level = level + vertices[static_cast<std::size_t>(start)].altitude;
    const float target = amount + level;
    std::list<int> basin, rim, spill;
    GetBasinVertices(start, level, target, basin, rim, spill);
    float fill = target;
    for (int edge : spill) {
        int ex = 0, ez = 0;
        terrain_->GetVertex(edge, ex, ez);
        if (!terrain_->GetTextureValue(ex, ez, context) || context.textureValue == context.component->textureValue) {
            GetBasinVertices(start, level, level, basin, rim, spill);     // a spill vertex the brush does not cover: fill only to the old level
            fill = level;
            break;
        }
    }
    for (int member : basin) {
        const std::size_t m = static_cast<std::size_t>(member);
        if (vertices[m].altitude + info_[m].depth < fill) {
            float newDepth = fill - vertices[m].altitude;
            info_[m].depth = newDepth;
            if (cSTETerrain::MaxAllowedHeight() < newDepth) info_[m].depth = cSTETerrain::MaxAllowedHeight();
            int mx = 0, mz = 0;
            terrain_->GetVertex(member, mx, mz);
            if (mx < rect.left) rect.left = mx;
            if (rect.right < mx) rect.right = mx;
            if (mz < rect.top) rect.top = mz;
            if (rect.bottom < mz) rect.bottom = mz;
        }
    }
    SyncTerrain();
}

// 0x10072efca. Along each of the four edges the flat water is cut into runs; the connection to the neighbouring city is kept where the
// water is navigable, removed where it is not, and a new one is placed at the deepest spot of a run of more than two cells that has none,
// moved off shallow water (< 30) near the ends of the run. The edge walk uses the vertex count of the x axis for all four edges.
void cSTESurfaceWater::UpdateNeighborConnections() {
    if (std::getenv("SC4_WATERDEBUG")) std::fprintf(stderr, "water %p: UpdateNeighborConnections, terrain %p, %zu infos, initialized %d\n", static_cast<void*>(this), static_cast<void*>(terrain_), info_.size(), int(initialized_));
    static const int kStepX[4] = {1, 0, 1, 0}, kStepZ[4] = {0, 1, 0, 1};
    static const int kOutX[4] = {0, 1, 0, -1}, kOutZ[4] = {-1, 0, 1, 0};
    const std::uint32_t vertsX = terrain_->VertexCountX();
    const std::int32_t lastX = static_cast<std::int32_t>(terrain_->CellCountX()) - 1, lastZ = static_cast<std::int32_t>(terrain_->CellCountZ()) - 1;   // (cell counts - 1: slots 0x38 / 0x40)
    const std::int32_t startX[4] = {0, lastX, 0, 0}, startZ[4] = {0, 0, lastZ, 0};
    const auto indexOf = [&](std::int32_t x, std::int32_t z) { return terrain_->GetVertIndex(x, z); };
    const auto shallow = [&](std::int32_t x, std::int32_t z) { return info_[static_cast<std::size_t>(indexOf(x, z))].depth < kShallowWater; };
    for (int e = 0; e < 4; ++e) {
        const std::int32_t x0 = startX[e], z0 = startZ[e];
        const std::int32_t dx = kStepX[e], dz = kStepZ[e];
        bool runFlat = IsFlatWater(indexOf(x0, z0));
        if (!(1 < vertsX)) continue;
        float bestDepth = 0.0f;
        bool placed = false;
        std::int32_t best = 0;
        std::uint32_t px = static_cast<std::uint32_t>(x0), pz = static_cast<std::uint32_t>(z0);
        std::int32_t last = 0, runStart = 0;
        bool previousFlat = runFlat;
        for (std::uint32_t i = 1; i < vertsX; ++i) {
            px += static_cast<std::uint32_t>(dx);
            pz += static_cast<std::uint32_t>(dz);
            const int vertex = indexOf(static_cast<std::int32_t>(px), static_cast<std::int32_t>(pz));
            const bool flat = IsFlatWater(vertex);
            const bool wasFlat = runFlat;
            std::int32_t nextBest = best, nextRunStart = runStart;
            bool nextPlaced = placed, nextPrevious = previousFlat;
            bool runEnds = true;
            if (flat == wasFlat) {
                if (flat && px < terrain_->CellCountX() && pz < terrain_->CellCountZ()) {
                    const float depth = info_[static_cast<std::size_t>(vertex)].depth;
                    if (bestDepth < depth) { best = static_cast<std::int32_t>(i); bestDepth = depth; }
                    const int outX = kOutX[e] + static_cast<int>(px), outZ = kOutZ[e] + static_cast<int>(pz);
                    if (!IsNavigable(static_cast<int>(px), static_cast<int>(pz), 0, nullptr)) host_->NeighborDeleteConnection(kWaterConnectionType, outX, outZ);
                    else if (!placed) placed = host_->NeighborHasConnection(kWaterConnectionType, outX, outZ);
                }
                nextBest = best; nextPlaced = placed;
                runEnds = (i & 0x3f) == 0;
            }
            const std::int32_t lastSeen = (flat == wasFlat) ? static_cast<std::int32_t>(i) : last;   // the same-state path has already moved `last` on
            if (runEnds) {
                nextPlaced = false;
                bestDepth = 0.0f;
                nextBest = static_cast<std::int32_t>(i);
                nextRunStart = static_cast<std::int32_t>(i);
                runFlat = flat;
                nextPrevious = wasFlat;
                if (2 < lastSeen - runStart && wasFlat && !placed) {
                    const std::int32_t mid = (lastSeen + runStart) / 2;
                    std::int32_t stepX = best * dx, stepZ = best * dz;
                    std::int32_t cx = stepX + x0, cz = stepZ + z0;
                    if (previousFlat && best < mid && best - runStart < 3) {
                        std::uint32_t k = static_cast<std::uint32_t>(best) + 1;
                        std::uint32_t walkX = static_cast<std::uint32_t>(x0), walkZ = static_cast<std::uint32_t>(z0);
                        std::uint32_t counter = k;
                        bool moved = false;
                        for (;;) {
                            best = static_cast<std::int32_t>(counter);
                            const std::int32_t vx = static_cast<std::int32_t>(dx * k + walkX), vz = static_cast<std::int32_t>(dz * k + walkZ);
                            if (shallow(vx, vz)) {
                                cz = stepZ + static_cast<std::int32_t>(walkZ);
                                cx = stepX + static_cast<std::int32_t>(walkX);
                                best = best - 1;
                                moved = true;
                                break;
                            }
                            walkZ += static_cast<std::uint32_t>(dz);
                            walkX += static_cast<std::uint32_t>(dx);
                            if (!(best < mid && (counter = static_cast<std::uint32_t>(best) + 1, best - runStart < 3))) break;
                        }
                        if (!moved) {
                            cz = stepZ + static_cast<std::int32_t>(walkZ);
                            cx = stepX + static_cast<std::int32_t>(walkX);
                        }
                    }
                    if (flat && mid < best && lastSeen - best < 3) {
                        std::int32_t budget = (lastSeen + 1) - best;
                        for (;;) {
                            best = best - 1;
                            if (shallow(cx - dx, cz - dz)) break;
                            cx -= dx;
                            cz -= dz;
                            if (best <= mid) break;
                            const bool more = 2 < budget;
                            ++budget;
                            if (more) break;
                        }
                    }
                    host_->NeighborCreateConnection(kWaterConnectionType, cx + kOutX[e], cz + kOutZ[e]);
                    bestDepth = 0.0f;
                    nextPlaced = false;
                }
            }
            placed = nextPlaced;
            best = nextBest;
            last = static_cast<std::int32_t>(i);
            runStart = nextRunStart;
            previousFlat = nextPrevious;
        }
    }
}

// 0x10072f5f0: one flood step, then the terrain redisplays the whole grid (water only, no altitude adjustment, flags 3).
void cSTESurfaceWater::AnimateWater() {
    UpdateFloodWaters();
    terrain_->RedisplayTerrain(false, true, TerrainRect{0, 0, static_cast<std::int32_t>(terrain_->VertexCountX()) - 1, static_cast<std::int32_t>(terrain_->VertexCountZ()) - 1}, 3);
}

// 0x10072f696: a finished scrub rebuilds the connections and the bodies once the city is up.
void cSTESurfaceWater::EndBrushScrub(std::uint32_t brushType, std::uint32_t arg) {
    (void)brushType; (void)arg;
    if (host_ && host_->CityInitialized()) {
        UpdateNeighborConnections();
        UpdateBodiesOfWater();
    }
}

// 0x10072ec5a.
bool cSTESurfaceWater::DoMessage(cIGZMessage2* base) {
    auto* message = static_cast<cIGZMessage2Standard*>(base);
    const std::uint32_t type = message->GetType();
    if (type == kWaterMsgRebuildBodies) {
        if (bodiesDirty_) {
            UpdateBodiesOfWater();
            bodiesDirty_ = false;
        }
    } else if (type == kWaterMsgCityCell4 || (type == kWaterMsgCityCell1 && host_->CityInitialized())) {
        UpdateNeighborConnections();
        UpdateBodiesOfWater();
    } else if (type == kWaterMsgOccupantAdded || type == kWaterMsgOccupantMoved) {
        if (message->GetData3() == kWaterOccupantGroup) {
            auto* object = static_cast<cIGZUnknown*>(message->GetVoid1());
            IWaterCellOccupant* occupant = object ? host_->AsCellOccupant(object) : nullptr;
            if (occupant && !occupant->IsRemoved()) {
                int cellX = 0, cellZ = 0;
                occupant->GetCell(cellX, cellZ);
                if (0.0f < GetWaterDepthForCell(cellX, cellZ)) bodiesDirty_ = true;
            }
        }
    } else if (type == kWaterMsgAnimate) {
        if (perVertex_) AnimateWater();
    } else if (type == kWaterMsgNetworkBuilt) {
        IWaterNetworkOccupant* built = host_->AsNetworkOccupant(message->GetIGZUnknown());
        if (message->GetData1() != 0 && built != nullptr && built->HasNetworkFlag(kNetworkOccupantFlagBridge)) {
            float position[3] = {0, 0, 0};
            built->GetPosition(position);
            int cellX = 0, cellZ = 0;
            host_->PositionToCell(position[0], position[2], cellX, cellZ);
            const std::uint8_t mask = built->ConnectionMask();
            bool joined = false;
            for (int i = 0; i < 4; ++i) {
                if (((mask >> i) & 1) == 0) continue;
                const int nx = kDeltaX[i] + cellX, nz = kDeltaZ[i] + cellZ;
                if ((nx | nz) < 0 || nx >= host_->CitySizeX() - 1 || nz >= host_->CitySizeZ() - 1) continue;
                IWaterNetworkOccupant* other = host_->NetworkOccupantAt(nx, nz, kNetworkLookupMask, true);
                if (other != nullptr && other->HasNetworkFlag(kNetworkOccupantFlagBridge) && !other->IsRemoved()) joined = true;
            }
            if (!joined) UpdateBodiesOfWater();
        }
    }
    return true;
}

// 0x10072e91c: version 1 record of the water: per vertex the depth, the three flow channels, the velocity and the sediment.
bool cSTESurfaceWater::SaveWaterInfo(cIGZPersistDBSegment* segment) const {
    cSC4AutoSaveStream out(segment, kWaterInfoType, kWaterInfoGroup, kWaterInfoInstance, 1);
    if (!out.IsOpen()) return false;
    WriteWaterRecord(*out.GetStream());
    return true;
}

void cSTESurfaceWater::WriteWaterRecord(cIGZOStream& stream) const {
    const std::uint32_t count = terrain_->VertexCount();
    stream.SetUint32(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const tWaterInfo& info = info_[i];
        stream.SetFloat32(info.depth);
        stream.SetFloat32(info.flow[0]);
        stream.SetFloat32(info.flow[1]);
        stream.SetFloat32(info.flow[2]);
        stream << info.velocity;
        stream.SetFloat32(info.sediment);
    }
}

// 0x10072eb28.
bool cSTESurfaceWater::LoadWaterInfo(cIGZPersistDBSegment* segment) {
    cSC4AutoLoadStream in(segment, kWaterInfoType, kWaterInfoGroup, kWaterInfoInstance);
    if (!in.IsOpen() || in.GetVersion() > 1) return false;
    ReadWaterRecord(*in.GetStream());
    return true;
}

void cSTESurfaceWater::ReadWaterRecord(cIGZIStream& stream) {
    std::uint32_t count = 0;
    stream.GetUint32(count);
    info_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        tWaterInfo& info = info_[i];
        stream.GetFloat32(info.depth);
        stream.GetFloat32(info.flow[0]);
        stream.GetFloat32(info.flow[1]);
        stream.GetFloat32(info.flow[2]);
        stream >> info.velocity;
        stream.GetFloat32(info.sediment);
    }
    SyncTerrain();
}

namespace {
cIGZPersistDBSegment* SegmentOfStream(cIGZOStream& stream) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw) || !raw) return nullptr;
    auto* segmentStream = static_cast<cISC4DBSegmentOStream*>(raw);
    cISC4DBSegment* segment = segmentStream->GetSegment();
    cIGZPersistDBSegment* result = segment ? segment->AsIGZPersistDBSegment() : nullptr;
    segmentStream->Release();
    return result;
}
cIGZPersistDBSegment* SegmentOfStream(cIGZIStream& stream) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &raw) || !raw) return nullptr;
    auto* segmentStream = static_cast<cISC4DBSegmentIStream*>(raw);
    cISC4DBSegment* segment = segmentStream->GetSegment();
    cIGZPersistDBSegment* result = segment ? segment->AsIGZPersistDBSegment() : nullptr;
    segmentStream->Release();
    return result;
}
}  // namespace

// 0x10072e86a: version 1, the terrain, then the water record in the stream's segment.
bool cSTESurfaceWater::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    SC4WriteGZSerializable(stream, terrain_);
    return SaveWaterInfo(SegmentOfStream(stream));
}

// 0x10072ea64: versions 0 and 1.
bool cSTESurfaceWater::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 1) return false;
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, 0xc70bd37e, &raw, false);
    terrain_ = static_cast<cSTETerrain*>(raw);
    return LoadWaterInfo(SegmentOfStream(stream));
}

}  // namespace sc4
