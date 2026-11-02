// cGZPersistResourceManager — the persistence resource manager service (id 0x056b906e,
// iid 0x656b8efc): an ordered set of database segments, per-type resource factories, and cache
// strategies. Plus the small classes it is built from: cGZPersistResource (key + refcount base of
// resources), cGZPersistResourceFactory (locked base of factories), cGZPersistBinaryResource /
// Factory (raw blobs, type 0xca63e2a3) and cGZPersistCacheStrategyBasic.
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (vtable 0x100aae8b0 and friends).
//
// Behaviour kept: GetResource consults the cache strategies in order, then loads through the
// factory registered for the key's type from the first segment (front to back) holding the key,
// and caches the result; SaveResource reports success even when no factory exists for the
// resource; segments registered at the front shadow later ones.
// Deviations: the garbage collector erases collected cache entries (the original leaves a null
// entry that makes later lookups of that key fail or crash).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "gz/cIGZPersistResourceManager.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZSystemService.h"

// ---- resource base ---------------------------------------------------------------------------
class cGZPersistResource : public cIGZPersistResource {
public:
    cGZPersistResource() = default;
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 1 or 0x456b8f1d
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount_); }   // atomic
    std::uint32_t Release() override;                               // deletes at 0

    cGZPersistResourceKey GetKey() const override { return key_; }
    bool SetKey(const cGZPersistResourceKey& key) override { key_ = key; return true; }
    std::uint32_t GetType() const override { return key_.type; }
    std::uint32_t GetGroup() const override { return key_.group; }
    std::uint32_t GetInstance() const override { return key_.instance; }
    bool SetType(std::uint32_t type) override { key_.type = type; return true; }
    bool SetGroup(std::uint32_t group) override { key_.group = group; return true; }
    bool SetInstance(std::uint32_t instance) override { key_.instance = instance; return true; }
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refCount_.load()); }

protected:
    ~cGZPersistResource() override = default;

private:
    std::atomic<std::int32_t> refCount_{0};
    cGZPersistResourceKey key_{};
};

// ---- factory base ----------------------------------------------------------------------------
class cGZPersistResourceFactory : public cIGZPersistResourceFactory {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 1 or 0xa56b8f17
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;                               // locked; destroys at <= 1

    bool CreateInstance(std::uint32_t type, std::uint32_t iid, void** out, std::uint32_t, cIGZUnknown*) override {
        return CreateInstance(type, iid, out);
    }
    bool CreateInstance(cIGZPersistDBRecord& record, std::uint32_t iid, void** out, std::uint32_t flags,
                        cIGZUnknown* owner) override;
    bool Read(cIGZPersistResource& resource, cIGZPersistDBRecord& record) override;
    bool Write(const cIGZPersistResource& resource, cIGZPersistDBRecord& record) override;
    using cIGZPersistResourceFactory::CreateInstance;
    // [10]: builds the instance for the record's key (default: by type only).
    bool CreateInstance(cIGZPersistDBRecord& record, std::uint32_t iid, void** out) override {
        return CreateInstance(record.GetKey().type, iid, out);
    }

protected:
    std::recursive_mutex mutex_;

private:
    std::uint32_t refCount_ = 0;
};

// ---- binary blobs ----------------------------------------------------------------------------
constexpr std::uint32_t kGZResourceTypeBinary = 0xca63e2a3;
constexpr std::uint32_t kGZIID_cIGZPersistBinaryResource = 0xca63d8e9;

class cIGZPersistBinaryResource : public cIGZUnknown {
public:
    virtual bool SetData(const void* data, std::uint32_t length) = 0;   // [3] copies; (null,0) clears
    virtual void* GetData() const = 0;                                  // [4]
    virtual std::uint32_t GetDataLength() const = 0;                    // [5]
};

class cGZPersistBinaryResource final : public cGZPersistResource, public cIGZPersistBinaryResource {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;   // + 0xca63d8e9
    std::uint32_t AddRef() override { return cGZPersistResource::AddRef(); }
    std::uint32_t Release() override { return cGZPersistResource::Release(); }
    bool SetData(const void* data, std::uint32_t length) override;
    void* GetData() const override { return const_cast<std::uint8_t*>(data_.data()); }
    std::uint32_t GetDataLength() const override { return static_cast<std::uint32_t>(data_.size()); }

private:
    std::vector<std::uint8_t> data_;
};

class cGZPersistBinaryResourceFactory final : public cGZPersistResourceFactory {
public:
    using cGZPersistResourceFactory::CreateInstance;
    bool CreateInstance(std::uint32_t type, std::uint32_t iid, void** out) override;   // only 0xca63e2a3
    bool SerialRead(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) override;
    bool SerialWrite(cIGZPersistResource& resource, cIGZPersistDBSerialRecord& record) override;
};

// ---- cache strategy --------------------------------------------------------------------------
class cGZPersistCacheStrategyBasic final : public cIGZPersistCacheStrategy {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override;   // 1 or 0x6bb923e4
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;
    void Init() override {}
    void Shutdown() override;
    void SetKeyFilter(cIGZPersistResourceKeyFilter* filter) override;
    cIGZPersistResourceKeyFilter* GetKeyFilter() override { return filter_.get(); }
    bool GetResource(const cGZPersistResourceKey& key, cIGZPersistResource** out) override;
    bool CacheResource(const cGZPersistResourceKey& key, cIGZPersistResource& resource) override;
    bool UncacheResource(const cGZPersistResourceKey& key) override;
    bool HasCachedResource(const cGZPersistResourceKey& key) override;
    bool CanCacheResource(const cGZPersistResourceKey& key) override;
    std::uint32_t GetCachedResourceList(cIGZPersistResourceKeyList* list, cIGZPersistResourceKeyFilter* filter) override;
    std::uint32_t GetCachedResourceListForType(cIGZPersistResourceKeyList* list, std::uint32_t type) override;
    bool OnIdle(std::uint32_t elapsed) override;                    // every 129th idle tick collects
    bool IsGarbageCollectionActive() override;
    void SetGarbageCollectionActive(bool active) override;
    bool ForceGarbageCollection() override;                          // up to 8 passes
    void FlushCache() override;
    bool GarbageCollect();                                           // drops entries only the cache holds

private:
    std::recursive_mutex cacheMutex_, stateMutex_;
    std::map<cGZPersistResourceKey, cIGZPersistResource*, cGZPersistResourceKeyOrder> cache_;
    std::int32_t idleCounter_ = 0;
    bool gcActive_ = false;
    cRZAutoRefCount<cIGZPersistResourceKeyFilter> filter_;
    std::int32_t refCount_ = 0;
};

// ---- manager ---------------------------------------------------------------------------------
class cGZPersistResourceManager final : public cRZSystemService, public cIGZPersistResourceManager {
public:
    cGZPersistResourceManager();     // adds a cGZPersistCacheStrategyBasic

    bool QueryInterface(std::uint32_t iid, void** out) override;   // 0x656b8efc, else the service base
    std::uint32_t AddRef() override { return cRZSystemService::AddRef(); }
    std::uint32_t Release() override { return cRZSystemService::Release(); }

    bool Init() override { return true; }
    bool Shutdown() override;
    bool OnIdle(std::uint32_t elapsed) override;

    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t flags,
                     cIGZUnknown* owner) override;
    bool GetPrivateResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t flags,
                            cIGZUnknown* owner) override;
    bool GetNewResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t flags,
                        cIGZUnknown* owner) override;
    bool GetNewResource(std::uint32_t type, std::uint32_t iid, void** out, std::uint32_t flags,
                        cIGZUnknown* owner) override;
    void RegisterResource(const cGZPersistResourceKey& key, cIGZPersistResource& resource) override;
    bool RegisterResource(cIGZPersistResource* resource) override;
    bool UnregisterResource(const cGZPersistResourceKey& key) override;
    bool HasRegisteredResource(const cGZPersistResourceKey& key) override;
    bool Save(const cGZPersistResourceKey& key, cIGZPersistDBSegment& segment) override;
    bool Save(cIGZPersistResourceKeyList& keys, cIGZPersistDBSegment& segment) override;
    bool SaveResource(cIGZPersistResource* resource, const cGZPersistResourceKey& key,
                      cIGZPersistDBSegment& segment) override;
    bool TestForKey(cGZPersistResourceKey& key) override;
    std::uint32_t GetResourceList(cIGZPersistResourceKeyList** list, cIGZPersistResourceKeyFilter* filter) override;
    std::uint32_t GetResourceListForType(cIGZPersistResourceKeyList** list, std::uint32_t type) override;
    std::uint32_t GetAvailableResourceList(cIGZPersistResourceKeyList** list,
                                           cIGZPersistResourceKeyFilter* filter) override;
    std::uint32_t GetAvailableResourceListForType(cIGZPersistResourceKeyList** list, std::uint32_t type) override;
    bool RegisterObjectFactory(std::uint32_t classId, std::uint32_t type, cIGZPersistResourceFactory* factory) override;
    // The COM director the original asks for a factory registered by class id alone (null factory): returns the
    // object with one reference, queried for `iid`.
    using ClassCreator = bool (*)(std::uint32_t classId, std::uint32_t iid, void** out);
    static void SetClassCreator(ClassCreator creator);
    bool UnregisterObjectFactory(std::uint32_t classId) override;
    void FindObjectFactory(cIGZPersistResource& resource, cIGZPersistResourceFactory** out) override;
    void FindObjectFactory(const cGZPersistResourceKey& key, cIGZPersistResourceFactory** out) override;
    bool FindObjectFactory(std::uint32_t type, cIGZPersistResourceFactory** out) override;
    std::uint32_t GetFactoryCount() override;
    cIGZPersistResourceFactory* GetFactoryByIndex(std::uint32_t index) override;
    bool RegisterDBSegment(cIGZPersistDBSegment& segment) override { return RegisterDBSegmentFront(segment); }
    bool RegisterDBSegmentFront(cIGZPersistDBSegment& segment) override;
    bool RegisterDBSegmentBack(cIGZPersistDBSegment& segment) override;
    bool UnregisterDBSegment(cIGZPersistDBSegment& segment) override;
    bool TestDBSegment(cIGZPersistDBSegment& segment) override;
    bool FindDBSegment(const cGZPersistResourceKey& key, cIGZPersistDBSegment** out) override;
    bool FindDBSegment(std::uint32_t segmentId, cIGZPersistDBSegment** out) override;
    std::uint32_t GetSegmentCount() override;
    cIGZPersistDBSegment* GetSegmentByIndex(std::uint32_t index) override;
    std::uint32_t EnumerateDBSegments(cIGZPersistDBSegment** out, std::uint32_t& count) override;
    void EnumerateDBSegments(bool (*callback)(cIGZPersistDBSegment*, void*), void* context) override;
    bool OpenDBRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** record, bool write) override;
    bool CloseDBRecord(const cGZPersistResourceKey& key, cIGZPersistDBRecord** record) override;
    bool AddCacheStrategy(cIGZPersistCacheStrategy* strategy) override;
    bool RemoveCacheStrategy(cIGZPersistCacheStrategy* strategy) override;
    bool IsGarbageCollectionActive() override;
    void SetGarbageCollectionActive(bool active) override;
    bool ForceGarbageCollection() override;

private:
    bool AddResourceToCache(const cGZPersistResourceKey& key, cIGZPersistResource* resource);
    bool CreateNewResource(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t flags,
                           cIGZUnknown* owner);
    bool CreateAndSerialize(const cGZPersistResourceKey& key, std::uint32_t iid, void** out, std::uint32_t flags,
                            cIGZUnknown* owner);
    bool ReadRecord(const cGZPersistResourceKey& key, cIGZPersistResourceFactory* factory,
                    cIGZPersistDBSegment* segment, std::uint32_t iid, void** out, std::uint32_t flags,
                    cIGZUnknown* owner);

    std::recursive_mutex factoryMutex_, segmentMutex_, cacheMutex_, counterMutex_;
    std::unordered_map<std::uint32_t, std::uint32_t> typeToClass_;           // resource type -> class id
    std::map<std::uint32_t, cRZAutoRefCount<cIGZPersistResourceFactory>> factories_;   // class id -> factory
    std::deque<cIGZPersistDBSegment*> segments_;                              // front shadows back
    std::list<cRZAutoRefCount<cIGZPersistCacheStrategy>> strategies_;
    std::uint32_t newResourceGroup_ = 0;
    std::uint32_t newResourceCounter_ = 0xffff;
};
