#include "app/NativeLotSerial.h"

#include <unordered_map>

#include "rz/cRZAutoRefCount.h"
#include "sc4/Lot.h"
#include "tools/city_probe_plain.h"

namespace native {
namespace {
std::unordered_map<sc4::cSC4Lot*, cRZAutoRefCount<cIGZUnknown>>& Table() {
    static auto* table = new std::unordered_map<sc4::cSC4Lot*, cRZAutoRefCount<cIGZUnknown>>();   // never destroyed (exit order)
    return *table;
}
}  // namespace

cIGZUnknown* LotSerializable(sc4::cSC4Lot* lot) {
    if (!lot) return nullptr;
    auto& slot = Table()[lot];
    if (!slot.get()) {
        cIGZSerializable* adapter = sc4::WrapPlain(lot);   // takes a reference to the lot; one reference to the adapter
        slot = adapter;
        adapter->Release();
    }
    return slot.get();
}

void RememberLotSerializable(sc4::cSC4Lot* lot, cIGZUnknown* adapter) {
    if (lot && adapter) Table()[lot] = adapter;
}

void ForgetLotSerializables() { Table().clear(); }

}  // namespace native
