#include "sc4/ViewInputControlTerrainScrubber.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sc4/Exemplar.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/VisualEffect.h"

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

cSC4ViewInputControlTerrainScrubber::~cSC4ViewInputControlTerrainScrubber() {
    for (int i = 0; i < 2; ++i) ReleaseEcho(i, i);
}

void cSC4ViewInputControlTerrainScrubber::ReleaseEcho(int index, int transition) {
    if (!echo_[index]) return;
    if (echo_[index]->IsStarted()) echo_[index]->Stop(transition);
    echo_[index]->Release();
    echo_[index] = nullptr;
}

// The two echo effects of the brush's UI elements: the active pair while the button is down, the inactive pair otherwise
// (values 0/1 and 4/5, or 2/3 and 6/7 as effect group / type), and the matching cursor.
void cSC4ViewInputControlTerrainScrubber::SetCursorEchoState(bool active) {   // 0x002b5028
    const std::uint32_t* v = ui_.values;
    const std::uint32_t first[2] = {active ? v[0] : v[2], active ? v[1] : v[3]};
    const std::uint32_t second[2] = {active ? v[4] : v[6], active ? v[5] : v[7]};
    echoCursor_ = active ? v[8] : v[9];
    ReleaseEcho(0, 0);
    ReleaseEcho(1, 1);
    if (!g_host || !g_host->BrushEffectsEnabled()) return;   // render property 0x11
    echo_[0] = g_host->CreateEffect(first[0], first[1]);
    echo_[1] = g_host->CreateEffect(second[0], second[1]);
}

// The first echo is the brush circle, scaled to the brush radius times the width; the second is scaled by the zoom
// (3 / 1.7^zoom). Shown echoes start, hidden ones stop; the cursor follows.
void cSC4ViewInputControlTerrainScrubber::SetCursorEchoPos(const float position[3], bool show) {   // 0x002b496c
    echoPosition_[0] = position[0];
    echoPosition_[1] = position[1];
    echoPosition_[2] = position[2];
    for (int i = 0; i < 2; ++i) {
        cSC4VisualEffect* echo = echo_[i];
        if (!echo) continue;
        cS3DTransform transform;
        float scale = 1.0f;
        if (i == 0) {
            scale = (terrain_ ? terrain_->BrushRadius(brushType_) : 32.0f) * params_.radiusScale;   // terrain vslot 0xf4
        } else {
            scale = static_cast<float>(3.0 / std::pow(1.7, static_cast<double>(g_host ? g_host->ViewZoom() : 0)));
        }
        transform.scale = scale;
        transform.flags = static_cast<std::uint8_t>(1 | (scale != 1.0f ? 2 : 0));
        transform.translation[0] = position[0];
        transform.translation[1] = position[1];
        transform.translation[2] = position[2];
        transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 2);
        if (i == 0) echo->SetSourceTransform(transform);   // vslot 0x20 (as the place-lot tool's effects)
        else echo->SetEffectTransform(transform);          // vslot 0x1c
        if (show) {
            if (!echo->IsStarted()) echo->Start(0);
        } else if (echo->IsStarted()) {
            echo->Stop(i);
        }
    }
    SetCursor(show ? echoCursor_ : hiddenCursor_);
}

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
    SetCursorEchoState(false);
    SetCursor(hiddenCursor_);
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
    // The original stops both echoes but clears +0xe4 twice (the second effect keeps its reference until the tool goes).
    ReleaseEcho(0, 0);
    ReleaseEcho(1, 1);
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
        SetCursorEchoPos(position, false);
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
    SetCursorEchoState(true);
    SetCursorEchoPos(position, true);
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
    bool shown = false;
    if (GetCursorPositionInWorld(x, y, position) || scrubbing_) {
        if (scrubbing_) {
            PushDelayedAction(lastX_, lastZ_, position[0], position[2]);
            ProcessDelayedActions(false);
            lastX_ = position[0];
            lastZ_ = position[2];
            movedSinceTick_ = true;
        }
        shown = true;
    }
    SetCursorEchoPos(position, shown);
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
    SetCursorEchoState(false);
    if (echo_[0]) SetCursorEchoPos(echoPosition_, echo_[0]->IsStarted());
    return true;
}

bool cSC4ViewInputControlTerrainScrubber::OnMouseExit() {
    cursorOverView_ = false;
    if (echo_[0]) SetCursorEchoPos(echoPosition_, false);
    return false;
}

// Shift + 1..9, 0 sets the width (tenths, 0 = full); shift + F1..F10 the strength, and the echo follows. Shift and
// control alone (keys 0x10 / 0x11) pick the modifier strength and width and resize the echo, without taking the key.
bool cSC4ViewInputControlTerrainScrubber::OnKeyDown(std::uint32_t key, std::uint32_t modifiers) {
    if (!IsOnTop()) return false;
    if (modifiers & 1) {
        if (0x2f < key && key < 0x3a) {
            baseWidth_ = static_cast<float>(key == 0x30 ? 10 : static_cast<int>(key - 0x30)) / 10.0f;
            params_.radiusScale = baseWidth_;
            if (echo_[0]) SetCursorEchoPos(echoPosition_, true);
            return true;
        }
        if (0x6f < key && key < 0x7a) {
            baseStrength_ = static_cast<float>(key - 0x6f) / 10.0f;
            params_.strengthScale = baseStrength_;
            if (echo_[0]) SetCursorEchoPos(echoPosition_, true);
            return true;
        }
    }
    if (key - 0x10 > 1 || !echo_[0]) return false;
    ApplyModifierKeys(modifiers);
    SetCursorEchoPos(echoPosition_, true);
    return false;
}

bool cSC4ViewInputControlTerrainScrubber::OnKeyUp(std::uint32_t key, std::uint32_t modifiers) {   // 0x002b4d58
    if (key - 0x10 > 1 || !echo_[0]) return false;
    ApplyModifierKeys(modifiers);
    SetCursorEchoPos(echoPosition_, true);
    return false;
}

bool cSC4ViewInputControlTerrainScrubber::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    if (type == kTerrainScrubberMsgViewChanged[0] || type == kTerrainScrubberMsgViewChanged[1]) {
        // The camera moved under a still cursor: the cursor's world point is picked again.
        cISC4View3DWin* view = g_host ? g_host->View() : nullptr;
        if (view && view->GetCurrentViewInputControl() == this) {
            movedSinceTick_ = false;
            int x = 0, y = 0;
            if (g_host->CursorPosition(x, y)) OnMouseMove(x, y, 0);
            if (echo_[0]) SetCursorEchoPos(echoPosition_, echo_[0]->IsStarted());
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
