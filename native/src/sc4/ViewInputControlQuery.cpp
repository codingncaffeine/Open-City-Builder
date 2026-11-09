#include "sc4/ViewInputControlQuery.h"

#include <cmath>
#include <cstring>

#include "gz/StreamOps.h"
#include "gz/Win.h"
#include "gz/WinBMP.h"
#include "gz/WinGen.h"
#include "gz/WinSlider.h"
#include "gz/WinText.h"
#include "gz/WinTextEdit.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZPoint.h"
#include "rz/cRZVariant.h"
#include "sc4/SCProperty.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/SC4Utilities.h"

namespace sc4 {
namespace {

IQueryHost* g_queryHost = nullptr;

// The three tip strings the original keeps as file-scope cRZString globals (gsQueryString, gsQueryString2, gsQueryString3).
cRZString gsQueryString1;
cRZString gsQueryString2;
cRZString gsQueryString3;

// Message types the tool listens to (registered in Init, removed in Shutdown).
constexpr std::uint32_t kMsgTick = 0xa456d95;           // refresh the mouse tip
constexpr std::uint32_t kMsgCoverage = 0xa456d96;       // recompute the coverage rings
constexpr std::uint32_t kMsgDisableTip = 0x26d31ec2;    // the view left query mode
constexpr std::uint32_t kMsgMouseMove = 0xa6b79602;     // the window manager forwarded a mouse move
constexpr std::uint32_t kMsgDialogTick = 0x66956814;    // periodic dialog row refresh
constexpr std::uint32_t kMsgUpdatePaths = 0x69247dc7;   // routes changed
constexpr std::uint32_t kMsgAutomatonPick = 0x2acd2cf3; // click that missed an automaton: send it to the automaton code
constexpr std::uint32_t kTipOwner = 0x2a456cb1;         // the view's tool-tip id for the query tool
constexpr std::uint32_t kPropQueryUi = 0x2a499f85;      // an occupant's query dialog (UI script instance)
constexpr std::uint32_t kQueryDialogId = 0x10000005;    // the query dialogs' window id
constexpr std::uint32_t kEnumRowIidDialog = 0x22ba0121; // EnumChildren argument of the dialog passes
constexpr std::uint32_t kSoundNothingToQuery = 0xaa62c9d5;

// iids the tool queries
constexpr std::uint32_t kIID_cISC4Automaton = 0xa9b40f05;
constexpr std::uint32_t kIID_cISC4PropOccupant = 0xe9793a65;
constexpr std::uint32_t kIID_cISC4BuildingOccupant = 0x87dda3a9;

// String instances (group 0xea5524eb).
constexpr std::uint32_t kStringGroup = 0xea5524eb;
constexpr std::uint32_t kStrYes = 0xca5d4f33;
constexpr std::uint32_t kStrNo = 0x6a5d4f37;
constexpr std::uint32_t kStrSimoleon = 0xa7c204;
constexpr std::uint32_t kStrRatingBad = 0x4a5fe266;
constexpr std::uint32_t kStrRatingFair = 0xa5fe260;
constexpr std::uint32_t kStrRatingGood = 0xaa5fe249;
constexpr std::uint32_t kStrAbandoned[7] = {0, 0xcc06a5de, 0xcc06a5df, 0xcc06a5e0,
                                            0xcc06a5e1, 0, 0xcc06a5e2};

// Properties the tool reads off occupants and lots.
constexpr std::uint32_t kPropHighlighted = 0x6a95e503;   // bool: occupant takes part in the highlight sweep
constexpr std::uint32_t kPropQuerySound = 0xaa1dd397;    // the default query sound of an occupant
constexpr std::uint32_t kPropLotQuerySound = 0xa902434;  // sound when the lot state is 4 (abandoned / derelict?)
constexpr std::uint32_t kPropBuildingQuerySound = 0xaa905ab9;   // sound for lot states 2 and 3

template <typename T>
T* ChildAs(cIGZWin* win, std::uint32_t id, std::uint32_t iid) {
    if (!win) return nullptr;
    void* out = nullptr;
    if (!win->GetChildAsRecursive(id, iid, &out)) return nullptr;
    return static_cast<T*>(out);
}

void HideWindow(cIGZWin* win) {
    if (win) win->HideWindow();
}
void ShowWindow(cIGZWin* win) {
    if (win) win->ShowWindow();
}

// nsGZWinUtils::SetWindowVisible (@ 0x1006a1f74): show or hide a child and re-layout.
void SetWindowVisible(cIGZWin* parent, std::uint32_t id, bool visible) {
    cIGZWin* child = ChildAs<cIGZWin>(parent, id, kGZIID_cIGZWin);
    if (!child) return;
    if (visible) child->ShowWindow();
    else child->HideWindow();
    child->Release();
}

std::uint32_t StringLength(const cIGZString* s) { return s ? s->Strlen() : 0; }

}  // namespace

IQueryHost* QueryHost() { return g_queryHost; }
void SetQueryHost(IQueryHost* host) { g_queryHost = host; }

cIGZWin* cSC4ViewInputControlQuery::sQueryDialog_ = nullptr;
cIGZWin::EnumChildFn cSC4ViewInputControlQuery::sEnumSetFunction_ = nullptr;
void* cSC4ViewInputControlQuery::sEnumContext1_ = nullptr;
void* cSC4ViewInputControlQuery::sEnumContext2_ = nullptr;

cIGZWin* cSC4ViewInputControlQuery::QueryDialogWindow() { return sQueryDialog_; }
void cSC4ViewInputControlQuery::SetQueryDialogWindow(cIGZWin* window) { sQueryDialog_ = window; }
cIGZWin::EnumChildFn cSC4ViewInputControlQuery::EnumSetFunction() { return sEnumSetFunction_; }
void cSC4ViewInputControlQuery::SetEnumSetFunction(cIGZWin::EnumChildFn fn, void* context1, void* context2) {
    sEnumSetFunction_ = fn;
    sEnumContext1_ = context1;
    sEnumContext2_ = context2;
}

// @ 0x1002c1ec2
cSC4ViewInputControlQuery::cSC4ViewInputControlQuery(EQueryType type, std::uint32_t tipPeriod) {
    queryType_ = type;
    tipPeriod_ = tipPeriod;
    tipEnabled_ = true;
    showCoordinates_ = false;
    unknownD8_ = 3;
    pathView_ = nullptr;
    occupant_ = nullptr;
    callout_ = nullptr;
    legendMinimized_ = nullptr;
    legendExpanded_ = nullptr;
    occupantHighlight_ = 0;
    pickX_ = pickY_ = pickZ_ = 0.0f;
    hoverValid_ = false;
    mouseDown_ = false;
}

// @ 0x1002dc524 (the deleting variant at 0x1002dbdc8 only adds the delete)
cSC4ViewInputControlQuery::~cSC4ViewInputControlQuery() {
    if (pathView_) { pathView_->Release(); pathView_ = nullptr; }
    // Quirk kept: the original releases the hovered occupant (+0x48) only when the hover moves, never here.
    if (legendExpanded_) { legendExpanded_->Release(); legendExpanded_ = nullptr; }
    if (legendMinimized_) { legendMinimized_->Release(); legendMinimized_ = nullptr; }
    if (callout_) { callout_->Release(); callout_ = nullptr; }
}

std::uint32_t cSC4ViewInputControlQuery::AddRef() { return ++refCount_; }

std::uint32_t cSC4ViewInputControlQuery::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();
    return 0;
}

bool cSC4ViewInputControlQuery::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cSC4ViewInputControlQuery || iid == kGZIID_cIGZUnknown) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cISC4ViewInputControl) {
        *out = static_cast<cSC4ViewInputControl*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// The message-target sub-object (this + 0x40 in the original) has its own vtable; these four thunks forward to the
// primary object.
std::uint32_t cSC4ViewInputControlQuery::AddRefFromMessageTarget() { return AddRef(); }
std::uint32_t cSC4ViewInputControlQuery::ReleaseFromMessageTarget() { return Release(); }
bool cSC4ViewInputControlQuery::QueryInterfaceFromMessageTarget(std::uint32_t iid, void** out) {
    return QueryInterface(iid, out);
}
bool cSC4ViewInputControlQuery::DoMessageFromMessageTarget(cIGZMessage2* message) { return DoMessage(message); }

// @ 0x1002c2078
bool cSC4ViewInputControlQuery::Init() {
    if (initialized_) return true;
    const bool network = queryType_ == kQueryNetwork;
    id_ = network ? (0xc7af928e | 1) : 0xc7af928e;
    cursorId_ = network ? 0xcb8d5b54 : 0x816d7f74;
    cSC4ViewInputControl::Init();
    IQueryHost* host = QueryHost();
    if (!host) return true;
    if (cIGZMessageServer2* messages = host->MessageServer()) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        messages->AddNotification(target, kMsgTick);
        messages->AddNotification(target, kMsgCoverage);
        messages->AddNotification(target, kMsgDisableTip);
        messages->AddNotification(target, kMsgMouseMove);
        messages->AddNotification(target, kMsgDialogTick);
        messages->AddNotification(target, kMsgUpdatePaths);
    }
    tipTimer_.SetCallbackFunction(&cSC4ViewInputControlQuery::MouseTipCallback, this);
    tipEnabled_ = true;
    if (IQueryView* view = host->QueryView()) showCoordinates_ = view->ShowCoordinates();
    if (cISC4WinCalloutBox* callout = host->CreateCalloutBox()) {
        callout->SetTextFontStyle(0xa85f1a83);
        callout->SetTitleFontStyle(0x0a52f5c6);
        cRZRect avoid;
        avoid.left = -40;
        avoid.top = -40;
        avoid.right = 40;
        avoid.bottom = 40;
        callout->SetAvoidanceRect(avoid);
        HideWindow(callout->AsIGZWin());
        if (cIGZWin* viewWin = ViewWindow()) viewWin->ChildAdd(callout->AsIGZWin());
        callout_ = callout;
    }
    if (network) CreateLegendWindows();
    return true;
}

cIGZWin* cSC4ViewInputControlQuery::ViewWindow() {
    IQueryHost* host = QueryHost();
    if (!host) return nullptr;
    IQueryView* view = host->QueryView();
    return view ? view->ViewWindow() : nullptr;
}

// The network-mode legend: one script window with the minimised and the expanded legend children.
void cSC4ViewInputControlQuery::CreateLegendWindows() {
    IQueryHost* host = QueryHost();
    cIGZWin* viewWin = ViewWindow();
    if (!host || !viewWin) return;
    cIGZWin* created = host->CreateWindowFromScript(kLegendInstance, viewWin, kLegendWindowId);
    if (!created) return;
    cIGZWin* minimised = ChildAs<cIGZWin>(created, kLegendMinimizedChild, kGZIID_cIGZWin);
    cIGZWin* expanded = ChildAs<cIGZWin>(created, kLegendExpandedChild, kGZIID_cIGZWin);
    if (minimised) {
        created->ChildRemove(minimised);
        viewWin->ChildAdd(minimised);
        minimised->GZWinMoveTo(viewWin->GetW() - 2 - minimised->GetW(), 2);
        host->AttachLegendWinProc(minimised);
        minimised->PullToFront();
        minimised->ShowWindow();
        legendMinimized_ = minimised;
    }
    if (expanded) {
        created->ChildRemove(expanded);
        viewWin->ChildAdd(expanded);
        const int x = legendMinimized_ ? legendMinimized_->GetR() - expanded->GetW() : 2;
        const int y = legendMinimized_ ? legendMinimized_->GetT() : 2;
        expanded->GZWinMoveTo(x, y);
        host->AttachLegendWinProc(expanded);
        expanded->PullToFront();
        expanded->HideWindow();
        legendExpanded_ = expanded;
    }
    viewWin->ChildAdd(created);
    created->Release();
}

// @ 0x1002c2642
bool cSC4ViewInputControlQuery::Shutdown() {
    if (!initialized_) return true;
    IQueryHost* host = QueryHost();
    if (IQueryView* view = host ? host->QueryView() : nullptr) view->ClearTip(kTipOwner);
    if (callout_) {
        if (cIGZWin* viewWin = ViewWindow()) viewWin->ChildDelete(callout_->AsIGZWin());
        callout_->Release();
        callout_ = nullptr;
    }
    if (legendMinimized_) {
        if (host) host->DetachWinProc(legendMinimized_);
        if (cIGZWin* viewWin = ViewWindow()) viewWin->ChildDelete(legendMinimized_);
        legendMinimized_->Release();
        legendMinimized_ = nullptr;
    }
    if (legendExpanded_) {
        if (host) host->DetachWinProc(legendExpanded_);
        if (cIGZWin* viewWin = ViewWindow()) viewWin->ChildDelete(legendExpanded_);
        legendExpanded_->Release();
        legendExpanded_ = nullptr;
    }
    if (pathView_) {
        if (host && host->Renderer()) host->Renderer()->RemoveLayer(pathView_);
        pathView_->ShutdownView();
        pathView_->Release();
        pathView_ = nullptr;
    }
    if (occupant_) {
        occupant_->SetHighlight(static_cast<int>(occupantHighlight_), false);
        SetHighlightState(0);
        occupantHighlight_ = 0;
        occupant_->Release();
        occupant_ = nullptr;
    }
    if (cIGZMessageServer2* messages = host ? host->MessageServer() : nullptr) {
        cIGZMessageTarget2* target = static_cast<cIGZMessageTarget2*>(this);
        messages->RemoveNotification(target, kMsgTick);
        messages->RemoveNotification(target, kMsgCoverage);
        messages->RemoveNotification(target, kMsgDisableTip);
        messages->RemoveNotification(target, kMsgMouseMove);
        messages->RemoveNotification(target, kMsgDialogTick);
        messages->RemoveNotification(target, kMsgUpdatePaths);
    }
    if (tipTimer_.IsRunning()) {
        tipEnabled_ = false;
        tipTimer_.Stop();
    }
    cSC4ViewInputControl::Shutdown();
    return true;
}

// @ 0x1002c28f6: highlight every occupant of the hovered lot that carries the highlight property, then the occupant
// itself (the original calls SetHighlight(state, false) on each lot occupant with property 0x6a95e503 set).
void cSC4ViewInputControlQuery::SetHighlightState(int state) {
    if (!occupant_) return;
    IQueryHost* host = QueryHost();
    IQueryLotManager* lots = host ? host->LotManager() : nullptr;
    if (!lots) return;
    IQueryLot* lot = lots->GetLotForOccupant(occupant_);
    if (!lot) return;
    lot->AddRef();
    std::vector<cISC4Occupant*> occupants;
    lot->GetOccupants(occupants);
    for (cISC4Occupant* o : occupants) {
        if (!o) continue;
        bool highlighted = false;
        cISCPropertyHolder* holder = o->AsPropertyHolder();
        if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropHighlighted, highlighted) && highlighted) {
            o->SetHighlight(state, false);
        }
    }
    lot->Release();
}

// @ 0x1002c1dca
void cSC4ViewInputControlQuery::UpdatePaths() {
    if (!pathView_) return;
    pathView_->ClearPaths();
    ShowAllRoutesThatIntersectCityRect(cityCellRect_);
}

// @ 0x1002c1dfe
void cSC4ViewInputControlQuery::SetLegendMinimized(bool minimized) {
    if (!legendMinimized_ || !legendExpanded_) return;
    if (minimized) {
        legendMinimized_->HideWindow();
        legendExpanded_->HideWindow();
        legendMinimized_->GZWinMoveTo(legendExpanded_->GetR() - legendMinimized_->GetW(), legendExpanded_->GetT());
    } else {
        legendExpanded_->ShowWindow();
        legendExpanded_->GZWinMoveTo(legendMinimized_->GetR() - legendExpanded_->GetW(), legendMinimized_->GetT());
        legendMinimized_->ShowWindow();
    }
}

// @ 0x1002c2596
void cSC4ViewInputControlQuery::MouseTipCallback(void* self) {
    auto* tool = static_cast<cSC4ViewInputControlQuery*>(self);
    IQueryHost* host = QueryHost();
    if (!host || !tool) return;
    cIGZMessageServer2* messages = host->MessageServer();
    if (!messages) return;
    // The server keeps the posted message by reference until it delivers it, so it lives on the heap (as the original).
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMsgTick);
    messages->GeneralMessagePostToTarget(message, static_cast<cIGZMessageTarget2*>(tool));
    message->Release();
}

// @ 0x1002d4eb0
void cSC4ViewInputControlQuery::CoverageRingCallback(void* self) {
    auto* tool = static_cast<cSC4ViewInputControlQuery*>(self);
    IQueryHost* host = QueryHost();
    if (!host || !tool) return;
    cIGZMessageServer2* messages = host->MessageServer();
    if (!messages) return;
    auto* message = new cRZMessage2Standard();   // heap: the queue holds it until delivery
    message->AddRef();
    message->SetType(kMsgCoverage);
    messages->MessagePost(message, false);
    message->Release();
}

// @ 0x1002c2a0e
bool cSC4ViewInputControlQuery::DoMessage(cIGZMessage2* message) {
    IQueryHost* host = QueryHost();
    if (!message || !host) return false;
    // The original compares the message type as a SIGNED 32-bit int (0xa6b79602 is negative).
    const std::int32_t type = static_cast<std::int32_t>(message->GetType());
    if (type > 0x26d31ec1) {
        if (type == kMsgUpdatePaths) {
            UpdatePaths();
            return false;
        }
        if (type == kMsgDialogTick) {
            IQuerySimulator* sim = host->Simulator();
            if (!sim || sim->FrameCount() % 10 != 0) return false;
            cIGZWin* dialog = sQueryDialog_;
            if (!dialog || !sEnumSetFunction_) return false;
            if (sEnumContext1_) dialog->EnumChildren(kGZIID_cIGZWin, sEnumSetFunction_, sEnumContext1_);
            if (!sEnumContext2_) return false;
            dialog->EnumChildren(kGZIID_cIGZWin, sEnumSetFunction_, sEnumContext2_);
            return false;
        }
        if (type == kMsgDisableTip) {
            tipEnabled_ = false;
            return false;
        }
        return false;
    }
    if (type == kMsgMouseMove) {
        if (cIGZWinMgr* wm = ViewInputHost() ? ViewInputHost()->WindowManager() : nullptr) {
            int x = 0, y = 0;
            wm->GetMousePosition(x, y);
            OnMouseMove(x, y, 0);
        }
        return false;
    }
    if (type == kMsgCoverage) {
        if (IQueryView* view = host->QueryView()) view->RecalcCoverage();
        return false;
    }
    if (type != kMsgTick) return false;
    if (!tipEnabled_) return false;

    cRZString title, text;
    std::uint32_t icon = 0;
    std::uint32_t meter = 0;
    float meterValue = 0.5f;
    if (!occupant_) {
        if (hoverValid_) {
            GetTerrainSummaryInfo(title, text);
        } else if (!showCoordinates_) {
            return false;
        }
    } else {
        const std::uint32_t occupantType = occupant_->GetType();
        if (queryType_ == kQueryNetwork) {
            if (occupantType == 0xc772bf98) GetNetworkOccupantTrafficInfo(title, text);
            else if (occupantType == 0x278128a0) GetBuildingOccupantTrafficInfo(title, text, icon);
        } else {
            if (occupantType == 0xa823821e) GetPropOccupantSummaryInfo(title);
            else if (occupantType == 0xc772bf98) GetNetworkOccupantSummaryInfo(title, text);
            else if (occupantType == 0x278128a0) GetBuildingOccupantTipInfo(title, text, icon, meter, meterValue);
            else if (occupantType == 0x2890d4de) { host->LoadString(kStringGroup, 0x0a5bb9fd, title); }
            else if (occupantType == 0x6a11bd2b) { host->LoadString(kStringGroup, 0x4a5bba21, title); }
            else if (occupantType == 0x74758926) GetFloraOccupantSummaryInfo(title);
        }
    }
    if (showCoordinates_) {
        if (StringLength(&title) == 0) title.AssignFromCString("Terrain");
        cRZString coords;
        coords.Sprintf("x=%.1f y=%.1f z=%.1f\ncell x=%d cell z=%d", pickX_, pickY_, pickZ_,
                       static_cast<int>(pickX_ * 0.0625f), static_cast<int>(pickZ_ * 0.0625f));
        text.AppendString(coords);
    }
    if (!callout_ || icon == 0) {
        HideWindow(callout_ ? callout_->AsIGZWin() : nullptr);
        if (IQueryView* view = host->QueryView()) view->SetTip(kTipOwner, 0, &title, &text, 0);
    } else {
        // PARTIAL: the original also feeds the icon buffer (cSLBuffer from resource 0x856ddbac / 0x46a006b0) and its
        // meter into cISC4WinCalloutBox::SetIconMeter; the buffer path is not recovered.
        (void)meter;
        (void)meterValue;
        SetTip(&title, &text);
        if (IQueryView* view = host->QueryView()) view->ClearTip(kTipOwner);
    }
    return false;
}

// @ 0x1002c5b68 (title, text; either may be null or empty)
void cSC4ViewInputControlQuery::SetTip(const cIGZString* title, const cIGZString* text) {
    if (StringLength(title) != 0 || StringLength(text) != 0) {
        if (!callout_) return;
        cRZString empty;
        callout_->SetTitle(title ? *title : empty);
        callout_->SetText(text ? *text : empty);
        cIGZWin* win = callout_->AsIGZWin();
        if (!win->IsVisible()) {
            if (cIGZWinMgr* wm = ViewInputHost() ? ViewInputHost()->WindowManager() : nullptr) {
                int x = 0, y = 0;
                wm->GetCursorPos(ViewWindow(), x, y);
                cRZPoint point;
                point.x = x;
                point.y = y;
                callout_->SetPointOfInterest(point);
            }
            ShowWindow(win);
        }
        return;
    }
    IQueryHost* host = QueryHost();
    if (host && host->QueryView()) host->QueryView()->ClearTip(kTipOwner);
    HideWindow(callout_ ? callout_->AsIGZWin() : nullptr);
}

// @ 0x1002c5ca0
bool cSC4ViewInputControlQuery::OnKeyDown(std::uint32_t, std::uint32_t) { return false; }

// @ 0x1002d4ea8
bool cSC4ViewInputControlQuery::ShouldStack() { return false; }

// @ 0x1002cf312: the debug query needs the SC4 debug flag and the three debug keys (shift, control, alt) held.
bool cSC4ViewInputControlQuery::IsDebugQueryEnabled() {
    IQueryHost* host = QueryHost();
    if (!host || !host->IsDebugMode()) return false;
    return host->IsKeyDown(0x10) && host->IsKeyDown(0x11) && host->IsKeyDown(0x12);
}

// @ 0x1002cdd7e
bool cSC4ViewInputControlQuery::PickQueriableOccupant(int x, int y, cISC4Occupant** out, IOccupantFilter* filter) {
    *out = nullptr;
    IQueryHost* host = QueryHost();
    IQueryRenderer* renderer = host ? host->Renderer() : nullptr;
    if (!renderer) return false;
    IPickResult* result = renderer->Pick(x, y, filter);
    if (!result) return false;
    bool found = false;
    if (cIGZUnknown* object = result->GetObject()) {
        void* occupant = nullptr;
        if (object->QueryInterface(kGZIID_cISC4Occupant, &occupant)) {
            *out = static_cast<cISC4Occupant*>(occupant);
            found = true;
        }
    }
    result->Release();
    return found;
}

// @ 0x1002d45fc / 0x1002d470a (same body, different filter and interface)
bool cSC4ViewInputControlQuery::PickAutomaton(int x, int y, cIGZUnknown** out) {
    *out = nullptr;
    IQueryHost* host = QueryHost();
    IQueryRenderer* renderer = host ? host->Renderer() : nullptr;
    if (!renderer) return false;
    IOccupantFilter* filter = renderer->CreateFilter(2);   // cSC4AutomatonFilter
    if (!filter) return false;
    bool found = false;
    if (IPickResult* result = renderer->Pick(x, y, filter)) {
        if (cIGZUnknown* object = result->GetObject()) {
            void* automaton = nullptr;
            if (object->QueryInterface(kIID_cISC4Automaton, &automaton)) {
                *out = static_cast<cIGZUnknown*>(automaton);
                found = true;
            }
        }
        result->Release();
    }
    filter->Release();
    return found;
}

bool cSC4ViewInputControlQuery::PickStoplightProp(int x, int y, cIGZUnknown** out) {
    *out = nullptr;
    IQueryHost* host = QueryHost();
    IQueryRenderer* renderer = host ? host->Renderer() : nullptr;
    if (!renderer) return false;
    IOccupantFilter* filter = renderer->CreateFilter(3);   // cStoplightPropFilter
    if (!filter) return false;
    bool found = false;
    if (IPickResult* result = renderer->Pick(x, y, filter)) {
        if (cIGZUnknown* object = result->GetObject()) {
            void* prop = nullptr;
            if (object->QueryInterface(kIID_cISC4PropOccupant, &prop)) {
                *out = static_cast<cIGZUnknown*>(prop);
                found = true;
            }
        }
        result->Release();
    }
    filter->Release();
    return found;
}

// @ 0x1002d4342
bool cSC4ViewInputControlQuery::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    IQueryHost* host = QueryHost();
    tipTimer_.Stop();
    if (IQueryView* view = host ? host->QueryView() : nullptr) view->ClearTip(kTipOwner);
    HideWindow(callout_ ? callout_->AsIGZWin() : nullptr);
    cIGZUnknown* automaton = nullptr;
    if (modifiers & 1) {
        if (PickAutomaton(x, y, &automaton)) {
            if (host->City()) host->City()->FollowAutomaton(automaton);
            automaton->Release();
        } else if (cIGZMessageServer2* messages = host->MessageServer()) {
            cRZMessage2Standard message;
            message.SetType(kMsgAutomatonPick);
            message.SetData1(static_cast<std::uint64_t>(x));
            message.SetData2(static_cast<std::uint64_t>(y));
            messages->MessageSend(&message);
        }
        cIGZUnknown* prop = nullptr;
        if (PickStoplightProp(x, y, &prop)) {
            if (host->City()) host->City()->SelectStoplightProp(prop);
            prop->Release();
        }
    }
    if (modifiers & 2) {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        if (IQueryView* view = host->QueryView()) view->PickTerrain(x, y, pos, false);
        int cellX = 0, cellZ = 0;
        if (host->City()) host->City()->PositionToCell(pos[0], pos[2], cellX, cellZ);
        if (host->ResidentialSim()) host->ResidentialSim()->SetQueryCell(cellX >> 2, cellZ >> 2);
    }
    mouseDown_ = true;
    if (queryType_ == kQueryNetwork) {
        GetOccupantPaths();
    } else {
        mouseDown_ = false;
        DescribePick(x, y, gsQueryString1, gsQueryString2, gsQueryString3, IsDebugQueryEnabled());
    }
    SetCapture();
    return true;
}

// @ 0x1002d4818
bool cSC4ViewInputControlQuery::OnMouseUpL(int, int, std::uint32_t) {
    IQueryHost* host = QueryHost();
    tipTimer_.Stop();
    if (IQueryView* view = host ? host->QueryView() : nullptr) view->ClearTip(kTipOwner);
    HideWindow(callout_ ? callout_->AsIGZWin() : nullptr);
    mouseDown_ = false;
    return true;
}

// @ 0x1002d4e2a
bool cSC4ViewInputControlQuery::OnMouseExit() {
    IQueryHost* host = QueryHost();
    tipTimer_.Stop();
    if (IQueryView* view = host ? host->QueryView() : nullptr) view->ClearTip(kTipOwner);
    HideWindow(callout_ ? callout_->AsIGZWin() : nullptr);
    if (occupant_) {
        occupant_->SetHighlight(static_cast<int>(occupantHighlight_), false);
        SetHighlightState(0);
        occupant_->Release();
        occupant_ = nullptr;
    }
    hoverValid_ = false;
    return true;
}

// @ 0x1002d4862
bool cSC4ViewInputControlQuery::OnMouseMove(int x, int y, std::uint32_t modifiers) {
    (void)modifiers;
    IQueryHost* host = QueryHost();
    hoverValid_ = false;
    if (!host || !view3D_) return true;
    cISC4Occupant* picked = nullptr;
    IOccupantFilter* filter = nullptr;
    if (IQueryRenderer* renderer = host->Renderer()) filter = renderer->CreateFilter(queryType_ == kQueryNetwork ? 0 : 1);
    if (filter && PickQueriableOccupant(x, y, &picked, filter)) {
        hoverValid_ = true;
        // A building occupant is replaced by the occupant of its lot.
        bool highlighted = false;
        cISCPropertyHolder* holder = picked->AsPropertyHolder();
        if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropHighlighted, highlighted) && highlighted) {
            if (IQueryLot* lot = host->LotManager() ? host->LotManager()->GetLotForOccupant(picked) : nullptr) {
                lot->AddRef();
                std::vector<cISC4Occupant*> occupants;
                lot->GetOccupants(occupants);
                if (!occupants.empty() && occupants.front()) {
                    cISC4Occupant* lotOccupant = occupants.front();
                    lotOccupant->AddRef();
                    picked->Release();
                    picked = lotOccupant;
                }
                lot->Release();
            }
        }
    }
    float pos[3] = {0.0f, 0.0f, 0.0f};
    if (!hoverValid_) {
        IQueryView* view = host->QueryView();
        if (view && view->PickTerrain(x, y, pos, false)) {
            int cellX = 0, cellZ = 0;
            if (host->City()) host->City()->PositionToCell(pos[0], pos[2], cellX, cellZ);
            if (IQueryLot* lot = host->LotManager() ? host->LotManager()->GetLotAtCell(cellX, cellZ, 0) : nullptr) {
                lot->AddRef();
                std::vector<cISC4Occupant*> occupants;
                lot->GetOccupants(occupants);
                cISC4Occupant* candidate = occupants.empty() ? nullptr : occupants.front();
                if (queryType_ == kQueryNetwork) {
                    if (candidate && candidate->IsOccupantGroup(0x1006)) {
                        candidate->AddRef();
                        picked = candidate;
                        hoverValid_ = true;
                    }
                } else if (candidate) {
                    candidate->AddRef();
                    picked = candidate;
                    hoverValid_ = true;
                }
                lot->Release();
            }
            if (queryType_ != kQueryNetwork) hoverValid_ = true;
        }
        if (std::fabs(pickX_ - pos[0]) >= 0.0001f || std::fabs(pickY_ - pos[1]) >= 0.0001f ||
            std::fabs(pickZ_ - pos[2]) >= 0.0001f) {
            pickX_ = pos[0];
            pickY_ = pos[1];
            pickZ_ = pos[2];
        }
    } else if (showCoordinates_) {
        if (IQueryView* view = host->QueryView()) {
            view->PickTerrain(x, y, pos, false);
            pickX_ = pos[0];
            pickY_ = pos[1];
            pickZ_ = pos[2];
        }
    }
    if (picked != occupant_) {
        if (occupant_) {
            occupant_->SetHighlight(static_cast<int>(occupantHighlight_), false);
            SetHighlightState(0);
            occupant_->Release();
        }
        occupant_ = picked;
        if (occupant_) {
            occupantHighlight_ = occupant_->GetHighlight();
            occupant_->SetHighlight(7, false);
            SetHighlightState(7);
            occupant_->AddRef();      // the member's own reference
        } else {
            occupantHighlight_ = 0;
        }
        if (picked) picked->Release();   // drop the reference the pick handed back
    } else if (picked) {
        picked->Release();
    }
    if (!mouseDown_) {
        tipTimer_.Stop();
        tipEnabled_ = true;
        tipTimer_.StartOneShot(tipPeriod_, static_cast<cIGZUnknown*>(static_cast<cSC4ViewInputControl*>(this)));
    }
    if (callout_) {
        cIGZWin* win = callout_->AsIGZWin();
        if (win && win->IsVisible()) {
            cRZPoint point;
            point.x = x;
            point.y = y;
            callout_->SetPointOfInterest(point);
        }
    }
    if (filter) filter->Release();
    return true;
}

// @ 0x1002cde0c
void cSC4ViewInputControlQuery::PlayOccupantQuerySound(cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    IViewInputHost* base = ViewInputHost();
    if (!occupant || !host || !base) return;
    std::uint32_t sound = 0;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    IQueryLot* lot = host->LotManager() ? host->LotManager()->GetLotForOccupant(occupant) : nullptr;
    if (lot) {
        lot->AddRef();
        if (lot->GetState() == 4 && holder && cSCPropertyHelp::GetPropertyValue(holder, kPropLotQuerySound, sound)) {
            base->PlaySound(1, sound);
            lot->Release();
            return;
        }
        const int state = lot->GetState();
        if ((state == 2 || state == 3) && holder &&
            cSCPropertyHelp::GetPropertyValue(holder, kPropBuildingQuerySound, sound)) {
            base->PlaySound(1, sound);
            lot->Release();
            return;
        }
    }
    if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropQuerySound, sound)) base->PlaySound(1, sound);
    if (lot) lot->Release();
}

// @ 0x1002d4f40
bool cSC4ViewInputControlQuery::SetEditControlText(cIGZWin* win, std::uint32_t id, cIGZString& text) {
    gz::cIGZWinTextEdit* edit = ChildAs<gz::cIGZWinTextEdit>(win, id, gz::kGZIID_cIGZWinTextEdit);
    if (!edit) return false;
    edit->SetText(text, false);
    edit->ClearFlag(8);       // vslot 0x270
    edit->SetFlag(0x200);     // vslot 0x260
    edit->Release();
    return true;
}

// @ 0x1002d4fb2
bool cSC4ViewInputControlQuery::GetEditControlText(cIGZWin* win, std::uint32_t id, cIGZString& text,
                                                   std::uint32_t& length) {
    gz::cIGZWinTextEdit* edit = ChildAs<gz::cIGZWinTextEdit>(win, id, gz::kGZIID_cIGZWinTextEdit);
    if (!edit) return false;
    length = edit->GetTextLength();
    text.Resize(length);
    text.AssignFromCString(edit->GetText());
    edit->Release();
    return true;
}

// @ 0x1002d5094
bool cSC4ViewInputControlQuery::SetStaticControlText(cIGZWin* win, std::uint32_t id, const cIGZString& text) {
    gz::cIGZWinText* control = ChildAs<gz::cIGZWinText>(win, id, gz::kGZIID_cIGZWinText);
    if (!control) return false;
    control->SetCaption(text);
    control->Release();
    return true;
}

// @ 0x1002d50dc
bool cSC4ViewInputControlQuery::SetStaticControlNumeric(cIGZWin* win, std::uint32_t id, std::uint32_t value) {
    cRZString text;
    text.Sprintf("%d", static_cast<int>(value));
    return SetStaticControlText(win, id, text);
}

// @ 0x1002d5168
bool cSC4ViewInputControlQuery::SetStaticControlNumericPercent(cIGZWin* win, std::uint32_t id, std::uint32_t value) {
    cRZString text;
    text.Sprintf("%d%%", static_cast<int>(value));
    return SetStaticControlText(win, id, text);
}

// @ 0x1002d51f4
bool cSC4ViewInputControlQuery::SetStaticControlSimoleons(cIGZWin* win, std::uint32_t id, std::int64_t value) {
    cRZString text;
    SC4MakeMoneyString(value, text);
    return SetStaticControlText(win, id, text);
}

// @ 0x1002d527a: the locale-formatted amount with the simoleon symbol appended.
bool cSC4ViewInputControlQuery::SetStaticControlSimoleonsFloat(cIGZWin* win, std::uint32_t id, double value) {
    IQueryHost* host = QueryHost();
    if (!host) return false;
    cRZString text;
    host->FormatMoneyFloat(value, text);
    cRZString symbol;
    host->LoadString(kStringGroup, kStrSimoleon, symbol);
    text.AppendString(symbol);
    return SetStaticControlText(win, id, text);
}

namespace {
// The sliders take fraction * 100 rounded to the nearest even integer: the compiled code adds 1.5 * 2^23 in float and reads the
// low mantissa bits back (@ 0x1002d5e6c, 0x1002d60e4, 0x1002d7267 ...).
int RoundedPercentForSlider(float fraction) {
#pragma clang fp contract(off)   // the multiply and the add round separately on x86 (an arm64 fused multiply-add would not)
    const float shifted = fraction * 100.0f + 12582912.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &shifted, sizeof bits);
    return static_cast<int>(bits + 0xb4c00000u);
}
}  // namespace

// @ 0x1002d54a0
bool cSC4ViewInputControlQuery::SetSliderControlValue(cIGZWin* win, std::uint32_t id, int minimum, int maximum,
                                                      int value, std::int64_t money, cISC4Occupant* occupant,
                                                      tSetOrGetFn callback, std::uint32_t* array,
                                                      std::uint32_t arraySize) {
    gz::cIGZWinSlider* slider = ChildAs<gz::cIGZWinSlider>(win, id, gz::kGZIID_cIGZWinSlider);
    if (!slider) return false;
    if (minimum < maximum) {
        slider->SetMinimumValue(minimum);
        slider->SetMaximumValue(maximum);
    }
    // Quirk kept: the original sets maximum + 1 and then value, both through SetValue.
    slider->SetValue(value + 1);
    slider->SetValue(value);
    cIGZWin* sliderWin = slider->AsIGZWin();
    sliderWin->PullToFront();
    // The window keeps a counted reference to every parameter, so the variants are heap-allocated.
    cIGZVariant* existing = nullptr;
    if (sliderWin->GetParam(0x6a8cc9d6, &existing) && existing) existing->Release();
    auto* fn = new cRZVariant();
    fn->SetValVoidPtr(reinterpret_cast<void*>(callback));
    sliderWin->SetParam(0x6a8cc9d6, fn);
    auto* amount = new cRZVariant();
    amount->SetValSint64(money);
    sliderWin->SetParam(0x2aa6e8ad, amount);
    if (array) {
        auto* list = new cRZVariant();
        list->SetValVoid(array, arraySize * static_cast<std::uint32_t>(sizeof(std::uint32_t)));
        sliderWin->SetParam(0x0ab3f6db, list);
    }
    (void)occupant;
    slider->Release();
    return SetStaticControlSimoleons(
        win, id + 0x100, static_cast<std::int64_t>(static_cast<float>(money) * (static_cast<float>(value) / 100.0f) + 0.5f));
}

// @ 0x1002d5758
bool cSC4ViewInputControlQuery::GetSliderControlValue(cIGZWin* win, std::uint32_t id, int& value) {
    gz::cIGZWinSlider* slider = ChildAs<gz::cIGZWinSlider>(win, id, gz::kGZIID_cIGZWinSlider);
    if (!slider) return false;
    value = slider->GetValue();
    slider->Release();
    return true;
}

// @ 0x1002d5926
bool cSC4ViewInputControlQuery::SetPerformanceMeterValue(cIGZWin* win, std::uint32_t id, float value) {
    gz::cIGZWinBMP* meter = ChildAs<gz::cIGZWinBMP>(win, id, gz::kGZIID_cIGZWinBMP);
    if (!meter) return false;
    auto* variant = new cRZVariant();
    variant->SetValFloat32(value);
    meter->AsIGZWin()->SetParam(0xea563ab0, variant);   // the window keeps the counted reference
    meter->Release();
    return true;
}

// ---- airport / seaport rows ------------------------------------------------------------------------------------------------
// The occupant forms read the occupant's bounding city cells (vslot 0x58) and pass the first cell to the cell form; no occupant
// answers false. The cell forms look the lot up (lot manager vslot 0x28, flag 0) and ask the port developer.
namespace {
bool PortCellOfOccupant(cISC4Occupant* occupant, int& x, int& z) {
    if (!occupant) return false;
    SC4CellRect cells{};
    occupant->GetBoundingCityCells(cells);
    x = cells.x0;
    z = cells.z0;
    return true;
}
IQueryLot* PortLotAt(int x, int z) {
    IQueryHost* host = QueryHost();
    return host && host->LotManager() ? host->LotManager()->GetLotAtCell(x, z, 0) : nullptr;
}
// The traffic meter: full below 80 % use, then falling linearly to empty at capacity (the compiled constants are 0.8f and
// 0.19999999f). An unanswered or zero-capacity developer leaves the meter empty.
float PortTrafficMeter(IQueryPortDeveloper* developer, IQueryLot* lot) {
    float meter = 0.0f;
    if (!lot || !developer) return meter;
    std::uint32_t usage = 0, capacity = 0;
    if (!developer->GetUsageAndCapacity(lot, usage, capacity) || capacity == 0) return meter;
    const float capacityF = static_cast<float>(capacity);
    if (static_cast<float>(usage) <= 0.8f * capacityF) return 1.0f;
    const float free = capacity > usage ? static_cast<float>(static_cast<std::uint64_t>(capacity - usage)) : 0.0f;
    return free / (capacityF * 0.19999999f);
}
bool PortNumeric(cIGZWin* win, std::uint32_t id, bool set, IQueryPortDeveloper* developer, int x, int z, bool wantCapacity) {
    if (!set) return true;
    IQueryLot* lot = PortLotAt(x, z);
    if (!lot) return false;
    std::uint32_t usage = 0, capacity = 0;
    if (developer) developer->GetUsageAndCapacity(lot, usage, capacity);
    return cSC4ViewInputControlQuery::SetStaticControlNumeric(win, id, wantCapacity ? capacity : usage);
}
bool PortMaintenance(cIGZWin* win, std::uint32_t id, bool set, IQueryPortDeveloper* developer, int x, int z) {
    if (!set) return true;
    IQueryLot* lot = PortLotAt(x, z);
    if (!lot || !developer) return false;
    return cSC4ViewInputControlQuery::SetStaticControlSimoleons(win, id, developer->GetMaintenanceCost(lot));
}
IQueryPortDeveloper* Airport() { IQueryHost* host = QueryHost(); return host ? host->AirportDeveloper() : nullptr; }
IQueryPortDeveloper* Seaport() { IQueryHost* host = QueryHost(); return host ? host->SeaportDeveloper() : nullptr; }
}  // namespace


// @ 0x1002d655a
bool cSC4ViewInputControlQuery::SetOrGetAirportOccupantAirTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetAirportAirTrafficVolume(win, id, set, x, z);
}

// @ 0x1002d6a9e
bool cSC4ViewInputControlQuery::SetOrGetAirportOccupantMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetAirportMaintenanceCost(win, id, set, x, z);
}

// @ 0x1002d6b6e
bool cSC4ViewInputControlQuery::SetOrGetAirportOccupantUsage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetAirportUsage(win, id, set, x, z);
}

// @ 0x1002d6c4c
bool cSC4ViewInputControlQuery::SetOrGetAirportOccupantCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetAirportCapacity(win, id, set, x, z);
}

// @ 0x1002d6d2a
bool cSC4ViewInputControlQuery::SetOrGetSeaportOccupantUsage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetSeaportUsage(win, id, set, x, z);
}

// @ 0x1002d6e08
bool cSC4ViewInputControlQuery::SetOrGetSeaportOccupantCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetSeaportCapacity(win, id, set, x, z);
}

// @ 0x1002d6ee6
bool cSC4ViewInputControlQuery::SetOrGetSeaportOccupantSeaTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetSeaportSeaTrafficVolume(win, id, set, x, z);
}

// @ 0x1002d7016
bool cSC4ViewInputControlQuery::SetOrGetSeaportOccupantMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetSeaportMaintenanceCost(win, id, set, x, z);
}

// @ 0x1002d65ae
bool cSC4ViewInputControlQuery::SetOrGetAirportAirTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    if (!set) return true;
    return SetPerformanceMeterValue(win, id, PortTrafficMeter(Airport(), PortLotAt(x, z)));
}
// @ 0x1002d6f3a
bool cSC4ViewInputControlQuery::SetOrGetSeaportSeaTrafficVolume(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    if (!set) return true;
    return SetPerformanceMeterValue(win, id, PortTrafficMeter(Seaport(), PortLotAt(x, z)));
}
// @ 0x1002d6af2
bool cSC4ViewInputControlQuery::SetOrGetAirportMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    return PortMaintenance(win, id, set, Airport(), x, z);
}
// @ 0x1002d706a
bool cSC4ViewInputControlQuery::SetOrGetSeaportMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    return PortMaintenance(win, id, set, Seaport(), x, z);
}
// @ 0x1002d6bc2
bool cSC4ViewInputControlQuery::SetOrGetAirportUsage(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    return PortNumeric(win, id, set, Airport(), x, z, false);
}
// @ 0x1002d6ca0
bool cSC4ViewInputControlQuery::SetOrGetAirportCapacity(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    return PortNumeric(win, id, set, Airport(), x, z, true);
}
// @ 0x1002d6d7e
bool cSC4ViewInputControlQuery::SetOrGetSeaportUsage(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    return PortNumeric(win, id, set, Seaport(), x, z, false);
}
// @ 0x1002d6e5c
bool cSC4ViewInputControlQuery::SetOrGetSeaportCapacity(cIGZWin* win, std::uint32_t id, bool set, int x, int z) {
    return PortNumeric(win, id, set, Seaport(), x, z, true);
}

// ---- school / hospital / library rows ---------------------------------------------------------------------------------------
namespace {
IQueryResidentialSim* ResidentialSim() { IQueryHost* host = QueryHost(); return host ? host->ResidentialSim() : nullptr; }
bool GetEducationRecord(cISC4Occupant* occupant, bool hospital, QueryEducationRecord& record) {
    IQueryResidentialSim* sim = ResidentialSim();
    if (!sim) return false;
    return hospital ? sim->GetHospitalQueryData(occupant, record) : sim->GetSchoolQueryData(occupant, record);
}
std::uint32_t gSchoolSliderIds[4] = {0x101a, 0x101b, 0x101d, 0x101f};   // 0x100b05330: rows refreshed when the staff slider moves
std::uint32_t gHospitalSliderIds[4] = {0x1023, 0x1024, 0, 0};           // 0x100b05340
// The meter rows: the grade of the school or hospital, false when the simulator has no data for the occupant.
bool EducationGrade(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant, bool hospital) {
    if (!set) return true;
    QueryEducationRecord record;
    if (!GetEducationRecord(occupant, hospital, record)) return false;
    return cSC4ViewInputControlQuery::SetPerformanceMeterValue(win, id, record.grade);
}
// The count rows (students, patients, books borrowed): the record's count, printed when the simulator answered.
bool EducationCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant, bool hospital) {
    QueryEducationRecord record;
    bool answered = GetEducationRecord(occupant, hospital, record);
    if (set && answered) answered = cSC4ViewInputControlQuery::SetStaticControlNumeric(win, id, record.count);
    return answered;
}
// The capacity rows ask the simulator whether or not they print.
bool EducationCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant, bool wantSecond) {
    std::uint32_t first = 0, second = 0;
    if (IQueryResidentialSim* sim = ResidentialSim()) sim->EstimateCurrentOccupantCapacity(occupant, first, second);
    if (!set) return true;
    return cSC4ViewInputControlQuery::SetStaticControlNumeric(win, id, wantSecond ? second : first);
}
// The funding sliders: set shows the record's fraction and cost; get reads the slider back (percent * 0.01) and stores both
// fractions, the one that moved and the other from the record.
bool EducationFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant, bool hospital, bool second,
                      cSC4ViewInputControlQuery::tSetOrGetFn self, std::uint32_t* ids, std::uint32_t idCount) {
    QueryEducationRecord record;
    if (!GetEducationRecord(occupant, hospital, record)) return false;
    if (set) {
        const float fraction = second ? record.secondFunding : record.staffFunding;
        const std::int32_t cost = second ? record.secondCost : record.staffCost;
        cSC4ViewInputControlQuery::SetSliderControlValue(win, id, 1, 0, RoundedPercentForSlider(fraction),
                                                         static_cast<std::int64_t>(static_cast<std::uint32_t>(cost)), occupant, self, ids,
                                                         idCount);
        return true;
    }
    int value = 0;
    cSC4ViewInputControlQuery::GetSliderControlValue(win, id, value);
    const float moved = static_cast<float>(value) * 0.01f;
    if (IQueryResidentialSim* sim = ResidentialSim())
        sim->SetOccupantFundingPercentages(occupant, second ? record.staffFunding : moved, second ? moved : record.secondFunding,
                                           !hospital);
    return true;
}
}  // namespace

// @ 0x1002d7142
bool cSC4ViewInputControlQuery::SetOrGetSchoolGrade(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationGrade(win, id, set, occupant, false);
}
// @ 0x1002d76e8
bool cSC4ViewInputControlQuery::SetOrGetHospitalGrade(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationGrade(win, id, set, occupant, true);
}
// @ 0x1002d7530
bool cSC4ViewInputControlQuery::SetOrGetSchoolStudentCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCount(win, id, set, occupant, false);
}
// @ 0x1002d75ea (the library reads the school record's count too)
bool cSC4ViewInputControlQuery::SetOrGetLibraryBooksBorrowedCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCount(win, id, set, occupant, false);
}
// @ 0x1002d7ad0
bool cSC4ViewInputControlQuery::SetOrGetHospitalPatientCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCount(win, id, set, occupant, true);
}
// @ 0x1002d748c
bool cSC4ViewInputControlQuery::SetOrGetSchoolTeacherCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCapacity(win, id, set, occupant, true);
}
// @ 0x1002d74de
bool cSC4ViewInputControlQuery::SetOrGetSchoolDeskCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCapacity(win, id, set, occupant, false);
}
// @ 0x1002d75dc: the museum's tickets are the desk count
bool cSC4ViewInputControlQuery::SetOrGetMuseumExhibitTicketsAvailable(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetSchoolDeskCount(win, id, set, occupant);
}
// @ 0x1002d7696
bool cSC4ViewInputControlQuery::SetOrGetLibraryBooksInCollectionCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCapacity(win, id, set, occupant, false);
}
// @ 0x1002d7a2c
bool cSC4ViewInputControlQuery::SetOrGetHospitalDoctorCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCapacity(win, id, set, occupant, true);
}
// @ 0x1002d7a7e
bool cSC4ViewInputControlQuery::SetOrGetHospitalBedCount(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationCapacity(win, id, set, occupant, false);
}
// @ 0x1002d71fc
bool cSC4ViewInputControlQuery::SetOrGetSchoolLocalStaffFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationFunding(win, id, set, occupant, false, false, &SetOrGetSchoolLocalStaffFunding, gSchoolSliderIds, 4);
}
// @ 0x1002d7346
bool cSC4ViewInputControlQuery::SetOrGetSchoolLocalBusFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationFunding(win, id, set, occupant, false, true, &SetOrGetSchoolLocalBusFunding, nullptr, 0);
}
// @ 0x1002d77a2
bool cSC4ViewInputControlQuery::SetOrGetHospitalLocalStaffFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationFunding(win, id, set, occupant, true, false, &SetOrGetHospitalLocalStaffFunding, gHospitalSliderIds, 4);
}
// @ 0x1002d78e8
bool cSC4ViewInputControlQuery::SetOrGetHospitalLocalAmbulanceFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return EducationFunding(win, id, set, occupant, true, true, &SetOrGetHospitalLocalAmbulanceFunding, nullptr, 0);
}

// ---- landfill / incinerator / recycling / power rows ------------------------------------------------------------------------
namespace {
IQueryPollutionSim* PollutionSim() { IQueryHost* host = QueryHost(); return host ? host->PollutionSim() : nullptr; }
IQueryPowerSim* PowerSim() { IQueryHost* host = QueryHost(); return host ? host->PowerSim() : nullptr; }
IQueryLandfillDeveloper* Landfill() { IQueryHost* host = QueryHost(); return host ? host->LandfillDeveloper() : nullptr; }
// full up to 80 % use, then falling to empty at capacity (compiled constants 0.8f / 0.19999999f); no capacity leaves it empty
float UsageMeter(std::uint32_t usage, std::uint32_t capacity) {
    if (capacity == 0) return 0.0f;
    const float capacityF = static_cast<float>(capacity);
    if (static_cast<float>(usage) <= capacityF * 0.8f) return 1.0f;
    const float free = capacity > usage ? static_cast<float>(static_cast<std::uint64_t>(capacity - usage)) : 0.0f;
    return free / (capacityF * 0.19999999f);
}
// cvttss2si r64 then the low 32 bits: out-of-range and NaN give INT64_MIN, whose low half is 0
std::uint32_t TruncatedUnsigned(float value) {
    if (!(value > -9.2233720368547758e18f && value < 9.2233720368547758e18f)) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(value));
}
}  // namespace

// @ 0x1002d7b7c
bool cSC4ViewInputControlQuery::SetOrGetLandfillUsage(cIGZWin* win, std::uint32_t id, bool set, int, int) {
    if (!set) return true;
    std::uint32_t usage = 0, capacity = 0;
    if (IQueryLandfillDeveloper* landfill = Landfill()) landfill->GetLandfillUsage(usage, capacity);
    SetPerformanceMeterValue(win, id, UsageMeter(usage, capacity));
    return true;
}
// @ 0x1002d7c22: last month's landfilled garbage scaled by the garbage factor
bool cSC4ViewInputControlQuery::SetOrGetLandfillNetTrashThisMonth(cIGZWin* win, std::uint32_t id, bool set, int, int) {
    if (!set) return true;
    IQueryPollutionSim* pollution = PollutionSim();
    if (!pollution) return false;
    const std::int64_t landfilled = pollution->GarbageLandfilledPrevMonth();
    const float scale = pollution->GarbageScalingFactor();
    return SetStaticControlNumeric(win, id, TruncatedUnsigned(scale * static_cast<float>(landfilled)));
}

// @ 0x1002d7c92: the meter takes the efficiency as a whole number of percent
bool cSC4ViewInputControlQuery::SetOrGetIncineratorEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    float efficiency = 0.0f;
    if (IQueryPollutionSim* pollution = PollutionSim()) pollution->CurrentOccupantEfficiency(occupant, efficiency);
    return SetPerformanceMeterValue(win, id, static_cast<float>(TruncatedUnsigned(efficiency * 100.0f)));
}
// @ 0x1002d7cfe
bool cSC4ViewInputControlQuery::SetOrGetIncineratorPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    std::uint32_t used = 0, capacity = 0, percent = 0;
    if (IQueryPollutionSim* pollution = PollutionSim()) {
        pollution->IncineratorUsedCapacity(occupant, used);
        pollution->IncineratorCurrentCapacity(occupant, capacity);
    }
    if (capacity != 0) percent = (used * 100u) / capacity;
    return SetStaticControlNumericPercent(win, id, percent);
}
// @ 0x1002d7d84
bool cSC4ViewInputControlQuery::SetOrGetIncineratorTrashIncinerated(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    std::uint32_t used = 0;
    float scale = 0.0f;
    if (IQueryPollutionSim* pollution = PollutionSim()) {
        pollution->IncineratorUsedCapacity(occupant, used);
        scale = pollution->GarbageScalingFactor();
    }
    return SetStaticControlNumeric(win, id, TruncatedUnsigned(scale * static_cast<float>(used)));
}
// @ 0x1002d8378
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyTrashIncinerated(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    std::uint32_t used = 0;
    float scale = 0.0f;
    if (IQueryPollutionSim* pollution = PollutionSim()) {
        pollution->WasteToEnergyUsedCapacity(occupant, used);
        scale = pollution->GarbageScalingFactor();
    }
    return SetStaticControlNumeric(win, id, TruncatedUnsigned(scale * static_cast<float>(used)));
}

// @ 0x1002d7e5c: current over maximum capacity, at most 1
bool cSC4ViewInputControlQuery::SetOrGetRecyclingCenterEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    float maximum = 0.0f, current = 0.0f;
    if (IQueryPollutionSim* pollution = PollutionSim()) {
        pollution->RecyclingCenterMaxCapacity(occupant, maximum);
        pollution->RecyclingCenterCurrentCapacity(occupant, current);
    }
    float efficiency = 0.0f;
    if (current > 0.0f) efficiency = current / maximum;   // kSC4PercentageZero
    return SetPerformanceMeterValue(win, id, efficiency < 1.0f ? efficiency : 1.0f);
}
// @ 0x1002d7ef6: the share of the garbage that was recycled, over the centre's current capacity, as a percent
bool cSC4ViewInputControlQuery::SetOrGetRecyclingCenterPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPollutionSim* pollution = PollutionSim();
    float recycledShare = 0.0f, current = 0.0f;
    if (pollution) {
        const std::int64_t produced = pollution->GarbageProducedPrevMonth();
        const std::int64_t recycled = pollution->GarbageRecycledPrevMonth();
        if (produced != 0) recycledShare = static_cast<float>(recycled) / static_cast<float>(produced);
        pollution->RecyclingCenterCurrentCapacity(occupant, current);
    }
    float fraction = 0.0f;
    if (current > 0.0f) {
        fraction = recycledShare / current;
        if (fraction > 1.0f) fraction = 1.0f;
    }
    return SetStaticControlNumericPercent(win, id, TruncatedUnsigned(fraction * 100.0f));
}
// @ 0x1002d7fc8: the recycled garbage shared between the recycling centres
bool cSC4ViewInputControlQuery::SetOrGetRecyclingCenterTrashRecycled(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant*) {
    if (!set) return true;
    IQueryPollutionSim* pollution = PollutionSim();
    std::uint32_t result = 0;
    if (pollution) {
        const std::int64_t recycled = pollution->GarbageRecycledPrevMonth();
        const float scale = pollution->GarbageScalingFactor();
        const int centres = pollution->RecyclingCenterCount();
        if (centres != 0) result = static_cast<std::uint32_t>(static_cast<std::int64_t>(scale * static_cast<float>(recycled)) / centres);
    }
    return SetStaticControlNumeric(win, id, result);
}

// @ 0x1002d8064 and its power plant twin @ 0x1002d80a2
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPowerSim* power = PowerSim();
    return SetPerformanceMeterValue(win, id, power ? power->GetCurrentPlantEfficiencyPercent(occupant) : 0.0f);
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantEfficiency(win, id, set, occupant);
}
// @ 0x1002d80ee (the waste-to-energy plant @ 0x1002d80e0 forwards): used over capacity as a whole percent
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    std::uint32_t capacity = 0, used = 0, percent = 0;
    if (IQueryPowerSim* power = PowerSim()) power->GetOccupantPowerProduced(occupant, capacity, used);
    if (capacity != 0) percent = (used * 100u) / capacity;
    return SetStaticControlNumericPercent(win, id, percent);
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPercentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantPercentCapacity(win, id, set, occupant);
}
// @ 0x1002d81ae / 0x1002d8162: always answers true, even when the number could not be shown
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantMaxPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (set) {
        std::uint32_t maximum = 0;
        if (IQueryPowerSim* power = PowerSim()) power->GetOccupantMaxPowerProduced(occupant, maximum);
        SetStaticControlNumeric(win, id, maximum);
    }
    return true;
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyMaxPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantMaxPowerOutputCapacity(win, id, set, occupant);
}
// @ 0x1002d8208 (twin @ 0x1002d81fa): the first figure of the produced pair
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantCurrentPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    std::uint32_t capacity = 0, used = 0;
    if (IQueryPowerSim* power = PowerSim()) power->GetOccupantPowerProduced(occupant, capacity, used);
    return SetStaticControlNumeric(win, id, capacity);
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyCurrentPowerOutputCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantCurrentPowerOutputCapacity(win, id, set, occupant);
}
// @ 0x1002d8272 (twin @ 0x1002d8264): the second figure of the produced pair
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantPowerUsed(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (set) {
        std::uint32_t capacity = 0, used = 0;
        if (IQueryPowerSim* power = PowerSim()) power->GetOccupantPowerProduced(occupant, capacity, used);
        SetStaticControlNumeric(win, id, used);
    }
    return true;
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPowerUsed(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantPowerUsed(win, id, set, occupant);
}
// @ 0x1002d82d8 (twin @ 0x1002d82ca): the monthly cost over the first figure of the produced pair
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantCostPerUnit(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    std::uint32_t capacity = 0, used = 0;
    double perUnit = 0.0;
    IQueryPowerSim* power = PowerSim();
    if (power) power->GetOccupantPowerProduced(occupant, capacity, used);
    if (capacity != 0) {
        std::int64_t cost = 0;
        if (power) power->GetMonthlyPowerGenerationCost(occupant, cost);
        perUnit = static_cast<double>(cost) / static_cast<double>(capacity);
    }
    return SetStaticControlSimoleonsFloat(win, id, perUnit);
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyCostPerUnit(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantCostPerUnit(win, id, set, occupant);
}
// @ 0x1002d843e (twin @ 0x1002d83f4)
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantPercentCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPowerSim* power = PowerSim();
    const float condition = power ? power->GetCurrentPlantConditionPercent(occupant) : 0.0f;
    return SetStaticControlNumericPercent(win, id, TruncatedUnsigned(condition * 100.0f));
}
bool cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPercentCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetPowerPlantPercentCondition(win, id, set, occupant);
}
// @ 0x1002d8488: the funding slider shows the plant's funding fraction and monthly cost; moving it stores the fraction
bool cSC4ViewInputControlQuery::SetOrGetPowerPlantLocalFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    IQueryPowerSim* power = PowerSim();
    if (set) {
        float fraction = 0.0f;
        std::int64_t cost = 0;
        if (!power || !power->GetPowerPlantFundingPercentage(occupant, fraction)) return false;
        if (!power->GetMonthlyPowerGenerationCost(occupant, cost)) return false;
        return SetSliderControlValue(win, id, 1, 0, RoundedPercentForSlider(fraction), cost, occupant,
                                     &SetOrGetPowerPlantLocalFunding, nullptr, 0);
    }
    int value = 0;
    GetSliderControlValue(win, id, value);
    if (power) power->SetPowerPlantFundingPercentage(occupant, static_cast<float>(value) * 0.01f);
    return true;
}

// ---- water building rows ---------------------------------------------------------------------------------------------------
namespace {
IQueryPlumbingSim* PlumbingSim() { IQueryHost* host = QueryHost(); return host ? host->PlumbingSim() : nullptr; }
constexpr std::uint32_t kStrPumpWorking = 0x0a5d4f43, kStrPumpBroken = 0x2a5d4f3b;
}  // namespace

// @ 0x1002d859e (the treatment plant @ 0x1002daf82 forwards)
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    float efficiency = 0.0f;
    if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, nullptr, nullptr, &efficiency, nullptr, nullptr)) return false;
    return SetPerformanceMeterValue(win, id, efficiency);
}
bool cSC4ViewInputControlQuery::SetOrGetWaterTreatmentPlantEfficiency(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetWaterBuildingEfficiency(win, id, set, occupant);
}
// @ 0x1002d860e: water used over the current capacity, as a whole percent
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingUsagePercent(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    std::uint32_t current = 0, used = 0, percent = 0;
    if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, &current, nullptr, nullptr, nullptr, &used)) return false;
    if (current != 0) percent = (used * 100u) / current;
    return SetStaticControlNumericPercent(win, id, percent);
}
// @ 0x1002d868e / 0x1002d86f2 / 0x1002d875a: one figure of the producer info
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingMaxCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    std::uint32_t maximum = 0;
    if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, nullptr, &maximum, nullptr, nullptr, nullptr)) return false;
    return SetStaticControlNumeric(win, id, maximum);
}
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingCurrentCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    std::uint32_t current = 0;
    if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, &current, nullptr, nullptr, nullptr, nullptr)) return false;
    return SetStaticControlNumeric(win, id, current);
}
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingWaterUsed(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    std::uint32_t used = 0;
    if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, nullptr, nullptr, nullptr, nullptr, &used)) return false;
    return SetStaticControlNumeric(win, id, used);
}
// @ 0x1002d87c0: the monthly upkeep over the current capacity
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingCostPerUnit(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    std::uint32_t current = 0;
    std::int64_t cost = 0;
    if (!plumbing || !plumbing->GetWaterProducerInfo(occupant, &current, nullptr, nullptr, nullptr, nullptr) ||
        !plumbing->GetOccupantMonthlyUpkeepCost(occupant, cost))
        return false;
    const double perUnit = current == 0 ? 0.0 : static_cast<double>(cost) / static_cast<double>(current);
    return SetStaticControlSimoleonsFloat(win, id, perUnit);
}
// @ 0x1002d8860: the pump's state as a word
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingPumpCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPlumbingSim* plumbing = PlumbingSim();
    bool pumpWorking = false;
    if (!host || !plumbing || !plumbing->GetWaterProducerInfo(occupant, nullptr, nullptr, nullptr, &pumpWorking, nullptr)) return false;
    cRZString text;
    host->LoadString(kStringGroup, pumpWorking ? kStrPumpWorking : kStrPumpBroken, text);
    return SetStaticControlText(win, id, text);
}
// @ 0x1002d894c: the producer's condition as a whole percent (0 when the simulator has none)
bool cSC4ViewInputControlQuery::SetOrGetWaterBuildingPercentCondition(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryPlumbingSim* plumbing = PlumbingSim();
    float condition = 0.0f;
    std::uint32_t percent = 0;
    if (plumbing && plumbing->GetWaterProducerConditionPercent(occupant, condition)) percent = TruncatedUnsigned(condition * 100.0f);
    return SetStaticControlNumericPercent(win, id, percent);
}

// ---- landmark rows / the transit station's global funding ---------------------------------------------------------------------
namespace {
constexpr std::uint32_t kPurposeTransitFunding = 0xc921ceeb;   // the department the transit rows read (GetDepartmentBudget argument)
constexpr std::uint32_t kPropLandmarkBudgetPurpose = 0xaa59670c;
constexpr std::uint32_t kPropLandmarkHomage = 0xaa5c23fa;
IQueryBudgetSim* BudgetSim() { IQueryHost* host = QueryHost(); return host ? host->BudgetSim() : nullptr; }
}  // namespace

// @ 0x1002d8e3c: the department's funding fraction as a whole percent (0 without a department)
bool cSC4ViewInputControlQuery::SetOrGetTransitStationGlobalFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant*) {
    if (!set) return true;
    IQueryBudgetSim* budget = BudgetSim();
    float fraction = 0.0f;
    std::uint32_t percent = 0;
    if (budget && budget->DepartmentFundingFraction(kPurposeTransitFunding, fraction)) percent = TruncatedUnsigned(fraction * 100.0f);
    return SetStaticControlNumericPercent(win, id, percent);
}
// @ 0x1002d8e9e: the landmark's budget item cost
bool cSC4ViewInputControlQuery::SetOrGetLandmarkFunding(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryBudgetSim::Item item;
    if (IQueryBudgetSim* budget = BudgetSim()) budget->GetBudgetItemForPurpose(occupant->AsPropertyHolder(), kPropLandmarkBudgetPurpose, item);
    return SetStaticControlSimoleons(win, id, item.cost);
}
// @ 0x1002d8f14: the item's cost scaled by its department's funding fraction (float multiply, truncated)
bool cSC4ViewInputControlQuery::SetOrGetLandmarkMaintenance(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryBudgetSim* budget = BudgetSim();
    IQueryBudgetSim::Item item;
    float fraction = 0.0f;
    if (budget) {
        budget->GetBudgetItemForPurpose(occupant->AsPropertyHolder(), kPropLandmarkBudgetPurpose, item);
        budget->DepartmentFundingFraction(item.department, fraction);
    }
    return SetStaticControlSimoleons(win, id, static_cast<std::int64_t>(static_cast<float>(item.cost) * fraction));
}
// @ 0x1002d8fc4: the landmark's homage text, a string named by a resource key property
bool cSC4ViewInputControlQuery::SetOrGetLandmarkHomage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    cGZPersistResourceKey key;
    if (!host || !cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropLandmarkHomage, key)) return false;
    cRZString text;
    host->LoadString(key.group, key.instance, text);
    return SetStaticControlText(win, id, text);
}

// ---- transit station rows ---------------------------------------------------------------------------------------------------
namespace {
// The station data under the occupant: its position -> city cell -> the traffic simulator's query (vslot 0x118).
bool TransitDataOf(cISC4Occupant* occupant, IQueryTrafficSim::TransitData& data) {
    IQueryHost* host = QueryHost();
    IQueryCity* city = host ? host->City() : nullptr;
    IQueryTrafficSim* traffic = host ? host->TrafficSim() : nullptr;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    int cellX = 0, cellZ = 0;
    if (city) city->PositionToCell(position[0], position[2], cellX, cellZ);
    return traffic && traffic->GetTransitSwitchQueryData(cellX, cellZ, data);
}
}  // namespace

// @ 0x1002d89b0: served over actual usage, at most 1 (full when nothing is in use); nothing served leaves the meter empty
bool cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryTrafficSim::TransitData data;
    float quality = 0.0f;
    if (TransitDataOf(occupant, data) && data.served != 0) {
        if (data.usage == 0) {
            quality = 1.0f;
        } else {
            quality = static_cast<float>(static_cast<std::uint32_t>(data.served)) / static_cast<float>(static_cast<std::uint32_t>(data.usage));
            if (quality > 1.0f) quality = 1.0f;
        }
    }
    return SetPerformanceMeterValue(win, id, quality);
}
// @ 0x1002d8aae: usage over capacity as a whole percent
bool cSC4ViewInputControlQuery::SetOrGetTransitStationUsagePercent(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryTrafficSim::TransitData data;
    std::uint32_t percent = 0;
    if (TransitDataOf(occupant, data) && data.capacity != 0) {
        const float ratio = static_cast<float>(static_cast<std::uint32_t>(data.usage)) / static_cast<float>(static_cast<std::uint32_t>(data.capacity));
        percent = TruncatedUnsigned(ratio * 100.0f);
    }
    return SetStaticControlNumericPercent(win, id, percent);
}
// @ 0x1002d8ba2 / 0x1002d8c80 / 0x1002d8d5e: one figure each; false when the simulator has no station there
bool cSC4ViewInputControlQuery::SetOrGetTransitStationMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryTrafficSim::TransitData data;
    if (!TransitDataOf(occupant, data)) return false;
    return SetStaticControlSimoleons(win, id, static_cast<std::int64_t>(static_cast<std::uint32_t>(data.maintenance)));
}
bool cSC4ViewInputControlQuery::SetOrGetTransitStationActualUsage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryTrafficSim::TransitData data;
    if (!TransitDataOf(occupant, data)) return false;
    return SetStaticControlNumeric(win, id, static_cast<std::uint32_t>(data.usage));
}
bool cSC4ViewInputControlQuery::SetOrGetTransitStationMaxCapacity(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryTrafficSim::TransitData data;
    if (!TransitDataOf(occupant, data)) return false;
    return SetStaticControlNumeric(win, id, static_cast<std::uint32_t>(data.capacity));
}

// ---- RCI building rows: occupancy, customers, land value, the goofy stat -------------------------------------------------------
namespace {
// The lot under an occupant: position -> city cell -> lot manager (flag 0).
IQueryLot* LotUnder(cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    IQueryCity* city = host ? host->City() : nullptr;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    int cellX = 0, cellZ = 0;
    if (city) city->PositionToCell(position[0], position[2], cellX, cellZ);
    return host && host->LotManager() ? host->LotManager()->GetLotAtCell(cellX, cellZ, 0) : nullptr;
}
}  // namespace

// @ 0x1002d9baa: the lot's capacity for its occupant wealth, else the building's profile capacity
std::uint32_t cSC4ViewInputControlQuery::GetOccupantCapacity(cISC4Occupant* occupant) {
    IQueryLot* lot = LotUnder(occupant);
    if (!lot) return 0;
    std::uint16_t capacity = lot->GetCapacity(lot->GetOccupantWealth());
    if (capacity == 0) {
        void* building = nullptr;
        if (occupant->QueryInterface(kIID_cISC4BuildingOccupant, &building)) {
            auto* profile = static_cast<IQueryBuildingOccupant*>(building);
            capacity = profile->GetProfileCapacity();
            profile->Release();
        }
    }
    return capacity;
}
// @ 0x1002d9a92: capacity times the lot's occupancy (the original also looks up the building profile and developer type and
// drops them), 0 without a lot or a building
std::uint32_t cSC4ViewInputControlQuery::GetCurrentOccupancyValue(cISC4Occupant* occupant) {
    IQueryLot* lot = LotUnder(occupant);
    if (!lot) return 0;
    lot->GetOccupantWealth();
    if (lot->GetBuildingType(true) == 0) return 0;
    const float occupancy = lot->GetOccupancy();
    return TruncatedUnsigned(static_cast<float>(GetOccupantCapacity(occupant)) * occupancy);
}
// @ 0x1002da05e
int cSC4ViewInputControlQuery::GetLandValue(cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    IQueryCity* city = host ? host->City() : nullptr;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    int cellX = 0, cellZ = 0;
    if (city) city->PositionToCell(position[0], position[2], cellX, cellZ);
    IQueryLandValueSim* landValue = host ? host->LandValueSim() : nullptr;
    return landValue ? landValue->GetLandValueType(cellX, cellZ) : 0;
}

// @ 0x1002d9ca8: "current/capacity"
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingCurrentOccupancy(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (occupant && set) {
        const std::uint32_t current = GetCurrentOccupancyValue(occupant);
        const std::uint32_t capacity = GetOccupantCapacity(occupant);
        cRZString text;
        text.Sprintf("%d/%d", current, capacity);
        SetStaticControlText(win, id, text);
    }
    return true;
}
// @ 0x1002d9d5e: the capacity, left alone when it is 0
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingMaxOccupancy(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (occupant && set) {
        const std::uint32_t capacity = GetOccupantCapacity(occupant);
        if (capacity != 0) {
            cRZString text;
            text.Sprintf("%d", capacity);
            SetStaticControlText(win, id, text);
        }
    }
    return true;
}
// @ 0x1002dac26: occupancy over capacity as a number (a zero capacity divides to infinity, as compiled)
bool cSC4ViewInputControlQuery::SetOrGetCommercialServiceBuildingCustomers(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (occupant && set) {
        const std::uint32_t current = GetCurrentOccupancyValue(occupant);
        const std::uint32_t capacity = GetOccupantCapacity(occupant);
        SetStaticControlNumeric(win, id, TruncatedUnsigned((static_cast<float>(current) * 100.0f) / static_cast<float>(capacity)));
    }
    return true;
}
// @ 0x1002da28c: land value in three words (the lowest word also stands for anything unknown)
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingLandValue(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (occupant && set) {
        IQueryHost* host = QueryHost();
        const int value = GetLandValue(occupant);
        cRZString text;
        std::uint32_t instance = 0x4a5fe266;
        if (value == 3) instance = 0xaa5fe249;
        else if (value == 2) instance = 0x0a5fe260;
        if (host) host->LoadString(kStringGroup, instance, text);
        SetStaticControlText(win, id, text);
    }
    return true;
}

// @ 0x1002d6944: the mayor rating pushed through the property's curve (identity 0..256 without one)
bool cSC4ViewInputControlQuery::SetOrGetGoofyStat(cIGZWin* win, std::uint32_t id, bool set, cISCPropertyHolder* holder, std::uint32_t propertyId) {
    if (!set) return true;
    cSC4ResponseCurve curve;
    curve.SetResponseCurveData(std::vector<SC4Point>{SC4Point{0.0f, 0.0f}, SC4Point{256.0f, 256.0f}});
    if (holder) {
        if (cISCProperty* property = holder->GetProperty(propertyId)) {
            const cIGZVariant* value = property->GetPropertyValue();
            curve.SetResponseCurveData(value->RefFloat32(), value->GetCount());
        }
    }
    IQueryHost* host = QueryHost();
    IQueryAuraSim* aura = host ? host->AuraSim() : nullptr;
    const int rating = aura ? aura->GetMayorRating() : 0;
    return SetStaticControlNumeric(win, id, TruncatedUnsigned(curve.GetOutputValue(static_cast<float>(rating))));
}
// @ 0x1002dac96
bool cSC4ViewInputControlQuery::SetOrGetOccupantGoofyStat(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return SetOrGetGoofyStat(win, id, set, occupant->AsPropertyHolder(), 0xaa60e745);
}
// @ 0x1002d68bc: the tuning exemplar {0x6534284a, 0x67cd5fa1, 0xca270e9d}'s goofy stat curve; false when it is missing, also when clearing
bool cSC4ViewInputControlQuery::SetOrGetAirportLostLuggage(cIGZWin* win, std::uint32_t id, bool set, int, int) {
    IQueryHost* host = QueryHost();
    cGZPersistResourceKey key;
    key.type = 0x6534284a;
    key.group = 0x67cd5fa1;
    key.instance = 0xca270e9d;
    cISCPropertyHolder* holder = host ? host->GetTuningProperties(key) : nullptr;
    if (!holder) return false;
    return SetOrGetGoofyStat(win, id, set, holder, 0xaa60e745);
}
// @ 0x1002d6876
bool cSC4ViewInputControlQuery::SetOrGetAirportOccupantLostLuggage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    int x = 0, z = 0;
    if (!PortCellOfOccupant(occupant, x, z)) return false;
    return SetOrGetAirportLostLuggage(win, id, set, x, z);
}

// ---- RCI rating rows --------------------------------------------------------------------------------------------------------
namespace {
// cvttss2si r32: out-of-range and NaN give INT32_MIN
int TruncatedInt(float value) {
    if (!(value > -2147483648.0f && value < 2147483648.0f)) return INT32_MIN;
    return static_cast<int>(value);
}
// The three-way ranking the "bad is high" maps share (pollution, garbage): below the low threshold the rating is bad (2) with the
// magnitude negated, below the high one neutral (1), else good (0) with half the value.
std::uint32_t RankBadWhenHigh(int value, int low, int high, int& out) {
    if (value < low) { out = -value; return 2; }
    if (value < high) { out = 0; return 1; }
    out = value / 2;
    return 0;
}
// The mirror for the "good is high" maps (school, hospital): bad 0, neutral 1, good 2.
std::uint32_t RankGoodWhenHigh(int value, int low, int high, int& out) {
    if (value < low) { out = -value; return 0; }
    if (value < high) { out = 0; return 1; }
    out = value / 2;
    return 2;
}
}  // namespace

// @ 0x1002cf38e: the pollution curves (index 2 is never added; the original reaches index 3 through it) over the pollution maps
std::uint32_t cSC4ViewInputControlQuery::GetSubjectivePollution(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(0, low, high);
    int sum = 0;
    for (int map = 0; map < 4; ++map) {
        const cSC4ResponseCurve* curve = developer->PollutionCurve(map);
        if (!curve || map == 2) continue;
        const float reading = static_cast<float>(host ? host->PollutionMapValue(map, x, z) : 0);
        sum = TruncatedInt(static_cast<float>(sum) + curve->GetOutputValue(reading));
    }
    return RankBadWhenHigh(sum, low, high, value);
}
// @ 0x1002cf488: pollution map 2 through the third curve
std::uint32_t cSC4ViewInputControlQuery::GetSubjectiveGarbage(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(1, low, high);
    int reading = 0;
    if (const cSC4ResponseCurve* curve = developer->PollutionCurve(2))
        reading = TruncatedInt(curve->GetOutputValue(static_cast<float>(host ? host->PollutionMapValue(2, x, z) : 0)));
    return RankBadWhenHigh(reading, low, high, value);
}
// @ 0x1002cf544 / 0x1002cf5fe: with no curve the rating is neutral and the value 0
std::uint32_t cSC4ViewInputControlQuery::GetSubjectiveSchoolGrade(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(2, low, high);
    const cSC4ResponseCurve* curve = developer->SchoolCurve();
    if (!curve) { value = 0; return 1; }
    return RankGoodWhenHigh(TruncatedInt(curve->GetOutputValue(host ? host->SchoolGradeValue(x, z) : 0.0f)), low, high, value);
}
std::uint32_t cSC4ViewInputControlQuery::GetSubjectiveHospitalGrade(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(3, low, high);
    const cSC4ResponseCurve* curve = developer->HospitalCurve();
    if (!curve) { value = 0; return 1; }
    return RankGoodWhenHigh(TruncatedInt(curve->GetOutputValue(host ? host->HospitalGradeValue(x, z) : 0.0f)), low, high, value);
}
// @ 0x1002cf6b8: crime is bad when high (bad 2, good 0), a missing curve neutral
std::uint32_t cSC4ViewInputControlQuery::GetSubjectiveCrimeLevel(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(4, low, high);
    const cSC4ResponseCurve* curve = developer->CrimeCurve();
    if (!curve) { value = 0; return 1; }
    return RankBadWhenHigh(TruncatedInt(curve->GetOutputValue(static_cast<float>(host ? host->CrimeValue(x, z) : 0))), low, high, value);
}
// @ 0x1002cf776: the curve at the traffic byte and at the next one decide the rating from the slope. The value is only written for
// the bad and good ends; a missing curve or the middle band leaves it alone and answers neutral.
std::uint32_t cSC4ViewInputControlQuery::GetSubjectiveTrafficLevel(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(5, low, high);
    const cSC4ResponseCurve* curve = developer->TrafficCurve();
    if (!curve) return 1;
    const std::uint32_t reading = host ? host->TrafficValue(x, z) : 0;
    const float here = curve->GetOutputValue(static_cast<float>(reading));
    const float next = curve->GetOutputValue(static_cast<float>(reading + 1));
    if (static_cast<float>(low) > here) {
        value = TruncatedInt(-here);
        return here > next ? 2u : 0u;
    }
    if (here < static_cast<float>(high)) return 1;
    value = TruncatedInt(here * 0.5f);
    return here <= next ? 2u : 0u;
}
// @ 0x1002cf87c: a zero trip length reads as 255
std::uint32_t cSC4ViewInputControlQuery::GetSubjectiveTripLength(IQueryGrowthDeveloper* developer, int x, int z, int& value) {
    IQueryHost* host = QueryHost();
    int low = 0, high = 0;
    developer->GetRatingThresholds(6, low, high);
    const cSC4ResponseCurve* curve = developer->TripLengthCurve();
    if (!curve) { value = 0; return 1; }
    const std::uint32_t reading = host ? host->TripLengthValue(x, z) : 0;
    const int rated = TruncatedInt(curve->GetOutputValue(reading == 0 ? 255.0f : static_cast<float>(reading)));
    if (low > rated) { value = -rated; return 2; }
    if (high <= rated) { value = rated / 2; return 0; }
    value = 0;
    return 1;
}

// @ 0x1002d99be: the lot of the occupant, its centre cell, the best growth developer for the lot; the rating picks one of three strings
void cSC4ViewInputControlQuery::GetSubjectiveOccupantRatingString(cISC4Occupant* occupant, tSubjectiveFn rating, const std::uint32_t* strings,
                                                                  cRZString& out) {
    IQueryHost* host = QueryHost();
    if (!host || !host->LotManager() || !host->City()) return;
    IQueryLot* lot = host->LotManager()->GetLotForOccupant(occupant);
    if (!lot) return;
    float center[3] = {0.0f, 0.0f, 0.0f};
    lot->GetCenter(center);
    int cellX = 0, cellZ = 0;
    host->City()->PositionToCell(center[0], center[2], cellX, cellZ);
    IQueryGrowthDeveloper* developer = host->FindBestDeveloper(lot);
    if (!developer) return;
    int value = 0;
    const std::uint32_t index = rating(developer, cellX, cellZ, value);
    host->LoadString(kStringGroup, strings[index], out);
}

namespace {
constexpr std::uint32_t kGeneralRatingStrings[3] = {0x4a5fe266, 0x0a5fe260, 0xaa5fe249};   // sGeneralRatingStringGUIDs 0x100a1c798
constexpr std::uint32_t kCommuteRatingStrings[3] = {0xaa71fae3, 0x6a71fadc, 0x0a71fad3};   // sCommuteRatingStringGUIDs 0x100a1c7a4
constexpr std::uint32_t kStrCommuteAbandoned = 0x0a71fad3;
bool RatingRow(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant, cSC4ViewInputControlQuery::tSubjectiveFn rating) {
    if (occupant && set) {
        cRZString text;
        cSC4ViewInputControlQuery::GetSubjectiveOccupantRatingString(occupant, rating, kGeneralRatingStrings, text);
        cSC4ViewInputControlQuery::SetStaticControlText(win, id, text);
    }
    return true;
}
}  // namespace
// @ 0x1002da0e2 / 0x1002da170 / 0x1002da1fe / 0x1002da462 / 0x1002da724 / 0x1002da7b2
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return RatingRow(win, id, set, occupant, &GetSubjectivePollution);
}
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingGarbage(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return RatingRow(win, id, set, occupant, &GetSubjectiveGarbage);
}
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingCrime(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return RatingRow(win, id, set, occupant, &GetSubjectiveCrimeLevel);
}
bool cSC4ViewInputControlQuery::SetOrGetRCIBuildingTraffic(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return RatingRow(win, id, set, occupant, &GetSubjectiveTrafficLevel);
}
bool cSC4ViewInputControlQuery::SetOrGetResidentialBuildingEQ(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return RatingRow(win, id, set, occupant, &GetSubjectiveSchoolGrade);
}
bool cSC4ViewInputControlQuery::SetOrGetResidentialBuildingHQ(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    return RatingRow(win, id, set, occupant, &GetSubjectiveHospitalGrade);
}
// @ 0x1002da38a: an abandoned lot (reason 4) has its own word; otherwise the trip length rating over the commute strings
bool cSC4ViewInputControlQuery::SetOrGetResidentialBuildingCommute(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    if (!occupant || !set) return true;
    IQueryHost* host = QueryHost();
    cRZString text;
    IQueryLot* lot = host && host->LotManager() ? host->LotManager()->GetLotForOccupant(occupant) : nullptr;
    if (lot && lot->GetAbandonReason() == 4) {
        host->LoadString(kStringGroup, kStrCommuteAbandoned, text);
    } else {
        GetSubjectiveOccupantRatingString(occupant, &GetSubjectiveTripLength, kCommuteRatingStrings, text);
    }
    SetStaticControlText(win, id, text);
    return true;
}

// @ 0x1002daf96
// PARTIAL: the original is a switch over the cell-based control ids (0x1011..0x1015 airport/seaport traffic, 0x1026 /
// 0x1027 landfill, 0x1028 the landfill developer's monthly cost, 0x104b..0x104e the neighbour-connection rows and
// 0x1066..0x107e the neighbour deals / airport / seaport rows). Only the four neighbour-connection rows this port
// implements are dispatched; every other arm belongs to a row handler that needs an unrecovered developer/simulator
// (see notes/ledger_pending/cSC4ViewInputControlQuery.txt). The dispatcher always answers false, like the original.
bool cSC4ViewInputControlQuery::SetOrGetControlValue(cIGZWin* win, std::uint32_t id, bool set, tCellInfo* info) {
    if (!info) return false;
    const std::uint32_t rowId = static_cast<std::uint32_t>(info->idOffset);
    switch (id) {
    case 0x1026: SetOrGetLandfillUsage(win, 0x1026, set, info->x, info->z); return false;
    case 0x1027: SetOrGetLandfillNetTrashThisMonth(win, 0x1027, set, info->x, info->z); return false;
    case 0x1011: SetOrGetAirportAirTrafficVolume(win, 0x1011, set, info->x, info->z); return false;
    case 0x1013: SetOrGetAirportMaintenanceCost(win, 0x1013, set, info->x, info->z); return false;
    case 0x1014: SetOrGetAirportLostLuggage(win, 0x1014, set, info->x, info->z); return false;
    case 0x1015: SetOrGetSeaportSeaTrafficVolume(win, 0x1015, set, info->x, info->z); return false;
    case 0x1078: SetOrGetSeaportMaintenanceCost(win, 0x1078, set, info->x, info->z); return false;
    case 0x107b: SetOrGetAirportUsage(win, 0x107b, set, info->x, info->z); return false;
    case 0x107c: SetOrGetAirportCapacity(win, 0x107c, set, info->x, info->z); return false;
    case 0x107d: SetOrGetSeaportUsage(win, 0x107d, set, info->x, info->z); return false;
    case 0x107e: SetOrGetSeaportCapacity(win, 0x107e, set, info->x, info->z); return false;
    case 0x1028:
        if (set) {
            IQueryLandfillDeveloper* landfill = Landfill();
            SetStaticControlSimoleons(win, 0x1028, landfill ? landfill->GetCurrentBudget() : 0);
        }
        return false;
    case 0x104b:
        SetOrGetNeighborConnectionNeighborCityName(win, rowId + 0x104b, set, info->x, info->z, info->connectionType);
        return false;
    case 0x104c:
        SetOrGetNeighborConnectionNeighborCityMayorName(win, rowId + 0x104c, set, info->x, info->z,
                                                        info->connectionType);
        return false;
    case 0x104d:
        SetOrGetNeighborConnectionCongestion(win, rowId + 0x104d, set, info->x, info->z);
        return false;
    default:
        return false;
    }
}

// @ 0x1002d5a0a
bool cSC4ViewInputControlQuery::SetOrGetName(cIGZWin* win, std::uint32_t id, bool set, cISC4Occupant* occupant) {
    cRZString name;
    GetOccupantName(occupant, name);
    if (set) return SetEditControlText(win, id, name);
    cRZString edited;
    std::uint32_t length = 0;
    if (!GetEditControlText(win, id, edited, length)) return true;
    if (edited.IsEqualToChars(name.ToChar(), name.Strlen(), true)) return true;
    if (occupant) {
        void* building = nullptr;
        if (occupant->QueryInterface(kIID_cISC4BuildingOccupant, &building)) {
            auto* named = static_cast<IQueryBuildingOccupant*>(building);
            named->SetName(edited);
            named->Release();
        }
    }
    return true;
}

// @ 0x1002d5bc0
bool cSC4ViewInputControlQuery::SetOrGetWhyOccupantAbandoned(cIGZWin* win, std::uint32_t id, bool set,
                                                             cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    if (!set || !host) return false;
    cRZString reason;
    IQueryLot* lot = host->LotManager() ? host->LotManager()->GetLotForOccupant(occupant) : nullptr;
    if (lot) {
        lot->AddRef();
        const int state = lot->GetState();
        const int abandoned = lot->GetAbandonReason();
        if (state == 4 && abandoned >= 1 && abandoned <= 6) {
            const std::uint32_t instance = (abandoned == 5) ? 0 : kStrAbandoned[abandoned];
            if (instance) host->LoadString(kStringGroup, instance, reason);
        }
        lot->Release();
    }
    SetWindowVisible(win, id, reason.Strlen() != 0);
    SetStaticControlText(win, id, reason);
    return true;
}

// @ 0x1002dacd2
bool cSC4ViewInputControlQuery::SetOrGetOccupantIsPowered(cIGZWin* win, std::uint32_t id, bool set,
                                                          cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    if (!set || !host) return true;
    cRZString text;
    IQueryPowerSim* power = host->PowerSim();
    if (power && power->IsPowered(occupant)) host->LoadString(kStringGroup, kStrYes, text);
    else host->LoadString(kStringGroup, kStrNo, text);
    SetStaticControlText(win, id, text);
    return true;
}

// @ 0x1002dadd6
bool cSC4ViewInputControlQuery::SetOrGetOccupantIsWatered(cIGZWin* win, std::uint32_t id, bool set,
                                                          cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    if (!set || !host) return true;
    cRZString text;
    IQueryPlumbingSim* plumbing = host->PlumbingSim();
    if (plumbing && plumbing->IsWatered(occupant)) host->LoadString(kStringGroup, kStrYes, text);
    else host->LoadString(kStringGroup, kStrNo, text);
    SetStaticControlText(win, id, text);
    return true;
}

// @ 0x1002daed6
// PARTIAL: the original reads/writes the toggle state of a button control through nsGZWinUtils::GetButtonToggleState
// (0x1006a21eb) / SetButtonToggleState (0x1006a215d), which are not recovered; only the lot lookup is ported.
bool cSC4ViewInputControlQuery::SetOrGetOccupantIsHistorical(cIGZWin*, std::uint32_t, bool, cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    if (!host || !host->LotManager()) return false;
    IQueryLot* lot = host->LotManager()->GetLotForOccupant(occupant);
    if (!lot) return false;
    lot->AddRef();
    lot->Release();
    return true;
}

// @ 0x1002da692
bool cSC4ViewInputControlQuery::SetOrGetResidentialBuildingMayorApproval(cIGZWin* win, std::uint32_t id, bool set,
                                                                         cISC4Occupant* occupant) {
    if (!occupant || !set) return true;
    cRZString text;
    GetRatingIn3DegreesFromPercent(text, GetMayorRatingPercent(occupant));
    SetStaticControlText(win, id, text);
    return true;
}

// @ 0x1002d2616
void cSC4ViewInputControlQuery::GetOccupantName(cISC4Occupant* occupant, cRZString& out) {
    if (!occupant) return;
    void* building = nullptr;
    if (occupant->QueryInterface(kIID_cISC4BuildingOccupant, &building)) {
        auto* named = static_cast<IQueryBuildingOccupant*>(building);
        named->GetName(out);
        named->Release();
    }
}

// @ 0x1002da640. Quirk kept: the `percent < 33.3` arm of the outer test can never run (the outer test already
// established 33.3 <= percent); only the boundary values matter.
void cSC4ViewInputControlQuery::GetRatingIn3DegreesFromPercent(cRZString& out, float percent) {
    std::uint32_t instance;
    if (33.3f <= percent) {
        instance = (66.6f <= percent) ? kStrRatingGood : kStrRatingFair;
    } else {
        instance = kStrRatingBad;
    }
    IQueryHost* host = QueryHost();
    if (host) host->LoadString(kStringGroup, instance, out);
}

// @ 0x1002da4f0: the average aura over the cells of the occupant's lot, as a percentage (0..100).
float cSC4ViewInputControlQuery::GetMayorRatingPercent(cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    if (!occupant || !host || !host->City() || !host->LotManager() || !host->AuraSim()) return 0.0f;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    int cellX = 0, cellZ = 0;
    host->City()->PositionToCell(position[0], position[2], cellX, cellZ);
    IQueryLot* lot = host->LotManager()->GetLotAtCell(cellX, cellZ, 0);
    if (!lot) return 0.0f;
    lot->AddRef();
    SC4CellRect rect;
    lot->GetCellRect(rect);
    int count = 0;
    int total = 0;
    for (int x = rect.x0; x <= rect.x1; ++x) {
        for (int z = rect.z0; z <= rect.z1; ++z) {
            ++count;
            total += 0x80 + static_cast<int>(host->AuraSim()->GetAura(x, z));
        }
    }
    lot->Release();
    if (count == 0) return 0.0f;
    return (static_cast<float>(total) / static_cast<float>(count)) * 100.0f / 255.0f;
}

// --- dialog row dispatch (the EnumFunc* callbacks) ----------------------------------------------------------

namespace {

// The interface the row enumeration recurses through (EnumChildren argument, not an iid the port needs to know).
constexpr std::uint32_t kEnumRowIid = 0x22ba0121;
// cSLString group/instance of the neighbour-city name placeholder (loaded before the city's own name).
constexpr std::uint32_t kStrNeighborCityGroup = 0x6a231eaa;
constexpr std::uint32_t kStrNeighborCityInstance = 0x0a8e15f9;
// Property holding the budget value ids summed by SetOrGetOccupantCost.
constexpr std::uint32_t kPropBudgetValues = 0xea54d285;

}  // namespace

// @ 0x100a92150, 0x7f entries, index = (id & 0xfff) - 0x1000. Entries this port has not implemented yet are null
// (the original points them at the SetOrGet* rows listed in notes/ledger_pending/cSC4ViewInputControlQuery.txt).
cSC4ViewInputControlQuery::tSetOrGetFn const cSC4ViewInputControlQuery::kRowFunctions[0x7f] = {
    &cSC4ViewInputControlQuery::SetOrGetName, &cSC4ViewInputControlQuery::SetOrGetFireStationEffectiveness, &cSC4ViewInputControlQuery::SetOrGetFireStationLocalFunding, &cSC4ViewInputControlQuery::SetOrGetFireStationEngineCount,   // 0x1000..0x1003
    &cSC4ViewInputControlQuery::SetOrGetFireStationMaintenanceCost, nullptr, &cSC4ViewInputControlQuery::SetOrGetPoliceStationEffectiveness, &cSC4ViewInputControlQuery::SetOrGetPoliceStationArrestCount,   // 0x1004..0x1007
    &cSC4ViewInputControlQuery::SetOrGetPoliceStationCrimeCount, &cSC4ViewInputControlQuery::SetOrGetPoliceStationLocalFunding, &cSC4ViewInputControlQuery::SetOrGetPoliceStationPatrolCarCount, &cSC4ViewInputControlQuery::SetOrGetPoliceStationMaintenanceCost,   // 0x1008..0x100b
    &cSC4ViewInputControlQuery::SetOrGetJailCondition, &cSC4ViewInputControlQuery::SetOrGetJailCellCount, &cSC4ViewInputControlQuery::SetOrGetJailInmateCount, &cSC4ViewInputControlQuery::SetOrGetJailMaintenanceCost,   // 0x100c..0x100f
    &cSC4ViewInputControlQuery::SetOrGetParkMaintenanceCost, &cSC4ViewInputControlQuery::SetOrGetAirportOccupantAirTrafficVolume, nullptr, &cSC4ViewInputControlQuery::SetOrGetAirportOccupantMaintenanceCost,   // 0x1010..0x1013
    &cSC4ViewInputControlQuery::SetOrGetAirportOccupantLostLuggage, &cSC4ViewInputControlQuery::SetOrGetSeaportOccupantSeaTrafficVolume, nullptr, &cSC4ViewInputControlQuery::SetOrGetSchoolGrade,   // 0x1014..0x1017
    &cSC4ViewInputControlQuery::SetOrGetSchoolLocalStaffFunding, &cSC4ViewInputControlQuery::SetOrGetSchoolLocalBusFunding, &cSC4ViewInputControlQuery::SetOrGetSchoolTeacherCount, &cSC4ViewInputControlQuery::SetOrGetSchoolDeskCount,   // 0x1018..0x101b
    &cSC4ViewInputControlQuery::SetOrGetSchoolStudentCount, &cSC4ViewInputControlQuery::SetOrGetMuseumExhibitTicketsAvailable, &cSC4ViewInputControlQuery::SetOrGetLibraryBooksBorrowedCount, &cSC4ViewInputControlQuery::SetOrGetLibraryBooksInCollectionCount,   // 0x101c..0x101f
    &cSC4ViewInputControlQuery::SetOrGetHospitalGrade, &cSC4ViewInputControlQuery::SetOrGetHospitalLocalStaffFunding, &cSC4ViewInputControlQuery::SetOrGetHospitalLocalAmbulanceFunding, &cSC4ViewInputControlQuery::SetOrGetHospitalDoctorCount,   // 0x1020..0x1023
    &cSC4ViewInputControlQuery::SetOrGetHospitalBedCount, &cSC4ViewInputControlQuery::SetOrGetHospitalPatientCount, nullptr, nullptr,   // 0x1024..0x1027
    nullptr, &cSC4ViewInputControlQuery::SetOrGetIncineratorEfficiency, &cSC4ViewInputControlQuery::SetOrGetIncineratorPercentCapacity, &cSC4ViewInputControlQuery::SetOrGetIncineratorTrashIncinerated,   // 0x1028..0x102b
    nullptr, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyEfficiency, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPercentCapacity, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyMaxPowerOutputCapacity,   // 0x102c..0x102f
    &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyCurrentPowerOutputCapacity, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPowerUsed, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyCostPerUnit, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyTrashIncinerated,   // 0x1030..0x1033
    &cSC4ViewInputControlQuery::SetOrGetPowerPlantEfficiency, &cSC4ViewInputControlQuery::SetOrGetPowerPlantPercentCapacity, &cSC4ViewInputControlQuery::SetOrGetPowerPlantMaxPowerOutputCapacity, &cSC4ViewInputControlQuery::SetOrGetPowerPlantCurrentPowerOutputCapacity,   // 0x1034..0x1037
    &cSC4ViewInputControlQuery::SetOrGetPowerPlantPowerUsed, &cSC4ViewInputControlQuery::SetOrGetPowerPlantCostPerUnit, &cSC4ViewInputControlQuery::SetOrGetWaterBuildingEfficiency, &cSC4ViewInputControlQuery::SetOrGetWaterBuildingUsagePercent,   // 0x1038..0x103b
    &cSC4ViewInputControlQuery::SetOrGetWaterBuildingMaxCapacity, &cSC4ViewInputControlQuery::SetOrGetWaterBuildingCurrentCapacity, &cSC4ViewInputControlQuery::SetOrGetWaterBuildingWaterUsed, &cSC4ViewInputControlQuery::SetOrGetWaterBuildingCostPerUnit,   // 0x103c..0x103f
    &cSC4ViewInputControlQuery::SetOrGetWaterBuildingPumpCondition, &cSC4ViewInputControlQuery::SetOrGetTransitStationServiceQuality, &cSC4ViewInputControlQuery::SetOrGetTransitStationUsagePercent, &cSC4ViewInputControlQuery::SetOrGetTransitStationMaintenanceCost,   // 0x1040..0x1043
    &cSC4ViewInputControlQuery::SetOrGetTransitStationActualUsage, &cSC4ViewInputControlQuery::SetOrGetTransitStationMaxCapacity, &cSC4ViewInputControlQuery::SetOrGetTransitStationGlobalFunding, &cSC4ViewInputControlQuery::SetOrGetLandmarkFunding,   // 0x1044..0x1047
    nullptr, &cSC4ViewInputControlQuery::SetOrGetLandmarkMaintenance, &cSC4ViewInputControlQuery::SetOrGetLandmarkHomage, nullptr,   // 0x1048..0x104b
    nullptr, nullptr, nullptr, nullptr,   // 0x104c..0x104f
    nullptr, &cSC4ViewInputControlQuery::SetOrGetRCIBuildingCurrentOccupancy, &cSC4ViewInputControlQuery::SetOrGetRCIBuildingMaxOccupancy, nullptr,   // 0x1050..0x1053
    &cSC4ViewInputControlQuery::SetOrGetRCIBuildingPollution, &cSC4ViewInputControlQuery::SetOrGetRCIBuildingCrime, &cSC4ViewInputControlQuery::SetOrGetRCIBuildingLandValue, &cSC4ViewInputControlQuery::SetOrGetResidentialBuildingCommute,   // 0x1054..0x1057
    &cSC4ViewInputControlQuery::SetOrGetResidentialBuildingMayorApproval, &cSC4ViewInputControlQuery::SetOrGetResidentialBuildingEQ, &cSC4ViewInputControlQuery::SetOrGetResidentialBuildingHQ, nullptr,   // 0x1058..0x105b
    &cSC4ViewInputControlQuery::SetOrGetCommercialServiceBuildingCustomers, nullptr, &cSC4ViewInputControlQuery::SetOrGetRecyclingCenterEfficiency, &cSC4ViewInputControlQuery::SetOrGetRecyclingCenterPercentCapacity,   // 0x105c..0x105f
    &cSC4ViewInputControlQuery::SetOrGetRecyclingCenterTrashRecycled, &cSC4ViewInputControlQuery::SetOrGetWasteToEnergyPercentCondition, &cSC4ViewInputControlQuery::SetOrGetPowerPlantPercentCondition, &cSC4ViewInputControlQuery::SetOrGetPowerPlantLocalFunding,   // 0x1060..0x1063
    nullptr, nullptr, nullptr, nullptr,   // 0x1064..0x1067
    nullptr, nullptr, nullptr, &cSC4ViewInputControlQuery::SetOrGetOccupantGoofyStat,   // 0x1068..0x106b
    &cSC4ViewInputControlQuery::SetOrGetOccupantCost, &cSC4ViewInputControlQuery::SetOrGetWaterTreatmentPlantEfficiency, nullptr, nullptr,   // 0x106c..0x106f
    nullptr, nullptr, nullptr, &cSC4ViewInputControlQuery::SetOrGetWaterBuildingPercentCondition,   // 0x1070..0x1073
    &cSC4ViewInputControlQuery::SetOrGetRCIBuildingTraffic, &cSC4ViewInputControlQuery::SetOrGetRCIBuildingGarbage, &cSC4ViewInputControlQuery::SetOrGetOccupantIsPowered, &cSC4ViewInputControlQuery::SetOrGetOccupantIsWatered,   // 0x1074..0x1077
    &cSC4ViewInputControlQuery::SetOrGetSeaportOccupantMaintenanceCost, &cSC4ViewInputControlQuery::SetOrGetWhyOccupantAbandoned, &cSC4ViewInputControlQuery::SetOrGetOccupantIsHistorical, &cSC4ViewInputControlQuery::SetOrGetAirportOccupantUsage,   // 0x1078..0x107b
    &cSC4ViewInputControlQuery::SetOrGetAirportOccupantCapacity, &cSC4ViewInputControlQuery::SetOrGetSeaportOccupantUsage, &cSC4ViewInputControlQuery::SetOrGetSeaportOccupantCapacity,   // 0x107c..0x107e
};

// @ 0x1002db37e: apply the message filter to the child, then recurse through the same callback.
bool cSC4ViewInputControlQuery::EnumFuncSetFilter(cIGZWin*, std::uint32_t, void* child, void* context) {
    auto* win = static_cast<cIGZWin*>(child);
    if (!win) return true;
    win->AddMessageFilter(static_cast<cIGZWinMessageFilter*>(context));
    win->EnumChildren(kEnumRowIid, &cSC4ViewInputControlQuery::EnumFuncSetFilter, context);
    return true;
}

// @ 0x1002db3bc: drop the filter and the five slider/filter parameters. Deviation: the original releases each
// parameter object twice (0x1002db40f/0x1002db419); with this port's counted cRZVariant the window's SetParam(id,
// nullptr) already drops its reference, so the second release would free the object and then touch it. Only the
// caller's own reference is released here.
bool cSC4ViewInputControlQuery::EnumFuncClearFilter(cIGZWin*, std::uint32_t, void* child, void* context) {
    auto* win = static_cast<cIGZWin*>(child);
    if (!win) return true;
    win->RemoveMessageFilter(static_cast<cIGZWinMessageFilter*>(context));
    static const std::uint32_t keys[5] = {0xea563ab0, 0x8adc29d4, 0x6a8cc9d6, 0x2aa6e8ad, 0x0ab3f6db};
    for (std::uint32_t key : keys) {
        cIGZVariant* variant = nullptr;
        if (!win->GetParam(key, &variant)) continue;
        win->SetParam(key, nullptr);
        if (variant) variant->Release();
    }
    win->EnumChildren(kEnumRowIid, &cSC4ViewInputControlQuery::EnumFuncClearFilter, context);
    return true;
}

// @ 0x1002db566
bool cSC4ViewInputControlQuery::EnumFuncSetValue(cIGZWin* win, std::uint32_t id, void* child, void* context) {
    auto* window = static_cast<cIGZWin*>(child);
    const std::uint32_t index = id & 0xfff;
    bool handled = false;
    if ((id & 0x1000) != 0 && win && context && index <= 0x7e) {
        tSetOrGetFn fn = kRowFunctions[index];
        if (fn) handled = fn(win, id, true, static_cast<cISC4Occupant*>(context));
    }
    // The original recurses into the child only when the row handler declined (or there is none for the id).
    if (!handled && window) window->EnumChildren(kEnumRowIid, &cSC4ViewInputControlQuery::EnumFuncSetValue, context);
    return true;
}

// @ 0x1002db5d8: sliders and bitmaps (interfaces 0x231a1c57 / 0x8810) are only released, never updated.
bool cSC4ViewInputControlQuery::EnumFuncUpdateValue(cIGZWin* win, std::uint32_t id, void* child, void* context) {
    auto* window = static_cast<cIGZWin*>(child);
    if (window) {
        void* other = nullptr;
        if (window->QueryInterface(0x231a1c57, &other) || window->QueryInterface(0x8810, &other)) {
            static_cast<cIGZUnknown*>(other)->Release();
            return true;
        }
    }
    const std::uint32_t index = id & 0xfff;
    bool handled = false;
    if ((id & 0x1000) != 0 && win && context && index <= 0x7e) {
        tSetOrGetFn fn = kRowFunctions[index];
        if (fn) handled = fn(win, id, true, static_cast<cISC4Occupant*>(context));
    }
    if (!handled && window) window->EnumChildren(kEnumRowIid, &cSC4ViewInputControlQuery::EnumFuncUpdateValue, context);
    return true;
}

// @ 0x1002db69e
bool cSC4ViewInputControlQuery::EnumFuncGetValue(cIGZWin* win, std::uint32_t id, void* child, void* context) {
    auto* window = static_cast<cIGZWin*>(child);
    const std::uint32_t index = id & 0xfff;
    bool handled = false;
    if ((id & 0x1000) != 0 && win && context && index <= 0x7e) {
        tSetOrGetFn fn = kRowFunctions[index];
        if (fn) handled = fn(win, id, false, static_cast<cISC4Occupant*>(context));
    }
    if (!handled && window) window->EnumChildren(kEnumRowIid, &cSC4ViewInputControlQuery::EnumFuncGetValue, context);
    return true;
}

// @ 0x1002db70e / 0x1002db748
bool cSC4ViewInputControlQuery::EnumFuncNeighborDialogSetValue(cIGZWin* win, std::uint32_t id, void* cellInfo,
                                                              void* occupant) {
    (void)occupant;
    SetOrGetControlValue(win, id, true, static_cast<tCellInfo*>(cellInfo));
    return true;
}
bool cSC4ViewInputControlQuery::EnumFuncNeighborDialogGetValue(cIGZWin* win, std::uint32_t id, void* cellInfo,
                                                              void* occupant) {
    (void)occupant;
    SetOrGetControlValue(win, id, false, static_cast<tCellInfo*>(cellInfo));
    return true;
}

// --- fire / police / jail / park rows ----------------------------------------------------------------------

// @ 0x1002d5de4
bool cSC4ViewInputControlQuery::SetOrGetFireStationEffectiveness(cIGZWin* win, std::uint32_t id, bool set,
                                                                 cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryFireSim* fire = host ? host->FireSim() : nullptr;
    if (!fire) return false;
    return SetPerformanceMeterValue(win, id, fire->StationEffectiveness(occupant));
}

// @ 0x1002d5e1e
bool cSC4ViewInputControlQuery::SetOrGetFireStationLocalFunding(cIGZWin* win, std::uint32_t id, bool set,
                                                                cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    IQueryFireSim* fire = host ? host->FireSim() : nullptr;
    if (!fire) return false;
    if (set) {
        int cost = 0;
        float fraction = 0.0f;
        fire->StationFunding(occupant, &cost, &fraction);
        // The compiled code adds 1.5 * 2^23 in float and reads the mantissa back: fraction * 100 rounded to the nearest even integer.
        SetSliderControlValue(win, id, 1, 0, RoundedPercentForSlider(fraction), cost, occupant,
                              &cSC4ViewInputControlQuery::SetOrGetFireStationLocalFunding, nullptr, 0);
    } else {
        int value = 0;
        GetSliderControlValue(win, id, value);
        fire->SetStationFunding(occupant, static_cast<float>(value) * 0.01f);
    }
    return true;
}

// @ 0x1002d5f02 (and the police patrol-car twin at 0x1002d617a)
bool cSC4ViewInputControlQuery::SetOrGetFireStationEngineCount(cIGZWin* win, std::uint32_t id, bool set,
                                                               cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryFireSim* fire = host ? host->FireSim() : nullptr;
    if (!fire) return false;
    std::uint32_t availability = 0, baseCall = 0, count = 0;
    if (!fire->DispatchInfo(occupant, &availability, &baseCall, &count)) return false;
    return SetStaticControlNumeric(win, id, count);
}

// @ 0x1002d5f82
bool cSC4ViewInputControlQuery::SetOrGetFireStationMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set,
                                                                   cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryFireSim* fire = host ? host->FireSim() : nullptr;
    if (!fire) return false;
    int cost = 0;
    fire->StationFunding(occupant, &cost, nullptr);
    return SetStaticControlSimoleons(win, id, cost);
}

// @ 0x1002d5fd8
bool cSC4ViewInputControlQuery::SetOrGetPoliceStationEffectiveness(cIGZWin* win, std::uint32_t id, bool set,
                                                                   cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    return SetPerformanceMeterValue(win, id, police->StationEffectiveness(occupant));
}

// @ 0x1002d6016
bool cSC4ViewInputControlQuery::SetOrGetPoliceStationArrestCount(cIGZWin* win, std::uint32_t id, bool set,
                                                                 cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    return SetStaticControlNumeric(win, id, static_cast<std::uint32_t>(police->ArrestCount(occupant)));
}

// @ 0x1002d6056
bool cSC4ViewInputControlQuery::SetOrGetPoliceStationCrimeCount(cIGZWin* win, std::uint32_t id, bool set,
                                                                cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    return SetStaticControlNumeric(win, id, static_cast<std::uint32_t>(police->CrimeCount(occupant)));
}

// @ 0x1002d6096
bool cSC4ViewInputControlQuery::SetOrGetPoliceStationLocalFunding(cIGZWin* win, std::uint32_t id, bool set,
                                                                  cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    if (set) {
        int cost = 0;
        float fraction = 0.0f;
        police->StationFunding(occupant, &cost, &fraction);
        SetSliderControlValue(win, id, 1, 0, RoundedPercentForSlider(fraction), cost, occupant,
                              &cSC4ViewInputControlQuery::SetOrGetPoliceStationLocalFunding, nullptr, 0);
    } else {
        int value = 0;
        GetSliderControlValue(win, id, value);
        police->SetStationFunding(occupant, static_cast<float>(value) * 0.01f);
    }
    return true;
}

// @ 0x1002d617a
bool cSC4ViewInputControlQuery::SetOrGetPoliceStationPatrolCarCount(cIGZWin* win, std::uint32_t id, bool set,
                                                                    cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    std::uint32_t availability = 0, baseCall = 0, count = 0;
    if (!police->DispatchInfo(occupant, &availability, &baseCall, &count)) return false;
    return SetStaticControlNumeric(win, id, count);
}

// @ 0x1002d61fa
bool cSC4ViewInputControlQuery::SetOrGetPoliceStationMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set,
                                                                     cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    int cost = 0;
    police->StationFunding(occupant, &cost, nullptr);
    return SetStaticControlSimoleons(win, id, cost);
}

// @ 0x1002d6250: the jail fills from the design capacity (0.8 * capacity) to the absolute one; the meter runs from 1
// down to 0 across that band.
bool cSC4ViewInputControlQuery::SetOrGetJailCondition(cIGZWin* win, std::uint32_t id, bool set,
                                                      cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    float meter = police->JailEfficiency();
    const int capacity = police->JailCapacity(occupant);
    if (capacity == 0) {
        meter = 0.0f;
    } else {
        const float capacityF = static_cast<float>(capacity);
        const std::uint32_t inmates = static_cast<std::uint32_t>(static_cast<std::int64_t>(meter * capacityF));
        if (static_cast<float>(inmates) <= capacityF * 0.8f) {
            meter = 1.0f;
        } else {
            const std::uint32_t freeCells = static_cast<std::uint32_t>(capacity) - inmates;
            if (static_cast<std::uint32_t>(capacity) < inmates || freeCells == 0) {
                meter = 0.0f;
            } else {
                meter = static_cast<float>(freeCells);
            }
            meter = meter / (capacityF * 0.19999999f);
        }
    }
    return SetPerformanceMeterValue(win, id, meter);
}

// @ 0x1002d631c
bool cSC4ViewInputControlQuery::SetOrGetJailCellCount(cIGZWin* win, std::uint32_t id, bool set,
                                                      cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    const int capacity = police->JailCapacity(occupant);
    return SetStaticControlNumeric(
        win, id, static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(capacity) * 0.8f)));
}

// @ 0x1002d636a
bool cSC4ViewInputControlQuery::SetOrGetJailInmateCount(cIGZWin* win, std::uint32_t id, bool set,
                                                        cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    const float efficiency = police->JailEfficiency();
    const int capacity = police->JailCapacity(occupant);
    return SetStaticControlNumeric(
        win, id, static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(capacity) * efficiency)));
}

// @ 0x1002d63e6
bool cSC4ViewInputControlQuery::SetOrGetJailMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set,
                                                            cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryPoliceSim* police = host ? host->PoliceSim() : nullptr;
    if (!police) return false;
    return SetStaticControlSimoleons(win, id, police->JailMaintenance(occupant));
}

// @ 0x1002d6426: the park row is the generic occupant-cost row.
bool cSC4ViewInputControlQuery::SetOrGetParkMaintenanceCost(cIGZWin* win, std::uint32_t id, bool set,
                                                            cISC4Occupant* occupant) {
    return SetOrGetOccupantCost(win, id, set, occupant);
}

// @ 0x1002d6434: sum the monthly cost of every budget value the occupant's property vector names.
bool cSC4ViewInputControlQuery::SetOrGetOccupantCost(cIGZWin* win, std::uint32_t id, bool set,
                                                     cISC4Occupant* occupant) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryBudgetSim* budget = host ? host->BudgetSim() : nullptr;
    if (!budget || !occupant) return false;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    std::vector<std::uint32_t> values;
    if (!holder || !cSCPropertyHelp::GetPropertyValue(holder, kPropBudgetValues, values)) return false;
    std::int64_t total = 0;
    for (std::uint32_t value : values) {
        std::int64_t cost = 0;
        if (budget->OccupantValueCost(holder, value, cost)) total += cost;
    }
    return SetStaticControlSimoleons(win, id, total);
}

// --- neighbour-connection rows -----------------------------------------------------------------------------

// @ 0x1002d906e
IQueryNeighborCity* cSC4ViewInputControlQuery::FindCityOnOtherSideOfConnection(int connectionType, int x, int z) {
    IQueryHost* host = QueryHost();
    IQueryNeighborsSim* neighbors = host ? host->NeighborsSim() : nullptr;
    if (!neighbors || !neighbors->IsValidConnectionCell(x, z)) return nullptr;
    if (!neighbors->FindConnection(static_cast<std::uint32_t>(connectionType), x, z)) return nullptr;
    return neighbors->FindCityOnOtherSide(x, z);
}

// @ 0x1002d90d6
bool cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityName(cIGZWin* win, std::uint32_t id, bool set,
                                                                          int x, int z, int connectionType) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    if (!host) return false;
    cRZString text;
    host->LoadString(kStrNeighborCityGroup, kStrNeighborCityInstance, text);
    if (IQueryNeighborCity* city = FindCityOnOtherSideOfConnection(connectionType, x, z)) {
        city->GetCityName(text);
        city->Release();
    }
    return SetStaticControlText(win, id, text);
}

// @ 0x1002d9184 (quirk: a missing city answers false, not the usual true)
bool cSC4ViewInputControlQuery::SetOrGetNeighborConnectionNeighborCityMayorName(cIGZWin* win, std::uint32_t id,
                                                                               bool set, int x, int z,
                                                                               int connectionType) {
    if (!set) return true;
    IQueryNeighborCity* city = FindCityOnOtherSideOfConnection(connectionType, x, z);
    if (!city) return false;
    cRZString text;
    city->GetMayorName(text);
    city->Release();
    return SetStaticControlText(win, id, text);
}

// @ 0x1002d922e: the congestion map byte scaled to a percentage, clamped to the city's cell counts.
bool cSC4ViewInputControlQuery::SetOrGetNeighborConnectionCongestion(cIGZWin* win, std::uint32_t id, bool set,
                                                                     int x, int z) {
    if (!set) return true;
    IQueryHost* host = QueryHost();
    IQueryCity* city = host ? host->City() : nullptr;
    IQueryTrafficSim* traffic = host ? host->TrafficSim() : nullptr;
    if (!city || !traffic) return false;
    const int cellsX = city->CellCountX();
    const int cellsZ = city->CellCountZ();
    if (x < 0) x = 0;
    if (x > cellsX - 1) x = cellsX - 1;
    int cellZ = 0;
    if (z >= 0) cellZ = z;
    if (cellZ > cellsZ - 1) cellZ = cellsZ - 1;
    const std::uint8_t congestion = traffic->CongestionCell(x, cellZ);
    const double percent = (static_cast<double>(congestion) / 255.0) * 100.0;
    return SetStaticControlNumericPercent(win, id, static_cast<std::uint32_t>(static_cast<std::int64_t>(percent)));
}

// --- text builders that are still empty stubs (each marked PARTIAL; QUERY-6) ------------------------------------
// They are declared so the message plumbing compiles and leave their strings empty.
// PARTIAL stub (QUERY-6)
void cSC4ViewInputControlQuery::GetBuildingOccupantTrafficInfo(cRZString&, cRZString&, std::uint32_t& icon) {
    icon = 0;
}
// PARTIAL stub (QUERY-6)
void cSC4ViewInputControlQuery::GetNetworkOccupantTrafficInfo(cRZString&, cRZString&) {}
// PARTIAL stub (QUERY-6)
void cSC4ViewInputControlQuery::GetBuildingOccupantTipInfo(cRZString&, cRZString&, std::uint32_t& icon,
                                                           std::uint32_t& meter, float&) {
    icon = 0;
    meter = 0;
}
// @ 0x1002c5558 / 0x1002c55ae: the property's name, else the exemplar's localized name, else the plain property 0x20
void cSC4ViewInputControlQuery::GetPropOccupantSummaryInfo(cRZString& title) {
    IQueryHost* host = QueryHost();
    cISCPropertyHolder* holder = occupant_->AsPropertyHolder();
    if (cSCPropertyHelp::GetPropertyValue(holder, 0x899afbad, title)) return;
    if (host && host->LoadExemplarString(title, holder, 0x8a416a99)) return;
    cSCPropertyHelp::GetPropertyValue(holder, 0x20, title);
}
void cSC4ViewInputControlQuery::GetFloraOccupantSummaryInfo(cRZString& title) { GetPropOccupantSummaryInfo(title); }

// @ 0x1002c515c: the network piece's kind as a word, tried in the order road(2), rail(8), ... ; the pipe network adds the pipe counts in debug
void cSC4ViewInputControlQuery::GetNetworkOccupantSummaryInfo(cRZString& title, cRZString& text) {
    IQueryHost* host = QueryHost();
    IQueryNetworkOccupant* network = host && occupant_ ? host->NetworkOccupant(occupant_) : nullptr;
    if (!network) return;
    // {network type, string instance}; the first type the piece is of wins
    static constexpr struct { std::uint32_t type, instance; } kKinds[] = {
        {2, 0xaa5589fb}, {8, 0x8b8c5cbd}, {0xc, 0x2be09c9e}, {0, 0x6a5589d4}, {1, 0x4a5589f3}, {10, 0x4bd9fdd8},
        {0xb, 0xcbe09c9b}, {9, 0x0be09ca2}, {3, 0x8a558a04}, {4, 0x0a558a0a}, {7, 0x8a558a10}, {6, 0x8b8c5cd0}};
    for (const auto& kind : kKinds) {
        if (!network->IsOfType(kind.type)) continue;
        host->LoadString(kStringGroup, kind.instance, title);
        if (kind.type == 4 && IsDebugQueryEnabled()) {   // the pipe network
            IQueryPlumbingSim* plumbing = host->PlumbingSim();
            const int distressed = plumbing ? plumbing->GetNumDistressedPipes() : 0;
            const int pipes = plumbing ? plumbing->GetNumPipes() : 0;
            text.Sprintf("Distressed Pipes: %d / %d\n", distressed, pipes);
        }
        break;
    }
    network->Release();
}

// @ 0x1002cfae0: the wealth of the lot's building type (its profile), else of the lot's occupants
std::uint32_t cSC4ViewInputControlQuery::GetBuildingWealth(IQueryLot* lot) {
    const std::uint32_t buildingType = lot->GetBuildingType(false);
    if (buildingType != 0) {
        IQueryHost* host = QueryHost();
        return host ? host->BuildingTypeWealth(buildingType) : 0;
    }
    return static_cast<std::uint32_t>(lot->GetOccupantWealth());
}

// @ 0x1002cfb90: the sentences the terrain tip shows for a lot, each with an importance key (the multimap keeps equal keys in insertion order):
// a power-line-less lot needing power, pollution / garbage / school / hospital / crime verdicts, the traffic and trip length verdicts that
// depend on the zone (residential 1-3, commercial 4-6, industrial 7-9), a vacated or abandoned lot and a lot that stays below its building's wealth.
void cSC4ViewInputControlQuery::GetLotHeadlines(IQueryLot* lot, std::multimap<int, cRZString>& headlines) {
    IQueryHost* host = QueryHost();
    if (!host || !host->City()) return;
    bool needsPower = lot->GetNeedsPower();
    float center[3] = {0.0f, 0.0f, 0.0f};
    lot->GetCenter(center);
    int cellX = 0, cellZ = 0;
    host->City()->PositionToCell(center[0], center[2], cellX, cellZ);
    int buildingWealth = 0;
    const std::uint32_t buildingType = lot->GetBuildingType(false);
    IQueryGrowthDeveloper* developer = host->FindBestDeveloper(lot);
    const std::uint32_t zone = host->ZoneTypeAt(cellX, cellZ);
    if (!developer) return;
    if (buildingType == 0) needsPower = true;
    else buildingWealth = static_cast<int>(GetBuildingWealth(lot));

    auto add = [&](std::uint32_t instance, int key) {
        cRZString text;
        host->LoadString(kStringGroup, instance, text);
        headlines.emplace(key, text);
    };
    std::uint32_t reading = 0;
    if (cISCPropertyHolder* holder = lot->AsPropertyHolder()) {
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x8999b20b, reading) && reading != 0) add(0x0a5bba2a, 0x5a);
    }
    if (needsPower && !lot->GetHasPower()) add(0x6a5bbba0, 100);
    int value = 0;
    if (GetSubjectivePollution(developer, cellX, cellZ, value) == 2) add(0x8a5bbbf1, value);
    if (GetSubjectiveGarbage(developer, cellX, cellZ, value) == 2) add(0x0a82439f, value);
    std::uint32_t verdict = GetSubjectiveSchoolGrade(developer, cellX, cellZ, value);
    if (verdict == 0) add(0xca5bbc43, value);
    else if (verdict == 2) add(0x2a5bbc39, value);
    verdict = GetSubjectiveHospitalGrade(developer, cellX, cellZ, value);
    if (verdict == 0) add(0x2a5bbc4d, value);
    else if (verdict == 2) add(0x6a5bbc57, value);
    verdict = GetSubjectiveCrimeLevel(developer, cellX, cellZ, value);
    if (verdict == 0) add(0x4a5bbc61, value);
    else if (verdict == 2) add(0x6a5bbc73, value);

    // the trip verdicts by zone group: residential (bits 0xe), commercial (0x70) and industrial (0x380) words differ; other zones skip them
    if (zone < 10) {
        const bool residential = ((0xeu >> zone) & 1) != 0;
        const bool commercial = !residential && ((0x70u >> zone) & 1) != 0;
        const bool industrial = !residential && !commercial && ((0x380u >> zone) & 1) != 0;
        if (residential || commercial || industrial) {
            verdict = GetSubjectiveTrafficLevel(developer, cellX, cellZ, value);
            if (verdict == 0) add(commercial ? 0x6aa87825 : 0x2a5bbc91, value);
            else if (verdict == 2) add(commercial ? 0x0aa87852 : 0x6a5bbc80, value);
            verdict = GetSubjectiveTripLength(developer, cellX, cellZ, value);
            if (residential) {
                if (verdict == 0) add(lot->GetAbandonReason() == 4 ? 0xaa5bbc97 : 0xaa5bbc9f, value);
                else if (verdict == 2) add(0xaa5bbc97, value);
            } else {
                if (verdict == 0) add(0xcaa8785c, value);
                else if (verdict == 2) add(0x2aa8786b, value);
            }
        }
    }
    const int state = lot->GetState();
    if (state == 4) add(0x0a5fbdf2, 0);
    else if (state == 2) add(0x4a5fbdc4, 0);
    if (buildingWealth <= lot->GetOccupantWealth()) return;
    add(0x6a5fbed8, 1);
}

// @ 0x1002c5604: over land the title is "terrain", the zone type names the ground below it (zone 0 and 0xf replace the
// title instead of filling the text, as compiled), then the three most important headlines of the lot under the cell are appended to the text.
void cSC4ViewInputControlQuery::GetTerrainSummaryInfo(cRZString& title, cRZString& text) {
    IQueryHost* host = QueryHost();
    if (!host || !host->City()) return;
    int cellX = 0, cellZ = 0;
    host->City()->PositionToCell(pickX_, pickZ_, cellX, cellZ);
    if (host->TerrainCellHasWater(cellX, cellZ)) return;
    const std::uint32_t zone = host->ZoneTypeAt(cellX, cellZ);
    host->LoadString(kStringGroup, 0x6a5bbca6, title);
    static constexpr std::uint32_t kZoneStrings[16] = {0xaa5bbde4, 0x0a5bbdef, 0xaa5bbe08, 0x8a5bbe12, 0x6a5bbe1f, 0x6a5bbe29, 0xaa5bbe33, 0x0a5bbe3c,
                                                       0xca5bbe4a, 0x0a5bbe51, 0x6a5bbe61, 0xaa5bbe69, 0x2a5bbe6f, 0xca5bbe78, 0x2a5bbe80, 0x2a5bbe85};
    if (zone < 16) host->LoadString(kStringGroup, kZoneStrings[zone], (zone == 0 || zone == 0xf) ? title : text);
    IQueryLot* lot = host->LotManager() ? host->LotManager()->GetLotAtCell(cellX, cellZ, 0) : nullptr;
    if (!lot) return;
    std::multimap<int, cRZString> headlines;
    GetLotHeadlines(lot, headlines);
    int appended = 0;
    for (auto it = headlines.rbegin(); it != headlines.rend() && appended < 3; ++it, ++appended) {
        if (text.Strlen() != 0) text.AppendString(cRZString("\n"));
        text.AppendString(it->second);
    }
}
// PARTIAL stub (QUERY-6)
// @ 0x1002ccd38 (i386 0x00219cde): the click of the building query. The picked occupant (or the occupant of the lot under
// the cursor) is replaced by its lot's building; its query sound plays and its dialog opens: a network piece at the city
// edge opens the neighbor connection dialog, an occupant with a query UI (property 0x2a499f85) its script dialog, and a
// landfill / airport / seaport zone the zone dialog; anything else plays the "nothing to query" sound.
//   PARTIAL: the three debug description lines (the cell / piece / building texts the debug query shows) stay at their
//   defaults; the coverage rings of the queried building are not drawn while its dialog is open.
void cSC4ViewInputControlQuery::DescribePick(int x, int y, cRZString& line1, cRZString& line2, cRZString& line3, bool detailed) {
    (void)detailed;
    line1 = cRZString("Query");
    line2 = cRZString("Query extra: none");
    line3 = cRZString("Query extra2: none");
    IQueryHost* host = QueryHost();
    IQueryView* view = host ? host->QueryView() : nullptr;
    if (!view) return;
    cISC4Occupant* occupant = nullptr;
    IQueryLotManager* lots = host->LotManager();
    IOccupantFilter* filter = host->Renderer() ? host->Renderer()->CreateFilter(1) : nullptr;   // cSC4QueriableOccupantFilter
    // The lot's building stands in for the picked occupant (or for the empty cell under the cursor).
    auto takeBuilding = [&](IQueryLot* lot) {
        if (!lot) return;
        lot->AddRef();
        if (cISC4Occupant* building = lot->GetBuildingOccupant()) {
            building->AddRef();
            if (occupant) occupant->Release();
            occupant = building;
        }
        lot->Release();
    };
    if (filter && PickQueriableOccupant(x, y, &occupant, filter)) {
        if (lots) takeBuilding(lots->GetLotForOccupant(occupant));
    } else {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        if (view->PickTerrain(x, y, pos, false) && host->City() && lots) {
            int cellX = 0, cellZ = 0;
            host->City()->PositionToCell(pos[0], pos[2], cellX, cellZ);
            takeBuilding(lots->GetLotAtCell(cellX, cellZ, 0));
        }
    }
    if (filter) filter->Release();
    if (!occupant) return;

    PlayOccupantQuerySound(occupant);
    bool neighborOnly = false;
    if (IQueryNetworkOccupant* network = host->NetworkOccupant(occupant)) {
        network->Release();
        DoNeighborConnectionQueryDialog(occupant);
    } else {
        void* raw = nullptr;
        if (occupant->QueryInterface(0xa8c86142, &raw) && raw) {   // a neighbor connection occupant
            static_cast<cIGZUnknown*>(raw)->Release();
            DoNeighborConnectionQueryDialog(occupant);
            neighborOnly = true;
        }
    }
    if (!neighborOnly) {
        std::uint32_t queryUi = 0;
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropQueryUi, queryUi)) {
            DoQueryDialog(occupant);
        } else {
            float position[3] = {0.0f, 0.0f, 0.0f};
            occupant->GetPosition(position);
            int cellX = 0, cellZ = 0;
            if (host->City()) host->City()->PositionToCell(position[0], position[2], cellX, cellZ);
            const std::uint32_t zone = host->ZoneTypeAt(cellX, cellZ);
            if (zone <= 0xe && ((1u << zone) & 0x5800u) != 0) {   // 11 landfill, 12 seaport, 14 airport
                DoZoneBasedQueryDialog(zone, static_cast<std::uint32_t>(cellX), static_cast<std::uint32_t>(cellZ));
            } else if (IViewInputHost* base = ViewInputHost()) {
                base->PlaySound(1, kSoundNothingToQuery);
            }
        }
    }
    occupant->Release();
}

namespace {
// The parts the three query dialogs share (each original entry point inlines them): the dialog's window proc and the
// performance meter filter, the row fill (`set` over every row with `setContext`), the dialog tick refresh, the modal loop,
// the rows read back on OK (`get`), and the teardown.
bool RunQueryDialog(cIGZWin* dialog, cISC4Occupant* procOccupant, cIGZWin::EnumChildFn tickFn, void* tickContext1,
                    void* tickContext2, const std::vector<std::pair<cIGZWin::EnumChildFn, void*>>& fills,
                    cIGZWin::EnumChildFn get, void* getContext) {
    IQueryHost* host = QueryHost();
    dialog->PullToFront();
    cSC4QueryDialogWinProc* proc = nullptr;
    void* raw = nullptr;
    if (dialog->QueryInterface(gz::kGZIID_cIGZWinGen, &raw) && raw) {
        auto* gen = static_cast<gz::cIGZWinGen*>(raw);
        proc = new cSC4QueryDialogWinProc(dialog);
        proc->AddRef();
        if (procOccupant) proc->SetOccupant(procOccupant);
        gen->SetWinProc(proc);
        gen->Release();
    }
    static cSC4QueryMessageFilter filter;
    dialog->EnumChildren(kEnumRowIidDialog, &cSC4ViewInputControlQuery::EnumFuncSetFilter, &filter);
    if (procOccupant) procOccupant->AddRef();
    cSC4ViewInputControlQuery::SetEnumSetFunction(tickFn, tickContext1, tickContext2);
    cSC4ViewInputControlQuery::SetQueryDialogWindow(dialog);
    dialog->AddRef();
    for (const auto& fill : fills) dialog->EnumChildren(kEnumRowIidDialog, fill.first, fill.second);
    host->RestoreDialogPlace(dialog);
    const int result = host->RunModalDialog(dialog);
    host->SaveDialogPlace(dialog);
    cSC4ViewInputControlQuery::SetEnumSetFunction(nullptr, nullptr, nullptr);
    if (procOccupant) procOccupant->Release();
    cSC4ViewInputControlQuery::SetQueryDialogWindow(nullptr);
    dialog->Release();
    if (result == 0 && get) dialog->EnumChildren(kEnumRowIidDialog, get, getContext);
    dialog->EnumChildren(kEnumRowIidDialog, &cSC4ViewInputControlQuery::EnumFuncClearFilter, &filter);
    if (proc) proc->SetOccupant(nullptr);
    if (raw) {   // the window proc goes with the window
        static_cast<gz::cIGZWinGen*>(raw)->SetWinProc(nullptr);
    }
    host->CloseDialog(dialog);
    dialog->Release();
    if (proc) proc->Release();
    return result == 0;
}
}  // namespace

// i386 0x00218ed0: the building's own query dialog (script instance = its property 0x2a499f85).
bool cSC4ViewInputControlQuery::DoQueryDialog(cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    std::uint32_t instance = 0;
    cISCPropertyHolder* holder = occupant ? occupant->AsPropertyHolder() : nullptr;
    if (!host || !holder || !cSCPropertyHelp::GetPropertyValue(holder, kPropQueryUi, instance)) return false;
    cIGZWin* dialog = host->CreateWindowFromScript(instance, nullptr, kQueryDialogId);
    if (!dialog) return false;
    return RunQueryDialog(dialog, occupant, &cSC4ViewInputControlQuery::EnumFuncUpdateValue, occupant, nullptr,
                          {{&cSC4ViewInputControlQuery::EnumFuncSetValue, occupant}}, &cSC4ViewInputControlQuery::EnumFuncGetValue,
                          occupant);
}

// i386 0x00219306: the dialog of a landfill (11), seaport (12) or airport (14) zone cell.
bool cSC4ViewInputControlQuery::DoZoneBasedQueryDialog(std::uint32_t zoneType, std::uint32_t cellX, std::uint32_t cellZ) {
    IQueryHost* host = QueryHost();
    if (!host) return false;
    std::uint32_t instance = zoneType;
    if (zoneType == 0xc) {
        instance = 0x4a562da5;
    } else if (zoneType == 0xe) {
        if (IViewInputHost* base = ViewInputHost()) base->PlaySound(1, 0x2a62c927);
        instance = 0xaa561f93;
    } else if (zoneType == 0xb) {
        instance = 0x6a561b3a;
    }
    cIGZWin* dialog = host->CreateWindowFromScript(instance, nullptr, kQueryDialogId);
    if (!dialog) return false;
    tCellInfo info;
    info.x = static_cast<std::int32_t>(cellX);
    info.z = static_cast<std::int32_t>(cellZ);
    info.idOffset = 0x200;
    info.connectionType = 0;
    return RunQueryDialog(dialog, nullptr, &cSC4ViewInputControlQuery::EnumFuncNeighborDialogSetValue, &info, nullptr,
                          {{&cSC4ViewInputControlQuery::EnumFuncNeighborDialogSetValue, &info}},
                          &cSC4ViewInputControlQuery::EnumFuncNeighborDialogGetValue, &info);
}

// i386 0x00219668: a network piece on the city edge that connects to a neighbor: one or two connections of the piece's
// connection type around its cell, in the dialog for that type (one or two connections).
bool cSC4ViewInputControlQuery::DoNeighborConnectionQueryDialog(cISC4Occupant* occupant) {
    IQueryHost* host = QueryHost();
    IQueryCity* city = host ? host->City() : nullptr;
    IQueryNeighborsSim* neighbors = host ? host->NeighborsSim() : nullptr;
    if (!occupant || !city || !neighbors) return false;
    float position[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(position);
    int cellX = 0, cellZ = 0;
    city->PositionToCell(position[0], position[2], cellX, cellZ);
    // The original reads one cell count (city vslot 0x174) for both edges.
    const int last = city->CellCountX() - 1;
    if (cellX != 0 && cellX != last && cellZ != 0 && cellZ != last) return false;

    std::uint32_t connectionType = 0;
    if (IQueryNetworkOccupant* network = host->NetworkOccupant(occupant)) {
        std::uint32_t t = network->IsOfType(6) ? 8 : (network->IsOfType(0) ? 2 : 10);
        if (network->IsOfType(0xc)) t = 0xc;
        if (network->IsOfType(2)) t = 3;
        if (network->IsOfType(7)) t = 6;
        if (network->IsOfType(8)) t = 7;
        if (network->IsOfType(9)) t = 0xb;
        if (network->IsOfType(1)) t = 1;
        if (network->IsOfType(4)) t = 5;
        connectionType = t;
        network->Release();
    } else {
        void* raw = nullptr;
        if (occupant->QueryInterface(0xa8c86142, &raw) && raw) {
            static_cast<cIGZUnknown*>(raw)->Release();
            connectionType = 4;
        }
    }
    static constexpr int kXOffsets[4] = {-1, 0, 1, 0};   // _XOffsets.288346
    static constexpr int kZOffsets[4] = {0, -1, 0, 1};   // _ZOffsets.288347
    std::vector<tCellInfo> cells;
    for (int i = 0; i < 4; ++i) {
        const int x = cellX + kXOffsets[i], z = cellZ + kZOffsets[i];
        if (!neighbors->IsValidConnectionCell(x, z) || !neighbors->FindConnection(connectionType, x, z)) continue;
        tCellInfo info;
        info.x = x;
        info.z = z;
        info.idOffset = static_cast<std::int32_t>(cells.size()) << 9;
        info.connectionType = static_cast<std::int32_t>(connectionType);
        cells.push_back(info);
    }
    if (cells.empty() || cells.size() > 2) return false;
    const bool one = cells.size() == 1;
    std::uint32_t instance = 0;
    switch (connectionType) {
    case 1: case 6: case 7: case 0xb: instance = one ? 0xea566a49u : 0x4a5e7ed3u; break;
    case 2: case 3: case 8: case 10: case 0xc: instance = one ? 0xca566f94u : 0xaa5e14ccu; break;
    case 4: instance = one ? 0x4a566c14u : 0x8a5e7bd2u; break;
    case 5: instance = one ? 0x4a566d6eu : 0x2a5e7490u; break;
    default: break;   // the original leaves the instance unset here
    }
    cIGZWin* dialog = host->CreateWindowFromScript(instance, nullptr, kQueryDialogId);
    if (!dialog) return false;
    std::vector<std::pair<cIGZWin::EnumChildFn, void*>> fills;
    for (tCellInfo& info : cells) fills.push_back({&cSC4ViewInputControlQuery::EnumFuncNeighborDialogSetValue, &info});
    // The original writes no get pass for this dialog.
    return RunQueryDialog(dialog, nullptr, &cSC4ViewInputControlQuery::EnumFuncNeighborDialogSetValue, &cells[0],
                          cells.size() > 1 ? &cells[1] : nullptr, fills, nullptr, nullptr);
}

// ---- cSC4QueryDialogWinProc ------------------------------------------------------------------------------------------------

cSC4QueryDialogWinProc::cSC4QueryDialogWinProc(cIGZWin* dialog) : dialog_(dialog) {   // i386 0x00218e3c
    if (dialog_) dialog_->AddRef();
}

cSC4QueryDialogWinProc::~cSC4QueryDialogWinProc() {
    if (dialog_) dialog_->Release();
    if (occupant_) occupant_->Release();
}

bool cSC4QueryDialogWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc || iid == 1) {
        *out = static_cast<gz::cIGZWinProc*>(this);
        AddRef();
        return true;
    }
    return false;
}

std::uint32_t cSC4QueryDialogWinProc::Release() {
    if (refs_ > 1) return --refs_;
    delete this;
    return 0;
}

bool cSC4QueryDialogWinProc::SetOccupant(cISC4Occupant* occupant) {
    if (occupant_) {
        occupant_->Release();
        occupant_ = nullptr;
    }
    if (occupant) {
        occupant_ = occupant;
        occupant->AddRef();
    }
    return occupant != nullptr;
}

void cSC4QueryDialogWinProc::EndDialog(std::uint32_t result) {
    IQueryHost* host = QueryHost();
    if (host && dialog_) host->EndModalDialog(dialog_, result);
    if (dialog_) {
        dialog_->Release();
        dialog_ = nullptr;
    }
    if (occupant_) {
        occupant_->Release();
        occupant_ = nullptr;
    }
}

bool cSC4QueryDialogWinProc::DoWinMsg(cIGZWin* window, std::uint32_t type, std::uint64_t p1, std::uint64_t p2, std::uint64_t) {
    if (!dialog_ || window != dialog_ || type != 3) return false;
    const std::uint32_t message = static_cast<std::uint32_t>(p1);
    const std::uint32_t id = static_cast<std::uint32_t>(p2);
    if (message == 0x287259f6) {   // a button: OK (0) / Cancel (1)
        if (id < 2) {
            EndDialog(id);
            return true;
        }
        return false;
    }
    if (message != 0x887113a4) return false;   // a slider moved
    void* raw = nullptr;
    if (!window->GetChildAsRecursive(id, gz::kGZIID_cIGZWinSlider, &raw) || !raw) return false;
    auto* slider = static_cast<gz::cIGZWinSlider*>(raw);
    const int value = slider->GetValue();
    cIGZWin* sliderWin = slider->AsIGZWin();
    slider->Release();
    cIGZVariant* variant = nullptr;
    double amount = 0.0;
    if (sliderWin->GetParam(0x2aa6e8ad, &variant) && variant) {   // GetParam answers a counted reference
        amount = static_cast<double>(variant->GetValSint64());
        variant->Release();
    }
    cSC4ViewInputControlQuery::SetStaticControlSimoleons(window, id + 0x100,
                                                         static_cast<std::int64_t>(amount * (static_cast<double>(value) / 100.0)));
    variant = nullptr;
    if (sliderWin->GetParam(0x6a8cc9d6, &variant) && variant) {
        auto callback = reinterpret_cast<cSC4ViewInputControlQuery::tSetOrGetFn>(variant->GetValVoidPtr());
        variant->Release();
        if (callback && occupant_) callback(window, sliderWin->GetID(), false, occupant_);
    }
    variant = nullptr;
    if (sliderWin->GetParam(0x0ab3f6db, &variant) && variant) {   // the rows that follow the slider
        std::uint32_t size = variant->GetCount();   // bytes
        std::vector<std::uint32_t> ids(size / sizeof(std::uint32_t));
        size = static_cast<std::uint32_t>(ids.size() * sizeof(std::uint32_t));
        if (!ids.empty()) variant->GetValVoid(ids.data(), size);
        variant->Release();
        for (std::uint32_t row : ids) {
            const std::uint32_t index = row & 0xfff;
            if ((row & 0x1000) == 0 || index > 0x7e || !occupant_) continue;
            if (cSC4ViewInputControlQuery::tSetOrGetFn fn = cSC4ViewInputControlQuery::kRowFunctions[index]) fn(window, row, true, occupant_);
        }
    }
    return false;
}
// PARTIAL stub (QUERY-6)
void cSC4ViewInputControlQuery::GetOccupantPaths() {}
// PARTIAL stub (QUERY-6)
void cSC4ViewInputControlQuery::ShowAllRoutesThatIntersectCityRect(const SC4CellRect&) {}
// PARTIAL stub (QUERY-6)
void cSC4ViewInputControlQuery::WriteDetailedDebugInfo(int, int) {}

}  // namespace sc4
