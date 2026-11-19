#include "sc4/GraphsViewWinProc.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "gz/BarGraph.h"
#include "gz/Buffer.h"
#include "gz/Font.h"
#include "gz/Graph.h"
#include "gz/PieGraph.h"
#include "gz/Win.h"
#include "gz/WinGen.h"
#include "gz/WinBtn.h"
#include "gz/WinText.h"
#include "gz/WinSlider.h"
#include "sc4/Exemplar.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SLBuffer.h"
#include "sc4/SLString.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"

namespace sc4 {

namespace {

IGraphsViewHost* g_host = nullptr;
IGraphLineDrawing* g_lineDrawing = nullptr;

// ---- the graph exemplar --------------------------------------------------------------------------------------
constexpr std::uint32_t kGraphExemplarType = 0x6534284a;    // the exemplar type every tuning resource uses
constexpr std::uint32_t kGraphExemplarGroup = 0xca4ad545;
constexpr std::uint32_t kGraphExemplarIid = 0xa52160f5;     // cISCResExemplar
constexpr std::uint32_t kPropertyStyle = 0xaa4c0d1b;
constexpr std::uint32_t kPropertyTitle = 0x8a416a99;        // localized title key
constexpr std::uint32_t kPropertyName = 0x20;               // fallback title
constexpr std::uint32_t kPropertyValueId = 0x6a4aee40;
constexpr std::uint32_t kPropertyOperandId = 0x6a4aee5f;
constexpr std::uint32_t kPropertyCombine = 0x6a4aee68;
constexpr std::uint32_t kPropertyPlotName = 0x6a4aeedc;
constexpr std::uint32_t kPropertyPlotColor = 0x6a4aeedf;
constexpr std::uint32_t kPropertyPlotScale = 0x6a4aeee2;
constexpr std::uint32_t kPropertyBarSeries = 0x6a4aeee3;
constexpr std::uint32_t kPropertyBarFillColor = 0x6a4aeee4;
constexpr std::uint32_t kPropertyBarOutlineColor = 0x6a4aeee5;
constexpr std::uint32_t kPropertyAxisMax = 0x6a4aeded;      // +0x84, sentinel -FLT_MAX
constexpr std::uint32_t kPropertyAxisMin = 0xea4aedc6;      // +0x88, sentinel +FLT_MAX
constexpr std::uint32_t kPropertyAutoTickSpacing = 0x6a4aedef;   // +0x8c
constexpr std::uint32_t kStringGroup = 0x6a231eaa;          // the UI string group of the graph labels
constexpr std::uint32_t kAxisCaptionOffset = 0x8a5d420d;    // second x-axis caption

// ---- the panel furniture -------------------------------------------------------------------------------------
constexpr std::uint32_t kCollapsedPanelId = 0x8a8b5b72;
constexpr std::uint32_t kExpandedPanelId = 0x8a8b5b71;
constexpr std::uint32_t kCollapseCommandId = 0xa8b7e00;     // in the expanded panel
constexpr std::uint32_t kExpandCommandId = 0xa8b7e01;       // in the collapsed panel
constexpr std::uint32_t kToggleCommandId = 0xca8b82f9;      // in both
constexpr std::uint32_t kOptionContainerId = 0xaa9244b6;
constexpr std::uint32_t kTimeScaleSliderId = 0xea4a7cf2;
constexpr std::uint32_t kTimeScaleTextId = 0x8a2871d9;
constexpr std::uint32_t kGraphAreaWindowId = 0x8a4b0aa0;
constexpr std::uint32_t kTimeScaleCaptionId = 0x8a2871d8;   // the "Time Scale" caption (UI script 0x96a006b0/0xea2871aa: GZWinText 0x8a2871d8)
constexpr std::uint32_t kTimeScaleWindowIds[3] = {kTimeScaleSliderId, kTimeScaleTextId, kTimeScaleCaptionId};
constexpr std::uint32_t kSliderCommandSender = 0x887113a3;  // gz::kGZSliderValueChanged
constexpr std::uint32_t kFirstOptionId = 0x4000;            // two buttons per option: +0x4000 / +0x4100
constexpr std::uint32_t kSecondOptionOffset = 0x100;
constexpr std::uint32_t kFirstSeriesButtonId = 0x4000000;
constexpr std::uint32_t kSeriesButtonMask = 0x1f;
constexpr std::uint32_t kMonthlyMessage = 0x66956816;
constexpr std::uint32_t kMessageTargetIid = 0x452294aa;   // cIGZMessageTarget2 (the +0 view)

// ---- colours, fonts and the legend button -------------------------------------------------------------------
constexpr std::uint32_t kTitleFontStyle = 0xe9c86b5f;
constexpr std::uint32_t kLabelFontStyle = 0xe9c86b6e;
constexpr std::uint32_t kLegendFontStyle = 0xe9c86b5e;
constexpr std::uint32_t kButtonSoundId = 0x4a5c31d7;
constexpr std::uint32_t kButtonImageType = 0x856ddbac, kButtonImageGroup = 0x46a006b0, kButtonImageInstance = 0x14416245;
constexpr std::uint32_t kLegendButtonStyle = 4;
constexpr int kLegendFirstY = 0x14;       // 20
constexpr int kLegendStepY = 14;          // 0x44 - 0x3c + 4

// ---- the time scale table (0x100a1ca30) ----------------------------------------------------------------------
struct TimeScaleInfo {
    std::uint32_t points;        // +0
    std::uint32_t periodDays;    // +4
    std::uint32_t caption;       // +8, string id in group 0x6a231eaa
};
constexpr TimeScaleInfo kTimeScaleTable[6] = {
    {0x0c, 0x1e, 0x2a5d4165},    // 1 year, monthly
    {0x3c, 0x1e, 0xca5d417f},    // 5 years, monthly
    {0x78, 0x1e, 0xca5d4184},    // 10 years, monthly
    {0x64, 0xb6, 0x8a5d4189},    // 50 years, half-yearly
    {0x64, 0x16d, 0x8a5d418e},   // 100 years, yearly
    {0x64, 0x721, 0x2a5d4194},   // 500 years, 5-yearly
};

constexpr float kFloatMax = 3.4028235e38f, kNegativeFloatMax = -3.4028235e38f;

// ---- helper accessors ----------------------------------------------------------------------------------------

std::uint32_t MakeColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    IWinGraphics* graphics = WinGraphics();
    return graphics ? graphics->MakeNativeColor(r, g, b) : 0u;
}

cIGZFont* StyleFont(std::uint32_t styleId) {   // RZ::WCM() vslot 0x28
    gz::IWinButtonHost* host = gz::WinButtonHost();
    if (!host) return nullptr;
    gz::IWinFont* font = host->Font(styleId);
    return font ? font->AsFont() : nullptr;
}

const cIGZVariant* GraphProperty(const cISCPropertyHolder* holder, std::uint32_t id) {
    if (!holder) return nullptr;
    const cISCProperty* property = holder->GetProperty(id);
    return property ? property->GetPropertyValue() : nullptr;
}

std::uint32_t PropertyCount(const cISCPropertyHolder* holder, std::uint32_t id) {
    const cIGZVariant* value = GraphProperty(holder, id);
    return value ? value->GetCount() : 0;
}

std::uint32_t PropertyUint32(const cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t index) {
    const cIGZVariant* value = GraphProperty(holder, id);
    if (!value || index >= value->GetCount()) return 0;
    const std::uint32_t* data = value->RefUint32();
    return data ? data[index] : 0;
}

std::uint8_t PropertyUint8(const cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t index) {
    const cIGZVariant* value = GraphProperty(holder, id);
    if (!value || index >= value->GetCount()) return 0;
    const std::uint8_t* data = value->RefUint8();
    return data ? data[index] : 0;
}

float PropertyFloat(const cISCPropertyHolder* holder, std::uint32_t id, std::uint32_t index, float fallback) {
    const cIGZVariant* value = GraphProperty(holder, id);
    if (!value || index >= value->GetCount()) return fallback;
    const float* data = value->RefFloat32();
    return data ? data[index] : fallback;
}

// The localized string a plot name points at (cSLString 0x6a231eaa / instance).
cRZString PlotName(const cISCPropertyHolder* holder, std::uint32_t index) {
    const std::uint32_t instance = PropertyUint32(holder, kPropertyPlotName, index);
    if (instance == 0) return cRZString();
    return cSLString(kStringGroup, instance, true);
}

// The original's logarithmic plot scale (only applied when both axis bounds are set):
//   value > 0: (log(value)/log(max) + value/max) * max * 0.5
//   value < 0: (log(-value)/log(-min) + value/min) * min * 0.5
float LogScale(float value, float positiveBound, float negativeBound) {
    if (value > 0.0f) {
        return (std::log(value) / std::log(positiveBound) + value / positiveBound) * positiveBound * 0.5f;
    }
    if (value < 0.0f) {
        return (std::log(-value) / std::log(-negativeBound) + value / negativeBound) * negativeBound * 0.5f;
    }
    return 0.0f;
}

void SetWindowProc(cIGZWin* window, gz::cIGZWinProc* proc) {
    if (!window) return;
    gz::cIGZWinGen* gen = nullptr;
    if (window->QueryInterface(gz::kGZIID_cIGZWinGen, reinterpret_cast<void**>(&gen))) {
        gen->SetWinProc(proc);
        gen->Release();
    }
}

void AdoptPanel(cIGZWin*& slot, cIGZWin* found) {
    if (slot == found) return;
    if (found) found->AddRef();
    cIGZWin* old = slot;
    slot = found;
    if (old) old->Release();
}

// The default host: the recovered engine accessors. The string loader and the tutorial filter are not recovered, so
// they report "not available" (the panel then falls back to the plain title / lets the command through), exactly like
// the default nSC4UI host does.
class DefaultGraphsViewHost final : public IGraphsViewHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }

    bool FilterUICommandMessage(cGZMessage&) override { return true; }   // nsSC4TutorialSystem @ 0x10006008e, see GRAPHSVIEW-TUTORIAL

    bool LoadExemplarString(cIGZString&, cISCPropertyHolder*, std::uint32_t) override {
        return false;   // SC4LoadExemplarString @ 0x100121132, see GRAPHSVIEW-STRINGS
    }

    cISCResExemplar* LoadGraphExemplar(std::uint32_t option) override {
        if (!SL::spRM) return nullptr;
        cISCResExemplar* exemplar = nullptr;
        const cGZPersistResourceKey key{kGraphExemplarType, kGraphExemplarGroup, option};
        if (!SL::spRM->GetResource(key, kGraphExemplarIid, reinterpret_cast<void**>(&exemplar), 0, nullptr))
            return nullptr;
        return exemplar;
    }

    bool HistoryPoint(std::uint32_t series, const cIGZDate& date, float& value) override {
        auto* warehouse = reinterpret_cast<cSC4HistoryWarehouse*>(SL::spHistoryWarehouse);   // spCity vslot 0x100
        return warehouse ? warehouse->GetPoint(series, date, value) : false;
    }

    std::uint32_t SimulatorDayNumber() override {   // simulator vslot 0x40
        auto* simulator = reinterpret_cast<cSC4Simulator*>(SL::spSimulator);
        return simulator ? simulator->GetSimDateNumber() : 0;
    }

    void SimulatorStartDate(cIGZDate& out) override {   // simulator vslot 0x28
        if (auto* simulator = reinterpret_cast<cSC4Simulator*>(SL::spSimulator)) simulator->GetSimStartDate(out);
    }
};

IGraphsViewHost* HostOrDefault() {
    static DefaultGraphsViewHost defaultHost;
    return g_host ? g_host : &defaultHost;
}

}  // namespace

IGraphLineDrawing* GraphLineDrawing() { return g_lineDrawing; }
void SetGraphLineDrawing(IGraphLineDrawing* drawing) { g_lineDrawing = drawing; }
IGraphsViewHost* GraphsViewHost() { return HostOrDefault(); }
void SetGraphsViewHost(IGraphsViewHost* host) { g_host = host; }

// ---- cSC4LineGraph -------------------------------------------------------------------------------------------

bool cSC4LineGraph::DrawLine(cIGZBuffer* buffer, int x0, int y0, int x1, int y1, int color,
                             std::uint32_t thickness) {   // @ 0x1002e2cf6
    if (IGraphLineDrawing* drawing = GraphLineDrawing()) {
        drawing->DrawLineAntialiased(buffer, x0, y0, x1, y1, color, thickness);
        return true;
    }
    return gz::cGZLineGraph::DrawLine(buffer, x0, y0, x1, y1, color, thickness);
}

void cSC4LineGraph::DrawLine(cIGZBuffer* buffer, cRZRect& segment, std::uint32_t series) {   // @ 0x1002e2d2a
    gz::cGZLineGraph::DrawLine(buffer, segment, series);
}

// ---- SetupGraphButtons ---------------------------------------------------------------------------------------

bool SetupGraphButtons(cIGZWin* parent, std::uint32_t id, void* child, void* context) {   // @ 0x1002e0341
    (void)parent;
    (void)id;
    if (child) static_cast<cIGZWin*>(child)->SetNotificationTarget(static_cast<cIGZWin*>(context));
    return true;
}

// ---- construction / destruction ------------------------------------------------------------------------------

cSC4GraphsViewWinProc::cSC4GraphsViewWinProc(cIGZWin* window) : window_(window) {   // @ 0x1002dfbf6
    if (window_) window_->AddRef();
}

cSC4GraphsViewWinProc::~cSC4GraphsViewWinProc() {   // @ 0x1002dfca6
    Shutdown();
    if (window_) {
        cIGZWin* window = window_;
        window_ = nullptr;
        window->Release();
    }
    // Shutdown already released the graph and the two panels; the original's destructor repeats the three releases.
    if (graph_) { graph_->Release(); graph_ = nullptr; }
    if (expandedPanel_) { expandedPanel_->Release(); expandedPanel_ = nullptr; }
    if (collapsedPanel_) { collapsedPanel_->Release(); collapsedPanel_ = nullptr; }
}

bool cSC4GraphsViewWinProc::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x1002dff1e
    if (iid == kMessageTargetIid) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == gz::kGZIID_cIGZWinProc) {
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4GraphsViewWinProc::Release() {   // @ 0x1002dff9e
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// ---- Init / Shutdown -----------------------------------------------------------------------------------------

bool cSC4GraphsViewWinProc::Init() {   // @ 0x1002dffda
    if (!initialised_) {
        if (!window_) return true;   // port guard
        cIGZWin* parent = window_->GetParentWin();
        AdoptPanel(collapsedPanel_, parent ? parent->GetChildWindowFromID(kCollapsedPanelId) : nullptr);
        AdoptPanel(expandedPanel_, parent ? parent->GetChildWindowFromID(kExpandedPanelId) : nullptr);
        SetTimeScale(0);
        if (collapsedPanel_) collapsedPanel_->HideWindow();
        if (expandedPanel_) expandedPanel_->HideWindow();
        SetWindowProc(window_, this);
        SetWindowProc(collapsedPanel_, this);
        SetWindowProc(expandedPanel_, this);
        window_->SetFlag(kWinFlag800, false);
        if (collapsedPanel_) collapsedPanel_->SetFlag(kWinFlag800, false);
        if (expandedPanel_) expandedPanel_->SetFlag(kWinFlag800, false);
        if (cIGZMessageServer2* server = HostOrDefault()->MessageServer()) server->AddNotification(this, kMonthlyMessage);
        initialised_ = true;
    }
    // The four command buttons and the two option-button groups notify the frame window (the original dereferences
    // the child lookups unconditionally; the port guards them).
    if (collapsedPanel_) {
        if (cIGZWin* w = collapsedPanel_->GetChildWindowFromIDRecursive(kExpandCommandId)) w->SetNotificationTarget(window_);
        if (cIGZWin* w = collapsedPanel_->GetChildWindowFromIDRecursive(kToggleCommandId)) w->SetNotificationTarget(window_);
    }
    if (expandedPanel_) {
        if (cIGZWin* w = expandedPanel_->GetChildWindowFromIDRecursive(kCollapseCommandId)) w->SetNotificationTarget(window_);
        if (cIGZWin* w = expandedPanel_->GetChildWindowFromIDRecursive(kToggleCommandId)) w->SetNotificationTarget(window_);
    }
    if (window_) {
        if (cIGZWin* buttons = window_->GetChildWindowFromIDRecursive(kOptionContainerId))
            buttons->EnumChildren(kGZIID_cIGZWin, SetupGraphButtons, window_);
    }
    return true;
}

bool cSC4GraphsViewWinProc::Shutdown() {   // @ 0x1002dfdc0
    if (initialised_) {
        initialised_ = false;
        if (cIGZMessageServer2* server = HostOrDefault()->MessageServer())
            server->RemoveNotification(this, kMonthlyMessage);
        if (expandedPanel_ && graph_) expandedPanel_->ChildRemove(graph_);
        if (graph_) {
            gz::cGZGraph* graph = graph_;
            graph_ = nullptr;
            graph->Release();
        }
        SetWindowProc(window_, nullptr);
        SetWindowProc(collapsedPanel_, nullptr);
        SetWindowProc(expandedPanel_, nullptr);
        if (collapsedPanel_) {
            cIGZWin* panel = collapsedPanel_;
            collapsedPanel_ = nullptr;
            panel->Release();
        }
        if (expandedPanel_) {
            cIGZWin* panel = expandedPanel_;
            expandedPanel_ = nullptr;
            panel->Release();
        }
    }
    return true;
}

// ---- time scale ----------------------------------------------------------------------------------------------

void cSC4GraphsViewWinProc::SetTimeScale(std::uint32_t scale) {   // @ 0x1002e0266
    timeScale_ = scale;
    if (!expandedPanel_) return;
    gz::cIGZWinText* text = nullptr;
    if (expandedPanel_->GetChildAsRecursive(kTimeScaleTextId, gz::kGZIID_cIGZWinText,
                                            reinterpret_cast<void**>(&text))) {
        const std::uint32_t index = scale < 6 ? scale : 5;   // the original reads past the table
        const cSLString caption(kStringGroup, kTimeScaleTable[index].caption, true);
        text->SetCaption(caption);
        text->Release();
    }
    if (graph_) {
        RefreshGraph();
        graph_->InvalidateSelfAndParents();
    }
}

// ---- window procedure ----------------------------------------------------------------------------------------

bool cSC4GraphsViewWinProc::DoWinProcMessage(cIGZWin*, cGZMessage& message) {   // @ 0x1002e0358
    if (!window_ || !window_->IsVisible() || message.type != kGZMsgCommand) return false;
    if (!HostOrDefault()->FilterUICommandMessage(message)) return true;
    if (message.param1 == kSliderCommandSender) {
        if (message.param2 == kTimeScaleSliderId) {
            gz::cIGZWinSlider* slider = nullptr;
            if (window_->GetChildAsRecursive(kTimeScaleSliderId, gz::kGZIID_cIGZWinSlider,
                                             reinterpret_cast<void**>(&slider))) {
                const std::uint32_t value = static_cast<std::uint32_t>(slider->GetValue());
                if (value != timeScale_) SetTimeScale(value);
                slider->Release();
            }
        }
    } else if (message.param1 == gz::kWinCommandSender) {
        const std::uint64_t id = message.param2;
        if (id == kCollapseCommandId) {
            SetExpanded(false);
        } else if (id == kExpandCommandId) {
            SetExpanded(true);
        } else if (id == kToggleCommandId) {
            SetExpanded(!expanded_);
        } else if (id > 0x3fff && id < 0x5000) {
            const std::uint32_t option = static_cast<std::uint32_t>(id) & 0xff;
            SetExpanded(true);
            SelectOption(option);
            if (currentOption_ != option) {
                ShowGraph(option);
                currentOption_ = option;
            }
        } else if ((id & ~static_cast<std::uint64_t>(kSeriesButtonMask)) == kFirstSeriesButtonId) {
            RefreshGraph();
        }
    }
    return false;
}

bool cSC4GraphsViewWinProc::DoMessage(cIGZMessage2* message) {   // @ 0x1002e29b2
    if (message && message->GetType() == kMonthlyMessage) {
        if (graph_) {
            RefreshGraph();
            graph_->InvalidateSelfAndParents();
        }
        return true;
    }
    return false;
}

// ---- expand / collapse and option buttons --------------------------------------------------------------------

void cSC4GraphsViewWinProc::SetExpanded(bool expanded) {   // @ 0x1002e0518
    if (!expanded) {
        if (expandedPanel_) expandedPanel_->HideWindow();
        if (collapsedPanel_) collapsedPanel_->ShowWindow();
    } else {
        if (collapsedPanel_) collapsedPanel_->HideWindow();
        if (expandedPanel_) expandedPanel_->ShowWindow();
    }
    expanded_ = expanded;
}

void cSC4GraphsViewWinProc::SelectOption(std::uint32_t option) {   // @ 0x1002e056a
    if (!window_) return;
    const std::uint32_t ids[4] = {
        currentOption_ + kFirstOptionId, currentOption_ + kFirstOptionId + kSecondOptionOffset,
        option + kFirstOptionId, option + kFirstOptionId + kSecondOptionOffset};
    for (int i = 0; i < 4; ++i) {
        gz::cIGZWinBtn* button = nullptr;
        if (window_->GetChildAsRecursive(ids[i], gz::kGZIID_cIGZWinBtn, reinterpret_cast<void**>(&button))) {
            button->SetBtnFlag(1, i >= 2);   // the previous option's two buttons off, the new one's on
            button->Release();
        }
    }
}

// ---- ShowGraph -----------------------------------------------------------------------------------------------

void cSC4GraphsViewWinProc::AddLegendEntry(std::uint32_t index, const cRZRect& area, int y,
                                           std::uint32_t outlineColor, std::uint32_t textColor) {
    const LinePlotInfo& plot = plots_[index];
    const int width = area.right - area.left;
    const int left = width - (seriesCount_ < 3 ? 0x6a : 0x5a);   // with legend buttons the text starts further right
    // The original writes a 10x6 rectangle behind every legend label, outlines it and fills it with the plot colour.
    gz::MiscGraphRect* swatch = graph_->AddMiscGraphRect();
    swatch->rect.left = left;
    swatch->rect.top = y + 3;
    swatch->rect.right = left + 0xa;
    swatch->rect.bottom = y + 9;
    swatch->outline = true;
    swatch->outlineThickness = 1;
    swatch->outlineColor = outlineColor;
    swatch->fill = true;
    swatch->fillColor = MakeColor(plot.red, plot.green, plot.blue);
    gz::MiscGraphTextInfo* text = graph_->AddMiscGraphTextInfo();
    text->text = plot.name;
    text->rect.left = swatch->rect.right + 4;
    text->rect.top = y;
    text->rect.right = width - 4;
    text->rect.bottom = y + 10;
    text->textAlignment = 0;
    text->outline = false;
    text->fill = false;
    text->SetFont(StyleFont(kLegendFontStyle), gz::kAutoColor);
    text->fontColor = textColor;
}

void cSC4GraphsViewWinProc::ShowGraph(std::uint32_t option) {   // @ 0x1002e0688
    if (!expandedPanel_) return;
    if (graph_) {   // drop the old control first, whatever happens to the exemplar lookup below
        expandedPanel_->ChildRemove(graph_);
        gz::cGZGraph* old = graph_;
        graph_ = nullptr;
        old->Release();
    }
    cISCResExemplar* exemplar = HostOrDefault()->LoadGraphExemplar(option);
    if (!exemplar) return;
    cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
    std::uint32_t style = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPropertyStyle, style);
    cRZString title;
    if (!HostOrDefault()->LoadExemplarString(title, holder, kPropertyTitle))
        cSCPropertyHelp::GetPropertyValue(holder, kPropertyName, title);

    if (graphStyle_ != style) plots_.clear();   // the old control was already dropped above
    graphStyle_ = style;

    cIGZWinMgr* manager = window_ ? window_->GetWindowManager() : nullptr;
    switch (style) {
    case 3: {
        auto* pie = new gz::cGZPieGraph(manager);
        pie->AddRef();
        graph_ = pie;
        break;
    }
    case 2: {
        auto* bar = new gz::cGZBarGraph(manager);
        bar->AddRef();
        graph_ = bar;
        break;
    }
    case 1: {
        auto* line = new cSC4LineGraph(manager);
        line->AddRef();
        graph_ = line;
        break;
    }
    default:
        break;
    }
    if (!graph_) {   // an unknown style builds no graph and does not touch the panel
        exemplar->Release();
        return;
    }

    // Show the time-scale slider / label only on the line chart (the original hides them for the bar and pie charts).
    for (std::uint32_t id : kTimeScaleWindowIds) {
        if (cIGZWin* window = expandedPanel_->GetChildWindowFromIDRecursive(id)) {
            if (graphStyle_ == 1) window->ShowWindow(); else window->HideWindow();
            window->InvalidateSelfAndParents();
        }
    }
    cRZRect area{};   // quirk guard: the original leaves this uninitialised when the layout window is missing
    cIGZWin* areaWindow = nullptr;
    if (expandedPanel_->GetChildAs(kGraphAreaWindowId, kGZIID_cIGZWin, reinterpret_cast<void**>(&areaWindow))) {
        areaWindow->GetArea(area);
        areaWindow->Release();
    }
    graph_->SetArea(area);
    expandedPanel_->ChildAdd(graph_);

    const std::uint32_t titleColor = MakeColor(0x3f, 0x49, 0x67);
    cIGZFont* titleFont = StyleFont(kTitleFontStyle);
    cIGZFont* labelFont = StyleFont(kLabelFontStyle);
    graph_->SetTitleFont(titleFont, titleColor);
    graph_->SetAxisLabelFont(gz::kAxisHorizontal, labelFont, titleColor);
    graph_->SetAxisLabelFont(gz::kAxisVertical, labelFont, titleColor);
    graph_->SetAxisTickFont(gz::kAxisHorizontal, labelFont, titleColor);
    graph_->SetAxisTickFont(gz::kAxisVertical, labelFont, titleColor);
    const std::uint32_t lineColor = MakeColor(0xaa, 0xbb, 0xc3);
    graph_->SetBackgroundOutline(false);
    graph_->SetBackgroundFillColor(MakeColor(0xda, 0xe0, 0xe5));
    graph_->SetGraphAreaOutlineAndFill(true, true);
    graph_->SetGraphAreaOutlineColor(MakeColor(0xae, 0xbf, 0xc0));
    graph_->SetGraphAreaFillColor(MakeColor(0xef, 0xf3, 0xf7));
    graph_->SetAxisLineColor(lineColor);
    graph_->SetTickColor(lineColor);
    graph_->SetAxisTickTextDrawing(gz::kAxisHorizontal, gz::kUsageOn);
    graph_->SetTitle(title, false);

    if (graphStyle_ == 1) {
        gz::cIGZLineGraph* line = nullptr;
        if (graph_->QueryInterface(gz::kGZIID_cIGZLineGraph, reinterpret_cast<void**>(&line))) {
            graph_->SetMajorTickUsage(gz::kAxisHorizontal, gz::kUsageOn);
            graph_->SetMinorTickUsage(gz::kAxisHorizontal, gz::kUsageOff);
            const std::uint32_t count = PropertyCount(holder, kPropertyValueId);
            seriesCount_ = count;
            plots_.resize(count);
            optionCount_ = 1;
            int y = kLegendFirstY;
            for (std::uint32_t i = 0; i < count; ++i) {
                LinePlotInfo& plot = plots_[i];
                plot.valueId = PropertyUint32(holder, kPropertyValueId, i);
                plot.operandId = PropertyUint32(holder, kPropertyOperandId, i);
                plot.combine = PropertyUint32(holder, kPropertyCombine, i);
                plot.series = 0;   // the line chart appends one line per enabled plot
                plot.name = PlotName(holder, i);
                if (PropertyCount(holder, kPropertyPlotColor) > i * 3) {
                    plot.red = PropertyUint8(holder, kPropertyPlotColor, i * 3);
                    plot.green = PropertyUint8(holder, kPropertyPlotColor, i * 3 + 1);
                    plot.blue = PropertyUint8(holder, kPropertyPlotColor, i * 3 + 2);
                } else {
                    plot.red = plot.green = plot.blue = 0;
                }
                plot.scale = PropertyFloat(holder, kPropertyPlotScale, i, 1.0f);
                const auto it = legendState_.find(plot.valueId);
                plot.enabled = it == legendState_.end() ? true : it->second;
                if (count >= 3) {   // a legend toggle per plot, sitting left of its swatch
                    cSLBuffer image(kButtonImageType, kButtonImageGroup, kButtonImageInstance, 1, false);
                    auto* button = new gz::cGZWinBtn(manager);
                    cIGZWin* asWindow = button->AsIGZWin();
                    asWindow->SetArea(area.right - area.left - 0x6c, y, area.right - area.left - 0x5c, y + 0x10);
                    asWindow->SetID(i + kFirstSeriesButtonId);
                    asWindow->SetNotificationTarget(window_);
                    button->SetBtnFlag(1, plot.enabled);
                    button->SetSoundID(0, kButtonSoundId);
                    button->SetStyle(kLegendButtonStyle, image.GetBuffer(), nullptr);
                    graph_->ChildAdd(asWindow);
                }
                AddLegendEntry(i, area, y, lineColor, titleColor);
                y += kLegendStepY;
            }
            axisMaxOverride_ = kNegativeFloatMax;
            axisMinOverride_ = kFloatMax;
            autoTickSpacing_ = 0.0f;
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyAxisMin, axisMinOverride_);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyAxisMax, axisMaxOverride_);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyAutoTickSpacing, autoTickSpacing_);
            RefreshGraph();
            line->Release();
        }
    } else if (graphStyle_ == 2) {
        gz::cIGZBarGraph* bar = nullptr;
        if (graph_->QueryInterface(gz::kGZIID_cIGZBarGraph, reinterpret_cast<void**>(&bar))) {
            bar->SetSeriesCount(1);
            const std::uint32_t count = PropertyCount(holder, kPropertyValueId);
            seriesCount_ = count;
            plots_.resize(count);
            optionCount_ = 1;
            for (std::uint32_t i = 0; i < count; ++i) {
                LinePlotInfo& plot = plots_[i];
                plot.valueId = PropertyUint32(holder, kPropertyValueId, i);
                plot.name = PlotName(holder, i);
                plot.scale = PropertyFloat(holder, kPropertyPlotScale, i, 1.0f);
                const std::uint32_t series = PropertyUint32(holder, kPropertyBarSeries, i);
                plot.series = series;
                plot.combine = PropertyUint32(holder, kPropertyCombine, i);
                plot.enabled = true;
                bar->InsertCategoryName(plot.name, i);   // each plot is a category, named under its bar (the original's "1-10" ...)
                if (series + 1 > optionCount_) optionCount_ = series + 1;
            }
            if (optionCount_ > 0x10) optionCount_ = 0x10;
            bar->SetSeriesCount(optionCount_);
            const std::uint32_t fillCount = PropertyCount(holder, kPropertyBarFillColor);
            const std::uint32_t outlineCount = PropertyCount(holder, kPropertyBarOutlineColor);
            for (std::uint32_t series = 0; series < optionCount_; ++series) {
                std::uint8_t r = 0xaa, g = 0xbb, b = 0xc3;
                if (series * 3 + 2 < fillCount) {
                    r = PropertyUint8(holder, kPropertyBarFillColor, series * 3);
                    g = PropertyUint8(holder, kPropertyBarFillColor, series * 3 + 1);
                    b = PropertyUint8(holder, kPropertyBarFillColor, series * 3 + 2);
                }
                bar->SetBarFillColor(MakeColor(r, g, b), series);
                r = 0x67; g = 0x71; b = 0x76;
                if (series * 3 + 2 < outlineCount) {
                    r = PropertyUint8(holder, kPropertyBarOutlineColor, series * 3);
                    g = PropertyUint8(holder, kPropertyBarOutlineColor, series * 3 + 1);
                    b = PropertyUint8(holder, kPropertyBarOutlineColor, series * 3 + 2);
                }
                bar->SetBarOutlineColor(MakeColor(r, g, b), series);
            }
            axisMaxOverride_ = kNegativeFloatMax;
            axisMinOverride_ = kFloatMax;
            autoTickSpacing_ = 0.0f;
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyAxisMin, axisMinOverride_);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyAxisMax, axisMaxOverride_);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyAutoTickSpacing, autoTickSpacing_);
            RefreshGraph();
            bar->Release();
        }
    }
    graph_->ShowWindow();
    exemplar->Release();
}

// ---- RefreshGraph --------------------------------------------------------------------------------------------

void cSC4GraphsViewWinProc::RefreshGraph() {   // @ 0x1002e1a80
    if (!graph_) return;
    if (graphStyle_ == 2) {
        gz::cIGZBarGraph* bar = nullptr;
        if (graph_->QueryInterface(gz::kGZIID_cIGZBarGraph, reinterpret_cast<void**>(&bar))) {
            RefreshBarGraph(bar);
            bar->Release();
        }
    } else if (graphStyle_ == 1) {
        gz::cIGZLineGraph* line = nullptr;
        if (graph_->QueryInterface(gz::kGZIID_cIGZLineGraph, reinterpret_cast<void**>(&line))) {
            RefreshLineGraph(line);
            line->Release();
        }
    }
}

void cSC4GraphsViewWinProc::SetVerticalAxis(float low, float high, float spacing, bool setHorizontalMin) {
    graph_->SetAxisMinValue(gz::kAxisVertical, low);
    graph_->SetAxisMaxValue(gz::kAxisVertical, high);
    // The exemplar's auto tick spacing hands the spacing to the graph; otherwise the computed one is used (the original's mayor
    // rating graph shows the computed 40 between -100 and 100). (0 would never end DrawTicks.)
    graph_->SetMajorTickSpacing(gz::kAxisVertical, autoTickSpacing_ != 0.0f ? gz::kAutoValue : spacing);
    if (setHorizontalMin) graph_->SetAxisMinValue(gz::kAxisHorizontal, 0.0f);
}

void cSC4GraphsViewWinProc::RefreshBarGraph(gz::cIGZBarGraph* bar) {
    // One sample per plot at the simulated day, grouped into the bar-chart series the plots name.
    const cGZDate date(HostOrDefault()->SimulatorDayNumber());
    std::vector<std::vector<float>> values(optionCount_);
    for (auto& series : values) series.resize(seriesCount_);
    for (std::uint32_t i = 0; i < seriesCount_; ++i) {
        LinePlotInfo& plot = plots_[i];
        float value = 0.0f;
        if (!HostOrDefault()->HistoryPoint(plot.valueId, date, value)) value = 0.0f;
        if (plot.combine == 5 && axisMaxOverride_ != kNegativeFloatMax && axisMinOverride_ != kFloatMax)
            value = LogScale(value, axisMaxOverride_, axisMinOverride_);
        // The original indexes the 16-vector table with the raw series number; the port clamps it (see GRAPHSVIEW-SERIES).
        if (plot.series >= values.size() || i >= values[plot.series].size()) continue;
        values[plot.series][i] = value * plot.scale;
    }
    for (std::uint32_t series = 0; series < values.size(); ++series) bar->SetData(values[series], series);

    const float highest = bar->GetHighestAxisValue(gz::kAxisVertical);
    float lowest = bar->GetLowestAxisValue(gz::kAxisVertical);
    if (lowest > 0.0f) lowest = 0.0f;
    float high = axisMaxOverride_ != kNegativeFloatMax ? axisMaxOverride_ : highest;
    const float low = axisMinOverride_ != kFloatMax ? axisMinOverride_ : lowest;
    float spacing = 0.0f;
    const int range = static_cast<int>(high - low);
    if (range == 0) {
        graph_->SetMajorTickDecimalPlaces(gz::kAxisVertical, 1);
        high = 1.0f;
        spacing = 0.2f;
    } else if (range < 3) {
        graph_->SetMajorTickDecimalPlaces(gz::kAxisVertical, 1);
        high = std::floor(high);
        spacing = 0.5f;
    } else {
        graph_->SetMajorTickDecimalPlaces(gz::kAxisVertical, 0);
        std::uint32_t magnitude = 1;
        int scaled = range;
        if (scaled > 99) {
            do {
                magnitude *= 10;
                const int next = (scaled + 9) / 10;
                const bool again = scaled + 9 > 999;
                scaled = next;
                if (!again) break;
            } while (true);
        }
        if (scaled >= 51) spacing = 10.0f * magnitude;
        else if (scaled >= 21) spacing = 5.0f * magnitude;
        else if (scaled >= 11) spacing = 4.0f * magnitude;
        else if (scaled >= 6) spacing = 2.0f * magnitude;
        else spacing = static_cast<float>(magnitude);
        if (axisMaxOverride_ == kNegativeFloatMax) high /= 0.95f;
    }
    SetVerticalAxis(low, high, spacing, false);
}

void cSC4GraphsViewWinProc::RefreshLineGraph(gz::cIGZLineGraph* line) {
    line->RemoveAllSeries();
    line->SetSeriesCount(seriesCount_);
    // The legend toggles decide which plots make it into the chart; the enabled ones are compacted into the first
    // series (the original's per-plot index counter only advances for an enabled plot).
    std::uint32_t series = 0;
    for (std::uint32_t i = 0; i < seriesCount_; ++i) {
        LinePlotInfo& plot = plots_[i];
        gz::cIGZWinBtn* button = nullptr;
        if (graph_->GetChildAsRecursive(kFirstSeriesButtonId + i, gz::kGZIID_cIGZWinBtn,
                                        reinterpret_cast<void**>(&button))) {
            const bool enabled = button->GetBtnFlag(1);
            button->Release();
            plot.enabled = enabled;
            legendState_[plot.valueId] = enabled;
        } else {
            plot.enabled = true;
        }
        if (!plot.enabled) continue;
        gz::LineGraphSeriesInfo info;
        info.symbolUse = false;
        info.symbol = 'A';
        info.symbolSize = 12;
        info.lineUse = true;
        info.color = MakeColor(plot.red, plot.green, plot.blue);
        info.thickness = 1;
        line->SetSeriesInfo(info, series++);
    }

    const std::uint32_t index = timeScale_ < 6 ? timeScale_ : 5;   // the original reads past the table
    const TimeScaleInfo& scale = kTimeScaleTable[index];
    cGZDate step(HostOrDefault()->SimulatorDayNumber() - 4);
    const std::uint32_t endDay = step.DayNumber();
    step.Set(endDay - endDay % scale.periodDays);                       // round down to a whole period
    const std::uint32_t roundedEnd = step.DayNumber();
    step.Set(roundedEnd - (scale.points + 1) * scale.periodDays);       // the first sample's predecessor
    cGZDate simStart;
    HostOrDefault()->SimulatorStartDate(simStart);

    for (std::uint32_t i = 0; i <= scale.points; ++i) {
        cGZDate next(step.DayNumber() + scale.periodDays);   // the sample date
        cGZDate previous(step.DayNumber() - 1);              // built by the original, never read
        (void)previous;
        const bool beforeStart = step <= simStart;
        std::uint32_t seriesIndex = 0;
        for (std::uint32_t p = 0; p < seriesCount_; ++p) {
            LinePlotInfo& plot = plots_[p];
            if (!plot.enabled) continue;
            float value = 0.0f, operand = 0.0f;
            if (!beforeStart) {
                if (!HostOrDefault()->HistoryPoint(plot.valueId, next, value)) value = 0.0f;
                if (plot.combine != 0) {
                    if (!HostOrDefault()->HistoryPoint(plot.operandId, next, operand)) operand = 0.0f;
                }
            }
            if (plot.combine != 0) {
                switch (plot.combine) {
                case 1: value = value + operand; break;
                case 2: value = value - operand; break;
                case 3: value = value * operand; break;
                case 4: value = std::fabs(operand) > 1e-07f ? value / operand : 0.0f; break;
                default: break;
                }
            }
            value *= plot.scale;
            line->InsertData(value, seriesIndex, 0xffffffffu);
            ++seriesIndex;
        }
        step = next;
    }

    line->RemoveCategoryNames();
    line->InsertCategoryName(cSLString(kStringGroup, scale.caption + 1, true), 0);
    line->InsertCategoryName(cSLString(kStringGroup, kAxisCaptionOffset, true), scale.points);   // "Now" under the last sample

    const float highest = line->GetHighestAxisValue(gz::kAxisVertical);
    float lowest = line->GetLowestAxisValue(gz::kAxisVertical);
    if (lowest > 0.0f) lowest = 0.0f;
    float high = axisMaxOverride_ != kNegativeFloatMax ? axisMaxOverride_ : highest;
    const float low = axisMinOverride_ != kFloatMax ? axisMinOverride_ : lowest;
    float spacing = 0.0f;
    const int range = static_cast<int>(high - low);
    if (range == 0) {
        graph_->SetMajorTickDecimalPlaces(gz::kAxisVertical, 1);
        high = 1.0f;
        spacing = 0.2f;
    } else if (range < 3) {
        graph_->SetMajorTickDecimalPlaces(gz::kAxisVertical, 1);
        high = std::floor(high);
        spacing = 0.5f;
    } else {
        graph_->SetMajorTickDecimalPlaces(gz::kAxisVertical, 0);
        std::uint32_t magnitude = 1;
        int scaled = range;
        if (scaled > 99) {
            do {
                magnitude *= 10;
                const int next = (scaled + 9) / 10;
                const bool again = scaled + 9 > 999;
                scaled = next;
                if (!again) break;
            } while (true);
        }
        if (scaled >= 51) spacing = 10.0f * magnitude;
        else if (scaled >= 21) spacing = 5.0f * magnitude;
        else if (scaled >= 11) spacing = 4.0f * magnitude;
        else if (scaled >= 6) spacing = 2.0f * magnitude;
        else spacing = static_cast<float>(magnitude);
        if (axisMaxOverride_ == kNegativeFloatMax) high /= 0.95f;
    }
    SetVerticalAxis(low, high, spacing, true);
}

// ---- show / hide ---------------------------------------------------------------------------------------------

void cSC4GraphsViewWinProc::ShowWindow() {   // @ 0x1002e2a7a
    ShowGraph(currentOption_);
    SelectOption(currentOption_);
    visible_ = true;
    if (window_) window_->ShowWindow();
    cIGZWin* panel = expanded_ ? expandedPanel_ : collapsedPanel_;
    if (panel) panel->ShowWindow();
}

void cSC4GraphsViewWinProc::HideWindow() {   // @ 0x1002e2ad2
    if (window_) {
        ShowGraph(0);   // instance 0 does not exist: this drops the graph control
        window_->HideWindow();
        visible_ = false;
    }
    if (expandedPanel_) expandedPanel_->HideWindow();
    if (collapsedPanel_) collapsedPanel_->HideWindow();
}

void cSC4GraphsViewWinProc::ShowGraphType(std::uint32_t option) {   // @ 0x1002e2b2e
    if (currentOption_ != option) {
        SelectOption(option);
        ShowGraph(option);
        currentOption_ = option;
    }
    ShowWindow();
    if (collapsedPanel_) collapsedPanel_->HideWindow();
    if (expandedPanel_) expandedPanel_->ShowWindow();
    expanded_ = true;
}

bool cSC4GraphsViewWinProc::IsVisible() { return visible_; }   // @ 0x1002e2b86

}  // namespace sc4
