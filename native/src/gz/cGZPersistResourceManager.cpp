#include "gz/cGZPersistResourceManager.h"

#include <algorithm>

#include "gz/cGZPersistResourceKeyList.h"

// =============================================================================== resource base

bool cGZPersistResource::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZPersistResource) {
        *out = static_cast<cIGZPersistResource*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cGZPersistResource::Release() {
    std::int32_t now = --refCount_;
    if (now == 0) {
        delete this;
        return 0;
    }
    return static_cast<std::uint32_t>(now);
}

// ============================================================================== factory base

bool cGZPersistResourceFactory::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZPersistResourceFactory) {
        *out = static_cast<cIGZPersistResourceFactory*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cGZPersistResourceFactory::AddRef() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    return ++refCount_;
}

std::uint32_t cGZPersistResourceFactory::Release() {
    std::unique_lock<std::recursive_mutex> guard(mutex_);
    if (refCount_ < 2) {
        guard.unlock();
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cGZPersistResourceFactory::Read(cIGZPersistResource& resource, cIGZPersistDBRecord& record) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    void* raw = nullptr;
    if (!record.QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &raw)) return false;
    auto* serial = static_cast<cIGZPersistDBSerialRecord*>(raw);
    bool ok = SerialRead(resource, *serial);
    serial->Release();
    return ok;
}

bool cGZPersistResourceFactory::Write(const cIGZPersistResource& resource, cIGZPersistDBRecord& record) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    void* raw = nullptr;
    if (!record.QueryInterface(kGZIID_cIGZPersistDBSerialRecord, &raw)) return false;
    auto* serial = static_cast<cIGZPersistDBSerialRecord*>(raw);
    bool ok = SerialWrite(const_cast<cIGZPersistResource&>(resource), *serial);
    serial->Release();
    return ok;
}

bool cGZPersistResourceFactory::CreateInstance(cIGZPersistDBRecord& record, std::uint32_t iid, void** out,
                                               std::uint32_t flags, cIGZUnknown* owner) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    void* raw = nullptr;
    bool created;
    if (flags == 0 && owner == nullptr)
        created = CreateInstance(record.GetKey().type, kGZIID_cIGZPersistResource, &raw);
    else
        created = CreateInstance(record.GetKey().type, kGZIID_cIGZPersistResource, &raw, flags, owner);
    if (!created) return false;
    auto* resource = static_cast<cIGZPersistResource*>(raw);
    resource->SetKey(record.GetKey());
    bool ok = Read(*resource, record) && resource->QueryInterface(iid, out);
    resource->Release();
    return ok;
}

// ================================================================================ binary blob

bool cGZPersistBinaryResource::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZPersistBinaryResource) {
        *out = static_cast<cIGZPersistBinaryResource*>(this);
        AddRef();
        return true;
    }
    return cGZPersistResource::QueryInterface(iid, out);
}

bool cGZPersistBinaryResource::SetData(const void* data, std::uint32_t length) {
    data_.clear();
    if (data && length) data_.assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + length);
    return true;
}

bool cGZPersistBinaryResourceFactory::CreateInstance(std::uint32_t type, std::uint32_t iid, void** out) {
    if (type != kGZResourceTypeBinary) return false;
    auto* resource = new cGZPersistBinaryResource();
    resource->AddRef();
    bool ok = resource->QueryInterface(iid, out);
    resource->Release();
    return ok;
}

bool cGZPersistBinaryResourceFactory::SerialRead(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) {
    if (resource.GetType() != kGZResourceTypeBinary) return false;
    void* raw = nullptr;
    if (!resource.QueryInterface(kGZIID_cIGZPersistBinaryResource, &raw)) return false;
    auto* blob = static_cast<cIGZPersistBinaryResource*>(raw);
    std::uint32_t size = record.GetSize();
    bool ok;
    if (size == 0) {
        ok = blob->SetData(nullptr, 0);
    } else {
        std::vector<std::uint8_t> bytes(size);
        ok = record.GetFieldVoid(bytes.data(), size) && blob->SetData(bytes.data(), size);
    }
    blob->Release();
    return ok;
}

bool cGZPersistBinaryResourceFactory::SerialWrite(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) {
    if (resource.GetType() != kGZResourceTypeBinary) return false;
    void* raw = nullptr;
    if (!resource.QueryInterface(kGZIID_cIGZPersistBinaryResource, &raw)) return false;
    auto* blob = static_cast<cIGZPersistBinaryResource*>(raw);
    void* data = blob->GetData();
    std::uint32_t length = blob->GetDataLength();
    bool ok = false;
    if (data && length) ok = record.SetFieldVoid(data, length);
    else if (!data && !length) ok = true;               // an empty blob writes an empty record
    // (data xor length set: the original reports failure)
    blob->Release();
    return ok;
}

// ============================================================================= cache strategy

bool cGZPersistCacheStrategyBasic::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZPersistCacheStrategy) {
        *out = static_cast<cIGZPersistCacheStrategy*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cGZPersistCacheStrategyBasic::AddRef() {
    std::lock_guard<std::recursive_mutex> guard(stateMutex_);
    return static_cast<std::uint32_t>(++refCount_);
}

std::uint32_t cGZPersistCacheStrategyBasic::Release() {
    std::unique_lock<std::recursive_mutex> guard(stateMutex_);
    if (refCount_ < 2) {
        guard.unlock();
        delete this;
        return 0;
    }
    return static_cast<std::uint32_t>(--refCount_);
}

void cGZPersistCacheStrategyBasic::Shutdown() {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    FlushCache();
}

void cGZPersistCacheStrategyBasic::FlushCache() {
    for (auto& [key, resource] : cache_) {
        (void)key;
        if (resource) resource->Release();
    }
    cache_.clear();
}

void cGZPersistCacheStrategyBasic::SetKeyFilter(cIGZPersistResourceKeyFilter* filter) { filter_ = filter; }

bool cGZPersistCacheStrategyBasic::GetResource(const cGZPersistResourceKey& key, cIGZPersistResource** out) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    auto it = cache_.find(key);
    if (it == cache_.end() || !it->second) return false;
    *out = it->second;
    it->second->AddRef();
    return true;
}

bool cGZPersistCacheStrategyBasic::CacheResource(const cGZPersistResourceKey& key, cIGZPersistResource& resource) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    if (cache_.find(key) != cache_.end()) return false;
    resource.AddRef();
    resource.SetKey(key);
    cache_.emplace(key, &resource);
    return true;
}

bool cGZPersistCacheStrategyBasic::UncacheResource(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return false;
    cIGZPersistResource* resource = it->second;
    cache_.erase(it);
    if (resource) resource->Release();
    return true;
}

bool cGZPersistCacheStrategyBasic::HasCachedResource(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    return cache_.find(key) != cache_.end();
}

bool cGZPersistCacheStrategyBasic::CanCacheResource(const cGZPersistResourceKey& key) {
    return filter_ ? filter_->IsKeyIncluded(key) : true;
}

std::uint32_t cGZPersistCacheStrategyBasic::GetCachedResourceList(cIGZPersistResourceKeyList* list,
                                                                  cIGZPersistResourceKeyFilter* filter) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    std::uint32_t count = 0;
    for (const auto& [key, resource] : cache_) {
        (void)resource;
        if (filter_ && !filter_->IsKeyIncluded(key)) continue;
        if (filter && !filter->IsKeyIncluded(key)) continue;
        if (list) list->Insert(key);
        ++count;
    }
    return count;
}

std::uint32_t cGZPersistCacheStrategyBasic::GetCachedResourceListForType(cIGZPersistResourceKeyList* list,
                                                                         std::uint32_t type) {
    cGZPersistResourceKeyFilterByType filter(type);
    filter.AddRef();
    return GetCachedResourceList(list, &filter);
}

bool cGZPersistCacheStrategyBasic::IsGarbageCollectionActive() {
    std::lock_guard<std::recursive_mutex> guard(stateMutex_);
    return gcActive_;
}

void cGZPersistCacheStrategyBasic::SetGarbageCollectionActive(bool active) {
    std::lock_guard<std::recursive_mutex> guard(stateMutex_);
    gcActive_ = active;
}

bool cGZPersistCacheStrategyBasic::OnIdle(std::uint32_t) {
    if (gcActive_) {
        std::int32_t previous = idleCounter_++;
        if (previous > 0x7f) {
            idleCounter_ = 0;
            GarbageCollect();
        }
    }
    return true;
}

bool cGZPersistCacheStrategyBasic::ForceGarbageCollection() {
    std::uint32_t passes = 0;
    for (; passes < 8; ++passes)
        if (!GarbageCollect()) return passes != 0;
    return true;
}

// A cached resource whose only reference is the cache's own is dropped.
bool cGZPersistCacheStrategyBasic::GarbageCollect() {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    std::uint32_t released = 0;
    for (auto it = cache_.begin(); it != cache_.end();) {
        cIGZPersistResource* resource = it->second;
        if (resource && resource->GetRefCount() < 2) {
            resource->Release();
            ++released;
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
    return released != 0;
}

// ===================================================================================== manager

cGZPersistResourceManager::cGZPersistResourceManager() : cRZSystemService(0x056b906e, 1000000) {
    AddCacheStrategy(new cGZPersistCacheStrategyBasic());   // the list holds the only reference
}

bool cGZPersistResourceManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_PersistResourceManager) {
        *out = static_cast<cIGZPersistResourceManager*>(this);
        AddRef();
        return true;
    }
    return cRZSystemService::QueryInterface(iid, out);
}

bool cGZPersistResourceManager::Shutdown() {
    std::scoped_lock guard(cacheMutex_, factoryMutex_, segmentMutex_);
    for (auto& strategy : strategies_) strategy->Shutdown();
    factories_.clear();
    typeToClass_.clear();
    for (cIGZPersistDBSegment* segment : segments_) segment->Release();
    segments_.clear();
    return true;
}

bool cGZPersistResourceManager::OnIdle(std::uint32_t elapsed) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    bool any = false;
    for (auto& strategy : strategies_) any |= strategy->OnIdle(elapsed);
    return any;
}

// ------------------------------------------------------------------------------- segments

bool cGZPersistResourceManager::RegisterDBSegmentFront(cIGZPersistDBSegment& segment) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    segment.AddRef();
    UnregisterDBSegment(segment);   // never twice
    segments_.push_front(&segment);
    return true;
}

bool cGZPersistResourceManager::RegisterDBSegmentBack(cIGZPersistDBSegment& segment) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    segment.AddRef();
    UnregisterDBSegment(segment);
    segments_.push_back(&segment);
    return true;
}

bool cGZPersistResourceManager::UnregisterDBSegment(cIGZPersistDBSegment& segment) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    auto it = std::find(segments_.begin(), segments_.end(), &segment);
    if (it == segments_.end()) return false;
    (*it)->Release();
    segments_.erase(it);
    return true;
}

bool cGZPersistResourceManager::TestDBSegment(cIGZPersistDBSegment& segment) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    return std::find(segments_.begin(), segments_.end(), &segment) != segments_.end();
}

bool cGZPersistResourceManager::FindDBSegment(const cGZPersistResourceKey& key, cIGZPersistDBSegment** out) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    *out = nullptr;
    for (cIGZPersistDBSegment* segment : segments_) {
        if (segment->TestForRecord(key)) {
            *out = segment;
            segment->AddRef();
            return true;
        }
    }
    return false;
}

bool cGZPersistResourceManager::FindDBSegment(std::uint32_t segmentId, cIGZPersistDBSegment** out) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    *out = nullptr;
    for (cIGZPersistDBSegment* segment : segments_) {
        if (segment->GetSegmentID() == segmentId) {
            *out = segment;
            segment->AddRef();
            return true;
        }
    }
    return false;
}

std::uint32_t cGZPersistResourceManager::GetSegmentCount() {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    return static_cast<std::uint32_t>(segments_.size());
}

cIGZPersistDBSegment* cGZPersistResourceManager::GetSegmentByIndex(std::uint32_t index) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    return index < segments_.size() ? segments_[index] : nullptr;
}

std::uint32_t cGZPersistResourceManager::EnumerateDBSegments(cIGZPersistDBSegment** out, std::uint32_t& count) {
    std::uint32_t total = static_cast<std::uint32_t>(segments_.size());
    if (out && count != 0) {
        std::uint32_t n = std::min(count, total);
        count = 0;
        for (std::uint32_t i = 0; i < n; ++i) {
            out[i] = segments_[i];
            out[i]->AddRef();
            ++count;
        }
    }
    return total;
}

void cGZPersistResourceManager::EnumerateDBSegments(bool (*callback)(cIGZPersistDBSegment*, void*), void* context) {
    if (!callback) return;
    for (cIGZPersistDBSegment* segment : segments_)
        if (!callback(segment, context)) return;
}

bool cGZPersistResourceManager::OpenDBRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** record,
                                             bool write) {
    cIGZPersistDBSegment* segment = nullptr;
    if (!FindDBSegment(key, &segment)) return false;
    bool ok = segment->OpenRecord(key, record, write ? 3u : 1u);
    segment->Release();
    return ok;
}

bool cGZPersistResourceManager::CloseDBRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord** record) {
    bool ok = (*record)->Close();
    (*record)->Release();
    return ok;
}

// ------------------------------------------------------------------------------- factories

namespace {
cGZPersistResourceManager::ClassCreator g_classCreator = nullptr;
}

void cGZPersistResourceManager::SetClassCreator(ClassCreator creator) { g_classCreator = creator; }

// Without a factory the class id is asked of the COM director; a class already registered keeps its factory, so
// one factory serves every type registered under its class.
bool cGZPersistResourceManager::RegisterObjectFactory(std::uint32_t classId, std::uint32_t type,
                                                      cIGZPersistResourceFactory* factory) {
    std::lock_guard<std::recursive_mutex> guard(factoryMutex_);
    if (!factory) {
        if (factories_.find(classId) == factories_.end()) {
            void* raw = nullptr;
            if (!g_classCreator || !g_classCreator(classId, kGZIID_cIGZPersistResourceFactory, &raw) || !raw) return false;
            auto* created = static_cast<cIGZPersistResourceFactory*>(raw);
            factories_[classId] = cRZAutoRefCount<cIGZPersistResourceFactory>(created);
            created->Release();   // the creator's reference
        }
        typeToClass_[type] = classId;
        return true;
    }
    typeToClass_[type] = classId;
    factories_[classId] = cRZAutoRefCount<cIGZPersistResourceFactory>(factory);
    return true;
}

bool cGZPersistResourceManager::UnregisterObjectFactory(std::uint32_t classId) {
    std::lock_guard<std::recursive_mutex> guard(factoryMutex_);
    auto it = factories_.find(classId);
    if (it == factories_.end()) return false;
    factories_.erase(it);
    for (auto t = typeToClass_.begin(); t != typeToClass_.end();)
        t = (t->second == classId) ? typeToClass_.erase(t) : std::next(t);
    return true;
}

bool cGZPersistResourceManager::FindObjectFactory(std::uint32_t type, cIGZPersistResourceFactory** out) {
    std::lock_guard<std::recursive_mutex> guard(factoryMutex_);
    auto t = typeToClass_.find(type);
    if (t == typeToClass_.end()) return false;
    auto f = factories_.find(t->second);
    if (f == factories_.end()) return false;
    *out = f->second.get();
    (*out)->AddRef();
    return true;
}

void cGZPersistResourceManager::FindObjectFactory(cIGZPersistResource& resource, cIGZPersistResourceFactory** out) {
    FindObjectFactory(resource.GetType(), out);
}

void cGZPersistResourceManager::FindObjectFactory(const cGZPersistResourceKey& key, cIGZPersistResourceFactory** out) {
    FindObjectFactory(key.type, out);
}

std::uint32_t cGZPersistResourceManager::GetFactoryCount() {
    std::lock_guard<std::recursive_mutex> guard(factoryMutex_);
    return static_cast<std::uint32_t>(factories_.size());
}

cIGZPersistResourceFactory* cGZPersistResourceManager::GetFactoryByIndex(std::uint32_t index) {
    std::lock_guard<std::recursive_mutex> guard(factoryMutex_);
    if (index >= factories_.size()) return nullptr;
    auto it = factories_.begin();
    std::advance(it, index);
    return it->second.get();
}

// ------------------------------------------------------------------------------- resources

bool cGZPersistResourceManager::AddResourceToCache(const cGZPersistResourceKey& key, cIGZPersistResource* resource) {
    for (auto& strategy : strategies_) {
        if (strategy->CanCacheResource(key)) {
            strategy->CacheResource(key, *resource);
            return true;
        }
    }
    return false;
}

bool cGZPersistResourceManager::ReadRecord(const cGZPersistResourceKey& key, cIGZPersistResourceFactory* factory,
                                           cIGZPersistDBSegment* segment, std::uint32_t iid, void** out,
                                           std::uint32_t flags, cIGZUnknown* owner) {
    cIGZPersistDBRecord* record = nullptr;
    if (!segment->OpenRecord(key, &record, 1)) {
        if (record) segment->CloseRecord(&record);
        return false;
    }
    void* raw = nullptr;
    if (factory->CreateInstance(*record, kGZIID_cIGZPersistResource, &raw, flags, owner)) {
        auto* resource = static_cast<cIGZPersistResource*>(raw);
        resource->SetKey(key);
        bool ok = resource->QueryInterface(iid, out);
        segment->CloseRecord(record);
        record->Release();
        resource->Release();
        return ok;
    }
    segment->CloseRecord(&record);
    return false;
}

bool cGZPersistResourceManager::CreateAndSerialize(const cGZPersistResourceKey& key, std::uint32_t iid, void** out,
                                                   std::uint32_t flags, cIGZUnknown* owner) {
    cIGZPersistResourceFactory* factory = nullptr;
    if (!FindObjectFactory(key.type, &factory)) return false;
    cIGZPersistDBSegment* segment = nullptr;
    bool ok = false;
    if (FindDBSegment(key, &segment)) {
        ok = ReadRecord(key, factory, segment, iid, out, flags, owner);
        segment->Release();
    }
    factory->Release();
    return ok;
}

bool cGZPersistResourceManager::CreateNewResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out,
                                                  std::uint32_t flags, cIGZUnknown* owner) {
    cIGZPersistResourceFactory* factory = nullptr;
    if (!FindObjectFactory(key.type, &factory)) return false;
    bool ok = factory->CreateInstance(key.type, iid, out, flags, owner);
    factory->Release();
    return ok;
}

bool cGZPersistResourceManager::GetResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out,
                                            std::uint32_t flags, cIGZUnknown* owner) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    cIGZPersistResource* resource = nullptr;
    bool found = false;
    for (auto& strategy : strategies_) {
        if (resource) { resource->Release(); resource = nullptr; }
        if (strategy->GetResource(key, &resource)) { found = true; break; }
    }
    if (!found) {
        if (resource) { resource->Release(); resource = nullptr; }
        void* raw = nullptr;
        if (!CreateAndSerialize(key, kGZIID_cIGZPersistResource, &raw, flags, owner)) return false;
        resource = static_cast<cIGZPersistResource*>(raw);
        AddResourceToCache(key, resource);
    }
    bool ok = resource->QueryInterface(iid, out);
    resource->Release();
    return ok;
}

bool cGZPersistResourceManager::GetPrivateResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out,
                                                   std::uint32_t flags, cIGZUnknown* owner) {
    void* raw = nullptr;
    if (!CreateAndSerialize(key, kGZIID_cIGZPersistResource, &raw, flags, owner)) return false;
    auto* resource = static_cast<cIGZPersistResource*>(raw);
    bool ok = resource->QueryInterface(iid, out);
    resource->Release();
    return ok;
}

bool cGZPersistResourceManager::GetNewResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out,
                                               std::uint32_t flags, cIGZUnknown* owner) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    void* raw = nullptr;
    if (!CreateNewResource(key, kGZIID_cIGZPersistResource, &raw, flags, owner)) return false;
    auto* resource = static_cast<cIGZPersistResource*>(raw);
    resource->SetKey(key);
    bool ok = resource->QueryInterface(iid, out);
    if (ok) AddResourceToCache(key, resource);
    resource->Release();
    return ok;
}

bool cGZPersistResourceManager::GetNewResource(std::uint32_t type, std::uint32_t iid, void** out,
                                               std::uint32_t flags, cIGZUnknown* owner) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    cGZPersistResourceKey key;
    {
        std::lock_guard<std::recursive_mutex> counterGuard(counterMutex_);
        key = {type, newResourceGroup_, newResourceCounter_++};
    }
    void* raw = nullptr;
    if (!CreateNewResource(key, kGZIID_cIGZPersistResource, &raw, flags, owner)) return false;
    auto* resource = static_cast<cIGZPersistResource*>(raw);
    resource->SetKey(key);
    bool ok = resource->QueryInterface(iid, out);
    if (ok) AddResourceToCache(key, resource);
    resource->Release();
    return ok;
}

void cGZPersistResourceManager::RegisterResource(const cGZPersistResourceKey& key, cIGZPersistResource& resource) {
    resource.SetKey(key);
    RegisterResource(&resource);
}

bool cGZPersistResourceManager::RegisterResource(cIGZPersistResource* resource) {
    if (!resource) return false;
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    cGZPersistResourceKey key = resource->GetKey();
    if (HasRegisteredResource(key)) return true;
    return AddResourceToCache(key, resource);
}

bool cGZPersistResourceManager::UnregisterResource(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    bool removed = false;
    for (auto& strategy : strategies_) {
        if (strategy->HasCachedResource(key)) removed = strategy->UncacheResource(key);
        if (removed) break;
    }
    return removed;
}

bool cGZPersistResourceManager::HasRegisteredResource(const cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    for (auto& strategy : strategies_)
        if (strategy->HasCachedResource(key)) return true;
    return false;
}

bool cGZPersistResourceManager::Save(const cGZPersistResourceKey& key, cIGZPersistDBSegment& segment) {
    void* raw = nullptr;
    if (!GetResource(key, kGZIID_cIGZPersistResource, &raw, 0, nullptr)) return false;
    auto* resource = static_cast<cIGZPersistResource*>(raw);
    bool ok = SaveResource(resource, key, segment);
    resource->Release();
    return ok;
}

bool cGZPersistResourceManager::Save(cIGZPersistResourceKeyList& keys, cIGZPersistDBSegment& segment) {
    bool all = true;
    for (std::uint32_t i = 0, n = keys.Size(); i < n; ++i)
        if (!Save(*keys.GetKey(i), segment)) all = false;
    return all;
}

bool cGZPersistResourceManager::SaveResource(cIGZPersistResource* resource, const cGZPersistResourceKey& key,
                                             cIGZPersistDBSegment& segment) {
    cIGZPersistDBRecord* record = nullptr;
    bool opened;
    if (segment.TestForRecord(resource->GetKey())) opened = segment.OpenRecord(resource->GetKey(), &record, 3);
    else opened = segment.CreateNewRecord(key, &record);
    bool result = false;
    if (opened) {
        result = true;   // (sic) success is reported even when no factory can write the resource
        cIGZPersistResourceFactory* factory = nullptr;
        if (FindObjectFactory(resource->GetType(), &factory)) {
            if (factory->Write(*resource, *record)) {
                if (segment.CloseRecord(&record)) record = nullptr;
            }
            factory->Release();
        }
    }
    if (record) segment.AbortRecord(&record);
    return result;
}

bool cGZPersistResourceManager::TestForKey(cGZPersistResourceKey& key) {
    std::lock_guard<std::recursive_mutex> guard(segmentMutex_);
    for (cIGZPersistDBSegment* segment : segments_)
        if (segment->TestForRecord(key)) return true;
    return false;
}

std::uint32_t cGZPersistResourceManager::GetResourceList(cIGZPersistResourceKeyList** list,
                                                         cIGZPersistResourceKeyFilter* filter) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    auto* keys = new cGZPersistResourceKeyList();
    keys->AddRef();
    *list = keys;
    std::uint32_t total = 0;
    for (auto& strategy : strategies_) total += strategy->GetCachedResourceList(keys, filter);
    return total;
}

std::uint32_t cGZPersistResourceManager::GetResourceListForType(cIGZPersistResourceKeyList** list,
                                                                std::uint32_t type) {
    cGZPersistResourceKeyFilterByType filter(type);
    filter.AddRef();
    return GetResourceList(list, &filter);
}

std::uint32_t cGZPersistResourceManager::GetAvailableResourceList(cIGZPersistResourceKeyList** list,
                                                                  cIGZPersistResourceKeyFilter* filter) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    auto* keys = new cGZPersistResourceKeyList();
    keys->AddRef();
    *list = keys;
    std::uint32_t total = 0;
    for (cIGZPersistDBSegment* segment : segments_) total += segment->GetResourceKeyList(keys, filter);
    return total;
}

std::uint32_t cGZPersistResourceManager::GetAvailableResourceListForType(cIGZPersistResourceKeyList** list,
                                                                         std::uint32_t type) {
    cGZPersistResourceKeyFilterByType filter(type);
    filter.AddRef();
    return GetAvailableResourceList(list, &filter);
}

// ------------------------------------------------------------------------------- strategies

bool cGZPersistResourceManager::AddCacheStrategy(cIGZPersistCacheStrategy* strategy) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    for (auto& s : strategies_)
        if (s.get() == strategy) return false;
    strategies_.emplace_back(strategy);
    return true;
}

bool cGZPersistResourceManager::RemoveCacheStrategy(cIGZPersistCacheStrategy* strategy) {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    for (auto it = strategies_.begin(); it != strategies_.end(); ++it) {
        if (it->get() == strategy) {
            strategies_.erase(it);
            return true;
        }
    }
    return false;
}

bool cGZPersistResourceManager::IsGarbageCollectionActive() {
    bool any = false;
    for (auto& strategy : strategies_) any |= strategy->IsGarbageCollectionActive();
    return any;
}

void cGZPersistResourceManager::SetGarbageCollectionActive(bool active) {
    for (auto& strategy : strategies_) strategy->SetGarbageCollectionActive(active);
}

bool cGZPersistResourceManager::ForceGarbageCollection() {
    std::lock_guard<std::recursive_mutex> guard(cacheMutex_);
    bool any = false;
    for (auto& strategy : strategies_) any |= strategy->ForceGarbageCollection();
    return any;
}
