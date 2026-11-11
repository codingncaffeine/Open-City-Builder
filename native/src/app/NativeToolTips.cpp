// cSC4ToolTipManager (Mac i386 0x0051ce5c..0x00524e40: Init 0x0051d75e, DoMessage 0x0051f898, Shutdown 0x0051d2d0) on the
// native application, plus the buffer service every cSC4WinCalloutBox needs to draw (the catalog's item callouts, the
// query tool's hover callout and the city view's cursor text box use the same window).
//
// The original is a framework service listening on the message server for the tip messages the windows post
// (0x22c010d0 start, 0x22c010d1 update, 0x22c010d2 hide, 0x22c010d8 hide all). In this port the buttons hand their tip
// requests to gz::IWinButtonHost (NativeAudio.cpp), which forwards here. Its cRZCallbackTimer (750 ms period and
// resolution, posting 0xca56dd76) is counted on the frame loop: the SDL timer thread must not touch the windows.
//
// Behaviour kept from the original:
// - the tip text is split at the first "|": with the Expanded Tool Tips preference (SL::spPreferences +0xf0a) on and both
//   halves present, the box shows a title and a body on the large skin (0x14416190, margins 8,8, avoidance +-28);
//   otherwise one line (the part before "|" when there is one, else the whole text) on the small skin (0x14416192,
//   margins 5,3, avoidance -4..20);
// - the box appears one timer period after the request, at the pointer, and every later period it moves to where the
//   pointer is and is pulled to the front; a text with "ESRB" in its first part shows at once and does not follow;
// - hiding stops the timer and hides the box.
#include "app/NativeToolTips.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "gz/Buffer.h"
#include "gz/WinInterfaces.h"
#include "rz/cRZString.h"
#include "sc4/SLBuffer.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinCalloutBox.h"

namespace native {
namespace {

constexpr std::uint32_t kTipTextFont = 0xa85f1a83, kTipTitleFont = 0xe9c86c9d;
constexpr std::uint32_t kTipImageGroup = 0x46a006b0, kTipImageExpanded = 0x14416190, kTipImageSimple = 0x14416192;
constexpr std::size_t kPrefsExpandedToolTips = 0xf0a;
constexpr double kTipPeriodMs = 750.0;   // SetPeriod(0x2ee) / SetResolution(0x2ee)

bool Debug() { static const bool on = std::getenv("SC4_TIPDEBUG") != nullptr; return on; }
double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// RZ::GS() vslot 0x20 for the callout boxes: a fresh, uninitialised buffer (the box sizes it itself).
class CalloutServices final : public sc4::IWinCalloutBoxServices {
public:
    cIGZBuffer* CreateBuffer() override { auto* buffer = new cGZBuffer(); buffer->AddRef(); return buffer; }
};
CalloutServices g_calloutServices;

class ToolTipManager {
public:
    void SetManager(cIGZWinMgr* manager) { manager_ = manager; }

    // cSC4ToolTipManager::Init: the callout window under the main window, hidden, with its fonts; the two skins.
    bool Init() {
        if (initialized_) return true;
        if (!manager_ || !manager_->GetMainWindow()) return false;
        auto* box = new sc4::cSC4WinCalloutBox(manager_);
        box->AddRef();
        box_ = box;
        box_->SetTextFontStyle(kTipTextFont);     // vslot 0x2c
        box_->SetTitleFontStyle(kTipTitleFont);   // vslot 0x28
        box_->AsIGZWin()->HideWindow();           // window vslot 0x118
        expanded_ = LoadSkin(kTipImageExpanded);
        if (expanded_) box_->SetBackgroundImage(expanded_, -1, -1);   // vslot 0x10
        manager_->GetMainWindow()->ChildAdd(box_->AsIGZWin());        // main window vslot 0x38
        simple_ = LoadSkin(kTipImageSimple);
        initialized_ = true;
        if (Debug()) std::fprintf(stderr, "tip: init skins %p %p\n", static_cast<void*>(expanded_), static_cast<void*>(simple_));
        return true;
    }

    // DoMessage 0x22c010d0 / 0x22c010d1.
    void Show(const cIGZString& tip) {
        if (!Init()) return;
        cRZString rest(tip);
        const cRZString first = rest.SplitToken("|");
        const std::uint8_t* preferences = SL::spPreferences;
        const bool expanded = preferences && preferences[kPrefsExpandedToolTips] != 0 && rest.Strlen() != 0 && first.Strlen() != 0;
        if (!expanded) {
            box_->SetTitle(cRZString());                              // vslot 0x18
            box_->SetText(first.Strlen() != 0 ? first : rest);        // vslot 0x20
            box_->SetBackgroundImage(simple_, 5, 3);
            box_->SetAvoidanceRect(cRZRect{-4, -4, 20, 20});           // vslot 0x44
        } else {
            box_->SetTitle(first);
            box_->SetText(rest);
            box_->SetBackgroundImage(expanded_, 8, 8);
            box_->SetAvoidanceRect(cRZRect{-28, -28, 28, 28});
        }
        if (Debug()) std::fprintf(stderr, "tip: show '%s' | '%s' expanded %d\n", first.ToChar(), rest.ToChar(), int(expanded));
        showing_ = true;
        if (first.FindChars("ESRB", 0, true) == static_cast<std::size_t>(-1)) {
            timerRunning_ = true;                                     // Stop + StartPeriodic
            nextTick_ = NowMs() + kTipPeriodMs;
        } else {
            timerRunning_ = false;
            OnTimer();                                                // static_tip_timer_callback(0) at once
        }
    }

    // DoMessage 0x22c010d2 / 0x22c010d8.
    void Hide() {
        if (!initialized_) return;
        timerRunning_ = false;
        cIGZWin* window = box_->AsIGZWin();
        if (showing_ && Debug()) std::fprintf(stderr, "tip: hide\n");
        window->HideWindow();                 // vslot 0x118
        window->InvalidateSelfAndParents();   // vslot 0x170
        showing_ = false;
    }

    void Tick() {
        if (!timerRunning_) return;
        const double now = NowMs();
        if (now < nextTick_) return;
        nextTick_ = now + kTipPeriodMs;
        OnTimer();
    }

private:
    // DoMessage 0xca56dd76: follow the pointer, come to the front, show.
    void OnTimer() {
        if (!showing_ || !box_) return;
        cIGZWin* main = manager_->GetMainWindow();
        int x = 0, y = 0;
        manager_->GetCursorPos(main, x, y);   // WM vslot 0x84
        box_->SetPointOfInterest(cRZPoint{x, y});   // vslot 0x3c
        cIGZWin* window = box_->AsIGZWin();
        window->PullToFront();                // vslot 0x5c
        if (!window->IsVisible()) window->ShowWindow();
        if (Debug()) std::fprintf(stderr, "tip: at %d,%d visible %d\n", x, y, int(window->IsVisible()));
    }

    static cIGZBuffer* LoadSkin(std::uint32_t instance) {
        cSLBuffer image(kSLBitmapResourceType, kTipImageGroup, instance, 1, false);
        if (!image.Load() || !image.GetBuffer()) return nullptr;
        cIGZBuffer* buffer = image.GetBuffer();
        buffer->AddRef();   // held as long as the manager (the original keeps both in cRZAutoRefCount members)
        return buffer;
    }

    cIGZWinMgr* manager_ = nullptr;
    sc4::cISC4WinCalloutBox* box_ = nullptr;   // +0xc
    cIGZBuffer* expanded_ = nullptr;           // +0x10
    cIGZBuffer* simple_ = nullptr;             // +0x14
    bool initialized_ = false;                 // +8
    bool showing_ = false;                     // +9
    bool timerRunning_ = false;                // the timer at +0x18
    double nextTick_ = 0;
};
ToolTipManager g_tips;

}  // namespace

void InstallToolTips(cIGZWinMgr* manager) {
    sc4::SetCalloutBoxServices(&g_calloutServices);
    g_tips.SetManager(manager);
}
void ShowToolTip(const cIGZString& text) { g_tips.Show(text); }
void HideToolTip() { g_tips.Hide(); }
void TickToolTips() { g_tips.Tick(); }

}  // namespace native
