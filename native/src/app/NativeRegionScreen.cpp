#include "app/NativeJobs.h"
#include "app/NativeRegionScreen.h"
#include "app/NativeAnimationTicks.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

#include "app/NativeBoot.h"
#include "app/NativeUi.h"
#include "dbpf/DBPFReader.h"
#include "gz/RZServices.h"
#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/App.h"
#include "sc4/Nation.h"
#include "sc4/RegionalCity.h"
#include "sc4/SC4UI.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/Exemplar.h"

namespace native {

using namespace sc4;

namespace {

RegionScreenServices g_services;

cISC4Nation* Nation() { return g_services.app ? g_services.app->GetNation() : nullptr; }
cISC4Region* ActiveRegion() {
    cISC4Nation* nation = Nation();
    return nation ? nation->GetActiveRegion() : nullptr;
}
cSC4RegionalCity* Concrete(cISC4RegionalCity* city) { return static_cast<cSC4RegionalCity*>(city); }

// ---- a CPU bitmap as the window system's buffer (the region snapshots) --------------------------------------------------------
class BitmapBuffer final : public IWinBuffer {
public:
    BitmapBuffer() { buffer_ = new cGZBuffer(); buffer_->AddRef(); }
    explicit BitmapBuffer(cGZBuffer* adopted) : buffer_(adopted) {}   // takes the caller's reference
    ~BitmapBuffer() { if (buffer_) buffer_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { if (--refs_ == 0) { delete this; return 0; } return refs_; }
    const cRZRect* GetBounds() const override { return buffer_->GetBufferArea(); }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return buffer_->ConvertRGBValueToNative(r, g, b); }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override { buffer_->ConvertNativeValueToRGB(n, r, g, b); }
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    bool Init(int width, int height) override { return buffer_->Init(std::uint32_t(width), std::uint32_t(height), cGZBufferColorType{kBufferColorARGB8888, 32}); }
    bool Fill(std::uint32_t color, const cRZRect& rect) override { return buffer_->Fill(color, rect, nullptr); }
    gz::IDrawTargetBuffer* AsDrawSource() override { return buffer_; }
    cGZBuffer* Pixels() { return buffer_; }

private:
    int refs_ = 1;
    cGZBuffer* buffer_;
};

// ---- the screen's notification helper (cRZAutoMsg2Notification) --------------------------------------------------------------
class NotificationHelper final : public INotificationHelper {
public:
    void AddMessageTypes(const std::uint32_t* types, int count) override { types_.assign(types, types + count); }
    void AddAllNotifications(cIGZMessageTarget2* target) override {
        if (SL::spMS2) for (std::uint32_t type : types_) SL::spMS2->AddNotification(target, type);
    }
    void RemoveAllNotifications(cIGZMessageTarget2* target) override {
        if (SL::spMS2) for (std::uint32_t type : types_) SL::spMS2->RemoveNotification(target, type);
    }

private:
    std::vector<std::uint32_t> types_;
};

class Renderer final : public IRegionRenderer {
public:
    bool Init(int, int) override { return true; }
    void Shutdown() override {}
    void GetScreenOffsets(float& x, float& y) override { x = y = 0; }
};
// The animation tick manager (GZCOM 0xa9c73857 / iid 0x86ad10ed): Init / Shutdown / DoAnimationTick (vslots 0x18 / 0x20 / 0x48).
class FrameService final : public IRegionFrameService {
public:
    bool Init() override { return StartAnimationTicks(); }
    void Shutdown() override { StopAnimationTicks(); }
    void EndFrame() override { TickAnimation(); }
};
class EffectsManager final : public IEffectsManager {
public:
    bool Init() override { return true; }
    void Shutdown() override {}
};

// ---- a city, as the screen and the view see it ---------------------------------------------------------------------------------
class CityAdapter final : public IRegionalCity, public IRegionCityView {
public:
    explicit CityAdapter(cISC4RegionalCity* city) : city_(city) { city_->AddRef(); }
    ~CityAdapter() { city_->Release(); }
    // both bases are cIGZUnknown: one shared count
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { if (--refs_ == 0) { delete this; return 0; } return refs_; }
    // IRegionalCity
    int GetPopulation() override { return Concrete(city_)->GetPopulation(); }
    bool IsOccupied() override { return Concrete(city_)->GetEstablished(); }
    void GetSize(int& w, int& h) override { Concrete(city_)->GetCitySize(w, h); }
    void GetName(cRZString& out) override { Concrete(city_)->GetCityName(out); }
    // IRegionCityView
    void GetPosition(int& x, int& z) const override { Concrete(city_)->GetPosition(x, z); }
    int GetPopulation() const override { return Concrete(city_)->GetPopulation(); }
    int GetCommercialJobs() const override { return Concrete(city_)->GetCommercialJobs(); }
    int GetIndustrialJobs() const override { return Concrete(city_)->GetIndustrialJobs(); }
    int GetMayorRating() const override { return Concrete(city_)->GetMayorRating(); }
    int GetDifficultyLevel() const override { return int(Concrete(city_)->GetDifficultyLevel()); }
    void GetCityName(cIGZString& out) const override { Concrete(city_)->GetCityName(out); }
    void GetMayorName(cIGZString& out) const override { Concrete(city_)->GetMayorName(out); }
    bool GetEstablished() const override { return Concrete(city_)->GetEstablished(); }
    double GetBudget() const override { return Concrete(city_)->GetBudget(); }
    int GetTutorialGUID() const override { return Concrete(city_)->GetTutorialGUID(); }
    bool IsTutorial() const override { return Concrete(city_)->IsTutorial(); }
    void GetPointsOfInterest(int, std::vector<RegionalCityPointOfInterest>&) override {}
    RegionCityRef* Ref() { return &ref_; }
    std::string SavePath() const { cRZString path; city_->GetCitySaveFilePath(path); return path.ToChar(); }
    void Adopt() { ref_ = static_cast<IRegionalCity*>(this); }

private:
    cISC4RegionalCity* city_;
    std::uint32_t refs_ = 0;
    RegionCityRef ref_;
};

// One adapter per regional city, found by its cell. (The region's cells map every cell of a city to the same record.)
std::map<cISC4RegionalCity*, std::unique_ptr<CityAdapter, void (*)(CityAdapter*)>> g_adapters;

CityAdapter* AdapterFor(cISC4RegionalCity* city) {
    auto it = g_adapters.find(city);
    if (it != g_adapters.end()) return it->second.get();
    auto adapter = std::unique_ptr<CityAdapter, void (*)(CityAdapter*)>(new CityAdapter(city), [](CityAdapter* a) { a->AddRef(); a->Release(); });
    CityAdapter* raw = adapter.get();
    raw->Adopt();
    g_adapters.emplace(city, std::move(adapter));
    return raw;
}

CityAdapter* CityAt(int x, int z) {
    cISC4Region* region = ActiveRegion();
    if (!region) return nullptr;
    cRZAutoRefCount<cISC4RegionalCity>* holder = region->GetCity(x, z);
    return holder && holder->get() ? AdapterFor(holder->get()) : nullptr;
}

// ---- the region map's tiled canvas ----------------------------------------------------------------------------------------------
class TiledCanvas final : public sc4::ITiledScrollingCanvas {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { if (--refs_ == 0) { delete this; return 0; } return refs_; }

    bool Init(int width, int height, int tileWidth, int tileHeight, sc4::ITiledScrollingCanvasSource* source) override {
        width_ = width; height_ = height; tileW_ = tileWidth; tileH_ = tileHeight; source_ = source;
        cols_ = (width + tileWidth - 1) / tileWidth;
        rows_ = (height + tileHeight - 1) / tileHeight;
        tiles_.clear();
        for (int i = 0; i < cols_ * rows_; ++i) tiles_.emplace_back(new Buffer(tileWidth, tileHeight, false), true);
        dirty_ = true;
        return true;
    }
    bool Shutdown() override { tiles_.clear(); cols_ = rows_ = 0; dirty_ = false; source_ = nullptr; return true; }
    void ScrollTo(int, int) override { InvalidateAllTiles(); }
    void InvalidateRect(const cRZRect& rect) override {
        for (int r = 0; r < rows_; ++r)
            for (int c = 0; c < cols_; ++c)
                if (rect.left < (c + 1) * tileW_ && rect.right > c * tileW_ && rect.top < (r + 1) * tileH_ && rect.bottom > r * tileH_) {
                    tiles_[std::size_t(r * cols_ + c)].second = true;
                    dirty_ = true;
                }
    }
    void InvalidateAllTiles() override { for (auto& tile : tiles_) tile.second = true; dirty_ = true; }

    // Re-renders the dirty tiles and blits every tile onto the screen, every frame: the drifting clouds draw translucently
    // over the map each frame, so the map under them must be fresh (blitting only after a change let the clouds stack
    // up into solid white blocks). Returns false when there is nothing to draw.
    bool Draw() {
        if (!source_) return false;
        dirty_ = false;
        gz::cIGZDrawContext* screen = static_cast<gz::cIGZDrawContext*>(g_services.graphics->Primary()->GetDrawContext());
        for (int r = 0; r < rows_; ++r)
            for (int c = 0; c < cols_; ++c) {
                auto& tile = tiles_[std::size_t(r * cols_ + c)];
                if (tile.second) {
                    source_->RenderTile(tile.first->Pixels(), c * tileW_, r * tileH_);   // the CPU pixels: the cities are blended by hand
                    tile.second = false;
                }
                screen->Resume();
                const cRZRect from{0, 0, tileW_, tileH_}, to{c * tileW_, r * tileH_, (c + 1) * tileW_, (r + 1) * tileH_};
                screen->Blt(tile.first->Pixels(), from, to);
                screen->Suspend();
            }
        return true;
    }
    void MarkDirty() { dirty_ = true; }

private:
    std::uint32_t refs_ = 0;
    int width_ = 0, height_ = 0, tileW_ = 256, tileH_ = 256, cols_ = 0, rows_ = 0;
    sc4::ITiledScrollingCanvasSource* source_ = nullptr;
    std::vector<std::pair<std::unique_ptr<Buffer>, bool>> tiles_;   // buffer, dirty
    bool dirty_ = false;
};
std::vector<TiledCanvas*> g_layers;

class ViewHost final : public sc4::IRegionViewHost {
public:
    sc4::ITiledScrollingCanvas* CreateTiledScrollingCanvas() override { auto* canvas = new TiledCanvas(); canvas->AddRef(); return canvas; }
    void AddRenderLayer(sc4::ITiledScrollingCanvas* canvas) override { g_layers.push_back(static_cast<TiledCanvas*>(canvas)); }
    void RemoveRenderLayer(sc4::ITiledScrollingCanvas* canvas) override {
        g_layers.erase(std::remove(g_layers.begin(), g_layers.end(), static_cast<TiledCanvas*>(canvas)), g_layers.end());
    }
    void InvalidateRenderer() override { for (TiledCanvas* layer : g_layers) layer->MarkDirty(); }
    int NextActiveTutorialCity() override { return -1; }
    sc4::IRegionCityView* TutorialCity(int) override { return nullptr; }
    bool TutorialItemHidesMayorName(int) override { return false; }
    sc4::IRegionCityView* FindRegionalCity(int, int x, int z) override { return CityAt(x, z); }
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        return g_services.createWindowFromScript ? g_services.createWindowFromScript(key, parent, id) : nullptr;
    }
    bool CreateRegionEffect(const char*, sc4::IRegionViewEffect**) override { return false; }
    sc4::cISCPropertyHolder* SignpostTuning() override { return nullptr; }
    cIGZWin* ChildWindowForCursorPos(cIGZWin* window, int x, int y) override { return nSC4UI::GetChildWindowForCursorPos(window, x, y); }
} g_viewHost;

// ---- the region view as the screen's IRegionView ------------------------------------------------------------------------------
class ViewAdapter final : public IRegionView {
public:
    ViewAdapter() {
        view_ = new sc4::cSC4WinRegionView(g_services.graphics ? static_cast<cIGZWinMgr*>(mgr()) : nullptr);
        view_->AddRef();
    }
    ~ViewAdapter() override {
        for (sc4::cSC4RegionViewCity* city : mirrors_) delete city;
        view_->Release();
    }
    static cIGZWinMgr* mgr();

    void AddCity(const RegionViewCity& city) override {
        auto* mirror = new sc4::cSC4RegionViewCity();
        mirror->left = city.left; mirror->top = city.top; mirror->x = city.x; mirror->z = city.z;
        mirror->mapX = city.mapX; mirror->mapY = city.mapY; mirror->size = city.size;
        if (city.snapshot) mirror->image = static_cast<BitmapBuffer*>(city.snapshot.get())->Pixels();
        if (city.mask) mirror->mask = static_cast<BitmapBuffer*>(city.mask.get())->Pixels();
        BuildHitSpans(*mirror);
        mirrors_.push_back(mirror);
        copies_.push_back(std::make_unique<RegionViewCity>(city));
        view_->AddCity(mirror);
    }
    void UpdateCity(RegionViewCity* city) override { if (auto* m = MirrorOf(city->x, city->z)) view_->UpdateCity(m); }
    void SetScrollPos(int x, int y) override { view_->SetScrollPos(x, y); }
    RegionViewCity* GetCityAtPoint(int x, int y) override { return CopyOf(view_->GetCityAtPoint(x, y)); }
    RegionViewCity* GetSelectedCity() override { return CopyOf(view_->GetSelectedCity()); }
    void ClearSelection() override { view_->ClearSelection(); }
    void SetSignpostMode(bool mode, bool second) override { view_->SetSignpostMode(mode, second); }
    void EnableLabels(bool labels, bool second) override { view_->EnableLabels(labels, second); }
    void EnableGrid(bool on) override { view_->EnableGrid(on); }
    void EnableTutorialArrow(bool on) override { view_->EnableTutorialArrow(on); }
    void SetMapMode(int mode) override { view_->SetMapMode(mode); }
    void SelectCity(int x, int y) override { view_->SelectCity(x, y); }
    void GetRegionPlayfieldRect(RegionViewCity* city, cRZRect& out) override { if (auto* m = MirrorOf(city->x, city->z)) view_->GetRegionPlayfieldRect(*m, out); }
    void SetSignpostType(std::uint32_t type, std::uint32_t sub) override { view_->SetSignpostType(type, sub); }
    void SetScrollIndicator(bool on) override { view_->SetScrollIndicator(on); }
    void SetRegionIndex(int index) override { view_->SetRegionIndex(index); }
    void SetLabelColours(std::uint32_t a, std::uint32_t b) override { view_->SetLabelColours(a, b); }
    cIGZWin* AsWindow() override { return static_cast<cIGZWin*>(static_cast<cGZWin*>(view_)); }

private:
    // The hit-test runs of SC4CreateSpanList (threshold 0, no alpha bytes): per row, a start marker x | y << 16 and an end marker
    // (exclusive) for every run of pixels with any alpha; GetCityAtPoint tests membership by the parity of a lower_bound.
    static void BuildHitSpans(sc4::cSC4RegionViewCity& city) {
        city.hitSpans.clear();
        cIGZBuffer* image = city.image;
        if (!image) return;
        const std::uint32_t width = image->Width(), height = image->Height();
        for (std::uint32_t row = 0; row < height; ++row) {
            std::uint32_t start = 0;
            bool inside = false;
            for (std::uint32_t col = 0; col <= width; ++col) {
                const bool solid = col < width && (image->GetPixel(col, row) >> 24) != 0;
                if (solid && !inside) { start = col; inside = true; }
                else if (!solid && inside) {
                    city.hitSpans.push_back(start | (row << 16));
                    city.hitSpans.push_back(col | (row << 16));
                    inside = false;
                }
            }
        }
    }
    sc4::cSC4RegionViewCity* MirrorOf(int x, int z) {
        for (sc4::cSC4RegionViewCity* m : mirrors_) if (m->x == x && m->z == z) return m;
        return nullptr;
    }
    RegionViewCity* CopyOf(sc4::cSC4RegionViewCity* m) {
        if (!m) return nullptr;
        for (auto& c : copies_) if (c->x == m->x && c->z == m->z) return c.get();
        return nullptr;
    }
    sc4::cSC4WinRegionView* view_;
    std::vector<sc4::cSC4RegionViewCity*> mirrors_;
    std::vector<std::unique_ptr<RegionViewCity>> copies_;
};

// ---- the screen's host --------------------------------------------------------------------------------------------------------------
class ScreenHost final : public IRegionScreenHost {
public:
    cIGZMessageServer2* MessageServer2() override { return SL::spMS2; }
    cIGZCommandServer* CommandServer() override { return nullptr; }
    std::uint8_t* Preferences() override { return g_services.app->GetPreferences(); }
    void SavePreferences() override { g_services.app->SavePreferences(); }
    int DoubleClickTimeMs() override { return 500; }
    void PlayUiSound(std::uint32_t) override {}
    cIGZUnknown* GraphicSystemObject() override { return nullptr; }
    void SetTerrainMap(cSC4RegionalTerrainMap*) override {}
    void SetViewUtilities(IRegionRenderer*) override {}
    void ClearEffectsManagerGlobal() override {}
    void ClearViewGlobals() override {}
    void ClearTerrainMapGlobal() override {}
    void FlushResources() override {}
    IRegionRenderer* CreateRenderer() override { return &renderer_; }
    IRegionFrameService* CreateFrameService() override { return &frame_; }
    IEffectsManager* CreateEffectsManager() override { return &effects_; }
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { return nullptr; }
    INotificationHelper* CreateNotificationHelper() override { return &notifications_; }
    cIGZMessage2Standard* CreateStandardMessage() override { auto* m = new cRZMessage2Standard(); m->AddRef(); return m; }
    void PostEngineMessage(std::uint32_t type) override {
        if (!SL::spMS2) return;
        cRZAutoRefCount<cRZMessage2Standard> message(new cRZMessage2Standard());
        message->SetType(type);
        SL::spMS2->MessagePost(message.get(), false);
    }

    int RegionCount() override { return Nation() ? int(Nation()->GetRegionCount()) : 0; }
    int CurrentRegionIndex() override { return Nation() ? Nation()->GetActiveRegionIndex() : 0; }
    void UpdateRegionViewState() override {}
    bool PreviousRegionIndex(int&) override { return false; }
    void SelectRegion(int index) override { if (Nation()) Nation()->SetActiveRegion(std::uint32_t(index)); }
    const char* RegionNameAt(int index) override {
        cISC4Region* region = Nation() ? Nation()->GetRegion(std::uint32_t(index)) : nullptr;
        cIGZString* name = region ? region->GetName() : nullptr;
        return name ? name->ToChar() : nullptr;
    }
    const char* RegionName() override { return RegionNameAt(CurrentRegionIndex()); }
    // cISC4App::RequestGoToRegionView(false): the region screen is destroyed and rebuilt for the active region. The
    // region screen's own button calls this, so the rebuild waits for the end of the frame.
    bool ReloadRegion() override {
        static int pending = -1;
        if (pending >= 0) return true;
        pending = RegisterFrameSwap([] {
            UnregisterFrameSwap(pending);
            pending = -1;
            // RequestGoToRegionView(false) with no city open: destroy the current screen, make a region screen. The app's own
            // screen hooks are only installed with a city, so the nation host's (the app window's) do it here.
            if (::INationHost* nation = ::NationHost()) {
                nation->DestroyCurrentScreen();
                nation->CreateRegionScreen();
            }
        });
        return true;
    }
    void RequestQuitFromRegion(bool ask) override { if (cSC4App* app = BootApplication()) app->RequestQuitFromRegion(ask); }
    void DeleteRegion(int index) override { if (Nation() && index >= 0) Nation()->DeleteRegion(std::uint32_t(index)); }
    bool RegionCellBounds(int& left, int& top, int& right, int& bottom) override {
        cISC4Region* region = ActiveRegion();
        if (!region) return false;
        sc4::SC4RectT<int> rect;
        region->GetBoundingRect(rect);
        left = rect.x0; top = rect.z0; right = rect.x1; bottom = rect.z1;
        return true;
    }
    int CitySnapshotSet() override { return 0; }
    void CityLocations(std::vector<cISC4Region::cLocation>& out) override {
        if (cISC4Region* region = ActiveRegion()) region->GetCityLocations(out);
    }
    RegionCityRef* FindCity(int x, int z) override { CityAdapter* adapter = CityAt(x, z); return adapter ? adapter->Ref() : nullptr; }
    RegionCityRef* FindOrCreateCity(const RegionViewCity& city, bool) override { return FindCity(city.x, city.z); }
    bool LaunchRegionCityByName(const cRZString&, RegionCityRef*) override { return false; }
    void ClearRegionViewSlot() override {}
    std::uint32_t LaunchCityFromRegion(RegionCityRef* ref) override {
        auto* adapter = ref ? dynamic_cast<CityAdapter*>(ref->get()) : nullptr;
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "launch city adapter=%p\n", (void*)adapter);
        if (!adapter || !g_services.loadCity) return 0;
        const std::string path = adapter->SavePath();
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "launch city path \"%s\"\n", path.c_str());
        int x = 0, z = 0;
        adapter->GetPosition(x, z);
        cISC4Region* region = ActiveRegion();
        return !path.empty() && g_services.loadCity(path, region ? region->GetCity(x, z) : nullptr) ? 1 : 0;
    }
    void QuitToRegionSelect(bool) override {}

    IRegionView* CreateRegionView() override { return new ViewAdapter(); }
    IWinBuffer* LoadRegionScreenSnapshot(std::uint32_t instance) override {
        cGZBuffer* image = g_services.loadBitmap ? g_services.loadBitmap(0x6a1eed2c, instance) : nullptr;
        return image ? new BitmapBuffer(image) : nullptr;
    }
    IWinBuffer* CreateBlankDisplayBuffer() override { return new BitmapBuffer(); }
    // The pictures a city file carries for the region map: PNG records {0x8a2482b9, 0x4a2482bb, 0} (the city's block) and 2 (its mask).
    bool LoadCityThumbnails(int x, int z, IWinBuffer** image, IWinBuffer** mask) override {
        cISC4Region* region = ActiveRegion();
        cRZAutoRefCount<cISC4RegionalCity>* holder = region ? region->GetCity(x, z) : nullptr;
        if (!holder || !holder->get() || !DecodeFunction()) {
            if (std::getenv("SC4_DEBUG")) std::printf("region: city %d,%d no thumbnail (region %d, city %d, decoder %d)\n", x, z, region != nullptr, holder && holder->get(), DecodeFunction() != nullptr);
            return false;
        }
        cRZString path;
        Concrete(holder->get())->GetCitySaveFilePath(path);
        dbpf::Package package;
        if (!package.Open(path.ToChar())) {
            if (std::getenv("SC4_DEBUG")) std::printf("region: city %d,%d cannot open %s\n", x, z, path.ToChar());
            return false;
        }
        auto load = [&](std::uint32_t instance) -> IWinBuffer* {
            const dbpf::Entry* entry = package.Find(0x8a2482b9, 0x4a2482bb, instance);
            std::vector<std::uint8_t> bytes;
            if (!entry || !package.Read(*entry, bytes)) return nullptr;
            cGZBuffer* decoded = DecodeFunction()(bytes);
            return decoded ? new BitmapBuffer(decoded) : nullptr;
        };
        *image = load(0);
        *mask = load(2);
        if (std::getenv("SC4_DEBUG")) std::printf("region: city %d,%d thumbnail %s from %s\n", x, z, *image ? "loaded" : "MISSING", path.ToChar());
        return *image != nullptr;
    }

    // The region screen's tuning exemplar {0x6534284a, 0x690f693f, 0xaa383bfe} (ReadTunableValues, Mac i386 0x0019fbde): scroll
    // speeds, city half-sizes, water erosion, the grid / label colours and the cloud opacity. One reference, released by the
    // caller through the holder (it is the exemplar itself).
    cISCPropertyHolder* CreateTuningProperties() override {
        if (!SL::spRM) return nullptr;
        void* raw = nullptr;
        const cGZPersistResourceKey key{0x6534284a, 0x690f693f, 0xaa383bfe};
        if (!SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        return static_cast<cISCResExemplar*>(raw)->AsISCPropertyHolder();
    }
    // Property 0xca383cb1: the clouds' opacity range, two bytes.
    bool CloudOpacityBytes(cISCPropertyHolder* holder, std::uint8_t& first, std::uint8_t& second) override {
        std::vector<std::uint8_t> bytes;
        if (!holder || !cSCPropertyHelp::GetPropertyValue(holder, 0xca383cb1, bytes) || bytes.size() < 2) return false;
        first = bytes[0];
        second = bytes[1];
        return true;
    }
    bool CameraBasis(RegionCameraBasis&) override { return false; }
    void SetWaterErosionTunables(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void SetRegionHeightGlobals(std::uint32_t base, std::uint32_t other) override {
        cSC4Region::sBaseTerrainTypeHeight[0] = base;
        cSC4Region::sBaseTerrainTypeHeight[1] = other;
    }
    void ActivatePanelButton(cIGZWin*, cIGZWin*) override {}
    void RunGameOptionsDialog() override {
        native::RunGameOptionsDialog([] { return BootApplication() ? BootApplication()->GetPreferences() : nullptr; });
    }
    void RunAudioOptionsDialog() override {
        native::RunAudioOptionsDialog([] { return BootApplication() ? BootApplication()->GetPreferences() : nullptr; });
    }
    void RunGraphicOptionsDialog() override {
        native::RunGraphicOptionsDialog([] { return BootApplication() ? BootApplication()->GetPreferences() : nullptr; });
    }
    void FormatNumber(int value, cRZString& out) override {
        // thousands separated by commas, as the English language utility makes them
        std::string digits = std::to_string(value < 0 ? -value : value);
        for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(std::size_t(i), ",");
        out = cRZString(((value < 0 ? "-" : "") + digits).c_str());
    }

private:
    Renderer renderer_;
    FrameService frame_;
    EffectsManager effects_;
    NotificationHelper notifications_;
} g_screenHost;

cIGZWinMgr* g_winMgr = nullptr;

}  // namespace

cIGZWinMgr* ViewAdapter::mgr() { return g_winMgr; }

void InstallRegionScreenHost(const RegionScreenServices& services) {
    g_services = services;
    g_winMgr = RZ::WM();
    sc4::SetRegionScreenHost(&g_screenHost);
    sc4::SetRegionViewHost(&g_viewHost);
}

bool DrawRegionLayers() {
    bool drew = false;
    for (TiledCanvas* layer : g_layers) drew = layer->Draw() || drew;
    return drew;
}

}  // namespace native
