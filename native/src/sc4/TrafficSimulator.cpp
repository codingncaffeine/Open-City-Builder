#include "sc4/TrafficSimulator.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>

#include <memory>
#include <vector>

#include "gz/StreamOps.h"
#include "sc4/DBSegment.h"
#include "sc4/Lot.h"
#include "sc4/SCProperty.h"
#include "sc4/PathFinder.h"

namespace sc4 {

namespace {
// 1.5 * 2^23 rounding trick of the original: float -> nearest integer
inline int RoundToInt(float v) { return int(std::lrintf(v)); }

// x86 cvttsd2si: NaN and out-of-range values give the integer indefinite value (LLONG_MIN). The neighbour
// capacity calculation divides by a city population sum that is 0 when every connected city is empty; the
// hardware then converts +inf to LLONG_MIN and the low 32 bits land in the trip capacity.
inline std::int64_t TruncateToInt64(double v) {
    if (!(v >= -9223372036854775808.0 && v < 9223372036854775808.0)) return std::numeric_limits<std::int64_t>::min();
    return std::int64_t(v);
}

// 0x100a16330: cISC4NeighborConnection types usable by each transit network type
constexpr std::uint32_t kValidNeighborConnectionTypesForTransitNetworkType[6] = {0x200, 0x170c, 0x2,
                                                                                 0x40,  0x80,   0x800};
// exemplar ids of the neighbour pool holder and of the connection that carries traffic volumes
constexpr std::uint32_t kNeighborPoolRegionX = 0x4a23576b, kNeighborPoolRegionZ = 0x4a23576c;
constexpr std::uint32_t kNeighborConnectionCost = 0xca947077;
constexpr std::uint32_t kConnectionTrafficVolumeBase = 0xaa9ea81a;   // + trip type: 9 volumes per network type
constexpr std::uint32_t kConnectionTrafficVolumeArrivedBase = 0xaa9ea81d;  // + trip type: last cycle's volumes

constexpr std::uint32_t kExemplarProperty_curveF78 = 0xca5f7821, kExemplarProperty_curveFB8 = 0x4be09ec9,
                        kExemplarProperty_curveFD8 = 0x4be09eca, kExemplarProperty_fundingCurve = 0x0bd86fd4;
}  // namespace

const std::array<std::uint8_t, 9> cSC4TrafficSimulator::kTravelModeToTransitNetwork = {0, 1, 1, 2, 1, 2, 3, 4, 5};

// ---- CellSet ------------------------------------------------------------------------------------------

bool CellSet::Insert(std::uint16_t cell) {
    if (!index_.emplace(cell, items_.size()).second) return false;
    items_.push_back(cell);
    return true;
}

bool CellSet::Erase(std::uint16_t cell) {
    const auto it = index_.find(cell);
    if (it == index_.end()) return false;
    const std::size_t slot = it->second;
    const std::uint16_t last = items_.back();
    items_[slot] = last;
    index_[last] = slot;
    items_.pop_back();
    index_.erase(cell);
    return true;
}

// ---- construction / tunables ----------------------------------------------------------------------------

cSC4TrafficSimulator::cSC4TrafficSimulator(ITrafficHost* host, cRZRandom* random) : host_(host), random_(random) {
    tunables_.travelTypeEnabled.fill(true);
    tunables_.travelTypeFlag_e95.fill(true);
    travelTypeAffectedByTraffic_.fill(false);
    for (auto& row : tunables_.inverseMaximumSpeed) row.fill(FLT_MAX);
    tunables_.tripScaleForDisplay = 0.0f;
    tunables_.tripScale = 90.0f;
}

// 0x100077980 — the class answers its own interface (0x68027f37) and cIGZUnknown at +0, the message
// target 2 (0x452294aa) at +8 and the serializable (0xe4fda3d4) at +0x10. The class id 0x6990c1aa is
// not answered (as in the original).
bool cSC4TrafficSimulator::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == 1 || iid == kGZIID_cISC4TrafficSimulator) *out = static_cast<cISC4TrafficSimulator*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x100077a10
std::uint32_t cSC4TrafficSimulator::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

// The original's DoMessage is the daily-simulation stage machine (still unrecovered); the port drives
// the same work through ITrafficHost, so no message is handled here.
bool cSC4TrafficSimulator::DoMessage(cIGZMessage2*) {
    return false;
}

// 0x10007a5f8 / 0x10007b550 — the stream format is not recovered yet; stubbed so the class can be a
// cIGZSerializable. See notes/ledger_pending/cSC4TrafficSimulator.txt.
// The record a city file carried, replayed (RecordTape.h): the port's traffic simulator keeps none of its state yet.
bool cSC4TrafficSimulator::Write(cIGZOStream& stream) const {
    if (cityRecord_.Empty()) return false;
    return cityRecord_.Replay(stream);
}
// 0x10007b550. The city file's traffic record is read completely (every field, reference and nested list in the original's order and with
// the original's version conditions, from version 0x13 up). The port's traffic simulator keeps none of this state yet (the grids, accident lists, tunnel and ferry
// tables of the original live outside the recovered class), so the values are consumed, not stored: loading a city no longer fails and
// the records that follow stay in step. Versions below 0x13 are refused, as in the original.
bool cSC4TrafficSimulator::Read(cIGZIStream& stream) {
    constexpr std::uint32_t kMaxCount = 1u << 22;
    cRecordTape& t = cityRecord_;   // kept as read and saved back (RecordTape.h)
    t.Clear();
    std::uint16_t version = 0;
    t.GetUint16(stream, version);
    if (version < 0x13) return false;
    std::int32_t pathTypes = 8, extraTypes = 1, edgeSets = 4, tunnelWidth = 7;   // d4, d0, ec, 68 of the original
    if (version >= 0x18) {
        t.GetSint32(stream, pathTypes);
        if (version > 0x24) t.GetSint32(stream, extraTypes);
        if (version >= 0x1c) { t.GetSint32(stream, edgeSets); t.GetSint32(stream, tunnelWidth); }
    }
    if (pathTypes < 0 || extraTypes < 0 || tunnelWidth < 0 || pathTypes > 1024 || extraTypes > 1024 || tunnelWidth > 1024) return false;

    std::uint8_t u8 = 0;
    std::uint16_t u16 = 0;
    std::int16_t s16 = 0;
    std::uint32_t u32 = 0;
    std::int32_t s32 = 0;
    float f32 = 0.0f;
    double f64 = 0.0;
    bool flag = false;
    const auto reference = [&](std::uint32_t) { t.Ref(stream); };
    const auto many = [&](std::uint32_t count, const auto& each) {
        if (count > kMaxCount) { count = 0; t.GetUint32(stream, u32); }   // refuse an absurd count: force the failure below
        for (std::uint32_t i = 0; i < count && !stream.GetError(); ++i) each();
    };
    const auto rangedEntry = [&]() { t.GetFloat32(stream, f32); t.GetUint32(stream, u32); t.GetSint32(stream, s32); t.GetSint32(stream, s32); t.GetFloat32(stream, f32); };

    for (int i = 0; i < 6; ++i) reference(0xc9bd939a);          // six grids
    std::uint32_t rows = 0, columns = 0;
    t.GetUint32(stream, rows);
    t.GetUint32(stream, columns);
    if (rows > kMaxCount || columns > kMaxCount || static_cast<std::uint64_t>(rows) * columns > (1u << 26)) return false;
    for (std::uint64_t i = static_cast<std::uint64_t>(rows) * columns; i != 0 && !stream.GetError(); --i) t.GetUint8(stream, u8);   // a rows x columns byte table
    t.GetUint32(stream, u32);                                       // the float grids
    many(u32, [&] { reference(0xc9bd939a); });
    reference(0xc9bd939a);
    reference(0xc9bd939a);
    t.GetUint32(stream, u32);                                       // edge densities: key, 4 x 4 + 1 + 4 x 4 shorts
    many(u32, [&] {
        t.GetUint32(stream, u32);
        // before version 0x14 the 33 shorts are stored as floats, followed by one more float the original reads and drops
        if (version < 0x14) for (int k = 0; k < 34; ++k) t.GetFloat32(stream, f32);
        else for (int k = 0; k < 33; ++k) t.GetUint16(stream, u16);
    });
    for (int i = 0; i < 9; ++i) t.GetUint32(stream, u32);
    reference(0xc9bd939a);
    reference(0xc9bd939a);
    t.GetUint32(stream, u32);
    t.GetUint32(stream, u32);                                       // transit switches
    many(u32, [&] {
        t.GetUint32(stream, u32);
        reference(0x26bb767e);
        t.GetFloat32(stream, f32); t.GetFloat32(stream, f32); t.GetFloat32(stream, f32);
        if (version < 0x1d) { t.GetUint32(stream, u32); many(u32, [&] { t.GetUint8(stream, u8); }); }   // the old rule list: bytes
        t.GetBool(stream, flag); t.GetUint8(stream, u8); t.GetUint8(stream, u8);
        t.GetFloat32(stream, f32);
        t.GetFloat32(stream, f32);
        if (version >= 0x1d) {
            t.GetUint32(stream, u32);                               // the rules
            many(u32, [&] { for (int b = 0; b < 5; ++b) t.GetBool(stream, flag); t.GetUint16(stream, u16); t.GetUint16(stream, u16); });
            if (version >= 0x21) { t.GetFloat32(stream, f32); t.GetBool(stream, flag); }
        }
    });
    reference(0x223e2b67);
    reference(0x223e2b67);
    std::int64_t s64 = 0;
    t.GetSint64(stream, s64);
    t.GetFloat32(stream, f32);
    if (version > 0x28) t.GetFloat32(stream, f32);
    t.GetFloat32(stream, f32);
    if (version < 0x16) t.GetFloat32(stream, f32); else t.GetFloat64(stream, f64);   // widened to a double when stored
    if (version > 0x28) t.GetFloat64(stream, f64);
    t.GetUint32(stream, u32);
    t.GetUint8(stream, u8);
    t.GetSint32(stream, s32);
    t.GetUint32(stream, u32);
    t.GetUint32(stream, u32);
    t.GetFloat32(stream, f32);
    for (int set = 0; set < 2; ++set) {                           // two sets of 16-bit keys
        t.GetUint32(stream, u32);
        many(u32, [&] { t.GetUint16(stream, u16); });
    }
    if (version > 0x19) {                                         // later versions: two values, a float and two more key sets
        t.GetUint32(stream, u32); t.GetUint32(stream, u32); t.GetFloat32(stream, f32);
        for (int set = 0; set < 2; ++set) {
            t.GetUint32(stream, u32);
            many(u32, [&] { t.GetUint16(stream, u16); });
        }
    }
    t.GetUint8(stream, u8);
    {
        std::int8_t state = 0;                                    // ScanningIterator
        t.GetSint8(stream, state);
        t.GetSint32(stream, s32); t.GetSint32(stream, s32); t.GetUint32(stream, u32);
        for (int i = 0; i < 4; ++i) t.GetSint32(stream, s32);
    }
    t.GetBool(stream, flag);
    for (int i = 0; i < 4; ++i) t.GetSint32(stream, s32);
    t.GetBool(stream, flag);
    t.GetUint32(stream, u32);
    t.GetUint32(stream, u32);
    if (version < 0x19) { t.GetUint8(stream, u8); t.GetUint8(stream, u8); } else { t.GetSint16(stream, s16); t.GetSint16(stream, s16); }
    {
        std::int8_t state = 0;                                    // BoundaryIterator
        t.GetSint8(stream, state); t.GetSint8(stream, state);
        for (int i = 0; i < 7; ++i) t.GetSint32(stream, s32);
    }
    t.GetBool(stream, flag); t.GetBool(stream, flag);
    t.GetFloat32(stream, f32);
    t.GetSint32(stream, s32); t.GetSint32(stream, s32);
    t.GetUint32(stream, u32);
    for (int i = 0; i < 4; ++i) t.GetUint32(stream, u32);
    t.GetUint8(stream, u8);
    t.GetUint32(stream, u32);
    for (int i = 0; i < 4; ++i) t.GetSint32(stream, s32);
    for (int row = 0; row < 3; ++row)
        for (std::int32_t i = 0; i < tunnelWidth && !stream.GetError(); ++i) t.GetUint32(stream, u32);
    t.GetUint8(stream, u8); t.GetUint8(stream, u8);
    t.GetSint32(stream, s32); t.GetSint32(stream, s32);
    if (version > 0x18) t.GetSint32(stream, s32);
    for (int i = 0; i < 4; ++i) t.GetUint32(stream, u32);
    if (version >= 0x19) {
        for (int i = 0; i < 3; ++i) t.GetSint32(stream, s32);
        for (int i = 0; i < 4; ++i) t.GetUint32(stream, u32);
    }
    if (version >= 0x28) for (int i = 0; i < 6; ++i) t.GetUint32(stream, u32);

    for (int pass = 0; pass < 2; ++pass) {                        // the two density tables
        for (std::int32_t i = 0, n = version < 0x23 ? edgeSets : pathTypes; i < n && !stream.GetError(); ++i) rangedEntry();
        if (version > 0x24) for (std::int32_t i = 0; i < extraTypes && !stream.GetError(); ++i) rangedEntry();
        for (std::int32_t i = 0; i < pathTypes && !stream.GetError(); ++i) t.GetUint16(stream, u16);
        if (version > 0x24) for (std::int32_t i = 0; i < extraTypes && !stream.GetError(); ++i) t.GetUint16(stream, u16);
        t.GetUint16(stream, u16);
        std::int32_t rowCount = 3;                                // before version 0x22 the table has three rows
        if (version >= 0x22) t.GetSint32(stream, rowCount);          // later versions store the number of rows
        for (std::int32_t i = 0; i < rowCount && !stream.GetError(); ++i) {
            t.GetUint16(stream, u16); t.GetFloat32(stream, f32); t.GetSint32(stream, s32); t.GetSint32(stream, s32); t.GetFloat32(stream, f32);
        }
        t.GetFloat32(stream, f32); t.GetFloat32(stream, f32);
        t.GetBool(stream, flag);
        t.GetFloat32(stream, f32);
        if (version >= 0x15) t.GetUint8(stream, u8);
    }
    t.GetUint32(stream, u32);                                        // tunnels
    many(u32, [&] {
        t.GetUint16(stream, u16); t.GetUint8(stream, u8); t.GetUint8(stream, u8); t.GetUint16(stream, u16); t.GetUint16(stream, u16);
        for (std::int32_t i = 0; i < tunnelWidth; ++i) t.GetFloat32(stream, f32);
    });
    t.GetFloat32(stream, f32); t.GetFloat32(stream, f32); t.GetUint32(stream, u32);
    if (version >= 0x15) { t.GetFloat32(stream, f32); t.GetUint32(stream, u32); }
    for (std::int32_t i = 0; i < pathTypes && !stream.GetError(); ++i) t.GetUint32(stream, u32);
    t.GetUint32(stream, u32);
    many(u32, [&] { t.GetUint32(stream, u32); });                     // up to four connection ids
    t.GetUint32(stream, u32);                                        // property holders keyed by 16-bit ids
    many(u32, [&] { t.GetUint16(stream, u16); cSCPropertyHolder holder; holder.Read(stream); });
    if (version < 0x1b) return !stream.GetError();                // older records end here
    for (std::int32_t i = 0; i < tunnelWidth && !stream.GetError(); ++i) { t.GetUint32(stream, u32); t.GetUint32(stream, u32); }
    if (version > 0x26) t.GetUint32(stream, u32);
    if (version <= 0x1d) return !stream.GetError();               // no ferry terminals before version 0x1e
    t.GetUint32(stream, u32);                                        // ferry terminals
    many(u32, [&] {
        if (version < 0x20) t.GetUint16(stream, u16);
        for (int i = 0; i < 6; ++i) t.GetUint8(stream, u8);
        t.GetUint8(stream, u8);
        t.GetUint16(stream, u16); t.GetUint16(stream, u16); t.GetUint16(stream, u16);
        if (version >= 0x26) { t.GetUint8(stream, u8); t.GetUint8(stream, u8); }
        std::vector<std::uint8_t> bytes;
        t.GetUint32(stream, u32);                                    // links to other terminals
        many(u32, [&] {
            t.GetFloat32(stream, f32); t.GetSint16(stream, s16); t.GetSint16(stream, s16); t.GetUint16(stream, u16);
            t.GetBytes(stream, bytes, kStreamVectorLimit);
            if (version >= 0x24) for (int i = 0; i < 4; ++i) t.GetUint16(stream, u16);
        });
        t.GetUint32(stream, u32);                                    // links to neighbour connections
        many(u32, [&] {
            t.GetFloat32(stream, f32);
            for (int i = 0; i < 4; ++i) t.GetSint16(stream, s16);
            t.GetBytes(stream, bytes, kStreamVectorLimit);
            if (version >= 0x1f) t.GetBytes(stream, bytes, kStreamVectorLimit);
            if (version >= 0x24) for (int i = 0; i < 4; ++i) t.GetUint16(stream, u16);
        });
    });
    return !stream.GetError();
}

bool cSC4TrafficSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    if (!exemplar) return false;
    TrafficTunables& t = tunables_;
    using H = cSCPropertyHelp;
    H::GetPropertyValue(exemplar, 0x491332e6, t.value_bcc);

    for (int type = 0; type < 9; ++type) {
        std::vector<float> speeds;
        auto& perMinute = t.cellsPerMinute[type];
        auto& inverse = t.inverseMaximumSpeed[type];
        int filled = 0;
        if (H::GetPropertyValue(exemplar, 0x491332e7 + type, speeds)) {
            filled = int(std::min<std::size_t>(speeds.size(), 13));
            for (int i = 0; i < filled; ++i) {
                if (speeds[i] <= 0.0f) {
                    perMinute[i] = 0.0f;
                    inverse[i] = FLT_MAX;
                } else {
                    perMinute[i] = ((speeds[i] * 1000.0f) / 60.0f) / host_->CityCellSize();
                    inverse[i] = 1.0f / perMinute[i];
                }
            }
        }
        for (int i = filled; i < 13; ++i) {
            perMinute[i] = 0.0f;
            inverse[i] = FLT_MAX;
        }
    }
    H::GetPropertyValue(exemplar, 0x29136788, t.value_bd4);
    H::GetPropertyValue(exemplar, 0x29136789, t.value_bd0);

    std::vector<float> values;
    H::GetPropertyValue(exemplar, 0x6a84493e, values);
    for (std::size_t i = 0; i < 13; ++i) t.budgetScale[i] = i < values.size() ? values[i] : 0.0f;

    SC4Percentage percentage;
    if (H::GetPropertyValue(exemplar, 0xa92356ae, percentage)) t.value_de0 = percentage.value;
    if (H::GetPropertyValue(exemplar, 0xa92356af, percentage)) t.value_de4 = percentage.value;
    H::GetPropertyValue(exemplar, 0xa92356b0, t.potholePenalty);
    H::GetPropertyValue(exemplar, 0xa92356b6, t.value_ea0);
    H::GetPropertyValue(exemplar, 0xa92356b7, t.value_ea4);
    H::GetPropertyValue(exemplar, 0xa92356b8, t.value_ea8);

    if (H::GetPropertyValue(exemplar, 0xa92356b1, values)) {
        for (std::size_t i = 0; i < 9 && i < values.size(); ++i) t.values_dfc[i] = values[i];
    }
    if (H::GetPropertyValue(exemplar, 0xa92356b3, values)) {
        for (std::size_t i = 0; i < 13; ++i) {
            const float v = i < values.size() ? values[i] : 0.0f;
            t.values_e24[i] = v;
            t.inverse_e58[i] = std::fabs(v) > 1e-10f ? 1.0f / v : 1.0f;
        }
    }
    std::vector<bool> flags;
    const auto readFlags = [&](std::uint32_t id, auto& out) {
        if (!H::GetPropertyValue(exemplar, id, flags)) return;
        for (std::size_t i = 0; i < 9; ++i) out[i] = i < flags.size() ? bool(flags[i]) : true;
    };
    readFlags(0xa92356b4, t.travelTypeEnabled);
    readFlags(0xa92356bf, travelTypeAffectedByTraffic_);
    readFlags(0xa92356b5, t.travelTypeFlag_e95);

    // congestion -> inverse speed curve: y values are stored as speeds and used as their reciprocal
    {
        std::vector<float> curveValues;
        if (H::GetPropertyValue(exemplar, 0xa92356b9, curveValues)) congestionToInverseSpeed_.SetResponseCurveData(curveValues.data(), curveValues.size());
        std::vector<SC4Point> points;
        congestionToInverseSpeed_.GetResponseCurveData(points);
        for (SC4Point& p : points) p.y = 1.0f / p.y;
        congestionToInverseSpeed_.SetResponseCurveData(points);
        if (!points.empty()) t.value_eb0 = 255.0f / (points.back().y - 1.0f);
    }
    H::GetPropertyValue(exemplar, 0xa92356ba, t.tripScale);
    if (H::GetPropertyValue(exemplar, 0xa92356bb, values)) {
        t.count_ec0 = std::uint8_t(values.size());
        t.inverse_ec8.assign(values.size() & 0xff, 0.0f);
        for (std::size_t i = 0; i < t.count_ec0 && i < values.size(); ++i) t.inverse_ec8[i] = 1.0f / values[i];
    }
    const auto readNine = [&](std::uint32_t id, auto& out) {
        if (!H::GetPropertyValue(exemplar, id, values)) return;
        for (std::size_t i = 0; i < 9; ++i) out[i] = i < values.size() ? values[i] : 0.0f;
    };
    readNine(0xa92356bc, t.values_ee0);
    readNine(0xea8c3cdb, t.values_f04);
    readNine(0xcad64136, t.values_f28);
    H::GetPropertyValue(exemplar, 0xa92356bd, t.value_f4c);
    H::GetPropertyValue(exemplar, 0x49a2e8be, t.freightScalingFactor);
    H::GetPropertyValue(exemplar, 0x4a678060, t.value_f60);
    H::GetPropertyValue(exemplar, 0xca76013b, t.tripScaleForDisplay);
    H::GetPropertyValue(exemplar, 0xea7b5f06, t.value_f68);
    if (H::GetPropertyValue(exemplar, 0xa92356be, values)) {
        const std::size_t n = std::min<std::size_t>(values.size(), 3);
        for (std::size_t i = 0; i < n; ++i) t.backgroundTrafficWeights[i] = values[i];
        for (std::size_t i = n; i < 3; ++i) t.backgroundTrafficWeights[i] = 1.0f;
    }

    const auto loadCurve = [&](cSC4ResponseCurve& curve, std::uint32_t id, const std::vector<SC4Point>& fallback) {
        std::vector<float> curveValues;
        if (exemplar->HasProperty(id) && H::GetPropertyValue(exemplar, id, curveValues)) curve.SetResponseCurveData(curveValues.data(), curveValues.size());
        else curve.SetResponseCurveData(fallback);
    };
    loadCurve(curveF78_, kExemplarProperty_curveF78, {{0.0f, 0.0f}, {1.0f, 1.0f}});
    loadCurve(curveFB8_, kExemplarProperty_curveFB8, {{0.25f, 0.0f}, {1.0f, 0.05f}, {2.0f, 0.25f}});
    loadCurve(curveFD8_, kExemplarProperty_curveFD8, {{0.0f, 1.0f}, {1000.0f, 1.0f}});
    t.value_ff8 = 10.0f;
    H::GetPropertyValue(exemplar, 0x4be09ecb, t.value_ff8);
    t.value_ffc = 60.0f;
    H::GetPropertyValue(exemplar, 0x4be09ecc, t.value_ffc);
    loadCurve(fundingCurve_, kExemplarProperty_fundingCurve, {{0.0f, 0.02f}, {100.0f, -0.01f}, {120.0f, -0.04f}});
    t.railAccidentChance = 1.0f;
    H::GetPropertyValue(exemplar, 0x0bd86fd5, t.railAccidentChance);
    H::GetPropertyValue(exemplar, 0x8c3caef5, t.value_f70);
    UpdateTravelStrategies(exemplar);
    return true;
}

void cSC4TrafficSimulator::UpdateTravelStrategies(const cISCPropertyHolder* exemplar) {
    if (exemplar) {
        for (int wealth = 0; wealth < 4; ++wealth) {
            std::vector<std::uint8_t> bytes;
            if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x4953e8a3 + wealth, bytes)) {
                for (int i = 0; i < 3 && i < int(bytes.size()); ++i) travelStrategies_[wealth * 3 + i] = bytes[i];
            }
        }
    }
    std::vector<std::vector<std::int32_t>> ordinances;
    host_->ActiveOrdinanceStrategyAdjustments(ordinances);
    for (const auto& adjust : ordinances) {
        if (adjust.size() < 9) continue;
        for (int group = 0; group < 3; ++group) {
            for (int i = 0; i < 3; ++i) {
                std::uint8_t& slot = travelStrategies_[3 + group * 3 + i];
                int v = int(slot) + adjust[group * 3 + i];
                if (v < 0) v = 0;
                if (v > 99) v = 100;
                slot = std::uint8_t(v);
            }
        }
    }
}

void cSC4TrafficSimulator::GetTravelStrategyPercentages(unsigned wealth, std::vector<SC4Percentage>& out) const {
    out.resize(3);
    for (int i = 0; i < 3; ++i) out[i].value = float(travelStrategies_[wealth * 3 + i]) * 0.01f;
}

// ---- network info grid ----------------------------------------------------------------------------------

void cSC4TrafficSimulator::InitGrid(int cellsX, int cellsZ) {
    lastX_ = cellsX - 1;
    lastZ_ = cellsZ - 1;
    grid_.assign(std::size_t(cellsX), std::vector<std::vector<NetworkInfo>>(std::size_t(cellsZ)));
    // Init (0x100077ac8) sizes the neighbour tables from the city: west/east by depth, north/south by width,
    // the flag table (width + 2) x (depth + 2) and the fixed 0x10000-cell connection table.
    neighborConnections_[0].assign(std::size_t(std::max(cellsZ, 0)), {});
    neighborConnections_[1].assign(std::size_t(std::max(cellsX, 0)), {});
    neighborConnections_[2].assign(std::size_t(std::max(cellsZ, 0)), {});
    neighborConnections_[3].assign(std::size_t(std::max(cellsX, 0)), {});
    cellTripFlags_.assign(std::size_t(std::max(cellsX, 0)) + 2,
                          std::vector<std::uint8_t>(std::size_t(std::max(cellsZ, 0)) + 2, 0));
    cellConnections_.assign(0x10000, {});
    connectionCounts_.fill(0);
    tripCapacityTotals_.fill(0);
    neighborPools_.clear();
    if (!defaultNeighborPoolValid_) {
        defaultNeighborPoolValid_ = true;
        // Init's SetMaxTripCapacity(default pool, 3, 0xffffffff): the border lane group never runs out
        SetMaxTripCapacity(&defaultNeighborPool_, 3, 0xffffffff);
    }
}

std::vector<NetworkInfo>* cSC4TrafficSimulator::Cell(int x, int z) const {
    if (x < 0 || z < 0 || std::size_t(x) >= grid_.size() || std::size_t(z) >= grid_[std::size_t(x)].size()) return nullptr;
    return &grid_[std::size_t(x)][std::size_t(z)];
}

NetworkInfo* cSC4TrafficSimulator::FindNetworkInfo(int networkType, int x, int z) const {
    std::vector<NetworkInfo>* cell = Cell(x, z);
    if (!cell) return nullptr;
    for (NetworkInfo& info : *cell) {
        if (info.networkType == networkType) return &info;
    }
    return nullptr;
}

NetworkInfo* cSC4TrafficSimulator::GetNetworkInfo(int networkType, int x, int z) { return FindNetworkInfo(networkType, x, z); }
const NetworkInfo* cSC4TrafficSimulator::GetNetworkInfo(int networkType, int x, int z) const { return FindNetworkInfo(networkType, x, z); }

NetworkInfo* cSC4TrafficSimulator::AddNetworkInfo(int networkType, int x, int z) {
    if (NetworkInfo* existing = FindNetworkInfo(networkType, x, z)) return existing;
    std::vector<NetworkInfo>* cell = Cell(x, z);
    if (!cell) return nullptr;
    NetworkInfo info;
    info.networkType = std::uint8_t(networkType);
    cell->push_back(info);
    return &cell->back();
}

void cSC4TrafficSimulator::RemoveNetworkInfo(int networkType, int x, int z) {
    std::vector<NetworkInfo>* cell = Cell(x, z);
    if (!cell) return;
    const auto it = std::find_if(cell->begin(), cell->end(), [&](const NetworkInfo& i) { return i.networkType == networkType; });
    if (it != cell->end()) cell->erase(it);
}

float cSC4TrafficSimulator::GetCapacity(int networkType, int x, int z) const {
    const NetworkInfo* info = GetNetworkInfo(networkType, x, z);
    return info ? 1.0f / info->inverseCapacity : 0.0f;
}

std::uint8_t cSC4TrafficSimulator::GetConnectionCount(int networkType, int x, int z) const {
    const NetworkInfo* info = GetNetworkInfo(networkType, x, z);
    return info ? info->connectionCount : 0;
}

std::uint32_t cSC4TrafficSimulator::EdgeDensityKey(int x, int z, int travelType, bool secondDirection) {
    return std::uint32_t(x << 8) | std::uint32_t(z) | (std::uint32_t(travelType) & 0xff) << 16 | std::uint32_t(secondDirection) << 24;
}

TrafficEdgeDensity& cSC4TrafficSimulator::EdgeDensityEntry(int x, int z, int travelType, bool secondDirection) {
    return edgeDensities_[EdgeDensityKey(x, z, travelType, secondDirection)];
}

const TrafficEdgeDensity& cSC4TrafficSimulator::GetTrafficEdgeDensity(int x, int z, int travelType, bool secondDirection) const {
    const auto it = edgeDensities_.find(EdgeDensityKey(x, z, travelType, secondDirection));
    return it == edgeDensities_.end() ? emptyEdgeDensity_ : it->second;
}

float cSC4TrafficSimulator::GetTravelTimeRatio(int x, int z, int travelType) const {
    if (travelTypeAffectedByTraffic_[std::size_t(travelType)]) {
        std::uint32_t density = 0;
        for (int k = 0; k < 9; ++k) {
            if (!tunables_.travelTypeEnabled[k]) continue;
            // both directions of the edge (the original passes the second flag implicitly for the second call)
            density += GetTrafficEdgeDensity(x & 0xff, z & 0xff, k, false).density;
            density += GetTrafficEdgeDensity(x & 0xff, z & 0xff, k, true).density;
        }
        const NetworkInfo* info = GetNetworkInfo(kTravelModeToTransitNetwork[std::size_t(travelType)], x, z);
        if (info) return congestionToInverseSpeed_.GetOutputValue(float(density) * info->inverseCapacity);
    }
    return 1.0f;
}

float cSC4TrafficSimulator::GetBackgroundTraffic(int x, int z) const {
    static constexpr std::uint32_t kGroup0[] = {0x1010, 0x1020, 0x1030};
    static constexpr std::uint32_t kGroup1[] = {0x3110, 0x3120, 0x3130, 0x3320, 0x3330};
    static constexpr std::uint32_t kGroup2[] = {0x4100, 0x4200, 0x4300, 0x4400};
    int sums[3] = {0, 0, 0};
    for (std::uint32_t id : kGroup0) sums[0] += host_->DemandGridValue(id, x, z);
    for (std::uint32_t id : kGroup1) sums[1] += host_->DemandGridValue(id, x, z);
    for (std::uint32_t id : kGroup2) sums[2] += host_->DemandGridValue(id, x, z);
    float result = 0.0f;
    for (int i = 0; i < 3; ++i) result += float(sums[i]) * tunables_.backgroundTrafficWeights[std::size_t(i)];
    return result;
}

// 0x10008d8bc: with a transit switch under the cell, its capacity, usage and percent; otherwise the station record whose cell lies in
// the rectangle of the lot at (x, z). A strike zeroes the served count, the usage and the percent. The maintenance figure is the
// budget item's monthly cost scaled by its department's funding fraction (truncated), left 0 when there is no item or department.
bool cSC4TrafficSimulator::GetTransitSwitchQueryData(int x, int z, TransitSwitchQueryData& out) {
    const bool striking = strikeState_ == kStrikeOn;
    cISCPropertyHolder* holder = nullptr;
    const std::uint32_t switchId = host_->TransitSwitchIdAtCell(x, z);
    if (switchId != 0) {
        TransitSwitch& entry = transitSwitches_[switchId];
        out.capacity = static_cast<std::int32_t>(entry.capacity);
        if (striking) {
            out.served = 0;
            out.usage = 0;
            out.percent = 0.0f;   // kSC4PercentageZero
        } else {
            out.served = static_cast<std::int32_t>(entry.capacity);
            out.usage = static_cast<std::int32_t>(entry.usage);
            out.percent = entry.percent;
        }
        out.maintenance = 0;
        holder = host_->TransitSwitchProperties(entry.occupant);
    } else {
        ITrafficLot* lot = host_->LotAt(x, z, 0);
        if (!lot) return false;
        SC4RectInt32 rect;
        lot->OccupiedRect(rect);
        const TransitStationRecord* station = nullptr;
        for (const TransitStationRecord& candidate : transitStations_) {
            if (rect.x0 <= candidate.x && rect.z0 <= candidate.z && candidate.x <= rect.x1 && candidate.z <= rect.z1) {
                station = &candidate;
                break;
            }
        }
        if (!station) return false;
        if (striking) {
            out.served = 0;
            out.capacity = 0;
            out.usage = 0;
            out.percent = 0.0f;   // kSC4PercentageZero
        } else {
            out.served = station->capacity;
            out.capacity = station->capacity;
            out.usage = station->capacity;
            out.percent = 1.0f;   // kSC4PercentageOneHundred
        }
        out.maintenance = 0;
        holder = host_->LotBuildingProperties(lot);
        if (!holder) return true;
    }
    std::uint32_t department = 0;
    std::int64_t cost = 0;
    float fraction = 0.0f;
    if (host_->BudgetItemForPurpose(holder, kBudgetPurposeTransitStation, department, cost) &&
        host_->DepartmentFundingFraction(department, fraction))
        out.maintenance = static_cast<std::int32_t>(static_cast<std::int64_t>(static_cast<float>(cost) * fraction));
    return true;
}

float cSC4TrafficSimulator::TransitSwitchEntryCost(std::uint32_t id) {
    TransitSwitch& entry = transitSwitches_[id];
    const float ratio = entry.traffic / entry.capacity;
    if (5.0f <= ratio) return 1.7014117e+38f;
    return congestionToInverseSpeed_.GetOutputValue(ratio) * entry.cost * transitSwitchCostScale;
}

// ---- roads / rail damage ----------------------------------------------------------------------------------

void cSC4TrafficSimulator::ApplyPotholeChanges(CellSet& healthy, CellSet& damaged, int delta, int networkType) {
    CellSet& source = delta > 0 ? healthy : damaged;
    CellSet& target = delta > 0 ? damaged : healthy;
    std::uint32_t count = delta > 0 ? std::uint32_t(delta) : std::uint32_t(-delta);
    count = std::min<std::uint32_t>(count, std::uint32_t(source.Size()));
    for (; count != 0; --count) {
        const std::uint16_t cell = source.At(random_->RandomUint32Uniform(std::uint32_t(source.Size())));
        const int x = cell >> 8, z = cell & 0xff;
        if (NetworkInfo* info = GetNetworkInfo(networkType, x, z)) {
            info->potholePenalty = delta > 0 ? tunables_.potholePenalty : 0.0f;
            host_->PostPotholeMessage(x, z, delta > 0);
        }
        source.Erase(cell);
        target.Insert(cell);
    }
}

void cSC4TrafficSimulator::ApplyRoadFunding(const SC4Percentage& funding) {
    const float step = fundingCurve_.GetOutputValue(funding.value * 100.0f);
    roadFundingAccum_ = std::min(std::max(roadFundingAccum_ + step, 0.0f), 1.0f);
    const int target = int(roadFundingAccum_ * float(roadPotholeTotal_));
    if (roadPotholeCount_ != target) {
        const int delta = target - roadPotholeCount_;
        ApplyPotholeChanges(healthyRoads_, damagedRoads_, delta, 1);
        roadPotholeCount_ += delta;
        statRoadPotholes_ = short(roadPotholeCount_);
    }
}

void cSC4TrafficSimulator::ApplyMassTransitFunding(const SC4Percentage& funding) {
    const float step = fundingCurve_.GetOutputValue(funding.value * 100.0f);
    railFundingAccum_ = std::min(std::max(railFundingAccum_ + step, 0.0f), 1.0f);
    const int previous = railPotholeCount_;
    const int target = int(railFundingAccum_ * float(railPotholeTotal_));
    if (previous == target) return;
    railPotholeCount_ = target;
    ApplyPotholeChanges(healthyRail_, damagedRail_, target - previous, 2);
}

bool cSC4TrafficSimulator::CheckRailAccident(int x, int z) const {
    if (!damagedRail_.Contains(std::uint16_t((x << 8) | z))) return false;
    return random_->RandomDoubleUniform() < double(tunables_.railAccidentChance);
}

// ---- strikes and budgets ---------------------------------------------------------------------------------

void cSC4TrafficSimulator::UpdateStrikes() {
    const float funding = host_->MassTransitFundingFraction();
    const TrafficTunables& t = tunables_;
    if (strikeState_ == kStrikeNone) {
        if (funding < t.strikeThreatFunding && strikeEligible_ > 0) {
            strikeChance_ = (t.strikeThreatFunding - funding) * t.strikeChanceScale;
            if (random_->RandomDoubleUniform() < double(strikeChance_)) {
                strikeState_ = kStrikeThreatened;
                host_->PostMessage(kTrafficMessageStrikeThreatened);
            }
        }
        return;
    }
    if (strikeState_ == kStrikeOn) {
        if (funding < t.strikeEndFunding && std::uint32_t(strikeDays_) < t.strikeMaxDays && strikeEligible_ != 0) {
            ++strikeDays_;
            return;
        }
        strikeState_ = kStrikeNone;
        strikeActive_ = false;
        host_->PostMessage(kTrafficMessageStrikeEnded);
    } else if (strikeState_ == kStrikeThreatened) {
        if (t.strikeEndFunding <= funding || strikeEligible_ == 0) {
            strikeState_ = kStrikeNone;
            host_->PostMessage(kTrafficMessageStrikeAverted);
        } else {
            strikeState_ = kStrikeOn;
            strikeDays_ = 0;
            strikeActive_ = true;
            host_->PostMessage(kTrafficMessageStrikeBegan);
        }
    }
}

void cSC4TrafficSimulator::UpdateRoadsBudget() {
    const auto& k = tunables_.budgetScale;
    const auto line = [&](std::uint32_t count, float scale) { return RoundToInt(float(count) * scale); };
    host_->SetRoadBudgetExpense(2, line(counters_.c930, k[10]) + line(counters_.c908, k[0]));
    host_->SetRoadBudgetExpense(1, line(counters_.c914, k[3]));
    host_->SetRoadBudgetExpense(3, line(counters_.c938, k[12]) + line(counters_.c910, k[2]));
    host_->SetRoadBudgetExpense(4, line(counters_.c920, k[6]));
}

void cSC4TrafficSimulator::UpdateMassTransitBudget() {
    const auto& k = tunables_.budgetScale;
    const auto line = [&](std::uint32_t count, float scale) { return RoundToInt(float(count) * scale); };
    host_->SetMassTransitBudgetExpense(3, line(counters_.c90c, k[1]));
    host_->SetMassTransitBudgetExpense(4, line(counters_.c924, k[7]));
    host_->SetMassTransitBudgetExpense(5, line(counters_.c928, k[8]));
    host_->SetMassTransitBudgetExpense(6, line(counters_.c92c, k[9]));
}

// ---- trip capacity -----------------------------------------------------------------------------------------

std::uint32_t cSC4TrafficSimulator::GetTripCapacity(const cISCPropertyHolder* holder, std::uint32_t index) {
    cISCProperty* property = holder->GetProperty(kTripCapacityProperty);
    if (!property) return 0;
    cIGZVariant* value = property->GetPropertyValue();
    if (value->GetType() == 0x8004) return value->RefUint16()[index];
    return value->RefUint32()[index];
}

void cSC4TrafficSimulator::SetTripCapacity(cISCPropertyHolder* holder, std::uint32_t index, std::uint32_t valueToSet) {
    cISCProperty* property = holder->GetProperty(kTripCapacityProperty);
    if (!property) {
        auto* created = new cSCProperty(kTripCapacityProperty);
        std::uint16_t zeros[12] = {};
        created->GetPropertyValue()->RefUint16(zeros, 12);
        holder->AddProperty(created, false);   // the holder takes the only reference
        property = created;
    }
    cIGZVariant* value = property->GetPropertyValue();
    if (value->GetType() == 0x8004) {
        std::uint16_t* narrow = const_cast<std::uint16_t*>(value->RefUint16());
        if (valueToSet < 0xffff) {
            narrow[index] = std::uint16_t(valueToSet);
            return;
        }
        std::uint32_t widened[12];
        for (int i = 0; i < 12; ++i) widened[i] = narrow[i];
        value->RefUint32(widened, 12);
    }
    const_cast<std::uint32_t*>(value->RefUint32())[index] = valueToSet;
}

// ---- neighbour geometry ------------------------------------------------------------------------------------

int cSC4TrafficSimulator::GetNeighborConnectionInnerCell(int x, int z, int& innerX, int& innerZ) const {
    const bool xOutside = x < 0 || lastX_ < x;
    if (z < 0) {
        if (xOutside) return -1;
        innerX = x;
        innerZ = 0;
        return 1;
    }
    if (!((lastZ_ < z) != xOutside)) return -1;
    innerX = x;
    innerZ = z;
    if (xOutside) {
        if (x >= 0) { innerX = lastX_; return 2; }
        innerX = 0;
        return 0;
    }
    innerZ = lastZ_;
    return 3;
}

void cSC4TrafficSimulator::FindHighwayReturnTile(int& x, int& z, bool flag) const {
    const bool right = host_->IsRightHandTraffic();
    int step;
    if (right == flag) {
        if (x < 0) { --z; return; }
        if (lastX_ < x) { ++z; return; }
        if (z < 0) step = 1;
        else { step = -1; if (z <= lastZ_) return; }
    } else {
        if (x < 0) { ++z; return; }
        if (lastX_ < x) { --z; return; }
        if (z >= 0) { step = 1; if (z <= lastZ_) return; }
        else step = -1;
    }
    x += step;
}

void cSC4TrafficSimulator::InvalidateIntersectionData(int x0, int z0, int x1, int z1) {
    if (!intersectionDirty_) {
        dirtyX0_ = x0; dirtyZ0_ = z0; dirtyX1_ = x1; dirtyZ1_ = z1;
        intersectionDirty_ = true;
        return;
    }
    dirtyX0_ = std::min(dirtyX0_, x0);
    dirtyZ0_ = std::min(dirtyZ0_, z0);
    dirtyX1_ = std::max(dirtyX1_, x1);
    dirtyZ1_ = std::max(dirtyZ1_, z1);
}

// ---- subnetworks ---------------------------------------------------------------------------------------------

void cSC4TrafficSimulator::ClearSubnetworks() {
    nextSubnetworkId_ = 1;
    for (auto& column : grid_)
        for (auto& cell : column)
            for (NetworkInfo& info : cell) info.subnetwork.fill(0);
}

void cSC4TrafficSimulator::GetSubnetworksForLot(const cISCPropertyHolder* lot, std::vector<std::uint32_t>& out) const {
    out.clear();
    cSCPropertyHelp::GetPropertyValue(lot, kLotSubnetworksProperty, out);
}

namespace {
void AppendNetworkSubnetworks(const NetworkInfo& info, std::vector<std::uint32_t>& out) {
    for (std::uint16_t id : info.subnetwork)
        if (id != 0 && std::find(out.begin(), out.end(), id) == out.end()) out.push_back(id);
}
}  // namespace

// Road cells (network type 1) only; cells outside the grid are skipped. An inverted rectangle yields nothing.
void cSC4TrafficSimulator::GetSubnetworksInRectangle(const TrafficCellRect& rect, std::vector<std::uint32_t>& out) const {
    out.clear();
    for (int x = rect.x0; x <= rect.x1; ++x)
        for (int z = rect.z0; z <= rect.z1; ++z)
            if (const NetworkInfo* info = GetNetworkInfo(1, x, z)) AppendNetworkSubnetworks(*info, out);
}

void cSC4TrafficSimulator::GetSubnetworksInRegion(const TrafficCellRegion& region, std::vector<std::uint32_t>& out) const {
    out.clear();
    for (int x = region.bounds.x0; x <= region.bounds.x1; ++x)
        for (int z = region.bounds.z0; z <= region.bounds.z1; ++z)
            if (region.Contains(x, z))
                if (const NetworkInfo* info = GetNetworkInfo(1, x, z)) AppendNetworkSubnetworks(*info, out);
}

// False when either lot has no (or an empty) subnetwork list.
bool cSC4TrafficSimulator::AreLotsConnected(const cISCPropertyHolder* a, const cISCPropertyHolder* b) const {
    std::vector<std::uint32_t> first, second;
    if (!cSCPropertyHelp::GetPropertyValue(a, kLotSubnetworksProperty, first)) return false;
    if (!cSCPropertyHelp::GetPropertyValue(b, kLotSubnetworksProperty, second)) return false;
    for (std::uint32_t id : first)
        if (std::find(second.begin(), second.end(), id) != second.end()) return true;
    return false;
}

// Largest count of `occupantType` over the lot's subnetworks; ids past the published vector (and id 0's entry
// like any other) are handled as in the original: only the bound check applies.
std::uint32_t cSC4TrafficSimulator::GetConnectedOccupantCount(const cISCPropertyHolder* lot, std::uint32_t occupantType) const {
    std::vector<std::uint32_t> ids;
    if (!cSCPropertyHelp::GetPropertyValue(lot, kLotSubnetworksProperty, ids)) return 0;
    std::uint32_t best = 0;
    for (std::uint32_t id : ids) {
        if (id >= subnetworks_.size()) continue;
        const auto it = subnetworks_[id].occupantsByType.find(occupantType);
        if (it != subnetworks_[id].occupantsByType.end() && it->second > best) best = it->second;
    }
    return best;
}

// Destination types 0..3; anything else gives 0.
std::uint32_t cSC4TrafficSimulator::GetConnectedDestinationCount(const cISCPropertyHolder* lot, int destinationType) const {
    if (destinationType >= 4) return 0;
    std::vector<std::uint32_t> ids;
    if (!cSCPropertyHelp::GetPropertyValue(lot, kLotSubnetworksProperty, ids)) return 0;
    std::uint32_t best = 0;
    for (std::uint32_t id : ids)
        if (id < subnetworks_.size() && subnetworks_[id].destinations[std::size_t(destinationType)] > best)
            best = subnetworks_[id].destinations[std::size_t(destinationType)];
    return best;
}

// One entry per published subnetwork (entry 0 stays 0), the count of `occupantType` where present.
void cSC4TrafficSimulator::GetOccupantCountForAllSubnetworks(std::uint32_t occupantType, std::vector<std::uint32_t>& out) const {
    out.assign(subnetworks_.size(), 0);
    for (std::size_t i = 1; i < subnetworks_.size(); ++i) {
        const auto it = subnetworks_[i].occupantsByType.find(occupantType);
        if (it != subnetworks_[i].occupantsByType.end()) out[i] = it->second;
    }
}

// ---- neighbour connection tables ---------------------------------------------------------------------------

// Side selection of every border table function (0x1000820ac / 0x100082358 / 0x1000884e8 / 0x100089830 /
// 0x10008a090 / 0x100082ae2): west/east are indexed by z, north/south by x. False when (x, z) is not a border
// cell of one of the four sides (the original indexes the table unconditionally and reads out of bounds for a
// corner cell; the port refuses instead).
bool cSC4TrafficSimulator::NeighborSide(int x, int z, int lastX, int lastZ, int& side, int& index) {
    if (x < 0) {
        side = 0;
        index = z;
        return 0 <= z && z <= lastZ;
    }
    if (lastX < x) {
        side = 2;
        index = z;
        return 0 <= z && z <= lastZ;
    }
    if (z < 0) {
        side = 1;
        index = x;
        return true;
    }
    if (lastZ < z) {
        side = 3;
        index = x;
        return true;
    }
    side = -1;
    index = -1;
    return false;
}

TrafficNeighborConnection* cSC4TrafficSimulator::GetTrafficNeighborConnection(int x, int z) {
    int side = 0, index = 0;
    if (!NeighborSide(x, z, lastX_, lastZ_, side, index)) return nullptr;
    std::vector<TrafficNeighborConnection>& table = neighborConnections_[std::size_t(side)];
    if (index < 0 || std::size_t(index) >= table.size()) return nullptr;
    return &table[std::size_t(index)];
}

// 0x100089830: the first engine connection of a type the travel mode can use on that border cell.
void* cSC4TrafficSimulator::GetNeighborConnection(int travelMode, int x, int z) {
    TrafficNeighborConnection* entry = GetTrafficNeighborConnection(x, z);
    if (!entry) return nullptr;
    if (travelMode < 0 || travelMode > 8) return nullptr;
    const std::uint32_t valid = kValidNeighborConnectionTypesForTransitNetworkType[
        kTravelModeToTransitNetwork[std::size_t(travelMode)]];
    if ((valid & entry->types) == 0) return nullptr;
    for (int type = 0; type <= 12; ++type) {   // the binary tests 0..12 (type 12 is tried once more after the bound)

        if ((valid & entry->types & (1u << type)) == 0) continue;
        if (void* connection = host_->NeighborFindConnection(type, x, z)) return connection;
    }
    return nullptr;
}

// 0x1000884e8: the property holder of the pool this border cell belongs to (the default pool when the cell
// records no pool, or when the recorded pool no longer exists).
cISCPropertyHolder* cSC4TrafficSimulator::GetNeighborPoolPropertyHolder(int travelMode, int x, int z) {
    std::uint32_t valid = 0xffffffffu;
    if (travelMode != -1) {
        if (travelMode < 0 || travelMode > 8) return nullptr;
        valid = kValidNeighborConnectionTypesForTransitNetworkType[kTravelModeToTransitNetwork[std::size_t(travelMode)]] | 0x200u;
    }
    TrafficNeighborConnection* entry = GetTrafficNeighborConnection(x, z);
    if (!entry || (valid & entry->types) == 0) return nullptr;
    if (entry->pool != 0) {
        const auto it = neighborPools_.find(entry->pool);
        if (it != neighborPools_.end()) return &it->second;
        entry->pool = 0;
    }
    return DefaultNeighborPool();
}

// 0x1000820ac: record a connection on a border cell. The cost is the far side's property 0xca947077 or, when
// the far side has none, the trip scale. Cells whose far city is not established keep pool 0 (the default pool).
void cSC4TrafficSimulator::AddNeighborConnection(int type, int x, int z) {
    void* connection = host_->NeighborFindConnection(type, x, z);
    if (!connection) return;
    TrafficNeighborConnection* entry = GetTrafficNeighborConnection(x, z);
    if (!entry) return;
    entry->types = std::uint16_t(entry->types | std::uint16_t(1u << (type & 0x1f)));
    float cost = tunables_.tripScale;
    if (void* other = host_->NeighborFindConnectionOnOtherSide(connection)) {
        if (cISCPropertyHolder* properties = host_->NeighborConnectionProperties(other))
            cSCPropertyHelp::GetPropertyValue(properties, kNeighborConnectionCost, cost);
    }
    if (entry->cost < cost) entry->cost = cost;
    void* city = host_->NeighborConnectionCity(connection);
    if (!city || !host_->NeighborCityEstablished(city)) {
        entry->pool = 0;
        return;
    }
    int regionX = 0, regionZ = 0;
    host_->NeighborCityRegionCell(city, regionX, regionZ);
    for (const auto& [key, holder] : neighborPools_) {
        std::int32_t px = 0, pz = 0;
        if (!cSCPropertyHelp::GetPropertyValue(&holder, kNeighborPoolRegionX, px)) continue;
        if (!cSCPropertyHelp::GetPropertyValue(&holder, kNeighborPoolRegionZ, pz)) continue;
        if (px != regionX || pz != regionZ) continue;
        entry->pool = key;
        return;
    }
    cSCPropertyHolder holder;
    cSCPropertyHelp::SetPropertyValue(&holder, kNeighborPoolRegionX, std::int32_t(regionX));
    cSCPropertyHelp::SetPropertyValue(&holder, kNeighborPoolRegionZ, std::int32_t(regionZ));
    // the original walks the ordered pool map from its root for the smallest unused id (0x1000822ca)
    std::uint16_t pool = 1;
    while (neighborPools_.count(pool) != 0) ++pool;
    neighborPools_.emplace(pool, holder);
    entry->pool = pool;
}

// 0x100082358: drop one connection type from a border cell; when the cell has none left its pool goes back to
// the default and the lane-group bits of the cell's flag byte are cleared.
void cSC4TrafficSimulator::RemoveNeighborConnection(int type, int x, int z) {
    TrafficNeighborConnection* entry = GetTrafficNeighborConnection(x, z);
    if (!entry) return;
    entry->types = std::uint16_t(entry->types & ~std::uint16_t(1u << (type & 0x1f)));
    if (entry->types != 0) return;
    entry->pool = 0;
    RemoveUnusedPools();
    // the original indexes the flag table with the caller's coordinates (rows x + 1, columns z + 1)
    const int row = x + 1, column = z + 1;
    if (row < 0 || column < 0 || std::size_t(row) >= cellTripFlags_.size() ||
        std::size_t(column) >= cellTripFlags_[std::size_t(row)].size())
        return;
    std::uint8_t& flags = cellTripFlags_[std::size_t(row)][std::size_t(column)];
    for (int lane = 0; lane < 4; ++lane)
        if (((flags >> lane) & 1) != 0) --connectionCounts_[std::size_t(lane)];
    flags = 0;
}

// 0x1000828a2: erase every pool no border cell refers to any more. Quirk kept: the original scans indices
// 0 .. lastX - 1 of all four tables (it uses the x extent for the z-indexed tables too and stops one short),
// so a pool used only by the last slot of a table is erased.
void cSC4TrafficSimulator::RemoveUnusedPools() {
    std::vector<std::uint16_t> used;
    const int count = lastX_;
    for (int side = 0; side < 4; ++side) {
        const std::vector<TrafficNeighborConnection>& table = neighborConnections_[std::size_t(side)];
        const std::size_t limit = std::min<std::size_t>(table.size(), std::size_t(std::max(count, 0)));
        for (std::size_t i = 0; i < limit; ++i)
            if (table[i].pool != 0) used.push_back(table[i].pool);
    }
    for (auto it = neighborPools_.begin(); it != neighborPools_.end();) {
        if (std::find(used.begin(), used.end(), it->first) == used.end()) it = neighborPools_.erase(it);
        else ++it;
    }
}

// 0x100082ae2: does the inner cell of this border cell touch a map exit (a road cell whose mask or whose
// border record says a neighbour connection leaves there)?
bool cSC4TrafficSimulator::HasMapExit(int x, int z) {
    const bool xOutside = x < 0 || lastX_ < x;
    int side = 0, innerX = x, innerZ = z;
    if (z < 0) {
        side = 1;
        innerZ = 0;
        if (xOutside) return false;
    } else {
        if (!((lastZ_ < z) != xOutside)) return false;
        if (xOutside) {
            innerZ = z;
            innerX = 0;
        }
        side = xOutside ? 0 : 3;
        if (0 <= x && xOutside) {
            side = 2;
            innerX = lastX_;
            innerZ = z;
        } else if (!xOutside) {
            innerZ = lastZ_;
        }
    }
    const TrafficCellConnections& cell = CellConnections(innerX, innerZ);
    for (std::uint16_t mask : cell.masks) {
        for (int lane = 0; lane < 4; ++lane) {
            if (((mask >> (lane * 4 + side)) & 1) != 0) return true;
        }
    }
    const TrafficNeighborConnection* entry = GetTrafficNeighborConnection(x, z);
    return entry && (entry->types & 0x200) != 0;
}

// ---- congestion / commute accounting -----------------------------------------------------------------------

// 0x1000895c6: add a trip to the destination's lane group. A border destination counts on the neighbour pool
// holder and on the connection record; an inner destination on the lot. When the group's maximum capacity is
// reached the lane-group bit is cleared on every cell the destination occupies and its cell count goes down.
void cSC4TrafficSimulator::ApplyTrafficToDestination(std::uint32_t count, int x, int z, std::uint8_t networkType,
                                                     std::uint8_t tripType) {
    cISCPropertyHolder* destination = nullptr;
    cISCPropertyHolder* connection = nullptr;
    SC4RectInt32 cells;
    cells.x0 = cells.x1 = x;
    cells.z0 = cells.z1 = z;
    if (x < 0 || z < 0 || lastX_ < x || lastZ_ < z) {
        destination = GetNeighborPoolPropertyHolder(networkType, x, z);
        void* record = GetNeighborConnection(networkType, x, z);
        if (record) {
            connection = host_->NeighborConnectionProperties(record);
            if (connection && tripType < 3) {   // the original records the volume on the connection holder
                const std::uint32_t id = kConnectionTrafficVolumeBase + tripType;
                std::vector<std::uint32_t> volumes;
                if (!cSCPropertyHelp::GetPropertyValue(connection, id, volumes)) volumes.assign(9, 0);
                if (volumes.size() < 9) volumes.resize(9, 0);   // the game always stores nine network slots
                volumes[networkType] += count;
                cSCPropertyHelp::SetPropertyValue(connection, id, volumes);
            }
        }
    } else {
        ITrafficLot* lot = host_->LotAt(x, z, 0);
        if (!lot) return;
        destination = lot->Properties();
        lot->OccupiedRect(cells);
    }
    if (!destination) return;
    const std::uint32_t maximum = GetMaxTripCapacitySlot(destination, tripType);
    const std::uint32_t used = GetTripCapacity(destination, std::uint32_t(tripType) + 4u) + count;
    SetTripCapacity(destination, std::uint32_t(tripType) + 4u, used);
    if (connection) SetTripCapacity(connection, std::uint32_t(tripType) + 4u, used);
    if (maximum <= used) {
        for (int cellX = cells.x0; cellX <= cells.x1; ++cellX) {
            for (int cellZ = cells.z0; cellZ <= cells.z1; ++cellZ) {
                const int row = cellX + 1, column = cellZ + 1;
                if (row < 0 || column < 0 || std::size_t(row) >= cellTripFlags_.size() ||
                    std::size_t(column) >= cellTripFlags_[std::size_t(row)].size())
                    continue;
                cellTripFlags_[std::size_t(row)][std::size_t(column)] &= std::uint8_t(~(1u << tripType));
            }
        }
        --connectionCounts_[std::size_t(tripType)];
    }
}

// 0x1000836ba: rebuild the trip-capacity flag bytes of every cell from the lots that hold destinations.
// Quirks kept: the byte keeps bit 3 (only 0xf8 is masked off) and the lane counters go up even when the bit
// was already set. The lot-manager lookups pass 1 first and fall back to 0, the second one does not count.
void cSC4TrafficSimulator::WorkOnRefreshCommuteMap() {
    for (int z = 0; z <= lastZ_; ++z) {
        for (int x = 0; x <= lastX_; ++x) {
            std::uint8_t& flags = cellTripFlags_[std::size_t(x) + 1][std::size_t(z) + 1];
            flags = std::uint8_t(flags & 0xf8);
            ITrafficLot* lot = host_->LotAt(x, z, 1);
            const bool counted = lot != nullptr;
            if (!lot) lot = host_->LotAt(x, z, 0);
            if (!lot) continue;
            cISCPropertyHolder* properties = lot->Properties();
            if (!properties) continue;
            for (int lane = 0; lane < 4; ++lane) {
                if (GetMaxTripCapacitySlot(properties, std::uint32_t(lane)) == 0) continue;
                flags = std::uint8_t(flags | (1u << lane));
                if (counted) ++connectionCounts_[std::size_t(lane)];
            }
        }
    }
}

// 0x100083508: move every active lot's used trip capacities into the "arrived" counters and total the lots'
// maximum capacities per lane group (4 lane groups: indices 4..7 -> 8..11 of the lot's capacity array).
void cSC4TrafficSimulator::WorkOnClearTripDestinations() {
    for (int z = 0; z <= lastZ_; ++z) {
        for (int x = 0; x <= lastX_; ++x) {
            ITrafficLot* lot = host_->LotAt(x, z, 1);
            if (!lot) continue;
            cISCPropertyHolder* properties = lot->Properties();
            if (!properties) continue;
            for (std::uint32_t lane = 0; lane < 4; ++lane) {
                SetTripCapacity(properties, lane + 8u, GetTripCapacity(properties, lane + 4u));
                SetTripCapacity(properties, lane + 4u, 0);
            }
            for (std::uint32_t lane = 0; lane < 4; ++lane)
                tripCapacityTotals_[std::size_t(lane)] += int(GetMaxTripCapacitySlot(properties, lane));
        }
    }
}

// 0x100086086: roll each neighbour connection's traffic volumes over (0xaa9ea81a + k -> 0xaa9ea81d + k) and
// clear them, then move the used lane capacities 4..7 to the arrived counters 8..11 and zero them.
void cSC4TrafficSimulator::WorkOnUpdateNeighborConnections() {
    std::vector<void*> connections;
    host_->NeighborGetAllConnections(connections);
    const std::vector<std::uint32_t> zeros(9, 0);
    for (void* record : connections) {
        cISCPropertyHolder* properties = host_->NeighborConnectionProperties(record);
        if (!properties) continue;
        for (std::uint32_t k = 0; k < 2; ++k) {
            std::vector<std::uint32_t> volumes;
            if (cSCPropertyHelp::GetPropertyValue(properties, kConnectionTrafficVolumeBase + k, volumes))
                cSCPropertyHelp::SetPropertyValue(properties, kConnectionTrafficVolumeArrivedBase + k, volumes);
            cSCPropertyHelp::SetPropertyValue(properties, kConnectionTrafficVolumeBase + k, zeros);
        }
        for (std::uint32_t lane = 0; lane < 4; ++lane) {
            SetTripCapacity(properties, lane + 8u, GetTripCapacity(properties, lane + 4u));
            SetTripCapacity(properties, lane + 4u, 0);
        }
    }
}

// 0x100087336: hand the regional demand for the three wealth purposes out to the neighbour pools in proportion
// to each connected city's extrapolated population, then mark the border cells that have capacity left.
void cSC4TrafficSimulator::RecalculateNeighborConnectionCapacities() {
    const std::int32_t total[3] = {host_->RegionDemandTotal(0x2010), host_->RegionDemandTotal(0x2020),
                                   host_->RegionDemandTotal(0x2030)};
    std::vector<void*> cities;
    host_->NeighborGetConnectedCities(cities);
    double population[3] = {0.0, 0.0, 0.0};
    for (void* city : cities) {
        population[0] += host_->NeighborCityExtrapolatedPopulation(city, 0x2010);
        population[1] += host_->NeighborCityExtrapolatedPopulation(city, 0x2020);
        population[2] += host_->NeighborCityExtrapolatedPopulation(city, 0x2030);
    }
    for (void* city : cities) {
        int regionX = 0, regionZ = 0;
        host_->NeighborCityRegionCell(city, regionX, regionZ);
        cISCPropertyHolder* pool = nullptr;
        for (auto& [key, holder] : neighborPools_) {
            std::int32_t px = 0, pz = 0;
            if (!cSCPropertyHelp::GetPropertyValue(&holder, kNeighborPoolRegionX, px)) continue;
            if (!cSCPropertyHelp::GetPropertyValue(&holder, kNeighborPoolRegionZ, pz)) continue;
            if (px != regionX || pz != regionZ) continue;
            pool = &holder;
            break;
        }
        if (!pool) continue;
        for (std::uint32_t kind = 0; kind < 3; ++kind) {
            const int cityPopulation = host_->NeighborCityExtrapolatedPopulation(city, 0x2010u + kind * 0x10u);
            const std::int64_t share = TruncateToInt64((double(cityPopulation) / population[kind]) * double(total[kind]));
            SetMaxTripCapacity(pool, kind, std::uint32_t(share));
            SetTripCapacity(pool, kind + 4u, 0);
        }
        SetMaxTripCapacity(pool, 3, 0xffffffffu);
        SetTripCapacity(pool, 7, 0);
    }
    if (cISCPropertyHolder* defaultPool = DefaultNeighborPool()) SetTripCapacity(defaultPool, 7, 0);
    for (int side = 0; side < 4; ++side) {
        std::vector<TrafficNeighborConnection>& table = neighborConnections_[std::size_t(side)];
        // quirk kept: the original uses the x extent for the z-indexed tables and stops one short
        const std::size_t limit = std::min<std::size_t>(table.size(), std::size_t(std::max(lastX_, 0)));
        for (std::size_t index = 0; index < limit; ++index) {
            TrafficNeighborConnection& entry = table[index];
            if (entry.types == 0) continue;
            int x = 0, z = 0;
            switch (side) {
                case 0: x = -1; z = int(index); break;
                case 1: x = int(index); z = -1; break;
                case 2: x = lastX_ + 1; z = int(index); break;
                default: x = int(index); z = lastZ_ + 1; break;
            }
            if (!HasMapExit(x, z)) continue;
            cISCPropertyHolder* pool = nullptr;
            if (entry.pool != 0) {
                const auto it = neighborPools_.find(entry.pool);
                if (it != neighborPools_.end()) pool = &it->second;
                else {
                    entry.pool = 0;
                    pool = DefaultNeighborPool();
                }
            } else {
                pool = DefaultNeighborPool();
            }
            if (!pool) continue;
            const int row = x + 1, column = z + 1;
            if (row < 0 || column < 0 || std::size_t(row) >= cellTripFlags_.size() ||
                std::size_t(column) >= cellTripFlags_[std::size_t(row)].size())
                continue;
            for (int lane = 0; lane < 4; ++lane) {
                if (GetMaxTripCapacitySlot(pool, std::uint32_t(lane)) == 0) continue;
                cellTripFlags_[std::size_t(row)][std::size_t(column)] |= std::uint8_t(1u << lane);
                ++connectionCounts_[std::size_t(lane)];
            }
        }
    }
}

// ---- trip pipeline -----------------------------------------------------------------------------------------

// 0x100087c06: does the trip's source still have room for all of its trips? The source is a border pool when
// its cell is outside the map, otherwise the lot at that cell (a missing lot has no room at all).
bool cSC4TrafficSimulator::CapacityStillAvailable(const cSC4TripData& trip) {
    cISCPropertyHolder* properties = nullptr;
    const int x = trip.sourceX, z = trip.sourceZ;
    if (x < 0 || z < 0 || lastX_ < x || lastZ_ < z) {
        properties = GetNeighborPoolPropertyHolder(-1, x, z);
    } else {
        ITrafficLot* lot = host_->LotAt(x, z, 0);
        if (!lot) return trip.count == 0;
        properties = lot->Properties();
    }
    if (!properties) return trip.count == 0;
    const std::uint32_t maximum = GetMaxTripCapacitySlot(properties, trip.type);
    const std::uint32_t used = GetTripCapacity(properties, std::uint32_t(trip.type) + 4u);
    return trip.count <= maximum - used;
}

// 0x100087cc4: configure the two path finders for one trip.
void cSC4TrafficSimulator::SetupTrip(StandardTripParams& params, cSC4TripData& trip) {
    cISC4PathFinder* primary = params.primary;
    if (!primary) return;
    primary->SetTrafficDensity(trip.count);
    if (trip.kind != 1) {
        primary->SetTripDestinationFilter(0);
        primary->ClearAllDestinationFlags();
        std::uint8_t flag;
        if ((trip.flags & 2) == 0 || trip.type != 0) flag = std::uint8_t(1u << (trip.type & 0x1f));
        else flag = 7;
        primary->SetDestinationFlag(flag);
    } else {
        primary->SetTripDestinationFilter(3);
        SC4RectInt32 rect;
        rect.x0 = rect.x1 = trip.sourceX;
        rect.z0 = rect.z1 = trip.sourceZ;
        if (!(trip.sourceX < 0 || trip.sourceZ < 0 || lastX_ < trip.sourceX || lastZ_ < trip.sourceZ)) {
            if (ITrafficLot* lot = host_->LotAt(trip.sourceX, trip.sourceZ, 0)) lot->OccupiedRect(rect);
        }
        primary->SetEndRect(rect);
    }
    primary->SetMaxWorkTime(params.workTime);
    if (params.secondary) params.secondary->SetMaxWorkTime(params.workTime);
    if (params.source) primary->SetStartRect(host_->TrafficSourceStartRect(params.source));
    primary->ClearAllFlags();
    primary->ClearAllTravelTypeFlags();
    primary->SetStartingCostArray(nullptr);
    primary->SetMaxTripLength(tunables_.tripScale);
    if (trip.type < 3) {
        params.useSecondary = 0;
        params.secondPhase = 1;
        if (params.flag51 != 0) primary->SetFlag(4);
        primary->SetTravelTypeFlag(0x1cf, false);
        if ((trip.flags & 4) == 0) {
            primary->SetTravelTypeFlag(3, true);
            const float* costs;
            switch (trip.index) {
                case 0: costs = tunables_.values_f04.data(); break;
                case 1: costs = tunables_.values_f28.data(); break;
                case 2: costs = tunables_.values_ee0.data(); break;
                default: return;
            }
            primary->SetStartingCostArray(const_cast<float*>(costs));
        } else {
            primary->SetTravelTypeFlag(std::uint16_t(1u << (trip.index & 0x1f)), true);
        }
    } else if (trip.type == 3) {
        params.useSecondary = 1;
        primary->SetFlag(1);
        primary->SetTravelTypeFlag(0x30, true);
        params.secondPhase = 0;
    }
}

// 0x10008bf74: point a path finder at a lot. The travel-type cost array is chosen by the lot's heaviest trip
// index (ties keep the lower index) and the destination flag by the type of the lot's last trip. False when the
// lot has no trips at all, i.e. when nothing was configured. The original walks the libc++ list backwards
// (end.__prev_ chain); the sums do not depend on the order.
bool cSC4TrafficSimulator::SetupPathFinderForLot(cISC4PathFinder* pathFinder, ITrafficLot* lot) {
    if (!pathFinder || !lot) return false;
    void* source = lot->TrafficSource();
    if (!source) return false;
    std::list<cSC4TripData>* trips = lot->TripList(source);
    if (!trips) return false;
    if (trips->empty()) {
        lot->RefreshTrafficSource(source);
        if (trips->empty()) return false;
    }
    int counts[3] = {0, 0, 0};
    for (const cSC4TripData& trip : *trips) {
        if (trip.index >= 3) continue;   // the original indexes a three-element vector unconditionally
        counts[trip.index] += int(trip.count);
    }
    int heaviest = 0;
    for (int index = 1; index < 3; ++index)
        if (counts[index] > counts[heaviest]) heaviest = index;
    const std::uint8_t type = trips->back().type;
    pathFinder->SetStartRect(host_->TrafficSourceStartRect(source));
    pathFinder->ClearAllFlags();
    pathFinder->ClearAllTravelTypeFlags();
    pathFinder->SetStartingCostArray(nullptr);
    pathFinder->SetMaxTripLength(tunables_.tripScale);
    pathFinder->SetTripDestinationFilter(0);
    pathFinder->ClearAllDestinationFlags();
    pathFinder->SetDestinationFlag(std::uint8_t(1u << (type & 0x1f)));
    if (type < 3) {
        pathFinder->SetTravelTypeFlag(0x1cf, false);
        pathFinder->SetTravelTypeFlag(3, true);
        const float* costs = nullptr;
        switch (heaviest) {
            case 0: costs = tunables_.values_f04.data(); break;
            case 1: costs = tunables_.values_f28.data(); break;
            default: costs = tunables_.values_ee0.data(); break;
        }
        pathFinder->SetStartingCostArray(const_cast<float*>(costs));
        return true;
    }
    if (type == 3) {
        pathFinder->SetFlag(1);
        pathFinder->SetTravelTypeFlag(0x30, true);
        return true;
    }
    return false;
}

}  // namespace sc4
