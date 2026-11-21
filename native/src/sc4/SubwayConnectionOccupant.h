// cSC4SubwayConnectionOccupant - the connection piece of a subway (class id / interface 0xca15ed6b): a
// cSC4UndergroundConnectionOccupant for network type 7 that joins a subway tunnel (cSC4SubwayOccupant, 0xca16374f) to the
// station above. Recovered from the Mac i386 build (0x002f0282..0x002f0436).
//
// PARTIAL: the connection's geometry table (sCustomGeometryInfo, read only by the unported drawing) is not recovered.
#pragma once
#include "sc4/SubwayOccupant.h"
#include "sc4/UndergroundConnectionOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4SubwayConnectionOccupant = 0xca15ed6b;
constexpr std::uint32_t kSubwayConnectionOccupantType = 0x8a13918a;   // the occupant type Init stamps

class cSC4SubwayConnectionOccupant : public cSC4UndergroundConnectionOccupant {
public:
    cSC4SubwayConnectionOccupant() = default;                            // 0x002f0282
    bool QueryInterface(std::uint32_t iid, void** out) override;        // 0x002f03ec
    bool Init() override;                                                // 0x002f03a8
    bool Shutdown() override { return cSC4UndergroundConnectionOccupant::Shutdown(); }   // 0x002f03d6
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4SubwayConnectionOccupant; }
    void GetSortValue(std::uint32_t& out) const override { out = kGZCLSID_cSC4SubwayConnectionOccupant; }   // 0x002f0428
    std::uint32_t GetGZIID() const override { return kGZCLSID_cSC4SubwayConnectionOccupant; }
    std::uint32_t GetNetworkOccupantGZIID() const override { return kGZCLSID_cSC4SubwayOccupant; }
    std::uint32_t GetNetworkType() const override { return kSubwayNetworkType; }
    bool QueryOccupantInterface(cISC4Occupant* occupant, cSC4UndergroundOccupant*& out) override {
        return QueryPieceInterface(occupant, kGZCLSID_cSC4SubwayOccupant, out);
    }
};

}  // namespace sc4
