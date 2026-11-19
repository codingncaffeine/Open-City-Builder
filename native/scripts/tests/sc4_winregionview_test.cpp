// cSC4WinRegionView test: a fake host (canvas, tutorial registry, region city list, script windows, effects, tuning
// holder, cursor hit test), a fake window manager and framework, a fake language formatter and a fake cSLBuffer host.
// Covers the constructor's option bytes and two-vtable QueryInterface, Init (canvas / renderer / backdrop / icon buffers
// and the second-call early out), SetArea's canvas re-init, the world-space rectangle of InvalidateRegion, the scrolled
// window rectangles, the packed-span hit test, selection, scrolling (including the signpost y placement quirk), the
// option setters, the signpost creation / teardown / captions / bar and tutorial-ghost paths, the effect icons UpdateCity
// builds, ClearCities' effect stop, the backdrop blit of the partial RenderTile, the cursor transparency seam and the
// Shutdown / destructor release path.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "fake_winmgr2.h"
#include "gz/GZCOMDirector.h"
#include "sc4/SC4Utilities.h"
#include "sc4/SLBuffer.h"
#include "sc4/WinAuraAlert.h"
#include "sc4/WinRegionView.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Tuning / language / buffer services
// ---------------------------------------------------------------------------------------------------------------
struct FakeFormatter final : ILanguageFormatter {
    void FormatMoney(std::int64_t value, cIGZString& out, const cRZString&) override {
        out.AssignFromCString(("money:" + std::to_string(value)).c_str());
    }
    void FormatMoney(double, cIGZString&, const cIGZString&, const cRZString&) override {}
    void FormatNumber(std::int64_t value, cRZString& out) override {
        out.AssignFromCString(("num:" + std::to_string(value)).c_str());
    }
};
FakeFormatter g_formatter;

struct BufferRequest {
    std::uint32_t type = 0, group = 0, instance = 0;
    std::int32_t colorType = 0;
};
struct FakeBufferHost final : ISLBufferHost {
    std::vector<BufferRequest> requests;
    bool GetBufferResource(const cGZPersistResourceKey& key, std::int32_t colorType, bool, cIGZBuffer** out) override {
        requests.push_back({key.type, key.group, key.instance, colorType});
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        buffer->Init(8, 8, cGZBufferColorType{kBufferColorRGB565, 16});
        buffer->Fill(0x1234, nullptr);
        *out = buffer;
        return true;
    }
    bool HasResource(const cGZPersistResourceKey&) override { return true; }
    bool LowQualityTextures() override { return false; }
    bool LoadBufferFromFile(const std::string&, std::int32_t, cIGZBuffer** out) override { *out = nullptr; return false; }
    cIGZBuffer* CreateBuffer() override {
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        buffer->Init(8, 8, cGZBufferColorType{kBufferColorRGB565, 16});
        return buffer;
    }
};
FakeBufferHost g_bufferHost;

// ---------------------------------------------------------------------------------------------------------------
// Canvas / host / framework
// ---------------------------------------------------------------------------------------------------------------
struct FakeCanvas final : ITiledScrollingCanvas {
    int refs = 1;
    int inits = 0, initW = 0, initH = 0, tileW = 0, tileH = 0;
    ITiledScrollingCanvasSource* source = nullptr;
    int scrolls = 0, scrollX = 0, scrollY = 0;
    std::vector<cRZRect> invalidated;
    int invalidateAll = 0, shutdowns = 0;
    bool initResult = true;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init(int width, int height, int tw, int th, ITiledScrollingCanvasSource* src) override {
        ++inits;
        initW = width; initH = height; tileW = tw; tileH = th; source = src;
        return initResult;
    }
    bool Shutdown() override { ++shutdowns; return true; }
    void ScrollTo(int x, int y) override { ++scrolls; scrollX = x; scrollY = y; }
    void InvalidateRect(const cRZRect& rect) override { invalidated.push_back(rect); }
    void InvalidateAllTiles() override { ++invalidateAll; }
};

struct FakeEffect final : IRegionViewEffect {
    static int created;
    static int stopped;
    int refs = 0;                     // CreateEffect hands out one counted reference
    int starts = 0;
    cS3DTransform transform;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) { delete this; return 0; }
        return static_cast<std::uint32_t>(refs);
    }
    void Start(int) override { ++starts; }
    void Stop(int) override { ++stopped; }
    void SetEffectTransform(const cS3DTransform& value) override { transform = value; }
};
int FakeEffect::created = 0;
int FakeEffect::stopped = 0;

struct FakeRegionalCity final : IRegionCityView {
    int refs = 1;
    int x = 0, z = 0;
    int population = 0, commercial = 0, industrial = 0, rating = 0, difficulty = 0, tutorialGuid = 0;
    bool established = true, tutorial = false;
    double budget = 0;
    std::string name, mayor;
    std::map<int, std::vector<RegionalCityPointOfInterest>> points;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void GetPosition(int& outX, int& outZ) const override { outX = x; outZ = z; }
    int GetPopulation() const override { return population; }
    int GetCommercialJobs() const override { return commercial; }
    int GetIndustrialJobs() const override { return industrial; }
    int GetMayorRating() const override { return rating; }
    int GetDifficultyLevel() const override { return difficulty; }
    void GetCityName(cIGZString& out) const override { out.AssignFromCString(name.c_str()); }
    void GetMayorName(cIGZString& out) const override { out.AssignFromCString(mayor.c_str()); }
    bool GetEstablished() const override { return established; }
    double GetBudget() const override { return budget; }
    int GetTutorialGUID() const override { return tutorialGuid; }
    bool IsTutorial() const override { return tutorial; }
    void GetPointsOfInterest(int type, std::vector<RegionalCityPointOfInterest>& out) override { out = points[type]; }
};

struct FakeHost final : IRegionViewHost {
    FakeCanvas* canvas = new FakeCanvas();
    int addLayer = 0, removeLayer = 0, invalidations = 0;
    int tutorialItem = -1;
    IRegionCityView* tutorialCity = nullptr;
    std::map<int, bool> hidesMayorName;
    std::map<std::pair<int, int>, IRegionCityView*> cities;
    std::vector<std::tuple<int, int, int>> cityLookups;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>> scriptRequests;
    std::function<cIGZWin*(const cGZPersistResourceKey&, cIGZWin*, std::uint32_t)> scriptFactory;
    std::vector<std::string> effectNames;
    bool effectResult = true;
    cISCPropertyHolder* tuning = nullptr;
    cIGZWin* cursorChild = nullptr;
    int cursorCalls = 0;
    int cursorX = 0, cursorY = 0;

    ITiledScrollingCanvas* CreateTiledScrollingCanvas() override { return canvas; }
    void AddRenderLayer(ITiledScrollingCanvas* c) override { (void)c; ++addLayer; }
    void RemoveRenderLayer(ITiledScrollingCanvas* c) override { (void)c; ++removeLayer; }
    void InvalidateRenderer() override { ++invalidations; }
    int NextActiveTutorialCity() override { return tutorialItem; }
    IRegionCityView* TutorialCity(int item) override { (void)item; return tutorialCity; }
    bool TutorialItemHidesMayorName(int guid) override {
        auto it = hidesMayorName.find(guid);
        return it != hidesMayorName.end() && it->second;
    }
    IRegionCityView* FindRegionalCity(int regionIndex, int x, int z) override {
        cityLookups.emplace_back(regionIndex, x, z);
        auto it = cities.find({x, z});
        return it == cities.end() ? nullptr : it->second;
    }
    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        scriptRequests.emplace_back(key.type, key.group, key.instance, id);
        return scriptFactory ? scriptFactory(key, parent, id) : nullptr;
    }
    bool CreateRegionEffect(const char* name, IRegionViewEffect** out) override {
        effectNames.emplace_back(name ? name : "");
        if (!effectResult) { *out = nullptr; return false; }
        auto* effect = new FakeEffect();
        ++FakeEffect::created;
        effect->AddRef();   // the counted reference CreateEffect hands out
        *out = effect;
        return true;
    }
    cISCPropertyHolder* SignpostTuning() override { return tuning; }
    cIGZWin* ChildWindowForCursorPos(cIGZWin* window, int x, int y) override {
        ++cursorCalls;
        cursorX = x; cursorY = y;
        (void)window;
        return cursorChild;
    }
};

struct FakeFramework final : cIGZFrameWork {
    cIGZWinMgr* manager = nullptr;
    std::uint32_t refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }
    bool AddSystemService(cIGZSystemService*) override { return false; }
    bool RemoveSystemService(cIGZSystemService*) override { return false; }
    bool GetSystemService(std::uint32_t serviceId, std::uint32_t iid, void** out) override {
        if (serviceId == 0xa417445e && iid == 0x5a4 && manager) { *out = manager; return true; }
        return false;
    }
    bool EnumSystemServices(cIGZUnknownEnumerator&, cIGZUnknown*, std::uint32_t) override { return false; }
    bool AddHook(cIGZFrameWorkHooks*) override { return false; }
    bool RemoveHook(cIGZFrameWorkHooks*) override { return false; }
    bool AddToTick(cIGZSystemService*) override { return false; }
    bool RemoveFromTick(cIGZSystemService*) override { return false; }
    bool AddToOnIdle(cIGZSystemService*) override { return false; }
    bool RemoveFromOnIdle(cIGZSystemService*) override { return false; }
    int GetOnIdleInterval() const override { return 1; }
    void SetOnIdleInterval(int) override {}
    void OnTick(std::uint32_t) override {}
    void OnIdle() override {}
    bool IsTickEnabled() const override { return true; }
    void ToggleTick(bool) override {}
    void Quit(int) override {}
    void AbortiveQuit(int) override {}
    cIGZCmdLine* CommandLine() override { return nullptr; }
    bool IsInstall() const override { return false; }
    cIGZCOM* GetCOMObject() override { return nullptr; }
    int GetState() const override { return 0; }
    cIGZDebugStream* GetDebugStream() override { return nullptr; }
    cIGZDebugStream* DefaultDebugStream() override { return nullptr; }
    cIGZDebugStream* DebugStream() override { return nullptr; }
    void SetDebugStream(cIGZDebugStream*) override {}
    void SetDebugLevel(int) override {}
    int GetDebugLevel() const override { return 0; }
    cIGZOStream* StdOut() override { return nullptr; }
    cIGZOStream* StdErr() override { return nullptr; }
    cIGZIStream* StdIn() override { return nullptr; }
    bool GetStream(int, std::uint32_t, void**) override { return false; }
    bool SetStream(int, cIGZUnknown*) override { return false; }
    void SetApplication(cIGZApp*) override {}
    cIGZApp* Application() const override { return nullptr; }
    void ReportException(const char*) override {}
    cIGZUnknown* ExceptionNotificationObj() const override { return nullptr; }
};

// ---------------------------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------------------------
struct FakeWin final : cGZWin {
    using cGZWin::cGZWin;
};

cIGZBuffer* MakeBuffer(int w, int h, std::uint32_t fill = 0) {
    auto* buffer = new cGZBuffer();
    buffer->AddRef();
    buffer->Init(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), cGZBufferColorType{kBufferColorRGB565, 16});
    if (fill) buffer->Fill(fill, nullptr);
    return buffer;
}

FakeWin* AddChild(FakeMgr2& mgr, cGZWin* parent, std::uint32_t id, int w = 1, int h = 1) {
    auto* child = new FakeWin(&mgr);
    mgr.AddWindowToValidList(child);
    child->AddRef();
    child->SetID(id);
    child->SetArea(0, 0, w, h);
    parent->ChildAdd(child);
    child->Release();
    return child;
}

struct SignpostParts {
    FakeWin* window = nullptr;
    std::map<std::uint32_t, FakeWin*> children;
    cSC4WinAuraBar* bar = nullptr;
};

SignpostParts BuildSignpost(FakeMgr2& mgr, cIGZWin* parent, std::uint32_t id, std::uint32_t keyInstance) {
    SignpostParts parts;
    auto* window = new FakeWin(&mgr);
    mgr.AddWindowToValidList(window);
    window->AddRef();                 // the caller's reference (what the script service returns)
    window->SetID(id);
    window->SetArea(0, 0, 40, 30);
    const std::uint32_t ids[] = {kSignpostCityNameId, kSignpostMayorNameId, kSignpostBudgetId, kSignpostBudgetNegativeId,
                                 kSignpostPopulationId, kSignpostCommercialId, kSignpostIndustrialId,
                                 kSignpostDifficultyLowId, kSignpostDifficultyMediumId, kSignpostDifficultyHighId,
                                 kSignpostEmptyBudgetId};
    for (std::uint32_t childId : ids) parts.children[childId] = AddChild(mgr, window, childId);
    auto* bar = new cSC4WinAuraBar(&mgr);
    mgr.AddWindowToValidList(bar);
    bar->AddRef();
    bar->SetID(kRegionViewAuraBarId);
    bar->SetArea(0, 0, 20, 4);
    window->ChildAdd(bar);
    bar->Release();
    parts.bar = bar;
    (void)keyInstance;
    parts.window = window;
    parent->ChildAdd(window);
    return parts;
}

// ---------------------------------------------------------------------------------------------------------------
// Rig
// ---------------------------------------------------------------------------------------------------------------
struct Rig {
    FakeMgr2 mgr;
    FakeHost host;
    FakeFramework framework;
    cSC4WinRegionView* view = nullptr;

    Rig() {
        SetRegionViewHost(&host);
        SetRZFramework(&framework);
        framework.manager = &mgr;
        view = new cSC4WinRegionView(&mgr);
        view->AddRef();
        mgr.AddWindowToValidList(view);
        view->SetArea(0, 0, 640, 480);
    }
    ~Rig() {
        view->Release();
        SetRZFramework(nullptr);
        SetRegionViewHost(nullptr);
        delete host.canvas;
    }
};

// The original city record owns its signpost reference; the test creates them through the view, so drop it the same way.
void DropCitySignpost(cSC4RegionViewCity& city) {
    cIGZWin* window = city.signpost;
    city.signpost = nullptr;
    if (window) window->Release();
}

cSC4RegionViewCity MakeCity(int x, int z, float mapX, float mapY, cIGZBuffer* image) {
    cSC4RegionViewCity city;
    city.x = x; city.z = z; city.mapX = mapX; city.mapY = mapY;
    city.image = image;
    return city;
}

// ===============================================================================================================
// Tests
// ===============================================================================================================

void TestConstructAndQueryInterface() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    CHECK(view->SignpostMode() && !view->ForcedSignposts());
    CHECK(view->Grid() && !view->Labels() && !view->AlwaysLabels());
    CHECK(view->TutorialArrow());
    CHECK(view->MapMode() == 0);
    CHECK(view->Backdrop() == nullptr && view->Canvas() == nullptr && view->Cities().empty());
    CHECK(view->ScrollX() == 0 && view->ScrollY() == 0);
    CHECK(!view->GetFlag(0x10000) && view->GetFlag(0x8000));   // cGZWin initial flags are 0x8903
    // QueryInterface forwards to cGZWin: the three window iids, and not the canvas-source view.
    void* out = nullptr;
    CHECK(view->QueryInterface(kGZIID_cIGZWin, &out) && out == static_cast<cIGZWin*>(view));
    view->Release();   // QueryInterface returns an AddRef'd pointer
    out = nullptr;
    CHECK(view->QueryInterface(1, &out) && out != nullptr);
    view->Release();
    out = nullptr;
    CHECK(view->QueryInterface(kGZIID_cGZWinAlt, &out) && out != nullptr);
    view->Release();
    out = nullptr;
    CHECK(!view->QueryInterface(0x4a5d1208, &out));
    CHECK(!view->QueryInterface(0x5386d516, &out));
    // Both vtable groups share the reference count.
    const std::uint32_t before = view->AddRef();
    CHECK(view->Release() == before - 1);
}

void TestInitAndShutdown() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    g_bufferHost.requests.clear();
    CHECK(view->Init());
    CHECK(view->GetID() == kRegionViewWindowId);
    CHECK(rig.host.canvas->inits == 1);
    CHECK(rig.host.canvas->initW == 640 && rig.host.canvas->initH == 480);
    CHECK(rig.host.canvas->tileW == 0x100 && rig.host.canvas->tileH == 0x100);
    CHECK(rig.host.canvas->source == static_cast<ITiledScrollingCanvasSource*>(view));
    CHECK(rig.host.addLayer == 1);
    CHECK(view->Canvas() == rig.host.canvas);
    CHECK(!view->GetFlag(0x10000));
    CHECK(!view->GetFlag(0x8000));
    CHECK(view->Backdrop() != nullptr);
    CHECK(view->IconBuffers().size() == 2);
    CHECK(view->IconBuffers()[0].get() != nullptr && view->IconBuffers()[1].get() != nullptr);
    // Backdrop key {0x856ddbac, 0x6a1eed2c, 0x4a2805ff, colour type 1}, then the two icon bitmaps.
    CHECK(g_bufferHost.requests.size() == 3);
    if (g_bufferHost.requests.size() == 3) {
        CHECK(g_bufferHost.requests[0].type == 0x856ddbac && g_bufferHost.requests[0].group == 0x6a1eed2c);
        CHECK(g_bufferHost.requests[0].instance == 0x4a2805ff && g_bufferHost.requests[0].colorType == 1);
        CHECK(g_bufferHost.requests[1].group == 0x46a006b0 && g_bufferHost.requests[1].instance == 0xebabb1b0);
        CHECK(g_bufferHost.requests[2].group == 0x46a006b0 && g_bufferHost.requests[2].instance == 0xebabb1b1);
    }
    CHECK(view->Init());                              // second call: the early out
    CHECK(rig.host.canvas->inits == 1);
    // SetArea re-initialises the canvas in place.
    view->SetArea(0, 0, 320, 240);
    CHECK(rig.host.canvas->inits == 2);
    CHECK(rig.host.canvas->initW == 320 && rig.host.canvas->initH == 240);
    CHECK(view->Shutdown());
    CHECK(rig.host.canvas->shutdowns == 1);
    CHECK(rig.host.removeLayer == 1);
    CHECK(view->Canvas() == nullptr && view->Backdrop() == nullptr);
    CHECK(!view->GetFlag(kWinInitialized));
}

void TestCityRectsAndHitTest() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    cSC4RegionViewCity first = MakeCity(2, 3, 100.5f, 200.5f, image);
    cSC4RegionViewCity second = MakeCity(4, 5, 210.0f, 220.0f, image);
    rig.host.cities[{2, 3}] = nullptr;                // no regional city: UpdateCity only invalidates
    view->AddCity(&first);
    view->AddCity(&second);

    // InvalidateRegion is in world coordinates: floor(mapX), floor(mapY) - image height.
    CHECK(rig.host.canvas->invalidated.size() == 2);
    if (rig.host.canvas->invalidated.size() == 2) {
        const cRZRect& r = rig.host.canvas->invalidated[0];
        CHECK(r.left == 100 && r.top == 168 && r.right == 164 && r.bottom == 200);
        const cRZRect& s = rig.host.canvas->invalidated[1];
        CHECK(s.left == 210 && s.top == 188 && s.right == 274 && s.bottom == 220);
    }
    view->SetScrollPos(7, 9);
    cRZRect screen;
    view->GetRegionScreenRect(first, screen);
    CHECK(screen.left == 93 && screen.top == 159 && screen.right == 157 && screen.bottom == 191);
    cRZRect playfield;
    view->GetRegionPlayfieldRect(first, playfield);
    CHECK(playfield.left == 100 && playfield.top == 168 && playfield.right == 164 && playfield.bottom == 200);
    // The scroll moved the canvas.
    CHECK(rig.host.canvas->scrolls == 1 && rig.host.canvas->scrollX == 7 && rig.host.canvas->scrollY == 9);
    view->SetScrollPos(7, 9);                          // no change: no second ScrollTo
    CHECK(rig.host.canvas->scrolls == 1);

    // The hit test uses the packed span stream: a run on row 5 from x = 10 to 20.
    first.hitSpans = {0x0005000au, 0x00050014u};
    // Screen point 108,164 -> rel (15,5) -> key 0x00050010, lower_bound index 1 (odd) -> hit.
    CHECK(view->GetCityAtPoint(108, 164) == &first);
    // rel x 25 -> key 0x0005001a -> index 2 (even) -> miss (and second city does not cover it).
    CHECK(view->GetCityAtPoint(118, 164) == nullptr);
    // rel y 6 -> index 2 -> miss.
    CHECK(view->GetCityAtPoint(108, 165) == nullptr);
    // Outside the image: x 92 is left of the rectangle.
    CHECK(view->GetCityAtPoint(92, 164) == nullptr);
    // The last city in the list is tested first: move the second city over the same point.
    second.mapX = 100.5f;
    second.mapY = 200.5f;
    second.hitSpans = {0x0005000au, 0x00050014u};
    CHECK(view->GetCityAtPoint(108, 164) == &second);
    view->ClearCities();
    image->Release();
}

void TestSelection() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    cSC4RegionViewCity city = MakeCity(2, 3, 100.5f, 200.5f, image);
    city.hitSpans = {0x0005000au, 0x00050014u};
    rig.host.cities[{2, 3}] = nullptr;
    view->AddCity(&city);
    view->SetScrollPos(7, 9);
    CHECK(view->GetSelectedCity() == nullptr);
    view->SelectCity(108, 164);
    CHECK(view->GetSelectedCity() == &city);
    const std::size_t before = rig.host.canvas->invalidated.size();
    view->SelectCity(108, 164);                        // same city: no invalidation
    CHECK(rig.host.canvas->invalidated.size() == before);
    view->ClearSelection();
    CHECK(view->GetSelectedCity() == nullptr);
    CHECK(rig.host.canvas->invalidated.size() == before + 1);
    view->ClearCities();
    image->Release();
}

void TestOptions() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    view->SetSignpostMode(false, true);
    CHECK(!view->SignpostMode() && view->ForcedSignposts());
    view->EnableTutorialArrow(false);
    CHECK(!view->TutorialArrow());
    const int invalidations = rig.host.canvas->invalidateAll;
    view->EnableGrid(false);
    CHECK(!view->Grid());
    CHECK(rig.host.canvas->invalidateAll == invalidations + 1);
    view->EnableLabels(true, true);
    CHECK(view->Labels() && view->AlwaysLabels());
    CHECK(rig.host.canvas->invalidateAll == invalidations + 2);
    view->SetMapMode(1);
    CHECK(view->MapMode() == 1);
    CHECK(rig.host.canvas->invalidateAll == invalidations + 3);
}

void TestScrollPositionsTheSignpost() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    cSC4RegionViewCity city = MakeCity(2, 3, 100.5f, 200.5f, image);
    rig.host.cities[{2, 3}] = nullptr;
    view->AddCity(&city);
    auto* signpost = new FakeWin(&rig.mgr);
    rig.mgr.AddWindowToValidList(signpost);
    signpost->AddRef();
    signpost->SetArea(0, 0, 30, 20);
    view->ChildAdd(signpost);
    cSC4WinRegionView::SetCitySignpost(city, signpost);
    signpost->AddRef();                                // the city's reference
    signpost->Release();                               // the test's local reference
    view->SetScrollPos(10, 20);
    // rect = {90, 148, 154, 180}: x = (154 + 90)/2 - 30/2 = 107, y = (180 + 148)/2 - 20 = 144 (whole height).
    CHECK(signpost->GetL() == 107 && signpost->GetT() == 144);
    cSC4WinRegionView::SetCitySignpost(city, nullptr);
    signpost->Release();   // the city's reference
    view->ClearCities();
    image->Release();
}

void TestUpdateCityEffects() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    FakeRegionalCity regional;
    regional.x = 2; regional.z = 3;
    regional.points[0] = {RegionalCityPointOfInterest{10.0f, 20.0f, 30.0f}};
    regional.points[1] = {RegionalCityPointOfInterest{1.0f, 2.0f, 3.0f}};
    rig.host.cities[{2, 3}] = &regional;
    cSC4RegionViewCity city = MakeCity(2, 3, 100.5f, 200.5f, image);
    view->AddCity(&city);
    CHECK(city.icons.size() == 2);
    if (city.icons.size() == 2) {
        CHECK(city.icons[0].x == 68 && city.icons[0].y == 173);
        CHECK(city.icons[0].image == view->IconBuffers()[0].get());
        CHECK(city.icons[1].image == view->IconBuffers()[1].get());
    }
    CHECK(rig.host.effectNames.size() == 2);
    if (rig.host.effectNames.size() == 2) {
        CHECK(rig.host.effectNames[0] == "region_airport");
        CHECK(rig.host.effectNames[1] == "region_seaport");
    }
    CHECK(city.effects.size() == 2);
    if (city.effects.size() == 2) {
        auto* effect = static_cast<FakeEffect*>(city.effects.front().get());
        CHECK(effect->starts == 1);
        CHECK(effect->transform.flags == 1 && effect->transform.translation[0] == 10.0f + 2048.0f);
        CHECK(effect->transform.translation[1] == 20.0f && effect->transform.translation[2] == 30.0f + 3072.0f);
    }
    // ClearCities stops every effect.
    view->ClearCities();
    CHECK(FakeEffect::stopped == 2);
    CHECK(view->Cities().empty());
    image->Release();
}

void TestUpdateSignpostOccupied() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    FakeRegionalCity regional;
    regional.x = 2; regional.z = 3;
    regional.name = "Testville";
    regional.mayor = "Mayor Bob";
    regional.population = 5000;
    regional.commercial = 1200;
    regional.industrial = 900;
    regional.rating = 60;
    regional.difficulty = 2;
    regional.budget = -1234.0;
    rig.host.cities[{2, 3}] = &regional;
    view->SetRegionIndex(4);
    auto* oldFocus = new FakeWin(&rig.mgr);
    rig.mgr.AddWindowToValidList(oldFocus);
    oldFocus->AddRef();
    rig.mgr.focus = oldFocus;

    cSC4RegionViewCity city = MakeCity(2, 3, 100.5f, 200.5f, image);
    view->AddCity(&city);   // signpost type is still 0, so AddCity creates no signpost
    CHECK(city.signpost == nullptr);
    view->SetSignpostType(0x1111, 0x2222);   // this loops UpdateSignpost; no type 0x1111 window yet? it does create
    rig.host.scriptRequests.clear();
    SignpostParts parts;
    rig.host.scriptFactory = [&](const cGZPersistResourceKey&, cIGZWin* parent, std::uint32_t id) {
        parts = BuildSignpost(rig.mgr, parent, id, 0);
        return parts.window;
    };
    view->UpdateSignpost(city);

    CHECK(rig.host.scriptRequests.size() == 1);
    if (!rig.host.scriptRequests.empty()) {
        // the UI script resources: type 0, group 0x96a006b0
        CHECK(std::get<0>(rig.host.scriptRequests[0]) == 0);
        CHECK(std::get<1>(rig.host.scriptRequests[0]) == 0x96a006b0);
        CHECK(std::get<2>(rig.host.scriptRequests[0]) == 0x1111);
        CHECK(std::get<3>(rig.host.scriptRequests[0]) == 0x2222);
    }
    CHECK(city.signpost == parts.window);
    CHECK(rig.mgr.focus == oldFocus);                  // the focus is restored
    // InitSignpost: rect {100,168,164,200} -> x = (164 + 100)/2 - 40/2 = 112, y = (200 + 168)/2 - 30 = 154.
    CHECK(parts.window->GetL() == 112 && parts.window->GetT() == 154);
    // Occupied: city / mayor names, stats, difficulty flags and the negative budget branch.
    CHECK(std::strcmp(parts.children[kSignpostCityNameId]->GetCaption().ToChar(), "Testville") == 0);
    CHECK(std::strcmp(parts.children[kSignpostMayorNameId]->GetCaption().ToChar(), "Mayor Bob") == 0);
    CHECK(std::strcmp(parts.children[kSignpostBudgetId]->GetCaption().ToChar(), "") == 0);
    CHECK(std::strcmp(parts.children[kSignpostBudgetNegativeId]->GetCaption().ToChar(), "money:-1234") == 0);
    CHECK(parts.children[kSignpostBudgetId]->IsVisible() == false);
    CHECK(parts.children[kSignpostBudgetNegativeId]->IsVisible() == true);
    CHECK(std::strcmp(parts.children[kSignpostPopulationId]->GetCaption().ToChar(), "num:5000") == 0);
    CHECK(std::strcmp(parts.children[kSignpostCommercialId]->GetCaption().ToChar(), "num:1200") == 0);
    CHECK(std::strcmp(parts.children[kSignpostIndustrialId]->GetCaption().ToChar(), "num:900") == 0);
    CHECK(parts.children[kSignpostDifficultyHighId]->GetFlag(1));
    CHECK(!parts.children[kSignpostDifficultyLowId]->GetFlag(1));
    CHECK(!parts.children[kSignpostDifficultyMediumId]->GetFlag(1));
    CHECK(std::fabs(parts.bar->GetValue() - (60.0 / 200.0 + 0.5)) < 1e-9);
    // The bar image was loaded as colour type 9.
    bool barImage = false;
    for (const BufferRequest& request : g_bufferHost.requests) {
        if (request.instance == kRegionViewBarImageInstance && request.group == kRegionViewBarImageGroup &&
            request.colorType == 9) {
            barImage = true;
        }
    }
    CHECK(barImage);
    // UpdateSignpost called it with the region index of the view.
    CHECK(!rig.host.cityLookups.empty());
    if (!rig.host.cityLookups.empty()) CHECK(std::get<0>(rig.host.cityLookups.back()) == 4);
    // Re-running UpdateSignpost deletes the old window and builds a new one.
    cIGZWin* firstWindow = city.signpost;
    view->UpdateSignpost(city);
    CHECK(rig.host.scriptRequests.size() == 2);
    // (the new window may reuse the freed one's address, so compare by what was built, not by pointer)
    (void)firstWindow;
    CHECK(city.signpost != nullptr);
    DropCitySignpost(city);
    view->ClearCities();
    oldFocus->Release();
    image->Release();
}

void TestUpdateSignpostTutorialGhost() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    FakeRegionalCity tutorialCity;
    tutorialCity.x = 2; tutorialCity.z = 3;
    rig.host.tutorialItem = 7;
    rig.host.tutorialCity = &tutorialCity;
    FakeRegionalCity unestablished;
    unestablished.established = false;
    rig.host.cities[{2, 3}] = &unestablished;          // regional city exists but is not established
    cSC4RegionViewCity city = MakeCity(2, 3, 100.5f, 200.5f, image);
    view->AddCity(&city);
    view->SetSignpostType(0x1111, 0x2222);
    rig.host.scriptRequests.clear();
    SignpostParts parts;
    rig.host.scriptFactory = [&](const cGZPersistResourceKey&, cIGZWin* parent, std::uint32_t id) {
        parts = BuildSignpost(rig.mgr, parent, id, 0);
        return parts.window;
    };
    view->UpdateSignpost(city);
    // tutorial arrow on, tutorial position == city, city not selected -> the special tutorial key / window id.
    CHECK(rig.host.scriptRequests.size() == 1);
    if (!rig.host.scriptRequests.empty()) {
        CHECK(std::get<2>(rig.host.scriptRequests[0]) == 0xca539343);
        CHECK(std::get<3>(rig.host.scriptRequests[0]) == 0x0a551c53);
    }
    CHECK(city.signpost != nullptr);
    DropCitySignpost(city);
    view->ClearCities();
    image->Release();
}

void TestUpdateSignpostDefaultsAndSkip() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    view->Init();
    auto* image = MakeBuffer(64, 32);
    view->SetSignpostType(0, 0);
    cSC4RegionViewCity city = MakeCity(2, 3, 100.5f, 200.5f, image);
    view->AddCity(&city);
    // signpost type 0: no window.
    view->UpdateSignpost(city);
    CHECK(rig.host.scriptRequests.empty());
    CHECK(city.signpost == nullptr);
    // Forced signposts + no regional city: the default budget (tuning holder) and bar value 0.5.
    view->SetSignpostType(0x1111, 0x2222);
    view->SetSignpostMode(true, true);
    cSCPropertyHolder tuning;
    tuning.AddProperty(kRegionViewSignpostBudgetProperty, static_cast<std::uint32_t>(7777), false);
    rig.host.tuning = &tuning;
    SignpostParts parts;
    rig.host.scriptFactory = [&](const cGZPersistResourceKey&, cIGZWin* parent, std::uint32_t id) {
        parts = BuildSignpost(rig.mgr, parent, id, 0);
        return parts.window;
    };
    rig.host.cities.erase({2, 3});
    view->UpdateSignpost(city);
    CHECK(rig.host.scriptRequests.size() == 1);
    CHECK(std::strcmp(parts.children[kSignpostBudgetId]->GetCaption().ToChar(), "money:7777") == 0);
    CHECK(!parts.children[kSignpostEmptyBudgetId]->GetFlag(2));
    CHECK(std::fabs(parts.bar->GetValue() - 0.5) < 1e-9);
    DropCitySignpost(city);
    // Labels / tutorial: filter_tutorial_mayor_name clears the name of a tutorial city whose item hides it.
    FakeRegionalCity tutorialCity;
    tutorialCity.tutorial = true;
    tutorialCity.tutorialGuid = 9;
    tutorialCity.mayor = "Hidden";
    rig.host.hidesMayorName[9] = true;
    cRZString mayor("Hidden");
    view->filter_tutorial_mayor_name(&tutorialCity, &mayor);
    CHECK(mayor.Strlen() == 0);
    rig.host.hidesMayorName[9] = false;
    mayor.AssignFromCString("Hidden");
    view->filter_tutorial_mayor_name(&tutorialCity, &mayor);
    CHECK(std::strcmp(mayor.ToChar(), "Hidden") == 0);
    view->ClearCities();
    image->Release();
}

void TestRenderTileBackdrop() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    cIGZBuffer* backdrop = MakeBuffer(8, 8);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) backdrop->SetPixel(x, y, static_cast<std::uint32_t>(x + y * 8));
    }
    view->SetBackdrop(backdrop);
    cIGZBuffer* destination = MakeBuffer(4, 4);
    view->SetScrollPos(3, 5);
    view->RenderTile(destination, 1, 2);
    // offset = -(scroll + tile) = (-4, -7): dest(x,y) = source((x + 4) % 8, (y + 7) % 8).
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const std::uint32_t expected = static_cast<std::uint32_t>(((x + 4) % 8) + ((y + 7) % 8) * 8);
            CHECK(destination->GetPixel(x, y) == expected);
        }
    }
    // Without a backdrop the tile is filled black.
    view->SetBackdrop(nullptr);
    destination->Fill(0xffff, nullptr);
    view->RenderTile(destination, 0, 0);
    CHECK(destination->GetPixel(0, 0) == 0);
    backdrop->Release();
    destination->Release();
}

void TestPointTransparency() {
    Rig rig;
    cSC4WinRegionView* view = rig.view;
    rig.host.cursorChild = nullptr;
    CHECK(view->is_point_transparent(5, 6));
    CHECK(rig.host.cursorCalls == 1);
    auto* child = new FakeWin(&rig.mgr);
    child->AddRef();
    rig.host.cursorChild = child;
    CHECK(!view->is_point_transparent(7, 8));
    CHECK(rig.host.cursorX == 7 && rig.host.cursorY == 8);
    child->Release();
}

}  // namespace

int main() {
    SetLanguageFormatter(&g_formatter);
    SetSLBufferHost(&g_bufferHost);
    TestConstructAndQueryInterface();
    TestInitAndShutdown();
    TestCityRectsAndHitTest();
    TestSelection();
    TestOptions();
    TestScrollPositionsTheSignpost();
    TestUpdateCityEffects();
    TestUpdateSignpostOccupied();
    TestUpdateSignpostTutorialGhost();
    TestUpdateSignpostDefaultsAndSkip();
    TestRenderTileBackdrop();
    TestPointTransparency();
    SetSLBufferHost(nullptr);
    SetLanguageFormatter(nullptr);
    if (failures == 0) std::printf("all tests passed\n");
    return failures == 0 ? 0 : 1;
}
