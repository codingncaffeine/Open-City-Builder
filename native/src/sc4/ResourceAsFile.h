// SC4GetResourceAsFile (Mac i386 0x00024aea, x86_64 0x10003a3d2): a resource of the persistence layer opened as a file.
#pragma once

#include "gz/cGZPersistResourceKey.h"

class cIGZFile;

namespace sc4 {

// The record `key` of SL::spRM (opened for writing when `write`), served by a cGZDBSerialRecordAsIGZFile: *out holds one reference
// and closing the file closes the record. False (and *out null) when the record or the adapter is not there.
bool SC4GetResourceAsFile(const cGZPersistResourceKey& key, cIGZFile** out, bool write);

}  // namespace sc4
