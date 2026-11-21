#include "sc4/SLCursor.h"

#include "gz/Cursor.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/cRZFile.h"
#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

// cSLCursor::Load: only a record some segment holds is read; the temporary file is removed again either way.
cIGZCursor* LoadSLCursor(const cGZPersistResourceKey& requested) {
    cGZPersistResourceKey key = requested;
    if (key.type == 0) key.type = kCursorResourceType;
    if (!SL::spRM) return nullptr;
    cIGZPersistDBSegment* segment = nullptr;
    gz::cRZCursor* cursor = nullptr;
    if (SL::spRM->FindDBSegment(key, &segment)) {
        const cRZString extension(".cur");
        cRZString path;
        if (cRZFile::MakeTempPathName(path, &extension)) {
            if (SC4ReadBinaryRecord(key, path) != 0xffffffffu) {
                cursor = new gz::cRZCursor(path);
                if (cursor->IsLoaded()) {
                    cursor->AddRef();
                } else {
                    delete cursor;
                    cursor = nullptr;
                }
            }
            cRZFile::Remove(path);
        }
    }
    if (segment) segment->Release();
    return cursor;
}

}  // namespace sc4
