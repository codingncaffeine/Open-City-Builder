// The serializable identity of a lot. cSC4Lot is plain C++ in the port, so a city file reaches it through an adapter
// (tools/city_probe_plain.h); every reference a save writes to one lot must go through the same adapter, or the file
// would hold the lot twice. The lots read from a file keep the adapter they came in; a lot made since gets one on
// its first save. The table holds a reference to each adapter (and so each lot) until the next city is opened.
#pragma once
#include "gz/cIGZUnknown.h"

namespace sc4 { class cSC4Lot; }

namespace native {

cIGZUnknown* LotSerializable(sc4::cSC4Lot* lot);                        // borrowed; null for a null lot
void RememberLotSerializable(sc4::cSC4Lot* lot, cIGZUnknown* adapter);  // the adapter a city file's lot came in
void ForgetLotSerializables();                                          // a city is being opened

}  // namespace native
