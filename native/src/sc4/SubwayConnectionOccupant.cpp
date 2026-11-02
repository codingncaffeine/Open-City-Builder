#include "sc4/SubwayConnectionOccupant.h"

namespace sc4 {

namespace {
tCustomGeometryInfo sCustomGeometryInfo = {};   // PARTIAL: the piece tables are not recovered
}

bool cSC4SubwayConnectionOccupant::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4SubwayConnectionOccupant) {
        *out = static_cast<cSC4SubwayConnectionOccupant*>(this);
        AddRef();
        return true;
    }
    return cSC4UndergroundConnectionOccupant::QueryInterface(iid, out);
}

bool cSC4SubwayConnectionOccupant::Init() {
    type_ = kSubwayConnectionOccupantType;
    return cSC4UndergroundConnectionOccupant::Init(kGZCLSID_cSC4SubwayConnectionOccupant, &sCustomGeometryInfo);
}

}  // namespace sc4
