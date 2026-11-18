// cSC4UndergroundOccupant — the base of the buried network pieces (water pipes, subway tubes and the tunnel
// occupants). 52 functions at 0x1001a1462..0x1001ae6d8 (89 symbols with the thunks and the two constructors /
// destructors). It adds the underground geometry to one network tile of cSC4NetworkOccupant: a trench (floor quad),
// up to four wall quads from the tile corners down to the terrain, the piece's own pipe/tube model and a highlight
// quad, all built into the vertex vectors Draw submits.
//
// Original object layout (0x358 bytes):
//   +0x000 cSC4NetworkOccupant (cSC4Occupant at +0x08, cS3DModelInstanceBasic at +0x68, transform at +0x9c,
//          four tile corners at +0xd4 (0x18 bytes each: xyz, rgba, uv), piece id +0x134, rotation|flip<<7 +0x13c,
//          edge store +0x140, network flags +0x188) — 0x1d0 bytes
//   +0x1d0 cS3DTextureBinding* mPieceTexture   (texture instance = geometry info +0xc8)
//   +0x1d8 u32                 mWallQuadCount  (how many of the four wall vectors Draw submits)
//   +0x1e0 std::vector<sVF_V3F_C4UB_T2F> mWallGeometry[4]   (0x60 bytes)
//   +0x240 cS3DTextureBinding* mTrenchTexture  (texture instance = geometry info +0xdc)
//   +0x248 std::vector<sVF_V3F_C4UB_T2F> mTrenchGeometry     (floor quad, unindexed triangle strip)
//   +0x260 std::vector<sVF_V3F_C4UB_T2F> mHighlightGeometry
//   +0x278 cS3DTextureBinding* mPieceTextures[6]  (0x278, 0x280, 0x288, 0x290, 0x298, 0x2a0)
//   +0x2a8 std::vector<sVF_V3F_C4UB_T2F> mPieceGeometry
//   +0x2c0 std::vector<sVF_V3F_C4UB_T2F> mPieceGeometry2
//   +0x2d8 cS3DMatrix4 mModelMatrix (row major, translation in the last column; built by CreateTransformationMatrix)
//   +0x318 int mCellX, +0x31c int mCellZ
//   +0x320 cIS3DModelInstance* mCustomModel   (the pre-built model of an underground connection piece)
//   +0x328 bool mHideTriangle0, +0x329 bool mHideTriangle1, +0x32a bool mHideTriangle2
//   +0x32c float mTerrainHeights[4]
//   +0x33c float mModelY
//   +0x340 bool mModelDirty (a Set/ClearNetworkFlag of bits 0x30000 sets it), +0x341 bool mNeedsModelCreate,
//   +0x342 bool mModelCreated
//   +0x344 u32 mGZIID   (the underground network this occupant belongs to: pipe 0x49c05b9f, subway 0xca16374f)
//   +0x348 u32 mNetworkTypeId (persisted only)
//   +0x350 tCustomGeometryInfo* mGeometryInfo   (the derived class's static table, from GetCustomGeometryInfo)
//
// Three values persisted by Write/Read live inside the cS3DModelInstanceBasic base at +0x68: the low two bytes of its
// draw-attribute flags (+0x88, +0x89) and the fourth shade component (+0x98). This port keeps the base as a seam
// (`modelInstance_`, a pointer the derived class or the host supplies) and stores those three values here.
//
// Quirks kept (addresses are the original functions):
//  * the geometry info pointer passed to Init(corners, gziid, info) is stored at +0x350 and immediately overwritten by
//    GetCustomGeometryInfo() (0x1001aa836..0x1001aa928), so a caller cannot override it.
//  * GetBoundingBox / ComputeBoundingBox take the maximum y from the terrain heights but the minimum y from the
//    corner heights (0x1001ab97e / 0x1001abcfc).
//  * GetPosition reports the *minimum* terrain height as y, not the average of the corners (0x1001ae2aa).
//  * SetQuadFlipAndTriHide sets network flag 0x1000000 when the first hidden-triangle flag is set but clears
//    0x2000000 / 0x4000000 when the other two are set (opposite polarity, 0x1001ad979).
//  * ChangeZoomLevel adds the zoom level to the static binding table's instance bases, so repeated calls keep
//    increasing them (0x1001ab3e8).
//  * GetHookupData only refines the height for piece ids 0x300 and 0x4b00 and reports "no hookup" for everything that
//    is not in {0x200, 0x300, 0x800, 0xa00, 0x1400, 0x4b00, 0x14e00} (0x1001ac242).
//  * the underground record writes its own version word (3) in front of the network occupant's version word (8).
//  * the piece model is only created when the piece is in a cell the city owns (Draw, 0x1001ab495).
//
// Seams (see docs/SEAMS.md): the texture-binding factory, the city (cell lookup / ownership), the renderer (the
// per-pass call and the model-instance registration), the resource manager (the pre-built piece model), the
// underground network tool (occupant lookups) and the +0x68 cS3DModelInstanceBasic base. Everything the seams stand
// for is a single original call, noted on the method.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/DrawContext.h"
#include "sc4/NetworkOccupant.h"

namespace sc4 {

// ---- ids ----------------------------------------------------------------------------------------------------
constexpr std::uint32_t kGZIID_cISC4UndergroundOccupant = 0xa821ef94;    // QueryInterface answers this with itself
constexpr std::uint32_t kGZIID_cISC4UndergroundOccupant2 = 0xa9764c8c;   // (two more underground interfaces)
constexpr std::uint32_t kGZIID_cISC4UndergroundOccupant3 = 0x4a10afc2;
constexpr std::uint32_t kUndergroundDefaultSortValue = 0x4a10afc2;       // GetSortValue before a piece is set
constexpr std::uint32_t kGZIID_cSC4PipeOccupant = 0x49c05b9f;            // the gziid a pipe passes to Init
constexpr std::uint32_t kGZIID_cSC4SubwayOccupant = 0xca16374f;          // the gziid a subway passes to Init

// piece ids (+0x134) the geometry code switches on
constexpr std::uint32_t kUndergroundPieceStraight = 0x200;
constexpr std::uint32_t kUndergroundPieceCrossing = 0x300;
constexpr std::uint32_t kUndergroundPieceDiagonal0 = 0x800;
constexpr std::uint32_t kUndergroundPieceDiagonal1 = 0xa00;
constexpr std::uint32_t kUndergroundPieceDiagonal2 = 0x1400;
constexpr std::uint32_t kUndergroundPiecePipe = 0x4b00;
constexpr std::uint32_t kUndergroundPieceSubway = 0x14e00;

// network flag bits the underground geometry uses (+0x188)
constexpr std::uint32_t kNetworkFlagHideTriangle0 = 0x01000000;   // SetQuadFlipAndTriHide
constexpr std::uint32_t kNetworkFlagHideTriangle1 = 0x02000000;
constexpr std::uint32_t kNetworkFlagHideTriangle2 = 0x04000000;
constexpr std::uint32_t kNetworkFlagUndergroundNoPiece = 0x00200000;   // Draw skips the piece model
constexpr std::uint32_t kNetworkFlagUndergroundDirty = 0x00030000;     // Set/ClearNetworkFlag watch these bits

// messages: PostOccupantMessage posts one of these after the occupant message of the same call
constexpr std::uint32_t kMessageUndergroundRemoved = 0x8b87a975;   // used for 0x99ef1143 (removed from manager)
constexpr std::uint32_t kMessageUndergroundInserted = 0x8b87a974;  // used for 0x99ef1142 (inserted)

// texture-binding keys (type 0x7ab50e44)
constexpr std::uint32_t kTextureBindingType = 0x7ab50e44;
constexpr std::uint32_t kTextureBindingGroupPiece = 0x891b0e1a;    // instance from the geometry info
constexpr std::uint32_t kTextureBindingGroupZoom = 0x1abe787d;     // instance = base + zoom level

// ---- vertices -----------------------------------------------------------------------------------------------
// sVF_V3F_C4UB_T2F: what Draw submits (0x18 bytes, the format is 10 = V3F_C4UB_T2F).
struct UndergroundVertex {
    float x = 0, y = 0, z = 0;                   // +0
    std::uint8_t color[4] = {0xff, 0xff, 0xff, 0xff};   // +0xc rgba
    float u = 0, v = 0;                          // +0x10
};
static_assert(sizeof(UndergroundVertex) == 0x18, "sVF_V3F_C4UB_T2F stride");

// One 0x14-byte record of the per-piece texture table at tCustomGeometryInfo +0xc8. Only the first word (the texture
// binding instance) is read by the recovered code.
struct UndergroundTextureRecord {
    std::uint32_t instance = 0;                  // +0
    std::uint32_t data[4] = {};                  // +4 (not read by the recovered code)
};
static_assert(sizeof(UndergroundTextureRecord) == 0x14, "texture record stride");

// The per-network geometry table the derived occupants hand to Init: cSC4PipeOccupant::sCustomGeometryInfo @
// 0x100b01b58 and cSC4SubwayOccupant::sCustomGeometryInfo @ 0x100b02748. The named fields are the ones the recovered
// functions read; the per-piece vertex arrays at +0x00..+0x40 are only used by CreateUndergroundModel
// and keep their raw slots. Sizes verified against both statics.
struct tCustomGeometryInfo {
    const void* pieceArrays[9];                  // +0x00 .. +0x40 (per-piece vertex / index pointers)
    std::int32_t counts[6];                      // +0x48 .. +0x5c (12, 6, 12, 6, 6, 0 in both statics)
    const UndergroundVertex* vertices;           // +0x60 the model's vertex array
    const std::int32_t* edge0VertIndices;        // +0x68
    const std::int32_t* edge1VertIndices;        // +0x70
    const std::int32_t* hingeVertIndices;        // +0x78
    float* edgeDistancePercentages;              // +0x80 (written by Init)
    const UndergroundVertex* pieceVertices;      // +0x88
    const UndergroundVertex* pieceVertices2;     // +0x90
    const std::int32_t* pieceIndices;            // +0x98
    const std::int32_t* pieceIndices2;           // +0xa0
    float* hingeDistancePercentages;             // +0xa8 (written by Init)
    std::int32_t countB0;                        // +0xb0 (21 in both statics)
    std::int32_t percentageCount;                // +0xb4 (5 in both statics)
    std::int32_t countB8;                        // +0xb8 (11)
    std::int32_t countBC;                        // +0xbc (6)
    std::int32_t countC0;                        // +0xc0 (5)
    std::int32_t countC4;                        // +0xc4 (6)
    UndergroundTextureRecord textureRecords[17];  // +0xc8 .. +0x21c
};
static_assert(offsetof(tCustomGeometryInfo, textureRecords) == 0xc8, "texture table offset");
// (the struct ends at +0x21c; the compiler pads it to 8 for the pointers. The original statics are followed
// by unrelated globals, so the exact tail size does not matter.)
static_assert(offsetof(tCustomGeometryInfo, pieceArrays) == 0x00, "field offsets");
static_assert(offsetof(tCustomGeometryInfo, vertices) == 0x60, "field offsets");
static_assert(offsetof(tCustomGeometryInfo, hingeDistancePercentages) == 0xa8, "field offsets");
static_assert(offsetof(tCustomGeometryInfo, percentageCount) == 0xb4, "field offsets");

// ---- seams --------------------------------------------------------------------------------------------------
// cS3DTextureBinding as the underground geometry uses it: the first word of the binding is the GL texture id.
class IUndergroundTextureBinding {
public:
    virtual ~IUndergroundTextureBinding() = default;
    virtual const std::uint32_t* TextureIds() = 0;   // **binding in Draw
    virtual void Release() = 0;                      // cS3DTextureBinding::Release @ 0x1000d9bfa
};

// cSC4UndergroundConnectionOccupant (unrecovered): only the reference count is touched by this class.
class IUndergroundConnectionOccupant {
public:
    virtual ~IUndergroundConnectionOccupant() = default;
    virtual void Release() = 0;                      // cIGZUnknown vslot 0x10
};

// cIS3DMeshAndMaterial (iid 0x26a68448) as Draw walks it after GZCOM_Interface<cIS3DModelInstance,
// cIS3DMeshAndMaterial> @ 0x1005ee244.
class IUndergroundModelMeshes {
public:
    virtual ~IUndergroundModelMeshes() = default;
    virtual int Count() = 0;                                  // vslot 0x18
    virtual const SC4Mesh* Mesh(int index) = 0;               // vslot 0x28
    virtual s3d::cS3DMaterial* Material(int index) = 0;       // vslot 0x38
    virtual void Release() = 0;                               // vslot 0x10
};

class cSC4UndergroundOccupant;

class IUndergroundOccupantHost {
public:
    virtual ~IUndergroundOccupantHost() = default;
    // cSC4NetworkOccupant::StaticInit @ 0x10019c248 (inlined into cSC4UndergroundOccupant::StaticInit 0x1001aa79a):
    // resource manager, texture binding factory, the three packed render states and the avenue intersection set.
    virtual void InitNetworkOccupantStatics() = 0;
    // RZGetFramework()->GetSystemService(0x1ac0e11a, 0xfac0e219)->vslot 0x20({0x7ab50e44, group, instance}, &out).
    // The caller owns the result and Releases it.
    virtual IUndergroundTextureBinding* CreateTextureBinding(std::uint32_t group, std::uint32_t instance) = 0;
    // SL::spCity (0x100b21308) vslot 0x2f8: world x / z -> city cell.
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;
    // SL::spCity vslot 0x318: the cell belongs to the city (Draw, ModelStateChanged).
    virtual bool IsCellInCity(int cellX, int cellZ) = 0;
    // SL::spRenderer (0x100b214d0) vslot 0x50: the call at the start of SetupPackedRenderState.
    virtual void PrepareRenderer() = 0;
    // SL::spRenderer vslot 0xf8(model instance, 3): ModelStateChanged registers the piece with the renderer.
    virtual void AddModelInstanceToRenderer(s3d::cIS3DModelInstance* instance, int layer) = 0;
    // SL::spRM vslot 0x18({0x5ad0e817, 0xbadb57f1, networkType | pieceId}, 0x9ad0e7e3, &exemplar) followed by
    // cISCResExemplar vslot 0x20(0, &model). Returns one reference (the caller owns it) or null.
    virtual s3d::cIS3DModelInstance* CreateCustomModel(std::uint32_t instance) = 0;
    // cSC4UndergroundNetworkTool::FindUndergroundOccupant @ 0x1001a7d3e: one reference, null when the cell is empty.
    virtual cSC4UndergroundOccupant* FindUndergroundOccupant(int cellX, int cellZ, std::uint32_t gziid) = 0;
    // cSC4UndergroundNetworkTool::FindUndergroundConnectionOccupant @ 0x1001a81d6: true when the cell holds a
    // connection piece, whose reference then has to be released.
    virtual bool FindUndergroundConnectionOccupant(int cellX, int cellZ, std::uint32_t gziid,
                                                   IUndergroundConnectionOccupant*& out) = 0;
    // the model's triangle data (GZCOM_Interface<cIS3DModelInstance, cIS3DMeshAndMaterial>(iid 0x26a68448))
    virtual IUndergroundModelMeshes* ModelMeshes(s3d::cIS3DModelInstance* instance) = 0;
};
IUndergroundOccupantHost* UndergroundOccupantHost();
void SetUndergroundOccupantHost(IUndergroundOccupantHost* host);

// the cell offset of each edge (0 = -x, 1 = -z, 2 = +x, 3 = +z); edgeToCellOffset, i386 0x00bfb1a0 (filled by the
// static initializer at 0x00a808ca)
constexpr std::int32_t kEdgeCellOffset[4][2] = {{-1, 0}, {0, -1}, {1, 0}, {0, 1}};

// ---- the occupant -------------------------------------------------------------------------------------------
// The methods that are vtable slots in the original are virtual here even where the recovered base class has a
// non-virtual method of the same name (GetVertexHeights / ComputeBoundingBox / SetVertexHeights): the derived pipe and
// subway occupants are supposed to override the ones the original overrides (GetSortValue, GetDrawColor, ...).
class cSC4UndergroundOccupant : public cSC4NetworkOccupant {
public:
    cSC4UndergroundOccupant();                  // 0x1001aa4b0
    ~cSC4UndergroundOccupant() override;        // 0x1001aa61c (+ deleting 0x1001aa71e and the thunks)

    // cSC4Occupant / cSC4NetworkOccupant
    bool QueryInterface(std::uint32_t iid, void** out) override;              // 0x1001abdba
    std::uint32_t AddRef() override { return cSC4NetworkOccupant::AddRef(); }   // 0x1001ae4be
    std::uint32_t Release() override { return cSC4NetworkOccupant::Release(); } // 0x1001ae510
    bool Init() override;                                                    // 0x1001aa750 (vslot 0x260)
    bool Init(const float (*corners)[3], std::uint32_t gziid, tCustomGeometryInfo* info);   // 0x1001aa7de (vslot 0x320)
    bool Shutdown() override;   // 0x1001aabc4 (vslot 0x20; the original returns void, the recovered base bool)
    bool Write(cIGZOStream& stream) const override;                          // 0x1001aae32 (vslot 0x268)
    bool Read(cIGZIStream& stream) override;                                 // 0x1001ab0f2 (vslot 0x270)
    void GetPosition(float* xyz) override;                                   // 0x1001ae2aa (vslot 0x290)
    void GetBoundingBox(float* minXyz, float* maxXyz) override;              // 0x1001ab97e (vslot 0x2a8)
    void SetHighlight(int highlight, bool postNow) override;                 // 0x1001ae372 (vslot 0x288, empty)
    void PostOccupantMessage(std::uint32_t message, std::uint32_t data) override;   // 0x1001abee4 (vslot 0x3c0)
    virtual void SetNetworkFlag(std::uint32_t flags) override;               // 0x1001ae5f8 (vslot 0x50)
    virtual void ClearNetworkFlag(std::uint32_t flags) override;             // 0x1001ae642 (vslot 0x58)

    // the underground interface (vtable slots of the original)
    virtual void AddModel(cISCResExemplar*, const float*, std::uint8_t) {}            // 0x1001ab3e2 (vslot 0x330, empty)
    virtual void SetQuadDraw(std::uint32_t pieceId, std::uint8_t rotationFlip);      // 0x1001ac7b8 (vslot 0xf0)
    virtual bool HasViewDependentModel() const { return false; }                     // 0x1001ae6c0 (vslot 0x100)
    virtual void GetModelInstances(int x, int z, s3d::cIS3DModelInstance** out, int capacity, int& count);   // 0x1001aba4a
    virtual void GetVertexHeights(float* heights) const;                             // 0x1001ae26a (vslot 0x110)
    virtual void SetVertexHeights(float* heights);                                   // 0x1001ae216 (vslot 0x338)
    virtual void GetSortValue(std::uint32_t& out) const;                             // 0x1001abe72 (vslot 0x2e8)
    virtual void ChangeZoomLevel(SC4DrawContext* context);                           // 0x1001ab3e8 (vslot 0x308)
    virtual void ComputeBoundingBox(float* minXyz, float* maxXyz);                   // 0x1001abcfc (vslot 0x318)
    std::uint32_t GetGZIID() const { return gziid_; }                        // 0x1001ae6c8 (vslot 0x328)
    virtual void CreateTransformationMatrix();                                       // 0x1001add20 (vslot 0x340)
    virtual void CreateGeometry(bool update);                                        // 0x1001ae192 (vslot 0x348)
    virtual void SetQuadFlipAndTriHide();                                            // 0x1001ad83c (vslot 0x358)
    virtual void CreateWalls(bool update);                                           // 0x1001ada7a (vslot 0x360)
    virtual void CreateFloor(bool update);                                           // 0x1001adc4e (vslot 0x368)
    virtual void CreateHighlight(bool) {}                                            // 0x1001add1a (vslot 0x370, empty)
    virtual void GetHiddenTriangles(bool& t0, bool& t1, bool& t2) const;             // 0x1001ae28c (vslot 0x378)
    virtual void GetHookupData(float& height, bool& valid, int edge);                // 0x1001ac242 (vslot 0x380)
    virtual void GetTerrainHeights(float* heights);                                  // 0x1001abce0 (vslot 0x388)
    virtual void SetTerrainHeights(float* heights);                                  // 0x1001aba88 (vslot 0x390)
    virtual void GetQueryData(std::uint32_t& pieceId, int& networkTypeId, float* heights, float& modelY);   // 0x1001ae338
    static void SetTuningParams(float pieceDepth, bool usePieceDepth, float intersectionHeight);   // 0x1001aba6a
    virtual int GetZipperCandidateCount(cSC4UndergroundOccupant* occupant, int edge, int depth);           // 0x1001ac7ce
    virtual int GetZipperPieceType(bool& param);   // 0x1001ac8e0: 0 no zipper, 1 zipper (param = its direction), 2 both sides
    virtual void ModelStateChanged(bool updateNeighbors);                            // 0x1001ae37e (vslot 0x3b8)
    void ComputeModelYPosition();                                            // 0x1001abace
    virtual cS3DVector3 GetDrawColor() const;                                        // 0x1001ae562 (vslot 0x3d8)
    virtual void GetCurrentTextureBindings(IUndergroundTextureBinding*& a, IUndergroundTextureBinding*& b);   // 0x1001ae3e0
    bool IsPlaced();                                                         // 0x1001a1462 (vslot 0xa8)
    int Draw(SC4DrawContext* context, bool highlight);                       // 0x1001ab47c (vslot 0x2d8)
    int Draw(SC4DrawContext* context) { return Draw(context, false); }       // convenience

    // the three pure virtuals of the original base (slots 0x3c8 / 0x3d0 / 0x3e8)
    virtual const tCustomGeometryInfo* GetCustomGeometryInfo() const = 0;    // 0x3c8 (pipe / subway statics)
    virtual std::uint32_t GetNetworkType() const = 0;                        // 0x3d0 (pipe 4, subway 7)
    virtual std::int32_t GetModelInstanceOffset() const = 0;                 // 0x3e8 (pipe 0x6000000, subway 0x7000000)

    void AddWallQuad(std::uint32_t cornerA, std::uint32_t cornerB, std::vector<UndergroundVertex>& out);   // 0x1001ac048
    void MakeCustomUndergroundPiece(bool flipU, bool flipV, std::vector<UndergroundVertex>& out,
                                    const UndergroundVertex* vertices, int count, const std::int32_t* indicesA,
                                    int countA, int edgeA, const std::int32_t* indicesB, int countB, int edgeB,
                                    const float* hookupScale);               // 0x1001ac31c
    bool IsImaginaryConnection(int edge);                                    // 0x1001ada00

    // The model and the six texture bindings of one piece from the tCustomGeometryInfo piece tables.
    virtual void CreateUndergroundModel(int rotation, bool flip, bool update);   // 0x1001acae0 (i386 0x0013fe30)

    // static state the original keeps in cSC4UndergroundOccupant / cSC4NetworkOccupant statics
    static bool StaticInit();                                   // 0x1001aa79a
    static void SetupPackedRenderState();                       // 0x1001aad0e
    static std::int32_t CurrentZoom() { return sCurZoom_; }     // cSC4NetworkOccupant::sCurZoom
    // The original's zoom texture table (0x100b245f0): an intrusive list of {next, unused, instanceBase, binding}.
    // Its filler (a derived class's static init) is not recovered; a test or the port registers the entries here.
    struct ZoomTextureBinding {
        std::uint32_t instanceBase = 0;             // +0x10 in the original node
        IUndergroundTextureBinding* binding = nullptr;   // +0x18
    };
    static std::vector<ZoomTextureBinding>& ZoomTextureBindings();
    static const cSC4PackedRenderState& TrenchPackedRenderState();
    static const cSC4PackedRenderState& PiecePackedRenderState();
    static const cSC4PackedRenderState& HighlightPackedRenderState();

    // port wiring: the +0x68 cS3DModelInstanceBasic base (not owned), the three values the record keeps for it,
    // and the +0x320 custom model (owned, released in Shutdown).
    void SetModelInstance(s3d::cIS3DModelInstance* instance) { modelInstance_ = instance; }
    s3d::cIS3DModelInstance* ModelInstance() const { return modelInstance_; }
    std::uint8_t ModelAttributeFlags0() const { return modelFlags0_; }
    std::uint8_t ModelAttributeFlags1() const { return modelFlags1_; }
    float ModelShade3() const { return modelShade3_; }
    void SetModelAttributeState(std::uint8_t flags0, std::uint8_t flags1, float shade3) {
        modelFlags0_ = flags0; modelFlags1_ = flags1; modelShade3_ = shade3;
    }
    s3d::cIS3DModelInstance* CustomModel() const { return customModel_; }
    void SetCustomModel(s3d::cIS3DModelInstance* model);
    const std::vector<UndergroundVertex>& WallGeometry(int edge) const { return walls_[edge]; }
    const std::vector<UndergroundVertex>& TrenchGeometry() const { return trench_; }
    const std::vector<UndergroundVertex>& HighlightGeometry() const { return highlight_; }
    const std::vector<UndergroundVertex>& PieceGeometry() const { return piece_; }
    const std::vector<UndergroundVertex>& PieceGeometry2() const { return piece2_; }
    std::uint32_t WallQuadCount() const { return wallCount_; }              // +0x1d8
    const cS3DMatrix4& ModelMatrix() const { return modelMatrix_; }         // +0x2d8
    float ModelY() const { return modelY_; }                                // +0x33c
    bool ModelDirty() const { return modelDirty_; }                         // +0x340
    void SetModelDirty(bool dirty) { modelDirty_ = dirty; }                 // a derived Read sets +0x340
    bool NeedsModelCreate() const { return needsModelCreate_; }             // +0x341
    bool ModelCreated() const { return modelCreated_; }                     // +0x342
    std::uint32_t NetworkTypeId() const { return networkTypeId_; }          // +0x348
    const tCustomGeometryInfo* GeometryInfo() const { return geometryInfo_; }   // +0x350
    IUndergroundTextureBinding* PieceTexture() const { return pieceTexture_; }  // +0x1d0
    IUndergroundTextureBinding* TrenchTexture() const { return trenchTexture_; }   // +0x240
    IUndergroundTextureBinding* PieceTextureAt(int index) const { return pieceTextures_[index]; }   // +0x278

private:
    void ApplyTriangleFlags(bool hide0);

    IUndergroundTextureBinding* pieceTexture_ = nullptr;      // +0x1d0
    std::uint32_t wallCount_ = 0;                             // +0x1d8
    std::vector<UndergroundVertex> walls_[4];                 // +0x1e0
    IUndergroundTextureBinding* trenchTexture_ = nullptr;      // +0x240
    std::vector<UndergroundVertex> trench_;                    // +0x248
    std::vector<UndergroundVertex> highlight_;                 // +0x260
    IUndergroundTextureBinding* pieceTextures_[6] = {};        // +0x278
    std::vector<UndergroundVertex> piece_;                     // +0x2a8
    std::vector<UndergroundVertex> piece2_;                    // +0x2c0
    cS3DMatrix4 modelMatrix_;                                  // +0x2d8
    std::int32_t cellX_ = 0;                                   // +0x318
    std::int32_t cellZ_ = 0;                                   // +0x31c
    s3d::cIS3DModelInstance* customModel_ = nullptr;           // +0x320
    std::uint8_t modelFlags0_ = 0;                             // +0x88 (inside the model-instance base)
    std::uint8_t modelFlags1_ = 0;                             // +0x89
    float modelShade3_ = 0.0f;                                 // +0x98
    bool hideTriangle_[3] = {false, false, false};             // +0x328 (counted as three bytes at 0x328..0x32a)
    float terrainHeights_[4] = {};                             // +0x32c
    float modelY_ = 0.0f;                                      // +0x33c
    bool modelDirty_ = false;                                  // +0x340
    bool needsModelCreate_ = false;                            // +0x341
    bool modelCreated_ = false;                                // +0x342
    std::uint32_t gziid_ = 0;                                  // +0x344
    std::uint32_t networkTypeId_ = 0;                          // +0x348
    const tCustomGeometryInfo* geometryInfo_ = nullptr;        // +0x350
    s3d::cIS3DModelInstance* modelInstance_ = nullptr;         // the +0x68 base (port: a pointer, not owned)

    static std::int32_t sCurZoom_;                             // cSC4NetworkOccupant::sCurZoom
};

}  // namespace sc4
