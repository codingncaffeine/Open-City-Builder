// The sign tool's host (sc4/ViewInputControlSignTool.h) on the open city: the renderer's picks, the terrain, the prop
// manager and developer, the view projection and the framework's message server. And the signposts' host
// (sc4/SignpostOccupant.h): the renderer, the glyph texture manager, the frame textures and the view. And the labels' host
// (sc4/TextDecalOccupant.h): the terrain view's overlay managers, the glyph texture manager's string textures, the terrain.
// All replace the generated stubs.
#include "app/NativeSignLabel.h"

#include "sc4/Render3D.h"   // first: its filter forward declaration must not meet sc4/OccupantManager.h's first
#include "app/NativeAudio.h"
#include <cstdio>
#include <cstdlib>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeGlyphTextures.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativeDecalOverlay.h"
#include "app/NativeUi.h"
#include "gz/ClipboardManager.h"
#include "sc4/CommonIIDs.h"
#include "sc4/GlyphTextureManager.h"
#include "sc4/OccupantManager.h"
#include "s3d/ModelInstance.h"
#include "sc4/PropDeveloper.h"
#include "sc4/PropManager.h"
#include "sc4/ServiceLocator.h"
#include "sc4/OverlayManager.h"
#include "sc4/SignpostOccupant.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantFilters.h"
#include "sc4/TextDecalOccupant.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewInputControlLabelTool.h"
#include "sc4/ViewInputControlSignLabelDemolish.h"
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
            auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
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
    void AddProp(sc4::cISC4Occupant* occupant) override {
        auto* props = Plain<cSC4PropManager>(CityComponent::PropManager);
        const bool added = props && props->AddCityProp(occupant);
        if (Trace()) std::fprintf(stderr, "sign: prop added %d (manager %p)\n", added, static_cast<void*>(props));
    }
    void RemoveProp(sc4::cISC4Occupant* occupant) override {
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
    void PlaySound(std::uint32_t group, std::uint32_t id) override { PostSoundSystemEvent(group, id); }   // the group is the sound event (start)
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
    void RendererRemoveModel(s3d::cIS3DModelInstance* model) override {
        cSC43DRender* r = CityRenderer();
        if (Trace() && r && r->GetViewUtilities()) {
            cS3DBoundingBox box;
            model->GetWorldBounds(box);
            int lo[2] = {0, 0}, hi[2] = {0, 0};
            r->GetViewUtilities()->ProjectBoundingBoxToViewportRectangle(box.min, box.max, lo, hi);
            std::fprintf(stderr, "sign: signpost %p repaint rect %d,%d..%d,%d\n", static_cast<void*>(model), lo[0], lo[1], hi[0], hi[1]);
        }
        const bool removed = r && r->RemoveModel(model);
        if (Trace()) std::fprintf(stderr, "sign: signpost %p removed %d\n", static_cast<void*>(model), removed);
    }
    void RendererMoveModel(s3d::cIS3DModelInstance* model) override { if (cSC43DRender* r = CityRenderer()) r->MoveModel(model); }
    void RendererChangeModel(s3d::cIS3DModelInstance* model, int reason) override {
        cSC43DRender* r = CityRenderer();
        const bool changed = r && r->ChangeModel(model, reason);
        if (Trace()) std::fprintf(stderr, "sign: signpost %p changed (%d) %d\n", static_cast<void*>(model), reason, changed);
    }
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

// The terrain view's overlay manager by index (SL::TerrainView() vslot 0x48 i386), as the label holds it: resolved on every
// call because the managers live with the city view.
class LabelOverlay final : public ITextDecalOverlay {
public:
    void SetIndex(int index) { index_ = index; }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 2; }    // static: the four adapters live as long as the process
    std::uint32_t Release() override { return 1; }
    std::int32_t AddDecal(float width, float turns, ITextTexture* texture, const float* positionXZ) override {
        cSTEOverlayManager* manager = DecalOverlayManager(index_);
        auto* text = dynamic_cast<cSC4GlyphTextureManager::StringTexture*>(texture);
        if (!manager || !text) return -1;
        const std::uint32_t id = manager->AddDecal(text->AsOverlayTexture(), cS3DVector2{positionXZ[0], positionXZ[1]}, width, turns);
        if (Trace()) std::fprintf(stderr, "label: \"%s\" overlay %d decal %d width %.1f turns %.2f at %.1f,%.1f\n", text->Text().ToChar(), index_,
                                  static_cast<int>(id), width, turns, positionXZ[0], positionXZ[1]);
        return static_cast<std::int32_t>(id);
    }
    void MoveDecal(std::int32_t id, const float* positionXZ) override {
        if (cSTEOverlayManager* manager = DecalOverlayManager(index_)) manager->MoveDecal(static_cast<std::uint32_t>(id), cS3DVector2{positionXZ[0], positionXZ[1]});
    }
    void RemoveDecal(std::int32_t id) override {
        if (cSTEOverlayManager* manager = DecalOverlayManager(index_)) manager->RemoveOverlay(static_cast<std::uint32_t>(id));
    }
    void SetDecalAlpha(std::int32_t id, float alpha) override {
        if (cSTEOverlayManager* manager = DecalOverlayManager(index_)) manager->SetOverlayAlpha(static_cast<std::uint32_t>(id), alpha);
    }
    void SetDecalColor(std::int32_t id, const float* rgb) override {
        if (cSTEOverlayManager* manager = DecalOverlayManager(index_)) manager->SetOverlayColor(static_cast<std::uint32_t>(id), cS3DVector3{rgb[0], rgb[1], rgb[2]});
    }

private:
    int index_ = 0;
};

struct LabelHost final : ITextDecalHost {
    LabelHost() { for (int i = 0; i < 4; ++i) overlays_[i].SetIndex(i); }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    cISC4OccupantManager* OccupantManager() override { return signposts_.OccupantManager(); }
    ITextDecalViewSet* AcquireViewSet() override { return nullptr; }   // PARTIAL: View3D vslot 0x90; the model maker shows them
    bool PropDeveloperAllowsDecals() override { return signposts_.PropDeveloperAllowsSigns(); }
    ITextDecalOverlay* OverlayManager(std::uint32_t index) override { return index < 4 ? &overlays_[index] : nullptr; }
    // spGlyphTextureManager slot 5 GetCachedStringTexture with the "Label" font style
    bool CreateTextTexture(const cRZString& text, ITextTexture*& out) override {
        out = nullptr;
        cSC4GlyphTextureManager* manager = GlyphTextureManager();
        cSC4GlyphTextureManager::StringTexture* texture = nullptr;
        if (!manager || !manager->GetCachedStringTexture(kGlyphTextureType, text, &texture)) return false;
        out = texture;
        return true;
    }
    float TerrainHeight(float x, float z) override {
        cSTETerrain* terrain = Component<cSTETerrain>(CityComponent::Terrain);
        return terrain ? terrain->GetAltitude(x, z) : 0.0f;
    }
    int ViewRotation() override {   // spRenderer vslot 0x28 (i386): the draw context's rotation (+0x18)
        cSC43DRender* render = CityRenderer();
        SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr;
        int zoom = 0, rotation = 0;
        if (view) view->GetZoomAndRotation(zoom, rotation);
        return rotation;
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = static_cast<int>(x / 16.0f);
        cellZ = static_cast<int>(z / 16.0f);
    }

private:
    SignpostHost signposts_;
    LabelOverlay overlays_[4];
};

// A network piece for the label tool: the traffic network map's occupant, its edge words from the network occupant.
class LabelPiece final : public ILabelNetworkPiece {
public:
    ILabelNetworkPiece* Wrap(ITrafficNetworkMapOccupant* occupant) {
        occupant_ = occupant;
        network_ = occupant ? dynamic_cast<cSC4NetworkOccupant*>(occupant->AsOccupant()) : nullptr;
        return occupant ? this : nullptr;
    }
    bool HasAllNetworkFlags(std::uint32_t flags) override { return occupant_->HasNetworkFlag(flags); }
    bool HasAnyNetworkFlag(std::uint32_t flags) override { return occupant_->HasAnyNetworkFlag(flags); }
    bool IsOfType(std::uint32_t type) override { return occupant_->IsOfType(type); }
    std::uint32_t EdgeWord(std::uint32_t type) override { return network_ ? network_->GetEdgeTypesFlag(static_cast<std::uint8_t>(type)) : 0; }

private:
    ITrafficNetworkMapOccupant* occupant_ = nullptr;
    cSC4NetworkOccupant* network_ = nullptr;
};

cSC4TrafficNetworkMap* NetworkMap() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::TrafficNetwork) : nullptr;
    return object ? dynamic_cast<cSC4TrafficNetworkMap*>(object) : nullptr;
}

struct LabelToolHostImpl final : ILabelToolHost {
    bool HasRenderer() override { return CityRenderer() != nullptr; }
    bool PickTerrain(int x, int y, float out[3]) override {
        cSC43DRender* render = CityRenderer();
        return render && render->PickTerrain(x, y, out, false, true);
    }
    int ViewRotation() override { return labels_.ViewRotation(); }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cellX = cellZ = 0;
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    void CellCenter(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
    }
    bool CellHasWater(int cellX, int cellZ) override {
        cSTETerrain* terrain = Component<cSTETerrain>(CityComponent::Terrain);
        return terrain && cellX >= 0 && cellZ >= 0 && terrain->CellHasWater(static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellZ));
    }
    ILabelNetworkPiece* NetworkAtCell(int cellX, int cellZ, std::uint32_t mask) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        return map ? cell_.Wrap(map->FindNetworkOccupant(cellX, cellZ, mask, false)) : nullptr;
    }
    ILabelNetworkPiece* NetworkAtPosition(const float* position, std::uint32_t mask) override {
        cSC4TrafficNetworkMap* map = NetworkMap();
        return map ? at_.Wrap(map->FindNetworkOccupant(position, mask, true)) : nullptr;
    }
    cISC4TextDecalOccupant* LabelAt(float x, float z) override {
        cISC4OccupantManager* manager = labels_.OccupantManager();
        if (!manager) return nullptr;
        auto* filter = new cSC4DecalFilter();
        filter->AddRef();
        sc4::cISC4Occupant* occupant = nullptr;
        manager->GetFirstOccupantByPosition(occupant, x, z, filter);
        filter->Release();
        cISC4TextDecalOccupant* label = nullptr;
        void* raw = nullptr;
        if (occupant && occupant->QueryInterface(kGZIID_cISC4TextDecalOccupant, &raw) && raw) label = static_cast<cISC4TextDecalOccupant*>(raw);
        if (occupant) occupant->Release();
        return label;
    }
    cISC4TextDecalOccupant* CreateLabel() override {
        auto* label = new cSC4TextDecalOccupant();
        cISC4TextDecalOccupant* view = label;
        view->AddRef();
        return view;
    }
    void AddProp(sc4::cISC4Occupant* occupant) override {
        auto* props = Plain<cSC4PropManager>(CityComponent::PropManager);
        const bool added = props && props->AddCityProp(occupant);
        if (Trace()) std::fprintf(stderr, "label tool: prop added %d\n", added);
    }
    void RemoveProp(sc4::cISC4Occupant* occupant) override {
        if (auto* props = Plain<cSC4PropManager>(CityComponent::PropManager)) props->RemovePropA(occupant);
    }
    void ShowSignsAndLabels() override { if (cSC4PropDeveloper* developer = PropDeveloper()) developer->ShowSignsAndLabels(true); }
    void PlaySound(std::uint32_t group, std::uint32_t id) override { PostSoundSystemEvent(group, id); }   // the group is the sound event (start)
    cIGZWin* View3DWindow() override {
        cSC4View3DWin* view = CityView3DWin();
        return view ? static_cast<cIGZWin*>(static_cast<cGZWin*>(view)) : nullptr;
    }
    cIGZWin* CreateWindowFromScript(std::uint32_t group, std::uint32_t instance, cIGZWin* parent, std::uint32_t id) override {
        cIGZWin* window = native::CreateWindowFromScript(cGZPersistResourceKey{0, group, instance}, parent, id);
        if (Trace()) std::fprintf(stderr, "label tool: dialog %08x -> %s\n", id, window ? "made" : "FAILED");
        return window;
    }
    cIGZWinMgr* WindowManager() override {
        cIGZWin* view = View3DWindow();
        return view ? view->GetWindowManager() : nullptr;
    }

private:
    LabelHost labels_;
    LabelPiece cell_, at_;
};

struct SignLabelDemolishHostImpl final : ISignLabelDemolishHost {
    sc4::cISC4Occupant* PickOccupant(int x, int y, sc4::cISC4OccupantFilter* filter) override {
        static class Renderer renderer;
        IPickedObject* picked = nullptr;
        if (!CityRenderer() || !renderer.PickObject(x, y, filter, &picked) || !picked) return nullptr;
        cIGZUnknown* owner = picked->GetOwner();
        void* raw = nullptr;
        sc4::cISC4Occupant* occupant = nullptr;
        if (owner && owner->QueryInterface(kGZIID_cISC4Occupant, &raw) && raw) occupant = static_cast<sc4::cISC4Occupant*>(raw);
        picked->Release();
        return occupant;
    }
    bool PickTerrain(int x, int y, float out[3]) override { return tool_.PickTerrain(x, y, out); }
    sc4::cISC4Occupant* FirstOccupantAt(float x, float z, sc4::cISC4OccupantFilter* filter) override {
        cISC4OccupantManager* manager = labels_.OccupantManager();
        sc4::cISC4Occupant* occupant = nullptr;
        if (manager) manager->GetFirstOccupantByPosition(occupant, x, z, filter);
        return occupant;
    }
    void RemoveProp(sc4::cISC4Occupant* occupant) override {
        tool_.RemoveProp(occupant);
        if (Trace()) std::fprintf(stderr, "sign bulldozer: removed %p\n", static_cast<void*>(occupant));
    }
    void ShowSignsAndLabels() override { tool_.ShowSignsAndLabels(); }
    void PlaySound(std::uint32_t group, std::uint32_t id) override { PostSoundSystemEvent(group, id); }   // the group is the sound event (start)

private:
    LabelHost labels_;
    LabelToolHostImpl tool_;
};

}  // namespace

cSC4ViewInputControl* CreateLabelTool() { return new cSC4ViewInputControlLabelTool(); }
cSC4ViewInputControl* CreateSignLabelDemolishTool() { return new cSC4ViewInputControlSignLabelDemolish(); }

void InstallSignToolHost() {
    static SignHost host;
    SetSignToolHost(&host);
    static SignpostHost signposts;
    SetSignpostHost(&signposts);
    static LabelHost labels;
    SetTextDecalHost(&labels);
    static LabelToolHostImpl labelTool;
    SetLabelToolHost(&labelTool);
    static SignLabelDemolishHostImpl demolish;
    SetSignLabelDemolishHost(&demolish);
}

}  // namespace native
