// cSC4NetworkOccupantWithPreBuiltModel — see NetworkOccupantWithPreBuiltModel.h for the layout, the quirks and the
// seam map. Vtable of the original (address point 0x100a81d38): 0x000 QueryInterface, 0x008 AddRef, 0x010 Release,
// 0x040 HasNetworkFlag, 0x060 GetNetworkFlag, 0x0b8 PieceId, 0x0c0 GetRotation, 0x0c8 GetFlip, 0x0d0
// GetRotationAndFlip, 0x100 HasViewDependentModel, 0x108 GetModelInstances, 0x168 GetEdgeStore, 0x188 GetPathInfo,
// 0x1c0 GetAltitude, 0x1c8 GetNormal, 0x1d0 SetTranslucency, 0x1d8 GetOccupiedCell, 0x1e0 IsLit, 0x1e8 SetLit,
// 0x208 TransformNode, 0x248 IsSupportPylonPresent, 0x260 Init, 0x268 Write, 0x270 Read, 0x278 GetGZCLSID,
// 0x280 SetRotationAndFlip, 0x290/0x298 GetPosition, 0x2a8 GetBoundingBox, 0x2d8 Draw, 0x310 UpdateLighting,
// 0x318 ComputeBoundingBox, 0x320 SetVisibility, 0x328 SetAllFlags, 0x330 UpdateBaseModel.
#include "sc4/NetworkOccupantWithPreBuiltModel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "rz/cRZVariant.h"
#include "sc4/Exemplar.h"
#include "sc4/SCProperty.h"

namespace sc4 {

namespace {
IPreBuiltModelOccupantHost* g_host = nullptr;

// The original rotates / flips the model with this transform (GetModelInstances 0x1001bb191..0x1001bb3e0): the
// instance's own transform is copied (scale included - it is *not* forced to 1), a Y rotation built from sincosf
// overwrites the rotation, x is negated when the piece is flipped, and the model offset is added as translation.
cS3DTransform BuildModelTransform(INetworkOccupantModelInstance& instance, const cS3DVector3& position,
                                  const cS3DVector3& offset, float angle, bool flip) {
    cS3DTransform transform;   // the original writes the identity first, then GetTransform overwrites it
    transform.changeCounter = 0;
    transform.flags = 0;
    instance.ModelInstance()->GetTransform(transform);   // copies the transform and bumps its counter
    const float sine = std::sin(angle);
    const float cosine = std::cos(angle);
    const float signedCosZero = -(cosine * 0.0f);        // quirk: the off-diagonal zeros keep the sign of the term
    const float signedSinZero = sine * 0.0f + 0.0f;
    transform.rotation[0][0] = cosine;
    transform.rotation[0][1] = signedCosZero;
    transform.rotation[0][2] = sine;
    transform.rotation[1][0] = signedSinZero;
    transform.rotation[1][1] = 1.0f;
    transform.rotation[1][2] = signedCosZero;
    transform.rotation[2][0] = -sine;
    transform.rotation[2][1] = signedSinZero;
    transform.rotation[2][2] = cosine;
    transform.flags |= 4;                                // rotation
    ++transform.changeCounter;
    if (flip) {
        // the original negates the whole first row (both zero entries included).
        transform.rotation[0][0] = -cosine;
        transform.rotation[0][1] = cosine * 0.0f;
        transform.rotation[0][2] = -sine;
        transform.flags |= 4;
        ++transform.changeCounter;
    }
    transform.translation[0] = position.x + offset.x;
    transform.translation[1] = position.y + offset.y;
    transform.translation[2] = position.z + offset.z;
    transform.flags |= 1;                                // translation
    ++transform.changeCounter;
    return transform;
}

cISCResExemplar* DefaultExemplar(cSC4NetworkOccupantWithPreBuiltModel& occupant) {
    auto* holder = static_cast<cSCExemplarPropertyHolder*>(occupant.AsPropertyHolder());
    return holder ? holder->GetDefaultExemplar() : nullptr;
}

// The occupant interface the path info calls receive: the original passes `this` (the cISC4NetworkOccupant); this
// adapter forwards the thirteen slots to the occupant.
class PreBuiltPathOccupant final : public IPathNetworkOccupant {
public:
    explicit PreBuiltPathOccupant(cSC4NetworkOccupantWithPreBuiltModel& occupant) : occupant_(occupant) {}
    std::uint32_t PieceId() const override { return occupant_.PieceId(); }
    std::uint8_t GetRotation() const override { return occupant_.GetRotation(); }
    bool GetFlip() const override { return occupant_.GetFlip(); }
    void GetPosition(float* xyz) const override { occupant_.GetPosition(xyz); }
    void GetBoundingCityCells(SC4RectInt32& rect) const override { occupant_.GetBoundingCityCells(rect); }
    void TransformNode(cS3DVector3& point) override { occupant_.TransformNode(point); }
    float GetAltitude(float x, float z) const override { return occupant_.GetAltitude(x, z); }
    std::uint8_t GetOneWayDirections() const override { return occupant_.GetOneWayDirections(); }
    std::uint8_t GetActiveEdgesForType(std::uint32_t networkType) const override {
        return occupant_.GetActiveEdgesForType(networkType);
    }
    std::uint8_t GetActiveEdgesForTypeNoGroup(std::uint32_t networkType) const override {
        return occupant_.GetActiveEdgesForTypeNoGroup(networkType);
    }
    void AddPathConnection(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) override {
        occupant_.AddPathConnection(pathType, entry, exit);
    }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { occupant_.GetOccupiedCell(x, z); }
    const cSC4PathInfo* GetPathInfo() const override {
        return const_cast<const cSC4NetworkOccupantWithPreBuiltModel&>(occupant_).GetPathInfo();
    }

private:
    cSC4NetworkOccupantWithPreBuiltModel& occupant_;
};

// The two fill (shear) rows of RecomputeShearTransform: {x factor, z factor, constant}.
void SetFillRows(float mean, float ax, float ay, float bx, float by, float positionY, cS3DVector3& rowA,
                 cS3DVector3& rowB) {
    const float constant = mean - positionY;
    rowA.x = ax * 0.0625f;
    rowA.y = ay * 0.0625f;
    rowA.z = constant;
    rowB.x = bx * 0.0625f;
    rowB.y = by * 0.0625f;
    rowB.z = constant;
}

}  // namespace

IPreBuiltModelOccupantHost* PreBuiltModelOccupantHost() { return g_host; }
void SetPreBuiltModelOccupantHost(IPreBuiltModelOccupantHost* host) { g_host = host; }

// --------------------------------------------------------------------------------------------- lifecycle

cSC4NetworkOccupantWithPreBuiltModel::cSC4NetworkOccupantWithPreBuiltModel() {
    // 0x1001b9fd6 (C1) / 0x1001baf32 (C2). The vtable stores are the compiler's.
    networkFlags_ |= kNetworkFlagPreBuiltConstructed;   // this[0x18a] |= 0x40
    modelOffset_ = {};                      // quirk: the original does not initialise +0x1d0..+0x1d8 or +0x1f8
    fillA_ = {};
    fillB_ = {};
    supportModelId_ = 0;
    supportRotation_ = 0.0f;
    supportOffset_ = {};
    shadowPieceId_ = 0;
    shadowRotation_ = 0;
    shadowLand_ = -1;                       // *(u64*)(this+0x210) = -1, -1
    shadowWater_ = -1;
    lastZoom_ = 0xff;                       // *(u16*)(this+0x218) = 0xffff
    lastShadowView_ = 0xff;
    model_ = nullptr;
    supportModel_ = nullptr;
    pathInfo_ = nullptr;
    modelInstance_ = nullptr;
    modelStateFlags_ = 0;
}

cSC4NetworkOccupantWithPreBuiltModel::~cSC4NetworkOccupantWithPreBuiltModel() {
    // 0x1001d26d0: release the support model first (+0x228), then the model (+0x220); the base destructor
    // releases the path info at +0x190.
    SetSupportModel(nullptr);
    SetPreBuiltModel(nullptr);
}

bool cSC4NetworkOccupantWithPreBuiltModel::Init() {
    // 0x1001ba0e6
    if (IsInitialized()) return true;                       // this+8 vslot 0x28 (cSC4Occupant::IsInitialized)
    if (!cSC4NetworkOccupant::Init()) return false;
    SetModelStateFlags(0x50, 0x40);                         // *(u32*)(this+0x84) = (word & ~0x50) | 0x40
    if (g_host) SetLit(g_host->IsNight());                  // vslot 0x1e8 with the lighting manager vslot 0x48
    RecomputeShearTransform();
    return true;
}

bool cSC4NetworkOccupantWithPreBuiltModel::Shutdown() {
    // 0x1001ba29e
    if (!IsInitialized()) return true;
    SetPreBuiltModel(nullptr);
    SetSupportModel(nullptr);
    return cSC4NetworkOccupant::Shutdown();
}

// --------------------------------------------------------------------------------------------- COM

bool cSC4NetworkOccupantWithPreBuiltModel::QueryInterface(std::uint32_t iid, void** out) {
    // 0x1001bc350: the class iid and the two underground iids answer with the main object.
    if (iid == kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel || iid == kGZIID_PreBuiltUndergroundOccupant ||
        iid == kGZIID_PreBuiltUndergroundOccupant2) {
        *out = this;
        AddRef();
        return true;
    }
    if (cSC4Occupant::QueryInterface(iid, out)) return true;
    if (modelInstance_) return modelInstance_->QueryInterface(iid, out);   // the +0x68 base
    return false;
}

// --------------------------------------------------------------------------------------------- record

bool cSC4NetworkOccupantWithPreBuiltModel::Write(cIGZOStream& stream) const {
    // 0x1001bc4c0: the derived version word (4) comes before the base record (version 8).
    stream.SetUint16(4);
    if (cSC4NetworkOccupant::Write(stream)) {
        stream.SetFloat32(modelOffset_.x);
        stream.SetFloat32(modelOffset_.y);
        stream.SetFloat32(modelOffset_.z);
        stream.SetUint32(shadowPieceId_);
        stream.SetUint8(shadowRotation_);
        stream.SetUint32(supportModelId_);
        if (supportModelId_ != 0) {
            stream.SetFloat32(supportRotation_);
            stream.SetFloat32(supportOffset_.x);
            stream.SetFloat32(supportOffset_.y);
            stream.SetFloat32(supportOffset_.z);
        }
    }
    return !stream.GetError();
}

bool cSC4NetworkOccupantWithPreBuiltModel::Read(cIGZIStream& stream) {
    // 0x1001bc5e4: only versions 3 and 4; version 3 carries six legacy words before the new fields and one after.
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 3 && version <= 4) {
        if (cSC4NetworkOccupant::Read(stream)) {
            stream.GetFloat32(modelOffset_.x);
            stream.GetFloat32(modelOffset_.y);
            stream.GetFloat32(modelOffset_.z);
            if (version < 4) {
                std::uint32_t legacy = 0;
                for (int i = 0; i < 6; ++i) stream.GetUint32(legacy);
            }
            stream.GetUint32(shadowPieceId_);
            stream.GetUint8(shadowRotation_);
            stream.GetUint32(supportModelId_);
            if (supportModelId_ != 0) {
                stream.GetFloat32(supportRotation_);
                stream.GetFloat32(supportOffset_.x);
                stream.GetFloat32(supportOffset_.y);
                stream.GetFloat32(supportOffset_.z);
            }
            if (version < 4) {
                std::uint32_t legacy = 0;
                stream.GetUint32(legacy);
            }
        }
        return !stream.GetError();
    }
    return false;
}

// --------------------------------------------------------------------------------------------- drawing

void cSC4NetworkOccupantWithPreBuiltModel::Draw(SC4DrawContext* context, bool highlight) {
    // 0x1001ba316: the low byte of the render properties shadow-mode word keys the shadow refresh.
    if (!g_host) return;
    const std::int32_t mode = g_host->ShadowViewMode();
    if (static_cast<std::uint8_t>(mode) != lastShadowView_) {
        UpdateShadow(true);
        lastShadowView_ = static_cast<std::uint8_t>(mode);
    }
    g_host->DrawOccupant(*this, context, highlight);        // cSC4NetworkOccupant::Draw @ 0x10019e09a
}

void cSC4NetworkOccupantWithPreBuiltModel::UpdateShadow(bool force) {
    // 0x1001ba37a. Two overlay layers (land = 0, water = 1). Each handle is -1 (none), -2 (pending: the network
    // manager refused the shadow texture or the flat-terrain check skipped it) or an overlay id.
    if (!g_host) return;
    const bool visible = IsFlagSet(1) && IsFlagSet(4) && g_host->ShadowViewMode() > 2;
    bool remove = false;
    bool create = false;
    if (shadowLand_ == -1) {
        create = (shadowWater_ == -1 || force) && visible;
        remove = shadowWater_ != -1;
        if (remove && !create) {
            remove = !visible || force;
            create = false;
            if (!create && !remove) return;
        } else if (!create && !remove) {
            return;
        }
    } else {
        remove = true;
        create = true;
        if (!(visible && force)) {
            remove = !visible || force;
            create = false;
        }
        if (!create && !remove) return;
    }
    IShadowDecalOverlay* land = nullptr;
    IShadowDecalOverlay* water = nullptr;
    if (!g_host->GetShadowOverlays(&land, &water)) return;
    if (remove) {
        if (shadowLand_ == -2) {
            shadowLand_ = -1;
        } else if (shadowLand_ != -1) {
            land->RemoveOverlay(static_cast<std::uint32_t>(shadowLand_));
            shadowLand_ = -1;
        }
        if (shadowWater_ != -1) {
            if (shadowWater_ != -2) water->RemoveOverlay(static_cast<std::uint32_t>(shadowWater_));
            shadowWater_ = -1;
        }
    }
    if (create && !g_host->IsNight()) {                     // the lighting manager vslot 0x48 must be false
        int zoom = 0;
        int rotation = 0;
        g_host->GetZoomAndRotation(zoom, rotation);
        std::uint32_t cellX = 0;
        std::uint32_t cellZ = 0;
        GetOccupiedCell(cellX, cellZ);
        cS3DVector3 sun;
        g_host->GetSunDirection(sun);
        float offsetX = sun.x * -0.0f;
        float offsetZ = sun.z * -0.0f;
        const float sunY = sun.y;
        std::uint32_t pieceId = PieceId();
        std::uint8_t rotationAndFlip = GetRotationAndFlip();
        if (shadowPieceId_ != 0) {
            rotationAndFlip = shadowRotation_;
            pieceId = shadowPieceId_;
        }
        const std::uint32_t mapped =
            g_host->MapShadowTexture(pieceId + static_cast<std::uint32_t>(zoom), rotationAndFlip,
                                     GetNetworkFlag() & 0x1fff, GetEdgeStore().GetRaisedEdgeTypeConfig());
        shadowLand_ = -2;
        shadowWater_ = -2;
        if (mapped != 0) {
            if ((GetNetworkFlag() & kNetworkFlagPreBuiltShadowFlatMask) == kNetworkFlagPreBuiltShadowCheck) {
                float low = 100000.0f;
                float high = 0.0f;
                for (int corner = 0; corner < 4; ++corner) {
                    const float delta =
                        Corner(corner).y - g_host->TerrainAltitudeAtVertex(
                                               static_cast<int>(cellX) + kShadowCheckNextX[corner],
                                               static_cast<int>(cellZ) + kShadowCheckNextZ[corner]);
                    if (delta < low) low = delta;
                    if (delta > high) high = delta;
                }
                if (low <= 8.0f && high - low <= 5.8f) return;   // flat piece: no shadow, handles stay -2
            }
            offsetX /= sunY;
            offsetZ /= sunY;
            const std::int8_t signedRotation = static_cast<std::int8_t>(rotationAndFlip);
            const std::uint8_t shadowRotation =
                signedRotation < 0 ? static_cast<std::uint8_t>(signedRotation + 2)
                                   : static_cast<std::uint8_t>(-signedRotation);
            const bool flip = signedRotation < 0;
            const float rotationQuarter = static_cast<float>(shadowRotation & 3) * 0.25f;
            // quirk: the cell coordinate is cast to *unsigned* before the << 4 / float conversion.
            cS3DVector2 position;
            position.x = static_cast<float>(cellX << 4) + offsetX + 8.0f;
            position.y = static_cast<float>(cellZ << 4) + offsetZ + 8.0f;
            const cGZPersistResourceKey key{kPreBuiltShadowType, kPreBuiltShadowGroup, mapped};
            if (!g_host->CellHasWater(cellX, cellZ)) {
                shadowLand_ = static_cast<std::int32_t>(
                    land->AddShadowDecal(key, position, 16.0f, rotationQuarter, flip));
            } else {
                shadowWater_ = static_cast<std::int32_t>(
                    water->AddShadowDecal(key, position, 16.0f, rotationQuarter, flip));
            }
        }
    }
}

void cSC4NetworkOccupantWithPreBuiltModel::SetTranslucency(bool on, std::uint8_t alpha) {
    // 0x1001bbdfc: the base colours the tile, then the two models get the alpha in their draw attributes.
    cSC4NetworkOccupant::SetTranslucency(on, alpha);
    if (!g_host) return;
    const float alphaF = static_cast<float>(alpha) * 0.003921569f;
    s3d::cIS3DModelInstance* models[2] = {model_, supportModel_};
    for (s3d::cIS3DModelInstance* model : models) {
        if (!model) continue;
        s3d::SC4DrawAttributes attributes = model->GetDrawAttributes();
        attributes.flags &= ~2u;
        attributes.flags |= 2u;
        attributes.shade[3] = alphaF;
        if (!on) {
            attributes.shade[3] = 1.0f;
            attributes.flags &= ~2u;
        }
        model->SetDrawAttributes(attributes);
        if (g_host->HasModelInstance(model)) g_host->AddModelInstanceToRenderer(model, 5);
    }
}

// --------------------------------------------------------------------------------------------- model state

void cSC4NetworkOccupantWithPreBuiltModel::AddModel(cISCResExemplar* exemplar, const float* position,
                                                    std::uint8_t rotation) {
    // 0x1001ba886
    auto* holder = static_cast<cSCExemplarPropertyHolder*>(AsPropertyHolder());
    if (holder) holder->SetDefaultExemplar(exemplar);        // +0x18 vslot 0xd0
    SetRotationAndFlip(rotation);                            // vslot 0x280
    modelOffset_ = cS3DVector3{position[0], position[1], position[2]};
    ComputeBoundingBox(Bounds(), Bounds() + 3);              // vslot 0x318 with +0x170 / +0x17c
    if (!exemplar) return;
    cISCPropertyHolder* exemplarHolder = exemplar->AsISCPropertyHolder();
    if (exemplarHolder && !exemplarHolder->HasProperty(kPreBuiltPropWaterModel)) {
        // quirk: the property is checked on the exemplar but set on the occupant.
        auto* property = new cSCProperty(kPreBuiltPropModel);
        property->AddRef();
        cRZVariant value;
        value.SetValBool(true);
        property->SetPropertyValue(value);
        AsPropertyHolder()->AddProperty(property, false);
        property->Release();
    }
}

void cSC4NetworkOccupantWithPreBuiltModel::AddSupportModel(std::uint32_t modelId, float rotation,
                                                           const cS3DVector3& offset) {
    // 0x1001bb004
    supportModelId_ = modelId;
    supportRotation_ = rotation;
    supportOffset_ = offset;
}

bool cSC4NetworkOccupantWithPreBuiltModel::IsSupportPylonPresent() const {
    // 0x1001bc338
    if (supportModelId_ != 0) return true;
    return cSC4NetworkOccupant::IsSupportPylonPresent();
}

void cSC4NetworkOccupantWithPreBuiltModel::RecomputeShearTransform() {
    // 0x1001ba162: the two fill rows that lay the model on the four corner heights. The rows are
    // {x factor, z factor, constant} and the constant is the mean of the two summed corners minus the position y.
    float position[3] = {0, 0, 0};
    GetPosition(position);
    const float c0 = Corner(0).y;
    const float c1 = Corner(1).y;
    const float c2 = Corner(2).y;
    const float c3 = Corner(3).y;
    if (!HasNetworkFlag(kNetworkFlagPreBuiltDiagonal)) {
        SetFillRows((c0 + c2) * 0.5f, c2 - c1, c1 - c0, c3 - c0, c2 - c3, position[1], fillA_, fillB_);
    } else {
        SetFillRows((c1 + c3) * 0.5f, c3 - c0, c1 - c0, c2 - c1, c2 - c3, position[1], fillA_, fillB_);
    }
}

float cSC4NetworkOccupantWithPreBuiltModel::GetAltitude(float x, float z) const {
    // 0x1001bc0a4: the fill plane instead of the terrain triangles.
    float position[3];
    GetPosition(position);
    const float dx = x - position[0];
    const float dz = z - position[2];
    const float signedZ = HasNetworkFlag(kNetworkFlagPreBuiltDiagonal) ? dz : -dz;
    const cS3DVector3& row = (dx + signedZ >= 0.0f) ? fillB_ : fillA_;
    return row.z + dz * row.y + position[1] + dx * row.x;
}

void cSC4NetworkOccupantWithPreBuiltModel::GetNormal(float x, float z, float out[3]) const {
    // 0x1001bc178
    float position[3];
    GetPosition(position);
    const float dz = z - position[2];
    const float signedZ = HasNetworkFlag(kNetworkFlagPreBuiltDiagonal) ? dz : -dz;
    const cS3DVector3& row = ((x - position[0]) + signedZ >= 0.0f) ? fillB_ : fillA_;
    out[0] = -row.x;
    out[1] = 1.0f;
    out[2] = -row.y;
}

void cSC4NetworkOccupantWithPreBuiltModel::TransformNode(cS3DVector3& point) {
    // 0x1001bbf5e: rotate (x, z) by the piece rotation, mirror on flip, add the path origin, then the fill row.
    const std::uint8_t rotation = GetRotation();
    const bool flip = GetFlip();
    float rx = 0.0f;
    float rz = 0.0f;
    switch (rotation) {
        case 0: rx = point.x; rz = point.z; break;
        case 1: rx = -point.z; rz = point.x; break;
        case 2: rx = -point.x; rz = -point.z; break;
        case 3: rx = point.z; rz = -point.x; break;
        default: break;
    }
    if (flip) rx = -rx;
    if (GetPathIDOverride() != 0) {
        rx += PathOffsetX();
        rz += PathOffsetZ();
    }
    float signedZ = rz;
    if (!HasNetworkFlag(kNetworkFlagPreBuiltDiagonal)) signedZ = -rz;
    const cS3DVector3& row = (signedZ + rx >= 0.0f) ? fillB_ : fillA_;
    point.y = rz * row.y + rx * row.x + row.z + point.y;
}

void cSC4NetworkOccupantWithPreBuiltModel::SetLit(bool lit) {
    // 0x1001bc312
    cSC4NetworkOccupant::SetLit(lit);
    UpdateShadow(true);
}

void cSC4NetworkOccupantWithPreBuiltModel::SetVisibility(bool visible, bool postNow) {
    // 0x1001bc25e
    cSC4Occupant::SetVisibility(visible, postNow);
    UpdateShadow(false);
}

void cSC4NetworkOccupantWithPreBuiltModel::SetAllFlags(std::uint32_t flags) {
    // 0x1001bc2ae
    cSC4Occupant::SetAllFlags(flags);
    UpdateShadow(false);
}

void cSC4NetworkOccupantWithPreBuiltModel::UpdateLighting(void* lightingManager) {
    // 0x1001bc2f0: the base updates the tile lighting with the manager, then the shadow is refreshed.
    if (g_host) g_host->UpdateOccupantLighting(*this);
    (void)lightingManager;
    UpdateShadow(true);
}

// --------------------------------------------------------------------------------------------- bounds

void cSC4NetworkOccupantWithPreBuiltModel::ComputeBoundingBox(float* minXyz, float* maxXyz) {
    // 0x1001ba9d0
    if (!DefaultExemplar(*this)) {
        cSC4NetworkOccupant::ComputeBoundingBox(minXyz, maxXyz);
        return;
    }
    int zoom = 0;
    int rotation = 0;
    if (g_host) g_host->GetZoomAndRotation(zoom, rotation);
    s3d::cIS3DModelInstance* instances[10] = {};
    int count = 0;
    GetModelInstances(zoom, rotation, instances, 10, count);
    // Quirk (0x1001baa63): a zero instance count returns *without touching the output box at all*; the caller keeps
    // whatever bounds it had. (Only a negative count, never produced by GetModelInstances, reaches the corner
    // fallback.) In practice Init has already filled the bounds from the corners before AddModel runs.
    if (count == 0) return;
    // The corner fallback (LAB_1001bad17) is only reached for a negative count (0 returned above, >= 1 loops); it
    // gives the defined starting values.
    float minX = std::min(Corner(0).x, Corner(2).x);
    float minY = std::min(Corner(0).y, Corner(2).y);
    float minZ = std::min(Corner(0).z, Corner(2).z);
    float maxX = std::max(Corner(0).x, Corner(2).x);
    float maxY = std::max(Corner(0).y, Corner(2).y);
    float maxZ = std::max(Corner(0).z, Corner(2).z);
    float runMinX = 1.0f, runMaxX = 0.0f;   // the running box starts in the "empty" sentinel state (1, 0)
    float runMinY = 1.0f, runMaxY = 0.0f;
    float runMinZ = 1.0f, runMaxZ = 0.0f;
    if (count >= 1) {
        for (int i = 0; i < count; ++i) {
            s3d::cIS3DModelInstance* instance = instances[i];
            if (instance && instance != modelInstance_) {
                cS3DBoundingBox box;
                box.min[0] = box.min[1] = box.min[2] = 1.0f;
                box.max[0] = box.max[1] = box.max[2] = 0.0f;
                instance->GetWorldBounds(box);
                if (box.min[0] <= box.max[0]) {
                    if (runMinX <= runMaxX) {
                        if (runMaxX <= box.max[0]) runMaxX = box.max[0];
                        if (box.min[0] <= runMinX) runMinX = box.min[0];
                        if (runMaxY <= box.max[1]) runMaxY = box.max[1];
                        if (box.min[1] <= runMinY) runMinY = box.min[1];
                        if (runMaxZ <= box.max[2]) runMaxZ = box.max[2];
                        if (box.min[2] <= runMinZ) runMinZ = box.min[2];
                    } else {
                        runMinX = box.min[0];
                        runMaxX = box.max[0];
                        runMinY = box.min[1];
                        runMaxY = box.max[1];
                        runMinZ = box.min[2];
                        runMaxZ = box.max[2];
                    }
                    minX = runMinX;
                    maxX = runMaxX;
                    minY = runMinY;
                    maxY = runMaxY;
                    minZ = runMinZ;
                    maxZ = runMaxZ;
                }
            }
            if (instance) {
                // the occupant's own +0x68 model instance (excluded above) is released without clearing its owner
                if (instance != modelInstance_) instance->SetOwner(nullptr);
                instance->Release();
            }
            instances[i] = nullptr;
        }
        if (runMaxX < runMinX) {            // no valid instance box: fall back to the corner diagonal
            minX = std::min(Corner(0).x, Corner(2).x);
            minY = std::min(Corner(0).y, Corner(2).y);
            minZ = std::min(Corner(0).z, Corner(2).z);
            maxX = std::max(Corner(0).x, Corner(2).x);
            maxY = std::max(Corner(0).y, Corner(2).y);
            maxZ = std::max(Corner(0).z, Corner(2).z);
        }
    }
    for (const SC4AddOnPart& part : AddOnParts()) {
        for (const SC4TileCorner& corner : part.corner) {
            if (corner.x < minX) minX = corner.x;
            if (corner.y < minY) minY = corner.y;
            if (corner.z < minZ) minZ = corner.z;
            if (maxX <= corner.x && corner.x != maxX) maxX = corner.x;
            if (maxY <= corner.y && corner.y != maxY) maxY = corner.y;
            if (maxZ <= corner.z && corner.z != maxZ) maxZ = corner.z;
        }
    }
    // clamp to the tile: corner 0 grows the minimum, corner 2 lowers the maximum (x / z only)
    if (Corner(0).x > minX) minX = Corner(0).x;
    if (Corner(0).z > minZ) minZ = Corner(0).z;
    if (Corner(2).x < maxX) maxX = Corner(2).x;
    if (Corner(2).z < maxZ) maxZ = Corner(2).z;
    minXyz[0] = minX;
    minXyz[1] = minY;
    minXyz[2] = minZ;
    maxXyz[0] = maxX;
    maxXyz[1] = maxY;
    maxXyz[2] = maxZ;
}

// --------------------------------------------------------------------------------------------- models

void cSC4NetworkOccupantWithPreBuiltModel::GetModelInstances(int zoom, int rotation,
                                                             s3d::cIS3DModelInstance** out, int capacity,
                                                             int& count) {
    // 0x1001bb032
    (void)capacity;
    if (g_host) g_host->GetOccupantModelInstances(*this, zoom, rotation, out, capacity, count);
    cISCResExemplar* exemplar = DefaultExemplar(*this);
    if (exemplar) {
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        const bool exemplarFlag = holder && holder->HasProperty(kPreBuiltPropExemplarFlags);
        const bool waterModel = holder && holder->HasProperty(kPreBuiltPropWaterModel);
        const std::uint32_t rotationAndFlip = GetRotation();
        const std::uint32_t modelId =
            g_host ? g_host->GetModelInstanceID(static_cast<cISC4Occupant*>(this), zoom, rotation,
                                                static_cast<int>(rotationAndFlip), !exemplarFlag, nullptr, nullptr)
                   : 0;
        if (modelId == 0) return;                            // the original skips the zoom update here
        if (g_host) {
            const cGZPersistResourceKey key{kPreBuiltModelType, kPreBuiltModelGroup, modelId};
            if (s3d::cIS3DResModel* resource = g_host->CreateModelResource(key)) {
                INetworkOccupantModelInstance* instance = g_host->CreateOccupantModelInstance();
                if (!instance) {
                    SetPreBuiltModel(nullptr);               // LAB_1001bb1f3
                } else {
                    instance->ModelInstance()->AddRef();
                    if (!instance->Init(resource, 0)) {
                        SetPreBuiltModel(nullptr);
                    } else {
                        float position[3];
                        GetPosition(position);
                        const float angle = exemplarFlag
                                                ? -g_host->ViewRotationAngle()
                                                : g_host->GetObjectYRotationAngle(static_cast<int>(rotationAndFlip));
                        instance->ModelInstance()->SetTransform(
                            BuildModelTransform(*instance, cS3DVector3{position[0], position[1], position[2]},
                                                modelOffset_, angle, GetFlip()));
                        if (!HasNetworkFlag(kNetworkFlagPreBuiltShadowCheck)) {
                            if (!waterModel) {
                                cS3DVector3 offset{};
                                if (GetPathIDOverride() != 0) {
                                    offset.x = -PathOffsetX();
                                    offset.z = -PathOffsetZ();
                                }
                                instance->Prelight();
                                instance->Deform(GetRotation(), GetFlip(), fillA_, fillB_, offset,
                                                 HasNetworkFlag(kNetworkFlagPreBuiltDiagonal));
                                instance->FlipAndLight(GetFlip());
                            }
                        } else {
                            std::uint32_t cellX = 0;
                            std::uint32_t cellZ = 0;
                            GetOccupiedCell(cellX, cellZ);
                            float lowest = 0.0f;
                            if (!g_host->WaterSurfaceDisplayed()) {
                                const float h00 = g_host->TerrainAltitudeAtVertex(static_cast<int>(cellX),
                                                                                  static_cast<int>(cellZ));
                                const float h10 = g_host->TerrainAltitudeAtVertex(static_cast<int>(cellX) + 1,
                                                                                  static_cast<int>(cellZ));
                                const float h11 = g_host->TerrainAltitudeAtVertex(static_cast<int>(cellX) + 1,
                                                                                  static_cast<int>(cellZ) + 1);
                                const float h01 = g_host->TerrainAltitudeAtVertex(static_cast<int>(cellX),
                                                                                  static_cast<int>(cellZ) + 1);
                                lowest = std::min(std::min(h00, h10), std::min(h11, h01));
                            } else {
                                const float x0 = static_cast<float>(cellX << 4);
                                const float z0 = static_cast<float>(cellZ << 4);
                                const float c00 = g_host->WaterLevel(x0, z0);
                                const float c10 = g_host->WaterLevel(x0 + 16.0f, z0);
                                const float c11 = g_host->WaterLevel(x0 + 16.0f, z0 + 16.0f);
                                const float c01 = g_host->WaterLevel(x0, z0 + 16.0f);
                                lowest = std::min(std::min(c00, c10), std::min(c11, c01));
                            }
                            const cS3DVector3 lo{-FLT_MAX, lowest - position[1], -FLT_MAX};
                            const cS3DVector3 hi{FLT_MAX, FLT_MAX, FLT_MAX};
                            instance->Clip(lo, hi, true);
                            if (!waterModel) {
                                instance->Prelight();
                                instance->FlipAndLight(GetFlip());
                            }
                        }
                        if (Corner(0).color[3] != 0xff) {
                            s3d::SC4DrawAttributes attributes =
                                instance->ModelInstance()->GetDrawAttributes();
                            attributes.flags |= 2u;
                            attributes.shade[3] = static_cast<float>(Corner(0).color[3]) * 0.003921569f;
                            instance->ModelInstance()->SetDrawAttributes(attributes);
                        }
                        instance->ModelInstance()->SetOwner(static_cast<cISC4Occupant*>(this));
                        instance->ModelInstance()->AddRef();
                        out[count] = instance->ModelInstance();
                        ++count;
                        UpdateBaseModel(instance->ModelInstance());
                        SetPreBuiltModel(instance->ModelInstance());
                    }
                    instance->ModelInstance()->Release();
                }
                resource->Release();
            }
        }
        if (supportModelId_ != 0 && g_host) {
            const cGZPersistResourceKey supportKey{kPreBuiltModelType, kPreBuiltModelGroup, supportModelId_};
            if (s3d::cIS3DResModel* resource = g_host->CreateModelResource(supportKey)) {
                INetworkOccupantModelInstance* instance = g_host->CreateOccupantModelInstance();
                if (!instance) {
                    SetSupportModel(nullptr);                // LAB_1001bb834
                } else {
                    instance->ModelInstance()->AddRef();
                    if (!instance->Init(resource, 0)) {
                        SetSupportModel(nullptr);
                    } else {
                        float position[3];
                        GetPosition(position);
                        const float baseAngle = exemplarFlag
                                                    ? -g_host->ViewRotationAngle()
                                                    : g_host->GetObjectYRotationAngle(static_cast<int>(rotationAndFlip));
                        const cS3DVector3 offset{modelOffset_.x + supportOffset_.x,
                                                 modelOffset_.y + supportOffset_.y,
                                                 modelOffset_.z + supportOffset_.z};
                        instance->ModelInstance()->SetTransform(BuildModelTransform(
                            *instance, cS3DVector3{position[0], position[1], position[2]}, offset,
                            baseAngle + supportRotation_, GetFlip()));
                        if (!waterModel) {
                            instance->Prelight();
                            instance->FlipAndLight(GetFlip());
                        }
                        instance->ModelInstance()->SetOwner(static_cast<cISC4Occupant*>(this));
                        if (Corner(0).color[3] != 0xff) {
                            s3d::SC4DrawAttributes attributes =
                                instance->ModelInstance()->GetDrawAttributes();
                            attributes.flags |= 2u;
                            attributes.shade[3] = static_cast<float>(Corner(0).color[3]) * 0.003921569f;
                            instance->ModelInstance()->SetDrawAttributes(attributes);
                        }
                        instance->ModelInstance()->AddRef();
                        out[count] = instance->ModelInstance();
                        ++count;
                        SetSupportModel(instance->ModelInstance());
                    }
                    instance->ModelInstance()->Release();
                }
                resource->Release();
            }
        }
    }
    if (lastZoom_ != static_cast<std::uint8_t>(zoom)) {
        UpdateShadow(true);
        lastZoom_ = static_cast<std::uint8_t>(zoom);
    }
}

// --------------------------------------------------------------------------------------------- path info

cISC4PathInfo* cSC4NetworkOccupantWithPreBuiltModel::GetPathInfo() {
    // 0x1001bbbca
    if (pathInfo_) return pathInfo_;
    cISCResExemplar* exemplar = DefaultExemplar(*this);
    if (!exemplar || !g_host) return nullptr;
    cGZPersistResourceKey key{};
    exemplar->GetKey(key);
    (void)exemplar->AsISCPropertyHolder();                   // the original reads it and drops the result
    key.type = kPreBuiltPathInfoType;
    key.group = kPreBuiltPathInfoGroup;
    std::uint32_t pathId = key.instance;
    if (GetPathIDOverride() != 0) pathId = GetPathIDOverride();
    key.instance = g_host->MapPath(pathId);
    cISC4PathInfo* info = g_host->GetPathInfoResource(key);
    if (info) {
        PreBuiltPathOccupant occupant(*this);
        if (!info->UseElevation()) {
            cISC4PathInfo* clone = info->Clone();
            if (clone) {
                clone->SetUseElevationFlag(true);
                info->Release();
                info = clone;
            }
        }
        cISC4PathInfo* transformed = info->CreateTransformedCopy(&occupant);
        info->Release();
        if (transformed) {
            if (GetPathIDOverride() == 0) {
                pathInfo_ = transformed;
                SetPathInfoRef(transformed);
            } else {
                SC4RectFloat32 rect;
                rect.x0 = Corner(0).x;
                rect.z0 = Corner(0).z;
                rect.x1 = Corner(2).x;
                rect.z1 = Corner(2).z;
                cISC4PathInfo* offset = transformed->OffsetAndScissor(PathOffsetX(), 0.0f, PathOffsetZ(), rect);
                transformed->Release();
                pathInfo_ = offset;
                SetPathInfoRef(offset);
                if (!offset) return nullptr;
            }
            if (!HasNetworkFlag(kNetworkFlagPreBuiltKeepPaths)) {
                pathInfo_->RemoveWrongWayPaths(&occupant);
            }
        }
    }
    if (pathInfo_) {
        PreBuiltPathOccupant occupant(*this);
        pathInfo_->VerifyPathEdges(&occupant);
        ClearPathConnections();
        pathInfo_->InitializeNetworkConnectionBitmap(&occupant);
        return pathInfo_;
    }
    return nullptr;
}

const cSC4PathInfo* cSC4NetworkOccupantWithPreBuiltModel::GetPathInfo() const {
    // The resource behind GetPathInfoResource is a cSC4PathInfo (the factory only registers type 0x296678f7).
    auto* self = const_cast<cSC4NetworkOccupantWithPreBuiltModel*>(this);
    return static_cast<const cSC4PathInfo*>(self->GetPathInfo());
}

void cSC4NetworkOccupantWithPreBuiltModel::ClearCachedPathInfo() {
    if (pathInfo_) {
        cISC4PathInfo* info = pathInfo_;
        pathInfo_ = nullptr;
        info->Release();
        SetPathInfoRef(nullptr);
    }
}

void cSC4NetworkOccupantWithPreBuiltModel::ReleasePathInfo() {
    pathInfo_ = nullptr;
    cSC4NetworkOccupant::ReleasePathInfo();
}

// --------------------------------------------------------------------------------------------- IPathNetworkOccupant

void cSC4NetworkOccupantWithPreBuiltModel::GetBoundingCityCells(SC4RectInt32& rect) const {
    SC4CellRect cells;
    const_cast<cSC4NetworkOccupantWithPreBuiltModel*>(this)->cSC4Occupant::GetBoundingCityCells(cells);
    rect.x0 = cells.x0;
    rect.z0 = cells.z0;
    rect.x1 = cells.x1;
    rect.z1 = cells.z1;
}

std::uint8_t cSC4NetworkOccupantWithPreBuiltModel::GetOneWayDirections() const {
    return const_cast<cSC4NetworkOccupantWithPreBuiltModel*>(this)->cSC4NetworkOccupant::GetOneWayDirections();
}

std::uint8_t cSC4NetworkOccupantWithPreBuiltModel::GetActiveEdgesForType(std::uint32_t networkType) const {
    return static_cast<std::uint8_t>(
        cSC4NetworkOccupant::GetActiveEdgesForType(static_cast<std::uint8_t>(networkType)));
}

std::uint8_t cSC4NetworkOccupantWithPreBuiltModel::GetActiveEdgesForTypeNoGroup(std::uint32_t networkType) const {
    return static_cast<std::uint8_t>(
        cSC4NetworkOccupant::GetActiveEdgesForTypeNoGroup(static_cast<std::uint8_t>(networkType)));
}

// --------------------------------------------------------------------------------------------- port wiring

void cSC4NetworkOccupantWithPreBuiltModel::SetPreBuiltModel(s3d::cIS3DModelInstance* model) {
    if (model_ == model) return;
    if (model) model->AddRef();
    s3d::cIS3DModelInstance* previous = model_;
    model_ = model;
    if (previous) previous->Release();
}

void cSC4NetworkOccupantWithPreBuiltModel::SetSupportModel(s3d::cIS3DModelInstance* model) {
    if (supportModel_ == model) return;
    if (model) model->AddRef();
    s3d::cIS3DModelInstance* previous = supportModel_;
    supportModel_ = model;
    if (previous) previous->Release();
}

}  // namespace sc4
