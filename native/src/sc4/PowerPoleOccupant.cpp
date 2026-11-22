#include "sc4/PowerPoleOccupant.h"
#include "platform/Unwired.h"

#include <cmath>
#include <map>

#include "sc4/DBSegment.h"
#include "sc4/DrawContext.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/PackedRenderState.h"
#include "sc4/PowerLineGeometry.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ViewUtilities.h"

#pragma STDC FP_CONTRACT OFF

namespace sc4 {

namespace {

void WriteVertices(cIGZOStream& stream, const std::vector<UndergroundVertex>& vertices) {
    stream.SetUint32(static_cast<std::uint32_t>(vertices.size()));
    for (const auto& vertex : vertices) {
        stream.SetFloat32(vertex.x);
        stream.SetFloat32(vertex.y);
        stream.SetFloat32(vertex.z);
        stream.SetFloat32(vertex.u);
        stream.SetFloat32(vertex.v);
        for (std::uint8_t channel : vertex.color) stream.SetUint8(channel);
    }
}

void ReadVertices(cIGZIStream& stream, std::vector<UndergroundVertex>& vertices) {
    std::uint32_t count = 0;
    stream.GetUint32(count);
    vertices.assign(count, UndergroundVertex{});
    for (auto& vertex : vertices) {
        stream.GetFloat32(vertex.x);
        stream.GetFloat32(vertex.y);
        stream.GetFloat32(vertex.z);
        stream.GetFloat32(vertex.u);
        stream.GetFloat32(vertex.v);
        for (std::uint8_t& channel : vertex.color) stream.GetUint8(channel);
    }
}

void WriteCurves(cIGZOStream& stream, const std::vector<std::vector<cS3DVector3>>& curves) {
    stream.SetUint32(static_cast<std::uint32_t>(curves.size()));
    for (const auto& curve : curves) {
        stream.SetUint32(static_cast<std::uint32_t>(curve.size()));
        for (const cS3DVector3& point : curve) stream << point;
    }
}

void ReadCurves(cIGZIStream& stream, std::vector<std::vector<cS3DVector3>>& curves) {
    std::uint32_t count = 0;
    stream.GetUint32(count);
    curves.assign(count, {});
    for (auto& curve : curves) {
        std::uint32_t points = 0;
        stream.GetUint32(points);
        curve.assign(points, cS3DVector3{});
        for (cS3DVector3& point : curve) stream >> point;
    }
}

IPowerPoleOccupantHost* g_poleHost = nullptr;

// The static state of the class (StaticInit / StaticShutdown, i386 0x00256ac8 / 0x00255b7a).
bool sbInited = false;
float sCellWidthX = 16.0f, sCellWidthZ = 16.0f;   // SL::spCity cell widths, read by Init
int sCurZoom = -1;                                // the zoom the texture bindings were made for
PowerLineTunables sTunables;                      // InitPowerLineControlVariables
std::vector<float> sPowerLineWidthFactors;        // per zoom: the half width of a wire (0 hides the wires)
std::int64_t snCostOfDemolition = 0;

// The four textures (texture binding instances in group 0x1abe787d) and their driver ids.
constexpr std::uint32_t kPowerLineTexture = 0xaa9da78e;
constexpr std::uint32_t kPowerLineBuoyTexture = 0xc9af0fcd;
constexpr std::uint32_t kFoundationTopTexture = 0x0912220e;
constexpr std::uint32_t kFoundationWallTexture = 0x08080004;
std::map<std::uint32_t, int> sTexResIdToInfoMap;

int Texture(std::uint32_t instance) {
    auto it = sTexResIdToInfoMap.find(instance);
    return it != sTexResIdToInfoMap.end() ? it->second : 0;
}

// cSC4PackedRenderState sPackedRenderState: PackRenderState(true, true, true, true, false, 1, false, 3, 4, 5, 4, 0.1, 1, -1).
const cSC4PackedRenderState& PoleRenderState() {
    static cSC4PackedRenderState state = [] {
        cSC4PackedRenderState s;
        s.PackRenderState(true, true, true, true, false, 1, false, 3, 4, 5, 4, 0.1f, 1, -1);
        return s;
    }();
    return state;
}

// _cardinalConnectionPoints / _diagonalConnectionPoints (filled by the file's static initializer): for each direction
// (Get0To3Direction & 3) the four points, relative to the pole's position, where its wires hang.
constexpr float kCardinalConnectionPoints[4][4][3] = {
    {{0.0f, 31.7f, 4.8f}, {0.0f, 31.7f, -4.8f}, {0.0f, 38.7f, 4.8f}, {0.0f, 38.7f, -4.8f}},
    {{0.0f, 31.7f, 6.5f}, {0.0f, 31.7f, -6.5f}, {0.0f, 38.7f, 6.5f}, {0.0f, 38.7f, -6.5f}},
    {{-4.8f, 31.7f, 0.0f}, {4.8f, 31.7f, 0.0f}, {-4.8f, 38.7f, 0.0f}, {4.8f, 38.7f, 0.0f}},
    {{-6.5f, 31.7f, 0.0f}, {6.5f, 31.7f, 0.0f}, {-6.5f, 38.7f, 0.0f}, {6.5f, 38.7f, 0.0f}},
};
constexpr float kDiagonalConnectionPoints[4][4][3] = {
    {{-3.3f, 31.7f, 3.3f}, {3.3f, 31.7f, -3.3f}, {-3.3f, 38.7f, 3.3f}, {3.3f, 38.7f, -3.3f}},
    {{-4.5f, 31.7f, 4.5f}, {4.5f, 31.7f, -4.5f}, {-4.5f, 38.7f, 4.5f}, {4.5f, 38.7f, -4.5f}},
    {{-4.5f, 31.7f, -4.5f}, {4.5f, 31.7f, 4.5f}, {-4.5f, 38.7f, -4.5f}, {4.5f, 38.7f, 4.5f}},
    {{-3.3f, 31.7f, -3.3f}, {3.3f, 31.7f, 3.3f}, {-3.3f, 38.7f, -3.3f}, {3.3f, 38.7f, 3.3f}},
};

// _kTextureU / _kTextureV: the foundation top's corner texture coordinates.
constexpr float kTextureU[4] = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr float kTextureV[4] = {0.0f, 1.0f, 1.0f, 0.0f};

// The float-to-unsigned conversion of a cell coordinate (negative values become 0).
std::uint32_t ToCell(float value) {
    if (value <= 0.0f) value = 0.0f;
    return static_cast<std::uint32_t>(value);
}

// A vertex with position and texture coordinates (sVF_V3F_T2F, format 2).
struct WireVertex {
    float x, y, z, u, v;
};

}  // namespace

IPowerPoleOccupantHost* PowerPoleOccupantHost() { return g_poleHost; }
void SetPowerPoleOccupantHost(IPowerPoleOccupantHost* host) { g_poleHost = host; }

cSC4PowerPoleOccupant::cSC4PowerPoleOccupant() {
    SetProperties(3);
    type_ = 0x2890d4de;   // the constructor stamps occupant type 0x2890d4de
}

cSC4PowerPoleOccupant::~cSC4PowerPoleOccupant() {
    for (tConnection& connection : connections_) {
        if (connection.pole) connection.pole->Release();
        for (cSC4PowerLineOccupant* line : connection.lines) if (line) line->Release();
    }
    if (model_) model_->Release();
    if (effect_) effect_->Release();
}

bool cSC4PowerPoleOccupant::QueryInterface(std::uint32_t iid, void** out) {
    switch (iid) {
        case kGZCLSID_cSC4PowerPoleOccupant:   // cISC4PowerPoleOccupant, the primary base
            *out = static_cast<cISC4PowerPoleOccupant*>(this);
            AddRef();
            return true;
        case kGZCLSID_cSC4PowerPoleOccupantRecord:
        case 0xa9764c8c:
            *out = this;
            AddRef();
            return true;
        case kGZIID_PowerPoleMessageTarget:
            *out = static_cast<cIGZMessageTarget2*>(this);
            AddRef();
            return true;
        case 0x26a68448:
            if (model_) return model_->QueryInterface(iid, out);
            break;
        default:
            break;
    }
    if (cSC4Occupant::QueryInterface(iid, out)) return true;
    return s3d::cS3DModelInstanceBasic::QueryInterface(iid, out);
}

// 0x1003227aa: the resource manager, the texture bindings (made per zoom by ChangeZoomLevel), the wire curve tunables, the
// per-zoom wire widths (property 0xe9ac43e7, default 0.3 at every zoom) and the demolition cost (0x0a2eced8).
bool cSC4PowerPoleOccupant::StaticInit() {
    if (sbInited) return true;
    sbInited = true;
    IPowerPoleOccupantHost* host = g_poleHost;
    const cISCPropertyHolder* tuning = host ? host->TuningPropertyHolder() : nullptr;
    sTunables.Read(tuning);
    sTexResIdToInfoMap.clear();
    for (std::uint32_t instance : {kPowerLineTexture, kPowerLineBuoyTexture, kFoundationTopTexture, kFoundationWallTexture})
        sTexResIdToInfoMap[instance] = 0;
    sCurZoom = -1;
    sPowerLineWidthFactors.assign(5, 0.3f);
    if (tuning) {
        cSCPropertyHelp::GetPropertyValue(tuning, 0xe9ac43e7, sPowerLineWidthFactors);
        cSCPropertyHelp::GetPropertyValue(tuning, 0x0a2eced8, snCostOfDemolition);
    }
    return true;
}

void cSC4PowerPoleOccupant::DrawStatics(std::vector<float>& widths, int textures[4]) {
    widths = sPowerLineWidthFactors;
    textures[0] = Texture(kPowerLineTexture);
    textures[1] = Texture(kPowerLineBuoyTexture);
    textures[2] = Texture(kFoundationTopTexture);
    textures[3] = Texture(kFoundationWallTexture);
}

// 0x100323f20
bool cSC4PowerPoleOccupant::StaticShutdown() {
    if (sbInited) {
        sbInited = false;
        sTexResIdToInfoMap.clear();
    }
    return true;
}

// 0x10032261a: the city's cell widths, the "occupant inserted" subscription and no model yet.
bool cSC4PowerPoleOccupant::Init() {
    cSC4Occupant::Init();
    if (IPowerPoleOccupantHost* host = g_poleHost) {
        sCellWidthX = host->CityCellWidthX();
        sCellWidthZ = host->CityCellWidthZ();
    }
    if (!subscribed_) {
        if (cIGZMessageServer2* server = DBSegmentMessageServer()) {
            server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kPowerPoleViewMessage);
            subscribed_ = true;
        }
    }
    if (model_) {
        s3d::cIS3DModelInstance* model = model_;
        model_ = nullptr;
        model->Release();
    }
    return true;
}

// 0x1003226c6 Init(cISCResExemplar*, int): a new pole takes the view's zoom and rotation, its direction mask and exemplar,
// loads its model and clears its foundation and highlight.
void cSC4PowerPoleOccupant::InitFromExemplar(cIGZUnknown* exemplar, std::uint32_t directionMask) {
    if (!sbInited) StaticInit();
    if (IsInitialized()) return;
    Init();
    IPowerPoleOccupantHost* host = g_poleHost;
    modelRotation_ = host ? host->ViewRotation() : 0;
    modelZoom_ = host ? host->ViewZoom() : 0;
    style_ = directionMask;
    SetExemplar(exemplar);
    LoadModel();
    InitConnectionPoints();
    floor_.clear();
    walls_[0].clear();
    walls_[1].clear();
    s3d::SC4DrawAttributes attributes = GetDrawAttributes();
    attributes.flags &= ~0xffffu;
    attributes.shade[3] = 1.0f;
    SetDrawAttributes(attributes);
    if (distressed_) SetDistressed(true);   // a distressed pole restarts its sparks
}

void cSC4PowerPoleOccupant::SetExemplar(cIGZUnknown* exemplar) {
    void* raw = nullptr;
    cISCResExemplar* resource = nullptr;
    if (exemplar && exemplar->QueryInterface(kGZIID_cISCResExemplar, &raw) && raw) {
        resource = static_cast<cISCResExemplar*>(raw);
    }
    SetDefaultExemplar(resource);
    if (resource) resource->Release();
}

// 0x100321e42: a new exemplar reloads the model and the connection points and re-hangs every wire on both ends.
void cSC4PowerPoleOccupant::SetDefaultExemplar(cISCResExemplar* exemplar) {
    if (GetDefaultExemplar() == exemplar) return;
    cSC4Occupant::SetDefaultExemplar(exemplar);
    LoadModel();
    InitConnectionPoints();
    for (std::size_t i = 0; i < connections_.size(); ++i) {
        cSC4PowerPoleOccupant* other = connections_[i].pole;
        UpdateConnection(other);
        if (other) other->UpdateConnection(this);
    }
}

// 0x1003229fa
bool cSC4PowerPoleOccupant::Shutdown() {
    BreakAllConnectionsAtDestination(true);
    boundsDirty_ = false;
    if (model_) {
        s3d::cIS3DModelInstance* model = model_;
        model_ = nullptr;
        model->Release();
    }
    floor_.clear();
    walls_[0].clear();
    walls_[1].clear();
    if (subscribed_) {
        if (cIGZMessageServer2* server = DBSegmentMessageServer())
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPowerPoleViewMessage);
        subscribed_ = false;
    }
    if (effect_) {
        cIGZUnknown* held = effect_;
        effect_ = nullptr;
        if (g_poleHost && g_poleHost->VisualEffectStarted(held)) g_poleHost->StopVisualEffect(held, 1);
        held->Release();
    }
    cSC4Occupant::Shutdown();
    return true;
}

// 0x100323b00 SetHighlight(int, bool): the model's highlight colour index (0 = none) in the draw attributes.
void cSC4PowerPoleOccupant::SetModelState(int highlight, bool immediate) {
    (void)immediate;
    s3d::SC4DrawAttributes attributes = GetDrawAttributes();
    attributes.flags = (attributes.flags & ~0xff00u) | ((static_cast<std::uint32_t>(highlight) & 0xffu) << 8);
    if (highlight == 0) attributes.flags &= ~1u;
    else attributes.flags |= 1u;
    SetDrawAttributes(attributes);
}

// 0x100323dbe / DoOccupantInserted 0x100323d68: when the city inserts this pole its wire occupants follow, once.
bool cSC4PowerPoleOccupant::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kPowerPoleViewMessage) {
        auto* occupant = static_cast<cISC4Occupant*>(static_cast<cIGZMessage2Standard*>(message)->GetVoid1());
        if (occupant == static_cast<cISC4Occupant*>(this)) {
            if (subscribed_) {
                if (cIGZMessageServer2* server = DBSegmentMessageServer())
                    server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kPowerPoleViewMessage);
                subscribed_ = false;
            }
            InsertAllPowerLineOccupants();
        }
    }
    return true;
}

void cSC4PowerPoleOccupant::GetPosition(float* xyz) {
    xyz[0] = position_[0];
    xyz[1] = position_[1];
    xyz[2] = position_[2];
}

void cSC4PowerPoleOccupant::CellOfPosition(std::uint32_t& x, std::uint32_t& z) const {
    x = ToCell(position_[0] / sCellWidthX);
    z = ToCell(position_[2] / sCellWidthZ);
}

// 0x100323b6c: the position, the model's translation, the cell, the foundation's floor (the cell's lowest corner) and
// its geometry.
void cSC4PowerPoleOccupant::SetPosition(const float* xyz) {
    position_[0] = xyz[0];
    position_[1] = xyz[1];
    position_[2] = xyz[2];
    SetModelPosition();
    IPowerPoleOccupantHost* host = g_poleHost;
    const float cellSize = host ? host->TerrainCellSize() : 16.0f;
    cellX_ = static_cast<std::int32_t>(ToCell(xyz[0] / cellSize));
    cellZ_ = static_cast<std::int32_t>(ToCell(xyz[2] / cellSize));
    float corners[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    if (host) host->TerrainCornerHeights(cellX_, cellZ_, corners);
    floorHeight_ = corners[0];
    for (int i = 1; i < 4; ++i)
        if (corners[i] < floorHeight_) floorHeight_ = corners[i];
    CreateFloor();
    CreateWalls();
}

// 0x100322088 InitConnectionPoints: the exemplar instance's direction class (bits 8..11): 1-2 cardinal, 3-4 diagonal.
void cSC4PowerPoleOccupant::InitConnectionPoints() {
    cISCResExemplar* exemplar = GetDefaultExemplar();
    if (!exemplar) return;
    cGZPersistResourceKey key{0, 0, 0};
    exemplar->GetKey(key);
    const std::uint32_t kind = (key.instance & 0xf00) >> 8;
    if (kind == 0) return;
    if (kind < 3) connectionPoints_ = kCardinalConnectionPoints;
    else if (kind < 5) connectionPoints_ = kDiagonalConnectionPoints;
}

// 0x10031faaa: the four wire points of one direction, in world space.
void cSC4PowerPoleOccupant::GetLineConnectionPoints(std::uint8_t direction, cS3DVector3 (&out)[4]) {
    const float (*table)[4][3] = connectionPoints_ ? connectionPoints_ : kCardinalConnectionPoints;
    for (int i = 0; i < 4; ++i) {
        const float* p = table[direction & 3][i];
        out[i].x = p[0] + position_[0];
        out[i].y = p[1] + position_[1];
        out[i].z = p[2] + position_[2];
    }
}

// 0x10032069e: the model {type, group from the exemplar's 0x27812821 key} whose instance the model maker utility picks for
// the pole's zoom, rotation and state (1 for the end poles, direction masks 1 and 8), placed at the pole and turned
// against the camera's yaw.
bool cSC4PowerPoleOccupant::LoadModel() {
    IPowerPoleOccupantHost* host = g_poleHost;
    cISCResExemplar* exemplar = GetDefaultExemplar();
    if (!host || !exemplar) return false;
    cGZPersistResourceKey key{0x5ad0e817, 0xbadb57f1, 0};
    cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x27812821, key);
    const int state = (style_ == 1 || style_ == 8) ? 1 : 0;
    key.instance = host->ModelInstanceID(static_cast<cISC4Occupant*>(this), modelZoom_, modelRotation_, state);
    s3d::cIS3DModelInstance* model = host->CreateModelInstance(key);
    if (!model) return false;
    if (model_) model_->Release();
    model_ = model;
    SetModelPosition();
    model_->SetOwner(static_cast<cISC4Occupant*>(this));
    cS3DTransform transform;
    model_->GetTransform(transform);
    nS3D::SetOrientationEuler(transform, cS3DVector3{0.0f, -host->CameraYaw(), 0.0f});
    model_->SetTransform(transform);
    return false;
}

// 0x100321ce6: the model sits at the pole's position plus the exemplar's model offset (property 0x4835c6a4).
void cSC4PowerPoleOccupant::SetModelPosition() {
    if (!model_) return;
    cS3DTransform transform;
    model_->GetTransform(transform);
    std::vector<float> offset;
    if (cISCResExemplar* exemplar = GetDefaultExemplar())
        cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x4835c6a4, offset);
    offset.resize(3, 0.0f);   // the original reads three entries unchecked
    nS3D::SetTranslation(transform, cS3DVector3{position_[0] + offset[0], position_[1] + offset[1], position_[2] + offset[2]});
    model_->SetTransform(transform);
}

// 0x1003208d6: the foundation's top, a 10 m square around the pole at the pole's height (turned 45 degrees under a diagonal
// pole model: key instance 0x0503xxxx / 0x0504xxxx).
void cSC4PowerPoleOccupant::CreateFloor() {
    static constexpr float kCorners[4][2] = {{-1.0f, -1.0f}, {-1.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}};
    bool diagonal = false;
    if (cISCResExemplar* exemplar = GetDefaultExemplar()) {
        cGZPersistResourceKey key{0, 0, 0};
        if (cSCPropertyHelp::GetPropertyValue(exemplar->AsISCPropertyHolder(), 0x27812821, key)) {
            const std::uint32_t kind = key.instance & 0xffff0000u;
            diagonal = kind == 0x05030000u || kind == 0x05040000u;
        }
    }
    const float c = std::cos(0.78539f), s = std::sin(0.78539f);
    floor_.clear();
    for (int i = 0; i < 4; ++i) {
        float x = kCorners[i][0] * 10.0f * 0.5f;
        float z = kCorners[i][1] * 10.0f * 0.5f;
        if (diagonal) {   // v * RotateY(pi/4)
            const float rx = x * c - z * s;
            const float rz = x * s + z * c;
            x = rx;
            z = rz;
        }
        UndergroundVertex v;
        v.x = position_[0] + x;
        v.y = position_[1];
        v.z = position_[2] + z;
        v.u = kTextureU[i];
        v.v = kTextureV[i];
        floor_.push_back(v);
    }
}

// 0x100320be8: the two foundation sides that face the camera, by the model's rotation.
void cSC4PowerPoleOccupant::CreateWalls() {
    static constexpr std::size_t kEdges[4][2] = {{0, 1}, {3, 0}, {2, 3}, {1, 2}};
    static constexpr int kVisible[4][2] = {{2, 3}, {3, 0}, {0, 1}, {1, 2}};
    walls_[0].clear();
    walls_[1].clear();
    if (floor_.size() < 4) return;
    const int rotation = modelRotation_ & 3;
    const int a = kVisible[rotation][0], b = kVisible[rotation][1];
    AddWallQuad(kEdges[a][0], kEdges[a][1], walls_[0]);
    AddWallQuad(kEdges[b][0], kEdges[b][1], walls_[1]);
}

// 0x100322408: one side from the top edge (from, to) down to the floor height; v runs 1/16 per metre of drop.
void cSC4PowerPoleOccupant::AddWallQuad(std::size_t from, std::size_t to, std::vector<UndergroundVertex>& out) {
    out.clear();
    const UndergroundVertex& a = floor_[from];
    const UndergroundVertex& b = floor_[to];
    UndergroundVertex v;
    v.color[0] = v.color[1] = v.color[2] = 0xff;
    v.color[3] = a.color[3];
    v.x = a.x; v.y = a.y; v.z = a.z; v.u = 0.0f; v.v = 0.0f;
    out.push_back(v);
    v.y = floorHeight_; v.u = 0.0f; v.v = (a.y - floorHeight_) * 0.0625f;
    out.push_back(v);
    v.color[3] = b.color[3];
    v.x = b.x; v.y = floorHeight_; v.z = b.z; v.u = 1.0f; v.v = (b.y - floorHeight_) * 0.0625f;
    out.push_back(v);
    v.y = b.y; v.u = 1.0f; v.v = 0.0f;
    out.push_back(v);
}

// 0x100321efc: the pole's own model instance, its transform carrying the position.
void cSC4PowerPoleOccupant::GetModelInstances(s3d::cIS3DModelInstance** out, int& count) {
    float position[3];
    GetPosition(position);
    count = 0;
    nS3D::SetTranslation(transform_, cS3DVector3{position[0], position[1], position[2]});
    out[count++] = static_cast<s3d::cIS3DModelInstance*>(this);
    AddRef();
}

// The cells the pole and the wires it draws cover; the height runs from the foundation's floor to 60 m above the pole.
void cSC4PowerPoleOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    if (boundsDirty_) {
        boundsMinX_ = boundsMaxX_ = static_cast<int>(position_[0] / sCellWidthX);
        boundsMinZ_ = boundsMaxZ_ = static_cast<int>(position_[2] / sCellWidthZ);
        for (const tConnection& connection : connections_) {
            if (connection.curvesA.empty()) continue;
            if (static_cast<std::uint32_t>(connection.otherX) < static_cast<std::uint32_t>(boundsMinX_)) boundsMinX_ = connection.otherX;
            else if (static_cast<std::uint32_t>(boundsMaxX_) < static_cast<std::uint32_t>(connection.otherX)) boundsMaxX_ = connection.otherX;
            if (static_cast<std::uint32_t>(connection.otherZ) < static_cast<std::uint32_t>(boundsMinZ_)) boundsMinZ_ = connection.otherZ;
            else if (static_cast<std::uint32_t>(boundsMaxZ_) < static_cast<std::uint32_t>(connection.otherZ)) boundsMaxZ_ = connection.otherZ;
        }
        boundsDirty_ = false;
    }
    minXyz[0] = static_cast<float>(boundsMinX_) * sCellWidthX;
    minXyz[1] = floorHeight_;
    minXyz[2] = static_cast<float>(boundsMinZ_) * sCellWidthZ;
    maxXyz[0] = static_cast<float>(boundsMaxX_ + 1) * sCellWidthX;
    maxXyz[1] = position_[1] + 60.0f;
    maxXyz[2] = static_cast<float>(boundsMaxZ_ + 1) * sCellWidthZ;
}

// 0x100322268: the model's bounds.
void cSC4PowerPoleOccupant::GetBounds(cS3DBoundingBox& out) {
    if (model_) model_->GetBounds(out);
}

void cSC4PowerPoleOccupant::GetWorldBounds(cS3DBoundingBox& out) { GetBoundingBox(out.min, out.max); }

IPowerPoleOccupant* cSC4PowerPoleOccupant::GetConnectionOccupant(int index) {
    return connections_[static_cast<std::size_t>(index)].pole;
}

// The four wire curves (and the river buoys when either end stands in water) from this pole to `other`.
void cSC4PowerPoleOccupant::BuildCurves(cSC4PowerPoleOccupant* other, std::uint8_t direction, bool overWater,
                                        tConnection& connection) {
    cS3DVector3 mine[4], theirs[4];
    GetLineConnectionPoints(direction, mine);
    other->GetLineConnectionPoints(direction, theirs);
    for (int i = 0; i < 4; ++i) {
        cS3DVector3 control0, control1;
        int segments = 0;
        GetPowerLineControlPoints(sTunables, mine[i], theirs[i], control0, control1, segments);
        std::vector<cS3DVector3> curve(static_cast<std::size_t>(segments > 1 ? segments : 1));
        curve[0] = mine[i];
        if (segments > 1) Bezier3D4ControlsArray(mine[i], control0, control1, theirs[i], &curve[1], segments - 1);
        connection.curvesA.push_back(std::move(curve));
        if (overWater) {
            std::vector<cS3DVector3> buoys(3);
            Bezier3D4ControlsArray(mine[i], control0, control1, theirs[i], buoys.data(), 3);
            connection.curvesB.push_back(std::move(buoys));
        }
    }
}

// 0x10031f4aa: a new connection to `pole` (none to itself or twice to the same cell). The pole with the lower cell (z first,
// then x) draws the wire: it builds the curves and keeps the wire occupants of `lines`.
void cSC4PowerPoleOccupant::AddConnection(cSC4PowerPoleOccupant* pole, std::vector<tLineInfo>* lines) {
    if (!pole) return;
    std::uint32_t otherX = 0, otherZ = 0;
    pole->GetCell(otherX, otherZ);
    for (const tConnection& connection : connections_)
        if (static_cast<std::uint32_t>(connection.otherX) == otherX && static_cast<std::uint32_t>(connection.otherZ) == otherZ) return;
    std::uint32_t x = 0, z = 0;
    CellOfPosition(x, z);
    if (x == otherX && z == otherZ) return;
    IPowerPoleOccupantHost* host = g_poleHost;
    const int direction = host ? host->Get0To3Direction(static_cast<int>(x), static_cast<int>(z), static_cast<int>(otherX), static_cast<int>(otherZ)) : 0;
    const bool drawsWire = z < otherZ || (z == otherZ && x < otherX);
    tConnection connection;
    connection.pole = pole;
    pole->AddRef();
    connection.drawsWire = drawsWire;
    connection.otherX = static_cast<std::int32_t>(otherX);
    connection.otherZ = static_cast<std::int32_t>(otherZ);
    const bool overWater = host && (host->CellHasWater(cellX_, cellZ_) || host->CellHasWater(static_cast<int>(otherX), static_cast<int>(otherZ)));
    if (drawsWire) {
        BuildCurves(pole, static_cast<std::uint8_t>(direction), overWater, connection);
        UpdateLineOccupants(connection, lines);
        PostOccupantMessage(kMessageOccupantChanged, 0);
    }
    boundsDirty_ = true;
    connections_.push_back(std::move(connection));
}

// 0x10031f406: the connection keeps a reference to every wire occupant of the segment.
void cSC4PowerPoleOccupant::UpdateLineOccupants(tConnection& connection, std::vector<tLineInfo>* lines) {
    if (!lines) return;
    for (tLineInfo& info : *lines) {
        cSC4PowerLineOccupant* line = info.line;
        if (line) line->AddRef();
        connection.lines.push_back(line);
    }
}

// 0x10031fe0c: re-hangs the wire this pole draws to `pole` (after an exemplar change at either end). Quirk kept: the
// buoys are appended again without clearing the old ones.
void cSC4PowerPoleOccupant::UpdateConnection(cSC4PowerPoleOccupant* pole) {
    if (!pole) return;
    std::uint32_t otherX = 0, otherZ = 0;
    pole->GetCell(otherX, otherZ);
    for (tConnection& connection : connections_) {
        if (connection.pole != pole || !connection.drawsWire) continue;
        std::uint32_t x = 0, z = 0;
        CellOfPosition(x, z);
        if (otherX == x && otherZ == z) return;
        connection.curvesA.clear();
        IPowerPoleOccupantHost* host = g_poleHost;
        const int direction = host ? host->Get0To3Direction(static_cast<int>(x), static_cast<int>(z), static_cast<int>(otherX), static_cast<int>(otherZ)) : 0;
        const bool overWater = host && (host->CellHasWater(cellX_, cellZ_) || host->CellHasWater(static_cast<int>(otherX), static_cast<int>(otherZ)));
        BuildCurves(pole, static_cast<std::uint8_t>(direction), overWater, connection);
        PostOccupantMessage(kMessageOccupantChanged, 0);
        boundsDirty_ = true;
        return;
    }
}

// 0x100320270: drops the other pole and takes the wire occupants out of the city.
void cSC4PowerPoleOccupant::RemoveConnection(tConnection& connection, bool quiet) {
    if (connection.pole) {
        connection.pole->Release();
        connection.pole = nullptr;
    }
    IPowerPoleOccupantHost* host = g_poleHost;
    for (cSC4PowerLineOccupant* line : connection.lines) {
        if (!line) continue;
        if (host) host->RemoveOccupant(static_cast<cISC4Occupant*>(line));
        line->Release();
    }
    connection.lines.clear();
    connection.linesInserted = false;
    if (connection.drawsWire && !quiet) PostOccupantMessage(kMessageOccupantChanged, 0);
    boundsDirty_ = true;
}

// 0x100320340
void cSC4PowerPoleOccupant::RemoveConnection(cSC4PowerPoleOccupant* pole, bool quiet) {
    if (!pole) return;
    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
        if (it->pole != pole) continue;
        RemoveConnection(*it, quiet);
        connections_.erase(it);
        return;
    }
}

// 0x1003204a4: hides the wire to `pole` (false) or gives it back to the pole with the lower cell (true).
void cSC4PowerPoleOccupant::DrawConnection(cSC4PowerPoleOccupant* pole, bool byCellOrder) {
    for (tConnection& connection : connections_) {
        if (connection.pole != pole) continue;
        bool draws = false;
        if (byCellOrder) {
            std::uint32_t otherX = 0, otherZ = 0;
            pole->GetCell(otherX, otherZ);
            const auto x = static_cast<std::uint32_t>(cellX_), z = static_cast<std::uint32_t>(cellZ_);
            draws = z < otherZ || (z == otherZ && x < otherX);
        }
        connection.drawsWire = draws;
        return;
    }
}

// 0x100320544: every connected pole forgets this one, and this one drops its connections (the entries stay).
std::int64_t cSC4PowerPoleOccupant::GetCostOfDemolition() const { return snCostOfDemolition; }

// i386 0x0025571c: the pole's model bounds at its position, widened by the foundation's two wall vertex lists; a ray that
// meets the box nearer than the pick so far takes it. Without a model nothing is picked.
bool cSC4PowerPoleOccupant::Pick(cSC4PickContext* context) {
    if (!context || !model_) return false;
    cS3DBoundingBox bounds;
    model_->GetBounds(bounds);
    float boxMin[3], boxMax[3];
    for (int i = 0; i < 3; ++i) {
        boxMin[i] = bounds.min[i] + position_[i];
        boxMax[i] = bounds.max[i] + position_[i];
    }
    for (const std::vector<UndergroundVertex>& wall : walls_)
        for (const UndergroundVertex& v : wall) {
            const float p[3] = {v.x, v.y, v.z};
            for (int i = 0; i < 3; ++i) {
                if (p[i] < boxMin[i]) boxMin[i] = p[i];
                if (boxMax[i] < p[i]) boxMax[i] = p[i];
            }
        }
    float hit[3] = {0.0f, 0.0f, 0.0f};
    if (!SC4GeometryUtilities::RayIntersectsBoundingBox(&context->origin.x, &context->direction.x, boxMin, boxMax, hit)) return false;
    const float distance = SC4GeometryUtilities::LengthOfLine(&context->origin.x, hit);
    if (distance < context->maxDistance) {
        context->maxDistance = distance;
        return true;
    }
    return false;
}

bool cSC4PowerPoleOccupant::SavedPointsMatchTable() const {
    if (!connectionPoints_) return false;
    for (int i = 0; i < 4; ++i)
        for (int k = 0; k < 3; ++k) {
            const cS3DVector3& p = savedPoints_[static_cast<std::size_t>(i)];
            const float saved = k == 0 ? p.x : k == 1 ? p.y : p.z;
            if (saved != connectionPoints_[0][i][k]) return false;
        }
    return true;
}

// i386 0x00255134: a pole the power funding leaves unmaintained sparks ("powerpolezap" at the pole's position); a maintained
// one stops its sparks and drops the effect.
void cSC4PowerPoleOccupant::SetDistressed(bool distressed) {
    distressed_ = distressed;
    if (distressed) {
        if (!effect_ && g_poleHost) {
            effect_ = g_poleHost->CreateVisualEffect("powerpolezap");
            if (effect_) g_poleHost->StartVisualEffect(effect_, position_);
        }
    } else {
        if (effect_ && g_poleHost && g_poleHost->VisualEffectStarted(effect_)) g_poleHost->StopVisualEffect(effect_, 0);
        if (effect_) {
            effect_->Release();
            effect_ = nullptr;
        }
    }
}

void cSC4PowerPoleOccupant::BreakAllConnectionsAtDestination(bool quiet) {
    for (std::size_t i = 0; i < connections_.size(); ++i) {
        if (cSC4PowerPoleOccupant* other = connections_[i].pole) other->RemoveConnection(this, false);
        RemoveConnection(connections_[i], quiet);
    }
}

// 0x100323ce8: the wire occupants of each connection go into the city, once.
void cSC4PowerPoleOccupant::InsertAllPowerLineOccupants() {
    IPowerPoleOccupantHost* host = g_poleHost;
    for (tConnection& connection : connections_) {
        if (connection.linesInserted) continue;
        for (cSC4PowerLineOccupant* line : connection.lines)
            if (host) host->InsertOccupant(line ? static_cast<cISC4Occupant*>(line) : nullptr);
        connection.linesInserted = true;
    }
}

// 0x10032203c
bool cSC4PowerPoleOccupant::GetWireCells(cSC4PowerPoleOccupant* pole, std::vector<SC4PointU>& out) {
    for (std::size_t i = 0; i < connections_.size(); ++i)
        if (connections_[i].pole == pole) return GetWireCells(static_cast<int>(i), out);
    return false;
}

// 0x100322088: the cells under a wire; the pole that draws it walks them.
bool cSC4PowerPoleOccupant::GetWireCells(int index, std::vector<SC4PointU>& out) {
    if (index < 0 || static_cast<std::size_t>(index) >= connections_.size()) return false;
    tConnection& connection = connections_[static_cast<std::size_t>(index)];
    if (!connection.drawsWire && connection.pole) return connection.pole->GetWireCells(this, out);
    out.clear();
    std::vector<WireCell> cells;
    GetPowerWireCells(position_[0] / sCellWidthX, position_[2] / sCellWidthZ, static_cast<std::uint32_t>(connection.otherX),
                      static_cast<std::uint32_t>(connection.otherZ), cells);
    for (const WireCell& cell : cells) out.push_back(SC4PointU{static_cast<std::uint32_t>(cell.x), static_cast<std::uint32_t>(cell.z)});
    return true;
}

// 0x1003205b8: a new zoom rebinds the textures; the model and the foundation follow the view.
void cSC4PowerPoleOccupant::ChangeZoomLevel(SC4DrawContext* context) {
    IPowerPoleOccupantHost* host = g_poleHost;
    if (context->Zoom() != sCurZoom) {
        sCurZoom = context->Zoom();
        for (auto& [instance, texture] : sTexResIdToInfoMap) texture = host ? host->TextureId(instance) : 0;
    }
    modelRotation_ = context->ViewRotation();
    modelZoom_ = context->Zoom();
    LoadModel();
    CreateFloor();
    CreateWalls();
}

// 0x1003211ac: the pole model, the foundation's top and its two visible sides (lit for their facing and tinted by the
// highlight colour) and the wires.
int cSC4PowerPoleOccupant::Draw(SC4DrawContext* context, bool highlight) {
    if (!context) return 1;
    if (!model_) {
        LoadModel();
        CreateFloor();
        CreateWalls();
    }
    cS3DVector3 tint{1.0f, 1.0f, 1.0f};
    if (highlight) {
        const std::size_t index = (GetDrawAttributes().flags >> 8) & 0xffu;
        const std::vector<cS3DVector4>& colors = context->HighlightColors();
        if (index < colors.size()) tint = cS3DVector3{colors[index].x, colors[index].y, colors[index].z};
    }
    if (context->Zoom() != sCurZoom || context->Zoom() != modelZoom_ || modelRotation_ != context->ViewRotation())
        ChangeZoomLevel(context);
    if (model_) {
        model_->SetDrawAttributes(GetDrawAttributes());
        model_->Draw(context, highlight);
    }
    context->SetRenderState(PoleRenderState());
    context->ResetModelViewTransform();
    IPowerPoleOccupantHost* host = g_poleHost;
    const auto shadeFor = [&](const std::vector<UndergroundVertex>& quad) {
        float p[3][3];
        for (int i = 0; i < 3; ++i) {
            p[i][0] = quad[static_cast<std::size_t>(i)].x;
            p[i][1] = quad[static_cast<std::size_t>(i)].y;
            p[i][2] = quad[static_cast<std::size_t>(i)].z;
        }
        cS3DVector3 normal;
        SC4GeometryUtilities::GetNormalToTriangle(p[0], p[1], p[2], &normal.x);
        nS3D::Normalize(normal);
        const cS3DVector3 light = host ? host->LightColor(normal, cS3DVector3{p[0][0], p[0][1], p[0][2]}) : cS3DVector3{1.0f, 1.0f, 1.0f};
        context->SetShade(cS3DVector3{light.x * tint.x, light.y * tint.y, light.z * tint.z});
    };
    if (floor_.size() >= 3) {
        shadeFor(floor_);
        context->SetTexFiltering(1, 1, 0);
        context->SetTexWrapModes(2, 2, 0);
        context->SetTexture(Texture(kFoundationTopTexture), 0);
        context->DrawPrims(6, 10, static_cast<int>(floor_.size()), floor_.data());
        context->ResetShade();
    }
    context->SetTexFiltering(1, 1, 0);
    context->SetTexWrapModes(3, 3, 0);
    context->SetTexture(Texture(kFoundationWallTexture), 0);
    for (const std::vector<UndergroundVertex>& wall : walls_) {
        if (wall.size() < 3) continue;
        shadeFor(wall);
        context->DrawPrims(6, 10, static_cast<int>(wall.size()), wall.data());
    }
    context->ResetShade();
    DrawPowerlines(context);
    return 1;
}

// 0x100321672: each wire this pole draws as a strip of flat quads along its curve (half width = the zoom's width factor,
// the texture repeating every 8 m), and the buoys over water as small squares at the curve points.
void cSC4PowerPoleOccupant::DrawPowerlines(SC4DrawContext* context) {
    const std::size_t zoom = static_cast<std::size_t>(modelZoom_ < 0 ? 0 : modelZoom_);
    if (zoom >= sPowerLineWidthFactors.size()) return;
    const float width = sPowerLineWidthFactors[zoom];
    if (!(0.0f < width)) return;
    for (const tConnection& connection : connections_) {
        if (!connection.drawsWire) continue;
        context->SetTexture(Texture(kPowerLineTexture), 0);
        for (const std::vector<cS3DVector3>& curve : connection.curvesA) {
            for (std::size_t i = 0; i + 1 < curve.size(); ++i) {
                const cS3DVector3& a = curve[i];
                const cS3DVector3& b = curve[i + 1];
                float dx = b.x - a.x, dz = b.z - a.z;
                const float length = std::sqrt(dx * dx + dz * dz);
                if (length > 0.0f) {
                    dx /= length;
                    dz /= length;
                }
                const float px = dz, pz = -dx;   // the wire's horizontal normal
                const float repeat = length * 0.125f;
                const WireVertex quad[4] = {
                    {a.x - width * px, a.y, a.z - width * pz, 0.0f, 0.0f},
                    {width * px + a.x, a.y, width * pz + a.z, 0.0f, 1.0f},
                    {width * px + b.x, b.y, width * pz + b.z, repeat, 1.0f},
                    {b.x - width * px, b.y, b.z - width * pz, repeat, 0.0f},
                };
                context->DrawPrims(6, 2, 4, quad);
            }
        }
        if (connection.curvesB.empty()) continue;
        context->SetTexture(Texture(kPowerLineBuoyTexture), 0);
        for (const std::vector<cS3DVector3>& curve : connection.curvesB) {
            for (std::size_t i = 0; i + 1 < curve.size(); ++i) {
                const cS3DVector3& p = curve[i];
                const WireVertex quad[4] = {
                    {width * -4.0f + p.x, p.y, width * -4.0f + p.z, 0.0f, 0.0f},
                    {width * -4.0f + p.x, p.y, width * 4.0f + p.z, 0.0f, 1.0f},
                    {width * 4.0f + p.x, p.y, width * 4.0f + p.z, 1.0f, 1.0f},
                    {width * 4.0f + p.x, p.y, width * -4.0f + p.z, 1.0f, 0.0f},
                };
                context->DrawPrims(6, 2, 4, quad);
            }
        }
    }
}

bool cSC4PowerPoleOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(kPowerPoleRecordVersion);
    if (cSC4Occupant::Write(stream)) {
        stream << transform_;
        for (float value : position_) stream.SetFloat32(value);
        stream.SetUint32(static_cast<std::uint32_t>(connections_.size()));
        for (const tConnection& connection : connections_) {
            SC4WriteGZSerializable(stream, connection.pole ? static_cast<const cIGZSerializable*>(connection.pole) : nullptr);
            stream.SetUint32(static_cast<std::uint32_t>(connection.lines.size()));
            for (const cSC4PowerLineOccupant* line : connection.lines)
                SC4WriteGZSerializable(stream, line ? static_cast<const cIGZSerializable*>(line) : nullptr);
            stream.SetUint8(connection.linesInserted ? 1 : 0);
            stream.SetUint8(connection.drawsWire ? 1 : 0);
            stream.SetSint32(connection.otherX);
            stream.SetSint32(connection.otherZ);
            WriteCurves(stream, connection.curvesA);
            WriteCurves(stream, connection.curvesB);
        }
        stream.SetUint32(style_);
        stream.SetSint32(modelRotation_);
        stream.SetSint32(modelZoom_);
        stream.SetSint32(cellX_);
        stream.SetSint32(cellZ_);
        // the first four points of the pole's connection point table (+0xe0 i386: the cardinal or diagonal table); the
        // original reads them back into that static table, the port keeps them in savedPoints_ (a pole without a table)
        for (int i = 0; i < 4; ++i) {
            if (connectionPoints_) stream << cS3DVector3{connectionPoints_[0][i][0], connectionPoints_[0][i][1], connectionPoints_[0][i][2]};
            else stream << savedPoints_[static_cast<std::size_t>(i)];
        }
        WriteVertices(stream, floor_);
        WriteVertices(stream, walls_[0]);
        WriteVertices(stream, walls_[1]);
        stream.SetFloat32(floorHeight_);
        stream.SetUint8(field18c_ ? 1 : 0);
        const s3d::SC4DrawAttributes attributes = GetDrawAttributes();
        stream.SetUint8(static_cast<std::uint8_t>(attributes.flags & 0xff));
        stream.SetUint8(static_cast<std::uint8_t>((attributes.flags >> 8) & 0xff));
        stream.SetFloat32(attributes.shade[3]);
        stream.SetUint8(distressed_ ? 1 : 0);
    }
    return !stream.GetError();
}

// 0x10032315c: a loaded pole sets up the class statics first, as a new one does in Init(exemplar, mask).
bool cSC4PowerPoleOccupant::Read(cIGZIStream& stream) {
    if (!sbInited) StaticInit();
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < 5) {   // PARTIAL: the legacy transform of versions 3 and 4 is not recovered
        SC4_UNWIRED("cSC4PowerPoleOccupant::Read: version 3 / 4 record (legacy transform) refused");
        return false;
    }
    if (cSC4Occupant::Read(stream)) {
        stream >> transform_;
        for (float& value : position_) stream.GetFloat32(value);
        std::uint32_t count = 0;
        stream.GetUint32(count);
        connections_.assign(count, tConnection{});
        for (tConnection& connection : connections_) {
            void* pole = nullptr;
            SC4ReadGZSerializable(stream, kGZCLSID_cSC4PowerPoleOccupantRecord, &pole, false);
            connection.pole = static_cast<cSC4PowerPoleOccupant*>(pole);
            std::uint32_t lines = 0;
            stream.GetUint32(lines);
            connection.lines.assign(lines, nullptr);
            for (cSC4PowerLineOccupant*& line : connection.lines) {
                void* occupant = nullptr;
                SC4ReadGZSerializable(stream, kGZCLSID_cSC4PowerLineOccupant, &occupant, false);
                line = static_cast<cSC4PowerLineOccupant*>(occupant);
            }
            std::uint8_t flag = 0;
            stream.GetUint8(flag);
            connection.linesInserted = flag != 0;
            stream.GetUint8(flag);
            connection.drawsWire = flag != 0;
            stream.GetSint32(connection.otherX);
            stream.GetSint32(connection.otherZ);
            ReadCurves(stream, connection.curvesA);
            ReadCurves(stream, connection.curvesB);
        }
        stream.GetUint32(style_);
        stream.GetSint32(modelRotation_);
        stream.GetSint32(modelZoom_);
        stream.GetSint32(cellX_);
        stream.GetSint32(cellZ_);
        for (cS3DVector3& point : savedPoints_) stream >> point;
        InitConnectionPoints();
        ReadVertices(stream, floor_);
        ReadVertices(stream, walls_[0]);
        ReadVertices(stream, walls_[1]);
        stream.GetFloat32(floorHeight_);
        std::uint8_t flag = 0;
        stream.GetUint8(flag);
        field18c_ = flag != 0;
        s3d::SC4DrawAttributes attributes = GetDrawAttributes();
        std::uint8_t flags0 = 0, flags1 = 0;
        stream.GetUint8(flags0);
        stream.GetUint8(flags1);
        attributes.flags = (attributes.flags & ~0xffffu) | flags0 | (static_cast<std::uint32_t>(flags1) << 8);
        stream.GetFloat32(attributes.shade[3]);
        SetDrawAttributes(attributes);
        stream.GetUint8(flag);
        distressed_ = flag != 0;   // +0x10e (i386): the record's last byte is the distressed flag, not the preview flag
        if (model_ != nullptr) {
            s3d::cIS3DModelInstance* model = model_;
            model_ = nullptr;
            model->Release();
        }
    }
    return !stream.GetError();
}

}  // namespace sc4
