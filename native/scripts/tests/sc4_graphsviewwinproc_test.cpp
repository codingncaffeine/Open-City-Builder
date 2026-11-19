// cSC4GraphsViewWinProc — the graphs window procedure: window wiring, option / expand handling and the exemplar ->
// graph -> history-warehouse data feed. Uses fake windows (fake_winmgr.h), a fake graph host (fake_graph.h) and a fake
// history / exemplar host, so no game data is needed.
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "fake_graph.h"
#include "fake_winmgr.h"
#include "gz/BarGraph.h"
#include "gz/LineGraph.h"
#include "gz/PieGraph.h"
#include "gz/Win.h"
#include "gz/WinBtn.h"
#include "gz/WinSlider.h"
#include "rz/cRZVariant.h"
#include "sc4/Exemplar.h"
#include "sc4/GraphsViewWinProc.h"
#include "sc4/SCProperty.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

using namespace sc4;

// The globals the default host would read; defined here because ServiceLocator.cpp is not linked.
namespace SL {
cIGZMessageServer2* spMS2 = nullptr;
cIGZPersistResourceManager* spRM = nullptr;
cIGZUnknown* spSimulator = nullptr;
cIGZUnknown* spHistoryWarehouse = nullptr;
}  // namespace SL

namespace {

constexpr std::uint32_t kCollapsedPanelId = 0x8a8b5b72, kExpandedPanelId = 0x8a8b5b71;
constexpr std::uint32_t kCollapseId = 0xa8b7e00, kExpandId = 0xa8b7e01, kToggleId = 0xca8b82f9;
constexpr std::uint32_t kOptionContainerId = 0xaa9244b6, kAreaWindowId = 0x8a4b0aa0;
constexpr std::uint32_t kCommandSender = 0x287259f6;
constexpr std::uint32_t kMonthly = 0x66956816;

std::uint32_t MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
}

// ---- window-layer fakes ---------------------------------------------------------------------------------------

struct FakeBuffer final : IWinBuffer {
    cRZRect bounds{0, 0, 0, 0};
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { if (--refs <= 0) { delete this; return 0; } return refs; }
    const cRZRect* GetBounds() const override { return &bounds; }
    std::uint32_t MakeNativeColor(std::uint8_t, std::uint8_t, std::uint8_t) override { return 0; }
    void NativeToRGB(std::uint32_t, std::uint8_t&, std::uint8_t&, std::uint8_t&) override {}
    void* GetDrawContext() override { return nullptr; }
    bool SetFadeAlpha(std::uint8_t) override { return false; }
    bool Init(int w, int h) override { bounds = {0, 0, w, h}; return true; }
    void Shutdown() override {}
};

struct Gfx final : IWinGraphics {
    int created = 0, native = 0;
    std::uint32_t MakeNativeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) override {
        ++native;
        return 0xff000000u | (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b;
    }
    void NativeToRGB(std::uint32_t n, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b) override {
        r = (n >> 16) & 0xff; g = (n >> 8) & 0xff; b = n & 0xff;
    }
    IWinBuffer* PrimaryBuffer() override { return nullptr; }
    bool DrawSystemActive() override { return false; }
    IWinBuffer* GraphicsCreateBuffer() override { ++created; return new FakeBuffer; }
};

struct FakeFont final : gz::IWinFont {
    graphfakes::Font* font = nullptr;
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    cIGZFont* AsFont() override { return font; }
    std::uint32_t DefaultTextColor() override { return 0x111111; }
    std::uint32_t DefaultShadowColor() override { return 0x222222; }
    std::int16_t LineHeight(int, int) override { return 12; }
    int CountLines(const char*, std::uint32_t, int) override { return 1; }
    int WrappedWidth(const char*, std::uint32_t, int) override { return 40; }
    void MeasureText(const char*, std::uint32_t length, cRZRect& out) override { out = {0, 0, int(length) * 6, 12}; }
    int TextWidth(const char*, std::uint32_t length) override { return int(length) * 6; }
    void DrawText(IWinBuffer*, int, int, const char*, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
    void DrawTextBox(IWinBuffer*, const cRZRect&, const char*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, const cRZRect*, int) override {}
};

struct ButtonHost final : gz::IWinButtonHost {
    FakeFont font;
    graphfakes::Font graphFont;
    std::vector<std::uint32_t> sounds;
    ButtonHost() { font.font = &graphFont; }
    gz::IWinButtonFont* Font(std::uint32_t) override { return &font; }
    cIGZBuffer* Image(std::uint32_t) override { return nullptr; }
    std::uint32_t DefaultSoundId(int which) override { return 100 + which; }
    void PlaySound(std::uint32_t id) override { sounds.push_back(id); }
    void ShowTip(const gz::BtnTipRequest&) override {}
    void HideTip() override {}
    bool HasWindowColorManager() override { return true; }
};

struct FakeMessageServer final : cIGZMessageServer2 {
    std::vector<std::pair<cIGZMessageTarget2*, std::uint32_t>> added, removed;
    int refs = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2*, bool) override { return true; }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { added.push_back({target, type}); return true; }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { removed.push_back({target, type}); return true; }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return 1; }
    bool CreateMessage(std::uint32_t, std::uint32_t, void**) override { return false; }
};

struct FakeMessage final : cIGZMessage2 {
    std::uint32_t type = 0;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    bool Create(std::uint32_t, void**) const override { return false; }
    std::uint32_t GetType() const override { return type; }
    void SetType(std::uint32_t value) override { type = value; }
    bool IsEqual(const cIGZMessage2&) const override { return false; }
    bool IsLess(const cIGZMessage2&) const override { return false; }
};

// ---- the graph exemplars --------------------------------------------------------------------------------------

struct FakeExemplar final : cISCResExemplar {
    cISCPropertyHolder* holder = nullptr;
    int refs = 0;
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == 0xa52160f5) { *out = this; AddRef(); return true; }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { if (--refs <= 0) { delete this; return 0; } return refs; }
    cISCPropertyHolder* AsISCPropertyHolder() override { return holder; }
    bool IsPropertyLocal(std::uint32_t) override { return true; }
    cISCResExemplarCohort* GetCohort() override { return nullptr; }
    void SetCohort(cISCResExemplarCohort*, bool) override {}
    bool GetKey(cGZPersistResourceKey&) override { return false; }
    bool SetKey(const cGZPersistResourceKey&) override { return true; }
    void CompactProperties() override {}
    void GenerateCachedMap() override {}
    bool Write(cIGZOStream&) const override { return false; }
    bool Read(cIGZIStream&) override { return false; }
    std::uint32_t GetGZCLSID() const override { return 0; }
};

void AddUint32s(cSCPropertyHolder& holder, std::uint32_t id, const std::vector<std::uint32_t>& values) {
    cRZVariant variant;
    variant.RefUint32(values.data(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, true);
}

void AddFloats(cSCPropertyHolder& holder, std::uint32_t id, const std::vector<float>& values) {
    cRZVariant variant;
    variant.RefFloat32(values.data(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, true);
}

void AddBytes(cSCPropertyHolder& holder, std::uint32_t id, const std::vector<std::uint8_t>& values) {
    cRZVariant variant;
    variant.RefUint8(values.data(), static_cast<std::uint32_t>(values.size()));
    holder.AddProperty(id, &variant, true);
}

struct GraphData {
    cSCPropertyHolder holder;
};

// ---- the panel host ------------------------------------------------------------------------------------------

struct Host final : IGraphsViewHost {
    FakeMessageServer* server = nullptr;
    bool filterResult = true;
    bool loadStrings = false;
    cRZString title;
    std::uint32_t simDay = 10000, startDay = 9000;
    std::map<std::uint32_t, GraphData*> data;
    int exemplarLoads = 0, historyCalls = 0, stringCalls = 0;
    std::uint32_t lastOption = 0xffffffffu;

    cIGZMessageServer2* MessageServer() override { return server; }
    bool FilterUICommandMessage(cGZMessage&) override { return filterResult; }
    bool LoadExemplarString(cIGZString& out, cISCPropertyHolder*, std::uint32_t) override {
        ++stringCalls;
        if (!loadStrings) return false;
        out.AssignFromIGZString(title);
        return true;
    }
    cISCResExemplar* LoadGraphExemplar(std::uint32_t option) override {
        ++exemplarLoads;
        lastOption = option;
        auto it = data.find(option);
        if (it == data.end()) return nullptr;   // instance 0 (hide) and unknown options have no exemplar
        auto* exemplar = new FakeExemplar;
        exemplar->holder = &it->second->holder;
        return exemplar;
    }
    bool HistoryPoint(std::uint32_t series, const cIGZDate& date, float& value) override {
        ++historyCalls;
        value = static_cast<float>(series) * 10.0f + static_cast<float>(date.DayNumber() % 7);
        return true;
    }
    std::uint32_t SimulatorDayNumber() override { return simDay; }
    void SimulatorStartDate(cIGZDate& out) override { out.Set(startDay); }
};

// ---- the window tree -----------------------------------------------------------------------------------------

struct Tree {
    FakeMgr mgr;
    cGZWin* main = nullptr;
    cGZWin* parent = nullptr;
    cGZWin* frame = nullptr;
    cGZWin* collapsed = nullptr;
    cGZWin* expanded = nullptr;
    cGZWin* expandButton = nullptr;
    cGZWin* collapseButton = nullptr;
    cGZWin* toggleCollapsed = nullptr;
    cGZWin* toggleExpanded = nullptr;
    cGZWin* areaWindow = nullptr;
    cGZWin* options = nullptr;
    cGZWin* optionChild = nullptr;
    gz::cGZWinSlider* slider = nullptr;
    std::map<std::uint32_t, gz::cGZWinBtn*> optionButtons;

    cGZWin* Make(cGZWin* parentWindow) {
        auto* window = new cGZWin(&mgr);
        window->SetArea(0, 0, 800, 600);
        if (parentWindow) parentWindow->ChildAdd(window);
        return window;
    }
    gz::cGZWinBtn* MakeOption(std::uint32_t id) {
        auto* button = new gz::cGZWinBtn(&mgr);
        button->SetID(id);
        frame->ChildAdd(button->AsIGZWin());
        optionButtons[id] = button;
        return button;
    }

    explicit Tree(Host&) {
        main = new cGZWin(&mgr);
        main->AddRef();                 // the test owns the root
        main->SetArea(0, 0, 800, 600);
        mgr.mainWin = main;
        parent = Make(main);
        frame = Make(parent);
        collapsed = Make(parent);
        collapsed->SetID(kCollapsedPanelId);
        expanded = Make(parent);
        expanded->SetID(kExpandedPanelId);
        expandButton = Make(collapsed);
        expandButton->SetID(kExpandId);
        toggleCollapsed = Make(collapsed);
        toggleCollapsed->SetID(kToggleId);
        collapseButton = Make(expanded);
        collapseButton->SetID(kCollapseId);
        toggleExpanded = Make(expanded);
        toggleExpanded->SetID(kToggleId);
        areaWindow = Make(expanded);
        areaWindow->SetID(kAreaWindowId);
        options = Make(frame);
        options->SetID(kOptionContainerId);
        optionChild = new cGZWin(&mgr);       // a button-like child for SetupGraphButtons
        options->ChildAdd(optionChild);
        MakeOption(0x4000); MakeOption(0x4100);
        MakeOption(0x4001); MakeOption(0x4101);
        MakeOption(0x4002); MakeOption(0x4102);
        // the time-scale slider: value 3 of 0..0x20
        slider = new gz::cGZWinSlider(&mgr);
        slider->SetID(0xea4a7cf2);
        slider->SetArea(0, 0, 200, 20);
        slider->SetValue(3);
        frame->ChildAdd(slider->AsIGZWin());
    }
    ~Tree() {
        if (main) { cGZWin* root = main; main = nullptr; root->Release(); }
    }
};

std::vector<float> ExpectedLineSeries(const std::vector<std::uint32_t>& valueIds,
                                      const std::vector<std::uint32_t>& operandIds,
                                      const std::vector<std::uint32_t>& combine,
                                      const std::vector<float>& scales, std::uint32_t series, int samples) {
    // time scale 0: 12 points of 30 days; sim day 10000 -> rounded end 9990, first sample 9630.
    std::vector<float> out;
    std::uint32_t day = 9630;
    for (int i = 0; i < samples; ++i) {
        float value = static_cast<float>(valueIds[series]) * 10.0f + static_cast<float>(day % 7);
        if (combine[series] != 0) {
            const float operand = static_cast<float>(operandIds[series]) * 10.0f + static_cast<float>(day % 7);
            switch (combine[series]) {
            case 1: value += operand; break;
            case 2: value -= operand; break;
            case 3: value *= operand; break;
            case 4: value = operand != 0 ? value / operand : 0; break;
            default: break;
            }
        }
        out.push_back(value * scales[series]);
        day += 30;
    }
    return out;
}

}  // namespace

int main() {
    Host host;
    Gfx gfx;
    graphfakes::Host graphHost;
    ButtonHost buttonHost;
    FakeMessageServer server;
    host.server = &server;
    SL::spMS2 = &server;
    SetWinGraphics(&gfx);
    SetGraphHost(&graphHost);
    SetWinButtonHost(&buttonHost);
    SetGraphsViewHost(&host);

    // ---- exemplars -------------------------------------------------------------------------------------------
    GraphData line, bar, pie, invalid;
    {
        std::vector<std::uint32_t> style{1};
        AddUint32s(line.holder, 0xaa4c0d1b, style);
        AddUint32s(line.holder, 0x6a4aee40, {1, 2, 4});
        AddUint32s(line.holder, 0x6a4aee5f, {0, 3, 0});
        AddUint32s(line.holder, 0x6a4aee68, {0, 1, 0});
        AddFloats(line.holder, 0x6a4aeee2, {1.0f, 2.0f, 0.5f});
        AddBytes(line.holder, 0x6a4aeedf, {10, 20, 30, 40, 50, 60, 70, 80, 90});
    }
    {
        AddUint32s(bar.holder, 0xaa4c0d1b, {2});
        AddUint32s(bar.holder, 0x6a4aee40, {1, 2});
        AddUint32s(bar.holder, 0x6a4aeee3, {0, 1});
        AddUint32s(bar.holder, 0x6a4aee68, {0, 0});
        AddFloats(bar.holder, 0x6a4aeee2, {1.0f, 2.0f});
        AddBytes(bar.holder, 0x6a4aeee4, {1, 2, 3, 4, 5, 6});
        AddBytes(bar.holder, 0x6a4aeee5, {7, 8, 9, 10, 11, 12});
    }
    AddUint32s(pie.holder, 0xaa4c0d1b, {3});
    AddUint32s(invalid.holder, 0xaa4c0d1b, {0});
    host.data[1] = &line;
    host.data[2] = &bar;
    host.data[3] = &pie;
    host.data[4] = &invalid;

    Tree tree(host);
    auto* proc = new cSC4GraphsViewWinProc(tree.frame);
    proc->AddRef();   // the test owns one reference

    // ---- constructor state ----------------------------------------------------------------------------------
    CHECK(proc->Window() == tree.frame && proc->CollapsedPanel() == nullptr && proc->ExpandedPanel() == nullptr);
    CHECK(proc->Graph() == nullptr && proc->GraphStyle() == 0 && proc->CurrentOption() == 1);
    CHECK(proc->TimeScale() == 0 && proc->Expanded() && !proc->IsVisible() && !proc->Initialised());
    CHECK(proc->PlotCount() == 0 && proc->OptionCount() == 0);

    // ---- QueryInterface -------------------------------------------------------------------------------------
    {
        void* out = nullptr;
        CHECK(proc->QueryInterface(0x22e85d8e, &out));
        CHECK(out == static_cast<gz::cIGZWinProc*>(proc));
        CHECK(reinterpret_cast<char*>(out) == reinterpret_cast<char*>(proc) + 8);   // the second view
        static_cast<gz::cIGZWinProc*>(out)->Release();
        CHECK(proc->QueryInterface(0x452294aa, &out));
        CHECK(out == static_cast<cIGZMessageTarget2*>(proc));
        static_cast<cIGZMessageTarget2*>(out)->Release();
        CHECK(proc->QueryInterface(0x1234, &out) == false);
    }

    // ---- Init -----------------------------------------------------------------------------------------------
    CHECK(proc->Init());
    CHECK(proc->Initialised());
    CHECK(proc->CollapsedPanel() == tree.collapsed && proc->ExpandedPanel() == tree.expanded);
    CHECK(server.added.size() == 1 && server.added[0].first == static_cast<cIGZMessageTarget2*>(proc) &&
          server.added[0].second == kMonthly);
    CHECK(!tree.collapsed->IsVisible() && !tree.expanded->IsVisible());
    CHECK(tree.expandButton->GetNotificationTarget() == tree.frame);
    CHECK(tree.collapseButton->GetNotificationTarget() == tree.frame);
    CHECK(tree.toggleCollapsed->GetNotificationTarget() == tree.frame);
    CHECK(tree.toggleExpanded->GetNotificationTarget() == tree.frame);
    CHECK(tree.optionChild->GetNotificationTarget() == tree.frame);   // SetupGraphButtons
    CHECK(proc->Init());   // idempotent: no second notification

    // ---- the line graph -------------------------------------------------------------------------------------
    proc->ShowGraphType(1);
    CHECK(proc->GraphStyle() == 1 && proc->CurrentOption() == 1 && proc->IsVisible() && proc->Expanded());
    CHECK(tree.expanded->IsVisible() && !tree.collapsed->IsVisible());
    CHECK(tree.frame->IsVisible());
    CHECK(proc->PlotCount() == 3 && proc->OptionCount() == 1);
    CHECK(proc->LegendState().size() == 3);
    CHECK(proc->AxisMaxOverride() == -3.4028235e38f && proc->AxisMinOverride() == 3.4028235e38f);
    auto* lineGraph = dynamic_cast<gz::cGZLineGraph*>(proc->Graph());
    CHECK(lineGraph != nullptr);
    if (lineGraph) {
        CHECK(lineGraph->SeriesCount() == 3);
        CHECK(lineGraph->GetChildCount() == 3);   // one legend toggle per plot
        const std::vector<std::uint32_t> ids{1, 2, 4}, operands{0, 3, 0}, combine{0, 1, 0};
        const std::vector<float> scales{1.0f, 2.0f, 0.5f};
        for (std::uint32_t series = 0; series < 3; ++series) {
            const std::vector<float> expected = ExpectedLineSeries(ids, operands, combine, scales, series, 13);
            const std::vector<float>& got = lineGraph->Series(series);
            CHECK(got.size() == expected.size());
            for (std::size_t i = 0; i < got.size() && i < expected.size(); ++i) CHECK(got[i] == expected[i]);
        }
        CHECK(lineGraph->SeriesInfo(0).color == MakeColor(10, 20, 30));
        CHECK(lineGraph->SeriesInfo(1).color == MakeColor(40, 50, 60));
        CHECK(lineGraph->SeriesInfo(2).color == MakeColor(70, 80, 90));
        CHECK(lineGraph->SeriesInfo(0).lineUse && !lineGraph->SeriesInfo(0).symbolUse);
        CHECK(lineGraph->SeriesInfo(0).thickness == 1 && lineGraph->SeriesInfo(0).symbolSize == 12);
        CHECK(lineGraph->CategoryNames().size() == 1);   // both captions land on index 0 / time scale 0
    }

    // ---- the time scale -------------------------------------------------------------------------------------
    proc->SetTimeScale(2);   // 10 years, 120 points
    CHECK(proc->TimeScale() == 2);
    if (lineGraph) CHECK(lineGraph->SeriesCount() == 3 && lineGraph->Series(0).size() == 121);

    // ---- monthly message ------------------------------------------------------------------------------------
    {
        FakeMessage message;
        message.type = kMonthly;
        CHECK(proc->DoMessage(&message));
        message.type = 0x1234;
        CHECK(!proc->DoMessage(&message));
    }

    // ---- expand / collapse commands -------------------------------------------------------------------------
    CHECK(tree.frame->IsVisible());
    {
        cGZMessage message;
        message.type = 3;
        message.param1 = kCommandSender;
        message.param2 = kCollapseId;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(!proc->Expanded() && tree.collapsed->IsVisible() && !tree.expanded->IsVisible());
        message.param2 = kExpandId;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(proc->Expanded() && tree.expanded->IsVisible() && !tree.collapsed->IsVisible());
        message.param2 = kToggleId;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(!proc->Expanded());
        message.param2 = kToggleId;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(proc->Expanded());
        // the time-scale slider moves the graph to its value
        message.param1 = 0x887113a3;
        message.param2 = 0xea4a7cf2;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(tree.slider->GetValue() == 3 && proc->TimeScale() == 3);
        // an unrelated slider id is ignored
        message.param2 = 0xea4a7cf3;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(proc->TimeScale() == 3);
        // the tutorial filter swallows the command
        host.filterResult = false;
        message.param1 = kCommandSender;
        message.param2 = kToggleId;
        CHECK(proc->DoWinProcMessage(tree.frame, message));
        CHECK(proc->Expanded());
        host.filterResult = true;
    }

    // ---- the option command switches to the bar chart -------------------------------------------------------
    {
        cGZMessage message;
        message.type = 3;
        message.param1 = kCommandSender;
        message.param2 = 0x4002;
        CHECK(!proc->DoWinProcMessage(tree.frame, message));
        CHECK(proc->CurrentOption() == 2 && proc->GraphStyle() == 2 && proc->Expanded());
        CHECK(tree.optionButtons[0x4001]->IsOn() == false && tree.optionButtons[0x4101]->IsOn() == false);
        CHECK(tree.optionButtons[0x4002]->IsOn() && tree.optionButtons[0x4102]->IsOn());
        auto* barGraph = dynamic_cast<gz::cGZBarGraph*>(proc->Graph());
        CHECK(barGraph != nullptr);
        if (barGraph) {
            CHECK(barGraph->SeriesCount() == 2 && proc->PlotCount() == 2 && proc->OptionCount() == 2);
            CHECK(barGraph->Series(0).size() == 2 && barGraph->Series(1).size() == 2);
            // sim day 10000: 10000 % 7 == 4
            CHECK(barGraph->Series(0)[0] == 14.0f && barGraph->Series(0)[1] == 0.0f);
            CHECK(barGraph->Series(1)[0] == 0.0f && barGraph->Series(1)[1] == 48.0f);
            CHECK(barGraph->SeriesInfo(0).fillColor == MakeColor(1, 2, 3));
            CHECK(barGraph->SeriesInfo(0).outlineColor == MakeColor(7, 8, 9));
            CHECK(barGraph->SeriesInfo(1).fillColor == MakeColor(4, 5, 6));
            CHECK(barGraph->SeriesInfo(1).outlineColor == MakeColor(10, 11, 12));
        }
    }

    // ---- a legend toggle rebuilds the line chart with the enabled plots compacted ----------------------------
    proc->ShowGraphType(1);
    proc->SetTimeScale(0);   // back to 1 year / 12 points
    {
        // the legend buttons live under the graph control, which is a child of the panel
        CHECK(proc->Graph() != nullptr);
        if (proc->Graph()) {
            gz::cIGZWinBtn* graphToggle = nullptr;
            if (proc->Graph()->GetChildAsRecursive(0x4000001, 0x8810, reinterpret_cast<void**>(&graphToggle))) {
                graphToggle->SetBtnFlag(1, false);   // switch the second plot off
                graphToggle->Release();
                proc->RefreshGraph();
                CHECK(!proc->Plots()[1].enabled);
                CHECK(proc->LegendState().at(2) == false);
                auto* graph = dynamic_cast<gz::cGZLineGraph*>(proc->Graph());
                CHECK(graph != nullptr);
                if (graph) {
                    CHECK(graph->SeriesCount() == 3);
                    // plot 2 (value 4) moved into series 1
                    CHECK(!graph->Series(1).empty());
                    const float expected = (4.0f * 10.0f + static_cast<float>(9630 % 7)) * 0.5f;
                    CHECK(graph->Series(1)[0] == expected);
                }
            } else {
                CHECK(false);
            }
        }
    }

    // ---- pie and invalid styles -----------------------------------------------------------------------------
    proc->ShowGraphType(3);
    CHECK(proc->GraphStyle() == 3 && proc->CurrentOption() == 3);
    CHECK(dynamic_cast<gz::cGZPieGraph*>(proc->Graph()) != nullptr);
    proc->ShowGraphType(4);   // style 0: no graph is built
    CHECK(proc->Graph() == nullptr && proc->GraphStyle() == 0 && proc->CurrentOption() == 4);
    proc->RefreshGraph();     // no graph: nothing happens

    // ---- hide / show ----------------------------------------------------------------------------------------
    proc->ShowGraphType(1);
    CHECK(proc->Graph() != nullptr);
    proc->HideWindow();
    CHECK(!proc->IsVisible() && proc->Graph() == nullptr);
    CHECK(!tree.frame->IsVisible() && !tree.expanded->IsVisible() && !tree.collapsed->IsVisible());
    proc->ShowWindow();
    CHECK(proc->IsVisible() && proc->Graph() != nullptr);

    // ---- Shutdown -------------------------------------------------------------------------------------------
    CHECK(proc->Shutdown());
    CHECK(!proc->Initialised());
    CHECK(server.removed.size() == 1 && server.removed[0].first == static_cast<cIGZMessageTarget2*>(proc));
    CHECK(proc->Graph() == nullptr && proc->CollapsedPanel() == nullptr && proc->ExpandedPanel() == nullptr);
    CHECK(proc->Shutdown());   // idempotent

    proc->Release();   // deletes the object (and releases the frame window)

    if (failures == 0) std::puts("sc4_graphsviewwinproc: all passed");
    return failures ? 1 : 0;
}
