#include "sc4/UndergroundConnectionOccupant.h"

#include <cmath>

#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/ViewUtilities.h"

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

template <class T>
void ReleaseAndClear(T*& held) {
    if (held) {
        T* object = held;
        held = nullptr;
        object->Release();
    }
}


IUndergroundConnectionHost* g_host = nullptr;

// kTextureU / kTextureV (0x008486c0 / 0x008486b0): the trench floor quad's texture coordinates, corner by corner.
constexpr float kTextureU[4] = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr float kTextureV[4] = {0.0f, 1.0f, 1.0f, 0.0f};

// The eight neighbor offsets Init tries for a piece to join (x, z), straight ones first.
constexpr int kNeighborOffsets[8][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}, {-1, -1}, {1, -1}, {1, 1}, {-1, 1}};

cIGZMessageServer2* MessageServer() { return DBSegmentMessageServer(); }   // SL::spMS2

IUndergroundConnectionViewHost* g_viewHost = nullptr;
bool sbPieceConnectionsInited = false;                  // 0x00ab98a0
std::int32_t sCurZoom = 0;                               // 0x00c20e54
cSC4PackedRenderState mTrenchPackedRenderState;         // 0x00c20e30
cSC4PackedRenderState mPiecePackedRenderState;          // 0x00c20e28
cSC4PackedRenderState mHighlightPackedRenderState;      // 0x00c20e20

// sWateredStateRGBs (0x00c09940): the shaft's self-lit shade, dry and watered (Init sets the watered one).
const cS3DVector3 kWateredStateRGBs[2] = {{0.8f, 0.23f, 0.23f}, {1.0f, 1.0f, 1.0f}};
constexpr std::uint32_t kBuildingOccupantType = 0x278128a0;
constexpr std::uint32_t kMessageViewZoomRotation = 0xa6b79602;   // void1 the old {zoom, rotation}, void2 the new
constexpr std::uint8_t kHighlightColor[4] = {0x7f, 0x87, 0xe8, 0x64};   // the word 0x64e8877f
// The walls CreateWalls adds (corner pairs, all four sides of the cell).
constexpr std::uint32_t kWallCorners[4][2] = {{1, 0}, {0, 3}, {3, 2}, {2, 1}};

// cS3DMatrix4 from a cS3DMatrix3::RotateY of the identity (rows), the translation column cleared.
cS3DMatrix4 RotationY(float radians) {
    cS3DMatrix4 m;
    const float c = std::cos(radians), s = std::sin(radians);
    m.m[0][0] = c;
    m.m[0][2] = s;
    m.m[2][0] = -s;
    m.m[2][2] = c;
    return m;
}

void SetTranslation(cS3DMatrix4& m, float x, float y, float z) {   // cS3DMatrix4::SetColumn(3, {x, y, z, 1})
    m.m[0][3] = x;
    m.m[1][3] = y;
    m.m[2][3] = z;
    m.m[3][3] = 1.0f;
}

template <class T>
void ReleaseBinding(T*& binding) {
    if (binding) binding->Release();
    binding = nullptr;
}

}  // namespace

void SetUndergroundConnectionHost(IUndergroundConnectionHost* host) { g_host = host; }
void SetUndergroundConnectionViewHost(IUndergroundConnectionViewHost* host) { g_viewHost = host; }
const cSC4PackedRenderState& cSC4UndergroundConnectionOccupant::TrenchPackedRenderState() { return mTrenchPackedRenderState; }

cSC4UndergroundConnectionOccupant::cSC4UndergroundConnectionOccupant() { SetProperties(3); }

cSC4UndergroundConnectionOccupant::~cSC4UndergroundConnectionOccupant() {
    ReleaseAndClear(network_);
    ReleaseAndClear(building_);
}

bool cSC4UndergroundConnectionOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4PreBuiltUndergroundView || iid == kGZCLSID_cSC4UndergroundConnectionOccupant) {
        *out = static_cast<cSC4UndergroundConnectionOccupant*>(this);
        AddRef();
        return true;
    }
    if (cSC4Occupant::QueryInterface(iid, out)) return true;
    return s3d::cS3DModelInstanceBasic::QueryInterface(iid, out);
}

// 0x0022f87a: the statics, the occupant, the packed render states and the four view messages.
bool cSC4UndergroundConnectionOccupant::Init() {
    if (!sbPieceConnectionsInited) StaticInit();
    if (!IsInitialized()) {
        cSC4Occupant::Init();
        SetupPackedRenderState();
        if (cIGZMessageServer2* server = MessageServer(); server && !subscribed_) {
            for (std::uint32_t message : kUndergroundConnectionViewMessages)
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
            subscribed_ = true;
        }
    }
    return true;
}

// 0x0022f916. A loaded connection keeps its stored pieces; a new one drops them. The cell comes from the position, the
// trench floor quad from the cell's corners (the terrain heights less the trench depth, the cell's corner position and the
// city's cell widths). The connection then takes the network piece of its own cell; without one, every neighbor (straight
// ones first) holding a piece or a connection of the same network gets a piece laid from this cell to it by the network
// manager's tool (a new connection only), and the cell's piece is looked up again. A piece found is told to rebuild; the
// bounds follow and the view messages are subscribed.
bool cSC4UndergroundConnectionOccupant::Init(std::uint32_t gziid, const tUndergroundConnectionGeometryInfo* info) {
    if (IsInitialized()) return true;
    cSC4Occupant::Init();
    gziid_ = gziid;
    geometryInfo_ = info;
    if (!loaded_) {
        ReleaseAndClear(network_);
        ReleaseAndClear(building_);
    }
    float position[3] = {};
    GetPosition(position);
    float cornerX = 0.0f, cornerZ = 0.0f, widthX = 0.0f, widthZ = 0.0f;
    if (g_host) {
        g_host->PositionToCell(position[0], position[2], cellX_, cellZ_);
        g_host->CellCornerHeights(cellX_, cellZ_, cornerHeights_);
        g_host->CellCornerToPosition(cellX_, cellZ_, cornerX, cornerZ);
        widthX = static_cast<float>(static_cast<int>(g_host->CellWidthX()));
        widthZ = static_cast<float>(static_cast<int>(g_host->CellWidthZ()));
    }
    modelY_ = position[1];
    trenchFloorY_ = position[1] - kTrenchFloorDepth;
    const float cornerXs[4] = {cornerX, cornerX, cornerX + widthX, cornerX + widthX};
    const float cornerZs[4] = {cornerZ, cornerZ + widthZ, cornerZ + widthZ, cornerZ};
    for (int i = 0; i < 4; ++i) {
        floor_[i].x = cornerXs[i];
        floor_[i].y = cornerHeights_[i] - kTrenchFloorDepth;
        floor_[i].z = cornerZs[i];
        floor_[i].u = kTextureU[i];
        floor_[i].v = kTextureV[i];
    }
    value174_ = 1;
    modelDirty_ = true;
    ReleaseAndClear(network_);
    const std::uint32_t pieceId = GetNetworkOccupantGZIID();
    if (g_host) network_ = g_host->FindUndergroundOccupant(cellX_, cellZ_, pieceId);
    if (!network_ && g_host) {
        for (const auto& offset : kNeighborOffsets) {
            const int x = cellX_ + offset[0], z = cellZ_ + offset[1];
            bool join = false;
            if (cSC4UndergroundOccupant* piece = g_host->FindUndergroundOccupant(x, z, pieceId)) {
                piece->Release();
                join = true;
            } else if (cSC4UndergroundConnectionOccupant* other = g_host->FindUndergroundConnectionOccupant(x, z, pieceId)) {
                other->Release();
                join = true;
            }
            if (join && !loaded_) g_host->PlaceNetworkOccupants(GetNetworkType(), cellX_, cellZ_, x, z);
        }
        network_ = g_host->FindUndergroundOccupant(cellX_, cellZ_, pieceId);
    }
    if (network_) UpdateNetworkOccupant();
    ComputeBoundingBox(bounds_, bounds_ + 3);
    if (cIGZMessageServer2* server = MessageServer(); server && !subscribed_) {
        for (std::uint32_t message : kUndergroundConnectionViewMessages)
            server->AddNotification(static_cast<cIGZMessageTarget2*>(this), message);
        subscribed_ = true;
    }
    return true;
}

// 0x0022ff94
bool cSC4UndergroundConnectionOccupant::Shutdown() {
    ReleaseBinding(floorTexture_);
    ReleaseBinding(wallTexture_);
    ReleaseBinding(pieceTexture_);
    for (auto& wall : walls_) wall.clear();
    trench_.clear();
    highlight_.clear();
    piece_.clear();
    ReleaseAndClear(topModel_);
    ReleaseAndClear(bottomModel_);
    ReleaseAndClear(network_);
    ReleaseAndClear(building_);
    if (subscribed_) {
        if (cIGZMessageServer2* server = MessageServer())
            for (std::uint32_t message : kUndergroundConnectionViewMessages)
                server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), message);
        subscribed_ = false;
    }
    cSC4Occupant::Shutdown();
    return true;
}

void cSC4UndergroundConnectionOccupant::GetPosition(float* xyz) {
    xyz[0] = position_[0];
    xyz[1] = position_[1];
    xyz[2] = position_[2];
}

void cSC4UndergroundConnectionOccupant::SetPosition(const float* xyz) {
    position_[0] = xyz[0];
    position_[1] = xyz[1];
    position_[2] = xyz[2];
}

// 0x0022f174 / 0x0022f280: the position widened by the trench floor quad; the top is the highest terrain corner.
void cSC4UndergroundConnectionOccupant::ComputeBoundingBox(float* minXyz, float* maxXyz) {
    GetPosition(minXyz);
    GetPosition(maxXyz);
    for (int i = 0; i < 4; ++i) {
        if (floor_[i].x < minXyz[0]) minXyz[0] = floor_[i].x;
        if (floor_[i].y < minXyz[1]) minXyz[1] = floor_[i].y;
        if (floor_[i].z < minXyz[2]) minXyz[2] = floor_[i].z;
        if (floor_[i].x > maxXyz[0]) maxXyz[0] = floor_[i].x;
        if (cornerHeights_[i] > maxXyz[1]) maxXyz[1] = cornerHeights_[i];
        if (floor_[i].z > maxXyz[2]) maxXyz[2] = floor_[i].z;
    }
}

void cSC4UndergroundConnectionOccupant::GetBoundingBox(float* minXyz, float* maxXyz) { ComputeBoundingBox(minXyz, maxXyz); }

void cSC4UndergroundConnectionOccupant::GetWorldBounds(cS3DBoundingBox& out) { GetBoundingBox(out.min, out.max); }

// 0x0022f3da: the bounds are recomputed and a visible connection tells the renderer its model changed.
void cSC4UndergroundConnectionOccupant::ModelStateChanged() {
    if (modelDirty_) return;
    modelDirty_ = true;
    ComputeBoundingBox(bounds_, bounds_ + 3);
    if ((GetFlags() & 2) != 0 && g_host) g_host->ChangeModelInRenderer(this, 3);
}

// 0x0022f4e0: the network piece rebuilds (with its neighbors).
void cSC4UndergroundConnectionOccupant::UpdateNetworkOccupant() {
    if (auto* piece = dynamic_cast<cSC4UndergroundOccupant*>(network_)) piece->ModelStateChanged(true);
}

// 0x0022f508: a new building is held; losing the building takes the connection out of the occupant manager.
void cSC4UndergroundConnectionOccupant::SetBuildingOccupant(cISC4Occupant* building) {
    if (building_ == building) return;
    if (building_) building_->Release();
    building_ = building;
    if (!building) {
        if (g_host) g_host->RemoveOccupant(this, true, 0);
    } else {
        building->AddRef();
    }
}

// 0x0022f5aa: after the occupant message, an insertion into (removal from) the occupant manager is posted as the
// underground message 0x8b87a974 (0x8b87a975) with the connection as its object.
void cSC4UndergroundConnectionOccupant::PostOccupantMessage(std::uint32_t message, std::uint32_t data) {
    cSC4Occupant::PostOccupantMessage(message, data);
    if ((message | 1u) != kMessageOccupantRemovedFromManager) return;
    cIGZMessageServer2* server = MessageServer();
    if (!server) return;
    auto* msg = new cRZMessage2Standard();
    msg->AddRef();
    msg->SetType(message == kMessageOccupantInserted ? kMessageUndergroundInserted : kMessageUndergroundRemoved);
    cISC4Occupant* self = static_cast<cSC4Occupant*>(this);
    msg->SetVoid1(self);
    msg->SetIGZUnknown(self);
    msg->SetData2(IsFlagSet(kOccupantFlagVisible) ? 1 : 0);
    msg->SetData3(GetType());
    msg->SetData4(data);
    server->MessagePost(msg, false);
    msg->Release();
}

// 0x0023197a. An underground piece removed from this connection's cell is let go (the model is dirty again and the trench
// floor drops back to the position's depth). The building removed takes the connection with it: out of the occupant
// manager, its piece rebuilt, shut down. A piece of this network inserted on this cell becomes its piece; the connection's
// own insertion has its piece rebuild its walls. A view rotation (message 0xa6b79602 whose old and new rotation differ) rebuilds
// the drawn geometry.
bool cSC4UndergroundConnectionOccupant::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    if (type == kMessageUndergroundRemoved) {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        cSC4UndergroundOccupant* piece = nullptr;
        if (!occupant || !QueryOccupantInterface(occupant, piece) || !piece) return true;
        if (network_ && dynamic_cast<cSC4UndergroundOccupant*>(network_) == piece) {
            ReleaseAndClear(network_);
            modelDirty_ = true;
            float position[3] = {};
            GetPosition(position);
            trenchFloorY_ = position[1] - kTrenchFloorDepth;
        }
        piece->Release();
    } else if (type == kMessageBuildingRemoved) {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        if (!occupant || occupant != building_) return true;
        ReleaseAndClear(building_);
        AddRef();   // the occupant manager may hold the last reference
        if (g_host) g_host->RemoveOccupant(this, false, 0);
        UpdateNetworkOccupant();
        Shutdown();
        Release();
    } else if (type == kMessageUndergroundInserted) {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        cSC4UndergroundOccupant* piece = nullptr;
        if (!occupant || !QueryOccupantInterface(occupant, piece) || !piece) {
            if (occupant == static_cast<cISC4Occupant*>(static_cast<cSC4Occupant*>(this)))
                if (auto* own = dynamic_cast<cSC4UndergroundOccupant*>(network_)) own->CreateWalls(true);
            return true;
        }
        std::uint32_t x = 0, z = 0;
        piece->GetOccupiedCell(x, z);
        if (static_cast<std::uint32_t>(cellX_) == x && static_cast<std::uint32_t>(cellZ_) == z) {
            if (network_) network_->Release();
            network_ = piece;
            network_->AddRef();
            ModelStateChanged();
        }
        piece->Release();
    } else if (type == kMessageViewZoomRotation) {
        const auto* before = static_cast<const std::int32_t*>(standard->GetVoid1());
        const auto* after = static_cast<const std::int32_t*>(standard->GetVoid2());
        if (before && after && before[1] != after[1]) CreateGeometry();   // the original reads both without a check
    }
    return true;
}

// 0x002f01fa / 0x002f0436
bool cSC4UndergroundConnectionOccupant::QueryPieceInterface(cISC4Occupant* occupant, std::uint32_t pieceId,
                                                            cSC4UndergroundOccupant*& out) {
    out = nullptr;
    void* raw = nullptr;
    if (!occupant || !occupant->QueryInterface(pieceId, &raw) || !raw) return false;
    auto* piece = static_cast<cSC4UndergroundOccupant*>(raw);
    void* underground = nullptr;
    const bool found = piece->QueryInterface(kGZIID_cISC4UndergroundOccupant3, &underground) && underground;
    if (found) out = static_cast<cSC4UndergroundOccupant*>(underground);
    piece->Release();
    return found;
}

// 0x00230344
bool cSC4UndergroundConnectionOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(kUndergroundConnectionRecordVersion);
    if (cSC4Occupant::Write(stream)) {
        stream.SetUint32(wallCount_);
        for (const auto& wall : walls_) WriteVertices(stream, wall);
        WriteVertices(stream, trench_);
        WriteVertices(stream, highlight_);
        WriteVertices(stream, piece_);
        stream.SetSint32(cellX_);
        stream.SetSint32(cellZ_);
        for (float height : cornerHeights_) stream.SetFloat32(height);
        stream.SetFloat32(modelY_);
        stream.SetSint32(value174_);
        stream.SetUint8(modelDirty_ ? 1 : 0);
        SC4WriteGZSerializable(stream, network_);
        SC4WriteGZSerializable(stream, building_);
        stream.SetUint32(gziid_);
        for (float value : position_) stream.SetFloat32(value);
    }
    return !stream.GetError();
}

// 0x0023089a: as Write; the model is dirty and the connection counts as loaded.
bool cSC4UndergroundConnectionOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kUndergroundConnectionRecordVersion) return false;
    if (cSC4Occupant::Read(stream)) {
        stream.GetUint32(wallCount_);
        for (auto& wall : walls_) ReadVertices(stream, wall);
        ReadVertices(stream, trench_);
        ReadVertices(stream, highlight_);
        ReadVertices(stream, piece_);
        stream.GetSint32(cellX_);
        stream.GetSint32(cellZ_);
        for (float& height : cornerHeights_) stream.GetFloat32(height);
        stream.GetFloat32(modelY_);
        stream.GetSint32(value174_);
        std::uint8_t flag = 0;
        stream.GetUint8(flag);
        ReleaseAndClear(network_);
        ReleaseAndClear(building_);
        void* network = nullptr;
        SC4ReadGZSerializable(stream, kUndergroundConnectionOccupantIID, &network, false);
        network_ = static_cast<cISC4Occupant*>(network);
        void* building = nullptr;
        SC4ReadGZSerializable(stream, kUndergroundConnectionOccupantIID, &building, false);
        building_ = static_cast<cISC4Occupant*>(building);
        stream.GetUint32(gziid_);
        for (float& value : position_) stream.GetFloat32(value);
        modelDirty_ = true;
        loaded_ = true;
    }
    return !stream.GetError();
}

// ------------------------------------------------------------------------------------------------------------- drawing

// 0x0022f0f2 (the resource manager reference it takes is the view host's).
bool cSC4UndergroundConnectionOccupant::StaticInit() {
    sbPieceConnectionsInited = true;
    return true;
}

// 0x0022f6e8: the same three packed states as the underground pieces'.
void cSC4UndergroundConnectionOccupant::SetupPackedRenderState() {
    if (!g_viewHost) return;   // the original tests SL::spRenderer
    g_viewHost->PrepareRenderer();
    mTrenchPackedRenderState.PackRenderState(true, true, true, false, false, 1, false, 3, -1, -1, -1, -1.0f, 1, -1);
    mPiecePackedRenderState.PackRenderState(true, true, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);
    mHighlightPackedRenderState.PackRenderState(true, true, true, false, true, 0, false, 3, 4, 5, -1, -1.0f, 1, -1);
}

// 0x0022fefc: the zoom is kept; the per-zoom texture table it then re-binds (sTexResIdToInfoMap, 0x00c20e40) has no writer in the
// original, so there is nothing to re-bind.
void cSC4UndergroundConnectionOccupant::ChangeZoomLevel(SC4DrawContext* context) {
    if (context) sCurZoom = context->Zoom();
}

void cSC4UndergroundConnectionOccupant::GetModelInstances(int, int, s3d::cIS3DModelInstance** out, int capacity, int& count) {
    count = 0;
    if (!out || capacity < 1) return;
    count = 1;
    out[0] = static_cast<s3d::cS3DModelInstanceBasic*>(this);
    out[0]->AddRef();
}

// 0x002315e4. The shaft's transform: turned to face the camera when the table asks for it, at the connection's position and
// height; the models' transform: turned with the view rotation. Then the shaft, the walls and the trench floor.
void cSC4UndergroundConnectionOccupant::CreateGeometry() {
    pieceMatrix_ = cS3DMatrix4{};
    if (geometryInfo_ && geometryInfo_->faceCamera && g_viewHost) {
        float yaw = 0.0f, pitch = 0.0f, screenPitch = 0.0f;
        g_viewHost->GetCameraViewAngles(yaw, pitch, screenPitch);
        pieceMatrix_ = RotationY(-yaw);
    }
    SetTranslation(pieceMatrix_, position_[0], modelY_, position_[2]);
    int zoom = 0, rotation = 0;
    if (g_viewHost) g_viewHost->GetZoomAndRotation(zoom, rotation);
    viewMatrix_ = RotationY(SC4ViewUtilities::GetObjectYRotationAngle(rotation));
    SetTranslation(viewMatrix_, position_[0], modelY_, position_[2]);
    CreatePieceModel();
    CreateWalls();
    CreateFloor();
}

// 0x00230c2e. The shaft: the table's vertices with the top ones at the connection's height (the building's, when there is
// one) and the bottom ones down to the trench floor (the network piece's model height, else the trench depth below) and the
// table's offset further; u runs across the shaft (or one per vertex pair for a quad strip), v down it, both in units of the
// shaft's larger width measured from the origin. Then the two models and the wall and floor textures.
void cSC4UndergroundConnectionOccupant::CreatePieceModel() {
    piece_.clear();
    ReleaseBinding(pieceTexture_);
    ReleaseAndClear(topModel_);
    ReleaseAndClear(bottomModel_);
    const tUndergroundConnectionGeometryInfo* info = geometryInfo_;
    if (!info) return;
    if (g_viewHost) pieceTexture_ = g_viewHost->CreateTextureBinding(kTextureBindingGroupZoom, info->pieceTexture);
    for (std::uint32_t i = 0; i < info->vertexCount; ++i) piece_.push_back(info->vertices[i]);
    cISC4Occupant* building = building_;
    if (!building && g_viewHost) building = g_viewHost->FindBuildingOccupant(cellX_, cellZ_);
    if (building) {
        float position[3] = {};
        building->GetPosition(position);
        modelY_ = position[1];
    }
    cIGZUnknown* owner = static_cast<cISC4Occupant*>(static_cast<cSC4Occupant*>(this));
    if (g_viewHost) {
        topModel_ = g_viewHost->CreateModel(info->topModel);
        if (topModel_) topModel_->SetOwner(owner);
        bottomModel_ = g_viewHost->CreateModel(info->bottomModel);
        if (bottomModel_) bottomModel_->SetOwner(owner);
    }
    for (std::uint32_t i = 0; i < info->indexCount; ++i) piece_[info->topIndices[i]].y = 0.0f;
    if (auto* piece = dynamic_cast<cSC4UndergroundOccupant*>(network_)) {
        std::uint32_t pieceId = 0;
        int networkTypeId = 0;
        float heights[4] = {};
        piece->GetQueryData(pieceId, networkTypeId, heights, trenchFloorY_);
    } else {
        trenchFloorY_ = modelY_ - kTrenchFloorDepth;
    }
    for (std::uint32_t i = 0; i < info->indexCount; ++i)
        piece_[info->bottomIndices[i]].y = (trenchFloorY_ - modelY_) - info->bottomOffset;
    float minX = 0.0f, maxX = 0.0f, minZ = 0.0f, maxZ = 0.0f;   // the original starts the extent at the origin
    for (const UndergroundVertex& v : piece_) {
        minX = v.x < minX ? v.x : minX;
        maxX = v.x > maxX ? v.x : maxX;
        minZ = v.z < minZ ? v.z : minZ;
        maxZ = v.z > maxZ ? v.z : maxZ;
    }
    const float widthX = maxX - minX, widthZ = maxZ - minZ;
    const float extent = std::fabs(widthZ <= widthX ? widthX : widthZ);
    SetTranslation(pieceMatrix_, position_[0], modelY_, position_[2]);
    const float half = extent * 0.5f;
    const float (*m)[4] = pieceMatrix_.m;
    for (std::size_t i = 0; i < piece_.size(); ++i) {
        UndergroundVertex& v = piece_[i];
        const float x = v.x, y = v.y, z = v.z;
        v.u = info->quadStrip ? static_cast<float>(static_cast<int>(i) / 2) : (half + x) / extent;
        v.v = std::fabs(y) / extent;
        v.x = x * m[0][0] + y * m[0][1] + z * m[0][2] + m[0][3];
        v.y = x * m[1][0] + y * m[1][1] + z * m[1][2] + m[1][3];
        v.z = x * m[2][0] + y * m[2][1] + z * m[2][2] + m[2][3];
        v.color[0] = v.color[1] = v.color[2] = v.color[3] = 0xff;
    }
    ReleaseBinding(floorTexture_);
    ReleaseBinding(wallTexture_);
    if (g_viewHost) {
        floorTexture_ = g_viewHost->CreateTextureBinding(kTextureBindingGroupPiece, info->floorTexture);
        wallTexture_ = g_viewHost->CreateTextureBinding(kTextureBindingGroupPiece, info->wallTexture);
    }
    ModelStateChanged();
}

// 0x00231386: one wall quad from corner a to corner b, from the terrain down to the trench floor. Quirk kept: the far corner's
// lower v uses the near corner's depth.
void cSC4UndergroundConnectionOccupant::AddWallQuad(std::uint32_t a, std::uint32_t b, std::vector<UndergroundVertex>& out) {
    const float depth = (cornerHeights_[a] - floor_[a].y) * 0.0625f;
    auto vertex = [](const UndergroundVertex& corner, float y, float u, float v) {
        UndergroundVertex out;
        out.x = corner.x;
        out.y = y;
        out.z = corner.z;
        out.color[0] = out.color[1] = out.color[2] = 0xff;
        out.color[3] = corner.color[3];
        out.u = u;
        out.v = v;
        return out;
    };
    out.push_back(vertex(floor_[a], cornerHeights_[a], 0.0f, 0.0f));
    out.push_back(vertex(floor_[a], floor_[a].y, 0.0f, depth));
    out.push_back(vertex(floor_[b], floor_[b].y, 1.0f, depth));
    out.push_back(vertex(floor_[b], cornerHeights_[b], 1.0f, 0.0f));
}

// 0x00231506: the four walls of the cell, only when no network piece is under it (the piece has its own).
void cSC4UndergroundConnectionOccupant::CreateWalls() {
    for (auto& wall : walls_) wall.clear();
    if (network_) return;
    wallCount_ = 0;
    for (const auto& corners : kWallCorners) {
        AddWallQuad(corners[0], corners[1], walls_[wallCount_]);
        ++wallCount_;
    }
    ModelStateChanged();
}

// 0x00230b96: the trench floor quad, only when no network piece is under the cell.
void cSC4UndergroundConnectionOccupant::CreateFloor() {
    trench_.clear();
    if (network_) return;
    for (const UndergroundVertex& corner : floor_) {
        UndergroundVertex v = corner;
        v.color[0] = v.color[1] = v.color[2] = 0xff;
        trench_.push_back(v);
    }
    ModelStateChanged();
}

// 0x00230aa4: the highlight quad over the cell at the terrain's height, corners 1, 0, 2, 3.
void cSC4UndergroundConnectionOccupant::CreateHighlight() {
    highlight_.clear();
    for (std::uint32_t corner : {1u, 0u, 2u, 3u}) {
        UndergroundVertex v;
        v.x = floor_[corner].x;
        v.y = cornerHeights_[corner];
        v.z = floor_[corner].z;
        for (int c = 0; c < 4; ++c) v.color[c] = kHighlightColor[c];
        v.u = floor_[corner].u;
        v.v = floor_[corner].v;
        highlight_.push_back(v);
    }
    ModelStateChanged();
}

// 0x00230b60
void cSC4UndergroundConnectionOccupant::SetHighlight(int highlight, bool) {
    if (highlight == 0) highlight_.clear();
    else CreateHighlight();
    ModelStateChanged();
}

// 0x00231ba6. A changed connection rebuilds first. The highlight pass draws the highlight quad. Render pass 0 draws the trench
// floor and the walls; pass 1 the shaft (one strip, or quad by quad each shaded by the light on its face) under the watered
// shade, then the top model at the connection's height and the bottom model at the trench floor, turned with the view. A
// pass other than 0 clears the change.
int cSC4UndergroundConnectionOccupant::Draw(SC4DrawContext* context, bool highlight) {
    if (!context) return 1;
    if (modelDirty_) CreateGeometry();
    if (highlight) {
        if (!highlight_.empty()) {
            context->ResetModelViewTransform();
            context->SetRenderState(mHighlightPackedRenderState);
            context->DrawPrims(1, 0x0a, static_cast<int>(highlight_.size()), highlight_.data());
        }
        return 1;
    }
    if (context->RenderPass() == 0) {
        if (floorTexture_ && !trench_.empty()) {
            const std::uint32_t texture = floorTexture_->TextureIds()[0];
            context->ResetModelViewTransform();
            context->SetRenderState(mTrenchPackedRenderState);
            context->SetTexture(static_cast<int>(texture), 0);
            context->SetTexWrapModes(2, 2, 0);
            context->SetTexFiltering(1, 1, 0);
            context->DrawPrims(6, 0x0a, static_cast<int>(trench_.size()), trench_.data());
        }
        if (wallTexture_ && wallCount_ != 0) {
            const std::uint32_t texture = wallTexture_->TextureIds()[0];
            context->ResetModelViewTransform();
            context->SetRenderState(mTrenchPackedRenderState);
            context->SetTexture(static_cast<int>(texture), 0);
            context->SetTexWrapModes(2, 2, 0);
            context->SetTexFiltering(1, 1, 0);
            for (std::uint32_t i = 0; i < wallCount_ && i < 4; ++i)
                if (!walls_[i].empty()) context->DrawPrims(6, 0x0a, static_cast<int>(walls_[i].size()), walls_[i].data());
        }
        return 2;
    }
    if (context->RenderPass() == 1) {
        context->SetSelfLitShade(kWateredStateRGBs[value174_ == 0 ? 0 : 1]);
        if (pieceTexture_ && !piece_.empty()) {
            const std::uint32_t texture = pieceTexture_->TextureIds()[0];
            context->ResetModelViewTransform();
            context->SetRenderState(mTrenchPackedRenderState);
            context->SetTexture(static_cast<int>(texture), 0);
            context->SetTexWrapModes(3, 3, 0);
            context->SetTexFiltering(1, 1, 0);
            if (geometryInfo_ && geometryInfo_->quadStrip) {
                for (std::size_t i = 0; i + 2 < piece_.size(); i += 2) {
                    const float p0[3] = {piece_[i].x, piece_[i].y, piece_[i].z};
                    const float p1[3] = {piece_[i + 1].x, piece_[i + 1].y, piece_[i + 1].z};
                    const float p2[3] = {piece_[i + 2].x, piece_[i + 2].y, piece_[i + 2].z};
                    float n[3] = {};
                    SC4GeometryUtilities::GetNormalToTriangle(p0, p1, p2, n);
                    cS3DVector3 normal{n[0], n[1], n[2]};
                    const float length = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
                    if (length > 0.0f) {   // cS3DVector3::Normalize
                        normal.x /= length;
                        normal.y /= length;
                        normal.z /= length;
                    }
                    if (g_viewHost) context->SetShade(g_viewHost->LightColor(normal));
                    context->DrawPrims(1, 0x0a, 4, &piece_[i]);
                }
                context->ResetShade();
            } else {
                context->DrawPrims(1, 0x0a, static_cast<int>(piece_.size()), piece_.data());
            }
        }
        auto drawModel = [&](s3d::cIS3DModelInstance* model, float y) {
            if (!model) return;
            SetTranslation(viewMatrix_, position_[0], y, position_[2]);
            context->SetModelTransform(viewMatrix_);
            const s3d::SC4DrawAttributes attributes = model->GetDrawAttributes();
            IUndergroundModelMeshes* meshes = g_viewHost ? g_viewHost->ModelMeshes(model) : nullptr;
            if (!meshes) return;
            const int count = meshes->Count();
            for (int i = 0; i < count; ++i) {
                s3d::cS3DMaterial* material = meshes->Material(i);
                const SC4Mesh* mesh = meshes->Mesh(i);
                if (!material || !mesh) continue;
                material->flags = static_cast<std::uint16_t>(material->flags | 0x10);
                material->b9 = 4;
                material->b10 = 5;
                context->SetModelShade(model, nullptr);
                context->SetRenderState(*material, &attributes);
                context->RenderMesh(*mesh);
                context->ResetShade();
            }
            meshes->Release();
        };
        drawModel(topModel_, modelY_);
        drawModel(bottomModel_, trenchFloorY_);
        context->ResetShade();
    }
    modelDirty_ = false;
    return 1;
}

}  // namespace sc4
