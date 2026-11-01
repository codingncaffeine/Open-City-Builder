#include "sc4/PollutionSimulator.h"
#include "sc4/SimStreamOps.h"

#include <algorithm>
#include <cmath>

#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "sc4/ProtectionSimulator.h"

namespace sc4 {

namespace {
// Messages posted at the end of a cycle when the matching grid changed.
constexpr std::uint32_t kMessageRadiationChanged = 0xda6dd35d;
constexpr std::uint32_t kMessageAirChanged = 0xfa6dd0bb;
constexpr std::uint32_t kMessageWaterChanged = 0x5a6dd1aa;
constexpr std::uint32_t kMessageGarbageChanged = 0xda6dd2cb;
constexpr std::uint32_t kMessageSanitationEfficiency = 0xea418d8f;
// history records
constexpr std::uint32_t kHistoryAir = 0x0a1100e8;
constexpr std::uint32_t kHistoryWater = 0xea110130;
constexpr std::uint32_t kHistoryGarbage = 0xea110134;
constexpr std::uint32_t kHistoryRadiation = 0xea110137;
constexpr std::uint32_t kHistoryTotal = 0xea110138;

inline std::int32_t RoundToEven(float v) { return static_cast<std::int32_t>(std::nearbyint(v)); }
// A float to an unsigned int the way the original converts it (0 below zero, saturating at 2^32).
inline std::uint32_t ToUint(float v) { return v <= 0.0f ? 0u : v >= 4294967296.0f ? 0xffffffffu : static_cast<std::uint32_t>(v); }

constexpr std::uint32_t kMessageDaily = 0x66956814;
constexpr std::uint32_t kMessageMonthly = 0x66956816;
constexpr std::uint32_t kMessageSimulationBegin = 0x66956810;
constexpr std::uint32_t kMessageOccupantInserted = 0x99ef1142;
constexpr std::uint32_t kMessageOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kMessageCityStarted = 0x26d31ec1;
constexpr std::uint32_t kMessageTunablesChanged = 0xca1b151c;
constexpr std::uint32_t kMessageViewLayer = 0xc6cdbf1a;
constexpr std::uint32_t kNotifications[] = {kMessageOccupantInserted, kMessageOccupantRemoved, kMessageCityStarted,
                                            kMessageTunablesChanged, kMessageViewLayer};
constexpr std::uint32_t kGarbageImportDeals = 0x4773ffe5, kGarbageExportDeals = 0x4773ffe4;
constexpr float kDealShare = 0.1f;                // cSTESurfaceWater::UpdateFloodWaters()::kMaxSedimentToDeposit, shared constant
constexpr float kMaxCellGarbage = 32767.0f;       // 0x00a70720
}  // namespace

cSC4PollutionSimulator::cSC4PollutionSimulator(IPollutionHost* host) : host_(host) {}

// 0x100094c90: the original answers +0 (iid 1 / 0x877181ec), +8 (message target), +0x10 (0x89c498be)
// and +0x18 (serializable). The port keeps a single cIGZSerializable base, so every accepted id resolves
// to `this`; the serializable id is what the city-load serializer (SC4ReadGZSerializable) asks for.
bool cSC4PollutionSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == 0xe4fda3d4) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == 0x452294aa) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == 0x877181ec || iid == kGZCLSID_cSC4PollutionSimulator)
        *out = static_cast<cIGZSerializable*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x100094d48: below 2 destroys the object (the original runs FinalRelease, which Shutdowns and deletes).
std::uint32_t cSC4PollutionSimulator::Release() {
    if (refCount_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refCount_;
}

// 0x1000961d0. PARTIAL: the original writes version 4 then, in order, the four value grids (+0x78),
// the four source grids (+0x98), two s32 (+0xb8/+0xbc), the four occupant ilists (+0xc0/+0xd8/+0xf0/+0x108),
// then the scalars the port keeps, then two u64 (+0x2d0/+0x2d8), two u64 (+0x250/+0x258), one double
// (+0x268), one s32 (+0x2e4), a RandomIterator (+0x2e8) and the prop-request curve (+0x1c0). The occupant
// lists, the two +0xb8/+0xbc counters, the four un-identified u64/double/s32 slots and the RandomIterator
// belong to the unrecovered daily-diffusion state, so they are not serialised here — the field order of
// the scalars below matches the binary.
bool cSC4PollutionSimulator::Write(cIGZOStream& stream) const {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentOStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return WriteCityRecord(stream);
    }
    stream.SetUint16(4);
    for (auto& grid : valueGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    for (auto& grid : sourceGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    stream.SetUint32(recyclingCenterCountSnapshot);
    stream.SetSint32(averageAir_);
    stream.SetSint32(averageWater_);
    stream.SetUint32(airSum);
    stream.SetUint32(waterSum);
    stream.SetUint32(periodValue21c[0]);
    stream.SetUint32(periodValue21c[1]);
    stream.SetUint32(periodValue224[0]);
    stream.SetUint32(periodValue224[1]);
    stream.SetUint32(cellCount);
    stream.SetUint16(static_cast<std::uint16_t>(averageGarbage_));
    stream.SetUint32(monthlyIncineratorCapacity_);
    stream.SetUint32(monthlyWasteToEnergyCapacity_);
    stream.SetFloat32(recyclingEffect_);
    stream.SetUint8(garbageUncollected_ ? 1 : 0);
    stream.SetUint32(totalGarbageUncollected_);
    stream.SetSint32(abductedPrev_);
    stream.SetSint64(producedPrev_);
    stream.SetSint64(importedPrev_);
    stream.SetSint64(exportedPrev_);
    stream.SetFloat64(recycledPrev_);
    stream.SetSint64(incineratedPrev_);
    stream.SetSint64(convertedPrev_);
    stream.SetSint64(landfilledPrev_);
    stream.SetSint64(recycledThisMonth_);
    stream.SetSint64(wasteToEnergyThisMonth_);
    stream.SetSint64(incineratedThisMonth_);
    stream.SetSint64(landfilledThisMonth_);
    stream.SetUint32(abductedThisMonth_);
    stream.SetUint32(static_cast<std::uint32_t>(recyclingAllowance_));
    stream.SetSint32(activeLandfillCount_);
    stream.SetUint8(dirtyRadiation ? 1 : 0);
    stream.SetUint8(dirtyAir ? 1 : 0);
    stream.SetUint8(dirtyWater ? 1 : 0);
    stream.SetUint8(dirtyGarbage ? 1 : 0);
    stream.SetFloat32(efficiency_);
    stream.SetUint32(static_cast<std::uint32_t>(garbageSupply_));
    stream.SetUint32(static_cast<std::uint32_t>(garbageCapacity_));
    return !stream.GetError();
}

// 0x100096588. PARTIAL: mirrors Write; the version gate and the occupant-list / RandomIterator / curve
// reads belong to the unrecovered state and are left out (see Write's field list).
// 0x100096588: the record of a city file. The eight grids are kept; the four building lists are object references (consumed: the port
// finds the buildings through the occupant manager); the counters, totals and flags are kept under their original offsets. A stream that is
// not a city-file segment stream (the unit tests) carries the port's own reduced record, read below.
bool cSC4PollutionSimulator::ReadCityRecord(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 4) return false;
    for (auto& grid : valueGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) grid.reset(SimGridCast<std::int16_t>(raw));
    }
    for (auto& grid : sourceGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) grid.reset(SimGridCast<std::int16_t>(raw));
    }
    std::uint32_t u32 = 0;
    std::int64_t s64 = 0;
    double f64 = 0.0;
    std::uint8_t u8 = 0;
    fromCityFile_ = true;
    loaded_ = true;
    stream.GetUint32(cellX_);                                // +0xb8
    stream.GetUint32(cellZ_);                                // +0xbc
    for (auto& kept : fileLists_) {                           // +0xc0 / +0xd8 / +0xf0 / +0x108: lists of occupants (kept as read)
        kept.clear();
        stream.GetUint32(u32);
        if (u32 < 10000000u)
            for (std::uint32_t i = 0; i < u32 && !stream.GetError(); ++i) { kept.emplace_back(); kept.back().Read(stream); }
    }
    stream.GetUint32(recyclingCenterCountSnapshot);           // +0x208
    stream.GetSint32(averageAir_);                            // +0x20c
    stream.GetSint32(averageWater_);                          // +0x210
    stream.GetUint32(airSum);                                 // +0x214
    stream.GetUint32(waterSum);                               // +0x218
    stream.GetUint32(periodValue21c[0]);                      // +0x21c
    stream.GetUint32(periodValue21c[1]);                      // +0x220
    stream.GetUint32(periodValue224[0]);                      // +0x224
    stream.GetUint32(periodValue224[1]);                      // +0x228
    stream.GetUint32(cellCount);                              // +0x22c
    if (version < 3) {                                        // versions 1-2 (the PC saves) carried a string here, dropped on read
        cRZString dropped;
        stream.GetGZStr(dropped);
    }
    std::uint16_t avgGarbage = 0;
    stream.GetUint16(avgGarbage); averageGarbage_ = static_cast<std::int16_t>(avgGarbage);   // +0x234
    stream.GetUint32(monthlyIncineratorCapacity_);            // +0x238
    stream.GetUint32(monthlyWasteToEnergyCapacity_);          // +0x23c
    stream.GetFloat32(recyclingEffect_);                      // +0x240
    stream.GetUint8(u8); garbageUncollected_ = u8 != 0;       // +0x244
    stream.GetUint32(totalGarbageUncollected_);               // +0x248
    stream.GetSint64(file2d0_);                               // +0x2d0
    stream.GetSint64(sanitationCost_);                               // +0x2d8
    if (version > 3) { stream.GetUint32(u32); abductedPrev_ = static_cast<std::int32_t>(u32); }   // +0x2c8
    stream.GetSint64(producedPrev_);                          // +0x288
    stream.GetSint64(importedPrev_);                          // +0x290
    stream.GetSint64(exportedPrev_);                          // +0x298
    stream.GetFloat64(recycledPrev_);                         // +0x2a0
    stream.GetSint64(incineratedPrev_);                       // +0x2a8
    stream.GetSint64(convertedPrev_);                         // +0x2b0
    stream.GetSint64(landfilledPrev_);                        // +0x2b8
    stream.GetSint64(producedThisMonth_);                               // +0x250
    stream.GetSint64(importedThisMonth_);                               // +0x258
    stream.GetSint64(recycledThisMonth_);                     // +0x260
    stream.GetFloat64(recyclingShare268_);                    // +0x268
    stream.GetSint64(wasteToEnergyThisMonth_);                // +0x270
    stream.GetSint64(incineratedThisMonth_);                  // +0x278
    stream.GetSint64(landfilledThisMonth_);                   // +0x280
    stream.GetUint32(abductedThisMonth_);                     // +0x2c4
    std::int32_t allowance = 0;
    stream.GetSint32(allowance); recyclingAllowance_ = allowance;   // +0x2c0
    std::int32_t landfills = 0;
    stream.GetUint32(u32); landfills = static_cast<std::int32_t>(u32); activeLandfillCount_ = landfills;   // +0x2cc
    stream.GetUint32(cellsPerDay_);                               // +0x2e4
    stream >> walk_;                                      // +0x2e8
    stream.GetUint8(u8); dirtyRadiation = u8 != 0;            // +0x328..
    stream.GetUint8(u8); dirtyAir = u8 != 0;
    stream.GetUint8(u8); dirtyWater = u8 != 0;
    stream.GetUint8(u8); dirtyGarbage = u8 != 0;
    if (version < 3) stream.GetUint32(u32);                   // versions 1-2: one more word, dropped on read
    stream.GetFloat32(efficiency_);                           // +0x348
    std::int32_t supply = 0, capacity = 0;
    stream.GetSint32(supply); garbageSupply_ = supply;        // +0x34c
    stream.GetSint32(capacity); garbageCapacity_ = capacity;  // +0x350
    fileWaterGrid_ = cRetainedReference();
    fileWaterGrid_.Read(stream);                              // +0x1c0: the water grid
    (void)s64;
    return !stream.GetError();
}

// The city-file record, as ReadCityRecord reads it (version 4); what the port does not model is written as the file had it.
bool cSC4PollutionSimulator::WriteCityRecord(cIGZOStream& stream) const {
    stream.SetUint16(4);
    for (auto& grid : valueGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    for (auto& grid : sourceGrids_) SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid.get()));
    stream.SetUint32(cellX_);                                // +0xb8
    stream.SetUint32(cellZ_);                                // +0xbc
    for (const auto& kept : fileLists_) {                     // +0xc0 / +0xd8 / +0xf0 / +0x108
        stream.SetUint32(std::uint32_t(kept.size()));
        for (const cRetainedReference& occupant : kept) occupant.Write(stream);
    }
    stream.SetUint32(recyclingCenterCountSnapshot);           // +0x208
    stream.SetSint32(averageAir_);                            // +0x20c
    stream.SetSint32(averageWater_);                          // +0x210
    stream.SetUint32(airSum);                                 // +0x214
    stream.SetUint32(waterSum);                               // +0x218
    stream.SetUint32(periodValue21c[0]);                      // +0x21c
    stream.SetUint32(periodValue21c[1]);                      // +0x220
    stream.SetUint32(periodValue224[0]);                      // +0x224
    stream.SetUint32(periodValue224[1]);                      // +0x228
    stream.SetUint32(cellCount);                              // +0x22c
    stream.SetUint16(static_cast<std::uint16_t>(averageGarbage_));   // +0x234
    stream.SetUint32(monthlyIncineratorCapacity_);            // +0x238
    stream.SetUint32(monthlyWasteToEnergyCapacity_);          // +0x23c
    stream.SetFloat32(recyclingEffect_);                      // +0x240
    stream.SetUint8(garbageUncollected_ ? 1 : 0);             // +0x244
    stream.SetUint32(totalGarbageUncollected_);               // +0x248
    stream.SetSint64(file2d0_);                               // +0x2d0
    stream.SetSint64(sanitationCost_);                               // +0x2d8
    stream.SetUint32(static_cast<std::uint32_t>(abductedPrev_));   // +0x2c8
    stream.SetSint64(producedPrev_);                          // +0x288
    stream.SetSint64(importedPrev_);                          // +0x290
    stream.SetSint64(exportedPrev_);                          // +0x298
    stream.SetFloat64(recycledPrev_);                         // +0x2a0
    stream.SetSint64(incineratedPrev_);                       // +0x2a8
    stream.SetSint64(convertedPrev_);                         // +0x2b0
    stream.SetSint64(landfilledPrev_);                        // +0x2b8
    stream.SetSint64(producedThisMonth_);                               // +0x250
    stream.SetSint64(importedThisMonth_);                               // +0x258
    stream.SetSint64(recycledThisMonth_);                     // +0x260
    stream.SetFloat64(recyclingShare268_);                    // +0x268
    stream.SetSint64(wasteToEnergyThisMonth_);                // +0x270
    stream.SetSint64(incineratedThisMonth_);                  // +0x278
    stream.SetSint64(landfilledThisMonth_);                   // +0x280
    stream.SetUint32(abductedThisMonth_);                     // +0x2c4
    stream.SetSint32(static_cast<std::int32_t>(recyclingAllowance_));   // +0x2c0
    stream.SetUint32(static_cast<std::uint32_t>(activeLandfillCount_)); // +0x2cc
    stream.SetUint32(cellsPerDay_);                               // +0x2e4
    stream << walk_;                                      // +0x2e8
    stream.SetUint8(dirtyRadiation ? 1 : 0);                  // +0x328..
    stream.SetUint8(dirtyAir ? 1 : 0);
    stream.SetUint8(dirtyWater ? 1 : 0);
    stream.SetUint8(dirtyGarbage ? 1 : 0);
    stream.SetFloat32(efficiency_);                           // +0x348
    stream.SetSint32(static_cast<std::int32_t>(garbageSupply_));     // +0x34c
    stream.SetSint32(static_cast<std::int32_t>(garbageCapacity_));   // +0x350
    fileWaterGrid_.Write(stream);                             // +0x1c0
    return !stream.GetError();
}

bool cSC4PollutionSimulator::Read(cIGZIStream& stream) {
    void* dbStream = nullptr;
    if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream) {
        static_cast<cIGZUnknown*>(dbStream)->Release();
        return ReadCityRecord(stream);
    }
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version > 4) return false;
    for (auto& grid : valueGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) {
            grid.reset(SimGridCast<std::int16_t>(raw));   // keeps the reference the read took
        }
    }
    for (auto& grid : sourceGrids_) {
        void* raw = nullptr;
        if (SC4ReadGZSerializable(stream, kGZIID_cISC4SimGrid, &raw, false)) {
            grid.reset(SimGridCast<std::int16_t>(raw));   // keeps the reference the read took
        }
    }
    stream.GetUint32(recyclingCenterCountSnapshot);
    stream.GetSint32(averageAir_);
    stream.GetSint32(averageWater_);
    stream.GetUint32(airSum);
    stream.GetUint32(waterSum);
    stream.GetUint32(periodValue21c[0]);
    stream.GetUint32(periodValue21c[1]);
    stream.GetUint32(periodValue224[0]);
    stream.GetUint32(periodValue224[1]);
    stream.GetUint32(cellCount);
    std::uint16_t avgGarbage = 0;
    stream.GetUint16(avgGarbage);
    averageGarbage_ = static_cast<std::int16_t>(avgGarbage);
    stream.GetUint32(monthlyIncineratorCapacity_);
    stream.GetUint32(monthlyWasteToEnergyCapacity_);
    stream.GetFloat32(recyclingEffect_);
    std::uint8_t uncollected = 0;
    stream.GetUint8(uncollected);
    garbageUncollected_ = uncollected != 0;
    stream.GetUint32(totalGarbageUncollected_);
    stream.GetSint32(abductedPrev_);
    stream.GetSint64(producedPrev_);
    stream.GetSint64(importedPrev_);
    stream.GetSint64(exportedPrev_);
    stream.GetFloat64(recycledPrev_);
    stream.GetSint64(incineratedPrev_);
    stream.GetSint64(convertedPrev_);
    stream.GetSint64(landfilledPrev_);
    stream.GetSint64(recycledThisMonth_);
    stream.GetSint64(wasteToEnergyThisMonth_);
    stream.GetSint64(incineratedThisMonth_);
    stream.GetSint64(landfilledThisMonth_);
    stream.GetUint32(abductedThisMonth_);
    std::uint32_t recyclingAllowance = 0;
    stream.GetUint32(recyclingAllowance);
    recyclingAllowance_ = static_cast<std::int32_t>(recyclingAllowance);
    stream.GetSint32(activeLandfillCount_);
    std::uint8_t b = 0;
    stream.GetUint8(b); dirtyRadiation = b != 0;
    stream.GetUint8(b); dirtyAir = b != 0;
    stream.GetUint8(b); dirtyWater = b != 0;
    stream.GetUint8(b); dirtyGarbage = b != 0;
    stream.GetFloat32(efficiency_);
    std::uint32_t supply = 0, capacity = 0;
    stream.GetUint32(supply); garbageSupply_ = static_cast<int>(supply);
    stream.GetUint32(capacity); garbageCapacity_ = static_cast<int>(capacity);
    return !stream.GetError();
}

void cSC4PollutionSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    tunables_ = PollutionTunables();
    // default response curves: the prop request / effect curves start as the single point (0, 0)
    const std::vector<SC4Point> zero{{0.0f, 0.0f}};
    propRequestCurve_.SetResponseCurveData(zero);
    garbageTransportCapacity_.SetResponseCurveData(zero);
    const std::vector<SC4Point> identity{{0.0f, 1.0f}, {1.0f, 1.0f}};
    ageCurve_.SetResponseCurveData(identity);
    efficiencyCurve_.SetResponseCurveData(identity);
    if (!exemplar) return;
    PollutionTunables& t = tunables_;
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropTrafficAirFactor, t.trafficAirFactor);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxGarbageForUI, t.maxGarbageForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxAirForUI, t.maxAirForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxWaterForUI, t.maxWaterForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxRadiationForUI, t.maxRadiationForUI);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxAirDelta, t.maxAirDelta);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxWaterDelta, t.maxWaterDelta);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropGarbageThreshold, t.garbageThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropAirThreshold, t.airThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropWaterThreshold, t.waterThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropRadiationThreshold, t.radiationThreshold);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropAbductionAmount, t.abductionAmount);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropGarbageScaling, t.garbageScalingFactor);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropFloat2e0, t.value2e0);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropMaxGarbageOnValve, t.maxGarbageOnValve);
    cSCPropertyHelp::GetPropertyValue(exemplar, kPollutionPropLandfillTileCapacity, t.landfillTileCapacity);
    auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id) {
        std::vector<float> values;
        if (exemplar->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(exemplar, id, values))
            curve.SetResponseCurveData(values.data(), values.size());
    };
    loadCurve(propRequestCurve_, 0x69a437f6);
    loadCurve(garbageTransportCapacity_, 0x8c19c3e9);
    loadCurve(ageCurve_, 0x0911e2e8);
    loadCurve(efficiencyCurve_, 0x0911e2e9);
    loadCurve(fundingEfficiencyCurve_, kPollutionPropFundingEfficiencyCurve);
}

void cSC4PollutionSimulator::CreateGrids() {
    for (int i = 0; i < 4; ++i) {
        if (!valueGrids_[i]) {
            valueGrids_[i] = SimGridPtr<std::int16_t>(new cSC4SimGrid<std::int16_t>());
            valueGrids_[i]->SetTractSize(2);
            valueGrids_[i]->SetInstanceID(kPollutionGridInstanceValueBase + static_cast<std::uint32_t>(i));
        }
        if (!sourceGrids_[i]) {
            sourceGrids_[i] = SimGridPtr<std::int16_t>(new cSC4SimGrid<std::int16_t>());
            sourceGrids_[i]->SetTractSize(i == 2 ? 1 : 2);
            sourceGrids_[i]->SetInstanceID(kPollutionGridInstanceSourceBase + static_cast<std::uint32_t>(i));
        }
    }
}

cSC4SimGrid<std::int16_t>* cSC4PollutionSimulator::GetPollutionGrid(PollutionType type) const {
    return type == kPollutionRadiation ? valueGrids_[3].get() : sourceGrids_[type].get();
}

std::int16_t cSC4PollutionSimulator::GetPollutionValue(PollutionType type, int x, int z) const {
    const std::int16_t v = valueGrids_[type]->GetCellValue(x, z);
    return v < 0 ? 0 : v;
}

bool cSC4PollutionSimulator::IsRadioactive(unsigned x, unsigned z) const {
    return valueGrids_[3]->GetCellValue(static_cast<int>(x), static_cast<int>(z)) != 0;
}

void cSC4PollutionSimulator::GetAirValue(unsigned x, unsigned z, int& out) const {
    out = sourceGrids_[0]->GetCellValue(static_cast<int>(x), static_cast<int>(z));
}

void cSC4PollutionSimulator::GetWaterValue(unsigned x, unsigned z, int& out) const {
    out = sourceGrids_[1]->GetCellValue(static_cast<int>(x), static_cast<int>(z));
}

// The garbage grid keeps the "active landfill" flag in bit 15; the value is read through the sign-extended short
// with that bit cleared, which leaves negative results for flagged cells (original quirk).
void cSC4PollutionSimulator::GetGarbageValue(unsigned x, unsigned z, int& out) const {
    const int v = sourceGrids_[2]->GetCellValue(static_cast<int>(x), static_cast<int>(z));
    out = v & static_cast<int>(0xffff7fff);
}

bool cSC4PollutionSimulator::IsAirPolluted(unsigned x, unsigned z) const {
    int value = 0;
    GetAirValue(x, z, value);
    return tunables_.airThreshold <= static_cast<std::uint32_t>(value);
}

bool cSC4PollutionSimulator::IsWaterPolluted(unsigned x, unsigned z) const {
    int value = 0;
    GetWaterValue(x, z, value);
    return tunables_.waterThreshold <= static_cast<std::uint32_t>(value);
}

bool cSC4PollutionSimulator::IsGarbageAccumulated(unsigned x, unsigned z) const {
    int value = 0;
    GetGarbageValue(x, z, value);
    return tunables_.garbageThreshold <= static_cast<std::uint32_t>(value);
}

void cSC4PollutionSimulator::SetIsRadioactive(unsigned x, unsigned z, bool on) {
    auto& grid = *valueGrids_[3];
    const int shift = grid.GetTractShift();
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift), on ? 1 : 0);
    dirtyRadiation = true;
}

void cSC4PollutionSimulator::SetAirValue(unsigned x, unsigned z, int value) {
    auto& grid = *sourceGrids_[0];
    const int shift = grid.GetTractShift();
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift), static_cast<std::int16_t>(value));
    dirtyAir = true;
}

void cSC4PollutionSimulator::SetWaterValue(unsigned x, unsigned z, int value) {
    auto& grid = *sourceGrids_[1];
    const int shift = grid.GetTractShift();
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift), static_cast<std::int16_t>(value));
    dirtyWater = true;
}

void cSC4PollutionSimulator::SetGarbageValue(unsigned x, unsigned z, int value) {
    auto& grid = *sourceGrids_[2];
    const int shift = grid.GetTractShift();
    const std::uint16_t flag = static_cast<std::uint16_t>(grid.GetCellValue(static_cast<int>(x), static_cast<int>(z))) & 0x8000;
    grid.SetTractValue(static_cast<int>(x >> shift), static_cast<int>(z >> shift),
                       static_cast<std::int16_t>(flag | static_cast<std::uint16_t>(value)));
    dirtyGarbage = true;
}

bool cSC4PollutionSimulator::IsActiveLandfill(unsigned x, unsigned z) const {
    return (static_cast<std::uint16_t>(sourceGrids_[2]->GetCellValue(static_cast<int>(x), static_cast<int>(z))) &
            0x8000) != 0;
}

// Turning a landfill on needs the cell to be zoned landfill (type 0xe); turning it off always works. The write
// goes straight to the tract (x, z) without shifting the cell coordinates, as in the original.
void cSC4PollutionSimulator::SetIsActiveLandfill(unsigned x, unsigned z, bool on, std::uint32_t zoneTypeAtCell) {
    if (zoneTypeAtCell != 0xe && on) return;
    auto& grid = *sourceGrids_[2];
    const bool active = IsActiveLandfill(x, z);
    std::int16_t value;
    if (!active && on) {
        value = static_cast<std::int16_t>(grid.GetCellValue(static_cast<int>(x), static_cast<int>(z)) | 0x8000);
        ++activeLandfillCount_;
    } else {
        if (!active || on) return;
        const std::int16_t current = grid.GetCellValue(static_cast<int>(x), static_cast<int>(z));
        if (activeLandfillCount_ != 0) --activeLandfillCount_;
        value = static_cast<std::int16_t>(current & 0x7fff);
    }
    grid.SetTractValue(static_cast<int>(x), static_cast<int>(z), value);
    dirtyGarbage = true;
}

std::int32_t cSC4PollutionSimulator::GetBuildingGarbageCapacity(cISCPropertyHolder* occupant) const {
    std::int32_t value = 0;
    if (!cSCPropertyHelp::GetPropertyValue(occupant, kPollutionOccGarbageCapacity, value)) return 0;
    return value;
}

void cSC4PollutionSimulator::AddToGarbageSupply(int amount) {
    if (amount < 0 && garbageSupply_ + amount < 0) amount = -garbageSupply_;
    garbageSupply_ = std::max(0, garbageSupply_ + amount);
}

int cSC4PollutionSimulator::GetGarbageSupply() {
    if (garbageSupply_ < 0) garbageSupply_ = 0;
    return garbageSupply_;
}

// Average garbage value over the rectangle through the prop-request curve, rounded to the nearest int.
int cSC4PollutionSimulator::GetRequestedShare(int x0, int z0, int x1, int z1) const {
    std::uint32_t total = 0;
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            int value = 0;
            GetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), value);
            total += static_cast<std::uint32_t>(value);
        }
    }
    const std::uint32_t area = static_cast<std::uint32_t>(((z1 + 1) - z0) * ((x1 + 1) - x0));
    const float average = static_cast<float>(static_cast<std::uint64_t>(total) / area) / 32767.0f;
    return RoundToEven(propRequestCurve_.GetOutputValue(average));
}

bool cSC4PollutionSimulator::AddOrRemoveOccupant(std::list<IPollutionOccupant*>& list, IPollutionOccupant* occupant,
                                                 bool add) {
    if (add) {
        list.push_back(occupant);
        return true;
    }
    auto it = std::find(list.begin(), list.end(), occupant);
    if (it == list.end()) return false;
    list.erase(it);
    return true;
}

// ------------------------------------------------------------------------------------- occupant helpers

void cSC4PollutionSimulator::GetRecyclingCenterMaxCapacity(IPollutionOccupant* occupant, float& percentage) const {
    percentage = 0.0f;
    SC4Percentage p;
    if (cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccRecyclingMax, p)) percentage = p.value;
}

void cSC4PollutionSimulator::GetRecyclingCenterCurrentCapacity(IPollutionOccupant* occupant, float& percentage) const {
    std::uint32_t value = 0;
    if (GetCurrentCapacity(kPollutionOccRecyclingMax, occupant, value)) percentage = static_cast<float>(value) * 0.01f;
    else percentage = 0.0f;
}

void cSC4PollutionSimulator::GetMaxCapacity(std::uint32_t property, IPollutionOccupant* occupant,
                                            std::uint32_t& out) const {
    out = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), property, out);
}

// capacity * efficiency (zero when the occupant has no power), then scaled by the age curve of the occupant
bool cSC4PollutionSimulator::GetCurrentCapacity(std::uint32_t property, IPollutionOccupant* occupant,
                                                std::uint32_t& out) const {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    if (!cSCPropertyHelp::GetPropertyValue(holder, property, out)) {
        out = 0;
        return false;
    }
    if (!host_->OccupantPowered(occupant)) out = 0;
    out = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(out) * efficiency_));
    if (holder->HasProperty(kPollutionOccAgeCurve)) {
        cSC4ResponseCurve curve;
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAgeCurve, values))
            curve.SetResponseCurveData(values.data(), values.size());
        float age = 0.0f;
        cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age);
        out = static_cast<std::uint32_t>(RoundToEven(curve.GetOutputValue(age) * static_cast<float>(out)));
    }
    return true;
}

void cSC4PollutionSimulator::GetIncineratorCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetCurrentCapacity(kPollutionOccIncinerator, occupant, out);
}

void cSC4PollutionSimulator::GetWasteToEnergyCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetCurrentCapacity(kPollutionOccWasteToEnergy, occupant, out);
}

void cSC4PollutionSimulator::GetWaterTreatmentMaxCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetMaxCapacity(kPollutionOccWaterTreatment, occupant, out);
}

void cSC4PollutionSimulator::GetWaterTreatmentCurrentCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetCurrentCapacity(kPollutionOccWaterTreatment, occupant, out);
}

void cSC4PollutionSimulator::GetWaterTreatmentUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    GetMaxCapacity(kPollutionOccWaterTreatmentUsed, occupant, out);
}

// 100 % scaled by the age curve; fails (leaving 100 %) when the occupant has no age curve
bool cSC4PollutionSimulator::GetCurrentOccupantCondition(IPollutionOccupant* occupant, float& percentage) const {
    percentage = 1.0f;                                                  // kSC4PercentageOneHundred
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    if (!holder->HasProperty(kPollutionOccAgeCurve)) return false;
    cSC4ResponseCurve curve;
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAgeCurve, values))
        curve.SetResponseCurveData(values.data(), values.size());
    float age = 0.0f;
    cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age);
    percentage = curve.GetOutputValue(age) * percentage;
    return true;
}

bool cSC4PollutionSimulator::GetCurrentOccupantEfficiency(IPollutionOccupant* occupant, float& percentage) const {
    if (host_->PowerAvailable()) {
        percentage = efficiency_;
        float condition = 0.0f;
        if (GetCurrentOccupantCondition(occupant, condition)) {
            percentage = condition * percentage;
            return true;
        }
    }
    percentage = 0.0f;
    return false;
}

bool cSC4PollutionSimulator::GetUnusedCapacity(std::uint32_t maxProperty, std::uint32_t usedProperty,
                                               IPollutionOccupant* occupant, std::uint32_t& out) const {
    std::uint32_t maximum = 0;
    if (!GetCurrentCapacity(maxProperty, occupant, maximum)) return false;
    std::uint32_t used = 0;
    if (!cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), usedProperty, used)) return false;
    out = maximum < used ? 0 : maximum - used;
    return true;
}

void cSC4PollutionSimulator::GetIncineratorUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccIncineratorUsed, out);
}

void cSC4PollutionSimulator::GetWasteToEnergyUsedCapacity(IPollutionOccupant* occupant, std::uint32_t& out) const {
    out = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccWasteToEnergyUsed, out);
}

bool cSC4PollutionSimulator::CalcOccupantMaintenanceCost(IPollutionOccupant* occupant, std::int64_t& out) const {
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    out = 0;
    float age = 0.0f;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccMaintenanceBase, out)) return false;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age)) return false;
    if (!holder->HasProperty(kPollutionOccMaintenanceCurve)) return false;
    cSC4ResponseCurve curve;
    std::vector<float> values;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccMaintenanceCurve, values))
        curve.SetResponseCurveData(values.data(), values.size());
    out = static_cast<std::int64_t>(curve.GetOutputValue(age) * static_cast<float>(out));
    return true;
}

// Copies each occupant's "capacity used" counter into the saved property and zeroes the counter.
void cSC4PollutionSimulator::ResetOccupantListCapacityUsedProperty(std::list<IPollutionOccupant*>& list,
                                                                    std::uint32_t currentProperty,
                                                                    std::uint32_t savedProperty) const {
    for (IPollutionOccupant* occupant : list) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::uint32_t current = 0;
        if (!holder->HasProperty(currentProperty)) continue;
        cSCPropertyHelp::GetPropertyValue(holder, currentProperty, current);
        if (holder->HasProperty(savedProperty)) cSCPropertyHelp::SetPropertyValue(holder, savedProperty, current);
        cSCPropertyHelp::SetPropertyValue(holder, currentProperty, std::uint32_t{0});
    }
}

void cSC4PollutionSimulator::ComputeWasteToEnergyListCurrentCapacity() {
    for (IPollutionOccupant* occupant : wasteToEnergy_) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::uint32_t rate = 0, capacity = 0, used = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergyRate, rate) &&
            cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergy, capacity) &&
            cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergyUsed, used) && capacity != 0) {
            cSCPropertyHelp::SetPropertyValue(
                holder, kPollutionOccWasteToEnergyProduced,
                static_cast<std::uint32_t>(static_cast<std::uint64_t>(used) * rate / capacity));
        }
    }
}

// Recycling centres sorted by efficiency (best first); the best centres worth `slots` units of capacity decide
// the city-wide effect: sum(factor * efficiency) / max(slots, sum(factor)).
void cSC4PollutionSimulator::CalculateRecyclingCenterEffect() {
    const int slots = static_cast<int>(host_->RecyclingCapacitySlots());
    std::list<IPollutionOccupant*> sorted;
    if (recyclingCenterCountSnapshot == 0) {
        recyclingEffect_ = 0.0f;
        return;
    }
    for (IPollutionOccupant* occupant : recyclingCenters_) {
        float efficiency = 0.0f;
        if (!GetCurrentOccupantEfficiency(occupant, efficiency)) continue;
        auto position = sorted.begin();
        for (; position != sorted.end(); ++position) {
            float other = 0.0f;
            if (GetCurrentOccupantEfficiency(*position, other) && other <= efficiency) break;
        }
        sorted.insert(position, occupant);
    }
    recyclingCenters_ = sorted;
    float factorSum = 0.0f, weighted = 0.0f;
    if (slots > 0 && !recyclingCenters_.empty()) {
        const float limit = static_cast<float>(slots);
        for (IPollutionOccupant* occupant : recyclingCenters_) {
            float factor = 0.0f;
            if (cSCPropertyHelp::GetPropertyValue(occupant->PropertyHolder(), kPollutionOccRecyclingFactor, factor)) {
                factorSum += factor;
                float efficiency = 0.0f;
                if (GetCurrentOccupantEfficiency(occupant, efficiency)) weighted += factor * efficiency;
            }
            if (!(factorSum < limit)) break;
        }
        if (0.0f < factorSum) {
            recyclingEffect_ = weighted / std::max(limit, factorSum);
            return;
        }
    }
    recyclingEffect_ = 0.0f;
}

// Disposal priority: free recycling allowance, then incinerators, then waste-to-energy, then the landfill
// transport (limited by the landfill free space and the transport curve), then alien abduction.
float cSC4PollutionSimulator::eatGarbage(float amount, std::uint32_t& wasteToEnergyBudget,
                                         std::uint32_t& incineratorBudget) {
    if (amount <= 0.0f) return 0.0f;
    float eaten = 0.0f;
    const int recyclable = recyclingAllowance_ - static_cast<int>(recycledThisMonth_);
    if (recyclable > 0) {
        const float room = static_cast<float>(recyclable);
        const float taken = room < amount ? room : amount;
        recycledThisMonth_ = static_cast<std::int64_t>(static_cast<float>(recycledThisMonth_) + taken);
        eaten += taken;
        if (amount <= room) return eaten;
        amount -= room;
    }
    auto burn = [&](std::uint32_t& budget, std::int64_t& counter) {
        if (!(0.0f < amount) || budget == 0) return;
        float taken;
        if (static_cast<float>(budget) <= amount) {
            // original quirk: a budget no larger than the request is emptied without counting any garbage eaten
            taken = 0.0f;
            budget = 0;
        } else {
            budget -= static_cast<std::uint32_t>(RoundToEven(amount));
            taken = amount;
            amount = 0.0f;
        }
        counter = static_cast<std::int64_t>(static_cast<float>(counter) + taken);
        eaten += taken;
    };
    burn(incineratorBudget, incineratedThisMonth_);
    burn(wasteToEnergyBudget, wasteToEnergyThisMonth_);
    if (0.0f < amount) {
        int low = 0, high = 0;
        host_->LandfillRange(low, high);
        const int free = high - low;
        const float transport = garbageTransportCapacity_.GetOutputValue(static_cast<float>(host_->LandfillTransportLoad()));
        int room = RoundToEven(transport * efficiency_) - garbageSupply_;
        if (free < room) room = free;
        if (0 < room) {
            float moved;
            if (amount <= static_cast<float>(room)) {
                AddToGarbageSupply(RoundToEven(amount));
                moved = amount;
                amount = 0.0f;
            } else {
                moved = static_cast<float>(room);
                AddToGarbageSupply(room);
                amount -= moved;
            }
            eaten += moved;
        }
    }
    if (0.0f < amount) {
        const int allowance = static_cast<int>(tunables_.abductionAmount) - static_cast<int>(abductedThisMonth_);
        if (0 < allowance) {
            const float room = static_cast<float>(allowance);
            const float taken = amount <= room ? amount : room;
            abductedThisMonth_ = static_cast<std::uint32_t>(static_cast<std::int64_t>(static_cast<float>(abductedThisMonth_) + taken));
            eaten += taken;
        }
    }
    return eaten;
}

// Averages the period's sums into the published values, records the history, tells the UI which grids changed
// and refreshes the sanitation efficiency from the department funding.
void cSC4PollutionSimulator::EndOfCycle() {
    CalculateRecyclingCenterEffect();
    garbageUncollected_ = totalGarbageUncollected_ != 0;
    std::uint32_t radiationPercent = 0;
    std::uint64_t avgAir = 0, avgWater = 0;
    std::uint16_t avgGarbage = 0;
    if (cellCount != 0) {
        avgAir = airSum / cellCount;
        avgWater = waterSum / cellCount;
        avgGarbage = static_cast<std::uint16_t>(totalGarbageUncollected_ / cellCount);
        radiationPercent = (radiationCount * 100u) / cellCount;
    }
    averageAir_ = static_cast<std::int32_t>(avgAir);
    averageWater_ = static_cast<std::int32_t>(avgWater);
    averageGarbage_ = static_cast<std::int16_t>(avgGarbage);
    periodValue224[0] = periodValue21c[0];
    periodValue224[1] = periodValue21c[1];
    host_->AddHistory(kHistoryAir, static_cast<std::int64_t>(avgAir));
    host_->AddHistory(kHistoryWater, averageWater_);
    host_->AddHistory(kHistoryGarbage, static_cast<std::uint16_t>(averageGarbage_));
    host_->AddHistory(kHistoryRadiation, radiationCount);
    host_->AddHistory(kHistoryTotal, static_cast<std::int64_t>(radiationPercent) + averageAir_);
    cellCount = 0;
    radiationCount = 0;
    periodValue21c[0] = periodValue21c[1] = 0;
    airSum = waterSum = 0;
    if (dirtyRadiation) host_->PostMessage(kMessageRadiationChanged, 0);
    if (dirtyAir) host_->PostMessage(kMessageAirChanged, 0);
    if (dirtyWater) host_->PostMessage(kMessageWaterChanged, 0);
    if (dirtyGarbage) host_->PostMessage(kMessageGarbageChanged, 0);
    dirtyRadiation = dirtyAir = dirtyWater = dirtyGarbage = false;
    efficiency_ = fundingEfficiencyCurve_.GetOutputValue(host_->SanitationFunding() * 100.0f) * 0.01f;
    host_->PostMessage(kMessageSanitationEfficiency, 8);
}

// ---------------------------------------------------------------------------- occupant list passes

void cSC4PollutionSimulator::AgeOccupantList(std::list<IPollutionOccupant*>& list, std::uint32_t usedProperty,
                                             std::uint32_t capacityProperty) {
    for (IPollutionOccupant* occupant : list) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        float age = 0.0f, wear = 0.0f;
        std::uint32_t capacity = 0;
        std::int32_t used = 0;
        float warnAt = 99.0f, abandonAt = 99.0f;
        const float funding = host_->SanitationFunding();
        float load;
        if (usedProperty == 0) {
            load = 0.0f;
            if (recyclingCenterCountSnapshot != 0) load = static_cast<float>(recyclingShare268_) / static_cast<float>(recyclingCenterCountSnapshot);
        } else {
            cSCPropertyHelp::GetPropertyValue(holder, usedProperty, used);
            GetCurrentCapacity(capacityProperty, occupant, capacity);
            load = 0.0f;
            if (capacity != 0 && 0 < used) load = static_cast<float>(used) / static_cast<float>(capacity);
        }
        cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccAge, age);
        cSCPropertyHelp::GetPropertyValue(holder, 0xea1cf220, wear);
        cSCPropertyHelp::GetPropertyValue(holder, 0xea1cf221, warnAt);
        cSCPropertyHelp::GetPropertyValue(holder, 0xea1cf222, abandonAt);
        float step = wear;
        step = ageCurve_.GetOutputValue(funding) * step;
        age = efficiencyCurve_.GetOutputValue(load) * step + age;
        cSCPropertyHelp::SetPropertyValue(holder, kPollutionOccAge, age);
        if (abandonAt <= age || warnAt <= age) {
            if (!services_) continue;
            float position[3];
            occupant->Position(position);
            IPollutionLot* lot = services_->LotAtPosition(position);
            if (!lot) continue;
            if (abandonAt <= age) lot->SetState(4);
            else lot->SetState(2);
        }
    }
}

void cSC4PollutionSimulator::SetOccupantListUsedCapacity(std::list<IPollutionOccupant*>& list, std::uint32_t capacityProperty,
                                                         std::uint32_t usedProperty, std::uint32_t total,
                                                         std::uint32_t& remaining, std::uint32_t count) {
    if (total == 0 || count == 0 || remaining == 0) return;
    auto it = list.begin();
    std::uint32_t budget = remaining;
    if (total < budget) budget = total;
    std::uint64_t share = static_cast<std::uint64_t>(budget) / count;
    do {
        int portion = static_cast<int>(share);
        --count;
        if (it == list.end()) return;
        IPollutionOccupant* occupant = *it;
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::uint32_t capacity = 0;
        if (GetCurrentCapacity(capacityProperty, occupant, capacity) && holder->HasProperty(usedProperty)) {
            std::uint32_t used = 0;
            cSCPropertyHelp::GetPropertyValue(holder, usedProperty, used);
            int room = static_cast<int>(capacity - used);
            if (capacity < used || capacity - used == 0) room = 0;
            if (room < portion) {
                portion = room;
                if (count != 0) share = static_cast<std::uint64_t>(remaining) / count;     // re-spread what is left
            }
            remaining -= static_cast<std::uint32_t>(portion);
            cSCPropertyHelp::SetPropertyValue(holder, usedProperty, static_cast<std::uint32_t>(used + portion));
        }
        ++it;
    } while (remaining != 0);
}

void cSC4PollutionSimulator::CalcMaintenanceCosts(std::list<IPollutionOccupant*>& list) {
    if (!services_) return;
    for (IPollutionOccupant* occupant : list) {
        cISCPropertyHolder* holder = occupant->PropertyHolder();
        std::int64_t cost = 0;
        if (!CalcOccupantMaintenanceCost(occupant, cost)) continue;
        PollutionBudgetRecord record;
        record.zero = 0;
        if (!services_->FindBudgetLineItem(holder, 0xca58edc7, record) || cost == record.cost) continue;
        if (IPollutionDepartment* department = services_->DepartmentFor(record)) {
            department->AddCost(cost - record.cost);
            services_->SetBudgetLineItem(holder, record.id, cost);
        }
    }
}

void cSC4PollutionSimulator::AddRadiationToCity(const float position[3], float amount, float radius) {
    if ((amount != 0.0f || std::isnan(amount)) && 0.0f < radius) {
        cSC4SimGrid<std::int16_t>* grid = valueGrids_[kPollutionRadiation].get();
        std::int32_t tractX = 0, tractZ = 0;
        grid->PositionToTract(position[0], position[2], tractX, tractZ);
        AddRadiusEffect(grid->GetGridData(), grid->GetTractCountX(), grid->GetTractCountZ(), tractX, tractZ, radius, amount, 0.0f);
        if (visualEffectUpdate_) visualEffectUpdate_();
        host_->PostMessage(0xda6dd35d, 0);
    }
}

// ---------------------------------------------------------------------------- simulation

// @ 0x0006f836. StaticInit's curves are members here and the update period a constant. A loaded city (Read set this[0x11]) keeps its
// grids, sweep position and counters; a new one starts them. Every time: the tunables, the daily and monthly agents, the notifications
// and the total-pollution history length. PARTIAL: the per-8x8-block visual-effect slots (UpdatePollutionVisualEffect), the prop
// developer's requester registration and the sanitation department's line items 10 / 20 / 30 are not set up (no native counterparts).
bool cSC4PollutionSimulator::Init() {
    if (!loaded_) {
        loaded_ = true;
        CreateGrids();
        averageAir_ = averageWater_ = 0;
        averageGarbage_ = 0;
        airSum = waterSum = 0;
        periodValue21c[0] = periodValue21c[1] = 0;
        periodValue224[0] = periodValue224[1] = 0;
        cellCount = radiationCount = 0;
        totalGarbageUncollected_ = 0;
        monthlyIncineratorCapacity_ = monthlyWasteToEnergyCapacity_ = 0;
        recyclingEffect_ = 0.0f;
        abductedThisMonth_ = 0;
        abductedPrev_ = 0;
        recyclingAllowance_ = 0;
        garbageSupply_ = garbageCapacity_ = 0;
        producedPrev_ = importedPrev_ = exportedPrev_ = 0;
        recycledPrev_ = 0;
        incineratedPrev_ = convertedPrev_ = landfilledPrev_ = 0;
        activeLandfillCount_ = 0;
        recyclingCenterCountSnapshot = static_cast<std::uint32_t>(recyclingCenters_.size());
        const int countX = valueGrids_[0]->GetTractCountX(), countZ = valueGrids_[0]->GetTractCountZ();
        cellsPerDay_ = static_cast<std::uint32_t>((countX * countZ) / static_cast<int>(kUpdatePeriod >> 1));
        walk_.SetModular(false);
        walk_.SetRect(0, 0, countX - 1 < 0 ? 0 : countX - 1, countZ - 1);
        walk_.Reset(true);
        int a = 0, b = 0;
        walk_.Next(a, b);
        cellX_ = static_cast<std::uint32_t>(a * 2);
        cellZ_ = static_cast<std::uint32_t>(b * 2);
        dirtyRadiation = dirtyAir = dirtyWater = dirtyGarbage = false;
    }
    if (!running_) {
        running_ = true;
        ReadSimulatorTunableValues(host_->SimulatorExemplar());
        static const char* const kName = "cSC4PollutionSimulator";
        host_->AddAgent(this, 6, kName, 2);
        host_->AddAgent(this, 4, kName, 2);
        for (std::uint32_t message : kNotifications) host_->AddNotification(this, message);
        host_->CreateHistory(kHistoryTotal, 2);
    }
    return true;
}

bool cSC4PollutionSimulator::Shutdown() {
    if (running_) {
        running_ = false;
        host_->RemoveAgent(this);
        for (std::uint32_t message : kNotifications) host_->RemoveNotification(this, message);
    }
    return true;
}

// @ 0x0006e7d4
bool cSC4PollutionSimulator::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    switch (message->GetType()) {
    case kMessageMonthly: DoMonthlySimulation(); return true;
    case kMessageSimulationBegin: SimulationBegin(); return true;
    case kMessageDaily: DoDailySimulation(); return true;
    case kMessageTunablesChanged: ReadSimulatorTunableValues(host_->SimulatorExemplar()); return true;
    case kMessageOccupantInserted:
    case kMessageOccupantRemoved:
        DoOccupantChanged(standard->GetVoid1(), message->GetType() == kMessageOccupantInserted);
        return true;
    case kMessageViewLayer: {
        const auto* layer = static_cast<const std::int32_t*>(standard->GetVoid2());
        const bool shown = layer && *layer == 1;
        if (shown == effectsShown_) return true;
        effectsShown_ = shown;
        break;
    }
    case kMessageCityStarted: break;
    default: return true;
    }
    if (visualEffectUpdate_) visualEffectUpdate_();   // UpdatePollutionVisualEffect
    return true;
}

// @ 0x0006abf2: the sanitation efficiency from the department's funding (as EndOfCycle refreshes it).
void cSC4PollutionSimulator::SimulationBegin() {
    efficiency_ = fundingEfficiencyCurve_.GetOutputValue(host_->SanitationFunding() * 100.0f) * 0.01f;
}

// @ 0x0006c4f4. One day of the sweep: the plants' free capacity, the neighbour garbage deals, then `cellsPerDay_` 2x2-cell blocks in
// the walk's random order. Each block turns its building sources (scaled by the ordinances and the zones' factors) and the traffic into
// air, water and garbage: garbage is disposed of (export, plants, landfill, abduction) or left on the cells, water is treated by the
// plants' capacity, and the published air / water values move towards the targets by at most the tunable deltas. A finished walk ends
// the cycle (EndOfCycle: averages + history).
void cSC4PollutionSimulator::DoDailySimulation() {
    std::uint32_t waterCapacity = 0, waterPlants = 0;
    for (IPollutionOccupant* o : waterTreatment_) {
        std::uint32_t unused = 0;
        if (GetUnusedCapacity(kPollutionOccWaterTreatment, kPollutionOccWaterTreatmentUsed, o, unused)) waterCapacity += unused;
        ++waterPlants;
    }
    monthlyIncineratorCapacity_ = 0;
    std::uint32_t incineratorCapacity = 0, incinerators = 0;
    for (IPollutionOccupant* o : incinerators_) {
        std::uint32_t capacity = 0, unused = 0;
        if (GetCurrentCapacity(kPollutionOccIncinerator, o, capacity)) monthlyIncineratorCapacity_ += capacity;
        if (GetUnusedCapacity(kPollutionOccIncinerator, kPollutionOccIncineratorUsed, o, unused)) incineratorCapacity += unused;
        ++incinerators;
    }
    std::uint32_t incineratorLeft = incineratorCapacity;
    monthlyWasteToEnergyCapacity_ = 0;
    std::uint32_t wteCapacity = 0, wtePlants = 0;
    for (IPollutionOccupant* o : wasteToEnergy_) {
        std::uint32_t capacity = 0, unused = 0;
        if (GetCurrentCapacity(kPollutionOccWasteToEnergy, o, capacity)) monthlyWasteToEnergyCapacity_ += capacity;
        if (GetUnusedCapacity(kPollutionOccWasteToEnergy, kPollutionOccWasteToEnergyUsed, o, unused)) wteCapacity += unused;
        ++wtePlants;
    }
    std::uint32_t wteLeft = wteCapacity;

    // garbage the neighbours send (a tenth of each deal a day), then what they can take
    std::vector<IPollutionDeal*> deals;
    host_->GarbageDeals(kGarbageImportDeals, deals);
    int imported = 0;
    for (IPollutionDeal* deal : deals) {
        if (!(0.0f < static_cast<float>(deal->Amount() - deal->Used()))) continue;
        const int share = RoundToEven(static_cast<float>(deal->Amount()) * kDealShare);
        imported += share;
        deal->SetUsed(share + deal->Used());
        importedThisMonth_ += share;
    }
    deals.clear();
    host_->GarbageDeals(kGarbageExportDeals, deals);
    for (IPollutionDeal* deal : deals) {
        const int room = deal->Amount() - deal->Used();
        if (room <= 0) continue;
        int share = RoundToEven(static_cast<float>(deal->Amount()) * kDealShare);
        if (room < share) share = room;
        deal->SetUsed(share + deal->Used());
        recyclingAllowance_ += share;
    }
    const float importedEaten = eatGarbage(static_cast<float>(imported), incineratorLeft, wteLeft);
    AddToGarbageSupply(static_cast<int>(static_cast<float>(imported) - importedEaten));

    const float trafficFactor = host_->OrdinanceFactor(0x8a67e378, 0);
    const float airFactor = host_->OrdinanceFactor(0x08f79b8e, 0);
    const float waterFactor = host_->OrdinanceFactor(0xe8f79c8b, 0);
    const float garbageFactor = host_->OrdinanceFactor(0xe8f79c90, 0);
    float zoneFactors[16][3];
    for (std::uint32_t zone = 0; zone < 16; ++zone) {
        zoneFactors[zone][0] = host_->OrdinanceFactor(0x8a67e373, zone);
        zoneFactors[zone][1] = host_->OrdinanceFactor(0x8a67e374, zone);
        zoneFactors[zone][2] = host_->OrdinanceFactor(0x8a67e376, zone);
    }
    constexpr int kLandfillZone = 0xe;
    std::uint32_t waterLeft = waterCapacity;
    for (std::uint32_t step = 0; step < cellsPerDay_; ++step) {
        const int cx = static_cast<int>(cellX_), cz = static_cast<int>(cellZ_);
        const std::int16_t sourceAir = std::max<std::int16_t>(0, valueGrids_[0]->GetCellValue(cx, cz));
        const std::int16_t sourceWater = std::max<std::int16_t>(0, valueGrids_[1]->GetCellValue(cx, cz));
        const std::int16_t sourceGarbage = std::max<std::int16_t>(0, valueGrids_[2]->GetCellValue(cx, cz));
        const std::int16_t sourceRadiation = std::max<std::int16_t>(0, valueGrids_[3]->GetCellValue(cx, cz));
        std::int16_t air = static_cast<std::int16_t>(static_cast<int>(static_cast<float>(sourceAir) * airFactor));
        std::int16_t water = static_cast<std::int16_t>(static_cast<int>(static_cast<float>(sourceWater) * waterFactor));
        std::int16_t garbage = static_cast<std::int16_t>(static_cast<int>(static_cast<float>(sourceGarbage) * garbageFactor));
        int counted = 0;
        bool developed = false;
        float sumAir = 0.0f, sumWater = 0.0f, sumGarbage = 0.0f;
        for (int i = 0; i < 4; ++i) {
            const int zone = host_->ZoneTypeAt((i & 1) + cx, (i >> 1) + cz);
            if (zone == kLandfillZone) continue;
            const int z = zone & 0xf;
            sumAir += zoneFactors[z][0];
            sumWater += zoneFactors[z][1];
            sumGarbage += zoneFactors[z][2];
            ++counted;
            if (zone != 0) developed = true;
        }
        if (developed) ++cellCount;
        if (0 < counted) {
            const float n = static_cast<float>(counted);
            air = static_cast<std::int16_t>(static_cast<int>((sumAir / n) * static_cast<float>(air)));
            water = static_cast<std::int16_t>(static_cast<int>((sumWater / n) * static_cast<float>(water)));
            garbage = static_cast<std::int16_t>(static_cast<int>((sumGarbage / n) * static_cast<float>(garbage)));
        }
        std::uint32_t waterTarget = static_cast<std::uint32_t>(static_cast<int>(water));
        int airTarget = air;
        for (int i = 0; i < 4; ++i) {
            const int x = (i & 1) + cx, z = (i >> 1) + cz;
            const int zone = host_->ZoneTypeAt(x, z);
            int existing = 0;
            GetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), existing);
            const std::uint8_t traffic = host_->TrafficAirAt(x, z);
            const int airNext = static_cast<int>(static_cast<float>(airTarget) +
                                                 static_cast<float>(traffic) * tunables_.trafficAirFactor * trafficFactor);
            if (zone != kLandfillZone) {
                float produced = static_cast<float>(garbage) / static_cast<float>(counted);
                const int producedRounded = RoundToEven(produced);
                producedThisMonth_ += producedRounded;
                if (0.0f < recyclingEffect_ && 0.0f < produced) {
                    const float recycled = std::min(produced * recyclingEffect_, produced);
                    recyclingShare268_ += recycled;
                    produced -= recycled;
                }
                const float onCell = static_cast<float>(existing);
                SetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), 0);
                if (static_cast<std::uint32_t>(existing) <= totalGarbageUncollected_) totalGarbageUncollected_ -= static_cast<std::uint32_t>(existing);
                float left = (produced + onCell) - eatGarbage(produced + onCell, incineratorLeft, wteLeft);
                if (0.0f < left) {
                    if (left <= kMaxCellGarbage) {
                        const int valve = garbageCapacity_ - garbageSupply_;
                        if (-1 < valve || kMaxCellGarbage <= left) {
                            SetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), static_cast<int>(left));
                            totalGarbageUncollected_ = ToUint(left + static_cast<float>(totalGarbageUncollected_));
                        } else {
                            // the landfills are over capacity: the cell takes some of their garbage back
                            int pull = -valve;
                            float pulled;
                            if (pull <= producedRounded) pulled = static_cast<float>(pull);
                            else if (garbage != 0) { pull = producedRounded; pulled = static_cast<float>(pull); }
                            else { pull = 1; pulled = 1.0f; }
                            if (kMaxCellGarbage < pulled + left) pull = static_cast<int>(kMaxCellGarbage - left);
                            AddToGarbageSupply(-pull);
                            SetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), static_cast<int>(static_cast<float>(pull) + left));
                            totalGarbageUncollected_ = ToUint(static_cast<float>(pull) + left + static_cast<float>(totalGarbageUncollected_));
                        }
                    } else {
                        left -= kMaxCellGarbage;
                        SetGarbageValue(static_cast<unsigned>(x), static_cast<unsigned>(z), 0x7fff);
                        totalGarbageUncollected_ += 0x7fff;
                        if (0.0f < left) AddToGarbageSupply(static_cast<int>(left));
                    }
                }
            }
            airTarget = airNext;
        }
        periodValue21c[0] += waterTarget;
        if (waterLeft != 0 && 0 < static_cast<int>(waterTarget)) {
            // the treatment plants clean what they can, in proportion to last cycle's treated share
            std::uint32_t limit = waterLeft, treated;
            const auto lastTotal = static_cast<int>(periodValue224[0]), lastTreated = static_cast<int>(periodValue224[1]);
            if (lastTotal < 1 || lastTreated < 1 || periodValue224[1] < waterLeft) {
                treated = static_cast<int>(limit) <= static_cast<int>(waterTarget) ? limit : waterTarget;
            } else {
                limit = static_cast<std::uint32_t>(RoundToEven((static_cast<float>(lastTreated) / static_cast<float>(lastTotal)) * static_cast<float>(static_cast<int>(waterTarget))));
                if (limit != 0) treated = static_cast<int>(limit) <= static_cast<int>(waterTarget) ? limit : waterTarget;
                else treated = 1;
            }
            if (waterLeft < treated) treated = waterLeft;
            periodValue21c[1] += treated;
            waterTarget -= treated;
            waterLeft -= treated;
        }
        waterSum += waterTarget;
        airSum += static_cast<std::uint32_t>(airTarget);
        radiationCount += static_cast<std::uint32_t>(sourceRadiation);
        const int airClamped = airTarget < 0 ? 0 : std::min(airTarget, 0x7fff);
        const int waterClamped = static_cast<int>(waterTarget) < 0 ? 0 : std::min(static_cast<int>(waterTarget), 0x7fff);
        int current = 0;
        GetAirValue(cellX_, cellZ_, current);
        if (current < airClamped) SetAirValue(cellX_, cellZ_, current + static_cast<int>(std::min<std::uint32_t>(airClamped - current, tunables_.maxAirDelta)));
        else if (airClamped < current) SetAirValue(cellX_, cellZ_, current - static_cast<int>(std::min<std::uint32_t>(current - airClamped, tunables_.maxAirDelta)));
        GetWaterValue(cellX_, cellZ_, current);
        if (current < waterClamped) {
            SetWaterValue(cellX_, cellZ_, current + static_cast<int>(std::min<std::uint32_t>(waterClamped - current, tunables_.maxWaterDelta)));
        } else if (waterClamped < current) {
            int next = current - static_cast<int>(std::min<std::uint32_t>(current - waterClamped, tunables_.maxWaterDelta));
            if (next == 0) {
                // a cell only dries out once no neighbour holds water (the original bounds the +1 neighbours by the tract counts)
                int n = 0;
                bool dry = true;
                if (cx != 0) { GetWaterValue(cellX_ - 1, cellZ_, n); dry = n < 2; }
                if (cz != 0 && dry) { GetWaterValue(cellX_, cellZ_ - 1, n); dry = n < 2; }
                if (cellX_ < static_cast<std::uint32_t>(valueGrids_[0]->GetTractCountX()) - 1u && dry) { GetWaterValue(cellX_ + 1, cellZ_, n); dry = n < 2; }
                if (cellZ_ < static_cast<std::uint32_t>(valueGrids_[0]->GetTractCountZ()) - 1u && dry) { GetWaterValue(cellX_, cellZ_ + 1, n); dry = n < 2; }
                next = dry ? 0 : 1;
            }
            SetWaterValue(cellX_, cellZ_, next);
        }
        if (walk_.IsDone()) {
            walk_.Reset(true);
            EndOfCycle();
        }
        int a = 0, b = 0;
        walk_.Next(a, b);
        cellX_ = static_cast<std::uint32_t>(a * 2);
        cellZ_ = static_cast<std::uint32_t>(b * 2);
    }
    if (garbageCapacity_ - garbageSupply_ < 0) {
        const int over = -(garbageCapacity_ - garbageSupply_);
        int eaten = static_cast<int>(eatGarbage(static_cast<float>(over), incineratorLeft, wteLeft));
        const std::uint32_t left = static_cast<std::uint32_t>(over - eaten);
        if (tunables_.maxGarbageOnValve < left) eaten += static_cast<int>(left - tunables_.maxGarbageOnValve);
        AddToGarbageSupply(-eaten);
    }
    std::uint32_t used = waterCapacity - waterLeft;
    if (used != 0) SetOccupantListUsedCapacity(waterTreatment_, kPollutionOccWaterTreatment, kPollutionOccWaterTreatmentUsed, waterCapacity, used, waterPlants);
    used = incineratorCapacity - incineratorLeft;
    if (used != 0) SetOccupantListUsedCapacity(incinerators_, kPollutionOccIncinerator, kPollutionOccIncineratorUsed, incineratorCapacity, used, incinerators);
    used = wteCapacity - wteLeft;
    if (used != 0) SetOccupantListUsedCapacity(wasteToEnergy_, kPollutionOccWasteToEnergy, kPollutionOccWasteToEnergyUsed, wteCapacity, used, wtePlants);
    CalcMaintenanceCosts(waterTreatment_);
    CalcMaintenanceCosts(incinerators_);
    CalcMaintenanceCosts(recyclingCenters_);
}

// @ 0x0006e32e. Closes the month: the neighbour deals restart, the sanitation line item is charged for the garbage that left the city,
// the month's garbage counters become last month's and go to the history, the plants age and their used capacity resets.
void cSC4PollutionSimulator::DoMonthlySimulation() {
    std::vector<IPollutionDeal*> deals;
    host_->GarbageDeals(kGarbageImportDeals, deals);
    for (IPollutionDeal* deal : deals) deal->SetUsed(0);
    deals.clear();
    host_->GarbageDeals(kGarbageExportDeals, deals);
    std::int64_t exported = 0;
    for (IPollutionDeal* deal : deals) {
        deal->SetUsed(0);
        exported += deal->Amount();
    }
    sanitationCost_ = static_cast<std::int64_t>(static_cast<float>(exported + incineratedThisMonth_ + wasteToEnergyThisMonth_ + landfilledThisMonth_) *
                                                tunables_.value2e0);
    host_->SetSanitationLineItemCost(20, sanitationCost_);
    abductedPrev_ = static_cast<std::int32_t>(abductedThisMonth_);
    producedPrev_ = producedThisMonth_;
    importedPrev_ = importedThisMonth_;
    exportedPrev_ = recycledThisMonth_;           // +0x260 (i386 +0x160): the garbage eatGarbage sent to the neighbours
    recycledPrev_ = recyclingShare268_;
    incineratedPrev_ = incineratedThisMonth_;
    convertedPrev_ = wasteToEnergyThisMonth_;
    landfilledPrev_ = landfilledThisMonth_;
    host_->AddHistory(0xea110158, static_cast<std::int32_t>(producedThisMonth_));
    host_->AddHistory(0x8ab2f258, static_cast<std::int32_t>(importedThisMonth_));
    host_->AddHistory(0x6ab2f260, static_cast<std::int32_t>(recycledThisMonth_));
    host_->AddHistory(0xea11016f, static_cast<std::int32_t>(recyclingShare268_));
    host_->AddHistory(0xea110197, static_cast<std::int32_t>(incineratedThisMonth_));
    host_->AddHistory(0xea1101a1, static_cast<std::int32_t>(wasteToEnergyThisMonth_));
    host_->AddHistory(0xea1101aa, static_cast<std::int32_t>(landfilledThisMonth_));
    producedThisMonth_ = importedThisMonth_ = recycledThisMonth_ = 0;
    recyclingShare268_ = 0;
    incineratedThisMonth_ = wasteToEnergyThisMonth_ = landfilledThisMonth_ = 0;
    abductedThisMonth_ = 0;
    recyclingAllowance_ = 0;
    AgeOccupantList(waterTreatment_, kPollutionOccWaterTreatmentUsed, kPollutionOccWaterTreatment);
    AgeOccupantList(incinerators_, kPollutionOccIncineratorUsed, kPollutionOccIncinerator);
    AgeOccupantList(recyclingCenters_, 0, 0);
    ResetOccupantListCapacityUsedProperty(waterTreatment_, kPollutionOccWaterTreatmentUsed, 0x08fcdb69);
    ResetOccupantListCapacityUsedProperty(incinerators_, kPollutionOccIncineratorUsed, 0xa8fcdba8);
    ComputeWasteToEnergyListCurrentCapacity();
    ResetOccupantListCapacityUsedProperty(wasteToEnergy_, kPollutionOccWasteToEnergyUsed, 0x28fcdbb0);
    if (visualEffectUpdate_) visualEffectUpdate_();   // UpdatePollutionVisualEffect
    // PARTIAL: spNeighborsSim vslot 0x7c (the neighbour deals' monthly settlement) is the neighbours simulator's own pass.
}

// @ 0x0006ad02. A building's pollution enters (or leaves) the source grids: its garbage on every cell it covers, then each type's
// radius effect (centre value falling to the outer value at the radius) around its tract. The plants join or leave their lists and
// start with no used capacity. PARTIAL: the maintenance line item (spBudgetSim vslots 0xcc / 0xdc / 0xe0) is not booked.
void cSC4PollutionSimulator::DoOccupantChanged(void* occupant, bool added) {
    PollutionOccupantFootprint footprint;
    if (!occupant || !host_->OccupantFootprint(occupant, footprint) || !footprint.holder) return;
    cISCPropertyHolder* holder = footprint.holder;
    bool exempt = false;
    if (cSCPropertyHelp::GetPropertyValue(holder, 0x029244db, exempt) && exempt) return;
    std::vector<std::int32_t> centre;
    if (holder->HasProperty(0x27812851) && cSCPropertyHelp::GetPropertyValue(holder, 0x27812851, centre) && centre.size() >= 4) {
        std::vector<std::int32_t> outer(4, 0);
        std::vector<float> radius(4, 0.0f);
        if (holder->HasProperty(0xaa5832f3)) cSCPropertyHelp::GetPropertyValue(holder, 0xaa5832f3, outer);
        if (holder->HasProperty(0x68ee9764)) cSCPropertyHelp::GetPropertyValue(holder, 0x68ee9764, radius);
        outer.resize(4, 0);
        radius.resize(4, 0.0f);
        const std::int16_t garbage = static_cast<std::int16_t>(added ? centre[2] : -centre[2]);
        cSC4SimGrid<std::int16_t>& garbageGrid = *valueGrids_[2];
        const int shift = garbageGrid.GetTractShift();
        for (int x = footprint.cellX0; x <= footprint.cellX1; ++x)
            for (int z = footprint.cellZ0; z <= footprint.cellZ1; ++z)
                garbageGrid.SetTractValue(x >> shift, z >> shift, static_cast<std::int16_t>(garbageGrid.GetCellValue(x, z) + garbage));
        dirtyGarbage = true;
        for (int type = 0; type < 4; ++type) {
            if (!(0.0f < radius[type])) continue;
            cSC4SimGrid<std::int16_t>& grid = *valueGrids_[type];
            int tx = 0, tz = 0;
            grid.PositionToTract(footprint.position[0], footprint.position[2], tx, tz);
            const float peak = static_cast<float>(added ? centre[type] : -centre[type]);
            const float edge = static_cast<float>(added ? outer[type] : -outer[type]);
            AddRadiusEffect(grid.GetGridData(), grid.GetTractCountX(), grid.GetTractCountZ(), tx, tz, radius[type], peak, edge);
        }
    }
    IPollutionOccupant* plant = footprint.occupant;
    if (!plant) return;
    const auto startEmpty = [&](std::uint32_t usedProperty, std::uint32_t savedProperty) {
        cSCPropertyHelp::SetPropertyValue(holder, usedProperty, std::uint32_t{0});
        cSCPropertyHelp::SetPropertyValue(holder, savedProperty, std::uint32_t{0});
    };
    if (holder->HasProperty(kPollutionOccWaterTreatment)) {
        AddOrRemoveOccupant(waterTreatment_, plant, added);
        if (added) startEmpty(kPollutionOccWaterTreatmentUsed, 0x08fcdb69);
    }
    if (holder->HasProperty(kPollutionOccIncinerator)) {
        AddOrRemoveOccupant(incinerators_, plant, added);
        if (added) startEmpty(kPollutionOccIncineratorUsed, 0xa8fcdba8);
    }
    if (holder->HasProperty(kPollutionOccWasteToEnergy)) {
        std::uint32_t capacity = 0;
        cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergy, capacity);
        AddOrRemoveOccupant(wasteToEnergy_, plant, added);
        if (added) {
            startEmpty(kPollutionOccWasteToEnergyUsed, 0x28fcdbb0);
            std::uint32_t produced = 0;
            if (cSCPropertyHelp::GetPropertyValue(holder, kPollutionOccWasteToEnergyProduced, produced))
                cSCPropertyHelp::SetPropertyValue(holder, kPollutionOccWasteToEnergyRate, produced);
            monthlyWasteToEnergyCapacity_ += capacity;
        } else {
            monthlyWasteToEnergyCapacity_ -= capacity;
        }
        if (static_cast<std::int32_t>(monthlyWasteToEnergyCapacity_) < 0) monthlyWasteToEnergyCapacity_ = 0;
    }
    if (holder->HasProperty(kPollutionOccRecyclingMax)) {
        AddOrRemoveOccupant(recyclingCenters_, plant, added);
        recyclingCenterCountSnapshot = static_cast<std::uint32_t>(recyclingCenters_.size());
    }
}

}  // namespace sc4

