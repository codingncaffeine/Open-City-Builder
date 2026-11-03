// cSC4NetworkOccupantWithPreBuiltModel — the base of the network occupants that draw a pre-built 3-D model
// (tunnels, bridges, the pre-built-model pieces). 33 functions at 0x1001a2964..0x1001d2ae4 (67 with the
// compiler thunks and the two constructors / three destructors). It adds to one network tile of
// cSC4NetworkOccupant:
//   * a model offset and two fill (shear) rows, so the pre-built model is laid on the tile corners
//     (RecomputeShearTransform / TransformNode / GetAltitude / GetNormal);
//   * the pre-built model itself (+0x220) and an optional support model (+0x228, AddSupportModel), created
//     from the exemplar's model instance id and drawn with a rotation / flip transform (GetModelInstances);
//   * the two ground shadows the piece casts, one per land / water overlay layer (UpdateShadow);
//   * the path info of the piece (+0x190, GetPathInfo) and the per-type path connection bitmap (+0x198).
//
// Original object layout (0x230 bytes):
//   +0x000 cSC4NetworkOccupant (0x1d0 bytes; cSC4Occupant at +0x08, cS3DModelInstanceBasic at +0x68,
//          transform +0x9c, four tile corners +0xd4 (0x18 bytes each), piece id +0x134, rotation|flip<<7
//          +0x13c, edge store +0x140, network flags +0x188, path info +0x190, path connection bitmap +0x198,
//          path id override +0x1b8, path origin +0x1bc / +0x1c0, demolition cost +0x1c8)
//   +0x1d0 cS3DVector3 mModelOffset              (added to the model position, set by AddModel)
//   +0x1dc cS3DVector3 mFillA                    (first fill row: x factor, z factor, constant)
//   +0x1e8 cS3DVector3 mFillB                    (second fill row, used for the other diagonal)
//   +0x1f4 u32         mSupportModelId           (0 = no support model; Write only stores the rest when set)
//   +0x1f8 float       mSupportRotation
//   +0x1fc cS3DVector3 mSupportOffset
//   +0x208 u32         mShadowPieceId            (overrides the piece id used for the shadow when non-zero)
//   +0x20c u8          mShadowRotation           (its rotation and flip)
//   +0x210 i32         mShadowLand               (land overlay handle, -1 none, -2 pending)
//   +0x214 i32         mShadowWater              (water overlay handle, same sentinels)
//   +0x218 u8          mLastZoom                 (zoom GetModelInstances last ran at)
//   +0x219 u8          mLastShadowView           (the shadow-mode byte Draw last saw; starts 0xff)
//   +0x220 cIS3DModelInstance* mModel            (the pre-built model, counted)
//   +0x228 cIS3DModelInstance* mSupportModel     (counted)
//
// Quirks kept (addresses are the original functions):
//  * the constructor never initialises mModelOffset (0x1d0..0x1d8) or mSupportRotation (0x1f8); AddModel /
//    AddSupportModel are expected to set them. The port zeroes them (an uninitialised float is undefined
//    behaviour) — see PREBUILT-1.
//  * UpdateShadow sets both shadow handles to -2 *after* asking the network manager for the shadow texture and
//    leaves them at -2 when the manager returns 0 or when the piece sits on flat terrain; the next call then
//    resolves them to -1 without ever removing an overlay (0x1001ba37a).
//  * the shadow position uses (float)(std::uint32_t)(cellX << 4), so a negative cell wraps to a huge float
//    (0x1001ba2bc).
//  * the shadow rotation is (flip ? rotation + 2 : -rotation) & 3, not the piece rotation (0x1001ba2e4).
//  * the shadow is only created while the night lighting is off (lighting manager vslot 0x48 returns false,
//    0x1001ba4fd).
//  * AddModel checks property 0x6a845768 on the *exemplar* but sets property 0x4835c6a6 on the occupant
//    (0x1001ba8f4).
//  * ComputeBoundingBox skips the occupant's own cS3DModelInstanceBasic (+0x68) in the instance union, then
//    clamps the union to corner 0 (min) and corner 2 (max) in x / z, which can invert the box (0x1001baf00).
//  * the skipped own instance is still released, but its owner is *not* cleared; and a count of zero returns
//    without writing the output box at all (0x1001baa63).
//  * the model's world bounds are only unioned when box.min.x <= box.max.x; a rejected box is skipped but its
//    instance is still released (0x1001bacc0).
//  * Read accepts only versions 3 and 4; version 3 skips six words before and one word after the new fields
//    (0x1001bc5e4). The base record then has to be version 4 or later, so version 3 records fail.
//
// Seams (see docs/SEAMS.md): the rendering half of cSC4NetworkOccupant (Draw / GetModelInstances /
// UpdateLighting), the lighting manager, the view utilities, the render properties shadow-mode byte, the
// terrain queries and shadow overlay managers, the network manager shadow-texture / path mapping, the model
// maker utility, the resource manager (models and path info) and cSC4NetworkOccupantModelInstance. Every seam
// method mirrors one original call and carries the slot / address in its comment.
#pragma once
#include <cstddef>
#include <cstdint>

#include "gz/StreamOps.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/PathInfo.h"
#include "s3d/ModelInstance.h"

class SC4DrawContext;

namespace sc4 {

// ---- ids ----------------------------------------------------------------------------------------------------
constexpr std::uint32_t kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel = 0x49c1a034;
// QueryInterface answers these two with itself as well (the underground occupant iids).
constexpr std::uint32_t kGZIID_PreBuiltUndergroundOccupant = 0xa821ef94;
constexpr std::uint32_t kGZIID_PreBuiltUndergroundOccupant2 = 0xa9764c8c;

// network flag bits this class tests (+0x188)
constexpr std::uint32_t kNetworkFlagPreBuiltDiagonal = 0x01000000;   // picks the fill row, flips the z term
constexpr std::uint32_t kNetworkFlagPreBuiltShadowCheck = 0x00040000;   // UpdateShadow's height check
constexpr std::uint32_t kNetworkFlagPreBuiltShadowFlatMask = 0x00040304;
constexpr std::uint32_t kNetworkFlagPreBuiltConstructed = 0x00400000;   // the bit the constructor ORs in
constexpr std::uint32_t kNetworkFlagPreBuiltKeepPaths = 0x00100000;   // do not drop the wrong-way paths

// property ids (AddModel / GetModelInstances)
constexpr std::uint32_t kPreBuiltPropModel = 0x4835c6a6;          // set on the occupant by AddModel
constexpr std::uint32_t kPreBuiltPropExemplarFlags = 0x27812821;   // read on the exemplar
constexpr std::uint32_t kPreBuiltPropWaterModel = 0x6a845768;      // read on the exemplar

// resource keys
constexpr std::uint32_t kPreBuiltModelType = 0x5ad0e817;          // {type, 0xbadb57f1, model instance id}
constexpr std::uint32_t kPreBuiltModelGroup = 0xbadb57f1;
constexpr std::uint32_t kPreBuiltModelIID = 0x9ad0e7e3;           // the 3-D model resource iid
constexpr std::uint32_t kPreBuiltShadowType = 0x7ab50e44;         // {type, 0x2bc2759a, shadow instance}
constexpr std::uint32_t kPreBuiltShadowGroup = 0x2bc2759a;
constexpr std::uint32_t kPreBuiltShadowKeyIID = 0x2bc2759a;       // (same group; kept for the key literal)
constexpr std::uint32_t kPreBuiltPathInfoType = 0x296678f7;       // {type, 0xa966883f, mapped path id}
constexpr std::uint32_t kPreBuiltPathInfoGroup = 0xa966883f;

// the four cell offsets UpdateShadow's terrain check walks (0x100a1a5a0 / 0x100a1a5b0)
constexpr std::int32_t kShadowCheckNextX[4] = {0, 0, 1, 1};
constexpr std::int32_t kShadowCheckNextZ[4] = {0, 1, 1, 0};

// ---- seams --------------------------------------------------------------------------------------------------
// cISTEOverlayManager (the land / water shadow layer, unrecovered): the two calls UpdateShadow makes.
class IShadowOverlay {
public:
    virtual ~IShadowOverlay() = default;
    // cSTEOverlayManager::AddShadowDecal(cGZPersistResourceKey const&, cS3DVector2 const&, float, float, bool)
    // @ 0x1007292c6 (vslot 0x50). The float arguments are 16.0f and the shadow rotation; returns the overlay id.
    virtual std::uint32_t AddShadowDecal(const cGZPersistResourceKey& key, const cS3DVector2& position,
                                         float height, float rotation, bool flip) = 0;
    // cSTEOverlayManager::RemoveOverlay(unsigned int) @ 0x100729e80 (vslot 0x78).
    virtual void RemoveOverlay(std::uint32_t overlayId) = 0;
};

// cSC4NetworkOccupantModelInstance (unrecovered, 0x100714a7c..0x1007159e0). The extra methods GetModelInstances
// calls; the COM part (AddRef / Release / SetOwner / Get- / SetTransform / draw attributes) is the +0x148
// cIS3DModelInstance the same object implements.
class INetworkOccupantModelInstance {
public:
    virtual ~INetworkOccupantModelInstance() = default;
    virtual s3d::cIS3DModelInstance* ModelInstance() = 0;      // the +0x148 cIS3DModelInstance sub-object
    virtual bool Init(s3d::cIS3DResModel* model, std::uint32_t param) = 0;   // 0x100714bb0
    virtual void Prelight() = 0;                               // 0x10071563e
    virtual void Deform(int rotation, bool flip, const cS3DVector3& fillA, const cS3DVector3& fillB,
                        const cS3DVector3& offset, bool diagonal) = 0;       // 0x1007159e0
    virtual void FlipAndLight(bool flip) = 0;                  // 0x10071565c
    virtual void Clip(const cS3DVector3& lo, const cS3DVector3& hi, bool flag) = 0;   // 0x100714c22
};

class cSC4NetworkOccupantWithPreBuiltModel;
class IPreBuiltModelOccupantHost {
public:
    virtual ~IPreBuiltModelOccupantHost() = default;

    // ---- cSC4NetworkOccupant, rendering half (unrecovered in NetworkOccupant.cpp) ----
    // cSC4NetworkOccupant::Draw(SC4DrawContext*, bool) @ 0x10019e09a (vslot 0x2d8).
    virtual void DrawOccupant(cSC4NetworkOccupant& occupant, SC4DrawContext* context, bool highlight) = 0;
    // cSC4NetworkOccupant::GetModelInstances(int, int, cIS3DModelInstance**, int, int&) @ 0x10019eb90 (vslot 0x108).
    virtual void GetOccupantModelInstances(cSC4NetworkOccupant& occupant, int zoom, int rotation,
                                           s3d::cIS3DModelInstance** out, int capacity, int& count) = 0;
    // cSC4NetworkOccupant::UpdateLighting(cISC4LightingManager*) @ 0x10019f978 (vslot 0x310); the original
    // passes SL::spLightingManager.
    virtual void UpdateOccupantLighting(cSC4NetworkOccupant& occupant) = 0;

    // ---- cSC4LightingManager (unrecovered, SL::spLightingManager) ----
    virtual bool IsNight() = 0;                             // vslot 0x48: the lighting state Init copies into SetLit
    virtual void GetSunDirection(cS3DVector3& out) = 0;     // vslot 0x68: the three floats UpdateShadow offsets by

    // ---- SC4ViewUtilities (SL::spViewUtilities) ----
    virtual void GetZoomAndRotation(int& zoom, int& rotation) = 0;   // 0x10017375a
    virtual float GetObjectYRotationAngle(int rotation) = 0;         // 0x1001771a0
    virtual float ViewRotationAngle() = 0;                           // +0x24 (rotation * pi/2)

    // ---- render properties: *(int*)(*(long*)(SL::spRenderProperties + 0x30) + 0x398), the shadow-mode word
    // UpdateShadow compares (> 2) and Draw keys on (low byte) ----
    virtual std::int32_t ShadowViewMode() = 0;

    // ---- terrain (SL::spCity vslot 0x2a0 returns it) ----
    virtual bool CellHasWater(std::uint32_t cellX, std::uint32_t cellZ) = 0;   // terrain vslot 0x110
    virtual float TerrainAltitudeAtVertex(int x, int z) = 0;                   // terrain vslot 0x70
    virtual float WaterLevel(float x, float z) = 0;                            // terrain vslot 0xf8
    virtual bool WaterSurfaceDisplayed() = 0;                                  // terrain vslot 0x298
    // SL::spCity 0x2a0 -> cSTETerrain::GetView @ 0x100739a1e (terrain vslot 0x210) -> cISTETerrainView
    // vslot 0x90 GetOverlayManager(0 = land, 1 = water) @ 0x10074ce10. False when the chain is incomplete.
    virtual bool GetShadowOverlays(IShadowOverlay** land, IShadowOverlay** water) = 0;

    // ---- cSC4NetworkManager::MapShadowTexture(unsigned int, unsigned char&, unsigned int, unsigned int) @
    // 0x10016ece4 (SL::NetworkManager() vslot 0x70); returns the remapped instance ----
    virtual std::uint32_t MapShadowTexture(std::uint32_t instance, std::uint8_t& rotation,
                                           std::uint32_t networkFlags, std::uint32_t raisedEdges) = 0;

    // ---- cSC4ModelMakerUtility::GetModelInstanceID(cISC4Occupant*, int, int, int, bool, uint*, uint*) @
    // 0x1002c03da ----
    virtual std::uint32_t GetModelInstanceID(cISC4Occupant* occupant, int zoom, int rotation, int rotationAndFlip,
                                             bool flag, std::uint32_t* a, std::uint32_t* b) = 0;
    // cSC4NetworkOccupant::spResMan vslot 0x18(key, 0x9ad0e7e3, &model, 0, 0): one reference.
    virtual s3d::cIS3DResModel* CreateModelResource(const cGZPersistResourceKey& key) = 0;
    // new cSC4NetworkOccupantModelInstance (0x150 bytes, ctor 0x100714a7c). Reference count 0, like the ctor.
    virtual INetworkOccupantModelInstance* CreateOccupantModelInstance() = 0;

    // ---- renderer (SL::spRenderer): vslot 0xd8 HasModelInstance, vslot 0xf8 AddModelInstance(instance, layer) ----
    virtual bool HasModelInstance(s3d::cIS3DModelInstance* instance) = 0;
    virtual void AddModelInstanceToRenderer(s3d::cIS3DModelInstance* instance, int layer) = 0;

    // ---- cSC4NetworkOccupant::spResMan vslot 0x18(key, 0xa9667793, &pathInfo, 0, 0): one reference ----
    virtual cISC4PathInfo* GetPathInfoResource(const cGZPersistResourceKey& key) = 0;
    // cSC4NetworkManager::MapPath(unsigned int) @ 0x10016ee44 (SL::NetworkManager() vslot 0x78)
    virtual std::uint32_t MapPath(std::uint32_t pathId) = 0;
};
IPreBuiltModelOccupantHost* PreBuiltModelOccupantHost();
void SetPreBuiltModelOccupantHost(IPreBuiltModelOccupantHost* host);

// ---- the occupant -------------------------------------------------------------------------------------------
class cSC4NetworkOccupantWithPreBuiltModel : public cSC4NetworkOccupant, public IPathNetworkOccupant {
public:
    cSC4NetworkOccupantWithPreBuiltModel();                 // 0x1001b9fd6 (C1), 0x1001baf32 (C2)
    ~cSC4NetworkOccupantWithPreBuiltModel() override;       // 0x1001d26d0 (D2), 0x1001a2a60 (D1),
                                                            // 0x1001bc78c (D0)

    // cSC4Occupant / cSC4NetworkOccupant
    bool QueryInterface(std::uint32_t iid, void** out) override;              // 0x1001bc350
    using cSC4NetworkOccupant::Init;
    std::uint32_t AddRef() override { return cSC4NetworkOccupant::AddRef(); }    // 0x1001bc41c
    std::uint32_t Release() override { return cSC4NetworkOccupant::Release(); }  // 0x1001bc46e
    bool Init() override;                                                     // 0x1001ba0e6 (vslot 0x260)
    bool Shutdown() override;                                                 // 0x1001ba29e (vslot 0x20)
    bool Write(cIGZOStream& stream) const override;                           // 0x1001bc4c0 (vslot 0x268)
    bool Read(cIGZIStream& stream) override;                                  // 0x1001bc5e4 (vslot 0x270)
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NetworkOccupantWithPreBuiltModel; }
    bool IsManagedGlobally() const { return true; }                           // 0x1001a2964 (vslot 0x28)

    // the render / model slots of the original (see the vtable order in the .cpp)
    virtual bool HasViewDependentModel() const { return true; }               // 0x1001bbf56 (vslot 0x100)
    virtual void GetModelInstances(int zoom, int rotation, s3d::cIS3DModelInstance** out, int capacity,
                                   int& count);                               // 0x1001bb032 (vslot 0x108)
    float GetAltitude(float x, float z) const override;                       // 0x1001bc0a4 (vslot 0x1c0)
    virtual void GetNormal(float x, float z, float out[3]) const;             // 0x1001bc178 (vslot 0x1c8)
    virtual void SetTranslucency(bool on, std::uint8_t alpha) override;       // 0x1001bbdfc (vslot 0x1d0)
    virtual void SetLit(bool lit);                                            // 0x1001bc312 (vslot 0x1e8)
    void TransformNode(cS3DVector3& point) override;                          // 0x1001bbf5e (vslot 0x208)
    virtual bool IsSupportPylonPresent() const;                               // 0x1001bc338 (vslot 0x248)
    virtual void Draw(SC4DrawContext* context, bool highlight);               // 0x1001ba316 (vslot 0x2d8)
    virtual void UpdateLighting(void* lightingManager);                       // 0x1001bc2f0 (vslot 0x310)
    virtual void ComputeBoundingBox(float* minXyz, float* maxXyz);            // 0x1001ba9d0 (vslot 0x318)
    void SetVisibility(bool visible, bool postNow) override;                  // 0x1001bc25e (vslot 0x320)
    void SetAllFlags(std::uint32_t flags) override;                           // 0x1001bc2ae (vslot 0x328)
    virtual void UpdateBaseModel(s3d::cIS3DModelInstance* model) { (void)model; }   // 0x1001a2a8c (vslot 0x330)

    // model building
    void AddModel(cISCResExemplar* exemplar, const float* position, std::uint8_t rotation);   // 0x1001ba886
    void AddSupportModel(std::uint32_t modelId, float rotation, const cS3DVector3& offset);   // 0x1001bb004
    void RecomputeShearTransform();                                          // 0x1001ba162
    void UpdateShadow(bool force);                                           // 0x1001ba37a

    // IPathNetworkOccupant (the original passes `this` to the path info)
    using cSC4Occupant::GetBoundingCityCells;
    using cSC4NetworkOccupant::GetPosition;
    void GetBoundingCityCells(SC4RectInt32& rect) const override;
    std::uint32_t PieceId() const override { return cSC4NetworkOccupant::PieceId(); }
    std::uint8_t GetRotation() const override { return cSC4NetworkOccupant::GetRotation(); }
    bool GetFlip() const override { return cSC4NetworkOccupant::GetFlip() != 0; }
    void GetPosition(float* xyz) const override { cSC4NetworkOccupant::GetPosition(xyz); }
    std::uint8_t GetOneWayDirections() const override;
    std::uint8_t GetActiveEdgesForType(std::uint32_t networkType) const override;
    std::uint8_t GetActiveEdgesForTypeNoGroup(std::uint32_t networkType) const override;
    void AddPathConnection(std::uint32_t pathType, std::uint8_t entry, std::uint8_t exit) override {
        cSC4NetworkOccupant::AddPathConnection(pathType, entry, exit);
    }
    void GetOccupiedCell(std::uint32_t& x, std::uint32_t& z) const override { cSC4NetworkOccupant::GetOccupiedCell(x, z); }
    cISC4PathInfo* GetPathInfo() override;                                   // 0x1001bbbca (vslot 0x188)
    const cSC4PathInfo* GetPathInfo() const override;

    // port wiring
    void SetModelInstance(s3d::cIS3DModelInstance* instance) { modelInstance_ = instance; }   // the +0x68 base
    s3d::cIS3DModelInstance* ModelInstance() const { return modelInstance_; }
    s3d::cIS3DModelInstance* PreBuiltModel() const { return model_; }                // +0x220
    s3d::cIS3DModelInstance* SupportModel() const { return supportModel_; }          // +0x228
    void SetPreBuiltModel(s3d::cIS3DModelInstance* model);
    void SetSupportModel(s3d::cIS3DModelInstance* model);
    const cS3DVector3& ModelOffset() const { return modelOffset_; }                  // +0x1d0
    void SetModelOffset(const cS3DVector3& offset) { modelOffset_ = offset; }
    const cS3DVector3& FillA() const { return fillA_; }                              // +0x1dc
    const cS3DVector3& FillB() const { return fillB_; }                              // +0x1e8
    std::uint32_t SupportModelId() const { return supportModelId_; }                 // +0x1f4
    float SupportRotation() const { return supportRotation_; }                       // +0x1f8
    const cS3DVector3& SupportOffset() const { return supportOffset_; }              // +0x1fc
    std::uint32_t ShadowPieceId() const { return shadowPieceId_; }                   // +0x208
    std::uint8_t ShadowRotation() const { return shadowRotation_; }                  // +0x20c
    std::int32_t ShadowLand() const { return shadowLand_; }                         // +0x210
    std::int32_t ShadowWater() const { return shadowWater_; }                       // +0x214
    std::uint8_t LastZoom() const { return lastZoom_; }                              // +0x218
    std::uint8_t LastShadowView() const { return lastShadowView_; }                  // +0x219
    void SetShadowOverride(std::uint32_t pieceId, std::uint8_t rotation) {
        shadowPieceId_ = pieceId;
        shadowRotation_ = rotation;
    }
    cISC4PathInfo* CachedPathInfo() const { return pathInfo_; }
    void ClearCachedPathInfo();

protected:
    void ReleasePathInfo() override;   // 0x10019f722: also drops the cached path info pointer

private:
    cS3DVector3 modelOffset_;                 // +0x1d0 (the original leaves it uninitialised)
    cS3DVector3 fillA_;                       // +0x1dc
    cS3DVector3 fillB_;                       // +0x1e8
    std::uint32_t supportModelId_ = 0;        // +0x1f4
    float supportRotation_ = 0.0f;            // +0x1f8
    cS3DVector3 supportOffset_;               // +0x1fc
    std::uint32_t shadowPieceId_ = 0;         // +0x208
    std::uint8_t shadowRotation_ = 0;         // +0x20c
    std::int32_t shadowLand_ = -1;            // +0x210
    std::int32_t shadowWater_ = -1;           // +0x214
    std::uint8_t lastZoom_ = 0xff;            // +0x218
    std::uint8_t lastShadowView_ = 0xff;      // +0x219
    s3d::cIS3DModelInstance* model_ = nullptr;         // +0x220
    s3d::cIS3DModelInstance* supportModel_ = nullptr;  // +0x228
    cISC4PathInfo* pathInfo_ = nullptr;       // +0x190 (also the base's persist ref)
    s3d::cIS3DModelInstance* modelInstance_ = nullptr;   // the +0x68 cS3DModelInstanceBasic (not owned)
};

}  // namespace sc4
