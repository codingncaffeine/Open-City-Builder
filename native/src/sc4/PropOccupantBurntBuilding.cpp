#include "sc4/PropOccupantBurntBuilding.h"

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "s3d/S3DMath.h"
#include "sc4/DrawContext.h"

namespace sc4 {

namespace {
IBurntBuildingHost* g_host = nullptr;

constexpr float kFloorLift = 0.1f;            // cSTESurfaceWater::UpdateFloodWaters()::kMaxSedimentToDeposit, the same constant
constexpr float kTextureScale = 0.0625f;      // FLOAT_00a70700
constexpr std::uint8_t kLoadedFromSave = 0x2; // cSC4Occupant +0x52 (the original prop +0x36)
constexpr std::uint8_t kFlagShown = 0x2;      // cSC4Occupant +0x53 (the original prop +0x37)
}  // namespace

void SetBurntBuildingHost(IBurntBuildingHost* host) { g_host = host; }

cSC4PropOccupantBurntBuilding::cSC4PropOccupantBurntBuilding() = default;
cSC4PropOccupantBurntBuilding::~cSC4PropOccupantBurntBuilding() = default;

bool cSC4PropOccupantBurntBuilding::QueryInterface(std::uint32_t iid, void** out) { return cSC4PropOccupant::QueryInterface(iid, out); }

void cSC4PropOccupantBurntBuilding::SetBoundsFromOccupant(cISC4Occupant* occupant) {
    float min[3] = {}, max[3] = {};
    occupant->GetBoundingBox(min, max);
    SetBoundingBox(min, max);
    for (int i = 0; i < 3; ++i) {
        sourceMin_[i] = min[i];
        sourceMax_[i] = max[i];
    }
    Rebuild();
}

// The floor: the prop box's footprint 0.1 above its floor, corners (min x, min z), (max x, min z), (max x, max z),
// (min x, max z); the texture origin is the box minimum / 16.
void cSC4PropOccupantBurntBuilding::Rebuild() {
    float min[3] = {}, max[3] = {};
    GetBoundingBox(min, max);
    for (FloorVertex& v : floor_) v.y = min[1] + kFloorLift;
    floor_[0].x = min[0]; floor_[0].z = min[2];
    floor_[1].x = max[0]; floor_[1].z = min[2];
    floor_[2].x = max[0]; floor_[2].z = max[2];
    floor_[3].x = min[0]; floor_[3].z = max[2];
    hasFloor_ = true;
    for (int i = 0; i < 3; ++i) texOrigin_[i] = min[i] / 16.0f;
}

// The occupant (not the prop) initialises; the five bindings of the floor texture, the render state; a loaded one rebuilds.
bool cSC4PropOccupantBurntBuilding::Init() {
    if (IsInitialized()) return true;
    cSC4Occupant::Init();
    if (!hasTextures_) {
        for (std::int32_t& texture : textures_) texture = g_host ? g_host->TextureId(0x7ab50e44, 0x1abe787d, kBurntBuildingTextureInstance) : 0;
        hasTextures_ = true;
    }
    renderState_.PackRenderState(true, true, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);
    if ((cSC4Occupant::state_ & kLoadedFromSave) != 0) Rebuild();
    return true;
}

bool cSC4PropOccupantBurntBuilding::Shutdown() {
    if (!IsInitialized()) return true;
    cSC4Occupant::Shutdown();
    if (hasTextures_) {
        textures_.fill(0);
        hasTextures_ = false;
    }
    hasFloor_ = false;
    return true;
}

// version 3, the prop record, the source bounds (min then max)
bool cSC4PropOccupantBurntBuilding::Write(cIGZOStream& stream) const {
    stream.SetUint16(kBurntBuildingRecordVersion);
    if (cSC4PropOccupant::Write(stream)) {
        for (float v : sourceMin_) stream.SetFloat32(v);
        for (float v : sourceMax_) stream.SetFloat32(v);
    }
    return !stream.GetError();
}

bool cSC4PropOccupantBurntBuilding::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kBurntBuildingRecordVersion) return false;
    if (cSC4PropOccupant::Read(stream)) {
        for (float& v : sourceMin_) stream.GetFloat32(v);
        for (float& v : sourceMax_) stream.GetFloat32(v);
    }
    return !stream.GetError();
}

// The view's flag 2 puts the floor in the renderer (or takes it out).
void cSC4PropOccupantBurntBuilding::SetAllFlags(std::uint32_t flags) {
    if (((cSC4Occupant::flags_ ^ flags) & kFlagShown) != 0 && g_host) {
        auto* model = static_cast<s3d::cIS3DModelInstance*>(this);
        if ((flags & kFlagShown) == 0) {
            if (g_host->IsModelInserted(model)) g_host->RemoveModel(model);
        } else if (!g_host->IsModelInserted(model)) {
            g_host->InsertModel(model);
        }
    }
    cSC4Occupant::SetAllFlags(flags);
}

void cSC4PropOccupantBurntBuilding::GetWorldBounds(cS3DBoundingBox& out) {
    float min[3] = {}, max[3] = {};
    GetBoundingBox(min, max);
    for (int i = 0; i < 3; ++i) {
        out.min[i] = min[i];
        out.max[i] = max[i];
    }
}

void cSC4PropOccupantBurntBuilding::GetSortValue(std::uint32_t& out) const {
    out = hasTextures_ ? static_cast<std::uint32_t>(textures_[0]) : 0;
}

int cSC4PropOccupantBurntBuilding::Draw(SC4DrawContext* context, bool highlight) {
    if (highlight || !hasFloor_ || !context) return 1;
    const int zoom = context->Zoom();
    if (zoom < 0 || zoom >= 5 || !hasTextures_ || textures_[std::size_t(zoom)] == 0) return 1;
    context->ResetModelViewTransform();
    context->SetRenderState(renderState_);
    const s3d::SC4DrawAttributes attributes = GetDrawAttributes();
    context->SetModelShade(static_cast<s3d::cIS3DModelInstance*>(this), reinterpret_cast<const cS3DVector3*>(attributes.shade));
    context->SetTexture(textures_[std::size_t(zoom)], 0);
    context->SetTexWrapModes(3, 3, 0);
    context->EnableBlendStateFlag(false);
    context->SetDepthOffset(5);
    float point[3] = {texOrigin_[0], texOrigin_[1], texOrigin_[2]};
    if (g_host) g_host->ProjectToViewCoordinates(point);
    cS3DMatrix4 matrix;   // identity
    matrix.m[0][0] = kTextureScale;
    matrix.m[1][1] = -kTextureScale;
    matrix.m[2][2] = kTextureScale;
    matrix.m[3][0] = -point[0] * kTextureScale;
    matrix.m[3][1] = point[1] * kTextureScale;
    matrix.m[3][2] = -point[2] * kTextureScale;
    context->SetTexTransform4(matrix, 0);
    context->DrawPrims(6, 2, 4, floor_.data());
    context->ClearTexTransform(0);
    context->ResetShade();
    context->SetDefaultRenderState();
    context->SetDepthOffset(0);
    return 1;
}

}  // namespace sc4
