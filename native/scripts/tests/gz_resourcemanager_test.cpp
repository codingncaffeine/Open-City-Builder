#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "gz/cGZDBSegmentPackedFile.h"
#include "gz/cGZPersistResourceKeyList.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZString.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

static cGZDBSegmentPackedFile* OpenSegment(const std::string& path, bool write, std::uint32_t id) {
    auto* seg = new cGZDBSegmentPackedFile();
    seg->AddRef();
    seg->Init();
    cRZString p(path.c_str());
    seg->SetPath(p);
    seg->SetSegmentID(id);
    CHECK(seg->Open(true, write));
    return seg;
}

int main() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / "sc4_manager_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string pathA = (dir / "a.dat").string(), pathB = (dir / "b.dat").string();
    const cGZPersistResourceKey keyA{kGZResourceTypeBinary, 1, 100}, keyB{kGZResourceTypeBinary, 2, 200};

    // --- save two blobs through the manager into two packages
    {
        auto* mgr = new cGZPersistResourceManager();
        mgr->AddRef();
        auto* factory = new cGZPersistBinaryResourceFactory();
        factory->AddRef();
        CHECK(mgr->RegisterObjectFactory(0x1234, kGZResourceTypeBinary, factory));
        CHECK(mgr->GetFactoryCount() == 1 && mgr->GetFactoryByIndex(0) == factory);
        cIGZPersistResourceFactory* found = nullptr;
        CHECK(mgr->FindObjectFactory(kGZResourceTypeBinary, &found) && found == factory);
        found->Release();
        CHECK(!mgr->FindObjectFactory(0x77, &found));

        // a factory registered by class id alone comes from the class creator (the COM director); without one the
        // registration is refused, and a second type of the same class shares the created factory
        CHECK(!mgr->RegisterObjectFactory(0x5678, 0x66, nullptr) && !mgr->FindObjectFactory(0x66, &found));
        static int created = 0;
        cGZPersistResourceManager::SetClassCreator([](std::uint32_t classId, std::uint32_t, void** out) {
            if (classId != 0x5678) return false;
            auto* made = new cGZPersistBinaryResourceFactory();
            made->AddRef();
            *out = static_cast<cIGZPersistResourceFactory*>(made);
            ++created;
            return true;
        });
        CHECK(mgr->RegisterObjectFactory(0x5678, 0x66, nullptr) && mgr->RegisterObjectFactory(0x5678, 0x67, nullptr) && created == 1);
        CHECK(!mgr->RegisterObjectFactory(0x9999, 0x68, nullptr));
        cIGZPersistResourceFactory* first = nullptr;
        cIGZPersistResourceFactory* second = nullptr;
        CHECK(mgr->FindObjectFactory(0x66, &first) && mgr->FindObjectFactory(0x67, &second) && first && first == second);
        if (first) first->Release();
        if (second) second->Release();
        cGZPersistResourceManager::SetClassCreator(nullptr);
        CHECK(mgr->UnregisterObjectFactory(0x5678));

        auto* segA = OpenSegment(pathA, true, 11);
        auto* segB = OpenSegment(pathB, true, 22);
        CHECK(mgr->RegisterDBSegment(*segA) && mgr->RegisterDBSegmentBack(*segB));
        CHECK(mgr->GetSegmentCount() == 2 && mgr->TestDBSegment(*segA));

        void* raw = nullptr;
        CHECK(mgr->GetNewResource(keyA, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));
        auto* blobA = static_cast<cIGZPersistBinaryResource*>(raw);
        const char text[] = "hello persistence";
        CHECK(blobA->SetData(text, sizeof text));
        CHECK(mgr->HasRegisteredResource(keyA));
        blobA->Release();
        // saving by key goes through GetResource -> cache
        CHECK(mgr->Save(keyA, *segA));

        std::vector<std::uint8_t> big(5000);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>(i % 9);
        CHECK(mgr->GetNewResource(keyB, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));
        auto* blobB = static_cast<cIGZPersistBinaryResource*>(raw);
        blobB->SetData(big.data(), static_cast<std::uint32_t>(big.size()));
        blobB->Release();
        CHECK(mgr->Save(keyB, *segB));

        CHECK(segA->Close() && segB->Close());
        mgr->Shutdown();
        segA->Release();
        segB->Release();
        factory->Release();
        mgr->Release();
    }

    // --- fresh manager: load from the packages
    {
        auto* mgr = new cGZPersistResourceManager();
        mgr->AddRef();
        auto* factory = new cGZPersistBinaryResourceFactory();
        factory->AddRef();
        mgr->RegisterObjectFactory(0x1234, kGZResourceTypeBinary, factory);
        auto* segA = OpenSegment(pathA, false, 11);
        auto* segB = OpenSegment(pathB, false, 22);
        mgr->RegisterDBSegment(*segA);
        mgr->RegisterDBSegment(*segB);   // front: segB shadows
        CHECK(mgr->GetSegmentByIndex(0) == segB);
        CHECK(mgr->TestForKey(const_cast<cGZPersistResourceKey&>(keyA)));
        cIGZPersistDBSegment* owner = nullptr;
        CHECK(mgr->FindDBSegment(keyB, &owner) && owner == segB);
        owner->Release();
        CHECK(mgr->FindDBSegment(22u, &owner) && owner == segB);
        owner->Release();

        void* raw = nullptr;
        CHECK(mgr->GetResource(keyA, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));
        auto* blob = static_cast<cIGZPersistBinaryResource*>(raw);
        CHECK(blob->GetDataLength() == 18 && !std::strcmp(static_cast<char*>(blob->GetData()), "hello persistence"));
        // second request is served from the cache: same object
        void* again = nullptr;
        CHECK(mgr->GetResource(keyA, kGZIID_cIGZPersistBinaryResource, &again, 0, nullptr) && again == raw);
        static_cast<cIGZUnknown*>(again)->Release();
        // a private copy is a different object
        void* priv = nullptr;
        CHECK(mgr->GetPrivateResource(keyA, kGZIID_cIGZPersistBinaryResource, &priv, 0, nullptr) && priv != raw);
        static_cast<cIGZUnknown*>(priv)->Release();
        blob->Release();

        CHECK(mgr->GetResource(keyB, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));
        auto* blobB = static_cast<cIGZPersistBinaryResource*>(raw);
        CHECK(blobB->GetDataLength() == 5000 && static_cast<std::uint8_t*>(blobB->GetData())[4999] == 4999 % 9);
        blobB->Release();
        CHECK(!mgr->GetResource({kGZResourceTypeBinary, 9, 9}, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));
        CHECK(!mgr->GetResource({0x55555555, 1, 1}, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));  // no factory

        // listings
        cIGZPersistResourceKeyList* list = nullptr;
        CHECK(mgr->GetResourceList(&list, nullptr) == 2);   // both are cached now
        list->Release();
        CHECK(mgr->GetAvailableResourceListForType(&list, kGZResourceTypeBinary) == 2);
        list->Release();

        // garbage collection drops cached resources nobody else holds, and the key is reloadable
        mgr->SetGarbageCollectionActive(true);
        CHECK(mgr->IsGarbageCollectionActive());
        CHECK(mgr->ForceGarbageCollection());
        CHECK(!mgr->HasRegisteredResource(keyA));
        CHECK(mgr->GetResource(keyA, kGZIID_cIGZPersistBinaryResource, &raw, 0, nullptr));
        static_cast<cIGZUnknown*>(raw)->Release();

        // new-resource keys are unique and start at 0xffff
        void* n1 = nullptr; void* n2 = nullptr;
        CHECK(mgr->GetNewResource(kGZResourceTypeBinary, kGZIID_cIGZPersistResource, &n1, 0, nullptr));
        CHECK(mgr->GetNewResource(kGZResourceTypeBinary, kGZIID_cIGZPersistResource, &n2, 0, nullptr));
        CHECK(static_cast<cIGZPersistResource*>(n1)->GetInstance() == 0xffff &&
              static_cast<cIGZPersistResource*>(n2)->GetInstance() == 0x10000);
        static_cast<cIGZUnknown*>(n1)->Release();
        static_cast<cIGZUnknown*>(n2)->Release();

        CHECK(mgr->UnregisterDBSegment(*segA) && !mgr->UnregisterDBSegment(*segA));
        CHECK(mgr->UnregisterObjectFactory(0x1234) && mgr->GetFactoryCount() == 0);

        mgr->Shutdown();
        segA->Close(); segB->Close();
        segA->Release(); segB->Release();
        factory->Release();
        mgr->Release();
    }

    std::filesystem::remove_all(dir);
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::puts("gz_resourcemanager: all tests passed");
    return 0;
}
