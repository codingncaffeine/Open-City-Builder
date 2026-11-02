#include <cstdio>
#include <cstdlib>

#include "sc4/DBSegment.h"

#include <algorithm>

#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "rz/cRZVariant.h"
#include "rz/nRZHash.h"

namespace sc4 {

namespace {
ISerializableFactory* g_factory = nullptr;
cIGZMessageServer2* g_messageServer = nullptr;
cIGZPersistResourceManager* g_resourceManager = nullptr;
constexpr std::uint32_t kMaxCrcSpan = 250000;
constexpr std::uint32_t kMaxObjectsPerClass = 10000000;
constexpr std::uint32_t kMaxCrcBuffer = 0x3d091;   // GetCRC refuses spans at or above this
}  // namespace

ISerializableFactory* SerializableFactory() { return g_factory; }
void SetSerializableFactory(ISerializableFactory* f) { g_factory = f; }
cIGZMessageServer2* DBSegmentMessageServer() { return g_messageServer; }
void SetDBSegmentMessageServer(cIGZMessageServer2* s) { g_messageServer = s; }
cIGZPersistResourceManager* PersistResourceManagerService() { return g_resourceManager; }
void SetPersistResourceManagerService(cIGZPersistResourceManager* m) { g_resourceManager = m; }

// =============================================================================================
// cSC4DBSegmentIStream
// =============================================================================================
cSC4DBSegmentIStream::~cSC4DBSegmentIStream() = default;

bool cSC4DBSegmentIStream::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DBSegmentIStreamAlt || iid == kGZIID_cISC4DBSegmentIStream) {
        *out = static_cast<cISC4DBSegmentIStream*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cIGZUnknown) {
        *out = static_cast<cIGZUnknown*>(static_cast<cIGZIStream*>(this));
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4DBSegmentIStream::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4DBSegmentIStream::Open(cISC4DBSegment* segment, const cGZPersistResourceKey& key) {
    if (!Close()) return false;
    if (segment) {
        segment_ = segment;
        cIGZPersistDBSegment* persist = segment_->AsIGZPersistDBSegment();
        cIGZPersistDBRecord* raw = nullptr;
        if (persist->OpenRecord(key, &raw, 1)) {
            record_ = nullptr;
            void* serial = nullptr;
            if (raw->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &serial)) {
                // the QueryInterface reference becomes the stream's own
                cIGZPersistDBSerialRecord* r = static_cast<cIGZPersistDBSerialRecord*>(serial);
                record_ = r;
                r->Release();
            } else {
                persist->CloseRecord(raw);
            }
        }
        if (raw) raw->Release();
    }
    ok_ = record_.get() != nullptr;
    return ok_;
}

bool cSC4DBSegmentIStream::Close() {
    if (!segment_ || !record_) return ok_;
    cIGZPersistDBSegment* persist = segment_->AsIGZPersistDBSegment();
    if (!persist->CloseRecord(record_->AsIGZPersistDBRecord())) {
        ok_ = false;
        return false;
    }
    record_ = nullptr;
    segment_ = nullptr;
    return true;
}

bool cSC4DBSegmentIStream::ReadGZSerializable(cIGZSerializable** out, bool loadNow) {
    cISC4COMSerializer* serializer = segment_ ? segment_->GetCOMSerializer() : nullptr;
    if (!serializer) {
        ok_ = false;
        return false;
    }
    ok_ = ok_ && serializer->ReadGZSerializable(*this, out, loadNow);
    return ok_;
}

bool cSC4DBSegmentIStream::ReadResKey(cGZPersistResourceKey& key) {
    ok_ = ok_ && record_->GetFieldResKey(key);
    return ok_;
}

bool cSC4DBSegmentIStream::ReadVariant(cIGZVariant& variant) {
    std::uint32_t tag = 0;
    GetUint32(tag);
    if (tag == kVariantObjectTag) {
        cIGZSerializable* object = nullptr;
        if (!ReadGZSerializable(&object, false)) {
            ok_ = false;
            return false;
        }
        if (!object) return true;
        variant.SetValUnknown(object);
        object->Release();
        return true;
    }
    cIGZVariant* value = nullptr;
    if (!record_ || !record_->GetFieldVariant(&value)) {
        ok_ = false;
        return false;
    }
    variant.CopyFrom(*value);
    value->Release();
    return true;
}

bool cSC4DBSegmentIStream::Skip(std::uint32_t byteCount) {
    unsigned char scratch[0x400];
    while (byteCount >= 0x400) {
        GetVoid(scratch, 0x400);
        byteCount -= 0x400;
    }
    while (byteCount--) {
        std::int8_t b;
        GetSint8(b);
    }
    return ok_;
}

bool cSC4DBSegmentIStream::GetRZCharStr(char* buffer, std::uint32_t maxLength) {
    cRZString text;
    bool ok = ok_ && record_->GetFieldString(text);
    ok_ = ok;
    const char* chars = text.Data();
    const std::uint32_t n = std::min<std::uint32_t>(static_cast<std::uint32_t>(std::strlen(chars)) + 1, maxLength);
    for (std::uint32_t i = 0; i < n; ++i) buffer[i] = chars[i];
    return ok_;
}

bool cSC4DBSegmentIStream::GetGZStr(cIGZString& v) {
    ok_ = ok_ && record_->GetFieldString(v);
    return ok_;
}

bool cSC4DBSegmentIStream::GetGZSerializable(cIGZSerializable& object) {
    ok_ = ok_ && object.Read(*this);
    return ok_;
}

bool cSC4DBSegmentIStream::GetVoid(void* destination, std::uint32_t byteCount) {
    ok_ = ok_ && record_->GetFieldVoid(destination, byteCount);
    return ok_;
}

// =============================================================================================
// cSC4DBSegmentOStream
// =============================================================================================
cSC4DBSegmentOStream::~cSC4DBSegmentOStream() = default;

bool cSC4DBSegmentOStream::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DBSegmentOStreamAlt || iid == kGZIID_cISC4DBSegmentOStream) {
        *out = static_cast<cISC4DBSegmentOStream*>(this);
        AddRef();
        return true;
    }
    if (iid == kGZIID_cIGZUnknown) {
        *out = static_cast<cIGZUnknown*>(static_cast<cIGZOStream*>(this));
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4DBSegmentOStream::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

bool cSC4DBSegmentOStream::Open(cISC4DBSegment* segment, const cGZPersistResourceKey& key, bool fromStart) {
    if (!Close()) return false;
    if (segment) {
        segment_ = segment;
        cIGZPersistDBSegment* persist = segment_->AsIGZPersistDBSegment();
        cIGZPersistDBRecord* raw = nullptr;
        if (!persist->OpenRecord(key, &raw, 2)) {
            if (raw) {
                cIGZPersistDBRecord* stale = raw;
                raw = nullptr;
                stale->Release();
            }
            persist->CreateNewRecord(key, &raw);
        }
        if (!raw) return false;
        record_ = nullptr;
        void* serial = nullptr;
        if (!raw->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &serial)) {
            persist->CloseRecord(raw);
            raw->Release();
            return false;
        }
        cIGZPersistDBSerialRecord* r = static_cast<cIGZPersistDBSerialRecord*>(serial);
        record_ = r;
        r->Release();
        // appending (the default) continues at the end of an existing record
        record_->SeekAbsolute(fromStart ? 0 : record_->GetSize());
        raw->Release();
    }
    ok_ = record_.get() != nullptr;
    if (!ok_ && segment_) segment_ = nullptr;
    return ok_;
}

bool cSC4DBSegmentOStream::Close() {
    if (!segment_ || !record_) return ok_;
    cIGZPersistDBSegment* persist = segment_->AsIGZPersistDBSegment();
    if (!persist->CloseRecord(record_->AsIGZPersistDBRecord())) {
        ok_ = false;
        return false;
    }
    record_ = nullptr;
    segment_ = nullptr;
    return true;
}

bool cSC4DBSegmentOStream::WriteGZSerializable(const cIGZSerializable* object) {
    if (ok_) {
        cISC4COMSerializer* serializer = segment_->GetCOMSerializer();
        ok_ = serializer->WriteGZSerializable(*this, object);
    }
    return ok_;
}

bool cSC4DBSegmentOStream::WriteResKey(const cGZPersistResourceKey& key) {
    ok_ = ok_ && record_->SetFieldResKey(key);
    return ok_;
}

bool cSC4DBSegmentOStream::WriteVariant(const cIGZVariant& variant) {
    const std::uint16_t type = variant.GetType();
    const bool isObject = type == cIGZVariant::kTypeUnknown;
    bool ok = SetUint32(isObject ? kVariantObjectTag : 0);
    if (ok) {
        if (isObject) {
            void* raw = nullptr;
            if (const_cast<cIGZVariant&>(variant).GetValUnknown(kGZIID_cIGZSerializable, &raw)) {
                auto* object = static_cast<cIGZSerializable*>(raw);
                ok = WriteGZSerializable(object);
                object->Release();
            } else {
                ok = WriteGZSerializable(nullptr);
            }
        } else {
            ok = record_->SetFieldVariant(&variant);
        }
        if (ok) return true;
    }
    ok_ = false;
    return false;
}

bool cSC4DBSegmentOStream::SetRZCharStr(const char* value) {
    cRZString text(value);
    return SetGZStr(text);
}

bool cSC4DBSegmentOStream::SetGZStr(const cIGZString& value) {
    ok_ = ok_ && record_->SetFieldString(value);
    return ok_;
}

bool cSC4DBSegmentOStream::SetGZSerializable(const cIGZSerializable& object) {
    ok_ = ok_ && object.Write(*this);
    return ok_;
}

bool cSC4DBSegmentOStream::SetVoid(const void* source, std::uint32_t byteCount) {
    ok_ = ok_ && record_->SetFieldVoid(source, byteCount);
    return ok_;
}

// =============================================================================================
// cSC4COMSerializer
// =============================================================================================
cSC4COMSerializer::cSC4COMSerializer(ISerializableFactory* factory) : factory_(factory) { Reset(); }

cSC4COMSerializer::~cSC4COMSerializer() { Close(); }

bool cSC4COMSerializer::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4COMSerializer || iid == kGZIID_cIGZUnknown) {
        *out = static_cast<cISC4COMSerializer*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4COMSerializer::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cSC4COMSerializer::Reset() {
    collecting_ = false;
    pending_.clear();
    classes_.clear();
    totalObjects_ = 0;
    classesLoaded_ = false;
    saved_ = false;
    mode_ = 0;
    segment_ = nullptr;
}

bool cSC4COMSerializer::Open(cISC4DBSegment* segment, int mode) {
    if (segment_ && mode_ != 0) Close();
    if (!segment) return false;
    cIGZPersistDBSegment* persist = segment->AsIGZPersistDBSegment();
    const bool open = persist->IsOpen();
    if (mode == 0 || !open) return false;
    segment_ = segment;
    mode_ = mode;
    return true;
}

bool cSC4COMSerializer::Close() {
    bool result = true;
    if (mode_ == 2 && !saved_) result = SaveClassObjects();
    Reset();
    return result;
}

bool cSC4COMSerializer::GetCRC(cIGZPersistDBSerialRecord* record, std::uint32_t length, std::uint32_t& crc) {
    if (length >= kMaxCrcBuffer) return false;
    const std::uint32_t position = record->GetPosition();
    crcBuffer_.resize(static_cast<std::size_t>(length) + 1);
    const bool ok = record->GetFieldVoid(crcBuffer_.data(), length);
    if (ok) {
        crcBuffer_[length] = 0;
        crc = nRZHash::CRC32(crcBuffer_.data(), length);
    }
    record->SeekAbsolute(position);
    return ok;
}

bool cSC4COMSerializer::WriteGZSerializable(cIGZOStream& stream, const cIGZSerializable* object) {
    if (!segment_ || mode_ == 0) return false;
    const std::uint32_t classId = object ? object->GetGZCLSID() : 0;
    const std::uint64_t id = reinterpret_cast<std::uint64_t>(object);   // identity is the address
    stream.SetUint64(id);
    if (object) {
        stream.SetUint32(classId);
        OnSetGZSerializable(object, id, classId);
    }
    return !stream.GetError();
}

bool cSC4COMSerializer::OnSetGZSerializable(const cIGZSerializable* object, std::uint64_t id, std::uint32_t classId) {
    auto classIt = classes_.find(classId);
    if (classIt == classes_.end()) {
        // first object of the class: drop any stale class record so the save starts clean
        const cGZPersistResourceKey key{classId, kClassObjectGroup, 0};
        cIGZPersistDBSegment* persist = segment_->AsIGZPersistDBSegment();
        if (persist->TestForRecord(key)) persist->DeleteRecord(key);
        classIt = classes_.emplace(classId, cClassInfo{}).first;
    }
    cClassInfo& info = classIt->second;
    if (info.objects.find(id) != info.objects.end()) return true;   // already registered
    cRZAutoRefCount<cIGZSerializable>& slot = info.objects[id];
    ++info.actual;
    ++totalObjects_;
    slot = const_cast<cIGZSerializable*>(object);
    if (collecting_) pending_[classId].objects[id] = slot;
    return true;
}

bool cSC4COMSerializer::OnGetGZSerializable(cIGZSerializable** out, std::uint64_t id, std::uint32_t classId) {
    *out = nullptr;
    auto classIt = classes_.find(classId);
    if (classIt == classes_.end()) return false;
    cClassInfo& info = classIt->second;
    auto objectIt = info.objects.find(id);
    if (objectIt != info.objects.end()) {
        *out = objectIt->second.get();
        (*out)->AddRef();
        return true;
    }
    cIGZSerializable* created = factory_ ? factory_->CreateSerializable(classId) : nullptr;
    if (!created) return false;
    *out = created;
    info.objects[id] = created;       // the map holds its own reference
    ++info.actual;
    ++totalObjects_;
    return true;
}

bool cSC4COMSerializer::ReadGZSerializable(cIGZIStream& stream, cIGZSerializable** out, bool loadNow) {
    if (!segment_ || mode_ == 0) return false;
    std::uint64_t id = 0;
    cIGZVariant* userData = stream.GetUserData();
    // an unset version reads as "new" (the original dereferenced null here)
    const bool wide = !userData || userData->AsUint32() >= kWideIdVersion;
    if (!wide) {
        std::uint32_t narrow = 0;
        stream.GetUint32(narrow);
        id = narrow;
    } else {
        stream.GetUint64(id);
    }
    if (id == 0) {
        *out = nullptr;
    } else {
        std::uint32_t classId = 0;
        stream.GetUint32(classId);
        const bool ok = OnGetGZSerializable(out, id, classId);
        if (ok && loadNow) LoadSingleObject(*out, id, classId);
        if (!ok) return false;
    }
    return !stream.GetError();
}

bool cSC4COMSerializer::LoadSingleObject(cIGZSerializable* object, std::uint64_t id, std::uint32_t classId) {
    cClassInfo& info = classes_[classId];
    const cGZPersistResourceKey key{classId, kClassObjectGroup, 0};
    cISC4DBSegmentIStream* stream = nullptr;
    if (!segment_->OpenIStream(key, &stream)) return false;
    cIGZPersistDBSerialRecord* record = stream->GetRecord();
    bool ok = false;
    std::uint32_t remaining = info.expected;
    if (remaining - 1u < kMaxObjectsPerClass - 1u) {
        ok = true;
        for (; remaining != 0; --remaining) {
            const std::uint32_t position = record->GetPosition();
            std::uint32_t size = 0;
            stream->GetUint32(size);
            if (record->GetSize() + position < size) {
                ok = false;
                break;
            }
            std::uint32_t storedCrc = 0;
            stream->GetUint32(storedCrc);
            std::uint32_t span = size - 8;
            if (span > kMaxCrcSpan - 1) span = kMaxCrcSpan;
            std::uint32_t crc = 0;
            if (!GetCRC(record, span, crc) || storedCrc != crc) {
                record->SeekAbsolute(position + size);
                ok = false;
                break;
            }
            std::uint64_t entryId = 0;
            stream->GetUint64(entryId);
            if (entryId == id) {
                if (!object->Read(*stream)) {
                    ok = false;
                    break;
                }
                record->GetPosition();
                break;
            }
            record->SeekAbsolute(position + size);
        }
    }
    const bool closed = segment_->CloseIStream(stream);
    stream->Release();
    return ok && closed && info.expected == info.actual;
}

bool cSC4COMSerializer::LoadClassObjects(std::uint32_t version) {
    cRZVariant* versionVariant = new cRZVariant(version);
    versionVariant->AddRef();
    cRZAutoRefCount<cIGZVariant> versionRef(versionVariant);
    versionVariant->Release();
    if (mode_ != 1 || classesLoaded_) return false;

    bool result = false;
    cISC4DBSegmentIStream* table = nullptr;
    const cGZPersistResourceKey tableKey{kClassTableType, kClassObjectGroup, 0};
    if (segment_->OpenIStream(tableKey, &table)) {
        table->SetUserData(versionRef.get());
        std::uint32_t tableVersion = 0, classCount = 0, objectCount = 0;
        table->GetUint32(tableVersion);
        table->GetUint32(classCount);
        table->GetUint32(objectCount);
        if (tableVersion == 1) {
            for (std::uint32_t i = 0; i < classCount; ++i) {
                std::uint32_t classId = 0;
                table->GetUint32(classId);
                cClassInfo& info = classes_[classId];
                table->GetUint32(info.expected);
                info.actual = 0;
                if (std::getenv("SC4_CLASSES")) std::fprintf(stderr, "class table: %08x x%u\n", classId, info.expected);
            }
        }
        const bool tableError = table->GetError();
        const bool closed = segment_->CloseIStream(table);
        result = !tableError && tableVersion == 1 && closed;
        if (!result && std::getenv("SC4_DEBUG")) std::fprintf(stderr, "LoadClassObjects: class table failed (version %u error %d closed %d)\n", tableVersion, int(tableError), int(closed));
        table->Release();

        if (result) {
            const float classTotal = static_cast<float>(classes_.size());
            std::uint32_t index = 0;
            for (auto& [classId, info] : classes_) {
                cISC4DBSegmentIStream* stream = nullptr;
                const cGZPersistResourceKey key{classId, kClassObjectGroup, 0};
                if (segment_->OpenIStream(key, &stream)) {
                    stream->SetUserData(versionRef.get());
                    cIGZPersistDBSerialRecord* record = stream->GetRecord();
                    if (info.expected < kMaxObjectsPerClass) {
                        for (std::uint32_t n = info.expected; n != 0; --n) {
                            const std::uint32_t position = record->GetPosition();
                            std::uint32_t size = 0;
                            stream->GetUint32(size);
                            if (record->GetSize() + position < size) {
                                if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "LoadClassObjects: class %08x object size check failed\n", classId);
                                result = false;
                                break;
                            }
                            std::uint32_t storedCrc = 0;
                            stream->GetUint32(storedCrc);
                            std::uint32_t span = size - 8;
                            if (span > kMaxCrcSpan - 1) span = kMaxCrcSpan;
                            std::uint32_t crc = 0;
                            if (!GetCRC(record, span, crc) || storedCrc != crc) {
                                if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "LoadClassObjects: class %08x crc mismatch\n", classId);
                                record->SeekAbsolute(position + size);
                                result = false;
                                break;
                            }
                            std::uint64_t id = 0;
                            if (version < kWideIdVersion) {
                                std::uint32_t narrow = 0;
                                stream->GetUint32(narrow);
                                id = narrow;
                            } else {
                                stream->GetUint64(id);
                            }
                            cIGZSerializable* object = nullptr;
                            if (OnGetGZSerializable(&object, id, classId)) {
                                std::string peek;   // SC4_DUMPFAIL: the first payload bytes, taken before the read can put the stream in error
                                if (std::getenv("SC4_DUMPFAIL")) {
                                    const std::uint32_t at = record->GetPosition();
                                    char text[8];
                                    for (int k = 0; k < 96 && at + k < position + size; ++k) { std::uint8_t b = 0; stream->GetUint8(b); std::snprintf(text, sizeof text, " %02x", b); peek += text; }
                                    record->SeekAbsolute(at);
                                }
                                std::string whole;   // SC4_DUMPFULL: the whole payload, kept to write out when the read fails
                                if (std::getenv("SC4_DUMPFULL")) {
                                    const std::uint32_t at = record->GetPosition();
                                    for (std::uint32_t k = at; k < position + size; ++k) { std::uint8_t b = 0; stream->GetUint8(b); whole.push_back(char(b)); }
                                    record->SeekAbsolute(at);
                                }
                                const bool readOk = object->Read(*stream);
                                if (std::getenv("SC4_DEBUG") && (!readOk || stream->GetError() || record->GetPosition() != position + size))
                                    std::fprintf(stderr, "LoadClassObjects: class %08x object %u read %s, error %d, consumed %d of %u\n", classId, info.expected - n, readOk ? "ok" : "FAILED", int(stream->GetError()), int(record->GetPosition()) - int(position), size);
                                if (!whole.empty() && (!readOk || record->GetPosition() != position + size)) {
                                    const char* dir = std::getenv("SC4_DUMPFULL");   // a directory, or /tmp for any other value
                                    char name[512];
                                    std::snprintf(name, sizeof name, "%s/payload_%08x.bin", dir[0] == '/' ? dir : "/tmp", classId);
                                    if (FILE* f = std::fopen(name, "wb")) { std::fwrite(whole.data(), 1, whole.size(), f); std::fclose(f); }
                                }
                                if (!readOk && !peek.empty()) std::fprintf(stderr, "payload %08x:%s\n", classId, peek.c_str());
                                if (record->GetPosition() != position + size) record->SeekAbsolute(position + size);
                            } else {
                                record->SeekAbsolute(position + size);
                            }
                            if (object) object->Release();
                        }
                    } else {
                        result = false;
                    }
                    const bool classClosed = segment_->CloseIStream(stream);
                    if (info.expected != info.actual && std::getenv("SC4_DEBUG")) std::fprintf(stderr, "LoadClassObjects: class %08x expected %u objects, created %u\n", classId, info.expected, info.actual);
                    if (info.expected != info.actual) result = false;
                    if (!classClosed && std::getenv("SC4_DEBUG")) std::fprintf(stderr, "LoadClassObjects: class %08x stream close failed\n", classId);
                    if (!classClosed) result = false;
                    stream->Release();
                }
                segment_->SetPercentageCompletion(static_cast<float>(index) * 100.0f / classTotal);
                ++index;
            }
        }
    }
    classesLoaded_ = true;
    return result;
}

bool cSC4COMSerializer::SaveClassObjects() {
    if (mode_ != 2 || saved_) return true;
    if (!classesLoaded_ && classes_.empty()) return true;

    collecting_ = true;
    pending_.clear();
    for (auto& [classId, info] : classes_) pending_[classId] = info;   // every registered object is pending

    std::int32_t savedCount = 0;
    std::int32_t processed = 0;
    bool failed = false;
    while (processed != totalObjects_ && !failed) {
        const std::int32_t before = processed;
        for (auto& [classId, info] : pending_) {
            if (info.objects.empty()) continue;
            cISC4DBSegmentOStream* stream = nullptr;
            const cGZPersistResourceKey key{classId, kClassObjectGroup, 0};
            if (!segment_->OpenOStream(key, &stream, false)) {
                failed = true;
                break;
            }
            cIGZPersistDBSerialRecord* record = stream->GetRecord();
            // objects written below may register further objects into pending_ (and into this map)
            while (!info.objects.empty()) {
                auto it = info.objects.begin();
                const std::uint64_t id = it->first;
                cRZAutoRefCount<cIGZSerializable> object = it->second;
                const std::uint32_t start = record->GetPosition();
                stream->SetUint32(0);
                stream->SetUint32(0);
                stream->SetUint64(id);
                if (!object->Write(*stream)) {
                    record->SeekAbsolute(start);
                } else {
                    const std::uint32_t end = record->GetPosition();
                    record->SeekAbsolute(start);
                    const std::uint32_t size = end - start;
                    stream->SetUint32(size);
                    record->SeekAbsolute(start + 8);
                    std::uint32_t span = size - 8;
                    if (span > kMaxCrcSpan - 1) span = kMaxCrcSpan;
                    std::uint32_t crc = 0;
                    GetCRC(record, span, crc);
                    record->SeekAbsolute(start + 4);
                    stream->SetUint32(crc);
                    record->SeekAbsolute(end);
                    ++savedCount;
                }
                info.objects.erase(id);
                ++processed;
                if (stream->GetError()) {
                    segment_->CloseOStream(stream);
                    failed = true;
                    break;
                }
            }
            if (failed) {
                stream->Release();
                break;
            }
            const bool closed = segment_->CloseOStream(stream);
            stream->Release();
            if (!closed) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        segment_->SetPercentageCompletion(static_cast<float>(processed) * 100.0f / static_cast<float>(totalObjects_));
        if (processed == before) break;   // nothing left to write (deviation: the original could spin)
    }
    if (failed) return false;

    collecting_ = false;
    cISC4DBSegmentOStream* table = nullptr;
    const cGZPersistResourceKey tableKey{kClassTableType, kClassObjectGroup, 0};
    bool ok = segment_->OpenOStream(tableKey, &table, true);
    if (ok) {
        table->SetUint32(1);
        table->SetUint32(static_cast<std::uint32_t>(classes_.size()));
        table->SetSint32(savedCount);
        for (auto& [classId, info] : classes_) {
            table->SetUint32(classId);
            table->SetUint32(info.actual);
        }
        const bool closed = segment_->CloseOStream(table);
        ok = closed && !table->GetError();
        table->Release();
    }
    if (!ok) return false;
    saved_ = true;
    return true;
}

// =============================================================================================
// cSC4DBSegment
// =============================================================================================
cSC4DBSegment::cSC4DBSegment() {
    auto* serializer = new cSC4COMSerializer(SerializableFactory());
    serializer_ = serializer;
    messageServer_ = DBSegmentMessageServer();
}

cSC4DBSegment::~cSC4DBSegment() { Close(); }

bool cSC4DBSegment::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZPersistDBSegment) {
        *out = static_cast<cIGZPersistDBSegment*>(static_cast<cGZDBSegmentPackedFile*>(this));
    } else if (iid == kGZIID_cISC4DBSegment) {
        *out = static_cast<cISC4DBSegment*>(this);
    } else {
        return cGZDBSegmentPackedFile::QueryInterface(iid, out);
    }
    AddRef();
    return true;
}

bool cSC4DBSegment::OpenIStream(const cGZPersistResourceKey& key, cISC4DBSegmentIStream** out) {
    *out = nullptr;
    auto* stream = new cSC4DBSegmentIStream();
    stream->AddRef();
    bool ok = stream->Open(this, key);
    if (ok) ok = stream->QueryInterface(kGZIID_cISC4DBSegmentIStream, reinterpret_cast<void**>(out));
    stream->Release();
    return ok;
}

bool cSC4DBSegment::OpenOStream(const cGZPersistResourceKey& key, cISC4DBSegmentOStream** out, bool fromStart) {
    *out = nullptr;
    auto* stream = new cSC4DBSegmentOStream();
    stream->AddRef();
    bool ok = stream->Open(this, key, fromStart);
    if (ok) ok = stream->QueryInterface(kGZIID_cISC4DBSegmentOStream, reinterpret_cast<void**>(out));
    stream->Release();
    return ok;
}

bool cSC4DBSegment::LoadClassObjects(std::uint32_t version) {
    return serializer_ && serializer_->LoadClassObjects(version);
}

bool cSC4DBSegment::SaveClassObjects() { return serializer_ && serializer_->SaveClassObjects(); }

void cSC4DBSegment::SetPercentageCompletion(float percent) {
    float clamped = percent < 0.0f ? 0.0f : percent;
    if (clamped > 100.0f) clamped = 100.0f;
    const float previous = percent_;
    if (previous < clamped) percent_ = clamped;
    if (static_cast<int>(previous) / 2 < static_cast<int>(clamped) / 2) {
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(kSegmentProgressMessage);
        message->SetData1(static_cast<std::uint64_t>(static_cast<std::int64_t>(percent_)) & 0xffffffffu);
        if (messageServer_) messageServer_->MessageSend(message);
        message->Release();
    }
}

bool cSC4DBSegment::Init() { return cGZDBSegmentPackedFile::Init(); }

bool cSC4DBSegment::Shutdown() {
    Close();
    serializer_ = nullptr;
    messageServer_ = nullptr;
    return cGZDBSegmentPackedFile::Shutdown();
}

bool cSC4DBSegment::Open(bool read, bool write) {
    percent_ = 0.0f;
    bool ok = cGZDBSegmentPackedFile::Open(read, write);
    if (ok) ok = serializer_->Open(this, 2 - static_cast<int>(read));
    if (ok) return true;
    if (IsOpen()) Close();
    return false;
}

bool cSC4DBSegment::Close() {
    if (!IsOpen()) return true;
    const bool saved = serializer_->SaveClassObjects();
    const bool serializerClosed = serializer_->Close();
    const bool fileClosed = cGZDBSegmentPackedFile::Close();
    return fileClosed && serializerClosed && saved;
}

// =============================================================================================
// helpers
// =============================================================================================
bool SC4IsGZSerializable(cIGZUnknown* object) {
    void* out = nullptr;
    return object->QueryInterface(kGZIID_cIGZSerializable, &out);   // (reference intentionally not released: as original)
}

bool SC4WriteGZSerializable(cISC4DBSegmentOStream* stream, const cIGZUnknown* object) {
    cIGZSerializable* serializable = nullptr;
    if (object) {
        void* raw = nullptr;
        if (const_cast<cIGZUnknown*>(object)->QueryInterface(kGZIID_cIGZSerializable, &raw))
            serializable = static_cast<cIGZSerializable*>(raw);
    }
    const bool result = stream->WriteGZSerializable(serializable);
    if (serializable) serializable->Release();
    return result;
}

bool SC4WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw)) return false;
    auto* db = static_cast<cISC4DBSegmentOStream*>(raw);
    const bool result = SC4WriteGZSerializable(db, object);
    db->Release();
    return result;
}

bool SC4ReadGZSerializable(cISC4DBSegmentIStream& stream, std::uint32_t iid, void** out, bool loadNow) {
    if (!out) return false;
    *out = nullptr;
    cIGZSerializable* object = nullptr;
    if (!stream.ReadGZSerializable(&object, loadNow)) return false;
    bool ok = true;
    if (object) ok = object->QueryInterface(iid, out);
    if (object) object->Release();
    return ok;
}

bool SC4ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out, bool loadNow) {
    if (!out) return false;
    *out = nullptr;
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &raw)) return false;
    auto* db = static_cast<cISC4DBSegmentIStream*>(raw);
    const bool result = SC4ReadGZSerializable(*db, iid, out, loadNow);
    db->Release();
    return result;
}

bool SC4WriteGZPersistRef(cISC4DBSegmentOStream* stream, const cIGZUnknown* resource) {
    cIGZPersistResource* persist = nullptr;
    if (resource) {
        void* raw = nullptr;
        if (const_cast<cIGZUnknown*>(resource)->QueryInterface(kGZIID_cIGZPersistResource, &raw))
            persist = static_cast<cIGZPersistResource*>(raw);
    }
    bool result;
    if (persist) {
        const cGZPersistResourceKey key = persist->GetKey();
        stream->SetUint32(key.type);
        stream->SetUint32(key.group);
        stream->SetUint32(key.instance);
        result = !stream->GetError();
        persist->Release();
    } else {
        stream->SetUint32(0);
        stream->SetUint32(0);
        stream->SetUint32(0);
        result = false;
    }
    return result;
}

bool SC4WriteGZPersistRef(cIGZOStream& stream, const cIGZUnknown* resource) {
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &raw)) return false;
    auto* db = static_cast<cISC4DBSegmentOStream*>(raw);
    const bool result = SC4WriteGZPersistRef(db, resource);
    db->Release();
    return result;
}

bool SC4ReadGZPersistRef(cISC4DBSegmentIStream& stream, std::uint32_t iid, void** out) {
    cGZPersistResourceKey key;
    stream.GetUint32(key.type);
    stream.GetUint32(key.group);
    stream.GetUint32(key.instance);
    if (stream.GetError()) return false;
    cIGZPersistResourceManager* manager = PersistResourceManagerService();
    return manager && manager->GetResource(key, iid, out, 0, 0);
}

bool SC4ReadGZPersistRef(cIGZIStream& stream, std::uint32_t iid, void** out) {
    if (!out) return false;
    *out = nullptr;
    void* raw = nullptr;
    if (!stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &raw)) return false;
    auto* db = static_cast<cISC4DBSegmentIStream*>(raw);
    const bool result = SC4ReadGZPersistRef(*db, iid, out);
    db->Release();
    return result;
}

bool SC4ReadBinaryRecord(cIGZPersistDBSegment* segment, const cGZPersistResourceKey& key,
                         std::vector<std::uint8_t>& data, std::uint32_t maxSize) {
    std::uint32_t size = segment->GetRecordSize(key);
    if (maxSize < size) return false;
    data.resize(size);
    return segment->ReadRecord(key, data.data(), size) == size;
}

}  // namespace sc4
