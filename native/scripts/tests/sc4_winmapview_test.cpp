// Unit test for cSC4WinMapView (src/sc4/WinMapView.cpp): the window lifecycle, QueryInterface, the message
// registrations, the parent-window panel switching, the view stack, the legend fill and the highlight merge.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fake_winmgr.h"
#include "gz/Font.h"
#include "gz/WinBtn.h"
#include "gz/WinFlatRect.h"
#include "gz/WinText.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/WinMapView.h"
#include "gz/Buffer.h"
#include "sc4/GrowthDeveloper.h"
#include "sc4/cSC4SimGrid.h"

using namespace sc4;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            ++g_failures;                                                                \
        }                                                                                \
    } while (0)

// A message server that only counts and records the notifications.
struct FakeMessageServer final : cIGZMessageServer2 {
    std::uint32_t refs = 1;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool MessageSend(cIGZMessage2*) override { return false; }
    bool MessagePost(cIGZMessage2*, bool) override { return false; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.emplace_back(target, type);
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.emplace_back(target, type);
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return false; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return false; }
    bool OnTick(std::uint32_t) override { return false; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return refs; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeEffect final : IWinMapViewEffect {
    std::uint32_t refs = 1;
    int places = 0, activates = 0;
    float position[3] = {0, 0, 0};
    float size = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) {
            delete this;
            return 0;
        }
        return refs;
    }
    void Place(const float p[3], float s) override {
        ++places;
        position[0] = p[0]; position[1] = p[1]; position[2] = p[2];
        size = s;
    }
    void Activate(bool) override { ++activates; }
};

struct FakeMarkedCells final : IWinMapViewMarkedCells {
    int* shutdownCounter = nullptr;
    int inits = 0, shutdowns = 0, clears = 0, added = 0;
    bool ignoreDepth = false, water = false, network = false;
    float cellX = 0, cellZ = 0;
    void Init() override { ++inits; }
    void Shutdown() override {
        ++shutdowns;
        if (shutdownCounter) ++*shutdownCounter;
    }
    void SetShouldIgnoreDepthBuffer(bool on) override { ignoreDepth = on; }
    void SetShouldUseWaterAltitude(bool on) override { water = on; }
    void SetShouldUseNetworkAltitude(bool on) override { network = on; }
    void ClearMarkedCells() override { ++clears; cells.clear(); }
    std::vector<WinMapCell> cells;
    std::vector<std::uint32_t> colors;
    void ClearCells() { cells.clear(); }
    void AddMarkedCells(const std::vector<WinMapCell>& more) override { ++added; cells.insert(cells.end(), more.begin(), more.end()); }
    void SetCellColors(const std::vector<std::uint32_t>& c) override { colors = c; }
    void SetCellSize(float x, float z) override { cellX = x; cellZ = z; }
};

struct FakeColorSink final : IWinMapViewColorSink {
    std::uint32_t baseColor = 0, baseFlags = 0;
    int resets = 0;
    std::map<int, std::uint32_t> colors;
    void SetBaseColor(std::uint32_t color, std::uint32_t flags) override { baseColor = color; baseFlags = flags; }
    void ResetColors() override { ++resets; colors.clear(); }
    void SetColor(int index, std::uint32_t rgb) override { colors[index] = rgb; }
    cIGZBuffer* overlay = nullptr;   // one reference while held
    int overlays = 0, refreshes = 0, width = 0, height = 0;
    void SetOverlay(cIGZBuffer* buffer) override {
        ++overlays;
        if (buffer) buffer->AddRef();
        if (overlay) overlay->Release();
        overlay = buffer;
    }
    void Refresh() override { ++refreshes; }
    bool MapSize(int& w, int& h) override { w = width; h = height; return width > 0; }
};

struct FakeHost final : IWinMapViewHost {
    FakeMessageServer server;
    bool noServer = false;
    FakeColorSink sink;
    FakeMarkedCells* marked = nullptr;
    std::vector<WinMapHighlight> highlights;
    std::uint32_t now = 0;
    std::uint32_t animationFlags = 0;
    int animationAdds = 0, animationRemoves = 0, notificationCalls = 0;
    int addMarked = 0, removeMarked = 0;
    int createEffects = 0;
    int markedShutdowns = 0;
    cRZString effectName;
    std::vector<std::uint32_t> colors, labels;
    std::uint32_t title = 0;
    bool legend = false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stringRequests;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> commands;
    int cityX = 0, cityZ = 0;
    std::vector<std::pair<int, int>> roads;
    int rotation = 0;
    cIGZBuffer* decal = nullptr;

    std::uint32_t NowMilliseconds() override { return now; }
    cIGZMessageServer2* MessageServer2() override { return noServer ? nullptr : &server; }
    void AddAnimationTarget(cIGZMessageTarget2*) override { ++animationAdds; }
    void RemoveAnimationTarget(cIGZMessageTarget2*) override { ++animationRemoves; }
    void SetAnimationNotifications(cIGZMessageTarget2*, std::uint32_t flags) override {
        ++notificationCalls;
        animationFlags = flags;
    }
    void CollectHighlights(std::uint32_t, std::vector<WinMapHighlight>& out) override { out = highlights; }
    IWinMapViewMarkedCells* CreateMarkedCells() override { return marked; }
    bool AddMarkedCells(IWinMapViewMarkedCells*) override { ++addMarked; return true; }
    bool RemoveMarkedCells(IWinMapViewMarkedCells*) override { ++removeMarked; return true; }
    bool CreateEffect(const cRZString& name, IWinMapViewEffect** out) override {
        ++createEffects;
        effectName = name;
        auto* effect = new FakeEffect();
        effect->AddRef();
        *out = effect;
        return true;
    }
    bool LoadLegendData(std::uint32_t, std::vector<std::uint32_t>& c, std::vector<std::uint32_t>& l,
                        std::uint32_t& t) override {
        if (!legend) return false;
        c = colors;
        l = labels;
        t = title;
        return true;
    }
    void LoadString(std::uint32_t group, std::uint32_t instance, cRZString& out) override {
        stringRequests.emplace_back(group, instance);
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "s%08x", instance);
        out = cRZString(buffer);
    }
    cIGZFont* LegendFont() override { return nullptr; }
    IWinMapViewColorSink* ViewColorSink(cIGZWin*) override { return &sink; }
    bool OccupantLotPosition(cISC4Occupant*, float[3]) override { return false; }
    bool IsTransportationCell(int x, int z) override {
        for (auto& road : roads) if (road.first == x && road.second == z) return true;
        return false;
    }
    int CitySizeXInCells() override { return cityX; }
    int CitySizeZInCells() override { return cityZ; }
    cIGZBuffer* CreatePotholeDecal() override { return decal; }
    int MapRotation() override { return rotation; }
    void SendCommand(std::uint32_t command, std::uint32_t a, std::uint32_t b) override {
        commands.emplace_back(command, a | b);
    }
    bool filter = true;
    bool FilterUICommand(const cGZMessage&) override { return filter; }
    // SetDataView / Update
    bool hasExemplar = false;
    WinMapViewExemplar exemplar;
    std::vector<std::vector<std::uint8_t>> grid8;
    std::vector<int> gridRequests;
    bool LoadDataViewExemplar(std::uint32_t, WinMapViewExemplar& out) override {
        if (hasExemplar) out = exemplar;
        return hasExemplar;
    }
    // The stops are taken as the palette itself (the real ramp interpolation is nSC4UI's, tested on its own).
    void InterpolateRamp(std::uint32_t* out, int count, const std::uint32_t* stops, int stopCount) override {
        for (int i = 0; i < count; ++i) out[i] = i < stopCount ? stops[i] : 0;
    }
    cIGZBuffer* CreateMapBuffer() override {
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        return buffer;
    }
    bool SimulatorGrid(int source, int, WinMapGrid& out) override {
        gridRequests.push_back(source);
        if (source != kGridPower || grid8.empty()) return false;
        out.u8 = &grid8;
        out.cellSizeX = out.cellSizeZ = 64.0f;
        return true;
    }
};

// A minimal occupant: only the highlight notification and the position matter to the view.
struct FakeOccupant final : cISC4Occupant {
    std::uint32_t refs = 1;
    int highlight = 0;
    int highlightPosts = 0;
    float position[3] = {1.0f, 2.0f, 3.0f};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool IsInitialized() const override { return true; }
    cISCPropertyHolder* AsPropertyHolder() override { return nullptr; }
    std::uint32_t GetType() const override { return 0; }
    void GetPosition(float* xyz) override { xyz[0] = position[0]; xyz[1] = position[1]; xyz[2] = position[2]; }
    void SetPosition(const float*) override {}
    void GetBoundingBox(float*, float*) override {}
    SC4CellRect cells;
    void GetBoundingCityCells(SC4CellRect& out) override { out = cells; }
    void SetRemovalFlags(int) override {}
    void UnsetRemovalFlags(int) override {}
    bool CanRemove(int) override { return true; }
    void PostOccupantMessage(std::uint32_t, std::uint32_t) override {}
    std::uint32_t GetHighlight() const override { return static_cast<std::uint32_t>(highlight); }
    void SetHighlight(int value, bool postNow) override {
        highlight = value;
        if (postNow) ++highlightPosts;
    }
    void SetVisibility(bool, bool) override {}
    cISC43DPlaceableObject* GetPlaceableObject() const override { return nullptr; }
    cISC43DPlaceableObject* GetOrCreatePlaceableObject() override { return nullptr; }
    void SetPlaceableObject(cISC43DPlaceableObject*) override {}
    bool IsOccupantGroup(std::uint32_t) const override { return false; }
    void AddOccupantGroup(std::uint32_t) override {}
    bool GetOccupantGroups(std::set<std::uint32_t>&) const override { return false; }
    bool GetOccupantManagerBBox(std::uint8_t*) const override { return false; }
    void SetOccupantManagerBBox(const std::uint8_t*) override {}
    bool GetLotTag(std::uint32_t&) const override { return false; }
    void SetLotTag(std::uint32_t) override {}
    void SetFlag(std::uint32_t) override {}
    void SetAllFlags(std::uint32_t) override {}
    void ClearFlag(std::uint32_t) override {}
    bool IsFlagSet(std::uint32_t) override { return false; }
    std::uint32_t GetFlags() override { return 0; }
};

// The city geometry cSC4SimGrid asks for.
struct FakeSimCity final : ISimGridCity {
    std::int32_t x = 64, z = 64;
    float cellX = 16.0f, cellZ = 16.0f;
    std::int32_t CitySizeXInCells() const override { return x; }
    std::int32_t CitySizeZInCells() const override { return z; }
    float CellSizeXMeters() const override { return cellX; }
    float CellSizeZMeters() const override { return cellZ; }
};


// A CPU buffer with a 4-byte-per-pixel surface, enough for the rotated alpha blit test.
struct FakeBuffer final : cIGZBuffer {
    std::uint32_t refs = 1;
    int width = 0, height = 0;
    cRZRect area{};
    std::vector<std::uint32_t> pixels;
    int locks = 0;
    FakeBuffer(int w, int h, std::uint32_t fill) : width(w), height(h), pixels(static_cast<std::size_t>(w * h), fill) {
        area = cRZRect{0, 0, w, h};
    }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) { delete this; return 0; }
        return refs;
    }
    bool Init(std::uint32_t, std::uint32_t, cGZBufferColorType) override { return true; }
    bool Shutdown() override { return true; }
    bool Uninitialize() override { return true; }
    bool Lock(eLockFlags) override { ++locks; return true; }
    bool Unlock(eLockFlags) override { --locks; return true; }
    bool IsLocked() const override { return locks != 0; }
    std::uint32_t Width() const override { return static_cast<std::uint32_t>(width); }
    std::uint32_t Height() const override { return static_cast<std::uint32_t>(height); }
    const cRZRect* GetBufferArea() const override { return &area; }
    void GetBufferArea(cRZRect& out) const override { out = area; }
    std::uint32_t GetMemoryUsage() const override { return 0; }
    cGZBufferColorType GetColorType() const override { return cGZBufferColorType{}; }
    void GetColorType(cGZBufferColorType& out) const override { out = cGZBufferColorType{}; }
    std::uint32_t GetBitsPerPixel() const override { return 32; }
    std::uint32_t GetBytesPerPixel() const override { return 4; }
    bool Fill(std::uint32_t, const cRZRect&, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, int, int, int, int, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, const cRZRect*) override { return true; }
    std::uint32_t GetPixel(std::uint32_t x, std::uint32_t y) const override {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
    void SetPixel(std::uint32_t x, std::uint32_t y, std::uint32_t color) override {
        pixels[static_cast<std::size_t>(y) * width + x] = color;
    }
    void SetTransparency(std::uint32_t) override {}
    bool GetTransparency() const override { return false; }
    bool IsPixelTransparent(std::uint32_t, std::uint32_t) override { return false; }
    bool GetTransparentColor(std::uint32_t&) const override { return false; }
    void RemoveTransparency() override {}
    bool DrawLine(int, int, int, int, int, std::uint32_t) override { return false; }
    bool Blt(cIGZBuffer*, const cRZRect&, const cRZRect&, const cRZRect*) override { return false; }
    std::uint32_t ConvertRGBValueToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    bool ConvertNativeValueToRGB(std::uint32_t v, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = static_cast<std::uint8_t>(v >> 16); g = static_cast<std::uint8_t>(v >> 8); b = static_cast<std::uint8_t>(v);
        return true;
    }
    std::uint32_t ConvertRGBAValueToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) override {
        return (std::uint32_t(a) << 24) | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    bool ConvertNativeValueToRGBA(std::uint32_t v, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b,
                                  std::uint8_t& a) override {
        a = static_cast<std::uint8_t>(v >> 24);
        return ConvertNativeValueToRGB(v, r, g, b);
    }
    void* GetColorSurfaceBits() override { return pixels.data(); }
    std::uint32_t GetColorSurfaceStride() override { return static_cast<std::uint32_t>(width * 4); }
    std::uint32_t GetChangeCounter() override { return 0; }
    cIGZBufferHardwareCache* GetHardwareCache() override { return nullptr; }
    bool SetHardwareCache(cIGZBufferHardwareCache*) override { return false; }
    bool IsReady() override { return true; }
};

FakeSimCity g_simCity;

// The two developer grids UpdateOccupancyMap reads.
struct FakeGrowthDeveloper final : cISC4GrowthDeveloper {
    mutable cSC4SimGrid<std::uint16_t> capacity;
    mutable cSC4SimGrid<std::int16_t> population;
    std::uint32_t refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init(cISCPropertyHolder*) override { return true; }
    bool Shutdown() override { return true; }
    std::int32_t GetType() const override { return 0; }
    std::int32_t GetSubType() const override { return 0; }
    std::int32_t GetPurpose() const override { return 0; }
    std::int32_t GetWealth() const override { return 0; }
    void GetConstructionInfo(ConstructionProfile&) const override {}
    bool GrowthAndDeclineListsAreValid() const override { return false; }
    const std::vector<GrowthItem>& GetGrowthList() const override { return growth_; }
    const std::vector<GrowthItem>& GetDeclineList() const override { return growth_; }
    cSC4SimGrid<std::int16_t>* GetDesirabilityMap() const override { return nullptr; }
    cSC4SimGrid<std::uint16_t>* GetCapacityMap() const override { return &capacity; }
    cSC4SimGrid<std::int16_t>* GetPopulationMap() const override { return &population; }
    cSC4SimGrid<std::uint16_t>* GetMaxCapacityMap() const override { return nullptr; }
    std::int32_t GetTotalCapacity() const override { return 0; }
    IGrowthDemandApi* GetSourceDemand() const override { return nullptr; }
    std::uint8_t GetPowerSupplyCap() const override { return 0; }
    std::uint8_t GetWaterSupplyCap() const override { return 0; }
    std::uint8_t GetFireCoverageCap() const override { return 0; }
    std::uint8_t GetParkCap() const override { return 0; }
    std::int32_t GetDesirabilityThreshold() const override { return 0; }
    float GetAbandonmentThreshold(std::int32_t) const override { return 0.0f; }
    std::uint32_t GetMonopolyUse() const override { return 0; }
    const std::map<std::uint32_t, SC4Percentage>& GetStylePercentages() const override { return styles_; }
    void StartMapUpdate() override {}
    bool IsMapUpdateFinished() const override { return true; }
    std::int32_t EstimateDesirability(int, int) const override { return 0; }
    void GetSubjectiveThresholds(int, int& low, int& high) const override { low = high = 0; }
    bool DoMessage(cIGZMessage2*) override { return false; }
    bool Write(cIGZOStream&) const override { return false; }
    bool Read(cIGZIStream&) override { return false; }
    std::uint32_t GetGZCLSID() const override { return 0; }
    std::vector<GrowthItem> growth_;
    std::map<std::uint32_t, SC4Percentage> styles_;
};

constexpr std::uint32_t kPanelIds[] = {0x8a2871b1, 0x8a2871b2, 0x8a2871c3, 0x8a2871c4, 0xaa32bcd4,
                                       0xaa32bcd5, 0xcb72f247, 0x8100, 0x8101, 0x4200, 0x8a2871d6, 0x8a2871c5};

struct Fixture {
    FakeMgr mgr;
    FakeWinGraphics graphics;
    FakeHost host;
    cGZWin* parent = nullptr;
    cSC4WinMapView* view = nullptr;
    std::vector<gz::cGZWinText*> texts;
    std::vector<gz::cGZWinFlatRect*> flats;
    std::vector<gz::cGZWinBtn*> buttons;

    Fixture() {
        SetWinGraphics(&graphics);
        SetSimGridCity(&g_simCity);
        SetWinMapViewHost(&host);
        parent = new cGZWin(&mgr);
        parent->SetID(0x1000);
        for (std::uint32_t id : kPanelIds) {
            auto* child = new cGZWin(&mgr);
            child->SetID(id);
            child->AddRef();   // the parent's reference (the controls are released by ChildDeleteAll)
            parent->ChildAdd(child);
        }
        auto* title = new gz::cGZWinText(&mgr);
        title->SetID(0x8a909d6a);
        title->AddRef();
        parent->ChildAdd(title);
        texts.push_back(title);
        for (std::uint32_t id : {0x8a909e00u, 0x8a909e01u}) {
            auto* text = new gz::cGZWinText(&mgr);
            text->SetID(id);
            text->AddRef();
            parent->ChildAdd(text);
            texts.push_back(text);
        }
        for (std::uint32_t id : {0x8a909e10u, 0x8a909e11u}) {
            auto* flat = new gz::cGZWinFlatRect(&mgr);
            flat->SetID(id);
            flat->AddRef();
            parent->ChildAdd(flat);
            flats.push_back(flat);
        }
        for (std::uint32_t id : {0x5001u, 0x6001u, 0x8001u, 0x8100u, 0x8003u}) {
            auto* button = new gz::cGZWinBtn(&mgr);
            button->SetID(id);
            button->AddRef();
            parent->ChildAdd(button);
            buttons.push_back(button);
        }
        host.marked = new FakeMarkedCells();
        host.marked->shutdownCounter = &host.markedShutdowns;
        view = new cSC4WinMapView(&mgr);
        // The factory's references: cGZWinGen::Shutdown releases one at the end, so keep two in hand.
        view->AddRef();
        view->AddRef();
        view->SetID(0x2ba6bb98);
        parent->ChildAdd(view);
    }
    ~Fixture() {
        view->Shutdown();
        parent->ChildRemove(view);   // the view keeps its factory references; remove it before deleting
        delete view;
        // ChildDeleteAll releases every remaining child (a cGZWin has no outstanding reference, so it is deleted).
        parent->ChildDeleteAll();
        delete parent;
        // The view owns the marked-cell view (created through the host) and deletes it in Shutdown.
        SetWinMapViewHost(nullptr);
    }
    cGZWin* child(std::uint32_t id) { return static_cast<cGZWin*>(parent->GetChildWindowFromID(id)); }
    gz::cGZWinText* text(std::uint32_t id) {
        for (auto* t : texts) if (t->GetID() == id) return t;
        return nullptr;
    }
    gz::cGZWinFlatRect* flat(std::uint32_t id) {
        for (auto* f : flats) if (f->GetID() == id) return f;
        return nullptr;
    }
    gz::cGZWinBtn* button(std::uint32_t id) {
        for (auto* b : buttons) if (b->GetID() == id) return b;
        return nullptr;
    }
};

void TestConstructionAndQueryInterface() {
    FakeMgr mgr;
    auto* view = new cSC4WinMapView(&mgr);
    CHECK(view->dataView() == 0);
    CHECK(view->baseView() == 0);
    CHECK(view->viewStack().empty());
    CHECK(view->desirabilityView() == 99);
    CHECK(view->trafficView() == 0x4c);
    CHECK(view->viewView() == 0);
    CHECK(view->highlightView() == 0);
    CHECK(view->effectName().Strlen() == 0);
    CHECK(view->messageServer() == nullptr);
    CHECK(view->markedCells() == nullptr);
    CHECK(!view->isDirty());
    CHECK(!view->viewActive());

    CHECK(view->AddRef() == 1);   // the factory's reference (the constructor starts at 0)
    cIGZWin* asWindow = nullptr;
    CHECK(view->QueryInterface(0x22ba0121, reinterpret_cast<void**>(&asWindow)));
    CHECK(asWindow == static_cast<cIGZWin*>(view));
    CHECK(view->Release() == 1);   // QI added and we released one reference

    cISC4ViewManager* asViewManager = nullptr;
    CHECK(view->QueryInterface(kGZIID_cISC4ViewManager, reinterpret_cast<void**>(&asViewManager)));
    CHECK(asViewManager == static_cast<cISC4ViewManager*>(view));
    CHECK(view->Release() == 1);

    cIGZMessageTarget2* asTarget = nullptr;
    CHECK(view->QueryInterface(kGZIID_cIGZMessageTarget2_, reinterpret_cast<void**>(&asTarget)));
    CHECK(asTarget == static_cast<cIGZMessageTarget2*>(view));
    CHECK(view->Release() == 1);

    delete view;
}

void TestInitAndShutdown() {
    Fixture f;
    CHECK(f.view->Init());
    CHECK(f.host.server.added.size() == 3);
    CHECK(f.view->markedCells() == f.host.marked);
    CHECK(f.host.marked->inits == 1);
    CHECK(f.host.marked->ignoreDepth);
    CHECK(f.host.marked->water);
    CHECK(f.view->messageServer() == &f.host.server);
    CHECK(f.host.server.refs == 2);

    // Started even when the message server is missing: a second Init does nothing.
    FakeMgr mgr;
    FakeHost bare;
    bare.noServer = true;
    SetWinMapViewHost(&bare);
    auto* view = new cSC4WinMapView(&mgr);
    CHECK(!view->Init());
    CHECK(view->messageServer() == nullptr);
    SetWinMapViewHost(&f.host);
    view->AddRef();
    delete view;

    f.view->Shutdown();
    // The seven panels of the parent are destroyed and the three notifications removed.
    CHECK(f.host.server.removed.size() >= 3);
    CHECK(f.host.markedShutdowns == 1);
    CHECK(f.view->markedCells() == nullptr);
    CHECK(f.view->messageServer() == nullptr);
}

void TestMessageTarget() {
    Fixture f;
    f.view->Init();
    f.host.now = 5000;
    // The activate/deactivate notifications drive the animation target.
    auto* activate = new cRZMessage2Standard();
    activate->SetType(kWinMapMsgViewActivate);
    CHECK(f.view->DoMessage(static_cast<cIGZMessage2*>(activate)));
    CHECK(f.view->viewActive());
    CHECK(f.host.animationAdds == 1);
    auto* deactivate = new cRZMessage2Standard();
    deactivate->SetType(kWinMapMsgViewDeactivate);
    CHECK(f.view->DoMessage(static_cast<cIGZMessage2*>(deactivate)));
    CHECK(!f.view->viewActive());
    CHECK(f.host.animationRemoves == 1);

    // The moved notification marks the view dirty when the two payload rectangles differ in their second int.
    f.view->Init();   // already started, no-op
    auto* moved = new cRZMessage2Standard();
    moved->SetType(kWinMapMsgViewMoved);
    int rectA[4] = {0, 7, 10, 10};
    int rectB[4] = {0, 8, 10, 10};
    moved->SetVoid1(rectA);
    moved->SetVoid2(rectB);
    CHECK(f.view->DoMessage(static_cast<cIGZMessage2*>(moved)));
    CHECK(f.view->isDirty());
    activate->Release();
    deactivate->Release();
    moved->Release();
}

void TestLegend() {
    Fixture f;
    f.view->Init();
    f.host.legend = true;
    f.host.colors = {0x112233, 0x445566, 0};   // the third swatch stays hidden
    f.host.labels = {0x100, 0x101};
    f.host.title = 0x102;
    f.view->DisplayLegend(5);

    CHECK(f.text(0x8a909d6a) != nullptr);
    CHECK(std::strcmp(f.text(0x8a909d6a)->GetCaption().Data(), "s00000102") == 0);
    CHECK(f.flat(0x8a909e10)->GetFlag(kWinVisible));
    CHECK(f.flat(0x8a909e11)->GetFlag(kWinVisible));
    CHECK(f.text(0x8a909e00)->GetFlag(kWinVisible));
    CHECK(f.text(0x8a909e01)->GetFlag(kWinVisible));
    CHECK(std::strcmp(f.text(0x8a909e00)->GetCaption().Data(), "s00000100") == 0);
    CHECK(std::strcmp(f.text(0x8a909e01)->GetCaption().Data(), "s00000101") == 0);
    // FakeWinGraphics' native colour is 0xff000000 | rgb.
    CHECK(f.flat(0x8a909e10)->GetFillColor() == 0xff112233u);
    CHECK(f.flat(0x8a909e11)->GetFillColor() == 0xff445566u);
    // The swatch area starts at (0x173, 0x3d) and the label at (0x116, 0x18).
    CHECK(f.flat(0x8a909e10)->GetArea().left == 0x173);
    CHECK(f.flat(0x8a909e10)->GetArea().top == 0x3d);

    // View 0: the title only and the traffic legend hidden.
    f.view->DisplayLegend(0);
    CHECK(std::strcmp(f.text(0x8a909d6a)->GetCaption().Data(), "s2a5e854f") == 0);
    CHECK(!f.text(0x8a909e00)->GetFlag(kWinVisible));
    CHECK(!f.text(0x8a909e01)->GetFlag(kWinVisible));
}

void TestWindowSwitching() {
    Fixture f;
    f.view->Init();
    f.view->ShowWindow();
    CHECK(f.view->IsVisible());
    CHECK(f.child(0x8a2871b2)->GetFlag(kWinVisible));
    CHECK(f.child(0x8a2871b1)->GetFlag(kWinVisible));   // the legend panel was not visible yet
    CHECK(f.view->desirabilityView() == 0x28);          // the default view mode is chosen once
    CHECK(f.button(0x5001)->IsOn());
    CHECK(f.button(0x6001)->IsOn());
    CHECK(f.button(0x8001)->IsOn());
    CHECK(f.button(0x8100)->IsOn());
    CHECK(f.button(0x8003)->IsOn());
    // Every control in the 82-entry list gets the view as its notification target; 0x4a32ca92 is one of them.
    CHECK(f.child(0x8a2871d6) != nullptr);

    f.view->HideWindow();
    CHECK(!f.view->IsVisible());
    CHECK(!f.child(0x4200)->GetFlag(kWinVisible));
    CHECK(!f.child(0x8a2871b1)->GetFlag(kWinVisible));
    CHECK(!f.child(0x8a2871b2)->GetFlag(kWinVisible));
}

void TestViewStack() {
    Fixture f;
    f.view->Init();
    f.view->ShowWindow();
    f.view->SetBaseView(0x18);
    CHECK(f.view->baseView() == 0x18);
    f.view->PushView(5);
    f.view->PushView(7);
    CHECK(f.view->viewStack().size() == 2);
    CHECK(f.view->viewStack().back() == 7);
    f.view->PopView(5);
    CHECK(f.view->viewStack().size() == 1);
    CHECK(f.view->viewStack().front() == 7);
    f.view->PopView(5);   // not present: no change
    CHECK(f.view->viewStack().size() == 1);
    f.view->PopView(7);
    CHECK(f.view->viewStack().empty());
}

void TestCommandRouting() {
    Fixture f;
    f.view->Init();
    f.view->ShowWindow();
    f.host.legend = true;
    f.host.colors = {0x101010};
    f.host.labels = {0x200};
    f.host.title = 0x201;

    cGZMessage command{};
    command.type = kGZMsgCommand;
    command.param1 = 0x287259f6;
    command.param2 = 0x6001;   // the first desirability button
    f.view->DoMessage(command);   // the answer is the window base's
    CHECK(f.view->desirabilityView() == 0x28);
    CHECK(f.button(0x6001)->IsOn());

    // The traffic mode button switches the view and excludes the other traffic buttons.
    command.param2 = 0x8006;
    f.view->DoMessage(command);
    CHECK(f.view->trafficView() == 0x4f);
    CHECK(f.button(0x8003)->IsOn() == false);

    // The route overlay buttons only toggle the flag and reset the view.
    command.param2 = 0x8101;
    f.view->DoMessage(command);
    CHECK(f.button(0x8100)->IsOn() == false);

    // The refresh command reaches the command server.
    command.param2 = 0x8a2871d5;
    f.view->DoMessage(command);
    CHECK(f.host.commands.size() == 1);
    CHECK(f.host.commands[0].first == 0x6a935cf1);

    // A command of another family is forwarded to the window base (and does not touch the view state).
    command.param1 = 0x12345678;
    const std::uint32_t before = f.view->desirabilityView();
    f.view->DoMessage(command);
    CHECK(f.view->desirabilityView() == before);

    // The tutorial filter's veto answers true without touching the view.
    f.host.filter = false;
    command.param1 = 0x287259f6;
    command.param2 = 0x6002;
    CHECK(f.view->DoMessage(command));
    CHECK(f.view->desirabilityView() == before);

    // HookUpdateMessages asks the animation manager for the view's flags and registers the view's message; the
    // data view is 0 here, so the default animation-tick message is watched.
    f.view->HookUpdateMessages();
    CHECK(f.host.notificationCalls == 1);
    CHECK(f.host.animationFlags == 0);
    CHECK(f.view->watchedMessages().size() == 1);
    CHECK(f.view->watchedMessages()[0] == kWinMapMsgAnimationTick);
    bool registered = false;
    for (const auto& entry : f.host.server.added)
        if (entry.second == kWinMapMsgAnimationTick) registered = true;
    CHECK(registered);
    // A watched message marks the view dirty.
    auto* tick = new cRZMessage2Standard();
    tick->SetType(kWinMapMsgAnimationTick);
    f.view->DoMessage(static_cast<cIGZMessage2*>(tick));
    CHECK(f.view->isDirty());
    tick->Release();
}

void TestHighlights() {
    Fixture f;
    f.view->Init();
    // The crime hot spot: no occupant, a position and a scale.
    WinMapHighlight spot;
    spot.occupant = nullptr;
    spot.scale = 100.0f;
    spot.x = 11.0f;
    spot.y = 22.0f;
    spot.z = 33.0f;
    f.host.highlights = {spot};
    CHECK(f.view->UpdateHighlights());
    CHECK(f.view->activeHighlights().size() == 1);
    CHECK(f.view->newHighlights().empty());
    const auto& info = *f.view->activeHighlights().begin();
    CHECK(info.occupant == nullptr);
    CHECK(info.scale == 100.0f);
    CHECK(info.x == 11.0f && info.y == 22.0f && info.z == 33.0f);
    CHECK(f.host.createEffects == 0);   // the effect name is empty until SetDataView runs

    // An occupant highlight: the occupant is told it is highlighted, its position is kept and the record moves to
    // the active set on the next merge.
    auto* occupant = new FakeOccupant();
    WinMapHighlight occupantHighlight;
    occupantHighlight.occupant = occupant;
    occupantHighlight.scale = 0.5f;
    f.host.highlights = {occupantHighlight};
    CHECK(f.view->UpdateHighlights());
    CHECK(occupant->highlight == 1);
    // A null occupant (the crime spot) sorts before every real occupant, so the spot is dropped when an occupant
    // highlight appears: the active set holds the occupant only.
    CHECK(f.view->activeHighlights().size() == 1);
    CHECK(f.view->newHighlights().empty());
    bool foundOccupant = false;
    for (const auto& entry : f.view->activeHighlights()) {
        if (entry.occupant == occupant) {
            foundOccupant = true;
            CHECK(entry.x == 1.0f && entry.y == 2.0f && entry.z == 3.0f);
        }
    }
    CHECK(foundOccupant);

    // Nothing highlighted any more: the record is removed and the occupant is unhighlighted.
    f.host.highlights.clear();
    CHECK(f.view->UpdateHighlights());
    CHECK(f.view->activeHighlights().empty());
    CHECK(occupant->highlight == 0);
    delete occupant;

    // The crime spot again, twice: the second merge refreshes the null-occupant record in place.
    f.host.highlights = {spot};
    CHECK(f.view->UpdateHighlights());
    CHECK(f.view->activeHighlights().size() == 1);
    CHECK(f.view->UpdateHighlights());
    CHECK(f.view->activeHighlights().size() == 1);
}

void TestOccupancyMap() {
    Fixture f;
    f.view->Init();
    FakeGrowthDeveloper developer;
    // Both grids at cell resolution: the occupancy grid copies the population and divides by the capacity.
    developer.capacity.SetTractSize(1);
    developer.population.SetTractSize(1);
    for (auto& column : developer.capacity.GetGridData())
        for (auto& value : column) value = 5;
    for (auto& column : developer.population.GetGridData())
        for (auto& value : column) value = 10;
    f.view->UpdateOccupancyMap(&developer);
    CHECK(f.view->occupancyGrid() != nullptr);
    CHECK(f.view->occupancyGrid()->GetTractSize() == 1);
    CHECK(f.view->occupancyGrid()->GetInstanceID() == 0x89d5c471);
    CHECK(f.view->occupancyGrid()->GetCellValue(0, 0) == 200);   // 10 * 100 / 5
    CHECK(f.view->occupancyGrid()->GetCellValue(63, 63) == 200);

    // A coarser population grid is expanded: each 2x2 block gets the tract's value before the division.
    developer.population.SetTractSize(2);
    developer.population.GetGridData()[0][0] = 30;
    developer.population.GetGridData()[1][1] = 30;
    f.view->UpdateOccupancyMap(&developer);
    CHECK(f.view->occupancyGrid()->GetCellValue(0, 0) == 600);   // 30 * 100 / 5
    CHECK(f.view->occupancyGrid()->GetCellValue(3, 3) == 600);
    CHECK(f.view->occupancyGrid()->GetCellValue(2, 2) == 600);   // from tract (1,1)

    // The occupancy grid is owned by the view and released in Shutdown.
    f.view->Shutdown();
    CHECK(f.view->occupancyGrid() == nullptr);
}


void TestRotatedAlphaBlt() {
    // 4x4 source, opaque distinct colours; 8x8 black destination.
    auto* source = new FakeBuffer(4, 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            source->SetPixel(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                             0xff000000u | (std::uint32_t(x * 0x40) << 16) | (std::uint32_t(y * 0x40) << 8) | 0x20);
    auto* destination = new FakeBuffer(8, 8, 0xff000000u);

    SC4MapViewRotatedAlphaBlt(destination, source, 4, 4, 0);   // centred, unrotated
    // Destination (2,2) is the source's (0,0): the red channel is close to 0x20 and blue is exact.
    const std::uint32_t pixel = destination->GetPixel(2, 2);
    CHECK(((pixel >> 24) & 0xff) == 0xff);
    CHECK(std::abs(static_cast<int>((pixel >> 16) & 0xff) - 0x00) <= 1);
    CHECK(((pixel >> 8) & 0xff) == 0x00);
    // Destination (5,5) is the source's (3,3): red 0xc0, green 0xc0.
    const std::uint32_t corner = destination->GetPixel(5, 5);
    CHECK(std::abs(static_cast<int>((corner >> 16) & 0xff) - 0xc0) <= 1);
    CHECK(std::abs(static_cast<int>((corner >> 8) & 0xff) - 0xc0) <= 1);
    CHECK(!source->IsLocked() && !destination->IsLocked());

    // Rotation 1 maps destination (2,2) to the source's (3,0): red 0xc0, green 0x00.
    auto* rotated = new FakeBuffer(8, 8, 0xff000000u);
    SC4MapViewRotatedAlphaBlt(rotated, source, 4, 4, 1);
    const std::uint32_t rotatedPixel = rotated->GetPixel(2, 2);
    CHECK(std::abs(static_cast<int>((rotatedPixel >> 16) & 0xff) - 0xc0) <= 1);
    CHECK(std::abs(static_cast<int>((rotatedPixel >> 8) & 0xff) - 0x00) <= 1);

    // Rotation 2 maps destination (2,2) to the source's (3,3) and (5,5) back to (0,0).
    auto* flipped = new FakeBuffer(8, 8, 0xff000000u);
    SC4MapViewRotatedAlphaBlt(flipped, source, 4, 4, 2);
    const std::uint32_t flippedCorner = flipped->GetPixel(2, 2);
    CHECK(std::abs(static_cast<int>((flippedCorner >> 16) & 0xff) - 0xc0) <= 1);
    CHECK(std::abs(static_cast<int>((flippedCorner >> 8) & 0xff) - 0xc0) <= 1);
    const std::uint32_t flippedOrigin = flipped->GetPixel(5, 5);
    CHECK(std::abs(static_cast<int>((flippedOrigin >> 16) & 0xff) - 0x00) <= 1);
    CHECK(std::abs(static_cast<int>((flippedOrigin >> 8) & 0xff) - 0x00) <= 1);

    // Rotation 3 maps destination (2,2) to the source's (0,3) and (5,5) to (3,0).
    auto* turned = new FakeBuffer(8, 8, 0xff000000u);
    SC4MapViewRotatedAlphaBlt(turned, source, 4, 4, 3);
    const std::uint32_t turnedPixel = turned->GetPixel(2, 2);
    CHECK(std::abs(static_cast<int>((turnedPixel >> 16) & 0xff) - 0x00) <= 1);
    CHECK(std::abs(static_cast<int>((turnedPixel >> 8) & 0xff) - 0xc0) <= 1);
    const std::uint32_t turnedCorner = turned->GetPixel(5, 5);
    CHECK(std::abs(static_cast<int>((turnedCorner >> 16) & 0xff) - 0xc0) <= 1);
    CHECK(std::abs(static_cast<int>((turnedCorner >> 8) & 0xff) - 0x00) <= 1);
    turned->Release();
    flipped->Release();

    // A destination rectangle that is fully outside the blit area is untouched.
    auto* outside = new FakeBuffer(8, 8, 0xff000000u);
    SC4MapViewRotatedAlphaBlt(outside, source, -20, -20, 0);
    CHECK(outside->GetPixel(0, 0) == 0xff000000u);
    outside->Release();
    rotated->Release();
    destination->Release();
    source->Release();
}

void TestDisplayConverters() {
    Fixture f;
    f.view->Init();
    // The converters run their arithmetic and hand the byte grid to the (partial) Uint8 display; they must not
    // touch the buffer or crash for empty and non-empty grids.
    std::vector<std::vector<std::uint16_t>> u16 = {{0, 0x1000, 0xffff}, {0x8000, 0xfffe, 1}};
    f.view->DisplayStandardUint16GridScaled(u16, nullptr, 1.0f, 1.0f);
    std::vector<std::vector<std::int16_t>> s16 = {{-32768, -0x81, 0}, {0x7f, 0x80, 32767}};
    f.view->DisplayStandardSint16GridScaled(s16, nullptr, 1.0f, 1.0f);
    std::vector<std::vector<float>> f32 = {{-1.0f, 0.0f, 1.0f}, {2.5f, 300.0f, -300.0f}};
    f.view->DisplayStandardFloat32GridAsUint8(f32, nullptr, 1.0f, 1.0f);
    std::vector<std::vector<std::uint8_t>> u8 = {{0, 1}, {2, 3}};
    f.view->DisplayStandardUint8Grid(u8, nullptr, 1.0f, 1.0f);
    std::vector<std::vector<std::int8_t>> s8 = {{-1, 0}, {1, 2}};
    f.view->DisplayStandardSint8Grid(s8, nullptr, 1.0f, 1.0f);
    CHECK(f.host.addMarked == 0);
}

void TestInitWindow() {
    Fixture f;
    f.view->Init();
    f.view->InitWindow();
    CHECK(f.host.sink.baseColor == 0xffffffff);
    CHECK(f.host.sink.resets == 1);
    CHECK(f.host.sink.colors.size() == 9);
    CHECK(f.host.sink.colors[1] == 0x00ff00);
    CHECK(f.host.sink.colors[9] == 0xbc9f37);
}

void TestPotholeDecals() {
    Fixture f;
    f.view->Init();
    f.host.cityX = 2;
    f.host.cityZ = 2;
    f.host.roads = {{1, 0}};
    f.view->DrawTransportationPotholeDecals(nullptr);   // no buffer: nothing happens
    CHECK(f.host.decal == nullptr);
}

}  // namespace

// SetDataView + Update (i386 0x0027fc98 / 0x0027db2e) + DisplayStandardUint8Grid / MakeMapColorBuffer: the view's grid is
// drawn into a new buffer in blocks of the power-of-two size that covers the map, [x][z] -> pixel (x, z), in the
// premultiplied palette with red and blue back in place, and handed to the map as its overlay.
void TestDataViewImage() {
    Fixture f;
    f.view->Init();
    auto* activate = new cRZMessage2Standard();
    activate->SetType(kWinMapMsgViewActivate);
    f.view->DoMessage(static_cast<cIGZMessage2*>(activate));
    f.host.sink.width = 16;   // a 4-cell grid -> blocks of 4
    f.host.sink.height = 16;
    f.host.hasExemplar = true;
    f.host.exemplar.hasDataView = true;
    f.host.exemplar.dataView = 7;
    f.host.exemplar.hasRamp = true;
    std::vector<std::uint32_t> ramp(16);
    for (std::uint32_t v = 0; v < 16; ++v) ramp[v] = 0xff000000u | ((v * 8) << 16) | ((v * 4) << 8) | (0x10 + v);   // alpha 0xff: premultiply keeps them
    f.host.exemplar.ramp = ramp;
    f.host.grid8.assign(4, std::vector<std::uint8_t>(4, 0));
    for (int x = 0; x < 4; ++x)
        for (int z = 0; z < 4; ++z) f.host.grid8[x][z] = static_cast<std::uint8_t>(x * 4 + z);

    CHECK(f.view->SetDataView(7));
    CHECK(f.view->dataView() == 7);
    CHECK(!f.host.gridRequests.empty() && f.host.gridRequests.back() == kGridPower);
    // paletteA is stored with red and blue swapped
    CHECK(f.view->paletteA()[5] == (0xff000000u | (0x15u << 16) | ((5u * 4) << 8) | (5u * 8)));
    cIGZBuffer* overlay = f.host.sink.overlay;
    CHECK(overlay != nullptr);
    if (overlay && overlay->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) {
        const auto* bits = static_cast<const std::uint8_t*>(overlay->GetColorSurfaceBits());
        const std::uint32_t stride = overlay->GetColorSurfaceStride();
        int wrong = 0;
        for (int py = 0; py < 16; ++py)
            for (int px = 0; px < 16; ++px) {
                const std::uint32_t pixel = reinterpret_cast<const std::uint32_t*>(bits + py * stride)[px];
                if (pixel != ramp[(px / 4) * 4 + py / 4]) ++wrong;
            }
        CHECK(wrong == 0);
        overlay->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
    }
    // once the marked-cell view is registered, every cell whose paletteA colour has alpha is marked in that colour, and a
    // redraw REPLACES the cells (SetMarkedCells), it does not add to them
    FakeMarkedCells* marked = f.host.marked;
    CHECK(marked != nullptr && f.view->markedCellsRegistered());
    if (marked) {
        CHECK(marked->cells.size() == 16);
        CHECK(marked->colors.size() == 16 && marked->colors[5] == f.view->paletteA()[5]);
        f.view->Update();
        CHECK(marked->cells.size() == 16);
    }
    // the same view again is cached; view 0 clears the overlay
    const int overlays = f.host.sink.overlays;
    CHECK(f.view->SetDataView(7));
    CHECK(f.host.sink.overlays == overlays);
    CHECK(f.view->SetDataView(0));
    CHECK(f.host.sink.overlay == nullptr);
    CHECK(f.view->dataView() == 0);
}

// Update's lot shading and coverage circles over the active highlights: the occupant's cells scaled to the map, blended in the
// lot colour; a 2 px ring at the coverage radius in the circle colour, nothing at the centre.
void TestHighlightOverlay() {
    Fixture f;
    f.view->Init();
    auto* activate = new cRZMessage2Standard();
    activate->SetType(kWinMapMsgViewActivate);
    f.view->DoMessage(static_cast<cIGZMessage2*>(activate));
    f.host.cityX = f.host.cityZ = 4;   // 64 m city on a 64 px map: one pixel per metre
    f.host.sink.width = f.host.sink.height = 64;
    f.host.hasExemplar = true;
    f.host.exemplar.hasDataView = true;
    f.host.exemplar.dataView = 7;
    f.host.exemplar.hasHighlightView = true;
    f.host.exemplar.highlightView = 7;
    f.host.exemplar.hasLotShadeColor = true;
    f.host.exemplar.lotShadeColor = 0xff0000ffu;   // opaque blue
    f.host.exemplar.hasCircleColor = true;
    f.host.exemplar.circleColor = 0xffff0000u;     // opaque red
    f.host.grid8.assign(4, std::vector<std::uint8_t>(4, 0));
    FakeOccupant occupant;
    occupant.cells = SC4CellRect{0, 0, 0, 0};       // cell (0, 0) -> pixels 0..16
    occupant.position[0] = 32.0f;                   // the ring's centre (x, z) = (32, 32)
    occupant.position[2] = 32.0f;
    f.host.highlights = {WinMapHighlight{&occupant, 10.0f}};
    CHECK(f.view->SetDataView(7));
    cIGZBuffer* overlay = f.host.sink.overlay;
    CHECK(overlay != nullptr);
    if (overlay && overlay->Lock(static_cast<cIGZBuffer::eLockFlags>(0x8040))) {
        const auto* bits = static_cast<const std::uint8_t*>(overlay->GetColorSurfaceBits());
        const std::uint32_t stride = overlay->GetColorSurfaceStride();
        auto at = [&](int x, int y) { return reinterpret_cast<const std::uint32_t*>(bits + y * stride)[x]; };
        CHECK((at(8, 8) & 0xff) > 0xf0);             // inside the lot: blue
        CHECK((at(20, 20) & 0xff) == 0);             // outside it: not
        CHECK(((at(42, 32) >> 16) & 0xff) > 0x80);   // on the ring (radius 10 around 32.5): red, faded by its distance
        CHECK(((at(32, 32) >> 16) & 0xff) == 0);     // the centre: not
        overlay->Unlock(static_cast<cIGZBuffer::eLockFlags>(0x8040));
    }
    f.view->SetDataView(0);
}

int main() {
    TestConstructionAndQueryInterface();
    TestInitAndShutdown();
    TestMessageTarget();
    TestLegend();
    TestWindowSwitching();
    TestViewStack();
    TestCommandRouting();
    TestHighlights();
    TestOccupancyMap();
    TestRotatedAlphaBlt();
    TestDisplayConverters();
    TestInitWindow();
    TestPotholeDecals();
    TestDataViewImage();
    TestHighlightOverlay();
    if (g_failures == 0) std::printf("sc4_winmapview_test: all checks passed\n");
    return g_failures == 0 ? 0 : 1;
}
