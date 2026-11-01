#include "sc4/ViewInputControlTerrainScrubber.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sc4/Exemplar.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
ITerrainScrubberHost* g_host = nullptr;

// The statics ReadTuningParameters overrides (Mac i386 0x00ab9a00..0x00ab9a14).
bool sbInited = false;
std::uint32_t kTexUpdateInterval = 100;           // applications between texture flushes
float kModKey1StrengthMultiplier = 0.25f;         // control held
float kModKey1WidthMultiplier = 0.25f;
float kModKey2StrengthMultiplier = 4.0f;          // shift held
float kModKey2WidthMultiplier = 1.0f;
float kBrushStrengthIncreaseRate = 10.0f;         // per second since the last application
constexpr float kMaxStrengthRatio = 2.0f;
constexpr cGZPersistResourceKey kTuningKey{0x6534284a, 0x88cd66e9, 1};
}  // namespace

ITerrainScrubberHost* TerrainScrubberHost() { return g_host; }
void SetTerrainScrubberHost(ITerrainScrubberHost* host) { g_host = host; }

cSC4ViewInputControlTerrainScrubber::cSC4ViewInputControlTerrainScrubber(std::uint32_t brushType, bool freeBrush)
    : brushType_(brushType), freeBrush_(freeBrush) {
    id_ = kTerrainScrubberToolId;
    cursorId_ = kTerrainScrubberCursorId;
}

cSC4ViewInputControlTerrainScrubber::~cSC4ViewInputControlTerrainScrubber() = default;

bool cSC4ViewInputControlTerrainScrubber::QueryInterface(std::uint32_t iid, void** out) {   // 0x002b44c0
    if (iid == kGZIID_cIGZMessageTarget2) {
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

void cSC4ViewInputControlTerrainScrubber::ReadTuningParameters() {
    cISCPropertyHolder* tuning = g_host ? g_host->TuningExemplar() : nullptr;
    if (!tuning) return;
    float rampSeconds = 0.0f;
    cSCPropertyHelp::GetPropertyValue(tuning, 0x69db8d00, kTexUpdateInterval);
    cSCPropertyHelp::GetPropertyValue(tuning, 0x69db8d01, kModKey1StrengthMultiplier);
    cSCPropertyHelp::GetPropertyValue(tuning, 0x69db8d02, kModKey1WidthMultiplier);
    cSCPropertyHelp::GetPropertyValue(tuning, 0x69db8d03, kModKey2StrengthMultiplier);
    cSCPropertyHelp::GetPropertyValue(tuning, 0x69db8d04, kModKey2WidthMultiplier);
    cSCPropertyHelp::GetPropertyValue(tuning, 0x69db8d05, rampSeconds);
    if (0.0f < rampSeconds) kBrushStrengthIncreaseRate = 1.0f / rampSeconds;
}

// The original also registers StaticShutdown with the application (SL::spSC4 vslot 0x10c) to clear the flag.
void cSC4ViewInputControlTerrainScrubber::StaticInit() {
    if (sbInited) return;
    sbInited = true;
    ReadTuningParameters();
}

bool cSC4ViewInputControlTerrainScrubber::Init() {
    if (initialized_) return true;
    if (!sbInited) StaticInit();
    cSC4ViewInputControl::Init();
    terrain_ = g_host ? g_host->Terrain() : nullptr;
    if (terrain_) terrain_->GetUIElementsForBrush(brushType_, ui_);   // vslot 0xfc
    // The original also keeps the terrain view (vslot 0x108) to switch its automatic cell cursor off while scrubbing.
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr)
        for (std::uint32_t type : kTerrainScrubberMsgViewChanged) server->AddNotification(this, type);
    // SetCursorEchoState(false): the brush-circle echo effects (SL::spEffectsManager) are not created natively.
    return true;
}

bool cSC4ViewInputControlTerrainScrubber::Shutdown() {
    if (!initialized_) return true;
    if (scrubbing_) {
        if (terrain_) terrain_->EndBrushScrub(brushType_, params_.redisplayFlags);   // vslot 0xf0
        cursorOverView_ = scrubbing_ = false;
        ReleaseCapture();
        if (g_host) g_host->StopTick(this, kTerrainScrubberTickInterval);
    }
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr)
        for (std::uint32_t type : kTerrainScrubberMsgViewChanged) server->RemoveNotification(this, type);
    if (terrain_) {
        terrain_->StopAccumulatingTextureUpdates();                  // vslot 0x13c
        terrain_ = nullptr;
    }
    return cSC4ViewInputControl::Shutdown();
}

// The anchor is the terrain altitude under the press: with caps lock on, the drag follows the plane at that height.
bool cSC4ViewInputControlTerrainScrubber::SetCursorAnchor(int x, int y) {
    cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
    float position[3] = {};
    if (!view || !view->PickTerrain(x, y, position, true)) return false;   // view vslot 0x90
    if (terrain_) anchorAltitude_ = terrain_->GetAltitude(position[0], position[2]);   // terrain vslot 0x30
    return true;
}

bool cSC4ViewInputControlTerrainScrubber::GetCursorPositionInWorld(int x, int y, float* out) {
    bool found = false;
    if (scrubbing_ && g_host && g_host->CapsLockOn()) {
        found = true;
    } else {
        cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
        if (view && view->PickTerrain(x, y, out, false)) return true;
    }
    // SC4ViewUtilities: the ray, lowered by the anchor altitude, against the ground plane, clipped to the city.
    if (g_host && g_host->ViewportPointOnPlane(x, y, anchorAltitude_, out)) out[1] = 0.0f;
    return found;
}

void cSC4ViewInputControlTerrainScrubber::PushDelayedAction(float fromX, float fromZ, float toX, float toZ) {
    actions_.push_back({dragTimer_.GetElapsedTimeU32() + actionDelay_, fromX, fromZ, toX, toZ});
}

void cSC4ViewInputControlTerrainScrubber::RecordBrushApplication() {
    ticksSincePush_ = 0;
    if (kTexUpdateInterval < ++applicationsSinceFlush_) {
        if (terrain_) terrain_->StopAccumulatingTextureUpdates();
        applicationsSinceFlush_ = 0;
        if (kTexUpdateInterval != 0 && terrain_) terrain_->StartAccumulatingTextureUpdates();
    }
}

// Every due action is dropped but the last, which the brush is applied along once. Past the first application the
// strength grows with the time since the previous one (kBrushStrengthIncreaseRate per second, at most twice).
void cSC4ViewInputControlTerrainScrubber::ProcessDelayedActions(bool all) {
    const std::uint32_t now = all ? UINT_MAX : dragTimer_.GetElapsedTimeU32();
    bool any = false;
    DelayedAction action{};
    while (!actions_.empty() && actions_.front().due <= now) {
        action = actions_.front();
        actions_.pop_front();
        any = true;
    }
    if (!any || !terrain_) return;
    const float savedStrength = params_.strengthScale;
    if (!firstApplication_) {
        float seconds = 0.0f;
        sinceApplication_.GetElapsedTimeFloat(seconds);   // microseconds
        const float ratio = std::min(seconds / 1000000.0f * kBrushStrengthIncreaseRate, kMaxStrengthRatio);
        params_.strengthScale *= ratio;
    }
    sinceApplication_.Restart();
    params_.previousX = action.fromX;
    params_.previousZ = action.fromZ;
    params_.currentX = action.toX;
    params_.currentZ = action.toZ;
    params_.valid = true;
    bool apply = true;
    if (!freeBrush_) {
        const float scaled = static_cast<float>(terrain_->GetCostPerBrushApplication(brushType_)) * params_.strengthScale;
        const std::int64_t cost = static_cast<std::int64_t>(std::floor(scaled + 0.5f));
        if (cISC4View3DWin* view = g_host ? g_host->View() : nullptr)
            view->SetCostIndicator(static_cast<std::uint32_t>(runningCost_ + cost), static_cast<std::uint32_t>(cost));   // vslot 0xac
        apply = !g_host || !g_host->BudgetPresent() || g_host->WithdrawFunds(cost);
        if (apply) runningCost_ += cost;
    }
    const float before = terrain_->GetAltitude(params_.currentX, params_.currentZ);
    const bool applied = apply && terrain_->ApplyBrush(brushType_, params_);   // vslot 0xe4
    if (std::getenv("SC4_DEBUG"))
        std::fprintf(stderr, "terrain brush %#x: %s at %.0f,%.0f strength %.2f altitude %.2f -> %.2f rect %d,%d..%d,%d\n", brushType_,
                     !apply ? "no funds" : applied ? "applied" : "refused", params_.currentX, params_.currentZ, params_.strengthScale, before,
                     terrain_->GetAltitude(params_.currentX, params_.currentZ), params_.rect.left, params_.rect.top, params_.rect.right, params_.rect.bottom);
    firstApplication_ = false;
    RecordBrushApplication();
    params_.strengthScale = savedStrength;
}

// Control (2) picks the fine brush, shift (1) the strong one; otherwise the strength and width the keys set.
void cSC4ViewInputControlTerrainScrubber::ApplyModifierKeys(std::uint32_t modifiers) {
    params_.strengthScale = baseStrength_;
    params_.radiusScale = baseWidth_;
    if (modifiers & 2) {
        params_.strengthScale = kModKey1StrengthMultiplier;
        params_.radiusScale = kModKey1WidthMultiplier;
    } else if (modifiers & 1) {
        params_.strengthScale = kModKey2StrengthMultiplier;
        params_.radiusScale = kModKey2WidthMultiplier;
    }
}

bool cSC4ViewInputControlTerrainScrubber::OnMouseDownL(int x, int y, std::uint32_t modifiers) {
    cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
    if (!view || !terrain_) return false;
    float position[3] = {};
    if (!SetCursorAnchor(x, y) || !GetCursorPositionInWorld(x, y, position)) {
        if (std::getenv("SC4_DEBUG")) std::fprintf(stderr, "terrain brush %#x: no terrain under %d,%d\n", brushType_, x, y);
        return true;
    }
    if (!freeBrush_) {   // a brush with a cost does not start with an empty treasury
        const std::int64_t cost = terrain_->GetCostPerBrushApplication(brushType_);   // vslot 0xf8
        if (g_host->BudgetPresent() && 0 < cost && g_host->Funds() < 1) return true;
    }
    scrubbing_ = cursorOverView_ = firstApplication_ = true;
    params_.valid = true;
    params_.zoom = -1;
    params_.redisplayFlags = freeBrush_ ? 1u : 2u;
    params_.levelAltitude = position[1];
    ApplyModifierKeys(modifiers);
    terrain_->BeginBrushScrub(brushType_);                          // vslot 0xec (type, redisplay flags)
    lastX_ = position[0];
    lastZ_ = position[2];
    view->KillKeyboardScrolling();                                  // vslot 0x8c
    SetCapture();
    applicationsSinceFlush_ = 0;
    if (kTexUpdateInterval != 0) terrain_->StartAccumulatingTextureUpdates();   // vslot 0x138
    runningCost_ = 0;
    dragTimer_.Restart();
    PushDelayedAction(lastX_, lastZ_, lastX_, lastZ_);
    ProcessDelayedActions(false);
    g_host->StartTick(this, kTerrainScrubberTickInterval);          // SL::ATM() vslot 0x14
    return true;
}

bool cSC4ViewInputControlTerrainScrubber::OnMouseMove(int x, int y, std::uint32_t) {
    cursorOverView_ = scrubbing_;
    if (movedSinceTick_) return true;   // one application per animation tick from the moves
    float position[3] = {};
    if (GetCursorPositionInWorld(x, y, position) || scrubbing_) {
        if (scrubbing_) {
            PushDelayedAction(lastX_, lastZ_, position[0], position[2]);
            ProcessDelayedActions(false);
            lastX_ = position[0];
            lastZ_ = position[2];
            movedSinceTick_ = true;
        }
    }
    return true;
}

bool cSC4ViewInputControlTerrainScrubber::OnMouseUpL(int, int, std::uint32_t) {
    if (!scrubbing_) return false;
    ProcessDelayedActions(true);
    if (cISC4View3DWin* view = g_host ? g_host->View() : nullptr) view->ClearCostIndicator(true);   // vslot 0xb0
    ReleaseCapture();
    if (g_host) g_host->StopTick(this, kTerrainScrubberTickInterval);
    if (terrain_) terrain_->EndBrushScrub(brushType_, params_.redisplayFlags);
    cursorOverView_ = scrubbing_ = movedSinceTick_ = false;
    if (terrain_) terrain_->StopAccumulatingTextureUpdates();
    return true;
}

bool cSC4ViewInputControlTerrainScrubber::OnMouseExit() {
    cursorOverView_ = false;
    return false;
}

// Shift + 1..9, 0 sets the width (tenths, 0 = full); shift + F1..F10 the strength. Shift and control alone only
// re-pick the echo's brush, and there is no echo effect natively, so they fall through like the original without one.
bool cSC4ViewInputControlTerrainScrubber::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (!IsOnTop()) return false;
    if (modifiers & 1) {
        if (0x2f < key && key < 0x3a) {
            baseWidth_ = static_cast<float>(key == 0x30 ? 10 : static_cast<int>(key - 0x30)) / 10.0f;
            params_.radiusScale = baseWidth_;
            return true;
        }
        if (0x6f < key && key < 0x7a) {
            baseStrength_ = static_cast<float>(key - 0x6f) / 10.0f;
            params_.strengthScale = baseStrength_;
            return true;
        }
    }
    return false;
}

bool cSC4ViewInputControlTerrainScrubber::OnKeyUp(std::uint32_t, std::uint32_t) { return false; }

bool cSC4ViewInputControlTerrainScrubber::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    if (type == kTerrainScrubberMsgViewChanged[0] || type == kTerrainScrubberMsgViewChanged[1]) {
        // The camera moved under a still cursor: the cursor's world point is picked again.
        cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
        if (view && view->GetCurrentViewInputControl() == this) {
            movedSinceTick_ = false;
            int x = 0, y = 0;
            if (g_host->CursorPosition(x, y)) OnMouseMove(x, y, 0);
        }
    } else if (type == kTerrainScrubberMsgTick && scrubbing_) {
        movedSinceTick_ = false;
        if (2 < ++ticksSincePush_) {   // holding still keeps applying the brush where it is
            PushDelayedAction(lastX_, lastZ_, lastX_, lastZ_);
            ticksSincePush_ = 0;
        }
        ProcessDelayedActions(false);
    }
    return true;
}

}  // namespace sc4
