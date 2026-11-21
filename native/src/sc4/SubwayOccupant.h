// cSC4SubwayOccupant - one piece of subway tunnel (class id / interface id 0xca16374f). A cSC4UndergroundOccupant for network
// type 7 (model instance offset 0x7000000) plus the train paths' floor quad. Recovered from the Mac i386 build (0x00136bea..0x0013775a,
// 0x00677ea4..); the Windows record is the same (network-occupants.md).
//
// The piece tables (sCustomGeometryInfo) are in SubwayOccupantGeometry.cpp; StaticInit fills the two diagonal distance
// tables, as the pipe's does.
//
// The train paths: GetPathInfo (0x00137320) loads the piece's path file from group 0xa966883f, transforms it onto the piece and
// sets every path point's height from the tube floor (InitPathBottomQuad, 0x00136e1c: the piece model's bottom quad for the
// crossing and pipe pieces, else the trench floor; GetAltitude, 0x00136d5c, interpolates it half a unit lower). The
// original object adds that quad (four vertices, i386 +0x26c) to the underground occupant.
#pragma once
#include "sc4/UndergroundOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4SubwayOccupant = 0xca16374f;
constexpr std::uint32_t kSubwayNetworkType = 7;
constexpr std::int32_t kSubwayModelInstanceOffset = 0x7000000;

class cSC4SubwayOccupant : public cSC4UndergroundOccupant {
public:
    cSC4SubwayOccupant() = default;                                      // 0x00136bea
    ~cSC4SubwayOccupant() override = default;                            // 0x001377a6

    bool QueryInterface(std::uint32_t iid, void** out) override;        // 0x00136ce6
    bool Init() override;                                                // 0x0013771a
    bool Init(const float (*corners)[3]);                                // 0x001376c8
    bool Shutdown() override { return cSC4UndergroundOccupant::Shutdown(); }   // 0x00136c3e
    bool Write(cIGZOStream& stream) const override;                      // 0x00136c56 (record version 2)
    bool Read(cIGZIStream& stream) override;                             // 0x00136c92
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4SubwayOccupant; }   // 0x00677ea4
    void GetSortValue(std::uint32_t& out) const override;               // 0x00136d22
    const tCustomGeometryInfo* GetCustomGeometryInfo() const override;  // 0x00677ec2
    std::uint32_t GetNetworkType() const override { return kSubwayNetworkType; }                 // 0x00677ecc
    std::int32_t GetModelInstanceOffset() const override { return kSubwayModelInstanceOffset; }   // 0x00677ed6

    // the train paths
    float GetAltitude(float x, float z) const override;                  // 0x00136d5c (vslot 0xe0)
    virtual void InitPathBottomQuad();                                   // 0x00136e1c (vslot 0x1f8)
    cISC4PathInfo* GetPathInfo() override;                               // 0x00137320
    const UndergroundVertex& PathBottomVertex(int corner) const { return pathBottomQuad_[corner]; }

    static bool StaticInit();                                            // 0x0013756a
    static tCustomGeometryInfo sCustomGeometryInfo;                      // i386 0x00ab83c0
    static float sDiagonalPieceEdgeDistancePercentages[5];              // i386 0x00bfaa34
    static float sDiagonalCapEdgeDistancePercentages[5];                // i386 0x00bfaa20

private:
    UndergroundVertex pathBottomQuad_[4];                                // i386 +0x26c
};

}  // namespace sc4
