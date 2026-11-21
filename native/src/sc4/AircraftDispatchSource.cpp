#include "sc4/AircraftDispatchSource.h"

#include <algorithm>
#include <cfloat>
#include <vector>

#include "sc4/CommonIIDs.h"
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {

IAircraftDispatchSourceHost* g_host = nullptr;

constexpr std::uint32_t kSourceMsgOccupantInserted = 0x99ef1142;
constexpr std::uint32_t kSourceMsgOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kSourceMsgDispatchBegin = 0xe94671eb;
constexpr std::uint32_t kSourceMsgDispatchEnd = 0x894671f6;
constexpr std::uint32_t kSourceMsgDispatchActivated = 0xe94671ec;
constexpr std::uint32_t kSourceMsgDispatchDeactivated = 0xe94671ed;
constexpr std::uint32_t kSourceRecordVersion = 1;
constexpr std::uint32_t kOccupantListReadLimit = 10000000;   // operator>>(cIGZIStream&, ilist<cISC4Occupant*>&) @ 0x0009851e

}  // namespace

IAircraftDispatchSourceHost* AircraftDispatchSourceHost() { return g_host; }
void SetAircraftDispatchSourceHost(IAircraftDispatchSourceHost* host) { g_host = host; }

// The destructors (0x004d2752 / 0x004d2788) only tear down the station list; the references were dropped in Shutdown.
cSC4AircraftDispatchSource::~cSC4AircraftDispatchSource() = default;

// ---- cIGZUnknown ----------------------------------------------------------------------------------------------------------

// 0x004d1d3e: the dispatch source (its primary view), the message target and the serializable; nothing else (not even
// cIGZUnknown's own id).
bool cSC4AircraftDispatchSource::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cISC4DispatchSource) *out = static_cast<IDispatchSource*>(this);
    else if (iid == kGZIID_cIGZMessageTarget2) *out = static_cast<cIGZMessageTarget2*>(this);
    else if (iid == kGZIID_cIGZSerializable) *out = static_cast<cIGZSerializable*>(this);
    else return false;
    AddRef();
    return true;
}

// 0x004d256a: the last reference runs FinalRelease.
std::uint32_t cSC4AircraftDispatchSource::Release() {
    if (refCount_ < 2) {
        FinalRelease();
        return 0;
    }
    return --refCount_;
}

// 0x004d253e: Shutdown, then the deleting destructor.
void cSC4AircraftDispatchSource::FinalRelease() {
    Shutdown();
    delete this;
}

// ---- init / shutdown ------------------------------------------------------------------------------------------------------

// 0x004d2322: holds the city and the message server, hears the occupant and dispatch messages and registers with the dispatch
// manager.
bool cSC4AircraftDispatchSource::Init() {
    if (initialized_) return true;
    initialized_ = true;
    if (!g_host) return true;
    if (!city_) {
        city_ = g_host->City();
        if (city_) city_->AddRef();
    }
    if (!messageServer_) {
        messageServer_ = g_host->MessageServer();
        if (messageServer_) {
            messageServer_->AddRef();
            auto* target = static_cast<cIGZMessageTarget2*>(this);
            messageServer_->AddNotification(target, kSourceMsgOccupantInserted);
            messageServer_->AddNotification(target, kSourceMsgOccupantRemoved);
            messageServer_->AddNotification(target, kSourceMsgDispatchBegin);
            messageServer_->AddNotification(target, kSourceMsgDispatchEnd);
            messageServer_->AddNotification(target, kSourceMsgDispatchActivated);
            messageServer_->AddNotification(target, kSourceMsgDispatchDeactivated);
        }
    }
    if (cISC4DispatchManager* manager = g_host->DispatchManager()) manager->RegisterDispatchSource(this);
    return true;
}

// 0x004d241e: leaves the dispatch manager, releases the stations (through the occupant manager), the notifications, the message
// server and the city.
bool cSC4AircraftDispatchSource::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;
    if (g_host) {
        if (cISC4DispatchManager* manager = g_host->DispatchManager()) manager->UnregisterDispatchSource(this);
        g_host->ReleaseOccupantList(stations_);
    }
    if (messageServer_) {
        auto* target = static_cast<cIGZMessageTarget2*>(this);
        messageServer_->RemoveNotification(target, kSourceMsgOccupantInserted);
        messageServer_->RemoveNotification(target, kSourceMsgOccupantRemoved);
        messageServer_->RemoveNotification(target, kSourceMsgDispatchBegin);
        messageServer_->RemoveNotification(target, kSourceMsgDispatchActivated);
        messageServer_->RemoveNotification(target, kSourceMsgDispatchDeactivated);
        messageServer_->RemoveNotification(target, kSourceMsgDispatchEnd);
        messageServer_->Release();
        messageServer_ = nullptr;
    }
    if (city_) {
        city_->Release();
        city_ = nullptr;
    }
    return true;
}

// ---- messages -------------------------------------------------------------------------------------------------------------

// 0x004d1e44: a building inserted / removed (unless it carries the "no automata" property) updates the station list; the
// dispatch manager's messages for this source's type (Data1) run the handler with the dispatch id (Data2). Other types of
// dispatch message are answered as handled; any other message is not.
bool cSC4AircraftDispatchSource::DoMessage(cIGZMessage2* message) {
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    switch (type) {
    case kSourceMsgOccupantInserted:
    case kSourceMsgOccupantRemoved: {
        auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
        if (!occupant) return true;
        if (occupant->AsPropertyHolder()->HasProperty(kDispatchSourceNoAutomataProperty)) return true;
        if (type == kSourceMsgOccupantInserted) DoOccupantInserted(occupant);
        else DoOccupantRemoved(occupant);
        return true;
    }
    case kSourceMsgDispatchBegin:
    case kSourceMsgDispatchEnd:
    case kSourceMsgDispatchActivated:
    case kSourceMsgDispatchDeactivated: {
        if (static_cast<std::uint32_t>(standard->GetData1()) != traits_.type) return true;
        const auto id = static_cast<std::uint32_t>(standard->GetData2());
        if (type == kSourceMsgDispatchBegin) DoDispatchBegin(id);
        else if (type == kSourceMsgDispatchEnd) DoDispatchEnd(id);
        else DoDispatchActivated(id, type == kSourceMsgDispatchActivated);
        return true;
    }
    default:
        return false;
    }
}

// 0x004d2154: reads the dispatch's record and does nothing with it.
void cSC4AircraftDispatchSource::DoDispatchActivated(std::uint32_t id, bool active) {
    (void)active;
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    if (!manager) return;
    DispatchInfo info;
    manager->GetDispatchInfo(id, info);
}

// 0x004d26e8: the unit of a dispatch that still has its record goes home (its bit is set again).
void cSC4AircraftDispatchSource::DoDispatchEnd(std::uint32_t id) {
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    if (!manager) return;
    DispatchInfo info;
    if (manager->GetDispatchInfo(id, info)) ReturnDispatchToStation(info.station, info.callNumber);
}

// ---- stations -------------------------------------------------------------------------------------------------------------

// 0x004d1fc6: the unit count, the mask of the units at home (cut to the count) and the base call number. False when a property
// that is asked for is missing.
bool cSC4AircraftDispatchSource::GetDispatchesAtStation(cISC4Occupant* station, std::uint32_t* mask, std::uint32_t* base,
                                                        std::uint32_t* count) {
    const cISCPropertyHolder* holder = station->AsPropertyHolder();
    std::uint32_t units = 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, traits_.countProperty, units)) return false;
    if (count) *count = units;
    if (mask) {
        std::uint32_t bits = 0;
        if (!cSCPropertyHelp::GetPropertyValue(holder, traits_.maskProperty, bits)) return false;
        *mask = ((1u << (units & 31)) - 1u) & bits;
    }
    if (base) {
        std::uint32_t first = 0;
        if (!cSCPropertyHelp::GetPropertyValue(holder, traits_.baseProperty, first)) return false;
        *base = first;
    }
    return true;
}

void cSC4AircraftDispatchSource::SetStationAvailabilityFlags(cISC4Occupant* station, std::uint32_t mask) {
    cSCPropertyHelp::SetPropertyValue(station->AsPropertyHolder(), traits_.maskProperty, mask);
}

void cSC4AircraftDispatchSource::SetStationBaseCallNumber(cISC4Occupant* station, std::uint32_t base) {
    cSCPropertyHelp::SetPropertyValue(station->AsPropertyHolder(), traits_.baseProperty, base);
}

// 0x004d28c6: a building with units becomes a station (one reference): all its units are home, and its call numbers are the
// first run of `count` numbers from 1 that no other station uses, searched below the next free number (the run may end past
// it). The next free number grows to the run's end.
void cSC4AircraftDispatchSource::DoOccupantInserted(cISC4Occupant* occupant) {
    if (occupant->GetType() != kDispatchSourceBuildingOccupantType) return;
    std::int32_t declared = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), traits_.countProperty, declared);
    if (declared <= 0) return;
    occupant->AddRef();
    stations_.push_back(occupant);
    std::uint32_t count = 0;
    GetDispatchesAtStation(occupant, nullptr, nullptr, &count);
    SetStationAvailabilityFlags(occupant, (1u << (count & 31)) - 1u);

    std::vector<bool> used(nextCallNumber_, false);
    for (cISC4Occupant* other : stations_) {
        if (other == occupant) continue;
        std::uint32_t base = 0, units = 0;
        GetDispatchesAtStation(other, nullptr, &base, &units);
        for (std::uint32_t i = 0; i < units; ++i) {
            if (base + i < used.size()) used[base + i] = true;
        }
    }
    const auto inUse = [&](std::uint32_t number) { return number < used.size() && used[number]; };
    std::uint32_t base = 1;
    while (base < nextCallNumber_) {
        std::uint32_t i = 0;
        while (i < count && !inUse(base + i)) ++i;
        if (i >= count) break;
        base += i + 1;
    }
    SetStationBaseCallNumber(occupant, base);
    nextCallNumber_ = std::max(nextCallNumber_, base + count);
}

// 0x004d217e: a building on the list leaves it (and its reference is dropped).
void cSC4AircraftDispatchSource::DoOccupantRemoved(cISC4Occupant* occupant) {
    if (occupant->GetType() != kDispatchSourceBuildingOccupantType) return;
    const auto it = std::find(stations_.begin(), stations_.end(), occupant);
    if (it == stations_.end()) return;
    stations_.erase(it);
    occupant->Release();
}

// 0x004d2802 over CopyOccupantList 0x004d27b0: the stations, one new reference each; false when there are none.
bool cSC4AircraftDispatchSource::GetStations(std::list<cISC4Occupant*>& out) {
    out.clear();
    for (cISC4Occupant* station : stations_) {
        station->AddRef();
        out.push_back(station);
    }
    return !out.empty();
}

// 0x004d21d8: the units of every station.
int cSC4AircraftDispatchSource::GetMaxPossibleDispatchCount() {
    std::uint32_t units = 0;
    int total = 0;
    for (cISC4Occupant* station : stations_) {
        GetDispatchesAtStation(station, nullptr, nullptr, &units);
        total += static_cast<int>(units);
    }
    return total;
}

// 0x004d2108: false without units or a dispatch manager; otherwise true, with the flag set when the manager already has as many
// dispatches of this type as there are units.
bool cSC4AircraftDispatchSource::CanDispatch(float x, float z, bool* alreadyDispatched) {
    (void)x;
    (void)z;
    const int units = GetMaxPossibleDispatchCount();
    cISC4DispatchManager* manager = g_host ? g_host->DispatchManager() : nullptr;
    if (units < 1 || !manager) return false;
    const auto dispatched = static_cast<std::uint32_t>(manager->GetDispatchCount(traits_.type));
    *alreadyDispatched = static_cast<std::uint32_t>(units) <= dispatched;
    return true;
}

// 0x004d2232: the station with a unit at home nearest to (x, z) on the ground plane sends one (DispatchFromStation with call
// number 0, the first unit at home). Null when no station has a unit at home.
cISC4Occupant* cSC4AircraftDispatchSource::DispatchFromNearestStation(float x, float z, std::uint32_t* callNumber) {
    const float target[2] = {x, z};
    float nearest = FLT_MAX;
    cISC4Occupant* best = nullptr;
    for (cISC4Occupant* station : stations_) {
        std::uint32_t mask = 0;
        GetDispatchesAtStation(station, &mask, nullptr, nullptr);
        if (mask == 0) continue;
        float position[3] = {0.0f, 0.0f, 0.0f};
        station->GetPosition(position);
        const float ground[2] = {position[0], position[2]};
        const float distance = SC4GeometryUtilities::LengthOfLine2D(ground, target);
        if (distance < nearest) {
            nearest = distance;
            best = station;
        }
    }
    *callNumber = 0;
    if (best) DispatchFromStation(best, callNumber);
    return best;
}

// 0x004d2590: call number 0 takes the first unit at home; another number takes that unit (false when it is past the station's
// last unit). The unit's bit is cleared and the call number handed back.
bool cSC4AircraftDispatchSource::DispatchFromStation(cISC4Occupant* station, std::uint32_t* callNumber) {
    std::uint32_t mask = 0, base = 0, count = 0;
    GetDispatchesAtStation(station, &mask, &base, &count);
    std::uint32_t index = 0, bit = 0;
    if (*callNumber == 0) {
        while (index < count) {
            bit = 1u << (index & 31);
            if (bit & mask) break;
            ++index;
        }
    } else {
        index = *callNumber - base;
        if (count < index) return false;
        bit = 1u << (index & 31);
    }
    SetStationAvailabilityFlags(station, mask & ~bit);
    *callNumber = index + base;
    return true;
}

// 0x004d264a: the unit goes home (its bit is set) unless every unit already is or its bit already is set.
bool cSC4AircraftDispatchSource::ReturnDispatchToStation(cISC4Occupant* station, std::uint32_t callNumber) {
    std::uint32_t mask = 0, base = 0, count = 0;
    GetDispatchesAtStation(station, &mask, &base, &count);
    const auto home = static_cast<std::uint32_t>(__builtin_popcount(mask));
    const std::uint32_t bit = 1u << ((callNumber - base) & 31);
    if (home >= count || (bit & mask) != 0) return false;
    SetStationAvailabilityFlags(station, mask | bit);
    return true;
}

// 0x004d272e
void cSC4AircraftDispatchSource::RevokeDispatchFromStation(cISC4Occupant* station, std::uint32_t callNumber) {
    ReturnDispatchToStation(station, callNumber);
}

// 0x004d20d4: the aircraft group of the class, a null resource key and the dispatch flags.
bool cSC4AircraftDispatchSource::GetDispatchAutomataProperties(cISC4Occupant* station, std::uint32_t& group,
                                                               cGZPersistResourceKey& key, std::uint32_t& flags,
                                                               const float position[3]) {
    (void)station;
    (void)position;
    group = traits_.automataGroup;
    key = cGZPersistResourceKey{};
    flags = kDispatchSourceAutomataFlags;
    return true;
}

// ---- streams --------------------------------------------------------------------------------------------------------------

// 0x004d1d98: version 1, the stations (operator<< 0x00097fbe: the count, then each occupant's reference), the next call number.
bool cSC4AircraftDispatchSource::Write(cIGZOStream& stream) const {
    stream.SetUint16(kSourceRecordVersion);
    stream.SetUint32(static_cast<std::uint32_t>(stations_.size()));
    for (cISC4Occupant* station : stations_) SC4WriteGZSerializable(stream, station);
    stream.SetUint32(nextCallNumber_);
    return !stream.GetError();
}

// 0x004d1de8: a version outside the class's range is refused. The station list (operator>> 0x0009851e) is emptied without
// releasing what it held, then filled with one reference per occupant the record names. The original also keeps a null entry for a reference that does not
// resolve (and would crash on it later); the port leaves those out.
bool cSC4AircraftDispatchSource::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < traits_.minReadVersion || version > traits_.maxReadVersion) return false;
    stations_.clear();
    std::uint32_t count = 0;
    stream.GetUint32(count);
    if (count < kOccupantListReadLimit) {
        for (; count != 0; --count) {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cISC4Occupant, &raw, false);
            if (raw) stations_.push_back(static_cast<cISC4Occupant*>(raw));
        }
    }
    stream.GetUint32(nextCallNumber_);
    return !stream.GetError();
}

}  // namespace sc4
