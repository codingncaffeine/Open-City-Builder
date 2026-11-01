#include <vector>
#include "sc4/PoliceSimulator.h"

#include <algorithm>
#include <cmath>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/DBSegment.h"

namespace sc4 {

namespace {
inline int RoundEven(float v) { return static_cast<int>(std::nearbyint(v)); }
inline IPoliceOccupant* Police(IProtectionStation* station) { return static_cast<IPoliceOccupant*>(station); }

template <class List, class T>
typename List::iterator FindIn(List& list, T* value) { return std::find(list.begin(), list.end(), value); }
}  // namespace

cSC4PoliceSimulator::cSC4PoliceSimulator(IPoliceHost* host) : host_(host), protection_(this, 2, 40) {
    ReadSimulatorTunableValues(nullptr);
}

// ---- identity -------------------------------------------------------------------------------------------

bool cSC4PoliceSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);                     // +0x18
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);              // +0x8
    else if (iid == kGZIID_cISC4ProtectionDispatchSimulator) *out = static_cast<cISC4ProtectionDispatchSimulator*>(this);  // +0x10
    else if (iid == 1 || iid == kGZIID_cISC4PoliceSimulator) *out = static_cast<cISC4PoliceSimulator*>(this);  // +0
    else return false;
    AddRef();
    return true;
}

std::uint32_t cSC4PoliceSimulator::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4PoliceSimulator::DoMessage(cIGZMessage2*) { return true; }   // routing unrecovered

// ---- streams ---------------------------------------------------------------------------------------------

void cSC4PoliceSimulator::WriteGridInt16(cIGZOStream& stream, cSC4SimGrid<std::int16_t>* grid) {
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(grid));
}
bool cSC4PoliceSimulator::ReadGridInt16(cIGZIStream& stream, cSC4SimGrid<std::int16_t>** out) {
    void* raw = nullptr;
    if (!SC4ReadGZSerializable(stream, kInt16GridIid, &raw, false)) return false;
    *out = SimGridCast<std::int16_t>(raw);
    return true;
}
void cSC4PoliceSimulator::WriteGridUint8(cIGZOStream& stream, cSC4SimGrid<std::uint8_t>* grid) {
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(grid));
}
bool cSC4PoliceSimulator::ReadGridUint8(cIGZIStream& stream, cSC4SimGrid<std::uint8_t>** out) {
    void* raw = nullptr;
    if (!SC4ReadGZSerializable(stream, kUint8GridIid, &raw, false)) return false;
    *out = SimGridCast<std::uint8_t>(raw);
    return true;
}
void cSC4PoliceSimulator::WriteStation(cIGZOStream& stream, IProtectionStation* station) {
    host_->WriteOccupant(stream, Police(station));
}
bool cSC4PoliceSimulator::ReadStation(cIGZIStream& stream, IProtectionStation** out) {
    IPoliceOccupant* occupant = nullptr;
    if (!host_->ReadOccupant(stream, &occupant)) return false;
    *out = occupant;
    return true;
}

bool cSC4PoliceSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(8);
    if (!protection_.Write(stream)) return false;
    filePowerGrid_.Write(stream);                                         // +0x128 police power grid (unrecovered)
    // a loaded city's jails are the references its file named (fundedJails_/jails_ stay empty then)
    const bool keptFunded = fundedJails_.empty(), keptJails = jails_.empty();
    stream.SetUint32(std::uint32_t(keptFunded ? fileFundedJails_.size() : fundedJails_.size()));   // +0x168
    if (keptFunded) for (const cRetainedReference& jail : fileFundedJails_) jail.Write(stream);
    else for (IPoliceOccupant* jail : fundedJails_) host_->WriteOccupant(stream, jail);
    stream.SetUint32(std::uint32_t(keptJails ? fileJails_.size() : jails_.size()));                 // +0x130
    if (keptJails) for (const cRetainedReference& jail : fileJails_) jail.Write(stream);
    else for (IPoliceOccupant* jail : jails_) host_->WriteOccupant(stream, jail);
    stream.SetSint32(jailCapacity_);                                      // +0x148
    stream.SetSint32(jailPopulation_);                                    // +0x14c
    stream.SetSint32(criminals_);                                         // +0x150
    stream.SetSint32(arrestsPrev_);                                       // +0x154
    stream.SetSint32(arrestsThisMonth_);                                  // +0x15c
    stream.SetSint32(unjailedAdded_);                                     // +0x164
    stream.SetSint32(crimesPrev_);                                        // +0x158
    stream.SetSint32(crimesThisMonth_);                                   // +0x160
    fileDispatch_.Write(stream);                                          // +0x118 dispatch helper (unrecovered)
    return !stream.GetError();
}

bool cSC4PoliceSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 8) return false;
    if (!protection_.Read(stream)) return false;
    filePowerGrid_ = cRetainedReference();
    filePowerGrid_.Read(stream);                                          // +0x128 police power grid
    // In a city file the jails are object references the host does not resolve yet: they are consumed so the record stays in step.
    void* dbStream = nullptr;
    const bool references = stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream;
    if (dbStream) static_cast<cIGZUnknown*>(dbStream)->Release();
    const auto readJail = [&](std::list<IPoliceOccupant*>& into) {
        IPoliceOccupant* jail = nullptr;
        if (references) {
            cRetainedReferences& kept = &into == &fundedJails_ ? fileFundedJails_ : fileJails_;
            kept.emplace_back();
            kept.back().Read(stream);
        } else if (host_->ReadOccupant(stream, &jail) && jail) {
            into.push_back(jail);
        }
    };
    std::uint32_t count = 0;
    stream.GetUint32(count);                                              // +0x168 funded jails
    fundedJails_.clear();
    fileFundedJails_.clear();
    fileJails_.clear();
    for (std::uint32_t i = 0; i < count && count < 10000000u; ++i) readJail(fundedJails_);
    stream.GetUint32(count);                                              // +0x130 jails
    jails_.clear();
    for (std::uint32_t i = 0; i < count && count < 10000000u; ++i) readJail(jails_);
    stream.GetSint32(jailCapacity_);                                      // +0x148
    stream.GetSint32(jailPopulation_);                                    // +0x14c
    stream.GetSint32(criminals_);                                         // +0x150
    stream.GetSint32(arrestsPrev_);                                       // +0x154
    stream.GetSint32(arrestsThisMonth_);                                  // +0x15c
    stream.GetSint32(unjailedAdded_);                                     // +0x164
    stream.GetSint32(crimesPrev_);                                        // +0x158
    stream.GetSint32(crimesThisMonth_);                                   // +0x160
    fileDispatch_ = cRetainedReference();
    if (version >= 8) fileDispatch_.Read(stream);                         // +0x118 dispatch helper
    return !stream.GetError();
}

void cSC4PoliceSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    tunables_ = PoliceTunables();
    // default curves: station and jail efficiency saturate at 110% for 120% funding; strike / jailbreak chance
    // rise linearly from 20% at 0 funding... as in the original constants
    const std::vector<SC4Point> efficiency{{0.0f, 0.0f}, {100.0f, 100.0f}, {120.0f, 110.0f}};
    stationEfficiency_.SetResponseCurveData(efficiency);
    jailEfficiency_.SetResponseCurveData(efficiency);
    const std::vector<SC4Point> chance{{0.0f, 20.0f}, {100.0f, 100.0f}};
    strikeChance_.SetResponseCurveData(chance);
    jailbreakChance_.SetResponseCurveData(chance);
    if (!exemplar) return;
    SC4Percentage percentage;
    auto readFraction = [&](std::uint32_t id, float& out) {
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, percentage)) out = percentage.value;
    };
    auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id) {
        std::vector<float> values;
        if (exemplar->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(exemplar, id, values))
            curve.SetResponseCurveData(values.data(), values.size());
    };
    readFraction(0x68ddae92, tunables_.unjailedFraction);
    loadCurve(stationEfficiency_, 0x68ddae93);
    loadCurve(jailEfficiency_, 0x68ddaea0);
    readFraction(0x68ddae95, tunables_.value1c8);
    readFraction(0x68ddae97, tunables_.rehabilitationRate);
    readFraction(0x68ddae9a, tunables_.strikeEfficiency);
    std::int32_t integer = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x68ddae9b, integer)) tunables_.strikeMinDays = static_cast<std::uint32_t>(integer);
    readFraction(0x68ddae9c, tunables_.strikeEndFunding);
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x68ddae9f, integer)) tunables_.value200 = integer;
    loadCurve(strikeChance_, 0x68ddae9e);
    loadCurve(jailbreakChance_, 0xea614f63);
    std::vector<float> range;
    if (exemplar->HasProperty(0xea614f64) && cSCPropertyHelp::GetPropertyValue(exemplar, 0xea614f64, range) && range.size() > 1) {
        tunables_.jailbreakMin = range[0] * 0.01f;
        tunables_.jailbreakMax = range[1] * 0.01f;
    }
    readFraction(0xea614f65, tunables_.jailbreakEndThreshold);
}

// ---- efficiency -----------------------------------------------------------------------------------------

float cSC4PoliceSimulator::GetStationEfficiency(IPoliceOccupant* station) {
    float fraction = 0.0f;
    if (!host_->FundingFraction(station, kPoliceStationFundingProperty, fraction)) fraction = 0.0f;
    float efficiency = stationEfficiency_.GetOutputValue(fraction * 100.0f) * 0.01f;
    if (IsStationOnStrike(station)) efficiency *= tunables_.strikeEfficiency;
    return efficiency;
}

float cSC4PoliceSimulator::CalculateJailEfficiency(IPoliceOccupant* jail) {
    float percent = 100.0f;
    float fraction = 0.0f;
    if (host_->FundingFraction(jail, kPoliceJailFundingProperty, fraction)) percent = fraction * 100.0f;
    return jailEfficiency_.GetOutputValue(percent) * 0.01f;
}

bool cSC4PoliceSimulator::ProtectionGetStationEffect(IPoliceOccupant* station, ProtectionEffect& effect) {
    cISCPropertyHolder* holder = station->PropertyHolder();
    effect.peak = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceStationEffectPeak, effect.peak);
    if (effect.peak == 0) return false;
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceStationEffectEdge, effect.edge);
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceStationRadius, effect.radius);
    float position[3];
    station->Position(position);
    effect.x = position[0];
    effect.z = position[2];
    effect.capacity = effect.peak;
    float factor;
    if (station->UsesDepartmentBudget())
        factor = stationEfficiency_.GetOutputValue(host_->DepartmentBudgetFraction() * 100.0f) / 100.0f;
    else
        factor = GetStationEfficiency(station);
    cSC4ProtectionSimulator::AdjustProtectionEffect(effect, factor);
    return true;
}

bool cSC4PoliceSimulator::ProtectionGetDispatchEffect(IPoliceOccupant* station, ProtectionEffect& effect) {
    cISCPropertyHolder* holder = station->PropertyHolder();
    effect.x = effect.z = 0.0f;
    effect.peak = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceDispatchPeak, effect.peak);
    if (effect.peak == 0) return false;
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceDispatchEdge, effect.edge);
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceDispatchRadius, effect.radius);
    effect.capacity = effect.peak;
    cSC4ProtectionSimulator::AdjustProtectionEffect(effect, GetStationEfficiency(station));
    return true;
}

bool cSC4PoliceSimulator::GetStationDispatchRadius(IPoliceOccupant* station, float& radius) {
    ProtectionEffect effect;
    if (!ProtectionGetStationEffect(station, effect)) {
        radius = 0.0f;
        return false;
    }
    radius = effect.radius;
    return true;
}

// How much of the station's service the city asks for: `share` of its capacity is in use; below the edge
// strength the full effect applies, above the peak none, in between it falls off linearly.
bool cSC4PoliceSimulator::GetOccupantCoverage(IPoliceOccupant* station, const SC4Percentage& share, float& covered,
                                              float& strength) {
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

// ---- criminals ------------------------------------------------------------------------------------------

// Spreads the criminals over the jails: each jail holds capacity * efficiency * (criminals / total capacity),
// everything above the total capacity stays on the streets.
void cSC4PoliceSimulator::RecalculateCriminalStatistics() {
    jailCapacity_ = 0;
    jailPopulation_ = 0;
    float fraction = 0.0f;
    if (!jails_.empty()) {
        for (IPoliceOccupant* jail : jails_) jailCapacity_ += GetJailCapacity(jail);
        if (jailCapacity_ > 0) fraction = jailCapacity_ < criminals_ ? 1.0f : static_cast<float>(criminals_) / static_cast<float>(jailCapacity_);
    }
    for (IPoliceOccupant* jail : jails_) {
        cISCPropertyHolder* holder = jail->PropertyHolder();
        std::uint32_t capacity = 0;
        cSCPropertyHelp::GetPropertyValue(holder, kPoliceJailCapacity, capacity);
        const std::uint32_t effective = static_cast<std::uint32_t>(RoundEven(static_cast<float>(capacity) * CalculateJailEfficiency(jail)));
        const std::uint32_t inmates = static_cast<std::uint32_t>(RoundEven(static_cast<float>(effective) * fraction));
        cSCPropertyHelp::SetPropertyValue(holder, kPoliceJailPopulation, inmates);
        jailPopulation_ += static_cast<int>(inmates);
    }
}

void cSC4PoliceSimulator::DoMonthlyCriminalRehabilitation() {
    const int released = RoundEven(static_cast<float>(criminals_) * tunables_.rehabilitationRate);
    if (released == 0) return;
    criminals_ = std::max(criminals_ - released, 0);
    RecalculateCriminalStatistics();
}

void cSC4PoliceSimulator::UpdatePoliceStationCrimeStats(IPoliceOccupant* station, UpdateCrimeType type) {
    std::vector<std::uint32_t> stats;
    if (!cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kPoliceStationCrimeStats, stats)) stats.assign(4, 0);
    if (stats.size() < 4) stats.resize(4, 0);
    if (type == kRollover) {
        stats[1] = stats[3];
        stats[0] = stats[2];
        stats[2] = stats[3] = 0;
    } else if (type == kCrime || type == kArrest) {
        if (type == kArrest) ++stats[3];
        ++stats[2];
    }
    cSCPropertyHelp::SetPropertyValue(station->PropertyHolder(), kPoliceStationCrimeStats, stats);
}

void cSC4PoliceSimulator::ProcessMonthlyCrimeStats() {
    if (arrestsThisMonth_ > 0) {
        criminals_ = std::max(criminals_ + arrestsThisMonth_, 0);
        RecalculateCriminalStatistics();
    }
    arrestsPrev_ = arrestsThisMonth_;
    arrestsThisMonth_ = 0;
    crimesPrev_ = crimesThisMonth_;
    crimesThisMonth_ = 0;
    for (IProtectionStation* station : protection_.Stations()) UpdatePoliceStationCrimeStats(Police(station), kRollover);
    host_->AddHistory(kPoliceHistoryCrimes, crimesPrev_);
    host_->AddHistory(kPoliceHistoryArrests, arrestsPrev_);
}

int cSC4PoliceSimulator::ResetUnjailedCriminals() {
    const int allowed = static_cast<int>(static_cast<float>(jailCapacity_) * tunables_.unjailedFraction);
    int result;
    if (criminals_ < allowed) {
        result = unjailedAdded_;
    } else {
        result = (criminals_ - allowed) + unjailedAdded_;
        criminals_ = allowed;
    }
    unjailedAdded_ = 0;
    return result;
}

int cSC4PoliceSimulator::GetJailCapacity(IPoliceOccupant* jail) {
    cISCPropertyHolder* holder = jail->PropertyHolder();
    const float efficiency = CalculateJailEfficiency(jail);
    std::uint32_t capacity = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kPoliceJailCapacity, capacity);
    return RoundEven(static_cast<float>(capacity) * efficiency);
}

long cSC4PoliceSimulator::GetJailCurrentUpkeepCost(IPoliceOccupant* jail, long maintenance) {
    float fraction = 0.0f;
    if (!host_->FundingFraction(jail, kPoliceJailFundingProperty, fraction)) return 0;
    return static_cast<long>(fraction * static_cast<float>(maintenance));
}

float cSC4PoliceSimulator::GetJailedPercentage() const {
    if (jailCapacity_ < criminals_) return 1.0f;
    return static_cast<float>(criminals_) / static_cast<float>(jailCapacity_);
}

int cSC4PoliceSimulator::GetPoliceStationArrestCount(IPoliceOccupant* station) {
    std::vector<std::uint32_t> stats;
    return cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kPoliceStationCrimeStats, stats) && stats.size() > 1
               ? static_cast<int>(stats[1]) : 0;
}

int cSC4PoliceSimulator::GetPoliceStationCrimeCount(IPoliceOccupant* station) {
    std::vector<std::uint32_t> stats;
    return cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kPoliceStationCrimeStats, stats) && !stats.empty()
               ? static_cast<int>(stats[0]) : 0;
}

// ---- strikes --------------------------------------------------------------------------------------------

bool cSC4PoliceSimulator::IsStationOnStrike(IPoliceOccupant* station) {
    std::uint32_t start = 0;
    return cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kPoliceStrikeStartDate, start) && start != 0;
}

// A station strikes with a chance from the strike curve of its funding; once striking it stays out at least
// `strikeMinDays` and then returns when funding recovered to the end threshold. True when the state changed.
bool cSC4PoliceSimulator::UpdateStrikingState(IPoliceOccupant* station) {
    const std::uint32_t date = static_cast<std::uint32_t>(host_->CurrentDay());
    std::uint32_t start = 0;
    cSCPropertyHelp::GetPropertyValue(station->PropertyHolder(), kPoliceStrikeStartDate, start);
    std::uint32_t next;
    float fraction = 0.0f;
    if (start == 0) {
        const bool found = host_->FundingFraction(station, kPoliceStationFundingProperty, fraction);
        const float chance = strikeChance_.GetOutputValue(found ? fraction * 100.0f : 100.0f) * 0.01f;
        next = static_cast<float>(random_.RandomDoubleUniform()) < chance ? date : 0;
    } else {
        next = 0;
        if (date - start <= tunables_.strikeMinDays) {
            if (!host_->FundingFraction(station, kPoliceStationFundingProperty, fraction)) fraction = 1.0f;
            next = start;
            if (tunables_.strikeEndFunding <= fraction) next = 0;
        }
    }
    if (next == start) return false;
    cSCPropertyHelp::SetPropertyValue(station->PropertyHolder(), kPoliceStrikeStartDate, next);
    host_->SetStrikerGroup(station, next != 0);
    return true;
}

float cSC4PoliceSimulator::GetChanceOfStrike() {
    if (onStrike_) return 1.0f;
    float chance = 0.0f;
    for (IProtectionStation* station : protection_.Stations()) {
        float fraction = 0.0f;
        const bool found = host_->FundingFraction(Police(station), kPoliceStationFundingProperty, fraction);
        const float c = strikeChance_.GetOutputValue(found ? fraction * 100.0f : 100.0f) * 0.01f;
        chance = (chance + c) - c * chance;
    }
    return chance;
}

bool cSC4PoliceSimulator::BeginStrike(IPoliceOccupant* station) {
    if (onStrike_) return false;
    onStrike_ = true;
    strikeStart_ = host_->CurrentDay();
    host_->PostMessage(kPoliceMessageStrikeBegin, station);
    return true;
}

bool cSC4PoliceSimulator::EndStrike() {
    if (!onStrike_) return false;
    onStrike_ = false;
    strikeStart_ = 0;
    host_->PostMessage(kPoliceMessageStrikeEnd, nullptr);
    return true;
}

int cSC4PoliceSimulator::GetStrikeLength() { return strikeStart_ == 0 ? -1 : host_->CurrentDay() - strikeStart_; }

int cSC4PoliceSimulator::GetStrikingStations(std::list<IPoliceOccupant*>& out) {
    int count = 0;
    if (!onStrike_) return 0;
    for (IProtectionStation* station : protection_.Stations()) {
        if (IsStationOnStrike(Police(station))) {
            out.push_back(Police(station));
            ++count;
        }
    }
    return count;
}

// Original quirk kept: once one station changed state, every later striking station (re)starts the city-wide
// strike because the "changed" flag is sticky across the loop.
void cSC4PoliceSimulator::CheckForStrikeStartOrEnd() {
    if (protection_.Stations().empty()) return;
    int striking = 0;
    bool changed = false;
    for (IProtectionStation* station : protection_.Stations()) {
        IPoliceOccupant* police = Police(station);
        if (UpdateStrikingState(police)) changed = true;
        if (IsStationOnStrike(police)) {
            ++striking;
            if (changed) BeginStrike(police);
        }
    }
    if (changed) {
        if (striking == 0 && IsOnStrike()) EndStrike();
        protection_.UpdateUnadjustedProtectionGrid();
    }
}

// ---- jailbreaks -----------------------------------------------------------------------------------------

bool cSC4PoliceSimulator::IsInJailbreak(IPoliceOccupant* jail) const {
    if (!jail || !IsJail(jail)) return false;
    return std::find(jailbreakBuildings_.begin(), jailbreakBuildings_.end(), jail) != jailbreakBuildings_.end();
}

float cSC4PoliceSimulator::GetChanceOfJailbreak(IPoliceOccupant* jail) {
    if (!jail || !IsJail(jail) || jailPopulation_ < 1) return 0.0f;
    if (IsInJailbreak(jail)) return 0.0f;
    return jailbreakChance_.GetOutputValue(GetJailedPercentage() * 100.0f) * 0.01f;
}

bool cSC4PoliceSimulator::BeginJailbreak(IPoliceOccupant* jail) {
    if (!jail || !IsJail(jail) || IsInJailbreak(jail)) return false;
    jailbreakBuildings_.push_back(jail);
    const float fraction = static_cast<float>(random_.RandomDoubleRangeUniform(tunables_.jailbreakMin, tunables_.jailbreakMax));
    const int escaped = RoundEven(static_cast<float>(GetJailCapacity(jail)) * fraction);
    if (escaped != 0) {
        criminals_ = std::max(criminals_ - escaped, 0);
        RecalculateCriminalStatistics();
    }
    host_->PostMessage(kPoliceMessageJailbreakBegin, jail);
    host_->SetJailbreakGroup(jail, true);
    return true;
}

// Quirk kept: always reports false even when the jailbreak ended.
bool cSC4PoliceSimulator::EndJailbreak(IPoliceOccupant* jail) {
    if (!jail || !IsJail(jail)) return false;
    host_->SetJailbreakGroup(jail, false);
    auto it = FindIn(jailbreakBuildings_, jail);
    if (it == jailbreakBuildings_.end()) return false;
    host_->PostMessage(kPoliceMessageJailbreakEnd, jail);
    jailbreakBuildings_.erase(it);
    return false;
}

void cSC4PoliceSimulator::CheckForJailbreakEnd() {
    for (auto it = jailbreakBuildings_.begin(); it != jailbreakBuildings_.end();) {
        IPoliceOccupant* jail = *it++;
        if (GetJailedPercentage() < tunables_.jailbreakEndThreshold) EndJailbreak(jail);
    }
}

void cSC4PoliceSimulator::CheckForJailbreakStart() {
    for (IPoliceOccupant* jail : std::list<IPoliceOccupant*>(jails_)) {
        const float chance = GetChanceOfJailbreak(jail);
        if (0.0f < chance && static_cast<float>(random_.RandomDoubleUniform()) < chance) BeginJailbreak(jail);
    }
}

int cSC4PoliceSimulator::GetJailbreakBuildings(std::list<IPoliceOccupant*>& out) const {
    out.assign(jailbreakBuildings_.begin(), jailbreakBuildings_.end());
    return static_cast<int>(out.size());
}

// ---- buildings ------------------------------------------------------------------------------------------

bool cSC4PoliceSimulator::IsPoliceStation(IPoliceOccupant* occupant) { return occupant->PropertyHolder()->HasProperty(kPoliceStationRadius); }
bool cSC4PoliceSimulator::IsJail(IPoliceOccupant* occupant) { return occupant->PropertyHolder()->HasProperty(kPoliceJailCapacity); }

// The three occupant hooks post 0xa949569a (data: the simulator) only for police stations: vslot 0xb0 is
// IsPoliceStation(occupant); the decompile drops the occupant argument (it is in RSI, checked against the disassembly).
void cSC4PoliceSimulator::DoOccupantInserted(IPoliceOccupant* occupant) {
    protection_.DoOccupantInserted(occupant);
    if (occupant->OccupantType() != kProtectionBuildingOccupantType) return;
    if (IsPoliceStation(occupant)) host_->PostMessage(kPoliceMessageStationsChanged, nullptr);
    if (!IsJail(occupant)) return;
    jails_.push_back(occupant);
    float fraction = 0.0f;
    if (host_->FundingFraction(occupant, kPoliceJailFundingProperty, fraction)) {
        if (FindIn(fundedJails_, occupant) == fundedJails_.end()) fundedJails_.push_back(occupant);
        host_->RegisterFundedStation(occupant, kPoliceJailFundingProperty, true);
    }
    RecalculateCriminalStatistics();
}

void cSC4PoliceSimulator::DoOccupantRemoved(IPoliceOccupant* occupant) {
    if (occupant->OccupantType() != kProtectionBuildingOccupantType) return;
    auto funded = FindIn(fundedJails_, occupant);
    if (funded != fundedJails_.end()) fundedJails_.erase(funded);
    auto jail = FindIn(jails_, occupant);
    if (jail != jails_.end()) {
        jails_.erase(jail);
        float fraction = 0.0f;
        if (host_->FundingFraction(occupant, kPoliceJailFundingProperty, fraction))
            host_->RegisterFundedStation(occupant, kPoliceJailFundingProperty, false);
        RecalculateCriminalStatistics();
    }
    protection_.DoOccupantRemoved(occupant);
    if (IsPoliceStation(occupant)) host_->PostMessage(kPoliceMessageStationsChanged, nullptr);
}

void cSC4PoliceSimulator::DoOccupantChanged(IPoliceOccupant* occupant) {
    protection_.DoOccupantChanged(occupant);
    if (IsPoliceStation(occupant)) host_->PostMessage(kPoliceMessageStationsChanged, nullptr);
}

// 0x1000a5826: a null cost succeeds without asking the budget; otherwise the budget record's cost (+0x10).
bool cSC4PoliceSimulator::ProtectionGetStationFunding(IPoliceOccupant* station, int* cost) const {
    if (!cost) return true;
    int c = 0;
    float f = 0.0f;
    if (!host_->FundingInfo(station, kPoliceStationFundingProperty, c, f)) return false;
    *cost = c;
    return true;
}

IPoliceOccupant* cSC4PoliceSimulator::GetNearestStation(float x, float z) {
    IPoliceOccupant* best = nullptr;
    float bestDistance = 3.4028235e+38f;
    for (IProtectionStation* station : protection_.Stations()) {
        float position[3];
        station->Position(position);
        const float distance = std::sqrt((position[0] - x) * (position[0] - x) + (position[2] - z) * (position[2] - z));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = Police(station);
        }
    }
    return best;
}

int cSC4PoliceSimulator::GetTotalDispatchCount() {
    int total = 0;
    for (IProtectionStation* station : protection_.Stations()) {
        std::uint32_t count = 0;
        total += cSCPropertyHelp::GetPropertyValue(Police(station)->PropertyHolder(), kPoliceStationDispatchCount, count)
                     ? static_cast<int>(count) : 0;
    }
    return total;
}

bool cSC4PoliceSimulator::GetDispatchesAtStation(IPoliceOccupant* station, std::uint32_t* mask, std::uint32_t* baseCall,
                                                 std::uint32_t* count) {
    cISCPropertyHolder* holder = station->PropertyHolder();
    std::uint32_t total = 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPoliceStationDispatchCount, total)) return false;
    if (count) *count = total;
    if (mask) {
        std::uint32_t flags = 0;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kPoliceStationAvailability, flags)) return false;
        *mask = ((1u << (total & 31)) - 1u) & flags;
    }
    if (baseCall) {
        std::uint32_t base = 0;
        if (!cSCPropertyHelp::GetPropertyValue(holder, kPoliceStationBaseCallNumber, base)) return false;
        *baseCall = base;
    }
    return true;
}

// ---- schedule and protection hooks --------------------------------------------------------------------------

void cSC4PoliceSimulator::DoMonthlySimulation() {
    DoMonthlyCriminalRehabilitation();
    CheckForStrikeStartOrEnd();
    CheckForJailbreakEnd();
    CheckForJailbreakStart();
}

// The original resets the funding scale to 1.0 every day before the base sweep (efficiency is applied to the
// station effects instead).
void cSC4PoliceSimulator::DoDailySimulation() {
    protection_.scale = 1.0f;
    protection_.DoDailySimulation();
}

void cSC4PoliceSimulator::ProtectionEndOfCycle() {
    host_->AddHistory(kPoliceHistoryPercentCoverage, static_cast<int>(protection_.CoverageShare() * 100.0f));
    host_->AddHistory(kPoliceHistoryAverageCoverage, static_cast<int>(protection_.AverageProtection()));
    host_->PostMessage(kPoliceMessageCoverage, nullptr);
}

bool cSC4PoliceSimulator::GetStationEffect(IProtectionStation* station, ProtectionEffect& effect) {
    return ProtectionGetStationEffect(Police(station), effect);
}

void cSC4PoliceSimulator::SetStationDispatchMask(IProtectionStation* station, std::uint32_t mask) {
    cSCPropertyHelp::SetPropertyValue(Police(station)->PropertyHolder(), kPoliceStationAvailability, mask);
}

void cSC4PoliceSimulator::SetStationDispatchOffset(IProtectionStation* station, std::uint32_t offset) {
    cSCPropertyHelp::SetPropertyValue(Police(station)->PropertyHolder(), kPoliceStationBaseCallNumber, offset);
}

void cSC4PoliceSimulator::GetStationInfo(IProtectionStation* station, int* available, std::uint32_t* offset,
                                         std::uint32_t* count) {
    std::uint32_t mask = 0, base = 0, total = 0;
    const bool ok = GetDispatchesAtStation(Police(station), available ? &mask : nullptr, offset ? &base : nullptr, count ? &total : nullptr);
    if (available) *available = ok ? static_cast<int>(mask) : 0;
    if (offset) *offset = ok ? base : 0;
    if (count) *count = ok ? total : 0;
}

void cSC4PoliceSimulator::RegisterStationProperty(IProtectionStation* station, bool add) {
    host_->RegisterFundedStation(Police(station), kPoliceStationFundingProperty, add);
}

void cSC4PoliceSimulator::GetDispatchEffect(const ProtectionDispatchInfo& info, ProtectionEffect& effect) {
    if (info.origin) ProtectionGetDispatchEffect(Police(info.origin), effect);
}

}  // namespace sc4
