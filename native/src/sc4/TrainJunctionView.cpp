#include "sc4/TrainJunctionView.h"

#include <cmath>
#include <cstring>

#include "gz/cGZPersistResourceKey.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "s3d/S3DMath.h"
#include "sc4/DrawContext.h"
#include "sc4/PathInfo.h"

namespace sc4 {

namespace {

ITrainJunctionViewHost* g_host = nullptr;

cS3DVector3 g_cellVectors[4][4];   // kfCellVectors (0x00c19620)

constexpr std::uint32_t kArrowTextureInstance[6] = {0x6be09921, 0x6be09922, 0x6be09923, 0x6be09924, 0x6be09925, 0x6be09926};
constexpr std::uint32_t kLeverModelInstance = 0x8bb70000, kLeverClickedModelInstance = 0x8bb80000;
constexpr float kArrowLength = 12.0f;           // 0x00a706e0
constexpr float kHalf = 0.5f;                   // 0x00a706b4
constexpr float kLiftStart = 0.1f;              // Update: +0x18 = 0x3dcccccd
constexpr float kLiftStep = 0.01f;              // 0x00a706d0 (cSC4Tank::sfBarrelVelocityScalingFactor shares it)
constexpr float kLeverLift = 0.2f;              // 0x00a707b8
constexpr float kMonorailArrowOffset = 3.0f;    // _kfMonorailArrowOffset (0x00abc2e0)
constexpr float kTurnThreshold = 0.25f;         // 0x00a70780 / 0x00a70834
constexpr std::uint32_t kPathTypeMask = 0xf000000, kMonorailPathType = 0x7000000;

bool IsMonorail(std::uint32_t key) { return (key & kPathTypeMask) == kMonorailPathType; }

cS3DVector3 Normalized(cS3DVector3 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (length > 0.0f) v = cS3DVector3{v.x / length, v.y / length, v.z / length};   // port guard: a zero vector stays
    return v;
}

std::uint8_t FromEdge(std::uint32_t key) { return static_cast<std::uint8_t>(key >> 8); }
std::uint8_t ToEdge(std::uint32_t key) { return static_cast<std::uint8_t>(key); }

}  // namespace

ITrainJunctionViewHost* TrainJunctionViewHost() { return g_host; }
void SetTrainJunctionViewHost(ITrainJunctionViewHost* host) { g_host = host; }

const cS3DVector3& SC4TrainJunctionView::CellVector(int from, int to) { return g_cellVectors[from & 3][to & 3]; }

SC4TrainJunctionView::~SC4TrainJunctionView() { Shutdown(); }

bool SC4TrainJunctionView::QueryInterface(std::uint32_t iid, void** out) {
    (void)iid;
    if (out) *out = nullptr;
    return false;
}

std::uint32_t SC4TrainJunctionView::Release() {
    if (refs_ <= 1) {
        delete this;
        return 0;
    }
    return --refs_;
}

// 0x004b56ec: the render state, the cell vector table (each edge midpoint to each other, unit length off the diagonal),
// the six arrow textures and the two lever models.
bool SC4TrainJunctionView::Init() {
    if (initialized_) return true;
    if (g_host && g_host->RendererPresent()) {
        g_host->RendererGetDrawContext();
        state_.PackRenderState(true, true, true, true, true, 1, true, 3, 4, 5, 4, 0.0f, 1, -1);
    }
    initialized_ = true;
    const cS3DVector3 midpoints[4] = {{0.0f, 0.0f, 8.0f}, {8.0f, 0.0f, 0.0f}, {16.0f, 0.0f, 8.0f}, {8.0f, 0.0f, 16.0f}};
    for (int from = 0; from < 4; ++from) {
        for (int to = 0; to < 4; ++to) {
            const cS3DVector3& a = midpoints[from];
            const cS3DVector3& b = midpoints[to];
            cS3DVector3 v{b.x - a.x, b.y - a.y, b.z - a.z};
            g_cellVectors[from][to] = from != to ? Normalized(v) : v;
        }
    }
    if (!g_host) return true;
    for (int slot = 0; slot < 6; ++slot) bindings_[slot] = g_host->CreateTextureBinding(kArrowTextureInstance[slot]);
    s3d::cIS3DModelInstanceAnimated* model = nullptr;
    if (g_host->GetAnimatedModelFromResourceKey(cGZPersistResourceKey{0x5ad0e817, 0xbadb57f1, kLeverModelInstance}, &model)) lever_ = model;
    model = nullptr;
    if (g_host->GetAnimatedModelFromResourceKey(cGZPersistResourceKey{0x5ad0e817, 0xbadb57f1, kLeverClickedModelInstance}, &model))
        leverClicked_ = model;
    return true;
}

// 0x004b3826
void SC4TrainJunctionView::Shutdown() {
    if (!initialized_) return;
    initialized_ = false;
    for (int slot : {2, 3, 0, 1, 4, 5}) {   // the original's release order
        if (bindings_[slot]) {
            bindings_[slot]->Release();
            bindings_[slot] = nullptr;
        }
    }
    if (lever_) {
        lever_->Release();
        lever_ = nullptr;
    }
    if (leverClicked_) {
        leverClicked_->Release();
        leverClicked_ = nullptr;
    }
}

// 0x004b7d54: every junction of every visible cell, for each of the four path types.
bool SC4TrainJunctionView::Update(SC4DrawContext* context) {
    int rect[4] = {0, 0, -1, -1};
    if (!g_host || !g_host->VisibleCellRect(rect)) return true;
    lift_ = kLiftStart;
    std::vector<ITrafficNetworkMapOccupant*> occupants;
    for (int x = rect[0]; x <= rect[2]; ++x) {
        for (int z = rect[1]; z <= rect[3]; ++z) {
            occupants.clear();
            g_host->CellOccupants(x, z, occupants);
            for (ITrafficNetworkMapOccupant* occupant : occupants) {
                std::vector<std::uint32_t> keys;
                for (std::uint8_t pathType = 0; pathType < 4; ++pathType) {
                    std::uint16_t current = 0;
                    const std::size_t count = g_host->TrainJunctionKeys(occupant, pathType, keys, current);
                    if (count) DrawTrainJunction(context, x, z, occupant, count, keys, current);
                }
            }
        }
    }
    return true;
}

// 0x004b780a: the other paths' arrows (alpha 0.7 on the clicked or pointer cell, else 0.4), the switched path's arrow and
// the lever above its midpoint (alpha 0.8, else 0.5), the clicked cell's lever being the other model.
void SC4TrainJunctionView::DrawTrainJunction(SC4DrawContext* context, int cellX, int cellZ, ITrafficNetworkMapOccupant* occupant,
                                             std::size_t count, const std::vector<std::uint32_t>& keys, std::uint16_t current) {
    const cISC4PathInfo* pathInfo = g_host ? g_host->PathInfo(occupant) : nullptr;
    if (!pathInfo || keys.empty() || !context) return;   // port guard: the original reads them unchecked
    cS3DVector3 midpoint{0.0f, 0.0f, 0.0f};
    bool clicked = false;
    float arrowAlpha = 0.4f, leverAlpha = 0.5f;
    if (cellX == mouseDownX_ && cellZ == mouseDownZ_) {
        clicked = true;
        arrowAlpha = 0.7f;
        leverAlpha = 0.8f;   // 0x00a7070c
    } else if (cellX == highlightX_ && cellZ == highlightZ_) {
        arrowAlpha = 0.7f;
        leverAlpha = 0.8f;
    }
    const std::uint32_t first = keys[0];
    const tPath* firstPath = pathInfo->GetPath(first, nullptr);
    pathInfo->GetPathMidpoint(firstPath ? &firstPath->points : nullptr, midpoint);
    const cS3DVector3 facing = CellVector(FromEdge(first), ToEdge(first));
    context->ResetModelViewTransform();
    context->ResetTransparency();
    context->SetRenderState(state_);
    context->SetTransparency(arrowAlpha);
    TrainJunctionVertex quad[4];
    auto drawArrow = [&](std::uint32_t key, bool selected) {
        const tPath* path = pathInfo->GetPath(key, nullptr);
        if (!path) return;
        IPylonTextureBinding* binding = GetPathBitmap(FromEdge(key), ToEdge(key), pathInfo, &path->points, selected);
        if (!binding) return;
        GetArrowBitmapQuad(quad, midpoint, FromEdge(key), ToEdge(key), pathInfo, &path->points, key);
        lift_ += kLiftStep;
        context->SetTexture(static_cast<int>(binding->TextureId()), 0);
        context->DrawPrims(6, 2, 4, quad);
    };
    for (std::size_t i = 0; i < count && i < keys.size(); ++i)
        if (i != current) drawArrow(keys[i], false);
    context->SetTransparency(leverAlpha);
    if (current >= keys.size()) return;   // port guard
    const std::uint32_t selectedKey = keys[current];
    drawArrow(selectedKey, true);
    s3d::cIS3DModelInstanceAnimated* lever = clicked ? leverClicked_ : lever_;
    if (!lever) return;
    lever->AddRef();
    cS3DVector3 position = midpoint;
    position.y += kLeverLift + lift_;
    if (IsMonorail(selectedKey)) position.y += kMonorailArrowOffset;
    cS3DTransform transform;
    nS3D::Matrix3 rotation;
    nS3D::AlignZToDirWithYUp(facing, rotation);
    std::memcpy(transform.rotation, rotation.m, sizeof(rotation.m));
    transform.translation[0] = position.x;
    transform.translation[1] = position.y;
    transform.translation[2] = position.z;
    transform.flags |= 5;
    transform.changeCounter += 2;
    lever->SetTransform(transform);
    lever->SetOpacity(true, leverAlpha);
    lever->Draw(context, false);
    lever->Release();
}

// 0x004b4130
void SC4TrainJunctionView::GetArrowBitmapQuad(TrainJunctionVertex* quad, const cS3DVector3& midpoint, std::uint8_t from, std::uint8_t to,
                                              const cISC4PathInfo* pathInfo, const std::vector<cS3DVector3>* path, std::uint32_t key) {
    (void)pathInfo;
    (void)path;
    const cS3DVector3& v = CellVector(from, to);
    const float dx = v.x * kArrowLength, dz = v.z * kArrowLength;
    const float sideX = -v.z * kArrowLength, sideZ = dx;   // the arrow's width, across the vector
    float y = g_host ? g_host->TerrainAltitude(midpoint.x + dx, midpoint.z + dz) : midpoint.y;
    if (y < midpoint.y) y = midpoint.y;
    y += lift_;
    if (IsMonorail(key)) y += kMonorailArrowOffset;
    const float x0 = midpoint.x + kHalf * sideX, z0 = midpoint.z + kHalf * sideZ;
    quad[0] = {x0, y, z0, 0.0f, 0.0f};
    quad[1] = {x0 + dx, y, z0 + dz, 0.0f, 1.0f};
    quad[2] = {quad[1].x - sideX, y, quad[1].z - sideZ, 1.0f, 1.0f};
    quad[3] = {quad[2].x - dx, y, quad[2].z - dz, 1.0f, 0.0f};
}

// 0x004b5502: the path's last stretch (from three points before the end, or from the start when that is shorter than
// 0.25), flattened, against the cell vector: a turn one way above 0.25, the other way below -0.25, else straight.
IPylonTextureBinding* SC4TrainJunctionView::GetPathBitmap(std::uint8_t from, std::uint8_t to, const cISC4PathInfo* pathInfo,
                                                         const std::vector<cS3DVector3>* path, bool selected) const {
    (void)pathInfo;
    if (!path || path->empty()) return nullptr;   // port guard
    const std::size_t size = path->size();
    const cS3DVector3& back = path->back();
    const cS3DVector3& start = (*path)[size > 3 ? size - 3 : 0];
    cS3DVector3 direction{back.x - start.x, 0.0f, back.z - start.z};
    if (std::sqrt(direction.x * direction.x + direction.z * direction.z) < kTurnThreshold) {
        const cS3DVector3& front = path->front();
        direction = cS3DVector3{back.x - front.x, 0.0f, back.z - front.z};
    }
    direction = Normalized(direction);
    const float turn = cross(CellVector(from, to), direction).y;
    int slot = 4;                                // +0x3c / +0x40 straight
    if (turn < -kTurnThreshold) slot = 2;        // +0x34 / +0x38
    else if (turn > kTurnThreshold) slot = 0;    // +0x2c / +0x30
    return bindings_[slot + (selected ? 1 : 0)];
}

}  // namespace sc4
