#include "sc4/ViewInputControlDisaster.h"

#include "sc4/AnimationTickManager.h"

namespace sc4 {

namespace {
IDisasterToolHost* g_host = nullptr;
}
IDisasterToolHost* DisasterToolHost() { return g_host; }
void SetDisasterToolHost(IDisasterToolHost* host) { g_host = host; }

// ---- the torch -------------------------------------------------------------------------------------------------------------

bool cSC4ViewInputControlTorch::Init() {
    if (initialized_) return true;
    cursorId_ = kTorchCursor;
    id_ = kDisasterToolId;
    active_ = false;
    cSC4ViewInputControl::Init();
    timer_.Start();
    SendAudio(kTorchAudioInit, 0, 0, 0);
    return true;
}

bool cSC4ViewInputControlTorch::Shutdown() {
    if (!initialized_) return true;
    timer_.Stop();
    ShutdownTorchEffect();
    SendAudio(kTorchAudioShutdown, 0, 0, 0);
    if (g_host) g_host->StopTick(this, kTorchTickInterval);
    cSC4ViewInputControl::Shutdown();
    return true;
}

void cSC4ViewInputControlTorch::SendAudio(std::uint32_t type, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    if (g_host) g_host->SendToolAudio(type, a, b, c);
}

// The idle flame under the cursor.
void cSC4ViewInputControlTorch::InitTorchEffect() {
    if (effect_) return;
    IEffectToolHost* effects = EffectToolHost();
    IToolEffectsManager* manager = effects ? effects->EffectsManager() : nullptr;
    if (manager && manager->CreateEffect("torch_inactive", &effect_) && effect_) effect_->Start(0);
    active_ = false;
}

void cSC4ViewInputControlTorch::UpdateTorchEffect() {
    if (!effect_) return;
    cS3DTransform transform;
    transform.translation[0] = pos_.x;
    transform.translation[1] = pos_.y;
    transform.translation[2] = pos_.z;
    transform.flags |= 1;
    transform.changeCounter = static_cast<std::uint8_t>(transform.changeCounter + 1);
    effect_->SetSourceTransform(transform);
}

void cSC4ViewInputControlTorch::ShutdownTorchEffect() {
    if (effect_) {
        if (effect_->IsStarted()) effect_->Stop(1);
        effect_->Release();
        effect_ = nullptr;
    }
    active_ = false;
    SendAudio(kTorchAudioActive, 0, 0, 0);
}

// Swaps the cursor's flame for the burning one, or back.
void cSC4ViewInputControlTorch::ActivateTorch(bool on) {
    if (!effect_ || active_ == on) return;
    active_ = on;
    effect_->Stop(0);
    effect_->Release();
    effect_ = nullptr;
    IEffectToolHost* effects = EffectToolHost();
    IToolEffectsManager* manager = effects ? effects->EffectsManager() : nullptr;
    if (on) {
        if (manager) manager->CreateEffect("torch_active", &effect_);
    } else {
        if (manager) manager->CreateEffect("torch_inactive", &effect_);
        SendAudio(kTorchAudioUp, 0, 0, 0);
    }
    if (effect_) effect_->Start(0);
    SendAudio(kTorchAudioActive, on ? 1u : 0u, 0, 0);
}

// The button goes down over the terrain: the cell there is lit, the tool ticks while it stays down.
bool cSC4ViewInputControlTorch::OnMouseDownL(int x, int y, std::uint32_t) {
    SendAudio(kTorchAudioDown, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), timer_.GetElapsedTimeU32());
    auto* view = static_cast<IEffectView*>(view3D_);
    if (!view || !view->PickTerrain(x, y, &pos_, false)) return true;
    ActivateTorch(true);
    UpdateTorchEffect();
    if (g_host) {
        // Port guard: the original keeps this cell in locals, so until the cursor moves its ticks light the cell the tool
        // burned last (cell 0,0 on a first press); here the ticks start on the pressed cell.
        g_host->PositionToCell(pos_.x, pos_.z, cellX_, cellZ_);
        g_host->StartTick(this, kTorchTickInterval);
        g_host->IgniteCell(cellX_, cellZ_, false, true, 0);
        g_host->SetFireCause(1);
    }
    SetCursor(kTorchCursorBurning);
    return true;
}

bool cSC4ViewInputControlTorch::OnMouseUpL(int x, int y, std::uint32_t) {
    SendAudio(kTorchAudioUp, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), timer_.GetElapsedTimeU32());
    if (g_host) g_host->StopTick(this, kTorchTickInterval);
    ActivateTorch(false);
    SetCursor(cursorId_);
    return true;
}

// The flame follows the cursor; with the button held (modifier bit 0x10) it burns, and the cell it burns follows too.
bool cSC4ViewInputControlTorch::OnMouseMove(int x, int y, std::uint32_t mods) {
    const std::uint32_t elapsed = timer_.GetElapsedTimeU32();
    auto* view = static_cast<IEffectView*>(view3D_);
    if (!view || !view->PickTerrain(x, y, &pos_, false)) {
        ShutdownTorchEffect();
    } else {
        if (!effect_) InitTorchEffect();
        if (!active_ && (mods & 0x10) != 0) ActivateTorch(true);
        UpdateTorchEffect();
        if (active_ && g_host) g_host->PositionToCell(pos_.x, pos_.z, cellX_, cellZ_);
    }
    SendAudio(kTorchAudioMove, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y), elapsed);
    return true;
}

bool cSC4ViewInputControlTorch::OnMouseExit() {
    ShutdownTorchEffect();
    return true;
}

// Every animation tick while it burns: the cell under the cursor is lit again.
bool cSC4ViewInputControlTorch::DoMessage(cIGZMessage2* message) {
    if (message && message->GetType() == kAnimationTickMessage && active_ && g_host) {
        g_host->IgniteCell(cellX_, cellZ_, true, true, 0);
        g_host->SetFireCause(1);
    }
    return true;
}

// ---- the effect disaster tool ------------------------------------------------------------------------------------------------

cSC4ViewInputControlEffectDisaster::cSC4ViewInputControlEffectDisaster(bool hasPickProperty, bool pick) {
    cursorId_ = kEffectDisasterCursor;
    id_ = kDisasterToolId;
    if (hasPickProperty) pickFlag_ = pick;
}

// Until a disaster names its own, the effect disaster manager runs what the tool starts.
bool cSC4ViewInputControlEffectDisaster::Init() {
    if (initialized_) return false;
    cSC4ViewInputControlEffect::Init();
    managerFound_ = g_host && g_host->HasDisasterManager(kEffectDisasterManagerDefault);
    ownManager_ = false;
    managerId_ = kEffectDisasterManagerDefault;
    return true;
}

void cSC4ViewInputControlEffectDisaster::SetDisasterManager(std::uint32_t id) {
    if (id == 0) return;
    if (g_host && g_host->HasDisasterManager(id)) {
        managerFound_ = true;
        ownManager_ = true;
    }
    managerId_ = id;
}

bool cSC4ViewInputControlEffectDisaster::Shutdown() {
    if (!initialized_) return false;
    managerFound_ = false;
    if (instance_) {
        instance_->Release();
        instance_ = nullptr;
    }
    if (control_) {
        control_->Release();
        control_ = nullptr;
    }
    cSC4ViewInputControlEffect::Shutdown();
    return true;
}

// A disaster with a manager of its own starts where the button came up (the picked point, in whole world units).
bool cSC4ViewInputControlEffectDisaster::OnMouseUpL(int x, int y, std::uint32_t mods) {
    if (!cSC4ViewInputControlEffect::OnMouseUpL(x, y, mods)) return false;
    if (managerFound_ && ownManager_ && g_host)
        g_host->AttemptDisasterStart(managerId_, static_cast<int>(pickPos_.x), static_cast<int>(pickPos_.z));
    return true;
}

// The effect lands: with the effect disaster manager it becomes a disaster instance; lifting it stops that instance.
// (Unlike the base, the original does not start the control effect itself here.)
void cSC4ViewInputControlEffectDisaster::SetTerrainControlledEffectState(bool on) {
    if (on) {
        if (!control_) return;   // the original has no null check
        control_->SetSourceTransform(transform_);
        if (!ownManager_ && managerFound_ && g_host) {
            cIGZUnknown* made = g_host->StartDisasterEffect(kEffectDisasterManagerDefault, control_, pickPos_, managerId_);
            if (instance_) instance_->Release();
            instance_ = made;
        }
    } else if (instance_ && g_host) {
        g_host->StopDisasterEffect(instance_);
    }
}

}  // namespace sc4
