// cGZDBSerialRecordAsIGZFile (GZCOM class 0x0a32c0da, iid 0x8a32c0ac): a database record served as a plain file (cIGZFile).
#pragma once
#include <cstdint>

#include "gz/cIGZFile.h"
#include "gz/cIGZPersistDBSerialRecord.h"
#include "gz/cIGZUnknown.h"
#include "rz/cRZString.h"

constexpr std::uint32_t kGZCLSID_cGZDBSerialRecordAsIGZFile = 0x0a32c0da;
constexpr std::uint32_t kGZIID_cIGZDBSerialRecordAsIGZFile = 0x8a32c0ac;

// The adapter's own interface: slot 3 hands it the record (one reference taken), slot 4 gives it back, slot 5 is the file face.
class cIGZDBSerialRecordAsIGZFile : public cIGZUnknown {
public:
    virtual bool SetDBSerialRecord(cIGZPersistDBSerialRecord* record) = 0;   // [3]
    virtual cIGZPersistDBSerialRecord* GetDBSerialRecord() = 0;               // [4]
    virtual cIGZFile* AsIGZFile() = 0;                                        // [5] (not AddRef'd)
};

namespace gz {

// cGZDBSerialRecordAsIGZFile (GZCOM class 0x0a32c0da, iid 0x8a32c0ac): a database record served as a plain file (cIGZFile). The
// resource helper SC4GetResourceAsFile, the Gimex reader / writer and the packed segment's GetRecordAsIGZFile make it, hand it a
// serial record (SetDBSerialRecord) and read or write through its cIGZFile face; closing the file closes the record.
class cGZDBSerialRecordAsIGZFile final : public cIGZDBSerialRecordAsIGZFile, public cIGZFile {
public:
    cGZDBSerialRecordAsIGZFile() = default;
    ~cGZDBSerialRecordAsIGZFile() override { if (record_) Close(); }

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZDBSerialRecordAsIGZFile || iid == kGZIID_cIGZUnknown) { *out = static_cast<cIGZDBSerialRecordAsIGZFile*>(this); }
        else if (iid == kGZIID_cIGZFile) { *out = static_cast<cIGZFile*>(this); }
        else return false;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ < 2) {
            if (IsOpen()) Close();
            delete this;
            return 0;
        }
        return --refs_;
    }
    // @ 0x10036982c: takes a reference on the new record, drops the old one, always succeeds.
    bool SetDBSerialRecord(cIGZPersistDBSerialRecord* record) override {
        if (record) record->AddRef();
        if (record_) record_->Release();
        record_ = record;
        return true;
    }
    cIGZPersistDBSerialRecord* GetDBSerialRecord() override { return record_; }   // @ 0x1003709ce
    cIGZFile* AsIGZFile() override { return this; }                               // @ 0x1003709d8 (the cIGZFile base at +8)
    bool Open(int, int, int) override { return false; }
    bool IsOpen() const override { return record_ != nullptr; }
    bool Close() override {
        if (!record_) return false;
        record_->AsIGZPersistDBRecord()->Close();
        record_->Release();
        record_ = nullptr;
        return true;
    }
    int Position() override { return record_ ? static_cast<int>(record_->GetPosition()) : -1; }
    std::uint32_t Length() override { return record_ ? record_->GetSize() : 0; }
    bool SetLength(std::uint32_t n) override { return record_ ? record_->SetSize(n) : false; }
    void SeekToBegin() override { if (record_) record_->SeekAbsolute(0); }
    void SeekToEnd() override {
        if (!record_) return;
        std::uint32_t size = record_->GetSize();
        if (size) record_->SeekAbsolute(size);
    }
    void SeekToRelativePosition(int d) override { if (record_) record_->SeekRelative(d); }
    void SeekToPosition(std::uint32_t p) override { if (record_) record_->SeekAbsolute(p); }
    std::uint32_t Seek(int offset, int origin) override {
        if (!record_) return 0xffffffffu;
        switch (origin) {
            case 0: return record_->SeekAbsolute(static_cast<std::uint32_t>(offset)) ? static_cast<std::uint32_t>(offset) : 0xffffffffu;
            case 1: return record_->SeekRelative(offset) ? record_->GetPosition() : 0xffffffffu;
            case 2: {
                std::uint32_t size = record_->GetSize();
                if (size == 0) return 0xffffffffu;
                std::uint32_t p = size + static_cast<std::uint32_t>(offset);
                return record_->SeekAbsolute(p) ? p : 0xffffffffu;
            }
            default: return 0xffffffffu;
        }
    }
    bool Read(void* d, std::uint32_t n) override { return record_ && record_->GetFieldVoid(d, n); }
    bool ReadWithCount(void* d, std::uint32_t& n) override {
        if (!record_) return false;
        std::uint32_t pos = record_->GetPosition(), size = record_->GetSize();
        if (size < n + pos) n = size - pos;
        return record_->GetFieldVoid(d, n);
    }
    bool Write(const void* s, std::uint32_t n) override { return record_ && record_->SetFieldVoid(s, n); }
    bool WriteWithCount(const void* s, std::uint32_t& n) override {
        if (!record_) return false;
        if (!record_->SetFieldVoid(s, n)) n = 0;
        return true;
    }
    bool Flush() override { return true; }
    bool Remove() override { return false; }
    bool Rename(const cIGZString&) override { return false; }
    bool Exists() override { return record_ != nullptr; }
    bool Copy(const cIGZString&, bool) override { return false; }
    bool Copy(const cIGZFile&, bool) override { return false; }
    void GetPath(cIGZString&) const override {}
    const cIGZString* GetPath() const override {
        static const cRZString kEmpty;
        return &kEmpty;
    }
    const char* GetPathCStr() const override { return nullptr; }
    void SetPath(const cIGZString&) override {}
    void SetPath(const char*) override {}
    bool GetOpenModes(int& access, int& creation, int& share) const override {
        access = 0;
        creation = 3;
        share = 1;
        if (!record_) return false;
        std::uint32_t flags = record_->AsIGZPersistDBRecord()->GetAccessFlags();
        if (flags & 1) access |= 1;
        if (flags & 2) access |= 2;
        return true;
    }
    void GetBuffering(std::uint32_t&, std::uint32_t&) const override {}
    bool SetBuffering(std::uint32_t, std::uint32_t) override { return true; }

private:
    cIGZPersistDBSerialRecord* record_ = nullptr;
    std::uint32_t refs_ = 0;
};


// The director's maker (cGZResManCOMDirector::CreateGZDBSerialRecordAsIGZFile).
cIGZUnknown* CreateGZDBSerialRecordAsIGZFile();

}  // namespace gz
