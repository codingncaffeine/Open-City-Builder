#include "sc4/FireProtectionSimulator.h"

#include <algorithm>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
inline IFireOccupant* Fire(IProtectionStation* station) { return static_cast<IFireOccupant*>(station); }
}  // namespace

cSC4FireProtectionSimulator::cSC4FireProtectionSimulator(IFireHost* host) : host_(host), protection_(this, 2, 50) {
    ReadSimulatorTunableValues(nullptr);
}

// ---- identity -------------------------------------------------------------------------------------------

bool cSC4FireProtectionSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);                     // +0x18
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);              // +0x8
    else if (iid == kGZIID_cISC4ProtectionDispatchSimulator) *out = static_cast<cISC4ProtectionDispatchSimulator*>(this);  // +0x10
    else if (iid == 1 || iid == kGZIID_cISC4FireProtectionSimulator) *out = static_cast<cISC4FireProtectionSimulator*>(this);  // +0
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4FireProtectionSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

namespace {
// cSC4ProtectionSimulator::ProtectionInit's notifications: occupants inserted / removed / changed, funding changed, the dispatch
// messages and the tunables reload.
constexpr std::uint32_t kProtectionNotifications[] = {0x99ef1142, 0x99ef1143, 0xc8dac052, 0x08dac3e7, 0xe94671eb,
                                                      0x894671f6, 0xe94671ec, 0xe94671ed, 0xc53d10aa};
constexpr std::uint32_t kFireTuningInstance = 0x68f5524a;
}  // namespace

// @ 0x00237e72. ProtectionInit: the coverage grids (fire's instance ids), the sweep, the daily and monthly agents, the notifications.
// PARTIAL: the dispatch manager registration, the fire department's budget line, the flammability simulator reference and the fire
// plane dispatch source are not set up (the dispatch layer is not native yet).
bool cSC4FireProtectionSimulator::Init() {
    if (running_) return true;
    running_ = true;
    ReadSimulatorTunableValues(host_->SimulatorExemplar());
    protection_.CreateGrids(0, 0);
    if (auto* grid = protection_.AdjustedGrid()) grid->SetInstanceID(0x49d5b8f9);
    if (auto* grid = protection_.UnadjustedGrid()) grid->SetInstanceID(0x89d5b91a);
    static const char* const kName = "cSC4ProtectionSimulator";
    host_->AddAgent(this, 4, kName, 2);
    host_->AddAgent(this, 6, kName, 2);
    for (std::uint32_t message : kProtectionNotifications) host_->AddNotification(this, message);
    return true;
}

bool cSC4FireProtectionSimulator::Shutdown() {
    if (!running_) return true;
    running_ = false;
    host_->RemoveAgent(this);
    for (std::uint32_t message : kProtectionNotifications) host_->RemoveNotification(this, message);
    return true;
}

// @ 0x00237e0a over cSC4ProtectionSimulator::DoMessage @ 0x0007cace. PARTIAL: the dispatch messages (0xe94671eb..ed, 0x894671f6)
// are not routed: no dispatches exist natively.
bool cSC4FireProtectionSimulator::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    const auto station = [&]() -> IFireOccupant* {
        IFireOccupant* occupant = host_->FireOccupant(standard->GetVoid1());
        cISCPropertyHolder* holder = occupant ? occupant->PropertyHolder() : nullptr;
        return holder && !holder->HasProperty(0x029244db) ? occupant : nullptr;
    };
    switch (type) {
    case 0x99ef1142: if (IFireOccupant* o = station()) DoOccupantInserted(o); break;
    case 0x99ef1143: if (IFireOccupant* o = station()) DoOccupantRemoved(o); break;
    case 0xc8dac052: if (IFireOccupant* o = station()) DoOccupantChanged(o); break;
    case 0x66956814: DoDailySimulation(); break;
    case 0x66956816: DoMonthlySimulation(); break;
    case 0x08dac3e7:   // DoFundingChanged: the department's funding moved
        if (!protection_.Stations().empty()) protection_.UpdateUnadjustedProtectionGrid();
        break;
    case 0xc53d10aa:
        if (static_cast<std::uint32_t>(standard->GetData1()) == 0xe7e2c2db && static_cast<std::uint32_t>(standard->GetData3()) == kFireTuningInstance)
            ReadSimulatorTunableValues(host_->SimulatorExemplar());
        break;
    default: break;
    }
    return true;
}

// ---- streams ---------------------------------------------------------------------------------------------

void cSC4FireProtectionSimulator::WriteGridInt16(cIGZOStream& stream, cSC4SimGrid<std::int16_t>* grid) {
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(grid));
}
bool cSC4FireProtectionSimulator::ReadGridInt16(cIGZIStream& stream, cSC4SimGrid<std::int16_t>** out) {
    void* raw = nullptr;
    if (!SC4ReadGZSerializable(stream, kInt16GridIid, &raw, false)) return false;
    *out = SimGridCast<std::int16_t>(raw);
    return true;
}
void cSC4FireProtectionSimulator::WriteGridUint8(cIGZOStream& stream, cSC4SimGrid<std::uint8_t>* grid) {
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(grid));
}
bool cSC4FireProtectionSimulator::ReadGridUint8(cIGZIStream& stream, cSC4SimGrid<std::uint8_t>** out) {
    void* raw = nullptr;
    if (!SC4ReadGZSerializable(stream, kUint8GridIid, &raw, false)) return false;
    *out = SimGridCast<std::uint8_t>(raw);
    return true;
}
void cSC4FireProtectionSimulator::WriteStation(cIGZOStream& stream, IProtectionStation* station) {
    host_->WriteOccupant(stream, Fire(station));
}
bool cSC4FireProtectionSimulator::ReadStation(cIGZIStream& stream, IProtectionStation** out) {
    IFireOccupant* occupant = nullptr;
    if (!host_->ReadOccupant(stream, &occupant)) return false;
    *out = occupant;
    return true;
}

bool cSC4FireProtectionSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);
    if (!protection_.Write(stream)) return false;
    // +0x120 the dispatch helper (cRZAutoRefCount<...>, iid 0xc9c00435) is not recovered: the one a file named, else null.
    fileDispatch_.Write(stream);
    return !stream.GetError();
}

bool cSC4FireProtectionSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 3) return false;
    if (!protection_.Read(stream)) return false;
    fileDispatch_ = cRetainedReference();
    if (version >= 3) fileDispatch_.Read(stream);                         // +0x120
    return !stream.GetError();
}

void cSC4FireProtectionSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    tunables_ = FireTunables();
    const std::vector<SC4Point> efficiency{{0.0f, 0.0f}, {100.0f, 100.0f}, {120.0f, 110.0f}};
    stationEfficiency_.SetResponseCurveData(efficiency);
    const std::vector<SC4Point> chance{{0.0f, 20.0f}, {100.0f, 100.0f}};
    strikeChance_.SetResponseCurveData(chance);
    if (!exemplar) return;
    auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id) {
        std::vector<float> values;
        if (exemplar->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(exemplar, id, values))
            curve.SetResponseCurveData(values.data(), values.size());
    };
    SC4Percentage percentage;
    loadCurve(strikeChance_, 0x29233e98);
    loadCurve(stationEfficiency_, 0x29233e8d);
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x29233e8f, percentage)) tunables_.value148 = percentage.value;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x29233e94, tunables_.strikeEfficiency);
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x29233e96, percentage)) tunables_.strikeEndFunding = percentage.value;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x69b425db, tunables_.noRoadFactor);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x29233e95, tunables_.strikeMinDays);
}

bool cSC4FireProtectionSimulator::IsFireStation(IFireOccupant* occupant) const {
    return occupant->PropertyHolder()->HasProperty(kFireStationRadius);
}

// ---- efficiency ------------------------------------------------------------------------------------------

float cSC4FireProtectionSimulator::GetStationEfficiency(IFireOccupant* station) {
    float fraction = 0.0f;
    if (!host_->FundingFraction(station, kFireFundingProperty, fraction)) fraction = 0.0f;
    float efficiency = stationEfficiency_.GetOutputValue(fraction * 100.0f) / 100.0f;
    if (!host_->LotAllowsDispatch(station)) efficiency *= tunables_.noRoadFactor;
    if (IsStationOnStrike(station)) efficiency *= tunables_.strikeEfficiency;
    return efficiency;
}

bool cSC4FireProtectionSimulator::GetStationFunding(IFireOccupant* station, int* cost, SC4Percentage* fraction) const {
    int c = 0;
    float f = 0.0f;
    if (!host_->FundingInfo(station, kFireFundingProperty, c, f)) return false;
    if (fraction) fraction->value = f;
    if (cost) *cost = c;
    return true;
}

bool cSC4FireProtectionSimulator::SetStationFunding(IFireOccupant* station, const SC4Percentage& fraction) {
    return host_->SetFundingFraction(station, kFireFundingProperty, fraction);
}


float cSC4FireProtectionSimulator::GetStationDispatchRadius(IFireOccupant* station) {
    ProtectionEffect effect;
    if (!ProtectionGetDispatchEffect(station, effect)) return 0.0f;
    return effect.radius;
}

// The coverage curve is the same as the police one: `share` of the station's capacity is in use; below the
// edge strength the full radius applies, above the peak none, in between a linear falloff.
bool cSC4FireProtectionSimulator::GetOccupantCoverage(IFireOccupant* station, const SC4Percentage& share, float& covered,
                                                      float& strength) {
    covered = 0.0f;
    strength = 0.0f;
    ProtectionEffect effect;
    if (!ProtectionGetStationEffect(station, effect)) return false;
    strength = effect.radius;
    const std::uint32_t used = static_cast<std::uint32_t>(static_cast<long>(static_cast<float>(effect.capacity) * share.value));
    const std::uint32_t peak = static_cast<std::uint32_t>(effect.peak), edge = static_cast<std::uint32_t>(effect.edge);
    if (peak < used)
        covered = 0.0f;
    else if (used < edge)
        covered = effect.radius;
    else
        covered = (static_cast<float>(peak - used) * effect.radius) / static_cast<float>(peak - edge);
    return true;
}

int cSC4FireProtectionSimulator::GetTotalDispatchCount() {
    int total = 0;
    for (IProtectionStation* station : protection_.Stations()) {
        std::uint32_t count = 0;
        total += cSCPropertyHelp::GetPropertyValue(Fire(station)->PropertyHolder(), kFireStationDispatchCount, count)
                     ? static_cast<int>(count) : 0;
    }
    return total;
}

bool cSC4FireProtectionSimulator::GetDispatchesAtStation(IFireOccupant* station, std::uint32_t* mask, std::uint32_t* baseCall,
                                                         std::uint32_t* count) {
    cISCPropertyHolder* holder = station->PropertyHolder();
    std::uint32_t total = 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kFireStationDispatchCount, total)) return false;
    if (count) *count = total;
    if (mask) {
        std::uint32_t flags = 0;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kFireStationAvailability, flags)) return false;
        *mask = ((1u << (total & 31)) - 1u) & flags;
    }
    if (baseCall) {
        std::uint32_t base = 0;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kFireStationBaseCallNumber, base)) return false;
        *baseCall = base;
    }
    return true;
}

bool cSC4FireProtectionSimulator::GetDispatchAutomataProperties(const void*, std::uint32_t& automatonType,
                                                                FireResourceKey& modelKey, FireResourceKey& secondKey) {
    automatonType = 0x4202;
    modelKey = FireResourceKey();
    secondKey.type = 0x11882;
    return true;
}

int cSC4FireProtectionSimulator::GetFireStations(std::list<IFireOccupant*>& out) const {
    for (IProtectionStation* station : protection_.Stations()) out.push_back(Fire(station));
    return static_cast<int>(protection_.Stations().size());
}

// ---- strikes ----------------------------------------------------------------------------------------------

bool cSC4FireProtectionSimulator::IsStationOnStrike(IFireOccupant* station) {
    std::uint32_t start = 0;
    return cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kFireStrikeStartDate, start) && start != 0;
}

bool cSC4FireProtectionSimulator::UpdateStrikingState(IFireOccupant* station) {
    const std::uint32_t date = static_cast<std::uint32_t>(host_->CurrentDay());
    std::uint32_t start = 0;
    cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kFireStrikeStartDate, start);
    std::uint32_t next;
    float fraction = 0.0f;
    if (start == 0) {
        const bool found = host_->FundingFraction(station, kFireFundingProperty, fraction);
        const float chance = strikeChance_.GetOutputValue(found ? fraction * 100.0f : 100.0f);
        next = static_cast<float>(random_.RandomDoubleRangeUniform(0.0, 100.0)) < chance ? date : 0;
    } else {
        next = 0;
        if (date - start <= static_cast<std::uint32_t>(tunables_.strikeMinDays)) {
            if (!host_->FundingFraction(station, kFireFundingProperty, fraction)) fraction = 1.0f;
            next = start;
            if (tunables_.strikeEndFunding <= fraction) next = 0;
        }
    }
    if (next == start) return false;
    cSCPropertyHelp::SetPropertyValue(station->PropertyHolder(), kFireStrikeStartDate, next);
    host_->SetStrikerGroup(station, next != 0);
    return true;
}

float cSC4FireProtectionSimulator::GetChanceOfStrike() {
    if (onStrike_) return 1.0f;
    float chance = 0.0f;
    for (IProtectionStation* station : protection_.Stations()) {
        float fraction = 0.0f;
        const bool found = host_->FundingFraction(Fire(station), kFireFundingProperty, fraction);
        const float c = strikeChance_.GetOutputValue(found ? fraction * 100.0f : 100.0f) * 0.01f;
        chance = (chance + c) - c * chance;
    }
    return chance;
}

bool cSC4FireProtectionSimulator::BeginStrike(IFireOccupant* station) {
    if (onStrike_) return false;
    onStrike_ = true;
    strikeStart_ = host_->CurrentDay();
    host_->PostMessage(kFireMessageStrikeBegin, station);
    return true;
}

bool cSC4FireProtectionSimulator::EndStrike() {
    if (!onStrike_) return false;
    onStrike_ = false;
    strikeStart_ = 0;
    host_->PostMessage(kFireMessageStrikeEnd, nullptr);
    return true;
}

int cSC4FireProtectionSimulator::GetStrikeLength() { return strikeStart_ == 0 ? -1 : host_->CurrentDay() - strikeStart_; }

int cSC4FireProtectionSimulator::GetStrikingStations(std::list<IFireOccupant*>& out) {
    int count = 0;
    for (IProtectionStation* station : protection_.Stations()) {
        if (IsStationOnStrike(Fire(station))) {
            out.push_back(Fire(station));
            ++count;
        }
    }
    return count;
}

// ---- protection hooks ---------------------------------------------------------------------------------------

bool cSC4FireProtectionSimulator::ProtectionGetStationEffect(IFireOccupant* station, ProtectionEffect& effect) {
    cISCPropertyHolder* holder = station->PropertyHolder();
    effect.edge = 0;
    effect.radius = 0.0f;
    effect.peak = 0;
    const bool hasPeak = cSCPropertyHelp::GetPropertyValue(holder, kFireStationEffectPeak, effect.peak);
    cSCPropertyHelp::GetPropertyValue(holder, kFireStationEffectEdge, effect.edge);
    const bool hasRadius = cSCPropertyHelp::GetPropertyValue(holder, kFireStationRadius, effect.radius);
    float position[3];
    station->Position(position);
    effect.x = position[0];
    effect.z = position[2];
    effect.capacity = effect.peak;
    if (!hasPeak || !hasRadius) return false;
    float factor;
    if (station->UsesDepartmentBudget())
        factor = stationEfficiency_.GetOutputValue(host_->DepartmentBudgetFraction() * 100.0f) / 100.0f;
    else
        factor = GetStationEfficiency(station);
    cSC4ProtectionSimulator::AdjustProtectionEffect(effect, factor);
    return true;
}

bool cSC4FireProtectionSimulator::ProtectionGetDispatchEffect(IFireOccupant* station, ProtectionEffect& effect) {
    cISCPropertyHolder* holder = station->PropertyHolder();
    effect.x = effect.z = 0.0f;
    effect.peak = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kFireDispatchPeak, effect.peak);
    if (effect.peak == 0) return false;
    cSCPropertyHelp::GetPropertyValue(holder, kFireDispatchEdge, effect.edge);
    cSCPropertyHelp::GetPropertyValue(holder, kFireDispatchRadius, effect.radius);
    effect.capacity = effect.peak;
    cSC4ProtectionSimulator::AdjustProtectionEffect(effect, GetStationEfficiency(station));
    return true;
}

bool cSC4FireProtectionSimulator::ProtectionGetStationFunding(IFireOccupant* station, int* cost) const {
    if (!cost) return true;
    int c = 0;
    float f = 0.0f;
    if (!host_->FundingInfo(station, kFireFundingProperty, c, f)) return false;
    *cost = c;
    return true;
}

void cSC4FireProtectionSimulator::ProtectionSetStationAvailabilityFlags(IFireOccupant* station, std::uint32_t flags) {
    cSCPropertyHelp::SetPropertyValue(station->PropertyHolder(), kFireStationAvailability, flags);
}

void cSC4FireProtectionSimulator::ProtectionSetStationBaseCallNumber(IFireOccupant* station, std::uint32_t number) {
    cSCPropertyHelp::SetPropertyValue(station->PropertyHolder(), kFireStationBaseCallNumber, number);
}

void cSC4FireProtectionSimulator::ProtectionEndOfCycle() {
    host_->AddHistory(kFireHistoryPercentCoverage, static_cast<int>(protection_.CoverageShare() * 100.0f));
    host_->AddHistory(kFireHistoryAverageCoverage, static_cast<int>(protection_.AverageProtection()));
    host_->AddHistory(kFireHistoryCombinedCoverage,
                      static_cast<int>((host_->PoliceCoverageShare() * 100.0f + protection_.CoverageShare() * 100.0f) * 0.5f));
    host_->PostMessage(kFireMessageCoverage, nullptr);
}

// ---- schedule ----------------------------------------------------------------------------------------------------

// Original quirk kept: once one station changed state, every later striking station (re)starts the city-wide
// strike because the "changed" flag is sticky across the loop.
void cSC4FireProtectionSimulator::DoMonthlySimulation() {
    if (protection_.Stations().empty()) return;
    int striking = 0;
    bool changed = false;
    for (IProtectionStation* station : protection_.Stations()) {
        IFireOccupant* fire = Fire(station);
        if (UpdateStrikingState(fire)) changed = true;
        if (IsStationOnStrike(fire)) {
            ++striking;
            if (changed) BeginStrike(fire);
        }
    }
    if (changed) {
        if (striking == 0 && IsOnStrike()) EndStrike();
        protection_.UpdateUnadjustedProtectionGrid();
    }
}

void cSC4FireProtectionSimulator::DoDailySimulation() {
    protection_.scale = 1.0f;
    protection_.DoDailySimulation();
}

bool cSC4FireProtectionSimulator::GetStationEffect(IProtectionStation* station, ProtectionEffect& effect) {
    return ProtectionGetStationEffect(Fire(station), effect);
}

void cSC4FireProtectionSimulator::SetStationDispatchMask(IProtectionStation* station, std::uint32_t mask) {
    ProtectionSetStationAvailabilityFlags(Fire(station), mask);
}

void cSC4FireProtectionSimulator::SetStationDispatchOffset(IProtectionStation* station, std::uint32_t offset) {
    ProtectionSetStationBaseCallNumber(Fire(station), offset);
}

void cSC4FireProtectionSimulator::GetStationInfo(IProtectionStation* station, int* available, std::uint32_t* offset,
                                                 std::uint32_t* count) {
    std::uint32_t mask = 0, base = 0, total = 0;
    const bool ok = GetDispatchesAtStation(Fire(station), available ? &mask : nullptr, offset ? &base : nullptr, count ? &total : nullptr);
    if (available) *available = ok ? static_cast<int>(mask) : 0;
    if (offset) *offset = ok ? base : 0;
    if (count) *count = ok ? total : 0;
}

void cSC4FireProtectionSimulator::RegisterStationProperty(IProtectionStation* station, bool add) {
    host_->RegisterFundedStation(Fire(station), kFireFundingProperty, add);
}

void cSC4FireProtectionSimulator::GetDispatchEffect(const ProtectionDispatchInfo& info, ProtectionEffect& effect) {
    if (info.origin) ProtectionGetDispatchEffect(Fire(info.origin), effect);
}

}  // namespace sc4
