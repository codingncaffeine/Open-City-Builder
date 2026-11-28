// The prop manager and the props on the native application. Replaces the generated neutral IPropManagerHost / IPropHost, under
// which the prop catalogue was empty (no prop type was valid) and a new prop had no occupant manager to go into: the prop
// exemplars come from the resource manager, props go into the city's occupant manager and read the city's clock and date.
//
// PARTIAL (named): the registered prop classes and terrain-decal props (a plain cSC4PropOccupant stands in, as the manager does
// when a class is not registered), the prop-data-updated message, the lot at a cell (street props) and text decals.
#include <map>
#include "platform/Unwired.h"

#include "app/NativeCity.h"
#include "app/NativeProps.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/GZCOMDirector.h"
#include "gz/cGZPersistResourceManager.h"
#include "gz/cIGZPersistResourceKeyList.h"
#include "rz/cRZRandom.h"
#include "sc4/CommonIIDs.h"
#include "sc4/Exemplar.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/PropOccupantVisualEffect.h"
#include "sc4/PropOccupantTerrainDecal.h"
#include "sc4/PropOccupantBurntBuilding.h"
#include "sc4/DrawContext.h"
#include "sc4/Render3D.h"
#include "sc4/ViewUtilities.h"
#include "app/NativeCityView.h"
#include "app/NativeModelMaker.h"
#include "sc4/AnimationTickManager.h"
#include "app/NativeAnimationTicks.h"
#include "app/NativeDecalOverlay.h"
#include "sc4/ServiceLocator.h"
#include "app/NativeCitySystems.h"
#include "app/NativeKeyboard.h"
#include "gz/CommandServer.h"
#include "gz/cIGZMessage2.h"
#include "sc4/TextDecalOccupant.h"
#include "sc4/View3DWin.h"
#include "sc4/cSTETerrain.h"
#include "sc4/Simulator.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

class TypeFilter final : public cIGZPersistResourceKeyFilter {
public:
    explicit TypeFilter(std::uint32_t type) : type_(type) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool IsKeyIncluded(const cGZPersistResourceKey& key) override { return key.type == type_; }
private:
    std::uint32_t type_;
};

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
template <class T> T* Plain(CityComponent c) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}

// The manager's cIGZMessageTarget2 view (+4 in the original): spMS2 delivers its four messages here.
class PropManagerTarget final : public cIGZMessageTarget2 {
public:
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZMessageTarget2 && iid != 1) return false;
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { return refs_ > 1 ? --refs_ : 1; }   // static: lives as long as the process
    bool DoMessage(cIGZMessage2* message) override { return manager ? manager->DoMessage(message) : true; }
    cSC4PropManager* manager = nullptr;
private:
    std::uint32_t refs_ = 1;
} g_propManagerTarget;

class PropManagerHost final : public IPropManagerHost {
public:
    void ForEachExemplarKey(const std::function<void(const cGZPersistResourceKey&)>& fn) override {
        if (!SL::spRM) return;
        cIGZPersistResourceKeyList* list = nullptr;
        TypeFilter filter(0x6534284a);
        SL::spRM->GetAvailableResourceList(&list, &filter);
        if (!list) return;
        std::vector<cGZPersistResourceKey> keys;
        for (std::uint32_t i = 0; i < list->Size(); ++i) keys.push_back(*list->GetKey(i));
        list->Release();
        for (const cGZPersistResourceKey& key : keys) fn(key);
    }
    cISCPropertyHolder* AcquireExemplar(const cGZPersistResourceKey& key) override {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        auto* exemplar = static_cast<cISCResExemplar*>(raw);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        held_[holder] = exemplar;
        return holder;
    }
    void ReleaseExemplar(cISCPropertyHolder* holder) override {
        auto it = held_.find(holder);
        if (it == held_.end()) return;
        it->second->Release();
        held_.erase(it);
    }
    void SetDefaultExemplar(cSC4PropOccupant* prop, cISCPropertyHolder* exemplar) override {
        auto it = held_.find(exemplar);
        if (it != held_.end() && prop) prop->SetDefaultExemplar(it->second);
    }
    // GZCOM()->CreateInstance(class, cISC4PropOccupant 0xe9793a65, &out) (i386 0x0012cf92): any prop class a creator is
    // registered for, one reference; null leaves the manager's own fallbacks.
    cSC4PropOccupant* CreateRegisteredProp(std::uint32_t classId) override {
        cIGZCOM* com = GZCOM();
        void* raw = nullptr;
        if (!com || !com->CreateInstance(classId, kGZIID_cISC4PropOccupant, &raw) || !raw) return nullptr;
        return static_cast<cSC4PropOccupant*>(static_cast<cISC4PropOccupant*>(raw));
    }
    cSC4PropOccupant* CreateTerrainDecalProp() override {                                  // prop class 0x2977aa48 (one reference)
        auto* prop = new cSC4PropOccupantTerrainDecal();
        prop->AddRef();
        return prop;
    }
    cSC4PropOccupant* CreateVisualEffectProp() override {                                  // prop class 0x2977aa49 (one reference)
        auto* prop = new cSC4PropOccupantVisualEffect();
        prop->AddRef();
        return prop->AsPropOccupant();
    }
    // Message 0x896fdc70 (i386 PostPropExemplarDataUpdatedMessage 0x0012d4fc): nothing in the binary listens for it (the constant
    // appears only in the post), so it is not posted natively.
    void PostPropExemplarDataUpdated() override {}
    void PositionToCityCell(float x, float z, int& cx, int& cz) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cx, cz);
        else { cx = static_cast<int>(x / 16.0f); cz = static_cast<int>(z / 16.0f); }
    }
    void CityCellToPosition(int cx, int cz, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCornerToPosition(cx, cz, x, z);
        else { x = static_cast<float>(cx) * 16.0f; z = static_cast<float>(cz) * 16.0f; }
    }
    cSC4Lot* LotAtCell(int x, int z) override { return NativeLotAtCell(x, z, false); }   // lot manager vslot 0x28
    void SubscribeMessages(cSC4PropManager* manager, bool on) override {
        g_propManagerTarget.manager = on ? manager : nullptr;
        if (!SL::spMS2) return;
        for (std::uint32_t type : kPropManagerMessages) {
            if (on) SL::spMS2->AddNotification(&g_propManagerTarget, type);
            else SL::spMS2->RemoveNotification(&g_propManagerTarget, type);
        }
    }
    void RegisterTextDecalCommand(bool on) override {
        cIGZCommandServer* server = GameCommandServer();
        if (!server) return;
        if (on) server->RegisterCommand(kMessagePropTextDecal, "TextDecal", nullptr);
        else server->UnregisterCommand(kMessagePropTextDecal);
    }
    cIGZUnknown* CurrentViewFilter() override {
        cSC4View3DWin* view = CityView3DWin();
        if (!view) return nullptr;
        cSC4ViewFilterInformation info;
        view->GetCurrentViewFilterInfo(info);   // one reference for the caller
        return info.filter;
    }
    float TerrainAltitude(float x, float z) override {
        cIGZUnknown* object = Component(CityComponent::Terrain);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return 0.0f;
        auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
        const float altitude = terrain->GetAltitude(x, z);
        static_cast<cIGZSerializable*>(raw)->Release();
        return altitude;
    }
    cISC4TextDecalOccupant* NewTextDecal() override { return new cSC4TextDecalOccupant(); }
private:
    std::map<cISCPropertyHolder*, cISCResExemplar*> held_;
} g_managerHost;

class PropHost final : public IPropHost {
public:
    int ClockHour() override { cSC424HourClock* c = Plain<cSC424HourClock>(CityComponent::Clock24Hour); return c ? static_cast<int>(c->GetHour()) : 12; }
    int ClockMinute() override { cSC424HourClock* c = Plain<cSC424HourClock>(CityComponent::Clock24Hour); return c ? static_cast<int>(c->GetMinute()) : 0; }
    void SimulatorDate(cGZDate& out) override {
        cIGZUnknown* object = Component(CityComponent::Simulator);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        if (serializable->GetGZCLSID() == kGZCLSID_cSC4Simulator) out = static_cast<cSC4Simulator*>(serializable)->GetSimDate();
        serializable->Release();
    }
    float AnimationTimeDilation() override {
        cIGZUnknown* object = Component(CityComponent::Simulator);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return 1.0f;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        float dilation = 1.0f;
        if (serializable->GetGZCLSID() == kGZCLSID_cSC4Simulator) dilation = static_cast<cSC4Simulator*>(serializable)->GetAnimationTimeDilation();
        serializable->Release();
        return dilation;
    }
    std::uint32_t RandomPercent() override { static cRZRandom random(0x9a0u); return random.RandomUint32Uniform(100); }
    cISC4OccupantManager* OccupantManager() override {
        cIGZUnknown* object = Component(CityComponent::OccupantManager);
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
        auto* manager = static_cast<cISC4OccupantManager*>(raw);
        manager->Release();   // the city keeps the manager
        return manager;
    }
} g_propHost;

// The terrain decal props' world: the terrain view's overlay managers (app/NativeDecalOverlay) and the animation tick manager.
struct TerrainDecalHost final : sc4::ITerrainDecalHost {
    sc4::cSTEOverlayManager* OverlayManager(int index) override { return DecalOverlayManager(index); }
    bool AddTick(cIGZMessageTarget2* target) override {
        sc4::cSC4AnimationTickManager* atm = AnimationTickManager();
        return atm && atm->AddTickableObject(target, 0xc0);
    }
    void RemoveTick(cIGZMessageTarget2* target) override {
        if (sc4::cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, 0xc0);
    }
} g_terrainDecalHost;

// The burnt buildings' world: the floor texture's id (the FSH of the key), the view projection and the city renderer.
struct BurntBuildingHost final : sc4::IBurntBuildingHost {
    std::int32_t TextureId(std::uint32_t, std::uint32_t group, std::uint32_t instance) override {
        return static_cast<std::int32_t>(FshTextureId(group, instance));
    }
    void ProjectToViewCoordinates(float point[3]) override {
        sc4::cSC43DRender* render = CityRenderer();
        if (const SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) view->ProjectPointToViewCoordinates(point);
    }
    bool IsModelInserted(s3d::cIS3DModelInstance* model) override { sc4::cSC43DRender* r = CityRenderer(); return r && r->IsModelInserted(model); }
    bool InsertModel(s3d::cIS3DModelInstance* model) override { sc4::cSC43DRender* r = CityRenderer(); return r && r->InsertModel(model); }
    bool RemoveModel(s3d::cIS3DModelInstance* model) override { sc4::cSC43DRender* r = CityRenderer(); return r && r->RemoveModel(model); }
} g_burntBuildingHost;

}  // namespace

void InstallPropHosts() {
    sc4::SetPropManagerHost(&g_managerHost);
    sc4::SetPropHost(&g_propHost);
    sc4::SetTerrainDecalHost(&g_terrainDecalHost);
    sc4::SetBurntBuildingHost(&g_burntBuildingHost);
}

// The volcano's scorch marks (cSTEVolcanoLava::AddDecal @ 0x004f9986): a terrain decal prop, Init, the texture at the size,
// SetOpacityFade(fadeIn, keep, 0, 1), into the occupant manager, SetPosition; the caller keeps the one reference.
void* NativeCreateFadingDecal(const cGZPersistResourceKey& key, float size, float fadeIn, const float position[3]) {
    auto* decal = new sc4::cSC4PropOccupantTerrainDecal();
    decal->AddRef();
    sc4::cISC4Occupant* occupant = decal;
    occupant->Init();
    decal->SetDecalTexture(key, size);
    decal->SetOpacityFade(fadeIn, sc4::cISC4PropOccupantTerrainDecal::kKeepDecal, 0.0f, 1.0f);
    decal->PlaceInOccupantManager(true);
    occupant->SetPosition(position);
    return decal;
}
// TickDecals @ 0x004f9b7a: SetOpacityFade(fadeOut, action, 1, 0), then Release.
void NativeRetireFadingDecal(void* handle, float fadeOut, int completionAction) {
    auto* decal = static_cast<sc4::cSC4PropOccupantTerrainDecal*>(handle);
    if (!decal) return;
    decal->SetOpacityFade(fadeOut, static_cast<sc4::cISC4PropOccupantTerrainDecal::tCompletionAction>(completionAction), 1.0f, 0.0f);
    decal->Release();
}

}  // namespace native
