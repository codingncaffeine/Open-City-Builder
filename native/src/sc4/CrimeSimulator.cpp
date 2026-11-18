#include "sc4/CrimeSimulator.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "gz/StreamOps.h"
#include "rz/cRZString.h"
#include "sc4/DBSegment.h"
#include "sc4/SimStreamOps.h"

namespace sc4 {

namespace {

// DAT_100a1ae90: demand ids summed for each wealth level, zero terminated (stride 6).
constexpr int kDemandFactors[3][6] = {
    {0x4100, 0x4200, 0x4300, 0x3110, 0x3120, 0},
    {0x4400, 0x3320, 0x3130, 0, 0, 0},
    {0x3330, 0, 0, 0, 0, 0},
};
// DAT_100a1af60 (zone type 1..9 -> bit) and DAT_100a1af90 (lot wealth 0..6 -> bit)
constexpr std::uint32_t kZoneBits[9] = {1, 1, 1, 2, 2, 2, 4, 4, 4};
constexpr std::uint32_t kWealthBits[7] = {1, 1, 2, 2, 4, 2, 2};

constexpr std::uint32_t kSimGridIid = 0xc9bd939a;
constexpr std::uint16_t kStreamVersion = 6;

void LoadCurve(cSC4ResponseCurve& curve, const cISCPropertyHolder* exemplar, std::uint32_t id) {
    std::vector<float> values;
    if (exemplar->HasProperty(id) && cSCPropertyHelp::GetPropertyValue(exemplar, id, values))
        curve.SetResponseCurveData(values.data(), values.size());
}

cSC4SimGrid<std::uint8_t>* NewGrid(int tractSize, std::uint32_t instance) {
    auto* grid = new cSC4SimGrid<std::uint8_t>();
    grid->AddRef();
    grid->Init();
    grid->SetTractSize(tractSize);
    grid->SetInstanceID(instance);
    grid->SetTractValues(0);
    return grid;
}

void ReleaseGrid(cSC4SimGrid<std::uint8_t>*& grid) {
    if (!grid) return;
    grid->Shutdown();
    grid->Release();
    grid = nullptr;
}

}  // namespace

std::uint32_t ParseCrimeFilterData(const std::vector<std::uint8_t>& values) {
    // @ 0x1001ff54d. The empty case stores 0xffffffff and falls through to the store of 0 (kept).
    std::uint32_t mask = 0;
    for (std::uint8_t v : values) {
        if (v == 0) return 0xffffffff;
        mask |= 1u << ((v - 1u) & 0x1f);
    }
    return mask;
}

int TruncateFloatX86(float v) {
    if (!(v < 2147483648.0f && v >= -2147483648.0f)) return INT_MIN;   // also NaN
    return static_cast<int>(v);
}

int RoundFloatMagic(float v) {
    const float biased = v + 12582912.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &biased, sizeof(bits));
    return static_cast<int>(bits) - 0x4b400000;
}

std::uint8_t AverageInCellRectU8(const cSC4SimGrid<std::uint8_t>& grid, int x, int z, int w, int h) {
    const int shift = grid.GetTractShift();
    const auto& data = grid.GetGridData();
    const int tx = x >> shift, tz = z >> shift;
    if (tx == ((w - 1 + x) >> shift) && tz == ((z + h - 1) >> shift)) return data[tx][tz];
    std::int64_t sum = 0;
    for (int i = x; i < x + w; ++i)
        for (int j = z; j < z + h; ++j) sum += data[i >> shift][j >> shift];
    const double average = static_cast<double>(sum) / static_cast<double>(h * w);
    // cvttsd2si: out-of-range / NaN give INT_MIN (area 0 divides by zero)
    const int truncated = (average < 2147483648.0 && average >= -2147483648.0) ? static_cast<int>(average) : INT_MIN;
    return static_cast<std::uint8_t>(truncated);
}

void AddRadiusEffectU8(std::vector<std::vector<std::uint8_t>>& grid, int width, int height, int cx, int cz, float radius,
                       float peak, float edge) {
    // @ 0x1000d7098. For radii below 16 the original reads the distances from a lookup table; it holds the same
    // float distances the else branch computes, so one path serves both.
    const int r = RoundFloatMagic(radius);
    const int x0 = std::max(cx - r, 0);
    const int x1 = std::min(cx + r, width - 1);
    const int z0 = std::max(cz - r, 0);
    const int z1 = std::min(cz + r, height - 1);
    const float slope = (peak - edge) / radius;
    for (int x = x0; x <= x1; ++x) {
        for (int z = z0; z <= z1; ++z) {
            const float dx = static_cast<float>(x - cx), dz = static_cast<float>(z - cz);
            const float dzz = dz * dz;
            const float distance = std::sqrt(dzz + dx * dx);
            if (!(distance < radius)) continue;
            const float falloff = distance * slope;
            int value = RoundFloatMagic(peak - falloff) + grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)];
            if (value < 0) value = 0;
            if (value > 0xfe) value = 0xff;
            grid[static_cast<std::size_t>(x)][static_cast<std::size_t>(z)] = static_cast<std::uint8_t>(value);
        }
    }
}

// ---- crime type registry --------------------------------------------------------------------------------

void CrimeTypeRegistry::LoadAll(ICrimeHost& host) {
    types_.clear();   // @ 0x1001fd15c: frees the old list, then loads every exemplar of type 0x6534284a group 0xea12f32c
    for (const cGZPersistResourceKey& key : host.ListCrimeTypeExemplars()) LoadFromExemplar(host, key);
}

void CrimeTypeRegistry::LoadFromExemplar(ICrimeHost& host, const cGZPersistResourceKey& key) {
    cISCPropertyHolder* exemplar = host.LoadExemplar(key);   // @ 0x1001ff2b8
    if (!exemplar) return;
    CrimeType* type = nullptr;
    for (auto& existing : types_)
        if (existing->id == key.instance) { type = existing.get(); break; }
    if (!type) {
        types_.push_back(std::make_unique<CrimeType>());
        type = types_.back().get();
    }
    // reset to defaults (the id and name are rewritten below; the visual key is only replaced when the property exists)
    const cGZPersistResourceKey keepVisual = type->visual;
    *type = CrimeType();
    type->visual = keepVisual;
    type->id = key.instance;
    std::string name;
    if (host.LoadExemplarString(key.group, key.instance, kCrimeTypeNameStringGroup, name)) {
        type->name = name;
    } else {
        cRZString fallback;
        if (cSCPropertyHelp::GetPropertyValue(exemplar, kCrimeTypeName, fallback)) type->name = fallback.str();
    }
    cSCPropertyHelp::GetPropertyValue(exemplar, kCrimeTypeVisual, type->visual);
    std::vector<std::uint8_t> filter;
    auto readFilter = [&](std::uint32_t id, std::uint32_t& out) {
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, filter)) out = ParseCrimeFilterData(filter);
    };
    readFilter(kCrimeTypeZoneFilter, type->filter[0]);
    readFilter(kCrimeTypeWealthFilter, type->filter[1]);
    readFilter(kCrimeTypePurposeFilter, type->filter[2]);
    readFilter(kCrimeTypeBuildingWealthFilter, type->filter[3]);
    cSCPropertyHelp::GetPropertyValue(exemplar, kCrimeTypeSeverity, type->severity);
    std::uint8_t arson = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, kCrimeTypeArson, arson)) type->arson = arson != 0;
    cSCPropertyHelp::GetPropertyValue(exemplar, kCrimeTypeWeight, type->weight);
    host.ReleaseExemplar(exemplar);
}

// ---- lifecycle -------------------------------------------------------------------------------------------

cSC4CrimeSimulator::cSC4CrimeSimulator(ICrimeHost* host, CrimeTypeRegistry* registry) : host_(host), registry_(registry) {}

cSC4CrimeSimulator::~cSC4CrimeSimulator() {
    ClearCrimeEvents();
    ReleaseGrid(crimeGrid_);
    ReleaseGrid(effectGrid_);
}

bool cSC4CrimeSimulator::Init(const cISCPropertyHolder* tunables) {
    if (initialized_) return false;
    initialized_ = true;
    if (!registry_->initialized) {   // StaticInit @ 0x1001fd0a2
        registry_->initialized = true;
        registry_->LoadAll(*host_);
    }
    ReadSimulatorTunableValues(tunables);
    if (!crimeGrid_) crimeGrid_ = NewGrid(tunables_.tractSize, kCrimeGridInstance);
    if (!effectGrid_) effectGrid_ = NewGrid(tunables_.tractSize, kCrimeEffectGridInstance);
    if (!loaded_) {
        const int countX = crimeGrid_->GetTractCountX(), countZ = crimeGrid_->GetTractCountZ();
        iterator_.SetRect(0, 0, countX < 1 ? 0 : countX - 1, countZ < 1 ? 0 : countZ - 1);
        host_->RegisterPropRequester();   // @ 0x00173b20: spPropDeveloper vslot 0x14 (this + 8)
    }
    hotspotX_ = hotspotZ_ = 0;
    hotspotValue_ = 0;
    crimeMultiplier_ = 1.0f;
    return true;
}

bool cSC4CrimeSimulator::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        ReleaseGrid(crimeGrid_);
        ReleaseGrid(effectGrid_);
        ClearCrimeEvents();
    }
    return true;
}

void cSC4CrimeSimulator::ClearCrimeEvents() {
    for (CrimeEvent* event : events_) {
        if (!event) continue;
        if (event->lot) event->lot->Release();
        delete event;
    }
    events_.clear();
}

void cSC4CrimeSimulator::ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar) {
    tunables_.tractSize = 2;
    tunables_.decay = 0.5f;
    tunables_.crimeFactor = 1.0f;
    if (!exemplar) return;
    std::int32_t tract = 0;
    if (cSCPropertyHelp::GetPropertyValue(exemplar, 0x47e2c200, tract)) tunables_.tractSize = tract;
    LoadCurve(shareCurve_, exemplar, 0x49a34301);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x8a0cbaac, tunables_.decay);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x8a0cbaad, tunables_.crimeFactor);
    const std::vector<SC4Point> residentDefault{{0.0f, 0.1f}, {255.0f, 0.0f}};
    const std::vector<SC4Point> factorDefault{{1.0f, 0.0f}, {2.0f, 0.1f}, {5.0f, 0.5f}};
    for (int i = 0; i < 3; ++i) {
        residentCurve_[i].SetResponseCurveData(residentDefault);
        LoadCurve(residentCurve_[i], exemplar, 0x8a0cbaa4 + i);
        factorCurve_[i].SetResponseCurveData(factorDefault);
        LoadCurve(factorCurve_[i], exemplar, 0x8a0cbaa7 + i);
    }
    criminalCurve_.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {1000000.0f, 50000.0f}});
    LoadCurve(criminalCurve_, exemplar, 0x8a0cbaaa);
    chanceCurve_.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 0.0f}, {255.0f, 1.0f}});
    LoadCurve(chanceCurve_, exemplar, 0x8a0cbaab);
}

// ---- messages --------------------------------------------------------------------------------------------

bool cSC4CrimeSimulator::DoMessage(std::uint32_t message, std::uint32_t data1, std::uint32_t data2, ICrimeOccupant* occupant) {
    switch (message) {
        case kCrimeMsgIdle:
        case kCrimeMsgIdleAlt:
            DoIdle();
            return true;
        case kCrimeMsgMonthDone:
            if (step_ == kIdle) step_ = kWaiting;
            return true;
        case kCrimeMsgOccupantAdded:
        case kCrimeMsgOccupantRemoved: {
            cISCPropertyHolder* holder = occupant ? occupant->PropertyHolder() : nullptr;
            if (holder && holder->HasProperty(kCrimeNonCrimeOccupantProperty)) return true;
            DoOccupantChanged(occupant, message == kCrimeMsgOccupantAdded);
            return true;
        }
        case kCrimeMsgOrdinanceChanged:
            crimeMultiplier_ = host_->OrdinanceFactor(kCrimeOrdinanceId);
            return true;
        case kCrimeMsgExemplarReloaded:
            if (data1 == kCrimeTunableGroup && data2 == kCrimeTunableInstance) {
                cGZPersistResourceKey key{kCrimeTypeExemplarType, kCrimeTunableGroup, kCrimeTunableInstance};
                cISCPropertyHolder* exemplar = host_->LoadExemplar(key);
                ReadSimulatorTunableValues(exemplar);
                if (exemplar) host_->ReleaseExemplar(exemplar);
            } else if (data1 == kCrimeTypeExemplarGroup) {
                registry_->LoadAll(*host_);
            }
            return true;
        default:
            return true;
    }
}

void cSC4CrimeSimulator::DoIdle() {
    switch (step_) {
        case kWaiting:
            if (initialized_) step_ = kCensus;
            break;
        case kCensus:
            DoCensus();
            CalculateEconomicFactors();
            step_ = kCriminalBase;
            break;
        case kCriminalBase:
            criminalBase_ = ComputeCriminalBase();
            step_ = kCrimesToCommit;
            break;
        case kCrimesToCommit: {
            const std::uint32_t jailed = static_cast<std::uint32_t>(host_->PoliceJailPopulation());
            const int unjailed = host_->PoliceResetUnjailedCriminals();
            const std::uint32_t base = static_cast<std::uint32_t>(criminalBase_);
            const int gap = static_cast<int>(base - jailed);
            std::uint32_t crimes;
            const float scaled = static_cast<float>(unjailed) * tunables_.crimeFactor;
            if (base < jailed || gap == 0) {
                crimes = static_cast<std::uint32_t>(TruncateFloatX86(scaled));
            } else {
                const float extra = criminalCurve_.GetOutputValue(static_cast<float>(gap));
                crimes = static_cast<std::uint32_t>(TruncateFloatX86(scaled) + TruncateFloatX86(extra));
            }
            const float product = static_cast<float>(crimes) * crimeMultiplier_;
            crimesToAdd_ = RoundFloatMagic(product);
            step_ = kApplyCrimes;
            break;
        }
        case kApplyCrimes:
            DecayCrimeGrid();
            ClearCrimeEvents();
            monthlyCrimes_ = AddCrimes(crimesToAdd_);
            host_->PoliceProcessMonthlyCrimeStats();
            ComputeAverageCrime();
            step_ = kPost;
            break;
        case kPost:
            host_->PostCrimesUpdated();
            step_ = kIdle;
            break;
        case kIdle:
            break;
    }
}

void cSC4CrimeSimulator::DoOccupantChanged(ICrimeOccupant* occupant, bool added) {
    cISCPropertyHolder* holder = occupant ? occupant->PropertyHolder() : nullptr;
    if (!holder || !effectGrid_ || !holder->HasProperty(kCrimeBuildingEffectProperty)) return;
    std::vector<std::uint8_t> bytes;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kCrimeBuildingEffectProperty, bytes) || bytes.size() < 2) return;
    const std::uint32_t signedStrength = added ? bytes[0] : 0u - static_cast<std::uint32_t>(bytes[0]);
    const float strength = static_cast<float>(static_cast<int>(signedStrength));
    const float radius = static_cast<float>(bytes[1]) / static_cast<float>(effectGrid_->GetTractSize());
    float position[3];
    occupant->Position(position);
    int tx = 0, tz = 0;
    effectGrid_->PositionToTract(position[0], position[2], tx, tz);
    AddRadiusEffectU8(effectGrid_->GetGridData(), effectGrid_->GetTractCountX(), effectGrid_->GetTractCountZ(), tx, tz,
                      radius, strength, 0.0f);
}

// ---- monthly steps ---------------------------------------------------------------------------------------

void cSC4CrimeSimulator::DoCensus() {
    ICrimeCensusGrid* populations[3] = {};
    ICrimeCensusGrid* values[3] = {};
    for (int wealth = 0; wealth < 3; ++wealth) host_->ResidentialGrids(wealth, populations[wealth], values[wealth]);
    for (int wealth = 0; wealth < 3; ++wealth) {
        for (int band = 0; band < 4; ++band) census_[wealth][band] = 0;
        ICrimeCensusGrid* population = populations[wealth];
        ICrimeCensusGrid* value = values[wealth];
        if (!population || !value) continue;
        const int countX = population->CountX(), countZ = population->CountZ();
        for (int x = 0; x < countX; ++x) {
            for (int z = 0; z < countZ; ++z) {
                const int band = TruncateFloatX86(value->Value(x, z) / 50.0f);
                // the original indexes row*4 + band without a check; bands past 3 land in the next row's counters
                const long flat = static_cast<long>(wealth) * 4 + band;
                if (flat < 0 || flat >= 12) continue;
                census_[flat / 4][flat % 4] += population->Population(x, z);
            }
        }
    }
}

void cSC4CrimeSimulator::CalculateEconomicFactors() {
    for (int wealth = 0; wealth < 3; ++wealth) {
        float sumA = 0.0f, sumB = 0.0f, sumC = 0.0f;
        int index = 0;
        int id = kDemandFactors[wealth][0];
        do {
            float a = 0, b = 0, c = 0;
            host_->DemandValues(id, a, b, c);
            sumA = sumA + a;
            sumB = sumB + b;
            sumC = sumC + c;
            id = kDemandFactors[wealth][++index];
        } while (id != 0);
        const float f = (sumA - sumC) / (sumB - sumC);
        factors_[wealth] = (f + f) + -1.0f;
    }
}

int cSC4CrimeSimulator::ComputeCriminalBase() const {
    // The original returns this in eax (Ghidra shows void): per wealth level the residents of each value band weighted
    // by their curve, plus all residents weighted by the factor curve, truncated per level.
    int total = 0;
    for (int wealth = 0; wealth < 3; ++wealth) {
        float accumulated = 0.0f;
        int residents = 0;
        for (int band = 0; band < 4; ++band) {
            const int count = static_cast<int>(census_[wealth][band]);
            const float curve = residentCurve_[wealth].GetOutputValue(static_cast<float>(band * 25));
            const float product = static_cast<float>(count) * curve;
            accumulated = accumulated + product;
            residents += count;
        }
        const float factorCurve = factorCurve_[wealth].GetOutputValue(factors_[wealth]);
        const float product = static_cast<float>(residents) * factorCurve;
        accumulated = accumulated + product;
        total += TruncateFloatX86(accumulated);
    }
    return total;
}

void cSC4CrimeSimulator::DecayCrimeGrid() {
    if (!crimeGrid_) return;
    auto& data = crimeGrid_->GetGridData();
    const int countX = crimeGrid_->GetTractCountX(), countZ = crimeGrid_->GetTractCountZ();
    for (int x = 0; x < countX; ++x) {
        for (int z = 0; z < countZ; ++z) {
            const std::uint8_t v = data[x][z];
            if (v != 0) data[x][z] = static_cast<std::uint8_t>(TruncateFloatX86(static_cast<float>(v) * tunables_.decay));
        }
    }
    if (hotspotX_ >= 0 && hotspotX_ < countX && hotspotZ_ >= 0 && hotspotZ_ < countZ) hotspotValue_ = data[hotspotX_][hotspotZ_];
}

int cSC4CrimeSimulator::AddCrimes(int count) {
    int added = 0;
    if (!crimeGrid_ || !effectGrid_) return 0;
    auto& data = crimeGrid_->GetGridData();
    const cSC4SimGrid<std::uint8_t>* police = host_->PoliceCoverageGrid();
    for (int n = 0; n < count; ++n) {
        ICrimeLot* lot = host_->RandomLot();
        if (!lot) continue;
        const int shift = crimeGrid_->GetTractShift();
        int x0, z0, x1, z1;
        lot->GetBounds(x0, z0, x1, z1);
        const int w = (1 - x0) + x1, h = (1 - z0) + z1;
        const int policing = police ? AverageInCellRectU8(*police, x0, z0, w, h) : 0;
        const int effect = AverageInCellRectU8(*effectGrid_, x0, z0, w, h);
        const float chance = chanceCurve_.GetOutputValue(static_cast<float>(policing - effect));
        const double roll = host_->RandomDouble();
        if (static_cast<double>(chance) <= roll) {
            const std::uint8_t severity = AddCrimeEvent(lot);
            if (severity != 0) {
                x0 >>= (shift & 0x1f);
                z0 >>= (shift & 0x1f);
                x1 >>= (shift & 0x1f);
                z1 >>= (shift & 0x1f);
                for (int x = x0; x <= x1; ++x) {
                    for (int z = z0; z <= z1; ++z) {
                        unsigned v = static_cast<unsigned>(data[x][z]) + severity;
                        if (v > 0xfe) v = 0xff;
                        data[x][z] = static_cast<std::uint8_t>(v);
                        if (hotspotValue_ < static_cast<int>(v)) {
                            hotspotValue_ = static_cast<int>(v);
                            hotspotX_ = x;
                            hotspotZ_ = z;
                        }
                    }
                }
                ++added;
            }
        }
    }
    return added;
}

void cSC4CrimeSimulator::ComputeAverageCrime() {
    if (!crimeGrid_) return;
    const auto& data = crimeGrid_->GetGridData();
    const int countX = crimeGrid_->GetTractCountX(), countZ = crimeGrid_->GetTractCountZ();
    const int size = crimeGrid_->GetTractSize();
    long sum = 0, tracts = 0;
    int cellX = 0;
    for (int x = 0; x < countX; ++x, cellX += size) {
        int cellZ = 0;
        for (int z = 0; z < countZ; ++z, cellZ += size) {
            if (host_->ZoneTypeAt(cellX, cellZ) != 0) {
                sum += data[x][z];
                ++tracts;
            }
        }
    }
    averageCrime_ = static_cast<float>(sum) / static_cast<float>(tracts);   // 0/0 = NaN when nothing is zoned
}

void cSC4CrimeSimulator::GetLotBits(ICrimeLot* lot, std::uint32_t& zone, std::uint32_t& buildingClass,
                                    std::uint32_t& buildingSubclass, std::uint32_t& lotWealth) {
    zone = buildingClass = buildingSubclass = lotWealth = 0x80000000;
    int cx = 0, cz = 0;
    lot->GetCell(cx, cz);
    const std::uint32_t zoneType = static_cast<std::uint32_t>(host_->ZoneTypeAt(cx, cz)) - 1u;
    if (zoneType < 9) zone = kZoneBits[zoneType];
    const std::uint32_t wealth = lot->Wealth();
    if (wealth < 7) lotWealth = kWealthBits[wealth];
    ICrimeBuilding* building = lot->Building();
    if (!building) {
        buildingSubclass = 0x100;
        return;
    }
    const std::int32_t* info = building->ClassAndSubclass();
    buildingClass = 1u << ((static_cast<std::uint32_t>(info[1]) - 1u) & 0x1f);
    buildingSubclass = info[0] != 0 ? 1u << ((static_cast<std::uint32_t>(info[0]) - 1u) & 0x1f) : 0x100u;
}

std::uint8_t cSC4CrimeSimulator::AddCrimeEvent(ICrimeLot* lot) {
    std::uint32_t bits[4];
    GetLotBits(lot, bits[0], bits[1], bits[2], bits[3]);
    const auto& types = registry_->Types();
    std::vector<std::uint32_t> candidates;
    float totalWeight = 0.0f;
    for (std::uint32_t i = 0; i < types.size(); ++i) {
        const CrimeType& type = *types[i];
        if ((type.filter[0] & bits[0]) && (type.filter[1] & bits[1]) && (type.filter[2] & bits[2]) && (type.filter[3] & bits[3])) {
            candidates.push_back(i);
            totalWeight = totalWeight + type.weight;
        }
    }
    if (candidates.empty()) return 0;
    float remaining = static_cast<float>(host_->RandomDoubleRange(0.0, static_cast<double>(totalWeight)));
    const CrimeType* chosen = nullptr;
    if (remaining >= 0.0f) {
        std::size_t next = 0;
        do {
            if (next >= candidates.size()) break;
            chosen = types[candidates[next++]].get();
            remaining = remaining - chosen->weight;
        } while (remaining >= 0.0f);
    }
    if (!chosen) chosen = types[candidates[0]].get();   // the original would use an unset pointer for a negative roll
    auto* event = new CrimeEvent();
    if (lot) {
        lot->AddRef();
        event->lot = lot;
    }
    event->typeId = chosen->id;
    event->description = chosen->name;
    event->visual = chosen->visual;
    event->visualsCreated = false;
    events_.push_back(event);
    if (chosen->arson) {
        ICrimeBuilding* building = lot->Building();
        if (building) host_->StartArson(lot, building);
    }
    host_->PostCrimeEvent(event->typeId, event->lot, event->description);
    return chosen->severity;
}

// ---- queries ---------------------------------------------------------------------------------------------

int cSC4CrimeSimulator::GetRequestedShare(int x0, int z0, int x1, int z1) const {
    const std::uint8_t average = AverageInCellRectU8(*crimeGrid_, x0, z0, x1 - x0, z1 - z0);
    const float share = shareCurve_.GetOutputValue(static_cast<float>(average) / 255.0f);
    return RoundFloatMagic(share);
}

std::uint32_t cSC4CrimeSimulator::SelectProp(const std::vector<std::uint32_t>& candidates, int x, int z) {
    ICrimeLot* lot = host_->LotAt(x, z);
    std::vector<std::uint32_t> anyCrime, matching;
    anyCrime.reserve(candidates.size());
    matching.reserve(candidates.size());
    for (std::uint32_t prop : candidates) {
        const int crimeId = host_->PropCrimeTypeId(prop);
        if (crimeId == 0) {
            anyCrime.push_back(prop);
            continue;
        }
        for (const CrimeEvent* event : events_) {
            if (static_cast<int>(event->typeId) == crimeId && event->lot == lot) {
                matching.push_back(prop);
                break;
            }
        }
    }
    if (!matching.empty()) return matching[host_->RandomUint32(static_cast<std::uint32_t>(matching.size()))];
    if (anyCrime.empty()) return 0;
    return anyCrime[host_->RandomUint32(static_cast<std::uint32_t>(anyCrime.size()))];
}

bool cSC4CrimeSimulator::GetCrimeHotspot(std::uint8_t* value, int* x, int* z) const {
    if (hotspotValue_ < 1) return false;
    if (value) *value = static_cast<std::uint8_t>(hotspotValue_);
    if (x) *x = crimeGrid_->GetTractSize() * hotspotX_;
    if (z) *z = crimeGrid_->GetTractSize() * hotspotZ_;
    return true;
}

bool cSC4CrimeSimulator::GetCrimeIncidentArray(std::vector<CrimeIncident>& out) const {
    out.resize(events_.size());
    for (std::size_t i = 0; i < events_.size(); ++i) {
        const CrimeEvent* event = events_[i];
        if (!event || !event->lot) continue;
        float position[2] = {0, 0};
        event->lot->GetPosition(position[0], position[1]);
        CrimeIncident& incident = out[i];
        host_->LotPositionToIncidentXZ(position[0], position[1], incident.x, incident.z);
        incident.visual = event->visual;
        incident.typeId = event->typeId;
        incident.visualsCreated = event->visualsCreated ? 1 : 0;
    }
    return !events_.empty();
}

bool cSC4CrimeSimulator::SetVisualsCreated(std::uint32_t index, bool created) {
    if (index >= events_.size()) return false;
    events_[index]->visualsCreated = created;
    return true;
}

bool cSC4CrimeSimulator::GetVisualsCreated(std::uint32_t index) const {
    return index < events_.size() && events_[index]->visualsCreated;
}

namespace {
float Distance3(const float a[3], const float b[3]) {
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return std::sqrt(dz * dz + dx * dx + dy * dy);
}
}  // namespace

bool IsCrimePropOccupant(ICrimeOccupant* occupant) {
    // tCrimePropOccupantFilter::IsOccupantIncluded @ 0x1001fff04
    if (occupant->OccupantType() != kCrimePropOccupantType) return false;
    cISCPropertyHolder* holder = occupant->PropertyHolder();
    return holder && holder->HasProperty(kCrimePropFilterProperty);
}

bool IsCrimeSimOccupant(ICrimeOccupant* occupant) {
    // tCrimeSimOccupantFilter::IsOccupantIncluded @ 0x1001fff82
    return occupant->OccupantType() == kCrimeSimOccupantType && occupant->IsOccupantGroup(kCrimeSimOccupantGroup);
}

void* cSC4CrimeSimulator::GetNearestCrimeProp(const float position[3], float radius) {
    // @ 0x1001ff8f6: occupants in the box +-radius (y unbounded) that are available department buildings (the
    // tCrimePropOccupantFilter lets through props with property 0x8aa864d5); the nearest within `radius` wins.
    const float lo[3] = {position[0] - radius, -3.4028235e+38f, position[2] - radius};
    const float hi[3] = {position[0] + radius, 3.4028235e+38f, position[2] + radius};
    std::vector<ICrimeOccupant*> found = host_->OccupantsInBox(lo, hi);
    void* best = nullptr;
    float limit = radius;
    for (ICrimeOccupant* occupant : found) {
        if (!IsCrimePropOccupant(occupant)) continue;
        float p[3];
        occupant->Position(p);
        const float distance = Distance3(p, position);
        if (distance >= limit) continue;
        if (void* prop = occupant->IdlePropOccupant()) {
            limit = distance;
            best = prop;
        }
    }
    host_->ReleaseOccupants(found);
    return best;
}

void* cSC4CrimeSimulator::GetNearestCrimeSim(const float position[3], float radius) {
    // @ 0x1001ffbd4: same search for sim occupants that answer interface 0xa9b40f05 (the first one found within the
    // shrinking radius wins, so the last closer candidate is kept).
    const float lo[3] = {position[0] - radius, -3.4028235e+38f, position[2] - radius};
    const float hi[3] = {position[0] + radius, 3.4028235e+38f, position[2] + radius};
    std::vector<ICrimeOccupant*> found = host_->OccupantsInBox(lo, hi);
    void* best = nullptr;
    float limit = radius;
    for (ICrimeOccupant* occupant : found) {
        if (!IsCrimeSimOccupant(occupant)) continue;
        float p[3];
        occupant->Position(p);
        const float distance = Distance3(p, position);
        if (distance >= limit) continue;
        if (void* automaton = occupant->CrimeAutomaton()) {
            best = automaton;
            limit = distance;
        }
    }
    host_->ReleaseOccupants(found);
    return best;
}

// ---- streams ---------------------------------------------------------------------------------------------

bool cSC4CrimeSimulator::Write(cIGZOStream& stream) const {
    stream.SetUint16(kStreamVersion);
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(crimeGrid_));
    SC4WriteGZSerializable(stream, static_cast<const cISC4SimGridBase*>(effectGrid_));
    stream << iterator_;
    stream.SetUint32(unknownA0_);
    stream.SetUint32(unknownA4_);
    stream.SetFloat32(averageCrime_);
    stream.SetFloat32(crimeMultiplier_);
    stream.SetUint32(static_cast<std::uint32_t>(monthlyCrimes_));
    stream.SetSint32(hotspotX_);
    stream.SetSint32(hotspotZ_);
    stream.SetSint32(hotspotValue_);
    stream.SetSint32(static_cast<std::int32_t>(events_.size()));
    for (const CrimeEvent* event : events_) {
        if (!event) continue;
        host_->WriteLot(stream, event->lot);
        stream.SetUint32(event->typeId);
        cRZString description(event->description.c_str());
        stream.SetGZStr(description);
        stream << event->visual;
    }
    return !stream.GetError();
}

bool cSC4CrimeSimulator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kStreamVersion) return false;
    ReleaseGrid(crimeGrid_);
    ReleaseGrid(effectGrid_);
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, kSimGridIid, &raw, false);
    crimeGrid_ = SimGridCast<std::uint8_t>(raw);
    raw = nullptr;
    SC4ReadGZSerializable(stream, kSimGridIid, &raw, false);
    effectGrid_ = SimGridCast<std::uint8_t>(raw);
    stream >> iterator_;
    stream.GetUint32(unknownA0_);
    stream.GetUint32(unknownA4_);
    stream.GetFloat32(averageCrime_);
    stream.GetFloat32(crimeMultiplier_);
    std::uint32_t monthly = 0;
    stream.GetUint32(monthly);
    monthlyCrimes_ = static_cast<int>(monthly);
    stream.GetSint32(hotspotX_);
    stream.GetSint32(hotspotZ_);
    stream.GetSint32(hotspotValue_);
    std::int32_t count = 0;
    stream.GetSint32(count);
    for (std::int32_t i = 0; i < count; ++i) {
        auto* event = new CrimeEvent();
        // in a city file the lot is an object reference: the lot manager resolves it later, here it is consumed
        void* dbStream = nullptr;
        if (stream.QueryInterface(kGZIID_cISC4DBSegmentIStream, &dbStream) && dbStream) {
            static_cast<cIGZUnknown*>(dbStream)->Release();
            void* lot = nullptr;
            SC4ReadGZSerializable(stream, 0xe818fced, &lot, false);
            if (lot) static_cast<cIGZUnknown*>(lot)->Release();
            event->lot = nullptr;
        } else {
            event->lot = host_->ReadLot(stream);
        }
        stream.GetUint32(event->typeId);
        cRZString description;
        stream.GetGZStr(description);
        event->description = description.str();
        stream >> event->visual;
        event->visualsCreated = false;
        events_.push_back(event);
    }
    loaded_ = true;
    return !stream.GetError();
}

}  // namespace sc4
