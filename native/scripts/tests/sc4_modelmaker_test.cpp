// cSC4ModelMaker — tests for the recovered part (src/sc4/ModelMaker.h).
// The seams are faked: the view/renderer/lighting/terrain/city host, the model-maker utility
// (cSC4ModelMakerUtility), the resource manager + the S3D resource, and the two helper objects. The occupant, the
// placeable object, the model instance and the two managers are scripted fakes so the tests can watch every call.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "rz/cRZVersion.h"
#include "sc4/Lot.h"
#include "sc4/ModelMaker.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/ZoneDeveloper.h"

// The two service-locator pointers the model maker reads (SL::spRM / SL::spMS2). Defining them here keeps the test
// free of the whole service locator; the fake message server is installed by main.
namespace SL {
cIGZPersistResourceManager* spRM = nullptr;
cIGZMessageServer2* spMS2 = nullptr;
}  // namespace SL

namespace {

int g_failures = 0;
void Check(bool condition, const char* what, int line) {
    if (condition) return;
    std::printf("FAIL line %d: %s\n", line, what);
    ++g_failures;
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

using sc4::cISC4Occupant;
using sc4::cISC43DPlaceableObject;
using sc4::cISC4OccupantFilter;
using sc4::cISC4OccupantManager;
using sc4::cISC4LotManager;
using sc4::cSC4Lot;
using sc4::OccupantList;
using sc4::OccupantCallback;
using sc4::SC4CellRect;

// ================================================================================================================
// fakes
// ================================================================================================================

// A model instance that records everything the model maker does to it.
struct FakeModel final : s3d::cIS3DModelInstance {
    cIGZUnknown* owner = nullptr;
    void* custom = nullptr;
    cS3DBoundingBox bounds;
    cS3DTransform transform;
    int properties = 2;
    s3d::SC4DrawAttributes attributes;
    std::uint32_t sortValue = 0;
    int refCount = 1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != s3d::kGZIID_cIS3DModelInstance) return false;
        *out = this;
        ++refCount;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    cIGZUnknown* GetOwner() override { return owner; }
    void SetOwner(cIGZUnknown* value) override { owner = value; }
    void* GetCustomData() override { return custom; }
    void SetCustomData(void* value) override { custom = value; }
    void GetBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetWorldBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& value) override { transform = value; }
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 1; }
    int GetProperties() override { return properties; }
    void SetProperties(int value) override { properties = value; }
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return attributes; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes& value) override { attributes = value; }
    void GetSortValue(std::uint32_t& out) const override { out = sortValue; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
};

// The occupant's placeable object: an ordered model list plus the visibility / zoom / rotation words.
struct FakePlaceable final : cISC43DPlaceableObject {
    std::vector<s3d::cIS3DModelInstance*> models;
    bool visible = false;
    bool overlaysVisible = false;
    std::uint32_t zooms = 0;
    std::uint32_t rotations = 0;
    int refCount = 1;
    int removeAllCalls = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != sc4::kGZIID_cISC43DPlaceableObject) return false;
        *out = this;
        ++refCount;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            for (s3d::cIS3DModelInstance* model : models) model->Release();
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    int GetModelCount() override { return static_cast<int>(models.size()); }
    std::uint32_t GetModels(sc4::cIS3DModelInstance** out, std::uint32_t& count) override {
        const std::uint32_t n = count < models.size() ? count : static_cast<std::uint32_t>(models.size());
        for (std::uint32_t i = 0; i < n; ++i) out[i] = reinterpret_cast<sc4::cIS3DModelInstance*>(models[i]);
        count = n;
        return n;
    }
    bool AddModel(sc4::cIS3DModelInstance* model) override {
        auto* instance = reinterpret_cast<s3d::cIS3DModelInstance*>(model);
        instance->AddRef();
        models.push_back(instance);
        return true;
    }
    bool RemoveModel(sc4::cIS3DModelInstance* model) override {
        auto* instance = reinterpret_cast<s3d::cIS3DModelInstance*>(model);
        for (std::size_t i = 0; i < models.size(); ++i) {
            if (models[i] != instance) continue;
            models[i]->Release();
            models.erase(models.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
        return false;
    }
    bool AddOverlay(std::uint32_t) override { return true; }
    void SetVisibility(bool value) override { visible = value; }
    void SetOverlaysVisible(bool value) override { overlaysVisible = value; }
    bool IsVisible() const override { return visible; }
    void RemoveAll() override {
        ++removeAllCalls;
        for (s3d::cIS3DModelInstance* model : models) model->Release();
        models.clear();
    }
    std::uint32_t GetZoomLevelsCreatedFor() const override { return zooms; }
    void SetZoomLevelsCreatedFor(std::uint32_t value) override { zooms = value; }
    std::uint32_t GetRotationsCreatedFor() const override { return rotations; }
    void SetRotationsCreatedFor(std::uint32_t value) override { rotations = value; }
};

// The occupant. Scriptable position / cells / flags / placeable / highlight.
struct FakeOccupant final : cISC4Occupant {
    std::uint32_t type = 0;
    float position[3] = {0, 0, 0};
    SC4CellRect cells{0, 0, 0, 0};
    std::uint32_t flags = 0;
    int highlight = 0;
    std::uint32_t state = 0;
    FakePlaceable* placeable = nullptr;   // borrowed
    bool createdPlaceable = false;
    int refCount = 1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != sc4::kGZIID_cISC4Occupant) return false;
        *out = this;
        ++refCount;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    sc4::cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof position); }
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        for (int i = 0; i < 3; ++i) {
            minXyz[i] = position[i] - 1.0f;
            maxXyz[i] = position[i] + 1.0f;
        }
    }
    void GetBoundingCityCells(SC4CellRect& out) override { out = cells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return static_cast<std::uint32_t>(highlight); }
    void SetHighlight(int value, bool) override { highlight = value; }
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return placeable; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override {
        createdPlaceable = true;
        return placeable;
    }
    void SetPlaceableObject(cISC43DPlaceableObject* object) override {
        placeable = static_cast<FakePlaceable*>(object);
    }
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t flag) override { flags |= flag; }
    void SetAllFlags(std::uint32_t value) override { flags = value; }
    void ClearFlag(std::uint32_t flag) override { flags &= ~flag; }
    bool IsFlagSet(std::uint32_t flag) override { return (flags & flag) != 0; }
    std::uint32_t GetFlags() override { return flags; }
};

// A cISC4OccupantFilter with a scripted answer.
struct FakeFilter final : cISC4OccupantFilter {
    bool included = true;
    std::uint32_t onlyType = 0;
    bool typeLimited = false;
    int refCount = 1;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != sc4::kGZIID_cISC4OccupantFilter) return false;
        *out = this;
        ++refCount;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    bool IsOccupantIncluded(cISC4Occupant*) override { return included; }
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return !typeLimited || type == onlyType; }
    bool IsPropertyHolderIncluded(sc4::cISCPropertyHolder*) override { return true; }
};

// The S3D model resource: creates FakeModel instances.
struct FakeResModel final : s3d::cIS3DResModel {
    cS3DBoundingBox bounds;
    int refCount = 1;
    int created = 0;
    std::vector<s3d::ShadowMeshData> shadowMeshes;
    bool GetShadowMeshes(std::vector<s3d::ShadowMeshData>& out) override {
        out = shadowMeshes;
        return !out.empty();
    }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        ++refCount;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    void ComputeBounds(cS3DBoundingBox& out) override { out = bounds; }
    bool CreateModelInstance(std::uint32_t, s3d::cIS3DModelInstance** out) override {
        ++created;
        auto* model = new FakeModel();
        model->bounds = bounds;
        *out = model;
        return true;
    }
};

struct FakeResourceManager final : cIGZPersistResourceManager {
    FakeResModel* resource = nullptr;   // borrowed
    cGZPersistResourceKey lastKey;
    int lookups = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool GetResource(const cGZPersistResourceKey& key, std::uint32_t, void** out, std::uint32_t,
                     cIGZUnknown*) override {
        ++lookups;
        lastKey = key;
        if (!resource) return false;
        resource->AddRef();
        *out = static_cast<s3d::cIS3DResModel*>(resource);
        return true;
    }
    bool GetPrivateResource(const cGZPersistResourceKey&, std::uint32_t, void**, std::uint32_t, cIGZUnknown*) override { return false; }
    bool GetNewResource(const cGZPersistResourceKey&, std::uint32_t, void**, std::uint32_t, cIGZUnknown*) override { return false; }
    bool GetNewResource(std::uint32_t, std::uint32_t, void**, std::uint32_t, cIGZUnknown*) override { return false; }
    void RegisterResource(const cGZPersistResourceKey&, cIGZPersistResource&) override {}
    bool RegisterResource(cIGZPersistResource*) override { return false; }
    bool UnregisterResource(const cGZPersistResourceKey&) override { return false; }
    bool HasRegisteredResource(const cGZPersistResourceKey&) override { return false; }
    bool Save(const cGZPersistResourceKey&, cIGZPersistDBSegment&) override { return false; }
    bool Save(cIGZPersistResourceKeyList&, cIGZPersistDBSegment&) override { return false; }
    bool SaveResource(cIGZPersistResource*, const cGZPersistResourceKey&, cIGZPersistDBSegment&) override { return false; }
    bool TestForKey(cGZPersistResourceKey&) override { return false; }
    std::uint32_t GetResourceList(cIGZPersistResourceKeyList**, cIGZPersistResourceKeyFilter*) override { return 0; }
    std::uint32_t GetResourceListForType(cIGZPersistResourceKeyList**, std::uint32_t) override { return 0; }
    std::uint32_t GetAvailableResourceList(cIGZPersistResourceKeyList**, cIGZPersistResourceKeyFilter*) override { return 0; }
    std::uint32_t GetAvailableResourceListForType(cIGZPersistResourceKeyList**, std::uint32_t) override { return 0; }
    bool RegisterObjectFactory(std::uint32_t, std::uint32_t, cIGZPersistResourceFactory*) override { return false; }
    bool UnregisterObjectFactory(std::uint32_t) override { return false; }
    void FindObjectFactory(cIGZPersistResource&, cIGZPersistResourceFactory**) override {}
    void FindObjectFactory(const cGZPersistResourceKey&, cIGZPersistResourceFactory**) override {}
    bool FindObjectFactory(std::uint32_t, cIGZPersistResourceFactory**) override { return false; }
    std::uint32_t GetFactoryCount() override { return 0; }
    cIGZPersistResourceFactory* GetFactoryByIndex(std::uint32_t) override { return nullptr; }
    bool RegisterDBSegment(cIGZPersistDBSegment&) override { return false; }
    bool RegisterDBSegmentFront(cIGZPersistDBSegment&) override { return false; }
    bool RegisterDBSegmentBack(cIGZPersistDBSegment&) override { return false; }
    bool UnregisterDBSegment(cIGZPersistDBSegment&) override { return false; }
    bool TestDBSegment(cIGZPersistDBSegment&) override { return false; }
    bool FindDBSegment(const cGZPersistResourceKey&, cIGZPersistDBSegment**) override { return false; }
    bool FindDBSegment(std::uint32_t, cIGZPersistDBSegment**) override { return false; }
    std::uint32_t GetSegmentCount() override { return 0; }
    cIGZPersistDBSegment* GetSegmentByIndex(std::uint32_t) override { return nullptr; }
    std::uint32_t EnumerateDBSegments(cIGZPersistDBSegment**, std::uint32_t& count) override {
        count = 0;
        return 0;
    }
    void EnumerateDBSegments(bool (*)(cIGZPersistDBSegment*, void*), void*) override {}
    bool OpenDBRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord**, bool) override { return false; }
    bool CloseDBRecord(const cGZPersistResourceKey&, cIGZPersistDBRecord**) override { return false; }
    bool AddCacheStrategy(cIGZPersistCacheStrategy*) override { return false; }
    bool RemoveCacheStrategy(cIGZPersistCacheStrategy*) override { return false; }
    bool IsGarbageCollectionActive() override { return false; }
    void SetGarbageCollectionActive(bool) override {}
    bool ForceGarbageCollection() override { return false; }
};

// The occupant manager: every query returns the scripted occupant list.
// A building occupant for DoBuildingCompletionChanged (its occupant and completion only).
struct FakeBuildingOccupant final : sc4::cISC4BuildingOccupant {
    cISC4Occupant* occupant = nullptr;
    float completion = 1.0f;
    sc4::BuildingProfile profile;
    cRZString name;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs > 1 ? --refs : refs; }
    cISC4Occupant* AsOccupant() override { return occupant; }
    std::uint32_t GetBuildingType() const override { return 0; }
    void SetBuildingType(std::uint32_t) override {}
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { return completion; }
    void SetCompletionPercent(float value) override { completion = value; }
    const sc4::BuildingProfile& GetBuildingProfile() const override { return profile; }
    const cRZString& GetBuildingName() const override { return name; }
    const cRZString& GetExemplarName() const override { return name; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return true; }
    bool IsLit() const override { return false; }
    void SetLit(bool) override {}
    void SetName(const cRZString&) override {}
    bool GetName(cRZString&) override { return false; }
};

struct FakeOccupantManager final : cISC4OccupantManager {
    std::vector<cISC4Occupant*> occupants;
    cISC4OccupantFilter* lastFilter = nullptr;
    int queries = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool SetCitySize(float, float) override { return true; }
    void GetOccupantManagerCellSizes(float& x, float& z) const override { x = z = 64.0f; }
    void GetWorldCellCount(int& x, int& z) const override { x = z = 192; }
    void GetOccupantManagerCellCount(int& x, int& z) const override { x = z = 192; }
    void GetOccupantManagerCellBounds(int& x0, int& z0, int& x1, int& z1) const override {
        x0 = z0 = 0;
        x1 = z1 = 191;
    }
    void WorldCellToOccupantManagerCell(int x, int z, int& ox, int& oz) const override {
        ox = x;
        oz = z;
    }
    void OccupantManagerCellToWorldCell(int x, int z, int& ox, int& oz) const override {
        ox = x;
        oz = z;
    }
    void WorldCellToStandardCityCell(int, int, int& ox, int& oz) const override {
        ox = 0;
        oz = 0;
    }
    void StandardCityCellToWorldCell(int, int, int& ox, int& oz) const override {
        ox = 0;
        oz = 0;
    }
    void WorldCellToPosition(int, int, float&, float&) const override {}
    void PositionToWorldCell(float, float, int& x, int& z) const override { x = z = 0; }
    void OccupantManagerCellToPosition(int, int, float&, float&) const override {}
    void PositionToOccupantManagerCell(float, float, int& x, int& z) const override { x = z = 0; }
    void StandardCityCellToPosition(int, int, float&, float&) const override {}
    void PositionToStandardCityCell(float, float, int& x, int& z) const override { x = z = 0; }
    bool InsertOccupant(cISC4Occupant*, std::uint32_t) override { return false; }
    bool RemoveOccupant(cISC4Occupant*, bool, std::uint32_t) override { return false; }
    bool RemoveOccupants(int, bool, std::uint32_t) override { return false; }
    bool RemoveOccupants(cISC4OccupantFilter*, bool, std::uint32_t) override { return false; }
    bool MoveOccupant(cISC4Occupant*, bool) override { return false; }
    bool IsCellEmpty(int, int) const override { return true; }
    int GetBoundingCells(int, int, int (*)[2], int& count) override {
        count = 0;
        return 0;
    }
    bool FindOccupant(cISC4Occupant*, bool, int*, int*) override { return false; }
    bool GetFirstOccupantByPosition(cISC4Occupant*&, float, float, std::uint32_t) override { return false; }
    bool GetFirstOccupantByPosition(cISC4Occupant*&, float, float, cISC4OccupantFilter*) override { return false; }
    bool GetFirstOccupant(cISC4Occupant*&, int, int, std::uint32_t) override { return false; }
    bool GetFirstOccupant(cISC4Occupant*&, int, int, cISC4OccupantFilter*) override { return false; }
    bool GetFirstOccupantByStandardCityCell(cISC4Occupant*&, int, int, std::uint32_t) override { return false; }
    bool GetFirstOccupantByStandardCityCell(cISC4Occupant*&, int, int, cISC4OccupantFilter*) override { return false; }
    bool GetFirstOccupantByStandardCityCells(cISC4Occupant*&, const int*, const int*, std::uint32_t) override { return false; }
    bool GetFirstOccupantByStandardCityCells(cISC4Occupant*&, const int*, const int*, cISC4OccupantFilter*) override { return false; }
    bool GetOccupantsByBBox(OccupantList&, const float*, const float*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByBBox(OccupantList&, const float*, const float*, cISC4OccupantFilter*, std::uint32_t) override { return false; }
    bool GetOccupantsByOccupantManagerCells(OccupantList&, const int*, const int*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByOccupantManagerCells(OccupantList& out, const int*, const int*, cISC4OccupantFilter* filter, std::uint32_t) override {
        return Fill(out, filter);
    }
    bool GetOccupantsByStandardCityCells(OccupantList&, const int*, const int*, std::uint32_t, std::uint32_t) override { return false; }
    bool GetOccupantsByStandardCityCells(OccupantList& out, const int*, const int*, cISC4OccupantFilter* filter, std::uint32_t) override {
        return Fill(out, filter);
    }
    bool IterateOccupantsByBBox(OccupantCallback, void*, const float*, const float*, std::uint32_t) override { return false; }
    bool IterateOccupantsByBBox(OccupantCallback, void*, const float*, const float*, cISC4OccupantFilter*) override { return false; }
    bool IterateOccupants(OccupantCallback, void*, const int*, const int*, std::uint32_t) override { return false; }
    bool IterateOccupants(OccupantCallback, void*, const int*, const int*, cISC4OccupantFilter*) override { return false; }
    bool IterateOccupantsByStandardCityCell(OccupantCallback, void*, const int*, const int*, cISC4OccupantFilter*) override { return false; }
    bool IterateOccupantsByStandardCityCell(OccupantCallback, void*, const int*, const int*, std::uint32_t) override { return false; }
    void ReleaseOccupantList(OccupantList& list) override {
        for (cISC4Occupant* occupant : list) occupant->Release();
        list.clear();
    }
    bool Fill(OccupantList& out, cISC4OccupantFilter* filter) {
        ++queries;
        lastFilter = filter;
        for (cISC4Occupant* occupant : occupants) {
            occupant->AddRef();
            out.push_back(occupant);
        }
        return true;
    }
};

// The lot manager: one scripted lot at one cell.
struct FakeLotManager final : cISC4LotManager {
    cSC4Lot* lot = nullptr;   // borrowed
    int cellX = 0;
    int cellZ = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    cSC4Lot* GetLot(int x, int z, bool) override {
        return (x == cellX && z == cellZ) ? lot : nullptr;
    }
    cSC4Lot* GetLot(const cS3DVector3&) override { return nullptr; }
    int GetLotsInCellRect(std::list<cRZAutoRefCount<cSC4Lot>>&, int, int, int, int, bool) override { return 0; }
    int GetLotsSurroundingLot(const cSC4Lot*, std::list<cRZAutoRefCount<cSC4Lot>>&, int) override { return 0; }
    int GetLotsSurroundingLot(int, int, std::list<cRZAutoRefCount<cSC4Lot>>&, int) override { return 0; }
    int GetLotsSurroundingRect(const sc4::SC4Rect<int>&, std::list<cRZAutoRefCount<cSC4Lot>>&, int) override { return 0; }
    void GetVerticesAtEdgeOfLot(const sc4::SC4Rect<int>&, int, sc4::SC4Rect<int>&) const override {}
    bool GetNeighborCellsAtEdgeOfLot(const sc4::SC4Rect<int>&, int, sc4::SC4Rect<int>&) const override { return false; }
    cSC4Lot* GetRandomLot() override { return nullptr; }
    cSC4Lot* GetOccupantLot(cISC4Occupant*) override { return nullptr; }
    sc4::IZoneDeveloperLotIterator* GetSafeLotIterator() override { return nullptr; }
    int GetLotCount() override { return lot ? 1 : 0; }
    float GetLotSavvyTerrainVertexAltitude(int, int, const cSC4Lot*, bool*) const override { return 0; }
    float GetLotSavvyTerrainAltitude(float, float, bool*) const override { return 0; }
    float GetLotSavvyCellAltitude(int, int, bool*) const override { return 0; }
    int CanCreateLot(int, int, int, int, bool, bool, sc4::cSC4LotConfiguration*) override { return 0; }
    int CanCreatePloppedLot(sc4::SC4Rect<int>, int, sc4::cSC4LotConfiguration*) override { return 0; }
    bool CreateLotIfPossible(int, int, int, int, int, cSC4Lot*&, bool, bool, sc4::cSC4LotConfiguration*) override { return false; }
    bool CreateLot(int, int, int, int, int, cSC4Lot*&) override { return false; }
    void DeleteLot(cSC4Lot*) override {}
    void DeleteLots(int, int, int, int) override {}
    void GetLotVertexAltitudeAdjustmentRange(int, int, float& minAltitude, float& maxAltitude) override {
        minAltitude = 0;
        maxAltitude = 0;
    }
    sc4::cSC4SimGrid<std::uint16_t>* GetMaximumCapacityGrid() const override { return nullptr; }
};

// The two helper objects.
struct FakeLoader final : sc4::IModelBackgroundLoader {
    int refCount = 1;
    int expandCalls = 0;
    int startCalls = 0;
    int cancelCalls = 0;
    int suspendCalls = 0;
    int resumeCalls = 0;
    int refocusCalls = 0;
    int communicationCalls = 0;
    int pendingCells = 0;
    int lastZoom = -1;
    int lastRotation = -1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    void Shutdown() override {}
    bool Init(sc4::cSC4ModelMaker*) override { return true; }
    void ExpandLoadRect(sc4::SC4RectT<int>& rect, int cells) override {
        ++expandCalls;
        pendingCells = cells;
        rect.x0 -= cells;
        rect.z0 -= cells;
        rect.x1 += cells;
        rect.z1 += cells;
    }
    void StartLoadingForZoomAndRotation(int zoom, int rotation, const sc4::SC4RectT<int>&,
                                        const sc4::SC4Point<int>&) override {
        ++startCalls;
        lastZoom = zoom;
        lastRotation = rotation;
    }
    void RefocusLoadToNewViewLocation(const sc4::SC4RectT<int>&, const sc4::SC4Point<int>&) override {
        ++refocusCalls;
    }
    void CancelAllLoadTasks() override { ++cancelCalls; }
    void SuspendLoading() override { ++suspendCalls; }
    void ResumeLoading() override { ++resumeCalls; }
    void ProcessBackgroundThreadToMainThreadCommunications() override { ++communicationCalls; }
};

struct FakeCoordinator final : sc4::IModelAnimationCoordinator {
    int refCount = 1;
    int calls = 0;
    int shuts = 0;
    bool lastValue = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    void Shutdown() override { ++shuts; }
    bool Init() override { return true; }
    void SetOccupantAnimation(cISC4Occupant*, bool on) override {
        ++calls;
        lastValue = on;
    }
};

// The message server and the animation tick manager (both only need the notification calls the model maker makes).
struct FakeMessageServer final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added;
    std::vector<std::uint32_t> removed;
    int refCount = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refCount); }
    bool MessageSend(cIGZMessage2*) override { return false; }
    bool MessagePost(cIGZMessage2*, bool) override { return false; }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        added.push_back(type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        removed.push_back(type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

// The host seam: every value scripted, every call recorded.
struct FakeHost final : sc4::IModelMakerHost {
    FakeMessageServer messages;
    FakeMessageServer* tick = nullptr;
    FakeOccupantManager* occupantManager = nullptr;
    FakeLotManager* lotManager = nullptr;
    FakeLoader* loader = nullptr;
    FakeCoordinator* coordinator = nullptr;
    FakeFilter* viewFilter = nullptr;
    FakeFilter* zoomFilter = nullptr;
    FakeResourceManager* resources = nullptr;
    FakeResModel* resource = nullptr;

    float focusX = 40.0f;
    float focusZ = 60.0f;
    float groundRect[4] = {320.0f, 480.0f, 640.0f, 800.0f};
    int viewZoom = 2;
    int viewRotation = 1;
    float rotationAngle = 0.0f;
    float viewportUnitsPerWorldUnit = 1.0f;
    float modelSizeThreshold = 0.0f;
    float textureAlpha = 1.0f;
    int detailLevel = 2;
    float lighting[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int terrainX = 100;
    int terrainZ = 100;
    std::vector<std::pair<int, int>> refreshedCells;
    std::vector<std::pair<int, int>> setCellFlags;
    int cityCellX = 0;
    int cityCellZ = 0;
    std::string configBackgroundLoad;
    std::string configIgnoreMissing;
    std::vector<int> zoomFilterValues;
    int filterRefreshes = 0;
    int rendererChanges = 0;
    int rendererUpdates = 0;
    int shadowDecals = 0;
    int shadowQuality = 5;
    float shadowMinimum = 0.0f;
    struct FakeOverlay final : sc4::IShadowOverlay {
        int adds = 0;
        std::uint64_t lastTexture = 0;
        int lastCount = 0;
        cS3DBoundingBox lastBox;
        std::uint32_t AddShadow(std::uint64_t texture, int count, const cS3DVector3*, const cS3DVector2*, const cS3DBoundingBox& box,
                                const cS3DTransform&) override {
            ++adds; lastTexture = texture; lastCount = count; lastBox = box;
            return 7;
        }
    } overlay;
    int ShadowRenderQuality() override { return shadowQuality; }
    float ShadowMinimumHeight() override { return shadowMinimum; }
    sc4::IShadowOverlay* ShadowOverlay(int) override { return &overlay; }
    std::vector<s3d::cIS3DModelInstance*> rendererModels;
    std::vector<std::pair<s3d::cIS3DModelInstance*, int>> changeCodes;

    cIGZUnknown* Renderer() override { return reinterpret_cast<cIGZUnknown*>(resources); }
    cISC4OccupantFilter* CurrentViewFilter() override {
        if (!viewFilter) return nullptr;
        viewFilter->AddRef();
        return viewFilter;
    }
    void RefreshViewFilter() override { ++filterRefreshes; }
    bool RendererHasModelInstance(s3d::cIS3DModelInstance*) override { return true; }
    void RendererUpdateModelInstance(s3d::cIS3DModelInstance* instance) override {
        ++rendererUpdates;
        rendererModels.push_back(instance);
    }
    void RendererChangeModelInstance(s3d::cIS3DModelInstance* instance, int code) override {
        ++rendererChanges;
        rendererModels.push_back(instance);
        changeCodes.emplace_back(instance, code);
    }
    int ViewZoom() override { return viewZoom; }
    int ViewRotation() override { return viewRotation; }
    float ViewRotationAngle() override { return rotationAngle; }
    float ViewportUnitsPerWorldUnit() override { return viewportUnitsPerWorldUnit; }
    bool ViewGroundRectangle(float& fx, float& fz, float rect[4]) override {
        fx = focusX;
        fz = focusZ;
        for (int i = 0; i < 4; ++i) rect[i] = groundRect[i];
        return true;
    }
    float ModelSizeThreshold() override { return modelSizeThreshold; }
    float TextureTintAlpha() override { return textureAlpha; }
    int RenderDetailLevel() override { return detailLevel; }
    void LightingColour(float out[4]) override {
        for (int i = 0; i < 4; ++i) out[i] = lighting[i];
    }
    int TerrainSizeX() override { return terrainX; }
    int TerrainSizeZ() override { return terrainZ; }
    std::uint32_t TerrainCellFlags(int, int) override { return 3; }
    void TerrainSetCellFlags(int x, int z) override { setCellFlags.emplace_back(x, z); }
    void TerrainRefreshCell(int x, int z) override { refreshedCells.emplace_back(x, z); }
    bool TerrainCellHasWater(int, int) override { return false; }
    void TerrainNormalAt(float, float, float out[3]) override {
        out[0] = 0;
        out[1] = 1;
        out[2] = 0;
    }
    bool NetworkAt(float, float) override { return false; }
    void PositionToCityCell(float, float, int& cx, int& cz) override {
        cx = cityCellX;
        cz = cityCellZ;
    }
    bool ReadConfigOption(const char* key, std::string& value) override {
        if (std::strcmp(key, "backgroundLoad") == 0) {
            value = configBackgroundLoad;
            return !value.empty();
        }
        if (std::strcmp(key, "backgroundLoadIgnoreMissingModels") == 0) {
            value = configIgnoreMissing;
            return !value.empty();
        }
        return false;
    }
    sc4::IModelBackgroundLoader* CreateBackgroundLoader() override { return loader; }
    sc4::IModelAnimationCoordinator* CreateAnimationCoordinator() override { return coordinator; }
    cISC4OccupantFilter* CreateZoomRotationModelChangeOccupantFilter() override {
        if (zoomFilter) zoomFilter->AddRef();
        return zoomFilter;
    }
    void SetZoomRotationFilter(cISC4OccupantFilter*, int zoom, int rotation) override {
        zoomFilterValues.push_back(zoom);
        zoomFilterValues.push_back(rotation);
    }
    std::uint32_t GetModelInstanceID(cISC4Occupant*, int, int, int, bool, std::uint32_t* a,
                                     std::uint32_t* b) override {
        if (a) *a = 0x11111111;
        if (b) *b = 0x22222222;
        return 0x33333333;
    }
    std::uint32_t MapShadowTexture(std::uint32_t instance, std::uint8_t& rotation, std::uint32_t,
                                   std::uint32_t) override {
        rotation = 0;
        return instance + 1;
    }
    std::uint32_t AddShadowDecal(int, const cGZPersistResourceKey&, float, float, float, float, bool) override {
        ++shadowDecals;
        return 1;
    }
    cIGZMessageServer2* MessageServer() override { return &messages; }
    cIGZMessageServer2* AnimationTickManager() override {
        return tick ? reinterpret_cast<cIGZMessageServer2*>(tick) : nullptr;
    }
    cISC4OccupantManager* OccupantManager() override { return occupantManager; }
    cISC4LotManager* LotManager() override { return lotManager; }
    cIGZUnknown* BuildingDevelopmentSim() override { return nullptr; }
    cIGZUnknown* Terrain() override { return nullptr; }
    cIGZUnknown* ViewUtilities() override { return nullptr; }
};

// The model-maker utility seam: one instance id per model.
struct FakeUtility final : sc4::IModelMakerUtility {
    int modelClass = 0x1000;
    std::vector<std::uint32_t> ids{0xdeadbeef};
    bool usesBuildingStyle = true;
    bool foundationUsesBuildingStyle = true;
    int instanceIdsCalls = 0;
    std::uint32_t lastZoom = 0;
    std::uint32_t lastRotation = 0;
    std::uint32_t propertyInstance = 0x2a0a0000;
    int GetOccupantModelClass(cISC4Occupant*) override { return modelClass; }
    void GetOccupantOrientationAndState(cISC4Occupant*, std::uint32_t& orientation, int& state) override {
        orientation = 0;
        state = 0;
    }
    void GetAllModelInstanceIDs(std::vector<std::uint32_t>& out, std::vector<float>* transforms, cISC4Occupant*,
                                int zoom, int rotation, int, bool, std::uint32_t, std::uint32_t*,
                                std::uint32_t*) override {
        ++instanceIdsCalls;
        lastZoom = static_cast<std::uint32_t>(zoom);
        lastRotation = static_cast<std::uint32_t>(rotation);
        out = ids;
        if (transforms) transforms->assign(out.size() * 3, 0.0f);
    }
    bool OccupantUsesBuildingStyleModels(cISC4Occupant*) override { return usesBuildingStyle; }
    bool FoundationOccupantUsesBuildingStyleModels(cISC4Occupant*) override { return foundationUsesBuildingStyle; }
    std::uint32_t GetModelProperty1Instance(std::uint32_t, int, int) override { return propertyInstance; }
    bool GetAnimatedModelFromResourceKey(const cGZPersistResourceKey&,
                                         s3d::cIS3DModelInstanceAnimated**) override {
        return false;
    }
};

// The version platform: a scripted amount of memory.
struct FakeVersionPlatform final : IRZVersionPlatform {
    std::uint64_t memoryMB = 64;
    bool BundleName(std::string&) override { return false; }
    bool BundleShortVersion(std::string&) override { return false; }
    bool AppPath(std::string&) override { return false; }
    std::uint64_t CPUFrequencyMax() override { return 0; }
    bool MemoryMB(std::uint64_t& physical, std::uint32_t& available) override {
        physical = memoryMB;
        available = static_cast<std::uint32_t>(memoryMB / 2);
        return true;
    }
    bool UserName(std::string&) override { return false; }
    bool ComputerName(std::string&) override { return false; }
};

// ---- a fake message --------------------------------------------------------------------------------------------
struct FakeMessage final : cIGZMessage2Standard {
    std::uint32_t type = 0;
    std::uint64_t data1 = 0;
    std::uint64_t data2 = 0;
    std::uint64_t data3 = 0;
    void* void1 = nullptr;
    void* void2 = nullptr;
    cIGZUnknown* unknown = nullptr;
    int refCount = 1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != 0x65297976) return false;
        *out = this;
        ++refCount;
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refCount); }
    std::uint32_t Release() override {
        if (--refCount == 0) {
            delete this;
            return 0;
        }
        return static_cast<std::uint32_t>(refCount);
    }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
    bool GetHasData1() const override { return data1 != 0; }
    std::uint64_t GetData1() const override { return data1; }
    void* GetVoid1() const override { return void1; }
    void SetData1(std::uint64_t value) override { data1 = value; }
    void SetVoid1(void* value) override { void1 = value; }
    bool GetHasData2() const override { return data2 != 0; }
    std::uint64_t GetData2() const override { return data2; }
    void* GetVoid2() const override { return void2; }
    void SetData2(std::uint64_t value) override { data2 = value; }
    void SetVoid2(void* value) override { void2 = value; }
    bool GetHasData3() const override { return data3 != 0; }
    std::uint64_t GetData3() const override { return data3; }
    void* GetVoid3() const override { return nullptr; }
    void SetData3(std::uint64_t value) override { data3 = value; }
    void SetVoid3(void*) override {}
    bool GetHasData4() const override { return false; }
    std::uint64_t GetData4() const override { return 0; }
    void* GetVoid4() const override { return nullptr; }
    void SetData4(std::uint64_t) override {}
    void SetVoid4(void*) override {}
    bool GetHasString() const override { return false; }
    bool GetString(std::uint32_t, void**) override { return false; }
    void SetString(cIGZString*) override {}
    bool GetHasIGZUnknown() const override { return unknown != nullptr; }
    cIGZUnknown* GetIGZUnknown() override { return unknown; }
    void SetIGZUnknown(cIGZUnknown* value) override { unknown = value; }
    bool GetHasExtra() const override { return false; }
    bool GetExtra(std::uint32_t, void**) override { return false; }
    void SetExtra(cIGZSerializable*) override {}
};

// A stream that records the tagged write calls (the model maker only needs SetUint16 / SetSint32 / SetUint32 /
// SetFloat32 and GetError for its run; the serializable objects go through QueryInterface, which fails here).
struct FakeOStream final : cIGZOStream {
    std::vector<std::pair<char, double>> calls;
    bool error = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void Flush() override {}
    bool SetSint8(std::int8_t) override { return true; }
    bool SetUint8(std::uint8_t) override { return true; }
    bool SetSint16(std::int16_t) override { return true; }
    bool SetUint16(std::uint16_t value) override {
        calls.emplace_back('h', value);
        return true;
    }
    bool SetSint32(std::int32_t value) override {
        calls.emplace_back('i', value);
        return true;
    }
    bool SetUint32(std::uint32_t value) override {
        calls.emplace_back('u', value);
        return true;
    }
    bool SetSint64(std::int64_t) override { return true; }
    bool SetUint64(std::uint64_t) override { return true; }
    bool SetFloat32(float value) override {
        calls.emplace_back('f', value);
        return true;
    }
    bool SetFloat64(double) override { return true; }
    bool SetRZCharStr(const char*) override { return true; }
    bool SetGZStr(const cIGZString&) override { return true; }
    bool SetGZSerializable(const cIGZSerializable&) override { return true; }
    bool SetVoid(const void*, std::uint32_t) override { return true; }
    bool GetError() const override { return error; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

// An input stream answering one version word and nothing else (Read rejects everything but version 2).
struct FakeIStream final : cIGZIStream {
    std::uint16_t version = 2;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Skip(std::uint32_t) override { return false; }
    bool GetSint8(std::int8_t&) override { return false; }
    bool GetUint8(std::uint8_t&) override { return false; }
    bool GetSint16(std::int16_t&) override { return false; }
    bool GetUint16(std::uint16_t& value) override {
        value = version;
        return true;
    }
    bool GetSint32(std::int32_t&) override { return false; }
    bool GetUint32(std::uint32_t&) override { return false; }
    bool GetSint64(std::int64_t&) override { return false; }
    bool GetUint64(std::uint64_t&) override { return false; }
    bool GetFloat32(float&) override { return false; }
    bool GetFloat64(double&) override { return false; }
    bool GetRZCharStr(char*, std::uint32_t) override { return false; }
    bool GetGZStr(cIGZString&) override { return false; }
    bool GetGZSerializable(cIGZSerializable&) override { return false; }
    bool GetVoid(void*, std::uint32_t) override { return false; }
    bool GetError() const override { return false; }
    void SetUserData(cIGZVariant*) override {}
    cIGZVariant* GetUserData() override { return nullptr; }
};

}  // namespace

// ================================================================================================================
// the tests
// ================================================================================================================
int main() {
    FakeVersionPlatform platform;
    SetRZVersionPlatform(&platform);

    // ---- 1. the constructor's defaults ------------------------------------------------------------------------
    {
        auto* maker = new sc4::cSC4ModelMaker();
        CHECK(maker->Zoom() == -1);
        CHECK(maker->Rotation() == -1);
        CHECK(maker->ZoomMask() == 0x1f);
        CHECK(maker->RotationMask() == 0xf);
        CHECK(maker->BackgroundLoadingEnabled());
        CHECK(!maker->IgnoreMissingModels());
        CHECK(maker->LoadRadius(0) == 0x100);   // memset_pattern16
        CHECK(maker->LoadRadius(4) == 0x100);
        CHECK(maker->DeferredDepth() == 0);
        CHECK(maker->DeferredOperations().empty());
        CHECK(maker->ModelFilter() == nullptr);
        maker->Release();   // reference count 0 -> destroys
    }

    // ---- 2. QueryInterface / AddRef / Release --------------------------------------------------------------
    {
        auto* maker = new sc4::cSC4ModelMaker();
        void* out = nullptr;
        CHECK(maker->QueryInterface(1, &out) && out == static_cast<cIGZMessageTarget2*>(maker));
        out = nullptr;
        CHECK(maker->QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(maker));
        out = nullptr;
        CHECK(maker->QueryInterface(sc4::kGZCLSID_cSC4ModelMaker, &out) &&
              out == static_cast<cIGZSerializable*>(maker));
        out = nullptr;
        CHECK(maker->QueryInterface(0xe4fda3d4, &out) && out == static_cast<cIGZSerializable*>(maker));
        out = nullptr;
        CHECK(!maker->QueryInterface(0x12345678, &out) && out == nullptr);
        CHECK(!maker->QueryInterface(0, &out));
        // four successful QueryInterface calls each took a reference
        CHECK(maker->Release() == 3);
        CHECK(maker->Release() == 2);
        CHECK(maker->Release() == 1);
        CHECK(maker->Release() == 0);   // reference count below 2 deletes
    }
    {
        auto* maker = new sc4::cSC4ModelMaker();
        CHECK(maker->AddRef() == 1);
        CHECK(maker->AddRef() == 2);
        CHECK(maker->Release() == 1);   // still alive
        CHECK(maker->Release() == 0);   // deleted
    }

    // ---- 3. Init: filters, messages, view filter, LOD offset ------------------------------------------------
    {
        FakeHost host;
        host.detailLevel = 1;
        FakeFilter viewFilter;
        host.viewFilter = &viewFilter;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        CHECK(maker->Init());
        CHECK(maker->Init());   // idempotent
        CHECK(maker->ModelFilter() != nullptr);
        CHECK(maker->TerrainHidingFilter() != nullptr);
        // the model filter includes the eleven model-bearing types, the terrain filter five of them
        CHECK(maker->ModelFilter()->IsOccupantTypeIncluded(sc4::kModelOccupantTypeBuilding));
        CHECK(maker->ModelFilter()->IsOccupantTypeIncluded(sc4::kModelOccupantTypeMiscB));
        CHECK(!maker->ModelFilter()->IsOccupantTypeIncluded(0x1234));
        CHECK(maker->TerrainHidingFilter()->IsOccupantTypeIncluded(sc4::kModelOccupantTypeNetwork));
        CHECK(!maker->TerrainHidingFilter()->IsOccupantTypeIncluded(sc4::kModelOccupantTypeBuilding));
        // the view filter took one reference from CurrentViewFilter
        CHECK(viewFilter.refCount == 2);
        // the three basic notifications
        CHECK(host.messages.added.size() == 3);
        CHECK(host.messages.added[0] == 0x26d31ec1);
        CHECK(host.messages.added[1] == 0x26c63343);
        CHECK(host.messages.added[2] == 0x26c63345);
        // the LOD offset follows the render detail level (1 here)
        CHECK(sc4::nModelLODOffset == 1);
        // the load radius doubled to 64 MB of memory
        CHECK(maker->LoadRadius(0) == 32);
        CHECK(maker->LoadRadius(1) == 16);
        CHECK(maker->LoadRadius(4) == 6);
        CHECK(maker->Shutdown());
        CHECK(!maker->IsInitialized());
        CHECK(host.messages.removed.size() == 3);
        maker->Release();
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 4. the load-radius rounding / clamping --------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        platform.memoryMB = 4;   // 0.5*4 = 2 -> clamped to 8
        maker->DetermineLoadRadiusAroundVisibleArea();
        CHECK(maker->LoadRadius(0) == 8);
        CHECK(maker->LoadRadius(2) == 6);
        platform.memoryMB = 0x400;   // 1024 MB: 512, 256, 128, 64, 32
        maker->DetermineLoadRadiusAroundVisibleArea();
        CHECK(maker->LoadRadius(0) == 512);
        CHECK(maker->LoadRadius(1) == 256);
        CHECK(maker->LoadRadius(2) == 128);
        CHECK(maker->LoadRadius(3) == 64);
        CHECK(maker->LoadRadius(4) == 32);
        platform.memoryMB = 64;
        maker->Release();
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 5. the occupant messages, deferred and immediate ----------------------------------------------------
    {
        FakeHost host;
        FakeFilter* viewFilter = new FakeFilter();
        host.viewFilter = viewFilter;
        FakeCoordinator* coordinator = new FakeCoordinator();
        host.coordinator = coordinator;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        maker->Init();
        auto* placeable = new FakePlaceable();
        auto* occupant = new FakeOccupant();
        occupant->type = sc4::kModelOccupantTypeBuilding;
        occupant->placeable = placeable;
        occupant->cells = SC4CellRect{0, 0, 1, 1};
        occupant->flags = sc4::kOccupantFlagVisible;

        FakeMessage message;
        message.type = sc4::kMessageOccupantInitialized;
        message.void1 = occupant;
        message.data3 = occupant->type;
        // deferred: the operation is queued, the handler is not called
        maker->EnableDeferredOccupantOperations(true);
        CHECK(maker->DeferredDepth() == 1);
        CHECK(maker->DoMessage(&message));
        CHECK(maker->DeferredOperations().size() == 1);
        CHECK(maker->DeferredOperations()[0].operation == sc4::DeferredOccupantOperation::kCreated);
        CHECK(maker->DeferredOperations()[0].occupant == occupant);
        // showing queues a second record
        message.type = sc4::kMessageOccupantShown;
        message.data2 = 1;
        CHECK(maker->DoMessage(&message));
        CHECK(maker->DeferredOperations().size() == 2);
        CHECK(maker->DeferredOperations()[1].operation == sc4::DeferredOccupantOperation::kShown);
        // committing empties the queue
        maker->EnableDeferredOccupantOperations(false);
        CHECK(maker->DeferredDepth() == 0);
        CHECK(maker->DeferredOperations().empty());
        CHECK(occupant->createdPlaceable);
        // highlight: old == new is ignored, a change records the two bytes
        maker->EnableDeferredOccupantOperations(true);
        message.type = sc4::kMessageOccupantHighlight;
        message.data2 = 0;
        message.data3 = 2;
        CHECK(maker->DoMessage(&message));
        CHECK(maker->DeferredOperations().size() == 1);
        CHECK(maker->DeferredOperations()[0].arg1 == 0);
        CHECK(maker->DeferredOperations()[0].arg2 == 2);
        maker->EnableDeferredOccupantOperations(false);
        // the city messages
        message.type = sc4::kMessageCityLoaded;
        CHECK(maker->DoMessage(&message));
        message.type = sc4::kMessageCityUnloaded;
        CHECK(maker->DoMessage(&message));
        // an unknown message is accepted and ignored
        message.type = 0x12345678;
        CHECK(maker->DoMessage(&message));
        CHECK(maker->DoMessage(nullptr));

        maker->Shutdown();
        maker->Release();
        occupant->Release();
        placeable->Release();
        viewFilter->Release();   // the maker released its own reference in Shutdown
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 6. the model path: AddModels -> LoadBuildingModels -> AddModel -> the placeable object --------------
    {
        FakeHost host;
        FakeFilter* viewFilter = new FakeFilter();
        host.viewFilter = viewFilter;
        FakeResourceManager resources;
        FakeResModel* resource = new FakeResModel();
        resource->bounds.min[0] = 0;
        resource->bounds.min[1] = 0;
        resource->bounds.min[2] = 0;
        resource->bounds.max[0] = 16;
        resource->bounds.max[1] = 8;
        resource->bounds.max[2] = 16;
        resources.resource = resource;
        host.resources = &resources;
        FakeUtility utility;
        utility.ids = {0xdeadbeef};
        sc4::SetModelMakerHost(&host);
        sc4::SetModelMakerUtility(&utility);
        SL::spRM = &resources;

        auto* maker = new sc4::cSC4ModelMaker();
        maker->Init();
        CHECK(maker->OccupantManager() == nullptr);   // Init does not take the managers
        maker->DoPostCityInit();                      // takes the view zoom / rotation and the terrain size
        CHECK(maker->CityRect().x1 == 99);
        CHECK(maker->CityRect().z1 == 99);
        CHECK(maker->Zoom() == 2);
        CHECK(maker->Rotation() == 1);
        maker->UpdateModelLoadRect();
        // 64 MB -> radius[2] = 8; the ground rectangle is 320..640 x 480..800 world units -> cells 20..40 x 30..50
        CHECK(maker->LoadRect().x0 == 20 - 8);
        CHECK(maker->LoadRect().z0 == 30 - 8);
        CHECK(maker->LoadRect().x1 == 40 + 8);
        CHECK(maker->LoadRect().z1 == 50 + 8);

        auto* placeable = new FakePlaceable();
        auto* occupant = new FakeOccupant();
        occupant->type = sc4::kModelOccupantTypeBuilding;
        occupant->placeable = placeable;
        occupant->cells = SC4CellRect{30, 40, 31, 41};
        occupant->position[0] = 100.0f;
        occupant->position[1] = 2.0f;
        occupant->position[2] = 200.0f;
        occupant->flags = sc4::kOccupantFlagVisible;

        CHECK(maker->AddModels(occupant, 2, 1, sc4::kModelOccupantTypeBuilding, false));
        CHECK(utility.instanceIdsCalls == 1);
        CHECK(utility.lastZoom == 2);
        CHECK(utility.lastRotation == 1);
        CHECK(resource->created == 1);
        CHECK(placeable->models.size() == 1);
        CHECK(placeable->zooms == (1u << 2));
        CHECK(placeable->rotations == (1u << 1));
        if (!placeable->models.empty()) {
            auto* model = static_cast<FakeModel*>(placeable->models[0]);
            CHECK(model->owner == occupant);
            CHECK((model->properties & 0x40) != 0);   // the building flag
            // the transform: translation = the occupant position, translation and rotation flagged
            CHECK(model->transform.translation[0] == 100.0f);
            CHECK(model->transform.translation[1] == 2.0f);
            CHECK(model->transform.translation[2] == 200.0f);
            CHECK((model->transform.flags & nS3D::kTransformTranslation) != 0);
            CHECK((model->transform.flags & nS3D::kTransformRotation) != 0);
            // ChangeModels re-adds to the renderer with the layer argument (3 when zero)
            const int changesBefore = host.rendererChanges;
            CHECK(maker->ChangeModels(occupant, 0, 0));
            CHECK(host.rendererChanges == changesBefore + 1);
            CHECK(host.changeCodes.back().second == 3);
            CHECK(maker->ChangeModels(occupant, 0, 2));
            CHECK(host.changeCodes.back().second == 2);
            // the highlight path sets bit 0 and the highlight byte, then re-adds to the renderer
            const int highlightChanges = host.rendererChanges;
            maker->DoOccupantHilighted(occupant, 0, 3);
            CHECK((model->attributes.flags & 1u) == 1u);
            CHECK(((model->attributes.flags >> 8) & 0xff) == 3u);
            CHECK(host.rendererChanges == highlightChanges + 1);
            CHECK(host.changeCodes.back().second == 1);
            // old == new is a no-op
            const int noChanges = host.rendererChanges;
            maker->DoOccupantHilighted(occupant, 2, 2);
            CHECK(host.rendererChanges == noChanges);
            // moving the occupant rewrites the translation only for props / flora
            model->transform.translation[0] = -1;
            maker->DoOccupantMoved(occupant, false, sc4::kModelOccupantTypeBuilding);
            CHECK(model->transform.translation[0] == -1);   // building: untouched
            occupant->type = sc4::kModelOccupantTypeProp;
            const int updatesBefore = host.rendererUpdates;
            maker->DoOccupantMoved(occupant, false, sc4::kModelOccupantTypeProp);
            CHECK(model->transform.translation[0] == 100.0f);
            CHECK((model->transform.flags & nS3D::kTransformTranslation) != 0);
            CHECK(host.rendererUpdates == updatesBefore + 1);
            // building completion (spec model-maker 9.5): the unfinished bit's flips are draw-property changes (renderer
            // ChangeModel 5), every call moves the model (MoveModel). Re-inserting instead left a second grid element behind.
            occupant->type = sc4::kModelOccupantTypeBuilding;
            FakeBuildingOccupant building;
            building.occupant = occupant;
            model->properties &= ~1u;
            const int completionChanges = host.rendererChanges, completionMoves = host.rendererUpdates;
            building.completion = 0.5f;
            maker->DoBuildingCompletionChanged(&building);
            CHECK((model->properties & 1u) == 1u);
            CHECK(host.rendererChanges == completionChanges + 1);
            CHECK(host.changeCodes.back().second == 5);
            CHECK(host.rendererUpdates == completionMoves + 1);
            maker->DoBuildingCompletionChanged(&building);   // still unfinished: no change, a move
            CHECK(host.rendererChanges == completionChanges + 1);
            CHECK(host.rendererUpdates == completionMoves + 2);
            building.completion = 1.0f;
            maker->DoBuildingCompletionChanged(&building);
            CHECK((model->properties & 1u) == 0u);
            CHECK(host.rendererChanges == completionChanges + 2);
            CHECK(host.rendererUpdates == completionMoves + 3);
            // RemoveModels clears the placeable object and calls RemoveAll
            CHECK(maker->RemoveModels(occupant, sc4::kModelOccupantTypeBuilding));
            CHECK(placeable->removeAllCalls == 1);
            CHECK(placeable->models.empty());
        }

        maker->Shutdown();
        maker->Release();
        occupant->Release();
        placeable->Release();
        delete resource;
        SL::spRM = nullptr;
        sc4::SetModelMakerUtility(nullptr);
        sc4::SetModelMakerHost(nullptr);
        viewFilter->Release();
    }

    // ---- 7. the lot condition --------------------------------------------------------------------------------
    {
        FakeHost host;
        FakeLotManager lotManager;
        cSC4Lot lot(10, 10, 2, 2, 0);
        lot.SetCondition(3);
        lotManager.lot = &lot;
        lotManager.cellX = 10;
        lotManager.cellZ = 10;
        host.lotManager = &lotManager;
        FakeOccupantManager manager;
        host.occupantManager = &manager;
        FakeFilter* viewFilter = new FakeFilter();
        host.viewFilter = viewFilter;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        maker->Init();
        host.cityCellX = 10;
        host.cityCellZ = 10;
        auto* occupant = new FakeOccupant();
        std::uint32_t condition = 0;
        // the city rectangle is empty until DoPostCityInit filled it -> no lot
        CHECK(!maker->GetOccupantLotCondition(occupant, condition));
        // give the maker a city rectangle through Read (not needed here): use a fake city size by Init + DoPostCityInit
        FakeCoordinator* coordinator = new FakeCoordinator();
        host.coordinator = coordinator;
        maker->DoPostCityInit();
        CHECK(maker->CityRect().x1 == 99);
        CHECK(maker->IgnoreMissingModels() == false);
        CHECK(maker->GetOccupantLotCondition(occupant, condition));
        CHECK(condition == 3);
        host.cityCellX = 50;   // no lot there
        CHECK(!maker->GetOccupantLotCondition(occupant, condition));
        // the lot-condition message resolves the lot from the occupant's cell and visits the lot's occupants
        host.cityCellX = 10;
        host.cityCellZ = 10;
        auto* placeable = new FakePlaceable();
        auto* neighbour = new FakeOccupant();
        neighbour->placeable = placeable;
        manager.occupants.push_back(neighbour);
        FakeMessage message;
        message.type = sc4::kMessageLotConditionChanged;
        message.unknown = occupant;
        message.data1 = 1;
        message.data2 = 3;
        const int queriesBefore = manager.queries;
        CHECK(maker->DoMessage(&message));
        CHECK(manager.queries == queriesBefore + 1);   // DoLotStateChanged queried the lot's cells
        neighbour->Release();
        placeable->Release();
        maker->Shutdown();
        maker->Release();
        occupant->Release();
        sc4::SetModelMakerHost(nullptr);
        viewFilter->Release();   // the maker dropped its own reference in Shutdown
    }

    // ---- 8. the zoom / rotation and translation updates ------------------------------------------------------
    {
        FakeHost host;
        FakeFilter* viewFilter = new FakeFilter();
        FakeFilter* zoomFilter = new FakeFilter();
        host.viewFilter = viewFilter;
        host.zoomFilter = zoomFilter;
        FakeOccupantManager manager;
        host.occupantManager = &manager;
        FakeLoader* loader = new FakeLoader();
        host.loader = loader;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        maker->Init();
        maker->DoPostCityInit();
        CHECK(maker->BackgroundLoader() == loader);   // background loading is on by default
        auto* placeable = new FakePlaceable();
        auto* occupant = new FakeOccupant();
        occupant->type = sc4::kModelOccupantTypeProp;
        occupant->placeable = placeable;
        occupant->cells = SC4CellRect{30, 40, 31, 41};
        manager.occupants.push_back(occupant);
        // the translation update iterates the model filter's occupants
        maker->DoViewTranslationChange();
        CHECK(manager.queries > 0);
        CHECK(manager.lastFilter == maker->ModelFilter());
        // background loading is on and the city is not loaded yet: the change starts a background load task
        const int startsBefore = loader->startCalls;
        maker->DoZoomAndRotationChange(2, 1, 3, 2);
        CHECK(maker->Zoom() == 3);
        CHECK(maker->Rotation() == 2);
        CHECK(loader->startCalls == startsBefore + 1);
        CHECK(loader->lastZoom == 3 && loader->lastRotation == 2);
        CHECK(host.zoomFilterValues.empty());
        // once the city is loaded the change is done synchronously: the filter gets the new values and the
        // occupants are visited
        FakeMessage loaded;
        loaded.type = sc4::kMessageCityLoaded;
        maker->DoMessage(&loaded);
        const int queriesBefore = manager.queries;
        maker->DoZoomAndRotationChange(3, 2, 4, 3);
        CHECK(loader->cancelCalls == 1);
        CHECK(host.zoomFilterValues.size() == 2);
        CHECK(host.zoomFilterValues[0] == 4 && host.zoomFilterValues[1] == 3);
        CHECK(manager.queries > queriesBefore);
        // suspension / resumption
        FakeMessage message;
        message.type = sc4::kMessageSuspendLoading;
        maker->DoMessage(&message);
        CHECK(loader->suspendCalls == 1);
        message.type = sc4::kMessageResumeLoading;
        maker->DoMessage(&message);
        CHECK(loader->resumeCalls == 1);
        // the background loader's main-thread message
        message.type = sc4::kMessageBackgroundLoaderCommunication;
        maker->DoMessage(&message);
        CHECK(loader->communicationCalls == 1);
        // the lighting update only rebuilds when a channel moved
        maker->DoLightingChanged();
        CHECK(host.lighting[0] == 0.0f);
        host.lighting[0] = 1.0f;
        host.lighting[1] = 0.5f;
        host.lighting[2] = 0.25f;
        maker->DoLightingChanged();
        maker->DoLightingChanged();   // same value: no work
        // the unload path (plenty of memory in the fake platform) keeps the models
        maker->DoViewTranslationChange();
        maker->UnloadUnnecessaryModels();
        maker->Shutdown();
        maker->Release();
        occupant->Release();
        placeable->Release();
        sc4::SetModelMakerHost(nullptr);
        zoomFilter->Release();
    }

    // ---- 9. FindModelTransform ------------------------------------------------------------------------------
    {
        FakeHost host;
        host.rotationAngle = 0.0f;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        const float position[3] = {10.0f, 20.0f, 30.0f};
        cS3DTransform transform;
        maker->FindModelTransform(nullptr, false, 0, position, nullptr, transform);
        CHECK(transform.translation[0] == 10.0f);
        CHECK(transform.translation[1] == 20.0f);
        CHECK(transform.translation[2] == 30.0f);
        CHECK((transform.flags & nS3D::kTransformTranslation) != 0);
        CHECK((transform.flags & nS3D::kTransformRotation) != 0);
        CHECK(transform.rotation[0][0] == 1.0f);   // angle 0
        CHECK(transform.rotation[1][1] == 1.0f);
        CHECK(transform.rotation[2][2] == 1.0f);
        CHECK(transform.rotation[0][2] == 0.0f);
        CHECK(transform.changeCounter == 2);
        // with an offset and a rotation, the translation is the position plus the offset
        const float offset[3] = {1.0f, 2.0f, 3.0f};
        cS3DTransform rotated;
        maker->FindModelTransform(nullptr, true, 0, position, offset, rotated);
        CHECK(rotated.translation[0] == 11.0f);
        CHECK(rotated.translation[1] == 22.0f);
        CHECK(rotated.translation[2] == 33.0f);
        // a 90 degree object rotation turns the x axis into -z
        cS3DTransform quarter;
        maker->FindModelTransform(nullptr, true, 1, position, offset, quarter);
        CHECK(quarter.rotation[0][0] < 0.0001f && quarter.rotation[0][0] > -0.0001f);
        CHECK(quarter.rotation[0][2] < -0.99f && quarter.rotation[0][2] > -1.01f);
        maker->Release();
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 10. the terrain hiding -----------------------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        const sc4::SC4RectT<int> rect{0, 0, 3, 3};
        // the fake reports flag 3 everywhere, so nothing changes and the return value is false
        CHECK(!maker->DoTerrainHidingForCells(rect, false));
        CHECK(host.setCellFlags.empty());
        CHECK(!maker->DoTerrainHidingForCells(rect, true));   // (flags & 3) == 3: the water pass writes nothing
        auto* occupant = new FakeOccupant();
        occupant->cells = SC4CellRect{0, 0, 1, 1};
        CHECK(!maker->DoTerrainHidingForOccupantCells(occupant, sc4::kModelOccupantTypeNetwork, true));
        occupant->Release();
        maker->Release();
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 11. ApplyStateEffectToModels dispatch --------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        auto* placeable = new FakePlaceable();
        auto* occupant = new FakeOccupant();
        occupant->placeable = placeable;
        // every type is accepted; nothing crashes without a texture factory
        CHECK(maker->ApplyStateEffectToModels(occupant, sc4::kModelOccupantTypeBuilding, 0));
        CHECK(maker->ApplyStateEffectToModels(occupant, sc4::kModelOccupantTypeProp, 2));
        CHECK(maker->ApplyStateEffectToModels(occupant, sc4::kModelOccupantTypeNetwork, 1));
        CHECK(maker->ApplyStateEffectToModels(occupant, sc4::kModelOccupantTypeFoundation, 1));
        CHECK(maker->ApplyStateEffectToModels(occupant, 0x1234, 1));
        maker->ApplyLotStateEffect(occupant, sc4::kModelOccupantTypeBuilding, 2);
        maker->RebuildDynamicTexture(0);
        maker->RebuildDynamicTexture(4);
        maker->RebuildDynamicTexture(5);   // out of range
        CHECK(maker->DynamicTexture(0) == nullptr);
        maker->Release();
        occupant->Release();
        placeable->Release();
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 12. the persistence handshake ----------------------------------------------------------------------
    {
        FakeHost host;
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        // Write starts with version 2 and then writes the four cached view words, the city rectangle and +0x58
        FakeOStream out;
        host.viewZoom = 2;
        host.viewRotation = 1;
        CHECK(maker->Write(out));
        CHECK(!out.calls.empty());
        CHECK(out.calls[0].first == 'h' && out.calls[0].second == 2);
        // the four ints after the (null) renderer reference and before the rectangle
        CHECK(out.calls.size() >= 10);
        int intCount = 0;
        for (std::size_t i = 1; i < out.calls.size() && intCount < 4; ++i) {
            if (out.calls[i].first == 'i') ++intCount;
        }
        CHECK(intCount == 4);
        // a failed stream makes Write report failure
        out.error = true;
        CHECK(!maker->Write(out));
        // Read only accepts version 2
        FakeIStream in;
        in.version = 3;
        CHECK(!maker->Read(in));
        in.version = 2;
        // version 2 is accepted; the original never checks the individual reads, only the stream's error flag, so a
        // stream that answers nothing still reports success (quirk kept)
        CHECK(maker->Read(in));
        maker->Release();
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 12b. CreateOccupantShadow: the textured meshes of the model become decals on the land overlay ----------
    {
        FakeHost host;
        FakeResourceManager resources;
        FakeResModel* resource = new FakeResModel();
        resource->bounds.max[0] = 16; resource->bounds.max[1] = 8; resource->bounds.max[2] = 16;
        s3d::ShadowMeshData textured;
        textured.points = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
        textured.uvs = {{0, 0}, {1, 0}, {0, 1}};
        textured.textureLayers = 1;
        textured.texture = 0x1234;
        resource->shadowMeshes = {textured};
        resources.resource = resource;
        host.resources = &resources;
        FakeUtility utility;
        sc4::SetModelMakerHost(&host);
        sc4::SetModelMakerUtility(&utility);
        SL::spRM = &resources;
        auto* maker = new sc4::cSC4ModelMaker();
        maker->Init();
        auto* placeable = new FakePlaceable();
        auto* occupant = new FakeOccupant();
        occupant->type = 0x2890d4de;
        occupant->placeable = placeable;
        CHECK(maker->CreateOccupantShadow(occupant, 2, 1));
        CHECK(host.overlay.adds == 1 && host.overlay.lastTexture == 0x1234 && host.overlay.lastCount == 3);
        CHECK(resources.lastKey.type == 0x11111111 && resources.lastKey.group == 0x22222222 && resources.lastKey.instance == 0x33333333);
        CHECK(host.overlay.lastBox.max[1] > 7.0f);   // quality 5 passes the model's own box
        // too small on screen: no shadow, no decal
        host.shadowMinimum = 100.0f;
        CHECK(!maker->CreateOccupantShadow(occupant, 2, 1) && host.overlay.adds == 1);
        // an occupant that is neither a building, prop, flora nor the shadow type casts none
        host.shadowMinimum = 0.0f;
        occupant->type = 5;
        CHECK(!maker->CreateOccupantShadow(occupant, 2, 1) && host.overlay.adds == 1);
        maker->Release();
        occupant->Release();
        placeable->Release();
        delete resource;
        sc4::SetModelMakerHost(nullptr);
    }

    // ---- 13. the placeholder instance ids ----------------------------------------------------------------
    {
        FakeHost host;
        FakeResourceManager resources;
        FakeResModel* resource = new FakeResModel();
        resources.resource = resource;
        host.resources = &resources;
        sc4::SetModelMakerUtility(nullptr);
        sc4::SetModelMakerHost(&host);
        auto* maker = new sc4::cSC4ModelMaker();
        // without a resource manager there is no placeholder
        auto* occupant = new FakeOccupant();
        CHECK(maker->CreatePlaceHolderModelInstance(occupant, 2, 1, 0, false) == nullptr);
        maker->Release();
        occupant->Release();
        delete resource;
        sc4::SetModelMakerHost(nullptr);
    }

    std::printf(g_failures == 0 ? "all model-maker tests passed\n" : "%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
