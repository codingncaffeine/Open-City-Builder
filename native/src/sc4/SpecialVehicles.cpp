#include "sc4/SpecialVehicles.h"

#include "gz/StreamOps.h"

namespace sc4 {

namespace {
IGetawayVanHost* g_getawayVanHost = nullptr;
}
IGetawayVanHost* GetawayVanHost() { return g_getawayVanHost; }
void SetGetawayVanHost(IGetawayVanHost* host) { g_getawayVanHost = host; }

// Mac i386 0x0049d9a8: the carjacked car leaves town at once, with the struggling figures on it and the alarm sound.
bool cSC4CarjackedVehicle::PostInit() {
    if (!cSC4Automaton::PostInit()) return false;
    SkipTown();
    AttachAnim(kCarjackingSimsModelKey, static_cast<s3d::PlayMode>(1), true, 5.0f);
    StartSFX(kCarjackingSfx);
    return true;
}

bool cSC4CarjackedVehicle::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    return cSC4Vehicle::Write(stream);
}

bool cSC4CarjackedVehicle::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 1 && cSC4Vehicle::Read(stream);
}

// Mac i386 0x0039189c: the robbers get away when the van is the running mission's target, or when the police power at its
// cell is zero or no higher than the crime there; without the two grids a coin decides. Either way the van leaves town with
// the matching figures on it.
bool cSC4GetawayVan::PostInit() {
    if (!cSC4Automaton::PostInit()) return false;
    IGetawayVanHost* city = GetawayVanHost();
    bool escapes = false;
    std::uint8_t police = 0, crime = 0;
    if (city && city->IsSituationTarget(AsOccupant())) {
        escapes = true;
    } else if (city && city->PoliceAndCrimeAt(static_cast<int>(Position().x * 0.0625f), static_cast<int>(0.0625f * Position().z), police,
                                              crime)) {
        escapes = !(police != 0 && crime < police);
    } else {
        escapes = Host() ? Host()->RandomBelow(2) == 0 : false;   // RandomUint32Uniform() < 0x7fffffff
    }
    escapes_ = escapes;
    SkipTown();
    AttachAnim(escapes ? kGetawayEscapeModelKey : kGetawayCaughtModelKey, static_cast<s3d::PlayMode>(1), true, 5.0f);
    return true;
}

bool cSC4GetawayVan::Write(cIGZOStream& stream) const {
    stream.SetUint16(3);
    const bool ok = cSC4Vehicle::Write(stream);
    if (ok) {
        stream << false;             // the bool, always false
        stream.SetSint32(0);
    }
    return ok;
}

bool cSC4GetawayVan::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != 3 || !cSC4Vehicle::Read(stream)) return false;
    bool flag = false;
    stream >> flag;
    std::int32_t value = 0;
    stream.GetSint32(value);
    return true;
}

bool cSC4MayorLimo::Write(cIGZOStream& stream) const {
    stream.SetUint16(1);
    return cSC4Vehicle::Write(stream);
}

bool cSC4MayorLimo::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 1 && cSC4Vehicle::Read(stream);
}

}  // namespace sc4
