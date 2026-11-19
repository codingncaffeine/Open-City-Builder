// cSC4ObliterateCity (PC 0x0049C790..0x0049D820; spec OpenSC4 region-city-dialogs.md section 5). See the header.
#include "sc4/ObliterateCity.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

#include "rz/cRZMessage2Standard.h"

namespace sc4 {

namespace {
IObliterateCityHost* g_obliterateHost = nullptr;

bool Trace() {
    static const bool on = std::getenv("SC4_OBLITERATEDEBUG") != nullptr;
    return on;
}

// The tuning, read by the first tool after start-up and kept for the process (the static flag of the original).
struct HeldTuning {
    bool read = false;
    IObliterateCityHost::Tuning tuning;
    std::uint64_t smokeUs = 250000;
};
HeldTuning& Held() {
    static HeldTuning held;
    return held;
}

// The response curve (foundations.md 4): linear between the points, flat outside them.
float CurveValue(const std::vector<std::pair<float, float>>& curve, float x) {
    if (curve.empty()) return 0.0f;
    if (x <= curve.front().first) return curve.front().second;
    if (x >= curve.back().first) return curve.back().second;
    for (std::size_t i = 1; i < curve.size(); ++i) {
        if (x > curve[i].first) continue;
        const auto& [x0, y0] = curve[i - 1];
        const auto& [x1, y1] = curve[i];
        return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
    }
    return curve.back().second;
}
float CurveMaxInput(const std::vector<std::pair<float, float>>& curve) { return curve.empty() ? 0.0f : curve.back().first; }
}  // namespace

IObliterateCityHost* ObliterateCityHost() { return g_obliterateHost; }
void SetObliterateCityHost(IObliterateCityHost* host) { g_obliterateHost = host; }

// The tool's occupant filter (0x0049C7E0): every occupant except flora and fire.
class cSC4ObliterateCity::Filter final : public cSC4OccupantFilter {
public:
    bool IsOccupantIncluded(cISC4Occupant* occupant) override {
        const std::uint32_t type = occupant->GetType();
        return type != kOccupantTypeFlora && type != kOccupantTypeFire;
    }
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return type != kOccupantTypeFlora && type != kOccupantTypeFire; }
};

cSC4ObliterateCity::~cSC4ObliterateCity() {
    if (finale_) {
        finale_->Release();
        finale_ = nullptr;
    }
    if (filter_) {
        filter_->Release();
        filter_ = nullptr;
    }
}

bool cSC4ObliterateCity::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0x452294aa) {   // the message target at +0x28
        *out = static_cast<cIGZMessageTarget2*>(this);
        AddRef();
        return true;
    }
    if (iid == 1 || iid == kGZIID_cISC4ViewInputControl) {
        *out = static_cast<cSC4ViewInputControl*>(this);
        AddRef();
        return true;
    }
    return false;
}

// 0x0049D390
bool cSC4ObliterateCity::Init() {
    if (initialized_ || !g_obliterateHost) return true;
    IObliterateCityHost* host = g_obliterateHost;
    HeldTuning& held = Held();
    if (!held.read) {
        held.tuning = host->ReadTuning();
        held.smokeUs = static_cast<std::uint64_t>(held.tuning.smokeSeconds * 1000000.0f);
        held.read = true;
    }
    id_ = kObliterateCityToolId;
    cursorId_ = kObliterateCursorBusy;
    cSC4ViewInputControl::Init();
    SetCapture();
    host->DisableSave();
    auto* filter = new Filter();
    filter->AddRef();
    filter_ = filter;
    int cellsX = 0, cellsZ = 0;
    host->CityCellCount(cellsX, cellsZ);
    maxX_ = cellsX - 1;
    maxZ_ = cellsZ - 1;
    host->SimulatorHiddenPause();
    host->AddTick(static_cast<cIGZMessageTarget2*>(this), kObliterateTickPriority);
    if (cIGZMessageServer2* server = host->Messages()) server->AddNotification(static_cast<cIGZMessageTarget2*>(this), kMessageObliterateEffectEnded);
    // the block iterator: every block once, shuffled
    const int size = static_cast<int>(std::max<std::uint32_t>(1, held.tuning.blockSize));
    blocks_.clear();
    for (int bz = 0; bz <= maxZ_ / size; ++bz)
        for (int bx = 0; bx <= maxX_ / size; ++bx) blocks_.push_back({bx, bz});
    std::mt19937 random(static_cast<std::uint32_t>(std::rand()));
    std::shuffle(blocks_.begin(), blocks_.end(), random);
    next_ = 0;
    sceneUs_ = 0;
    processed_ = skipped_ = removedTarget_ = 0;
    state_ = kIsolatedRemoval;
    csiWasVisible_ = host->SituationIndicatorVisible();
    host->SetSituationIndicatorVisible(false);
    PostMessage(kMessageObliterateRadioFlag, 1);
    PostMessage(kMessageObliterateRadioStop, 0);
    if (Trace()) std::fprintf(stderr, "obliterate: init, %zu blocks of %d cells, curve to %.1f s, smoke %.1f s\n", blocks_.size(), size,
                              CurveMaxInput(held.tuning.curve), held.tuning.smokeSeconds);
    return true;
}

// 0x0049C8B0
bool cSC4ObliterateCity::Shutdown() {
    if (!initialized_) return true;
    IObliterateCityHost* host = g_obliterateHost;
    if (host) {
        if (cIGZMessageServer2* server = host->Messages()) server->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kMessageObliterateEffectEnded);
        host->RemoveTick(static_cast<cIGZMessageTarget2*>(this), kObliterateTickPriority);
    }
    if (finale_) {
        finale_->Stop(true);
        finale_->Release();
        finale_ = nullptr;
    }
    if (host) host->SimulatorHiddenResume();
    if (filter_) {
        filter_->Release();
        filter_ = nullptr;
    }
    if (host) {
        host->EnableSave();
        host->SetSituationIndicatorVisible(csiWasVisible_);
    }
    ReleaseCapture();
    if (Trace()) std::fprintf(stderr, "obliterate: shut down in state %d after %d blocks\n", state_, processed_);
    return cSC4ViewInputControl::Shutdown();
}

// 0x0049C790: every key is taken; Escape ends the scene where it is.
bool cSC4ObliterateCity::OnKeyDown(std::uint32_t key, std::uint32_t) {
    if (key == 0x1b) EndInput();
    return true;
}

// DoAnimationTick (0x0049D820): the tick drives the states; the finale's end message ends the scene.
bool cSC4ObliterateCity::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    if (type == kMessageObliterateEffectEnded) {
        if (finale_) {
            IObliterateEffect* effect = finale_;
            finale_ = nullptr;
            effect->Release();
            EndScene();
        }
        return true;
    }
    if (type == 0x86ad10ee) Tick(static_cast<cIGZMessage2Standard*>(message)->GetData2());
    return true;
}

void cSC4ObliterateCity::Tick(std::uint64_t elapsedUs) {
    switch (state_) {
        case kIsolatedRemoval:
            sceneUs_ += elapsedUs;
            DoIsolatedRemoval();
            break;
        case kSmokeForming:
            smokeUs_ += elapsedUs;
            if (smokeUs_ >= Held().smokeUs) StartFinale();
            break;
        case kFinale:
            finaleUs_ += elapsedUs;
            if (finaleUs_ > 20000000u && finale_) {
                finale_->Stop(false);
                finale_->Release();
                finale_ = nullptr;
            }
            if (!finale_ && state_ != kEnded) EndScene();
            break;
        default:
            break;   // idle and ended ignore ticks
    }
}

// 0x0049D5A0: the curve gives how many blocks should be gone by now; the increase is removed in one batch.
void cSC4ObliterateCity::DoIsolatedRemoval() {
    IObliterateCityHost* host = g_obliterateHost;
    const auto& tuning = Held().tuning;
    const float seconds = static_cast<float>(sceneUs_) / 1000000.0f;
    if (seconds >= CurveMaxInput(tuning.curve)) {
        StartSmokeScreenFormingState();
        return;
    }
    const int target = static_cast<int>(std::nearbyint(CurveValue(tuning.curve, seconds)));
    if (target <= removedTarget_) return;
    const int batch = target - removedTarget_;
    removedTarget_ = target;
    const int size = static_cast<int>(std::max<std::uint32_t>(1, tuning.blockSize));
    for (int i = 0; i < batch; ++i) {
        if (next_ >= blocks_.size()) {
            StartSmokeScreenFormingState();
            return;
        }
        const auto [bx, bz] = blocks_[next_++];
        const int x0 = std::min(bx * size, maxX_), z0 = std::min(bz * size, maxZ_);
        const int x1 = std::min(bx * size + size - 1, maxX_), z1 = std::min(bz * size + size - 1, maxZ_);
        // sparkles for the first block, an occupied one, or after six empty ones in a row
        if (host) {
            if (processed_ == 0 || host->CellsOccupied(x0, z0, x1, z1, filter_) || skipped_ >= 6) {
                skipped_ = 0;
                if (IObliterateEffect* sparkle = host->CreateEffect("sparkles_on_demolish", static_cast<float>(x0 + x1) * 16.0f * 0.5f,
                                                                    static_cast<float>(z0 + z1) * 16.0f * 0.5f, false, 0))
                    sparkle->Release();
            } else {
                ++skipped_;
            }
            host->DemolishCells(x0, z0, x1, z1, filter_);
        }
        ++processed_;
    }
    if (Trace()) std::fprintf(stderr, "obliterate: %.2f s, %d blocks removed, %zu left\n", seconds, processed_, blocks_.size() - next_);
}

// 0x0049CD90: the finale effect starts at the centre of the city and tells the tool when it ends.
void cSC4ObliterateCity::StartSmokeScreenFormingState() {
    state_ = kSmokeForming;
    smokeUs_ = 0;
    IObliterateCityHost* host = g_obliterateHost;
    if (host) {
        finale_ = host->CreateEffect("obliterate_finale", static_cast<float>(maxX_) * 16.0f * 0.5f, static_cast<float>(maxZ_) * 16.0f * 0.5f, true,
                                     kMessageObliterateEffectEnded);
    }
    if (Trace()) std::fprintf(stderr, "obliterate: smoke screen forming (finale effect %s)\n", finale_ ? "started" : "missing");
}

// 0x0049D070 / 0x0049CEA0: everything the filter accepts goes at once, the whole city is demolished and dezoned.
void cSC4ObliterateCity::StartFinale() {
    state_ = kFinale;
    if (IObliterateCityHost* host = g_obliterateHost) {
        host->RemoveOccupants(filter_);
        host->DemolishCells(0, 0, maxX_, maxZ_, filter_);
        host->DezoneCity(maxX_, maxZ_);
    }
    finaleUs_ = 0;
    if (Trace()) std::fprintf(stderr, "obliterate: finale, the city is cleared\n");
}

// 0x0049CC30
void cSC4ObliterateCity::EndScene() {
    if (state_ == kEnded) return;
    state_ = kEnded;
    AddRef();   // EndInput releases the view's reference; the post below still needs the tool's state
    EndInput();
    PostMessage(kMessageCityObliterationSceneEnded, 0);
    if (Trace()) std::fprintf(stderr, "obliterate: scene ended\n");
    Release();
}

// 0x0049CA70: a standard message queued on message server 2.
void cSC4ObliterateCity::PostMessage(std::uint32_t type, std::uint64_t data1) {
    cIGZMessageServer2* server = g_obliterateHost ? g_obliterateHost->Messages() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(data1);
    server->MessagePost(message, false);
    message->Release();
}

}  // namespace sc4
