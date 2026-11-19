#include "sc4/ToolTipManager.h"

#include <cstdio>
#include <cstdlib>

#include "gz/Buffer.h"
#include "gz/RZServices.h"
#include "gz/WinBtn.h"
#include "gz/WinInterfaces.h"
#include "rz/cRZString.h"
#include "sc4/SLBuffer.h"
#include "sc4/ServiceLocator.h"
#include "sc4/WinCalloutBox.h"

namespace {
constexpr std::uint32_t kTipTextFont = 0xa85f1a83, kTipTitleFont = 0xe9c86c9d;
constexpr std::uint32_t kTipImageGroup = 0x46a006b0, kTipImageExpanded = 0x14416190, kTipImageSimple = 0x14416192;
constexpr std::size_t kPrefsExpandedToolTips = 0xf0a;
constexpr std::uint32_t kTipMessages[] = {kMsgToolTipTimer, kMsgToolTipShow, kMsgToolTipUpdate, kMsgToolTipHide, kMsgToolTipHideAll,
                                          0x22c010d5, 0x22c010d6};

// cSLBuffer(key {0x856ddbac, 0x46a006b0, instance}, 1, false): the skin, held by the manager.
cIGZBuffer* LoadSkin(std::uint32_t instance) {
    cSLBuffer image(kSLBitmapResourceType, kTipImageGroup, instance, 1, false);
    if (!image.Load() || !image.GetBuffer()) return nullptr;
    cIGZBuffer* buffer = image.GetBuffer();
    buffer->AddRef();
    return buffer;
}
}  // namespace

cSC4ToolTipManager::~cSC4ToolTipManager() {
    Shutdown();
    if (simpleImage_) simpleImage_->Release();
    if (expandedImage_) expandedImage_->Release();
    if (box_) box_->Release();
}

bool cSC4ToolTipManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != 1 && iid != kGZIID_cIGZWinToolTipMgr) return false;
    *out = this;
    AddRef();
    return true;
}

std::uint32_t cSC4ToolTipManager::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4ToolTipManager::Init(cIGZFont*, signed char, signed char) {
    if (initialized_) return true;
    cIGZWinMgr* windows = RZ::WM();
    if (!windows) return true;
    if (cIGZMessageServer* messages = RZ::MS())
        for (std::uint32_t type : kTipMessages) messages->AddNotification(this, type);
    auto* box = new sc4::cSC4WinCalloutBox(windows);
    box->AddRef();
    box_ = box;
    box_->SetTextFontStyle(kTipTextFont);     // vslot 0x2c
    box_->SetTitleFontStyle(kTipTitleFont);   // vslot 0x28
    box_->AsIGZWin()->HideWindow();           // window vslot 0x118
    expandedImage_ = LoadSkin(kTipImageExpanded);
    box_->SetBackgroundImage(expandedImage_, -1, -1);   // vslot 0x10
    windows->GetMainWindow()->ChildAdd(box_->AsIGZWin());   // main window vslot 0x38
    simpleImage_ = LoadSkin(kTipImageSimple);
    timer_.SetPeriod(750);
    timer_.SetResolution(750);
    timer_.SetCallbackFunction(&cSC4ToolTipManager::static_tip_timer_callback, nullptr);
    initialized_ = true;
    return true;
}

bool cSC4ToolTipManager::Shutdown() {
    if (!initialized_) return true;
    timer_.Stop();
    if (cIGZMessageServer* messages = RZ::MS())
        for (std::uint32_t type : {kMsgToolTipTimer, kMsgToolTipShow, kMsgToolTipHide, kMsgToolTipHideAll, kMsgToolTipUpdate,
                                   0x22c010d5u, 0x22c010d6u})
            messages->RemoveNotification(this, type);
    if (cIGZWinMgr* windows = RZ::WM()) {
        if (box_) windows->GetMainWindow()->ChildRemove(box_->AsIGZWin());   // main window vslot 0x40
        if (box_) box_->Release();
        box_ = nullptr;
    }
    if (expandedImage_) expandedImage_->Release();
    expandedImage_ = nullptr;
    if (simpleImage_) simpleImage_->Release();
    simpleImage_ = nullptr;
    initialized_ = false;
    return true;
}

// RZ::MS() vslot 0x10: the timer's message is posted (the manager handles it on the next message pump).
void cSC4ToolTipManager::static_tip_timer_callback(void*) {
    if (cIGZMessageServer* messages = RZ::MS()) {
        static const cGZMessage message{kMsgToolTipTimer, 0, 0, 0};
        messages->MessagePost(message, false);
    }
}

bool cSC4ToolTipManager::DoMessage(cGZMessage& message) {
    const std::uint32_t type = message.type;
    if (type == kMsgToolTipShow || type == kMsgToolTipUpdate) {
        const auto* request = reinterpret_cast<const gz::BtnTipRequest*>(message.param1);
        if (!request || !request->text) return true;
        cRZString rest(*request->text);
        const cRZString first = rest.SplitToken("|");
        const bool expanded = SL::spPreferences && SL::spPreferences[kPrefsExpandedToolTips] != 0 && rest.Strlen() != 0 && first.Strlen() != 0;
        if (!expanded) {
            box_->SetTitle(cRZString());                              // vslot 0x18
            box_->SetText(first.Strlen() != 0 ? first : rest);        // vslot 0x20
            box_->SetBackgroundImage(simpleImage_, 5, 3);
            box_->SetAvoidanceRect(cRZRect{-4, -4, 20, 20});           // vslot 0x44
        } else {
            box_->SetTitle(first);
            box_->SetText(rest);
            box_->SetBackgroundImage(expandedImage_, 8, 8);
            box_->SetAvoidanceRect(cRZRect{-28, -28, 28, 28});
        }
        showing_ = true;
        if (std::getenv("SC4_TIPDEBUG")) std::fprintf(stderr, "tip: show '%s' | '%s' expanded %d\n", first.ToChar(), rest.ToChar(), int(expanded));
        if (first.FindChars("ESRB", 0, true) == static_cast<std::size_t>(-1)) {
            timer_.Stop();
            timer_.StartPeriodic(0, nullptr);
        } else {
            static_tip_timer_callback(nullptr);
        }
        return true;
    }
    if (type == kMsgToolTipHide || type == kMsgToolTipHideAll) {
        timer_.Stop();
        cIGZWin* window = box_->AsIGZWin();
        window->HideWindow();                 // vslot 0x118
        window->InvalidateSelfAndParents();   // vslot 0x170
        showing_ = false;
        return true;
    }
    if (type == kMsgToolTipTimer) {   // follow the pointer, come to the front, show
        if (!showing_) return true;
        cIGZWinMgr* windows = RZ::WM();
        cIGZWin* main = windows->GetMainWindow();
        int x = 0, y = 0;
        windows->GetCursorPos(main, x, y);         // WM vslot 0x84
        box_->SetPointOfInterest(cRZPoint{x, y});  // vslot 0x3c
        cIGZWin* window = box_->AsIGZWin();
        window->PullToFront();                     // vslot 0x5c
        if (!window->IsVisible()) window->ShowWindow();
        if (std::getenv("SC4_TIPDEBUG")) std::fprintf(stderr, "tip: at %d,%d visible %d\n", x, y, int(window->IsVisible()));
        return true;
    }
    return false;
}
