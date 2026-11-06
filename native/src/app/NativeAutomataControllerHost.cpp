// IAutomataControllerHost on the native application (was the generated neutral stub, whose 0 x 0 city left the controller
// grid without tracts: once the automata scripts ran and a saved controller found its template, the grid wrote outside its
// empty table). The answers come from the loaded city: its size, the terrain, the zone developer's lot map, the controller
// manager's queues and tract grid, the network cells and the city renderer's view utilities.
//   PARTIAL: effect triggers (the 3D view's active-effect query) answer "no effect"; controller loop sounds are not posted.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeZoneDeveloperHost.h"
#include "rz/cRZRandom.h"
#include "sc4/AutomataController.h"
#include "sc4/AutomataControllerGrid.h"
#include "sc4/AutomataControllerManager.h"
#include "sc4/AutomataScriptSystem.h"
#include "sc4/Lot.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
bool NativeViewAvailable();                          // app/NativeAutomataManagerHost.cpp (the city renderer's view utilities)
bool NativePointInViewFrustum(const float* xyz);
namespace {
using namespace sc4;

template <class T> T* CityComponentAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
template <class T> T* SerializableComponent(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
int Cells(bool z) {
    cSC4City* city = HostCity();
    return city ? (z ? city->CellCountZ() : city->CellCountX()) : 0;
}

struct NativeAutomataControllerHost final : IAutomataControllerHost {
    // SL::spCity vslot 0x180: the automata script system (the controller keeps a reference)
    cIGZUnknown* CityContext() override {
        return static_cast<IControllerScriptSystem*>(CityComponentAs<cSC4AutomataScriptSystem>(CityComponent::AutomataScriptSystem));
    }
    bool GetOccupantLot(cISC4Occupant* occupant, SC4CellRect& rect, std::uint32_t& facing) override {
        auto* lots = SerializableComponent<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId);
        cSC4Lot* lot = lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
        if (!lot) return false;
        std::int32_t bounds[4] = {};
        lot->GetBoundingRect(bounds);
        rect.x0 = bounds[0];
        rect.z0 = bounds[1];
        rect.x1 = bounds[2];
        rect.z1 = bounds[3];
        facing = lot->GetFacing();
        return true;
    }
    bool PointInCity(float x, float z) override { return x >= 0.0f && z >= 0.0f && x < CityWidth() && z < CityDepth(); }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float CityTerrainHeight(float x, float z) override { return TerrainHeight(x, z); }
    float GroundHeight(float x, float z) override { return TerrainHeight(x, z); }   // the network manager's height: the terrain's here
    float CityWidth() override { return float(Cells(false)) * 16.0f; }
    float CityDepth() override { return float(Cells(true)) * 16.0f; }
    IControllerQueue* GetQueue(cISC4Occupant* occupant, bool create) override {
        auto* manager = CityComponentAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
        return manager ? manager->GetControllerQInfo(occupant, create) : nullptr;
    }
    void RegisterActiveController(void* controller) override {
        if (auto* manager = CityComponentAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager))
            manager->MoveController(static_cast<cSC4AutomataController*>(controller));
    }
    bool BeginEffectQuery() override { return false; }
    bool IsEffectActive(std::uint32_t) override { return false; }
    void EndEffectQuery() override {}
    bool SoundAvailable() override { return false; }
    void PostSound(std::uint32_t, std::uint32_t, std::int64_t, std::int64_t, std::int64_t) override {}
    std::uint32_t RandomBelow(std::uint32_t count) override {
        cRZRandom random(0xffffffffu);
        return count ? random.RandomUint32Uniform(count) : 0;
    }
    bool ViewAvailable() override { return NativeViewAvailable(); }
    bool IsPointInViewFrustum(const float* xyz) override { return NativePointInViewFrustum(xyz); }
    int CityCellsX() override { return Cells(false); }
    int CityCellsZ() override { return Cells(true); }
    bool HasNetworkAt(int cellX, int cellZ) override {
        return cellX >= 0 && cellZ >= 0 && cellX < Cells(false) && cellZ < Cells(true) && NativeNetworkTypesAt(cellX, cellZ) != 0;
    }
};

}  // namespace

// The controller manager for hosts that cannot include its header next to their own (name clashes).
cIGZUnknown* NativeAddController(sc4::cISC4Occupant* occupant, const char* templateName) {
    auto* manager = CityComponentAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
    return manager ? manager->AddControllerFromTemplateName(occupant, cRZString(templateName), nullptr) : nullptr;
}
void NativeRemoveController(cIGZUnknown* controller) {
    auto* manager = CityComponentAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
    if (manager && controller) manager->RemoveController(static_cast<cSC4AutomataController*>(controller));
}

// IAutomatonHost::FindAttraction (cSC4Automaton::UpdateAttraction, i386 0x000c65d2: spCity vslot 0xbc, the controller manager,
// then its vslot 0x58 GetCompositeAttractionEffect). The strongest attractor in reach, not referenced; the sum of the pulls
// goes into `direction`.
sc4::cSC4AutomataController* NativeFindAttraction(sc4::cISC4Occupant* occupant, cS3DVector3* direction, float* distance, float* strength) {
    auto* manager = CityComponentAs<cSC4AutomataControllerManager>(CityComponent::AutomataControllerManager);
    if (!manager || !occupant || !direction || !distance || !strength) return nullptr;
    cSC4AutomataController* found = manager->GetCompositeAttractionEffect(occupant, *direction, *distance, *strength);
    static const bool trace = std::getenv("SC4_ATTRACTDEBUG") != nullptr;
    if (trace && found) {
        static int lines = 0;
        if (lines++ < 200) {
            float at[3] = {};
            occupant->GetPosition(at);
            std::fprintf(stderr, "attraction: occupant %p at (%.0f, %.0f) controller %p distance %.1f strength %.1f\n", static_cast<void*>(occupant),
                         at[0], at[2], static_cast<void*>(found), *distance, *strength);
        }
    }
    return found;
}

void InstallAutomataControllerHost() { static NativeAutomataControllerHost s; sc4::SetAutomataControllerHost(&s); }

}  // namespace native
