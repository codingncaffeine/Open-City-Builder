// The sign tool's host (sc4/ViewInputControlSignTool.h) on the open city: the renderer's picks, the terrain, the prop
// manager and developer, the view projection and the framework's message server. And the signposts' host
// (sc4/SignpostOccupant.h): the renderer, the glyph texture manager, the frame textures and the view. Both replace the
// generated stubs.
#include "app/NativeSignLabel.h"

#include "sc4/Render3D.h"   // first: its filter forward declaration must not meet sc4/OccupantManager.h's first
#include <cstdio>
#include <cstdlib>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeGlyphTextures.h"
#include "app/NativeCityView.h"
#include "gz/ClipboardManager.h"
#include "sc4/CommonIIDs.h"
#include "sc4/GlyphTextureManager.h"
#include "sc4/OccupantManager.h"
#include "s3d/ModelInstance.h"
#include "sc4/PropDeveloper.h"
#include "sc4/PropManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SignpostOccupant.h"
#include "sc4/ViewInputControlSignTool.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
std::uint32_t NativeFshTexture(std::uint32_t group, std::uint32_t instance);   // app/NativeModelMaker.cpp
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_SIGNDEBUG") != nullptr;
    return on;
}

template <class T> T* Component(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    if (!object) return nullptr;
    if (T* direct = dynamic_cast<T*>(object)) return direct;
    void* raw = nullptr;
    if (!object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}

// The prop manager and developer are plain objects behind adapters (their QueryInterface answers the object itself).
template <class T> T* Plain(CityComponent c) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}

// What the renderer's occupant pick hands the tool: the picked model's owner (one counted reference on the wrapper).
class Picked final : public IPickedObject {
public:
    explicit Picked(cIGZUnknown* owner) : owner_(owner) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        delete this;
        return 0;
    }
    cIGZUnknown* GetOwner() override { return owner_; }

private:
    cIGZUnknown* owner_;
    std::uint32_t refs_ = 1;
};

class Renderer final : public ISignToolRenderer {
public:
    bool PickObject(int x, int y, sc4::cISC4OccupantFilter* filter, IPickedObject** out) override {
        *out = nullptr;
        cSC43DRender* render = CityRenderer();
        s3d::cIS3DModelInstance* model = nullptr;
        // The renderer's pick takes no filter natively (PARTIAL): the nearest model is tested against the tool's filter.
        if (!render || !render->Pick(x, y, nullptr, model) || !model) return false;
        cIGZUnknown* owner = model->GetOwner();
        if (filter && owner) {
            void* raw = nullptr;
            if (!owner->QueryInterface(kGZIID_cISC4Occupant, &raw) || !raw) return false;
            auto* occupant = static_cast<cISC4Occupant*>(raw);
            const bool included = filter->IsOccupantIncluded(occupant);
            occupant->Release();
            if (!included) return false;
        }
        *out = new Picked(owner);
        return true;
    }
    bool PickTerrain(int x, int y, float out[3], bool flagA, bool flagB) override {
        cSC43DRender* render = CityRenderer();
        const bool hit = render && render->PickTerrain(x, y, out, flagA, flagB);
        if (Trace()) std::fprintf(stderr, "sign: terrain pick %d,%d -> %d (%.1f, %.1f, %.1f)\n", x, y, hit, out[0], out[1], out[2]);
        return hit;
    }
};

struct SignHost final : ISignToolHost {
    ISignToolRenderer* Renderer() override {
        static class Renderer renderer;
        return CityRenderer() ? &renderer : nullptr;
    }
    float TerrainAltitude(float x, float z) override {
        cSTETerrain* terrain = Component<cSTETerrain>(CityComponent::Terrain);
        return terrain ? terrain->GetAltitude(x, z) : 0.0f;
    }
    float SeaLevel() override {
        cSTETerrain* terrain = Component<cSTETerrain>(CityComponent::Terrain);
        return terrain ? terrain->GetSeaLevel() : 0.0f;
    }
    void AddProp(cISC4Occupant* occupant) override {
        auto* props = Plain<cSC4PropManager>(CityComponent::PropManager);
        const bool added = props && props->AddCityProp(occupant);
        if (Trace()) std::fprintf(stderr, "sign: prop added %d (manager %p)\n", added, static_cast<void*>(props));
    }
    void RemoveProp(cISC4Occupant* occupant) override {
        if (auto* props = Plain<cSC4PropManager>(CityComponent::PropManager)) props->RemovePropA(occupant);
    }
    void SetPropDeveloperMode(int mode) override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::PropDeveloper) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return;   // its serializable view, by class id
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        if (serializable->GetGZCLSID() == kGZCLSID_cSC4PropDeveloper) static_cast<cSC4PropDeveloper*>(serializable)->ShowSignsAndLabels(mode != 0);
        serializable->Release();
    }
    cISC4SignpostOccupant* CreateSignpost() override { return new cSC4SignpostOccupant(); }
    void ProjectToViewport(float point[3]) override {
        cSC43DRender* render = CityRenderer();
        if (SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) view->ProjectPointToViewportCoordinates2D(point);
    }
    void PlaySound(std::uint32_t, std::uint32_t) override {}   // the sound system is not wired natively
    cIGZMessageServer* MessageServer() override { return SL::spMS; }
    cIGZClipboardManager* ClipboardManager() override { return nullptr; }   // the clipboard service is not started natively
};

// cS3DTextureBinding of a frame / post texture: the driver texture stays in the process cache.
class FrameTexture final : public ISignpostTextureBinding {
public:
    explicit FrameTexture(std::uint32_t id) : id_(id) {}
    std::uint32_t TextureId() const override { return id_; }
    void Release() override { delete this; }
private:
    std::uint32_t id_;
};

cSC4PropDeveloper* PropDeveloper() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::PropDeveloper) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    cSC4PropDeveloper* developer = serializable->GetGZCLSID() == kGZCLSID_cSC4PropDeveloper ? static_cast<cSC4PropDeveloper*>(serializable) : nullptr;
    serializable->Release();   // the city keeps it
    return developer;
}

struct SignpostHost final : ISignpostHost {
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void AddTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cIGZMessageServer2* ticks = AnimationTickServerView()) ticks->AddNotification(target, static_cast<std::uint32_t>(channel));
    }
    void RemoveTickNotification(cIGZMessageTarget2* target, int channel) override {
        if (cIGZMessageServer2* ticks = AnimationTickServerView()) ticks->RemoveNotification(target, static_cast<std::uint32_t>(channel));
    }
    bool PropDeveloperAllowsSigns() override {   // spPropDeveloper vslot 0x50 AreSignsAndLabelsVisible
        cSC4PropDeveloper* developer = PropDeveloper();
        return developer && developer->AreSignsAndLabelsVisible();
    }
    cISC4OccupantManager* OccupantManager() override {
        cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
        auto* manager = static_cast<cISC4OccupantManager*>(raw);
        manager->Release();
        return manager;
    }
    bool RendererHasModel(s3d::cIS3DModelInstance* model) override { cSC43DRender* r = CityRenderer(); return r && r->IsModelInserted(model); }
    void RendererInsertModel(s3d::cIS3DModelInstance* model) override {
        cSC43DRender* r = CityRenderer();
        const bool inserted = r && r->InsertModel(model);
        if (Trace()) std::fprintf(stderr, "sign: signpost %p inserted %d\n", static_cast<void*>(model), inserted);
    }
    void RendererRemoveModel(s3d::cIS3DModelInstance* model) override { if (cSC43DRender* r = CityRenderer()) r->RemoveModel(model); }
    void RendererMoveModel(s3d::cIS3DModelInstance* model) override { if (cSC43DRender* r = CityRenderer()) r->MoveModel(model); }
    void RendererChangeModel(s3d::cIS3DModelInstance* model, int reason) override { if (cSC43DRender* r = CityRenderer()) r->ChangeModel(model, reason); }
    void StringToGlyphTextureArray(std::uint32_t fontId, cIGZString& text, std::vector<cRZAutoRefCount<ISignpostGlyphTextureInfo>>& out) override {
        out.clear();
        if (cSC4GlyphTextureManager* manager = GlyphTextureManager()) manager->StringToGlyphTextureArray(fontId, text, out);
        if (Trace()) std::fprintf(stderr, "sign: \"%s\" -> %zu glyphs\n", text.ToChar(), out.size());
    }
    ISignpostTextureBinding* CreateTextureBinding(std::uint32_t instance) override {   // {0x7ab50e44, 0x1abe787d, instance}
        const std::uint32_t id = NativeFshTexture(0x1abe787d, instance);
        if (Trace()) std::fprintf(stderr, "sign: frame texture %08x -> %u\n", instance, id);
        return id ? new FrameTexture(id) : nullptr;
    }
    int ViewZoom() override { SC4ViewUtilities* v = Utilities(); return v ? v->Zoom() : 5; }
    void CameraViewDirection(float& x, float& z) override {
        SC4ViewUtilities* v = Utilities();
        x = v ? v->ViewDirection().x : 0.0f;
        z = v ? v->ViewDirection().z : 1.0f;
    }
    float ViewportUnitsToWorldUnits(float units) override { SC4ViewUtilities* v = Utilities(); return v ? v->ViewportUnitsToWorldUnits(units) : units; }

private:
    static SC4ViewUtilities* Utilities() { cSC43DRender* r = CityRenderer(); return r ? r->GetViewUtilities() : nullptr; }
};

}  // namespace

void InstallSignToolHost() {
    static SignHost host;
    SetSignToolHost(&host);
    static SignpostHost signposts;
    SetSignpostHost(&signposts);
}

}  // namespace native
