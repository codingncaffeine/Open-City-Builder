// cSC4PipeConnectionOccupant - the connection piece of a water pipe (class id / interface 0xea0a5acf): a
// cSC4UndergroundConnectionOccupant for network type 4 that joins a pipe (cSC4PipeOccupant, 0x49c05b9f) to the building
// above. Recovered from SimCity 4 Deluxe Edition 1.2.1 (0x1003dde60..0x1003de14a).
//
// Its geometry table (sCustomGeometryInfo): a half pipe facing the camera from the building down to the trench, with the
// models 0x6a089741 (top) and 0x6a089740 (bottom).
#pragma once
#include "sc4/PipeOccupant.h"
#include "sc4/UndergroundConnectionOccupant.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PipeConnectionOccupant = 0xea0a5acf;
constexpr std::uint32_t kPipeConnectionOccupantType = 0x2a183167;   // the occupant type Init stamps at +0x48

class cSC4PipeConnectionOccupant : public cSC4UndergroundConnectionOccupant {
public:
    cSC4PipeConnectionOccupant() = default;                              // 0x1003dde60
    bool QueryInterface(std::uint32_t iid, void** out) override;        // 0x1003ddfac
    bool Init() override;                                                // 0x1003ddf84
    bool Shutdown() override { return cSC4UndergroundConnectionOccupant::Shutdown(); }   // 0x1003ddfa2
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PipeConnectionOccupant; }   // 0x1003de126
    void GetSortValue(std::uint32_t& out) const override { out = kGZCLSID_cSC4PipeConnectionOccupant; }   // 0x1003de078
    std::uint32_t GetGZIID() const override { return kGZCLSID_cSC4PipeConnectionOccupant; }     // 0x1003de132
    std::uint32_t GetNetworkOccupantGZIID() const override { return kGZCLSID_cSC4PipeOccupant; }   // 0x1003de13e
    std::uint32_t GetNetworkType() const override { return kPipeNetworkType; }                  // 0x1003de14a
    bool QueryOccupantInterface(cISC4Occupant* occupant, cSC4UndergroundOccupant*& out) override {
        return QueryPieceInterface(occupant, kGZCLSID_cSC4PipeOccupant, out);
    }

    static const tUndergroundConnectionGeometryInfo sCustomGeometryInfo;   // i386 0x00ab9ba0
};

}  // namespace sc4
