#include "sc4/AutomataGenerator.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sc4/GeometryUtilities.h"

namespace sc4 {

namespace {
// DAT_100a184d0: seconds-of-day units per period unit {minute, hour, day, week} in controller hours
constexpr float kPeriodUnits[4] = {0.016666668f, 1.0f, 24.0f, 168.0f};
constexpr float kTwoPi = 6.2831854820251465f;   // as a double: the float 2*pi
constexpr std::uint32_t kMaxAttempts = 0x19;
}  // namespace

int AutomataRoundMagic(float v) {
    const float biased = v + 12582912.0f;
    std::uint32_t bits;
    std::memcpy(&bits, &biased, sizeof(bits));
    return static_cast<int>(bits) - 0x4b400000;
}

int AutomataFloorIdiom(float x) {
    const float shifted = x + -0.5f;
    const int n = AutomataRoundMagic(shifted);
    const int n1 = n + 1;
    const float fn1 = static_cast<float>(n1);
    if (x != fn1 || std::isnan(x) || std::isnan(fn1)) return n;
    return n1;
}

// @ 0x1000f5aae. The folded delta is added to the stored hours without re-normalising (kept).
ControllerTime& ControllerTime::operator+=(float delta) {
    if (delta < 0.0f) {
        std::int16_t d = days;
        do {
            delta = delta + 24.0f;
            d = static_cast<std::int16_t>(d - 1);
        } while (delta < 0.0f);
        days = d;
    }
    if (24.0f <= delta) {
        std::int16_t d = days;
        do {
            delta = delta + -24.0f;
            d = static_cast<std::int16_t>(d + 1);
        } while (24.0f <= delta);
        days = d;
    }
    time = delta + time;
    return *this;
}

// @ 0x1000fa3e0 (and the 0x1000fa4e6 complete-object wrapper)
cSC4AutomataGenerator::cSC4AutomataGenerator(IAutomataGeneratorHost* host) : host_(host), random_(0xffffffffu) {}

// @ 0x1000fbae0 / 0x1000fbaea (deleting version); the remaining teardown is cSC4AutomataController's destructor.
cSC4AutomataGenerator::~cSC4AutomataGenerator() {}

std::uint32_t cSC4AutomataGenerator::Release() {   // @ 0x1000fbac0
    if (1 < refCount_) return --refCount_;
    FinalRelease();
    return 0;
}

void cSC4AutomataGenerator::FinalRelease() {   // @ 0x1000fbb9c
    Shutdown();
    delete this;
}

bool cSC4AutomataGenerator::Init() {   // @ 0x1000fa4f0
    if (!initialized) return host_->BaseInit();
    return false;
}

bool cSC4AutomataGenerator::Shutdown() {   // @ 0x1000fa504
    if (initialized) return host_->BaseShutdown();
    return false;
}

// @ 0x1000fa518: deactivating a generator that is not asked to keep its automata releases them all.
void cSC4AutomataGenerator::Activate(bool active) {
    if (initialized && !active && tmpl_ && tmpl_->type == kAutomataGeneratorTemplateType) {
        if (host_->CheckFlag(kAutoFlagReleaseOnDeactivate)) host_->BaseReleaseAutomata();
        host_->BaseSyncLiveCount();
    }
    host_->BaseActivate(active);
}

// @ 0x1000fa5be: writes into the (shared) template.
void cSC4AutomataGenerator::SetAutomataGroups(const std::vector<std::uint32_t>& groups) {
    if (tmpl_ && tmpl_->type == kAutomataGeneratorTemplateType && &tmpl_->groups != &groups) tmpl_->groups = groups;
}

void cSC4AutomataGenerator::SetTemplate(GeneratorTemplate* tmpl) {   // @ 0x1000fa5f2
    tmpl_ = tmpl;
    if (!tmpl) return;
    host_->BaseSetTemplate();
    if (tmpl->type == kAutomataGeneratorTemplateType) {
        lifetimeLimit_ = tmpl->lifetimeAutomataLimit;
        groupLimit_ = static_cast<std::uint32_t>(random_.RandomSint32RangeUniform(tmpl->groupSizeMin, tmpl->groupSizeMax));
    }
}

// @ 0x1000fa66e: the first parameter is unused.
void cSC4AutomataGenerator::CheckTimeOfDay(const ControllerTime&, const ControllerTime& delta) {
    elapsed_ += delta.time;
    elapsed_.days = static_cast<std::int16_t>(elapsed_.days + delta.days);
}

// @ 0x1000fbb40
bool cSC4AutomataGenerator::IsExpired() const {
    const float lifetime = host_->BaseLifetime();
    if (lifetime <= 0.0f || host_->BaseAge() < lifetime) {
        if (lifetimeLimit_ == 0) return false;
        return lifetimeLimit_ <= spawned_;
    }
    return true;
}

// @ 0x1000fa6a4
void cSC4AutomataGenerator::DoAnimationTick(float dt, std::uint32_t arg, const ControllerTime& time) {
    host_->BaseDoAnimationTick(dt, arg, time);
    if (IsExpired()) return;
    if (host_->CheckFlag(kAutoFlagSuspended)) return;
    if (!host_->CheckFlag(kAutoFlagVisible)) return;
    if (!host_->IsAutomataLayerVisible()) return;

    elapsed_ += time.time;
    elapsed_.days = static_cast<std::int16_t>(elapsed_.days + time.days);
    // due when period <= elapsed (days first, then hours)
    if (!(period_.days <= elapsed_.days && (period_.days < elapsed_.days || period_.time <= elapsed_.time))) return;
    GeneratorTemplate* tmpl = tmpl_;
    if (!tmpl) return;   // the original dereferences it

    std::uint32_t limit = 0xffffffffu;
    limit = groupLimit_;
    if (limit < 2) limit = tmpl->maxAutomata == 0 ? 0xffffffffu : tmpl->maxAutomata;
    static const bool trace = std::getenv("SC4_GENERATORDUE") != nullptr;   // why a due generator does or does not spawn
    if (host_->BaseLiveCount() < limit) {
        float now = 0;
        std::int32_t day = 0;
        host_->ClockNow(now, day);
        if ((tmpl->dayMask >> (static_cast<std::uint32_t>(day) & 0x1f)) & 1u) {
            const float curve = tmpl->spawnCurve.GetOutputValue(now);
            const double roll = random_.RandomDoubleUniform();
            const float magnitude = curve < 0.0f ? -curve : curve;
            if (trace)
                std::fprintf(stderr, "generator %p due: live %u of %u, clock %.2f day %d, curve %.3f, roll %.2f of %.2f\n", static_cast<void*>(this),
                             host_->BaseLiveCount(), limit, now, day, curve, roll, tmpl->probability);
            if (0.0f < magnitude && static_cast<float>(roll) <= tmpl->probability) TriggerGenerator(curve);
        } else if (trace) {
            std::fprintf(stderr, "generator %p due: day %d is not in its day mask %x\n", static_cast<void*>(this), day, tmpl->dayMask);
        }
    } else if (trace) {
        std::fprintf(stderr, "generator %p due: full, %u of %u\n", static_cast<void*>(this), host_->BaseLiveCount(), limit);
    }
    float period = 0.0f;
    if (static_cast<std::uint64_t>(static_cast<std::int64_t>(tmpl->periodUnit)) < 4)
        period = kPeriodUnits[tmpl->periodUnit] / tmpl->rate;
    period_.time = 0.0f;
    period_.days = 0;
    period_ += period;
    elapsed_.time = 0.0f;
    elapsed_.days = 0;
}

// @ 0x1000fb1e6: cells whose network matches `mask` inside the ring between the minimum and maximum radius around the
// occupant's bounding box (16-unit cells); a facing occupant limits the ring to its front side.
void cSC4AutomataGenerator::GetNetworksInRadius(std::vector<CellPoint>& out, std::uint32_t mask) {
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};
    host_->GetOccupantBoundingBox(bmin, bmax);
    const float minR = tmpl_->minRadius, maxR = tmpl_->radius;

    int outerMinX = AutomataFloorIdiom((bmin[0] - maxR) * 0.0625f);
    int outerMinZ = AutomataFloorIdiom((bmin[2] - maxR) * 0.0625f);
    int outerMaxX = AutomataFloorIdiom((bmax[0] + maxR) * 0.0625f);
    int outerMaxZ = AutomataFloorIdiom((bmax[2] + maxR) * 0.0625f);
    const float inner = (maxR != minR) ? minR : 0.0f;   // cmpneqss mask & minR
    int innerMinX = AutomataFloorIdiom((bmin[0] - inner) * 0.0625f);
    int innerMinZ = AutomataFloorIdiom((bmin[2] - inner) * 0.0625f);
    int innerMaxX = AutomataFloorIdiom((bmax[0] + inner) * 0.0625f);
    int innerMaxZ = AutomataFloorIdiom((inner + bmax[2]) * 0.0625f);

    if (host_->CheckFlag(kAutoFlagLotFacing) && host_->HasOccupant()) {
        switch (host_->OccupantFacing()) {
            case 0: outerMaxZ = innerMinZ; innerMaxZ = innerMinZ; break;
            case 1: outerMinX = innerMaxX; innerMinX = innerMaxX; break;
            case 2: outerMinZ = innerMaxZ; innerMinZ = innerMaxZ; break;
            case 3: outerMaxX = innerMinX; innerMaxX = innerMinX; break;
            default: break;
        }
    }
    for (int z = outerMinZ; z <= outerMaxZ; ++z) {
        for (int x = outerMinX; x <= outerMaxX; ++x) {
            if (!host_->NetworkCellHasType(x, z, mask)) continue;
            if (innerMaxZ <= z || z <= innerMinZ || x <= innerMinX || innerMaxX <= x) out.push_back(CellPoint{x, z});
        }
    }
}

// @ 0x1000fb5e0
void cSC4AutomataGenerator::ChooseAutomataGroup() {
    const std::vector<std::uint32_t>& groups = tmpl_->groups;
    std::uint32_t count = static_cast<std::uint32_t>(groups.size());
    std::uint32_t index = 0;
    if (1 < count) index = random_.RandomUint32Uniform(count);
    group_ = groups.empty() ? 0 : groups[index];   // an empty list reads through a null pointer in the original
    groupSpawned_ = 0;
    groupLimit_ = static_cast<std::uint32_t>(random_.RandomSint32RangeUniform(tmpl_->groupSizeMin, tmpl_->groupSizeMax));
}

// @ 0x1000fa896
void cSC4AutomataGenerator::TriggerGenerator(float curveValue) {
    GeneratorTemplate* tmpl = tmpl_;
    if (!tmpl) return;
    std::uint32_t count = 0;
    if (tmpl->countMode == 2) {
        count = static_cast<std::uint32_t>(AutomataRoundMagic(static_cast<float>(host_->CityCount()) * tmpl->countScale));
        if (count == 0) return;
    } else if (tmpl->countMode == 1) {
        if (!host_->HasOccupant() || host_->OccupantType() != kBuildingOccupantType) return;
        float measure = 0.0f;
        bool have = true;
        if (host_->OccupantHasFlag(0x1503) || host_->OccupantHasFlag(0x1504)) {
            measure = host_->OccupantMeasureA();
        } else if (host_->OccupantHasFlag(0x1507)) {
            measure = host_->OccupantMeasureB();
        } else {
            int cx = 0, cz = 0;
            host_->OccupantLotCell(cx, cz);
            have = host_->LotMeasure(cx, cz, measure);
        }
        count = have ? static_cast<std::uint32_t>(AutomataRoundMagic(measure * tmpl->countScale)) : 0;
        if (count == 0) return;
    } else if (tmpl->countMode == 0) {
        const std::uint32_t rounded = static_cast<std::uint32_t>(AutomataRoundMagic(curveValue * tmpl->countScale));
        count = rounded != 0 ? rounded : 1;
    } else {
        return;
    }
    const std::uint32_t attempts = count < kMaxAttempts ? count : kMaxAttempts;

    float origin[3] = {0, 0, 0};
    float pos[3] = {0, 0, 0};   // persists across attempts (the list branch keeps the previous y)
    const void* clock = host_->Clock();
    if (!clock) return;
    if (host_->CheckFlag(kAutoFlagLotFacing)) host_->GetLotFacingPoint(origin);
    else host_->GetPosition(origin);
    const float center[2] = {origin[0], origin[2]};

    std::vector<CellPoint> cells;
    std::uint32_t mask = 0;
    if (!host_->CheckFlag(kAutoFlagNoNetworks)) {
        if (host_->CheckFlag(kAutoFlagLocalRoads)) {
            mask = kAutomataNetworkMaskLocal;
            GetNetworksInRadius(cells, mask);
            if (cells.empty()) return;
        }
    } else {
        mask = kAutomataNetworkMaskAll;
        GetNetworksInRadius(cells, mask);
        if (cells.empty()) return;
    }

    std::uint32_t maxLive = groupLimit_;
    if (maxLive < 2) maxLive = tmpl->maxAutomata == 0 ? 0xffffffffu : tmpl->maxAutomata;
    const float zeroVector[3] = {0, 0, 0};
    static const bool trace = std::getenv("SC4_GENERATORDUE") != nullptr;
    if (trace)
        std::fprintf(stderr, "generator %p triggers: %u attempts from (%.0f, %.0f), network mask %x, %zu cells\n", static_cast<void*>(this), attempts,
                     origin[0], origin[2], mask, cells.size());

    for (std::uint32_t attempt = 0; attempt < attempts; ++attempt) {
        if (maxLive < host_->BaseLiveCount()) break;
        float distance = 0.0f;
        bool place = false;
        if (cells.empty()) {
            const double angle = random_.RandomDoubleRangeUniform(0.0, kTwoPi);
            const double length = random_.RandomDoubleRangeUniform(static_cast<double>(tmpl->minRadius), static_cast<double>(tmpl->radius));
            float far[2] = {center[0] + 10000.0f, center[1] + 0.0f};
            SC4GeometryUtilities::RotatePoint2D(center, far, static_cast<float>(angle), far);
            const float dz = far[1] - center[1];
            const float dx = far[0] - center[0];
            float hit[2] = {0, 0};
            const bool intersects = host_->GetBoundingBoxIntersection(center, far, hit);
            const float dist = static_cast<float>(length);
            const float inv = 1.0f / std::sqrt(dz * dz + dx * dx + 0.0f);
            const float ux = dx * inv;
            float baseX, baseZ;
            if (intersects) { baseX = hit[0]; baseZ = hit[1]; }
            else { baseX = origin[0]; baseZ = origin[2]; }
            const float sz = dz * inv * dist + baseZ;
            const float sy = inv * 0.0f * dist + origin[1];
            pos[0] = ux * dist + baseX;
            pos[2] = sz;
            pos[1] = host_->TerrainHeightEnabled(pos[0], pos[2]) ? host_->TerrainHeight(pos[0], pos[2]) : sy;
            distance = dist;
            place = true;
        } else {
            const std::uint32_t index = random_.RandomUint32Uniform(static_cast<std::uint32_t>(cells.size()));
            const int cx = cells[index].x, cz = cells[index].z;
            const double rx = random_.RandomDoubleUniform();
            pos[0] = static_cast<float>(rx * 16.0 + static_cast<double>(cx << 4));
            const double rz = random_.RandomDoubleUniform();
            pos[2] = static_cast<float>(rz * 16.0 + static_cast<double>(cz << 4));
            float to[2] = {pos[0], pos[2]};
            float hit[2] = {0, 0};
            if (host_->GetBoundingBoxIntersection(center, to, hit)) {
                const float length = SC4GeometryUtilities::LengthOfLine2D(to, hit);
                float target = tmpl->minRadius;
                bool clamp = length < target;
                if (!clamp) {
                    target = tmpl->radius;
                    clamp = target < length;
                }
                if (clamp) {
                    const float dxv = to[0] - hit[0];
                    const float dzv = to[1] - hit[1];
                    const float inv = 1.0f / std::sqrt(dzv * dzv + dxv * dxv);
                    const float fx = dxv * inv * target;
                    const float fz = inv * dzv * target;
                    pos[0] = fx + hit[0];
                    pos[2] = fz + hit[1];
                }
            }
            if (host_->NetworkPositionAccepts(pos, mask)) {
                const float y = host_->NetworkHeight(pos[0], pos[2]);
                pos[1] = y;
                const float lz = origin[2] - pos[2], ly = origin[1] - y, lx = origin[0] - pos[0];
                distance = std::sqrt(lz * lz + ly * ly + lx * lx);
                place = true;
            }
        }
        if (!place) {
            if (trace) std::fprintf(stderr, "generator %p: the point (%.0f, %.0f) is not on its networks\n", static_cast<void*>(this), pos[0], pos[2]);
            continue;
        }

        // spawn
        if (group_ == 0 || groupLimit_ <= groupSpawned_) ChooseAutomataGroup();
        IAutomatonManager* manager = host_->ManagerForGroup(group_);
        if (trace && !manager) std::fprintf(stderr, "generator %p: no manager for group %x\n", static_cast<void*>(this), group_);
        if (!manager) break;
        IAutomaton* automaton = manager->CreateAutomaton(group_, pos, kAutomataSpawnFlags, zeroVector, 0, tmpl->spawnArgument);
        if (trace && !automaton) std::fprintf(stderr, "generator %p: the manager made no automaton of group %x\n", static_cast<void*>(this), group_);
        if (!automaton) continue;
        host_->BaseAddAutomaton(automaton);
        ++groupSpawned_;
        automaton->AddCallback(clock);
        if (const void* behavior = host_->GetBehaviorTemplate(distance, pos)) automaton->StartBehavior(behavior, this);
        else automaton->StartDefault(-1.0f, this);
        if (lifetimeLimit_ != 0) {
            ++spawned_;
            if (lifetimeLimit_ <= spawned_) break;
        }
    }
}

// @ 0x1000fb662 (thunk 0x1000fb702)
bool cSC4AutomataGenerator::Write(cIGZOStream& stream) const {
    stream.SetUint16(4);
    host_->BaseWrite(stream);
    stream.SetFloat32(elapsed_.time);
    stream.SetSint16(elapsed_.days);
    stream.SetFloat32(period_.time);
    stream.SetSint16(period_.days);
    stream.SetUint32(group_);
    stream.SetUint32(groupSpawned_);
    return !stream.GetError();
}

// @ 0x1000fb710 (thunk 0x1000fb7c4)
bool cSC4AutomataGenerator::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 4) return false;
    host_->BaseRead(stream);
    stream.GetFloat32(elapsed_.time);
    stream.GetSint16(elapsed_.days);
    stream.GetFloat32(period_.time);
    stream.GetSint16(period_.days);
    stream.GetUint32(group_);
    stream.GetUint32(groupSpawned_);
    return !stream.GetError();
}

}  // namespace sc4
