#include "audio/HitPropertySetResource.h"

#include <cstdio>
#include <cstring>

#include "gz/cGZAny.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "rz/RZConvert.h"
#include "rz/cRZString.h"

namespace {

constexpr std::uint32_t kTypePropertySetXml = 0xebfee345, kTypePropertySetBinary = 0xebfee349;
constexpr std::uint32_t kTypeTrackLegacy = 0x0b8d821a, kTypeTrackXml = 0x0b9eb87e;
constexpr std::uint32_t kIidPersistResource = 0x3b93c5e7, kIidHitResource = 0x456b8f1d;
constexpr std::uint32_t kRegSelfInstance = 0xff6f9546;
constexpr std::uint32_t kLegacyMagic = 0x544b4432;  // text header tag of the legacy track format

// The whole record: a database record's stream is asked for its record view (iid 0xc56b8f08) as the original does
// (size +0x1a0, bytes +0xd8, like cHitList::ReadResource); a port memory stream reports what is left instead.
bool ReadAll(cIGZIStream& stream, std::vector<std::uint8_t>& out) {
    cIGZPersistDBSerialRecord* record = nullptr;
    if (stream.QueryInterface(kGZIID_cIGZPersistDBSerialRecord, reinterpret_cast<void**>(&record)) && record) {
        out.assign(record->GetSize(), 0);
        const bool ok = out.empty() || record->GetFieldVoid(out.data(), static_cast<std::uint32_t>(out.size()));
        record->Release();
        return ok;
    }
    auto* sized = dynamic_cast<cIStreamRemaining*>(&stream);
    if (sized == nullptr) return false;
    out.resize(sized->RemainingBytes());
    return out.empty() || stream.GetVoid(out.data(), static_cast<std::uint32_t>(out.size()));
}

// Register ids in the order the legacy track text stores them (tokens separated by ',').
struct LegacyField {
    std::uint32_t id;
    bool hex;       // written as 0x%08lx instead of %ld
};
constexpr LegacyField kLegacyFields[] = {
    {0xffaec905, true},  {0xff5f15e1, true},  {0xff4dcd0d, false}, {0xff050a94, false}, {0xffb0333d, false},
    {0xff706616, false}, {0xffcc156f, false}, {0xffcc3b29, false}, {0xff891eaa, true},  {0xffae9ddb, false},
    {0xfff7b8df, false}, {0xff90aac8, false}, {0xff3c2160, true},  {0xffd7ee4d, true},  {0xff59fdd8, true},
    {0xff2d7cd1, true},  {0xff0246fe, false}, {0xffff4e79, false}, {0xff595ffd, false}, {0xff8dcfd8, false},
    {0xff69510b, false}, {0xff099db2, false}, {0xff53dbf3, false}, {0xff3ec880, false}, {0xff47f685, false},
    {0xffcaa344, false}, {0xff303b1b, false},
};

}  // namespace

template <bool kTrack>
std::int32_t cHitPropertySetResourceT<kTrack>::refCountValue() const {
    // The count lives in cGZPropertySet (+8 in the original); cHitPropertySetResource::GetRefCount
    // returns exactly that field.
    return refCount();
}

template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kIidPersistResource || iid == kIidHitResource) {
        *out = static_cast<cIHitPersistResource*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZPropertySet) {
        *out = static_cast<cIGZPropertySet*>(this);
        AddRef();
        return true;
    }
    return false;
}

// ReadBytes: what Read() does once the record has been loaded.
template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::ReadBytes(const std::uint8_t* data, std::size_t size) {
    if constexpr (kTrack) {
        SetValue(kRegSelfInstance, AnyUint32(key_.instance));
        if (key_.type == kTypeTrackLegacy) return ReadLegacyFormat(data, size);
        return PropertySetIO::ReadAuto(data, size, *this);
    } else {
        if (key_.type == kTypePropertySetXml || key_.type == kTypePropertySetBinary) {
            return PropertySetIO::ReadAuto(data, size, *this);
        }
        return false;
    }
}

template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::Read(cIGZIStream& stream, std::uint32_t, cIGZUnknown*) {
    std::vector<std::uint8_t> bytes;
    if (!ReadAll(stream, bytes)) return false;
    return ReadBytes(bytes.data(), bytes.size());
}

template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::Write(cIGZOStream& stream, std::uint32_t, cIGZUnknown*) {
    if constexpr (kTrack) {
        if (key_.type == kTypeTrackLegacy) return WriteOldFormat(stream);
        if (key_.type == kTypeTrackXml) return WriteXmlFormat(stream);
        return false;
    } else {
        if (key_.type == kTypePropertySetXml) return PropertySetIO::WriteXml(stream, *this);
        if (key_.type == kTypePropertySetBinary) return PropertySetIO::WriteBinary(stream, *this);
        return false;
    }
}

// CreatePrivateCopy: a new resource with the same key and contents (the copy also inherits the
// source's reference count through cGZPropertySetT::operator=, as in the binary).
template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::CreatePrivateCopy(std::uint32_t iid, void** out) {
    auto* copy = new cHitPropertySetResourceT<kTrack>(key_);
    static_cast<cGZPropertySet&>(*copy) = static_cast<const cGZPropertySet&>(*this);
    copy->key_ = key_;
    return copy->QueryInterface(iid, out);
}

// ReadLegacyFormat @ 0x10046ff0c — see the field order in kLegacyFields; the on-disk text is
//   [u32 length prefix] u32 'TKD2'-tag ... text "tag,version,<ignored>,soundId,entry,...,"
// Tokens are comma separated; empty tokens leave the register unset. Returns false when the
// sound id equals this resource's own instance (self reference).
template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::ReadLegacyFormat(const std::uint8_t* data, std::size_t size) {
    if constexpr (!kTrack) {
        (void)data; (void)size;
        return false;
    } else {
        auto u32 = [&](std::size_t at) {
            std::uint32_t v = 0;
            if (at + 4 <= size) std::memcpy(&v, data + at, 4);
            return v;
        };
        std::size_t pos = 4;
        std::uint32_t magic = u32(0);
        std::size_t length = size;
        if (size >= 4 && u32(0) == size - 4) {  // optional length prefix
            magic = u32(4);
            pos = 8;
            length = u32(0);
        }
        std::string raw;
        if (length >= 4 && pos <= size) {
            const std::size_t n = std::min<std::size_t>(length - 4, size - pos);
            raw.assign(reinterpret_cast<const char*>(data + pos), n);
        }
        std::size_t textOffset = 0;
        if (magic == kLegacyMagic) textOffset = 8;
        std::string text = textOffset <= raw.size() ? raw.substr(textOffset) : std::string();
        if (const auto nul = text.find('\0'); nul != std::string::npos) text.resize(nul);

        cRZString rest(text.c_str());
        auto next = [&rest]() { return rest.SplitToken(","); };
        next();                                                    // tag
        const std::uint32_t version = RZ::ConvertToUint(next().ToChar());
        next();                                                    // skipped token
        auto setIfPresent = [this](std::uint32_t id, const cRZString& token) {
            if (token.Strlen() != 0) SetValue(id, AnyUint32(RZ::ConvertToUint(token.ToChar())));
        };

        cRZString token = next();                                  // sound id
        if (token.Strlen() != 0) {
            const std::uint32_t soundId = RZ::ConvertToUint(token.ToChar());
            if (key_.instance == soundId) return false;
            SetValue(0xffaec905, AnyUint32(soundId));
        }
        for (std::uint32_t id : {0xff5f15e1u, 0xff4dcd0du, 0xff050a94u, 0xffb0333du}) setIfPresent(id, next());
        if (version != 0) setIfPresent(0xff706616, next());
        std::uint32_t use3D = 0;
        token = next();
        if (token.Strlen() != 0) {
            use3D = RZ::ConvertToUint(token.ToChar());
            SetValue(0xffcc156f, AnyUint32(use3D));
        }
        setIfPresent(0xffcc3b29, next());
        std::uint32_t semId = use3D != 0 ? 0x7dcded3d : 0x7dcded2d;
        token = next();
        if (token.Strlen() != 0) {
            if (const std::uint32_t v = RZ::ConvertToUint(token.ToChar()); v != 0) semId = v;
        }
        SetValue(0xff891eaa, AnyUint32(semId));
        for (std::uint32_t id : {0xffae9ddbu, 0xfff7b8dfu, 0xff90aac8u, 0xff3c2160u, 0xffd7ee4du, 0xff59fdd8u,
                                 0xff2d7cd1u, 0xff0246feu, 0xffff4e79u, 0xff595ffdu, 0xff8dcfd8u, 0xff69510bu}) {
            setIfPresent(id, next());
        }
        if (version > 1) {
            for (std::uint32_t id : {0xff099db2u, 0xff53dbf3u, 0xff3ec880u, 0xff47f685u, 0xffcaa344u, 0xff303b1bu}) {
                setIfPresent(id, next());
            }
        }
        return true;
    }
}

// WriteOldFormat @ 0x1004711c8: "TKDT,2," + one comma-terminated token per register (empty when
// unset or not an AnyUint32) + "ETKD".
template <bool kTrack>
bool cHitPropertySetResourceT<kTrack>::WriteOldFormat(cIGZOStream& out) {
    if constexpr (!kTrack) {
        (void)out;
        return false;
    } else {
        std::string text = "TKDT,2,";
        for (const LegacyField& field : kLegacyFields) {
            AnyUint32 any;
            if (!Value(field.id, &any)) {
                text += ",";
                continue;
            }
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, field.hex ? "0x%08lx," : "%ld,", static_cast<unsigned long>(any.get()));
            text += buffer;
        }
        text += "ETKD";
        out.SetVoid(text.data(), static_cast<std::uint32_t>(text.size()));
        return true;
    }
}

template class cHitPropertySetResourceT<false>;
template class cHitPropertySetResourceT<true>;
