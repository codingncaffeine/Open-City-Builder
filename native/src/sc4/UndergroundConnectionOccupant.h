// cSC4UndergroundConnectionOccupant - the piece that joins an underground network (pipe or subway) to a building above it
// (class id / interface 0x8a139184, object 0x300 bytes). A cSC4Occupant plus a cS3DModelInstanceBasic and a message target
// (view +0x60 subscribed to four view messages) that keeps the geometry of the connection piece (walls, trench, highlight,
// piece), the four corner heights, the cell and the two occupants it joins. Recovered from SimCity 4 Deluxe Edition 1.2.1
// (0x1002ef17a..0x1002f28b0).
//
// Record (version 2): the occupant, the wall quad count, seven vertex vectors, the cell, the four corner heights, the model
// height, an int, a flag, the building occupant, the network occupant, the interface id and the position.
//
// PARTIAL: Init(gziid, geometry) builds the corner and texture tables from the city (0x1002ef730), and the drawing,
// geometry (CreateGeometry / CreatePieceModel / CreateWalls / CreateFloor / CreateHighlight), DoMessage and the
// building / network occupant lookups are not recovered; a connection loads and saves with its geometry as stored.
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

class cSC4UndergroundConnectionOccupant : public cSC4Occupant, public cIGZMessageTarget2, public s3d::cS3DModelInstanceBasic {
public:
    cSC4UndergroundConnectionOccupant();                       // 0x1002ef17a
    ~cSC4UndergroundConnectionOccupant() override;             // 0x1002ef3ec

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x1002f12e8
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }     // 0x1002f21fc
    std::uint32_t Release() override { return cSC4Occupant::Release(); }   // 0x1002f224e

    bool Init() override;                                      // 0x1002ef53a
    bool Init(std::uint32_t gziid, tCustomGeometryInfo* info); // 0x1002ef730 (PARTIAL: records the arguments only)
    bool Shutdown() override;                                  // 0x1002efd16
    bool Write(cIGZOStream& stream) const override;            // 0x1002efec8
    bool Read(cIGZIStream& stream) override;                   // 0x1002f0178
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4UndergroundConnectionOccupant; }   // 0x1002f28b0
    void GetSortValue(std::uint32_t& out) const override { out = kGZCLSID_cSC4UndergroundConnectionOccupant; }   // 0x1002f139e
    void GetPosition(float* xyz) override;                     // 0x1002f217e
    void GetBoundingBox(float* minXyz, float* maxXyz) override;   // 0x1002f1154
    bool DoMessage(cIGZMessage2* message) override;            // 0x1002f25ea (PARTIAL)
    void GetWorldBounds(cS3DBoundingBox& out) override;        // 0x1002f246a
    // i386 0x0022f440: a connection piece hides both triangles of its cell (the first answer is always false).
    void GetHiddenTriangles(bool& first, bool& even, bool& odd) const { first = false; even = true; odd = true; }

    // the pure virtuals the pipe / subway connection classes answer
    virtual std::uint32_t GetGZIID() const = 0;                // slot of the derived class id
    virtual std::uint32_t GetNetworkOccupantGZIID() const = 0;
    virtual std::uint32_t GetNetworkType() const = 0;

    const std::vector<UndergroundVertex>& Walls(int edge) const { return walls_[edge]; }
    std::uint32_t WallQuadCount() const { return wallCount_; }

protected:
    void SetGZIIDField(std::uint32_t id) { gziid_ = id; }

private:
    std::uint32_t wallCount_ = 0;                       // +0xa8
    std::vector<UndergroundVertex> walls_[4];           // +0xb0 .. +0x110
    std::vector<UndergroundVertex> trench_;             // +0x118
    std::vector<UndergroundVertex> highlight_;          // +0x130
    std::vector<UndergroundVertex> piece_;              // +0x150
    cIGZUnknown* building_ = nullptr;                   // +0x168 (held)
    cIGZUnknown* network_ = nullptr;                    // +0x170 (held)
    std::int32_t cellX_ = 0, cellZ_ = 0;                // +0x1f8 / +0x1fc
    float cornerHeights_[4] = {};                       // +0x210
    float modelY_ = 0.0f;                               // +0x220
    std::int32_t value228_ = 0;                         // +0x228
    bool flag22c_ = false;                              // +0x22c
    float position_[3] = {};                            // +0x2e0
    std::uint32_t gziid_ = 0;                           // +0x2f8
    tCustomGeometryInfo* geometryInfo_ = nullptr;       // +0x2f0
    bool flag2fc_ = false;                              // +0x2fc
    bool subscribed_ = false;
};

}  // namespace sc4
