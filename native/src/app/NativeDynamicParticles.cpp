// The dynamic particle system's services (sc4/DynamicParticleSystem.h) on the native game: the city's terrain, its
// occupant manager, the 3D renderer's view-object list and the animation tick manager.
#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/CommonIIDs.h"
#include "sc4/DynamicParticleSystem.h"
#include "sc4/Occupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/Render3D.h"
#include "sc4/SC4Utilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

cSTETerrain* CityTerrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();   // the city keeps it
    return terrain;
}
cISC4OccupantManager* CityOccupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();   // the city keeps it
    return manager;
}

struct DynamicParticleHost final : IDynamicParticleHost {
    std::int32_t TerrainCellsX() override { cSTETerrain* t = CityTerrain(); return t ? static_cast<std::int32_t>(t->CellCountX()) : 0; }
    std::int32_t TerrainCellsZ() override { cSTETerrain* t = CityTerrain(); return t ? static_cast<std::int32_t>(t->CellCountZ()) : 0; }
    float TerrainCellSize() override { return cSTETerrain::CellWidth(); }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = CityTerrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    void TerrainNormal(float x, float z, cS3DVector3& out) override {   // spTerrainMap vslot 0x6c GetLerpedNormal
        cSTETerrain* t = CityTerrain();
        if (!t) {
            out = cS3DVector3{0.0f, 1.0f, 0.0f};
            return;
        }
        TerrainVector3 normal;
        t->GetLerpedNormal(x, z, normal);
        out = cS3DVector3{normal.x, normal.y, normal.z};
    }
    void OccupantsInBox(const float min[3], const float max[3], std::list<cISC4Occupant*>& out) override {
        if (cISC4OccupantManager* occupants = CityOccupants()) occupants->GetOccupantsByBBox(out, min, max, std::uint32_t{0}, 0xffffffffu);
    }
    bool IsStaticBuilding(cISC4Occupant* occupant) override {
        cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
        if (!holder) return false;
        void* raw = nullptr;
        const bool building = occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &raw) && raw;
        if (building) static_cast<cIGZUnknown*>(raw)->Release();
        return sc4::IsStaticBuilding(holder, building);
    }
    void ReleaseOccupant(cISC4Occupant* occupant) override { if (occupant) occupant->Release(); }
    void AddViewObject(cISC4ViewObject3D* object) override {
        if (cSC43DRender* renderer = CityRenderer()) renderer->AddViewObject(object, 3, 1000);
    }
    void RemoveViewObject(cISC4ViewObject3D* object) override {
        if (cSC43DRender* renderer = CityRenderer()) renderer->RemoveViewObject(object);
    }
    void AddTick(cIGZMessageTarget2* target) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, 0xc0);
    }
    void RemoveTick(cIGZMessageTarget2* target) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, 0xc0);
    }
};

}  // namespace

void InstallDynamicParticleHost() {
    static DynamicParticleHost host;
    SetDynamicParticleHost(&host);
}

}  // namespace native
