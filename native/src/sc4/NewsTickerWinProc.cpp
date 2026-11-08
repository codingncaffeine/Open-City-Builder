#include "sc4/NewsTickerWinProc.h"

#include <algorithm>
#include <string>
#include <vector>

#include "gz/Font.h"
#include "gz/RZServices.h"
#include "gz/WinCtrlMgr.h"
#include "rz/LanguageManager.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Advisor.h"
#include "sc4/ServiceLocator.h"

namespace sc4 {

namespace {

// The HTML fragments the news ticker builds its lines from (file-scope cRZStrings of SC4NewsTickerWinProc.cpp,
// initialised by __GLOBAL__sub_I_SC4NewsTickerWinProc.cpp @ 0x10031e0f4):
//   0x100b89240 kExpiredPriorityHeader 0x100b89290 kLowPriorityHeader 0x100b892b8 kMediumPriorityHeader
//   0x100b892e0 kHighPriorityHeader    0x100b89308 kHighestPriorityHeader 0x100b89330 kStandardPriorityHeader
//   0x100b89358 kPriorityFooter        0x100b89380 kBodyFooter          0x100b893a8 kHighestPriorityFooter
//   0x100b893d0 kHeaderFont            0x100b893f8 kHeaderFontJ         0x100b89420 kBodyFont
//   0x100b89448 kBodyFontDeactive      0x100b89470..0x100b896f0 the alarm / bad / neutral / good dot,
//   right-arrow, down-arrow and x bitmaps (14416250..1441625f).
const cRZString kExpiredPriorityHeader("<FONT COLOR=\"#8e9499\"");
const cRZString kLowPriorityHeader("<FONT COLOR=\"#3f4967\"");
const cRZString kMediumPriorityHeader("<FONT COLOR=\"#095a22\"");
const cRZString kHighPriorityHeader("<FONT COLOR=\"#b00000\"");
const cRZString kHighestPriorityHeader("<BLINK><FONT COLOR=\"#f70a0b\"");
const cRZString kStandardPriorityHeader("<FONT COLOR=\"#0f0f0f\"");
const cRZString kPriorityFooter("</B></FONT>");
const cRZString kBodyFooter("</I></FONT>");
const cRZString kHighestPriorityFooter("</B></FONT></BLINK>");
const cRZString kHeaderFont(" FACE=\"StocletITC TT\" SIZE=3><B>");
const cRZString kHeaderFontJ(" FACE=\"StocletITC TT\" SIZE=2><B>");
const cRZString kBodyFont("<FONT COLOR=\"#3f4967\" FACE=\"Arta\" SIZE=3><I>");
const cRZString kBodyFontDeactive("<FONT COLOR=\"#8e9499\" FACE=\"Arta\" SIZE=3><I>");
const cRZString kAlarmDot("<IMG SRC=\"sc4://HTML/46a006b0/14416250\">");
const cRZString kAlarmRightArrow("<IMG SRC=\"sc4://HTML/46a006b0/14416251\">");
const cRZString kAlarmDownArrow("<IMG SRC=\"sc4://HTML/46a006b0/14416252\">");
const cRZString kAlarmX("<IMG SRC=\"sc4://HTML/46a006b0/14416253\">");
const cRZString kBadDot("<IMG SRC=\"sc4://HTML/46a006b0/14416254\">");
const cRZString kBadRightArrow("<IMG SRC=\"sc4://HTML/46a006b0/14416255\">");
const cRZString kBadDownArrow("<IMG SRC=\"sc4://HTML/46a006b0/14416256\">");
const cRZString kBadX("<IMG SRC=\"sc4://HTML/46a006b0/14416257\">");
const cRZString kNeutralDot("<IMG SRC=\"sc4://HTML/46a006b0/14416258\">");
const cRZString kNeutralRightArrow("<IMG SRC=\"sc4://HTML/46a006b0/14416259\">");
const cRZString kNeutralDownArrow("<IMG SRC=\"sc4://HTML/46a006b0/1441625A\">");
const cRZString kNeutralX("<IMG SRC=\"sc4://HTML/46a006b0/1441625B\">");
const cRZString kGoodDot("<IMG SRC=\"sc4://HTML/46a006b0/1441625C\">");
const cRZString kGoodRightArrow("<IMG SRC=\"sc4://HTML/46a006b0/1441625D\">");
const cRZString kGoodDownArrow("<IMG SRC=\"sc4://HTML/46a006b0/1441625E\">");
const cRZString kGoodX("<IMG SRC=\"sc4://HTML/46a006b0/1441625F\">");

constexpr std::uint32_t kThaiLanguage = 0xf;   // RZ::LM()->GetCurrentLanguage()
constexpr std::uint32_t kAdviceCurrent = 2;    // cISC4Advice flag: shown
constexpr std::uint32_t kAdviceLink = 0x20;    // cISC4Advice flag: the text has a link

cIGZWin* TickerParent(gz::cIGZWinGen* window) {
    if (!window) return nullptr;
    cIGZWin* asWindow = window->AsIGZWin();
    return asWindow ? asWindow->GetParentWin() : nullptr;
}

// The child windows the ticker uses hang off the parent of its own script window.
cIGZWin* TickerChild(gz::cIGZWinGen* window, std::uint32_t id) {
    cIGZWin* parent = TickerParent(window);
    return parent ? parent->GetChildWindowFromIDRecursive(id) : nullptr;
}

bool UsesSmallHeaderFont() {
    cIGZLanguageManager* languages = RZ::LM();
    return languages && languages->GetCurrentLanguage() == kThaiLanguage;
}

// The caption of a cIGZString as a std::string (the sources only ever pass "Title" / "Message" through here).
std::string Text(const cIGZString& value) { return std::string(value.ToChar(), value.Strlen()); }

// `a + b + c + d` over the fragments above (the original uses std::string operator+ chains).
cRZString Join(const cIGZString& a, const cIGZString& b, const cIGZString& c, const cIGZString& d) {
    std::string text = Text(a);
    text += Text(b);
    text += Text(c);
    text += Text(d);
    return cRZString(text.c_str());
}

}  // namespace

cSC4NewsTickerWinProc::cSC4NewsTickerWinProc() = default;

cSC4NewsTickerWinProc::~cSC4NewsTickerWinProc() = default;

// @ 0x10031bb8c. The two interface ids are the message target (this) and the window procedure (this + 8); no
// other id is accepted (not even 1). The original bumps the primary count through vslot 8.
bool cSC4NewsTickerWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kNewsTickerIIDMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == gz::kGZIID_cIGZWinProc) {
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// @ 0x10031bbec.
std::uint32_t cSC4NewsTickerWinProc::AddRef() { return ++refCount_; }

// @ 0x10031bc08: at one reference the original runs its deleting destructor (vslot 0x28) and answers 0.
std::uint32_t cSC4NewsTickerWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// @ 0x10031bc44. Attaches itself to the script window, finds the toolbar controls and the two advice lists and
// sets the flipper area up from the font's line height.
bool cSC4NewsTickerWinProc::Init(gz::cIGZWinGen* window) {
    if (initialised_) return true;
    if (window_.get() != window) window_ = window;   // takes its own reference (cRZAutoRefCount)
    if (!window_) return false;                      // port guard: the original dereferences it
    window_->SetWinProc(static_cast<gz::cIGZWinProc*>(this));   // cIGZWinGen vslot 0x38
    window_->SetUserArea(cRZRect{});                            // vslot 0x58 with a zeroed rect

    HideTicker();   // the original calls its own vslot 0x40

    cIGZWin* tickerWindow = window_->AsIGZWin();
    cIGZWin* parent = tickerWindow ? tickerWindow->GetParentWin() : nullptr;
    if (!parent) return false;   // port guard

    // The three toolbar buttons report to the script window (cIGZWin vslot 0x2b0).
    for (std::uint32_t id : {kNewsTickerButtonToggle, kNewsTickerButtonToggleAlt, kNewsTickerButtonStop}) {
        if (cIGZWin* control = parent->GetChildWindowFromIDRecursive(id)) control->SetNotificationTarget(tickerWindow);
    }

    cIGZWin* host = parent->GetChildWindowFromIDRecursive(kNewsTickerControlHost);
    if (!host) return false;   // port guard
    void* list = nullptr;
    if (host->GetChildAsRecursive(kNewsTickerAdviceListBody, kGZIID_cISC4WinAdviceList, &list)) {
        adviceList_ = static_cast<IC4WinAdviceList*>(list);
        if (list) static_cast<cIGZUnknown*>(list)->Release();   // GetChildAsRecursive handed over a reference
    }
    list = nullptr;
    // The flipper line lives in the ticker's own script window, not in the control host (Mac i386 Init: parent's child
    // 0xca2aedc0, then its child 0xaa12f33c).
    cIGZWin* flipperParent = parent->GetChildWindowFromIDRecursive(kNewsTickerWindowId);
    if (flipperParent && flipperParent->GetChildAsRecursive(kNewsTickerAdviceListFlipper, kGZIID_cISC4WinAdviceList, &list)) {
        flipperList_ = static_cast<IC4WinAdviceList*>(list);
        if (list) static_cast<cIGZUnknown*>(list)->Release();
    }
    if (!adviceList_ || !flipperList_) return false;   // port guard

    flipperList_->DisableVerticalScrollbar();       // vslot 0x50
    flipperList_->SetGutters(0, 0, 0, 0);           // vslot 0x40
    flipperArea_ = flipperList_->AsIGZWin()->GetArea();   // cIGZWin vslot 0x178

    // The flipper control itself is a gen window and gets a zeroed user area too.
    gz::cIGZWinGen* flipperGen = nullptr;
    if (host->QueryInterface(kGZIID_cIGZWinGen, reinterpret_cast<void**>(&flipperGen)) && flipperGen) {
        flipperGen->SetUserArea(cRZRect{});
        flipperGen->Release();
    }

    timer_.SetCallbackFunction(&cSC4NewsTickerWinProc::NewsTickCallback, this);
    gz::IWinFont* font = nullptr;
    if (gz::cIGZWinCtrlMgr* controls = RZ::WCM()) {
        font = controls->SystemFont(kNewsTickerFontId);
    } else if (gz::IWinFontSource* fonts = gz::WinFontSource()) {
        font = fonts->Font(kNewsTickerFontId);   // port: the native build has no control manager; its font seam
    }
    if (font) lineHeight_ = font->GlyphHeight();   // IWinFont vslot 0x118 (the cIGZFont line height)
    flipperArea_.bottom = lineHeight_ * 3 + flipperArea_.top;

    if (cIGZWin* label = parent->GetChildWindowFromIDRecursive(kNewsTickerResizeLabel)) {
        const int width = label->GetW();
        const int height = label->GetH();
        const std::uint32_t shortened = std::min<std::uint32_t>(static_cast<std::uint32_t>(height),
                                                                static_cast<std::uint32_t>(lineHeight_ * 2));
        label->SetSize(width, static_cast<int>(shortened));   // cIGZWin vslot 0x1a8
    }
    initialised_ = true;
    return true;
}

// @ 0x10031c016.
bool cSC4NewsTickerWinProc::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    timer_.Stop();
    if (adviceList_) adviceList_->ClearList();
    if (flipperList_) flipperList_->ClearList();
    messages_.clear();
    adviceList_ = nullptr;
    flipperList_ = nullptr;
    for (auto& entry : ring_) entry = nullptr;
    if (window_) {
        window_->SetWinProc(nullptr);
        window_->AsIGZWin()->GetWindowManager();   // kept: the original calls this and drops the answer
        window_ = nullptr;
    }
    RegisterMessageNotification(false);
    return true;
}

// @ 0x10031c0f0. The flag is flipped before the message server is asked, so a missing server leaves it flipped.
void cSC4NewsTickerWinProc::RegisterMessageNotification(bool on) {
    cIGZMessageServer2* server = SL::spMS2;
    if (on) {
        if (notificationsRegistered_) return;
        notificationsRegistered_ = true;
        if (!server) return;
        server->AddNotification(this, kNewsTickerAdviceChanged);
        server->AddNotification(this, kNewsTickerAdvisorRemoved);
        server->AddNotification(this, kNewsTickerAdviceAdded);
        server->AddNotification(this, kNewsTickerAdviceRemoved);
    } else {
        if (!notificationsRegistered_) return;
        notificationsRegistered_ = false;
        if (!server) return;
        server->RemoveNotification(this, kNewsTickerAdviceChanged);
        server->RemoveNotification(this, kNewsTickerAdvisorRemoved);
        server->RemoveNotification(this, kNewsTickerAdviceAdded);
        server->RemoveNotification(this, kNewsTickerAdviceRemoved);
    }
}

// @ 0x10031c1c0. Only messages from the ticker's own script window are handled: command 0x287259f6 with a
// button id toggles the ticker, and the attach / detach notifications register or drop the message listeners.
bool cSC4NewsTickerWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (!window_ || !window_->AsIGZWin()) return false;   // port guard
    if (window != window_->AsIGZWin()) return false;

    if (message.type == kGZMsgCommand) {
        if (message.param1 != gz::kWinCommandSender) return false;
        cIGZWin* control = TickerChild(window_, kNewsTickerControlHost);
        if (!control) return false;
        const std::uint64_t id = message.param2;
        if (id - kNewsTickerButtonToggle < 2) {   // 0xca2aedcc / 0xca2aedcd: show / hide
            if (!tickerVisible_) {
                control->SetFlag(kWinVisible, true);
                ShowTicker();
                updateBodyTextCtrl();
            } else {
                control->SetFlag(kWinVisible, false);
            }
            tickerVisible_ = !tickerVisible_;
        } else if (id == kNewsTickerButtonStop) {
            control->SetFlag(kWinVisible, false);
            tickerVisible_ = false;
        }
        return false;
    }
    if (message.type == gz::kWinProcShutdown) {
        RegisterMessageNotification(false);
    } else if (message.type == gz::kWinProcInit) {
        RegisterMessageNotification(true);
    }
    return false;
}

// @ 0x10031c2e0.
bool cSC4NewsTickerWinProc::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    if (type == kNewsTickerAdviceAdded) {
        cISC4Advice* advice = nullptr;
        cIGZUnknown* unknown = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();
        if (unknown && unknown->QueryInterface(kGZIID_cISC4Advice, reinterpret_cast<void**>(&advice)) && advice) {
            if (!advice->CheckFlags(kNewsTickerAdviceNewsOnly)) {
                rebuildPending_ = 1;
                AddMessage(advice);
                updateBodyTextCtrl();
            }
            advice->Release();
        }
        return true;
    }
    if (type == kNewsTickerAdviceChanged) {
        cISC4Advice* advice = nullptr;
        cIGZUnknown* unknown = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();
        if (unknown && unknown->QueryInterface(kGZIID_cISC4Advice, reinterpret_cast<void**>(&advice)) && advice) {
            updateBodyTextCtrl();
            advice->Release();
        }
        return true;
    }
    if (type == kNewsTickerAdvisorRemoved) {
        cISC4Advisor* advisor = nullptr;
        cIGZUnknown* unknown = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();
        if (unknown && unknown->QueryInterface(kGZIID_cISC4Advisor, reinterpret_cast<void**>(&advisor)) && advisor) {
            const int advisorId = advisor->GetID();   // cISC4Advisor vslot 0x28
            bool removed = false;
            for (auto it = messages_.begin(); it != messages_.end();) {
                cISC4Advice* advice = it->get();
                if (advice && advice->GetAdvisorID() == advisorId) {   // cISC4Advice vslot 0x58
                    it = messages_.erase(it);
                    removed = true;
                } else {
                    ++it;
                }
            }
            if (removed) {
                updateBodyTextCtrl();
                rebuildPending_ = 1;
                DoFlipperMessage();
            }
            advisor->Release();
        }
        return true;
    }
    if (type == kNewsTickerFlipperTick) {
        if (!flipperList_) {   // the original stops the timer and leaves
            timer_.Stop();
            return true;
        }
        cIGZWin* window = flipperList_->AsIGZWin();
        if (flipperScroll_ == 0) {
            window->SetArea(flipperArea_);   // cIGZWin vslot 0x1b8
            rebuildPending_ = 1;
            DoFlipperMessage();
            flipperScroll_ = flipperScrolled_ == 0 ? static_cast<std::uint32_t>(lineHeight_) : 1u;
        } else {
            cRZRect area = flipperArea_;
            area.left -= static_cast<int>(flipperScroll_);
            area.top -= static_cast<int>(flipperScroll_);
            area.right -= static_cast<int>(flipperScroll_);
            area.bottom -= static_cast<int>(flipperScroll_);
            window->SetArea(area);
            if (flipperScroll_ < static_cast<std::uint32_t>(lineHeight_)) {
                ++flipperScroll_;
                timer_.SetPeriod(kNewsTickerScrollMs);
            } else {
                flipperScroll_ = 0;
                timer_.SetPeriod(kNewsTickerLineDelayMs);
            }
            return true;
        }
        timer_.SetPeriod(kNewsTickerLineDelayMs);
        return true;
    }
    if (type == kNewsTickerAdviceRemoved) {
        cISC4Advice* advice = nullptr;
        cIGZUnknown* unknown = static_cast<cIGZMessage2Standard*>(message)->GetIGZUnknown();
        if (unknown && unknown->QueryInterface(kGZIID_cISC4Advice, reinterpret_cast<void**>(&advice)) && advice) {
            RemoveMessage(advice);
            updateBodyTextCtrl();
            advice->Release();
        }
        rebuildPending_ = 1;
        return true;
    }
    return true;
}

// @ 0x10031c644. Brings the ticker window and its control forward (with flag 0x800 held while moving), shows the
// control when the ticker is enabled and starts the 3 s flipper timer when it is not running yet.
void cSC4NewsTickerWinProc::ShowTicker() {
    cIGZWin* ticker = TickerChild(window_, kNewsTickerWindowId);
    if (ticker) {
        ticker->SetFlag(0x800, true);
        ticker->PullToFront();
        ticker->SetFlag(0x800, false);
        ticker->SetFlag(kWinVisible, true);
    }
    cIGZWin* control = TickerChild(window_, kNewsTickerControlHost);
    if (!control) return;
    control->SetFlag(0x800, true);
    control->PullToFront();
    control->SetFlag(0x800, false);
    if (tickerVisible_) {
        control->SetFlag(kWinVisible, true);
        updateBodyTextCtrl();
    }
    if (!timer_.IsRunning()) timer_.StartPeriodic(kNewsTickerLineDelayMs, static_cast<gz::cIGZWinProc*>(this));
}

// @ 0x10031c760.
void cSC4NewsTickerWinProc::HideTicker() {
    if (cIGZWin* control = TickerChild(window_, kNewsTickerControlHost)) control->SetFlag(kWinVisible, false);
    if (cIGZWin* ticker = TickerChild(window_, kNewsTickerWindowId)) ticker->SetFlag(kWinVisible, false);
    if (timer_.IsRunning()) timer_.Stop();
}

// @ 0x10031c7e0. Only the first entry with that advice is removed.
void cSC4NewsTickerWinProc::RemoveMessage(cISC4Advice* advice) {
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
        if (it->get() == advice) {
            messages_.erase(it);
            return;
        }
    }
}

// @ 0x10031c808. Ignored when the advice is already listed; otherwise inserted before the first entry whose
// priority is not higher (the list stays ordered high to low).
void cSC4NewsTickerWinProc::AddMessage(cISC4Advice* advice) {
    if (!advice) return;   // port guard: the original dereferences it when the list is not empty
    for (const auto& entry : messages_) {
        if (entry.get() == advice) return;
    }
    const int priority = advice->GetPriority();
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
        cISC4Advice* existing = it->get();
        if (existing && existing->GetPriority() <= priority) {
            messages_.insert(it, cRZAutoRefCount<cISC4Advice>(advice));
            return;
        }
    }
    messages_.push_back(cRZAutoRefCount<cISC4Advice>(advice));
}

// @ 0x10031db20: drops every advice that is not flagged "current".
void cSC4NewsTickerWinProc::RemoveInactive() {
    for (auto it = messages_.begin(); it != messages_.end();) {
        cISC4Advice* advice = it->get();
        if (!advice || !advice->CheckFlags(kAdviceCurrent)) it = messages_.erase(it);
        else ++it;
    }
}

// @ 0x10031c9a0. Rebuilds the advice panel: one entry per queued advice, with the header / icon set chosen by
// its mood (0 alarm, 1 bad, 5 good, everything else neutral) and the expired colours when it is no longer
// current. The title line is header + header font + title + footer; the body line is body font + message + body
// footer. Mood 7 has no body: its title line is passed twice and the first icon is used for both.
void cSC4NewsTickerWinProc::updateBodyTextCtrl() {
    if (!adviceList_) return;
    adviceList_->ClearList();
    adviceList_->SetbgImage(kNewsTickerImageBody);

    for (const auto& entry : messages_) {
        cISC4Advice* advice = entry.get();
        if (!advice) continue;   // port guard
        const int mood = advice->GetMood();

        cRZString header;
        cRZString iconFirst;    // the icon mood 7 passes twice
        cRZString iconRight;
        cRZString iconDown;
        cRZString iconX;
        cRZString bodyFont(kBodyFont);
        cRZString footer(kPriorityFooter);
        switch (mood) {
        case 0:
            header = kHighPriorityHeader;
            iconFirst = kAlarmDot;
            iconRight = kAlarmRightArrow;
            iconDown = kAlarmDownArrow;
            iconX = kAlarmX;
            break;
        case 1:
            header = kHighPriorityHeader;
            iconFirst = kBadDot;
            iconRight = kBadRightArrow;
            iconDown = kBadDownArrow;
            iconX = kBadX;
            break;
        case 5:
            header = kMediumPriorityHeader;
            iconFirst = kGoodDot;
            iconRight = kGoodRightArrow;
            iconDown = kGoodDownArrow;
            iconX = kGoodX;
            break;
        default:   // 2, 3, 4, 6 ...: the neutral set (quirk: the "standard" header is never used here)
        case 3:
            header = kLowPriorityHeader;
            iconFirst = kNeutralDot;
            iconRight = kNeutralRightArrow;
            iconDown = kNeutralDownArrow;
            iconX = kNeutralX;
            break;
        }
        if (!advice->CheckFlags(kAdviceCurrent)) {
            header = kExpiredPriorityHeader;
            iconFirst = kNeutralDot;
            iconRight = kNeutralRightArrow;
            iconDown = kNeutralDownArrow;
            iconX = kNeutralX;
            bodyFont = kBodyFontDeactive;
        }

        const cRZString titleLine = Join(header, kHeaderFont, advice->GetTextTitle(), footer);
        if (mood == 7) {
            adviceList_->AddEntry(iconFirst, iconFirst, titleLine, titleLine, false, advice, iconX, true,
                                  kNewsTickerImageBody);
        } else {
            const cRZString bodyLine = Join(bodyFont, advice->GetTextMessage(), kBodyFooter, cRZString());
            const bool hasLink = advice->CheckFlags(kAdviceLink);
            adviceList_->AddEntry(iconDown, iconRight, bodyLine, titleLine, hasLink, advice, iconX, false,
                                  kNewsTickerImageBody);
        }
    }
    adviceList_->RebuildHTML(true);
}

// @ 0x10031d224. One flipper line: the priority header + the header font (the smaller one for Thai) + the title
// + the footer. Without an advice the title is replaced by a single space and the low-priority header is used.
void cSC4NewsTickerWinProc::DisplayNewsFlipperLine(cISC4Advice* advice) {
    if (!flipperList_) return;   // port guard: the original dereferences it
    const cRZString empty;
    cRZString header;
    cRZString footer(kPriorityFooter);
    const cRZString& font = UsesSmallHeaderFont() ? kHeaderFontJ : kHeaderFont;

    if (!advice) {
        header = kLowPriorityHeader;
        const cRZString line = Join(header, font, cRZString(" "), footer);
        flipperList_->AddEntry(empty, empty, line, line, false, nullptr, empty, false, kNewsTickerImageFlipper);
        return;
    }
    switch (advice->GetMood()) {
    case 0:
        footer = kHighestPriorityFooter;
        header = kHighestPriorityHeader;
        break;
    case 1:
        header = kHighPriorityHeader;
        break;
    case 5:
        header = kMediumPriorityHeader;
        break;
    default:
    case 3:
        header = kLowPriorityHeader;
        break;
    }
    const cRZString line = Join(header, font, advice->GetTextTitle(), footer);
    flipperList_->AddEntry(empty, empty, line, line, false, advice, empty, false, kNewsTickerImageFlipper);
}

// @ 0x10031db66. Chooses the up-to-three advices shown on the flipper:
//  1. count the ring entries and remember the last non-null one and its priority,
//  2. lower the "wanted priority" to 59 when some current advice has a priority above 59,
//  3. when the ring's priority already reaches it, take the next qualifying advice *after* the one on screen,
//  4. otherwise take the first qualifying advice of the list,
//  5. append it to the ring (rotating the three entries when the ring is full) and redraw all three lines.
void cSC4NewsTickerWinProc::DoFlipperMessage() {
    cRZAutoRefCount<cISC4Advice> selected;
    ringCount_ = 0;
    if (!flipperList_) return;
    flipperList_->ClearList();
    flipperList_->SetbgImage(kNewsTickerImageFlipper);

    // 1. the last non-null ring entry (counted) and the highest ring priority.
    cRZAutoRefCount<cISC4Advice> current;
    std::uint32_t ringPriority = 0;
    for (auto& entry : ring_) {
        cISC4Advice* advice = entry.get();
        if (!advice) continue;
        if (current.get() != advice) current = advice;
        ringPriority = static_cast<std::uint32_t>(current->GetPriority());
        ++ringCount_;
    }

    std::uint32_t alreadyShown = 0;
    if (messages_.empty()) {
        alreadyShown = 0;
    } else {
        // 2. a current advice above priority 59 lowers the wanted priority to 59.
        std::uint32_t threshold = 0;
        for (const auto& entry : messages_) {
            cISC4Advice* advice = entry.get();
            if (advice && advice->CheckFlags(kAdviceCurrent) && advice->GetPriority() > 0x3b) {
                threshold = 0x3b;
                break;
            }
        }
        bool changed = false;
        if (ringPriority >= threshold) {
            // 3. skip the entries through the one on screen, then take the next qualifying advice.
            bool check = false;
            for (const auto& entry : messages_) {
                cISC4Advice* advice = entry.get();
                if (!advice) continue;
                if (!check) {
                    check = advice == current.get();
                    continue;
                }
                if (!advice->CheckFlags(kAdviceCurrent)) continue;
                if (static_cast<std::uint32_t>(advice->GetPriority()) < threshold) continue;
                if (ringCount_ == 0) {
                    ringCount_ = 1;
                    ring_[0] = advice;
                    changed = true;
                } else if (advice == ring_[ringCount_ - 1].get()) {
                    alreadyShown = 1;
                } else {
                    selected = advice;
                    changed = true;
                }
                if (changed) break;
            }
        }
        if (!changed) {
            // 4. the first qualifying advice of the whole list.
            alreadyShown = 0;
            for (const auto& entry : messages_) {
                cISC4Advice* advice = entry.get();
                if (!advice) continue;
                if (!advice->CheckFlags(kAdviceCurrent)) continue;
                if (static_cast<std::uint32_t>(advice->GetPriority()) < threshold) continue;
                if (ringCount_ == 0) {
                    selected = advice;
                    break;
                }
                if (advice == ring_[ringCount_ - 1].get()) {
                    alreadyShown = 1;
                    continue;
                }
                if (selected.get() != advice) selected = advice;
                break;
            }
        }
        // 5. append / rotate. The original skips the rotation when the candidate is the last entry on screen
        //    and there is only one entry.
        if (!selected) {
            if (!(alreadyShown && !ring_[1])) {
                for (int i = 0; i < 2; ++i) {
                    ring_[i] = ring_[i + 1];
                    flipperScrolled_ = 1;
                }
            }
            if (ring_[2]) ring_[2] = nullptr;
        } else if (!ring_[2]) {
            ring_[ringCount_] = selected;
            ++ringCount_;
        } else {
            for (int i = 0; i < 2; ++i) {
                ring_[i] = ring_[i + 1];
                flipperScrolled_ = 1;
            }
            --ringCount_;
            ring_[ringCount_] = selected;
            ++ringCount_;
        }
    }

    for (auto& entry : ring_) DisplayNewsFlipperLine(entry.get());
    if (rebuildPending_) {
        flipperList_->RebuildHTML(true);   // vslot 0x28
        rebuildPending_ = 0;
    }
}

// @ 0x10031bf54. The timer callback: posts kNewsTickerFlipperTick to the ticker (a standard message, counted).
void cSC4NewsTickerWinProc::NewsTickCallback(void* context) {
    cIGZMessageServer2* server = SL::spMS2;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kNewsTickerFlipperTick);
    server->GeneralMessagePostToTarget(message, static_cast<cIGZMessageTarget2*>(context));
    message->Release();
}

}  // namespace sc4
