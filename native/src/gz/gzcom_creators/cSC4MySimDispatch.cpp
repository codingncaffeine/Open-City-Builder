// GZCOM creator for sc4::cSC4MySimDispatch: a save reads each MySim's dispatch back through the serializer by class id
// (cSC4MySim::Read -> SC4ReadGZSerializable(0xeb8b6f9a)); the owner is set from the stream by cSC4MySimDispatch::Read.
#include "gz/GZCOM.h"
#include "sc4/MySimDispatch.h"

namespace gzcom {

void* GZCOM_CreatecSC4MySimDispatch() {
    auto* dispatch = new sc4::cSC4MySimDispatch();
    return static_cast<void*>(static_cast<cIGZUnknown*>(static_cast<sc4::IMySimDispatch*>(dispatch)));
}

}  // namespace gzcom
