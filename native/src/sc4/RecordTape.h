// The layout of a city-file record whose model the port only partly recovers. The reader of the record reads through
// the tape: every value and every object reference is recorded in order, and the values the port keeps are bound to
// their members. Saving replays the tape: bound values are written from the members as they are now, the others as
// they were read, references are written again (with the ids of this save) and the counts in front of lists stay
// those of the file. A record read this way therefore saves byte-compatible with its reader.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/RetainedReference.h"

namespace sc4 {

class cRecordTape {
public:
    void Clear() { items_.clear(); }
    bool Empty() const { return items_.empty(); }

    // A value as read; `live` binds it to the member it was read into (written from there on save).
    std::uint8_t U8(cIGZIStream& s, std::uint8_t* live = nullptr) { std::uint8_t v = 0; s.GetUint8(v); Put(kU8, &v, live); if (live) *live = v; return v; }
    std::int8_t S8(cIGZIStream& s, std::int8_t* live = nullptr) { std::int8_t v = 0; s.GetSint8(v); Put(kS8, &v, live); if (live) *live = v; return v; }
    std::int16_t S16(cIGZIStream& s, std::int16_t* live = nullptr) { std::int16_t v = 0; s.GetSint16(v); Put(kS16, &v, live); if (live) *live = v; return v; }
    std::uint16_t U16(cIGZIStream& s, std::uint16_t* live = nullptr) { std::uint16_t v = 0; s.GetUint16(v); Put(kU16, &v, live); if (live) *live = v; return v; }
    std::uint32_t U32(cIGZIStream& s, std::uint32_t* live = nullptr) { std::uint32_t v = 0; s.GetUint32(v); Put(kU32, &v, live); if (live) *live = v; return v; }
    std::int32_t S32(cIGZIStream& s, std::int32_t* live = nullptr) { std::int32_t v = 0; s.GetSint32(v); Put(kS32, &v, live); if (live) *live = v; return v; }
    std::uint64_t U64(cIGZIStream& s, std::uint64_t* live = nullptr) { std::uint64_t v = 0; s.GetUint64(v); Put(kU64, &v, live); if (live) *live = v; return v; }
    std::int64_t S64(cIGZIStream& s, std::int64_t* live = nullptr) { std::int64_t v = 0; s.GetSint64(v); Put(kS64, &v, live); if (live) *live = v; return v; }
    float F32(cIGZIStream& s, float* live = nullptr) { float v = 0; s.GetFloat32(v); Put(kF32, &v, live); if (live) *live = v; return v; }
    double F64(cIGZIStream& s, double* live = nullptr) { double v = 0; s.GetFloat64(v); Put(kF64, &v, live); if (live) *live = v; return v; }
    // a bool stored as one byte
    bool Bool(cIGZIStream& s, bool* live = nullptr) {
        std::uint8_t v = 0;
        s.GetUint8(v);
        Item item{kBool, 0, live, {}};
        std::memcpy(&item.bits, &v, 1);
        items_.push_back(std::move(item));
        if (live) *live = v != 0;
        return v != 0;
    }
    // The stream's own getters, recorded by value (the reader keeps the value in a local, not bound to a member).
    void GetUint8(cIGZIStream& s, std::uint8_t& v) { v = U8(s); }
    void GetSint8(cIGZIStream& s, std::int8_t& v) { v = S8(s); }
    void GetUint16(cIGZIStream& s, std::uint16_t& v) { v = U16(s); }
    void GetSint16(cIGZIStream& s, std::int16_t& v) { v = S16(s); }
    void GetUint32(cIGZIStream& s, std::uint32_t& v) { v = U32(s); }
    void GetSint32(cIGZIStream& s, std::int32_t& v) { v = S32(s); }
    void GetUint64(cIGZIStream& s, std::uint64_t& v) { v = U64(s); }
    void GetSint64(cIGZIStream& s, std::int64_t& v) { v = S64(s); }
    void GetFloat32(cIGZIStream& s, float& v) { v = F32(s); }
    void GetFloat64(cIGZIStream& s, double& v) { v = F64(s); }
    void GetBool(cIGZIStream& s, bool& v) { v = Bool(s); }   // operator>>(bool&)
    // operator>>(std::vector<std::uint8_t>&): a count, then that many bytes (none past the stream vector limit)
    void GetBytes(cIGZIStream& s, std::vector<std::uint8_t>& v, std::uint32_t limit = 0x10000000u) {
        const std::uint32_t count = U32(s);
        v.clear();
        if (count < limit) { v.resize(count); for (std::uint8_t& b : v) b = U8(s); }
    }
    // an object reference (SC4ReadGZSerializable), kept as read; the object is returned borrowed
    cIGZUnknown* Ref(cIGZIStream& s) {
        Item item{kRef, 0, nullptr, {}};
        item.ref.Read(s);
        items_.push_back(std::move(item));
        return items_.back().ref.get();
    }

    bool Replay(cIGZOStream& s) const {
        for (const Item& item : items_) {
            const void* from = item.live ? item.live : &item.bits;
            switch (item.kind) {
                case kU8: s.SetUint8(Load<std::uint8_t>(from)); break;
                case kS8: s.SetSint8(Load<std::int8_t>(from)); break;
                case kS16: s.SetSint16(Load<std::int16_t>(from)); break;
                case kU16: s.SetUint16(Load<std::uint16_t>(from)); break;
                case kU32: s.SetUint32(Load<std::uint32_t>(from)); break;
                case kS32: s.SetSint32(Load<std::int32_t>(from)); break;
                case kU64: s.SetUint64(Load<std::uint64_t>(from)); break;
                case kS64: s.SetSint64(Load<std::int64_t>(from)); break;
                case kF32: s.SetFloat32(Load<float>(from)); break;
                case kF64: s.SetFloat64(Load<double>(from)); break;
                case kBool: s.SetUint8(item.live ? (*static_cast<const bool*>(item.live) ? 1 : 0) : Load<std::uint8_t>(from)); break;
                case kRef: item.ref.Write(s); break;
            }
        }
        return !s.GetError();
    }

private:
    enum Kind : std::uint8_t { kU8, kS8, kS16, kU16, kU32, kS32, kU64, kS64, kF32, kF64, kBool, kRef };
    struct Item {
        Kind kind;
        std::uint64_t bits;
        void* live;
        cRetainedReference ref;
    };
    template <class T> static T Load(const void* p) { T v; std::memcpy(&v, p, sizeof v); return v; }
    template <class T> void Put(Kind kind, const T* value, T* live) {
        Item item{kind, 0, live, {}};
        std::memcpy(&item.bits, value, sizeof(T));
        items_.push_back(std::move(item));
    }
    std::vector<Item> items_;
};

}  // namespace sc4
