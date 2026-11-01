#include "sc4/WinRCI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gz/DrawContext.h"

#include "sc4/DemandSimulator.h"
#include "sc4/cSC4Demand.h"

namespace sc4 {

namespace {
IWinRCIHost* g_host = nullptr;
}
void SetWinRCIHost(IWinRCIHost* host) { g_host = host; }

cSC4WinRCI::cSC4WinRCI(cIGZWinMgr* mgr) : cGZWin(mgr) {}

cSC4WinRCI::~cSC4WinRCI() = default;

bool cSC4WinRCI::QueryInterface(std::uint32_t iid, void** out) {   // @ 0x100464d4c
    if (iid == 1 || iid == kGZIID_cISC4WinRCI) {
        *out = static_cast<cISC4WinRCI*>(this);
    } else if (iid == 0x22ba0121) {
        *out = static_cast<cIGZWin*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

bool cSC4WinRCI::Init() {   // @ 0x100464e14
    if (!get_init_flag()) {
        cGZWin::Init();   // the answer is ignored; RZ::WCM() is called here and its result unused
        if (g_host) {
            if (cIGZMessageServer2* server = g_host->MessageServer()) {
                server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kWinRCIDemandChangedMessage);
            }
        }
    }
    return true;
}

bool cSC4WinRCI::Shutdown() {   // @ 0x100464e78
    if (!get_init_flag()) return true;
    if (g_host) {
        if (cIGZMessageServer2* server = g_host->MessageServer()) {
            server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kWinRCIDemandChangedMessage);
        }
    }
    return cGZWin::Shutdown();
}

void cSC4WinRCI::SetDemandType(const std::uint32_t* ids, int count) {   // @ 0x100464ee4
    // Quirk kept: the original copies `count` ids into a 16-entry array without a check; see WINRCI-1.
    if (count > kWinRCIMaxChannels) count = kWinRCIMaxChannels;
    for (int i = 0; i < count; ++i) demandIds_[i] = static_cast<std::int32_t>(ids[i]);
    count_ = count;
    RethinkBar();
}

bool cSC4WinRCI::RethinkBar() {   // @ 0x100464f14
    cISC4DemandSimulator* simulator = g_host ? g_host->DemandSimulator() : nullptr;
    if (!simulator) return false;
    const int oldMinimum = minimumSum_, oldMaximum = maximumSum_, oldValue = valueSum_;
    minimumSum_ = 0;
    maximumSum_ = 0;
    valueSum_ = 0;
    for (int i = 0; i < count_; ++i) {
        if (demandIds_[i] <= 0) continue;
        cSC4Demand* demand = simulator->GetDemand(static_cast<std::uint32_t>(demandIds_[i]), kWinRCIDemandKeyOffset);
        if (!demand) continue;
        minimumSum_ = static_cast<int>(static_cast<float>(minimumSum_) + demand->QueryActiveDemandMin());
        maximumSum_ = static_cast<int>(static_cast<float>(maximumSum_) + demand->QueryActiveDemandMax());
        valueSum_ = static_cast<int>(static_cast<float>(valueSum_) + demand->QueryActiveDemandValue());
    }
    const bool changed = !(oldMinimum == minimumSum_ && oldMaximum == maximumSum_) || oldValue != valueSum_;
    if (changed && std::getenv("SC4_DEBUG"))
        std::fprintf(stderr, "rci: bar %d channels: min %d max %d value %d\n", count_, minimumSum_, maximumSum_, valueSum_);
    return changed;
}

bool cSC4WinRCI::DoMessage(cIGZMessage2* message) {   // @ 0x100465056
    if (message->GetType() == kWinRCIDemandChangedMessage) {
        const int id = static_cast<int>(static_cast<cIGZMessage2Standard*>(message)->GetData1());   // vslot 0x48
        for (int i = 0; i < count_; ++i) {
            if (demandIds_[i] != id) continue;
            if (RethinkBar()) InvalidateSelfAndParents();
            return false;
        }
    }
    return false;
}

namespace {
// ratio = value/limit + log(value)/log(limit), in single precision as the original computes it; the caller passes the
// magnitudes' logs the same way (logf of the negated ints for a negative value).
float BarRatio(int value, int limit, bool negative) {
    const float v = static_cast<float>(value);
    const float l = static_cast<float>(limit);
    const float ratio = v / l;
    const float logValue = std::log(negative ? static_cast<float>(-value) : v);
    const float logLimit = std::log(negative ? static_cast<float>(-limit) : l);
    return logValue / logLimit + ratio;
}
}  // namespace

bool cSC4WinRCI::GZPaint() {   // @ 0x1004650d6
    auto* context = static_cast<gz::cIGZDrawContext*>(drawContext_);
    context->SetColor(color_);
    const int width = area_.right - area_.left;
    const int height = area_.bottom - area_.top;
    if (valueSum_ == 0) return true;
    const bool negative = valueSum_ < 0;
    const float ratio = BarRatio(valueSum_, negative ? minimumSum_ : maximumSum_, negative);
    cRZRect bar{};
    if (width > height) {
        const int middle = (width + static_cast<int>(static_cast<std::uint32_t>(width) >> 31)) >> 1;
        const double half = static_cast<double>(middle) * 0.5;
        bar.top = 0;
        bar.bottom = height;
        if (!negative) {
            bar.right = middle + 1;
            bar.left = static_cast<int>(static_cast<double>(ratio) * half + 0.5 + static_cast<double>(middle + 1));
        } else {
            bar.right = middle;
            bar.left = static_cast<int>(static_cast<double>(middle) - (static_cast<double>(ratio) * half + 0.5));
        }
    } else {
        const int middle = (height + static_cast<int>(static_cast<std::uint32_t>(height) >> 31)) >> 1;
        const double half = static_cast<double>(middle) * 0.5;
        bar.left = 0;
        bar.right = width;
        if (!negative) {
            bar.bottom = middle;
            bar.top = static_cast<int>(static_cast<double>(middle) - (static_cast<double>(ratio) * half + 0.5));
        } else {
            bar.top = middle + 1;
            bar.bottom = static_cast<int>(static_cast<double>(ratio) * half + 0.5 + static_cast<double>(middle + 1));
        }
    }
    // The original's bar can run past the window (a value above the maximum gives a ratio over 2; a maximum of 1 makes
    // log(max) zero) and relies on the window clip; the port's window paint resets the clip, so the bar is kept inside here.
    if (bar.left > bar.right) std::swap(bar.left, bar.right);
    if (bar.top > bar.bottom) std::swap(bar.top, bar.bottom);
    bar.left = std::clamp(bar.left, 0, width);
    bar.right = std::clamp(bar.right, 0, width);
    bar.top = std::clamp(bar.top, 0, height);
    bar.bottom = std::clamp(bar.bottom, 0, height);
    context->Fill(bar);
    return true;
}

}  // namespace sc4
