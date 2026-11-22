#include "sc4/MyStreetSim.h"

#include "platform/Unwired.h"

namespace sc4 {

cSC4MyStreetSim::cSC4MyStreetSim() {
    SetBitmapId(kMyStreetSimBitmapId);
    SetTrackHandle(-1);
}

bool cSC4MyStreetSim::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZCLSID_cSC4MyStreetSim || iid == kGZIID_cISC4MyStreetSim) {
        *out = static_cast<cSC4MyStreetSim*>(this);   // PARTIAL: the original answers the +0x430 view for 0x0bf61276
        AddRef();
        return true;
    }
    return cSC4MySim::QueryInterface(iid, out);
}

bool cSC4MyStreetSim::Init(int advisorType) {
    if (!cSC4MySim::Init(advisorType)) return false;
    if (LuaTableRef() == 0) CreateLuaTable();
    SC4_UNWIRED("cSC4MyStreetSim: the trip behavior (DoMessage, update_trip_variables, simulate_*, Attach/DetachAutomaton) is not ported");
    return true;
}

bool cSC4MyStreetSim::Shutdown() { return cSC4MySim::Shutdown(); }

bool cSC4MyStreetSim::Write(cIGZOStream& stream) const {
    cSC4MySim::Write(stream);
    return !stream.GetError();
}

bool cSC4MyStreetSim::Read(cIGZIStream& stream) {
    cSC4MySim::Read(stream);
    return !stream.GetError();
}

}  // namespace sc4
