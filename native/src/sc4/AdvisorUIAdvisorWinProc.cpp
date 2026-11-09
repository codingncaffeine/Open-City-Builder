#include "sc4/AdvisorUIAdvisorWinProc.h"

#include <set>
#include <string>

#include "gz/cGZPersistResourceKey.h"
#include "gz/WinInterfaces.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/Advice.h"
#include "sc4/SC4UI.h"

namespace sc4 {

namespace {

IAdvisorUIAdvisorHost* g_host = nullptr;

// updateBodyTextCtrl's function-local statics (same text as the ticker's file statics; the advisor window keeps its own).
const cRZString kExpiredPriorityHeader("<FONT COLOR=\"#8e9499\"");
const cRZString kLowPriorityHeader("<FONT COLOR=\"#3f4967\"");
const cRZString kMediumPriorityHeader("<FONT COLOR=\"#095a22\"");
const cRZString kHighPriorityHeader("<FONT COLOR=\"#b00000\"");
const cRZString kHighestPriorityHeader("<BLINK><FONT COLOR=\"#f70a0b\"");
const cRZString kPriorityFooter("</B></FONT>");
const cRZString kBodyFooter("</I></FONT>");
const cRZString kHighestPriorityFooter("</B></FONT></BLINK>");
const cRZString kHeaderFont(" FACE=\"StocletITC TT\" SIZE=3><B>");
const cRZString kBodyFont("<FONT COLOR=\"#3f4967\" FACE=\"Arta\" SIZE=3><I>");
const cRZString kBodyFontDeactive("<FONT COLOR=\"#8e9499\" FACE=\"Arta\" SIZE=3><I>");
// The four pictures of each mood: dot, right arrow, down arrow, x.
struct IconSet {
    cRZString dot, right, down, x;
};
const IconSet kAlarmIcons{cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416250\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416251\">"),
                          cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416252\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416253\">")};
const IconSet kBadIcons{cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416254\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416255\">"),
                        cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416256\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416257\">")};
const IconSet kNeutralIcons{cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416258\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/14416259\">"),
                            cRZString("<IMG SRC=\"sc4://HTML/46a006b0/1441625A\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/1441625B\">")};
const IconSet kGoodIcons{cRZString("<IMG SRC=\"sc4://HTML/46a006b0/1441625C\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/1441625D\">"),
                         cRZString("<IMG SRC=\"sc4://HTML/46a006b0/1441625E\">"), cRZString("<IMG SRC=\"sc4://HTML/46a006b0/1441625F\">")};

constexpr int kAdviceCurrent = 2;   // cISC4Advice flag: shown
constexpr int kAdviceLink = 0x20;   // cISC4Advice flag: the text has a link
constexpr int kMoodNone = 7;        // get_advisor_mood's start value; also the mood without a body line
constexpr int kMoodNeutral = 3;     // an advisor without advices

std::string Text(const cIGZString& value) { return std::string(value.ToChar(), value.Strlen()); }

int IndexOf(int advisorId) {
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        if (kAdvisorWindowAdvisorIds[i] == advisorId) return i;
    }
    return -1;
}

template <std::size_t N>
bool In(std::uint64_t id, const std::uint32_t (&ids)[N]) {
    for (std::uint32_t entry : ids) {
        if (entry == id) return true;
    }
    return false;
}

void SetVisible(cIGZWin* window, bool on) {
    if (window) window->SetFlag(kWinVisible, on);
}

}  // namespace

IAdvisorUIAdvisorHost* AdvisorUIAdvisorHost() { return g_host; }
void SetAdvisorUIAdvisorHost(IAdvisorUIAdvisorHost* host) { g_host = host; }

cSC4AdvisorUIAdvisorWinProc::cSC4AdvisorUIAdvisorWinProc() = default;

cSC4AdvisorUIAdvisorWinProc::~cSC4AdvisorUIAdvisorWinProc() = default;

// @ 0x001fd428: 0x22e85d8e is the window procedure view; the message target answers 0x452294aa and 1.
bool cSC4AdvisorUIAdvisorWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cSC4AdvisorUIAdvisorWinProc || iid == gz::kGZIID_cIGZWinProc) {
        // port: the cIGZWinProc id too, so the window can hold it like any other procedure
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else if (iid == 0x452294aa || iid == 1) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4AdvisorUIAdvisorWinProc::AddRef() { return ++refCount_; }

// @ 0x001fd47c: below two references the deleting destructor runs and 0 is answered.
std::uint32_t cSC4AdvisorUIAdvisorWinProc::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

cIGZWin* cSC4AdvisorUIAdvisorWinProc::Parent() const {
    cIGZWin* window = window_ ? window_->AsIGZWin() : nullptr;
    return window ? window->GetParentWin() : nullptr;
}

cIGZWin* cSC4AdvisorUIAdvisorWinProc::Child(std::uint32_t id) const {
    cIGZWin* parent = Parent();
    return parent ? parent->GetChildWindowFromIDRecursive(id) : nullptr;
}

// @ 0x001fd314. The flag flips before the server is asked.
void cSC4AdvisorUIAdvisorWinProc::RegisterMessageNotification(bool on) {
    if (on == notificationsRegistered_) return;
    notificationsRegistered_ = on;
    cIGZMessageServer2* server = g_host ? g_host->Messages() : nullptr;
    if (!server) return;
    for (std::uint32_t type : {kAdvisorWindowAdvisorRemoved, kAdvisorWindowAdviceChanged, kAdvisorWindowAdviceAdded,
                               kAdvisorWindowMoodToggled, kAdvisorWindowAdviceRemoved}) {
        if (on) server->AddNotification(this, type);
        else server->RemoveNotification(this, type);
    }
}

// @ 0x001ff692.
void cSC4AdvisorUIAdvisorWinProc::Init() {
    cIGZWin* view = g_host ? g_host->View3DWindow() : nullptr;
    if (!view) return;   // port guard: the original dereferences SL::spView3DWin
    std::set<cIGZWin*> windows;
    nSC4UI::GetWindowChildrenSet(view, windows);
    window_ = nullptr;
    const cGZPersistResourceKey key{0, kAdvisorWindowScriptGroup, kAdvisorWindowScriptInstance};
    void* created = nullptr;
    if (g_host->CreateWindowFromScript(key, view, kAdvisorWindowAll, gz::kGZIID_cIGZWinGen, &created) && created) {
        window_ = static_cast<gz::cIGZWinGen*>(created);
        static_cast<gz::cIGZWinGen*>(created)->Release();   // the auto reference holds it now
    }
    std::set<cIGZWin*> added;
    nSC4UI::GetWindowChildrenNotInSet(view, windows, added);
    nSC4UI::AlignWindowSetToWindow(added, nullptr);
    if (!window_) return;   // port guard

    nSC4UI::ClearAcceptFocusFlagRecursive(window_->AsIGZWin());
    for (std::uint32_t id : {kAdvisorWindowAll, kAdvisorWindowLarge, kAdvisorWindowSmall}) {
        cIGZWin* page = view->GetChildWindowFromIDRecursive(id);
        SetVisible(page, false);
        if (page) nSC4UI::ClearAcceptFocusFlagRecursive(page);
    }
    window_->SetWinProc(this);
    SetupAllAdvisorWinProcs();
}

// @ 0x001fd886.
void cSC4AdvisorUIAdvisorWinProc::Shutdown() {
    AddRef();
    if (adviceList_) adviceList_->ClearList();
    RemoveAllAdvisorWinProcs();
    if (window_) {
        cIGZWin* window = window_->AsIGZWin();
        cIGZWinMgr* manager = window ? window->GetWindowManager() : nullptr;
        if (manager) {
            // The original destroys the three pages and then the script window, which is page 0x6a15c767 itself when
            // the script answers that window (port: a window already destroyed is not destroyed twice).
            cIGZWin* destroyed[4] = {};
            int count = 0;
            for (std::uint32_t id : {kAdvisorWindowAll, kAdvisorWindowSmall, kAdvisorWindowLarge}) {
                destroyed[count++] = Child(id);
            }
            destroyed[count++] = window;
            for (int i = 0; i < count; ++i) {
                bool seen = destroyed[i] == nullptr;
                for (int j = 0; j < i && !seen; ++j) seen = destroyed[j] == destroyed[i];
                if (!seen) manager->DestroyWindow(destroyed[i]);
            }
        }
    }
    RegisterMessageNotification(false);
    advisor_ = nullptr;
    window_ = nullptr;
    adviceList_ = nullptr;
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        headWindows_[i] = nullptr;
        heads_[i] = nullptr;
    }
    Release();
}

// @ 0x001fdbe4.
void cSC4AdvisorUIAdvisorWinProc::ShowWindow() {
    if (window_) {   // port guard
        const std::uint32_t ids[] = {kAdvisorWindowAll, kAdvisorWindowSmall, kAdvisorWindowLarge};
        if (layout_ >= 0 && layout_ <= 2) SetVisible(Child(ids[layout_]), true);
    }
    if (static_cast<unsigned>(advisorIndex_) < kAdvisorWindowAdvisorCount) EnableSingleAdvisorWinProc(advisorId_, true);
    else EnableAllAdvisorWinProcs(true);
    visible_ = true;
}

// @ 0x001fdaee.
void cSC4AdvisorUIAdvisorWinProc::HideWindow() {
    if (window_) {
        for (std::uint32_t id : {kAdvisorWindowAll, kAdvisorWindowLarge, kAdvisorWindowSmall}) SetVisible(Child(id), false);
    }
    if (static_cast<unsigned>(advisorIndex_) < kAdvisorWindowAdvisorCount) EnableSingleAdvisorWinProc(advisorId_, false);
    else EnableAllAdvisorWinProcs(false);
    visible_ = false;
}

// The body SetupAllAdvisorWinProcs and SetupSingleAdvisorWinProc share: a new head for index `index` on `imageWindow`.
void cSC4AdvisorUIAdvisorWinProc::AttachHead(int index, cIGZWin* imageWindow) {
    const int advisorId = kAdvisorWindowAdvisorIds[index];
    heads_[index] = new cSC4AdvisorUIImageWinProc(advisorId);
    if (!imageWindow) return;   // port guard: the original dereferences it
    imageWindow->SetFlag(kAdvisorWindowImageFlag, true);
    headWindows_[index] = nullptr;
    void* gen = nullptr;
    if (imageWindow->QueryInterface(gz::kGZIID_cIGZWinGen, &gen) && gen) {
        headWindows_[index] = static_cast<gz::cIGZWinGen*>(gen);
        static_cast<gz::cIGZWinGen*>(gen)->Release();
    }
    if (headWindows_[index]) headWindows_[index]->SetWinProc(heads_[index].get());
    heads_[index]->Enable(visible_);
    if (g_host && g_host->CityPresent()) heads_[index]->SetMood(get_advisor_mood(g_host->Advisor(advisorId)));
}

// @ 0x001ff3f0.
void cSC4AdvisorUIAdvisorWinProc::SetupAllAdvisorWinProcs() {
    cIGZWin* parent = Parent();
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        if (heads_[i]) continue;
        AttachHead(i, parent ? parent->GetChildWindowFromIDRecursive(kAdvisorWindowImageIds[i]) : nullptr);
    }
}

// @ 0x001ff840.
void cSC4AdvisorUIAdvisorWinProc::SetupSingleAdvisorWinProc(int advisorId, std::uint32_t imageWindowId) {
    const int index = IndexOf(advisorId);
    if (index < 0 || heads_[index]) return;
    AttachHead(index, Child(imageWindowId));
}

// @ 0x001fd80e.
void cSC4AdvisorUIAdvisorWinProc::RemoveAllAdvisorWinProcs() {
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        if (!heads_[i]) continue;
        if (headWindows_[i]) headWindows_[i]->SetWinProc(nullptr);
        heads_[i]->Enable(false);
        heads_[i] = nullptr;
    }
}

// @ 0x001fda22. Quirk: unlike RemoveAll, the head is not disabled first.
void cSC4AdvisorUIAdvisorWinProc::RemoveSingleAdvisorWinProc(int advisorId) {
    const int index = IndexOf(advisorId);
    if (index < 0) return;
    if (headWindows_[index]) headWindows_[index]->SetWinProc(nullptr);
    if (heads_[index]) heads_[index] = nullptr;
}

// @ 0x001fd7d0.
void cSC4AdvisorUIAdvisorWinProc::EnableAllAdvisorWinProcs(bool enable) {
    for (auto& head : heads_) {
        if (head) head->Enable(enable);
    }
}

// @ 0x001fda98.
void cSC4AdvisorUIAdvisorWinProc::EnableSingleAdvisorWinProc(int advisorId, bool enable) {
    const int index = IndexOf(advisorId);
    if (index >= 0 && heads_[index]) heads_[index]->Enable(enable);
}

// @ 0x001fef02: only the first entry holding the advice.
void cSC4AdvisorUIAdvisorWinProc::RemoveMessage(cISC4Advice* advice) {
    for (auto it = messages_.begin(); it != messages_.end(); ++it) {
        if (it->get() == advice) {
            messages_.erase(it);
            return;
        }
    }
}

// @ 0x001fef32: appended (the ticker ranks by priority; this list keeps the order it is given).
void cSC4AdvisorUIAdvisorWinProc::AddMessage(cISC4Advice* advice) {
    messages_.push_back(cRZAutoRefCount<cISC4Advice>(advice));
}

// @ 0x001fef90.
int cSC4AdvisorUIAdvisorWinProc::get_advisor_mood(cISC4Advisor* advisor) {
    if (!advisor) return kMoodNeutral;
    const AdviceMap advices = static_cast<const cISC4Advisor*>(advisor)->GetCurrentAdvices();
    if (advices.empty()) return kMoodNeutral;
    int mood = kMoodNone;
    for (const auto& entry : advices) {
        cISC4Advice* advice = entry.first.get();
        if (!advice) continue;   // port guard
        const int value = advice->GetMood();
        if (value < mood) {
            mood = value;
            if (mood == 0) return 0;
        }
    }
    return mood;
}

// @ 0x001ff036.
void cSC4AdvisorUIAdvisorWinProc::SetAllMoods() {
    for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
        if (!heads_[i]) continue;
        cISC4Advisor* advisor = g_host ? g_host->Advisor(kAdvisorWindowAdvisorIds[i]) : nullptr;
        heads_[i]->SetMood(get_advisor_mood(advisor));
    }
}

// @ 0x001ff9e2. Advices that are no longer current lose flag 0x20 first.
void cSC4AdvisorUIAdvisorWinProc::GetMessageList(cISC4Advisor* advisor) {
    for (const auto& entry : messages_) {
        cISC4Advice* advice = entry.get();
        if (advice && !advice->CheckFlags(kAdviceCurrent)) advice->SetFlags(kAdviceLink, false);
    }
    advices_.clear();
    if (advisor) advices_ = advisor->GetCurrentAdvices();   // port guard on a missing advisor
    messages_.clear();
    for (auto it = advices_.rbegin(); it != advices_.rend(); ++it) AddMessage(it->first.get());
    SetAllMoods();
}

// @ 0x001ff590.
void cSC4AdvisorUIAdvisorWinProc::SetAllAdvisorsWindow() {
    SetVisible(Child(kAdvisorWindowSmall), false);
    SetVisible(Child(kAdvisorWindowLarge), false);
    SetVisible(Child(kAdvisorWindowAll), true);
    RemoveSingleAdvisorWinProc(advisorId_);
    advisor_ = nullptr;
    advisorIndex_ = -1;
    advisorId_ = -1;
    SetupAllAdvisorWinProcs();
    layout_ = 0;
}

void cSC4AdvisorUIAdvisorWinProc::FindAdviceList(cIGZWin* page, std::uint32_t listId) {
    if (adviceList_ || !page) return;
    void* list = nullptr;
    if (page->GetChildAsRecursive(listId, kGZIID_cIGZWinAdviceList, &list) && list) {
        adviceList_ = static_cast<cIGZWinAdviceList*>(list);
        static_cast<cIGZWinAdviceList*>(list)->Release();
    }
}

// @ 0x001ffd78.
void cSC4AdvisorUIAdvisorWinProc::SetIndividualAdvisorWindow(int advisorId) {
    const int index = IndexOf(advisorId);
    if (index < 0 || !g_host) return;
    cIGZWin* all = Child(kAdvisorWindowAll);
    RemoveAllAdvisorWinProcs();
    advisor_ = g_host->Advisor(advisorId);
    advisorId_ = advisorId;
    advisorIndex_ = index;
    SetVisible(all, false);
    cIGZWin* page = nullptr;
    std::uint32_t image = 0;
    if (!large_) {
        page = Child(kAdvisorWindowSmall);
        layout_ = 1;
        FindAdviceList(page, kAdvisorWindowSmallList);
        image = kAdvisorWindowSmallImage;
    } else {
        page = Child(kAdvisorWindowLarge);
        layout_ = 2;
        FindAdviceList(page, kAdvisorWindowLargeList);
        image = kAdvisorWindowLargeImage;
    }
    SetupSingleAdvisorWinProc(advisorId, image);
    SetVisible(page, true);
    if (!advisor_) return;   // port guard: the original dereferences the advisor for the caption

    // The caption: GetName + " - " + GetPersonalName (std::string operator+ over two cRZString copies).
    const std::string text = Text(advisor_->GetName()) + " - " + Text(advisor_->GetPersonalName());
    const cRZString caption(text.c_str());
    for (std::uint32_t id : {kAdvisorWindowCaption1, kAdvisorWindowCaption2}) {
        if (cIGZWin* label = Child(id)) label->SetCaption(caption);
    }
    if (page) page->SetCaption(caption);
    GetMessageList(advisor_.get());
    updateBodyTextCtrl();
}

// @ 0x001ffacc: only while one advisor is shown.
void cSC4AdvisorUIAdvisorWinProc::SetSmallIndividualAdvisorWindow() {
    if (static_cast<unsigned>(advisorIndex_) >= kAdvisorWindowAdvisorCount) return;
    SetVisible(Child(kAdvisorWindowLarge), false);
    cIGZWin* page = Child(kAdvisorWindowSmall);
    SetVisible(page, true);
    if (adviceList_) {
        adviceList_->ClearList();
        adviceList_ = nullptr;
    }
    FindAdviceList(page, kAdvisorWindowSmallList);
    layout_ = 1;
    large_ = false;
    RemoveSingleAdvisorWinProc(advisorId_);
    SetupSingleAdvisorWinProc(advisorId_, kAdvisorWindowSmallImage);
    GetMessageList(g_host ? g_host->Advisor(advisorId_) : nullptr);
    updateBodyTextCtrl();
}

// @ 0x001ffc22: the mirror of the small layout.
void cSC4AdvisorUIAdvisorWinProc::SetLargeIndividualAdvisorWindow() {
    if (static_cast<unsigned>(advisorIndex_) >= kAdvisorWindowAdvisorCount) return;
    SetVisible(Child(kAdvisorWindowSmall), false);
    cIGZWin* page = Child(kAdvisorWindowLarge);
    SetVisible(page, true);
    if (adviceList_) {
        adviceList_->ClearList();
        adviceList_ = nullptr;
    }
    FindAdviceList(page, kAdvisorWindowLargeList);
    layout_ = 2;
    large_ = true;
    RemoveSingleAdvisorWinProc(advisorId_);
    SetupSingleAdvisorWinProc(advisorId_, kAdvisorWindowLargeImage);
    GetMessageList(g_host ? g_host->Advisor(advisorId_) : nullptr);
    updateBodyTextCtrl();
}

// @ 0x001fdc9a. One entry per listed advice. Header / icons by mood: 0 alarm with the blinking header and footer,
// 1 bad (high header), 5 good (medium header), anything else neutral (low header); an advice that is no longer current
// takes the expired header, the neutral icons and the grey body font (quirk: the footer chosen by the mood stays, so
// an expired alarm closes a <BLINK> it never opened). The title line is header + header font + title + footer, the
// body line is body font + message + body footer. Mood 7 passes the dot twice and no link; the others pass the down
// and right arrows and the advice's link flag. The rebuild then disables 0xaa1d969d and 0x6a1d96c3.
void cSC4AdvisorUIAdvisorWinProc::updateBodyTextCtrl() {
    if (!adviceList_) return;
    adviceList_->ClearList();
    adviceList_->SetbgImage(kAdvisorWindowListImage);
    for (const auto& entry : messages_) {
        cISC4Advice* advice = entry.get();
        if (!advice) continue;   // port guard
        const int mood = advice->GetMood();
        cRZString header;
        cRZString footer(kPriorityFooter);
        cRZString bodyFont(kBodyFont);
        const IconSet* icons = &kNeutralIcons;
        switch (mood) {
        case 0:
            footer = kHighestPriorityFooter;
            header = kHighestPriorityHeader;
            icons = &kAlarmIcons;
            break;
        case 1:
            header = kHighPriorityHeader;
            icons = &kBadIcons;
            break;
        case 5:
            header = kMediumPriorityHeader;
            icons = &kGoodIcons;
            break;
        default:
            header = kLowPriorityHeader;
            break;
        }
        if (!advice->CheckFlags(kAdviceCurrent)) {
            header = kExpiredPriorityHeader;
            icons = &kNeutralIcons;
            bodyFont = kBodyFontDeactive;
        }
        const cRZString title((Text(header) + Text(kHeaderFont) + Text(advice->GetTextTitle()) + Text(footer)).c_str());
        const cRZString body((Text(bodyFont) + Text(advice->GetTextMessage()) + Text(kBodyFooter)).c_str());
        if (mood == kMoodNone) {
            adviceList_->AddEntry(icons->dot, icons->dot, body, title, false, advice, icons->x, false,
                                  kAdvisorWindowListImage);
        } else {
            const bool link = advice->CheckFlags(kAdviceLink);
            adviceList_->AddEntry(icons->down, icons->right, body, title, link, advice, icons->x, false,
                                  kAdvisorWindowListImage);
        }
    }
    adviceList_->RebuildHTML(false);
    for (std::uint32_t id : {kAdvisorWindowButtonA, kAdvisorWindowButtonB}) {
        if (cIGZWin* button = Child(id)) button->SetFlag(kWinEnabled, false);
    }
}

// @ 0x001ff0b8. Every handled type answers true.
bool cSC4AdvisorUIAdvisorWinProc::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    if (type == kAdvisorWindowMoodToggled) {
        const int index = IndexOf(static_cast<int>(standard->GetData1()));
        if (index >= 0 && heads_[index]) heads_[index]->SetMood(static_cast<int>(standard->GetData2()));
        return true;
    }
    if (type != kAdvisorWindowAdviceAdded && type != kAdvisorWindowAdviceRemoved && type != kAdvisorWindowAdviceChanged &&
        type != kAdvisorWindowAdvisorRemoved) {
        return true;
    }
    cISC4Advice* advice = nullptr;
    cIGZUnknown* unknown = standard->GetIGZUnknown();
    const bool isAdvice = unknown && unknown->QueryInterface(kGZIID_cISC4Advice, reinterpret_cast<void**>(&advice)) && advice;
    const bool shownAdvisor = isAdvice && advice->GetAdvisorID() == advisorId_;
    if (shownAdvisor) {
        if (type == kAdvisorWindowAdviceAdded) {
            AddMessage(advice);
        } else if (type == kAdvisorWindowAdviceRemoved) {
            RemoveMessage(advice);
        } else {   // changed, and the advisor-removed path (its payload is an advisor, so the query fails there)
            for (std::uint32_t id : {kAdvisorWindowButtonA, kAdvisorWindowButtonB}) {
                if (cIGZWin* button = Child(id)) button->SetFlag(kWinEnabled, true);
            }
        }
    }
    SetAllMoods();
    updateBodyTextCtrl();
    if (advice) advice->Release();
    return true;
}

// @ 0x002000ba. Only the script window's messages; every path answers false.
bool cSC4AdvisorUIAdvisorWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (!window_ || window != window_->AsIGZWin()) return false;
    if (g_host) g_host->FilterUICommandMessage(message);
    if (message.type == gz::kWinProcInit) {
        RegisterMessageNotification(true);
        cIGZWin* parent = window->GetParentWin();
        if (!parent) return false;   // port guard
        for (std::uint32_t id : kAdvisorWindowNotifiers) {
            if (cIGZWin* control = parent->GetChildWindowFromIDRecursive(id)) control->SetNotificationTarget(window);
        }
        return false;
    }
    if (message.type == gz::kWinProcShutdown) {
        RegisterMessageNotification(false);
        return false;
    }
    if (message.type != kGZMsgCommand) return false;
    const std::uint64_t id = message.param2;
    if (message.param1 == kAdvisorWindowButtonPressed) {
        for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
            if (kAdvisorWindowHeadButtons[i] == id) {
                if (heads_[i]) heads_[i]->SetOffset(static_cast<int>(message.param3));
                return false;
            }
        }
        return false;
    }
    if (message.param1 != gz::kWinCommandSender) return false;
    if (In(id, kAdvisorWindowClose)) {
        HideWindow();
    } else if (In(id, kAdvisorWindowToLarge)) {
        SetLargeIndividualAdvisorWindow();
    } else if (In(id, kAdvisorWindowToSmall)) {
        SetSmallIndividualAdvisorWindow();
    } else if (In(id, kAdvisorWindowToAll)) {
        SetAllAdvisorsWindow();
    } else {
        for (int i = 0; i < kAdvisorWindowAdvisorCount; ++i) {
            if (kAdvisorWindowHeadButtons[i] == id) {
                SetIndividualAdvisorWindow(kAdvisorWindowAdvisorIds[i]);
                break;
            }
        }
    }
    return false;
}

}  // namespace sc4
