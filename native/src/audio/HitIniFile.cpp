#include "audio/HitIniFile.h"

#include <cstdlib>

#include <cstdio>

#include <vector>

#include "rz/RZConvert.h"
#include "rz/cRZFile.h"

namespace sc4audio {

namespace {
inline std::uint32_t ToUint(const cRZString& text) { return RZ::ConvertToUint(text.ToChar()); }

// Releases a COM-style pointer when it goes out of scope.
template <class T>
struct Ref {
    T* ptr = nullptr;
    ~Ref() { if (ptr) ptr->Release(); }
    void** Out() { return reinterpret_cast<void**>(&ptr); }
};

constexpr std::size_t kTagLength = 15;
}  // namespace

cHitIniFile::cHitIniFile(IHitIniHost* host, cIGZFile* file) : cRZIniFile(file), host_(host) {
    if (cIGZSndSys* sys = host_->SoundSystem()) {
        if (const cIGZString* directory = sys->DataDirectory()) dataDirectory_.AssignFromIGZString(*directory);
    }
}

bool cHitIniFile::SetFilename(const cIGZString& path) {
    SetPath(cRZString(path));
    return true;
}

bool cHitIniFile::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cIHitIniFile) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cHitIniFile::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

void cHitIniFile::AddLineToStringMap(const cRZString& key, const cRZString& value, void* map) {
    (*static_cast<StringMap*>(map))[ToUint(key)] = value;
}

cHitIniFile::StringMap cHitIniFile::ReadSection(const char* section) {
    StringMap entries;
    EnumEntries(cRZString(section), AddLineToStringMap, &entries);
    return entries;
}

cRZString cHitIniFile::ResolveDataPath(cRZString path) const {
    path.Trim();
    if (path.BeginsWith(kHitDataDirectoryTag, kTagLength)) {
        cRZString rest = path.Mid(kTagLength, std::string::npos);
        path = cRZString((dataDirectory_.str() + rest.str()).c_str());
        cRZString adjusted = path;
        SC4AdjustPathSeparatorsForPlatform(path, adjusted);
        return adjusted;
    }
    return path;
}

bool cHitIniFile::EnumDefaultEntries() {
    if (cIGZSndMgr* manager = host_->SoundManager()) {
        cRZString value;
        if (ReadEntry(cRZString("Config"), cRZString("DefaultSndDataSourcePlayer2DClassId"), value))
            manager->SetDefaultSndDataSourcePlayer2DClassId(ToUint(value));
        if (ReadEntry(cRZString("Config"), cRZString("DefaultSndDataSourcePlayer3DClassId"), value))
            manager->SetDefaultSndDataSourcePlayer3DClassId(ToUint(value));
    }
    if (cIGZSndSys* system = host_->SoundSystem()) {
        // lines of the section "SndFileDefaultDataSourceClassIds": "classId,extension"
        StringMap lines = ReadSection("SndFileDefaultDataSourceClassIds");
        for (auto& line : lines) {
            cRZString value = line.second;
            const cRZString classId = value.SplitToken(",");
            system->SetExtensionDefaultDataSourceClassId(value, ToUint(classId));
        }
    }
    InitPackedDBSegments();
    InitDBSegments();
    InitTrackDataBlocks();
    InitHitManTypeGroups();
    InitDataSourceCreators();
    InitSndCreators();
    InitResourceFactories();
    return true;
}

bool cHitIniFile::InitPackedDBSegments() {
    for (auto& entry : ReadSection("PackedDBSegments")) ProcessPackedDBSegmentEntry(entry.first, entry.second);
    return true;
}

bool cHitIniFile::InitTrackDataBlocks() {
    for (auto& entry : ReadSection("TrackDataBlocks")) {
        cRZString path((dataDirectory_.str() + entry.second.str()).c_str());
        cRZString adjusted = path;
        SC4AdjustPathSeparatorsForPlatform(path, adjusted);
        ProcessTrackDataBlockEntry(adjusted);
    }
    return true;
}

bool cHitIniFile::ProcessTrackDataBlockEntry(const cRZString& path) {
    cIHitMan* hitMan = host_->HitManager();
    const bool loaded = hitMan && hitMan->LoadHitFile(path);
    if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "audio: hit file %s %s\n", path.ToChar(), loaded ? "loaded" : hitMan ? "FAILED" : "skipped (no hit manager)");
    return loaded;
}

bool cHitIniFile::InitHitManTypeGroups() {
    cIHitMan* hitMan = host_->HitManager();
    for (auto& entry : ReadSection("HitManTypeGroups")) {
        cRZString value = entry.second;
        // comma separated list; each member is associated with the entry's key
        for (cRZString token = value.SplitToken(","); token.Strlen() != 0; token = value.SplitToken(","))
            if (hitMan) hitMan->AddTypeToGroupAssociation(entry.first, ToUint(token));
        if (hitMan) hitMan->AddTypeToGroupAssociation(entry.first, ToUint(value));
    }
    return true;
}

void cHitIniFile::DataSourceCreatorsCallback(const cRZString& key, const cRZString& value, void* self) {
    auto* ini = static_cast<cHitIniFile*>(self);
    cRZString rest = value;
    const cRZString classIdText = rest.SplitToken(",");
    const std::uint32_t classId = ToUint(classIdText);
    const std::uint32_t id = ToUint(rest);
    Ref<cIGZSndDataSourceCreator> creator;
    ini->host_->CreateInstance(classId, kGZIID_cIGZSndDataSourceCreator, creator.Out());
    if (cIGZSndMgr* manager = ini->host_->SoundManager()) manager->RegisterSndDataSourceCreator(creator.ptr, id);
    ini->dataSourceSections_[id] = key;
}

void cHitIniFile::DataSourceEntryCallback(const cRZString& key, const cRZString& value, void* context) {
    auto* ctx = static_cast<DataSourceEntryContext*>(context);
    cIGZSndMgr* manager = ctx->self->host_->SoundManager();
    if (!manager) return;
    cRZString definition = value;
    Ref<cIGZSndDataSourceCreator> creator;
    manager->GetSndDataSourceCreator(ctx->creatorId, kGZIID_cIGZSndDataSourceCreator, creator.Out());
    creator.ptr->AddSndDataSourceDef(ToUint(key), definition);
}

bool cHitIniFile::InitDataSourceCreators() {
    EnumEntries(cRZString("SndDataSourceCreators"), DataSourceCreatorsCallback, this);
    for (auto& section : dataSourceSections_) {
        DataSourceEntryContext context{this, section.first};
        EnumEntries(section.second, DataSourceEntryCallback, &context);
    }
    return true;
}

void cHitIniFile::SoundDefListCallback(const cRZString& key, const cRZString& value, void* creator) {
    Ref<cIGZSoundCreatorSoundDefRegistrar> registrar;
    static_cast<cIGZSoundCreator*>(creator)->QueryInterface(kGZIID_cIGZSoundCreatorSoundDefRegistrar, registrar.Out());
    registrar.ptr->AddSoundDef(ToUint(key), value, 0, 0);
}

// Each entry creates the sound creator whose class id is the key; its value names a section of sound
// definitions (the text before the first comma, or the whole value without one) that is then enumerated
// into the creator. SplitToken returns nothing when the comma is absent, as in the game's own
// "0xdc45dcb1=TrackDefSegments".
bool cHitIniFile::InitSndCreators() {
    for (auto& entry : ReadSection("SndCreators")) {
        cRZString value = entry.second;
        cRZString section = value.SplitToken(",");
        if (section.Strlen() == 0) section = value;
        Ref<cIGZSoundCreator> creator;
        if (!host_->CreateInstance(entry.first, kGZIID_cIGZSoundCreator, creator.Out()) || !creator.ptr) continue;
        if (cIGZSndMgr* manager = host_->SoundManager()) {
            manager->RegisterSndCreator(creator.ptr);
            if (section.Strlen() != 0) EnumEntries(section, SoundDefListCallback, creator.ptr);
        }
    }
    return true;
}

bool cHitIniFile::InitResourceFactories() {
    for (auto& entry : ReadSection("ResourceFactories")) {
        if (cIGZPersistResourceManager* manager = host_->ResourceManager())
            manager->RegisterObjectFactory(ToUint(entry.second), entry.first, nullptr);
    }
    return true;
}

// Returns true when the file is missing (the segment is optional) or was registered, false when the segment
// class could not be created.
bool cHitIniFile::ProcessPackedDBSegmentEntry(std::uint32_t segmentId, const cRZString& value) {
    const std::uint32_t classId = ToUint(value);
    cRZString fields = value;
    fields.SplitToken(",");                                   // class id
    const cRZString typeSection = fields.SplitToken(",");     // section of handled type ids
    cRZString path = ResolveDataPath(fields);
    if (!host_->FileExists(path.str())) return true;
    Ref<cIGZUnknown> created;
    if (!host_->CreateInstance(classId, kGZIID_cGZDBSegmentPackedFile, created.Out())) return false;
    Ref<cIGZPersistDBSegment> segment;
    created.ptr->QueryInterface(kGZIID_cIGZPersistDBSegment, segment.Out());
    host_->InitPackedSegment(segment.ptr, segmentId, path.str(), false);
    segment.ptr->Open(true, false);
    if (cIHitMan* hitMan = host_->HitManager()) hitMan->AddDBSegment(segmentId, segment.ptr);
    if (cIGZPersistResourceManager* manager = host_->ResourceManager()) manager->RegisterDBSegmentFront(*segment.ptr);
    for (auto& type : ReadSection(typeSection.ToChar())) packedHandled_.insert(type.first);
    return true;
}

bool cHitIniFile::ProcessDBSegmentsEntryCreate(std::uint32_t segmentId, const cRZString& value) {
    Ref<cIHitDirectoryDBSegment> segment;
    if (!host_->CreateInstance(ToUint(value), kGZIID_cIHitDirectoryDBSegment, segment.Out())) return false;
    segment.ptr->SetOptions(true, true);
    segment.ptr->SetAllowTextAsKeys(allowTextAsKeys_);
    if (cIHitMan* hitMan = host_->HitManager()) hitMan->AddDBSegment(segmentId, segment.ptr->GetSegment());
    return true;
}

// "type:extension[,type:extension...]" (stage 4c): every member binds its type to the extension in the group's
// directory segment and associates the type with the group (hit manager slot 46); the remainder after the last
// comma is a member too. The association does not depend on the segment: without a directory segment class (loose
// sound files, none in the shipped data) the types still resolve to the group in the game's packed files.
bool cHitIniFile::ProcessDBSegmentDirectoryFilesEntry(std::uint32_t segmentId, const cRZString& value) {
    cIHitMan* hitMan = host_->HitManager();
    if (!hitMan) return false;
    Ref<cIHitDirectoryDBSegment> segment;
    hitMan->FindDBSegment(segmentId, kGZIID_cIHitDirectoryDBSegment, segment.Out());
    cIHitDirectoryFileMap* files = segment.ptr ? segment.ptr->GetFileMap() : nullptr;
    auto bind = [&](cRZString member) {
        const std::uint32_t type = ToUint(member);
        member.SplitToken(":");
        if (files) files->AddFile(type, member);
        hitMan->AddTypeToGroupAssociation(type, segmentId);
    };
    cRZString rest = value;
    for (cRZString token = rest.SplitToken(","); token.Strlen() != 0; token = rest.SplitToken(",")) bind(token);
    bind(rest);
    return true;
}

// Returns true when the hit manager is missing or the segment initialised; false when the segment rejects the path.
bool cHitIniFile::ProcessDBSegmentsEntryInit(std::uint32_t segmentId, const cRZString& value) {
    cRZString fields = value;
    fields.SplitToken(",");
    cRZString path = ResolveDataPath(fields);
    cIHitMan* hitMan = host_->HitManager();
    if (!hitMan) return true;
    Ref<cIGZPersistDBSegment> segment;
    hitMan->FindDBSegment(segmentId, kGZIID_cIGZPersistDBSegment, segment.Out());
    if (!segment.ptr || !host_->InitPackedSegment(segment.ptr, segmentId, path.str(), true)) return false;
    if (cIGZPersistResourceManager* manager = host_->ResourceManager()) manager->RegisterDBSegmentBack(*segment.ptr);
    return true;
}

bool cHitIniFile::InitDBSegments() {
    cRZString text;
    if (ReadEntry(cRZString("Config"), cRZString("AllowTextAsKey"), text)) allowTextAsKeys_ = text.str() == "true";
    const StringMap segments = ReadSection("DBSegments");
    const StringMap directoryFiles = ReadSection("DBSegmentDirectoryFiles");
    for (auto& entry : segments)
        if (!packedHandled_.count(entry.first)) ProcessDBSegmentsEntryCreate(entry.first, entry.second);
    for (auto& entry : directoryFiles)
        if (!packedHandled_.count(entry.first)) ProcessDBSegmentDirectoryFilesEntry(entry.first, entry.second);
    for (auto& entry : segments)
        if (!packedHandled_.count(entry.first)) ProcessDBSegmentsEntryInit(entry.first, entry.second);
    return true;
}

}  // namespace sc4audio
