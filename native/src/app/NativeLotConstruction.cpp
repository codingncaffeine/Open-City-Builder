// The lot developer on the native application (notes/GROWTH_PLAN.md layer 4): its terrain host (IBaseLotDeveloperHost, which ran
// on the city probe's neutral binding, so every slope and altitude read 0) and the services StartConstruction reaches outside the
// class (ILotConstructionServices): the lot configuration references lots keep, the building development simulator's placement,
// the city's cell corners, the terrain, and the flora a new building clears.
//
// PARTIAL (each named here and where it is called): the terrain leveller (LevelVertices changes nothing, so a steep lot keeps its
// slope) and levelled-vertex flags. The construction occupant is NativeConstruction.cpp.
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <unordered_set>
#include <vector>

#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCity.h"
#include "app/NativeConstruction.h"
#include "app/NativeFlora.h"
#include "app/NativeLotConstruction.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/SimHostRegistry.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZRandom.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CommonIIDs.h"
#include "sc4/FloraSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/Lot.h"
#include "sc4/LotConfiguration.h"
#include "sc4/LotConfigurationManager.h"
#include "sc4/LotDeveloper.h"
#include "sc4/OccupantFilters.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
cSTETerrain* Terrain() {
    cIGZUnknown* object = Component(CityComponent::Terrain);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    serializable->Release();
    return static_cast<cSTETerrain*>(serializable);
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();
    return manager;
}
cSC4LotConfigurationManager* LotConfigurations() {
    cIGZUnknown* object = Component(CityComponent::LotConfigurationManager);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4LotConfigurationManager*>(raw) : nullptr;
}

cSC4PropManager* Props() {
    cIGZUnknown* object = Component(CityComponent::PropManager);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<cSC4PropManager*>(raw) : nullptr;
}

// The manager's COM reference to one lot configuration (cISC4LotConfiguration, the id is all a lot asks of it). One per id, kept
// for the process like the manager's own table.
class ConfigurationRef final : public cISC4LotConfigurationRef {
public:
    explicit ConfigurationRef(std::uint32_t id) : id_(id) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ ? --refs_ : 0; }
    std::uint32_t GetID() const override { return id_; }
private:
    std::uint32_t id_;
    std::uint32_t refs_ = 1;
};
std::map<std::uint32_t, std::unique_ptr<ConfigurationRef>> g_references;

// cSC4LotDeveloper's terrain (+0x70), leveller (+0x38) and city (+0x30) as IBaseLotDeveloperHost.
class LotDeveloperHost final : public IBaseLotDeveloperHost {
public:
    float VertexHeight(int index) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(index) : 0.0f; }   // vslot 0x68
    float VertexWaterHeight(int index) override {                                     // vslot 0xf0: the water surface over the vertex
        cSTETerrain* t = Terrain();
        if (!t) return -FLT_MAX;
        const float depth = t->GetWaterDepthAtVertex(index);
        return depth > 0.0f ? t->GetAltitude(index) + depth : -FLT_MAX;
    }
    float HeightAt(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }   // vslot 0x60
    float WaterHeightAt(float x, float z) override {                                  // vslot 0xf8
        cSTETerrain* t = Terrain();
        if (!t) return -FLT_MAX;
        const int vx = static_cast<int>(x / 16.0f + 0.5f), vz = static_cast<int>(z / 16.0f + 0.5f);
        const float depth = t->GetWaterDepthAtVertex(vx, vz);
        return depth > 0.0f ? t->GetAltitudeAtVertex(vx, vz) + depth : -FLT_MAX;
    }
    bool VertexIsLevelled(int, int) override { return false; }                        // PARTIAL: vslot 0x128 (the levelled flag)
    int VerticesPerRow() override { cSTETerrain* t = Terrain(); return t ? static_cast<int>(t->CellCountX()) + 1 : 0; }
    float CityWidthMetres() override { cSTETerrain* t = Terrain(); return t ? static_cast<float>(t->CellCountX()) * 16.0f : 0.0f; }
    float CityDepthMetres() override { cSTETerrain* t = Terrain(); return t ? static_cast<float>(t->CellCountZ()) * 16.0f : 0.0f; }
    // PARTIAL: the terrain leveller (vslot 0x48) is not ported; no vertex changes, so the slope tests see the land as it is.
    int LevelVertices(float, bool, bool, int, int, int, int, void*, int, int) override { return 0; }
    void CellOfPosition(float x, float z, int& cellX, int& cellZ) override {         // city vslot 0x2f8
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
        else { cellX = static_cast<int>(x / 16.0f); cellZ = static_cast<int>(z / 16.0f); }
    }
    void OccupantBounds(void* occupant, float lo[3], float hi[3]) override {           // occupant vslot 0x50
        if (occupant) static_cast<cISC4Occupant*>(occupant)->GetBoundingBox(lo, hi);
    }
} g_host;

class ConstructionServices final : public ILotConstructionServices {
public:
    cISC4LotConfigurationRef* ConfigurationRef(cSC4LotConfiguration* configuration) override {
        return configuration ? NativeLotConfigurationRef(configuration->GetID()) : nullptr;
    }
    cSC4LotConfiguration* Configuration(cISC4LotConfigurationRef* reference) override {
        cSC4LotConfigurationManager* manager = LotConfigurations();
        return reference && manager ? manager->GetLotConfiguration(reference->GetID()) : nullptr;
    }
    std::int32_t BuildingPurpose(std::uint32_t type) override {
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? static_cast<std::int32_t>(profile->purpose) : 0;
    }
    std::uint32_t BuildingWealth(std::uint32_t type) override {
        const BuildingProfile* profile = NativeBuildingProfile(type);
        return profile ? profile->wealth : 0;
    }
    cSC4Lot* LotAt(int x, int z) override { return NativeLotAtCell(x, z, true); }
    std::uint32_t RandomBelow(std::uint32_t range) override { static cRZRandom random(0x10fu); return range ? random.RandomUint32Uniform(range) : 0; }
    bool LotTextureExists(std::uint32_t instance) override {
        if (!SL::spRM) return false;
        cGZPersistResourceKey key{0x7ab50e44, 0x0986135e, instance};
        return SL::spRM->TestForKey(key);
    }
    void ObjectPropTypes(const cSC4LotConfigurationObject& object, std::vector<std::uint32_t>& out) override {
        if (cSC4LotConfigurationManager* manager = LotConfigurations()) manager->GetObjectIDArray(&object, out);
    }
    cISC4Occupant* AddLotProp(std::uint32_t type, const float position[3], std::int32_t rotation) override {
        cSC4PropManager* props = Props();
        cSC4PropOccupant* prop = props ? props->AddLotProp(type, position, rotation) : nullptr;
        if (!prop) return nullptr;
        cISC4Occupant* occupant = prop;
        occupant->Release();   // the occupant manager keeps it (the original returns the manager's borrowed pointer)
        return occupant;
    }
    bool FloraTypeValid(std::uint32_t type) override { cSC4FloraSimulator* f = CityFloraSimulator(); return f && f->IsFloraTypeValid(type); }
    void FloraTypesInFamily(std::vector<std::uint32_t>& out, std::uint32_t family) override {
        cSC4FloraSimulator* f = CityFloraSimulator();
        if (!f) return;
        std::unordered_set<std::uint32_t> types;
        f->GetFloraTypesByFamily(types, family);
        out.assign(types.begin(), types.end());
    }
    cISC4Occupant* AddFlora(std::uint32_t type, float position[3]) override {
        cSC4FloraSimulator* f = CityFloraSimulator();
        return f ? f->AddNewFloraOccupant(type, position, false) : nullptr;
    }
    cISC4BuildingOccupant* AddNewBuildingOccupant(std::uint32_t type, float position[3], int rotation) override {
        return NativeAddNewBuildingOccupant(type, position, rotation);
    }
    void CellCornerToPosition(int x, int z, float& px, float& pz) override {
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(x, z, px, pz);
        else { px = static_cast<float>(x) * 16.0f; pz = static_cast<float>(z) * 16.0f; }
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    // The blocking-lot filter takes flora only (cSC4BlockingLotOccupantFilter); cSC4Demolition's flora case removes the occupant from
    // the manager at no cost (the network tool's interim demolition does the same).
    void DemolishBlockingOccupants(const float minimum[3], const float maximum[3]) override {
        cISC4OccupantManager* occupants = Occupants();
        if (!occupants) return;
        static cSC4BlockingLotOccupantFilter* blocking = [] { auto* f = new cSC4BlockingLotOccupantFilter(); f->AddRef(); return f; }();   // +0x58
        std::vector<cISC4Occupant*> found;
        occupants->IterateOccupantsByBBox(
            [](cISC4Occupant* occupant, void* out) {
                occupant->AddRef();
                static_cast<std::vector<cISC4Occupant*>*>(out)->push_back(occupant);
                return true;
            },
            &found, minimum, maximum, static_cast<cISC4OccupantFilter*>(blocking));
        for (cISC4Occupant* occupant : found) {
            occupants->RemoveOccupant(occupant, true, 0);
            occupant->Release();
        }
    }
    void StartConstructionOccupant(cSC4Lot* lot) override { StartNativeConstructionOccupant(lot); }
} g_services;

}  // namespace

cISC4LotConfigurationRef* NativeLotConfigurationRef(std::uint32_t id) {
    if (id == 0) return nullptr;
    auto it = g_references.find(id);
    if (it == g_references.end()) it = g_references.emplace(id, std::make_unique<ConfigurationRef>(id)).first;
    return it->second.get();
}

void ReadLotDeveloperTunables(cSC4LotDeveloper* developer) {   // cSC4LotDeveloper::Init reads the tunable exemplar
    static cSC4LotDeveloper* tuned = nullptr;
    if (!developer || developer == tuned || !SL::spRM) return;
    tuned = developer;
    const cGZPersistResourceKey key{kLotDeveloperTunablesKeyType, kLotDeveloperTunablesKeyGroup, kLotDeveloperTunablesKeyInstance};
    void* raw = nullptr;
    if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return;
    auto* exemplar = static_cast<cISCResExemplar*>(raw);
    developer->ReadSimulatorTunableValues(exemplar->AsISCPropertyHolder());
    exemplar->Release();
    if (std::getenv("SC4_SIMDEBUG")) {
        const LotDeveloperTunables& t = developer->Tunables();
        std::fprintf(stderr, "lot developer: tunables slope %.3f level-for-slope %d level-interior %d levelling %.3f\n", t.value12c, t.flag138,
                     t.flag139, t.maxLevelling);
    }
}

void InstallLotConstructionHosts() {
    gzcom::GetSimHosts().lotDeveloper = &g_host;
    cSC4LotDeveloper::SetConstructionServices(&g_services);
    InstallConstructionOccupantHost();
}

}  // namespace native
