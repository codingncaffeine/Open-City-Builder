// The animation coordinator's city services (sc4::IAnimationCoordinatorHost) and the model maker's animated models
// (GetAnimatedModelFromResourceKey): sprite animations come from NativeSprites.cpp.
//   PARTIAL: mesh animations (key type 0x5ad0e817, GetMeshAnimFromResourceKey) are not recovered; the model maker then
//   falls back to the static S3D model of the standard group, as the original does when no animation loads.
#include "app/NativeAnimationCoordinator.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeBoot.h"
#include "app/NativeCity.h"
#include "s3d/ModelInstanceAnimatedSprite.h"
#include "sc4/AnimationCoordinator.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/App.h"
#include "sc4/Budget.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/Lot.h"
#include "sc4/ModelMakerUtility.h"
#include "sc4/Occupant.h"
#include "sc4/PropOccupant.h"
#include "sc4/ServiceLocator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"

namespace native {

s3d::cS3DModelInstanceAnimatedSprite* NativeCreateSpriteInstance(const cGZPersistResourceKey& key);   // NativeSprites.cpp
void NativeVisibleTerrainRect(float out[4]);                                                            // NativeAutomataManagerHost.cpp
int NativeViewZoom();                                                                                    // NativeAutomataManagerHost.cpp
bool NativeProjectToViewport(const float* xyz, float out[3]);                                            // NativeAutomataManagerHost.cpp

namespace {
using namespace sc4;

template <class T> T* Component(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4ZoneDeveloper* Lots() { return Component<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }

bool Trace() { static const bool on = std::getenv("SC4_ANIMDEBUG") != nullptr; return on; }

cSC4AnimationCoordinator* g_live = nullptr;               // the city's coordinator (the kit's report)
std::vector<cIGZMessageTarget2*> g_ticking;               // what the coordinator registered with the tick manager

class CoordinatorHost final : public IAnimationCoordinatorHost {
public:
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    bool AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        cSC4AnimationTickManager* atm = AnimationTickManager();
        const bool ok = atm && atm->AddTickableObject(target, priority);
        if (ok) g_ticking.push_back(target);
        if (Trace()) std::fprintf(stderr, "anim: tick add %p -> %d\n", static_cast<void*>(target), ok ? 1 : 0);
        return ok;
    }
    bool RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        cSC4AnimationTickManager* atm = AnimationTickManager();
        const bool ok = atm && atm->RemoveTickableObject(target, priority);
        for (auto it = g_ticking.begin(); it != g_ticking.end(); ++it) if (*it == target) { g_ticking.erase(it); break; }
        if (Trace()) std::fprintf(stderr, "anim: tick remove %p -> %d\n", static_cast<void*>(target), ok ? 1 : 0);
        return ok;
    }
    // cSC4PowerSimulator::IsOccupantPowered (i386 0x00180878): the occupant's lot has power, or does not need it; no lot = no.
    bool IsOccupantPowered(cISC4Occupant* occupant) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
        return lot && (lot->GetHasPower() || !lot->GetNeedsPower());
    }
    bool OccupantFunding(cISC4Occupant* occupant, std::uint32_t purpose, float& funding) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
        cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
        void* raw = nullptr;
        if (!building || !building->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return false;
        auto* buildingOccupant = static_cast<cISC4Occupant*>(raw);
        cISCPropertyHolder* holder = buildingOccupant->AsPropertyHolder();
        auto* budget = Component<cSC4BudgetSimulator>(CityComponent::BudgetSimulator, kGZCLSID_cSC4BudgetSimulator);
        BudgetItem item;
        const bool ok = budget && holder && budget->GetBudgetItemForPurpose(holder, purpose, item);
        buildingOccupant->Release();
        if (ok) funding = item.localFunding;
        return ok;
    }
    bool OccupantLotCells(cISC4Occupant* occupant, int out[4]) override {
        cSC4ZoneDeveloper* lots = Lots();
        cSC4Lot* lot = lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
        if (!lot) return false;
        std::int32_t bounds[4] = {};
        lot->GetBoundingRect(bounds);
        for (int i = 0; i < 4; ++i) out[i] = bounds[i];
        return true;
    }
    int ViewZoom() override { return NativeViewZoom(); }
    void VisibleTerrainRect(float out[4]) override { NativeVisibleTerrainRect(out); }
    int CityCellsX() override { cSC4City* city = HostCity(); return city ? static_cast<int>(city->CellCountX()) : 0; }
    int CityDetail() override {
        cSC4App* app = BootApplication();
        const std::uint8_t* preferences = app ? app->GetPreferences() : nullptr;
        return preferences ? preferences[0xf16] : 0;
    }
    int OccupantModelClass(cISC4Occupant* occupant) override {
        static cSC4ModelMakerUtility utility;
        return utility.GetOccupantModelClass(occupant);
    }
} g_host;

}  // namespace

sc4::IModelAnimationCoordinator* NativeCreateAnimationCoordinator() {
    auto* coordinator = new cSC4AnimationCoordinator(&g_host);
    coordinator->AddRef();
    g_live = coordinator;
    g_ticking.clear();
    return coordinator;
}

// The kit's `animated [N]`: the coordinator's held count, the visible set rectangle, how many occupants tick, and for the first
// N ticking props: position, screen point and every animated model's frame / frame count.
void NativeAnimationReport(int limit) {
    if (!g_live) { std::printf("animated: no coordinator\n"); return; }
    int rect[4];
    g_live->VisibleSetRect(rect);
    std::printf("animated: held %d, sets %d, detail %d, zoom %d, visible sets %d,%d..%d,%d, ticking %zu\n", g_live->HeldCount(),
                g_live->SetsPerSide(), g_live->DetailAllows() ? 1 : 0, NativeViewZoom(), rect[0], rect[1], rect[2], rect[3], g_ticking.size());
    int shown = 0;
    for (cIGZMessageTarget2* target : g_ticking) {
        if (shown >= limit) break;
        auto* prop = dynamic_cast<cSC4PropOccupant*>(target);
        if (!prop) continue;
        float pos[3] = {};
        prop->GetPosition(pos);
        float screen[3] = {};
        const bool onScreen = NativeProjectToViewport(pos, screen);
        std::printf("  prop %08x at (%.0f, %.0f, %.0f) screen %s(%.0f, %.0f) frames:", prop->GetPropType(), pos[0], pos[1], pos[2],
                    onScreen ? "" : "off ", screen[0], screen[1]);
        if (cISC43DPlaceableObject* placeable = prop->GetPlaceableObject()) {
            std::uint32_t count = static_cast<std::uint32_t>(placeable->GetModelCount());
            std::vector<cIS3DModelInstance*> models(count, nullptr);
            if (count) placeable->GetModels(models.data(), count);
            for (std::uint32_t i = 0; i < count; ++i) {
                void* raw = nullptr;
                if (models[i] && models[i]->QueryInterface(s3d::kGZIID_cIS3DModelInstanceAnimated, &raw) && raw) {
                    auto* animated = static_cast<s3d::cIS3DModelInstanceAnimated*>(raw);
                    std::printf(" %d/%d", animated->GetFrame(), animated->GetFrameCount());
                    animated->Release();
                } else {
                    std::printf(" static");
                }
            }
        }
        std::printf("\n");
        ++shown;
    }
}

s3d::cIS3DModelInstanceAnimated* NativeCreateAnimatedModel(const cGZPersistResourceKey& key) {
    if (key.type == s3d::kGZIID_cIS3DResSpriteAnim) {
        s3d::cS3DModelInstanceAnimatedSprite* sprite = NativeCreateSpriteInstance(key);
        if (Trace()) std::fprintf(stderr, "anim: sprite %08x/%08x/%08x -> %p\n", key.type, key.group, key.instance, static_cast<void*>(sprite));
        return sprite;   // one reference (null when a resource is missing)
    }
    return nullptr;   // PARTIAL: GetMeshAnimFromResourceKey (key type 0x5ad0e817)
}

}  // namespace native
