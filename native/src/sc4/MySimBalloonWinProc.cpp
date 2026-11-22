#include "sc4/MySimBalloonWinProc.h"

#include <algorithm>

#include "gz/WinGen.h"
#include "rz/cRZMessage2Standard.h"

namespace sc4 {

namespace {
IMySimBalloonHost* g_host = nullptr;
constexpr std::uint32_t kGZIID_cIGZMessageTarget2Local = 0x452294aa;
constexpr std::uint32_t kBalloonAdviceShowing = 2;   // cISC4MySimBalloonAdvice status: on screen now
constexpr std::uint32_t kBalloonAdviceShown = 4;     // cISC4MySimBalloonAdvice status: was shown (or had no icon)

// The float -> unsigned conversion the original uses for the sound position (values below zero were clamped first).
int ToUnsigned(float value) { return static_cast<int>(static_cast<std::uint32_t>(value)); }
}  // namespace

IMySimBalloonHost* MySimBalloonHost() { return g_host; }
void SetMySimBalloonHost(IMySimBalloonHost* host) { g_host = host; }

cSC4MySimBalloonWinProc::cSC4MySimBalloonWinProc(cISC4MySim* sim) : sim_(sim) {
    if (sim_) sim_->AddRef();
    stop_balloon_sound(true);
}

cSC4MySimBalloonWinProc::~cSC4MySimBalloonWinProc() {
    if (window_) window_->Release();
    if (sim_) sim_->Release();
}

bool cSC4MySimBalloonWinProc::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == gz::kGZIID_cIGZWinProc) {
        *out = static_cast<gz::cIGZWinProc*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2Local) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cIGZUnknown) {
        *out = static_cast<cIGZUnknown*>(static_cast<gz::cIGZWinProc*>(this));
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4MySimBalloonWinProc::Release() {
    if (refs_ > 1) return --refs_;
    delete this;
    return 0;
}

cIGZWin* cSC4MySimBalloonWinProc::Win() const { return window_ ? window_->AsIGZWin() : nullptr; }

// tick_self: posts the tick message (the one just handled, or a new one) back to this target; the message server
// delivers it on its next tick, so the balloon runs once per frame.
void cSC4MySimBalloonWinProc::tick_self(cIGZMessage2* message) {
    cIGZMessageServer2* server = g_host ? g_host->Messages() : nullptr;
    if (!server) return;
    if (message) {
        server->GeneralMessagePostToTarget(message, static_cast<cIGZMessageTarget2*>(this));   // 0x1c
        return;
    }
    auto* tick = new cRZMessage2Standard();
    tick->AddRef();
    tick->SetType(kMessageMySimBalloonTick);
    server->GeneralMessagePostToTarget(tick, static_cast<cIGZMessageTarget2*>(this));
    tick->Release();
}

bool cSC4MySimBalloonWinProc::Shutdown() {
    if (initialized_) {
        advice_ = nullptr;
        if (sim_) { sim_->Release(); sim_ = nullptr; }
        if (window_) {
            if (cIGZWin* window = Win()) window->Shutdown();               // window 0x14
            window_->Release();
            window_ = nullptr;
        }
        initialized_ = false;
    }
    return true;
}

// check_enabled: disabled (5) while the sim is not in the city; the balloon hides then, and comes back hidden (4).
bool cSC4MySimBalloonWinProc::check_enabled() {
    if (!initialized_) return false;
    if (sim_) {
        if (!sim_->IsActive()) {                                          // 0x50
            if (state_ != 5) {
                cIGZWin* window = Win();
                if (window && window->IsVisible()) window->HideWindow();   // 0x11c / 0x118
                state_ = 5;
                timer_.Stop();
            }
        } else if (state_ == 5) {
            state_ = 4;
            timer_.Restart();
        }
    }
    return state_ != 5;
}

void cSC4MySimBalloonWinProc::stop_balloon_sound(bool immediate) {
    if (!g_host || !g_host->SoundSystemPresent() || !soundPlaying_) return;
    g_host->StopBalloonSound(sim_, immediate);                            // sound event 2
    soundPlaying_ = false;
}

// start_balloon_sound: the advice's sound for the sim's sex, played with the advice's chance (percent) when the sim may
// play a sound now, placed at the sim.
void cSC4MySimBalloonWinProc::start_balloon_sound() {
    if (!g_host || !g_host->SoundSystemPresent() || !advice_) return;
    const int soundId = advice_->GetSoundID(sim_->IsMale());              // 0x28(IsMale 0x48)
    const double roll = g_host->RandomPercent();
    if (!(roll < static_cast<double>(advice_->GetSoundChance())) || soundId == 0) return;   // 0x2c
    if (!sim_->RequestToPlaySound()) return;                              // 0xe0
    g_host->StartBalloonSound(static_cast<std::uint32_t>(advice_->GetSoundID(sim_->IsMale())), sim_);   // event 1
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (!sim_->GetLocation(position)) return;                             // 0x38
    for (float& value : position) value = std::max(value, 0.0f);
    g_host->PlaceBalloonSound(sim_, ToUnsigned(position[0]), ToUnsigned(position[1]), ToUnsigned(position[2]));
    soundPlaying_ = true;
}

bool cSC4MySimBalloonWinProc::Init() {
    if (!initialized_) {
        if (window_) { window_->Release(); window_ = nullptr; }
        window_ = g_host ? g_host->CreateWinGen() : nullptr;
        if (window_) {
            cIGZWin* window = window_->AsIGZWin();                         // 0xc
            if (cIGZWin* view = g_host->View3DWin()) view->ChildAdd(window);   // spView3DWin 0x38
            window->PrivateBuffer(true);                                   // 0x190
            window->SetFlag(0x20, true);                                   // 0x110
            window->SetFadeEffectPeriod(fade_, fade_);                     // 0x1b4
            window->SetArea(0, 0, 100, 100);                               // 0xd8
            window_->SetWinProc(this);                                     // 0x1c
            window_->SetWinGenFlag(0x1000, true);                          // 0x18
            window->HideWindow();                                          // 0x118
            window->SetFlag(0x8000, false);
            window->SetFlag(0x200000, true);
            window->SetFlag(0x20000, true);
            window->SetFlag(0x40000, true);
            window->SetFlag(4, true);
            window->SendToBack();                                          // 0x60
            window->SetID(kMySimBalloonWindowId);                          // 0x100
        }
        initialized_ = true;
    }
    timer_.Restart();
    tick_self(nullptr);
    return true;
}

// on_paint: the icons into the window's private buffer, the first at the bottom center, the others centered on it.
void cSC4MySimBalloonWinProc::on_paint() {
    if (!dirty_) return;
    cIGZWin* window = Win();
    auto* target = window ? static_cast<cIGZBuffer*>(window->GetPrivateBuffer()) : nullptr;   // 0x194
    if (!target || icons_.empty() || !icons_[0]) return;
    cIGZBuffer* first = icons_[0].get();
    const auto lockFlags = static_cast<cIGZBuffer::eLockFlags>(0x8010);
    target->Lock(lockFlags);                                               // 0x18
    const int down = static_cast<int>(target->Height()) - static_cast<int>(first->Height());
    const int across = static_cast<int>(static_cast<std::uint32_t>(static_cast<int>(target->Width()) -
                                                                   static_cast<int>(first->Width())) >> 1);
    auto blit = [target](cIGZBuffer* icon, int dx, int dy) {
        const cRZRect source = *icon->GetBufferArea();                     // 0x2c
        const cRZRect destination{source.left + dx, source.top + dy, source.right + dx, source.bottom + dy};
        target->Blt(icon, source, destination, nullptr);                   // 0x74
    };
    blit(first, across, down);
    for (std::size_t i = 1; i < icons_.size(); ++i) {
        cIGZBuffer* icon = icons_[i].get();
        if (!icon) continue;
        const int dy = static_cast<int>(static_cast<std::uint32_t>(static_cast<int>(first->Height()) -
                                                                   static_cast<int>(icon->Height())) >> 1) + down;
        const int dx = across + static_cast<int>(static_cast<std::uint32_t>(static_cast<int>(first->Width()) -
                                                                            static_cast<int>(icon->Width())) >> 1);
        blit(icon, dx, dy);
    }
    target->Unlock(lockFlags);                                             // 0x1c
    dirty_ = false;
}

bool cSC4MySimBalloonWinProc::DoWinProcMessage(cIGZWin* window, cGZMessage& message) {
    if (Win() != window) return false;
    if (message.type == kWinProcMsgPaint) {
        on_paint();
        return true;
    }
    return message.type == kWinProcMsgPaint + 1 || message.type == kWinProcMsgPaint - 1;
}

// set_new_balloon: the advice shown so far is marked shown; the sim's next event balloon is taken when it has an icon
// (it is marked shown otherwise), else the next regular balloon. The window grows to the largest icon.
bool cSC4MySimBalloonWinProc::set_new_balloon() {
    if (advice_) {
        advice_->SetStatusFlags(kBalloonAdviceShown, true);                // 0x34
        advice_->SetStatusFlags(kBalloonAdviceShowing, false);
        advice_ = nullptr;
        icons_.clear();
    }
    auto takeIcons = [this](cISC4MySimBalloonAdvice* advice) {
        const int count = advice->GetIconBalloonCount();                  // 0x1c
        icons_.resize(count > 0 ? static_cast<std::size_t>(count) : 0);
        bool any = false;
        for (int i = 0; i < count; ++i) {
            icons_[static_cast<std::size_t>(i)] = nullptr;
            if (cIGZBuffer* icon = advice->GetIconBalloon(i)) {            // 0x20
                icons_[static_cast<std::size_t>(i)] = cRZAutoRefCount<cIGZBuffer>(icon);
                any = true;
            }
        }
        return any;
    };
    if (cISC4MySimBalloonAdvice* event = sim_ ? sim_->GetNextBalloonEventAdvice() : nullptr) {   // 0xf4
        if (takeIcons(event)) {
            advice_ = cRZAutoRefCount<cISC4MySimBalloonAdvice>(event);
            advice_->SetStatusFlags(kBalloonAdviceShowing, true);
        } else {
            event->SetStatusFlags(kBalloonAdviceShown, true);
        }
    }
    cISC4MySimBalloonAdvice* regular = sim_ ? sim_->GetNextBalloonAdvice() : nullptr;          // 0xf0
    if (!advice_ && regular) {
        takeIcons(regular);
        advice_ = cRZAutoRefCount<cISC4MySimBalloonAdvice>(regular);
        advice_->SetStatusFlags(kBalloonAdviceShowing, true);
    }
    int width = 0, height = 0;
    for (const auto& icon : icons_) {
        if (!icon) continue;
        width = std::max(width, static_cast<int>(icon->Width()));          // 0x24
        height = std::max(height, static_cast<int>(icon->Height()));       // 0x28
    }
    if (cIGZWin* window = Win()) {
        if (window->GetW() < width || window->GetH() < height) window->SetSize(width, height);   // 0xa4 / 0xa8 / 0xd4
    }
    return !icons_.empty();
}

bool cSC4MySimBalloonWinProc::DoMessage(cIGZMessage2* message) {
    cIGZMessage2Standard* standard = nullptr;
    if (!message || !message->QueryInterface(kGZIID_cIGZMessage2Standard, reinterpret_cast<void**>(&standard)) ||
        !standard) {
        return false;
    }
    if (standard->GetType() != kMessageMySimBalloonTick) {
        standard->Release();
        return false;
    }
    tick_self(message);
    if (check_enabled()) {
        cIGZWin* window = Win();
        int x = -window->GetW();
        int y = -window->GetH();
        if (sim_->GetCurrentScreenLocationXY(&x, &y)) {                   // 0xd8
            x -= window->GetW() / 2;
            y = y - window->GetH() - 8;
        }
        window->GZWinMoveTo(x, y);                                         // 0xe0
        switch (state_) {
            case 1:                                                        // fading in
                if (timer_.GetElapsedTimeU32() > static_cast<std::uint32_t>(fade_)) {
                    start_balloon_sound();
                    state_ = 3;
                    timer_.Restart();
                }
                break;
            case 2:                                                        // fading out
                if (timer_.GetElapsedTimeU32() > static_cast<std::uint32_t>(fade_)) {
                    stop_balloon_sound(false);
                    state_ = 4;
                    timer_.Restart();
                }
                break;
            case 3:                                                        // shown
                if (timer_.GetElapsedTimeU32() > static_cast<std::uint32_t>(shown_)) {
                    window->HideWindow();
                    state_ = 2;
                    timer_.Restart();
                }
                break;
            case 4:                                                        // hidden: the next balloon when there is one
                if (timer_.GetElapsedTimeU32() > static_cast<std::uint32_t>(hidden_) && set_new_balloon()) {
                    stop_balloon_sound(false);
                    window->ShowWindow();                                  // 0x114
                    state_ = 1;
                    timer_.Restart();
                    window->InvalidateSelfAndParents();                    // 0x170
                    dirty_ = true;
                }
                break;
            default:
                break;
        }
    }
    standard->Release();
    return true;
}

}  // namespace sc4
