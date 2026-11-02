#include "sc4/SubwayOccupant.h"

namespace sc4 {

namespace {
bool sbSubwaysInited = false;
tCustomGeometryInfo sCustomGeometryInfo = {};   // PARTIAL: the piece tables are not recovered
}  // namespace

bool cSC4SubwayOccupant::StaticInit() {
    if (sbSubwaysInited) return false;
    sbSubwaysInited = true;
    return cSC4UndergroundOccupant::StaticInit();
}

bool cSC4SubwayOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZCLSID_cSC4SubwayOccupant) return cSC4UndergroundOccupant::QueryInterface(iid, out);
    *out = static_cast<cSC4SubwayOccupant*>(this);
    AddRef();
    return true;
}

bool cSC4SubwayOccupant::Init() {
    if (!sbSubwaysInited) StaticInit();
    if (!IsInitialized()) {
        cSC4UndergroundOccupant::Init();
        cSC4UndergroundOccupant::SetupPackedRenderState();
    }
    return true;
}

bool cSC4SubwayOccupant::Init(const float (*corners)[3]) {
    if (!sbSubwaysInited) StaticInit();
    if (!IsInitialized()) cSC4UndergroundOccupant::Init(corners, kGZCLSID_cSC4SubwayOccupant, &sCustomGeometryInfo);
    return true;
}

bool cSC4SubwayOccupant::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    cSC4UndergroundOccupant::Write(stream);
    return !stream.GetError();
}

bool cSC4SubwayOccupant::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 2) return false;
    if (cSC4UndergroundOccupant::Read(stream)) SetModelDirty(true);
    return !stream.GetError();
}

// The flag test is 0x6000000 (the pipe's model offset), as in the original.
void cSC4SubwayOccupant::GetSortValue(std::uint32_t& out) const {
    if (HasAnyNetworkFlag(0x6000000)) cSC4UndergroundOccupant::GetSortValue(out);
    else out = kGZCLSID_cSC4SubwayOccupant;
}

const tCustomGeometryInfo* cSC4SubwayOccupant::GetCustomGeometryInfo() const { return &sCustomGeometryInfo; }

}  // namespace sc4
