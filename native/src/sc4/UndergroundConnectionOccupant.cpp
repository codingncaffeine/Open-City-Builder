#include "sc4/UndergroundConnectionOccupant.h"

#include "rz/cRZMessage2Standard.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/DBSegment.h"

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

}  // namespace

void SetUndergroundConnectionHost(IUndergroundConnectionHost* host) { g_host = host; }

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

// 0x0022f87a: the occupant and the four view messages (the packed render states belong to the unported drawing).
bool cSC4UndergroundConnectionOccupant::Init() {
    if (!IsInitialized()) {
        cSC4Occupant::Init();
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
bool cSC4UndergroundConnectionOccupant::Init(std::uint32_t gziid, tCustomGeometryInfo* info) {
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

// 0x0022ff94 (the texture bindings it also releases belong to the unported drawing).
bool cSC4UndergroundConnectionOccupant::Shutdown() {
    for (auto& wall : walls_) wall.clear();
    trench_.clear();
    highlight_.clear();
    piece_.clear();
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
// own insertion has its piece rebuild its walls.
// PARTIAL: the view message 0xa6b79602 (a change between its two data words) rebuilds the drawn geometry (CreateGeometry,
// not recovered).
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

}  // namespace sc4
