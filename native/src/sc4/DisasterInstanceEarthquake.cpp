#include "sc4/DisasterInstanceEarthquake.h"

#include <cmath>

#include "gz/cIGZMessage2.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

IEarthquakeHost* g_host = nullptr;
EarthquakeTuning g_tuning;
bool g_tuningRead = false;          // sbInited
bool g_slopeRolled = false;         // sbFaultLineSlopeInited
float g_faultLineSlope = 0.0f;      // sFaultLineSlope

// The float -> int idiom of the binary (value - 0.5 + 12582912, the mantissa as the integer, one more when the value is whole).
int FloorIdiom(float value) {
    return static_cast<int>(std::floor(value));
}

}  // namespace

IEarthquakeHost* EarthquakeHost() { return g_host; }
void SetEarthquakeHost(IEarthquakeHost* host) { g_host = host; }

const EarthquakeTuning& cSC4DisasterInstanceEarthquake::Tuning() {
    if (g_tuningRead) return g_tuning;
    g_tuningRead = true;
    const cISCPropertyHolder* exemplar = g_host ? g_host->TuningExemplar() : nullptr;
    if (!exemplar) return g_tuning;
    const auto curve = [exemplar](std::uint32_t id, cSC4ResponseCurve& out) {   // a float array of x, y pairs
        std::vector<float> values;
        if (cSCPropertyHelp::GetPropertyValue(exemplar, id, values)) out.SetResponseCurveData(values.data(), values.size());
    };
    curve(0x4a08c500, g_tuning.brushStrengthAtEpicenterVsIntensity);
    curve(0x4a08c501, g_tuning.brushWidthAtEpicenterVsIntensity);
    curve(0x4a08c502, g_tuning.brushStrengthVsDistance);
    curve(0x4a08c503, g_tuning.brushWidthVsDistance);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4a08c504, g_tuning.distBetweenEffects);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4a08c505, g_tuning.faultLineCurviness);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4a08c506, g_tuning.maxFaultLineSlopeVariation);
    g_tuning.maxFaultLineSlopeVariation = static_cast<float>(static_cast<double>(g_tuning.maxFaultLineSlopeVariation) * 0.017453292519943295);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4a08c507, g_tuning.brushTravelSpeed);
    g_tuning.brushTravelSpeed = g_tuning.brushTravelSpeed / 1000000.0f;
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4a08c508, g_tuning.microSecsBetweenShakeEffects);
    cSCPropertyHelp::GetPropertyValue(exemplar, 0x4a08c509, g_tuning.distBetweenAutomataRepulsors);
    return g_tuning;
}

void cSC4DisasterInstanceEarthquake::ResetStatics() {
    g_tuningRead = false;
    g_slopeRolled = false;
    g_tuning = EarthquakeTuning{};
}

cSC4DisasterInstanceEarthquake::~cSC4DisasterInstanceEarthquake() {
    if (initialised_) Shutdown();
}

std::uint32_t cSC4DisasterInstanceEarthquake::Release() {
    if (refCount_ < 2) {
        delete this;
        return 0;
    }
    return --refCount_;
}

bool cSC4DisasterInstanceEarthquake::QueryInterface(std::uint32_t iid, void** out) {
    *out = nullptr;
    if (iid == kGZIID_cISC4DisasterInstance || iid == 1) *out = static_cast<cISC4DisasterInstance*>(this);
    if (!*out) return false;
    AddRef();
    return true;
}

// 0x00340a8a: the epicentre (clamped into the city), the fault line through it, the start effect; the instance ticks from now
// on, starts in the "button held" state and is registered with the disaster layer at its cell.
void cSC4DisasterInstanceEarthquake::Init(int x, int z) {
    Tuning();
    initialised_ = true;
    IDisasterHost* city = DisasterHost();
    maxX_ = g_host ? g_host->CitySizeX() - 1.0f : 0.0f;
    maxZ_ = g_host ? g_host->CitySizeZ() - 1.0f : 0.0f;
    cISC4DisasterLayer* layer = city ? city->DisasterLayer() : nullptr;
    manager_ = layer ? layer->GetDisasterManager(kDisasterManagerIdEarthquake) : nullptr;
    if (manager_) manager_->AddRef();
    epicenter_.x = static_cast<float>(x);
    epicenter_.y = static_cast<float>(z);
    if (!(0.0f <= epicenter_.x)) epicenter_.x = 0.0f;
    else if (maxX_ < epicenter_.x) epicenter_.x = maxX_;
    if (!(0.0f <= epicenter_.y)) epicenter_.y = 0.0f;
    else if (maxZ_ < epicenter_.y) epicenter_.y = maxZ_;
    ComputeFaultLine();
    if (g_host) g_host->AddTickable(this, kEarthquakeTickPriority);
    CreateParticleEffect("QuakeStart", epicenter_, false);
    shakeTime_ = 0;
    SetState(1);
    if (layer) {
        int cellX = 0, cellZ = 0;
        if (city) city->PositionToCell(epicenter_.x, epicenter_.y, cellX, cellZ);
        layer->RegisterDisasterLocation(this, cellX, cellZ, nullptr, 0, false);
        layer->DisasterHasBegun(this);
    }
}

// 0x0033fed0
bool cSC4DisasterInstanceEarthquake::Shutdown() {
    if (!initialised_) return true;
    initialised_ = false;
    ReleaseAutomataRepulsors();
    for (void*& effect : effect_) {
        if (effect && g_host) g_host->ReleaseEffect(effect);
        effect = nullptr;
    }
    if (g_host) g_host->RemoveTickable(this, kEarthquakeTickPriority);
    if (manager_) {
        cISC4DisasterManager* manager = manager_;
        manager_ = nullptr;
        manager->Release();
    }
    return true;
}

// 0x0034040c
void cSC4DisasterInstanceEarthquake::SetState(int state) {
    if (state == state_) return;
    stateTime_ = 0;
    state_ = state;
    if (state == 2) StartDeformation();
    else if (state == 4) EndEarthquakeAction();
    else if (state == 1) PlaceAutomataRepulsors();
}

// 0x0034057a: the animation tick carries the elapsed microseconds in its second data word.
bool cSC4DisasterInstanceEarthquake::DoMessage(cIGZMessage2* message) {
    if (!message || message->GetType() != kAnimationTickMessage) return true;
    void* raw = nullptr;
    if (!message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) || !raw) return true;
    auto* tick = static_cast<cIGZMessage2Standard*>(raw);
    const std::uint32_t microseconds = static_cast<std::uint32_t>(tick->GetData2());
    tick->Release();
    DoAnimationTick(microseconds);
    return true;
}

// 0x003404f4
void cSC4DisasterInstanceEarthquake::DoAnimationTick(std::uint32_t microseconds) {
    stateTime_ += microseconds;
    if (state_ == 2) {
        if (DoDeformation(microseconds)) {
            state_ = 3;   // not through SetState: the time in the state runs on
            if (g_host) g_host->EndLongTerrainOperation();
        }
    } else if (state_ == 3) {
        if (kEarthquakeDoneMicroseconds < stateTime_) SetState(4);
    } else if (state_ == 1) {
        DoMouseDownUpdate(microseconds);
    }
}

// 0x00340450: while the button is held the intensity grows by 0.1 a second up to 1, with a shake effect every so often.
void cSC4DisasterInstanceEarthquake::DoMouseDownUpdate(std::uint32_t microseconds) {
    if (intensity_ < 1.0f) {
        const float grown = intensity_ + static_cast<float>(microseconds) * 1e-07f;
        intensity_ = grown <= 1.0f ? grown : 1.0f;
    }
    shakeTime_ += microseconds;
    if (Tuning().microSecsBetweenShakeEffects <= shakeTime_) {
        CreateParticleEffect("QuakerShaker", epicenter_, false);
        shakeTime_ = 0;
    }
}

// 0x0033f1b8: a cubic Bezier through four control points.
void cSC4DisasterInstanceEarthquake::EvalBezier(const std::vector<cS3DVector2>& points, float t, cS3DVector2& out) {
    const float u = 1.0f - t;
    const float b0 = u * u * u, b1 = u * u * 3.0f * t, b2 = u * 3.0f * t * t, b3 = t * t * t;
    out.x = b3 * points[3].x + b2 * points[2].x + b1 * points[1].x + b0 * points[0].x;
    out.y = b0 * points[0].y + b1 * points[1].y + b2 * points[2].y + b3 * points[3].y;
}

// 0x003405b6: the line's direction (the session's slope, varied a little), then for each side the epicentre and three
// control points a third of the city further along each, every one pushed sideways by a random curviness.
void cSC4DisasterInstanceEarthquake::ComputeFaultLine() {
    const EarthquakeTuning& tuning = Tuning();
    const auto roll = [](double lo, double hi) { return g_host ? static_cast<float>(g_host->RandomRange(lo, hi)) : static_cast<float>(lo); };
    if (!g_slopeRolled) {
        g_faultLineSlope = roll(0.0, 3.1415927410125732);
        g_slopeRolled = true;
    }
    const float variation = roll(static_cast<double>(-tuning.maxFaultLineSlopeVariation), static_cast<double>(tuning.maxFaultLineSlopeVariation));
    slopeCos_ = std::cos(g_faultLineSlope + variation);
    slopeSin_ = std::sin(g_faultLineSlope + variation);
    const float sideX = -slopeSin_, sideZ = slopeCos_;   // the line's normal
    const float third = (1.0f + maxX_) / 3.0f;
    const float stepX = third * slopeCos_, stepZ = third * slopeSin_;
    faultLine_[0].assign(1, epicenter_);
    faultLine_[1].assign(1, epicenter_);
    float ax = epicenter_.x, az = epicenter_.y, bx = epicenter_.x, bz = epicenter_.y;
    for (int i = 0; i < 3; ++i) {
        float pushA = roll(static_cast<double>(-tuning.faultLineCurviness), static_cast<double>(tuning.faultLineCurviness));
        // the first pair shares one roll, mirrored; the other two pairs roll each side
        float pushB = i == 0 ? -pushA : roll(static_cast<double>(-tuning.faultLineCurviness), static_cast<double>(tuning.faultLineCurviness));
        ax -= stepX; az -= stepZ;
        bx += stepX; bz += stepZ;
        faultLine_[0].push_back(cS3DVector2{sideX * pushA + ax, pushA * sideZ + az});
        faultLine_[1].push_back(cS3DVector2{sideX * pushB + bx, pushB * sideZ + bz});
    }
}

// 0x0033ee7e: the curves are walked in steps of `step` world units of a third-of-the-city segment.
void cSC4DisasterInstanceEarthquake::ResetFaultLineIterator(float step) {
    lineDone_[0] = lineDone_[1] = false;
    iteratorT_ = 0.0f;
    iteratorStep_ = step / ((g_host ? g_host->CitySizeX() : 1.0f) * 3.0f);
}

// 0x0033f3b4: the next point of each curve that is still inside the city; a curve that leaves the city is done. False
// once both are done.
bool cSC4DisasterInstanceEarthquake::GetNextPointsOnFaultLine(PointOnFaultLine& a, PointOnFaultLine& b) {
    if (lineDone_[0] && lineDone_[1]) return false;
    PointOnFaultLine* out[2] = {&a, &b};
    for (int side = 0; side < 2; ++side) {
        out[side]->valid = !lineDone_[side];
        if (!out[side]->valid) continue;
        EvalBezier(faultLine_[side], iteratorT_, out[side]->point);
        const cS3DVector2& p = out[side]->point;
        if (p.x < 0.0f || maxX_ < p.x || p.y < 0.0f || maxZ_ < p.y) {
            out[side]->valid = false;
            lineDone_[side] = true;
        }
    }
    iteratorT_ += iteratorStep_;
    return true;
}

void* cSC4DisasterInstanceEarthquake::CreateParticleEffect(const char* name, const cS3DVector2& at, bool keep) {
    if (!g_host) return nullptr;
    const float xyz[3] = {at.x, g_host->Altitude(at.x, at.y), at.y};
    void* effect = g_host->CreateEffect(name, xyz);
    if (effect && !keep) {
        g_host->ReleaseEffect(effect);
        effect = nullptr;
    }
    return effect;
}

void cSC4DisasterInstanceEarthquake::PlaceParticleEffect(void* effect, const cS3DVector2& at) {
    if (!g_host || !effect) return;
    const float xyz[3] = {at.x, g_host->Altitude(at.x, at.y), at.y};
    g_host->MoveEffect(effect, xyz);
}

// 0x00340392 / 0x003402c0: a repulsor every so many units along both curves.
void cSC4DisasterInstanceEarthquake::PlaceAutomataRepulsors() {
    if (!g_host) return;
    PointOnFaultLine a, b;
    ResetFaultLineIterator(Tuning().distBetweenAutomataRepulsors);
    while (GetNextPointsOnFaultLine(a, b)) {
        for (const PointOnFaultLine* point : {&a, &b}) {
            if (!point->valid) continue;
            const float xyz[3] = {point->point.x, g_host->Altitude(point->point.x, point->point.y), point->point.y};
            if (void* controller = g_host->AddRepulsor("quake_repulsor", xyz)) repulsors_.push_back(controller);
        }
    }
}

void cSC4DisasterInstanceEarthquake::ReleaseAutomataRepulsors() {
    for (void* controller : repulsors_)
        if (g_host) g_host->RemoveRepulsor(controller);
    repulsors_.clear();
}

// 0x0033fa9c: the brushes start at the epicentre with the strength and width the intensity gives, each with a dust effect.
void cSC4DisasterInstanceEarthquake::StartDeformation() {
    const EarthquakeTuning& tuning = Tuning();
    distance_ = 0.0f;
    travel_ = 0.0f;
    epicenterStrength_ = tuning.brushStrengthAtEpicenterVsIntensity.GetOutputValue(intensity_);
    epicenterWidth_ = tuning.brushWidthAtEpicenterVsIntensity.GetOutputValue(intensity_);
    previous_[0].valid = previous_[1].valid = false;
    brush_[0].zoom = 0;
    brush_[0].valid = false;
    brush_[0].redisplayFlags = 3;
    brush_[1] = brush_[0];
    effect_[0] = CreateParticleEffect("Quaker", epicenter_, true);
    effect_[1] = CreateParticleEffect("Quaker", epicenter_, true);
    sinceEffect_[0] = sinceEffect_[1] = 0.0f;
    ResetFaultLineIterator(kEarthquakeStep);
    if (g_host) g_host->BeginLongTerrainOperation();
}

// 0x0033f4ac: the brushes advance by the distance the elapsed time allows, a step at a time: each step both brushes are
// applied from their previous point to the next point of their curve, with the strength and width for the distance so far.
// True when the quake is spent (both curves left the city, or the brush faded out).
bool cSC4DisasterInstanceEarthquake::DoDeformation(std::uint32_t microseconds) {
    const EarthquakeTuning& tuning = Tuning();
    if (!g_host) return true;
    std::int32_t touched[2][4] = {{INT_MAX, INT_MAX, INT_MIN, INT_MIN}, {INT_MAX, INT_MAX, INT_MIN, INT_MIN}};
    travel_ = static_cast<float>(microseconds) * tuning.brushTravelSpeed + travel_;
    const int steps = FloorIdiom((travel_ - distance_) * 0.125f);
    for (int step = 0; step < steps; ++step) {
        PointOnFaultLine next[2];
        if (!GetNextPointsOnFaultLine(next[0], next[1])) return true;
        const float strength = tuning.brushStrengthVsDistance.GetOutputValue(distance_) * epicenterStrength_;
        brush_[0].strengthScale = brush_[1].strengthScale = strength;
        const float width = tuning.brushWidthVsDistance.GetOutputValue(distance_) * epicenterWidth_;
        brush_[0].radiusScale = brush_[1].radiusScale = width;
        if (strength < kEarthquakeMinBrushStrength) return true;
        if (width < kEarthquakeMinBrushWidth) return true;
        for (int side = 0; side < 2; ++side) {
            if (next[side].valid) {
                EarthquakeBrush& brush = brush_[side];
                brush.currentX = next[side].point.x;
                brush.currentZ = next[side].point.y;
                if (!previous_[side].valid) {
                    brush.previousX = brush.currentX;
                    brush.previousZ = brush.currentZ;
                } else {
                    brush.previousX = previous_[side].point.x;
                    brush.previousZ = previous_[side].point.y;
                }
                g_host->ApplyBrush(side == 0 ? kEarthquakeBrushA : kEarthquakeBrushB, brush);
                if (brush.rect[0] <= touched[side][0]) touched[side][0] = brush.rect[0];
                if (brush.rect[1] <= touched[side][1]) touched[side][1] = brush.rect[1];
                if (touched[side][2] <= brush.rect[2]) touched[side][2] = brush.rect[2];
                if (touched[side][3] <= brush.rect[3]) touched[side][3] = brush.rect[3];
                if (tuning.distBetweenEffects <= sinceEffect_[side]) {
                    PlaceParticleEffect(effect_[side], next[side].point);
                    sinceEffect_[side] = 0.0f;
                }
            }
        }
        distance_ += kEarthquakeStep;
        sinceEffect_[0] += kEarthquakeStep;
        sinceEffect_[1] += kEarthquakeStep;
        previous_[0] = next[0];
        previous_[1] = next[1];
    }
    g_host->StartTextureUpdates();
    for (int side = 0; side < 2; ++side)
        if (touched[side][0] <= touched[side][2] && touched[side][1] <= touched[side][3]) g_host->RedisplayTerrain(touched[side]);
    return false;
}

// 0x0033f13a: the terrain's texture updates are let go, the layer forgets the quake and counts it ended, the manager drops
// the instance, and the instance shuts down.
void cSC4DisasterInstanceEarthquake::EndEarthquakeAction() {
    if (g_host) g_host->StopTextureUpdates();
    IDisasterHost* city = DisasterHost();
    if (cISC4DisasterLayer* layer = city ? city->DisasterLayer() : nullptr) {
        layer->UnregisterDisasterLocations(this);
        layer->DisasterHasEnded();
    }
    AddRef();
    if (manager_) manager_->RemoveInstanceFromList(this);
    Shutdown();
    Release();
}

}  // namespace sc4
