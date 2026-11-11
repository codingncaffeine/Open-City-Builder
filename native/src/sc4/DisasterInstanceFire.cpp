#include "sc4/DisasterInstanceFire.h"

#include <cmath>

#include "rz/cRZMessage2Standard.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IFireInstanceHost* g_host = nullptr;

constexpr const char* kEffectNameBurnDownFlora = "burndown_flora";
constexpr const char* kEffectNameBurnDownBuilding = "burndown_building";
constexpr const char* kEffectNameFloraScorch = "fire_flora_scorch";
constexpr std::uint32_t kPropertyStageLengths = 0xa9b6ecd3, kPropertySpreadOdds = 0xa9b6ecd4, kPropertySpreadRadius = 0xa9b6ecd5,
                        kPropertyWindFactor = 0xa9b6ecd6, kPropertyTopFlames = 0xa9b6ecd7, kPropertySideFlames = 0xa9b6ecd8,
                        kPropertyProtectionCurve = 0xa9b6ecd9, kPropertyFlammabilityCurve = 0xa9b6ecda,
                        kPropertyLocationZoom = 0xca821b8c;
constexpr std::uint32_t kExtinguishDelaySpread = 4000;   // 0x0033be1c: the flames die within four seconds of the call
constexpr std::uint32_t kOccupantFlagInCity = 1;

bool Burns(std::uint32_t type) { return type == kFireTargetTypeFlora || type == kFireTargetTypeProp || type == kOccupantTypeBuilding; }

// The occupant's cells clipped to the city; a box wholly outside becomes the one cell (0, 0) (0x0033d1ac, 0x0033e75c).
void ClipToCity(SC4CellRect& r, int countX, int countZ) {
    const int maxX = countX - 1, maxZ = countZ - 1;
    if (r.x1 < 0 || maxX < r.x0 || r.z1 < 0 || maxZ < r.z0) {
        r.x0 = r.z0 = r.x1 = r.z1 = 0;
        return;
    }
    if (r.x0 < 1) r.x0 = 0;
    if (r.z0 < 1) r.z0 = 0;
    if (maxX < r.x1) r.x1 = maxX;
    if (maxZ < r.z1) r.z1 = maxZ;
}
}  // namespace

IFireInstanceHost* FireInstanceHost() { return g_host; }
void SetFireInstanceHost(IFireInstanceHost* host) { g_host = host; }

// @ 0x00348694: a fire occupant that still has its target.
bool cSC4ActiveFireOccupantFilter::IsOccupantIncluded(cISC4Occupant* occupant) {
    if (!occupant || occupant->GetType() != kOccupantTypeFire) return false;
    cSC4FireOccupant* fire = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4FireOccupant, reinterpret_cast<void**>(&fire)) || !fire) return false;
    const bool burning = fire->GetTarget() != nullptr;
    fire->Release();
    return burning;
}

cSC4DisasterInstanceFire::cSC4DisasterInstanceFire() = default;

cSC4DisasterInstanceFire::~cSC4DisasterInstanceFire() {
    if (filter_) filter_->Release();
}

bool cSC4DisasterInstanceFire::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cISC4DisasterInstanceFire) *out = static_cast<cISC4DisasterInstanceFire*>(this);
    else if (iid == kGZIID_cISC4DisasterInstance || iid == 1) *out = static_cast<cISC4DisasterInstance*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

std::uint32_t cSC4DisasterInstanceFire::Release() {
    if (refCount_ == 1) Shutdown();
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

cISC4DisasterManager* cSC4DisasterInstanceFire::GetManager() const {
    cISC4DisasterLayer* layer = g_host ? g_host->DisasterLayer() : nullptr;
    return layer ? layer->GetDisasterManager(kDisasterManagerIdFire) : nullptr;
}

std::uint32_t cSC4DisasterInstanceFire::SetCause(std::uint32_t cause) {
    if (cause_ == 0) cause_ = cause;
    return cause_;
}

void cSC4DisasterInstanceFire::Post(std::uint32_t type, std::uint64_t data1, std::uint64_t data2, bool withData2) {
    cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr;
    if (!server) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetIGZUnknown(static_cast<cISC4DisasterInstanceFire*>(this));
    message->SetData1(data1);
    if (withData2) message->SetData2(data2);
    server->MessagePost(message, false);
    message->Release();
}

void cSC4DisasterInstanceFire::SendIgnitionMessage(cISC4Occupant* fire, bool lit) {
    Post(kMessageFireIgnition, reinterpret_cast<std::uintptr_t>(fire), lit ? 1 : 0, true);
}

void cSC4DisasterInstanceFire::SetPhase(int phase) {
    if (phase_ == phase) return;
    phase_ = phase;
    Post(kMessageDisasterPhase, kDisasterManagerIdFire, static_cast<std::uint64_t>(phase), true);
}

void cSC4DisasterInstanceFire::SetSeverity(std::uint32_t severity) {
    if (severity_ == severity) return;
    severity_ = severity;
    Post(kMessageFireSeverity, severity_, 0, false);
}

bool cSC4DisasterInstanceFire::ExtinguishFire(cSC4FireOccupant* fire) {
    if (!fire) return false;
    if (!fire->IsExtinguishing())
        fire->Extinguish(timer_.GetElapsedTimeU32() + random_.RandomUint32Uniform(kExtinguishDelaySpread));
    return true;
}

// A cell near the fire: Gaussian over the target's box widened on every side by the stage radius plus the scaled wind.
void cSC4DisasterInstanceFire::ChooseSpreadCell(cSC4FireOccupant* fire, int& x, int& z) {
    float lo[3] = {}, hi[3] = {};
    fire->GetBoundingBox(lo, hi);
    const std::uint32_t radius = spreadRadius_[fire->GetStage()];
    float windX = 0.0f, windZ = 0.0f;
    g_host->AmbientWind(windX, windZ);
    windX *= windFactor_;
    windZ *= windFactor_;
    const float reachX = static_cast<float>(radius) + windX;
    const float reachZ = windZ + static_cast<float>(radius);
    const float px = static_cast<float>(random_.RandomDoubleRangeGaussianFast(lo[0] - reachX, reachX + hi[0]));
    const float pz = static_cast<float>(random_.RandomDoubleRangeGaussianFast(lo[2] - reachZ, reachZ + hi[2]));
    g_host->PositionToCell(px, pz, x, z);
}

int cSC4DisasterInstanceFire::GetFireCountInCellRect(const SC4CellRect& rect) {
    int count = 0;
    for (cSC4FireOccupant* fire : fires_) {
        SC4CellRect cells{};
        fire->GetBoundingCityCells(cells);
        if (cells.x0 <= rect.x1 && rect.x0 <= cells.x1 && cells.z0 <= rect.z1 && rect.z0 <= cells.z1) ++count;
    }
    return count;
}

bool cSC4DisasterInstanceFire::DoMessage(cIGZMessage2* message) {
    switch (message->GetType()) {
        case kMessageSimTick:
        case kMessageAnimationTick:
            Update();
            break;
        case kMessageViewLevelChanged: {
            const bool surface = g_host && g_host->CurrentViewLevel() == 1;
            for (cSC4FireOccupant* fire : fires_) fire->SetVisible(surface);
            break;
        }
        case kMessageViewChanged:
            for (cSC4FireOccupant* fire : fires_) fire->ViewUpdate();
            break;
        default:
            break;
    }
    return true;
}

cSC4FireOccupant* cSC4DisasterInstanceFire::LookupFireFromTarget(cISC4Occupant* target) {
    for (cSC4FireOccupant* fire : fires_)
        if (fire->GetTarget() == target) return fire;
    return nullptr;
}

bool cSC4DisasterInstanceFire::BurnDown(cISC4Occupant* target) { return BurnDownFire(LookupFireFromTarget(target)); }
bool cSC4DisasterInstanceFire::Extinguish(cISC4Occupant* target) { return ExtinguishFire(LookupFireFromTarget(target)); }

// Inserted at a random place, so the list burns down in no particular order.
void cSC4DisasterInstanceFire::AddToToastList(cISC4Occupant* occupant) {
    const std::uint32_t size = static_cast<std::uint32_t>(toast_.size());
    const std::uint32_t index = size != 0 ? random_.RandomUint32Uniform(size) : 0;
    auto at = toast_.begin();
    std::advance(at, index);
    occupant->AddRef();
    toast_.insert(at, occupant);
}

void cSC4DisasterInstanceFire::KillAllFires() {
    if (!g_host || !g_host->HasCity()) return;
    while (!fires_.empty()) {
        cSC4FireOccupant* fire = fires_.front();
        SendIgnitionMessage(fire, false);
        g_host->Occupants()->RemoveOccupant(fire, true, 0);
        fires_.pop_front();
        fire->Release();
    }
    if (cISC4DisasterLayer* layer = g_host->DisasterLayer()) layer->UnregisterDisasterLocations(this);
    families_.clear();
}

// Puts one fire out. Its family's camera location moves to another fire of the family, or the family ends.
void cSC4DisasterInstanceFire::KillFire(cSC4FireOccupant* fire) {
    auto at = fires_.begin();
    for (; at != fires_.end(); ++at)
        if (*at == fire) break;
    if (at == fires_.end()) return;
    SendIgnitionMessage(fire, false);
    cISC4DisasterLayer* layer = g_host->DisasterLayer();
    if (layer && layer->UnregisterDisasterLocationByOccupant(this, fire->GetTarget())) {
        const std::uint32_t family = fire->GetFamily();
        bool moved = false;
        for (cSC4FireOccupant* other : fires_) {
            if (other == fire || other->GetFamily() != family) continue;
            layer->RegisterDisasterLocation(this, 0, 0, other->GetTarget(), locationZoom_, false);
            moved = true;
            break;
        }
        if (!moved) families_.erase(family);
    }
    g_host->Occupants()->RemoveOccupant(fire, true, 0);
    fires_.erase(at);
    fire->Release();
}

void cSC4DisasterInstanceFire::UpdatePhase() {
    if (fires_.empty()) {
        if (phase_ != 1) return;
        SetSeverity(0);
        SetPhase(3);
        if (cISC4DisasterLayer* layer = g_host->DisasterLayer()) {
            layer->UnregisterDisasterLocations(this);
            layer->DisasterHasEnded();
        }
        g_host->RecordHistory(kHistoryFiresEnded, 1);
        g_host->RemoveTickable(this, kFireTickPriority);
        families_.clear();
        return;
    }
    if (phase_ == 1) return;
    cause_ = 0;
    if (!g_host->HasSimulator()) return;
    if (cISC4DisasterLayer* layer = g_host->DisasterLayer()) layer->DisasterHasBegun(this);
    g_host->AddTickable(this, kFireTickPriority);
    nextUpdate_ = timer_.GetElapsedTimeU32();
    SetPhase(1);
    SetSeverity(1);
}

bool cSC4DisasterInstanceFire::QuickBurnCell(std::uint32_t x, std::uint32_t z) {
    OccupantList list;
    const int cell[2] = {static_cast<int>(x), static_cast<int>(z)};
    g_host->Occupants()->GetOccupantsByStandardCityCells(list, cell, cell, filter_, 0xffffffffu);
    bool any = false;
    for (cISC4Occupant* occupant : list) {
        AddToToastList(occupant);
        occupant->Release();
        any = true;
    }
    return any;
}

std::uint32_t cSC4DisasterInstanceFire::ExtinguishCell(std::uint32_t x, std::uint32_t z, int amount) {
    OccupantList list;
    cSC4ActiveFireOccupantFilter filter;
    filter.AddRef();
    const int cell[2] = {static_cast<int>(x), static_cast<int>(z)};
    g_host->Occupants()->GetOccupantsByStandardCityCells(list, cell, cell, &filter, 0xffffffffu);
    for (cISC4Occupant* occupant : list) {
        cSC4FireOccupant* fire = nullptr;
        occupant->QueryInterface(kGZIID_cISC4FireOccupant, reinterpret_cast<void**>(&fire));
        if (fire) {
            const std::uint32_t counter = fire->GetCounter();
            if (static_cast<std::uint32_t>(amount) < counter) {
                fire->SetCounter(counter - static_cast<std::uint32_t>(amount));
            } else {
                fire->SetCounter(0);
                ExtinguishFire(fire);
            }
        }
        occupant->Release();
        if (fire) fire->Release();
    }
    return 1;
}

bool cSC4DisasterInstanceFire::IgniteCell(std::uint32_t x, std::uint32_t z, bool roll, bool force, std::uint32_t family) {
    const std::uint8_t flammability = g_host->Flammability(static_cast<int>(x), static_cast<int>(z));
    const float chance = flammabilityCurve_.GetOutputValue(static_cast<float>(flammability));
    if (roll && chance < static_cast<float>(random_.RandomDoubleRangeUniform(0.0, 100.0))) return false;
    if (family == 0) family = random_.RandomUint32Uniform();
    OccupantList list;
    const int cell[2] = {static_cast<int>(x), static_cast<int>(z)};
    g_host->Occupants()->GetOccupantsByStandardCityCells(list, cell, cell, filter_, 0xffffffffu);
    bool any = false;
    for (cISC4Occupant* occupant : list) {
        if (Ignite(occupant, force, family, !force)) any = true;
        occupant->Release();
    }
    return any;
}

// Everything inside `inner` cells burns down at once (when it can burn); between `inner` and `outer` it is set alight.
bool cSC4DisasterInstanceFire::IgniteCellRadius(std::uint32_t x, std::uint32_t z, std::uint32_t inner, std::uint32_t outer) {
    const std::uint32_t family = random_.RandomUint32Uniform();
    const float cellWidth = g_host->CellWidthX();
    float center[2] = {};
    g_host->CellCenterToPosition(static_cast<int>(x), static_cast<int>(z), center[0], center[1]);
    std::uint32_t maxX = outer + x, maxZ = outer + z;
    int min[2] = {0, 0};
    if (static_cast<int>(x - outer) > -1) min[0] = static_cast<int>(x - outer);
    if (static_cast<int>(z - outer) > -1) min[1] = static_cast<int>(z - outer);
    if (static_cast<std::uint32_t>(cellCountX_) <= maxX) maxX = static_cast<std::uint32_t>(cellCountX_) - 1;
    if (static_cast<std::uint32_t>(cellCountZ_) <= maxZ) maxZ = static_cast<std::uint32_t>(cellCountZ_) - 1;
    const int max[2] = {static_cast<int>(maxX), static_cast<int>(maxZ)};
    OccupantList list;
    g_host->Occupants()->GetOccupantsByStandardCityCells(list, min, max, filter_, 0xffffffffu);
    bool any = false;
    for (cISC4Occupant* occupant : list) {
        float position[3] = {};
        occupant->GetPosition(position);
        const float dx = position[0] - center[0], dz = position[2] - center[1];
        const float distance = std::sqrt(dx * dx + dz * dz);
        if (cellWidth * static_cast<float>(inner) <= distance) {
            if (distance < static_cast<float>(outer) * cellWidth && Ignite(occupant, false, family, false)) any = true;
        } else {
            std::uint32_t flammable = 0;
            cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyFlammability, flammable);
            if (flammable != 0) AddToToastList(occupant);
            any = true;
        }
        occupant->Release();
    }
    return any;
}

bool cSC4DisasterInstanceFire::Shutdown() {
    if (!initialised_) return true;
    cISC4DisasterLayer* layer = g_host ? g_host->DisasterLayer() : nullptr;
    if (layer) layer->UnregisterDisasterLocations(this);
    if (cIGZMessageServer2* server = g_host ? g_host->MessageServer() : nullptr) {
        server->RemoveNotification(this, kMessageViewChanged);
        server->RemoveNotification(this, kMessageViewLevelChanged);
    }
    if (g_host) g_host->RemoveSimulatorAgent(this);
    if (filter_) {
        filter_->Release();
        filter_ = nullptr;
    }
    KillAllFires();
    for (cSC4FireOccupant* fire : fires_) fire->Release();
    fires_.clear();
    families_.clear();
    while (!toast_.empty()) {
        toast_.front()->Release();
        toast_.pop_front();
    }
    cSC4FireOccupant::mpTopFireCurve = nullptr;
    cSC4FireOccupant::mpSideFireCurve = nullptr;
    initialised_ = false;
    if (layer)
        if (cISC4DisasterManager* manager = layer->GetDisasterManager(kDisasterManagerIdFire)) manager->RemoveInstanceFromList(this);
    return true;
}

// The protection curve's answer for the target's cells, rounded to nearest: above zero the fire is being held back.
int cSC4DisasterInstanceFire::ProtectionStep(cISC4Occupant* target) {
    SC4CellRect cells{};
    target->GetBoundingCityCells(cells);
    ClipToCity(cells, cellCountX_, cellCountZ_);
    const std::uint8_t coverage = g_host->FireProtection(cells.x0, cells.z0, cells.x1 - cells.x0 + 1, cells.z1 - cells.z0 + 1);
    return static_cast<int>(std::lrintf(protectionCurve_.GetOutputValue(static_cast<float>(coverage))));
}

// Sets an occupant alight. `force` (the torch, arson, a disaster) ignores the pause state and the fire protection and starts
// the fire part of the way into its first stage; an occupant already burning then burns one step harder.
bool cSC4DisasterInstanceFire::Ignite(cISC4Occupant* occupant, bool force, std::uint32_t family, bool jump) {
    const std::uint32_t type = occupant->GetType();
    if (!(force || g_host->SimulatorEmergencyPaused() || !g_host->SimulatorPaused())) return false;
    if (!Burns(type)) return false;
    std::uint32_t flammable = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyFlammability, flammable);
    if (flammable == 0) return false;
    if (!force && ProtectionStep(occupant) > 0) return false;
    bool onFire = false;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyOnFire, onFire);
    if (onFire) {
        if (!force) return false;
        if (cSC4FireOccupant* fire = LookupFireFromTarget(occupant)) fire->SetCounter(fire->GetCounter() + 1);
        return true;
    }
    if (family == 0) family = random_.RandomUint32Uniform();
    auto* fire = new cSC4FireOccupant();
    if (families_.find(family) == families_.end()) {
        if (cISC4DisasterLayer* layer = g_host->DisasterLayer())
            layer->RegisterDisasterLocation(this, 0, 0, occupant, locationZoom_, jump && families_.empty());
        families_.insert(family);
    }
    fire->AddRef();
    fires_.push_back(fire);
    fire->Init();
    fire->SetTarget(occupant);
    fire->SetFamily(family);
    g_host->Occupants()->InsertOccupant(fire, 0);
    if (force) fire->SetCounter(fire->GetMaxStage() < 2 ? stageThresholds_[0] >> 1 : stageThresholds_[0]);
    float position[3] = {};
    occupant->GetPosition(position);
    g_host->AddTransientAuraEffect(position[0], position[2], 0);
    SendIgnitionMessage(fire, true);
    if (occupant->GetType() == kFireTargetTypeFlora) ++floraIgnited_;
    else if (occupant->GetType() != kFireTargetTypeProp) ++buildingsIgnited_;
    UpdatePhase();
    return true;
}

// Burns an occupant down: the burn-down effect, then flora dies, a building's lot is demolished to burnt rubble (and may leave
// radiation), and anything without a lot is simply removed.
void cSC4DisasterInstanceFire::ToastOccupant(cISC4Occupant* occupant) {
    const std::uint32_t type = occupant->GetType();
    if (Burns(type))
        g_host->StartEffectOnOccupant(type == kFireTargetTypeFlora ? kEffectNameBurnDownFlora : kEffectNameBurnDownBuilding, occupant);
    bool onFire = false;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kPropertyOnFire, onFire);
    if (onFire) Extinguish(occupant);
    if (type == kFireTargetTypeFlora) {
        float position[3] = {};
        occupant->GetPosition(position);
        g_host->StartEffectAt(kEffectNameFloraScorch, position);
        g_host->KillFloraOccupant(occupant);
    } else if (type == kOccupantTypeBuilding || type == kFireTargetTypeProp) {
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        std::uint32_t rubble = 0;
        cSCPropertyHelp::GetPropertyValue(holder, kPropertyRubbleFlags, rubble);
        bool demolishLot = true;
        if (type == kFireTargetTypeProp) cSCPropertyHelp::GetPropertyValue(holder, kPropertyDemolishLotWhenBurnt, demolishLot);
        SC4CellRect lot{};
        if (!demolishLot || !g_host->OccupantLotRect(occupant, lot)) {
            g_host->Occupants()->RemoveOccupant(occupant, true, 0);
        } else {
            float amount = 0.0f, radius = 0.0f;
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyRadiationAmount, amount);
            cSCPropertyHelp::GetPropertyValue(holder, kPropertyRadiationRadius, radius);
            float position[3] = {};
            occupant->GetPosition(position);
            g_host->DemolishRegion(lot, rubble | 0x300);
            if (amount > 0.0f) g_host->AddRadiation(position, radius, amount);
        }
    }
    ++damaged_;
}

bool cSC4DisasterInstanceFire::BurnDownFire(cSC4FireOccupant* fire) {
    if (!fire) return false;
    cISC4Occupant* target = fire->GetTarget();
    if (target) target->AddRef();
    KillFire(fire);
    if (target) {
        ToastOccupant(target);
        target->Release();
    }
    return true;
}

void cSC4DisasterInstanceFire::ProcessToastList() {
    for (int done = 0; done != 5 && !toast_.empty(); ++done) {
        cISC4Occupant* occupant = toast_.front();
        toast_.pop_front();
        ToastOccupant(occupant);
        occupant->Release();
    }
}

void cSC4DisasterInstanceFire::ReadTunableValues() {
    updateInterval_ = 1000;
    const cISCPropertyHolder* tuning = g_host ? g_host->TuningExemplar() : nullptr;
    if (!tuning) {
        spreadOdds_.assign(5, 10);
        stageThresholds_.assign(5, 10);
        spreadRadius_ = {0, 3, 5, 7, 9};
        windFactor_ = 0.0f;
        locationZoom_ = 4;
    } else {
        cSCPropertyHelp::GetPropertyValue(tuning, kPropertySpreadOdds, spreadOdds_);
        cSCPropertyHelp::GetPropertyValue(tuning, kPropertyStageLengths, stageThresholds_);
        cSCPropertyHelp::GetPropertyValue(tuning, kPropertySpreadRadius, spreadRadius_);
        cSCPropertyHelp::GetPropertyValue(tuning, kPropertyWindFactor, windFactor_);
        cSCPropertyHelp::GetPropertyValue(tuning, kPropertyLocationZoom, locationZoom_);
        g_host->LoadTuningCurve(topCurve_, kPropertyTopFlames);
        g_host->LoadTuningCurve(sideCurve_, kPropertySideFlames);
        g_host->LoadTuningCurve(protectionCurve_, kPropertyProtectionCurve);
        g_host->LoadTuningCurve(flammabilityCurve_, kPropertyFlammabilityCurve);
    }
    // The original indexes five entries of each list without a bounds check; short data would read past the end.
    stageThresholds_.resize(5, 10);
    spreadOdds_.resize(6, 10);
    spreadRadius_.resize(6, 0);
    std::uint32_t sum = 0;
    for (std::uint32_t i = 0; i != 5; ++i) {
        sum += stageThresholds_[i];
        stageThresholds_[i] = sum;
    }
}

bool cSC4DisasterInstanceFire::Init() {
    if (initialised_) return true;
    ReadTunableValues();
    initialised_ = true;
    if (cISC4DisasterManager* manager = GetManager()) manager->AddInstanceToList(this);
    cSC4FireOccupant::mpTopFireCurve = &topCurve_;
    cSC4FireOccupant::mpSideFireCurve = &sideCurve_;
    cellCountX_ = g_host->CellCountX();
    cellCountZ_ = g_host->CellCountZ();
    timer_.SetUnits(cRZTimer::Units(4));
    timer_.Restart();
    if (filter_) filter_->Release();
    filter_ = new cSC4OccupantTypesFilter();
    filter_->AddRef();
    filter_->AddType(kFireTargetTypeFlora);
    filter_->AddType(kOccupantTypeBuilding);
    filter_->AddType(kFireTargetTypeProp);
    if (cIGZMessageServer2* server = g_host->MessageServer()) {
        server->AddNotification(this, kMessageViewLevelChanged);
        server->AddNotification(this, kMessageViewChanged);
    }
    Update();
    return true;
}

// One step of one fire: the counter moves, the stage follows, the lot shows its damage, and the fire may spread.
void cSC4DisasterInstanceFire::UpdateFire(cSC4FireOccupant* fire) {
    cISC4Occupant* target = fire->GetTarget();
    target->AddRef();
    bool kill = true;
    if (target->IsFlagSet(kOccupantFlagInCity)) {
        if (fire->IsExtinguishing()) {
            kill = fire->Extinguish(timer_.GetElapsedTimeU32()) == 0;
        } else {
            kill = false;
            int step = ProtectionStep(target);
            const std::uint32_t hosed = fire->GetHosed();
            if (hosed != 0) step += static_cast<int>(hosed) * 4;
            if (step < 1) step = 1;
            else step = -static_cast<int>(random_.RandomUint32Uniform(static_cast<std::uint32_t>(step + 1)));
            const int before = static_cast<int>(fire->GetCounter());
            const int counter = step + before;
            bool showDamage = before < 0x14 && 0x13 < counter;
            if (counter < 0) {
                fire->SetCounter(0);
                ExtinguishFire(fire);
            } else {
                fire->SetCounter(static_cast<std::uint32_t>(counter));
                std::uint32_t stage = 0;
                while (stage != 5 && stageThresholds_[stage] <= static_cast<std::uint32_t>(counter)) ++stage;
                const std::uint32_t maxStage = fire->GetMaxStage();
                bool burntDown = false;
                if (stage != fire->GetStage()) {
                    if (maxStage <= stage) {
                        if (!tutorial_) BurnDownFire(fire);
                        burntDown = true;
                    } else {
                        fire->SetStage(stage);
                        if (0x13 < counter) showDamage = true;
                    }
                }
                if (!burntDown) {
                    const std::uint32_t type = fire->GetTarget()->GetType();
                    if (type == kOccupantTypeBuilding || (type == kFireTargetTypeProp && showDamage)) {
                        if (!(stage < maxStage - 1)) g_host->SetOccupantLotCondition(target, kLotConditionBurntOut);
                        else if (!(stage < maxStage - 2)) g_host->SetOccupantLotCondition(target, kLotConditionBurning);
                    }
                    const float odds = 1.0f / static_cast<float>(spreadOdds_[stage]);
                    if (static_cast<float>(random_.RandomDoubleRangeUniform(0.0, 1.0)) < odds) {
                        int x = 0, z = 0;
                        ChooseSpreadCell(fire, x, z);
                        if (x >= 0 && x < g_host->CellCountX() && z >= 0 && z < g_host->CellCountZ())
                            IgniteCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), true, false, fire->GetFamily());
                    }
                }
            }
        }
    }
    if (kill) KillFire(fire);
    target->Release();
}

void cSC4DisasterInstanceFire::UpdateAllFires() {
    tutorial_ = g_host->TutorialRunning();
    for (auto at = fires_.begin(); at != fires_.end();) {
        cSC4FireOccupant* fire = *at;
        ++at;                                   // the fire may be removed by its own update
        if (!fire->GetTarget()) {
            KillAllFires();
            return;
        }
        UpdateFire(fire);
    }
}

void cSC4DisasterInstanceFire::Update() {
    ProcessToastList();
    UpdatePhase();
    if (!fires_.empty()) {
        const std::uint32_t now = timer_.GetElapsedTimeU32();
        if (nextUpdate_ == 0) nextUpdate_ = now;
        if (nextUpdate_ <= now) {
            UpdateAllFires();
            nextUpdate_ += updateInterval_;
        }
    }
    if (floraIgnited_ != 0 || buildingsIgnited_ != 0) {
        Post(kMessageFireIgnitedCounts, buildingsIgnited_, floraIgnited_, true);
        floraIgnited_ = 0;
        buildingsIgnited_ = 0;
    }
}

}  // namespace sc4
