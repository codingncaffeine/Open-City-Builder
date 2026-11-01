#include "sc4/cSC4Demand.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sc4 {

namespace {
float FloatFromBits(std::uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
}  // namespace

bool cSC4Demand::Init() {
    if (initialized_) return true;
    initialized_ = true;
    if (!wasRead_) {
        supply_ = demand_ = newSupply_ = newDemand_ = extraA_ = 0;
        extraB_ = 0;
        activeMax_ = FloatFromBits(0x7e7fffff);
        activeMin_ = FloatFromBits(0xfe7fffff);
        active_ = 0;
        dirty_ = true;
        economyModifier_ = 0;
        taxModifier_ = 0;
    }
    return host_ != nullptr || true;
}

bool cSC4Demand::Shutdown() {
    if (initialized_) initialized_ = false;
    return true;
}

bool cSC4Demand::SimulationBegin() {
    if (!wasRead_) CalculateActiveDemand(false);
    float scaled = active_ >= 0.0f ? (active_ * 1000.0f) / activeMax_ : (active_ * -1000.0f) / activeMin_;
    if (host_) host_->PostMessage(kDemandChangedMessage, id_, static_cast<std::int32_t>(scaled));
    return true;
}

bool cSC4Demand::EndOfCycle() {
    float addedSupply = newSupply_, addedDemand = newDemand_;
    supply_ += addedSupply;
    demand_ += addedDemand;
    newSupply_ = 0;
    newDemand_ = 0;
    CalculateActiveDemand(false);
    return addedDemand != 0.0f || addedSupply != 0.0f;
}

void cSC4Demand::CalculateActiveDemand(bool smooth) {
    const float max = activeMax_, min = activeMin_;
    float net = demand_ - supply_;
    float target = max;
    if (net <= max) target = net < min ? min : net;
    const float modifier = economyModifier_ + taxModifier_;
    float scaledMax = max * modifier;
    float scaledDemand = demand_ * modifier;
    if (scaledMax <= scaledDemand) scaledDemand = scaledMax;
    float value = scaledDemand + target;
    float clamped = max;
    if (value <= max) clamped = value < min ? min : value;
    float ceiling = max - demandCap_ * max;
    if (clamped <= ceiling) ceiling = clamped;
    float result = min;
    if (min <= ceiling) result = ceiling;

    if (smooth) {
        float step = ((max - min) * 7.0f) / 100.0f;
        float current = active_;
        float delta = result - current;
        if (step <= delta || delta <= -step) {
            bool snapToZero = false;
            if (delta <= 0.0f) {
                if (result < 0.0f) {
                    result = 0.0f;
                    if (0.0f < current && 0.0f < current - step) snapToZero = true;
                }
                if (!snapToZero) result = current - step;
            } else {
                if (0.0f < result) {
                    result = 0.0f;
                    if (current < 0.0f && step + current < 0.0f) snapToZero = true;
                }
                if (!snapToZero) result = step + current;
            }
        }
    }
    active_ = result;

    float factor = result < 0.0f ? -1000.0f : 1000.0f;
    float divisor = result >= 0.0f ? max : min;
    float scaled = (result * factor) / divisor;
    // the original rounds with the 1.5 * 2^23 trick: round to nearest, ties to even
    std::int32_t rounded = static_cast<std::int32_t>(std::nearbyintf(scaled));
    if (host_) host_->PostMessage(kDemandChangedMessage, id_, rounded);
}

bool cSC4Demand::Write(cIGZOStream& s) const {
    s.SetUint16(2);
    s.SetUint32(id_);
    s.SetFloat32(supply_);
    s.SetFloat32(demand_);
    s.SetFloat32(newSupply_);
    s.SetFloat32(newDemand_);
    s.SetFloat32(extraA_);
    s.SetFloat32(extraB_);
    s.SetFloat32(activeMin_);
    s.SetFloat32(activeMax_);
    s.SetFloat32(active_);
    s.SetUint8(dirty_ ? 1 : 0);                 // the byte between the nine values and the two modifiers (+0x40)
    s.SetFloat32(economyModifier_);
    s.SetFloat32(taxModifier_);
    s.SetFloat32(demandCap_);
    return !s.GetError();
}

bool cSC4Demand::Read(cIGZIStream& s) {
    wasRead_ = true;
    std::uint16_t version = 0;
    s.GetUint16(version);
    if (version >= 3) return false;
    s.GetUint32(id_);
    s.GetFloat32(supply_);
    s.GetFloat32(demand_);
    s.GetFloat32(newSupply_);
    s.GetFloat32(newDemand_);
    s.GetFloat32(extraA_);
    s.GetFloat32(extraB_);
    s.GetFloat32(activeMin_);
    s.GetFloat32(activeMax_);
    s.GetFloat32(active_);
    std::uint8_t dirty = 0;
    s.GetUint8(dirty);                          // +0x40
    dirty_ = dirty != 0;
    s.GetFloat32(economyModifier_);
    s.GetFloat32(taxModifier_);
    if (version == 1) {
        std::uint8_t percent = 0;
        s.GetUint8(percent);
        demandCap_ = static_cast<float>(percent) * 0.01f;
    } else {
        s.GetFloat32(demandCap_);
    }
    if (id_ == 0x21010 && std::getenv("SC4_DEMANDDEBUG"))
        std::fprintf(stderr, "demand: object %p read %05x v%u demand %g active %g error %d\n", static_cast<void*>(this), id_, version, demand_, active_, s.GetError() ? 1 : 0);
    return !s.GetError();
}

}  // namespace sc4
