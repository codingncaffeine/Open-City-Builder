#include "sc4/ResourceAsFile.h"

#include "gz/GZCOMDirector.h"
#include "gz/cGZDBSerialRecordAsIGZFile.h"
#include "rz/cRZCOMDllDirector.h"
#include "gz/cIGZPersistResourceManager.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

// The record comes from the resource manager (vslot 0x94); its serial view (iid 0xc56b8f08) goes to a new adapter (GZCOM class
// 0x0a32c0da, slot 3) whose cIGZFile face is the result. On a failure after the record opened, the record is closed again.
bool SC4GetResourceAsFile(const cGZPersistResourceKey& key, cIGZFile** out, bool write) {
    cIGZPersistDBRecord* record = nullptr;
    bool ok = false;
    if (SL::spRM && SL::spRM->OpenDBRecord(key, &record, write) && record) {
        cIGZPersistDBSerialRecord* serial = nullptr;
        if (record->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, reinterpret_cast<void**>(&serial)) && serial) {
            cIGZDBSerialRecordAsIGZFile* adapter = nullptr;
            cIGZCOM* com = GZCOM();
            if (com && com->CreateInstance(kGZCLSID_cGZDBSerialRecordAsIGZFile, kGZIID_cIGZDBSerialRecordAsIGZFile,
                                           reinterpret_cast<void**>(&adapter)) && adapter &&
                adapter->QueryInterface(kGZIID_cIGZFile, reinterpret_cast<void**>(out))) {
                adapter->SetDBSerialRecord(serial);
                ok = true;
            }
            if (adapter) adapter->Release();
        }
        if (!ok) record->Close();
        if (serial) serial->Release();
    }
    if (!ok) *out = nullptr;
    if (record) record->Release();
    return ok;
}

}  // namespace sc4
