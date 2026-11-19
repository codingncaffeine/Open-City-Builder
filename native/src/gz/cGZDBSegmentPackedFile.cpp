#include "gz/cGZDBSegmentPackedFile.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

#include "gz/cGZDBRecords.h"
#include "gz/cGZDBSerialRecordAsIGZFile.h"
#include "gz/cGZPersistResourceKeyList.h"
#include "rz/cRZFastCompression3.h"

namespace {

constexpr cGZPersistResourceKey kDirectoryRecordKey{0xe86b1eef, 0xe86b1eef, 0x286b1f03};
constexpr std::uint32_t kDirectoryRecordType = 0xe86b1eef;
constexpr std::uint8_t kEmbeddedMarker[16] = {0x80, 0x9d, 0x88, 0xec, 0x8f, 0x24, 0x03, 0x6c,
                                              0xc9, 0xa6, 0x31, 0x56, 0x5b, 0xcf, 0x77, 0x22};

}  // namespace

// ---------------------------------------------------------------------------------------------

cGZDBSegmentPackedFile::cGZDBSegmentPackedFile() = default;

cGZDBSegmentPackedFile::~cGZDBSegmentPackedFile() { Shutdown(); }

void cGZDBSegmentPackedFile::FinalRelease() {
    if (openFlags_ & 3) Close();
    if (initialized_) Shutdown();
    delete this;
}

bool cGZDBSegmentPackedFile::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cGZDBSegmentPackedFile) {
        *out = this;
        AddRef();
        return true;
    }
    return cGZPersistDBSegment::QueryInterface(iid, out);
}

int cGZDBSegmentPackedFile::IsDatabaseFile(const cIGZString& path, bool searchEmbedded) {
    cRZFile file(path);
    if (!file.Open(1, 2, 1)) return -1;
    int result = -1;
    file.SeekToBegin();
    std::uint32_t ident = 0;
    if (file.Read(&ident, 4)) {
        result = ident == kPackedFileHeaderFileIdentifier ? 0 : -1;
        if (result != 0 && searchEmbedded) {
            auto* probe = new cGZDBSegmentPackedFile();
            probe->AddRef();
            probe->Init();
            probe->SetPath(path);
            result = static_cast<int>(probe->FindHeaderRecord());
            probe->Release();
        }
    }
    file.Close();
    return result;
}

bool cGZDBSegmentPackedFile::Init() {
    if (!initialized_) initialized_ = true;
    return true;
}

bool cGZDBSegmentPackedFile::Init(std::uint32_t segmentId, const cIGZString* path, bool) {
    if (!initialized_) {
        initialized_ = true;
        if (path) path_.AssignFromChars(path->Data(), path->Strlen());
        segmentId_ = segmentId;
    }
    return true;
}

bool cGZDBSegmentPackedFile::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        if (openFlags_ & 3) Close();
    }
    return true;
}

bool cGZDBSegmentPackedFile::Open(bool read, bool write) {
    if (openFlags_ & 3) return false;
    if (path_.Strlen() == 0) return false;
    if (!read && !write) return false;
    if (!initialized_) return false;
    openFlags_ = write ? 3u : (read ? 1u : 0u);
    file_.SetPath(path_);
    if (!file_.Open(static_cast<int>(openFlags_), write ? 3 : 2, 1)) {
        openFlags_ = 0;
        return false;
    }
    bool ok = false;
    if (file_.Length() != 0 || ((openFlags_ & 2) && WriteEmptyHeaderRecord())) {
        if (!ReadHeaderRecord()) {
            if ((openFlags_ & 2) && WriteEmptyHeaderRecord()) ok = true;
        } else if (ReadIndexRecord() && ReadHoleRecord()) {
            ok = ReadCompressedSetRecord();
        }
    }
    if (!ok) {
        file_.Close();
        openFlags_ = 0;
    }
    return ok;
}

bool cGZDBSegmentPackedFile::Close() {
    bool ok = true;
    if (openFlags_ & 3) {
        if (openFlags_ & 2) {
            if (!CloseOpenRecords()) ok = false;
            else ok = Flush();
        }
        index_.clear();
        holes_.clear();
        compressed_.clear();
        file_.Close();
        openFlags_ = 0;
    }
    return ok;
}

bool cGZDBSegmentPackedFile::CloseOpenRecords() {
    if (!(openFlags_ & 3)) return false;
    if (openRecords_.empty()) return true;
    std::vector<cGZPersistDBSerialRecord*> records;
    for (auto& entry : openRecords_) {
        entry.second->AddRef();
        records.push_back(entry.second);
    }
    for (cGZPersistDBSerialRecord* r : records) {
        cIGZPersistDBRecord* view = static_cast<cIGZPersistDBRecord*>(r);
        CloseRecord(view);
        r->Release();
    }
    return true;
}

bool cGZDBSegmentPackedFile::Flush() {
    if (!(openFlags_ & 2)) return true;
    if (!WriteCompressedSetRecord()) return false;
    if (holeUseFlags_ & kCompactOnFlush) {
        if (ShouldCompactDatabase()) CompactDatabase();
    }
    if (!WriteIndexRecord() || !WriteHoleRecord() || !WriteHeaderRecord()) return false;
    return file_.Flush();
}

// ------------------------------------------------------------------------------ header/index IO

bool cGZDBSegmentPackedFile::WriteEmptyHeaderRecord() {
    header_ = Header{};
    header_.identifier = kPackedFileHeaderFileIdentifier;
    header_.major = 1;
    header_.minor = 0;
    header_.userMajor = nextUserMajor_;
    header_.userMinor = nextUserMinor_;
    header_.flags = 0;
    header_.created = header_.modified = static_cast<std::uint32_t>(std::time(nullptr));
    header_.indexMajor = 7;
    if (!WriteHeaderRecord()) return false;
    return file_.SetLength(0x60);
}

bool cGZDBSegmentPackedFile::WriteHeaderRecord() {
    if (!(openFlags_ & 2) || !file_.IsOpen()) return false;
    if (file_.Seek(static_cast<int>(fileStartingOffset_), 0) != fileStartingOffset_) return false;
    Header out = header_;
    out.identifier = kPackedFileHeaderFileIdentifier;
    out.major = 1;
    out.userMinor = nextUserMinor_;
    out.modified = static_cast<std::uint32_t>(std::time(nullptr));
    return file_.Write(&out, sizeof out);
}

bool cGZDBSegmentPackedFile::ReadHeaderRecord() {
    static bool inRetry = false;  // (sic) the original guards its single re-scan with a static
    if (file_.Seek(static_cast<int>(fileStartingOffset_), 0) != fileStartingOffset_) return false;
    if (!file_.Read(&header_, sizeof header_)) return false;
    if (VerifyHeaderRecordIntegrity()) return true;
    std::uint32_t found = FindHeaderRecord();
    fileStartingOffset_ = found;
    if (found == 0xffffffffu) {
        fileStartingOffset_ = 0;
        return false;
    }
    if (inRetry) return false;
    inRetry = true;
    bool ok = ReadHeaderRecord();
    inRetry = false;
    return ok;
}

bool cGZDBSegmentPackedFile::VerifyHeaderRecordIntegrity() {
    if (header_.identifier != kPackedFileHeaderFileIdentifier || header_.major >= 2 || header_.indexCount >= 0x7ffffff)
        return false;
    std::uint32_t length = file_.Length();
    if (!(header_.indexOffset < length)) return false;
    if (static_cast<std::uint32_t>(header_.indexSize + header_.indexOffset) > length) return false;
    if (header_.holeCount >= 0x7ffffff) return false;
    if (!(header_.holeOffset < length)) return false;
    return static_cast<std::uint32_t>(header_.holeSize + header_.holeOffset) <= length;
}

std::uint32_t cGZDBSegmentPackedFile::FindHeaderRecord() {
    bool wasOpen = file_.IsOpen();
    if (!wasOpen) {
        file_.SetPath(path_);
        file_.Open(1, 2, 1);
        if (!file_.IsOpen()) return 0xffffffffu;
    }
    std::uint32_t result = 0xffffffffu;
    if (file_.Length() > 0x10) {
        std::vector<std::uint8_t> block(0x200);
        file_.SeekToBegin();
        while (true) {
            int blockStart = file_.Position();
            std::uint32_t count = 0x200;
            if (!file_.ReadWithCount(block.data(), count)) break;
            bool found = false;
            std::size_t size = count;
            if (size > 0xf) {
                for (std::size_t i = 0; i + 16 <= size; ++i) {
                    if (block[i] != 0x80) continue;
                    if (std::memcmp(&block[i], kEmbeddedMarker, 16) != 0) continue;
                    std::uint32_t after = static_cast<std::uint32_t>(blockStart + 0x10 + static_cast<int>(i));
                    int resume = file_.Position();
                    file_.SeekToPosition(after);
                    char tag[4];
                    if (file_.Read(tag, 4) && tag[0] == 'D' && tag[1] == 'B' && tag[2] == 'P' && tag[3] == 'F') {
                        result = after;
                        found = true;
                    } else {
                        file_.SeekToPosition(static_cast<std::uint32_t>(resume));
                    }
                    break;
                }
            }
            if (found) break;
            file_.SeekToRelativePosition(-0x10);
            if (count != 0x200) break;
        }
    }
    if (!wasOpen) file_.Close();
    return result;
}

bool cGZDBSegmentPackedFile::ReadIndexRecord() {
    if (header_.indexCount == 0) return true;
    std::uint32_t at = header_.indexOffset + fileStartingOffset_;
    if (file_.Seek(static_cast<int>(at), 0) != at) return false;
    std::vector<std::uint32_t> raw(static_cast<std::size_t>(header_.indexCount) * 5);
    if (!file_.Read(raw.data(), header_.indexCount * 20u)) return false;
    index_.clear();
    for (std::uint32_t i = 0; i < header_.indexCount; ++i) {
        const std::uint32_t* e = &raw[i * 5];
        cGZPersistResourceKey key{e[0], e[1], e[2]};
        index_.emplace(key, RecordDataInfo{e[3], e[4]});   // first duplicate wins (map semantics)
        IncrementTypeAndGroupUse(e[0], e[1], true);
    }
    if (!VerifyIndexRecordIntegrity()) return false;
    indexDirty_ = false;
    return true;
}

bool cGZDBSegmentPackedFile::VerifyIndexRecordIntegrity() {
    std::uint32_t length = file_.Length();
    for (const auto& [key, info] : index_) {
        (void)key;
        if (info.offset < 0x60 || length <= info.offset || length < info.offset + info.size) return false;
    }
    return true;
}

bool cGZDBSegmentPackedFile::ReadHoleRecord() {
    if (header_.holeCount == 0) return true;
    std::uint32_t at = header_.holeOffset + fileStartingOffset_;
    if (file_.Seek(static_cast<int>(at), 0) != at) return false;
    std::vector<std::uint32_t> raw(static_cast<std::size_t>(header_.holeCount) * 2);
    if (!file_.Read(raw.data(), header_.holeCount * 8u)) return false;
    for (std::uint32_t i = 0; i < header_.holeCount; ++i) holes_.insert(HoleRecord{raw[i * 2], raw[i * 2 + 1]});
    if (!VerifyHoleRecordIntegrity()) return false;
    holesDirty_ = false;
    return true;
}

bool cGZDBSegmentPackedFile::VerifyHoleRecordIntegrity() {
    std::uint32_t offset = header_.holeOffset;
    bool offsetOk = true;
    if (offset != 0) offsetOk = offset > 0x5f && offset < file_.Length();
    return header_.holeCount == (header_.holeSize >> 3) && offsetOk;
}

bool cGZDBSegmentPackedFile::ReadCompressedSetRecord() {
    compressed_.clear();
    std::uint32_t size = 0;
    if (DoReadRecord(kDirectoryRecordKey, nullptr, size) == 0xffffffffu) return true;  // no DIR record
    std::uint32_t expected = DoGetRecordSize(kDirectoryRecordKey);
    if (expected == 0xffffffffu) return true;
    std::vector<std::uint32_t> data(expected / 4 + 1);
    std::uint32_t capacity = expected;
    if (DoReadRecord(kDirectoryRecordKey, data.data(), capacity) == 0) return false;
    for (std::uint32_t i = 0; i < capacity / 16; ++i) {
        cGZPersistResourceKey key{data[i * 4], data[i * 4 + 1], data[i * 4 + 2]};
        compressed_.emplace(key, data[i * 4 + 3]);
    }
    return true;
}

bool cGZDBSegmentPackedFile::WriteCompressedSetRecord() {
    if (!(openFlags_ & 2) || !compressedSetDirty_) return true;
    DoDeleteRecord(kDirectoryRecordKey);
    std::vector<std::uint32_t> data;
    data.reserve(compressed_.size() * 4);
    for (const auto& [key, size] : compressed_) {
        data.push_back(key.type);
        data.push_back(key.group);
        data.push_back(key.instance);
        data.push_back(size);
    }
    if (!DoWriteRecord(kDirectoryRecordKey, data.data(), static_cast<std::uint32_t>(data.size() * 4)))
        return false;
    compressedSetDirty_ = false;
    return true;
}

bool cGZDBSegmentPackedFile::WriteIndexRecord() {
    if (!(openFlags_ & 2) || !indexDirty_) return true;
    std::uint32_t bytes = static_cast<std::uint32_t>(index_.size()) * 20;
    std::uint32_t oldSize = header_.indexSize;
    bool ok;
    if (bytes == 0) {
        ok = FreeSpace(header_.indexOffset, oldSize);
        header_.indexCount = 0;
        header_.indexSize = 0;
    } else {
        header_.indexCount = static_cast<std::uint32_t>(index_.size());
        header_.indexSize = bytes;
        bool haveSpace = true;
        if (oldSize != bytes) {
            haveSpace = FreeSpace(header_.indexOffset, oldSize);
            if (haveSpace) {
                header_.indexOffset = 0;
                haveSpace = AllocateSpace(header_.indexOffset, header_.indexSize);
                if (!haveSpace) {
                    header_.indexCount = 0;
                    header_.indexSize = 0;
                }
            }
        }
        if (!haveSpace) {
            ok = false;
        } else {
            std::vector<std::uint32_t> raw;
            raw.reserve(index_.size() * 5);
            for (const auto& [key, info] : index_) {
                raw.push_back(key.type);
                raw.push_back(key.group);
                raw.push_back(key.instance);
                raw.push_back(info.offset);
                raw.push_back(info.size);
            }
            ok = WriteFileSpan(raw.data(), header_.indexOffset, header_.indexSize);
            if (!ok && header_.indexOffset != 0) {
                FreeSpace(header_.indexOffset, header_.indexSize);
                header_.indexCount = 0;
                header_.indexSize = 0;
            }
        }
    }
    indexDirty_ = false;
    return ok;
}

bool cGZDBSegmentPackedFile::WriteHoleRecord() {
    if (!(openFlags_ & 2) || !holesDirty_) return true;
    bool ok = true;
    if (holes_.size() != header_.holeCount) {
        ok = FreeSpace(header_.holeOffset, header_.holeSize);
        if (ok && !holes_.empty()) {
            header_.holeSize = static_cast<std::uint32_t>(holes_.size()) << 3;
            ok = AllocateSpace(header_.holeOffset, header_.holeSize);
        }
        if (!ok) {
            header_.holeCount = 0;
            header_.holeSize = 0;
            holesDirty_ = false;
            return false;
        }
    }
    std::size_t count = holes_.size();
    if (count == 0) {
        header_.holeCount = 0;
        header_.holeSize = 0;
        ok = true;
    } else {
        header_.holeCount = static_cast<std::uint32_t>(count);
        header_.holeSize = static_cast<std::uint32_t>(count) * 8;
        std::vector<std::uint32_t> raw;
        raw.reserve(count * 2);
        for (const HoleRecord& h : holes_) {
            raw.push_back(h.offset);
            raw.push_back(h.size);
        }
        ok = WriteFileSpan(raw.data(), header_.holeOffset, header_.holeSize);
    }
    holesDirty_ = false;
    return ok;
}

bool cGZDBSegmentPackedFile::ShouldCompactDatabase() {
    if (!(openFlags_ & 3)) return false;
    std::uint32_t length = file_.Length();
    std::uint32_t holeBytes = 0;
    for (const HoleRecord& h : holes_) holeBytes += h.size;
    std::uint32_t used = length - holeBytes;
    if (length < holeBytes || used == 0) return false;
    float ratio = static_cast<float>(holeBytes) / static_cast<float>(used);
    return ratio > compactionRatio_;
}

// ---------------------------------------------------------------------------------------- spans

bool cGZDBSegmentPackedFile::SeekAbsolute(int position) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 3)) return false;
    int target = static_cast<int>(fileStartingOffset_) + position;
    return file_.Seek(target, 0) == static_cast<std::uint32_t>(target);
}

bool cGZDBSegmentPackedFile::ReadFileSpan(void* destination, std::uint32_t offset, std::uint32_t byteCount) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 3)) return false;
    std::uint32_t at = fileStartingOffset_ + offset;
    if (file_.Seek(static_cast<int>(at), 0) != at) return false;
    return file_.Read(destination, byteCount);
}

bool cGZDBSegmentPackedFile::ReadFileSpanWithCount(void* destination, std::uint32_t offset, std::uint32_t& byteCount) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 3)) return false;
    std::uint32_t at = fileStartingOffset_ + offset;
    if (file_.Seek(static_cast<int>(at), 0) != at) return false;
    return file_.ReadWithCount(destination, byteCount);
}

bool cGZDBSegmentPackedFile::WriteFileSpan(const void* source, std::uint32_t offset, std::uint32_t byteCount) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 2)) return false;
    std::uint32_t at = fileStartingOffset_ + offset;
    if (file_.Seek(static_cast<int>(at), 0) != at) return false;
    return file_.Write(source, byteCount);
}

bool cGZDBSegmentPackedFile::GetRecordAsIGZFile(cIGZPersistDBRecord* record, cIGZFile** file) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    auto* wrapper = new gz::cGZDBSerialRecordAsIGZFile();
    void* serial = nullptr;
    if (record && record->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &serial)) {
        wrapper->SetDBSerialRecord(static_cast<cIGZPersistDBSerialRecord*>(serial));
        static_cast<cIGZPersistDBSerialRecord*>(serial)->Release();
    }
    *file = wrapper->AsIGZFile();
    (*file)->AddRef();
    return true;
}

// ------------------------------------------------------------------------------ compression

bool cGZDBSegmentPackedFile::CompressPayload(const std::uint8_t* data, std::uint32_t size,
                                             std::vector<std::uint8_t>& out) {
    if (size >= 40000000u) {  // too large: stored as-is (the ratio test then rejects it)
        out.assign(data, data + size);
        return true;
    }
    cRZFastCompression3 compressor;
    if (size > 250000) compressor.EnableQuickCompression(true);
    // the encoder reads up to two bytes past the end; callers of the original padded their buffers
    std::vector<std::uint8_t> padded(data, data + size);
    padded.resize(size + 2, 0);
    out.resize(compressor.GetMaxLengthRequiredForCompressedData(size));
    std::uint32_t length = 0;
    if (!compressor.CompressData(padded.data(), size, out.data(), length)) {
        out.clear();
        return false;
    }
    out.resize(length);
    return true;
}

bool cGZDBSegmentPackedFile::DecompressPayload(const std::uint8_t* data, std::uint32_t size,
                                               std::vector<std::uint8_t>& out) {
    cRZFastCompression3 codec;
    if (!codec.ValidateCompressedData(data, size)) return false;
    std::uint32_t length = codec.GetLengthOfDecompressedData(data);
    out.assign(length, 0);
    std::uint32_t capacity = length;
    if (length == 0) return true;
    if (!codec.DecompressData(data, size, out.data(), capacity)) {
        out.clear();
        return false;
    }
    out.resize(capacity);
    return true;
}

bool cGZDBSegmentPackedFile::ShouldRecordBeCompressed(const cGZPersistResourceKey& key, std::uint32_t size) {
    if (size - 0x32u < 0xf423cfu && key.type != kDirectoryRecordType) {
        if (compressAll_) return true;
        if (!compressTypes_.empty() && compressTypes_.count(key.type)) return true;
        if (!compressGroups_.empty()) return compressGroups_.count(key.group) != 0;
    }
    return false;
}

bool cGZDBSegmentPackedFile::DecompressRecord(std::uint32_t offset, std::uint32_t storedSize,
                                              std::vector<std::uint8_t>& out) {
    if (offset == 0) return false;
    std::vector<std::uint8_t> stored(storedSize);
    if (!ReadFileSpan(stored.data(), offset, storedSize)) return false;
    return DecompressPayload(stored.data(), storedSize, out);
}

bool cGZDBSegmentPackedFile::DecompressRecord(const cGZPersistResourceKey& key, std::vector<std::uint8_t>& out) {
    if (compressed_.find(key) == compressed_.end()) return false;
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    return DecompressRecord(it->second.offset, it->second.size, out);
}

// ---------------------------------------------------------------------------- record operations

bool cGZDBSegmentPackedFile::DoTestForRecord(const cGZPersistResourceKey& key) {
    if (!(openFlags_ & 3)) return false;
    return index_.find(key) != index_.end() || openRecords_.find(key) != openRecords_.end();
}

std::uint32_t cGZDBSegmentPackedFile::DoGetRecordSize(const cGZPersistResourceKey& key) {
    if (!(openFlags_ & 3)) return 0xffffffffu;
    if (openFlags_ & 2) {
        auto open = openRecords_.find(key);
        if (open != openRecords_.end()) {
            std::uint32_t size = open->second->GetSize();
            if (size != 0xffffffffu) return size;
        }
    }
    auto c = compressed_.find(key);
    if (c != compressed_.end()) return c->second;
    auto it = index_.find(key);
    return it == index_.end() ? 0xffffffffu : it->second.size;
}

bool cGZDBSegmentPackedFile::DoOpenRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** out,
                                          std::uint32_t flags) {
    *out = nullptr;
    if (!(openFlags_ & 3)) return false;
    if (flags & 2) return DoOpenRecordForWrite(key, out);
    if (!(flags & 1)) return false;

    cGZPersistDBSerialRecord* record = nullptr;
    auto open = openRecords_.find(key);
    if (open == openRecords_.end()) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        if (compressed_.find(key) == compressed_.end()) {
            if (it->second.size > lazyReadThreshold_) {
                record = new cGZDBReadRecordPackedFile(it->second.offset, it->second.size, key, this, true);
            } else {
                auto buffer = std::make_shared<std::vector<std::uint8_t>>(it->second.size);
                if (!ReadFileSpan(buffer->data(), it->second.offset, it->second.size)) return false;
                record = new cGZDBReadRecordRAM(buffer, it->second.size, key, this, true);
            }
        } else {
            auto buffer = std::make_shared<std::vector<std::uint8_t>>();
            if (!DecompressRecord(it->second.offset, it->second.size, *buffer)) return false;
            auto size = static_cast<std::uint32_t>(buffer->size());
            record = new cGZDBReadRecordRAM(buffer, size, key, this, true);
        }
    } else {
        cGZPersistDBSerialRecord* existing = open->second;
        if (existing->GetAccessFlags() & 2) return false;      // open for write: exclusive
        if (existing->Kind() == cGZPersistDBSerialRecord::kKindPackedRead) {
            auto* src = static_cast<cGZDBReadRecordPackedFile*>(existing);
            record = new cGZDBReadRecordPackedFile(src->FileOffset(), src->GetSize(), key, this, true);
        } else if (existing->Kind() == cGZPersistDBSerialRecord::kKindRAMRead) {
            auto* src = static_cast<cGZDBReadRecordRAM*>(existing);
            record = new cGZDBReadRecordRAM(src->SharedBuffer(), src->GetSize(), key, this, true);
        } else {
            return false;
        }
    }
    record->AddRef();           // the caller's reference
    record->AddRef();           // the open-record table's reference
    openRecords_.emplace(key, record);
    *out = static_cast<cIGZPersistDBRecord*>(record);
    return true;
}

bool cGZDBSegmentPackedFile::DoOpenRecordForWrite(const cGZPersistResourceKey& key, cIGZPersistDBRecord** out) {
    if (!(openFlags_ & 2)) return false;
    if (openRecords_.find(key) != openRecords_.end()) return false;
    auto it = index_.find(key);
    if (it == index_.end()) return DoCreateNewRecord(key, out);

    cGZPersistDBSerialRecord* record;
    if (compressed_.find(key) == compressed_.end()) {
        record = new cGZDBWriteRecordPackedFile(it->second.offset, it->second.size, key, this, true);
    } else {
        std::vector<std::uint8_t> plain;
        if (!DecompressRecord(it->second.offset, it->second.size, plain)) return false;
        record = new cGZDBWriteRecordPackedFile(plain.data(), static_cast<std::uint32_t>(plain.size()), key, this, true);
    }
    record->AddRef();
    *out = static_cast<cIGZPersistDBRecord*>(record);
    record->AddRef();
    openRecords_.emplace(key, record);
    return true;
}

bool cGZDBSegmentPackedFile::DoCreateNewRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** out) {
    if (!(openFlags_ & 2)) return false;
    if (index_.find(key) != index_.end()) return false;
    if (openRecords_.find(key) != openRecords_.end()) return false;
    auto* record = new cGZDBWriteRecordPackedFile(std::uint32_t{0}, std::uint32_t{0}, key, this, true);
    record->AddRef();
    *out = static_cast<cIGZPersistDBRecord*>(record);
    record->AddRef();
    openRecords_.emplace(key, record);
    return true;
}

void cGZDBSegmentPackedFile::RemoveIndexedRecord(
    const cGZPersistResourceKey& key,
    std::map<cGZPersistResourceKey, RecordDataInfo, cGZPersistResourceKeyOrder>::iterator it) {
    RecordDataInfo info = it->second;
    IncrementTypeAndGroupUse(key.type, key.group, false);
    SetRecordCompressed(key, false, 0);
    index_.erase(it);
    FreeSpace(info.offset, info.size);
    indexDirty_ = true;
}

bool cGZDBSegmentPackedFile::DoDeleteRecord(const cGZPersistResourceKey& key) {
    if (!(openFlags_ & 2)) return false;
    if (openRecords_.find(key) != openRecords_.end()) return false;
    auto it = index_.find(key);
    if (it == index_.end()) return false;
    RemoveIndexedRecord(key, it);
    return true;
}

bool cGZDBSegmentPackedFile::DoCloseRecord(cIGZPersistDBRecord* view) {
    if (!(openFlags_ & 3)) return false;
    cGZPersistResourceKey key = view->GetKey();
    auto range = openRecords_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        cGZPersistDBSerialRecord* record = it->second;
        if (static_cast<cIGZPersistDBRecord*>(record) != view) continue;
        if (view->GetAccessFlags() & 2) WriteRecordInternal(record, key);
        openRecords_.erase(it);
        record->DoPostClose();
        record->Release();
        return true;
    }
    return false;
}

bool cGZDBSegmentPackedFile::DoAbortRecord(cIGZPersistDBRecord* view) {
    if (!(openFlags_ & 3)) return false;
    cGZPersistResourceKey key = view->GetKey();
    auto range = openRecords_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        cGZPersistDBSerialRecord* record = it->second;
        if (static_cast<cIGZPersistDBRecord*>(record) != view) continue;
        openRecords_.erase(it);
        record->DoPostClose();
        record->Release();
        return true;
    }
    return false;
}

// Stores the contents of a closed write record: drops the old copy, compresses when worthwhile,
// otherwise streams the raw bytes into freshly allocated space.
bool cGZDBSegmentPackedFile::WriteRecordInternal(cGZPersistDBSerialRecord* record, const cGZPersistResourceKey& key) {
    if (!(openFlags_ & 2)) return false;
    auto old = index_.find(key);
    if (old != index_.end()) RemoveIndexedRecord(key, old);

    std::uint32_t size = record->GetSize();
    if (size == 0) {
        index_[key] = RecordDataInfo{0, 0};
        IncrementTypeAndGroupUse(key.type, key.group, true);
        indexDirty_ = true;
        return true;
    }
    if (ShouldRecordBeCompressed(key, size)) {
        std::vector<std::uint8_t> plain(size + 2, 0);
        if (record->SeekAbsolute(0) && record->GetFieldVoid(plain.data(), size)) {
            std::vector<std::uint8_t> packed;
            if (CompressPayload(plain.data(), size, packed) &&
                static_cast<float>(packed.size()) / static_cast<float>(size) < 0.8f) {
                std::uint32_t offset = 0;
                auto packedSize = static_cast<std::uint32_t>(packed.size());
                if (AllocateSpace(offset, packedSize)) {
                    if (WriteFileSpan(packed.data(), offset, packedSize)) {
                        IncrementTypeAndGroupUse(key.type, key.group, true);
                        SetRecordCompressed(key, true, size);
                        index_[key] = RecordDataInfo{offset, packedSize};
                        indexDirty_ = true;
                        return true;
                    }
                    FreeSpace(offset, packedSize);
                }
            }
        }
    }
    std::uint32_t offset = 0;
    if (!AllocateSpace(offset, size)) return false;
    bool ok = false;
    if (record->Kind() == cGZPersistDBSerialRecord::kKindWrite)
        ok = static_cast<cGZDBWriteRecordPackedFile*>(record)->WriteRecordToParent(offset, size);
    if (ok) {
        index_[key] = RecordDataInfo{offset, size};
        IncrementTypeAndGroupUse(key.type, key.group, true);
        indexDirty_ = true;
    } else {
        FreeSpace(offset, size);
    }
    return ok;
}

std::uint32_t cGZDBSegmentPackedFile::DoReadRecord(const cGZPersistResourceKey& key, void* buffer,
                                                   std::uint32_t& size) {
    std::uint32_t result = 0xffffffffu, copied = 0;
    if (openFlags_ & 1) {
        bool blocked = false;
        if (openFlags_ & 2) {
            auto open = openRecords_.find(key);
            if (open != openRecords_.end() && (open->second->GetAccessFlags() & 2)) blocked = true;
        }
        if (!blocked) {
            auto it = index_.find(key);
            if (it != index_.end()) {
                bool isCompressed = compressed_.find(key) != compressed_.end();
                if (!buffer) {
                    result = isCompressed ? compressed_[key] : it->second.size;
                } else if (!isCompressed) {
                    std::uint32_t n = std::min(size, it->second.size);
                    std::uint32_t at = it->second.offset + fileStartingOffset_;
                    result = 0;
                    if (file_.Seek(static_cast<int>(at), 0) == at && file_.Read(buffer, n)) copied = result = n;
                } else {
                    std::vector<std::uint8_t> plain;
                    result = 0;
                    if (DecompressRecord(it->second.offset, it->second.size, plain) && plain.size() <= size) {
                        std::memcpy(buffer, plain.data(), plain.size());
                        copied = result = static_cast<std::uint32_t>(plain.size());
                    }
                }
            }
        }
    }
    size = copied;
    return result;
}

bool cGZDBSegmentPackedFile::DoWriteRecord(const cGZPersistResourceKey& key, void* buffer, std::uint32_t size) {
    if (!(openFlags_ & 2)) return false;
    if (openRecords_.find(key) != openRecords_.end()) return false;
    auto old = index_.find(key);
    if (old != index_.end()) {
        RemoveIndexedRecord(key, old);
    }
    const auto* data = static_cast<const std::uint8_t*>(buffer);
    std::vector<std::uint8_t> packed;
    bool storePacked = false;
    if (ShouldRecordBeCompressed(key, size)) {
        if (CompressPayload(data, size, packed) && static_cast<float>(packed.size()) / static_cast<float>(size) < 0.8f)
            storePacked = true;
    }
    const std::uint8_t* payload = storePacked ? packed.data() : data;
    std::uint32_t payloadSize = storePacked ? static_cast<std::uint32_t>(packed.size()) : size;
    std::uint32_t offset = 0;
    if (!AllocateSpace(offset, payloadSize)) return false;
    if (!WriteFileSpan(payload, offset, payloadSize)) {
        FreeSpace(offset, payloadSize);
        return false;
    }
    if (storePacked) SetRecordCompressed(key, true, size);
    index_[key] = RecordDataInfo{offset, payloadSize};
    IncrementTypeAndGroupUse(key.type, key.group, true);
    indexDirty_ = true;
    return true;
}

bool cGZDBSegmentPackedFile::DoReadRecordToFile(const cGZPersistResourceKey& key, const cIGZString& path) {
    std::uint32_t size = DoGetRecordSize(key);
    if (size == 0xffffffffu) return false;
    std::error_code ec;
    std::filesystem::path p(path.ToChar());
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    cRZFile out(path);
    if (!out.Open(2, 3, 1)) { cRZFile::Remove(path); return false; }
    cIGZPersistDBRecord* record = nullptr;
    bool ok = OpenRecord(key, &record, 1);
    if (!ok) { out.Close(); cRZFile::Remove(path); return false; }
    void* raw = nullptr;
    ok = record->QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &raw);
    if (ok) {
        auto* serial = static_cast<cIGZPersistDBSerialRecord*>(raw);
        out.SetLength(0);
        out.SeekToPosition(0);
        std::vector<std::uint8_t> chunk(0x400);
        std::uint32_t left = size;
        while (left != 0 && ok) {
            std::uint32_t n = std::min<std::uint32_t>(left, 0x400);
            ok = serial->GetFieldVoid(chunk.data(), n) && out.Write(chunk.data(), n);
            left -= n;
        }
        out.Close();
        CloseRecord(record);
        serial->Release();
    }
    record->Release();
    if (!ok) cRZFile::Remove(path);
    return ok;
}

bool cGZDBSegmentPackedFile::DoWriteRecordFromFile(const cGZPersistResourceKey& key, const cIGZString& path) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 2) || openRecords_.find(key) != openRecords_.end()) return false;
    auto old = index_.find(key);
    if (old != index_.end()) RemoveIndexedRecord(key, old);
    cRZFile in(path);
    if (!in.Open(1, 2, 1)) return false;
    std::uint32_t size = in.Length();
    if (size == 0) return false;
    std::vector<std::uint8_t> data(size);
    if (!in.Read(data.data(), size)) return false;
    return DoWriteRecord(key, data.data(), size);
}

// ------------------------------------------------------------------------------ raw record access

std::uint32_t cGZDBSegmentPackedFile::GetRecordSizeRaw(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 3)) return 0xffffffffu;
    auto it = index_.find(key);
    return it == index_.end() ? 0xffffffffu : it->second.size;
}

std::uint32_t cGZDBSegmentPackedFile::ReadRecordRaw(const cGZPersistResourceKey& key, void* buffer,
                                                    std::uint32_t& size) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    std::uint32_t result = 0xffffffffu;
    if (!(openFlags_ & 1)) return result;
    if (openFlags_ & 2) {
        auto open = openRecords_.find(key);
        if (open != openRecords_.end() && (open->second->GetAccessFlags() & 2)) {
            size = 0;
            return result;
        }
    }
    auto it = index_.find(key);
    if (it == index_.end()) return result;
    if (!buffer) {
        auto c = compressed_.find(key);
        return c != compressed_.end() ? c->second : it->second.size;
    }
    std::uint32_t n = std::min(size, it->second.size);
    std::uint32_t at = it->second.offset + fileStartingOffset_;
    result = 0;
    if (file_.Seek(static_cast<int>(at), 0) == at && file_.Read(buffer, n)) result = n;
    return result;
}

bool cGZDBSegmentPackedFile::WriteRecordRaw(const cGZPersistResourceKey& key, const void* buffer,
                                            std::uint32_t size) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 2) || openRecords_.find(key) != openRecords_.end()) return false;
    auto old = index_.find(key);
    if (old != index_.end()) RemoveIndexedRecord(key, old);
    std::uint32_t offset = 0;
    if (!AllocateSpace(offset, size)) return false;
    if (!WriteFileSpan(buffer, offset, size)) {
        FreeSpace(offset, size);
        return false;
    }
    index_[key] = RecordDataInfo{offset, size};
    IncrementTypeAndGroupUse(key.type, key.group, true);
    indexDirty_ = true;
    return true;
}

std::uint32_t cGZDBSegmentPackedFile::GetRecordPosition(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 3)) return 0xffffffffu;
    auto it = index_.find(key);
    return it == index_.end() ? 0xffffffffu : it->second.offset;
}

// ------------------------------------------------------------------------------ space management

bool cGZDBSegmentPackedFile::AllocateSpace(std::uint32_t& offset, std::uint32_t size) {
    if (!(openFlags_ & 2)) return false;
    std::uint32_t length = file_.Length();
    if (holeUseFlags_ & kReuseHoles) {
        for (auto it = holes_.begin(); it != holes_.end(); ++it) {
            if (size <= it->size) {
                HoleRecord hole = *it;
                offset = hole.offset;
                holes_.erase(it);
                FreeSpace(hole.offset + size, hole.size - size);
                return true;
            }
        }
    }
    if (!file_.SetLength(length + size)) return false;
    offset = length;
    return true;
}

bool cGZDBSegmentPackedFile::FreeSpace(std::uint32_t offset, std::uint32_t size) {
    if (offset != 0 && size != 0) holes_.insert(HoleRecord{offset, size});  // same-size holes collapse
    holesDirty_ = true;
    return true;
}

bool cGZDBSegmentPackedFile::GetFreeSpaceMetrics(std::uint32_t& holeCount, std::uint32_t& freeBytes) {
    holeCount = 0;
    freeBytes = 0;
    if (!(openFlags_ & 3)) return false;
    for (const HoleRecord& h : holes_) {
        ++holeCount;
        freeBytes += h.size;
    }
    return true;
}

// ------------------------------------------------------------------------ misc configuration

bool cGZDBSegmentPackedFile::SetFileStartingOffset(std::uint32_t offset) {
    bool closed = (openFlags_ & 3) == 0;
    if (closed) fileStartingOffset_ = offset;
    return closed;
}

bool cGZDBSegmentPackedFile::SetPath(const cIGZString& path) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (openFlags_ & 3) return false;
    path_.AssignFromChars(path.Data(), path.Strlen());
    return true;
}

void cGZDBSegmentPackedFile::GetPath(cIGZString& out) const { out.AssignFromChars(path_.Data(), path_.Strlen()); }

void cGZDBSegmentPackedFile::GetCurrentUserVersion(std::uint32_t& major, std::uint32_t& minor) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    major = header_.userMajor;
    minor = header_.userMinor;
}

void cGZDBSegmentPackedFile::GetNextUserVersion(std::uint32_t& major, std::uint32_t& minor) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    major = nextUserMajor_;
    minor = nextUserMinor_;
}

void cGZDBSegmentPackedFile::SetNextUserVersion(std::uint32_t major, std::uint32_t minor) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    nextUserMajor_ = major;
    nextUserMinor_ = minor;
}

bool cGZDBSegmentPackedFile::GetModificationTime(std::uint32_t& created, std::uint32_t& modified) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!(openFlags_ & 3)) return false;
    created = header_.created;
    modified = header_.modified;
    return true;
}

bool cGZDBSegmentPackedFile::SetFileBuffering(std::uint32_t readSize, std::uint32_t writeSize) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!file_.SetBuffering(readSize, writeSize)) return false;
    readBuffering_ = readSize;
    writeBuffering_ = writeSize;
    return true;
}

bool cGZDBSegmentPackedFile::SetHoleUseFlags(int flags) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    holeUseFlags_ = static_cast<std::uint32_t>(flags);
    return true;
}

bool cGZDBSegmentPackedFile::IsRecordOpen(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return openRecords_.find(key) != openRecords_.end();
}

std::uint32_t cGZDBSegmentPackedFile::GetRecordCount(cIGZPersistResourceKeyFilter* filter) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (!filter) return static_cast<std::uint32_t>(index_.size());
    return GetResourceKeyList(nullptr, filter);
}

std::uint32_t cGZDBSegmentPackedFile::GetResourceKeyList(cIGZPersistResourceKeyList* list,
                                                         cIGZPersistResourceKeyFilter* filter) {
    if (!(openFlags_ & 3)) return 0;
    std::uint32_t count = 0;
    for (const auto& [key, info] : index_) {
        (void)info;
        if (key.type == kDirectoryRecordType) continue;
        if (filter && !filter->IsKeyIncluded(key)) continue;
        if (list) list->Insert(key);
        ++count;
    }
    return count;
}

void cGZDBSegmentPackedFile::IncrementTypeAndGroupUse(std::uint32_t type, std::uint32_t group, bool increment) {
    if (!countTypesAndGroups_) return;
    auto bump = [&](std::unordered_map<std::uint32_t, std::int32_t>& use, std::uint32_t id) {
        auto it = use.find(id);
        if (it == use.end()) {
            if (increment) use[id] = 1;
        } else if (increment) {
            ++it->second;
        } else if (--it->second <= 0) {
            use.erase(it);
        }
    };
    bump(typeUse_, type);
    bump(groupUse_, group);
}

std::uint32_t cGZDBSegmentPackedFile::GetUsedTypeCount() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return static_cast<std::uint32_t>(typeUse_.size());
}

std::uint32_t cGZDBSegmentPackedFile::GetUsedGroupCount() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return static_cast<std::uint32_t>(groupUse_.size());
}

void cGZDBSegmentPackedFile::EnableUsedTypeAndGroupCounting(bool enable) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    countTypesAndGroups_ = enable;
    typeUse_.clear();
    groupUse_.clear();
    if (enable)
        for (const auto& [key, info] : index_) {
            (void)info;
            IncrementTypeAndGroupUse(key.type, key.group, true);
        }
}

bool cGZDBSegmentPackedFile::VerifyDatabase() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return (openFlags_ & 3) != 0;  // (sic) only reports whether it is open
}

void cGZDBSegmentPackedFile::EnableCompressionForAllRecords(bool enable) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    compressAll_ = enable;
}

void cGZDBSegmentPackedFile::EnableCompressionByType(std::uint32_t type, bool enable) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (enable) compressTypes_.insert(type);
    else compressTypes_.erase(type);
}

void cGZDBSegmentPackedFile::EnableCompressionByGroup(std::uint32_t group, bool enable) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (enable) compressGroups_.insert(group);
    else compressGroups_.erase(group);
}

bool cGZDBSegmentPackedFile::GetRecordCompressed(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return compressed_.find(key) != compressed_.end();
}

void cGZDBSegmentPackedFile::SetRecordCompressed(const cGZPersistResourceKey& key, bool compressed,
                                                 std::uint32_t uncompressedSize) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    auto it = compressed_.find(key);
    if (compressed) {
        if (it != compressed_.end()) {
            it->second = uncompressedSize;
            compressedSetDirty_ = true;
            return;
        }
        compressed_.emplace(key, uncompressedSize);
        compressedSetDirty_ = true;
    } else if (it != compressed_.end()) {
        compressed_.erase(it);
        compressedSetDirty_ = true;
    }
}

std::uint32_t cGZDBSegmentPackedFile::GetCompressedRecordLength(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (compressed_.find(key) == compressed_.end()) return 0xffffffffu;
    return GetRecordSizeRaw(key);
}

// ----------------------------------------------------------------------- compaction and copying

bool cGZDBSegmentPackedFile::CompactDatabase() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (compacting_) return false;
    compacting_ = true;
    struct Reset { bool& f; ~Reset() { f = false; } } reset{compacting_};

    if (!(openFlags_ & 3)) return false;
    if (holes_.empty()) {
        if (header_.holeCount == 0) return true;
        FreeSpace(header_.holeOffset, header_.holeSize);
        header_.holeCount = 0;
        header_.holeSize = 0;
        holesDirty_ = false;
        if (holes_.empty()) return false;
    }
    if (!(openFlags_ & 2) || !openRecords_.empty()) return false;
    if (header_.holeCount != 0) {
        FreeSpace(header_.holeOffset, header_.holeSize);
        header_.holeCount = 0;
        header_.holeSize = 0;
        holesDirty_ = false;
    }
    if (index_.empty()) {
        compressed_.clear();
        header_.indexCount = 0;
        header_.indexSize = 0;
        indexDirty_ = false;
        file_.SetLength(0x60);
        return true;
    }
    // Copy every record into a fresh package next to the original, then swap the files.
    cRZString dir;
    cRZFile::GetPathDirectory(path_, dir);
    cRZString scratch("GZSegmentPackedFile_");
    cRZString oldName("GZSegmentPackedFile_");
    if (!cRZFile::MakeTempFileName(dir, scratch, nullptr)) return false;
    if (!cRZFile::MakeTempFileName(dir, oldName, nullptr)) { cRZFile::Remove(scratch); return false; }
    if (cRZFile::FileExists(oldName)) cRZFile::Remove(oldName);

    auto* fresh = new cGZDBSegmentPackedFile();
    fresh->AddRef();
    bool ok = false;
    if (fresh->Init() && fresh->SetPath(scratch) && fresh->Open(true, true)) {
        if (CopyDatabaseRecords(static_cast<cIGZPersistDBSegment*>(fresh), nullptr, false, true) != -1) {
            fresh->SetHoleUseFlags(static_cast<int>(fresh->holeUseFlags_ & ~1u));
            ok = fresh->Close();
            if (ok) {
                holeUseFlags_ &= ~1u;
                ok = Close();
                if (ok) ok = cRZFile::Rename(path_, oldName);
                if (ok) {
                    if (!cRZFile::Rename(scratch, path_)) {
                        cRZFile::Rename(oldName, path_);
                        ok = false;
                    }
                }
                if (ok) ok = Open(true, true);
            }
        }
        if (fresh->IsOpen()) fresh->Close();
    }
    fresh->Shutdown();
    fresh->Release();
    if (cRZFile::FileExists(oldName)) cRZFile::Remove(oldName);
    if (cRZFile::FileExists(scratch)) cRZFile::Remove(scratch);
    return ok;
}

// Direction: `intoThis == false` copies from this segment into `other`; true copies from `other`
// into this one. `keepSource == false` deletes the record from its source afterwards. Packed files
// copy the stored (compressed) bytes directly and keep the compressed flag.
bool cGZDBSegmentPackedFile::CopyDatabaseRecord(cIGZPersistDBSegment* other, const cGZPersistResourceKey& key,
                                                bool intoThis, bool keepSource) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    cIGZPersistDBSegment* self = static_cast<cIGZPersistDBSegment*>(this);
    cIGZPersistDBSegment* source = intoThis ? other : self;
    cIGZPersistDBSegment* dest = intoThis ? self : other;
    if (!source->TestForRecord(key)) return false;

    void* rawSource = nullptr;
    void* rawDest = nullptr;
    cGZDBSegmentPackedFile* srcPacked = nullptr;
    cGZDBSegmentPackedFile* dstPacked = nullptr;
    if (source->QueryInterface(kGZIID_cGZDBSegmentPackedFile, &rawSource)) {
        srcPacked = static_cast<cGZDBSegmentPackedFile*>(rawSource);
        srcPacked->Release();
    }
    if (dest->QueryInterface(kGZIID_cGZDBSegmentPackedFile, &rawDest)) {
        dstPacked = static_cast<cGZDBSegmentPackedFile*>(rawDest);
        dstPacked->Release();
    }
    if (srcPacked && dstPacked && srcPacked->GetRecordCompressed(key)) {
        std::uint32_t storedSize = srcPacked->GetRecordSizeRaw(key);
        std::uint32_t plainSize = srcPacked->GetCompressedRecordLength(key);
        std::uint32_t storedUncompressed = srcPacked->GetRecordCompressed(key) ? srcPacked->compressed_[key] : 0;
        (void)plainSize;
        if (storedSize != 0xffffffffu) {
            std::vector<std::uint8_t> raw(storedSize ? storedSize : 1);
            std::uint32_t n = storedSize;
            if (srcPacked->ReadRecordRaw(key, raw.data(), n) != 0xffffffffu &&
                dstPacked->WriteRecordRaw(key, raw.data(), n)) {
                dstPacked->SetRecordCompressed(key, true, storedUncompressed);
                if (!keepSource) source->DeleteRecord(key);
                return true;
            }
        }
    }
    std::uint32_t size = source->GetRecordSize(key);
    if (size == 0xffffffffu) return false;
    std::vector<std::uint8_t> data(size ? size : 1);
    std::uint32_t n = size;
    if (source->ReadRecord(key, data.data(), n) == 0xffffffffu) return false;
    if (!dest->WriteRecord(key, data.data(), n)) return false;
    if (!keepSource) source->DeleteRecord(key);
    return true;
}

int cGZDBSegmentPackedFile::CopyDatabaseRecords(cIGZPersistDBSegment* other, cIGZPersistResourceKeyFilter* filter,
                                                bool intoThis, bool keepSource) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    cIGZPersistDBSegment* self = static_cast<cIGZPersistDBSegment*>(this);
    self->Lock();
    other->Lock();
    auto* keys = new cGZPersistResourceKeyList();
    keys->AddRef();
    if (intoThis) other->GetResourceKeyList(keys, filter);
    else GetResourceKeyList(keys, filter);
    int copied = 0;
    for (std::uint32_t i = 0; i < keys->Size(); ++i)
        if (CopyDatabaseRecord(other, *keys->GetKey(i), intoThis, keepSource)) ++copied;
    keys->Release();
    other->Unlock();
    self->Unlock();
    return copied;
}

int cGZDBSegmentPackedFile::CopyDatabaseRecords(const cIGZString& path, cIGZPersistResourceKeyFilter* filter,
                                                bool intoThis, bool keepSource) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    auto* other = new cGZDBSegmentPackedFile();
    other->AddRef();
    int result = -1;
    if (other->Init() && other->SetPath(path) && other->Open(true, !intoThis || !keepSource)) {
        result = CopyDatabaseRecords(static_cast<cIGZPersistDBSegment*>(other), filter, intoThis, keepSource);
        other->Close();
    }
    other->Shutdown();
    other->Release();
    return result;
}
