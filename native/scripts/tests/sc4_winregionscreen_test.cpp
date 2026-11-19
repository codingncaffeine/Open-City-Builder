// cSC4WinRegionScreen test: fake region view / host / services, a fake window manager and the real clocks.
// Covers the constructor's field set-up, the three-interface QueryInterface, Init (the leaving-app + region-music +
// entered-app messages, the 14 commands, the backdrop / view / 12 snapshots / scroll / clouds children, the
// application services and the saved view position), the message dispatch (cGZMessage scroll messages, the
// accelerator message, the command-dispatcher message), the scroll command table, the bounds math with its
// truncation quirk, the renderer position, GZPaint (timer, smooth scroll, clamping, the y-negated scroll position),
// the signpost mouse paths, the panel switching, the tuning / preference reads, the snapshot defaults, the
// region/city launch helpers, the population refresh and the shutdown release path.
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <SDL.h>

#include "fake_winmgr2.h"
#include "sc4/WinRegionScreen.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
using namespace sc4;

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Fakes
// ---------------------------------------------------------------------------------------------------------------
struct FakeBuffer final : IWinBuffer {
    int refs = 1;
    cRZRect bounds{0, 0, 16, 16};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    const cRZRect* GetBounds() const override { return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return 0xff000000u | (r << 16) | (g << 8) | b;
    }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return true; }
};

struct FakeCity final : IRegionalCity {
    int refs = 1;
    int population = 0;
    bool occupied = false;
    int cellsX = 1, cellsZ = 1;
    std::string name;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int GetPopulation() override { return population; }
    bool IsOccupied() override { return occupied; }
    void GetSize(int& x, int& z) override { x = cellsX; z = cellsZ; }
    void GetName(cRZString& out) override { out.AssignFromCString(name.c_str()); }
};

struct FakeWindow final : cGZWin {
    using cGZWin::cGZWin;
};

struct FakeView final : IRegionView {
    explicit FakeView(cIGZWinMgr* mgr) {
        window = new FakeWindow(mgr);
        window->AddRef();
    }
    ~FakeView() override { window->Release(); }
    FakeWindow* window = nullptr;
    RegionViewCity* selected = nullptr;
    RegionViewCity* cityAtPoint = nullptr;
    std::vector<std::string> log;
    int scrollX = 0, scrollY = 0;
    int mapMode = -1;
    int regionIndex = -1;
    bool labels = false, grid = false, tutorial = false, scrollIndicator = false;
    std::uint32_t signpostType = 7, signpostSub = 7;
    std::uint32_t colour1 = 0, colour2 = 0;
    int addedCities = 0;

    cIGZWin* AsWindow() override { return window; }
    void AddCity(const RegionViewCity&) override { ++addedCities; log.push_back("addCity"); }
    void UpdateCity(RegionViewCity*) override { log.push_back("updateCity"); }
    void SetScrollPos(int x, int y) override { scrollX = x; scrollY = y; }
    RegionViewCity* GetCityAtPoint(int, int) override { return cityAtPoint; }
    RegionViewCity* GetSelectedCity() override { return selected; }
    void ClearSelection() override { selected = nullptr; log.push_back("clear"); }
    void SetSignpostMode(bool mode, bool second) override {
        log.push_back(std::string("signpostMode:") + (mode ? '1' : '0') + (second ? '1' : '0'));
    }
    void EnableLabels(bool on, bool second) override {
        labels = on;
        log.push_back(std::string("labels:") + (on ? '1' : '0') + (second ? '1' : '0'));
    }
    void EnableGrid(bool on) override { grid = on; log.push_back(std::string("grid:") + (on ? '1' : '0')); }
    void EnableTutorialArrow(bool on) override { tutorial = on; }
    void SetMapMode(int mode) override { mapMode = mode; log.push_back("mapMode:" + std::to_string(mode)); }
    void SelectCity(int, int) override { log.push_back("select"); }
    void GetRegionPlayfieldRect(RegionViewCity*, cRZRect& out) override { out = cRZRect{0, 0, 10, 10}; }
    void SetSignpostType(std::uint32_t type, std::uint32_t sub) override { signpostType = type; signpostSub = sub; }
    void SetScrollIndicator(bool on) override { scrollIndicator = on; log.push_back(std::string("scroll:") + (on ? '1' : '0')); }
    void SetRegionIndex(int index) override { regionIndex = index; }
    void SetLabelColours(std::uint32_t first, std::uint32_t second) override { colour1 = first; colour2 = second; }
};

struct FakeNotificationHelper final : INotificationHelper {
    std::vector<std::uint32_t> types;
    int added = 0, removed = 0;
    void AddMessageTypes(const std::uint32_t* list, int count) override { types.assign(list, list + count); }
    void AddAllNotifications(cIGZMessageTarget2*) override { ++added; }
    void RemoveAllNotifications(cIGZMessageTarget2*) override { ++removed; }
};

struct FakeAccelerator final : cIGZWinKeyAccelerator {
    std::uint32_t key = 0, modifiers = 0, command = 0;
    bool keyDown = false, accept = false;
    int refs = 1, tests = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool RegisterAccelerator(std::uint32_t, std::uint32_t) override { return true; }
    bool RegisterAccelerator(std::uint32_t, const cGZMessage&) override { return true; }
    bool UnregisterAccelerator(std::uint32_t) override { return true; }
    bool IsAcceleratorPresent(std::uint32_t, bool, bool) override { return accept; }
    bool TestForAccelerator(const cGZMessage&, std::uint32_t&) override { return false; }
    bool TestForAccelerator(const cGZMessage&, cGZMessage&) override { return false; }
    bool TestForAccelerator(std::uint32_t k, std::uint32_t mods, bool down, std::uint32_t& out) override {
        ++tests; key = k; modifiers = mods; keyDown = down;
        out = command;
        return accept;
    }
    bool TestForAccelerator(std::uint32_t, std::uint32_t, bool, cGZMessage&) override { return false; }
    int GetKeysForAccelerator(std::uint32_t, std::uint32_t*, std::uint32_t&) override { return 0; }
    bool ProcessAccelerator(const cGZMessage&) override { return false; }
};

// One posted message as the fake server saw it: the type plus the Data1 payload the music request carries.
struct PostedMessage {
    std::uint32_t type = 0;
    bool hasData1 = false;
    std::uint64_t data1 = 0;
};

struct FakeMessageServer final : cIGZMessageServer2 {
    std::vector<PostedMessage> posted;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override {
        PostedMessage entry;
        entry.type = message->GetType();
        if (auto* standard = dynamic_cast<cIGZMessage2Standard*>(message)) {
            entry.hasData1 = standard->GetHasData1();
            entry.data1 = standard->GetData1();
        }
        posted.push_back(entry);
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeCommandServer final : cIGZCommandServer {
    std::vector<std::uint32_t> registered, unregistered, executed;
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void CreateCommandParameterSet(cIGZCommandParameterSet**) override {}
    bool RegisterCommand(std::uint32_t id, const char*, const char*) override { registered.push_back(id); return true; }
    bool RegisterCommands(const cGZCommandInfoEntry*, std::uint32_t) override { return true; }
    bool RegisterCommandParameterInfo(std::uint32_t, const char*) override { return true; }
    bool UnregisterCommand(std::uint32_t id) override { unregistered.push_back(id); return true; }
    bool UnregisterCommands(const cGZCommandInfoEntry*, std::uint32_t) override { return true; }
    bool GetCommandInfo(std::uint32_t, cIGZString&, cIGZString*) const override { return false; }
    std::uint32_t GetCommandID(const cIGZString&) const override { return 0; }
    std::uint32_t GetCommandIDFromCommandString(const char*, std::uint32_t) const override { return 0; }
    void EnumerateCommands(std::vector<std::uint32_t>&) const override {}
    bool RegisterCommandGenerator(cIGZCommandGenerator*) override { return true; }
    bool UnregisterCommandGenerator(cIGZCommandGenerator*) override { return true; }
    cIGZCommandDispatcher* GetCommandDispatcher() const override { return nullptr; }
    bool SetCommandDispatcher(cIGZCommandDispatcher*) override { return true; }
    std::uint32_t ExecuteCommand(std::uint32_t id, cIGZCommandParameterSet*, cIGZCommandParameterSet*) override {
        executed.push_back(id);
        return 0;
    }
    std::uint32_t ExecuteCommand(const char*, std::uint32_t, cIGZCommandParameterSet*) override { return 0; }
    std::uint32_t ExecuteCommandFormatted(const char*, cIGZCommandParameterSet*, ...) override { return 0; }
    bool ConvertStringToCommand(const char*, std::uint32_t, std::uint32_t&, cIGZCommandParameterSet*&) override { return false; }
    bool ConvertCommandToString(std::uint32_t, cIGZCommandParameterSet*, cIGZString&) override { return false; }
    bool ConvertCommandOutputToString(cIGZCommandParameterSet*, cIGZString&) override { return false; }
};

struct FakeGraphicSystem final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};

struct FakeRenderer final : IRegionRenderer {
    float screenX = 0, screenY = 0;
    int inits = 0, shutdowns = 0;
    bool Init(int, int) override { ++inits; return true; }
    void Shutdown() override { ++shutdowns; }
    void GetScreenOffsets(float& x, float& y) override { x = screenX; y = screenY; }
};

struct FakeFrameService final : IRegionFrameService {
    int frames = 0, shutdowns = 0;
    bool Init() override { return true; }
    void Shutdown() override { ++shutdowns; }
    void EndFrame() override { ++frames; }
};

struct FakeEffects final : IEffectsManager {
    int inits = 0, shutdowns = 0;
    bool Init() override { ++inits; return true; }
    void Shutdown() override { ++shutdowns; }
};

struct FakeHost final : IRegionScreenHost {
    FakeMessageServer messages;
    FakeCommandServer commands;
    FakeNotificationHelper notifications;
    FakeAccelerator accelerator;
    FakeRenderer* renderer = new FakeRenderer();
    FakeFrameService* frameService = new FakeFrameService();
    FakeEffects* effects = new FakeEffects();
    std::vector<std::uint8_t> preferences = std::vector<std::uint8_t>(0x1734, 0);
    std::vector<std::string> log;

    FakeGraphicSystem graphicSystem;
    cIGZWinMgr* mgr = nullptr;
    bool createView = false;
    int savedPreviousRegion = 0;
    bool savedRegionValid = false;
    std::vector<FakeBuffer*> snapshotBuffers;
    int snapshotLoads = 0, blankBuffers = 0;

    std::vector<std::string> regions{"Alpha", "Beta"};
    int currentRegion = 0;
    int cellLeft = 0, cellTop = 0, cellRight = 2, cellBottom = 1;
    int snapshotSet = 0;
    std::map<std::pair<int, int>, RegionCityRef> cityRefs;
    std::vector<std::pair<int, int>> createdCities;
    bool regionLaunchSucceeds = false;
    std::uint32_t launchCityResult = 1;
    RegionCameraBasis basis;
    cSCPropertyHolder* tuning = nullptr;
    std::uint8_t opacityFirst = 0, opacitySecond = 0;
    std::uint32_t erosion[3] = {0, 0, 0};
    std::uint32_t regionHeights[2] = {0, 0};
    int formatted = -1;
    int quitFlag = -1;

    cIGZMessageServer2* MessageServer2() override { return &messages; }
    cIGZCommandServer* CommandServer() override { return &commands; }
    std::uint8_t* Preferences() override { return preferences.data(); }
    void SavePreferences() override { log.push_back("savePreferences"); }
    int DoubleClickTimeMs() override { return 500; }
    void PlayUiSound(std::uint32_t) override { log.push_back("sound"); }
    cIGZUnknown* GraphicSystemObject() override { return &graphicSystem; }
    void SetTerrainMap(cSC4RegionalTerrainMap*) override { log.push_back("terrainMap"); }
    void SetViewUtilities(IRegionRenderer*) override { log.push_back("viewUtilities"); }
    void ClearEffectsManagerGlobal() override { log.push_back("clearEffects"); }
    void ClearViewGlobals() override { log.push_back("clearView"); }
    void ClearTerrainMapGlobal() override { log.push_back("clearTerrain"); }
    void FlushResources() override { log.push_back("flush"); }
    IRegionRenderer* CreateRenderer() override { return renderer; }
    IRegionFrameService* CreateFrameService() override { return frameService; }
    IEffectsManager* CreateEffectsManager() override { return effects; }
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { return &accelerator; }
    INotificationHelper* CreateNotificationHelper() override { return &notifications; }
    cIGZMessage2Standard* CreateStandardMessage() override {
        auto* message = new cRZMessage2Standard();
        message->AddRef();   // the host hands out one reference (the screen releases it)
        return message;
    }
    void PostEngineMessage(std::uint32_t type) override { messages.posted.push_back(PostedMessage{type, false, 0}); }
    int RegionCount() override { return static_cast<int>(regions.size()); }
    int CurrentRegionIndex() override { return currentRegion; }
    void UpdateRegionViewState() override { log.push_back("updateRegionViewState"); }
    bool PreviousRegionIndex(int& previousIndex) override {
        previousIndex = savedPreviousRegion;
        return savedRegionValid;
    }
    void SelectRegion(int index) override { currentRegion = index; log.push_back("selectRegion:" + std::to_string(index)); }
    const char* RegionNameAt(int index) override {
        return index >= 0 && index < static_cast<int>(regions.size()) ? regions[index].c_str() : nullptr;
    }
    const char* RegionName() override { return regions[0].c_str(); }
    bool ReloadRegion() override { log.push_back("reloadRegion"); return true; }
    bool RegionCellBounds(int& left, int& top, int& right, int& bottom) override {
        left = cellLeft; top = cellTop; right = cellRight; bottom = cellBottom;
        return true;
    }
    int CitySnapshotSet() override { return snapshotSet; }
    RegionCityRef* FindCity(int x, int z) override {
        auto it = cityRefs.find({x, z});
        return it == cityRefs.end() ? nullptr : &it->second;
    }
    RegionCityRef* FindOrCreateCity(const RegionViewCity& city, bool) override {
        createdCities.push_back({city.x, city.z});
        auto& ref = cityRefs[{city.x, city.z}];
        if (!ref.get()) ref = new FakeCity();
        return &ref;
    }
    bool LaunchRegionCityByName(const cRZString& name, RegionCityRef*) override {
        log.push_back(std::string("launchByName:") + name.ToChar());
        return regionLaunchSucceeds;
    }
    void ClearRegionViewSlot() override { log.push_back("clearRegionViewSlot"); }
    std::uint32_t LaunchCityFromRegion(RegionCityRef*) override { log.push_back("launchCity"); return launchCityResult; }
    void QuitToRegionSelect(bool flag) override { quitFlag = flag ? 1 : 0; log.push_back("quit"); }
    IRegionView* CreateRegionView() override { return createView && mgr ? new FakeView(mgr) : nullptr; }
    IWinBuffer* LoadRegionScreenSnapshot(std::uint32_t) override {
        ++snapshotLoads;
        return NewSnapshotBuffer();
    }
    IWinBuffer* CreateBlankDisplayBuffer() override {
        ++blankBuffers;
        return NewSnapshotBuffer();
    }
    cISCPropertyHolder* CreateTuningProperties() override {
        if (!tuning) return nullptr;
        tuning->AddRef();
        return tuning;
    }
    bool CloudOpacityBytes(cISCPropertyHolder*, std::uint8_t& first, std::uint8_t& second) override {
        first = opacityFirst; second = opacitySecond;
        return true;
    }
    void SetWaterErosionTunables(std::uint32_t clicks, std::uint32_t flowSteps, std::uint32_t smoothing) override {
        erosion[0] = clicks; erosion[1] = flowSteps; erosion[2] = smoothing;
    }
    void SetRegionHeightGlobals(std::uint32_t first, std::uint32_t second) override {
        regionHeights[0] = first; regionHeights[1] = second;
    }
    void ActivatePanelButton(cIGZWin*, cIGZWin*) override { log.push_back("activatePanel"); }
    void FormatNumber(int value, cRZString& out) override {
        formatted = value;
        const std::string text = std::to_string(value);
        out.AssignFromCString(text.c_str());
    }
    bool CameraBasis(RegionCameraBasis& out) override { out = basis; return true; }

    FakeBuffer* NewSnapshotBuffer() {
        auto* buffer = new FakeBuffer();
        snapshotBuffers.push_back(buffer);
        return buffer;
    }
};

int Count(const std::vector<std::string>& log, const std::string& value) {
    int n = 0;
    for (const auto& entry : log) n += entry == value ? 1 : 0;
    return n;
}

}  // namespace

int main() {
    FakeMgr2 mgr;
    FakeHost host;

    auto* main = new FakeWindow(&mgr);
    main->AddRef();
    main->SetArea(0, 0, 640, 480);
    mgr.mainWin = main;

    // Init asks the host for the view, the 12 snapshot buffers and the nation's saved region-view state.
    host.mgr = &mgr;
    host.createView = true;
    host.savedPreviousRegion = host.currentRegion;
    host.savedRegionValid = true;
    {
        const std::int32_t viewX = 400 * 256, viewY = 300 * 256;   // saved region-map position * 256
        std::memcpy(host.preferences.data() + kPrefsViewX, &viewX, sizeof viewX);
        std::memcpy(host.preferences.data() + kPrefsViewY, &viewY, sizeof viewY);
        host.preferences[kPrefsSignpostMode] = 1;   // LoadGUI reads it back into signpostMode_
    }

    SetRegionScreenHost(&host);
    auto* screen = new cSC4WinRegionScreen(&mgr);
    screen->AddRef();
    screen->SetArea(0, 0, 640, 480);

    // --- construction
    CHECK(screen->GetID() == kRegionScreenWindowId);
    CHECK(screen->GetFlag(kWinIgnoreCursor));
    CHECK(!screen->GetFlag(kWinPrivateBuffer));
    CHECK(screen->Notifications() == &host.notifications);
    CHECK(host.notifications.types.size() == 17);
    CHECK(host.notifications.types[3] == kCmdRegionQuitGame);
    CHECK(host.notifications.types[13] == kCmdRegionSetExpandedToolTips);
    CHECK(screen->CityHalfSizeX() == 0x96 && screen->CityHalfSizeY() == 0x96);
    CHECK(screen->SignpostMode() == 1);
    CHECK(screen->View() == nullptr && screen->Clouds() == nullptr && screen->Scroll() == nullptr);

    // --- Init: leaves / enters the app, posts the region-music request, registers the 14 commands, adds the
    // notifications, builds the children, starts the services and restores the saved view position.
    CHECK(screen->Init());
    CHECK(host.messages.posted.size() == 5);
    CHECK(host.messages.posted[0].type == kRegionScreenMsgLeavingApp);
    CHECK(host.messages.posted[1].type == kRegionScreenMsgRegionMusic);
    CHECK(host.messages.posted[1].hasData1);
    CHECK(host.messages.posted[1].data1 == 1);   // Data1 1 selects "krgn"; mayor mode posts 0 ("kmay")
    CHECK(host.messages.posted[2].type == kMsgRegionCityViewChanged);
    CHECK(host.messages.posted[3].type == kRegionScreenCameraGridMessage);
    CHECK(host.messages.posted[4].type == kRegionScreenMsgEnteredApp);
    CHECK(host.commands.registered.size() == 14);
    CHECK(host.commands.registered[0] == kCmdRegionQuitGame);
    CHECK(host.commands.registered[13] == kCmdRegionCancel);
    CHECK(host.notifications.added == 1);
    CHECK(screen->Accelerator() == &host.accelerator);
    CHECK(host.graphicSystem.refs == 1);
    CHECK(Count(host.log, "updateRegionViewState") == 1);
    CHECK(host.snapshotLoads == 12 && host.blankBuffers == 0);
    for (int i = 0; i < 12; ++i) CHECK(screen->SnapshotBuffer(i) != nullptr);
    CHECK(screen->TargetX() == 80.0f && screen->TargetY() == 60.0f);   // -w/2 + 400, -h/2 + 300
    CHECK(screen->SignpostMode() == 1);   // restored from the preference by LoadGUI
    CHECK(host.renderer->inits == 1 && host.effects->inits == 1);
    CHECK(Count(host.log, "terrainMap") == 1 && Count(host.log, "viewUtilities") == 1);
    CHECK(screen->Camera() != nullptr && screen->CameraControl() != nullptr && screen->TerrainMap() != nullptr);
    CHECK(screen->TerrainMap()->CellCountX() == 3 * 0x10 && screen->TerrainMap()->CellCountZ() == 2 * 0x10);

    // --- QueryInterface: the base's extra iid is refused, the filter view is answered, the message target is not.
    {
        void* out = nullptr;
        CHECK(screen->QueryInterface(1, &out) && out == static_cast<cIGZWin*>(screen));
        screen->Release();
        CHECK(screen->QueryInterface(kGZIID_cIGZWin, &out) && out == static_cast<cIGZWin*>(screen));
        screen->Release();
        CHECK(screen->QueryInterface(0xc6ae7085, &out) && out == static_cast<cIGZWinMessageFilter*>(screen));
        screen->Release();
        CHECK(!screen->QueryInterface(0xe98b2f57, &out));   // quirk: the base accepts it, the screen does not
        CHECK(!screen->QueryInterface(0x452294aa, &out));
        CHECK(screen->QueryInterface(0x22ba0121, &out) && out == static_cast<cIGZWin*>(screen));
        screen->Release();
    }

    // --- the region view, backdrop, scroll and clouds Init built
    auto* view = static_cast<FakeView*>(screen->View());
    auto* clouds = screen->Clouds();
    auto* scroll = screen->Scroll();
    CHECK(view != nullptr && clouds != nullptr && scroll != nullptr && screen->Backdrop() != nullptr);
    CHECK(view->window->GetParentWin() == screen);
    CHECK(view->window->GetW() == 640 && view->window->GetH() == 480);
    CHECK(view->window->GetFlag(kWinVisible));
    CHECK(view->regionIndex == host.currentRegion);
    CHECK(view->colour1 == 0x00808080 && view->colour2 == 0xffff0000);
    CHECK(clouds->GetParentWin() == screen && scroll->GetParentWin() == screen);
    CHECK(!clouds->IsVisible());   // the clouds preference is off

    // --- ComputeBaseLimits: 3x2 cells (the map is smaller than the window, so the bounds collapse)
    screen->ComputeBaseLimits();
    CHECK(screen->BoundMinX() == -224 && screen->BoundMinY() == -212);
    CHECK(screen->BoundMaxX() == -218 && screen->BoundMaxY() == -212);
    // 11x11 cells: the excess is halved on the origin and added back on the far side
    host.cellRight = 10;
    host.cellBottom = 10;
    screen->ComputeBaseLimits();
    CHECK(screen->BoundMinX() == -562 && screen->BoundMinY() == -195);
    CHECK(screen->BoundMaxX() == 506 && screen->BoundMaxY() == 329);
    host.cellRight = 2;
    host.cellBottom = 1;
    screen->ComputeBaseLimits();

    // --- UpdateRendererPosition: the basis scales the target and swaps z into y
    host.basis.right[0] = 1.0f; host.basis.right[1] = 0.0f; host.basis.right[2] = 0.0f;
    host.basis.up[0] = 0.0f; host.basis.up[1] = 0.0f; host.basis.up[2] = -1.0f;
    host.basis.worldPerUnit = 2.0f;
    screen->SetTarget(10.0f, 20.0f);
    screen->UpdateRendererPosition();
    const cS3DVector3& target = screen->CameraControl()->ViewTarget();
    CHECK(target.x == 660.0f && target.y == 520.0f && target.z == 0.0f);
    host.basis = RegionCameraBasis{};   // no camera plane offset from here on

    // --- GZPaint: first call starts the timer, then the clamp and the scroll position
    screen->SetBounds(-1000, -1000, 1000, 1000);
    g_ticks = 0;
    CHECK(screen->GZPaint());
    {
        // The camera plane offset comes from the control: x is rounded with the +12582912 bit trick and the -w/2
        // subtraction, y with 12582912 - offset (the negated value) and -h/2.
        const cS3DVector2 plane = screen->CameraControl()->ScreenOffset();
        float biasedX = plane.x + 12582912.0f;
        float biasedY = 12582912.0f - plane.y;
        std::int32_t bitsX = 0, bitsY = 0;
        std::memcpy(&bitsX, &biasedX, sizeof bitsX);
        std::memcpy(&bitsY, &biasedY, sizeof bitsY);
        CHECK(view->scrollX == bitsX - 320 - 0x4b400000);
        CHECK(view->scrollY == bitsY - 240 - 0x4b400000);
    }
    CHECK(host.frameService->frames == 1);

    screen->SetTarget(2000.0f, -2000.0f);
    g_ticks = 1000;
    screen->GZPaint();
    CHECK(screen->TargetX() == 1000.0f && screen->TargetY() == -1000.0f);   // clamped to the bounds
    CHECK(!screen->Smoothing());

    // Smooth scroll: 200 ms of the capped step covers 1200 * 0.2 = 240 units of a 1000-unit distance.
    screen->SetTarget(0.0f, 0.0f);
    screen->BeginSmoothing(1000.0f, 0.0f);
    g_ticks = 2000;
    screen->GZPaint();
    CHECK(std::fabs(screen->TargetX() - 240.0f) < 1e-3f);
    CHECK(screen->TargetY() == 0.0f);
    CHECK(screen->Smoothing());
    g_ticks = 2100;   // 100 ms more, 760 units left: 120 more
    screen->GZPaint();
    CHECK(std::fabs(screen->TargetX() - 360.0f) < 1e-3f);

    // --- DoMessage(cGZMessage&): scroll start / stop / delta and the accelerator message
    {
        cGZMessage message;
        message.type = kRegionScreenScrollStart;
        view->scrollIndicator = false;
        CHECK(screen->DoMessage(message));
        CHECK(view->scrollIndicator);
        message.type = kRegionScreenScrollStop;
        CHECK(screen->DoMessage(message));
        CHECK(!view->scrollIndicator);
        message.type = kRegionScreenScrollDelta;
        message.param1 = static_cast<std::uint64_t>(static_cast<std::int64_t>(256 * 4));
        message.param2 = static_cast<std::uint64_t>(static_cast<std::int64_t>(-256 * 2));
        const float beforeX = screen->TargetX(), beforeY = screen->TargetY();
        CHECK(screen->DoMessage(message));
        CHECK(std::fabs(screen->TargetX() - (beforeX + 4.0f)) < 1e-4f);
        CHECK(std::fabs(screen->TargetY() - (beforeY - 2.0f)) < 1e-4f);
    }
    {
        cGZMessage keyMessage;
        keyMessage.param1 = 0x2b;
        keyMessage.param2 = 0x100;
        cGZMessage message;
        message.type = 0x1b;
        message.param2 = 5;
        message.param3 = reinterpret_cast<std::uint64_t>(&keyMessage);
        host.accelerator.accept = true;
        host.accelerator.command = 0x1234;
        CHECK(screen->DoMessage(message));
        CHECK(host.accelerator.tests == 1);
        CHECK(host.accelerator.key == 0x2b && host.accelerator.modifiers == 0x100 && host.accelerator.keyDown);
        CHECK(host.commands.executed.size() == 1 && host.commands.executed[0] == 0x1234);
        message.param2 = 7;   // out of range: falls through to cGZWin::DoMessage (which answers false)
        CHECK(!screen->DoMessage(message));
        CHECK(host.accelerator.tests == 1);
    }

    // --- DoMessage(cIGZMessage2*) with the real command-dispatcher message
    {
        auto* dispatcher = new cGZMessage2CommandDispatcherMessage();
        dispatcher->AddRef();
        dispatcher->SetType(kCmdRegionCancel);
        view->log.clear();
        CHECK(screen->DoMessage(dispatcher));
        CHECK(Count(view->log, "clear") == 1);
        dispatcher->Release();   // DoMessage already dropped the reference QueryInterface took

        // The clouds preference message is not a dispatcher message: it takes the plain branch.
        auto* plain = new cRZMessage2Standard();
        plain->AddRef();
        plain->SetType(kMsgRegionCloudsPreference);
        host.preferences[kPrefsCloudsVisible] = 1;
        screen->DoMessage(plain);
        CHECK(clouds->GetFlag(kWinVisible));
        host.preferences[kPrefsCloudsVisible] = 0;
        screen->DoMessage(plain);
        CHECK(!clouds->GetFlag(kWinVisible));
        plain->Release();
    }

    // --- ProcessCommand: the scroll table and the tooltip toggle
    {
        scroll->SetTunables(10, 32, 3.0f);
        host.preferences[kPrefsExpandedToolTips] = 0;
        CHECK(!screen->ProcessCommand(kCmdRegionSetExpandedToolTips, nullptr, nullptr));
        CHECK(host.preferences[kPrefsExpandedToolTips] == 1);
        CHECK(!screen->ProcessCommand(kCmdRegionSetExpandedToolTips, nullptr, nullptr));
        CHECK(host.preferences[kPrefsExpandedToolTips] == 0);

        CHECK(!screen->ProcessCommand(kCmdRegionScrollRight, nullptr, nullptr));
        CHECK(scroll->EdgeVectorX() == scroll->EdgeSpeed() && scroll->EdgeVectorY() == 0.0f);
        CHECK(!screen->ProcessCommand(kCmdRegionScrollLeft, nullptr, nullptr));
        CHECK(scroll->EdgeVectorX() == -scroll->EdgeSpeed());
        CHECK(!screen->ProcessCommand(kCmdRegionScrollDown, nullptr, nullptr));
        CHECK(scroll->EdgeVectorY() == scroll->EdgeSpeed());
        CHECK(!screen->ProcessCommand(kCmdRegionScrollUp, nullptr, nullptr));
        CHECK(scroll->EdgeVectorY() == -scroll->EdgeSpeed());
        CHECK(!screen->ProcessCommand(kCmdRegionScrollLeftStop, nullptr, nullptr));
        CHECK(scroll->EdgeVectorX() == 0.0f);
        CHECK(!screen->ProcessCommand(kCmdRegionScrollUpStop, nullptr, nullptr));
        CHECK(scroll->EdgeVectorY() == 0.0f);

        view->log.clear();
        CHECK(!screen->ProcessCommand(kCmdRegionCancel, nullptr, nullptr));
        CHECK(Count(view->log, "clear") == 1);
    }

    // --- LoadRegion by name
    {
        host.currentRegion = 0;
        CHECK(screen->LoadRegion(cRZString("Alpha")));     // already current: no reload
        CHECK(Count(host.log, "reloadRegion") == 0);
        CHECK(screen->LoadRegion(cRZString("Beta")));
        CHECK(host.currentRegion == 1);
        CHECK(Count(host.log, "reloadRegion") == 1);
        CHECK(!screen->LoadRegion(cRZString("Gamma")));
        host.currentRegion = 0;
    }

    // --- cities, snapshots, population
    auto* snapshot = new FakeBuffer();
    auto* mask = new FakeBuffer();
    auto* smallSnapshot = new FakeBuffer();
    auto* small = new FakeBuffer();
    auto* cityA = new FakeCity();
    auto* cityB = new FakeCity();
    FakeWindow* populationWindow = nullptr;
    {
        screen->Cities().clear();
        RegionViewCity city;
        city.x = 1;
        city.z = 2;
        city.size = 0;
        screen->Cities().push_back(city);

        screen->SetSnapshotBuffer(0, snapshot);   // the screen takes a reference
        screen->SetSnapshotBuffer(1, mask);
        screen->SetSnapshotBuffer(6, small);
        CHECK(snapshot->refs == 2 && mask->refs == 2 && small->refs == 2);

        host.snapshotSet = 0;
        RegionViewCity& stored = screen->Cities().front();
        stored.snapshotSmall = smallSnapshot;
        smallSnapshot->AddRef();   // the city owns one reference
        screen->SetCitySnapshotsToDefault(stored);
        CHECK(stored.snapshot.get() == snapshot && stored.mask.get() == mask);
        CHECK(snapshot->refs == 3);
        CHECK(stored.snapshotSmall == nullptr && smallSnapshot->refs == 1);

        host.snapshotSet = 1;
        screen->SetCitySnapshotsToDefault(stored);
        CHECK(stored.snapshot.get() == small);
        CHECK(stored.mask.get() == screen->SnapshotBuffer(7));   // the small set's mask Init loaded
        CHECK(snapshot->refs == 2);
        host.snapshotSet = 0;
        screen->SetCitySnapshotsToDefault(stored);

        // population: 5 + 7 through the host's city refs
        cityA->population = 5;
        cityA->name = "Alpha City";
        cityB->population = 7;
        cityB->name = "Beta Town";
        host.cityRefs[{1, 2}] = cityA;
        RegionViewCity second;
        second.x = 3;
        second.z = 4;
        screen->Cities().push_back(second);
        host.cityRefs[{3, 4}] = cityB;
        populationWindow = new FakeWindow(&mgr);
        populationWindow->SetID(kRegionScreenPopulationChildId);
        populationWindow->AddRef();
        mgr.valid.insert(populationWindow);
        CHECK(screen->ChildAdd(populationWindow));
        CHECK(populationWindow->GetParentWin() == screen);
        screen->UpdatePopulation();
        CHECK(host.formatted == 12);
        CHECK(std::strcmp(populationWindow->GetCaption().ToChar(), "12") == 0);
    }

    // --- LaunchCity / LaunchRegionalCity
    {
        host.log.clear();
        host.regionLaunchSucceeds = false;
        host.launchCityResult = 1;
        cRZString out;
        CHECK(screen->LaunchCity(cRZString(""), 3, out, false));   // "any": launch by size
        CHECK(Count(host.log, "launchCity") == 1);

        host.log.clear();
        host.regionLaunchSucceeds = true;
        static_cast<FakeCity*>(host.cityRefs[{1, 2}].get())->occupied = true;
        CHECK(screen->LaunchCity(cRZString("Alpha City"), 0, out, false));
        CHECK(Count(host.log, "launchByName:Alpha City") == 1);
        CHECK(out.ToChar() == std::string("Alpha City"));

        host.log.clear();
        host.launchCityResult = 0;
        host.regionLaunchSucceeds = false;
        static_cast<FakeCity*>(host.cityRefs[{1, 2}].get())->occupied = false;
        CHECK(!screen->LaunchCity(cRZString(""), 0, out, false));   // the host refuses the launch
        CHECK(!screen->LaunchCity(cRZString("Missing"), 0, out, false));
        host.launchCityResult = 1;
    }

    // --- tuning
    {
        auto* holder = new cSCPropertyHolder();
        holder->AddRef();
        auto addUint = [holder](std::uint32_t id, std::uint32_t value) {
            cRZVariant variant(value);
            holder->AddProperty(id, &variant, false);
        };
        auto addFloat = [holder](std::uint32_t id, float value) {
            cRZVariant variant(value);
            holder->AddProperty(id, &variant, false);
        };
        addUint(kTuningScrollDeadZone, 12);
        addUint(kTuningScrollMaxDistance, 40);
        addFloat(kTuningScrollSpeedDivisor, 2.5f);
        addUint(kTuningCityHalfSizeX, 100);
        addUint(kTuningCityHalfSizeY, 200);
        addUint(kTuningWaterErosionClicks, 3);
        addUint(kTuningWaterErosionFlowSteps, 4);
        addUint(kTuningWaterErosionSmoothing, 5);
        addUint(kTuningLabelColor1, 0x11223344);
        addUint(kTuningLabelColor2, 0x55667788);
        {
            cRZVariant byte1(static_cast<std::uint8_t>(0x21));
            holder->AddProperty(kTuningBaseTerrainTypeHeight, &byte1, false);
            cRZVariant byte2(static_cast<std::uint8_t>(0x42));
            holder->AddProperty(kTuningRegionHeightByte, &byte2, false);
        }
        addFloat(kTuningSmoothDistance, 600.0f);
        addFloat(kTuningSmoothSeconds, 2.0f);
        host.tuning = holder;
        host.opacityFirst = 0x30;
        host.opacitySecond = 0x50;
        screen->ReadTunableValues();
        CHECK(screen->CityHalfSizeX() == 100 && screen->CityHalfSizeY() == 200);
        CHECK(host.erosion[0] == 3 && host.erosion[1] == 4 && host.erosion[2] == 5);
        CHECK(host.regionHeights[0] == 0x21 && host.regionHeights[1] == 0x42);
        CHECK(view->colour1 == 0x11223344 && view->colour2 == 0x55667788);
        CHECK(holder->Count() == 14);   // only read, nothing was created
        host.tuning = nullptr;
        holder->Release();   // the screen already dropped its reference
        screen->SetCityHalfSizes(0x96, 0x96);
    }

    // --- the signpost mouse paths
    {
        view->cityAtPoint = &screen->Cities().front();
        view->log.clear();
        CHECK(screen->GZOnMouseMove(5, 5, 0));
        CHECK(Count(view->log, "select") == 1);
        CHECK(screen->GZOnMouseExit(nullptr));
        CHECK(Count(view->log, "clear") == 1);

        view->selected = &screen->Cities().front();
        static_cast<FakeCity*>(host.cityRefs[{1, 2}].get())->occupied = true;
        host.log.clear();
        CHECK(screen->GZOnMouseDownL(5, 5, 0));
        CHECK(screen->SignpostActive());
        CHECK(view->signpostType == 0xca539340 && view->signpostSub == 0x0a551c50);
        CHECK(Count(host.log, "sound") == 1);
        CHECK(view->labels);
        static_cast<FakeCity*>(host.cityRefs[{1, 2}].get())->occupied = false;

        CHECK(screen->GZOnMouseDownR(5, 5, 0));   // forwards to the scroll window
        screen->RestoreSignposts();
        CHECK(!screen->SignpostActive());
        CHECK(view->signpostType == 0);
    }

    // --- panels and commands
    auto* panel0 = new FakeWindow(&mgr);
    panel0->AddRef();
    auto* panelButton = new FakeWindow(&mgr);
    panelButton->AddRef();
    {
        screen->SetMemberWindow(0, panel0);
        screen->SetMemberWindow(3, panelButton);
        panel0->ShowWindow();
        screen->ShowPanel(0);   // the visible panel is hidden again
        CHECK(!panel0->IsVisible());
        CHECK(!panelButton->IsVisible());
        screen->ShowPanel(1);   // another panel: the buttons are toggled, the panels are not created
        CHECK(!panel0->IsVisible());
        screen->ShowPanel(-1);  // hide all
        CHECK(!panel0->IsVisible());

        host.log.clear();
        CHECK(screen->GZOnCommand(0x287259f7, kRegionScreenGridCheckBoxId));
        CHECK(host.preferences[kPrefsShowGrid] == 0);   // no button child exists, so IsChecked() stays false
        CHECK(screen->GZOnCommand(0x287259f6, kRegionScreenPanelButtonIds[2]));
        CHECK(screen->GZOnCommand(0x287259f6, 0xebb91356));
        CHECK(screen->PanelsVisible());
        CHECK(screen->GZOnCommand(0x287259f6, 0x4bb92c1f));
        CHECK(!screen->PanelsVisible());
        CHECK(screen->GZOnCommand(0x287259f6, kRegionScreenMapModeButtonIds[1]));
        CHECK(view->mapMode == 1);
        CHECK(host.preferences[kPrefsMapMode] == 1);
        CHECK(screen->GZOnCommand(0x00000001, 0));   // unknown command: ignored
    }

    // --- preferences
    {
        host.log.clear();
        screen->SetTarget(10.0f, 20.0f);
        cSC4WinRegionScreen::SetUserPreferencesToCurrentRegion();
        CHECK(std::strncmp(reinterpret_cast<const char*>(host.preferences.data() + kPrefsRegionName), "Alpha", 5) == 0);
        std::int32_t viewX = 0;
        std::memcpy(&viewX, host.preferences.data() + kPrefsViewX, sizeof viewX);
        CHECK(viewX == INT32_MIN);
        screen->SetUserPreferencesToCurrentPosition();
        std::memcpy(&viewX, host.preferences.data() + kPrefsViewX, sizeof viewX);
        CHECK(viewX == static_cast<std::int32_t>((320.0f + 10.0f) * 256.0f));
        CHECK(Count(host.log, "savePreferences") == 2);
    }

    // --- Shutdown: unregisters the commands, posts the two region messages, releases the windows and the buffers
    {
        host.messages.posted.clear();
        host.commands.unregistered.clear();
        CHECK(screen->Shutdown());
        CHECK(host.messages.posted.size() == 2);
        CHECK(host.messages.posted[0].type == kRegionScreenMsgLeavingRegion);
        CHECK(host.messages.posted[1].type == kRegionScreenMsgLeftRegion);
        CHECK(host.commands.unregistered.size() == 14);
        CHECK(host.notifications.removed == 1);
        CHECK(host.renderer->shutdowns == 1 && host.effects->shutdowns == 1);
        CHECK(host.frameService->shutdowns == 1);
        CHECK(Count(host.log, "clearEffects") == 1 && Count(host.log, "clearView") == 1);
        CHECK(screen->Scroll() == nullptr && screen->Clouds() == nullptr && screen->View() == nullptr);
        CHECK(screen->Backdrop() == nullptr);
        CHECK(screen->SnapshotBuffer(0) == nullptr);
        CHECK(host.graphicSystem.refs == 0);
        CHECK(snapshot->refs == 1 && mask->refs == 1 && small->refs == 1);   // only the test's references are left
        CHECK(smallSnapshot->refs == 1);
    }

    // --- clean-up (the screen's child list, then the remaining references)
    screen->ChildDelete(populationWindow);
    populationWindow->Release();
    panel0->Release();
    panelButton->Release();
    delete snapshot;
    delete mask;
    delete small;
    delete smallSnapshot;
    screen->Release();   // the destructor runs here; Shutdown is a no-op the second time
    main->Release();

    host.cityRefs.clear();   // the host owns the two cities' references
    delete cityA;
    delete cityB;
    for (FakeBuffer* buffer : host.snapshotBuffers) delete buffer;   // the screen released its references
    delete host.renderer;
    delete host.frameService;
    delete host.effects;
    SetRegionScreenHost(nullptr);
    if (!failures) std::printf("winregionscreen OK\n");
    return failures;
}
