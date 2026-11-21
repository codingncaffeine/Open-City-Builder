// cSTESurfaceWater — the surface water of a city (vtable 0x100aea540, class id 0x0b8da7db, interface 0xab8da759, 0xa8 bytes): one
// tWaterInfo (0x24 bytes) per terrain vertex, the bodies-of-water map, and the flood / rain simulation.
//
// Object layout: +0x18 initialised, +0x1c refcount, +0x20 counted cSTETerrain, +0x28 vector<tWaterInfo>, +0x40 vector (cleared by
// EndBrushScrub), +0x58 list<int>, +0x70 per-vertex mode flag, +0x74 active channel, +0x78 cRZCellMap<bool>, +0x80 vector<float>,
// +0x98 counted cSC4SimGrid<u16> (the bodies of water, instance 0x4be3618b), +0xa0 "rebuild the bodies" flag. Three views: the
// primary interface (the getters), +8 the serializable (Write / Read / GetGZCLSID) and +0x10 the message target (DoMessage).
//
// Port divergences (recorded in the ledger): the terrain model keeps channel 0 (depth) and channel 0x1c (sediment) of every vertex
// in its own arrays (cSTETerrain::WaterDepth / Sediment); this object keeps the full tWaterInfo vector and copies those two channels
// into the terrain after every change it makes (SyncTerrain), so the terrain's water queries see the same numbers.
//
// The flood simulation (UpdateFloodWaters), the neighbour-connection scan, FindLocalMinimumOrWater, GetBasinVertices and Rain_Static are
// recovered from the disassembly; their float order is checked by reading, not by running the retail code (ledger evidence says so).
// Not reproduced: UpdateFloodWaters' cRZTimer (it only measures the pass) and its call to the city (its result is unused).
#pragma once
#include <cstdint>
#include <climits>
#include <list>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/StreamOps.h"
#include "gz/cIGZSerializable.h"
#include "sc4/SpiralIterator.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/cSTETerrain.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSTESurfaceWater = 0x0b8da7db;
constexpr std::uint32_t kGZIID_cISTESurfaceWater = 0xab8da759;
constexpr std::uint32_t kWaterInfoType = 0x8a91e7ef, kWaterInfoGroup = 0xe98f9525, kWaterInfoInstance = 4;   // SaveWaterInfo / LoadWaterInfo
constexpr std::uint32_t kBodiesOfWaterGridInstance = 0x4be3618b;
constexpr std::uint32_t kWaterMsgCityCell1 = 0x26d31ec1, kWaterMsgCityCell4 = 0x26d31ec4, kWaterMsgCityCell5 = 0x26d31ec5,
                        kWaterMsgNetworkBuilt = 0x89d3e9f3, kWaterMsgOccupantAdded = 0x99ef1142, kWaterMsgOccupantMoved = 0x99ef1143,
                        kWaterMsgRebuildBodies = 0x66956814, kWaterMsgAnimate = 0x86ad10ee;
constexpr std::uint32_t kWaterOccupantGroup = 0x2890d4de;             // the occupants IsNavigable and DoMessage look at
constexpr float kDraftClearanceDepth = 5.0f;                          // 0x100a16210
constexpr std::uint32_t kWaterConnectionType = 9;                      // the neighbour connection type the water scan places
constexpr float kShallowWater = 30.0f;                                // 0x100a1969c: depth below which a connection spot is moved
constexpr float kBridgeClearance = 20.0f;                             // a bridge must clear the water by this much to let ships through

struct tWaterInfo {                        // 0x24 bytes
    float depth = 0.0f;                    // +0x00
    float flow[3] = {0.0f, 0.0f, 0.0f};    // +0x04 / +0x08 / +0x0c (the active channel is chosen by +0x74)
    cS3DVector3 velocity{};                // +0x10
    float sediment = 0.0f;                 // +0x1c
    float scratch = 0.0f;                  // +0x20 (not saved)
};
static_assert(sizeof(tWaterInfo) == 0x24);

// A network occupant (interface 0xa821ef94) as the surface water uses it (the cISC4NetworkOccupant slots it calls).
class IWaterNetworkOccupant {
public:
    virtual ~IWaterNetworkOccupant() = default;
    virtual bool HasNetworkFlag(std::uint32_t flag) = 0;                   // vslot 0x40
    virtual bool IsUsable() = 0;                                            // vslot 0x98 IsUsable
    virtual void GetPosition(float xyz[3]) = 0;                             // vslot 0x180 -> vslot 0x40
    virtual std::uint8_t ActiveEdges() = 0;                                 // vslot 0x140 GetActiveEdges
    virtual bool PassageCeiling(float& height) = 0;                         // vslot 0x218 GetPassageCeiling (the clearance under a bridge)
};
// An occupant of group 0x2890d4de asked for iid 0xa8c86142 (cISC4PowerPoleOccupant: a power pole).
class IWaterCellOccupant {
public:
    virtual ~IWaterCellOccupant() = default;
    virtual void GetCell(int& x, int& z) = 0;                               // vslot 0x40
    virtual bool IsPreview() = 0;                                           // vslot 0x68 (the power line tool's preview pole)
};

class cSTESurfaceWater;

// The services the surface water reaches: SL::spCity, spMS2, spSimulator, ATM, spTerrain, the traffic network map, the lot manager and
// the occupant manager.
class ISurfaceWaterHost {
public:
    virtual ~ISurfaceWaterHost() = default;
    virtual cSTETerrain* Terrain() = 0;                                     // SL::spTerrain
    virtual bool CityInitialized() = 0;                                     // spCity vslot 0xb0
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;   // spCity vslot 0x2f8
    virtual int CitySizeX() = 0;                                            // spCity vslot 0x2e8
    virtual int CitySizeZ() = 0;                                            // spCity vslot 0x2f0
    virtual bool CellInCity(int x, int z) = 0;                              // spCity vslot 0x318
    virtual void AddNotifications(cSTESurfaceWater& water) = 0;             // spMS2 vslot 0x28 for the six messages
    virtual void RemoveNotifications(cSTESurfaceWater& water) = 0;          // spMS2 vslot 0x30
    virtual void RegisterTicks(cSTESurfaceWater& water) = 0;                // spSimulator vslot 0x98(target, 4, "cSTESurfaceWater", 2) and ATM vslot 0x28(target, 0xc0)
    virtual void UnregisterTicks(cSTESurfaceWater& water) = 0;              // ATM vslot 0x30(target, 0xc0) and spCity vslot 0x190 -> vslot 0xa8
    virtual IWaterNetworkOccupant* NetworkOccupantAt(int x, int z, std::uint32_t mask, bool exact) = 0;   // spTrafficNetworkMap vslot 0x50
    virtual IWaterNetworkOccupant* AsNetworkOccupant(cIGZUnknown* object) = 0;   // GZCOM_Interface(object, 0xa821ef94); counted by the host
    virtual IWaterCellOccupant* AsCellOccupant(cIGZUnknown* object) = 0;    // QueryInterface(0xa8c86142)
    virtual bool NeighborHasConnection(std::uint32_t type, int x, int z) = 0;   // spNeighborsSim vslot 0x50
    virtual void NeighborDeleteConnection(std::uint32_t type, int x, int z) = 0;   // vslot 0x38
    virtual void NeighborCreateConnection(std::uint32_t type, int x, int z) = 0;   // vslot 0x30(type, x, z, false)
    virtual const void* LotAtCell(int x, int z) = 0;                        // terrain vslot 0xe8 / spCity vslot 0x308 / spLotManager vslot 0x30
    // spOccupantManager vslot 0x140(list, rect, rect, 0x2890d4de, 0xffffffff): the occupants of the group inside one cell
    virtual void OccupantsInCell(int x, int z, std::uint32_t group, std::vector<IWaterCellOccupant*>& out) = 0;
};

class cSTESurfaceWater : public cIGZSerializable, public cIGZMessageTarget2 {
public:
    cSTESurfaceWater(ISurfaceWaterHost* host) : host_(host) {}
    ~cSTESurfaceWater();

    bool QueryInterface(std::uint32_t iid, void** out) override;              // @ 0x10072e1ea
    std::uint32_t AddRef() override { return ++refCount_; }                   // @ 0x10072e23a
    std::uint32_t Release() override;                                         // @ 0x10072e264

    bool Init();                                                              // 0x18 @ 0x10072e312
    bool Shutdown();                                                          // 0x20 @ 0x10072e6d8
    float GetWaterDepthAtVertex(int vertex) const;                            // 0x28 @ 0x10072f6dc
    float GetWaterDepthForCell(int x, int z) const;                           // 0x30 @ 0x10072f6f2
    virtual bool IsNavigable(int x, int z, int radius, const void* lot) const;   // 0x38 @ 0x10072f7ec
    bool IsNavigable(const cS3DVector3& position, int radius, const void* lot) const;   // 0x40 @ 0x10072f784
    cSC4SimGrid<std::uint16_t>* GetBodyOfWaterMap() const { return bodies_; }   // 0x48 @ 0x100730f38
    float AltitudeAtWhichFloodingWillOccur(int x, int z) const;               // 0x50 @ 0x100730b40
    const cS3DVector3& GetWaterVelocity(int x, int z) const;                  // 0x58 @ 0x100730c48
    int GetWaterType(int x, int z) const;                                     // 0x60 @ 0x100730c72
    virtual bool HasDraftClearance(int x, int z) const;                       // 0x68 @ 0x10072f76e

    bool Write(cIGZOStream& stream) const override;                           // @ 0x10072e86a
    bool Read(cIGZIStream& stream) override;                                  // @ 0x10072ea64
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSTESurfaceWater; }   // @ 0x100730f46
    bool DoMessage(cIGZMessage2* message) override;                           // @ 0x10072ec5a

    bool SaveWaterInfo(cIGZPersistDBSegment* segment) const;                  // @ 0x10072e91c
    bool LoadWaterInfo(cIGZPersistDBSegment* segment);                        // @ 0x10072eb28
    void WriteWaterRecord(cIGZOStream& stream) const;                         // the body of SaveWaterInfo (after the record header)
    void ReadWaterRecord(cIGZIStream& stream);                                // the body of LoadWaterInfo
    void ResetWaterToSeaLevel(const TerrainRect& rect);                       // @ 0x10072e648
    void UpdateBodiesOfWater();                                               // @ 0x10072f50a
    void FloodFillBodyOfWater(int x, int z, std::uint16_t id);                // @ 0x100730d26
    void AnimateWater();                                                      // @ 0x10072f5f0
    void EndBrushScrub(std::uint32_t brushType, std::uint32_t arg);           // @ 0x10072f696
    void CreateSpring(int vertex) { info_[static_cast<std::size_t>(vertex)].flow[0] += 1.0f; }   // @ 0x100730b16
    bool IsFlatWater(int vertex) const;                                       // @ 0x100730cd0

    // The recovered-later parts: they log SC4_UNWIRED and leave the water as it is.
    virtual void UpdateNeighborConnections();                                 // @ 0x10072efca
    virtual void UpdateFloodWaters();                                         // @ 0x10072fb5c
    int FindLocalMinimumOrWater(int vertex) const;                            // @ 0x1007303b0
    void GetBasinVertices(int seed, float level, float limit, std::list<int>& basin, std::list<int>& rim, std::list<int>& spill) const;   // @ 0x100730492
    bool Rain(int vertex, TerrainBrushContext& context, TerrainRect& rect, float amount);   // @ 0x100730820: true when the per-vertex mode takes the rain
    void RainStatic(int vertex, TerrainBrushContext& context, TerrainRect& rect, float amount);   // @ 0x100730882
    void AddRain(int vertex, float amount) { info_[static_cast<std::size_t>(vertex)].flow[1 + channel_] += amount; }   // cSTETerrain::BrushFnRain @ 0x10073083e

    // ---- accessors for the tests and the host ----
    std::vector<tWaterInfo>& Info() { return info_; }
    const std::vector<tWaterInfo>& Info() const { return info_; }
    bool Initialized() const { return initialized_; }
    bool PerVertexMode() const { return perVertex_; }
    void SetPerVertexMode(bool on) { perVertex_ = on; }
    bool BodiesDirty() const { return bodiesDirty_; }
    void SyncTerrain();                                                       // copies depth and sediment into the terrain

private:
    ISurfaceWaterHost* host_ = nullptr;
    bool initialized_ = false;                       // +0x18
    std::uint32_t refCount_ = 0;                     // +0x1c
    cSTETerrain* terrain_ = nullptr;                 // +0x20 (counted in the original)
    std::vector<tWaterInfo> info_;                   // +0x28
    std::vector<float> scratchFloats_;               // +0x80
    bool perVertex_ = false;                         // +0x70
    std::uint32_t channel_ = 0;                      // +0x74
    TerrainRect dirty_{};                            // +0x78 (the rect of the cell map: the cells the simulation visits)
    std::vector<float> flipStrength_;                // +0x40 per cell: how strongly the last flow asked the triangulation to flip
    cSC4SimGrid<std::uint16_t>* bodies_ = nullptr;   // +0x98 counted
    bool bodiesDirty_ = false;                       // +0xa0
};

}  // namespace sc4
