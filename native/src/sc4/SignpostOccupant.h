// cSC4SignpostOccupant — the region signpost occupant: the billboard sign the region view's sign tool creates
// and edits (text signs placed on the region terrain). 31 function symbols at 0x10070e6ae..0x1007110ca (the two
// record operators at 0x10070e69e / 0x10070e912 are ported in sc4/ClassAllocators.*, the GZCOM factory at
// 0x10070e660 in gz/GZCOM.*). Class id 0xab72fbb3 (which is also the prop type the sign stores for itself),
// interface 0x4b44fbe2 (cISC4SignpostOccupant, see the sign tool), record version 2 (version 1 carried an extra
// u32 after the text).
//
// It is a cSC4PropOccupant (prop at +8 of the original object) that also is a cS3DModelInstanceBasic (at +0xa8),
// a cIGZMessageTarget2 (at +0x10) and the cISC4SignpostOccupant interface (at +0). Original layout:
//   +0xe0 cRZString text                  the sign's text, one glyph quad per character
//   +0x108 u8 edit mode                   the sign tool's text-editing state
//   +0x109 u8 cursor visible              blinks every 500 ms while editing (the tick message 0x86ad10ee)
//   +0x10c u32 tick accumulator (ms)
//   +0x110 cS3DTransform                  the billboard transform; RebuildModel composes the camera-facing yaw into it
//   +0x148/+0x154 bounds (min, max)       the sign's own rectangle, local space
//   +0x160/+0x16c world bounds            the same rectangle after the transform
//   +0x178/+0x190 two pick boxes (min, max)
//   +0x1a8 sVF_V3F_T2F[4]                 the board textured quad (built but never drawn — see the quirk below)
//   +0x1f8 sVF_V3F_T2F[10][4]             the ten textured frame / post pieces
//   +0x518 std::vector<sVF_V3F_T2F>       the glyph quads (four per character, rebuilt from the glyph list)
//   +0x530 sVF_V3F_C4B[4]                 the board's background quad (vertex colours)
//   +0x570 sVF_V3F_C4B[4]                 the edit cursor quad (colour 0xff6a554e)
//   +0x5b8 cS3DTextureBinding*[10]        the frame / post textures (resource instances 0x8b4a6560..67)
//   +0x608 vector<cRZAutoRefCount<cISC4GlyphTextureInfo>>   one glyph texture per character
//   +0x620 cSC4PackedRenderState          the sign's render state
// The object is 0x628 bytes (the sign tool allocates that size).
//
// Quirks kept:
//  * The board's textured quad (+0x1a8) is built by RebuildModel but Draw never submits it (Draw starts at +0x1f8).
//  * The constructor leaves the edit state's cursor flag, the tick accumulator and the glyph texture list's
//    spare word uninitialised; the port zeroes them.
//  * SetText only rebuilds when the text actually changed, except for an empty string, which always rebuilds.
//  * LoadTextures clears the glyph list twice when the text is empty.
//  * Pick takes the first box whose hit distance is within the context's running maximum and returns immediately,
//    so a nearer hit in the second box is not looked for (the boxes are tested in the order +0x178 then +0x190).
//  * SetDynamic only stores the new vertex colours when the model's properties bit 0 changes; leaving edit mode
//    calls SetEditMode(false) through the sign interface.
//  * RebuildModel discards the rotation it composed last time when the transform's rotation flag (2) is already
//    set, and otherwise composes the camera yaw on top of the rotation the caller stored: the flag means
//    "rotation was built here", not "rotation is valid".
//  * GetOwner answers the occupant (this + 0x18 in the original), and SetOwner does nothing at all.
//  * Read skips an extra u32 for version 1 records and ignores the record version otherwise.
//  * Init stores the class id 0xab72fbb3 as the prop type (the prop type is not a kOccupantTypeProp value).
//
// PARTIAL: RebuildModel's second pick box (+0x190) and the last leg of its vectorised bounds block
// (0x10070fde1..0x10070fed2) could not be decoded bit-for-bit (the compiler packed the corner coordinates across
// vector lanes); the port computes both pick boxes as the axis-aligned boxes of the rotated sign / post
// rectangles. Everything else in RebuildModel (sizes, all 11 static quads with their texture coordinates, the
// glyph quads, the camera-facing yaw composition and the local / world bounds) is decoded from the disassembly.
#pragma once
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "sc4/PackedRenderState.h"
#include "sc4/PropOccupant.h"
#include "s3d/ModelInstance.h"

class SC4DrawContext;

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4SignpostOccupant = 0xab72fbb3;
constexpr std::uint32_t kGZIID_cISC4SignpostOccupant = 0x4b44fbe2;
constexpr std::uint32_t kSignpostPropType = 0xab72fbb3;          // Init stores the class id as the prop type
constexpr std::uint32_t kSignpostFontId = 0x2c1fd612;            // SC4GlyphAndFontInfo +0x28 in LoadTextures
constexpr std::uint32_t kSignpostFlagShown = 2;                  // "is in the 3D view / the prop developer allows it"
constexpr std::uint32_t kSignpostFlagInitialized = 0x10;         // SetFlag(0x10) in Init
constexpr std::uint32_t kSignpostMessageRefresh = 0xa6b79602;    // RebuildModel (also the text decal's refresh)
constexpr std::uint32_t kSignpostMessageVisibility = 0x06cdb65b; // data2 -> the set of occupants in the 3D view
constexpr std::uint32_t kSignpostMessageTick = 0x86ad10ee;       // the animation tick manager's tick (data2 = ms)
constexpr int kSignpostTickChannel = 0x80;                       // SL::ATM() notification channel while editing

// ---- vertices -----------------------------------------------------------------------------------------------
// Draw submits two interleaved formats: 1 = sVF_V3F_C4B (0x10 bytes) for the coloured quads and 2 =
// sVF_V3F_T2F (0x14 bytes) for the textured ones.
struct SignVertexC4B {
    float x = 0, y = 0, z = 0;                  // +0
    std::uint32_t color = 0xffffffffu;          // +0xc 0xAARRGGBB
};
struct SignVertexT2F {
    float x = 0, y = 0, z = 0;                  // +0
    float u = 0, v = 0;                         // +0xc
};
static_assert(sizeof(SignVertexC4B) == 0x10, "sVF_V3F_C4B stride");
static_assert(sizeof(SignVertexT2F) == 0x14, "sVF_V3F_T2F stride");

// ---- the sign interface (the sign tool's cISC4SignpostOccupant) ----------------------------------------------
// Declared here because the signpost is its only implementation; the sign tool header includes this one.
class cISC4SignpostOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                  // 0x18
    virtual void SetText(const cIGZString& text) = 0;         // 0x20
    virtual void GetText(cIGZString& text) = 0;               // 0x28
    virtual void SetDynamic(bool dynamic) = 0;                // 0x30
    virtual void SetEditMode(bool editing) = 0;               // 0x38
};

// ---- seams --------------------------------------------------------------------------------------------------
// cS3DTextureBinding as the sign reads it: vslot 0x10 is Release and the first word of the object is the
// texture handle (the same shape sc4/CatalogItem.h and sc4/ParticlesEffect.h use).
class ISignpostTextureBinding {
public:
    virtual ~ISignpostTextureBinding() = default;
    virtual std::uint32_t TextureId() const = 0;   // the binding's first word
    virtual void Release() = 0;                    // 0x1000d9bfa
};

// cISC4GlyphTextureInfo as the sign reads it (vtable 0x100ae8718, slots after cIGZUnknown).
class ISignpostGlyphTextureInfo : public cIGZUnknown {
public:
    virtual void GetGlyph(cIGZString& out) = 0;                  // 0x18
    virtual void GetTextureSize(int* widthHeight) = 0;           // 0x20
    virtual void GetGlyphSize(int* widthHeight) = 0;             // 0x28
    virtual void GetGlyphTextureCoordinates(float* rect) = 0;    // 0x30 left, top, right, bottom
    virtual ISignpostTextureBinding* GetTextureBinding() = 0;    // 0x38
};

// The set of occupants currently in the 3D view (View3D vslot 0x90 result; the object sc4/TextDecalOccupant.h
// calls ITextDecalViewSet).
class ISignpostViewSet : public cIGZUnknown {
public:
    virtual bool ContainsOccupant(cISC4Occupant* occupant) = 0;   // [3] 0x18
};

// The services behind the SL::sp* globals the original reaches.
class ISignpostHost {
public:
    virtual ~ISignpostHost() = default;
    virtual cIGZMessageServer2* Messages() = 0;                                     // SL::spMS2
    // SL::ATM() (the animation tick manager) vslot 0x28 / 0x30.
    virtual void AddTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    virtual void RemoveTickNotification(cIGZMessageTarget2* target, int channel) = 0;
    virtual bool PropDeveloperAllowsSigns() = 0;                                    // SL::spPropDeveloper vslot 0x50
    virtual cISC4OccupantManager* OccupantManager() = 0;                            // SL::spOccupantManager
    // SL::spRenderer (0x100b214d0): 0xd8 IsModelInserted, 0xe0 InsertModel, 0xe8 RemoveModel, 0xf0 MoveModel,
    // 0xf8 ChangeModel(instance, reason).
    virtual bool RendererHasModel(s3d::cIS3DModelInstance* model) = 0;
    virtual void RendererInsertModel(s3d::cIS3DModelInstance* model) = 0;
    virtual void RendererRemoveModel(s3d::cIS3DModelInstance* model) = 0;
    virtual void RendererMoveModel(s3d::cIS3DModelInstance* model) = 0;
    virtual void RendererChangeModel(s3d::cIS3DModelInstance* model, int reason) = 0;
    // SL::spGlyphTextureManager vslot 0x20: cSC4GlyphTextureManager::StringToGlyphTextureArray(font info, text,
    // out) @ 0x10070d372. The original passes a temporary {empty cRZString, font id}; only the font id is used.
    virtual void StringToGlyphTextureArray(std::uint32_t fontId, cIGZString& text,
                                           std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>>& out) = 0;
    // SL::spTextureBindingFactory (cIS3DTextureBindingFactory, service 0x1ac0e11a / iid 0xfac0e219) vslot 0x20:
    // CreateTexture({type 0x7ab50e44, group 0x1abe787d, instance}) -> one reference, held by the caller.
    virtual ISignpostTextureBinding* CreateTextureBinding(std::uint32_t instance) = 0;
    // SC4ViewUtilities (SL::spViewUtilities, 0x100b214d8): +0x14 the zoom index, +0x5c / +0x64 the view
    // direction's x / z and ViewportUnitsToWorldUnits @ 0x10017374c.
    virtual int ViewZoom() = 0;
    virtual void CameraViewDirection(float& x, float& z) = 0;
    virtual float ViewportUnitsToWorldUnits(float units) = 0;
};
ISignpostHost* SignpostHost();
void SetSignpostHost(ISignpostHost* host);

// kScaleByZoom @ 0x100a29430: the sign's size table, indexed by the view's zoom level (0..4).
extern const float kSignpostScaleByZoom[5];
// The board's vertex colours @ 0x100a2941c, indexed by the model's properties bit 0 (dynamic): 0xAARRGGBB.
extern const std::uint32_t kSignpostBoardColors[2];

// ---- the occupant -------------------------------------------------------------------------------------------
class cSC4SignpostOccupant : public cSC4PropOccupant,
                             public s3d::cS3DModelInstanceBasic,
                             public cISC4SignpostOccupant {   // the cIGZMessageTarget2 (+0x10) is the prop base's
public:
    cSC4SignpostOccupant();                        // @ 0x10070e6ae
    ~cSC4SignpostOccupant() override;              // @ 0x1007110ca (+ 0x100710d00, 0x100710d12)

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1007100a8
    std::uint32_t AddRef() override { return cSC4PropOccupant::AddRef(); }     // @ 0x10070e952
    std::uint32_t Release() override { return cSC4PropOccupant::Release(); }   // @ 0x10070e9c8

    // cISC4Occupant / cISC4PropOccupant
    bool Init() override;                          // @ 0x10070ea3e
    bool Shutdown() override;                      // @ 0x10070ff94
    void SetAllFlags(std::uint32_t flags) override;   // @ 0x10071027e
    void SetPosition(const float* xyz) override;   // @ 0x10071081c
    bool Write(cIGZOStream& stream) const override;   // @ 0x100710a0c
    bool Read(cIGZIStream& stream) override;       // @ 0x100710ae4

    // cIGZMessageTarget2
    bool DoMessage(cIGZMessage2* message) override;   // @ 0x100710136

    // cIS3DModelInstance (the model base at +0xa8 of the original)
    cIGZUnknown* GetOwner() override;              // @ 0x100710c36
    void SetOwner(cIGZUnknown* owner) override;    // @ 0x100710c40
    void GetBounds(cS3DBoundingBox& out) override;        // @ 0x100710c46
    void GetWorldBounds(cS3DBoundingBox& out) override;   // @ 0x100710c82
    void GetTransform(cS3DTransform& out) override;       // @ 0x100710cbe
    void SetTransform(const cS3DTransform& transform) override;   // @ 0x100710cd6
    void GetSortValue(std::uint32_t& out) const override; // @ 0x100710ce8
    bool Pick(cSC4PickContext* context) override;         // @ 0x100710712
    int Draw(SC4DrawContext* context, bool highlight) override;   // @ 0x100710510

    // cIGZSerializable
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4SignpostOccupant; }   // @ 0x100710cf4

    // cISC4SignpostOccupant
    cISC4Occupant* AsOccupant() override;          // @ 0x100710c10
    void SetText(const cIGZString& text) override; // @ 0x10071034c
    void GetText(cIGZString& text) override;       // @ 0x100710c1a
    void SetDynamic(bool dynamic) override;        // @ 0x1007103c0
    void SetEditMode(bool editing) override;       // @ 0x100710978

    // LoadTextures @ 0x10070ebf4 and RebuildModel @ 0x10070ed0c (public: SetText and SetEditMode drive them).
    void LoadTextures();
    void RebuildModel();

    // The recovered state, for tests and for the sign tool.
    const cRZString& Text() const { return text_; }
    bool EditMode() const { return editMode_; }
    bool CursorVisible() const { return cursorVisible_; }
    const cS3DTransform& Transform() const { return transform_; }
    const cSC4PackedRenderState& RenderState() const { return renderState_; }
    const std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>>& GlyphTextures() const { return glyphTextures_; }
    const std::vector<SignVertexT2F>& GlyphQuads() const { return glyphQuads_; }
    const SignVertexT2F* PieceQuads() const { return &pieceQuads_[0][0]; }   // [piece][vertex], 4 vertices each
    const SignVertexT2F* TextQuad() const { return textQuad_; }
    const SignVertexC4B* BackgroundQuad() const { return backgroundQuad_; }
    const SignVertexC4B* CursorQuad() const { return cursorQuad_; }
    ISignpostTextureBinding* Texture(int index) const { return textures_[index]; }
    const float* PickBox(int index) const { return pickBoxes_[index]; }   // min xyz, max xyz

protected:
    // cSC4PropOccupant::DoMessage (0x10015ebb2, owned by the prop class) is not ported; the original tail-calls it
    // from the sign's DoMessage. Defaults to "not handled", like the prop's own body.
    virtual bool DoPropMessage(cIGZMessage2*) { return true; }

private:
    void SetBoardColors(std::uint32_t color);   // the four background vertices (RebuildModel and SetDynamic)
    void WorldBoxesFromTransform(const float* localMin, const float* localMax, float* outMin, float* outMax) const;

    cRZString text_;                                    // +0xe0
    bool editMode_ = false;                             // +0x108
    bool cursorVisible_ = false;                        // +0x109
    int tickMs_ = 0;                                    // +0x10c
    cS3DTransform transform_;                           // +0x110
    float localBounds_[6];                              // +0x148 min, +0x154 max
    float worldBounds_[6];                              // +0x160 min, +0x16c max
    float pickBoxes_[2][6];                             // +0x178, +0x190
    SignVertexT2F textQuad_[4];                         // +0x1a8
    SignVertexT2F pieceQuads_[10][4];                   // +0x1f8
    std::vector<SignVertexT2F> glyphQuads_;             // +0x518
    SignVertexC4B backgroundQuad_[4];                   // +0x530
    SignVertexC4B cursorQuad_[4];                       // +0x570
    ISignpostTextureBinding* textures_[10] = {};        // +0x5b8
    std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>> glyphTextures_;   // +0x608
    cSC4PackedRenderState renderState_;                 // +0x620
};

}  // namespace sc4
