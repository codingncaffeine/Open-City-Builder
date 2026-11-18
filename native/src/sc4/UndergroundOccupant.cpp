// cSC4UndergroundOccupant — the implementation. See UndergroundOccupant.h for the layout, the seams and the quirks.
//
// Everything here is a hand port of the exported decompile (work/sc4/cSC4UndergroundOccupant.txt, 0x1001a1462..
// 0x1001ae6d8), cross-checked against the disassembly where the decompiler lost the structure: the corner tables of
// ComputeModelYPosition / GetHookupData, the flip/hide switch of SetQuadFlipAndTriHide, the rotation and flip matrices
// of CreateTransformationMatrix, the min/max scan of MakeCustomUndergroundPiece and the register arguments of Draw.
// Compile without FMA contraction: the float operation order is the original's.
#pragma STDC FP_CONTRACT OFF

#include "sc4/UndergroundOccupant.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "rz/cRZMessage2Standard.h"
#include "sc4/DBSegment.h"
#include "sc4/ViewUtilities.h"

namespace sc4 {

namespace {

IUndergroundOccupantHost* g_host = nullptr;

// The original's statics: the packed render states live in .bss at 0x100b24638 / 0x100b24640 / 0x100b24648, the
// "pieces initialised" flag at 0x100b24634 and the tuning values at 0x100b03008 / 0x100b0300c / 0x100b03010.
bool sbPiecesInited = false;
cSC4PackedRenderState mTrenchPackedRenderState;
cSC4PackedRenderState mPiecePackedRenderState;
cSC4PackedRenderState mHighlightPackedRenderState;
float kPieceDepth = 3.0f;                 // 0x100b03008
bool kUsePieceDepth = true;               // 0x100b0300c
float kPieceIntersectionHeight = 10.0f;   // 0x100b03010

constexpr float kTextureU[4] = {0.0f, 0.0f, 1.0f, 1.0f};   // 0x100a1a2b0
constexpr float kTextureV[4] = {0.0f, 1.0f, 1.0f, 0.0f};   // 0x100a1a2c0

// SetQuadFlipAndTriHide: bit k of 0x4238 marks the piece ids 0x200 + 512 * k that use the nWhichTriTable branch.
constexpr std::uint32_t kHingePieceMask = 0x4238;
constexpr std::int32_t kWhichTriTable[4] = {1, 0, 0, 1};   // 0x100a1a2d0

// GetHookupData: the two corners of each edge, (0,1) (0,3) (3,2) (1,2) — 0x100a1a250.
constexpr std::int32_t kHookupEdgeCorners[4][2] = {{0, 1}, {0, 3}, {3, 2}, {1, 2}};

// The corner triple ComputeModelYPosition averages, per rotation|flip<<7 (0x1001abb5a's switch).
struct CornerTriple { std::int32_t a, b, c; };
CornerTriple CornersForRotationFlip(std::uint8_t rotationFlip) {
    switch (rotationFlip) {
        case 0x80: return {0, 2, 1};
        case 0x81: return {1, 3, 2};
        case 0x82: return {0, 2, 3};
        case 0x83: return {1, 3, 0};
        case 0x00: return {1, 3, 2};
        case 0x01: return {0, 2, 1};
        case 0x02: return {1, 3, 0};
        case 0x03: return {0, 2, 3};
        default: return {0, 0, 0};
    }
}

// The wall quad corner pairs, kWallCorners @ 0x1001adc01: {1,0} {0,3} {3,2} {2,1}.
constexpr std::uint32_t kWallCorners[4][2] = {{1, 0}, {0, 3}, {3, 2}, {2, 1}};

void WriteVertexVector(cIGZOStream& stream, const std::vector<UndergroundVertex>& vertices) {
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

void ReadVertexVector(cIGZIStream& stream, std::vector<UndergroundVertex>& vertices) {
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

// cRZMessage2Standard posted for the manager insert / remove of an underground piece (0x1001abee0).
void PostManagerMessage(cSC4UndergroundOccupant& occupant, std::uint32_t type, std::uint32_t data) {
    cIGZMessageServer2* server = DBSegmentMessageServer();
    if (!server) return;                     // the original dereferences SL::spMS2; the port guards the null server
    auto* msg = new cRZMessage2Standard();
    msg->AddRef();
    msg->SetType(type);
    msg->SetVoid1(static_cast<cISC4Occupant*>(&occupant));
    msg->SetIGZUnknown(static_cast<cISC4Occupant*>(&occupant));
    msg->SetData2(occupant.IsFlagSet(kOccupantFlagVisible) ? 1 : 0);
    msg->SetData3(occupant.GetType());
    msg->SetData4(data);
    server->MessagePost(msg, false);
    msg->Release();
}

}  // namespace

IUndergroundOccupantHost* UndergroundOccupantHost() { return g_host; }
void SetUndergroundOccupantHost(IUndergroundOccupantHost* host) { g_host = host; }

std::int32_t cSC4UndergroundOccupant::sCurZoom_ = 0;
std::vector<cSC4UndergroundOccupant::ZoomTextureBinding> g_zoomTextureBindings;

std::vector<cSC4UndergroundOccupant::ZoomTextureBinding>& cSC4UndergroundOccupant::ZoomTextureBindings() {
    return g_zoomTextureBindings;
}
const cSC4PackedRenderState& cSC4UndergroundOccupant::TrenchPackedRenderState() { return mTrenchPackedRenderState; }
const cSC4PackedRenderState& cSC4UndergroundOccupant::PiecePackedRenderState() { return mPiecePackedRenderState; }
const cSC4PackedRenderState& cSC4UndergroundOccupant::HighlightPackedRenderState() { return mHighlightPackedRenderState; }

// ---------------------------------------------------------------------------------------------------------------
// construction
// ---------------------------------------------------------------------------------------------------------------

// 0x1001aa4b0. The original zeroes +0x1e0..+0x32a with one bzero (the four wall vectors, the trench / highlight /
// piece vectors, the model matrix, the cells, the custom model pointer, the hide flags and the terrain heights), then
// clears the four vector begin/end pairs and writes the identity matrix. It leaves +0x32c..+0x33f (the terrain heights
// and the model y) and +0x350 (the geometry info pointer) uninitialised; this port zeroes them so tests are
// deterministic.
cSC4UndergroundOccupant::cSC4UndergroundOccupant() {
    modelFlags0_ = 0;
    modelFlags1_ = 0;
    modelShade3_ = 0.0f;
    pieceTexture_ = nullptr;
    wallCount_ = 0;
    trenchTexture_ = nullptr;
    for (auto& binding : pieceTextures_) binding = nullptr;
    cellX_ = 0;
    cellZ_ = 0;
    customModel_ = nullptr;
    hideTriangle_[0] = hideTriangle_[1] = hideTriangle_[2] = false;
    for (float& height : terrainHeights_) height = 0.0f;
    modelY_ = 0.0f;
    modelDirty_ = false;
    needsModelCreate_ = false;
    modelCreated_ = false;
    gziid_ = 0;
    networkTypeId_ = 0;
    geometryInfo_ = nullptr;
    modelMatrix_ = cS3DMatrix4{};
}

// 0x1001aa61c (+ the deleting variant 0x1001aa71e and the adjustment thunks). Releases the eight texture bindings
// (the piece and trench textures and the six piece textures) and lets the vector destructors free the geometry.
cSC4UndergroundOccupant::~cSC4UndergroundOccupant() {
    if (pieceTexture_) pieceTexture_->Release();
    if (trenchTexture_) trenchTexture_->Release();
    for (auto*& binding : pieceTextures_)
        if (binding) binding->Release();
    pieceTexture_ = nullptr;
    trenchTexture_ = nullptr;
    for (auto*& binding : pieceTextures_) binding = nullptr;
    for (auto& wall : walls_) wall.clear();
    trench_.clear();
    highlight_.clear();
    piece_.clear();
    piece2_.clear();
    if (customModel_) {
        customModel_->Release();
        customModel_ = nullptr;
    }
}

void cSC4UndergroundOccupant::SetCustomModel(s3d::cIS3DModelInstance* model) {
    if (model == customModel_) return;
    if (customModel_) customModel_->Release();
    customModel_ = model;
}

bool cSC4UndergroundOccupant::QueryInterface(std::uint32_t iid, void** out) {   // 0x1001abdba
    if (iid == kGZIID_cISC4UndergroundOccupant || iid == kGZIID_cISC4UndergroundOccupant2 ||
        iid == kGZIID_cISC4UndergroundOccupant3) {
        *out = this;
        AddRef();
        return true;
    }
    if (cSC4Occupant::QueryInterface(iid, out)) return true;
    if (modelInstance_) return modelInstance_->QueryInterface(iid, out);   // cS3DModelInstanceBasic @ +0x68
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------------------------------------------

bool cSC4UndergroundOccupant::StaticInit() {   // 0x1001aa79a
    if (sbPiecesInited) return false;
    sbPiecesInited = true;
    SetupPackedRenderState();
    if (g_host) g_host->InitNetworkOccupantStatics();   // cSC4NetworkOccupant::StaticInit @ 0x10019c248
    return true;
}

void cSC4UndergroundOccupant::SetupPackedRenderState() {   // 0x1001aad0e
    if (!g_host) return;                       // the original tests SL::spRenderer
    g_host->PrepareRenderer();                 // SL::spRenderer vslot 0x50
    mTrenchPackedRenderState.PackRenderState(true, true, true, false, false, 1, false, 3, -1, -1, -1, -1.0f, 1, -1);
    mPiecePackedRenderState.PackRenderState(true, true, true, false, true, 1, false, 3, 4, 5, -1, -1.0f, 1, -1);
    mHighlightPackedRenderState.PackRenderState(true, true, true, false, true, 0, false, 3, 4, 5, -1, -1.0f, 1, -1);
}

bool cSC4UndergroundOccupant::Init() {   // 0x1001aa750
    if (!sbPiecesInited) StaticInit();
    if (!IsInitialized()) {
        cSC4NetworkOccupant::Init();
        geometryInfo_ = GetCustomGeometryInfo();   // slot 0x3c8
    }
    return true;
}

// 0x1001aa7de. Quirk: the geometry info argument is stored at +0x350 and immediately replaced by the result of the
// GetCustomGeometryInfo virtual, so a caller cannot pass a table of its own.
bool cSC4UndergroundOccupant::Init(const float (*corners)[3], std::uint32_t gziid, tCustomGeometryInfo* info) {
    if (!sbPiecesInited) StaticInit();
    if (IsInitialized()) return true;
    gziid_ = gziid;
    geometryInfo_ = info;
    for (int i = 0; i < 4; ++i) terrainHeights_[i] = corners[i][1];
    cSC4NetworkOccupant::Init(corners);
    for (int i = 0; i < 4; ++i) {
        corners_[i].u = kTextureU[i];          // (vslot 0x18 / 0x1c through the base's corner array)
        corners_[i].v = kTextureV[i];
    }
    float position[3];
    GetPosition(position);
    if (g_host) {
        g_host->PositionToCell(position[0], position[2], cellX_, cellZ_);   // SL::spCity vslot 0x2f8
    } else {
        cellX_ = static_cast<std::int32_t>(position[0] * 0.0625f);
        cellZ_ = static_cast<std::int32_t>(position[2] * 0.0625f);
    }
    modelMatrix_ = cS3DMatrix4{};
    hideTriangle_[0] = hideTriangle_[1] = hideTriangle_[2] = false;
    geometryInfo_ = GetCustomGeometryInfo();
    // The two bindings the geometry needs: the trench texture is record 1 of the table (geometry info +0xdc) and the
    // piece texture record 0 (geometry info +0xc8).
    if (g_host && geometryInfo_) {
        trenchTexture_ = g_host->CreateTextureBinding(kTextureBindingGroupPiece,
                                                     geometryInfo_->textureRecords[1].instance);
        pieceTexture_ = g_host->CreateTextureBinding(kTextureBindingGroupPiece,
                                                    geometryInfo_->textureRecords[0].instance);
    }
    for (auto*& binding : pieceTextures_) {
        if (binding) {
            binding->Release();
            binding = nullptr;
        }
    }
    for (auto& wall : walls_) wall.clear();
    wallCount_ = 0;
    trench_.clear();
    highlight_.clear();
    piece_.clear();
    piece2_.clear();
    modelDirty_ = false;
    needsModelCreate_ = false;
    modelCreated_ = false;
    ComputeBoundingBox(Bounds(), Bounds() + 3);   // vslot 0x318 into the network occupant's +0x170 bounds
    // Init fills the two distance percentage tables of the geometry info (0x1001aa9c0..0x1001aaa70).
    if (geometryInfo_) {
        tCustomGeometryInfo* geometry = const_cast<tCustomGeometryInfo*>(geometryInfo_);
        const int count = geometry->percentageCount;
        std::vector<float> distances(static_cast<std::size_t>(count > 0 ? count : 0), 0.0f);
        if (count > 0) {
            for (int i = 0; i < count; ++i) {
                const UndergroundVertex& from = geometry->vertices[geometry->edge0VertIndices[i]];
                const UndergroundVertex& to = geometry->vertices[geometry->hingeVertIndices[i]];
                const float dx = from.x - to.x;
                const float dz = from.z - to.z;
                distances[static_cast<std::size_t>(i)] = std::sqrt(dz * dz + dx * dx);
            }
        }
        if (geometry->edgeDistancePercentages) geometry->edgeDistancePercentages[0] = 1.0f;
        if (geometry->hingeDistancePercentages) geometry->hingeDistancePercentages[4] = 1.0f;
        if (count > 1) {
            const float last = distances[static_cast<std::size_t>(count - 1)];
            for (int i = 0; i + 1 < count; ++i) {
                const float distance = distances[static_cast<std::size_t>(i + 1)];
                const float percentage = distance / (last + distance);
                if (geometry->edgeDistancePercentages) geometry->edgeDistancePercentages[i + 1] = percentage;
                if (geometry->hingeDistancePercentages) geometry->hingeDistancePercentages[count - 2 - i] = percentage;
            }
        }
    }
    return true;
}

// 0x1001aabc4. Releases the eight texture bindings, clears the geometry and the model and shuts the base down.
bool cSC4UndergroundOccupant::Shutdown() {
    if (trenchTexture_) trenchTexture_->Release();
    if (pieceTexture_) pieceTexture_->Release();
    for (auto*& binding : pieceTextures_) {
        if (binding) binding->Release();
        binding = nullptr;
    }
    trenchTexture_ = nullptr;
    pieceTexture_ = nullptr;
    for (auto& wall : walls_) wall.clear();
    wallCount_ = 0;
    trench_.clear();
    highlight_.clear();
    piece_.clear();
    piece2_.clear();
    if (customModel_) {
        customModel_->Release();
        customModel_ = nullptr;
    }
    return cSC4NetworkOccupant::Shutdown();
}

// ---------------------------------------------------------------------------------------------------------------
// record
// ---------------------------------------------------------------------------------------------------------------

// 0x1001aae32. The underground record writes its own version word (3) in front of the network occupant's (8).
bool cSC4UndergroundOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);
    if (cSC4NetworkOccupant::Write(stream)) {
        stream.SetUint8(modelFlags0_);
        stream.SetUint8(modelFlags1_);
        stream.SetFloat32(modelShade3_);
        stream.SetUint32(wallCount_);
        for (const auto& wall : walls_) WriteVertexVector(stream, wall);
        WriteVertexVector(stream, trench_);
        WriteVertexVector(stream, highlight_);
        stream << modelMatrix_;
        stream.SetSint32(cellX_);
        stream.SetSint32(cellZ_);
        stream << hideTriangle_[0];
        stream << hideTriangle_[1];
        stream << hideTriangle_[2];
        for (float height : terrainHeights_) stream.SetFloat32(height);
        stream.SetFloat32(modelY_);
        stream.SetUint32(gziid_);
        stream.SetUint32(networkTypeId_);
    }
    return !stream.GetError();
}

// 0x1001ab0f2
bool cSC4UndergroundOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 3) return false;
    if (cSC4NetworkOccupant::Read(stream)) {
        stream.GetUint8(modelFlags0_);
        stream.GetUint8(modelFlags1_);
        stream.GetFloat32(modelShade3_);
        stream.GetUint32(wallCount_);
        for (auto& wall : walls_) ReadVertexVector(stream, wall);
        ReadVertexVector(stream, trench_);
        ReadVertexVector(stream, highlight_);
        stream >> modelMatrix_;
        stream.GetSint32(cellX_);
        stream.GetSint32(cellZ_);
        stream >> hideTriangle_[0];
        stream >> hideTriangle_[1];
        stream >> hideTriangle_[2];
        for (float& height : terrainHeights_) stream.GetFloat32(height);
        stream.GetFloat32(modelY_);
        stream.GetUint32(gziid_);
        stream.GetUint32(networkTypeId_);
        needsModelCreate_ = true;             // *(u16 *)(this + 0x341) = 1: needsModelCreate = 1, modelCreated = 0
        modelCreated_ = false;
    }
    return !stream.GetError();
}

// ---------------------------------------------------------------------------------------------------------------
// simple accessors / flags
// ---------------------------------------------------------------------------------------------------------------

bool cSC4UndergroundOccupant::IsPlaced() {   // 0x1001a1462
    if ((GetFlags() & 1) == 0) return false;
    if (customModel_) return true;
    return geometryInfo_ != nullptr;
}

void cSC4UndergroundOccupant::SetQuadDraw(std::uint32_t pieceId, std::uint8_t rotationFlip) {   // 0x1001ac7b8
    pieceId_ = pieceId;
    rotationFlip_ = rotationFlip;
    ComputeModelYPosition();
}

void cSC4UndergroundOccupant::SetNetworkFlag(std::uint32_t flags) {   // 0x1001ae5f8
    const std::uint32_t before = GetNetworkFlag();
    cSC4NetworkOccupant::SetNetworkFlag(flags);
    if (((GetNetworkFlag() ^ before) & kNetworkFlagUndergroundDirty) != 0) modelDirty_ = true;
}

void cSC4UndergroundOccupant::ClearNetworkFlag(std::uint32_t flags) {   // 0x1001ae642
    const std::uint32_t before = GetNetworkFlag();
    cSC4NetworkOccupant::ClearNetworkFlag(flags);
    if (((GetNetworkFlag() ^ before) & kNetworkFlagUndergroundDirty) != 0) modelDirty_ = true;
}

void cSC4UndergroundOccupant::GetSortValue(std::uint32_t& out) const {   // 0x1001abe72
    out = HasAnyNetworkFlag(0x06000000) ? pieceId_ : kUndergroundDefaultSortValue;
}

cS3DVector3 cSC4UndergroundOccupant::GetDrawColor() const {   // 0x1001ae562
    return cS3DVector3{1.0f, 1.0f, 1.0f};
}

void cSC4UndergroundOccupant::GetCurrentTextureBindings(IUndergroundTextureBinding*& first,
                                                        IUndergroundTextureBinding*& second) {   // 0x1001ae580
    const std::uint32_t flags = GetNetworkFlag();
    if (flags & 0x00020000) {
        first = pieceTextures_[4];
        second = pieceTextures_[5];
    } else if (flags & 0x00010000) {
        first = pieceTextures_[2];
        second = pieceTextures_[3];
    } else {
        first = pieceTextures_[0];
        second = pieceTextures_[1];
    }
}

// 0x1001ae372: the underground pieces have no highlight pass of their own (the original is empty).
void cSC4UndergroundOccupant::SetHighlight(int, bool) {}

// 0x1001aba4a: the +0x68 cS3DModelInstanceBasic base is the occupant's only model instance (add reference).
void cSC4UndergroundOccupant::GetModelInstances(int, int, s3d::cIS3DModelInstance** out, int, int& count) {
    count = 1;
    *out = modelInstance_;
    if (modelInstance_) modelInstance_->AddRef();
}

void cSC4UndergroundOccupant::GetVertexHeights(float* heights) const {   // 0x1001ae26a
    for (int i = 0; i < 4; ++i) heights[i] = corners_[i].y;
}

void cSC4UndergroundOccupant::SetVertexHeights(float* heights) {   // 0x1001ae216
    for (int i = 0; i < 4; ++i) corners_[i].y = heights[i];
    ComputeModelYPosition();
    CreateWalls(true);
    CreateFloor(true);
}

void cSC4UndergroundOccupant::GetTerrainHeights(float* heights) {   // 0x1001abce0
    for (int i = 0; i < 4; ++i) heights[i] = terrainHeights_[i];
}

void cSC4UndergroundOccupant::SetTerrainHeights(float* heights) {   // 0x1001aba88
    for (int i = 0; i < 4; ++i) terrainHeights_[i] = heights[i];
    ComputeBoundingBox(Bounds(), Bounds() + 3);
    ComputeModelYPosition();
}

void cSC4UndergroundOccupant::GetHiddenTriangles(bool& t0, bool& t1, bool& t2) const {   // 0x1001ae28c
    t0 = hideTriangle_[0];
    t1 = hideTriangle_[1];
    t2 = hideTriangle_[2];
}

// 0x1001ae338
void cSC4UndergroundOccupant::GetQueryData(std::uint32_t& pieceId, int& networkTypeId, float* heights,
                                           float& modelY) {
    pieceId = pieceId_;
    networkTypeId = static_cast<int>(networkTypeId_);
    for (int i = 0; i < 4; ++i) heights[i] = corners_[i].y;
    modelY = modelY_;
}

// 0x1001aba6a: the tuning values are process-wide (0x100b03008 / 0x100b0300c / 0x100b03010).
void cSC4UndergroundOccupant::SetTuningParams(float pieceDepth, bool usePieceDepth, float intersectionHeight) {
    kPieceDepth = pieceDepth;
    kUsePieceDepth = usePieceDepth;
    kPieceIntersectionHeight = intersectionHeight;
}

// ---------------------------------------------------------------------------------------------------------------
// geometry queries
// ---------------------------------------------------------------------------------------------------------------

// 0x1001ae2aa: x and z are the corner averages but y is the *lowest* terrain height, not a corner height.
void cSC4UndergroundOccupant::GetPosition(float* xyz) {
    xyz[0] = 0.0f;
    xyz[1] = FLT_MAX;
    xyz[2] = 0.0f;
    float sumX = 0.0f, sumZ = 0.0f, lowest = FLT_MAX;
    for (int i = 0; i < 4; ++i) {
        sumX += corners_[i].x;
        xyz[0] = sumX;
        sumZ += corners_[i].z;
        xyz[2] = sumZ;
        if (terrainHeights_[i] < lowest) {
            xyz[1] = terrainHeights_[i];
            lowest = terrainHeights_[i];
        }
    }
    xyz[0] = sumX * 0.25f;
    xyz[2] = sumZ * 0.25f;
}

// 0x1001ab97e and 0x1001abcfc (the same body: ComputeBoundingBox is called with the base's +0x170 bounds).
void cSC4UndergroundOccupant::GetBoundingBox(float* minXyz, float* maxXyz) {
    ComputeBoundingBox(minXyz, maxXyz);
}

void cSC4UndergroundOccupant::ComputeBoundingBox(float* minXyz, float* maxXyz) {
    GetPosition(minXyz);
    GetPosition(maxXyz);
    for (int i = 0; i < 4; ++i) {
        const SC4TileCorner& corner = corners_[i];
        if (corner.x < minXyz[0]) minXyz[0] = corner.x;
        if (corner.y < minXyz[1]) minXyz[1] = corner.y;
        if (corner.z < minXyz[2]) minXyz[2] = corner.z;
        if (maxXyz[0] < corner.x) maxXyz[0] = corner.x;
        if (maxXyz[1] < terrainHeights_[i]) maxXyz[1] = terrainHeights_[i];   // (sic) the terrain, not the corner
        if (maxXyz[2] < corner.z) maxXyz[2] = corner.z;
    }
}

// 0x1001abace: the y the piece model sits at, per piece type. 0x300 / 0x4b00 use the trench depth (the sum of the
// terrain heights or the lowest one minus kPieceDepth); the straight / diagonal pieces use the rotation triple.
void cSC4UndergroundOccupant::ComputeModelYPosition() {
    float position[3];
    GetPosition(position);
    const int piece = static_cast<int>(pieceId_);
    if (piece == 0x300 || piece == 0x4b00) {
        if (!kUsePieceDepth) {
            float sum = 0.0f;
            for (float height : terrainHeights_) sum += height;
            modelY_ = sum;
            modelY_ = sum * 0.25f + kPieceIntersectionHeight;
        } else {
            modelY_ = position[1] - kPieceDepth;
        }
        return;
    }
    const bool triplePiece = piece == 0x200 || piece == 0x800 || piece == 0xa00 || piece == 0x1400 ||
                             piece == 0x14e00;
    if (!triplePiece && piece != 0x300 && piece != 0x4b00) {
        modelY_ = corners_[0].y + kPieceIntersectionHeight;
        return;
    }
    const CornerTriple triple = CornersForRotationFlip(rotationFlip_);
    float y;
    if (!kUsePieceDepth) {
        y = (corners_[triple.a].y + corners_[triple.b].y) * 0.5f;
        if (corners_[triple.c].y <= y) y = corners_[triple.c].y;
        y = y + kPieceIntersectionHeight;
    } else {
        y = (terrainHeights_[triple.a] + terrainHeights_[triple.b]) * 0.5f;
        if (terrainHeights_[triple.c] <= y) y = terrainHeights_[triple.c];
        y = y - kPieceDepth;
    }
    modelY_ = y;
}

// 0x1001ac242: only the crossing (0x300) and the pipe (0x4b00) refine the height; the straight and diagonal pieces
// keep the model y. Anything else reports "no hookup data".
void cSC4UndergroundOccupant::GetHookupData(float& height, bool& valid, int edge) {
    height = modelY_;
    bool found = true;
    switch (pieceId_) {
        case kUndergroundPieceStraight:
        case kUndergroundPieceDiagonal0:
        case kUndergroundPieceDiagonal1:
        case kUndergroundPieceDiagonal2:
        case kUndergroundPieceSubway:
            break;
        case kUndergroundPieceCrossing:
        case kUndergroundPiecePipe: {
            const int a = kHookupEdgeCorners[edge & 3][0];
            const int b = kHookupEdgeCorners[edge & 3][1];
            height = (corners_[a].y + corners_[b].y) * 0.5f + kPieceIntersectionHeight;
            break;
        }
        default:
            found = false;
            break;
    }
    valid = found;
}

// ---------------------------------------------------------------------------------------------------------------
// geometry creation
// ---------------------------------------------------------------------------------------------------------------

// 0x1001ac048: one wall quad between two tile corners, from the corner heights down to the terrain heights. The v
// coordinate carries the height difference (in 16-unit tiles) and the alpha comes from the corner's own colour.
void cSC4UndergroundOccupant::AddWallQuad(std::uint32_t cornerA, std::uint32_t cornerB,
                                          std::vector<UndergroundVertex>& out) {
    UndergroundVertex vertex;
    const SC4TileCorner& a = corners_[cornerA];
    const SC4TileCorner& b = corners_[cornerB];
    vertex.x = a.x;
    vertex.y = terrainHeights_[cornerA];
    vertex.z = a.z;
    vertex.color[0] = 0xff;
    vertex.color[1] = 0xff;
    vertex.color[2] = 0xff;
    vertex.color[3] = a.color[3];
    vertex.u = 0.0f;
    vertex.v = 0.0f;
    out.push_back(vertex);

    vertex.y = a.y;
    vertex.v = (terrainHeights_[cornerA] - a.y) * 0.0625f;
    out.push_back(vertex);

    vertex.x = b.x;
    vertex.y = b.y;
    vertex.z = b.z;
    vertex.color[3] = b.color[3];
    vertex.u = 1.0f;
    out.push_back(vertex);

    vertex.y = terrainHeights_[cornerB];
    vertex.v = 0.0f;
    out.push_back(vertex);
}

// 0x1001adc4e: the floor quad from the four tile corners (their own u/v and alpha).
void cSC4UndergroundOccupant::CreateFloor(bool update) {
    trench_.clear();
    for (int i = 0; i < 4; ++i) {
        UndergroundVertex vertex;
        vertex.x = corners_[i].x;
        vertex.y = corners_[i].y;
        vertex.z = corners_[i].z;
        vertex.color[0] = 0xff;
        vertex.color[1] = 0xff;
        vertex.color[2] = 0xff;
        vertex.color[3] = corners_[i].color[3];
        vertex.u = corners_[i].u;
        vertex.v = corners_[i].v;
        trench_.push_back(vertex);
    }
    if (update) ModelStateChanged(true);
}

// 0x1001ada7a. Hides the two tile edges the quad flips over, then walls every edge that is not joined to another
// underground piece (a connection to an empty cell is a real wall only when it is an "imaginary" connection).
void cSC4UndergroundOccupant::CreateWalls(bool update) {
    for (auto& wall : walls_) wall.clear();
    SetQuadFlipAndTriHide();
    wallCount_ = 0;
    bool visible[4] = {true, true, true, true};
    bool connectionPresent = false;
    if (g_host) {
        IUndergroundConnectionOccupant* connection = nullptr;
        if (g_host->FindUndergroundConnectionOccupant(cellX_, cellZ_, GetGZIID(), connection)) {
            connectionPresent = true;
            if (connection) connection->Release();
        }
    }
    if (connectionPresent) {
        // a connection piece at this cell carries its own walls
    } else if (hideTriangle_[1] != hideTriangle_[2]) {
        std::uint32_t hideA = 0, hideB = 0, cornerB = 0;
        const bool first = hideTriangle_[0];
        if (!first) {
            hideA = hideTriangle_[1];
            hideB = (hideTriangle_[1] == 0 ? 1u : 0u) + 2u;
            cornerB = 2;
        } else {
            hideA = (hideTriangle_[1] != 0 ? 2u : 0u);
            hideB = hideA + 1;
            cornerB = 3;
        }
        visible[hideA] = false;
        visible[hideB] = false;
        AddWallQuad(first ? 1u : 0u, cornerB, walls_[wallCount_]);
        ++wallCount_;
    }
    for (std::uint32_t edge = 0; edge < 4; ++edge) {
        if (!visible[edge]) continue;
        if (ConnectsToNeighbor(static_cast<int>(edge), GetNetworkType()) && !IsImaginaryConnection(static_cast<int>(edge)))
            continue;
        AddWallQuad(kWallCorners[edge][0], kWallCorners[edge][1], walls_[wallCount_]);
        ++wallCount_;
    }
    if (update) ModelStateChanged(true);
}

// 0x1001ada00: with the high network flag bit set, a connection to a neighbouring underground piece that also has the
// bit set is "imaginary" (no wall).
bool cSC4UndergroundOccupant::IsImaginaryConnection(int edge) {
    if (static_cast<std::int32_t>(GetNetworkFlag()) >= 0) return false;
    if (!g_host) return false;
    cSC4UndergroundOccupant* neighbour = g_host->FindUndergroundOccupant(
        cellX_ + kEdgeCellOffset[edge & 3][0], cellZ_ + kEdgeCellOffset[edge & 3][1], gziid_);
    if (!neighbour) return false;
    const bool imaginary = static_cast<std::int32_t>(neighbour->GetNetworkFlag()) < 0;
    neighbour->Release();
    return imaginary;
}

// 0x1001ad83c: which of the quad's triangles are hidden, from the piece id and the rotation / flip. The flag writes
// then update the three hide network flags (the first has the opposite polarity to the other two — kept).
void cSC4UndergroundOccupant::SetQuadFlipAndTriHide() {
    const std::uint32_t relative = pieceId_ - 0x200u;
    const std::uint32_t key = (relative << 23) | (relative >> 9);   // ROL 23
    bool h0 = false, h1 = false, h2 = false;
    if (key > 0xeu) {
        h0 = false;
        h1 = true;
        h2 = true;
    } else if ((kHingePieceMask & (1u << key)) != 0) {
        const std::uint8_t rotation = GetRotation();
        const std::uint8_t flip = GetFlip();
        const std::int32_t which = kWhichTriTable[rotation];
        h0 = (static_cast<std::uint32_t>(rotation) | 2u) == 2u;
        if (which == 0) {
            h1 = true;
            h2 = false;
        } else if (which == 1) {
            h1 = false;
            h2 = true;
        }
        if (flip != 0) {
            h0 = !h0;
            h1 = !h1;
            h2 = !h2;
        }
    } else if (key != 0) {
        h0 = false;
        h1 = true;
        h2 = true;
    } else {
        // piece id 0x200: the hidden triangles come from the rotation|flip byte
        const std::uint8_t rotationFlip = rotationFlip_;
        if (static_cast<std::uint8_t>(rotationFlip - 0x80) <= 3u) {
            switch (rotationFlip) {
                case 0x80: h0 = true; h1 = false; h2 = true; break;
                case 0x81: h0 = false; h1 = false; h2 = true; break;
                case 0x82: h0 = true; h1 = true; h2 = false; break;
                default: h0 = false; h1 = true; h2 = false; break;
            }
        } else if (rotationFlip <= 3u) {
            switch (rotationFlip) {
                case 0x00: h0 = false; h1 = true; h2 = false; break;
                case 0x01: h0 = true; h1 = true; h2 = false; break;
                case 0x02: h0 = false; h1 = false; h2 = true; break;
                default: h0 = true; h1 = false; h2 = true; break;
            }
        } else {
            h0 = hideTriangle_[0];   // the default keeps the first flag and leaves the other two alone
            ApplyTriangleFlags(h0);
            return;
        }
    }
    hideTriangle_[0] = h0;
    hideTriangle_[1] = h1;
    hideTriangle_[2] = h2;
    ApplyTriangleFlags(h0);
}

void cSC4UndergroundOccupant::ApplyTriangleFlags(bool hide0) {
    if (hide0) SetNetworkFlag(kNetworkFlagHideTriangle0);
    else ClearNetworkFlag(kNetworkFlagHideTriangle0);
    if (!hideTriangle_[1]) SetNetworkFlag(kNetworkFlagHideTriangle1);
    else ClearNetworkFlag(kNetworkFlagHideTriangle1);
    if (!hideTriangle_[2]) SetNetworkFlag(kNetworkFlagHideTriangle2);
    else ClearNetworkFlag(kNetworkFlagHideTriangle2);
}

// 0x1001ac31c. Transforms the piece's vertices with the model matrix, computes the u/v from the tile-local position,
// and moves the vertices that hook up to a neighbour down/up by the neighbour's hookup offset.
void cSC4UndergroundOccupant::MakeCustomUndergroundPiece(bool flipU, bool flipV,
                                                         std::vector<UndergroundVertex>& out,
                                                         const UndergroundVertex* vertices, int count,
                                                         const std::int32_t* indicesA, int countA, int edgeA,
                                                         const std::int32_t* indicesB, int countB, int edgeB,
                                                         const float* hookupScale) {
    out.clear();
    cSC4UndergroundOccupant* neighbourA = nullptr;
    cSC4UndergroundOccupant* neighbourB = nullptr;
    if (g_host) {
        neighbourA = g_host->FindUndergroundOccupant(cellX_ + kEdgeCellOffset[edgeA & 3][0],
                                                     cellZ_ + kEdgeCellOffset[edgeA & 3][1], gziid_);
        neighbourB = g_host->FindUndergroundOccupant(cellX_ + kEdgeCellOffset[edgeB & 3][0],
                                                     cellZ_ + kEdgeCellOffset[edgeB & 3][1], gziid_);
    }
    bool validA = false, validB = false;
    float offsetA = 0.0f, offsetB = 0.0f;
    if (neighbourA) {
        float height = 0.0f;
        neighbourA->GetHookupData(height, validA, (edgeA + 2) & 3);
        offsetA = modelY_ - height;
    }
    if (neighbourB) {
        float height = 0.0f;
        neighbourB->GetHookupData(height, validB, (edgeB + 2) & 3);
        offsetB = modelY_ - height;
    }
    float minY = 0.0f, minZ = 0.0f, maxY = 0.0f, maxZ = 0.0f;
    for (int i = 0; i < count; ++i) {
        minY = std::min(minY, vertices[i].y);
        minZ = std::min(minZ, vertices[i].z);
        maxY = std::max(maxY, vertices[i].y);
        maxZ = std::max(maxZ, vertices[i].z);
    }
    const float rangeY = std::fabs(maxY - minY);
    const float rangeZ = std::fabs(maxZ - minZ);
    for (int i = 0; i < count; ++i) {
        const UndergroundVertex& source = vertices[i];
        UndergroundVertex vertex;
        vertex.color[0] = 0xff;
        vertex.color[1] = 0xff;
        vertex.color[2] = 0xff;
        vertex.color[3] = 0xff;
        float u = (source.x + 8.0f) * 0.0625f;
        if (flipU) u = 1.0f - u;
        float v;
        if (rangeY > rangeZ) v = (source.y - minY) / rangeY;
        else v = (source.z + 8.0f) * 0.0625f;
        if (flipV) v = 1.0f - v;
        vertex.u = u;
        vertex.v = v;
        vertex.x = modelMatrix_.m[0][0] * source.x + modelMatrix_.m[0][1] * source.y +
                   modelMatrix_.m[0][2] * source.z + modelMatrix_.m[0][3];
        vertex.y = modelMatrix_.m[1][0] * source.x + modelMatrix_.m[1][1] * source.y +
                   modelMatrix_.m[1][2] * source.z + modelMatrix_.m[1][3];
        vertex.z = modelMatrix_.m[2][0] * source.x + modelMatrix_.m[2][1] * source.y +
                   modelMatrix_.m[2][2] * source.z + modelMatrix_.m[2][3];
        for (int j = 0; j < countA; ++j) {
            if (indicesA && indicesA[j] == i) {
                float offset = offsetA;
                if (hookupScale && validA) offset *= hookupScale[j];
                vertex.y -= offset;
                break;
            }
        }
        for (int j = 0; j < countB; ++j) {
            if (indicesB && indicesB[j] == i) {
                float offset = offsetB;
                if (hookupScale && validB) offset *= hookupScale[j];
                vertex.y -= offset;
                break;
            }
        }
        out.push_back(vertex);
    }
    if (neighbourA) neighbourA->Release();
    if (neighbourB) neighbourB->Release();
}

// 0x1001add20: the piece's model matrix: the transform's rotation, mirrored in x when the piece is flipped, and the
// position (with the model y) in the last column. The original multiplies by an identity matrix first (kept).
void cSC4UndergroundOccupant::CreateTransformationMatrix() {
    const std::uint8_t rotation = GetRotation();
    const bool flip = GetFlip() != 0;
    const float angle = SC4ViewUtilities::GetObjectYRotationAngle(rotation);
    if (transform_.flags != 0) {
        transform_.rotation[0][0] = 1.0f;
        transform_.rotation[0][1] = 0.0f;
        transform_.rotation[0][2] = 0.0f;
        transform_.rotation[1][0] = 0.0f;
        transform_.rotation[1][1] = 1.0f;
        transform_.rotation[1][2] = 0.0f;
        transform_.rotation[2][0] = 0.0f;
        transform_.rotation[2][1] = 0.0f;
        transform_.rotation[2][2] = 1.0f;
        transform_.translation[0] = transform_.translation[1] = transform_.translation[2] = 0.0f;
        transform_.scale = 1.0f;
        transform_.flags = 0;
        ++transform_.changeCounter;
    }
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float r00 = transform_.rotation[0][0], r02 = transform_.rotation[0][2];
    const float r10 = transform_.rotation[1][0], r12 = transform_.rotation[1][2];
    const float r20 = transform_.rotation[2][0], r22 = transform_.rotation[2][2];
    transform_.rotation[0][0] = c * r00 - s * r02;
    transform_.rotation[1][0] = c * r10 - s * r12;
    transform_.rotation[2][0] = c * r20 - s * r22;
    transform_.rotation[0][2] = c * r02 + s * r00;
    transform_.rotation[1][2] = c * r12 + s * r10;
    transform_.rotation[2][2] = c * r22 + s * r20;
    transform_.flags = 4;   // the original assigns 4, it does not OR the bit in
    ++transform_.changeCounter;

    cS3DMatrix4 rotationMatrix;
    rotationMatrix.m[0][0] = transform_.rotation[0][0];
    rotationMatrix.m[0][1] = transform_.rotation[0][1];
    rotationMatrix.m[0][2] = transform_.rotation[0][2];
    rotationMatrix.m[0][3] = transform_.translation[0];
    rotationMatrix.m[1][0] = transform_.rotation[1][0];
    rotationMatrix.m[1][1] = transform_.rotation[1][1];
    rotationMatrix.m[1][2] = transform_.rotation[1][2];
    rotationMatrix.m[1][3] = transform_.translation[1];
    rotationMatrix.m[2][0] = transform_.rotation[2][0];
    rotationMatrix.m[2][1] = transform_.rotation[2][1];
    rotationMatrix.m[2][2] = transform_.rotation[2][2];
    rotationMatrix.m[2][3] = transform_.translation[2];
    rotationMatrix.m[3][0] = 0.0f;
    rotationMatrix.m[3][1] = 0.0f;
    rotationMatrix.m[3][2] = 0.0f;
    rotationMatrix.m[3][3] = 1.0f;
    modelMatrix_ = cS3DMatrix4{} * rotationMatrix;   // the original's redundant identity product
    if (flip) {
        cS3DMatrix4 mirror;
        mirror.m[0][0] = -1.0f;
        mirror.m[0][1] = 0.0f;
        mirror.m[0][2] = 0.0f;
        mirror.m[0][3] = 0.0f;
        modelMatrix_ = mirror * modelMatrix_;
    }
    float position[3];
    GetPosition(position);
    modelMatrix_.m[0][3] = position[0];
    modelMatrix_.m[1][3] = modelY_;
    modelMatrix_.m[2][3] = position[2];
    modelMatrix_.m[3][3] = 1.0f;
    transform_.translation[0] = position[0];
    transform_.translation[1] = modelY_;
    transform_.translation[2] = position[2];
    transform_.flags |= 1;
    ++transform_.changeCounter;
}

// 0x1001ae192: the whole geometry of one piece in one call.
void cSC4UndergroundOccupant::CreateGeometry(bool update) {
    const std::uint8_t rotation = GetRotation();
    const std::uint8_t flip = GetFlip();
    ComputeModelYPosition();
    CreateTransformationMatrix();
    CreateUndergroundModel(rotation, flip != 0, update);
    CreateWalls(update);
    CreateFloor(update);
}

// i386 0x0013fe30 (x64 0x1001acae0): the piece model of the solved piece. The six piece bindings (group 0x1abe787d,
// instances from the geometry table's texture records) and the two vertex lists come from the piece tables:
//   0xa00 / 0x800 / 0x1400 / 0x14e00  the diagonal piece (0x14e00 adds a second half turned by two)
//   0x300                             the straight cap (cap top, then cap end)
//   0x200                             the diagonal cap (cap top, then cap end)
//   0x4b00                            the straight piece; a subway zipper uses the zipper tables
// A piece without a model gets the custom model of exemplar {0x5ad0e817, 0xbadb57f1, offset | piece}. Then the wall
// and trench bindings are made again, and the model is marked dirty.
void cSC4UndergroundOccupant::CreateUndergroundModel(int rotation, bool flip, bool update) {
    const std::uint8_t startRotation = GetRotation();   // vslot 0x60, before the rotation below
    piece_.clear();
    piece2_.clear();
    for (auto*& binding : pieceTextures_) {
        if (binding) binding->Release();
        binding = nullptr;
    }
    if (customModel_) {
        customModel_->Release();
        customModel_ = nullptr;
    }
    const tCustomGeometryInfo* info = geometryInfo_;   // +0x268
    const auto bind = [&](int slot, int record) {
        if (g_host && info) pieceTextures_[slot] = g_host->CreateTextureBinding(kTextureBindingGroupZoom, info->textureRecords[record].instance);
    };
    const auto verts = [&](int index) { return static_cast<const UndergroundVertex*>(info->pieceArrays[index]); };
    const auto indices = [&](int index) { return static_cast<const std::int32_t*>(info->pieceArrays[index]); };
    const auto floats = [&](int index) { return static_cast<const float*>(info->pieceArrays[index]); };
    const std::uint8_t flipBit = flip ? 0x80 : 0;
    SetRotationAndFlip(static_cast<std::uint8_t>(flipBit | (rotation & 0xff)));   // vslot 0x140
    // The edge each piece end meets: the turn after the rotation, and across for a flipped odd rotation.
    const auto edgeAfter = [flip](int turn, int by) {
        int edge = (turn + by) & 3;
        if (flip && (turn == 1 || turn == 3)) edge = (edge + 2) & 3;
        return edge;
    };
    const auto edgeAcross = [flip](int turn) {
        const int edge = (turn + 2) & 3;
        return (!flip || (turn != 0 && turn != 2)) ? edge : (edge + 2) & 3;
    };
    int edgeA = edgeAfter(rotation, 3);
    int edgeB = edgeA;
    const std::uint32_t piece = PieceId();   // +0xf8
    if (info) {
        switch (piece) {
            case 0xa00: case 0x800: case 0x1400: case 0x14e00: {
                bind(0, 14);
                bind(2, 15);
                bind(4, 16);
                const int turned = (rotation + 2) & 3;
                edgeB = edgeAcross(rotation);
                MakeCustomUndergroundPiece(false, false, piece_, info->vertices, info->countB0, info->edge0VertIndices,
                                           info->percentageCount, edgeA, info->edge1VertIndices, info->percentageCount,
                                           edgeB, info->edgeDistancePercentages);
                if (piece == 0x14e00) {   // the second half, turned by two
                    SetRotationAndFlip(static_cast<std::uint8_t>(flipBit | turned));
                    CreateTransformationMatrix();   // vslot 0x1a0
                    const int secondA = edgeAfter(turned, 3);
                    const int secondB = edgeAcross(turned);
                    bind(1, 14);
                    bind(3, 15);
                    bind(5, 16);
                    MakeCustomUndergroundPiece(false, false, piece2_, info->vertices, info->countB0, info->edge0VertIndices,
                                               info->percentageCount, secondA, info->edge1VertIndices,
                                               info->percentageCount, secondB, info->edgeDistancePercentages);
                    SetRotationAndFlip(static_cast<std::uint8_t>(flipBit | ((turned + 2) & 3)));
                    CreateTransformationMatrix();
                }
                break;
            }
            case 0x300:   // the straight cap
                bind(0, 2);
                bind(1, 2);
                bind(2, 3);
                bind(4, 4);
                MakeCustomUndergroundPiece(false, false, piece_, verts(6), info->counts[2], indices(3), info->counts[1], edgeA,
                                           nullptr, 0, 0, floats(5));
                MakeCustomUndergroundPiece(false, false, piece2_, verts(7), info->counts[3], nullptr, 0, 0, nullptr, 0, 0, nullptr);
                break;
            case 0x200:   // the diagonal cap
                bind(0, 14);
                bind(1, 14);
                bind(2, 15);
                bind(4, 16);
                MakeCustomUndergroundPiece(true, false, piece_, info->pieceVertices, info->countB8, info->pieceIndices,
                                           info->countC0, edgeA, nullptr, 0, 0, info->hingeDistancePercentages);
                MakeCustomUndergroundPiece(true, false, piece2_, info->pieceVertices2, info->countBC, nullptr, 0, 0, nullptr, 0,
                                           0, nullptr);
                break;
            case 0x4b00: {   // the straight piece (a zipper where a subway changes depth)
                const int edgeC = edgeAfter(rotation, 1);
                bool zipperDirection = false;
                const int zipper = GetZipperPieceType(zipperDirection);   // vslot 0x1d8
                const UndergroundVertex* first = nullptr;
                const UndergroundVertex* second = nullptr;
                bool turn = false;
                if (zipper == 1) {
                    first = verts(2);
                    second = verts(1);
                    turn = zipperDirection && (startRotation == 0 || startRotation == 3);
                    bind(0, 11);
                    bind(1, 8);
                    bind(2, 12);
                    bind(3, 9);
                    bind(4, 13);
                    bind(5, 10);
                } else if (zipper == 2) {
                    first = verts(2);
                    bind(0, 5);
                    bind(2, 6);
                    bind(4, 7);
                } else {
                    first = verts(0);
                    bind(0, 2);
                    bind(2, 3);
                    bind(4, 4);
                }
                if (!pieceTextures_[0]) bind(0, 2);
                if (!pieceTextures_[2]) bind(2, 3);
                if (!pieceTextures_[4]) bind(4, 4);
                if (turn) {
                    SetRotationAndFlip(static_cast<std::uint8_t>(((GetRotation() + 2) & 3) | (GetRotationAndFlip() & 0x80)));
                    CreateTransformationMatrix();
                    edgeA = edgeC;
                } else {
                    edgeB = edgeC;
                }
                MakeCustomUndergroundPiece(false, false, piece_, first, info->counts[0], indices(3), info->counts[1], edgeA,
                                           indices(4), info->counts[1], edgeB, floats(5));
                if (second)
                    MakeCustomUndergroundPiece(false, false, piece2_, second, info->counts[0], indices(3), info->counts[1],
                                               edgeA, indices(4), info->counts[1], edgeB, floats(5));
                break;
            }
            default:
                break;
        }
    }
    if ((!pieceTextures_[0] || piece_.empty()) && g_host)   // spResMan vslot 0xc, exemplar 0x9ad0e7e3, vslot 0x10
        customModel_ = g_host->CreateCustomModel(static_cast<std::uint32_t>(GetModelInstanceOffset()) | piece);
    if (trenchTexture_) {
        trenchTexture_->Release();
        trenchTexture_ = nullptr;
    }
    if (pieceTexture_) {
        pieceTexture_->Release();
        pieceTexture_ = nullptr;
    }
    if (g_host && info) {
        trenchTexture_ = g_host->CreateTextureBinding(kTextureBindingGroupPiece, info->textureRecords[1].instance);   // +0x8c
        pieceTexture_ = g_host->CreateTextureBinding(kTextureBindingGroupPiece, info->textureRecords[0].instance);    // +0x78
    }
    if (update) ModelStateChanged(true);   // vslot 0x1dc
    modelDirty_ = true;                    // +0x25e
}

// 0x1001ae37e: the piece became visible (or a neighbour did). Rebuilds the bounds and registers the model with the
// renderer; with updateNeighbors, every connected underground piece is told to rebuild too.
void cSC4UndergroundOccupant::ModelStateChanged(bool updateNeighbors) {
    const bool inCity = g_host ? g_host->IsCellInCity(cellX_, cellZ_) : false;
    if (!inCity) {
        modelDirty_ = false;
        needsModelCreate_ = false;
        return;
    }
    if (modelDirty_) return;
    modelDirty_ = true;
    needsModelCreate_ = true;
    ComputeBoundingBox(Bounds(), Bounds() + 3);
    if ((GetFlags() & 2) != 0 && g_host) g_host->AddModelInstanceToRenderer(modelInstance_, 3);
    if (!updateNeighbors || !g_host) return;
    for (int edge = 0; edge < 4; ++edge) {
        if (!ConnectsToNeighbor(edge, GetNetworkType())) continue;
        cSC4UndergroundOccupant* neighbour =
            g_host->FindUndergroundOccupant(cellX_ + kEdgeCellOffset[edge][0], cellZ_ + kEdgeCellOffset[edge][1],
                                            gziid_);
        if (!neighbour) continue;
        neighbour->ModelStateChanged(true);
        neighbour->Release();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// zipper connections (the pieces that join two pipe / tube segments)
// ---------------------------------------------------------------------------------------------------------------

// 0x1001ac7ce: how many pipe segments a zipper piece can join at this edge (1 for a free end, more when another
// zipper follows).
int cSC4UndergroundOccupant::GetZipperCandidateCount(cSC4UndergroundOccupant* occupant, int edge, int depth) {
    int result = 0;
    if (occupant->pieceId_ != kUndergroundPiecePipe) return result;
    if (g_host) {
        IUndergroundConnectionOccupant* connection = nullptr;
        if (g_host->FindUndergroundConnectionOccupant(occupant->cellX_, occupant->cellZ_, GetGZIID(), connection)) {
            if (connection) connection->Release();
            return 0;
        }
    }
    if (depth != 0 && g_host) {
        cSC4UndergroundOccupant* next = g_host->FindUndergroundOccupant(
            occupant->cellX_ + kEdgeCellOffset[edge & 3][0], occupant->cellZ_ + kEdgeCellOffset[edge & 3][1],
            gziid_);
        if (next) {
            result = GetZipperCandidateCount(next, edge, depth - 1);
            next->Release();
            return result + 1;
        }
    }
    return 1;
}

// 0x1001ac8e0: which way a pipe zipper piece has to face. Only the subway occupant (gziid 0xca16374f) with a pipe
// piece (0x4b00) has one.
int cSC4UndergroundOccupant::GetZipperPieceType(bool& param) {
    if (gziid_ != kGZIID_cSC4SubwayOccupant) return 0;
    if (pieceId_ != kUndergroundPiecePipe) return 0;
    if (g_host) {
        IUndergroundConnectionOccupant* connection = nullptr;
        if (g_host->FindUndergroundConnectionOccupant(cellX_, cellZ_, GetGZIID(), connection)) {
            if (connection) connection->Release();
            return 0;
        }
    }
    const std::uint8_t rotation = GetRotation();
    const bool flip = GetFlip() != 0;
    std::uint8_t first = static_cast<std::uint8_t>((rotation + 3) & 3);
    std::uint8_t second = static_cast<std::uint8_t>((rotation + 1) & 3);
    if (flip) {
        const std::uint8_t forward = static_cast<std::uint8_t>((rotation + 1) & 3);
        if ((static_cast<std::uint32_t>(rotation) | 2u) == 3u) first = forward;   // the odd rotations swap
        second = static_cast<std::uint8_t>((rotation + 3) & 3);
        if ((static_cast<std::uint32_t>(rotation) | 2u) != 3u) second = forward;
    }
    cSC4UndergroundOccupant* occupantA = nullptr;
    cSC4UndergroundOccupant* occupantB = nullptr;
    if (g_host) {
        occupantA = g_host->FindUndergroundOccupant(cellX_ + kEdgeCellOffset[first][0],
                                                    cellZ_ + kEdgeCellOffset[first][1], gziid_);
        occupantB = g_host->FindUndergroundOccupant(cellX_ + kEdgeCellOffset[second][0],
                                                    cellZ_ + kEdgeCellOffset[second][1], gziid_);
    }
    int countA = 0;
    if (occupantA) {
        countA = GetZipperCandidateCount(occupantA, first, 4);
        occupantA->Release();
    }
    if (!occupantB) return 0;
    const int countB = GetZipperCandidateCount(occupantB, second, 4);
    occupantB->Release();
    if (countA > 1 && countB > 1) return 2;
    if (countA == 1 && countB > 2) {
        param = true;
    } else {
        if (countA < 3) return 0;
        if (countB != 1) return 0;
        param = false;
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// view
// ---------------------------------------------------------------------------------------------------------------

// 0x1001ab3e8: re-resolves the static per-zoom texture bindings after the zoom level changed. Quirk: the instance
// base stored in the table grows by the zoom level on every call.
void cSC4UndergroundOccupant::ChangeZoomLevel(SC4DrawContext* context) {
    const std::int32_t zoom = context ? context->Zoom() : 0;
    sCurZoom_ = zoom;
    if (!g_host) return;
    for (auto& entry : ZoomTextureBindings()) {
        if (entry.binding) {
            entry.binding->Release();
            entry.binding = nullptr;
        }
        entry.instanceBase += static_cast<std::uint32_t>(zoom);
        entry.binding = g_host->CreateTextureBinding(kTextureBindingGroupZoom, entry.instanceBase);
    }
}

// 0x1001ab47c: the highlight quad, the trench and the wall quads (render pass 0) or the piece model (pass 1).
int cSC4UndergroundOccupant::Draw(SC4DrawContext* context, bool highlight) {
    if (needsModelCreate_ && g_host && g_host->IsCellInCity(cellX_, cellZ_)) {
        CreateUndergroundModel(GetRotation(), GetFlip() != 0, false);
        needsModelCreate_ = false;
    }
    if (highlight) {
        if (!highlight_.empty()) {
            context->ResetModelViewTransform();
            context->SetRenderState(mHighlightPackedRenderState);
            context->DrawPrims(1, 0x0a, static_cast<int>(highlight_.size()), highlight_.data());
        }
        return 1;
    }
    if (context->RenderPass() == 0) {
        if (trenchTexture_ && !trench_.empty()) {
            const std::uint32_t texture = trenchTexture_->TextureIds()[0];
            context->ResetModelViewTransform();
            context->SetRenderState(mTrenchPackedRenderState);
            context->SetTexture(static_cast<int>(texture), 0);
            context->SetTexWrapModes(2, 2, 0);
            context->SetTexFiltering(1, 7, 0);
            context->DrawPrims(6, 0x0a, static_cast<int>(trench_.size()), trench_.data());
        }
        if (!pieceTexture_ || wallCount_ == 0) return 2;
        const std::uint32_t texture = pieceTexture_->TextureIds()[0];
        context->ResetModelViewTransform();
        context->SetRenderState(mTrenchPackedRenderState);
        context->SetTexture(static_cast<int>(texture), 0);
        context->SetTexWrapModes(2, 2, 0);
        context->SetTexFiltering(1, 7, 0);
        for (std::uint32_t i = 0; i < wallCount_; ++i) {
            const std::vector<UndergroundVertex>& wall = walls_[i & 3];
            if (wall.empty()) continue;
            context->DrawPrims(6, 0x0a, static_cast<int>(wall.size()), wall.data());
        }
        return 2;
    }
    if (!HasNetworkFlag(kNetworkFlagUndergroundNoPiece) && context->RenderPass() == 1) {
        IUndergroundTextureBinding* first = nullptr;
        IUndergroundTextureBinding* second = nullptr;
        GetCurrentTextureBindings(first, second);
        context->SetShade(GetDrawColor());
        if (first && !piece_.empty()) {
            context->ResetModelViewTransform();
            context->SetRenderState(mPiecePackedRenderState);
            context->SetTexture(static_cast<int>(first->TextureIds()[0]), 0);
            context->SetTexWrapModes(2, 2, 0);
            context->SetTexFiltering(1, 7, 0);
            context->DrawPrims(1, 0x0a, static_cast<int>(piece_.size()), piece_.data());
            if (!piece2_.empty() && second) {
                context->SetTexture(static_cast<int>(second->TextureIds()[0]), 0);
                context->DrawPrims(1, 0x0a, static_cast<int>(piece2_.size()), piece2_.data());
            }
        }
        if (modelInstance_) {
            context->SetModelTransform(modelMatrix_);
            const s3d::SC4DrawAttributes attributes = modelInstance_->GetDrawAttributes();
            IUndergroundModelMeshes* meshes = g_host ? g_host->ModelMeshes(modelInstance_) : nullptr;
            if (meshes) {
                const int count = meshes->Count();
                for (int i = 0; i < count; ++i) {
                    s3d::cS3DMaterial* material = meshes->Material(i);
                    const SC4Mesh* mesh = meshes->Mesh(i);
                    material->flags = static_cast<std::uint16_t>(material->flags | 0x10);
                    material->b9 = 4;
                    material->b10 = 5;
                    context->SetRenderState(*material, &attributes);
                    if ((rotationFlip_ & 0x80) != 0) context->EnableCullFaceFlag(false);
                    context->RenderMesh(*mesh);
                }
                meshes->Release();
            }
        }
        context->ResetShade();
        modelDirty_ = false;
    }
    return 1;
}

// ---------------------------------------------------------------------------------------------------------------
// messages
// ---------------------------------------------------------------------------------------------------------------

// 0x1001abee4: the occupant message plus, for the manager insert / remove, a message on the standard server so the
// manager can update the piece's record.
void cSC4UndergroundOccupant::PostOccupantMessage(std::uint32_t message, std::uint32_t data) {
    cSC4Occupant::PostOccupantMessage(message, data);
    if ((message | 1u) != kMessageOccupantRemovedFromManager) return;
    PostManagerMessage(*this,
                       message == kMessageOccupantInserted ? kMessageUndergroundInserted : kMessageUndergroundRemoved,
                       data);
}

}  // namespace sc4
