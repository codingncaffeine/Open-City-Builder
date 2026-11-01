// cSC4ConstructionOccupant tests: the identity and layout, Init / StaticInit / Shutdown, the lot build-up
// (RebuildModel, DivideLot, the crane, the props, the crew and the effects), the construction stage state machine
// (DoDailySim / DoAnimationTick), the transform / bounds queries, the rotation shading, the draw order and the
// version-5 stream round trip. The neighbours the class talks to are faked through IConstructionOccupantHost.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "fake_gdriver.h"
#include "gz/cGZMessageServer.h"
#include "gz/cIGZMessage2.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZDate.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/ConstructionOccupant.h"
#include "sc4/DrawContext.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/SCPropertyHelp.h"
#include "s3d/ModelInstance.h"

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                          \
    do {                                                                                          \
        if (!(condition)) {                                                                       \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++g_failures;                                                                         \
        }                                                                                         \
    } while (0)
#define CHECK_NEAR(a, b) CHECK(std::fabs(static_cast<double>(a) - static_cast<double>(b)) < 1e-3)

using namespace sc4;

// ---- fakes -----------------------------------------------------------------------------------------------------

struct FakeOccupant final : cISC4Occupant {
    std::uint32_t type = kOccupantTypeConstruction;
    float position[3] = {0, 0, 0};
    float min[3] = {0, 0, 0};
    float max[3] = {0, 0, 0};
    std::uint32_t flags = 0;
    bool initialized = true;
    std::uint32_t refs = 1;
    std::uint32_t addedGroup = 0;
    int positionSets = 0;
    cISCPropertyHolder* holder = nullptr;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4Occupant && iid != 1) return false;
        *out = static_cast<cISC4Occupant*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return initialized; }
    cISCPropertyHolder* AsPropertyHolder() override { return holder; }
    std::uint32_t GetType() const override { return type; }
    void GetPosition(float* xyz) override { std::memcpy(xyz, position, sizeof position); }
    void SetPosition(const float* xyz) override {
        std::memcpy(position, xyz, sizeof position);
        ++positionSets;
    }
    void GetBoundingBox(float* minXyz, float* maxXyz) override {
        std::memcpy(minXyz, min, sizeof min);
        std::memcpy(maxXyz, max, sizeof max);
    }
    void GetBoundingCityCells(SC4CellRect&) override {}
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t message, std::uint32_t) override { lastMessage = message; }
    std::uint32_t GetHighlight() const override { return 0; }
    void SetHighlight(int, bool) override {}
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t group) const override { return group == addedGroup; }
    void AddOccupantGroup(std::uint32_t group) override { addedGroup = group; }
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t flag) override { flags |= flag; }
    void SetAllFlags(std::uint32_t value) override { flags = value; }
    void ClearFlag(std::uint32_t flag) override { flags &= ~flag; }
    bool IsFlagSet(std::uint32_t flag) override { return (flags & flag) == flag; }
    std::uint32_t GetFlags() override { return flags; }

    std::uint32_t lastMessage = 0;
};

struct FakeBuilding final : cISC4BuildingOccupant {
    FakeOccupant* occupant = nullptr;
    float completion = 0.0f;
    BuildingProfile profile;
    std::uint32_t refs = 1;
    std::uint32_t type = 0x1234;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4BuildingOccupant && iid != 1) return false;
        *out = static_cast<cISC4BuildingOccupant*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    cISC4Occupant* AsOccupant() override { return occupant; }
    std::uint32_t GetBuildingType() const override { return type; }
    void SetBuildingType(std::uint32_t value) override { type = value; }
    std::int32_t GetBuildingAge() const override { return 0; }
    void SetBuildingAge(std::int32_t) override {}
    void SetBoundingBox(const float*, const float*) override {}
    const float& GetCompletionPercent() override { return completion; }
    void SetCompletionPercent(float value) override { completion = value; }
    const BuildingProfile& GetBuildingProfile() const override { return profile; }
    const cRZString& GetBuildingName() const override { return name; }
    const cRZString& GetExemplarName() const override { return name; }
    std::int32_t GetOrientation() const override { return 0; }
    bool SetOrientation(std::int32_t) override { return true; }
    bool IsLit() const override { return false; }
    void SetLit(bool) override {}
    void SetName(const cRZString&) override {}
    bool GetName(cRZString&) override { return false; }

    cRZString name;
};

struct FakeLot final : IConstructionLot {
    std::int32_t rect[4] = {2, 3, 4, 5};
    cISC4BuildingOccupant* building = nullptr;
    std::uint32_t refs = 1;
    std::uint32_t state = 0xff;
    std::uint32_t wealth = 0xff;
    std::uint8_t reason = 0xff;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4Lot && iid != 1) return false;
        *out = static_cast<IConstructionLot*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    void GetCellRect(std::int32_t out[4]) override { std::memcpy(out, rect, sizeof rect); }
    void SetState(std::uint32_t newState, std::uint32_t newWealth, std::uint8_t newReason) override {
        state = newState;
        wealth = newWealth;
        reason = newReason;
    }
    cISC4BuildingOccupant* GetBuilding() override { return building; }
};

struct FakeProp final : IConstructionProp {
    FakeOccupant* occupant = nullptr;
    std::uint32_t refs = 1;
    std::int32_t orientation = -1;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4PropOccupant && iid != 1) return false;
        *out = static_cast<IConstructionProp*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    cISC4Occupant* AsOccupant() override { return occupant; }
    void SetOrientation(std::int32_t value) override { orientation = value; }
};

struct FakeEffect final : IConstructionEffect {
    FakeOccupant* occupant = nullptr;
    std::uint32_t refs = 1;
    std::string name;
    float position[3] = {0, 0, 0};
    int inits = 0;
    int starts = 0;
    int stops = 0;

    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cISC4PropOccupantVisualEffect && iid != 1) return false;
        *out = static_cast<IConstructionEffect*>(this);
        ++refs;
        return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    bool Init() override {
        ++inits;
        return true;
    }
    void SetPosition(const float* xyz) override { std::memcpy(position, xyz, sizeof position); }
    void SetEffectType(const char* value) override { name = value ? value : ""; }
    bool StartEffect(bool) override {
        ++starts;
        return true;
    }
    bool StopEffect(bool) override {
        ++stops;
        return true;
    }
    cISC4Occupant* AsOccupant() override { return occupant; }
};

struct FakeAutomaton final : IConstructionAutomaton {
    std::uint32_t refs = 1;
    bool flag2 = true;
    float orientation[3] = {0, 0, 1};

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    cISC4Occupant* AsOccupant() override { return nullptr; }
    void SetAutomataFlag(std::uint32_t flag, bool on) override {
        if (flag == 2) flag2 = on;
    }
    void SetOrientation(const float* direction) override {
        std::memcpy(orientation, direction, sizeof orientation);
    }
};

struct FakeTexture final : IConstructionTexture {
    std::int32_t id = 0;
    std::uint32_t refs = 1;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    std::int32_t TextureId() const override { return id; }
};

struct FakeTileOccupant final : cISC4LotBaseTextureOccupant {
    FakeOccupant* occupant = nullptr;
    std::uint32_t refs = 1;
    std::vector<LotBaseTextureSpecification> specs;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    cISC4Occupant* AsOccupant() override { return occupant; }
    int GetUsage() const override { return 0; }
    void SetUsage(int) override {}
    std::size_t GetCellCoverage(std::vector<SC4PointInt>&, std::vector<bool>*) const override { return 0; }
    bool IsCellCovered(int, int, bool*) const override { return false; }
    void SetTextureSpecification(const LotBaseTextureSpecification* data, std::uint32_t count, bool*) override {
        specs.assign(data, data + count);
    }
    void UpdateVertexAltitudes(bool, bool, bool*) override {}
    void LoadModels(bool) override {}
    void UnloadModels() override {}
};

struct FakeModel final : s3d::cS3DModelInstanceBasic {
    cS3DBoundingBox box;
    cS3DTransform transform;
    int transformSets = 0;
    int draws = 0;

    void GetWorldBounds(cS3DBoundingBox& out) override { out = box; }
    void SetTransform(const cS3DTransform& value) override {
        transform = value;
        ++transformSets;
    }
    int Draw(SC4DrawContext*, bool) override {
        ++draws;
        return 1;
    }
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
};

struct FakeFilter final : cISC4OccupantFilter {
    bool include = true;
    std::uint32_t refs = 1;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (refs > 1) return --refs;
        delete this;
        return 0;
    }
    bool IsOccupantIncluded(cISC4Occupant*) override { return include; }
    bool IsOccupantTypeIncluded(std::uint32_t) override { return include; }
    bool IsPropertyHolderIncluded(cISCPropertyHolder*) override { return include; }
};

struct FakeRenderer final : IConstructionRenderer {
    std::set<s3d::cIS3DModelInstance*> models;
    int inserts = 0;
    int removes = 0;

    bool IsModelInserted(s3d::cIS3DModelInstance* model) override { return models.count(model) != 0; }
    bool InsertModel(s3d::cIS3DModelInstance* model) override {
        models.insert(model);
        ++inserts;
        return true;
    }
    bool RemoveModel(s3d::cIS3DModelInstance* model) override {
        models.erase(model);
        ++removes;
        return true;
    }
};

struct FakeMessages final : cIGZMessageServer2 {
    std::vector<std::uint32_t> added;
    std::vector<std::uint32_t> removed;
    std::vector<std::uint32_t> posted;

    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool MessageSend(cIGZMessage2* message) override {
        posted.push_back(message->GetType());
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        posted.push_back(message->GetType());
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        added.push_back(type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t type) override {
        removed.push_back(type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeHost final : IConstructionOccupantHost {
    FakeMessages messages;
    cRZRandom random{12345};
    sc4::cSCPropertyHolder settings;
    FakeLot* lot = nullptr;
    FakeBuilding* building = nullptr;
    FakeOccupant* buildingOccupant = nullptr;
    FakeRenderer renderer;
    FakeFilter* filter = nullptr;
    std::vector<FakeModel*> models;
    std::vector<FakeTexture*> textures;
    std::vector<FakeEffect*> effects;
    std::vector<FakeProp*> props;
    std::vector<FakeAutomaton*> automata;
    std::vector<FakeTileOccupant*> tiles;
    sc4::PropProfile profiles[8];
    std::vector<std::uint32_t> profileTypes;
    float terrainHeight = 0.0f;
    int terrainCellsX = 64;
    int terrainCellsZ = 64;
    std::uint32_t day = 100;
    int detailLevel = 3;
    int notifications[4] = {0, 0, 0, 0};
    int tickAdded = 0;
    int tickRemoved = 0;
    int inserted = 0;
    int removedOccupants = 0;
    int moved = 0;
    int automataRemoved = 0;
    int removedProps = 0;
    int projected = 0;
    float lastProjected[3] = {0, 0, 0};
    std::uint8_t light[3] = {100, 60, 20};

    cIGZMessageServer2* MessageServer() override { return &messages; }
    void AddTickNotification(cIGZMessageTarget2*, int channel) override {
        if (channel == kAnimationTickChannel) ++tickAdded;
    }
    void RemoveTickNotification(cIGZMessageTarget2*, int channel) override {
        if (channel == kAnimationTickChannel) ++tickRemoved;
    }
    cRZRandom* Random() override { return &random; }
    cISCPropertyHolder* ConstructionSettings() override { return &settings; }
    s3d::cIS3DModelInstance* CreateModelInstance(const cGZPersistResourceKey&, std::uint32_t) override {
        auto* model = new FakeModel();
        // A crane model 6 m long with 3 m arms.
        model->box.min[0] = -3.0f;
        model->box.min[1] = -0.5f;
        model->box.min[2] = -0.5f;
        model->box.max[0] = 3.0f;
        model->box.max[1] = 0.5f;
        model->box.max[2] = 0.5f;
        model->AddRef();
        model->AddRef();   // the test keeps one reference so the pointer stays valid
        models.push_back(model);
        return model;
    }
    IConstructionTexture* CreateTextureBinding(const cGZPersistResourceKey& key) override {
        auto* texture = new FakeTexture();
        texture->id = static_cast<std::int32_t>(key.instance);
        texture->AddRef();
        textures.push_back(texture);
        return texture;
    }
    const sc4::PropProfile* PropProfile(std::uint32_t type) override {
        for (std::size_t i = 0; i < profileTypes.size(); ++i) {
            if (profileTypes[i] == type) return &profiles[i];
        }
        return nullptr;
    }
    bool CreateProp(std::uint32_t, IConstructionProp** out) override {
        auto* prop = new FakeProp();
        prop->occupant = new FakeOccupant();
        prop->AddRef();
        props.push_back(prop);
        *out = prop;
        return true;
    }
    void AddLotProp(IConstructionLot*, cISC4Occupant*) override {}
    void RemoveProp(cISC4Occupant* occupant) override {
        ++removedProps;
        (void)occupant;
    }
    void RemoveAutomaton(IConstructionAutomaton* automaton) override {
        ++automataRemoved;
        if (automaton) automaton->Release();
    }
    IConstructionEffect* CreatePropEffect() override {
        auto* effect = new FakeEffect();
        effect->occupant = new FakeOccupant();
        effect->AddRef();
        effects.push_back(effect);
        return effect;
    }
    bool CreateLotBaseTextureOccupant(IConstructionLot*, cISC4LotBaseTextureOccupant** out) override {
        auto* tile = new FakeTileOccupant();
        tile->occupant = new FakeOccupant();
        tile->AddRef();
        tiles.push_back(tile);
        *out = tile;
        return true;
    }
    cISC4BuildingOccupant* LotBuilding(IConstructionLot*) override { return building; }
    IConstructionAutomaton* CreateAutomaton(std::uint32_t, const float[3], std::uint32_t,
                                            const cGZPersistResourceKey&, std::uint8_t) override {
        auto* automaton = new FakeAutomaton();
        automaton->AddRef();
        automata.push_back(automaton);
        return automaton;
    }
    bool InsertOccupant(cISC4Occupant*, std::uint32_t) override {
        ++inserted;
        return true;
    }
    bool RemoveOccupant(cISC4Occupant*, bool, std::uint32_t) override {
        ++removedOccupants;
        return true;
    }
    bool MoveOccupant(cISC4Occupant*, bool) override {
        ++moved;
        return true;
    }
    float TerrainAltitude(float x, float z) override {
        // A slight slope so the four corners differ.
        return terrainHeight + x * 0.01f + z * 0.02f;
    }
    int TerrainCellCountX() override { return terrainCellsX; }
    int TerrainCellCountZ() override { return terrainCellsZ; }
    bool TerrainCellHasWater(int x, int z) override { return x == 3 && z == 3; }
    void CityCellToPosition(int cellX, int cellZ, float& x, float& z) override {
        x = static_cast<float>(cellX) * 16.0f;
        z = static_cast<float>(cellZ) * 16.0f;
    }
    std::uint32_t SimulatorDay() override { return day; }
    cISC4OccupantFilter* ViewFilter() override { return filter; }
    void ProjectToViewCoordinates(float point[3]) override {
        ++projected;
        std::memcpy(lastProjected, point, sizeof lastProjected);
    }
    void TransformBoundingBox(const cS3DTransform& transform, cS3DBoundingBox& box) override {
        // The same math as SC4ViewUtilities::TransformBoundingBox (0x100176ae6).
        const float scale = transform.scale;
        float boxMin[3] = {box.min[0] * scale, box.min[1] * scale, box.min[2] * scale};
        float boxMax[3] = {box.max[0] * scale, box.max[1] * scale, box.max[2] * scale};
        for (int row = 0; row < 3; ++row) {
            float low = transform.translation[row];
            float high = transform.translation[row];
            for (int column = 0; column < 3; ++column) {
                const float a = boxMin[column] * transform.rotation[row][column];
                const float b = boxMax[column] * transform.rotation[row][column];
                low += (b <= a) ? b : a;
                high += (b <= a) ? a : b;
            }
            box.min[row] = low;
            box.max[row] = high;
        }
    }
    int RenderDetailLevel() override { return detailLevel; }
    void LightColourForDirection(const float[3], std::uint8_t rgb[3]) override {
        rgb[0] = light[0];
        rgb[1] = light[1];
        rgb[2] = light[2];
    }
    IConstructionRenderer* Renderer() override { return &renderer; }
    IConstructionLot* AdoptLot(cIGZUnknown* object) override {
        void* raw = nullptr;
        return object && object->QueryInterface(kGZIID_cISC4Lot, &raw) ? static_cast<IConstructionLot*>(static_cast<cIGZUnknown*>(raw)) : nullptr;
    }
};

FakeHost* g_host = nullptr;

cSC4ConstructionOccupant* MakeOccupant() {
    auto* occupant = new cSC4ConstructionOccupant();
    occupant->AddRef();
    return occupant;
}

void ReleaseOccupant(cSC4ConstructionOccupant*& occupant) {
    if (occupant) {
        occupant->Release();
        occupant = nullptr;
    }
}

}  // namespace

int main() {
    // ---- the tuning exemplar: everything StaticInit reads ------------------------------------------------
    FakeHost host;
    g_host = &host;
    SetConstructionOccupantHost(&host);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertySegmentHeight, 2.0f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyPostWidth, 1.0f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyScaffoldGap, 0.5f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyPlankWidth, 0.5f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyPlankTextureScale, 0.5f);      // -> 2
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyScaffoldTextureScale, 0.25f);  // -> 4
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyGirderTextureScale, 0.125f);   // -> 8
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyScaffoldGrowthRate, 0.5f);     // -> 2
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyScaffoldRemovalRate, 0.25f);   // -> 4
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyCranePostWidth, 0.5f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyCranePostToBuildingGap, 0.25f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyCraneHeightAboveScaffold, 2.0f);
    cSCPropertyHelp::SetPropertyValue(&host.settings, kPropertyCraneTimeOfFullRotation, 10.0f);
    for (std::uint32_t type : {0x02000000u, 0x02010000u, 0x02050000u, 0x03040000u, 0x03050000u, 0x03030000u,
                               0x02060000u}) {
        sc4::PropProfile profile;
        profile.size[0] = 1.0f;
        profile.size[1] = 1.0f;
        profile.size[2] = 1.0f;
        host.profiles[host.profileTypes.size()] = profile;
        host.profileTypes.push_back(type);
    }

    // ---- the building and the lot ---------------------------------------------------------------------------
    auto* buildingOccupant = new FakeOccupant();
    buildingOccupant->min[0] = 34.0f;
    buildingOccupant->min[1] = 1.34f;
    buildingOccupant->min[2] = 50.0f;
    buildingOccupant->max[0] = 44.0f;
    buildingOccupant->max[1] = 5.34f;
    buildingOccupant->max[2] = 60.0f;
    host.buildingOccupant = buildingOccupant;
    sc4::cSCPropertyHolder buildingProperties;
    cSCPropertyHelp::SetPropertyValue(&buildingProperties, kPropertyConstructionCraneType, static_cast<std::uint8_t>(3));
    buildingOccupant->holder = &buildingProperties;
    auto* building = new FakeBuilding();
    building->occupant = buildingOccupant;
    building->profile.growthStage = 10;
    host.building = building;
    auto* lot = new FakeLot();
    lot->building = building;
    host.lot = lot;

    // ---- identity and the constructor's defaults ------------------------------------------------------------
    {
        cSC4ConstructionOccupant* occupant = MakeOccupant();
        CHECK(occupant->GetGZCLSID() == kGZCLSID_cSC4ConstructionOccupant);
        CHECK(occupant->GetType() == kOccupantTypeConstruction);
        CHECK(occupant->GetProperties() == 5);
        CHECK(occupant->Stage() == 0);
        CHECK(occupant->SegmentCount() == 0);
        CHECK(!occupant->HasCrane());
        CHECK(occupant->Lot() == nullptr);
        CHECK(occupant->Bounds().min[0] == 1.0f && occupant->Bounds().max[0] == 0.0f);
        CHECK(occupant->RoofBounds().min[1] == 1.0f && occupant->RoofBounds().max[1] == 0.0f);
        CHECK(occupant->AsOccupant() == static_cast<cISC4Occupant*>(occupant));
        CHECK(occupant->GetOwner() == static_cast<cIGZUnknown*>(static_cast<cISC4Occupant*>(occupant)));
        void* raw = nullptr;
        CHECK(occupant->QueryInterface(kGZIID_cISC4ConstructionOccupant, &raw) && raw != nullptr);
        // SetPosition is a no-op and GetPosition is the box centre.
        const float position[3] = {7, 8, 9};
        occupant->SetPosition(position);
        float centre[3] = {0, 0, 0};
        occupant->GetPosition(centre);
        CHECK_NEAR(centre[0], 0.5f);
        CHECK_NEAR(centre[1], 0.5f);
        CHECK_NEAR(centre[2], 0.5f);
        float min[3] = {0, 0, 0}, max[3] = {0, 0, 0};
        occupant->GetBoundingBox(min, max);
        CHECK(min[0] == 1.0f && max[0] == 0.0f);
        // The transform round trip bumps the change counter.
        cS3DTransform transform;
        transform.translation[0] = 3.0f;
        transform.flags = nS3D::kTransformTranslation;
        occupant->SetTransform(transform);
        cS3DTransform out;
        occupant->GetTransform(out);
        CHECK_NEAR(out.translation[0], 3.0f);
        CHECK(out.changeCounter == 2);
        ReleaseOccupant(occupant);
    }

    // ---- Init: notifications, texture bindings, the occupant group ------------------------------------------
    cSC4ConstructionOccupant* occupant = MakeOccupant();
    {
        CHECK(occupant->Init());
        CHECK(occupant->IsInitialized());
        CHECK(host.messages.added.size() == 4);
        CHECK(host.tickAdded == 1);
        CHECK(host.textures.size() == 25);   // 5 rows x 5 zoom levels (the girder row is never filled)
        CHECK(occupant->IsOccupantGroup(kOccupantGroupConstruction));
        std::uint32_t sort = 0;
        occupant->GetSortValue(sort);
        CHECK(sort == 0x696f3000u);          // the first side's first zoom level texture id
        // StaticInit ran once and inverted the scale properties.
        CHECK_NEAR(sfSegmentHeight, 2.0f);
        CHECK_NEAR(sfPlankTextureScale, 2.0f);
        CHECK_NEAR(sfScaffoldTextureScale, 4.0f);
        CHECK_NEAR(sfGirderTextureScale, 8.0f);
        CHECK_NEAR(sfScaffoldGrowthRate, 2.0f);
        CHECK_NEAR(sfScaffoldRemovalRate, 4.0f);
        // 5 universal + 2 ground-only props have a profile.
        CHECK(snUniversalPropCount == 5);
        CHECK(snTotalPropCount == 8);   // the ground-only list repeats the 0x02050000 prop
        CHECK(sPropList.size() == 8);
        CHECK_NEAR(sPropList[0].sizeX, 1.0f);
        // The three crane models give 6 m length and 3 m arms (|min| and |max| of the box).
        CHECK_NEAR(sfCraneLength[0], 6.0f);
        CHECK_NEAR(sfCraneFrontArmLength[0], 3.0f);
        CHECK_NEAR(sfCraneBackArmLength[0], 3.0f);
        CHECK(occupant->Init());             // idempotent
        CHECK(host.textures.size() == 25);
    }

    // ---- SetLot: the model, the lots, the props, the crew and the effects ----------------------------------
    {
        const std::size_t postedBefore = host.messages.posted.size();
        occupant->SetLot(lot);
        CHECK(occupant->Lot() == lot);
        CHECK(occupant->Building() == building);
        CHECK(occupant->Stage() == 0);
        // The building box is 10 x 4 x 10 at (34, 0, 50); the terrain slopes, the lowest corner is the ground.
        float ground = host.TerrainAltitude(34.0f, 50.0f);
        ground = std::min(ground, host.TerrainAltitude(44.0f, 50.0f));
        ground = std::min(ground, host.TerrainAltitude(34.0f, 60.0f));
        ground = std::min(ground, host.TerrainAltitude(44.0f, 60.0f));
        CHECK_NEAR(occupant->BuildingBaseY(), 1.44f);
        // segments = floor((maxY - base) / 2) + 1 = floor(1.95) + 1 = 2
        CHECK(occupant->SegmentCount() == 2);
        // The footprint grew by the 0.5 m scaffold gap.
        CHECK_NEAR(occupant->RoofBounds().min[0], 33.5f);
        CHECK_NEAR(occupant->RoofBounds().max[0], 44.5f);
        CHECK_NEAR(occupant->RoofBounds().min[2], 49.5f);
        CHECK_NEAR(occupant->RoofBounds().max[2], 60.5f);
        CHECK_NEAR(occupant->RoofBounds().min[1], ground);
        // SetLot then drops the scaffold: SetScaffoldHeight stores the scaffold height in roof max Y (+0x118).
        CHECK_NEAR(occupant->RoofBounds().max[1], ground + 0.001f);
        CHECK_NEAR(occupant->Bounds().min[0], 32.5f);   // roof + post width (1 m)
        CHECK_NEAR(occupant->Bounds().max[0], 45.5f);
        CHECK_NEAR(occupant->Bounds().max[1], 5.34f);
        // RebuildModel again: SetLot's SetScaffoldHeight moved the top vertices to the initial scaffold height.
        occupant->RebuildModel();
        CHECK_NEAR(occupant->RoofBounds().max[1], 5.34f);
        // The four sides' vertices: the bottom ones on the ground, the top ones at max Y.
        const auto& side0 = occupant->SideVertices(0);
        CHECK(side0.size() == 4);
        CHECK_NEAR(side0[0].x, 33.5f);
        CHECK_NEAR(side0[0].z, 49.5f);
        CHECK_NEAR(side0[0].y, ground);
        CHECK_NEAR(side0[1].y, 5.34f);
        CHECK_NEAR(side0[2].x, 44.5f);
        CHECK_NEAR(side0[3].y, ground);
        // The planks (u/v are the texture coordinates) and the girders.
        CHECK(occupant->Planks().size() == 16);
        CHECK_NEAR(occupant->Planks()[0].y, 5.34f);
        CHECK_NEAR(occupant->Planks()[0].u, occupant->Planks()[0].x * sfPlankTextureScale);
        CHECK_NEAR(occupant->Planks()[0].v, occupant->Planks()[0].z * sfPlankTextureScale);
        CHECK(occupant->Girders().size() == 4);
        // The four corner posts: 20 vertices each, the top ones at maxY + postWidth.
        CHECK(occupant->Posts().size() == 80);
        CHECK_NEAR(occupant->Posts()[0].y, ground);
        CHECK_NEAR(occupant->Posts()[1].y, 6.34f);
        CHECK_NEAR(occupant->Posts()[16].y, 6.34f);
        CHECK((occupant->Posts()[0].colour & 0xffffffffu) == 0xff000000u);
        // The scaffold starts just above the ground and the completion is zero.
        CHECK_NEAR(occupant->ScaffoldHeight(), ground + 0.001f);
        CHECK_NEAR(building->completion, 0.0f);
        // Two construction messages (stage + complete) were posted.
        CHECK(host.messages.posted.size() == postedBefore + 2);
        CHECK(host.messages.posted[postedBefore] == kMessageConstructionStage);
        CHECK(host.messages.posted[postedBefore + 1] == kMessageConstructionComplete);
        // The crane: the model was created and inserted into the renderer.
        CHECK(occupant->HasCrane());
        CHECK(!host.models.empty());
        CHECK(host.models.back()->transformSets >= 1);
        // The inner crane's yaw range is one of the four documented pairs (0x10017d204).
        CHECK(occupant->CraneYawMin() <= occupant->CraneYawMax());
        const bool known = (occupant->CraneYawMin() == 0.0f && occupant->CraneYawMax() == 1.5707964f) ||
                           (occupant->CraneYawMin() == 0.7853982f && occupant->CraneYawMax() == 2.3561945f) ||
                           (occupant->CraneYawMin() == 1.5707964f && occupant->CraneYawMax() == 3.1415927f) ||
                           (occupant->CraneYawMin() == 2.3561945f && occupant->CraneYawMax() == 4.712389f);
        CHECK(known);
        CHECK(host.renderer.inserts >= 1);
        CHECK(host.renderer.IsModelInserted(occupant));
        // DivideLot made 8 rectangles around the building (the first one is the south strip).
        // They are only visible through the props that were placed, so re-run DivideLot into a local array.
        cS3DBoundingBox boxes[12];
        for (auto& box : boxes) {
            box.min[0] = box.min[1] = box.min[2] = 1.0f;
            box.max[0] = box.max[1] = box.max[2] = 0.0f;
        }
        occupant->DivideLot(boxes);
        CHECK_NEAR(boxes[0].min[0], 33.5f);
        CHECK_NEAR(boxes[0].min[2], 48.0f);   // the lot's min Z (cell 3 * 16)
        CHECK_NEAR(boxes[0].max[2], 49.5f);
        CHECK_NEAR(boxes[7].max[2], 96.0f);   // the lot's max Z ((5 + 1) * 16)
        CHECK_NEAR(boxes[7].min[0], 32.0f);   // the lot's min X (cell 2 * 16)
        // The props, effects and crew were created.
        CHECK(!host.props.empty());
        CHECK(!host.effects.empty());
        CHECK(!occupant->Sparks().empty());
        CHECK(occupant->Dust() != nullptr);
        CHECK(!occupant->GroundProps().empty());
        CHECK(!occupant->Crews().empty());
        CHECK(!host.tiles.empty());
        CHECK(!host.tiles.back()->specs.empty());
        // The lot texture covers every dry cell of the lot (3 x 3 minus the water cell (3,3)).
        CHECK(host.tiles.back()->specs.size() == 8);
    }

    // ---- Draw: the side order, the texture rows and the primitive counts ------------------------------------
    {
        sc4test::FakeGDriver driver;
        SC4DrawContext context;
        context.SetDriver(&driver);
        context.Init();
        context.SetZoomAndRotation(0, 1);   // rotation 1: the side order is 2, 0, 3, 1
        const int before = driver.drawArrayCalls;
        CHECK(occupant->Draw(&context, false) == 1);
        // Four sides, the planks, the posts and the crane base = 7 draw calls (the girder row has no binding).
        CHECK(driver.drawArrayCalls == before + 7);
        CHECK(driver.lastPrimitive == 6);
        CHECK(driver.lastDrawCount == 0x10);
        CHECK(host.projected > 0);
        CHECK(occupant->Draw(&context, true) == 1);   // the shadow pass does nothing
        CHECK(driver.drawArrayCalls == before + 7);
    }

    // ---- AddToRenderer with a view filter -------------------------------------------------------------------
    {
        auto* filter = new FakeFilter();
        filter->include = false;
        host.filter = filter;
        host.renderer.removes = 0;
        occupant->AddToRenderer();
        CHECK(!occupant->IsFlagSet(kOccupantFlagInRenderer));
        CHECK(host.renderer.removes >= 1);
        filter->include = true;
        host.renderer.inserts = 0;
        occupant->AddToRenderer();
        CHECK(occupant->IsFlagSet(kOccupantFlagInRenderer));
        CHECK(host.renderer.inserts >= 1);
        host.filter = nullptr;
        filter->Release();
    }

    // ---- the stage state machine ---------------------------------------------------------------------------
    {
        // Day 1: stage 0 -> 1, one segment done.
        host.day += 10;
        FakeBuilding* fakeBuilding = building;
        occupant->DoDailySim();
        CHECK(occupant->Stage() == 1);
        CHECK(occupant->CurrentSegment() == 1);
        // Animation raises the scaffold towards the building's base.
        occupant->DoAnimationTick(1000);
        CHECK(occupant->ScaffoldHeight() > 0.0f);
        // Day 2: stage 1 -> 2.
        host.day += 10;
        occupant->DoDailySim();
        CHECK(occupant->Stage() == 2);
        // A few animation ticks give a partial completion.
        for (int i = 0; i < 4; ++i) occupant->DoAnimationTick(1000);
        CHECK(fakeBuilding->completion >= 0.0f);
        // Day 3: stage 2 -> 1 with the second segment.
        host.day += 10;
        occupant->DoDailySim();
        CHECK(occupant->Stage() == 1);
        CHECK(occupant->CurrentSegment() == 2);
        // Day 4: stage 1 -> 2 again.
        host.day += 10;
        occupant->DoDailySim();
        CHECK(occupant->Stage() == 2);
        // Day 5: stage 2 -> 3 (all segments placed; the roof props are removed).
        host.day += 10;
        occupant->DoDailySim();
        CHECK(occupant->Stage() == 3);
        CHECK(occupant->RoofProps().empty());
        // The scaffold comes down.
        const float before = occupant->ScaffoldHeight();
        occupant->DoAnimationTick(1000);
        CHECK(occupant->ScaffoldHeight() <= before);
        // Day 6: stage 3 -> 4, completion 1, the lot becomes livable and the occupant shuts down.
        host.day += 10;
        occupant->DoDailySim();
        CHECK(occupant->Stage() == 4);
        CHECK_NEAR(fakeBuilding->completion, 1.0f);
        CHECK(lot->state == 3);
        CHECK(lot->wealth == 0);
        CHECK(lot->reason == 0);
        CHECK(host.removedOccupants >= 1);
        CHECK(host.removedProps >= 1);
        CHECK(host.removedProps >= 1);
        CHECK(!occupant->IsInitialized());
        CHECK(host.messages.removed.size() == 4);
        CHECK(host.tickRemoved == 1);
    }

    // ---- rotation shading -----------------------------------------------------------------------------------
    {
        auto* rotated = new cSC4ConstructionOccupant();
        rotated->AddRef();
        rotated->Init();
        rotated->SetLot(lot);
        rotated->DoRotationChange(0);
        CHECK(!rotated->RotationChanged());
        // Every post vertex gets the halved light colour (0xff000000 | b<<16 | g<<8 | r).
        const std::uint32_t expected = 0xff000000u | (std::uint32_t(host.light[2] >> 1) << 16) |
                                       (std::uint32_t(host.light[1] >> 1) << 8) | std::uint32_t(host.light[0] >> 1);
        CHECK(rotated->Posts()[0].colour == expected);
        CHECK(rotated->Posts()[79].colour == expected);
        // The crane base plate uses the prop orientation order (0, 2, 1, 3) but the fake light is uniform.
        CHECK(rotated->CranePosts()[0].colour == expected);
        ReleaseOccupant(rotated);
    }

    // ---- bounds with a rotated and scaled transform ---------------------------------------------------------
    {
        cS3DBoundingBox world;
        occupant->GetWorldBounds(world);
        cS3DBoundingBox plain;
        occupant->GetBounds(plain);
        CHECK_NEAR(plain.min[0], world.min[0]);
        CHECK_NEAR(plain.max[2], world.max[2]);
        // A translation only: the query subtracts it (the transform is the world-to-model one, 0x10017c0e2).
        cS3DTransform transform;
        transform.flags = nS3D::kTransformTranslation;
        transform.translation[0] = 5.0f;
        occupant->SetTransform(transform);
        cS3DBoundingBox moved;
        occupant->GetBounds(moved);
        CHECK_NEAR(moved.min[0], world.min[0] - 5.0f);
        CHECK_NEAR(moved.max[0], world.max[0] - 5.0f);
        // A 90-degree Y rotation: the x and z extents swap.
        transform.rotation[0][0] = 0.0f;
        transform.rotation[0][1] = 0.0f;
        transform.rotation[0][2] = 1.0f;
        transform.rotation[1][0] = 0.0f;
        transform.rotation[1][1] = 1.0f;
        transform.rotation[1][2] = 0.0f;
        transform.rotation[2][0] = -1.0f;
        transform.rotation[2][1] = 0.0f;
        transform.rotation[2][2] = 0.0f;
        transform.flags = nS3D::kTransformRotation;
        occupant->SetTransform(transform);
        cS3DBoundingBox turned;
        occupant->GetBounds(turned);
        CHECK_NEAR(turned.max[0] - turned.min[0], world.max[2] - world.min[2]);
        CHECK_NEAR(turned.max[2] - turned.min[2], world.max[0] - world.min[0]);
        CHECK_NEAR(turned.max[1], world.max[1]);
        // A uniform scale is inverted before the box is scaled.
        transform.flags = nS3D::kTransformScale;
        transform.scale = 2.0f;
        occupant->SetTransform(transform);
        cS3DBoundingBox scaled;
        occupant->GetBounds(scaled);
        CHECK_NEAR(scaled.max[0] - scaled.min[0], (world.max[0] - world.min[0]) * 0.5f);
        cS3DTransform out;
        occupant->GetTransform(out);
        CHECK_NEAR(out.translation[0], 5.0f);
        CHECK(out.flags == nS3D::kTransformScale);
    }

    // ---- the stream round trip ------------------------------------------------------------------------------
    {
        PortMemoryOStream stream;
        CHECK(occupant->Write(stream));
        CHECK(stream.bytes().size() > 8);
        auto* loaded = MakeOccupant();
        PortMemoryIStream input(stream.bytes().data(), stream.bytes().size());
        CHECK(loaded->Read(input));
        CHECK(loaded->Stage() == occupant->Stage());
        CHECK(loaded->SegmentCount() == occupant->SegmentCount());
        CHECK_NEAR(loaded->BuildingBaseY(), occupant->BuildingBaseY());
        CHECK_NEAR(loaded->RoofBounds().max[1], occupant->RoofBounds().max[1]);
        CHECK(loaded->CraneType() == occupant->CraneType());
        CHECK(loaded->Crews().size() == occupant->Crews().size());
        // A version below 4 is rejected.
        const unsigned char bad[2] = {3, 0};
        PortMemoryIStream rejected(bad, sizeof bad);
        auto* other = MakeOccupant();
        CHECK(!other->Read(rejected));
        ReleaseOccupant(other);
        // Release the loaded occupant; it owns the crew automata.
        ReleaseOccupant(loaded);
    }

    // ---- the crane type dispatch (a property on the building) -----------------------------------------------
    {
        auto* dispatch = MakeOccupant();
        dispatch->Init();
        cS3DBoundingBox boxes[12];
        for (auto& box : boxes) {
            box.min[0] = box.min[1] = box.min[2] = 1.0f;
            box.max[0] = box.max[1] = box.max[2] = 0.0f;
        }
        // No property: no crane.
        CHECK(dispatch->AttemptAddCrane(boxes) == -1);
        ReleaseOccupant(dispatch);
    }

    // ---- teardown -------------------------------------------------------------------------------------------
    ReleaseOccupant(occupant);
    SetConstructionOccupantHost(nullptr);
    host.filter = nullptr;
    delete buildingOccupant;
    // The building and the lot were released through the occupant's references; the fakes are leaked here on
    // purpose (they are small and the test process is about to exit).

    if (g_failures != 0) {
        std::fprintf(stderr, "%d CHECK(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sc4_constructionoccupant: all checks passed\n");
    return 0;
}
