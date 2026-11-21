// The binary-record-to-file readers of DBSegment.h (kept apart from DBSegment.cpp: they need the file class and the
// resource manager, which the stream helpers' users do not link).
#include "sc4/DBSegment.h"

#include <vector>

#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZPersistResourceManager.h"
#include "rz/cRZFile.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {
std::uint32_t SC4ReadBinaryRecord(cIGZPersistDBSegment* segment, const cGZPersistResourceKey& key, const cIGZString& path) {
    std::uint32_t size = 0xffffffffu;
    cRZFile file(path);
    const bool existed = cRZFile::FileExists(path);
    if (!file.Open(2, 3, 1)) return size;
    size = segment->GetRecordSize(key);
    if (size != 0xffffffffu) {
        std::vector<std::uint8_t> buffer(size);
        size = segment->ReadRecord(key, buffer.data(), size);
        if (size != 0xffffffffu) {
            file.SeekToBegin();
            file.SetLength(0);
            size = file.Write(buffer.data(), size) ? size : 0xffffffffu;
        }
    }
    file.Close();
    if (size == 0xffffffffu && !existed) cRZFile::Remove(path);
    return size;
}

std::uint32_t SC4ReadBinaryRecord(const cGZPersistResourceKey& key, const cIGZString& path) {
    cIGZPersistDBSegment* segment = nullptr;
    std::uint32_t result = 0xffffffffu;
    if (SL::spRM && SL::spRM->FindDBSegment(key, &segment)) result = SC4ReadBinaryRecord(segment, key, path);
    if (segment) segment->Release();
    return result;
}


}  // namespace sc4
