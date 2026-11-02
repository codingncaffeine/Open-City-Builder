// cSC4SubwayOccupant - one piece of subway tunnel (class id / interface id 0xca16374f). A cSC4UndergroundOccupant for network
// type 7 (model instance offset 0x7000000) with no field of its own. Recovered from the Mac i386 build (0x00136bea..0x0013775a,
// 0x00677ea4..); the Windows record is the same (network-occupants.md).
//
// PARTIAL, as for the pipe: the subway statics (StaticInit's diagonal-piece edge distances, the tCustomGeometryInfo piece
// tables) are not recovered, so a subway loads, saves and answers its interfaces but builds no model; GetAltitude
// (0x00136d5c), InitPathBottomQuad (0x00136e1c) and GetPathInfo (0x00137320), the train paths, are not ported.
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

    static bool StaticInit();                                            // 0x0013756a (PARTIAL)
};

}  // namespace sc4
