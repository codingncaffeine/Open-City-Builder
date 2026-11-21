// cSC4NetworkTunnelOccupant (GZCOM class 0x8a4bd52b): one end of a tunnel, a network piece with a pre-built model that
// knows the piece at the other end. Recovered from the Mac i386 build (0x00137872..0x00137b12).
#pragma once
#include <cstdint>

#include "sc4/NetworkOccupantWithPreBuiltModel.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4NetworkTunnelOccupant = 0x8a4bd52b;

class cSC4NetworkTunnelOccupant : public cSC4NetworkOccupantWithPreBuiltModel {
public:
    cSC4NetworkTunnelOccupant();                                    // i386 0x00137b12
    ~cSC4NetworkTunnelOccupant() override;

    bool QueryInterface(std::uint32_t iid, void** out) override;  // i386 0x00137872: answers its own class id
    bool Init() override;                                          // i386 0x001378c2
    bool Shutdown() override;                                      // i386 0x00137908
    bool Write(cIGZOStream& stream) const override;                // i386 0x00137a46 (version 2)
    bool Read(cIGZIStream& stream) override;                       // i386 0x00137970
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4NetworkTunnelOccupant; }

    cSC4NetworkOccupant* GetOtherEndOccupant() const { return otherEnd_; }   // i386 0x00137??? (+0x1c8)
    void SetOtherEndOccupant(cSC4NetworkOccupant* occupant);                // i386 0x00137950

private:
    cSC4NetworkOccupant* otherEnd_ = nullptr;   // i386 +0x1c8 (a held reference)
};

}  // namespace sc4
