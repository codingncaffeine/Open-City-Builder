// The game's database-segment layer on top of the packed-file segment: record streams
// (cSC4DBSegmentIStream / cSC4DBSegmentOStream), the class-object serializer (cSC4COMSerializer)
// and the segment itself (cSC4DBSegment). Recovered from Sim City 4 Deluxe Editionsub 1.2.1
// (cSC4DBSegment vtable 0x100aa5e50, streams 0x100aa5a30 / 0x100aa5c40, serializer 0x100aa6298;
// helpers SC4Read/WriteGZSerializable 0x100043172..0x1000435e3).
//
// What it does: every city simulator persists itself as a *serializable* referenced from its parent
// by identity. A stream handed to Write() writes an object reference as {u64 id (the object's
// address), u32 class id}; the serializer remembers the object per class and, when the segment is
// saved, writes each class's objects into one record keyed {classId, 0x299b2d1b, 0} as a list of
// entries {u32 size, u32 crc32 of the first <=250000 bytes after the header, u64 id, body}. A class
// table record {0x499b23fe, 0x299b2d1b, 0} = {u32 1, u32 classCount, i32 objectCount, then
// (classId, count) pairs} makes the file self-describing. Loading reads the table, then every class
// record, creating objects through the class factory by class id.
//
// Quirks kept: the object id written to disk is the live object's address; an entry whose CRC does
// not match aborts the class; loadSingleObject succeeds with "found nothing" when the class has no
// more entries; the count of objects per class must match the table or the load reports failure.
// Two deliberate deviations (documented in notes): a stream without user data reads 64-bit ids
// (the original dereferenced null), and SaveClassObjects stops when a pass writes nothing (the
// original could spin forever on an unreachable count).
#pragma once
#include "sc4/CommonIIDs.h"
#include <cstdint>
#include <map>
#include <vector>

#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZPersistResourceManager.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZAutoRefCount.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4DBSegment = 0x499899da;
constexpr std::uint32_t kGZIID_cISC4DBSegmentIStream = 0x4999a8f5;      // also 0x051b11bd
constexpr std::uint32_t kGZIID_cISC4DBSegmentIStreamAlt = 0x051b11bd;
constexpr std::uint32_t kGZIID_cISC4DBSegmentOStream = 0x6999a8fb;      // also 0xc51b11cb
constexpr std::uint32_t kGZIID_cISC4DBSegmentOStreamAlt = 0xc51b11cb;
constexpr std::uint32_t kGZIID_cISC4COMSerializer = 0x699899ea;
constexpr std::uint32_t kClassObjectGroup = 0x299b2d1b;
constexpr std::uint32_t kClassTableType = 0x499b23fe;
constexpr std::uint32_t kVariantObjectTag = 0xa9bee9ac;                 // ReadVariant/WriteVariant
constexpr std::uint32_t kSegmentProgressMessage = 0xc9e30725;
constexpr std::uint32_t kWideIdVersion = 0x42;                          // >= this: 64-bit object ids

class cISC4DBSegment;

// ---- class factory seam (GZCOM()->CreateInstance(classId, iid 0xe4fda3d4)) --------------------
class ISerializableFactory {
public:
    virtual ~ISerializableFactory() = default;
    // New serializable of that class id with one reference, or null.
    virtual cIGZSerializable* CreateSerializable(std::uint32_t classId) = 0;
};
ISerializableFactory* SerializableFactory();
void SetSerializableFactory(ISerializableFactory* factory);
cIGZMessageServer2* DBSegmentMessageServer();     // SL::spMS2 equivalent
void SetDBSegmentMessageServer(cIGZMessageServer2* server);
cIGZPersistResourceManager* PersistResourceManagerService();   // SL::spRM equivalent
void SetPersistResourceManagerService(cIGZPersistResourceManager* manager);

// ---- streams ----------------------------------------------------------------------------------
class cISC4DBSegmentIStream : public cIGZIStream {
public:
    virtual bool Open(cISC4DBSegment* segment, const cGZPersistResourceKey& key) = 0;     // [21]
    virtual bool Close() = 0;                                                             // [22]
    virtual bool IsOpen() = 0;                                                            // [23]
    virtual cIGZPersistDBSerialRecord* GetRecord() = 0;                                   // [24]
    virtual cISC4DBSegment* GetSegment() = 0;                                             // [25]
    virtual bool ReadGZSerializable(cIGZSerializable** out, bool loadNow) = 0;            // [26]
    virtual bool ReadResKey(cGZPersistResourceKey& key) = 0;                              // [27]
    virtual bool ReadVariant(cIGZVariant& variant) = 0;                                   // [28]
};

class cISC4DBSegmentOStream : public cIGZOStream {
public:
    virtual bool Open(cISC4DBSegment* segment, const cGZPersistResourceKey& key, bool fromStart) = 0;  // [21]
    virtual bool Close() = 0;                                                             // [22]
    virtual bool IsOpen() = 0;                                                            // [23]
    virtual cIGZPersistDBSerialRecord* GetRecord() = 0;                                   // [24]
    virtual cISC4DBSegment* GetSegment() = 0;                                             // [25]
    virtual bool WriteGZSerializable(const cIGZSerializable* object) = 0;                 // [26]
    virtual bool WriteResKey(const cGZPersistResourceKey& key) = 0;                       // [27]
    virtual bool WriteVariant(const cIGZVariant& variant) = 0;                            // [28]
};

// ---- the class-object serializer -------------------------------------------------------------
class cISC4COMSerializer : public cIGZUnknown {
public:
    virtual bool Open(cISC4DBSegment* segment, int mode) = 0;                             // [3] 1 = load, 2 = save
    virtual bool Close() = 0;                                                             // [4]
    virtual int GetMode() = 0;                                                            // [5]
    virtual bool LoadClassObjects(std::uint32_t version) = 0;                             // [6]
    virtual bool SaveClassObjects() = 0;                                                  // [7]
    virtual bool WriteGZSerializable(cIGZOStream& stream, const cIGZSerializable* object) = 0;  // [8]
    virtual bool ReadGZSerializable(cIGZIStream& stream, cIGZSerializable** out, bool loadNow) = 0;  // [9]
};

// ---- the segment interface (group 0 of cSC4DBSegment) ------------------------------------------
class cISC4DBSegment : public cIGZUnknown {
public:
    virtual cIGZPersistDBSegment* AsIGZPersistDBSegment() = 0;                            // [3]
    virtual cISC4COMSerializer* GetCOMSerializer() = 0;                                   // [4]
    virtual bool OpenIStream(const cGZPersistResourceKey& key, cISC4DBSegmentIStream** out) = 0;  // [5]
    virtual bool CloseIStream(cISC4DBSegmentIStream* stream) = 0;                         // [6]
    virtual bool LoadClassObjects(std::uint32_t version) = 0;                             // [7]
    virtual bool OpenOStream(const cGZPersistResourceKey& key, cISC4DBSegmentOStream** out, bool fromStart) = 0;  // [8]
    virtual bool CloseOStream(cISC4DBSegmentOStream* stream) = 0;                         // [9]
    virtual bool SaveClassObjects() = 0;                                                  // [10]
    virtual std::uint32_t GetLastError() const = 0;                                       // [11]
    virtual float GetPercentageCompletion() const = 0;                                    // [12]
    virtual void SetPercentageCompletion(float percent) = 0;                              // [13]
};

class cSC4DBSegmentIStream final : public cISC4DBSegmentIStream {
public:
    cSC4DBSegmentIStream() = default;
    ~cSC4DBSegmentIStream();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Open(cISC4DBSegment* segment, const cGZPersistResourceKey& key) override;
    bool Close() override;
    bool IsOpen() override { return record_ && segment_; }
    cIGZPersistDBSerialRecord* GetRecord() override { return record_.get(); }
    cISC4DBSegment* GetSegment() override { return segment_.get(); }
    bool ReadGZSerializable(cIGZSerializable** out, bool loadNow) override;
    bool ReadResKey(cGZPersistResourceKey& key) override;
    bool ReadVariant(cIGZVariant& variant) override;

    bool Skip(std::uint32_t byteCount) override;
    bool GetSint8(std::int8_t& v) override { ok_ = ok_ && record_->GetFieldSint8(v); return ok_; }
    bool GetUint8(std::uint8_t& v) override { ok_ = ok_ && record_->GetFieldUint8(v); return ok_; }
    bool GetSint16(std::int16_t& v) override { ok_ = ok_ && record_->GetFieldSint16(v); return ok_; }
    bool GetUint16(std::uint16_t& v) override { ok_ = ok_ && record_->GetFieldUint16(v); return ok_; }
    bool GetSint32(std::int32_t& v) override { ok_ = ok_ && record_->GetFieldSint32(v); return ok_; }
    bool GetUint32(std::uint32_t& v) override { ok_ = ok_ && record_->GetFieldUint32(v); return ok_; }
    bool GetSint64(std::int64_t& v) override { ok_ = ok_ && record_->GetFieldSint64(v); return ok_; }
    bool GetUint64(std::uint64_t& v) override { ok_ = ok_ && record_->GetFieldUint64(v); return ok_; }
    bool GetFloat32(float& v) override { ok_ = ok_ && record_->GetFieldFloat32(v); return ok_; }
    bool GetFloat64(double& v) override { ok_ = ok_ && record_->GetFieldFloat64(v); return ok_; }
    bool GetRZCharStr(char* buffer, std::uint32_t maxLength) override;
    bool GetGZStr(cIGZString& v) override;
    bool GetGZSerializable(cIGZSerializable& object) override;
    bool GetVoid(void* destination, std::uint32_t byteCount) override;
    bool GetError() const override { return !ok_; }
    void SetUserData(cIGZVariant* userData) override { userData_ = userData; }
    cIGZVariant* GetUserData() override { return userData_.get(); }

private:
    bool ok_ = true;
    std::uint32_t refCount_ = 0;
    cRZAutoRefCount<cIGZPersistDBSerialRecord> record_;
    cRZAutoRefCount<cISC4DBSegment> segment_;
    cRZAutoRefCount<cIGZVariant> userData_;
};

class cSC4DBSegmentOStream final : public cISC4DBSegmentOStream {
public:
    cSC4DBSegmentOStream() = default;
    ~cSC4DBSegmentOStream();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Open(cISC4DBSegment* segment, const cGZPersistResourceKey& key, bool fromStart) override;
    bool Close() override;
    bool IsOpen() override { return record_ && segment_; }
    cIGZPersistDBSerialRecord* GetRecord() override { return record_.get(); }
    cISC4DBSegment* GetSegment() override { return segment_.get(); }
    bool WriteGZSerializable(const cIGZSerializable* object) override;
    bool WriteResKey(const cGZPersistResourceKey& key) override;
    bool WriteVariant(const cIGZVariant& variant) override;

    void Flush() override {}
    bool SetSint8(std::int8_t v) override { ok_ = ok_ && record_->SetFieldSint8(v); return ok_; }
    bool SetUint8(std::uint8_t v) override { ok_ = ok_ && record_->SetFieldUint8(v); return ok_; }
    bool SetSint16(std::int16_t v) override { ok_ = ok_ && record_->SetFieldSint16(v); return ok_; }
    bool SetUint16(std::uint16_t v) override { ok_ = ok_ && record_->SetFieldUint16(v); return ok_; }
    bool SetSint32(std::int32_t v) override { ok_ = ok_ && record_->SetFieldSint32(v); return ok_; }
    bool SetUint32(std::uint32_t v) override { ok_ = ok_ && record_->SetFieldUint32(v); return ok_; }
    bool SetSint64(std::int64_t v) override { ok_ = ok_ && record_->SetFieldSint64(v); return ok_; }
    bool SetUint64(std::uint64_t v) override { ok_ = ok_ && record_->SetFieldUint64(v); return ok_; }
    bool SetFloat32(float v) override { ok_ = ok_ && record_->SetFieldFloat32(v); return ok_; }
    bool SetFloat64(double v) override { ok_ = ok_ && record_->SetFieldFloat64(v); return ok_; }
    bool SetRZCharStr(const char* value) override;
    bool SetGZStr(const cIGZString& value) override;
    bool SetGZSerializable(const cIGZSerializable& object) override;
    bool SetVoid(const void* source, std::uint32_t byteCount) override;
    bool GetError() const override { return !ok_; }
    void SetUserData(cIGZVariant* userData) override { userData_ = userData; }
    cIGZVariant* GetUserData() override { return userData_.get(); }

private:
    bool ok_ = true;
    std::uint32_t refCount_ = 0;
    cRZAutoRefCount<cIGZPersistDBSerialRecord> record_;
    cRZAutoRefCount<cISC4DBSegment> segment_;
    cRZAutoRefCount<cIGZVariant> userData_;
};

class cSC4COMSerializer final : public cISC4COMSerializer {
public:
    explicit cSC4COMSerializer(ISerializableFactory* factory = nullptr);
    ~cSC4COMSerializer();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    bool Open(cISC4DBSegment* segment, int mode) override;
    bool Close() override;
    int GetMode() override { return mode_; }
    bool LoadClassObjects(std::uint32_t version) override;
    bool SaveClassObjects() override;
    bool WriteGZSerializable(cIGZOStream& stream, const cIGZSerializable* object) override;
    bool ReadGZSerializable(cIGZIStream& stream, cIGZSerializable** out, bool loadNow) override;

private:
    struct cClassInfo {
        std::uint32_t expected = 0;   // count in the class table (load) — +0x28
        std::uint32_t actual = 0;     // objects registered this session — +0x2c
        std::map<std::uint64_t, cRZAutoRefCount<cIGZSerializable>> objects;
    };
    void Reset();
    bool GetCRC(cIGZPersistDBSerialRecord* record, std::uint32_t length, std::uint32_t& crc);
    bool OnSetGZSerializable(const cIGZSerializable* object, std::uint64_t id, std::uint32_t classId);
    bool OnGetGZSerializable(cIGZSerializable** out, std::uint64_t id, std::uint32_t classId);
    bool LoadSingleObject(cIGZSerializable* object, std::uint64_t id, std::uint32_t classId);

    bool collecting_ = false;                       // +0x18: save pass active
    std::map<std::uint32_t, cClassInfo> pending_;   // +0x20: objects still to be written
    std::map<std::uint32_t, cClassInfo> classes_;   // +0x38: every class seen
    int mode_ = 0;                                  // +0x50
    int totalObjects_ = 0;                          // +0x54
    bool classesLoaded_ = false;                    // +0x58
    bool saved_ = false;                            // +0x59
    cRZAutoRefCount<cISC4DBSegment> segment_;       // +0x60
    std::vector<unsigned char> crcBuffer_;          // +0x68
    ISerializableFactory* factory_;
    std::uint32_t refCount_ = 0;
};

class cSC4DBSegment : public cGZDBSegmentPackedFile, public cISC4DBSegment {
public:
    cSC4DBSegment();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cGZDBSegmentPackedFile::AddRef(); }
    std::uint32_t Release() override { return cGZDBSegmentPackedFile::Release(); }

    cIGZPersistDBSegment* AsIGZPersistDBSegment() override { return static_cast<cGZDBSegmentPackedFile*>(this); }
    cISC4COMSerializer* GetCOMSerializer() override { return serializer_.get(); }
    bool OpenIStream(const cGZPersistResourceKey& key, cISC4DBSegmentIStream** out) override;
    bool CloseIStream(cISC4DBSegmentIStream* stream) override { return stream->Close(); }
    bool LoadClassObjects(std::uint32_t version) override;
    bool OpenOStream(const cGZPersistResourceKey& key, cISC4DBSegmentOStream** out, bool fromStart) override;
    bool CloseOStream(cISC4DBSegmentOStream* stream) override { return stream->Close(); }
    bool SaveClassObjects() override;
    std::uint32_t GetLastError() const override { return lastError_; }
    float GetPercentageCompletion() const override { return percent_; }
    void SetPercentageCompletion(float percent) override;

    bool Init() override;
    bool Shutdown() override;
    bool Open(bool read, bool write) override;
    bool IsOpen() const override { return cGZDBSegmentPackedFile::IsOpen(); }
    bool Close() override;

protected:
    ~cSC4DBSegment() override;

private:
    cRZAutoRefCount<cISC4COMSerializer> serializer_;   // +0x2c8
    cRZAutoRefCount<cIGZMessageServer2> messageServer_;  // +0x2d0
    std::uint32_t lastError_ = 0;                      // +0x2d8
    float percent_ = 0.0f;                             // +0x2dc
};

// ---- free helpers (0x100043172..0x1000435e3) --------------------------------------------------
bool SC4IsGZSerializable(cIGZUnknown* object);
bool SC4WriteGZSerializable(cISC4DBSegmentOStream* stream, const cIGZUnknown* object);
bool SC4WriteGZSerializable(cIGZOStream& stream, const cIGZUnknown* object);
bool SC4ReadGZSerializable(cISC4DBSegmentIStream& stream, std::uint32_t iid, void** out, bool loadNow);
bool SC4ReadGZSerializable(cIGZIStream& stream, std::uint32_t iid, void** out, bool loadNow);
bool SC4WriteGZPersistRef(cISC4DBSegmentOStream* stream, const cIGZUnknown* resource);
bool SC4WriteGZPersistRef(cIGZOStream& stream, const cIGZUnknown* resource);
bool SC4ReadGZPersistRef(cISC4DBSegmentIStream& stream, std::uint32_t iid, void** out);
bool SC4ReadGZPersistRef(cIGZIStream& stream, std::uint32_t iid, void** out);
// SC4ReadBinaryRecord @ 0x1001e91f5: whole record into `data` (false when absent, larger than
// maxSize, or short).
bool SC4ReadBinaryRecord(cIGZPersistDBSegment* segment, const cGZPersistResourceKey& key,
                         std::vector<std::uint8_t>& data, std::uint32_t maxSize);
// SC4ReadBinaryRecord (Mac i386 0x00163106): the record written to the file at `path` (truncated first); the byte count,
// or 0xffffffff when the record is absent or a step fails. A file the call created is removed again on failure.
std::uint32_t SC4ReadBinaryRecord(cIGZPersistDBSegment* segment, const cGZPersistResourceKey& key, const cIGZString& path);
// SC4ReadBinaryRecord (Mac i386 0x00163240): the same from whichever segment of the resource manager holds the key.
std::uint32_t SC4ReadBinaryRecord(const cGZPersistResourceKey& key, const cIGZString& path);

}  // namespace sc4
