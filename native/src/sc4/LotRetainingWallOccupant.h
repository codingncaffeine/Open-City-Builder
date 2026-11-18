// cSC4LotRetainingWallOccupant — the lot-side retaining wall that hides a height drop at the edge of a locked
// lot that holds water (33 function symbols, 0x10015caa0..0x10015e0e6). Class id 0x49c05c9f, occupant type
// 0x895d1169, interface cISC4LotRetainingWallOccupant iid 0x295d1162. Version 2 record: occupant data, u32
// retaining-wall type, s32 edge flags, s32 edge hash[4], float3 position, float3 size, then the lot reference as
// a nested cISC4Lot (sc4/DBSegment.h SC4Write/ReadGZSerializable, iid 0xe818fced).
//
// Object layout (0xa8 bytes; sLotRetainingWallOccupantAllocator = 0xa8, set at 0x10015e3fc):
//   +0x00 cISC4LotRetainingWallOccupant vptr
//   +0x08 cSC4Occupant (its occupant type sits at +0x50, i.e. the base's +0x48)
//   +0x68 u32 retaining-wall type (the "wall type" id chosen from the lot configuration)
//   +0x6c padding (never written by the ctor)
//   +0x70 cSC4Lot* lot (one reference; SetLot AddRefs/Releases)
//   +0x78 s32 edge flags: bit i = the wall model on edge i exists
//   +0x7c s32 edge hash[4]: per-edge hash used to notice that a wall has to be rebuilt
//   +0x8c float3 position (the lot centre, y = lot altitude)
//   +0x98 float3 size (x/z = lot footprint in metres, y = highest edge altitude - lot altitude)
//
// A model is the unrecovered cSC4LotRetainingWallModel (0xc8 bytes, 0x1001671ac..; +0x3c built, +0x84 edge and
// +0x88/+0x90 texture data are the parts this occupant touches): ILotRetainingWallModel stands for it. The
// managers the original reached through SL::spLotManager / spTerrain / spOccupantManager / spBuildingDevelopmentSim
// / spRenderer / spRM are behind ILotRetainingWallHost (one method per original call, slots in the comments).
//
// Quirks kept: edge hash accumulation multiplies the *rounded* vertex height by x*z and adds 1 per sampled cell
// (rounding is the original's magic-constant bit trick, see RoundAltitudeToInt in the .cpp); an edge is only
// hashed when one of its cells has no lot or an unlocked lot; AddRetainingWallOccupant's create path returns
// false and its remove path returns true; Read succeeds even when the nested occupant record has a bad version.
#pragma once
#include <cstdint>
#include <list>
#include <vector>

#include "gz/StreamOps.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/LotRetainingWallModel.h"
#include "sc4/Occupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4LotRetainingWallOccupant = 0x49c05c9f;
constexpr std::uint32_t kGZIID_cISC4LotRetainingWallOccupant = 0x295d1162;
constexpr std::uint32_t kOccupantTypeLotRetainingWall = 0x895d1169;

// lot flag bit the occupant uses to mark a lot whose retaining wall it built (cSC4Lot's +0xcd byte)
constexpr std::uint32_t kLotFlagRetainingWall = 0x04;

// properties read from the occupant's property holder (or from the base texture exemplar, see below)
constexpr std::uint32_t kPropertyRetainingWallTextures = 0x295961f2;      // u32[5] texture ids
constexpr std::uint32_t kPropertyRetainingWallTextureScale = 0x295961f3; // float

// cISC4LotRetainingWallOccupant: the lot's wall as the model maker and the building-development simulator see it.
// Slots 3..12 of the primary vtable (0x18 AsOccupant .. 0x60 SetRetainingWallType); AddRef/Release/QueryInterface
// come from cIGZUnknown.
class cISC4LotRetainingWallOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                            // [3] 0x18
    virtual bool SetSize(const float* xyz) = 0;                         // [4] 0x20
    virtual cSC4Lot* GetLot() const = 0;                                // [5] 0x28
    virtual void SetLot(cSC4Lot* lot) = 0;                              // [6] 0x30
    virtual int GetEdgeFlags() const = 0;                               // [7] 0x38
    virtual void SetEdgeFlags(int flags) = 0;                           // [8] 0x40
    virtual void GetEdgeHashArray(int* out) const = 0;                  // [9] 0x48
    virtual void SetEdgeHashArray(const int* in) = 0;                   // [10] 0x50
    virtual std::uint32_t GetRetainingWallType() const = 0;             // [11] 0x58
    virtual void SetRetainingWallType(std::uint32_t type) = 0;          // [12] 0x60
};

// The model the occupant builds and finds is the recovered cSC4LotRetainingWallModel, through its cISC4LotRetainingWallModel
// interface (iid 0xa9e2831e) with the calls the occupant makes (sc4/LotRetainingWallModel.h).
using ILotRetainingWallModel = cISC4LotRetainingWallModel;

// The engine services of the original. Every method mirrors exactly one original call; the slot / address of
// that call is in the comment.
class ILotRetainingWallHost {
public:
    virtual ~ILotRetainingWallHost() = default;
    // cSC4LotRetainingWallModel::operator new @ 0x1001673c4 (0xc8) + ctor @ 0x1001671ac; reference count 0
    virtual ILotRetainingWallModel* CreateRetainingWallModel() = 0;
    // cISC4LotManager vslot 0x28 (x, z, 0): the lot covering a standard city cell
    virtual cSC4Lot* LotAtCell(int x, int z) = 0;
    // cISC4LotManager vslot 0x60 (lot rect, edge mask, out rect): the cells tested for a locked neighbour lot
    virtual bool EdgeCellRect(const SC4CellRect& lotRect, int edgeMask, SC4CellRect& out) = 0;
    // cISC4LotManager vslot 0x58 (lot rect, edge mask, out rect): the cells whose vertex heights are hashed
    virtual void EdgeTerrainRect(const SC4CellRect& lotRect, int edgeMask, SC4CellRect& out) = 0;
    // cISC4LotManager vslot 0x40 (lot, list, 0xf): the lots touching `lot`, as owning references
    virtual bool LotsAround(cSC4Lot* lot, std::list<cRZAutoRefCount<cSC4Lot>>& out) = 0;
    // cSTETerrain vslot 0x70: GetAltitudeAtVertex(x, z)
    virtual float TerrainAltitudeAtVertex(int x, int z) = 0;
    // cSC4OccupantManager vslot 0x100: GetFirstOccupantByStandardCityCell(out, x, z, type), one reference
    virtual bool FirstOccupantAtCell(cISC4Occupant*& out, int x, int z, std::uint32_t type) = 0;
    // cSC4OccupantManager vslot 0xa0: InsertOccupant(occupant, 0)
    virtual void InsertOccupant(cISC4Occupant* occupant) = 0;
    // cSC4OccupantManager vslot 0xa8: RemoveOccupant(occupant, true, 0)
    virtual void RemoveOccupant(cISC4Occupant* occupant) = 0;
    // cSC4BuildingDevelopmentSimulator vslot 0xe8: LoadLotRetainingWallProperties(occupant, type)
    virtual bool LoadRetainingWallProperties(cISC4LotRetainingWallOccupant* occupant, std::uint32_t type) = 0;
    // SL::spRenderer != 0 (0x100b214d0); the original dereferences it without a check in the remove path
    virtual bool RendererAvailable() = 0;
    // SL::spRenderer vslot 0xd8: is the model instance already in the renderer?
    virtual bool RendererHasModel(cIS3DModelInstance* model) = 0;
    // SL::spRenderer vslot 0xe0: add the model instance to the renderer
    virtual void RendererAddModel(cIS3DModelInstance* model) = 0;
    // SL::spRenderer vslot 0xe8: remove the model instance from the renderer
    virtual void RendererRemoveModel(cIS3DModelInstance* model) = 0;
    // SL::spRM vslot 0x18: GetResource({0x6534284a, 0x88cd66e9, 1}, iid 0xa52160f5, ...); one reference
    virtual bool LoadTextureExemplar(cISCResExemplar*& out) = 0;
    // cISC4Lot::GetLotConfiguration() vslot 0x48 -> cISC4LotConfiguration::GetRetainingWallIDArray() vslot 0xe0
    virtual const std::vector<std::uint32_t>& RetainingWallIds(cSC4Lot* lot) = 0;
    // cRZRandom::RandomUint32UniformBelow(&sRandom, range) @ 0x10003d366
    virtual std::uint32_t RandomBelow(std::uint32_t range) = 0;
    // SC4WriteGZSerializable(stream, lot) @ 0x1000435e3 / SC4ReadGZSerializable(stream, 0xe818fced, &lot,
    // false) @ 0x100043267: the nested cISC4Lot reference. cSC4Lot is not a COM object in this port, so the
    // DB-segment serialization of the reference sits behind the seam. Read returns one reference (or null).
    virtual bool WriteLotReference(cIGZOStream& stream, cSC4Lot* lot) = 0;
    virtual cSC4Lot* ReadLotReference(cIGZIStream& stream) = 0;
};
ILotRetainingWallHost* LotRetainingWallHost();
void SetLotRetainingWallHost(ILotRetainingWallHost* host);

class cSC4LotRetainingWallOccupant final : public cISC4LotRetainingWallOccupant, public cSC4Occupant {
public:
    cSC4LotRetainingWallOccupant() { type_ = kOccupantTypeLotRetainingWall; }
    ~cSC4LotRetainingWallOccupant() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }
    std::uint32_t Release() override { return cSC4Occupant::Release(); }

    // cISC4LotRetainingWallOccupant
    cISC4Occupant* AsOccupant() override;
    bool SetSize(const float* xyz) override;
    cSC4Lot* GetLot() const override { return lot_; }
    void SetLot(cSC4Lot* lot) override;
    int GetEdgeFlags() const override { return edgeFlags_; }
    void SetEdgeFlags(int flags) override { edgeFlags_ = flags; }
    void GetEdgeHashArray(int* out) const override;
    void SetEdgeHashArray(const int* in) override;
    std::uint32_t GetRetainingWallType() const override { return retainingWallType_; }
    void SetRetainingWallType(std::uint32_t type) override { retainingWallType_ = type; }

    // cSC4Occupant
    bool Init() override { return cSC4Occupant::Init(); }
    bool Shutdown() override;
    void GetPosition(float* xyz) override;
    void SetPosition(const float* xyz) override;
    void GetBoundingBox(float* minXyz, float* maxXyz) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4LotRetainingWallOccupant; }

    // ---- wall management (free/static in the original)
    // 0x10015d466: create (or, with create=false, remove) the wall model for one lot edge
    static bool AddRetainingWallModel(cISC4Occupant* occupant, cISC4LotRetainingWallOccupant* wall, int edge,
                                      bool create, bool addToRenderer);
    // 0x10015d802: create the models for every edge flag (create=false removes all of the occupant's models)
    static bool AddRetainingWallModels(cISC4Occupant* occupant, bool create, bool addToRenderer);
    // 0x10015d30e: the placeable object's model built for `lotEdge`, AddRef'd on success
    static bool FindRetainingWallModel(cISC43DPlaceableObject* placeable, int lotEdge,
                                       ILotRetainingWallModel*& out);
    // 0x10015d8ca: per-edge flags/hashes for the lot's four edges and the highest edge altitude
    static bool GetLotWallEdgeFlags(cSC4Lot* lot, float altitude, const SC4CellRect& lotRect, int* edgeFlags,
                                    float& maxAltitude);
    // 0x10015db0c: a random retaining-wall type id from the lot configuration (0 when there is none)
    static std::uint32_t GetRetainingWallType(cSC4Lot* lot);
    // 0x10015db62: create (true) or remove (false) the lot's retaining-wall occupant; returns what the original
    // returned (false on the create path, true when an occupant was removed)
    static bool AddRetainingWallOccupant(cSC4Lot* lot, bool create);
    // 0x10015de56: rebuild the wall when the lot's edge hashes changed
    static void UpdateRetainingWalls(cSC4Lot* lot);
    // 0x10015e004: UpdateRetainingWalls for every lot around the given one
    static void UpdateSurroundingRetainingWalls(cSC4Lot* lot);

private:
    std::uint32_t retainingWallType_ = 0;      // +0x68
    // +0x6c: never written by the original
    cSC4Lot* lot_ = nullptr;                   // +0x70
    int edgeFlags_ = 0;                        // +0x78
    int edgeHash_[4] = {0, 0, 0, 0};           // +0x7c
    cS3DVector3 position_;                     // +0x8c
    cS3DVector3 size_;                         // +0x98
};

}  // namespace sc4
