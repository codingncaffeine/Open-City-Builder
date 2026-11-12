#include "sc4/SpecialVehicles.h"

#include <vector>

#include "gz/StreamOps.h"
#include "sc4/SCPropertyHelp.h"

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

// i386 0x004b1536: always true, whatever the vehicle's Init answers.
bool cSC4MayorLimo::Init() {
    cSC4Vehicle::Init();
    std::vector<float> values;
    cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kMayorLimoTuningProperty, values);
    if (!values.empty()) cost_ = static_cast<std::uint32_t>(static_cast<std::int32_t>(values[0]));
    if (values.size() > 1) auraRadius_ = values[1];
    if (values.size() > 2) auraMagnitude_ = values[2];
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

bool cSC4Bus::Write(cIGZOStream& stream) const {
    stream.SetUint16(2);
    return cSC4Vehicle::Write(stream);
}

bool cSC4Bus::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    return version == 2 && cSC4Vehicle::Read(stream);
}

// 0x00391418: leaving the network at a bus stop (building group 0x1301) the bus takes a path of the tile and stands
// there for 4.8 seconds.
bool cSC4Bus::HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect) {
    if (lot && vehicleHost_ && vehicleHost_->IsLotBuildingInGroup(lot, kBusStationGroup) && ChooseRandomPath(occupant, key)) {
        PushState(kTrafficStateStopAtStation, nullptr, kBusStationStopMilliseconds, false, 0, 0, nullptr);
        return true;
    }
    return cSC4TrafficAutomaton::HandleExitNetwork(occupant, key, lot, rect);
}

// 0x0039155e: a bus that is still rolling in, or has not reached the stop, ticks as any traffic automaton. Standing at the
// stop its wait runs down; on half of the ticks it lets one waiting pedestrian (behavior 4) within 16 units board, and
// drives on as soon as nobody is left to board.
bool cSC4Bus::DoStopAtStationTick(float milliseconds) {
    if (!CheckAutomataFlag(kTrafficFlagStopped) && !CheckAutomataFlag(kAutomataFlagReachedDestination)) {
        cSC4TrafficAutomaton::DoStopAtStationTick(milliseconds);
        return true;
    }
    tAutomataState* state = GetState();
    if (!state) return true;   // port guard
    if (state->timer <= milliseconds) return cSC4TrafficAutomaton::DoStopAtStationTick(milliseconds);
    state->timer -= milliseconds;
    if (!Host() || Host()->RandomBelow(2) != 0) return true;
    const float lo[3] = {position_.x - kBusNearbyPedRadius, -3.4028235e+38f, position_.z - kBusNearbyPedRadius};
    const float hi[3] = {kBusNearbyPedRadius + position_.x, 3.4028235e+38f, kBusNearbyPedRadius + position_.z};
    std::vector<cISC4Occupant*> nearby;
    if (vehicleHost_) vehicleHost_->OccupantsInBox(lo, hi, 0x496e5783, -1, nearby);
    bool boarded = false;
    for (cISC4Occupant* occupant : nearby) {
        if (!occupant) continue;
        void* raw = nullptr;
        if (!boarded && occupant->QueryInterface(kAutomatonIid, &raw) && raw) {
            auto* pedestrian = static_cast<cSC4Automaton*>(raw);
            boarded = pedestrian->GetBehavior() == 4;
            if (boarded) pedestrian->Remove(false);
            pedestrian->Release();
        }
        occupant->Release();
    }
    if (!boarded) PopState();
    return true;
}

}  // namespace sc4
