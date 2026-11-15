#include "sc4/DisasterInstanceVolcano.h"

#include <vector>

#include "gz/cIGZMessage2.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

IVolcanoHost* g_host = nullptr;
VolcanoTuning g_tuning;
std::uint32_t g_nextToken = 1;

}  // namespace

IVolcanoHost* VolcanoHost() { return g_host; }
void SetVolcanoHost(IVolcanoHost* host) { g_host = host; }
const VolcanoTuning& cSC4DisasterInstanceVolcano::Tuning() { return g_tuning; }

cSC4DisasterInstanceVolcano::cSC4DisasterInstanceVolcano() : token_(g_nextToken++) {}

cSC4DisasterInstanceVolcano::~cSC4DisasterInstanceVolcano() {
    if (initialized_) Shutdown();
}

std::uint32_t cSC4DisasterInstanceVolcano::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4DisasterInstanceVolcano::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterInstance || iid == 1) *out = static_cast<cISC4DisasterInstance*>(this);
    else if (iid == kGZIID_cISC4DisasterInstanceVolcano) *out = static_cast<cISC4DisasterInstanceVolcano*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

int cSC4DisasterInstanceVolcano::GetPhase() const {
    const unsigned state = static_cast<unsigned>(state_);
    if (state < 4) {
        if (1 < state) return 1;
        if (state == 0) return 0;
    } else if (state == 4) {
        return 3;
    }
    return 2;
}

float cSC4DisasterInstanceVolcano::GetLavaHeightByCityCell(int x, int z) {
    return lava_ ? lava_->GetLavaHeightByCityCell(x, z) : 0.0f;
}

// The place in world units; the disaster layer learns its cell.
void cSC4DisasterInstanceVolcano::SetLocation(int x, int z) {
    locationX_ = x;
    locationZ_ = z;
    IDisasterHost* city = DisasterHost();
    cISC4DisasterLayer* layer = city ? city->DisasterLayer() : nullptr;
    if (!layer) return;
    int cellX = 0, cellZ = 0;
    city->PositionToCell(static_cast<float>(x), static_cast<float>(z), cellX, cellZ);
    layer->RegisterDisasterLocation(this, cellX, cellZ, nullptr, 0, false);
}

// The lava flows only when the tuning says so (the flag is read twice in the original: as a value, then again through the
// property itself) and the host has lava at all.
void cSC4DisasterInstanceVolcano::ReadTuningParameters() {
    const cISCPropertyHolder* exemplar = g_host ? g_host->TuningExemplar() : nullptr;
    if (!exemplar) return;
    const auto curve = [exemplar](std::uint32_t id, cSC4ResponseCurve& out) {
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, values)) out.SetResponseCurveData(values.data(), values.size());
    };
    curve(0xea4bc6e4, g_tuning.lavaProductionRate);
    curve(0x4a4bc714, g_tuning.lavaSolidificationRate);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x0a4bc742, g_tuning.totalLavaSeconds);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0xaa4bc72a, g_tuning.productionScale);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x0a4bc752, g_tuning.ventRadius);
    g_tuning.lavaShouldFlow = false;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x6a4bc8ed, g_tuning.lavaShouldFlow);
}

bool cSC4DisasterInstanceVolcano::Init() {
    initialized_ = true;
    IDisasterHost* city = DisasterHost();
    cISC4DisasterLayer* layer = city ? city->DisasterLayer() : nullptr;
    manager_ = layer ? layer->GetDisasterManager(kDisasterManagerIdVolcano) : nullptr;
    if (manager_) manager_->AddRef();
    if (g_host) g_host->AddNotification(this, kVolcanoEffectFinishedMessage);
    const float maxX = g_host ? g_host->CitySizeX() - 1.0f : 0.0f, maxZ = g_host ? g_host->CitySizeZ() - 1.0f : 0.0f;
    if (locationX_ < 0) locationX_ = 0;
    else if (maxX < static_cast<float>(locationX_)) locationX_ = static_cast<int>(maxX);
    if (locationZ_ < 0) locationZ_ = 0;
    else if (maxZ < static_cast<float>(locationZ_)) locationZ_ = static_cast<int>(maxZ);
    ReadTuningParameters();
    lavaTime_ = 0;
    if (g_host) g_host->AddTickable(this, kVolcanoTickPriority);
    positionX_ = static_cast<float>(locationX_);
    positionZ_ = static_cast<float>(locationZ_);
    SetState(1);
    if (layer) {
        int cellX = 0, cellZ = 0;
        city->PositionToCell(positionX_, positionZ_, cellX, cellZ);
        layer->RegisterDisasterLocation(this, cellX, cellZ, nullptr, 0, false);
        layer->DisasterHasBegun(this);
    }
    registered_ = true;
    return true;
}

bool cSC4DisasterInstanceVolcano::Shutdown() {
    if (!initialized_) return false;
    initialized_ = false;
    if (g_host) g_host->RemoveTickable(this, kVolcanoTickPriority);
    if (effectStarted_) {
        if (effect_ && g_host) g_host->StopEffect(effect_);
        effect_ = nullptr;
        effectStarted_ = false;
    }
    if (g_host) g_host->RemoveNotification(this, kVolcanoEffectFinishedMessage);
    if (lavaFlowing_ || registered_) EndVolcanoLava();
    if (manager_) {
        cISC4DisasterManager* manager = manager_;
        manager_ = nullptr;
        manager->RemoveInstanceFromList(this);
        manager->Release();
    }
    return true;
}

bool cSC4DisasterInstanceVolcano::SetState(int state) {
    if (state == state_) return false;
    state_ = state;
    const bool lava = g_tuning.lavaShouldFlow;
    if (state == 5) {
        if (lava && !lavaFlowing_ && lava_) {
            lavaTime_ = 0;
            lava_->StartLavaFlow(static_cast<float>(locationX_), static_cast<float>(locationZ_), static_cast<float>(g_tuning.ventRadius), true);
            lavaFlowing_ = true;
        }
        return true;
    }
    if (state == 3) {
        StartDeformation();
        if (!lava || lavaEnded_ || lavaFlowing_) return true;
        InitializeVolcanoLava();
        SetState(5);
    } else if (state == 7) {
        effectFinished_ = true;
        SetState(lava && lavaFlowing_ ? 5 : 8);
    } else if (state == 8) {
        if (lava) EndVolcanoLava();
        EndDeformation();
    }
    return true;
}

void cSC4DisasterInstanceVolcano::StartDeformation() {
    if (effectStarted_) return;
    if (g_host) {
        const float xyz[3] = {positionX_, g_host->Altitude(positionX_, positionZ_), positionZ_};
        effect_ = g_host->CreateEffect("volcano_control", xyz, kVolcanoEffectFinishedMessage, token_);
    }
    effectStarted_ = true;
}

// Without lava still flowing the instance is over: the manager drops it and it shuts down.
void cSC4DisasterInstanceVolcano::EndDeformation() {
    if (lavaFlowing_) return;
    AddRef();
    cISC4DisasterManager* manager = manager_;
    if (manager) manager->RemoveInstanceFromList(this);
    Shutdown();
    Release();
}

// The lava object holds the instance while it exists.
void cSC4DisasterInstanceVolcano::InitializeVolcanoLava() {
    if (!g_tuning.lavaShouldFlow || lavaFlowing_ || !g_host) return;
    lava_ = g_host->CreateLava();
    if (!lava_) return;
    AddRef();
    lava_->SetLavaProductionRate(0.0f);
    lava_->SetSolidificationRate(0.0f);
}

void cSC4DisasterInstanceVolcano::EndVolcanoLava() {
    const bool held = lava_ != nullptr;
    if (lavaFlowing_) {
        lavaFlowing_ = false;
        lavaEnded_ = true;
    }
    if (lava_) {
        IVolcanoLava* lava = lava_;
        lava_ = nullptr;
        lava->Shutdown();
        lava->Release();
    }
    if (registered_) {
        registered_ = false;
        IDisasterHost* city = DisasterHost();
        if (cISC4DisasterLayer* layer = city ? city->DisasterLayer() : nullptr) {
            layer->DisasterHasEnded();
            layer->UnregisterDisasterLocations(this);
        }
    }
    if (held) Release();
}

bool cSC4DisasterInstanceVolcano::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    void* raw = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(raw);
    const std::uint32_t type = standard->GetType();
    if (type == kAnimationTickMessage) {
        const std::uint32_t microseconds = static_cast<std::uint32_t>(standard->GetData2());
        standard->Release();
        DoAnimationTick(microseconds);
        return true;
    }
    const bool mine = type == kVolcanoEffectFinishedMessage && static_cast<std::uint32_t>(standard->GetData1()) == token_;
    standard->Release();
    if (mine) SetState(7);
    return true;
}

// While the lava flows its two rates follow their curves over the lava time; when that time is up the lava ends, and the
// volcano with it if its effect has finished (else it goes on erupting).
void cSC4DisasterInstanceVolcano::DoAnimationTick(std::uint32_t microseconds) {
    if (!g_tuning.lavaShouldFlow || !lavaFlowing_) return;
    lavaTime_ += microseconds;
    const float seconds = static_cast<float>(lavaTime_) / 1000000.0f;
    if (seconds <= g_tuning.totalLavaSeconds) {
        if (state_ != 5 || !lava_) return;
        // (the original tests the production curve for both rates)
        const bool curves = g_tuning.lavaProductionRate.PointCount() != 0;
        const float fraction = seconds / g_tuning.totalLavaSeconds;
        const float production = curves ? g_tuning.lavaProductionRate.GetOutputValue(fraction) * g_tuning.productionScale / kVolcanoRateDivisor : 0.0f;
        const float solidification = curves ? g_tuning.lavaSolidificationRate.GetOutputValue(fraction) / kVolcanoRateDivisor : 0.0f;
        lava_->SetLavaProductionRate(production);
        lava_->SetSolidificationRate(solidification);
        return;
    }
    AddRef();                      // port guard: ending the lava can drop the last reference
    EndVolcanoLava();
    SetState(effectFinished_ ? 8 : 3);
    Release();
}

}  // namespace sc4
