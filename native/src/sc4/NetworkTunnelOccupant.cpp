#include "sc4/NetworkTunnelOccupant.h"

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
constexpr std::uint16_t kTunnelRecordVersion = 2;
constexpr std::uint32_t kTunnelNetworkFlagClear = 0x00004000u;   // cleared by the constructor and again by Init
constexpr std::uint32_t kTunnelNetworkFlagSet = 0x10c00000u;
}  // namespace

// i386 0x00137b12: no other end yet; the network flags lose 0x4000 and gain 0x10c00000.
cSC4NetworkTunnelOccupant::cSC4NetworkTunnelOccupant() {
    networkFlags_ = (networkFlags_ & ~kTunnelNetworkFlagClear) | kTunnelNetworkFlagSet;
}

cSC4NetworkTunnelOccupant::~cSC4NetworkTunnelOccupant() { SetOtherEndOccupant(nullptr); }

bool cSC4NetworkTunnelOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4NetworkTunnelOccupant) {
        *out = this;
        AddRef();
        return true;
    }
    return cSC4NetworkOccupantWithPreBuiltModel::QueryInterface(iid, out);
}

// i386 0x001378c2: the base's Init, then ClearNetworkFlag(0x4000) (vslot 0x2c).
bool cSC4NetworkTunnelOccupant::Init() {
    if (IsInitialized()) return true;
    const bool ok = cSC4NetworkOccupantWithPreBuiltModel::Init();
    ClearNetworkFlag(kTunnelNetworkFlagClear);
    return ok;
}

// i386 0x00137908: the other end goes, then the base's Shutdown.
bool cSC4NetworkTunnelOccupant::Shutdown() {
    if (!IsInitialized()) return true;
    SetOtherEndOccupant(nullptr);
    return cSC4NetworkOccupantWithPreBuiltModel::Shutdown();
}

void cSC4NetworkTunnelOccupant::SetOtherEndOccupant(cSC4NetworkOccupant* occupant) {
    if (occupant == otherEnd_) return;
    if (occupant) occupant->AddRef();
    if (otherEnd_) otherEnd_->Release();
    otherEnd_ = occupant;
}

// i386 0x00137a46: version 2, the base's record, then a byte (1 = the other end follows as a serializable reference).
bool cSC4NetworkTunnelOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(kTunnelRecordVersion);
    if (cSC4NetworkOccupantWithPreBuiltModel::Write(stream)) {
        if (otherEnd_) {
            stream.SetUint8(1);
            SC4WriteGZSerializable(stream, static_cast<const cIGZSerializable*>(otherEnd_));
        } else {
            stream.SetUint8(0);
        }
    }
    return !stream.GetError();
}

// i386 0x00137970: only version 2 is read.
bool cSC4NetworkTunnelOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kTunnelRecordVersion) return false;
    if (cSC4NetworkOccupantWithPreBuiltModel::Read(stream)) {
        std::uint8_t hasOtherEnd = 0;
        stream.GetUint8(hasOtherEnd);
        SetOtherEndOccupant(nullptr);
        if (hasOtherEnd) {
            void* other = nullptr;
            SC4ReadGZSerializable(stream, kGZCLSID_cSC4NetworkTunnelOccupant, &other, false);
            otherEnd_ = static_cast<cSC4NetworkTunnelOccupant*>(other);   // the reference the read handed over
        }
    }
    return !stream.GetError();
}

}  // namespace sc4
