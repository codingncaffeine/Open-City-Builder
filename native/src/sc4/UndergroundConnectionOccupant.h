// cSC4UndergroundConnectionOccupant - the piece that joins an underground network (pipe or subway) to a building above it
// (class id / interface 0x8a139184, object 0x300 bytes). A cSC4Occupant plus a cS3DModelInstanceBasic and a message target
// (view +0x60 subscribed to four view messages) that keeps the geometry of the connection piece (walls, trench, highlight,
// piece), the four corner heights, the cell and the two occupants it joins. Recovered from SimCity 4 Deluxe Edition 1.2.1
// (0x1002ef17a..0x1002f28b0).
//
// Record (version 2): the occupant, the wall quad count, seven vertex vectors, the cell, the four corner heights, the model
// height, an int, a flag, the building occupant, the network occupant, the interface id and the position.
//
// A lot that is a subway station or has a water connection gets one when it is plopped (cSC4ViewInputControlPlaceLot::
// PlaceLot): SetPosition at the lot's center cell, Init (the cell, the trench floor quad, the network piece of the cell,
// or a new piece joining a neighbor piece), SetBuildingOccupant, then the occupant manager. DoMessage follows the network
// pieces inserted on and removed from its cell and removes the connection with its building. Mac i386
// 0x0022f0f2..0x0023239e; the offsets in the comments below are i386.
//
// PARTIAL: the drawing is not recovered: CreateGeometry (0x002315e4, run on the view message 0xa6b79602), CreatePieceModel,
// CreateWalls / AddWallQuad, CreateFloor, CreateHighlight / SetHighlight, ChangeZoomLevel, Draw and GetModelInstances;
// a connection keeps the geometry its record stored.
#pragma once
#include <vector>

#include "gz/StreamOps.h"
#include "gz/cIGZMessage2.h"
#include "s3d/ModelInstance.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/UndergroundOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4UndergroundConnectionOccupant = 0x8a139184;
constexpr std::uint32_t kGZIID_cISC4PreBuiltUndergroundView = 0xa9764c8c;
constexpr std::uint32_t kUndergroundConnectionOccupantIID = 0x26bb767e;   // cISC4Occupant (the stored neighbours)
constexpr std::uint16_t kUndergroundConnectionRecordVersion = 2;
constexpr std::uint32_t kUndergroundConnectionViewMessages[4] = {0x8b87a974, 0x8b87a975, 0x8b8b2cee, 0xa6b79602};
constexpr float kTrenchFloorDepth = 15.0f;   // 0x00ab98a4

class cSC4UndergroundConnectionOccupant;

// The engine globals the connection reaches (one method per original call).
class IUndergroundConnectionHost {
public:
    virtual ~IUndergroundConnectionHost() = default;
    virtual void PositionToCell(float x, float z, int& cellX, int& cellZ) = 0;           // spCity vslot 0x17c
    virtual void CellCornerToPosition(int cellX, int cellZ, float& x, float& z) = 0;    // spCity vslot 0x180
    virtual float CellWidthX() = 0;                                                    // spCity vslot 0x16c
    virtual float CellWidthZ() = 0;                                                    // spCity vslot 0x170
    // spCity vslot 0x150 (the terrain) vslot 0x44: a cell's corner heights, (x, z), (x, z+1), (x+1, z+1), (x+1, z).
    virtual void CellCornerHeights(int cellX, int cellZ, float* heights) = 0;
    // cSC4UndergroundNetworkTool::FindUndergroundOccupant (static): the cell's piece answering `gziid`, one
    // reference, or null.
    virtual cSC4UndergroundOccupant* FindUndergroundOccupant(int cellX, int cellZ, std::uint32_t gziid) = 0;
    // cSC4UndergroundNetworkTool::FindUndergroundConnectionOccupant (static): the cell's connection whose network piece id
    // is `gziid`, one reference, or null.
    virtual cSC4UndergroundConnectionOccupant* FindUndergroundConnectionOccupant(int cellX, int cellZ, std::uint32_t gziid) = 0;
    // SL::NetworkManager() vslot 0x14 GetNetworkTool(type, false); AddRef, Init, PlaceNetworkOccupants(x0, z0, x1, z1, 0, null,
    // true, -1) (cISC4NetworkTool vslot 0x1c), Shutdown, Release.
    virtual void PlaceNetworkOccupants(std::uint32_t networkType, int x0, int z0, int x1, int z1) = 0;
    virtual void RemoveOccupant(cISC4Occupant* occupant, bool shutdown, std::uint32_t data) = 0;   // spOccupantManager vslot 0x54
    virtual void ChangeModelInRenderer(cSC4UndergroundConnectionOccupant* connection, int change) = 0;   // spRenderer vslot 0x7c
};
void SetUndergroundConnectionHost(IUndergroundConnectionHost* host);

class cSC4UndergroundConnectionOccupant : public cSC4Occupant, public cIGZMessageTarget2, public s3d::cS3DModelInstanceBasic {
public:
    cSC4UndergroundConnectionOccupant();                       // 0x0023239e
    ~cSC4UndergroundConnectionOccupant() override;             // 0x00230510

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x0022f364
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }     // 0x0022f4a6
    std::uint32_t Release() override { return cSC4Occupant::Release(); }   // 0x0022f4b0

    bool Init() override;                                      // 0x0022f87a
    bool Init(std::uint32_t gziid, tCustomGeometryInfo* info); // 0x0022f916
    bool Shutdown() override;                                  // 0x0022ff94
    bool Write(cIGZOStream& stream) const override;            // 0x00230344
    bool Read(cIGZIStream& stream) override;                   // 0x0023089a
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4UndergroundConnectionOccupant; }
    void GetSortValue(std::uint32_t& out) const override { out = kGZCLSID_cSC4UndergroundConnectionOccupant; }   // 0x0022f3cc
    void GetPosition(float* xyz) override;                     // 0x0022f458
    void SetPosition(const float* xyz);                        // 0x0022f47e
    void GetBoundingBox(float* minXyz, float* maxXyz) override;   // 0x0022f174
    void ComputeBoundingBox(float* minXyz, float* maxXyz);     // 0x0022f280 (vslot 0xd8; the same body)
    bool DoMessage(cIGZMessage2* message) override;            // 0x0023197a (PARTIAL: CreateGeometry)
    void GetWorldBounds(cS3DBoundingBox& out) override;        // 0x0022f4ba
    void PostOccupantMessage(std::uint32_t message, std::uint32_t data) override;   // 0x0022f5aa
    void ModelStateChanged();                                  // 0x0022f3da
    void UpdateNetworkOccupant();                              // 0x0022f4e0 (vslot 0xdc)
    void SetBuildingOccupant(cISC4Occupant* building);         // 0x0022f508 (vslot 0xd0)
    // 0x0022f440: a connection piece hides both triangles of its cell (the first answer is always false).
    void GetHiddenTriangles(bool& first, bool& even, bool& odd) const { first = false; even = true; odd = true; }

    // the pure virtuals the pipe / subway connection classes answer
    virtual std::uint32_t GetGZIID() const = 0;                // slot of the derived class id
    virtual std::uint32_t GetNetworkOccupantGZIID() const = 0; // vslot 0xc8
    virtual std::uint32_t GetNetworkType() const = 0;          // vslot 0xcc
    // vslot 0xe0: the occupant as this connection's network piece (QueryInterface the piece id, then 0x4a10afc2), one
    // reference.
    virtual bool QueryOccupantInterface(cISC4Occupant* occupant, cSC4UndergroundOccupant*& out) = 0;

    const std::vector<UndergroundVertex>& Walls(int edge) const { return walls_[edge]; }
    std::uint32_t WallQuadCount() const { return wallCount_; }
    cISC4Occupant* NetworkOccupant() const { return network_; }     // +0xc4
    cISC4Occupant* BuildingOccupant() const { return building_; }   // +0xc8
    int CellX() const { return cellX_; }
    int CellZ() const { return cellZ_; }
    const UndergroundVertex& FloorVertex(int corner) const { return floor_[corner]; }
    float TrenchFloorY() const { return trenchFloorY_; }

protected:
    // The body of both QueryOccupantInterface overrides (0x002f01fa / 0x002f0436): QueryInterface the piece id, then the
    // underground occupant id 0x4a10afc2 on what answered.
    static bool QueryPieceInterface(cISC4Occupant* occupant, std::uint32_t pieceId, cSC4UndergroundOccupant*& out);
    void SetGZIIDField(std::uint32_t id) { gziid_ = id; }

private:
    std::uint32_t wallCount_ = 0;                       // +0x64
    std::vector<UndergroundVertex> walls_[4];           // +0x68 .. +0x8c
    std::vector<UndergroundVertex> trench_;             // +0x9c
    std::vector<UndergroundVertex> highlight_;          // +0xa8
    std::vector<UndergroundVertex> piece_;              // +0xb8
    cISC4Occupant* network_ = nullptr;                  // +0xc4 the network piece of the cell (held)
    cISC4Occupant* building_ = nullptr;                 // +0xc8 the building above (held)
    std::int32_t cellX_ = 0, cellZ_ = 0;                // +0x14c / +0x150
    float cornerHeights_[4] = {};                       // +0x15c the terrain heights at the cell's corners
    float modelY_ = 0.0f;                               // +0x16c the position's height
    float trenchFloorY_ = 0.0f;                         // +0x170 modelY - kTrenchFloorDepth
    std::int32_t value174_ = 0;                         // +0x174 (Init sets 1)
    bool modelDirty_ = false;                           // +0x178
    float bounds_[6] = {};                              // +0x17c min, +0x188 max
    UndergroundVertex floor_[4];                        // +0x1cc the trench floor quad
    float position_[3] = {};                            // +0x22c
    tCustomGeometryInfo* geometryInfo_ = nullptr;       // +0x238
    std::uint32_t gziid_ = 0;                           // +0x23c
    bool loaded_ = false;                               // +0x240 (Read sets it: Init keeps the stored pieces)
    bool subscribed_ = false;
};

}  // namespace sc4
