// cSC4View3DWin: the recovered slice — object lifecycle, QueryInterface / refcount, the camera and view
// state (zoom, rotation, scroll velocity, the scroll render-property snapshot), the view level / filter
// information messages, the input-tool stack and its dispatch, the five mouse handlers (the city router,
// the city-scroll tool, the model-bounds camera target and edge scrolling), the cursor-text list/window,
// the mission-panel / secondary-menu teardown, the progress screen, the animation recorder, the tool
// invokers, the two picks, the global effect triggers, the frame (GZPaint) and DoMessage(cGZMessage&).
// See src/sc4/View3DWin.h for the offsets, the seams and the quirks.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "gz/WinGen.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/WinAuraAlert.h"
#include "sc4/SC4UI.h"
#include "sc4/View3DWin.h"
#include "s3d/ModelInstance.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

namespace sc4 {
// Access to the members the surviving game code sets from the GUI paths that are not recovered.
struct View3DWinTestAccess {
    static bool& PaintEnabled(cSC4View3DWin& v) { return v.paintEnabled_; }
    static float& FrameTimeThreshold(cSC4View3DWin& v) { return v.frameTimeThreshold_; }
    static std::list<cRZAutoRefCount<cIGZMessage2>>& Deferred(cSC4View3DWin& v) { return v.deferredMessages_; }
    static std::vector<cSC4ViewInputControl*>& Tools(cSC4View3DWin& v) { return v.tools_; }
    static std::vector<cSC4ViewInputControl*>& Persistent(cSC4View3DWin& v) { return v.persistentTools_; }
    static cSC4CameraControl*& Camera(cSC4View3DWin& v) { return v.cameraControl_; }
    static IRenderer3D*& Renderer(cSC4View3DWin& v) { return v.renderer_; }
    static ICoverageHelper3D*& Coverage(cSC4View3DWin& v) { return v.coverageHelper_; }
    static cIGZWin*& ViewManagerSource(cSC4View3DWin& v) { return v.viewManagerSource_; }
    static ISimulatorPauseSink*& PauseSink(cSC4View3DWin& v) { return v.simulatorToPause_; }
    static cSC4RenderProperties& Snapshot(cSC4View3DWin& v) { return v.renderProperties_; }
    static bool& Scrolling(cSC4View3DWin& v) { return v.scrolling_; }
    static bool& Stopping(cSC4View3DWin& v) { return v.scrollStopping_; }
    static float& ScrollSpeed(cSC4View3DWin& v) { return v.scrollSpeed_; }
    static float& ScrollStopSpeed(cSC4View3DWin& v) { return v.scrollStopSpeed_; }
    static float& ScrollAngle(cSC4View3DWin& v) { return v.scrollAngle_; }
    static bool& ScrollStopScaled(cSC4View3DWin& v) { return v.scrollStopScaled_; }
    static int& KeyboardScrollX(cSC4View3DWin& v) { return v.keyboardScrollX_; }
    static int& KeyboardScrollY(cSC4View3DWin& v) { return v.keyboardScrollY_; }
    static int& EdgeScrollX(cSC4View3DWin& v) { return v.edgeScrollX_; }
    static int& EdgeScrollY(cSC4View3DWin& v) { return v.edgeScrollY_; }
    static bool& SimHidden(cSC4View3DWin& v) { return v.simHiddenPaused_; }
    static cIGZUnknown*& FilterInfo(cSC4View3DWin& v) { return v.filterInfo_; }
    static std::uint32_t& ViewLevel(cSC4View3DWin& v) { return v.viewLevel_; }
    static int& ZoomMin(cSC4View3DWin& v) { return v.zoomMin_; }
    static int& ZoomMax(cSC4View3DWin& v) { return v.zoomMax_; }
    static std::uint8_t& FocusState(cSC4View3DWin& v) { return v.focusState_; }
    static std::uint32_t& LastModifiers(cSC4View3DWin& v) { return v.lastMessageModifiers_; }
    static std::uint16_t& Unknown1c8(cSC4View3DWin& v) { return v.unknown1c8_; }
    static std::uint32_t& SimHiddenTimeout(cSC4View3DWin& v) { return v.simHiddenTimeoutMs_; }
    static cISC4WinCalloutBox*& CursorTextBox(cSC4View3DWin& v) { return v.cursorTextBox_; }
    static std::list<CursorTextInfo>& CursorText(cSC4View3DWin& v) { return v.cursorText_; }
    static cIGZBuffer*& CursorTextIcon(cSC4View3DWin& v) { return v.cursorTextIcon_; }
    static cIGZBuffer*& CostIndicatorBuffer(cSC4View3DWin& v) { return v.costIndicatorBuffer_; }
    static gz::cIGZWinBMP*& CostIndicatorText(cSC4View3DWin& v) { return v.costIndicatorText_; }
    static std::uint32_t& MayorSubPanelState(cSC4View3DWin& v) { return v.mayorSubPanelState_; }
    static std::uint32_t& MayorSubPanel(cSC4View3DWin& v) { return v.mayorSubPanel_; }
    static bool* PanelShown(cSC4View3DWin& v) { return v.panelShown_; }
    static ISimulatorPauseSink*& Simulator(cSC4View3DWin& v) { return v.simulatorToPause_; }
    static int& CursorTextX(cSC4View3DWin& v) { return v.cursorTextX_; }
    static int& CursorTextY(cSC4View3DWin& v) { return v.cursorTextY_; }
    static cIGZWin*& MissionPanel(cSC4View3DWin& v) { return v.missionPanel_; }
    static cIGZWin*& SecondaryMenu(cSC4View3DWin& v) { return v.secondaryMenu_; }
    static std::uint32_t& SecondaryMenuState(cSC4View3DWin& v) { return v.secondaryMenuState_; }
    static cIGZUnknown*& ProgressScreen(cSC4View3DWin& v) { return v.progressScreen_; }
    static int& MouseDownX(cSC4View3DWin& v) { return v.mouseDownX_; }
    static int& MouseDownY(cSC4View3DWin& v) { return v.mouseDownY_; }
    static bool& DraggedPastDeadZone(cSC4View3DWin& v) { return v.draggedPastDeadZone_; }
    // The GUI block AddGUI builds.
    static cIGZWin*& PanelWindow(cSC4View3DWin& v) { return v.panelWindow_; }
    static gz::cIGZWinGen*& PanelInterface(cSC4View3DWin& v) { return v.panelInterface_; }
    static cIGZWin*& ToolbarWindow(cSC4View3DWin& v) { return v.toolbarWindow_; }
    static std::vector<cIGZWin*>& GuiWindows(cSC4View3DWin& v) { return v.guiWindows_; }
    static std::vector<cIGZWin*>* PanelWindows(cSC4View3DWin& v) { return v.panelWindows_; }
    static gz::cIGZWinGen** PanelAttach(cSC4View3DWin& v) { return v.panelAttach_; }
    static gz::cIGZWinProc*& PanelUIWinProc(cSC4View3DWin& v) { return v.panelUIWinProc_; }
    static INewsTickerWindow*& NewsTickerWinProc(cSC4View3DWin& v) { return v.newsTickerWinProc_; }
    static IMySimWindow*& MySimWinProc(cSC4View3DWin& v) { return v.mySimWinProc_; }
    static IBudgetUIWindow*& BudgetUIWinProc(cSC4View3DWin& v) { return v.budgetUIWinProc_; }
    static cIGZWin*& Window470(cSC4View3DWin& v) { return v.window470_; }
    static cIGZWin*& Window478(cSC4View3DWin& v) { return v.window478_; }
    static IGraphsViewWindow*& GraphsViewWinProc(cSC4View3DWin& v) { return v.graphsViewWinProc_; }
    static IBuildingSelectWindow*& BuildingSelectWinProc(cSC4View3DWin& v) { return v.buildingSelectWinProc_; }
    static cIGZWin*& Window4a0(cSC4View3DWin& v) { return v.window4a0_; }
    static cIGZWin*& Window4a8(cSC4View3DWin& v) { return v.window4a8_; }
    static cIGZWin*& Window4b0(cSC4View3DWin& v) { return v.window4b0_; }
    static gz::cIGZWinText*& DateText(cSC4View3DWin& v) { return v.dateText_; }
    static gz::cIGZWinText*& BudgetText(cSC4View3DWin& v) { return v.budgetText_; }
    static gz::cIGZWinText*& PopulationText(cSC4View3DWin& v) { return v.populationText_; }
    static IAdvisorPopupQueue*& AdvisorPopups(cSC4View3DWin& v) { return v.advisorPopups_; }
    static int& CurrentPanel(cSC4View3DWin& v) { return v.currentPanel_; }
};
}  // namespace sc4

namespace {

// ------------------------------------------------------------------------------------------------ GZ fakes
struct FakeCursor final : cIGZCursor {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
};
struct FakeCursors final : cIGZCursorManager {
    FakeCursor byId;
    std::uint32_t lastLoaded = 0;
    int loadCount = 0;
    cIGZCursor* LoadCursor(std::uint32_t id) override { lastLoaded = id; ++loadCount; return &byId; }
    void SetCursor(cIGZCursor*) override {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
};
struct FakeMgr final : cIGZWinMgr {
    cIGZWin* mainWin = nullptr;
    cIGZWin *focus = nullptr, *capture = nullptr;
    FakeCursors cursors;
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZWin* GetMainWindow() override { return mainWin; }
    void AppendToPlotStrategy(cIGZWin*) override {}
    void Plot() override {}
    void PostMessageToTarget(cIGZWin*, const cGZMessage&) override {}
    void AddWindowToValidList(cIGZWin*) override {}
    void RemoveWindowFromValidList(cIGZWin*) override {}
    void CleanUpWindowReferences(cIGZWin* w) override { if (focus == w) focus = nullptr; if (capture == w) capture = nullptr; }
    bool DestroyWindow(cIGZWin* w) override {
        if (!w) return false;
        if (w->GetParentWin()) return w->GetParentWin()->ChildRemove(w);
        return true;
    }
    bool IsWindowValid(cIGZWin*) override { return true; }
    cIGZWin* GetWindowFromPoint(int, int) override { return nullptr; }
    void SetCurrentMouseWin(cIGZWin*) override {}
    cIGZWin* GZGetCapture() override { return capture; }
    void GZSetCapture(cIGZWin* w) override { capture = w; }
    void GZReleaseCapture(cIGZWin* w) override { if (capture == w) capture = nullptr; }
    cIGZCursorManager* GetCursorManager() override { return &cursors; }
    cIGZWin* GZGetFocus() override { return focus; }
    void GZSetFocus(cIGZWin* w) override { focus = w; }
    cIGZKeyboard* GetKeyboard() override { return nullptr; }
};
// A window that answers the view-manager interface the view asks for (0x67c5bede).
struct ViewManagerWindow final : cGZWin {
    explicit ViewManagerWindow(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kView3DViewManagerIid) { *out = this; AddRef(); return true; }
        return cGZWin::QueryInterface(iid, out);
    }
};
struct FakeMessageServer final : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::uint32_t> sent, posted;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return refs ? --refs : 0; }
    bool MessageSend(cIGZMessage2* message) override {
        if (message) { sent.push_back(message->GetType()); Record(message); }
        return true;
    }
    bool MessagePost(cIGZMessage2* message, bool) override {
        if (message) { posted.push_back(message->GetType()); Record(message); }
        return true;
    }
    // The payload of the last message (the messages are freed right after the call, so copy it now).
    void* lastVoid1 = nullptr;
    void* lastVoid2 = nullptr;
    std::uint64_t lastData1 = 0, lastData2 = 0;
    void Record(cIGZMessage2* message) {
        lastVoid1 = lastVoid2 = nullptr;
        lastData1 = lastData2 = 0;
        if (auto* standard = dynamic_cast<cIGZMessage2Standard*>(message)) {
            lastVoid1 = standard->GetVoid1();
            lastVoid2 = standard->GetVoid2();
            lastData1 = standard->GetData1();
            lastData2 = standard->GetData2();
        }
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t) override { return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 0; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};
struct FakeNotification final : INotificationHelper3D {
    int addTypes = 0, addAll = 0, removeAll = 0;
    int lastCount = 0;
    std::uint32_t lastType = 0, firstType = 0;
    void AddMessageTypes(const std::uint32_t* types, int count) override {
        ++addTypes;
        lastCount = count;
        if (types && count > 0) { firstType = types[0]; lastType = types[count - 1]; }
    }
    void AddAllNotifications(cIGZMessageTarget2*) override { ++addAll; }
    void RemoveAllNotifications(cIGZMessageTarget2*) override { ++removeAll; }
};
struct FakeAccelerator final : cIGZWinKeyAccelerator {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    bool RegisterAccelerator(std::uint32_t, std::uint32_t) override { return true; }
    bool RegisterAccelerator(std::uint32_t, const cGZMessage&) override { return true; }
    bool UnregisterAccelerator(std::uint32_t) override { return true; }
    bool IsAcceleratorPresent(std::uint32_t, bool, bool) override { return false; }
    bool TestForAccelerator(const cGZMessage&, std::uint32_t&) override { return false; }
    bool TestForAccelerator(const cGZMessage&, cGZMessage&) override { return false; }
    // The overload the view's key path (0x100784ed5) uses: key, modifiers, key-down flag, out command.
    bool TestForAccelerator(std::uint32_t key, std::uint32_t modifiers, bool keyDown, std::uint32_t& command) override {
        lastKey = key;
        lastModifiers = modifiers;
        lastDown = keyDown;
        ++tests;
        if (found) command = 0x4242;
        return found;
    }
    bool TestForAccelerator(std::uint32_t, std::uint32_t, bool, cGZMessage&) override { return false; }
    int GetKeysForAccelerator(std::uint32_t, std::uint32_t*, std::uint32_t&) override { return 0; }
    bool ProcessAccelerator(const cGZMessage&) override { return false; }
    int tests = 0;
    std::uint32_t lastKey = 0, lastModifiers = 0;
    bool lastDown = false, found = false;
};
struct FakeCoverage final : ICoverageHelper3D {
    int inits = 0, shutdowns = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    void Init() override { ++inits; }
    void Shutdown() override { ++shutdowns; }
};
struct FakePauseSink final : ISimulatorPauseSink {
    int pauses = 0, resumes = 0, refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    void Pause() override { ++pauses; }
    void Resume() override { ++resumes; }
};
struct FakeProgressScreen final : cIGZUnknown {
    int refs = 0, shutdowns = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { if (refs) --refs; return refs; }
};
struct FakeRenderer final : IRenderer3D {
    int refs = 1;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool Init() override { return true; }
    void Shutdown() override { ++shutdowns; }
    void* GetDrawContext() override { return &drawContextMarker; }
    void* GetCamera() override { return nullptr; }
    void ForceFullRedraw() override { ++redraws; }
    void Draw() override { ++draws; }
    bool SetZoom(int zoom) override { lastZoom = zoom; ++zoomCalls; return true; }
    bool ZoomIn() override { ++zoomIn; return zoomInResult; }
    bool ZoomOut() override { ++zoomOut; return zoomOutResult; }
    bool SetRotation(int rotation) override { lastRotation = rotation; ++rotationCalls; return true; }
    bool RotateLeft() override { ++lefts; return true; }
    bool RotateRight() override { ++rights; return true; }
    bool SetZoomAndRotation(int zoom, int rotation) override {
        lastZoom = zoom; lastRotation = rotation; ++zoomAndRotation;
        return true;
    }
    bool PickTerrain(int x, int y, float* out3, bool always, bool arg5) override {
        lastPickX = x; lastPickY = y; pickTerrainAlways = always; pickTerrainArg5 = arg5; ++terrainPicks;
        if (!pickTerrainResult) return false;
        out3[0] = 1.5f; out3[1] = 2.5f; out3[2] = 3.5f;
        return true;
    }
    bool Pick(int x, int y, cISC4OccupantFilter*, s3d::cIS3DModelInstance*& model) override {
        lastPickX = x; lastPickY = y; ++picks;
        model = pickResult;
        if (model) model->AddRef();
        return pickResult != nullptr;
    }
    int shutdowns = 0, redraws = 0, draws = 0, zoomCalls = 0, zoomIn = 0, zoomOut = 0, rotationCalls = 0;
    int lefts = 0, rights = 0, zoomAndRotation = 0, terrainPicks = 0, picks = 0;
    int lastZoom = -1, lastRotation = -1, lastPickX = 0, lastPickY = 0;
    bool pickTerrainResult = false, pickTerrainAlways = true, pickTerrainArg5 = false;
    bool zoomInResult = true, zoomOutResult = true;
    std::uint32_t drawContextMarker = 0;
    s3d::cIS3DModelInstance* pickResult = nullptr;
};
struct FakeModelInstance final : s3d::cIS3DModelInstance {
    int refs = 1, releases = 0;
    cIGZUnknown* owner = nullptr;
    cS3DBoundingBox bounds;
    int worldBounds = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { ++releases; return --refs; }
    cIGZUnknown* GetOwner() override { return owner; }
    void SetOwner(cIGZUnknown*) override {}
    void* GetCustomData() override { return nullptr; }
    void SetCustomData(void*) override {}
    void GetBounds(cS3DBoundingBox& out) override { out = bounds; }
    void GetWorldBounds(cS3DBoundingBox& out) override { out = bounds; ++worldBounds; }
    void GetTransform(cS3DTransform&) override {}
    void SetTransform(const cS3DTransform&) override {}
    bool Pick(cSC4PickContext*) override { return false; }
    int Draw(SC4DrawContext*, bool) override { return 0; }
    int GetProperties() override { return 0; }
    void SetProperties(int) override {}
    s3d::SC4DrawAttributes GetDrawAttributes() const override { return {}; }
    void SetDrawAttributes(const s3d::SC4DrawAttributes&) override {}
    void GetSortValue(std::uint32_t& out) const override { out = 0; }
    s3d::cIS3DResModel* GetResModel() const override { return nullptr; }
};
struct FakeOccupant final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 0x26bb767e) return false;
        *out = this; AddRef(); return true;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { if (refs) --refs; return refs; }
};

// ------------------------------------------------------------------------------------------------ the seam doubles
struct FakeCityMouseRouter final : ICityMouseRouter {
    bool active = false, allowEdgeScroll = true, handled = true;
    int calls = 0, lastFlags = 0;
    int mapX = 0, mapY = 0;                       // nonzero: rewrite the point like the city router does
    bool ProcessMouse(int& x, int& y, std::uint32_t flags) override {
        ++calls;
        lastFlags = static_cast<int>(flags);
        if (mapX) x = mapX;
        if (mapY) y = mapY;
        return handled;
    }
    bool IsActive() override { return active; }
    bool AllowsEdgeScroll() override { return allowEdgeScroll; }
};
// The cursor-text callout box (+0x5d0) and its window: CursorTextChanged drives the box's own interface and
// the window's ShowWindow / HideWindow / InvalidateSelfAndParents.
struct BoxWindow final : cGZWin {
    explicit BoxWindow(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    int shows = 0, hides = 0, invalidates = 0;
    void ShowWindow() override { ++shows; cGZWin::ShowWindow(); }
    void HideWindow() override { ++hides; cGZWin::HideWindow(); }
    void InvalidateSelfAndParents() override { ++invalidates; cGZWin::InvalidateSelfAndParents(); }
};
struct FakeCalloutBox final : cISC4WinCalloutBox {
    explicit FakeCalloutBox(cIGZWinMgr* mgr = nullptr) {
        window_ = new BoxWindow(mgr);
        window_->AddRef();
    }
    ~FakeCalloutBox() { if (window_) window_->Release(); }
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    cIGZWin* AsIGZWin() override { return window_; }
    void SetBackgroundImage(cIGZBuffer* image, int a, int b) override {
        background = image; ++backgrounds; bgA = a; bgB = b;
    }
    cIGZBuffer* GetBackgroundImage() const override { return background; }
    void SetTitle(const cIGZString& text) override { line1 = text.ToChar(); ++line1Sets; }
    const cIGZString& GetTitle() const override { return title_; }
    void SetText(const cIGZString& text) override { line2 = text.ToChar(); ++line2Sets; }
    const cIGZString& GetText() const override { return text_; }
    void SetTitleFontStyle(std::uint32_t style) override { titleFont = style; }
    void SetTextFontStyle(std::uint32_t style) override { textFont = style; }
    void SetIcon(cIGZBuffer* image) override { icon = image; ++iconSets; }
    cIGZBuffer* GetIcon() const override { return icon; }
    void SetIconMeter(cIGZBuffer*, std::uint32_t, float) override {}
    void SetPointOfInterest(const cRZPoint& value) override { point = value; ++positions; }
    const cRZPoint& GetPointOfInterest() const override { return point; }
    void SetAvoidanceRect(const cRZRect&) override {}
    const cRZRect& GetAvoidanceRect() const override { return avoidance_; }
    void SetConstraintRect(const cRZRect&) override {}
    const cRZRect& GetConstraintRect() const override { return constraint_; }

    int refs = 0;
    BoxWindow* window_ = nullptr;
    cIGZBuffer* background = nullptr;
    cIGZBuffer* icon = nullptr;
    int backgrounds = 0, bgA = 0, bgB = 0;
    std::string line1, line2;
    cRZString title_, text_;
    cRZPoint point;
    cRZRect avoidance_, constraint_;
    std::uint32_t titleFont = 0, textFont = 0;
    int positions = 0, line1Sets = 0, line2Sets = 0, iconSets = 0;
};
struct FakeTerrainEditor final : ITerrainEditor {
    float height = 0.0f;
    int type = 3;
    int heights = 0, heightsAt = 0, typeSets = 0, mode158 = 0, mode160 = 0, cancels = 0, activeFlags = 0;
    int brushCalls = 0, brush = -1, brushResultType = 0x77;
    bool brushResult = true;
    bool activeFlag = false;
    float TerrainHeight(float, float) override { ++heights; return height; }
    float TerrainHeightAt(float, float) override { ++heightsAt; return height + 1.0f; }
    void SetTerrainType(int value) override { type = value; ++typeSets; }
    int TerrainType() override { return type; }
    void CreateFlatAreas(bool smoothen) override { mode158 = smoothen ? 1 : 0; }
    void CreateShoreCliffs(bool redisplay) override { mode160 = redisplay ? 1 : 0; }
    float DefaultRaiseLowerAmount() override { ++cancels; return 10.0f; }
    void ChangeTerrainLevel(bool raise, float) override { activeFlag = raise; ++activeFlags; }
    bool TerrainTypeForBrush(int value, std::uint32_t& out) override {
        ++brushCalls;
        brush = value;
        out = static_cast<std::uint32_t>(brushResultType);
        return brushResult;
    }
};
struct FakeAnimationRecorder final : cGZWin {
    explicit FakeAnimationRecorder(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    int refs = 0, childAdds = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { ++refs; return static_cast<std::uint32_t>(refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
};


// The tool base class' Init talks to the view-input service locator.
struct FakeViewInputView final : IViewInputView {
    int refs = 0, ends = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { if (refs) --refs; return refs; }
    void EndInput(int) override { ++ends; }
    cSC4ViewInputControl* TopControl() override { return nullptr; }
};
struct FakeViewInputHost final : IViewInputHost {
    FakeViewInputView view;
    cIGZWinMgr* manager = nullptr;
    int sounds = 0;
    IViewInputView* View3D() override { return &view; }
    cIGZWinMgr* WindowManager() override { return manager; }
    void PlaySound(int, std::uint32_t) override { ++sounds; }
};

// A buffer (the toolbar skin, the callout background, the cursor icon, the cost indicator's 0x80x0x20 target).
struct FakeBuffer final : cIGZBuffer {
    int refs = 0;
    std::uint32_t width = 0x80, height = 0x20, transparency = 0;
    int inits = 0, transparencySets = 0;
    cRZRect area{0, 0, 0x80, 0x20};
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool Init(std::uint32_t w, std::uint32_t h, cGZBufferColorType) override {
        width = w; height = h; area = cRZRect{0, 0, static_cast<int>(w), static_cast<int>(h)}; ++inits; return true;
    }
    bool Shutdown() override { return true; }
    bool Uninitialize() override { return true; }
    bool Lock(eLockFlags) override { return true; }
    bool Unlock(eLockFlags) override { return true; }
    bool IsLocked() const override { return false; }
    std::uint32_t Width() const override { return width; }
    std::uint32_t Height() const override { return height; }
    const cRZRect* GetBufferArea() const override { return &area; }
    void GetBufferArea(cRZRect& out) const override { out = area; }
    std::uint32_t GetMemoryUsage() const override { return 0; }
    cGZBufferColorType GetColorType() const override { return static_cast<cGZBufferColorType>(1); }
    void GetColorType(cGZBufferColorType& out) const override { out = static_cast<cGZBufferColorType>(1); }
    std::uint32_t GetBitsPerPixel() const override { return 32; }
    std::uint32_t GetBytesPerPixel() const override { return 4; }
    bool Fill(std::uint32_t, const cRZRect&, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, int, int, int, int, const cRZRect*) override { return true; }
    bool Fill(std::uint32_t, const cRZRect*) override { return true; }
    std::uint32_t GetPixel(std::uint32_t, std::uint32_t) const override { return 0; }
    void SetPixel(std::uint32_t, std::uint32_t, std::uint32_t) override {}
    void SetTransparency(std::uint32_t color) override { transparency = color; ++transparencySets; }
    bool GetTransparency() const override { return transparency != 0; }
    bool IsPixelTransparent(std::uint32_t, std::uint32_t) override { return false; }
    bool GetTransparentColor(std::uint32_t& color) const override { color = transparency; return true; }
    void RemoveTransparency() override {}
    bool DrawLine(int, int, int, int, int, std::uint32_t) override { return true; }
    bool Blt(cIGZBuffer*, const cRZRect&, const cRZRect&, const cRZRect*) override { return true; }
    std::uint32_t ConvertRGBValueToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
    }
    bool ConvertNativeValueToRGB(std::uint32_t, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = g = b = 0; return true;
    }
    std::uint32_t ConvertRGBAValueToNative(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t) override {
        return ConvertRGBValueToNative(r, g, b);
    }
    bool ConvertNativeValueToRGBA(std::uint32_t, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, std::uint8_t& a) override {
        r = g = b = a = 0; return true;
    }
    void* GetColorSurfaceBits() override { return nullptr; }
    std::uint32_t GetColorSurfaceStride() override { return 0; }
    std::uint32_t GetChangeCounter() override { return 0; }
    cIGZBufferHardwareCache* GetHardwareCache() override { return nullptr; }
    bool SetHardwareCache(cIGZBufferHardwareCache*) override { return true; }
    bool IsReady() override { return true; }
};

// The cost indicator's cGZWinBMP window.
struct FakeBmpWindow final : cGZWin, gz::cIGZWinBMP {
    explicit FakeBmpWindow(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    cIGZBuffer* bmp = nullptr;
    std::uint32_t flags = 0;
    int setBmps = 0, imageAreas = 0;
    cRZRect imageArea;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinBMP) { *out = static_cast<gz::cIGZWinBMP*>(this); AddRef(); return true; }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return this; }
    void SetBMP(cIGZBuffer* buffer) override { bmp = buffer; ++setBmps; }
    bool GetBMP(std::uint32_t, void** out) const override { if (!bmp) return false; *out = bmp; return true; }
    void SetAlphaBlending(bool, float) override {}
    void GetAlphaBlending(bool& enable, float& alpha) const override { enable = false; alpha = 0.5f; }
    void SetImageArea(const cRZRect& area) override { imageArea = area; ++imageAreas; }
    void GetImageArea(cRZRect& area) const override { area = imageArea; }
    bool GetWinBMPFlag(int flag) const override { return (flags & static_cast<std::uint32_t>(flag)) != 0; }
    void SetWinBMPFlag(int flag, bool on) override {
        if (on) flags |= static_cast<std::uint32_t>(flag); else flags &= ~static_cast<std::uint32_t>(flag);
    }
};

// A script window: a cGZWin that also answers cIGZWinGen (every panel window AddGUI creates).
struct FakeGenWindow final : cGZWin, gz::cIGZWinGen {
    explicit FakeGenWindow(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinGen || iid == 0x5386d516) {
            *out = static_cast<gz::cIGZWinGen*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return this; }
    std::uint32_t GetCLSID() const override { return 0; }
    bool GetWinGenFlag(int) const override { return false; }
    bool SetWinGenFlag(int, bool) override { return true; }
    bool SetWinProc(gz::cIGZWinProc* proc) override { winProc = proc; ++winProcs; return true; }
    gz::cIGZWinProc* GetWinProc() const override { return winProc; }
    bool GetWinProcAs(std::uint32_t, void**) const override { return false; }
    bool GetUserArea(cRZRect& out) const override { out = {}; return true; }
    bool SetUserArea(const cRZRect& area) override { userArea = area; ++userAreas; return true; }
    bool SetBackgroundBitmap(gz::BGBltType, cIGZBuffer*) override { return true; }
    bool GetBackgroundBitmap(gz::BGBltType&, cIGZBuffer*&) const override { return false; }
    bool SetBackgroundBitmap(cIGZBuffer*) override { return true; }
    bool GetBackgroundBitmap(cIGZBuffer** out) const override { *out = nullptr; return true; }
    bool SetBackgroundBlitType(gz::BGBltType) override { return true; }
    bool GetBackgroundBlitType(gz::BGBltType& type) const override { type = gz::kBGTiled; return true; }
    bool SetUserData(std::uint32_t, std::uint32_t) override { return true; }
    bool GetUserData(std::uint32_t, std::uint32_t& value) const override { value = 0; return true; }
    bool SetUserData(std::uint32_t) override { return true; }
    std::uint32_t GetUserData() const override { return 0; }
    void SetUserData(std::uint32_t, cIGZUnknown*) override {}
    bool GetUserData(std::uint32_t, std::uint32_t, void**) override { return false; }
    void RemoveUserData(std::uint32_t) override {}
    bool SetGutters(std::uint8_t, std::uint8_t) override { return true; }
    bool GetGutters(int& x, int& y) override { x = y = 0; return true; }
    void SetGutters(std::int8_t, std::int8_t, std::int8_t, std::int8_t) override {}
    bool GetGutters(int& l, int& t, int& r, int& b) const override { l = t = r = b = 0; return true; }
    bool AddColorBar(int, int, int, const cRZRect&) override { return true; }
    void AddDividerH(int, int) override {}
    bool RemoveColorBar(int) override { return true; }
    gz::cIGZWinProc* winProc = nullptr;
    cRZRect userArea;
    int winProcs = 0, userAreas = 0;
};

// The compass window: a cGZWin that answers cISC4WinAlertBorder.
struct FakeCompassWindow final : cGZWin, cISC4WinAlertBorder {
    explicit FakeCompassWindow(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    cIGZBuffer* image = nullptr;
    int images = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4WinAlertBorder) { *out = static_cast<cISC4WinAlertBorder*>(this); AddRef(); return true; }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return this; }
    void SetImage(cIGZBuffer* buffer) override { image = buffer; ++images; }
};

// The window procedures AddGUI allocates through the host.
struct FakePanelUIProc final : gz::cIGZWinProc {
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
};
struct FakeNewsTicker final : INewsTickerWindow {
    int refs = 0, inits = 0, shows = 0, hides = 0;
    gz::cIGZWinGen* window = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
    bool Init(gz::cIGZWinGen* value) override { window = value; ++inits; return true; }
    void ShowTicker() override { ++shows; }
    void HideTicker() override { ++hides; }
};
struct FakeMySimProc final : IMySimWindow {
    int refs = 0, inits = 0, shows = 0, hides = 0;
    gz::cIGZWinGen* panel = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
    void Init() override { ++inits; }
    void Show() override { ++shows; }
    void Hide() override { ++hides; }
};
struct FakeBudgetProc final : IBudgetUIWindow {
    int refs = 0, inits = 0, shows = 0, hides = 0;
    bool visible = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    void Init() override { ++inits; }
    void ShowWindow() override { ++shows; visible = true; }
    void HideWindow() override { ++hides; visible = false; }
    bool IsVisible() override { return visible; }
};
struct FakeGraphsProc final : IGraphsViewWindow {
    int refs = 0, inits = 0, shows = 0, hides = 0, graphTypes = 0;
    std::uint32_t lastGraphType = 0;
    bool visible = false;
    cIGZWin* window = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
    bool Init() override { ++inits; return true; }
    void ShowWindow() override { ++shows; visible = true; }
    void HideWindow() override { ++hides; visible = false; }
    bool IsVisible() override { return visible; }
    void ShowGraphType(std::uint32_t option) override { lastGraphType = option; ++graphTypes; }
};
struct FakeBuildingSelectProc final : IBuildingSelectWindow {
    int refs = 0, inits = 0, shows = 0, hides = 0;
    bool visible = false;
    cIGZWin* window = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool DoWinProcMessage(cIGZWin*, cGZMessage&) override { return false; }
    bool Init() override { ++inits; return true; }
    void ShowWindow() override { ++shows; visible = true; }
    void HideWindow() override { ++hides; visible = false; }
    bool IsVisible() override { return visible; }
};
struct FakeAdvisorQueue final : IAdvisorPopupQueue {
    int refs = 0, inits = 0, shutdowns = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool Init() override { ++inits; return true; }
    bool Shutdown() override { ++shutdowns; return true; }
};
struct FakeAdvisorWindow final : IAdvisorWindow {
    int refs = 0, shows = 0, hides = 0;
    bool visible = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    void ShowAdvisorWindow() override { ++shows; visible = true; }
    void HideAdvisorWindow() override { ++hides; visible = false; }
    bool IsAdvisorWindowVisible() override { return visible; }
};
// The simulator's pause pair plus the speed / paused getters UpdateSpeedButtons reads.
struct FakeSpeedSimulator final : ISimulatorPauseSink {
    int refs = 1, pauses = 0, resumes = 0;
    int speed = 1;
    bool paused = false;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(--refs); }
    void Pause() override { ++pauses; }
    void Resume() override { ++resumes; }
    int Speed() override { return speed; }
    bool IsPaused() override { return paused; }
    int userPauses = 0, userResumes = 0, speedSets = 0;
    void UserPause() override { ++userPauses; paused = true; }
    void UserResume() override { ++userResumes; paused = false; }
    void SetSpeed(int value) override { ++speedSets; speed = value; }
};

// A toggle button the panel helpers can find (gz::kGZIID_cIGZWinBtn): it records the toggle state.
struct FakePanelButton final : cGZWin, gz::cIGZWinBtn {
    explicit FakePanelButton(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinBtn) { *out = static_cast<gz::cIGZWinBtn*>(this); AddRef(); return true; }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return static_cast<cIGZWin*>(static_cast<cGZWin*>(this)); }
    bool GetBtnFlag(int flag) const override { return (flags & flag) != 0; }
    bool SetBtnFlag(int flag, bool on) override { if (on) flags |= flag; else flags &= ~flag; return true; }
    bool ToggleOn() override { flags |= gz::kBtnOn; ++toggles; return true; }
    bool ToggleOff() override { flags &= ~gz::kBtnOn; ++toggles; return true; }
    bool IsOn() const override { return (flags & gz::kBtnOn) != 0; }
    bool SetChecked(bool checked) override { return SetBtnFlag(gz::kBtnOn, checked); }
    bool IsChecked() const override { return IsOn(); }
    int GetStyle() override { return lastStyle; }
    bool SetStyle(int style, cIGZBuffer* image, const cRZRect* area) override {
        lastStyle = style;
        lastImage = image;
        hasArea = area != nullptr;
        if (area) lastArea = *area;
        ++styles;
        return true;
    }
    bool SetButtonStyle(int) override { return true; }
    int GetImageW() const override { return 1; }
    int GetImageH() const override { return 1; }
    bool GetImageArea(cRZRect& out) const override { out = area; return true; }
    bool SetBuffer(cIGZBuffer*) override { return true; }
    bool GetBuffer(cIGZBuffer*& out) const override { out = image; return true; }
    bool SetFont(gz::IWinButtonFont*) override { return true; }
    bool SetFontStyle(std::uint32_t) override { return true; }
    bool GetFontStyle(std::uint32_t& style) override { style = 0; return true; }
    bool SetFontColor(int, std::uint32_t) override { return true; }
    bool GetFontColor(int, std::uint32_t& native) const override { native = 0; return true; }
    bool SetFontColor(int, gz::cRZColor) override { return true; }
    bool GetFontColor(int, gz::cRZColor&) const override { return true; }
    bool ClearFontColor(int) override { return true; }
    bool SetCaptionStyle(int) override { return true; }
    bool GetCaptionStyle(int& style) const override { style = 0; return true; }
    bool SetGutters(std::uint8_t, std::uint8_t) override { return true; }
    bool GetGutters(int& x, int& y) const override { x = y = 0; return true; }
    bool SetGutters(std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t) override { return true; }
    bool GetGutters(int& l, int& t, int& r, int& b) const override { l = t = r = b = 0; return true; }
    bool FitSizeToContent() override { return true; }
    bool GetMinMaxInfo(cRZRect& out) override { out = {1, 1, 1, 1}; return true; }
    bool SetTipText(const cIGZString&) override { return true; }
    bool GetTipText(cIGZString&) const override { return true; }
    bool ClearTipText() override { return true; }
    bool SetTipPlacementType(int) override { return true; }
    bool GetTipPlacementType(int& type) const override { type = 0; return true; }
    bool SetTipPlacementOffsets(int, int) override { return true; }
    bool GetTipPlacementOffsets(int& x, int& y) const override { x = y = 0; return true; }
    bool SetTipColor(int, std::uint8_t, std::uint8_t, std::uint8_t) override { return true; }
    bool GetTipColor(int, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const override { r = g = b = 0; return true; }
    void Flash(std::uint32_t, std::uint32_t) override {}
    bool SetSoundID(int, std::uint32_t) override { return true; }
    std::uint32_t GetSoundID(int) const override { return 0; }
    int flags = 0, toggles = 0;
    int lastStyle = 0, styles = 0;
    bool hasArea = false;
    cRZRect lastArea{0, 0, 0, 0};
    cRZRect area{0, 0, 0, 0};
    cIGZBuffer* image = nullptr;
    cIGZBuffer* lastImage = nullptr;
};

// ------------------------------------------------------------------------------------------------ the host
struct FakeHost final : IView3DWinHost {
    FakeMessageServer server;
    FakeNotification notifications;
    FakeAccelerator accelerator;
    FakeCoverage coverage;
    FakePauseSink pauseSink;
    FakeProgressScreen progress;
    std::uint8_t preferences[0x1734] = {};
    bool cityExists = false, cheatPresent = false, lotPlop = false, progressWanted = false;
    int virtualZoomLimit = 5;
    int onView3DCreate = 0, onView3DShutdown = 0;
    std::vector<std::uint32_t> registeredCheats, unregisteredCheats;
    std::vector<std::string> registeredCheatStrings;
    int addTargets = 0, removeTargets = 0, registerTables = 0, unregisterTables = 0;
    int lotConfigShutdowns = 0, modelMakerShutdowns = 0, cameraShutdowns = 0;
    int clockShutdowns = 0, validationShutdowns = 0, progressShutdowns = 0;
    std::vector<std::uint32_t> gameMessages;
    int lightingHighlights = 0, lightingRestores = 0;
    int costUpdates = 0;
    float lastCostDt = -1.0f;
    bool popupSafe = false, popupEnabled = false;
    std::uint32_t frameCounter = 0x1234;
    int hoverUpdates = 0;
    int cursorTextUpdates = 0, lastCursorX = 0, lastCursorY = 0;
    int frameMarkers = 0;
    std::uint32_t lastFrameMarker = 0;
    std::vector<std::uint32_t> cheatRuns;
    std::vector<std::string> cheatTexts;
    int catalogInvokes = 0;
    std::uint32_t catalogItem = 0, catalogParam = 0;
    bool catalogFlag = false;
    int acceleratorCommands = 0;
    std::uint32_t lastAcceleratorCommand = 0;
    int uiFilters = 0, adviceLinks = 0, urlHandles = 0;
    bool urlResult = false;
    // The new seams.
    FakeCityMouseRouter router;
    FakeCalloutBox cursorWindow;
    FakeTerrainEditor terrain;
    bool canZone = true, gsSelfScroll = false;
    int progressInits = 0, progressCaptions = 0, lastCaptionId = -1;
    std::string lastCaption;
    FakeAnimationRecorder* recorder = nullptr;
    int recordingSetups = 0;
    std::string recordingDirectory;
    int terrainDialogs = 0;
    std::vector<int> dialogResult;              // what RunChangeTerrainDialog leaves in `type`
    int globalTerrainSounds = 0;
    cSC4ViewInputControl* toolToCreate = nullptr;
    std::vector<std::uint32_t> toolTypes;
    std::vector<bool> toolFlags;
    int cityScrollSets = 0, lastScrollX = 0, lastScrollY = 0;
    int contourAdds = 0, contourRemoves = 0, lastContourRemoved = 0, nextContour = 0x99;
    bool projectResult = true;
    cS3DVector3 projectedPoint;
    // In the game both accessors answer the one render-property object (SL::spRenderProperties and
    // cISC4App vslot 0x160 are the same instance).
    cSC4RenderProperties globalProperties;

    cIGZMessageServer2* MessageServer2() override { return &server; }
    std::uint8_t* Preferences() override { return preferences; }
    bool CityExists() override { return cityExists; }
    void PostGameMessage(std::uint32_t type) override { gameMessages.push_back(type); }
    INotificationHelper3D* NotificationHelper() override { return &notifications; }
    void RegisterCommandTable() override { ++registerTables; }
    void UnregisterCommandTable() override { ++unregisterTables; }
    void AddCheatCodeTarget(cIGZWin*) override { ++addTargets; }
    void RemoveCheatCodeTarget(cIGZWin*) override { ++removeTargets; }
    bool RegisterCheatCode(std::uint32_t id, const char* text) override {
        registeredCheats.push_back(id);
        registeredCheatStrings.push_back(text ? text : "");
        return true;
    }
    bool UnregisterCheatCode(std::uint32_t id) override { unregisteredCheats.push_back(id); return true; }
    bool CheatCodeManagerPresent() override { return cheatPresent; }
    bool LotPlopCheatsEnabled() override { return lotPlop; }
    cIGZWinKeyAccelerator* CreateKeyAccelerator() override { accelerator.AddRef(); return &accelerator; }
    cSC4RenderProperties* GlobalRenderProperties() override { return &globalProperties; }
    cSC4RenderProperties* AppRenderProperties() override { return &globalProperties; }
    void SetScrollLightingHighlight() override { ++lightingHighlights; }
    void RestoreLighting() override { ++lightingRestores; }
    int VirtualZoomLimit() override { return virtualZoomLimit; }
    bool ProgressScreenWanted() override { return progressWanted; }
    cIGZUnknown* CreateProgressScreen(cIGZWin*, const cRZRect&, bool) override { return &progress; }
    void InitProgressScreen(cIGZUnknown*) override { ++progressInits; }
    void SetProgressCaption(cIGZUnknown*, int id, const cIGZString& text) override {
        ++progressCaptions;
        lastCaptionId = id;
        lastCaption = text.ToChar();
    }
    bool UserDataDirectory(cIGZString& out) override {
        out.AssignFromCString("/Users/test/SimCity 4");
        return true;
    }
    // The panel / GUI seams.
    std::string cityName = "Testville";
    int visualTimeMode = 0;
    int population = 123456;
    std::int64_t funds = 1234;
    int numberFormats = 0, moneyFormats = 0;
    std::string lastFormatted;
    std::uint32_t lastNative = 0;
    int budgetOpenCalls = 0, budgetRefreshes = 0;
    bool budgetOpen = false;
    int newsShutdowns = 0, mysimShutdowns = 0, budgetShutdowns = 0, graphsShutdowns = 0;
    int buildingShutdowns = 0;
    void GetCityName(cIGZString& out) override { out.AssignFromCString(cityName.c_str()); }
    int VisualTimeMode() override { return visualTimeMode; }
    int CityPopulation() override { return population; }
    std::int64_t CityFunds() override { return funds; }
    void FormatNumber(std::int64_t value, cIGZString& out) override {
        ++numberFormats;
        lastFormatted = std::to_string(value);
        out.AssignFromCString(lastFormatted.c_str());
    }
    void MakeMoneyString(std::int64_t value, cIGZString& out) override {
        ++moneyFormats;
        lastFormatted = "$" + std::to_string(value);
        out.AssignFromCString(lastFormatted.c_str());
    }
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        lastNative = (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
        return lastNative;
    }
    bool BudgetDialogOpen(cIGZUnknown*) override { ++budgetOpenCalls; return budgetOpen; }
    void RefreshBudgetDialog(cIGZUnknown*) override { ++budgetRefreshes; }
    void ShutdownNewsTickerWinProc(cIGZUnknown*) override { ++newsShutdowns; }
    void ShutdownMySimWinProc(cIGZUnknown*) override { ++mysimShutdowns; }
    void ShutdownBudgetUIWinProc(cIGZUnknown*) override { ++budgetShutdowns; }
    void ShutdownGraphsViewWinProc(cIGZUnknown*) override { ++graphsShutdowns; }
    void ShutdownBuildingSelectWinProc(cIGZUnknown*) override { ++buildingShutdowns; }
    // ---- AddGUI's construction seams (0x1007892fc and its callees)
    std::map<std::uint32_t, cIGZWin*> scriptWindows;                 // by window id
    std::vector<std::pair<cGZPersistResourceKey, std::uint32_t>> scriptRequests;
    std::map<std::uint32_t, cIGZBuffer*> images;                     // by instance
    std::vector<std::uint32_t> imageRequests;
    int toolbarButtons = 0, compassWindows = 0, compassAttaches = 0, mapViewInits = 0;
    std::vector<std::uint32_t> localisedStrings;
    FakePanelUIProc* panelProc = nullptr;
    FakeNewsTicker* newsProc = nullptr;
    FakeMySimProc* mySimProc = nullptr;
    FakeBudgetProc* budgetProc = nullptr;
    FakeGraphsProc* graphsProc = nullptr;
    FakeBuildingSelectProc* buildingProc = nullptr;
    FakeAdvisorQueue* advisorQueue = nullptr;
    FakeAdvisorWindow advisorWindow;
    FakeSpeedSimulator speedSimulator;
    FakeBmpWindow* bmpWindow = nullptr;
    FakeCompassWindow* compassWindow = nullptr;
    FakePanelButton* toolbarButton = nullptr;
    int imageBuffers = 0;
    bool disasterAlert = false, situationAlert = false, simulatorBusy = false;
    bool keysDown[0x20] = {};
    std::vector<std::uint32_t> commands;
    int establishDialogs = 0;
    bool establishDialog = true;

    // The four nSC4UI helpers AddGUI runs around every script window (they are recovered in SC4UI.cpp, which
    // this test links; the view reaches them through the host like every other outside call).
    std::uint32_t GetWindowChildrenSet(cIGZWin* window, std::set<cIGZWin*>& children) override {
        return nSC4UI::GetWindowChildrenSet(window, children);
    }
    std::uint32_t GetWindowChildrenNotInSet(cIGZWin* window, const std::set<cIGZWin*>& excluded,
                                            std::set<cIGZWin*>& out) override {
        return nSC4UI::GetWindowChildrenNotInSet(window, excluded, out);
    }
    void AlignWindowSetToWindow(const std::set<cIGZWin*>& windows, cIGZWin* parent) override {
        nSC4UI::AlignWindowSetToWindow(windows, parent);
    }
    void ClearAcceptFocusFlagRecursive(cIGZWin* window) override { nSC4UI::ClearAcceptFocusFlagRecursive(window); }

    cIGZWin* CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id) override {
        scriptRequests.emplace_back(key, id);
        auto it = scriptWindows.find(id);
        if (it == scriptWindows.end() || !it->second) return nullptr;
        if (parent) parent->ChildAdd(it->second);
        it->second->AddRef();
        return it->second;
    }
    bool CreateWindowFromScript(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id,
                                std::uint32_t iid, void** out) override {
        cIGZWin* window = CreateWindowFromScript(key, parent, id);
        if (!window) return false;
        const bool ok = window->QueryInterface(iid, out);
        window->Release();                     // QueryInterface kept its own reference
        return ok;
    }
    cIGZBuffer* LoadImageBuffer(std::uint32_t, std::uint32_t, std::uint32_t instance) override {
        imageRequests.push_back(instance);
        auto it = images.find(instance);
        if (it == images.end()) return nullptr;
        it->second->AddRef();
        return it->second;
    }
    cIGZWinMgr* windowManager = nullptr;
    cIGZWin* CreateToolbarButton() override {
        ++toolbarButtons;
        if (!toolbarButton) { toolbarButton = new FakePanelButton(windowManager); toolbarButton->AddRef(); }
        return toolbarButton;
    }
    cIGZWin* CreateCompassWindow() override {
        ++compassWindows;
        if (!compassWindow) { compassWindow = new FakeCompassWindow(windowManager); compassWindow->AddRef(); }
        return compassWindow;
    }
    void LocalisedString(std::uint32_t, std::uint32_t instance, cRZString& out) override {
        localisedStrings.push_back(instance);
        out.AssignFromCString("Show panel");
    }
    gz::cIGZWinProc* CreatePanelUIWinProc() override {
        if (!panelProc) { panelProc = new FakePanelUIProc(); panelProc->AddRef(); }
        return panelProc;
    }
    INewsTickerWindow* CreateNewsTickerWinProc() override {
        if (!newsProc) { newsProc = new FakeNewsTicker(); newsProc->AddRef(); }
        return newsProc;
    }
    IMySimWindow* CreateMySimWinProc(gz::cIGZWinGen* panel) override {
        if (!mySimProc) { mySimProc = new FakeMySimProc(); mySimProc->AddRef(); }
        mySimProc->panel = panel;
        return mySimProc;
    }
    IBudgetUIWindow* CreateBudgetUIWinProc() override {
        if (!budgetProc) { budgetProc = new FakeBudgetProc(); budgetProc->AddRef(); }
        return budgetProc;
    }
    IGraphsViewWindow* CreateGraphsViewWinProc(cIGZWin* window) override {
        if (!graphsProc) { graphsProc = new FakeGraphsProc(); graphsProc->AddRef(); }
        graphsProc->window = window;
        return graphsProc;
    }
    IBuildingSelectWindow* CreateBuildingSelectWinProc(cIGZWin* window) override {
        if (!buildingProc) { buildingProc = new FakeBuildingSelectProc(); buildingProc->AddRef(); }
        buildingProc->window = window;
        return buildingProc;
    }
    IAdvisorPopupQueue* CreateAdvisorPopupTracker() override {
        if (!advisorQueue) { advisorQueue = new FakeAdvisorQueue(); advisorQueue->AddRef(); }
        return advisorQueue;
    }
    void AttachCompassWinProc(gz::cIGZWinGen*) override { ++compassAttaches; }
    cISC4WinCalloutBox* CreateCalloutBox() override { cursorWindow.AddRef(); return &cursorWindow; }
    gz::cIGZWinBMP* CreateWinBMP() override {
        if (!bmpWindow) { bmpWindow = new FakeBmpWindow(windowManager); bmpWindow->AddRef(); }
        return bmpWindow;
    }
    void InitMapView(cIGZWin*) override { ++mapViewInits; }
    bool IsKeyDownNow(std::uint32_t key) override { return key < 0x20 && keysDown[key]; }
    void ExecuteCommand(std::uint32_t command) override { commands.push_back(command); }
    bool ShowEstablishCityDialog() override { ++establishDialogs; return establishDialog; }
    bool DisasterAlertActive() override { return disasterAlert; }
    bool SituationAlertActive() override { return situationAlert; }
    bool SimulatorBusy() override { return simulatorBusy; }
    IAdvisorWindow* AdvisorWindow() override { advisorWindow.AddRef(); return &advisorWindow; }
    cIGZBuffer* CreateImageBuffer() override {
        ++imageBuffers;
        if (!imageBuffers) return nullptr;
        auto* buffer = new FakeBuffer();
        buffer->AddRef();
        return buffer;
    }
    cIGZWin* CreateWinAnimationRecorder() override {
        if (!recorder) return nullptr;
        recorder->AddRef();
        return recorder;
    }
    void RecorderSetAnimationOutputDirectory(cIGZWin*, const cIGZString& directory, bool) override {
        ++recordingSetups;
        recordingDirectory = directory.ToChar();
    }
    ICityMouseRouter* CityMouseRouter() override { return &router; }
    bool CanInvokeZoningTool() override { return canZone; }
    ITerrainEditor* TerrainEditor() override { return &terrain; }
    bool GraphicsSystemAllowsSelfScroll() override { return gsSelfScroll; }
    void RunChangeTerrainDialog(int& type) override {
        ++terrainDialogs;
        if (!dialogResult.empty()) type = dialogResult[0];
    }
    cSC4ViewInputControl* CreateZoningTool(std::uint32_t type) override {
        toolTypes.push_back(type);
        return toolToCreate;
    }
    cSC4ViewInputControl* CreateTerrainScrubberTool(std::uint32_t type, bool flag) override {
        toolTypes.push_back(type);
        toolFlags.push_back(flag);
        return toolToCreate;
    }
    cSC4ViewInputControl* CreateLevelTerrainTool() override { return toolToCreate; }
    cSC4ViewInputControl* CreateCityScrollTool() override { return toolToCreate; }
    void CityScrollSetMouseScroll(cSC4ViewInputControl*, int x, int y) override {
        ++cityScrollSets;
        lastScrollX = x;
        lastScrollY = y;
    }
    void PlayGlobalTerrainEffectSound() override { ++globalTerrainSounds; }
    int AddContourCallback() override { ++contourAdds; return nextContour; }
    void RemoveContourCallback(int handle) override { ++contourRemoves; lastContourRemoved = handle; }
    bool ProjectPointToViewport(cS3DVector3& point) override {
        if (projectResult) point = projectedPoint;
        return projectResult;
    }
    void ShutdownProgressScreen(cIGZUnknown*) override { ++progressShutdowns; }
    void ShutdownValidationWindow(cIGZUnknown*) override { ++validationShutdowns; }
    void ShutdownLotConfigurationEditors() override { ++lotConfigShutdowns; }
    void ShutdownModelMaker(cIGZUnknown*) override { ++modelMakerShutdowns; }
    void ShutdownCameraObject(cIGZUnknown*) override { ++cameraShutdowns; }
    void ShutdownSimulationClock(cIGZUnknown*) override { ++clockShutdowns; }
    void UpdateCostIndicator(float dt) override { ++costUpdates; lastCostDt = dt; }
    bool PopupModalDialogsSafe() override { return popupSafe; }
    bool PopupModalDialogsEnabled() override { return popupEnabled; }
    std::uint32_t SimulationFrameCounter() override { return frameCounter; }
    bool FilterUiCommandMessage(cGZMessage&) override { ++uiFilters; return true; }
    bool HandleUrl(cIGZString*) override { ++urlHandles; return urlResult; }
    void HandleAdviceLink(cGZMessage&) override { ++adviceLinks; }
    void ProcessCheatCode(std::uint32_t id, const char* cmdLine) override {
        cheatRuns.push_back(id);
        cheatTexts.push_back(cmdLine ? cmdLine : "");
    }
    void InvokeCatalogItemCommand(std::uint32_t item, std::uint32_t param, bool flag) override {
        ++catalogInvokes; catalogItem = item; catalogParam = param; catalogFlag = flag;
    }
    int placeLots = 0;
    std::uint32_t lastPlaceLot = 0;
    cSC4ViewInputControl* CreatePlaceLotTool(std::uint32_t lot) override { ++placeLots; lastPlaceLot = lot; return nullptr; }
    int signTools = 0, saves = 0, regionRequests = 0;
    bool lastSaveAnnounce = false, lastSaveAlternate = false;
    cSC4ViewInputControl* CreateSignTool() override { ++signTools; return nullptr; }
    void RequestSaveCity(bool announce, bool alternate) override { ++saves; lastSaveAnnounce = announce; lastSaveAlternate = alternate; }
    void RequestGoToRegionView(bool) override { ++regionRequests; }
    void ExecuteAcceleratorCommand(std::uint32_t command) override {
        ++acceleratorCommands; lastAcceleratorCommand = command;
    }
    void Update24HourClockHover() override { ++hoverUpdates; }
    void UpdateCursorTextPositions(int x, int y) override { ++cursorTextUpdates; lastCursorX = x; lastCursorY = y; }
    void SetDrawContextFrameMarker(void* context, std::uint32_t frame) override {
        ++frameMarkers;
        lastFrameMarker = frame;
        if (context) *static_cast<std::uint32_t*>(context) = frame;
    }
    void OnView3DCreate(cISC4View3DWin* view) override { ++onView3DCreate; lastView = view; }
    void OnView3DShutdown() override { ++onView3DShutdown; }
    cISC4View3DWin* lastView = nullptr;
};

// ------------------------------------------------------------------------------------------------ tool double
struct ControlLog {
    int inits = 0, shutdowns = 0, activations = 0, deactivations = 0, mouseExits = 0;
    int characters = 0, keysDown = 0, keysUp = 0, wheels = 0;
    int downL = 0, downR = 0, upL = 0, upR = 0, moves = 0;
    std::vector<std::uint32_t> keys;
    bool handleCharacter = false, handleKeyDown = false, handleKeyUp = false, handleWheel = false;
    bool handleDownL = false, handleDownR = false, handleUpL = false, handleUpR = false, handleMove = false;
    bool selfScrolling = false;
    bool initResult = true;
};
struct TestControl final : cSC4ViewInputControl {
    explicit TestControl(ControlLog& log) : log_(log) {}
    bool Init() override {
        ++log_.inits;
        if (!log_.initResult) return false;
        return cSC4ViewInputControl::Init();   // sets winMgr_, which AmCapturing needs
    }
    bool IsSelfScrollingView() override { return log_.selfScrolling; }
    bool Shutdown() override { ++log_.shutdowns; return true; }
    void Activate() override { ++log_.activations; }
    void Deactivate() override { ++log_.deactivations; }
    bool OnCharacter(char) override { ++log_.characters; return log_.handleCharacter; }
    bool OnKeyDown(std::uint32_t key, std::uint32_t) override {
        ++log_.keysDown; log_.keys.push_back(key); return log_.handleKeyDown;
    }
    bool OnKeyUp(std::uint32_t key, std::uint32_t) override {
        ++log_.keysUp; log_.keys.push_back(key); return log_.handleKeyUp;
    }
    bool OnMouseWheel(int, int, std::uint32_t, int) override { ++log_.wheels; return log_.handleWheel; }
    bool OnMouseDownL(int, int, std::uint32_t) override { ++log_.downL; return log_.handleDownL; }
    bool OnMouseDownR(int, int, std::uint32_t) override { ++log_.downR; return log_.handleDownR; }
    bool OnMouseUpL(int, int, std::uint32_t) override { ++log_.upL; return log_.handleUpL; }
    bool OnMouseUpR(int, int, std::uint32_t) override { ++log_.upR; return log_.handleUpR; }
    bool OnMouseMove(int, int, std::uint32_t) override { ++log_.moves; return log_.handleMove; }
    bool OnMouseExit() override { ++log_.mouseExits; return true; }
    ControlLog& log_;
};

// A text control (gz::cIGZWinText) that records the caption and colour the refresh functions set.
struct FakeTextControl final : cGZWin, gz::cIGZWinText {
    explicit FakeTextControl(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinText) { *out = static_cast<gz::cIGZWinText*>(this); AddRef(); return true; }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return static_cast<cIGZWin*>(static_cast<cGZWin*>(this)); }
    void SetCaption(const cIGZString& value) override { caption = value.ToChar(); ++captions; }
    bool FitWindowToText(bool, bool) override { return true; }
    bool GetWinTextFlag(int) override { return false; }
    bool SetWinTextFlag(int, bool) override { return true; }
    void SetTextColor(std::uint32_t native) override { SetTextColor(kColorText, native); }
    void GetTextColor(std::uint32_t& native) override { native = colors[0]; }
    void ClearTextColor() override { colors[0] = colors[1] = 0; }
    bool SetTextColor(tFontColorIdx index, std::uint32_t native) override {
        colors[index & 1] = native; ++colorSets; return true;
    }
    bool GetTextColor(tFontColorIdx index, std::uint32_t& native) const override {
        native = colors[index & 1]; return true;
    }
    bool SetTextColor(tFontColorIdx index, gz::cRZColor color) override {
        return SetTextColor(index, color.rgb);
    }
    bool GetTextColor(tFontColorIdx index, gz::cRZColor& color) const override {
        color.rgb = colors[index & 1]; return true;
    }
    bool ClearTextColor(tFontColorIdx index) override { colors[index & 1] = 0; return true; }
    void SetBackgroundOpaque(bool) override {}
    bool IsBackgroundOpaque() override { return false; }
    bool SetFont(gz::IWinFont*) override { return true; }
    bool SetFontStyle(std::uint32_t) override { return true; }
    bool GetFontStyle(std::uint32_t& style) override { style = 0; return true; }
    bool SetAlignment(std::uint32_t) override { return true; }
    bool GetAlignment(std::uint32_t& alignment) const override { alignment = 0; return true; }
    bool SetGutters(std::int8_t x, std::int8_t y) override { gutters[0] = x; gutters[1] = y; return true; }
    bool GetGutters(std::int8_t& x, std::int8_t& y) override { x = gutters[0]; y = gutters[1]; return true; }
    bool SetTextOffsets(std::int8_t, std::int8_t) override { return true; }
    bool GetTextOffsets(std::int8_t& x, std::int8_t& y) override { x = y = 0; return true; }
    void SetOutlineUse(bool, std::uint32_t) override {}
    void GetOutlineUse(bool& on, std::uint32_t& color) const override { on = false; color = 0; }
    std::string caption;
    std::uint32_t colors[2] = {0, 0};
    std::int8_t gutters[2] = {0, 0};
    int captions = 0, colorSets = 0;
};

}  // namespace

int main() {
    FakeHost host;
    SetView3DWinHost(&host);
    FakeMgr mgr;
    host.windowManager = &mgr;
    host.cursorWindow.window_->SetWindowManager(&mgr);
    ViewManagerWindow mainWindow(&mgr);
    mainWindow.SetArea(0, 0, 800, 600);
    mgr.mainWin = &mainWindow;
    FakeRenderer renderer;
    FakeViewInputHost inputHost;
    inputHost.manager = &mgr;
    SetViewInputHost(&inputHost);

    // ------------------------------------------------------------------ constructor
    auto* view = new cSC4View3DWin(&mgr);
    CHECK(host.onView3DCreate == 1 && host.lastView == view);
    CHECK(host.notifications.addTypes == 1 && host.notifications.lastCount == 132);
    CHECK(host.notifications.firstType == 0xa6b79602 && host.notifications.lastType == 0xabcdee11);
    CHECK(View3DWinTestAccess::Unknown1c8(*view) == 0x0100);
    CHECK(View3DWinTestAccess::ViewLevel(*view) == 1);
    CHECK(View3DWinTestAccess::ZoomMin(*view) == 0 && View3DWinTestAccess::ZoomMax(*view) == 5);
    View3DWinTestAccess::Coverage(*view) = &host.coverage;

    // ------------------------------------------------------------------ Init
    host.cheatPresent = true;
    host.lotPlop = true;
    host.progressWanted = true;
    CHECK(view->Init());
    CHECK(host.notifications.addAll == 1);
    CHECK(host.registerTables == 1);
    CHECK(host.addTargets == 1);
    CHECK(host.registeredCheats.size() == 13);
    CHECK(host.registeredCheats[0] == kCheatGameOfLife && host.registeredCheatStrings[0] == "GOL");
    CHECK(host.registeredCheats[4] == kCheatSetRenderProperty && host.registeredCheatStrings[4] == "RenderProp");
    CHECK(host.registeredCheats[10] == kCheatLotPlop && host.registeredCheatStrings[10] == "LotPlop");
    CHECK(host.registeredCheats[12] == kCheatTollBooth && host.registeredCheatStrings[12] == "toll");
    CHECK(std::strcmp(view->GetCaption().ToChar(), "cSC4View3DWin") == 0);
    CHECK(view->GetID() == kView3DWindowId);
    CHECK(view->GetW() == 800 && view->GetH() == 600 && view->GetL() == 0 && view->GetT() == 0);
    CHECK(mgr.cursors.loadCount == 1 && mgr.cursors.lastLoaded == kView3DCursorId);
    CHECK(view->GetKeyAccelerator() == &host.accelerator);
    CHECK(host.coverage.inits == 1);
    CHECK(host.progress.refs == 1);
    CHECK(view->Init());                                       // second Init: nothing happens again
    CHECK(host.notifications.addAll == 1 && host.registerTables == 1 && host.addTargets == 1);

    // Without the lot-plop cheats only ten codes are registered.
    {
        host.registeredCheats.clear();
        host.registeredCheatStrings.clear();
        host.lotPlop = false;
        auto* bare = new cSC4View3DWin(&mgr);
        View3DWinTestAccess::Coverage(*bare) = &host.coverage;
        host.cheatPresent = false;
        CHECK(bare->Init());
        CHECK(host.registeredCheats.empty() && host.addTargets == 1);
        CHECK(bare->Shutdown());
        host.cheatPresent = true;
        host.registeredCheats.clear();
        host.registeredCheatStrings.clear();
        auto* copy = new cSC4View3DWin(&mgr);
        View3DWinTestAccess::Coverage(*copy) = &host.coverage;
        CHECK(copy->Init());
        CHECK(host.registeredCheats.size() == 10);
        CHECK(copy->Shutdown());
        delete copy;
        host.cheatPresent = true;
        host.lotPlop = true;
        delete bare;
        host.registeredCheats.clear();
        host.registeredCheatStrings.clear();
    }

    // ------------------------------------------------------------------ QueryInterface and refcount
    void* out = nullptr;
    CHECK(view->QueryInterface(1, &out) && out == static_cast<cISC4View3DWin*>(view));
    CHECK(view->QueryInterface(kGZIID_cISC4View3DWin, &out) && out == static_cast<cISC4View3DWin*>(view));
    CHECK(view->QueryInterface(kGZIID_cIGZWin, &out) && out == static_cast<cIGZWin*>(static_cast<cGZWin*>(view)));
    CHECK(view->QueryInterface(kGZIID_cIGZMessageTarget2, &out) && out == static_cast<cIGZMessageTarget2*>(view));
    CHECK(view->QueryInterface(kGZIID_cIGZWinMessageFilter, &out) && out == static_cast<cIGZWinMessageFilter*>(view));
    CHECK(!view->QueryInterface(0xe98b2f57, &out));            // refused although cGZWin answers it
    CHECK(!view->QueryInterface(0x12345678, &out));
    CHECK(view->AsIGZWin() == static_cast<cIGZWin*>(static_cast<cGZWin*>(view)));
    const std::uint32_t before = view->AddRef();
    CHECK(before == view->Release() + 1);

    CHECK(view->GetViewManager() == nullptr);
    View3DWinTestAccess::ViewManagerSource(*view) = &mainWindow;
    cIGZUnknown* manager = view->GetViewManager();
    CHECK(manager == static_cast<cIGZUnknown*>(static_cast<cGZWin*>(&mainWindow)));
    if (manager) manager->Release();
    // +0x498 is a stack window here; RemoveGUI owns it, so stop pointing at it.
    View3DWinTestAccess::ViewManagerSource(*view) = nullptr;

    // ------------------------------------------------------------------ plain accessors
    CHECK(view->GetRenderer() == nullptr);                     // set below
    CHECK(view->GetModelMaker() == nullptr);
    // The cGZWin key hooks answer false; GZOnCommand forwards to cGZWin.
    CHECK(!view->GZOnKeyDown(0x51, 0x2));
    CHECK(!view->GZOnKeyUp(0x51, 0x2));
    CHECK(!view->GZOnCommand(0x1234, 0));
    CHECK(view->GetCoverageHelper() == &host.coverage);
    CHECK(view->GetTerrainQueryEnabled() == false);
    view->DisplayTerrainPickDebugString(4, 5);
    view->SetErrorReportString("ignored");
    CHECK(view->GetLastSelectedBuilding() == 0 && view->GetLastSelectedLot() == 0);
    view->SetLastSelectedBuilding(0x11111111);
    view->SetLastSelectedLot(0x22222222);
    CHECK(view->GetLastSelectedBuilding() == 0x11111111 && view->GetLastSelectedLot() == 0x22222222);

    // The key accelerator is reference counted and only swapped when it changes.
    FakeAccelerator replacement;
    CHECK(host.accelerator.refs == 2);                         // the object's own + the view's reference
    view->SetKeyAccelerator(&replacement);
    CHECK(view->GetKeyAccelerator() == &replacement && replacement.refs == 2 && host.accelerator.refs == 1);
    view->SetKeyAccelerator(&replacement);
    CHECK(replacement.refs == 2);
    view->SetKeyAccelerator(nullptr);
    CHECK(view->GetKeyAccelerator() == nullptr && replacement.refs == 1);

    // ------------------------------------------------------------------ camera / zoom / rotation
    auto* camera = new cSC4CameraControl();
    camera->AddRef();                                          // the reference the view owns
    CHECK(camera->Init({1000.0f, 0.0f, 2000.0f}, new s3d::cS3DCamera()));
    View3DWinTestAccess::Camera(*view) = camera;
    View3DWinTestAccess::Renderer(*view) = &renderer;
    CHECK(view->GetRenderer() == &renderer);
    CHECK(camera->Zoom() == 0 && camera->Rotation() == 0);

    CHECK(view->SetZoom(3) && renderer.lastZoom == 3);
    CHECK(view->SetZoom(9) && renderer.lastZoom == 5);
    CHECK(view->SetZoom(-2) && renderer.lastZoom == 0);
    CHECK(renderer.zoomCalls == 3);

    camera->SetZoom(2.0f);
    CHECK(camera->GetVirtualZoom() == 2);
    CHECK(view->CanZoomIn() && view->CanZoomOut());
    CHECK(view->ZoomIn() && renderer.zoomIn == 1);
    CHECK(view->ZoomOut() && renderer.zoomOut == 1);
    renderer.zoomInResult = false;
    CHECK(!view->ZoomIn() && renderer.zoomIn == 2);
    View3DWinTestAccess::ZoomMax(*view) = 2;
    CHECK(!view->CanZoomIn() && !view->ZoomIn());
    View3DWinTestAccess::ZoomMin(*view) = 2;
    CHECK(!view->CanZoomOut() && !view->ZoomOut());
    View3DWinTestAccess::ZoomMin(*view) = 0;
    View3DWinTestAccess::ZoomMax(*view) = 5;

    CHECK(view->SetRotation(2) && renderer.lastRotation == 2);
    CHECK(view->RotateLeft() && renderer.lefts == 1);
    CHECK(view->RotateRight() && renderer.rights == 1);
    CHECK(view->SetZoomAndRotation(9, 3) && renderer.lastZoom == 5 && renderer.lastRotation == 3);
    CHECK(renderer.zoomAndRotation == 1);

    // SetZoomLimits: the stored max becomes the app's virtual limit when the argument is > 4, but the
    // clamp compares against the argument (quirk, 0x100781e96).
    host.virtualZoomLimit = 4;
    CHECK(view->SetZoomLimits(0, 6));
    CHECK(View3DWinTestAccess::ZoomMax(*view) == 4);
    camera->SetZoom(1.0f);
    host.virtualZoomLimit = 5;
    CHECK(view->SetZoomLimits(0, 6));
    CHECK(View3DWinTestAccess::ZoomMax(*view) == 5);
    CHECK(renderer.zoomCalls == 3);                            // nothing to change: the target is the current zoom
    CHECK(view->SetZoomLimits(3, 2));
    CHECK(renderer.lastZoom == 2);                             // raised to the min, then clamped to the max
    View3DWinTestAccess::ZoomMin(*view) = 0;
    View3DWinTestAccess::ZoomMax(*view) = 5;

    // ------------------------------------------------------------------ the sim-hidden pause
    View3DWinTestAccess::PauseSink(*view) = &host.pauseSink;
    host.pauseSink.refs = 1;
    view->DoTemporarySimHiddenPause();
    CHECK(View3DWinTestAccess::SimHidden(*view) && host.pauseSink.pauses == 1);
    view->DoTemporarySimHiddenPause();
    CHECK(host.pauseSink.pauses == 1);                         // already paused
    view->DoTemporarySimHiddenResume();
    CHECK(!View3DWinTestAccess::SimHidden(*view) && host.pauseSink.resumes == 1);
    view->DoTemporarySimHiddenResume();
    CHECK(host.pauseSink.resumes == 1);

    // ------------------------------------------------------------------ scrolling
    CHECK(view->ScrollStop());                                 // not scrolling: no-op
    CHECK(!View3DWinTestAccess::Stopping(*view));
    host.preferences[kPrefsEdgeScrollEnabled] = 0;
    View3DWinTestAccess::KeyboardScrollX(*view) = 1;           // east
    View3DWinTestAccess::ScrollSpeed(*view) = 0.2f;
    camera->SetZoom(0.0f);
    view->RecalcScrollVelocity();
    CHECK(View3DWinTestAccess::Scrolling(*view));
    CHECK(std::fabs(View3DWinTestAccess::ScrollAngle(*view) - 0.0f) < 1e-6f);
    CHECK(std::fabs(View3DWinTestAccess::ScrollSpeed(*view) - 0.2f) < 1e-6f);
    CHECK(host.gameMessages.size() == 1 && host.gameMessages[0] == kMsgViewScrollBegin);
    View3DWinTestAccess::KeyboardScrollX(*view) = -1;
    View3DWinTestAccess::KeyboardScrollY(*view) = 1;
    camera->SetZoom(0.0f);
    view->RecalcScrollVelocity();
    CHECK(std::fabs(View3DWinTestAccess::ScrollAngle(*view) - 2.3561945f) < 1e-5f);
    View3DWinTestAccess::KeyboardScrollX(*view) = 0;
    View3DWinTestAccess::KeyboardScrollY(*view) = 0;
    view->RecalcScrollVelocity();
    // The scroll stays "active" until GZPaint finishes the stop animation; RecalcScrollVelocity only
    // raises the stop speed (0x1007829..0x100781df6).
    CHECK(View3DWinTestAccess::Stopping(*view) && View3DWinTestAccess::Scrolling(*view));
    View3DWinTestAccess::KeyboardScrollX(*view) = 1;
    View3DWinTestAccess::KeyboardScrollY(*view) = 1;
    view->KillKeyboardScrolling();
    CHECK(View3DWinTestAccess::KeyboardScrollX(*view) == 0 && View3DWinTestAccess::KeyboardScrollY(*view) == 0);
    CHECK(View3DWinTestAccess::Scrolling(*view));             // still stopping
    View3DWinTestAccess::EdgeScrollX(*view) = 1;               // the preference drops both edge inputs
    View3DWinTestAccess::EdgeScrollY(*view) = 1;
    host.preferences[kPrefsEdgeScrollEnabled] = 1;
    view->RecalcScrollVelocity();
    CHECK(View3DWinTestAccess::EdgeScrollX(*view) == 0 && View3DWinTestAccess::EdgeScrollY(*view) == 0);
    host.preferences[kPrefsEdgeScrollEnabled] = 0;

    // ScrollStop: the stop speed comes from +0x4f0, scaled by the current speed when +0x4f4 is set.
    View3DWinTestAccess::Scrolling(*view) = true;
    View3DWinTestAccess::ScrollSpeed(*view) = 0.5f;
    View3DWinTestAccess::Stopping(*view) = false;
    CHECK(view->ScrollStop());
    CHECK(View3DWinTestAccess::Stopping(*view));
    CHECK(std::fabs(View3DWinTestAccess::ScrollStopSpeed(*view) - 1.0f) < 1e-6f);
    View3DWinTestAccess::ScrollStopScaled(*view) = true;
    View3DWinTestAccess::Stopping(*view) = false;
    CHECK(view->ScrollStop());
    CHECK(std::fabs(View3DWinTestAccess::ScrollStopSpeed(*view) - 0.5f) < 1e-6f);
    View3DWinTestAccess::ScrollStopScaled(*view) = false;
    View3DWinTestAccess::Scrolling(*view) = false;
    View3DWinTestAccess::Stopping(*view) = false;

    // SetScrolling drives DoScrollBegin / DoScrollEnd, which snapshot and restore the render properties.
    host.globalProperties.SetBoolValue(kPropNoPartialBackingStoreCopies, true);
    host.globalProperties.SetBoolValue(kPropRenderShadows, true);
    host.globalProperties.SetBoolValue(kPropRenderDecals, true);
    host.globalProperties.SetBoolValue(kPropRenderAutomata, true);
    host.globalProperties.SetBoolValue(kPropRenderParticleEffects, true);
    host.globalProperties.SetFloatValue(kPropRenderModelThreshold, 6.0f);
    host.globalProperties.SetFloatValue(kPropRenderModelThresholdScroll, 40.0f);
    view->SetScrolling(true, 1.25f, 0.75f);
    CHECK(View3DWinTestAccess::Scrolling(*view));
    CHECK(std::fabs(View3DWinTestAccess::ScrollAngle(*view) - 1.25f) < 1e-6f);
    CHECK(host.lightingHighlights == 1);
    // The snapshot keeps the state the scroll started from; the live object gets the reduced settings.
    CHECK(View3DWinTestAccess::Snapshot(*view).BoolValue(kPropRenderShadows));
    CHECK(View3DWinTestAccess::Snapshot(*view).BoolValue(kPropRenderAutomata));
    CHECK(std::fabs(View3DWinTestAccess::Snapshot(*view).FloatValue(kPropRenderModelThreshold) - 6.0f) < 1e-6f);
    CHECK(!host.globalProperties.BoolValue(kPropRenderShadows));
    CHECK(!host.globalProperties.BoolValue(kPropRenderDecals));
    CHECK(!host.globalProperties.BoolValue(kPropRenderAutomata));
    CHECK(!host.globalProperties.BoolValue(kPropRenderParticleEffects));
    CHECK(std::fabs(host.globalProperties.FloatValue(kPropRenderModelThreshold) - 40.0f) < 1e-6f);
    view->SetScrolling(false, 0.0f, 0.0f);
    CHECK(!View3DWinTestAccess::Scrolling(*view));
    CHECK(host.lightingRestores == 1);
    CHECK(renderer.redraws == 1);
    CHECK(host.globalProperties.BoolValue(kPropRenderShadows));      // restored from the snapshot
    CHECK(std::fabs(host.globalProperties.FloatValue(kPropRenderModelThreshold) - 6.0f) < 1e-6f);
    CHECK(host.gameMessages.size() >= 2);
    CHECK(host.gameMessages.back() == kMsgViewScrollEnd);
    // With the property switched off the snapshot is skipped entirely.
    host.globalProperties.SetBoolValue(kPropNoPartialBackingStoreCopies, false);
    host.globalProperties.SetBoolValue(kPropRenderShadows, true);
    view->SetScrolling(true, 0.0f, 0.0f);
    CHECK(host.lightingHighlights == 1 && host.globalProperties.BoolValue(kPropRenderShadows));
    view->SetScrolling(false, 0.0f, 0.0f);
    CHECK(host.lightingRestores == 1);

    // ------------------------------------------------------------------ the tool stack
    {
        ControlLog logA, logB;
        auto* controlA = new TestControl(logA);
        controlA->AddRef();                              // the caller's reference (the view AddRefs too)
        auto* controlB = new TestControl(logB);
        controlB->AddRef();
        host.server.posted.clear();
        CHECK(view->SetCurrentViewInputControl(controlA, 0));
        controlA->Release();                             // from here the stack owns it
        CHECK(view->GetCurrentViewInputControl() == controlA);
        CHECK(logA.inits == 1 && logA.activations == 1);
        CHECK(host.server.posted.size() == 1 && host.server.posted[0] == kMsgViewInputControlChanged);
        CHECK(host.server.lastVoid1 == nullptr && host.server.lastVoid2 == controlA);
        // Pushing a second tool deactivates the first one but keeps it on the stack.
        CHECK(view->SetCurrentViewInputControl(controlB, 0));
        controlB->Release();
        CHECK(view->GetCurrentViewInputControl() == controlB);
        CHECK(logA.mouseExits == 1 && logA.deactivations == 1);
        CHECK(logB.activations == 1);
        CHECK(View3DWinTestAccess::Tools(*view).size() == 2);
        // Removing the top tool deactivates / shuts down the control twice (the view's Shutdown and the
        // FinalRelease's), tells the message server about the change and re-activates nothing.
        host.server.posted.clear();
        CHECK(view->RemoveCurrentViewInputControl(false));
        CHECK(view->GetCurrentViewInputControl() == controlA);
        CHECK(logB.deactivations == 1 && logB.shutdowns == 2);
        CHECK(View3DWinTestAccess::Tools(*view).size() == 1);
        CHECK(host.server.posted.size() == 1);
        CHECK(host.server.lastVoid1 != nullptr && host.server.lastVoid2 == controlA);
        // Removing the last tool with keepCursor leaves the cursor alone; without it the default cursor
        // is loaded again.
        mgr.cursors.loadCount = 0;
        CHECK(view->RemoveCurrentViewInputControl(true));
        CHECK(view->GetCurrentViewInputControl() == nullptr);
        CHECK(logA.deactivations == 2 && logA.shutdowns == 2);
        CHECK(mgr.cursors.loadCount == 0);
        CHECK(!view->RemoveCurrentViewInputControl(false));      // nothing left
        CHECK(!view->RemoveAllViewInputControls(false));         // returns false by design
    }
    {
        ControlLog logC;
        auto* controlC = new TestControl(logC);
        controlC->AddRef();
        CHECK(view->SetCurrentViewInputControl(controlC, 0));
        controlC->Release();
        mgr.cursors.loadCount = 0;
        CHECK(view->RemoveCurrentViewInputControl(false));
        CHECK(mgr.cursors.loadCount == 1 && mgr.cursors.lastLoaded == kView3DCursorId);
        CHECK(logC.shutdowns == 2);
    }

    // Persistent tools: Init, AddRef, Activate; removal deactivates / shuts down / releases.
    {
        ControlLog logP, logQ;
        auto* persistent = new TestControl(logP);
        persistent->AddRef();
        CHECK(view->AddPersistentViewInputControl(persistent));
        persistent->Release();
        CHECK(View3DWinTestAccess::Persistent(*view).size() == 1);
        CHECK(logP.inits == 1 && logP.activations == 1);
        auto* other = new TestControl(logQ);
        CHECK(!view->RemovePersistentViewInputControl(other));    // not registered
        delete other;
        CHECK(view->RemovePersistentViewInputControl(persistent));
        CHECK(View3DWinTestAccess::Persistent(*view).empty());
        CHECK(logP.deactivations == 1 && logP.shutdowns == 2);
        auto* second = new TestControl(logP);
        second->AddRef();
        CHECK(view->AddPersistentViewInputControl(second));
        second->Release();
        CHECK(logP.inits == 2 && logP.activations == 2);
        CHECK(view->RemoveAllPersistentViewInputControls());
        CHECK(View3DWinTestAccess::Persistent(*view).empty());
        CHECK(logP.deactivations == 2 && logP.shutdowns == 4);
    }
    // A tool whose Init fails is not added at all.
    {
        ControlLog logF;
        auto* failing = new TestControl(logF);
        failing->AddRef();
        logF.initResult = false;
        CHECK(view->SetCurrentViewInputControl(failing, 0));
        CHECK(View3DWinTestAccess::Tools(*view).empty());
        CHECK(logF.inits == 1 && logF.activations == 0);
        CHECK(view->AddPersistentViewInputControl(failing));      // returns true, adds nothing
        CHECK(View3DWinTestAccess::Persistent(*view).empty());
        failing->Release();                                       // the caller's reference (FinalRelease deletes)
    }

    // GZOnCharacter: the current stack first, and a handled character skips the persistent tools.
    {
        ControlLog stackLog, persistLog;
        auto* stacked = new TestControl(stackLog);
        stacked->AddRef();
        auto* persistent = new TestControl(persistLog);
        persistent->AddRef();
        CHECK(view->AddPersistentViewInputControl(persistent));
        persistent->Release();
        CHECK(view->SetCurrentViewInputControl(stacked, 0));
        stacked->Release();
        stackLog.handleCharacter = true;
        CHECK(view->GZOnCharacter('a'));
        CHECK(stackLog.characters == 1 && persistLog.characters == 0);
        stackLog.handleCharacter = false;
        CHECK(!view->GZOnCharacter('b'));
        CHECK(stackLog.characters == 2 && persistLog.characters == 1);
        // The persistent loop is bounded by the current stack's size (quirk, 0x100782ab8): with two
        // persistent tools and one stacked tool the top persistent tool is skipped entirely.
        ControlLog lowLog;
        auto* low = new TestControl(lowLog);
        low->AddRef();
        CHECK(view->AddPersistentViewInputControl(low));
        low->Release();
        CHECK(!view->GZOnCharacter('c'));
        CHECK(persistLog.characters == 1 && lowLog.characters == 0);
        CHECK(view->RemoveAllPersistentViewInputControls());
        CHECK(!view->RemoveAllViewInputControls(true));
        CHECK(view->GetCurrentViewInputControl() == nullptr);
    }

    // ProcessKeyDown / ProcessKeyUp / GZOnMouseWheel dispatch to the tools.
    {
        ControlLog stackLog;
        auto* stacked = new TestControl(stackLog);
        stacked->AddRef();
        CHECK(view->SetCurrentViewInputControl(stacked, 0));
        stacked->Release();
        CHECK(!view->ProcessKeyDown(0x51, 0x2));
        CHECK(stackLog.keysDown == 1);
        View3DWinTestAccess::LastModifiers(*view) = 0;
        CHECK(!view->ProcessKeyUp(0x51, 0x4));
        CHECK(stackLog.keysUp == 1 && View3DWinTestAccess::LastModifiers(*view) == 0x4);
        stackLog.handleKeyDown = true;
        CHECK(view->ProcessKeyDown(0x52, 0));
        stackLog.handleKeyDown = false;
        CHECK(view->GZOnMouseWheel(10, 20, 0, 1));
        CHECK(stackLog.wheels == 1);
        // The wheel falls back to ZoomIn / ZoomOut (the camera sits at zoom 2, the limits are 0 / 5).
        camera->SetZoom(2.0f);
        const int zoomIns = renderer.zoomIn, zoomOuts = renderer.zoomOut;
        CHECK(view->GZOnMouseWheel(10, 20, 0, -1));
        CHECK(renderer.zoomOut == zoomOuts + 1);
        CHECK(view->GZOnMouseWheel(10, 20, 0, 1));
        CHECK(renderer.zoomIn == zoomIns + 1);
        CHECK(!view->RemoveAllViewInputControls(true));
    }

    // GZOnMouseExit: stops the edge scroll, updates the cursor text, tells the top tools.
    {
        ControlLog stackLog, persistLog;
        auto* stacked = new TestControl(stackLog);
        stacked->AddRef();
        auto* persistent = new TestControl(persistLog);
        persistent->AddRef();
        CHECK(view->AddPersistentViewInputControl(persistent));
        persistent->Release();
        CHECK(view->SetCurrentViewInputControl(stacked, 0));
        stacked->Release();
        View3DWinTestAccess::EdgeScrollX(*view) = 1;
        View3DWinTestAccess::EdgeScrollY(*view) = 1;
        host.cursorTextUpdates = 0;
        CHECK(view->GZOnMouseExit(nullptr));
        CHECK(View3DWinTestAccess::EdgeScrollX(*view) == 0 && View3DWinTestAccess::EdgeScrollY(*view) == 0);
        CHECK(host.cursorTextUpdates == 1 && host.lastCursorX == -10000 && host.lastCursorY == -10000);
        CHECK(stackLog.mouseExits == 1 && persistLog.mouseExits == 1);
        CHECK(view->RemoveAllPersistentViewInputControls());
        CHECK(!view->RemoveAllViewInputControls(true));
        View3DWinTestAccess::Scrolling(*view) = false;
    }

    // ------------------------------------------------------------------ picks
    {
        renderer.pickTerrainResult = true;
        float terrain[3] = {9.0f, 9.0f, 9.0f};
        CHECK(view->PickTerrain(10, 20, terrain, true));
        CHECK(renderer.terrainPicks == 1 && renderer.lastPickX == 10 && renderer.lastPickY == 20);
        CHECK(renderer.pickTerrainAlways == false && renderer.pickTerrainArg5 == true);
        CHECK(terrain[0] == 1.5f && terrain[1] == 2.5f && terrain[2] == 3.5f);
        renderer.pickTerrainResult = false;
        CHECK(!view->PickTerrain(0, 0, terrain, false));
        // No renderer: the out parameter gets two zeroed words, not three (quirk, 0x100782952).
        View3DWinTestAccess::Renderer(*view) = nullptr;
        terrain[0] = terrain[1] = terrain[2] = 9.0f;
        CHECK(!view->PickTerrain(0, 0, terrain, true));
        CHECK(terrain[0] == 0.0f && terrain[1] == 0.0f && terrain[2] == 9.0f);
        cISC4Occupant* occupantOut = reinterpret_cast<cISC4Occupant*>(0x1);
        CHECK(!view->PickOccupant(0, 0, occupantOut) && occupantOut == nullptr);
        View3DWinTestAccess::Renderer(*view) = &renderer;
        FakeOccupant occupant;
        FakeModelInstance model;
        model.owner = &occupant;
        renderer.pickResult = &model;
        CHECK(view->PickOccupant(3, 4, occupantOut));
        CHECK(occupantOut == reinterpret_cast<cISC4Occupant*>(&occupant));
        CHECK(model.releases == 1 && renderer.lastPickX == 3 && renderer.lastPickY == 4);
        // A model without an occupant answers false and is still released.
        FakeModelInstance bareModel;
        renderer.pickResult = &bareModel;
        CHECK(!view->PickOccupant(0, 0, occupantOut) && occupantOut == nullptr);
        CHECK(bareModel.releases == 1);
        renderer.pickResult = nullptr;
        renderer.pickTerrainResult = false;
    }

    // ------------------------------------------------------------------ global effect triggers
    host.server.sent.clear();
    CHECK(!view->GetGlobalEffectTriggerState(0xabc));
    view->AssertGlobalEffectTrigger(0xabc);
    CHECK(view->GetGlobalEffectTriggerState(0xabc));
    view->AssertGlobalEffectTrigger(0xabc);                       // a second holder
    CHECK(host.server.sent.size() == 1 && host.server.sent[0] == kMsgViewGlobalEffectTrigger);
    CHECK(host.server.lastData1 == 0xabc && host.server.lastData2 == 1);
    view->RetractGlobalEffectTrigger(0xabc);
    CHECK(view->GetGlobalEffectTriggerState(0xabc));               // still one holder
    view->RetractGlobalEffectTrigger(0xabc);
    CHECK(!view->GetGlobalEffectTriggerState(0xabc));
    CHECK(host.server.sent.size() == 2 && host.server.lastData2 == 0);
    view->RetractGlobalEffectTrigger(0xabc);                       // nothing to retract

    // ------------------------------------------------------------------ view filter / level information
    {
        auto* filterA = new FakeOccupant();                        // any refcounted object
        auto* filterB = new FakeOccupant();
        filterA->AddRef();
        filterB->AddRef();
        cSC4ViewFilterInformation info;
        view->GetCurrentViewFilterInfo(info);
        CHECK(info.filter == nullptr);
        info.filter = filterA;
        host.server.sent.clear();
        CHECK(view->SetCurrentViewFilterInfo(info));
        CHECK(View3DWinTestAccess::FilterInfo(*view) == filterA);
        CHECK(filterA->refs == 2);                                 // ours + the view's
        CHECK(host.server.sent.size() == 2);
        CHECK(host.server.sent[0] == kMsgViewFilterInfo && host.server.sent[1] == kMsgViewFilterInfoDone);
        cSC4ViewFilterInformation current;
        view->GetCurrentViewFilterInfo(current);
        CHECK(current.filter == filterA && filterA->refs == 3);    // the out parameter takes a reference
        if (current.filter) current.filter->Release();
        info.filter = filterB;
        CHECK(view->SetCurrentViewFilterInfo(info));
        CHECK(filterA->refs == 1 && filterB->refs == 2);
        // The level information sends the pair only when the level changes.
        host.server.sent.clear();
        cSC4ViewLevelInformation level;
        view->GetCurrentViewLevelInfo(level);
        CHECK(level.level == View3DWinTestAccess::ViewLevel(*view));
        level.level = 4;
        view->SetCurrentViewLevelInfo(level);
        CHECK(view->GetCurrentViewLevel() == 4);
        CHECK(host.server.sent.size() == 2);
        CHECK(host.server.sent[0] == kMsgViewLevelInfo && host.server.sent[1] == kMsgViewLevelInfoDone);
        host.server.sent.clear();
        view->SetCurrentViewLevel(4);                              // unchanged
        CHECK(host.server.sent.empty());
        view->SetCurrentViewLevel(2);
        CHECK(view->GetCurrentViewLevel() == 2 && host.server.sent.size() == 2);
        view->SetCurrentViewLevelInfo(level);                      // back to 4
        filterA->Release();
        filterB->Release();
        View3DWinTestAccess::FilterInfo(*view)->Release();
        View3DWinTestAccess::FilterInfo(*view) = nullptr;
    }

    // ------------------------------------------------------------------ DoMessage(cGZMessage&)
    {
        cGZMessage message;
        // Mouse enter / exit / move refresh the 24-hour clock hover and the focus byte; the message then
        // falls through to cGZWin::DoMessage, which dispatches it again (0x100784d1e).
        View3DWinTestAccess::FocusState(*view) = 0xAA;
        message.type = kMsgViewMouseEnter;
        host.hoverUpdates = 0;
        CHECK(!view->DoMessage(message));
        CHECK(View3DWinTestAccess::FocusState(*view) == 1 && host.hoverUpdates == 1);
        message.type = kMsgViewMouseExit;
        CHECK(view->DoMessage(message));                           // GZOnMouseExit answers true
        CHECK(View3DWinTestAccess::FocusState(*view) == 0 && host.hoverUpdates == 2);
        View3DWinTestAccess::FocusState(*view) = 0x55;
        message.type = kMsgViewMouseMove;
        // The move message is dispatched to GZOnMouseMove by cGZWin::DoMessage, which now handles it.
        CHECK(view->DoMessage(message));
        CHECK(host.hoverUpdates == 3);
        CHECK(host.uiFilters == 3);

        // Focus: clears every scroll input and leaves the mouse.
        host.cityExists = true;
        View3DWinTestAccess::KeyboardScrollX(*view) = 1;
        View3DWinTestAccess::KeyboardScrollY(*view) = 1;
        View3DWinTestAccess::EdgeScrollX(*view) = 1;
        View3DWinTestAccess::EdgeScrollY(*view) = 1;
        View3DWinTestAccess::Scrolling(*view) = true;
        host.cursorTextUpdates = 0;
        message.type = kMsgViewFocus;
        CHECK(view->DoMessage(message));
        CHECK(View3DWinTestAccess::KeyboardScrollX(*view) == 0 && View3DWinTestAccess::KeyboardScrollY(*view) == 0);
        CHECK(View3DWinTestAccess::EdgeScrollX(*view) == 0 && View3DWinTestAccess::EdgeScrollY(*view) == 0);
        CHECK(host.cursorTextUpdates == 1);
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;

        // A catalog command message.
        message = cGZMessage{};
        message.type = kMsgViewCommand;
        message.param1 = kCmdViewCatalogItem;
        message.param2 = 0x6a2b245f;                             // a lot: the place-lot tool for it (InvokeCatalogItemCommand 0x10078410a)
        message.param3 = 0x5678;
        CHECK(view->DoMessage(message));
        CHECK(host.placeLots == 1 && host.lastPlaceLot == 0x5678 && host.catalogInvokes == 0);
        CHECK(mgr.focus == static_cast<cIGZWin*>(static_cast<cGZWin*>(view)));
        message.param2 = 0x1234;                                 // an unknown family: nothing
        CHECK(view->DoMessage(message));
        CHECK(host.placeLots == 1 && host.catalogInvokes == 0);
        // ProcessCommand (0x10078e2e0): the lot commands, the sign tool, the save and region requests.
        view->ProcessViewCommand(0x6a935dc9u);
        CHECK(host.placeLots == 2 && host.lastPlaceLot == 0x05000001u);
        view->ProcessViewCommand(0x0b980581u);
        CHECK(host.signTools == 1);
        view->ProcessViewCommand(0x6a935e37u);
        CHECK(host.saves == 1 && host.lastSaveAnnounce && host.lastSaveAlternate);
        view->ProcessViewCommand(0x6a935e39u, true);
        CHECK(host.regionRequests == 1);

        // The cheat-code message hands the command line of the string in param2 to the cheat handler.
        cRZString cheatText("GOL");
        message = cGZMessage{};
        message.type = kMsgViewCheatCode;
        message.param1 = 0x8973a69c;
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&cheatText));
        CHECK(view->DoMessage(message));
        CHECK(host.cheatRuns.size() == 1 && host.cheatRuns[0] == 0x8973a69c);
        CHECK(host.cheatTexts.size() == 1 && host.cheatTexts[0] == "GOL");

        // The key message: param2 selects down / down-with-modifier / up, param3 points at the key record.
        std::uint32_t keyRecord[5] = {0, 0, 0x51, 0, 0x2};
        CHECK(view->GetKeyAccelerator() == nullptr);
        message = cGZMessage{};
        message.type = kMsgViewKey;
        message.param2 = 5;
        message.param3 = reinterpret_cast<std::uint64_t>(keyRecord);
        CHECK(!view->DoMessage(message));                          // no tool, no accelerator
        View3DWinTestAccess::LastModifiers(*view) = 0;
        message.param2 = 6;                                        // key up
        CHECK(!view->DoMessage(message));
        CHECK(View3DWinTestAccess::LastModifiers(*view) == 0x2);
        // With an accelerator the command is dispatched and the message is handled.
        view->SetKeyAccelerator(&host.accelerator);
        host.accelerator.found = true;
        host.accelerator.tests = 0;
        message.param2 = 4;                                        // key down with 0x20000000 added
        host.acceleratorCommands = 0;
        CHECK(view->DoMessage(message));
        CHECK(host.accelerator.tests == 1);
        CHECK(host.accelerator.lastKey == 0x51);
        CHECK(host.accelerator.lastModifiers == (0x2u | kKeyModifierKeyUp));
        CHECK(host.accelerator.lastDown);
        CHECK(host.acceleratorCommands == 1);
        host.accelerator.found = false;
        CHECK(!view->DoMessage(message));
        view->SetKeyAccelerator(nullptr);

        // Advice links and the HTML link message.
        message = cGZMessage{};
        host.adviceLinks = 0;
        for (std::uint32_t type : {kMsgViewAdviceLink, kMsgViewAdviceLink2, kMsgViewAdviceLink3}) {
            message.type = type;
            view->DoMessage(message);
        }
        CHECK(host.adviceLinks == 3);
        host.urlHandles = 0;
        host.urlResult = false;
        message = cGZMessage{};
        message.type = kMsgViewHtmlLink;
        message.param2 = reinterpret_cast<std::uint64_t>(static_cast<cIGZString*>(&cheatText));
        CHECK(view->DoMessage(message));                           // DoURL returned false -> handled
        CHECK(host.urlHandles == 1);
        host.urlResult = true;
        CHECK(!view->DoMessage(message));                          // DoURL returned true -> not handled (quirk)

        // An unhandled type falls through to cGZWin::DoMessage.
        message = cGZMessage{};
        message.type = 0x7777;
        CHECK(!view->DoMessage(message));
        host.cityExists = false;
    }

    // ------------------------------------------------------------------ GZPaint
    {
        View3DWinTestAccess::PaintEnabled(*view) = true;
        View3DWinTestAccess::FrameTimeThreshold(*view) = 0.0f;      // run the frame block every time
        View3DWinTestAccess::Scrolling(*view) = false;
        host.costUpdates = 0;
        host.frameMarkers = 0;
        renderer.draws = 0;
        CHECK(view->GZPaint());
        CHECK(host.costUpdates == 1 && host.lastCostDt >= 0.0f && host.lastCostDt <= 0.5f);
        CHECK(host.frameMarkers == 1 && host.lastFrameMarker == host.frameCounter);
        CHECK(renderer.draws == 1 && renderer.drawContextMarker == host.frameCounter);
        // The frame only resumes the simulation once the hidden timeout has passed (2 s by default, so
        // the pause started above survives this frame).
        View3DWinTestAccess::SimHidden(*view) = false;
        view->DoTemporarySimHiddenPause();
        CHECK(View3DWinTestAccess::SimHidden(*view));
        CHECK(view->GZPaint());
        CHECK(View3DWinTestAccess::SimHidden(*view));
        // A deferred message is drained only when popups are safe.
        auto* deferred = new cRZMessage2Standard();
        deferred->SetType(0x5150);
        View3DWinTestAccess::Deferred(*view).push_back(cRZAutoRefCount<cIGZMessage2>(deferred));
        host.popupSafe = false;
        host.server.posted.clear();
        CHECK(view->GZPaint() && host.server.posted.empty());
        CHECK(View3DWinTestAccess::Deferred(*view).size() == 1);
        host.popupSafe = true;
        host.popupEnabled = true;
        CHECK(view->GZPaint());
        CHECK(View3DWinTestAccess::Deferred(*view).empty());
        CHECK(host.server.posted.size() == 1 && host.server.posted[0] == 0x5150);
        host.popupSafe = false;
        host.popupEnabled = false;
        // A scrolling frame moves the camera by the angle / speed and consumes the stop speed.
        View3DWinTestAccess::Scrolling(*view) = true;
        View3DWinTestAccess::Stopping(*view) = true;
        View3DWinTestAccess::ScrollSpeed(*view) = 0.5f;
        View3DWinTestAccess::ScrollStopSpeed(*view) = 0.25f;
        View3DWinTestAccess::ScrollAngle(*view) = 0.0f;
        host.gameMessages.clear();
        const int draws = renderer.draws;
        CHECK(view->GZPaint());
        CHECK(renderer.draws == draws + 1);
        CHECK(std::fabs(View3DWinTestAccess::ScrollSpeed(*view) - 0.25f) < 1e-6f);
        CHECK(View3DWinTestAccess::Scrolling(*view));               // not finished yet
        CHECK(view->GZPaint());
        CHECK(!View3DWinTestAccess::Scrolling(*view));              // the stop speed caught up
        CHECK(host.gameMessages.size() == 1 && host.gameMessages[0] == kMsgViewScrollEnd);
        View3DWinTestAccess::Stopping(*view) = false;
        View3DWinTestAccess::PaintEnabled(*view) = false;
        View3DWinTestAccess::SimHidden(*view) = false;              // do not carry it into Shutdown
    }

    // ------------------------------------------------------------------ the mouse handlers
    {
        ControlLog stackLog, persistLog;
        auto* stacked = new TestControl(stackLog);
        stacked->AddRef();
        auto* persistent = new TestControl(persistLog);
        persistent->AddRef();
        CHECK(view->AddPersistentViewInputControl(persistent));
        persistent->Release();
        CHECK(view->SetCurrentViewInputControl(stacked, 0));
        stacked->Release();

        // The router (SL::spCity vslot 0x290) sees every button first; while it is active and refuses the
        // point the view consumes the event and the tools never see it.
        host.router.active = true;
        host.router.handled = false;
        host.router.calls = 0;
        CHECK(view->GZOnMouseDownL(10, 20, 0x5));
        CHECK(host.router.calls == 1 && host.router.lastFlags == -1);
        CHECK(persistLog.downL == 0 && stackLog.downL == 0);
        CHECK(View3DWinTestAccess::LastModifiers(*view) == 0x5);
        // When the router handles it the persistent tools come first, then the stack.
        host.router.handled = true;
        CHECK(view->GZOnMouseDownL(10, 20, 0x6));
        CHECK(persistLog.downL == 1 && stackLog.downL == 1);
        CHECK(View3DWinTestAccess::LastModifiers(*view) == 0x6);
        // The router rewrites the point in place and the tools get the rewritten one.
        host.router.mapX = 77;
        host.router.mapY = 88;
        CHECK(view->GZOnMouseDownL(1, 2, 0));
        CHECK(persistLog.downL == 2 && stackLog.downL == 2);
        host.router.mapX = host.router.mapY = 0;
        // The bottom stacked tool's answer is not looked at (quirk), but it is still called.
        stackLog.handleDownL = true;
        CHECK(view->GZOnMouseDownL(3, 4, 0));
        CHECK(stackLog.downL == 3);
        stackLog.handleDownL = false;

        // GZOnMouseDownR takes the capture, stores the button position and pushes the city-scroll tool.
        ControlLog scrollLog;
        auto* scrollTool = new TestControl(scrollLog);
        host.toolToCreate = scrollTool;
        host.cityScrollSets = 0;
        mgr.capture = nullptr;
        CHECK(view->GZOnMouseDownR(30, 40, 0x2));
        CHECK(persistLog.downR == 1 && stackLog.downR == 1);
        CHECK(mgr.capture == static_cast<cIGZWin*>(static_cast<cGZWin*>(view)));
        CHECK(host.cityScrollSets == 1 && host.lastScrollX == 30 && host.lastScrollY == 40);
        CHECK(View3DWinTestAccess::MouseDownX(*view) == 30 && View3DWinTestAccess::MouseDownY(*view) == 40);
        CHECK(!View3DWinTestAccess::DraggedPastDeadZone(*view));
        CHECK(view->GetCurrentViewInputControl() == scrollTool);
        CHECK(scrollLog.inits == 1);
        host.toolToCreate = nullptr;

        // GZOnMouseUpL recalculates the scroll while the current, non-self-scrolling tool holds the capture.
        host.preferences[kPrefsEdgeScrollEnabled] = 0;
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;
        View3DWinTestAccess::KeyboardScrollX(*view) = 1;
        CHECK(view->GZOnMouseUpL(5, 6, 0));
        CHECK(scrollLog.upL == 1 && persistLog.upL == 1 && stackLog.upL == 1);
        CHECK(View3DWinTestAccess::Scrolling(*view));           // the capture + RecalcScrollVelocity ran
        View3DWinTestAccess::KeyboardScrollX(*view) = 0;
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;

        // GZOnMouseUpR: the picked model's world-bounds centre becomes the camera target, and both terrain
        // heights are queried and discarded (quirk, 0x100783561 / 0x100783574).
        View3DWinTestAccess::DraggedPastDeadZone(*view) = false;
        FakeModelInstance model;
        model.bounds.min[0] = 0.0f; model.bounds.min[1] = 2.0f; model.bounds.min[2] = 4.0f;
        model.bounds.max[0] = 10.0f; model.bounds.max[1] = 6.0f; model.bounds.max[2] = 20.0f;
        renderer.pickResult = &model;
        host.terrain.heights = host.terrain.heightsAt = 0;
        CHECK(view->GZOnMouseUpR(50, 60, 0));
        CHECK(model.worldBounds == 1 && model.releases == 1);
        CHECK(host.terrain.heights == 1 && host.terrain.heightsAt == 1);
        CHECK(camera->ViewTarget().x == 5.0f && camera->ViewTarget().y == 4.0f &&
              camera->ViewTarget().z == 12.0f);
        CHECK(persistLog.upR == 1 && scrollLog.upR == 1);
        CHECK(mgr.capture == nullptr);                          // released
        // Without a model the terrain pick answers the target and the height queries are skipped.
        renderer.pickResult = nullptr;
        renderer.pickTerrainResult = true;
        host.terrain.heights = host.terrain.heightsAt = 0;
        CHECK(view->GZOnMouseUpR(1, 2, 0));
        CHECK(host.terrain.heights == 0 && host.terrain.heightsAt == 0);
        CHECK(camera->ViewTarget().x == 1.5f && camera->ViewTarget().y == 2.5f);
        renderer.pickTerrainResult = false;
        // Past the dead zone the camera is left alone (but the tools still see the release).
        View3DWinTestAccess::DraggedPastDeadZone(*view) = true;
        camera->SetViewTargetPosition({0.0f, 0.0f, 0.0f});
        CHECK(view->GZOnMouseUpR(9, 9, 0));
        CHECK(camera->ViewTarget().x == 0.0f && camera->ViewTarget().y == 0.0f);
        View3DWinTestAccess::DraggedPastDeadZone(*view) = false;

        // GZOnMouseMove: dead zone, cursor text and the edge-scroll inputs. RecalcScrollVelocity clears the
        // two edge inputs when the preference at 0xf09 is set, so the direction is observed on the scroll.
        host.preferences[kPrefsEdgeScrollEnabled] = 1;
        host.router.active = true;
        host.router.handled = true;
        host.router.allowEdgeScroll = true;
        View3DWinTestAccess::MouseDownX(*view) = 400;
        View3DWinTestAccess::MouseDownY(*view) = 300;
        View3DWinTestAccess::DraggedPastDeadZone(*view) = false;
        host.costUpdates = 0;
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;
        camera->SetZoom(0.0f);
        CHECK(view->GZOnMouseMove(400, 300, 0));
        CHECK(!View3DWinTestAccess::DraggedPastDeadZone(*view));
        CHECK(View3DWinTestAccess::EdgeScrollX(*view) == 0 && View3DWinTestAccess::EdgeScrollY(*view) == 0);
        CHECK(View3DWinTestAccess::CursorTextX(*view) == 400 && View3DWinTestAccess::CursorTextY(*view) == 300);
        CHECK(host.costUpdates == 1);
        CHECK(view->GZOnMouseMove(799, 300, 0));               // right edge -> east
        CHECK(View3DWinTestAccess::Scrolling(*view));
        CHECK(std::fabs(View3DWinTestAccess::ScrollAngle(*view) - 0.0f) < 1e-6f);
        View3DWinTestAccess::Scrolling(*view) = false;
        CHECK(view->GZOnMouseMove(0, 300, 0));                 // left edge -> west (pi)
        CHECK(std::fabs(View3DWinTestAccess::ScrollAngle(*view) - 3.14159274f) < 1e-5f);
        View3DWinTestAccess::Scrolling(*view) = false;
        CHECK(view->GZOnMouseMove(400, 599, 0));               // bottom edge -> 90 degrees
        CHECK(std::fabs(View3DWinTestAccess::ScrollAngle(*view) - 1.57079637f) < 1e-5f);
        CHECK(View3DWinTestAccess::DraggedPastDeadZone(*view));   // 299 pixels on one axis
        // With the preference off the edge inputs are cleared and nothing scrolls.
        host.preferences[kPrefsEdgeScrollEnabled] = 0;
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;
        CHECK(view->GZOnMouseMove(799, 599, 0));
        CHECK(View3DWinTestAccess::EdgeScrollX(*view) == 0 && View3DWinTestAccess::EdgeScrollY(*view) == 0);
        // A self-scrolling current tool leaves the edge inputs alone when the graphics system allows it.
        ControlLog selfLog;
        selfLog.selfScrolling = true;
        auto* selfTool = new TestControl(selfLog);
        selfTool->AddRef();
        CHECK(view->SetCurrentViewInputControl(selfTool, 0));
        selfTool->Release();
        host.preferences[kPrefsEdgeScrollEnabled] = 1;
        host.gsSelfScroll = false;
        View3DWinTestAccess::EdgeScrollX(*view) = 0;
        View3DWinTestAccess::EdgeScrollY(*view) = 0;
        View3DWinTestAccess::Scrolling(*view) = false;
        CHECK(view->GZOnMouseMove(10, 10, 0));                 // both inputs already 0: returns early
        CHECK(!View3DWinTestAccess::Scrolling(*view));
        View3DWinTestAccess::EdgeScrollX(*view) = 1;
        View3DWinTestAccess::Scrolling(*view) = false;
        CHECK(view->GZOnMouseMove(10, 10, 0));                 // the edge input survives into RecalcScrollVelocity
        CHECK(View3DWinTestAccess::Scrolling(*view));
        host.gsSelfScroll = true;
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;
        View3DWinTestAccess::EdgeScrollX(*view) = 0;
        CHECK(view->GZOnMouseMove(10, 10, 0));                 // the graphics system paused the scrolling
        CHECK(!View3DWinTestAccess::Scrolling(*view));
        host.gsSelfScroll = false;
        host.preferences[kPrefsEdgeScrollEnabled] = 0;
        View3DWinTestAccess::Scrolling(*view) = false;
        View3DWinTestAccess::Stopping(*view) = false;

        CHECK(view->RemoveAllPersistentViewInputControls());
        CHECK(!view->RemoveAllViewInputControls(true));
        host.router.active = false;
        host.router.handled = true;
    }

    // ------------------------------------------------------------------ cursor text
    {
        FakeCalloutBox& box = host.cursorWindow;
        box.window_->HideWindow();
        box.window_->shows = box.window_->hides = box.window_->invalidates = 0;
        box.positions = 0;
        box.line1Sets = box.line2Sets = box.iconSets = 0;
        View3DWinTestAccess::CursorTextBox(*view) = &box;
        View3DWinTestAccess::CursorTextIcon(*view) = reinterpret_cast<cIGZBuffer*>(0xabc);
        View3DWinTestAccess::CursorText(*view).clear();
        cRZString lineA("A"), lineB("B"), lineC("C"), lineD("D");

        view->SetCursorText(1, 10, &lineA, &lineB, 1);
        CHECK(View3DWinTestAccess::CursorText(*view).size() == 1);
        CHECK(box.line1 == "A" && box.line2 == "B");
        CHECK(box.icon == reinterpret_cast<cIGZBuffer*>(0xabc));
        CHECK(box.window_->IsVisible() && box.window_->shows == 1);
        CHECK(box.positions == 1 && box.point.x == 0 && box.point.y == 0);
        // A second entry is inserted in front of the last node (orders 20 then 10): the window still shows
        // the *last* node, which is entry 1.
        view->SetCursorText(2, 20, &lineC, &lineD, 0);
        CHECK(View3DWinTestAccess::CursorText(*view).size() == 2);
        CHECK(View3DWinTestAccess::CursorText(*view).front().key == 2);
        CHECK(box.line1 == "A" && box.line2 == "B");
        CHECK(box.icon == reinterpret_cast<cIGZBuffer*>(0xabc));   // still the last node
        // Order 5 is smaller than every stored order, so the original appends it at the end.
        view->SetCursorText(3, 5, &lineC, &lineD, 0);
        CHECK(View3DWinTestAccess::CursorText(*view).back().key == 3);
        CHECK(box.line1 == "C" && box.line2 == "D");
        // Order 40 lands in front of the first node (from the back) whose order is <= 40.
        view->SetCursorText(4, 40, &lineA, &lineB, 0);
        CHECK(View3DWinTestAccess::CursorText(*view).size() == 4);
        CHECK(View3DWinTestAccess::CursorText(*view).back().key == 3);
        // Updating an entry: a missing second line erases the *first* line (quirk, 0x10079144a).
        view->SetCursorText(2, 20, &lineC, nullptr, 0);
        {
            CursorTextInfo& info = View3DWinTestAccess::CursorText(*view).front();
            CHECK(info.key == 2 && info.line1.Strlen() == 0);
            CHECK(std::strcmp(info.line2.ToChar(), "D") == 0);
        }
        // Both strings empty: ClearCursorText runs (it calls CursorTextChanged) and SetCursorText calls it
        // once more, so the box sees two updates (quirk, 0x1007913c6).
        box.line1Sets = 0;
        view->SetCursorText(2, 20, nullptr, nullptr, 0);
        CHECK(box.line1Sets == 2);
        CHECK(View3DWinTestAccess::CursorText(*view).size() == 3);
        CHECK(View3DWinTestAccess::CursorText(*view).front().key != 2);
        // ClearCursorText walks the list from the back and erases the first match.
        view->ClearCursorText(4);
        CHECK(View3DWinTestAccess::CursorText(*view).size() == 2);
        view->ClearCursorText(1);
        view->ClearCursorText(3);
        CHECK(View3DWinTestAccess::CursorText(*view).empty());
        box.window_->hides = box.window_->invalidates = 0;
        view->ClearCursorText(0x99);                            // no entry: only the empty-list update
        CHECK(box.window_->hides == 1 && box.window_->invalidates == 1);

        // UpdateCursorTextPositions only moves the box while it is visible, then updates the cost indicator
        // with a zero delta.
        box.window_->HideWindow();
        box.positions = 0;
        host.costUpdates = 0;
        view->UpdateCursorTextPositions(12, 34);
        CHECK(box.positions == 0);
        CHECK(View3DWinTestAccess::CursorTextX(*view) == 12 && View3DWinTestAccess::CursorTextY(*view) == 34);
        CHECK(host.costUpdates == 1 && host.lastCostDt == 0.0f);
        box.window_->ShowWindow();
        view->UpdateCursorTextPositions(56, 78);
        CHECK(box.positions == 1 && box.point.x == 56 && box.point.y == 78);
        View3DWinTestAccess::CursorTextBox(*view) = nullptr;
        View3DWinTestAccess::CursorTextIcon(*view) = nullptr;
    }

    // ------------------------------------------------------------------ panels and the progress screen
    {
        // HideMissionStatusPanel ChildDeletes the panel and drops its reference.
        auto* mission = new cGZWin(&mgr);
        mission->AddRef();
        View3DWinTestAccess::MissionPanel(*view) = mission;
        view->HideMissionStatusPanel();
        CHECK(View3DWinTestAccess::MissionPanel(*view) == nullptr);
        view->HideMissionStatusPanel();                         // nothing left
        // CloseSecondaryMenu hides, ChildDeletes and releases the menu window and clears its state word.
        auto* menu = new cGZWin(&mgr);
        menu->AddRef();
        menu->SetArea(0, 0, 10, 10);
        View3DWinTestAccess::SecondaryMenu(*view) = menu;
        View3DWinTestAccess::SecondaryMenuState(*view) = 7;
        view->CloseSecondaryMenu();
        CHECK(View3DWinTestAccess::SecondaryMenu(*view) == nullptr);
        CHECK(View3DWinTestAccess::SecondaryMenuState(*view) == 0);
        view->CloseSecondaryMenu();
        // CreateProgressScreen stores one reference and calls Init; UpdateProgressMessage captions id 1
        // with the empty string the original passes.
        host.progress.refs = 0;
        host.progressInits = 0;
        host.progressCaptions = 0;
        View3DWinTestAccess::ProgressScreen(*view) = nullptr;
        view->CreateProgressScreen(true);
        CHECK(View3DWinTestAccess::ProgressScreen(*view) == &host.progress);
        CHECK(host.progress.refs == 1 && host.progressInits == 1);
        view->UpdateProgressMessage();
        CHECK(host.progressCaptions == 1 && host.lastCaptionId == 1 && host.lastCaption.empty());
        View3DWinTestAccess::ProgressScreen(*view) = nullptr;
        host.progress.refs = 0;
        view->UpdateProgressMessage();                          // no screen: no call
        CHECK(host.progressCaptions == 1);
        view->CreateProgressScreen(false);                      // the screen Shutdown will tear down
        CHECK(View3DWinTestAccess::ProgressScreen(*view) == &host.progress && host.progress.refs == 1);
    }

    // ------------------------------------------------------------------ the animation recorder
    {
        FakeAnimationRecorder recorder(&mgr);
        host.recorder = &recorder;
        host.recordingSetups = 0;
        recorder.refs = 0;
        view->AddWinAnimationRecorder(true);
        CHECK(host.recordingSetups == 1);
        CHECK(host.recordingDirectory == "/Users/test/SimCity 4RecordedAnimations/");
        CHECK(recorder.GetID() == kView3DAnimationRecorderId);
        CHECK(recorder.refs == 1);                             // the view released its reference; ChildAdd keeps one
        CHECK(view->GetChildWindowFromID(kView3DAnimationRecorderId) == &recorder);
        view->AddWinAnimationRecorder(true);                    // already there: nothing happens
        CHECK(host.recordingSetups == 1);
        view->AddWinAnimationRecorder(false);                   // removes the child, does not release it
        CHECK(view->GetChildWindowFromID(kView3DAnimationRecorderId) == nullptr);
        host.recorder = nullptr;
        view->AddWinAnimationRecorder(true);                    // no recorder from the host: no crash
        CHECK(host.recordingSetups == 1);
    }

    // ------------------------------------------------------------------ the tool invokers
    {
        host.router.active = false;
        CHECK(!view->RemoveAllViewInputControls(true));

        // InvokeZoningTool asks the city first (vslot 0xb0) and pushes the tool with operation 2.
        ControlLog zoneLog;
        auto* zoneTool = new TestControl(zoneLog);
        host.toolToCreate = zoneTool;
        host.toolTypes.clear();
        host.canZone = true;
        view->InvokeZoningTool(0x1234);
        CHECK(host.toolTypes.size() == 1 && host.toolTypes[0] == 0x1234);
        CHECK(view->GetCurrentViewInputControl() == zoneTool);
        CHECK(zoneLog.inits == 1);
        host.canZone = false;
        view->InvokeZoningTool(0x2222);
        CHECK(host.toolTypes.size() == 1);
        host.canZone = true;
        CHECK(!view->RemoveAllViewInputControls(true));

        ControlLog scrubLog;
        auto* scrub = new TestControl(scrubLog);
        host.toolToCreate = scrub;
        host.toolFlags.clear();
        view->InvokeLandscapeTool(9, true);
        CHECK(host.toolTypes.size() == 2 && host.toolTypes[1] == 9);
        CHECK(host.toolFlags.size() == 1 && host.toolFlags[0]);
        CHECK(view->GetCurrentViewInputControl() == scrub);
        CHECK(!view->RemoveAllViewInputControls(true));

        // ApplyDefaultTerrainBrush: brush 7 is ignored, otherwise the terrain answers the landscape type.
        host.terrain.brushCalls = 0;
        host.terrain.brushResult = true;
        view->ApplyDefaultTerrainBrush(7, false);
        CHECK(host.terrain.brushCalls == 0);
        ControlLog brushLog;
        auto* brushTool = new TestControl(brushLog);
        host.toolToCreate = brushTool;
        view->ApplyDefaultTerrainBrush(3, true);
        CHECK(host.terrain.brushCalls == 1 && host.terrain.brush == 3);
        CHECK(host.toolTypes.size() == 3 && host.toolTypes[2] == 0x77);
        CHECK(host.toolFlags.size() == 2 && host.toolFlags[1]);
        CHECK(!view->RemoveAllViewInputControls(true));

        // ChangeTerrainType writes the dialog's answer back to the terrain.
        host.terrain.type = 5;
        host.terrain.typeSets = 0;
        host.terrainDialogs = 0;
        host.dialogResult = {11};
        view->ChangeTerrainType();
        CHECK(host.terrainDialogs == 1 && host.terrain.type == 11 && host.terrain.typeSets == 1);
        host.dialogResult.clear();

        // RunGlobalTerrainTool: the sound first, then the per-case terrain / message / tool work.
        host.globalTerrainSounds = 0;
        host.server.posted.clear();
        view->RunGlobalTerrainTool(5);
        CHECK(host.globalTerrainSounds == 1);
        CHECK(host.server.posted.size() == 1 && host.server.posted[0] == kMsgViewTerrainDisplayChanged);
        CHECK(host.server.lastData1 == 0);
        view->RunGlobalTerrainTool(6);
        CHECK(host.server.lastData1 == 1);
        host.terrain.mode158 = 0;
        view->RunGlobalTerrainTool(1);
        CHECK(host.terrain.mode158 == 1);
        host.terrain.mode160 = 0;
        view->RunGlobalTerrainTool(2);
        CHECK(host.terrain.mode160 == 1);
        host.terrain.cancels = 0;
        host.terrain.activeFlags = 0;
        view->RunGlobalTerrainTool(3);
        CHECK(host.terrain.cancels == 1 && host.terrain.activeFlag);
        view->RunGlobalTerrainTool(4);
        CHECK(host.terrain.cancels == 2 && !host.terrain.activeFlag);
        host.terrainDialogs = 0;
        view->RunGlobalTerrainTool(0);                          // case 0 is the terrain-type dialog
        CHECK(host.terrainDialogs == 1);
        ControlLog levelLog;
        auto* levelTool = new TestControl(levelLog);
        host.toolToCreate = levelTool;
        view->RunGlobalTerrainTool(7);
        CHECK(view->GetCurrentViewInputControl() == levelTool);
        host.toolToCreate = nullptr;
        CHECK(!view->RemoveAllViewInputControls(true));

        // ToggleContourDisplay flips its flag and adds / removes the contour layer's callback.
        host.contourAdds = host.contourRemoves = 0;
        host.nextContour = 0x99;
        CHECK(view->ToggleContourDisplay());
        CHECK(host.contourAdds == 1);
        CHECK(view->ToggleContourDisplay() == false);
        CHECK(host.contourRemoves == 1 && host.lastContourRemoved == 0x99);
        CHECK(view->ToggleContourDisplay());                    // adds it again
        CHECK(host.contourAdds == 2);
        host.projectResult = true;
        host.projectedPoint = {12.75f, 34.5f, 0.0f};
        View3DWinTestAccess::CursorTextBox(*view) = &host.cursorWindow;
        host.cursorWindow.window_->ShowWindow();
        host.cursorWindow.positions = 0;
        view->SetCursorTextPosition3D(cS3DVector3{1.0f, 2.0f, 3.0f});
        CHECK(host.cursorWindow.positions == 1);
        CHECK(View3DWinTestAccess::CursorTextX(*view) == 12 && View3DWinTestAccess::CursorTextY(*view) == 34);
        host.projectedPoint = {5.0f, 6.0f, 0.0f};
        host.projectResult = false;
        host.cursorWindow.positions = 0;
        view->SetCursorTextPosition3D(cS3DVector3{1.0f, 2.0f, 3.0f});
        CHECK(host.cursorWindow.positions == 0);
        CHECK(View3DWinTestAccess::CursorTextX(*view) == 12);   // unchanged
        View3DWinTestAccess::CursorTextBox(*view) = nullptr;
    }

    // ------------------------------------------------------------------ the GUI: panel refresh, render properties, RemoveGUI
    {
        host.cityExists = true;                    // UpdateBudget returns early without a city
        // The toolbar AddGUI would build: five buttons and the title text, all children of +0x280.
        auto* panel = new cGZWin(&mgr);
        auto* zoomIn = new cGZWin(&mgr); zoomIn->SetID(kPanelZoomInButtonId);
        auto* zoomOut = new cGZWin(&mgr); zoomOut->SetID(kPanelZoomOutButtonId);
        auto* newCity = new cGZWin(&mgr); newCity->SetID(kPanelNewCityButtonId);
        auto* mayor = new cGZWin(&mgr); mayor->SetID(kPanelMayorButtonId);
        auto* obliterate = new cGZWin(&mgr); obliterate->SetID(kPanelObliterateButtonId);
        auto* title = new cGZWin(&mgr); title->SetID(kPanelCityNameId);
        panel->ChildAdd(zoomIn);
        panel->ChildAdd(zoomOut);
        panel->ChildAdd(newCity);
        panel->ChildAdd(mayor);
        panel->ChildAdd(obliterate);
        panel->ChildAdd(title);
        newCity->SetFlag(kWinEnabled, false);
        mayor->SetFlag(kWinEnabled, false);
        obliterate->SetFlag(kWinEnabled, false);
        View3DWinTestAccess::PanelWindow(*view) = panel;

        // UpdateZoomButtons: the two zoom buttons follow CanZoomIn / CanZoomOut.
        camera->SetZoom(2.0f);
        View3DWinTestAccess::ZoomMin(*view) = 0;
        View3DWinTestAccess::ZoomMax(*view) = 5;
        view->UpdateZoomButtons();
        CHECK(zoomIn->GetFlag(kWinEnabled) && zoomOut->GetFlag(kWinEnabled));
        View3DWinTestAccess::ZoomMax(*view) = 2;
        view->UpdateZoomButtons();
        CHECK(!zoomIn->GetFlag(kWinEnabled) && zoomOut->GetFlag(kWinEnabled));
        View3DWinTestAccess::ZoomMax(*view) = 5;

        // UpdateCityName copies the city's name into the toolbar's text 2.
        host.cityName = "Testville";
        view->UpdateCityName();
        CHECK(std::strcmp(title->GetCaption().ToChar(), "Testville") == 0);
        host.cityName = "New Name";
        view->UpdateCityName();
        CHECK(std::strcmp(title->GetCaption().ToChar(), "New Name") == 0);

        // UpdatePopulation formats the residential simulator's population through the language manager.
        auto* population = new FakeTextControl(&mgr);
        population->AddRef();
        View3DWinTestAccess::PopulationText(*view) = population;
        host.population = 4242;
        view->UpdatePopulation();
        CHECK(host.numberFormats == 1 && population->caption == "4242");
        host.population = -1;
        view->UpdatePopulation();
        CHECK(population->caption == "-1");

        // UpdateBudget: money string, colour by sign, and the open budget dialog refresh.
        auto* budget = new FakeTextControl(&mgr);
        budget->AddRef();
        View3DWinTestAccess::BudgetText(*view) = budget;
        host.funds = 5000;
        view->UpdateBudget();
        CHECK(host.moneyFormats == 1 && budget->caption == "$5000");
        CHECK(budget->colors[0] == 0x445566 && host.lastNative == 0x445566);
        host.funds = -25;
        view->UpdateBudget();
        CHECK(budget->caption == "$-25" && budget->colors[0] == 0x770000 && host.lastNative == 0x770000);
        host.budgetOpen = false;
        host.budgetOpenCalls = 0;
        auto* budgetProc = new FakeBudgetProc(); budgetProc->AddRef();
        View3DWinTestAccess::BudgetUIWinProc(*view) = budgetProc;
        view->UpdateBudget();
        CHECK(host.budgetOpenCalls == 1 && host.budgetRefreshes == 0);
        host.budgetOpen = true;
        view->UpdateBudget();
        CHECK(host.budgetOpenCalls == 2 && host.budgetRefreshes == 1);

        // DoCityEstablished re-enables the three city buttons, drops the active tool, refreshes budget and name.
        ControlLog establishLog;
        auto* tool = new TestControl(establishLog);
        tool->AddRef();                                  // the caller's reference (the view AddRefs too)
        CHECK(view->SetCurrentViewInputControl(tool, 0));
        CHECK(view->GetCurrentViewInputControl() == tool);
        const int moneyBefore = host.moneyFormats;
        view->DoCityEstablished();
        CHECK(newCity->GetFlag(kWinEnabled) && mayor->GetFlag(kWinEnabled) && obliterate->GetFlag(kWinEnabled));
        CHECK(view->GetCurrentViewInputControl() == nullptr);
        CHECK(establishLog.deactivations == 1 && establishLog.shutdowns == 1);
        tool->Release();                                 // drop the caller's reference
        CHECK(host.moneyFormats == moneyBefore + 1);
        CHECK(std::strcmp(title->GetCaption().ToChar(), "New Name") == 0);

        // UpdateDayNightButtons: real-time / noon / midnight, from the clock's visual mode.
        auto* menu = new cGZWin(&mgr);
        auto* realTime = new FakePanelButton(&mgr); realTime->SetID(kPanelRealTimeButtonId);
        auto* noon = new FakePanelButton(&mgr); noon->SetID(kPanelVisualModeButtonIds[0]);
        auto* midnight = new FakePanelButton(&mgr); midnight->SetID(kPanelVisualModeButtonIds[1]);
        menu->ChildAdd(realTime);
        menu->ChildAdd(noon);
        menu->ChildAdd(midnight);
        View3DWinTestAccess::SecondaryMenu(*view) = menu;
        host.visualTimeMode = 0;
        view->UpdateDayNightButtons();
        CHECK(realTime->IsOn() && !noon->IsOn() && !midnight->IsOn());
        host.visualTimeMode = 2;
        view->UpdateDayNightButtons();
        CHECK(!realTime->IsOn() && !noon->IsOn() && midnight->IsOn());
        host.visualTimeMode = 1;
        view->UpdateDayNightButtons();
        CHECK(!realTime->IsOn() && noon->IsOn() && !midnight->IsOn());
        View3DWinTestAccess::SecondaryMenu(*view) = nullptr;   // RemoveGUI does not own the menus
        delete menu;

        // SetRenderProperties: bool toggle / parse, int, float, an unknown name, and the notification each
        // accepted property posts. The ini callback applies the same setter.
        host.gameMessages.clear();
        cRZString name, value;
        name.AssignFromCString(host.globalProperties.NameFromBoolPropertyID(kPropRenderShadows));
        const bool shadowsBefore = host.globalProperties.BoolValue(kPropRenderShadows);
        CHECK(view->SetRenderProperties(name, cRZString()));            // empty: toggle
        CHECK(host.globalProperties.BoolValue(kPropRenderShadows) == !shadowsBefore);
        CHECK(host.gameMessages.size() == 1 && host.gameMessages[0] == kMsgRenderPropertiesChanged);
        value.AssignFromCString("true");
        CHECK(view->SetRenderProperties(name, value));
        CHECK(host.globalProperties.BoolValue(kPropRenderShadows));
        value.AssignFromCString("false");
        CHECK(view->SetRenderProperties(name, value));
        CHECK(!host.globalProperties.BoolValue(kPropRenderShadows));
        cRZString intName;
        intName.AssignFromCString(host.globalProperties.NameFromIntPropertyID(0));
        value.AssignFromCString("42");
        CHECK(view->SetRenderProperties(intName, value));
        CHECK(host.globalProperties.IntValue(0) == 42);
        cRZString floatName;
        floatName.AssignFromCString(host.globalProperties.NameFromFloatPropertyID(0));
        value.AssignFromCString("2.5");
        CHECK(view->SetRenderProperties(floatName, value));
        CHECK(host.globalProperties.FloatValue(0) == 2.5f);
        cRZString unknown, one;
        unknown.AssignFromCString("notAProperty");
        one.AssignFromCString("1");
        const std::size_t messagesBefore = host.gameMessages.size();
        CHECK(!view->SetRenderProperties(unknown, one));
        CHECK(host.gameMessages.size() == messagesBefore);
        cSC4View3DWin::SetRenderPropertiesStatic(intName, one, view);
        CHECK(host.globalProperties.IntValue(0) == 1);
        cSC4View3DWin::SetRenderPropertiesStatic(intName, value, nullptr);   // no context: nothing happens

        // RemoveGUI: every window leaves the manager, is shut down and released; the four window procedures with
        // a Shutdown get theirs, the panel procedure and the advisor popup queue are only released.
        auto makeGen = [&mgr]() { auto* window = new FakeGenWindow(&mgr); window->AddRef(); return window; };
        auto makeProc = []() { auto* proc = new FakePanelUIProc(); proc->AddRef(); return proc; };
        View3DWinTestAccess::PanelInterface(*view) = makeGen();
        View3DWinTestAccess::ToolbarWindow(*view) = makeGen();
        View3DWinTestAccess::GuiWindows(*view).push_back(makeGen());
        View3DWinTestAccess::PanelAttach(*view)[0] = makeGen();
        View3DWinTestAccess::PanelUIWinProc(*view) = makeProc();
        auto* tickerProc = new FakeNewsTicker(); tickerProc->AddRef();
        View3DWinTestAccess::NewsTickerWinProc(*view) = tickerProc;
        auto* mysimProc = new FakeMySimProc(); mysimProc->AddRef();
        View3DWinTestAccess::MySimWinProc(*view) = mysimProc;
        View3DWinTestAccess::Window470(*view) = makeGen();
        View3DWinTestAccess::Window478(*view) = makeGen();
        auto* graphsProc = new FakeGraphsProc(); graphsProc->AddRef();
        View3DWinTestAccess::GraphsViewWinProc(*view) = graphsProc;
        auto* buildingProc = new FakeBuildingSelectProc(); buildingProc->AddRef();
        View3DWinTestAccess::BuildingSelectWinProc(*view) = buildingProc;
        View3DWinTestAccess::Window4a0(*view) = makeGen();
        View3DWinTestAccess::Window4a8(*view) = makeGen();
        View3DWinTestAccess::Window4b0(*view) = makeGen();
        auto* queue = new FakeAdvisorQueue(); queue->AddRef();
        View3DWinTestAccess::AdvisorPopups(*view) = queue;
        auto* dateText = new FakeTextControl(&mgr);
        dateText->AddRef();
        View3DWinTestAccess::DateText(*view) = dateText;
        view->RemoveGUI();
        CHECK(host.newsShutdowns == 1 && host.mysimShutdowns == 1 && host.budgetShutdowns == 1);
        CHECK(host.graphsShutdowns == 1 && host.buildingShutdowns == 1);
        CHECK(tickerProc->refs == 0 && mysimProc->refs == 0 && graphsProc->refs == 0 && buildingProc->refs == 0);
        CHECK(queue->shutdowns == 1 && queue->refs == 0);
        CHECK(View3DWinTestAccess::PanelWindow(*view) == nullptr);
        CHECK(View3DWinTestAccess::PanelInterface(*view) == nullptr);
        CHECK(View3DWinTestAccess::ToolbarWindow(*view) == nullptr);
        CHECK(View3DWinTestAccess::GuiWindows(*view).empty());
        CHECK(View3DWinTestAccess::PanelAttach(*view)[0] == nullptr);
        CHECK(View3DWinTestAccess::PanelUIWinProc(*view) == nullptr);
        CHECK(View3DWinTestAccess::NewsTickerWinProc(*view) == nullptr);
        CHECK(View3DWinTestAccess::MySimWinProc(*view) == nullptr);
        CHECK(View3DWinTestAccess::BudgetUIWinProc(*view) == nullptr);
        CHECK(View3DWinTestAccess::Window470(*view) == nullptr);
        CHECK(View3DWinTestAccess::Window478(*view) == nullptr);
        CHECK(View3DWinTestAccess::GraphsViewWinProc(*view) == nullptr);
        CHECK(View3DWinTestAccess::BuildingSelectWinProc(*view) == nullptr);
        CHECK(View3DWinTestAccess::Window4a0(*view) == nullptr);
        CHECK(View3DWinTestAccess::Window4a8(*view) == nullptr);
        CHECK(View3DWinTestAccess::Window4b0(*view) == nullptr);
        CHECK(View3DWinTestAccess::DateText(*view) == nullptr);
        CHECK(View3DWinTestAccess::BudgetText(*view) == nullptr);
        CHECK(View3DWinTestAccess::PopulationText(*view) == nullptr);
        CHECK(View3DWinTestAccess::AdvisorPopups(*view) == nullptr);
        view->RemoveGUI();                                          // all null: no-op
        view->UpdateZoomButtons();                                  // no panel: no crash
        view->UpdateDayNightButtons();                              // no menu: no crash
        view->UpdatePopulation();                                   // no text: no crash
        host.cityExists = false;
    }

    // ------------------------------------------------------------------ AddGUI: the game's 42 windows
    {
        host.cityExists = true;
        host.canZone = false;                       // AddGUI opens panel 0 and disables two buttons
        host.scriptWindows.clear();
        host.scriptRequests.clear();
        host.imageRequests.clear();
        host.localisedStrings.clear();
        host.compassWindows = host.compassAttaches = host.toolbarButtons = host.mapViewInits = 0;
        host.advisorWindow = FakeAdvisorWindow();

        FakeBuffer skin, background, icon, costBuffer, alertImage;
        host.images.clear();
        host.images[kToolbarButtonSkinInstance] = &skin;
        host.images[kCursorTextBoxImageInstance] = &background;
        host.images[kCursorTextIconInstance] = &icon;
        host.images[kAlertImageDisaster] = &alertImage;
        host.images[kAlertImageSituation] = &alertImage;
        host.images[kAlertImageBusy] = &alertImage;
        skin.width = 96; skin.height = 32;
        background.AddRef();
        icon.AddRef();

        // Every script window AddGUI creates, by the id it asks the factory for.
        auto addWindow = [&](std::uint32_t id) {
            auto* window = new FakeGenWindow(&mgr);
            window->SetID(id);
            window->SetArea(0, 0, 200, 24);
            host.scriptWindows[id] = window;
            return window;
        };
        FakeGenWindow* panel = addWindow(kPanelWindowId);
        auto* title = new FakeTextControl(&mgr); title->SetID(kPanelCityNameId); panel->ChildAdd(title);
        auto* zoomIn = new FakePanelButton(&mgr); zoomIn->SetID(kPanelZoomInButtonId); panel->ChildAdd(zoomIn);
        auto* zoomOut = new FakePanelButton(&mgr); zoomOut->SetID(kPanelZoomOutButtonId); panel->ChildAdd(zoomOut);
        auto* newCity = new FakePanelButton(&mgr); newCity->SetID(kPanelNewCityButtonId); panel->ChildAdd(newCity);
        auto* mayorButton = new FakePanelButton(&mgr); mayorButton->SetID(kPanelMayorButtonId); panel->ChildAdd(mayorButton);
        auto* moduleButton = new FakePanelButton(&mgr); moduleButton->SetID(0x2988bc85u); panel->ChildAdd(moduleButton);
        auto* panel2Button = new FakePanelButton(&mgr); panel2Button->SetID(0x4988bc6au); panel->ChildAdd(panel2Button);
        auto* speed1 = new FakePanelButton(&mgr); speed1->SetID(kSpeedButtonIds[1]); panel->ChildAdd(speed1);
        speed1->image = &skin;
        speed1->area = cRZRect{0, 0, 96, 32};
        // The date text is a child of the toolbar, the budget / population texts of the view.
        auto* dateText = new FakeTextControl(&mgr); dateText->SetID(kDateTextId); panel->ChildAdd(dateText);
        auto* budgetText = new FakeTextControl(&mgr); budgetText->SetID(kBudgetTextId); view->ChildAdd(budgetText);
        auto* popText = new FakeTextControl(&mgr); popText->SetID(kPopulationTextId); view->ChildAdd(popText);
        for (int i = 0; i < 5; ++i) addWindow(kPanelScriptIds[i]);
        // Panel 1 / panel 2 own a child window that AddGUI looks up from the *view*, and so does the compass.
        auto* mySimChild = addWindow(kPanel2ChildId);
        view->ChildAdd(mySimChild);
        auto* panel1Child = addWindow(kPanel1ChildId);
        view->ChildAdd(panel1Child);
        auto* ticker = addWindow(kNewsTickerScriptId);
        auto* tickerControl = new cGZWin(&mgr); tickerControl->SetID(kNewsTickerControlId); ticker->ChildAdd(tickerControl);
        addWindow(kMapViewScriptId);
        addWindow(kMayorPanelScriptId);
        addWindow(kGraphsScriptId);
        addWindow(kBuildingSelectScriptId);
        auto* compassChild = addWindow(kCompassChildId);
        view->ChildAdd(compassChild);

        view->AddGUI();
        // 1. the toolbar panel, its gen view and its placement (viewH = 600, panelH = 24).
        CHECK(View3DWinTestAccess::PanelWindow(*view) == panel);
        CHECK(View3DWinTestAccess::PanelInterface(*view) == static_cast<gz::cIGZWinGen*>(panel));
        CHECK(panel->GetL() == 300 / 3 + 5 || panel->GetL() == 5);
        CHECK(panel->GetT() == (600 + 0xb) - 24);
        CHECK(panel->IsVisible());
        // 2. the toolbar button: id 0x43, the tip string, the panel as its notification target and the skin.
        CHECK(host.toolbarButtons == 1 && View3DWinTestAccess::ToolbarWindow(*view) == host.toolbarButton);
        CHECK(host.toolbarButton->GetID() == kToolbarButtonId);
        CHECK(host.localisedStrings.size() == 1 && host.localisedStrings[0] == kToolbarButtonTipInstance);
        CHECK(skin.transparencySets == 1);
        CHECK(host.toolbarButton->lastImage == &skin && host.toolbarButton->lastStyle == 2);
        CHECK(!host.toolbarButton->IsVisible());
        // 3. the five panel windows, each with its gen view and the panel win proc.
        int guiCount = 5;                                       // the five panels
        for (int i = 0; i < 5; ++i) {
            CHECK(View3DWinTestAccess::PanelAttach(*view)[i] != nullptr);
            CHECK(!View3DWinTestAccess::PanelWindows(*view)[i].empty());
        }
        CHECK(static_cast<FakeGenWindow*>(View3DWinTestAccess::PanelWindows(*view)[0].front())->winProcs == 1);
        CHECK(mySimChild->winProcs == 1 && panel1Child->winProcs == 1);
        guiCount += 2;                                          // panel 1 / panel 2's own children
        CHECK(host.mySimProc != nullptr && host.mySimProc->inits == 1);
        CHECK(host.mySimProc->panel == View3DWinTestAccess::PanelAttach(*view)[2]);
        // 4. the three text controls.
        CHECK(View3DWinTestAccess::DateText(*view) == static_cast<gz::cIGZWinText*>(dateText));
        CHECK(View3DWinTestAccess::BudgetText(*view) == static_cast<gz::cIGZWinText*>(budgetText));
        CHECK(View3DWinTestAccess::PopulationText(*view) == static_cast<gz::cIGZWinText*>(popText));
        // 5. the news ticker and its control child.
        CHECK(host.newsProc != nullptr && host.newsProc->inits == 1);
        CHECK(View3DWinTestAccess::NewsTickerWinProc(*view) == host.newsProc);
        guiCount += 2;                                          // the ticker and its control host
        CHECK(!ticker->IsVisible());
        // 6. the four other windows, the map view and the two procedures.
        CHECK(View3DWinTestAccess::Window4b0(*view) != nullptr);
        CHECK(View3DWinTestAccess::Window4a0(*view) != nullptr);
        CHECK(View3DWinTestAccess::Window4a8(*view) != nullptr);
        CHECK(host.mapViewInits == 1);
        CHECK(host.graphsProc != nullptr && host.graphsProc->inits == 1);
        CHECK(host.buildingProc != nullptr && host.buildingProc->inits == 1);
        CHECK(View3DWinTestAccess::GraphsViewWinProc(*view) == host.graphsProc);
        CHECK(View3DWinTestAccess::BuildingSelectWinProc(*view) == host.buildingProc);
        // 7. the budget procedure, the cursor-text box, its icon and the cost indicator.
        CHECK(host.budgetProc != nullptr && host.budgetProc->inits == 1);
        CHECK(View3DWinTestAccess::CursorTextBox(*view) == &host.cursorWindow);
        CHECK(host.cursorWindow.textFont == kCursorTextTextFont && host.cursorWindow.titleFont == kCursorTextTitleFont);
        CHECK(host.cursorWindow.background == &background);
        CHECK(host.cursorWindow.bgA == -1 && host.cursorWindow.bgB == -1);
        CHECK(View3DWinTestAccess::CursorTextIcon(*view) == &icon);
        CHECK(View3DWinTestAccess::CostIndicatorText(*view) == host.bmpWindow);
        CHECK(host.bmpWindow->setBmps == 1 && host.bmpWindow->imageAreas == 1);
        CHECK(View3DWinTestAccess::CostIndicatorBuffer(*view) != nullptr);
        CHECK(host.bmpWindow->GetWinBMPFlag(6));
        CHECK(host.bmpWindow->imageArea.right == 0x80 && host.bmpWindow->imageArea.bottom == 0x20);
        // 8. the advisor popup queue, the compass window and the compass win proc.
        CHECK(host.advisorQueue != nullptr && host.advisorQueue->inits == 1);
        CHECK(View3DWinTestAccess::AdvisorPopups(*view) == host.advisorQueue);
        CHECK(host.compassWindows == 1 && host.compassWindow != nullptr);
        CHECK(host.compassWindow->GetID() == kCompassWindowId && host.compassAttaches == 1);
        CHECK(compassChild->winProcs == 0);                     // AttachCompassWinProc is the host's job
        // 9. the empty-city start state: panel 0, and the two city buttons disabled.
        CHECK(View3DWinTestAccess::CurrentPanel(*view) == 0);
        CHECK(newCity->GetFlag(kWinEnabled) == false);
        CHECK(mayorButton->GetFlag(kWinEnabled) == false);
        CHECK(View3DWinTestAccess::GuiWindows(*view).size() == static_cast<std::size_t>(guiCount));
        // The script window requests are the panel, the five panels, the ticker and the four other windows.
        CHECK(host.scriptRequests.size() == 11);

        // ---- ShowPanel / HidePanel: the panel's windows follow the visible flag and the sub-panels' state.
        view->ShowPanel(0);
        CHECK(!View3DWinTestAccess::PanelShown(*view)[0] && !View3DWinTestAccess::PanelShown(*view)[1] &&
              !View3DWinTestAccess::PanelShown(*view)[2]);
        host.newsProc->hides = host.newsProc->shows = 0;
        view->ShowPanel(2);                                     // the MySim panel
        CHECK(View3DWinTestAccess::PanelShown(*view)[1]);
        CHECK(host.mySimProc->shows == 1);
        CHECK(host.newsProc->hides == 0);                       // a sub-panel is up: the ticker stays
        const int mySimHides = host.mySimProc->hides;
        view->HidePanel(2);
        CHECK(!View3DWinTestAccess::PanelShown(*view)[1]);
        CHECK(host.mySimProc->hides == mySimHides + 1);
        CHECK(host.newsProc->hides == 1);                       // all three sub-panels are gone
        // ---- ShowMayorSubPanel: each panel id shows its own window and hides the rest.
        host.advisorWindow = FakeAdvisorWindow();
        view->ShowMayorSubPanel(0x49edf9b7u);                   // the advisor window
        CHECK(host.advisorWindow.shows == 1 && View3DWinTestAccess::PanelShown(*view)[0]);
        CHECK(View3DWinTestAccess::MayorSubPanel(*view) == 0x49edf9b7u);
        CHECK(View3DWinTestAccess::MayorSubPanelState(*view) == 0);
        host.budgetProc->visible = false;
        view->ShowMayorSubPanel(0x41u);                          // the budget panel
        CHECK(host.budgetProc->shows == 1);
        CHECK(View3DWinTestAccess::MayorSubPanel(*view) == 0x41u);
        view->ShowMayorSubPanel(0x99887755u);                    // the map view: the budget hides
        CHECK(host.budgetProc->hides == 1);
        CHECK(View3DWinTestAccess::ViewManagerSource(*view) != nullptr);
        view->ShowMayorSubPanel(0x15200002u);                    // the graphs
        CHECK(host.graphsProc->shows == 1);
        host.graphsProc->visible = true;
        view->ShowMayorSubPanel(0);                              // 0 hides everything it can
        CHECK(View3DWinTestAccess::MayorSubPanel(*view) == 0);
        // ---- DoCityObliterated: the obliterate button goes off, panel 4 closes, the toggle comes up.
        View3DWinTestAccess::CurrentPanel(*view) = kPanelBudgetIndex;
        view->DoCityObliterated();
        CHECK(moduleButton->GetFlag(kWinEnabled));               // unaffected: only 0x4988bc6a is disabled
        CHECK(View3DWinTestAccess::CurrentPanel(*view) == kPanelBudgetIndex);
        View3DWinTestAccess::CurrentPanel(*view) = 0;
        // ---- MinimizeUI: hides the toolbar / panel / ticker / menu and records the compass visibility.
        host.newsProc->hides = host.newsProc->shows = 0;
        view->MinimizeUI(true);
        CHECK(!panel->IsVisible());
        CHECK(host.newsProc->hides >= 1);
        view->MinimizeUI(true);                                  // already minimised: no-op
        view->MinimizeUI(false);
        CHECK(panel->IsVisible());
        // ---- UpdateSpeedButtons: the pressed speed button follows the simulator's speed and pause flag.
        host.speedSimulator.speed = 1;
        host.speedSimulator.paused = false;
        View3DWinTestAccess::Simulator(*view) = &host.speedSimulator;
        view->UpdateSpeedButtons();
        CHECK(speed1->GetBtnFlag(gz::kBtnOn));
        CHECK(speed1->hasArea && speed1->lastArea.top == 0 && speed1->lastArea.bottom == 16);
        // ProcessCommand's speed commands: pause toggles the player's pause; a speed sets it and resumes a paused game.
        view->ProcessViewCommand(0x6a935cadu);
        CHECK(host.speedSimulator.userPauses == 1 && host.speedSimulator.paused);
        view->ProcessViewCommand(0x6a935cb5u);
        CHECK(host.speedSimulator.speedSets == 1 && host.speedSimulator.speed == 2 && host.speedSimulator.userResumes == 1);
        view->ProcessViewCommand(0x6a935cadu);
        CHECK(host.speedSimulator.userPauses == 2);
        view->ProcessViewCommand(0x6a935cadu);
        CHECK(host.speedSimulator.userResumes == 2 && !host.speedSimulator.paused);
        host.speedSimulator.speed = 1;
        View3DWinTestAccess::Simulator(*view) = nullptr;
        // ---- UpdateAlertBorder: the three images in the disaster / situation / busy order.
        host.disasterAlert = false;
        host.situationAlert = false;
        host.simulatorBusy = false;
        view->UpdateAlertBorder();
        CHECK(host.compassWindow->images == 1 && host.compassWindow->image == nullptr);
        host.disasterAlert = true;
        view->UpdateAlertBorder();
        CHECK(host.compassWindow->image == &alertImage);
        host.disasterAlert = false;
        host.situationAlert = true;
        view->UpdateAlertBorder();
        CHECK(host.compassWindow->image == &alertImage);
        host.situationAlert = false;
        host.simulatorBusy = true;
        view->UpdateAlertBorder();
        CHECK(host.compassWindow->image == &alertImage);
        host.simulatorBusy = false;
        // ---- DoMessage(cIGZWin*, cGZMessage&): the button notifications and the filter's answer.
        cGZMessage message;
        message.type = kGZMsgCommand;
        message.param1 = 0x287259f7u;                            // the "changed" notification
        message.param2 = 0x4988bc6au;
        host.commands.clear();
        CHECK(view->DoMessage(static_cast<cIGZWin*>(budgetText), message));   // runs DoPanelCommandGUI, not a command-server command
        CHECK(host.commands.empty());
        message.param2 = 0x1234u;                                // an unknown id: nothing runs
        host.commands.clear();
        CHECK(view->DoMessage(static_cast<cIGZWin*>(budgetText), message) && host.commands.empty());
        message.param1 = 0x287259f6u;                            // the button activation (PARTIAL)
        CHECK(view->DoMessage(static_cast<cIGZWin*>(budgetText), message));
        message.type = 0x12;                                     // the size-limit notification
        CHECK(view->DoMessage(static_cast<cIGZWin*>(budgetText), message));
        message.type = 5;
        CHECK(!view->DoMessage(static_cast<cIGZWin*>(budgetText), message));
        budgetText->SetFlag(0x1000, true);                       // a dialog child: not handled
        message.type = kGZMsgCommand;
        CHECK(!view->DoMessage(static_cast<cIGZWin*>(budgetText), message));
        budgetText->SetFlag(0x1000, false);
        // ---- DoPanelCommandGUI: the panel toggles open / close a panel.
        view->ShowPanel(0);
        View3DWinTestAccess::CurrentPanel(*view) = 0;
        panel2Button->flags |= gz::kBtnOn;
        view->DoPanelCommandGUI(0x6a935ca8u);                     // panel 2 on
        CHECK(View3DWinTestAccess::CurrentPanel(*view) == 2);
        panel2Button->flags &= ~gz::kBtnOn;
        view->DoPanelCommandGUI(0x6a935ca8u);                     // panel 2 off again
        CHECK(View3DWinTestAccess::CurrentPanel(*view) == -1);
        // ---- RemoveGUI takes the whole GUI down again.
        view->RemoveGUI();
        CHECK(View3DWinTestAccess::PanelWindow(*view) == nullptr);
        CHECK(View3DWinTestAccess::PanelInterface(*view) == nullptr);
        CHECK(View3DWinTestAccess::GuiWindows(*view).empty());
        CHECK(View3DWinTestAccess::CursorTextBox(*view) == nullptr);
        CHECK(View3DWinTestAccess::CostIndicatorText(*view) == nullptr);
        CHECK(View3DWinTestAccess::AdvisorPopups(*view) == nullptr);
        // With no windows left, panel 0 only records the pending sub-panel state (0x10078add7).
        view->ShowMayorSubPanel(0);
        CHECK(View3DWinTestAccess::MayorSubPanelState(*view) == 1);
        CHECK(host.advisorQueue->shutdowns == 1);
        CHECK(host.newsShutdowns >= 1 && host.mysimShutdowns >= 1 && host.budgetShutdowns >= 1);
        CHECK(host.graphsShutdowns >= 1 && host.buildingShutdowns >= 1);
        View3DWinTestAccess::Simulator(*view) = nullptr;
        View3DWinTestAccess::CursorTextIcon(*view) = nullptr;
        host.images.clear();
        background.Release();
        icon.Release();
    }

    // ------------------------------------------------------------------ Shutdown
    {
        const int removeAllBefore = host.notifications.removeAll;
        const int unregisterBefore = host.unregisterTables;
        const int onShutdownBefore = host.onView3DShutdown;
        host.unregisteredCheats.clear();
        host.removeTargets = 0;
        host.progress.refs = 2;                                    // the host's own reference + the view's
        renderer.shutdowns = 0;
        const int coverageShutdowns = host.coverage.shutdowns;
        const int lotConfigBefore = host.lotConfigShutdowns;
        View3DWinTestAccess::PanelWindow(*view) = new cGZWin(&mgr);
        const int progressShutdownsBefore = host.progressShutdowns;
        CHECK(view->Shutdown());
        CHECK(host.notifications.removeAll == removeAllBefore + 1);
        CHECK(host.unregisterTables == unregisterBefore + 1);
        CHECK(host.removeTargets == 1);
        CHECK(host.unregisteredCheats.size() == 13 && host.unregisteredCheats[0] == kCheatGameOfLife);
        CHECK(host.lotConfigShutdowns == lotConfigBefore + 1);
        CHECK(View3DWinTestAccess::PanelWindow(*view) == nullptr);   // Shutdown ran RemoveGUI
        CHECK(host.progressShutdowns == progressShutdownsBefore + 1);
        CHECK(host.onView3DShutdown == onShutdownBefore + 1);
        CHECK(renderer.shutdowns == 1);
        CHECK(host.coverage.shutdowns == coverageShutdowns + 1);
        CHECK(view->GetKeyAccelerator() == nullptr);
        CHECK(view->GetCoverageHelper() == &host.coverage);        // the helper object lives in the window
        CHECK(View3DWinTestAccess::Tools(*view).empty() && View3DWinTestAccess::Persistent(*view).empty());
        CHECK(View3DWinTestAccess::Camera(*view) == nullptr);       // released; the camera deleted itself
        CHECK(View3DWinTestAccess::Renderer(*view) == nullptr);
        // A second Shutdown is a no-op.
        CHECK(view->Shutdown() && host.notifications.removeAll == removeAllBefore + 1);
    }
    delete view;

    // ------------------------------------------------------------------ without a host
    SetView3DWinHost(nullptr);
    auto* orphan = new cSC4View3DWin(&mgr);
    CHECK(orphan->Init());                                         // cGZWin::Init runs; no service is touched
    CHECK(!orphan->SetZoom(1));
    CHECK(!orphan->RotateLeft() && !orphan->RotateRight() && !orphan->SetRotation(1));
    CHECK(!orphan->ZoomIn() && !orphan->ZoomOut() && !orphan->CanZoomIn() && !orphan->CanZoomOut());
    cISC4Occupant* orphanOccupant = nullptr;
    CHECK(!orphan->PickOccupant(0, 0, orphanOccupant) && orphanOccupant == nullptr);
    orphan->RecalcScrollVelocity();                                 // no inputs, no camera: no crash
    orphan->DoScrollBegin();
    orphan->DoScrollEnd();
    orphan->SetScrolling(true, 0.0f, 0.0f);
    orphan->SetScrolling(false, 0.0f, 0.0f);
    orphan->KillKeyboardScrolling();
    orphan->DoTemporarySimHiddenPause();
    orphan->DoTemporarySimHiddenResume();
    CHECK(orphan->ScrollStop());
    cSC4ViewLevelInformation level;
    level.level = 3;
    orphan->SetCurrentViewLevelInfo(level);
    CHECK(orphan->GetCurrentViewLevel() == 3);
    cSC4ViewFilterInformation info;
    orphan->GetCurrentViewFilterInfo(info);
    CHECK(info.filter == nullptr);
    orphan->AssertGlobalEffectTrigger(7);
    CHECK(orphan->GetGlobalEffectTriggerState(7));
    orphan->RetractGlobalEffectTrigger(7);
    CHECK(!orphan->GetGlobalEffectTriggerState(7));
    cGZMessage message;
    message.type = 0x7777;
    CHECK(!orphan->DoMessage(message));
    CHECK(orphan->Shutdown());
    delete orphan;

    host.cursorWindow.window_->AddRef();      // the host outlives its manager at exit: keep the window alive
    std::printf(failures ? "FAILED\n" : "sc4_view3dwin ok\n");
    return failures ? 1 : 0;
}
