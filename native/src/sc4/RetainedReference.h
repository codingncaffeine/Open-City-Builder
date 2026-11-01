// A serialized object reference that a city file carries and the port does not model yet (a simulator's grids, the
// occupants of its producers, its pipes): the object is kept as it was read and written back unchanged, so a saved city
// stays in step with the readers and keeps the object (the class record of the referenced object is written by the
// serializer as soon as the reference is).
#pragma once
#include <cstdint>
#include <vector>

#include "gz/cIGZUnknown.h"
#include "rz/cRZAutoRefCount.h"
#include "sc4/DBSegment.h"

namespace sc4 {

class cRetainedReference {
public:
    // SC4ReadGZSerializable(stream, cIGZUnknown, ...): the id (and class id) are consumed; a null reference keeps nothing
    bool Read(cIGZIStream& stream) {
        void* raw = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, 1, &raw, false);
        object_ = static_cast<cIGZUnknown*>(raw);
        if (raw) static_cast<cIGZUnknown*>(raw)->Release();   // the read's own reference (object_ holds one)
        return ok;
    }
    bool Write(cIGZOStream& stream) const { return SC4WriteGZSerializable(stream, object_.get()); }
    cIGZUnknown* get() const { return object_.get(); }

private:
    cRZAutoRefCount<cIGZUnknown> object_;
};

using cRetainedReferences = std::vector<cRetainedReference>;

}  // namespace sc4
