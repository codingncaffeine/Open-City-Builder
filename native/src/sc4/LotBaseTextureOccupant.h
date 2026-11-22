// cSC4LotBaseTextureOccupant — the occupant that paints a lot's base texture: the coloured, per-cell ground
// texture a lot (or a landfill cell) shows under its building. 27 hand-written function bodies (plus the 30
// compiler-emitted thunks of the two-base layout) at 0x1000b36e0..0x1000b5f1d; the plain/sized operators at
// 0x1000b36d0 / 0x1000b3776 and the GZCOM factory at 0x1000b360a are ported elsewhere, and
// 0x1000b68a9..0x1000b6915 is the translation unit's static initialiser. Class id 0xc97f987c, occupant
// type 0x497f6d9d, interface cISC4LotBaseTextureOccupant iid 0x097f6d98, record version 2.
//
// Original layout (0xa0 bytes; sLotBaseTextureOccupantAllocator = 0xa0, set at 0x1000b689b):
//   +0x00 cISC4LotBaseTextureOccupant vptr
//   +0x08 cSC4Occupant (its occupant type sits at +0x50, i.e. the base's +0x48)
//   +0x68 float3 bounds min            (x, y, z; the texture quad's corners)
//   +0x74 float3 bounds max
//   +0x80 s8 usage                    (GetUsage / SetUsage; the landfill's empty-cell texture leaves it 0)
//   +0x88 std::vector<LotBaseTextureSpecification> specs (0x88 begin, 0x90 end, 0x98 capacity)
// The primary vtable is 0x100a6e6d0: +0x18 AsOccupant, +0x20 GetUsage, +0x28 SetUsage, +0x30 GetCellCoverage,
// +0x38 IsCellCovered, +0x40 SetTextureSpecification, +0x48 UpdateVertexAltitudes, +0x50 LoadModels,
// +0x58 UnloadModels, the two dtor slots at +0x60/+0x68, then +0x70 Init, +0x78 Shutdown, +0x80 Write, +0x88
// Read, +0x90 GetGZCLSID, and the overrides of the non-primary cISC4Occupant base at +0x98 GetPosition, +0xa0
// SetPosition, +0xa8 GetBoundingBox, +0xb0 SetAllFlags. The secondary vtables are cSC4Occupant at +0x08,
// cIGZSerializable at +0x10, cSCExemplarPropertyHolder at +0x18 and cISCExemplarPropertyHolder at +0x40.
//
// A spec (0x10 bytes, only the first 0x0e serialised) is decoded from the only producer in the binary,
// cSC4LandfillZone::SetEmptyCellTexture @ 0x1000af28e (stack fields set at 0x1000af2ae..0x1000af2d6) and from the
// model that consumes it, cSC4CellTextureModel (0x1000b5146..0x1000b5eec):
//   +0x00 u32 texture id (the landfill passes 0x1005)
//   +0x04 u8 cell x            +0x05 u8 cell z
//   +0x06 u8 rotation 0..3     +0x07 u8 flip the U coordinate   (model Init copies them to +0x46/+0x47)
//   +0x08..+0x0b u8 RGBA       the quad's vertex colour; alpha 0xff is what GetCellCoverage counts as covered
//                              (the landfill passes 0xffffff and the zone overlay's display alpha)
//   +0x0c u8 texture combine mode (model +0x4c; the landfill passes 1)
//   +0x0d u8 animation frame; 0xff = not chosen yet, SetTextureSpecification randomises it from the binding's
//                              frame count and Draw indexes the binding's frame table with it
//   +0x0e..+0x0f never written, never serialised (struct tail padding)
//
// The unrecovered neighbours are seams: cSC4CellTextureModel (0x100 bytes) through ILotCellTextureModel, and the
// engine globals SL::spLotManager / spTerrain / spRenderer / spView3D / spOccupantManager plus the RZ texture-binding
// factory through ILotBaseTextureHost (one method per original call, slot/address in the comment).
//
// Quirks kept:
//  * SetPosition does nothing (a 6-byte body: `ret`).
//  * UpdateExistingModels and SetAllFlags both call cSC4Occupant::AsPropertyHolder() and throw the result away.
//  * CalculateBoundingBox's min/max cell scan uses signed max against 0x80000000 and unsigned min against 0x7fffffff;
//    for the 0..255 cell bytes this is a plain min/max.
//  * With no locked lot the Y range is sampled at the four terrain vertices of each spec's cell; a flat cell gets
//    min + 0.1 as its top. With a locked lot both Y values come from the lot's altitude (top = +0.1) and the terrain
//    is not sampled. An empty spec list yields the inverted placeholder box (-15.9, FLT_MAX, -15.9)..(15.9, -2^31, 15.9)
//    (the original reads spec[0] before it checks for an empty list and would fault on a never-populated occupant;
//    the port guards that read, and bails out entirely when no host is installed).
//  * SetTextureSpecification returns before touching the bounding box when `count` is 0.
//  * Specs whose frame byte is not 0xff are left alone; the frame count is cached per texture id for the whole
//    process (anonymous-namespace sTextureFrameCountCache), a missing binding counts as one frame, and frames < 2
//    always use frame 0 without drawing from the generator.
//  * Existing specs are only updated in place when *all* of the new specs match an unused old one by cell; otherwise
//    the models are unloaded, the list replaced and the models reloaded - but only when the 3D view set exists
//    (without it LoadModels is never called).
//  * UpdateExistingModels walks the placeable's models and indexes the spec list with the model index; the original
//    has no bounds check there (models and specs are created one-to-one), the port stops at the end of the list.
//  * Write/Read report success from the stream error alone: a nested occupant record with a wrong version makes
//    Read skip its fields yet still report success (and Write skips them too after a failed base write).
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gz/StreamOps.h"
#include "rz/cRZRandom.h"
#include "sc4/Occupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4LotBaseTextureOccupant = 0xc97f987c;
constexpr std::uint32_t kGZIID_cISC4LotBaseTextureOccupant = 0x097f6d98;
constexpr std::uint32_t kGZIID_cISC4CellTextureModel = 0xa9e2831d;
constexpr std::uint32_t kOccupantTypeLotBaseTexture = 0x497f6d9d;
constexpr std::uint32_t kLotBaseTextureRecordVersion = 2;

// Occupant flag bits this class reacts to (cSC4Occupant's +0x53 byte).
constexpr std::uint32_t kLotBaseTextureFlagManaged = 1;   // in the occupant manager (MoveOccupant requires it)
constexpr std::uint32_t kLotBaseTextureFlagShown = 2;     // should be in the 3D view / the renderer

// The cGZPersistResourceKey the texture binding is looked up with, first with instance = id + 4 and then id
// (0x1000b4874 / 0x1000b489c: the 8-byte constant 0x0986135e7ab50e44).
constexpr std::uint32_t kLotBaseTextureBindingType = 0x7ab50e44;
constexpr std::uint32_t kLotBaseTextureBindingGroup = 0x0986135e;

// SC4Point<int>: two int32. (cSC4ResponseCurve's global SC4Point is the float 2D point, hence the suffix.)
struct SC4PointInt {
    std::int32_t x = 0;
    std::int32_t z = 0;
};

// cISC4LotBaseTextureOccupant::LotBaseTextureSpecification (0x10 bytes). Field meaning as in the header comment.
struct LotBaseTextureSpecification {
    std::uint32_t textureId = 0;      // +0
    std::uint8_t cellX = 0;           // +4
    std::uint8_t cellZ = 0;           // +5
    std::uint8_t rotation = 0;        // +6
    std::uint8_t flip = 0;            // +7
    std::uint8_t red = 0;             // +8
    std::uint8_t green = 0;           // +9
    std::uint8_t blue = 0;            // +10
    std::uint8_t alpha = 0;           // +11
    std::uint8_t combineMode = 0;     // +12
    std::uint8_t frame = 0xff;        // +13
    // +14..+15: padding in the original, never serialised or copied field by field.
};
static_assert(sizeof(LotBaseTextureSpecification) == 0x10, "LotBaseTextureSpecification stride");
static_assert(offsetof(LotBaseTextureSpecification, alpha) == 0x0b, "spec alpha offset");
static_assert(offsetof(LotBaseTextureSpecification, frame) == 0x0d, "spec frame offset");

// cISC4LotBaseTextureOccupant (outer vtable 0x100a6e6d0, iid 0x097f6d98): slots after cIGZUnknown.
class cISC4LotBaseTextureOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                                                     // [3]  0x18
    virtual int GetUsage() const = 0;                                                            // [4]  0x20
    virtual void SetUsage(int usage) = 0;                                                        // [5]  0x28
    // the covered cells (cell x, cell z) and, optionally, one flag per cell (alpha == 0xff); returns the spec count
    virtual std::size_t GetCellCoverage(std::vector<SC4PointInt>& cells, std::vector<bool>* covered) const = 0;   // [6] 0x30
    virtual bool IsCellCovered(int x, int z, bool* covered) const = 0;                            // [7]  0x38
    virtual void SetTextureSpecification(const LotBaseTextureSpecification* specs, std::uint32_t count,
                                         bool* changed) = 0;                                     // [8]  0x40
    virtual void UpdateVertexAltitudes(bool allowUpdate, bool force, bool* changed) = 0;          // [9]  0x48
    virtual void LoadModels(bool setVisibility) = 0;                                             // [10] 0x50
    virtual void UnloadModels() = 0;                                                             // [11] 0x58
};

// cSC4CellTextureModel is not recovered yet. This is cISC4CellTextureModel (iid 0xa9e2831d, the interface the
// occupant gets from each placeable model by QueryInterface) as this class calls it, plus the port-only model
// instance accessor. The original creates the model with malloc(sCellTextureModelAllocator = 0x100) @ 0x1000b694c,
// cSC4CellTextureModel::cSC4CellTextureModel @ 0x1000b5146, AddRef, then Init(spec) @ 0x1000b5252.
class ILotCellTextureModel : public cIGZUnknown {
public:
    virtual void UpdateTextures(int zoom, const LotBaseTextureSpecification& spec) = 0;   // [3] 0x18 @ 0x1000b5968
    // bool,bool,cISTETerrain*,cISC4LightingManager*; this class always passes (true, true, null, null)
    virtual void SetupVertices(bool updateVertices, bool updateLighting, void* terrain, void* lighting) = 0;  // [4] 0x20
    // The cIS3DModelInstance subobject: the original adds the vtable's SI adjustor (*(int*)(*(void**)model - 0x18))
    // to the object pointer. Port-only.
    virtual cIS3DModelInstance* ModelInstance() = 0;
};

// The set of occupants currently in the 3D view (View3D vslot 0x90 result; the object sc4/TextDecalOccupant.h calls
// ITextDecalViewSet and sc4/SignpostOccupant.h calls ISignpostViewSet).
class ILotBaseTextureViewSet : public cIGZUnknown {
public:
    virtual bool ContainsOccupant(cISC4Occupant* occupant) = 0;   // [3] 0x18
};

// cS3DTextureBinding as the frame picker reads it: the frame count at +8 and the binding's Release.
class ILotBaseTextureBinding {
public:
    virtual ~ILotBaseTextureBinding() = default;
    virtual std::uint32_t FrameCount() const = 0;   // +0x08
    virtual void Release() = 0;                     // cS3DTextureBinding::Release @ 0x1000d9bfa
};

class cSC4Lot;   // sc4/Lot.h (forward-declared to keep this header light; the .cpp includes it)

// The engine services of the original. Every method mirrors exactly one original call; the slot / address of
// that call is in the comment.
class ILotBaseTextureHost {
public:
    virtual ~ILotBaseTextureHost() = default;
    // cISC4LotManager vslot 0x28 (x, z, 0): the lot covering a standard city cell; not owned
    virtual cSC4Lot* LotAtCell(int x, int z) = 0;
    // cSTETerrain vslot 0x70: GetAltitudeAtVertex(x, z)
    virtual float TerrainAltitudeAtVertex(int x, int z) = 0;
    // RZGetFramework()->QueryInterface(0x1ac0e11a, 0xfac0e219) (cIS3DTextureBindingFactory) vslot 0x20:
    // CreateTexture({type 0x7ab50e44, group 0x0986135e, instance}), one reference or null.
    virtual ILotBaseTextureBinding* CreateTextureBinding(std::uint32_t instance) = 0;
    // SL::spRenderer (0x100b214d0) vslot 0x50 -> SC4ViewUtilities; +0x14 is the zoom index
    virtual int ViewZoom() = 0;
    // SL::spRenderer vslot 0xe8 / 0xf0 / 0xf8: remove / move / change (reason 2) a model instance
    virtual void RendererRemoveModel(cIS3DModelInstance* model) = 0;
    virtual void RendererMoveModel(cIS3DModelInstance* model) = 0;
    virtual void RendererChangeModel(cIS3DModelInstance* model, int reason) = 0;
    // SL::spView3D vslot 0x90: the set of occupants in the 3D view, one reference or null
    virtual ILotBaseTextureViewSet* AcquireViewSet() = 0;
    // cSC4OccupantManager vslot 0xc0: MoveOccupant(occupant, false)
    virtual void MoveOccupant(cISC4Occupant* occupant) = 0;
    // cSC4CellTextureModel creation, see ILotCellTextureModel: one reference (or null)
    virtual ILotCellTextureModel* CreateCellTextureModel(const LotBaseTextureSpecification& spec) = 0;
};
ILotBaseTextureHost* LotBaseTextureHost();
void SetLotBaseTextureHost(ILotBaseTextureHost* host);

class cSC4LotBaseTextureOccupant final : public cISC4LotBaseTextureOccupant, public cSC4Occupant {
public:
    cSC4LotBaseTextureOccupant();        // @ 0x1000b36e0
    ~cSC4LotBaseTextureOccupant() override;   // @ 0x1000b37ac (+0x1000b382c, 0x1000b3852)

    bool QueryInterface(std::uint32_t iid, void** out) override;   // @ 0x1000b3cba
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }   // @ 0x1000b3d74
    std::uint32_t Release() override { return cSC4Occupant::Release(); }  // @ 0x1000b3dba

    // cISC4LotBaseTextureOccupant
    cISC4Occupant* AsOccupant() override;   // @ 0x1000b5ec6
    const std::vector<LotBaseTextureSpecification>& Specifications() const { return specs_; }   // the port's read-back
    int GetUsage() const override;          // @ 0x1000b5ed0
    void SetUsage(int usage) override;      // @ 0x1000b5ede
    std::size_t GetCellCoverage(std::vector<SC4PointInt>& cells, std::vector<bool>* covered) const override;  // 0x1000b401a
    bool IsCellCovered(int x, int z, bool* covered) const override;  // @ 0x1000b410e
    void SetTextureSpecification(const LotBaseTextureSpecification* specs, std::uint32_t count,
                                 bool* changed) override;            // @ 0x1000b4594
    void UpdateVertexAltitudes(bool allowUpdate, bool force, bool* changed) override;   // @ 0x1000b4f40
    void LoadModels(bool setVisibility) override;   // @ 0x1000b4fb8
    void UnloadModels() override;                   // @ 0x1000b52a8

    // cSC4Occupant
    bool Init() override { return cSC4Occupant::Init(); }   // @ 0x1000b38ac
    bool Shutdown() override;                              // @ 0x1000b38c4
    void GetPosition(float* xyz) override;                 // @ 0x1000b3e66
    void SetPosition(const float*) override {}             // @ 0x1000b3ef2 (empty in the original)
    void GetBoundingBox(float* minXyz, float* maxXyz) override;   // @ 0x1000b3efe
    void SetAllFlags(std::uint32_t flags) override;         // @ 0x1000b3f4e
    bool Write(cIGZOStream& stream) const override;         // @ 0x1000b3926
    bool Read(cIGZIStream& stream) override;                // @ 0x1000b3acc
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4LotBaseTextureOccupant; }   // @ 0x1000b5eec

    // cSC4LotBaseTextureOccupant::mRandom, seeded from the clock at start-up (0x1000b68a9..0x1000b6915).
    // Tests seed it to stay deterministic.
    static cRZRandom& Random();

private:
    void CalculateBoundingBox(bool* changed);   // @ 0x1000b4188
    void UpdateExistingModels(bool changed);    // @ 0x1000b4bd4

    cS3DVector3 boundsMin_;                              // original +0x68
    cS3DVector3 boundsMax_;                              // original +0x74
    std::int8_t usage_ = 0;                              // original +0x80
    std::vector<LotBaseTextureSpecification> specs_;     // original +0x88
    static cRZRandom mRandom;
};

}  // namespace sc4
