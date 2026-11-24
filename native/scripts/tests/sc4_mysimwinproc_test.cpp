// Test for cSC4MySimWinProc (src/sc4/MySimWinProc.cpp).
//
// The panel is driven over a real cGZWin / cGZWinGen window tree (the controls are ordinary windows carrying the original
// control ids) with fake interfaces behind the host seam: the agent simulator, the two model managers, the message
// server, the game-data registry (the real cSC4GameDataRegistry), the pick-sim dialog, the exemplar string loader and
// SC4PostMessage2. The five sims, their advisor and the advices are doubles of the recovered interfaces.
//
// Not covered here: the modal transport dialog itself (ShowChooseTransportDialog needs the UI script service and the
// window manager, neither of which can be built headless; the parts before/after the script window are exercised through
// the panel's command handler and SetCurrentSim) and the bitmap loads (cSLBuffer has no host, so every load answers null
// and the SetBMP calls are skipped - the lookups and flags around them are asserted).
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "fake_winmgr2.h"
#include "gz/WinBMP.h"
#include "gz/WinUtils.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/GameDataRegistry.h"
#include "sc4/MySimWinProc.h"

// The interface doubles below override whole recovered interfaces; the parameters they ignore are intentional.
#pragma clang diagnostic ignored "-Wunused-parameter"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { \
    std::printf("FAIL line %d: %s != %s\n", __LINE__, #a, #b); ++failures; } } while (0)

namespace {

using namespace sc4;

// ---- fake windows -----------------------------------------------------------------------------------------------

// A window that records the messages it receives.
struct FakeWindow : cGZWin {
    using cGZWin::cGZWin;
    cGZMessage last;
    int messages = 0;
    bool DoMessage(cGZMessage& message) override { last = message; ++messages; return true; }
};


// A window that also answers the cIGZWinGen view (the panel and the transport dialog are gen windows).
struct FakeGen final : gz::cIGZWinGen {
    explicit FakeGen(cIGZWinMgr* mgr) {
        window_ = new FakeWindow(mgr);
        window_->AddRef();
    }
    ~FakeGen() { if (window_) window_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<gz::cIGZWinGen*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs_); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs_ > 0 ? --refs_ : 0); }
    std::uint32_t refs_ = 0;
    cIGZWin* AsIGZWin() override { return window_; }
    bool SetWinProc(gz::cIGZWinProc* proc) override { winProc = proc; return true; }
    std::uint32_t GetCLSID() const override { return {}; }
    bool GetWinGenFlag(int flag) const override { return {}; }
    bool SetWinGenFlag(int flag, bool on) override { return {}; }
    gz::cIGZWinProc* GetWinProc() const override { return {}; }
    bool GetWinProcAs(std::uint32_t iid, void** out) const override { return {}; }
    bool GetUserArea(cRZRect& out) const override { return {}; }
    bool SetUserArea(const cRZRect& area) override { return {}; }
    bool SetBackgroundBitmap(gz::BGBltType type, cIGZBuffer* image) override { return {}; }
    bool GetBackgroundBitmap(gz::BGBltType& type, cIGZBuffer*& image) const override { return {}; }
    bool SetBackgroundBitmap(cIGZBuffer* image) override { return {}; }
    bool GetBackgroundBitmap(cIGZBuffer** out) const override { return {}; }
    bool SetBackgroundBlitType(gz::BGBltType type) override { return {}; }
    bool GetBackgroundBlitType(gz::BGBltType& type) const override { return {}; }
    bool SetUserData(std::uint32_t slot, std::uint32_t value) override { return {}; }
    bool GetUserData(std::uint32_t slot, std::uint32_t& value) const override { return {}; }
    bool SetUserData(std::uint32_t value) override { return {}; }
    std::uint32_t GetUserData() const override { return {}; }
    void SetUserData(std::uint32_t id, cIGZUnknown* object) override { }
    bool GetUserData(std::uint32_t id, std::uint32_t iid, void** out) override { return {}; }
    void RemoveUserData(std::uint32_t id) override { }
    bool SetGutters(std::uint8_t x, std::uint8_t y) override { return {}; }
    bool GetGutters(int& x, int& y) override { return {}; }
    void SetGutters(std::int8_t l, std::int8_t t, std::int8_t r, std::int8_t b) override { }
    bool GetGutters(int& l, int& t, int& r, int& b) const override { return {}; }
    bool AddColorBar(int id, int sizeMode, int outlineFlags, const cRZRect& area) override { return {}; }
    void AddDividerH(int id, int y) override { }
    bool RemoveColorBar(int id) override { return {}; }
    FakeWindow* window_ = nullptr;
    gz::cIGZWinProc* winProc = nullptr;
};

// A window that also answers the cIGZWinBMP view (a portrait / icon bitmap / transport image).
struct FakeBmpWin final : cGZWin, gz::cIGZWinBMP {
    explicit FakeBmpWin(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    cIGZBuffer* bmp = nullptr;
    int setBmp = 0;
    bool shown = false;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinBMP) {
            *out = static_cast<gz::cIGZWinBMP*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    void ShowWindow() override { shown = true; cGZWin::ShowWindow(); }
    cIGZWin* AsIGZWin() override { return this; }
    void SetBMP(cIGZBuffer* buffer) override { bmp = buffer; ++setBmp; }
    bool GetBMP(std::uint32_t, void** out) const override { if (!bmp) return false; *out = bmp; return true; }
    void SetAlphaBlending(bool, float) override {}
    void GetAlphaBlending(bool& enable, float& alpha) const override { enable = false; alpha = 0.5f; }
    bool GetWinBMPFlag(int flag) const override { return (flags & static_cast<std::uint32_t>(flag)) != 0; }
    void SetWinBMPFlag(int flag, bool on) override {
        if (on) flags |= static_cast<std::uint32_t>(flag); else flags &= ~static_cast<std::uint32_t>(flag);
    }
    std::uint32_t flags = 0;
    void SetImageArea(const cRZRect& area) override { }
    void GetImageArea(cRZRect& area) const override { }
};

// A window that also answers the cIGZWinBtn view (an icon / page / wealth / boot button).
struct FakeBtnWin final : cGZWin, gz::cIGZWinBtn {
    explicit FakeBtnWin(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    std::string tip;
    gz::IWinButtonFont* font = nullptr;
    std::uint32_t fontColor = 0;
    int fontColors = 0;
    int tips = 0;
    int styles = 0;
    bool on = false;
    std::uint32_t btnFlags = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == gz::kGZIID_cIGZWinBtn) {
            *out = static_cast<gz::cIGZWinBtn*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    cIGZWin* AsIGZWin() override { return this; }
    bool GetBtnFlag(int flag) const override { return (btnFlags & static_cast<std::uint32_t>(flag)) != 0; }
    bool SetBtnFlag(int flag, bool value) override {
        if (value) btnFlags |= static_cast<std::uint32_t>(flag);
        else btnFlags &= ~static_cast<std::uint32_t>(flag);
        return true;
    }
    bool IsOn() const override { return on; }
    int GetStyle() override { return 1; }
    bool SetStyle(int style, cIGZBuffer*, const cRZRect*) override { styles = style; return true; }
    bool SetButtonStyle(int style) override { styles = style; return true; }
    bool GetFontColor(int, std::uint32_t& native) const override { native = fontColor; return true; }
    bool SetFontColor(int, std::uint32_t native) override { fontColor = native; ++fontColors; return true; }
    bool GetFontColor(int, gz::cRZColor& color) const override { color = gz::cRZColor{fontColor}; return true; }
    bool SetFontColor(int, gz::cRZColor color) override { fontColor = color.rgb; ++fontColors; return true; }
    bool SetTipText(const cIGZString& text) override { tip = std::string(text.Data(), text.Strlen()); ++tips; return true; }
    bool GetTipText(cIGZString& out) const override { out.AssignFromCString(tip.c_str()); return true; }
    bool ToggleOn() override { return {}; }
    bool ToggleOff() override { return {}; }
    bool SetChecked(bool checked) override { return {}; }
    bool IsChecked() const override { return {}; }
    int GetImageW() const override { return {}; }
    int GetImageH() const override { return {}; }
    bool GetImageArea(cRZRect& out) const override { return {}; }
    bool SetBuffer(cIGZBuffer* image) override { return {}; }
    bool GetBuffer(cIGZBuffer*& out) const override { return {}; }
    bool SetFont(gz::IWinButtonFont* font) override { return {}; }
    bool SetFontStyle(std::uint32_t styleId) override { return {}; }
    bool GetFontStyle(std::uint32_t& styleId) override { return {}; }
    bool ClearFontColor(int index) override { return {}; }
    bool SetCaptionStyle(int style) override { return {}; }
    bool GetCaptionStyle(int& style) const override { return {}; }
    bool SetGutters(std::uint8_t x, std::uint8_t y) override { return {}; }
    bool GetGutters(int& x, int& y) const override { return {}; }
    bool SetGutters(std::uint8_t l, std::uint8_t t, std::uint8_t r, std::uint8_t b) override { return {}; }
    bool GetGutters(int& l, int& t, int& r, int& b) const override { return {}; }
    bool FitSizeToContent() override { return {}; }
    bool GetMinMaxInfo(cRZRect& out) override { return {}; }
    bool ClearTipText() override { return {}; }
    bool SetTipPlacementType(int type) override { return {}; }
    bool GetTipPlacementType(int& type) const override { return {}; }
    bool SetTipPlacementOffsets(int x, int y) override { return {}; }
    bool GetTipPlacementOffsets(int& x, int& y) const override { return {}; }
    bool SetTipColor(int index, std::uint8_t r, std::uint8_t g, std::uint8_t b) override { return {}; }
    bool GetTipColor(int index, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) const override { return {}; }
    void Flash(std::uint32_t count, std::uint32_t periodMs) override { }
    bool SetSoundID(int which, std::uint32_t id) override { return {}; }
    std::uint32_t GetSoundID(int which) const override { return {}; }
};

// A window that also answers the cIGZWinAdviceList view (the body list of the small / large page).
struct FakeAdviceWin final : cGZWin, IWinAdviceList {
    explicit FakeAdviceWin(cIGZWinMgr* mgr) : cGZWin(mgr) {}
    struct Entry {
        std::string a, b, c, d, e;
        bool enabled = false;
        cISC4Advice* advice = nullptr;
        bool f = false;
        std::uint32_t image = 0;
    };
    std::vector<Entry> entries;
    int clears = 0, rebuilds = 0, background = 0;
    int messages = 0;
    cGZMessage lastMessage;
    std::uint32_t bgImage = 0;
    bool lastRebuild = false;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZWinAdviceList) {
            *out = static_cast<IWinAdviceList*>(this);
            AddRef();
            return true;
        }
        return cGZWin::QueryInterface(iid, out);
    }
    std::uint32_t AddRef() override { return cGZWin::AddRef(); }
    std::uint32_t Release() override { return cGZWin::Release(); }
    void AddEntry(const cIGZString& a, const cIGZString& b, const cIGZString& c, const cIGZString& d, bool enabled,
                  cISC4Advice* advice, const cIGZString& e, bool f, std::uint32_t image) override {
        entries.push_back({std::string(a.Data(), a.Strlen()), std::string(b.Data(), b.Strlen()),
                           std::string(c.Data(), c.Strlen()), std::string(d.Data(), d.Strlen()),
                           std::string(e.Data(), e.Strlen()), enabled, advice, f, image});
    }
    void RebuildHTML(bool force) override { ++rebuilds; lastRebuild = force; }
    void SetbgImage(std::uint32_t image) override { ++background; bgImage = image; }
    void ClearList() override { ++clears; entries.clear(); }
    bool DoMessage(cGZMessage& message) override { ++messages; lastMessage = message; return true; }
    void ScrollToAnchor(const cIGZString&) override {}
    void DisableVerticalScrollbar() override {}
    cIGZWin* AsIGZWin() override { return this; }
};

// ---- fake game objects -----------------------------------------------------------------------------------------

struct FakeAdvisor final : cISC4Advisor {
    int refs = 0;
    int id = 3;
    cRZString personalName{"Mayor Bob"};
    cRZString dummy_;
    AdviceMap advices;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<cISC4Advisor*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    int GetID() const override { return id; }
    cIGZString& GetPersonalName() override { return personalName; }
    const AdviceMap& GetCurrentAdvices() const override { return advices; }
    AdviceMap& GetCurrentAdvices() override { return advices; }
    bool Init(int advisorType) override { return {}; }
    bool Shutdown() override { return {}; }
    const cIGZString& GetName() const override { return dummy_; }
    void SetName(const cIGZString& name) override { }
    void SetPersonalName(const cIGZString& name) override { }
    cISC4Advice* GetAdviceByGUID(int guid) const override { return {}; }
    cISC4Advice* GetAdviceBySessionID(int sessionId) const override { return {}; }
    bool Simulate(int period) override { return {}; }
    int GetHeadAnimCount(bool gender1) override { return {}; }
    bool SetHeadAnim(bool gender1, int index) override { return {}; }
    int GetHeadAnimID(int animIndex) override { return {}; }
    bool PushEvent(int eventId, cISC4AdviceSubject* subject) override { return {}; }
    bool CommandWouldTrigger(std::uint32_t commandId) override { return {}; }
    bool RetireAdvice(cISC4Advice* advice) override { return {}; }
};

struct FakeAdvice final : cISC4Advice {
    int refs = 0;
    int priority = 2;
    int advisorId = 3;
    bool current = true;
    bool shown = false;
    cRZString title{"Title"};
    cRZString message{"Message"};
    cRZString dummy_;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kMySimAdviceIid) return false;
        *out = static_cast<cISC4Advice*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    int GetPriority() const override { return priority; }
    int GetAdvisorID() const override { return advisorId; }
    bool CheckFlags(int flags) const override { return (flags == 2) ? current : shown; }
    bool SetFlags(int flags, bool on) override { if (flags == 0x20) shown = on; return true; }
    const cIGZString& GetTextTitle() const override { return title; }
    const cIGZString& GetTextMessage() const override { return message; }
    bool Init(int adviceId) override { return {}; }
    bool Shutdown() override { return {}; }
    bool Clone(cISC4Advice** out) override { return {}; }
    int GetMood() const override { return {}; }
    bool GetTrigger() const override { return {}; }
    cIGZString& GetTriggerString() override { return dummy_; }
    int GetGUID() const override { return {}; }
    void SetAdvisorID(int id) override { }
    int GetSessionID() const override { return {}; }
    int GetFrequencyInDays() const override { return {}; }
    int GetTimeOutInDays() const override { return {}; }
    std::uint32_t GetTextTitleGUID() const override { return {}; }
    std::uint32_t GetTextMessageGUID() const override { return {}; }
    bool DoLink(const cIGZString& link) override { return {}; }
    void NotifyGlobalEffectTriggerChanged() override { }
    bool SetEventSubject(cISC4AdviceSubject* subject) override { return {}; }
    cISC4AdviceSubject* GetEventSubject() override { return {}; }
    int GetEventID() override { return {}; }
    int GetCommandID() override { return {}; }
    bool UpdateText() override { return {}; }
    bool IsCurrent() const override { return {}; }
};

struct FakeMySim final : cISC4MySim {
    int refs = 0;
    std::uint8_t slot = 0;
    bool active = true;
    int wealth = 2;
    std::uint8_t sign = 0;
    std::uint8_t age = 42;
    bool male = true;
    std::uint32_t bitmapId = 0;
    cRZString bitmapFile{"portrait.bmp"};
    cRZString home{"Home"}, job{"Job"}, jobTitle{"Job Title"}, bio{"Bio"}, eq{"EQ"}, hq{"HQ"};
    std::uint32_t pedestrianBitmap = 0x1111, carBitmap = 0x2222;
    int deleted = 0, dispatchTools = 0, found = 0, houseTools = 0, jobTools = 0, cars = 0, peds = 0;
    FakeAdvisor advisor;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cISC4MySim) { *out = static_cast<cIGZUnknown*>(this); AddRef(); return true; }
        if (iid == kGZIID_cISC4MySimLink) { *out = static_cast<cISC4MySim*>(this); AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    cISC4Advisor* AsAdvisor() override { return &advisor; }
    std::uint8_t GetSlot() const override { return slot; }
    bool IsActive() const override { return active; }
    int GetWealth() const override { return wealth; }
    std::uint8_t GetSign() const override { return sign; }
    std::uint8_t GetAge() const override { return age; }
    bool IsMale() const override { return male; }
    std::uint32_t GetBitmapID() const override { return bitmapId; }
    const cRZString& GetBitmapFile() const override { return bitmapFile; }
    const cRZString& GetHomeName() const override { return home; }
    const cRZString& GetJobName() const override { return job; }
    const cRZString& GetJobTitle() const override { return jobTitle; }
    void GetJobTitle(cISC4BuildingOccupant*, cIGZString&) override {}
    const cRZString& GetBio() const override { return bio; }
    bool GetEQAsText(cRZString& out) override { out = eq; return true; }
    bool GetHQAsText(cRZString& out) override { out = hq; return true; }
    std::uint32_t GetModelButtonBitmapID(bool pedestrian) const override {
        return pedestrian ? pedestrianBitmap : carBitmap;
    }
    bool DeleteFromGame() override { ++deleted; return true; }
    bool EnterUDriveIt() override { ++dispatchTools; return true; }
    bool CreateDispatchTool() override { ++dispatchTools; return true; }
    void Find() override { ++found; }
    bool SetCar(cISCResExemplar*) override { ++cars; return true; }
    bool SetPedestrian(cISCResExemplar*) override { ++peds; return true; }
    void MoveIn(std::uint8_t slot, cRZString& name, bool male, std::uint8_t zodiac, cRZString& bio, std::uint32_t bitmapId, cRZString& bitmapFile) override { }
    bool FindNewResidence() override { return {}; }
    bool ChangeResidence(cISC4BuildingOccupant* building) override { return {}; }
    bool TestForResidence(cISC4BuildingOccupant* building, bool checkFilter, bool checkWealth) override { return {}; }
    bool ChangeJob(cISC4BuildingOccupant* building) override { return {}; }
    cISC4BuildingOccupant* GetResidence() const override { return {}; }
    cISC4BuildingOccupant* GetJob() const override { return {}; }
    void SetResidence(cISC4BuildingOccupant* building) override { }
    bool GetLocation(float* xyz) const override { return {}; }
    cGZPersistResourceKey GetVehicleKey() const override { return {}; }
    bool GetBitmapBuffer(cIGZBuffer** out) const override { return {}; }
    void ShowResidence() override { }
    void HideResidence() override { }
    void ShowLocation() override { }
    void HideLocation() override { }
    void SetPylonVisibility(bool visible) override { }
    void UpdateResidence() override { }
    void LeaveForWork() override { }
    void ReturnHome() override { }
    void AnnounceMoveIn() override { }
    void DoScriptCommand(int command) override { }
    void DoBuildingAdded(cISC4BuildingOccupant* building) override { }
    void DoBuildingChanged(cISC4BuildingOccupant* building) override { }
    void DoBuildingRemoved(cISC4BuildingOccupant* building, std::uint32_t data) override { }
    void DoFireAdded(float x, float z) override { }
    void DoPipeBurst(float x, float z) override { }
    void DoTornado(float x, float z) override { }
    void DoDisaster(std::uint32_t disaster, int x, int z) override { }
    void DoNeighborConnectionRemoved(int cellX, int cellZ) override { }
    int DistanceToClosest(std::uint32_t occupantGroup) override { return {}; }
    std::uint32_t DistanceToClosestSpecial(std::uint32_t buildingType) override { return {}; }
    float LocalSchoolGrade(std::uint32_t) override { return {}; }
    float LocalHospitalGrade(std::uint32_t) override { return {}; }
    void PlayNightSound() override { }
    bool GetCurrentScreenLocationXY(int* x, int* y) override { return {}; }
    bool RequestToPlaySound() override { return {}; }
    cISC4MySimBalloonAdvice* GetNextBalloonAdvice() override { return {}; }
    cISC4MySimBalloonAdvice* GetNextBalloonEventAdvice() override { return {}; }
    cISC4MySimBalloonAdvice* GetCurrentBalloonAdvice() const override { return {}; }
    cISC4MySimBalloonAdvice* GetCurrentBalloonEventAdvice() const override { return {}; }
    bool CanUDriveIt() override { return {}; }
    bool ExitUDriveIt() override { return {}; }
    bool ActivateLuaContext() override { return {}; }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    int refs = 0;
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    std::vector<cIGZMessage2*> posted;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<cIGZMessageServer2*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override { posted.push_back(message); return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        added.push_back({target, type});
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override {
        removed.push_back({target, type});
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeAgentSimulator final : IMySimAgentSimulator {
    int refs = 0;
    std::vector<cISC4MySim*> sims;
    std::vector<int> activePanel;
    cISC4MySim* tracked = nullptr;
    int tracks = 0, hides = 0, cancels = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<IMySimAgentSimulator*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    void GetActiveMySims(std::vector<cRZAutoRefCount<cISC4MySim>>& out) override {
        for (cISC4MySim* sim : sims) out.emplace_back(sim);
    }
    void TrackLocation(cISC4MySim* sim) override { tracked = sim; ++tracks; }
    void HideLocation() override { ++hides; }
    bool MySimLivesHere(cISC4BuildingOccupant*) override { return false; }
    void CancelMoveIn() override { ++cancels; }
    void SetActiveUIPanelSim(int index) override { activePanel.push_back(index); }
    void GetActiveUIPanelSim(int& index) override { index = activePanel.empty() ? -1 : activePanel.back(); }
};

struct FakeModelManager final : IModelManager {
    int refs = 0;
    std::vector<cISCResExemplar*> models;
    int calls = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<IModelManager*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    void ModelExemplars(std::vector<cISCResExemplar*>& out) override { ++calls; out = models; }
};

struct FakePickSim final : IPickSimWinProc {
    int refs = 0, inits = 0, shutdowns = 0;
    std::uint32_t index = 0;
    explicit FakePickSim(std::uint32_t i) : index(i) {}
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = static_cast<IPickSimWinProc*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    bool Init() override { ++inits; return true; }
    bool Shutdown() override { ++shutdowns; return true; }
};

// A model exemplar: a real cSCPropertyHolder holds the properties the panel reads; the interface itself is a double.
struct FakeExemplar final : cISCResExemplar {
    int refs = 0;
    cSCPropertyHolder holder;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1 && iid != kMySimTransportExemplarIid) return false;
        *out = static_cast<cISCResExemplar*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
    cISCPropertyHolder* AsISCPropertyHolder() override { return &holder; }
    void Set(std::uint32_t id, std::uint32_t value) { holder.AddProperty(id, value, true); }
    void Set(std::uint32_t id, const cIGZString& value) { holder.AddProperty(id, value, true); }
    bool IsPropertyLocal(std::uint32_t) override { return true; }
    cISCResExemplarCohort* GetCohort() override { return nullptr; }
    void SetCohort(cISCResExemplarCohort*, bool) override {}
    bool GetKey(cGZPersistResourceKey&) override { return false; }
    bool SetKey(const cGZPersistResourceKey&) override { return false; }
    void CompactProperties() override {}
    void GenerateCachedMap() override {}
    bool Write(cIGZOStream&) const override { return false; }
    bool Read(cIGZIStream&) override { return false; }
    std::uint32_t GetGZCLSID() const override { return 0; }
};

struct HostImpl final : IMySimWinProcHost {
    FakeMgr2* mgr = nullptr;
    FakeMessageServer server;
    sc4::cSC4GameDataRegistry* registry = nullptr;
    FakeAgentSimulator agent;
    FakeModelManager vehicles, pedestrians;
    std::string cityName = "Testville";
    std::vector<std::uint32_t> posted;
    std::vector<FakePickSim*> picks;
    std::vector<std::pair<cRZString, std::uint32_t>> exemplarStrings;

    cIGZMessageServer2* MessageServer() override { return &server; }
    sc4::cISC4GameDataRegistry* GameDataRegistry() override { return registry; }
    IMySimAgentSimulator* MySimAgentSimulator() override { return &agent; }
    IModelManager* VehicleManager() override { return &vehicles; }
    IModelManager* PedestrianManager() override { return &pedestrians; }
    int restores = 0, saves = 0;
    void RestoreWindowLocation(cIGZWin*) override { ++restores; }
    void SaveWindowLocation(cIGZWin*) override { ++saves; }
    FakeGen* dialogGen = nullptr;
    bool dialogCreated = false;
    cGZPersistResourceKey dialogKey{};
    std::uint32_t dialogId = 0, dialogIid = 0;
    bool CreateTransportDialog(const cGZPersistResourceKey& key, cIGZWin* parent, std::uint32_t id, std::uint32_t iid,
                               void** out) override {
        if (!dialogGen) return false;
        dialogCreated = true;
        dialogKey = key;
        dialogId = id;
        dialogIid = iid;
        dialogParent = parent;
        *out = static_cast<gz::cIGZWinGen*>(dialogGen);
        dialogGen->AddRef();
        return true;
    }
    cIGZWin* dialogParent = nullptr;
    int houseTools = 0, jobTools = 0;
    bool CreateChangeHouseTool(cISC4MySim*) override { ++houseTools; return true; }
    bool CreateChangeJobTool(cISC4MySim*) override { ++jobTools; return true; }
    bool MySimDirectory(cIGZString& out) override { out.AssignFromCString(cityName.c_str()); return true; }
    IPickSimWinProc* CreatePickSimWinProc(std::uint32_t index) override {
        picks.push_back(new FakePickSim(index));   // no reference yet; the panel takes one (and the double is leaked)
        return picks.back();
    }
    cIGZWinMgr* WindowManager() override { return mgr; }
    void PostMessage2(std::uint32_t type, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                      cIGZUnknown*) override {
        posted.push_back(type);
    }
    bool ExemplarString(cRZString& out, cISCPropertyHolder*, std::uint32_t property) override {
        for (const auto& entry : exemplarStrings) {
            if (entry.second == property) { out = entry.first; return true; }
        }
        return false;
    }
    FakePickSim* LastPick() const { return picks.empty() ? nullptr : picks.back(); }
};

// The window tree the panel expects: a control container (the panel's parent), the panel itself and one window per
// control id the panel looks up. Returns the container.
FakeWindow* BuildTree(FakeMgr2& mgr, FakeGen*& panel) {
    auto* controls = new FakeWindow(&mgr);
    controls->AddRef();
    controls->SetID(0x100);
    panel = new FakeGen(&mgr);
    panel->AsIGZWin()->SetID(0x200);
    controls->ChildAdd(panel->AsIGZWin());
    auto addWindow = [&](std::uint32_t id) {
        auto* window = new FakeWindow(&mgr);
        window->AddRef();
        window->SetID(id);
        controls->ChildAdd(window);
        window->Release();   // the parent's list now owns it
        return window;
    };
    // page frames
    addWindow(kMySimPanelWindow);
    addWindow(kMySimMainWindow);
    addWindow(kMySimDialogMain);
    addWindow(kMySimDialogStat);
    addWindow(kMySimDialogStatAlt);
    addWindow(kMySimDialogTransport);
    // the small / large page frames and their body lists
    auto* small = addWindow(kMySimDialogSmall);
    auto* large = addWindow(kMySimDialogLarge);
    auto addPageChild = [&](cIGZWin* page, std::uint32_t id) {
        auto* child = new FakeWindow(&mgr);
        child->AddRef();
        child->SetID(id);
        page->ChildAdd(child);
        child->Release();   // the parent's list now owns it
    };
    addPageChild(controls->GetChildWindowFromIDRecursive(kMySimDialogMain), 0x500);
    addPageChild(controls->GetChildWindowFromIDRecursive(kMySimDialogStat), 0x501);
    addPageChild(controls->GetChildWindowFromIDRecursive(kMySimDialogStatAlt), 0x502);
    addPageChild(controls->GetChildWindowFromIDRecursive(kMySimDialogTransport), 0x503);
    addPageChild(small, 0x504);
    addPageChild(large, 0x505);
    auto* smallBody = new FakeAdviceWin(&mgr);
    smallBody->AddRef();
    smallBody->SetID(kMySimBodySmall);
    small->ChildAdd(smallBody);
    smallBody->Release();   // the parent's list now owns it
    auto* largeBody = new FakeAdviceWin(&mgr);
    largeBody->AddRef();
    largeBody->SetID(kMySimBodyLarge);
    large->ChildAdd(largeBody);
    largeBody->Release();   // the parent's list now owns it
    // the five slot portraits / name texts / buttons and their three variants
    for (int slot = 0; slot < 5; ++slot) {
        auto* bmp = new FakeBmpWin(&mgr);
        bmp->AddRef();
        bmp->SetID(kMySimNameBitmapBase + static_cast<std::uint32_t>(slot));
        controls->ChildAdd(bmp);
        bmp->Release();   // the parent's list now owns it
        addWindow(kMySimNameTexts[slot]);
        for (std::uint32_t id : {kMySimNameButtons[slot], kMySimNameButtonsNew[slot], kMySimNameButtonsEmpty[slot]}) {
            auto* button = new FakeBtnWin(&mgr);
            button->AddRef();
            button->SetID(id);
            controls->ChildAdd(button);
            button->Release();   // the parent's list now owns it
        }
    }
    // the stat page captions, the wealth pips and the bio
    for (std::uint32_t id : {kMySimStatWealth, kMySimStatSign, kMySimStatAge, kMySimStatHome, kMySimStatJobTitle,
                             kMySimStatJob, kMySimStatEQ, kMySimStatHQ, kMySimStatBio, kMySimNameCaption,
                             kMySimNameCaption2, kMySimNameCaption3, kMySimNameCaption4, kMySimTransportCaption,
                             kMySimTransportCaption2, 0xea1f1dba}) {
        addWindow(id);
    }
    auto* portrait = new FakeBmpWin(&mgr);
    portrait->AddRef();
    portrait->SetID(kMySimPortrait);
    controls->ChildAdd(portrait);
    portrait->Release();   // the parent's list now owns it
    // the two transport button images
    for (std::uint32_t id : {kMySimTransportCarBmp, kMySimTransportCarBmp + 8}) {
        auto* bmp = new FakeBmpWin(&mgr);
        bmp->AddRef();
        bmp->SetID(id);
        controls->ChildAdd(bmp);
        bmp->Release();   // the parent's list now owns it
    }
    return controls;
}

// One fake transport dialog: the icon buttons and bitmaps of a page, the six page buttons and the wealth buttons.
FakeWindow* BuildDialog(FakeMgr2& mgr, FakeGen& gen) {
    FakeWindow* dialog = gen.window_;
    dialog->SetID(0x300);
    auto addButton = [&](std::uint32_t id) {
        auto* button = new FakeBtnWin(&mgr);
        button->AddRef();
        button->SetID(id);
        dialog->ChildAdd(button);
        button->Release();   // the parent's list now owns it
    };
    auto addBitmap = [&](std::uint32_t id) {
        auto* bmp = new FakeBmpWin(&mgr);
        bmp->AddRef();
        bmp->SetID(id);
        dialog->ChildAdd(bmp);
        bmp->Release();   // the parent's list now owns it
    };
    for (std::uint32_t i = 0; i < 28; ++i) {
        addButton(kMySimIconButtonBase + i);
        addBitmap(kMySimIconBitmapBase + i);
    }
    for (std::uint32_t i = 1; i <= 6; ++i) addButton(kMySimPageButtonBase + i);
    for (std::uint32_t i = 0; i < 3; ++i) addButton(kMySimWealthButtonBase + i);
    return dialog;
}

FakeBtnWin* ButtonOf(cIGZWin* window, std::uint32_t id) {
    cIGZWin* child = window->GetChildWindowFromIDRecursive(id);
    return static_cast<FakeBtnWin*>(child);
}

// A bare counted object used as the registry node of the callback test.
struct DummyUnknown final : cIGZUnknown {
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != 1) return false;
        *out = this;
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { return static_cast<std::uint32_t>(refs > 0 ? --refs : 0); }
};

}  // namespace

int main() {
    FakeMgr2 mgr;
    FakeGen* panel = nullptr;
    FakeWindow* controls = BuildTree(mgr, panel);
    HostImpl host;
    host.mgr = &mgr;
    cSC4GameDataRegistry* registry = new cSC4GameDataRegistry();
    registry->AddRef();
    host.registry = registry;
    SetMySimWinProcHost(&host);

    auto* proc = new cSC4MySimWinProc(panel);
    proc->AddRef();
    CHECK(proc->Panel() == panel);
    CHECK(panel->AsIGZWin()->GetParentWin() == controls);

    // ---- QueryInterface / reference counting ----
    void* out = nullptr;
    CHECK(proc->QueryInterface(gz::kGZIID_cIGZWinProc, &out));
    CHECK(out == static_cast<gz::cIGZWinProc*>(proc));
    CHECK(proc->QueryInterface(0x452294aa, &out));
    CHECK(out == static_cast<cIGZMessageTarget2*>(proc));
    CHECK(proc->QueryInterface(1, &out));
    CHECK(out != nullptr);
    CHECK(!proc->QueryInterface(0x12345678, &out));
    for (int i = 0; i < 3; ++i) proc->Release();   // the references the three successful queries took

    // ---- Init: six notifications, the seven page targets and the game-data entries ----
    proc->Init();
    CHECK_EQ(host.server.added.size(), std::size_t(6));
    CHECK(host.server.added[0].second == kMessageMySimsChanged);
    CHECK(host.server.added[1].second == kMySimMsgAdviceChanged);
    CHECK(host.server.added[2].second == kMySimMsgAdviceAdded);
    CHECK(host.server.added[3].second == kMySimMsgAdviceRemoved);
    CHECK(host.server.added[4].second == kMySimMsgCarChosen);
    CHECK(host.server.added[5].second == kMySimMsgPedestrianChosen);
    CHECK_EQ(registry->VisitImmediateChildren(registry->GetRootNodeEntry(),
                                              [](EntryId entry, void* context) {
                                                  auto* name = static_cast<cSC4GameDataRegistry*>(context)
                                                                   ->GetEntryName(entry);
                                                  return name && name->IsEqualToChars("mysims_ui", 9, true);
                                              },
                                              registry),
             true);
    // every page notification target points at the panel
    CHECK(controls->GetChildWindowFromIDRecursive(0x500)->GetNotificationTarget() == panel->AsIGZWin());
    CHECK(controls->GetChildWindowFromIDRecursive(0x504)->GetNotificationTarget() == panel->AsIGZWin());
    CHECK(controls->GetChildWindowFromIDRecursive(0x503)->GetNotificationTarget() == panel->AsIGZWin());

    // ---- SetDialogState shows exactly one page ----
    proc->SetDialogState(kMySimDialogLarge);
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimDialogLarge)->GetFlag(kWinVisible));
    CHECK(!controls->GetChildWindowFromIDRecursive(kMySimDialogMain)->GetFlag(kWinVisible));
    CHECK(!controls->GetChildWindowFromIDRecursive(kMySimDialogStat)->GetFlag(kWinVisible));
    proc->SetDialogState(kMySimDialogStatAlt);
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimDialogStatAlt)->GetFlag(kWinVisible));
    CHECK(!controls->GetChildWindowFromIDRecursive(kMySimDialogLarge)->GetFlag(kWinVisible));

    // ---- GetCurrentSims: two sims in slots 0 and 2 ----
    FakeMySim sim0, sim2;
    sim0.slot = 0;
    sim0.advisor.personalName = cRZString("Alice");
    sim2.slot = 2;
    sim2.advisor.personalName = cRZString("Carol");
    host.agent.sims = {&sim0, &sim2};
    proc->GetCurrentSims();
    CHECK_EQ(proc->SimCount(), 2u);
    CHECK(proc->Sim(0) == &sim0);
    CHECK(proc->Sim(2) == &sim2);
    CHECK(proc->Sim(1) == nullptr);
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimNameTexts[0]))
                 ->GetCaption().ToChar(),
             std::string("Alice"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimNameTexts[2]))
                 ->GetCaption().ToChar(),
             std::string("Carol"));
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimNameBitmapBase)->GetFlag(kWinVisible));
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimNameButtons[0])->GetFlag(kWinVisible));
    CHECK(!controls->GetChildWindowFromIDRecursive(kMySimNameButtonsEmpty[0])->GetFlag(kWinVisible));
    CHECK(ButtonOf(controls, 0xea1f1dba)->GetFlag(kWinEnabled));
    CHECK_EQ(host.agent.tracks, 1);
    CHECK(host.agent.tracked == nullptr);

    // ---- SetCurrentSim: the agent simulator follows the slot and the two transport images are looked up ----
    proc->SetCurrentSim(2, false, false);
    CHECK_EQ(proc->CurrentSim(), 2u);
    CHECK(!host.agent.activePanel.empty() && host.agent.activePanel.back() == 2);

    // ---- the main page refreshes its four captions ----
    proc->SetMySimMainWindow();
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimDialogMain)->GetFlag(kWinVisible));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimNameCaption))
                 ->GetCaption().ToChar(),
             std::string("Carol"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimNameCaption4))
                 ->GetCaption().ToChar(),
             std::string("Carol"));

    // ---- the stat page: wealth pips, age, home, job, title, EQ/HQ and the bio ----
    sim2.wealth = 2;
    proc->SetMySimStatWindow();
    CHECK_EQ(proc->Mode(), 2u);
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimStatWealth))
                 ->GetCaption().ToChar(),
             std::string("\xc2\xa7\xc2\xa7"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimStatAge))
                 ->GetCaption().ToChar(),
             std::string("42"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimStatHome))
                 ->GetCaption().ToChar(),
             std::string("Home"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimStatJob))
                 ->GetCaption().ToChar(),
             std::string("Job"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimStatEQ))
                 ->GetCaption().ToChar(),
             std::string("EQ"));
    CHECK_EQ(static_cast<FakeWindow*>(controls->GetChildWindowFromIDRecursive(kMySimStatHQ))
                 ->GetCaption().ToChar(),
             std::string("HQ"));

    // ---- the large page: the advisor's advices become list entries ----
    auto* adviceHigh = new FakeAdvice();       // intentionally leaked: the panel and the fake maps hold references
    auto* adviceExpired = new FakeAdvice();
    auto* adviceSpecial = new FakeAdvice();
    adviceHigh->priority = 1;
    adviceHigh->title = cRZString("High");
    adviceHigh->message = cRZString("High body");
    adviceExpired->priority = 5;
    adviceExpired->current = false;
    adviceExpired->title = cRZString("Old");
    adviceExpired->message = cRZString("Old body");
    adviceSpecial->priority = 7;
    adviceSpecial->title = cRZString("Special");
    sim2.advisor.advices.clear();
    sim2.advisor.advices[cRZAutoRefCount<cISC4Advice>(adviceHigh)] = 0;
    sim2.advisor.advices[cRZAutoRefCount<cISC4Advice>(adviceExpired)] = 1;
    sim2.advisor.advices[cRZAutoRefCount<cISC4Advice>(adviceSpecial)] = 2;
    proc->SetMySimLargeWindow();
    CHECK_EQ(proc->Mode(), 1u);
    CHECK_EQ(proc->Large(), 1u);
    CHECK_EQ(proc->Messages().size(), std::size_t(3));
    auto* body = static_cast<FakeAdviceWin*>(
        controls->GetChildWindowFromIDRecursive(kMySimDialogLarge)->GetChildWindowFromIDRecursive(kMySimBodyLarge));
    CHECK_EQ(body->entries.size(), std::size_t(3));
    CHECK_EQ(body->clears, 1);
    CHECK_EQ(body->rebuilds, 1);
    CHECK(body->lastRebuild);
    CHECK_EQ(body->bgImage, kMySimAdviceImage);
    // map order is the advice key, i.e. the pointer value; find the entries by their title instead
    const FakeAdviceWin::Entry* high = nullptr;
    const FakeAdviceWin::Entry* expired = nullptr;
    const FakeAdviceWin::Entry* special = nullptr;
    for (const auto& entry : body->entries) {
        if (entry.d.find("High") != std::string::npos) high = &entry;
        if (entry.d.find("Old") != std::string::npos) expired = &entry;
        if (entry.d.find("Special") != std::string::npos) special = &entry;
    }
    CHECK(high != nullptr);
    CHECK(expired != nullptr);
    CHECK(special != nullptr);
    if (high) {
        CHECK(high->d.find("#b00000") != std::string::npos);       // high priority header
        CHECK(high->d.find("StocletITC TT") != std::string::npos);  // header font
        CHECK(high->d.find("</B></FONT>") != std::string::npos);    // general footer
        CHECK(high->c.find("<FONT COLOR=\"#3f4967\"") != std::string::npos);   // body font
        CHECK(high->c.find("High body") != std::string::npos);
        CHECK(high->c.find("</I></FONT>") != std::string::npos);
        CHECK(high->a.find("14416256") != std::string::npos);       // bad down arrow
        CHECK(high->b.find("14416255") != std::string::npos);       // bad right arrow
        CHECK(high->e.find("14416257") != std::string::npos);       // bad x
        CHECK(high->enabled);
        CHECK_EQ(high->image, kMySimAdviceImage);
    }
    if (expired) {
        CHECK(expired->d.find("#8e9499") != std::string::npos);     // expired header
        CHECK(expired->c.find("8e9949") != std::string::npos);      // deactive body font
        CHECK(expired->a.find("1441625A") != std::string::npos);    // neutral down arrow
        CHECK(expired->enabled);
    }
    if (special) {
        CHECK_EQ(special->a, special->b);                            // priority 7 duplicates the icons
        CHECK_EQ(special->c, special->d);                            // and puts the title in the body slot
        CHECK(special->c.find("Special") != std::string::npos);
    }

    // ---- commands from the panel select slots and answer false ----
    const std::size_t cancels = static_cast<std::size_t>(host.agent.cancels);
    cGZMessage command;
    command.type = kGZMsgCommand;
    command.param1 = kMySimCommandSender;
    command.param2 = 0x8a1f1dc3;   // slot 0
    CHECK(!proc->DoWinProcMessage(panel->AsIGZWin(), command));
    CHECK_EQ(proc->CurrentSim(), 0u);
    CHECK_EQ(host.agent.cancels, static_cast<int>(cancels) + 1);
    command.param1 = 0xdeadbeef;   // not a button command: ignored
    CHECK(!proc->DoWinProcMessage(panel->AsIGZWin(), command));

    // ---- commands from the transport dialog filter the model icons ----
    FakeGen dialogGen(&mgr);
    FakeWindow* dialog = BuildDialog(mgr, dialogGen);
    auto* car1 = new FakeExemplar();       // intentionally leaked (the icon buttons hold variants on them)
    auto* car2 = new FakeExemplar();
    auto* car3 = new FakeExemplar();
    car1->Set(kMySimPropertyWealth, 1u);
    car1->Set(kMySimPropertySortOrder, 20u);
    car1->Set(kMySimPropertyBitmap, 0x9001u);
    car1->Set(kMySimPropertyName, cRZString("Sedan"));
    car2->Set(kMySimPropertyWealth, 1u);
    car2->Set(kMySimPropertySortOrder, 10u);
    car2->Set(kMySimPropertyNameAlt2, cRZString("Wagon"));
    car3->Set(kMySimPropertyWealth, 2u);   // other wealth: filtered out
    car3->Set(kMySimPropertySortOrder, 1u);
    host.vehicles.models = {car1, car2, car3};
    proc->SetCurrentSim(2, false, false);
    cGZMessage wealth;
    wealth.type = kGZMsgCommand;
    wealth.param1 = kMySimCommandSender;
    wealth.param2 = kMySimWealthButtonBase + 1;
    CHECK(!proc->DoWinProcMessage(dialog, wealth));   // wealth commands answer false
    CHECK_EQ(proc->WealthFilter(), 1u);
    CHECK_EQ(proc->TotalIcons(), 2u);
    // the two matching models are shown in sort order: Wagon (10) then Sedan (20)
    FakeBtnWin* icon0 = ButtonOf(dialog, kMySimIconButtonBase);
    FakeBtnWin* icon1 = ButtonOf(dialog, kMySimIconButtonBase + 1);
    FakeBtnWin* icon2 = ButtonOf(dialog, kMySimIconButtonBase + 2);
    CHECK_EQ(icon0->tip, std::string("Wagon"));
    CHECK_EQ(icon1->tip, std::string("Sedan"));
    CHECK(!icon2->GetFlag(kWinVisible));
    CHECK(icon0->GetFlag(kWinVisible));
    CHECK(icon0->AsIGZWin()->GetParam(kMySimExemplarParam, reinterpret_cast<cIGZVariant**>(&out)));
    if (out) static_cast<cIGZVariant*>(out)->Release();   // GetParam counts the variant it hands back
    // the six page buttons are captioned 1..6 and the first one is selected
    CHECK_EQ(static_cast<FakeWindow*>(dialog->GetChildWindowFromIDRecursive(kMySimPageButtonBase + 1))
                 ->GetCaption().ToChar(),
             std::string("1"));
    CHECK_EQ(static_cast<FakeWindow*>(dialog->GetChildWindowFromIDRecursive(kMySimPageButtonBase + 6))
                 ->GetCaption().ToChar(),
             std::string("6"));
    CHECK_EQ(ButtonOf(dialog, kMySimPageButtonBase + 1)->fontColor, 0xffffffffu);
    CHECK_EQ(ButtonOf(dialog, kMySimPageButtonBase + 2)->fontColor, 0xff0000ffu);
    CHECK(ButtonOf(dialog, kMySimPageButtonBase + 1)->GetFlag(kWinEnabled));
    CHECK(!ButtonOf(dialog, kMySimPageButtonBase + 2)->GetFlag(kWinEnabled));
    // selecting an icon button stores its id and ends the dialog with 1 on the "OK" command
    cGZMessage iconClick;
    iconClick.type = kGZMsgCommand;
    iconClick.param1 = kMySimCommandSender;
    iconClick.param2 = kMySimIconButtonBase + 1;
    CHECK(proc->DoWinProcMessage(dialog, iconClick));
    CHECK_EQ(proc->SelectedIconButton(), kMySimIconButtonBase + 1);
    cGZMessage ok;
    ok.type = kGZMsgCommand;
    ok.param1 = kMySimCommandSender;
    ok.param2 = 0x6bf326d1;
    CHECK(proc->DoWinProcMessage(dialog, ok));
    CHECK_EQ(mgr.dialogs.size(), std::size_t(1));
    CHECK_EQ(mgr.dialogs[0].code, 0xf);
    CHECK_EQ(mgr.dialogs[0].result, std::int64_t(1));
    // the pedestrian filter lists pedestrian models by sex instead of by wealth
    auto* ped1 = new FakeExemplar();
    auto* ped2 = new FakeExemplar();
    ped1->Set(kMySimPropertyMale, 0u);    // male
    ped1->Set(kMySimPropertySortOrder, 5u);
    ped1->Set(kMySimPropertyName, cRZString("Man"));
    ped2->Set(kMySimPropertyMale, 1u);    // female
    ped2->Set(kMySimPropertySortOrder, 6u);
    ped2->Set(kMySimPropertyName, cRZString("Woman"));
    host.pedestrians.models = {ped1, ped2};
    proc->ShowChooseTransportDialog(true);   // no dialog generator yet: only the filter flag is set
    CHECK(proc->PedestrianFilter());

    // ---- the full modal dialog: the chosen icon's exemplar becomes the sim's car ----
    host.dialogGen = &dialogGen;
    mgr.popupAnswer = 1;
    proc->SetCurrentSim(2, false, false);
    sim2.cars = 0;
    car2->Set(kMySimPropertyBitmap, 0x9002u);
    proc->ShowChooseTransportDialog(false);
    CHECK(host.dialogCreated);
    CHECK_EQ(host.dialogKey.group, kMySimPortraitImageGroup);
    CHECK_EQ(host.dialogKey.instance, kMySimTransportWindowVehicles);
    CHECK_EQ(host.dialogId, kMySimTransportDialogId);
    CHECK_EQ(host.dialogIid, gz::kGZIID_cIGZWinGen);
    CHECK(host.dialogParent == controls);
    CHECK_EQ(host.restores, 1);
    CHECK_EQ(host.saves, 1);
    CHECK(dialogGen.winProc == static_cast<gz::cIGZWinProc*>(proc));
    CHECK(!proc->PedestrianFilter());
    CHECK_EQ(sim2.cars, 1);   // icon 0 is the Wagon (sort order 10)
    CHECK(!mgr.destroyed.empty() && mgr.destroyed.back() == dialog);

    // ---- messages ----
    // advice added for the current sim's advisor: queued and drawn
    cRZMessage2Standard* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMySimMsgAdviceAdded);
    message->SetIGZUnknown(adviceHigh);
    body->entries.clear();
    proc->DoMessage(message);
    CHECK_EQ(proc->Messages().size(), std::size_t(1));
    CHECK_EQ(body->entries.size(), std::size_t(1));
    message->Release();
    // advice removed again
    message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMySimMsgAdviceRemoved);
    message->SetIGZUnknown(adviceExpired);   // advisor id matches but it is not in the list
    proc->DoMessage(message);
    message->Release();
    CHECK_EQ(proc->Messages().size(), std::size_t(1));
    // a message for another advisor is ignored
    auto* foreign = new FakeAdvice();
    foreign->advisorId = 99;
    message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMySimMsgAdviceAdded);
    message->SetIGZUnknown(foreign);
    proc->DoMessage(message);
    message->Release();
    CHECK_EQ(proc->Messages().size(), std::size_t(1));
    // "my sims changed" while hidden is ignored
    proc->Hide();
    CHECK(!proc->Shown());
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimPanelWindow)->GetFlag(kWinVisible) == false);
    CHECK_EQ(host.agent.hides, 1);
    const std::size_t tracks = static_cast<std::size_t>(host.agent.tracks);
    message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMessageMySimsChanged);
    proc->DoMessage(message);   // hidden: no new slot scan
    message->Release();
    CHECK_EQ(host.agent.tracks, static_cast<int>(tracks));
    proc->Show();
    CHECK(proc->Shown());
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimPanelWindow)->GetFlag(kWinVisible));
    CHECK_EQ(host.agent.tracks, static_cast<int>(tracks) + 2);   // GetCurrentSims + Show both track nothing
    // the two model-chosen messages feed the panel's command handler (no script service, so nothing opens)
    message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMySimMsgCarChosen);
    CHECK(proc->DoMessage(message));
    message->Release();
    CHECK(!proc->PedestrianFilter());
    // the "forward to the advice list" message reaches the body list window
    const int forwarded = body->messages;
    cGZMessage forward;
    forward.type = kMySimMsgForwardToAdviceList;
    proc->DoWinProcMessage(panel->AsIGZWin(), forward);
    CHECK_EQ(body->messages, forwarded + 1);

    // ---- BootSim removes the current sim and returns to the main page ----
    proc->SetCurrentSim(2, false, false);
    proc->BootSim();
    CHECK_EQ(sim2.deleted, 1);
    CHECK(controls->GetChildWindowFromIDRecursive(kMySimDialogMain)->GetFlag(kWinVisible));

    // ---- AddSim runs the modal pick-sim dialog ----
    proc->AddSim(3);
    CHECK(host.LastPick() != nullptr);
    CHECK_EQ(host.LastPick()->inits, 1);
    CHECK_EQ(host.LastPick()->shutdowns, 1);
    CHECK(!controls->GetChildWindowFromIDRecursive(kMySimDialogLarge)->GetFlag(kWinVisible));

    // ---- AddMessage / RemoveMessage ----
    proc->AddMessage(adviceSpecial);
    CHECK(adviceSpecial->shown);
    CHECK_EQ(proc->Messages().size(), std::size_t(2));
    proc->RemoveMessage(adviceSpecial);
    CHECK_EQ(proc->Messages().size(), std::size_t(1));

    // ---- the two game-data callbacks ----
    const std::size_t posted = host.posted.size();
    cRZVariant value;
    DummyUnknown callbackContext;
    EntryId callbackEntry = registry->AddChildEntry(static_cast<void*>(&callbackContext), "callback_context",
                                                    &s_sim_select_car, registry->GetRootNodeEntry(), 2, &host);
    CHECK(callbackEntry != 0);
    CHECK(s_sim_select_car(&callbackContext, value, false, registry));
    CHECK(host.posted.size() == posted + 1 && host.posted.back() == kMySimMsgCarChosen);
    CHECK(value.GetValBool());
    CHECK(s_sim_select_pedestrian(&callbackContext, value, true, registry));   // a write posts nothing
    CHECK(host.posted.size() == posted + 1);
    CHECK(s_sim_select_car(&callbackContext, value, false, nullptr));          // no registry: still true

    // ---- Shutdown: unsubscribes, removes the registry node, destroys the panel and frees the object ----
    const std::size_t windowCount = mgr.destroyed.size();
    proc->Shutdown();   // drops the reference it took; the handler's own reference keeps the object alive
    CHECK_EQ(host.server.removed.size(), std::size_t(6));
    CHECK_EQ(mgr.destroyed.size(), windowCount + 1);
    CHECK(panel->AsIGZWin()->GetParentWin() == nullptr);
    proc->Release();    // destroys the panel procedure (the transport exemplar reference goes here)

    SetMySimWinProcHost(nullptr);
    registry->Release();
    controls->Release();
    // The doubles are counted by hand and never delete themselves: free them once nothing can reference them.
    delete panel;   // after the panel procedure and the container: its window outlives both
    for (FakePickSim* pick : host.picks) delete pick;
    // the dialog's icon buttons keep their exemplars in a variant param: close the dialog before freeing the exemplars
    if (dialogGen.window_) dialogGen.window_->Release();
    dialogGen.window_ = nullptr;
    for (FakeExemplar* model : {car1, car2, car3, ped1, ped2}) delete model;
    sim2.advisor.advices.clear();   // the advisor's map holds references to them
    for (FakeAdvice* advice : {adviceHigh, adviceExpired, adviceSpecial, foreign}) delete advice;
    if (failures == 0) std::puts("sc4_mysimwinproc: all passed");
    return failures ? 1 : 0;
}
