#include "sc4/SubwayOccupant.h"

#include <cmath>

#include "sc4/PathInfo.h"

namespace sc4 {

namespace {
bool sbSubwaysInited = false;

constexpr std::uint32_t kSubwayPathGroup = 0xa966883f;          // the path files of the subway pieces
constexpr std::uint32_t kSubwayPathInstanceBase = 0x6000000;    // instance = piece id | this, unless a path id override is set
constexpr float kPathFloorDrop = 0.5f;                           // 0x00a706b4: GetAltitude answers this far below the floor
constexpr float kCrossingFloorScale = -2.0f;                     // 0x00a70804: InitPathBottomQuad's drop for a plain crossing

// The piece as the path info calls it (the original passes `this`): the occupant's own methods, its altitude from the floor.
class SubwayPathOccupant final : public IPathNetworkOccupant {
public:
    explicit SubwayPathOccupant(cSC4SubwayOccupant& occupant) : o_(occupant) {}
    std::uint32_t PieceId() const override { return o_.PieceId(); }
    std::uint8_t GetRotation() const override { return o_.GetRotation(); }
    bool GetFlip() const override { return o_.GetFlip() != 0; }
    void GetPosition(float* xyz) const override { o_.GetPosition(xyz); }
    void GetBoundingCityCells(SC4RectInt32& rect) const override {
        SC4CellRect cells;
        o_.GetBoundingCityCells(cells);
        rect.x0 = cells.x0; rect.z0 = cells.z0; rect.x1 = cells.x1; rect.z1 = cells.z1;
    }
    void TransformNode(cS3DVector3&) override {}   // empty in cSC4NetworkOccupant
    float GetAltitude(float x, float z) const override { return o_.GetAltitude(x, z); }
    std::uint8_t GetOneWayDirections() const override { return o_.GetOneWayDirections(); }
    std::uint8_t GetActiveEdgesForType(std::uint32_t t) const override {
        return static_cast<std::uint8_t>(o_.GetActiveEdgesForType(static_cast<std::uint8_t>(t)));
    }
    std::uint8_t GetActiveEdgesForTypeNoGroup(std::uint32_t t) const override {
        return static_cast<std::uint8_t>(o_.GetActiveEdgesForTypeNoGroup(static_cast<std::uint8_t>(t)));
    }
    void AddPathConnection(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) override { o_.AddPathConnection(pathType, entry, exit); }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { o_.GetOccupiedCell(x, z); }
    const cSC4PathInfo* GetPathInfo() const override { return dynamic_cast<const cSC4PathInfo*>(o_.GetPathInfo()); }
private:
    cSC4SubwayOccupant& o_;
};
}  // namespace

// 0x0013756a: the pipe's StaticInit over the subway tables. Each of the diagonal piece's five edge vertices is measured
// (x, z) to its hinge vertex; the piece's share for vertex u (1..4) is d[u] / (d[u] + d[5 - u]) and the cap's table holds
// the same shares in reverse order; both tables are 1 at their open end. The underground statics follow.
bool cSC4SubwayOccupant::StaticInit() {
    if (sbSubwaysInited) return false;
    const tCustomGeometryInfo& info = sCustomGeometryInfo;
    float distance[5];
    for (int i = 0; i < 5; ++i) {
        const UndergroundVertex& edge = info.vertices[info.edge0VertIndices[i]];
        const UndergroundVertex& hinge = info.vertices[info.hingeVertIndices[i]];
        const float dx = edge.x - hinge.x, dz = edge.z - hinge.z;
        distance[i] = std::sqrt(dx * dx + dz * dz);
    }
    sDiagonalPieceEdgeDistancePercentages[0] = 1.0f;
    sDiagonalCapEdgeDistancePercentages[4] = 1.0f;
    for (int u = 1; u < 5; ++u) {
        const float share = distance[u] / (distance[u] + distance[5 - u]);
        sDiagonalPieceEdgeDistancePercentages[u] = share;
        sDiagonalCapEdgeDistancePercentages[4 - u] = share;
    }
    sbSubwaysInited = true;
    return cSC4UndergroundOccupant::StaticInit();
}

bool cSC4SubwayOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZCLSID_cSC4SubwayOccupant) return cSC4UndergroundOccupant::QueryInterface(iid, out);
    *out = static_cast<cSC4SubwayOccupant*>(this);
    AddRef();
    return true;
}

bool cSC4SubwayOccupant::Init() {
    if (!sbSubwaysInited) StaticInit();
    if (!IsInitialized()) {
        cSC4UndergroundOccupant::Init();
        cSC4UndergroundOccupant::SetupPackedRenderState();
    }
    return true;
}

bool cSC4SubwayOccupant::Init(const float (*corners)[3]) {
    if (!sbSubwaysInited) StaticInit();
    if (!IsInitialized()) cSC4UndergroundOccupant::Init(corners, kGZCLSID_cSC4SubwayOccupant, &sCustomGeometryInfo);
    return true;
}

bool cSC4SubwayOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    cSC4UndergroundOccupant::Write(stream);
    return !stream.GetError();
}

bool cSC4SubwayOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (cSC4UndergroundOccupant::Read(stream)) SetModelDirty(true);
    return !stream.GetError();
}

// The flag test is 0x6000000 (the pipe's model offset), as in the original.
void cSC4SubwayOccupant::GetSortValue(std::uint32_t& out) const {
    if (HasAnyNetworkFlag(0x6000000)) cSC4UndergroundOccupant::GetSortValue(out);
    else out = kGZCLSID_cSC4SubwayOccupant;
}

const tCustomGeometryInfo* cSC4SubwayOccupant::GetCustomGeometryInfo() const { return &sCustomGeometryInfo; }

// 0x00136d5c: the floor quad split along its diagonal from corner 0 to corner 2; the point's height on its triangle, less half a
// unit.
float cSC4SubwayOccupant::GetAltitude(float x, float z) const {
    const UndergroundVertex* q = pathBottomQuad_;
    const float dx = x - q[0].x;
    const float dz = z - q[0].z;
    float y;
    if (dz < dx) y = (q[3].y - q[0].y) * dx * 0.0625f + q[0].y + dz * (q[2].y - q[3].y) * 0.0625f;
    else y = (q[1].y - q[0].y) * dz * 0.0625f + q[0].y + dx * (q[2].y - q[1].y) * 0.0625f;
    return y - kPathFloorDrop;
}

// 0x00136e1c. The crossing (0x300) and pipe (0x4b00) pieces with a built model take its bottom vertices 4, 5, 7 and 6 as the quad's
// corners, turned back by the piece's rotation; a piece that is not a zipper then lowers each corner by twice its depth below the
// matching top vertex (0, 1, 11, 10). Every other piece (or one without its model) takes the trench floor's four vertices.
void cSC4SubwayOccupant::InitPathBottomQuad() {
    const std::vector<UndergroundVertex>& piece = PieceGeometry();
    if (pieceId_ == kUndergroundPieceCrossing || pieceId_ == kUndergroundPiecePipe) {
        bool direction = false;
        const bool zipper = GetZipperPieceType(direction) != 0;
        if (piece.size() > 7) {
            static constexpr int kBottom[4] = {4, 5, 7, 6};
            static constexpr int kTop[4] = {0, 1, 11, 10};
            const unsigned turn = static_cast<std::uint8_t>(4 - GetRotation());
            for (int k = 0; k < 4; ++k) pathBottomQuad_[(turn + k) & 3] = piece[kBottom[k]];
            // the original reads the top vertices without checking for them (the crossing and pipe models have 12 and more)
            if (!zipper && piece.size() > 11)
                for (int k = 0; k < 4; ++k)
                    pathBottomQuad_[(turn + k) & 3].y += (piece[kBottom[k]].y - piece[kTop[k]].y) * kCrossingFloorScale;
            return;
        }
    }
    const std::vector<UndergroundVertex>& floor = TrenchGeometry();
    for (std::size_t i = 0; i < 4 && i < floor.size(); ++i) pathBottomQuad_[i] = floor[i];
}

// 0x00137320. On the first call: the geometry is built if it was not, the floor quad is set, and the path file {0x296678f7,
// 0xa966883f, the path id override or piece id | 0x6000000} (no path id mapping, unlike the base) is made flat (no elevation),
// transformed onto the piece, and every path point's height is set from the floor with the path's length summed again. Then
// the edges are verified and the connection bitmap rebuilt, as in the base.
cISC4PathInfo* cSC4SubwayOccupant::GetPathInfo() {
    if (persistRef_) return static_cast<cISC4PathInfo*>(persistRef_);
    if (!ModelCreated()) CreateGeometry(false);
    InitPathBottomQuad();
    SubwayPathOccupant self(*this);
    if (INetworkOccupantPathHost* host = NetworkOccupantPathHost()) {
        const std::uint32_t instance = pathOverride_ != 0 ? pathOverride_ : (PieceId() | kSubwayPathInstanceBase);
        if (cISC4PathInfo* resource = host->LoadPathInfo(cGZPersistResourceKey{0x296678f7, kSubwayPathGroup, instance})) {
            resource->SetUseElevationFlag(false);
            cISC4PathInfo* transformed = resource->CreateTransformedCopy(&self);
            resource->Release();
            if (transformed) {
                if (PathMap* paths = transformed->GetPathMap()) {
                    for (auto& entry : *paths) {
                        tPath& path = entry.second;
                        path.length = 0.0f;
                        cS3DVector3 previous;
                        for (std::size_t i = 0; i < path.points.size(); ++i) {
                            cS3DVector3& point = path.points[i];
                            point.y = GetAltitude(point.x, point.z);
                            if (i != 0) {
                                const float dx = point.x - previous.x, dy = point.y - previous.y, dz = point.z - previous.z;
                                path.length += std::sqrt(dx * dx + dy * dy + dz * dz);
                            }
                            previous = point;
                        }
                    }
                }
                persistRef_ = transformed;
            }
        }
    }
    if (!persistRef_) return nullptr;
    auto* info = static_cast<cISC4PathInfo*>(persistRef_);
    info->VerifyPathEdges(&self);
    ClearPathConnections();
    info->InitializeNetworkConnectionBitmap(&self);
    return info;
}

}  // namespace sc4
