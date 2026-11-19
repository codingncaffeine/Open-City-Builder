#include "gz/cGZDBSerialRecordAsIGZFile.h"

namespace gz {

cIGZUnknown* CreateGZDBSerialRecordAsIGZFile() { return static_cast<cIGZDBSerialRecordAsIGZFile*>(new cGZDBSerialRecordAsIGZFile()); }

}  // namespace gz
