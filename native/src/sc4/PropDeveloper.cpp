#include "sc4/PropDeveloper.h"
#include "sc4/SimStreamOps.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "rz/cRZDate.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/DBSegment.h"
#include "gz/StreamOps.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

IPropDeveloperHost* g_host = nullptr;

cIGZMessage2Standard& StandardMessage(cIGZMessage2* message) {
    return *static_cast<cIGZMessage2Standard*>(message);
}

// The 13 messages Init subscribes to and Shutdown unsubscribes (0x100121f04..0x100121f4a).
constexpr std::uint32_t kRegisteredMessages[] = {
    kPropMessageOccupantInserted,       kPropMessageOccupantRemoved,   kPropMessageLightsOff,
    kPropMessageLightsOn,               kPropMessageMinuteChanged,     kPropMessageDayNightChanged,
    kPropMessageCityInit,               kPropMessageLotStateChanged,   kPropMessagePowerChanged,
    kPropMessageGlobalEffectChanged,    kPropMessagePropsChanged,      kPropMessageTerrainChanged,
    kPropMessageTerrainChanged2,
};
constexpr const char* kAgentName = "cSC4PropDeveloper";

// The constructor's seed (0x100121847..0x1001218c3): seconds of the local day plus the days since
// cRZTime::mRefDate, times 28, all in 32-bit arithmetic.
std::uint32_t PropDeveloperSeed() {
    const std::time_t now = std::time(nullptr);
    const std::tm* local = std::localtime(&now);
    if (!local) return 0;
    const cRZDate today(static_cast<std::uint32_t>(local->tm_mon + 1),
                        static_cast<std::uint32_t>(local->tm_mday),
                        static_cast<std::uint32_t>(local->tm_year + 1900));
    if (today.JulianDay() == 0) return 0;
    const std::uint32_t reference =
        g_host ? g_host->TimeReferenceJulianDay() : kPropTimeReferenceJulianDay;
    const std::uint32_t secondsOfDay = static_cast<std::uint32_t>(local->tm_hour * 3600 + local->tm_min * 60 +
                                                                  local->tm_sec);
    const std::uint32_t days = today.JulianDay() - reference;
    return (secondsOfDay + days * kSecondsPerDay) * kPropSeedMultiplier;
}

// std::random_shuffle's no-generator overload is gone from C++17; the original picks a uniform index with
// std::rand() (libc++ walks i from the back and swaps with rand() % (i + 1)). Kept so the ordering quirk
// (a fresh shuffle every lights-on/off message) survives.
void ShuffleProps(std::vector<cISC4PropOccupant*>& props) {
    for (std::size_t i = props.size(); i > 1; --i) {
        const std::size_t j = static_cast<std::size_t>(std::rand()) % i;
        std::swap(props[i - 1], props[j]);
    }
}

std::uint32_t Uint32Of(const cIGZVariant* value) {
    return value ? value->AsUint32() : 0;
}

}  // namespace

IPropDeveloperHost* PropDeveloperHost() { return g_host; }
void SetPropDeveloperHost(IPropDeveloperHost* host) { g_host = host; }

// --------------------------------------------------------------------------------------------------- lifecycle

// @ 0x1001217ce. Three vptrs, the zeroed walk/registry state and the time-seeded generator.
cSC4PropDeveloper::cSC4PropDeveloper() : random_(PropDeveloperSeed()) {
    initialized_ = false;
    refCount_ = 0;
    phase_ = 0;
    cursorState_ = 0;
    cursorX_ = 0;
    cursorZ_ = 0;
    step_ = 1;
    tractRect_ = SC4Rect<int>(static_cast<std::int32_t>(kIntMin), static_cast<std::int32_t>(kIntMin),
                              static_cast<std::int32_t>(0x7fffffffu), static_cast<std::int32_t>(0x7fffffffu));
    requesterCreated_ = false;
    animationIndex_ = 0;
    animationMode_ = 0;
    animationBatch_ = 0;
    animationSeconds_ = 0;
    night_ = false;
    minuteThreshold_ = kPropMinuteThreshold;
    lastTimeOfDay_ = kPropLastTimeOfDayInitial;
    signsVisible_ = true;
}

// @ 0x1001219fc (and its two destructor thunks). The containers free their nodes; the referenced objects
// were released by Shutdown (the original's destructor does not release them either).
cSC4PropDeveloper::~cSC4PropDeveloper() = default;

// @ 0x100121ba2. The iid is tested as a *signed* int, exactly like the original: iid 0 routes through the
// interface comparisons and iid 1 is the primary view, not cIGZUnknown.
bool cSC4PropDeveloper::QueryInterface(std::uint32_t iid, void** out) {
    void* result = nullptr;
    if (static_cast<std::int32_t>(iid) < 1) {
        if (iid == kGZIID_cIGZSerializablePropDeveloper) {
            result = static_cast<cIGZSerializable*>(this);
        } else if (iid != kGZIID_cISC4PropDeveloper) {
            return false;
        } else {
            result = static_cast<cISC4PropDeveloper*>(this);
        }
    } else if (iid != 1) {
        if (iid != kGZIID_cIGZMessageTarget2PropDeveloper) return false;
        result = static_cast<cIGZMessageTarget2*>(this);
    } else {
        result = static_cast<cISC4PropDeveloper*>(this);
    }
    *out = result;
    (void)AddRef();   // primary vslot 0x08
    return true;
}

std::uint32_t cSC4PropDeveloper::AddRef() { return ++refCount_; }

// @ 0x100121b44: the deleting destructor is called through the primary vtable slot 0x60; a never-AddRef'd
// object is destroyed by the first Release (count 0 <= 1).
std::uint32_t cSC4PropDeveloper::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------
// Init / Shutdown
// ---------------------------------------------------------------------------------------------------------

// @ 0x100121c04.
bool cSC4PropDeveloper::Init() {
    if (initialized_) return true;
    if (!g_host) return false;
    initialized_ = true;

    // The stored services take their own references (the originals are released in Shutdown).
    if (!lotManager_) {
        lotManager_ = g_host->LotManager();                 // SL::spLotManager
        if (lotManager_) lotManager_->AddRef();
    }
    if (!propManager_) {
        propManager_ = g_host->PropManager();               // SL::spPropManager
        if (propManager_) propManager_->AddRef();
    }
    if (!networkLotManager_) {
        networkLotManager_ = g_host->NetworkLotManager();   // SL::spCity vslot 0x150
        if (networkLotManager_) networkLotManager_->AddRef();
    }
    if (!powerSimulator_) {
        powerSimulator_ = g_host->PowerSimulator();         // SL::spPowerSim
        if (powerSimulator_) powerSimulator_->AddRef();
    }

    // The default requester is a cSC4WealthRequester created here and kept in the requester vector. The
    // original's constructor starts the count at 1 (0x100121ccd writes 1 at +0x10); the recovered
    // cSC4WealthRequester starts at 0, so the creator's reference is taken explicitly.
    if (!requesterCreated_) {
        auto* requester = new cSC4WealthRequester(g_host->WealthRequesterHost(), g_host->RequesterRandom());
        requester->AddRef();               // the constructor's initial reference (port difference)
        AddPropRequester(requester);       // primary vslot 0x28 (AddRef)
        requester->Release();              // primary vslot 0x10
        requesterCreated_ = true;
    }

    // The tract rectangle: the city's cells / 8 - 1, clamped to 0 for an empty city.
    if (phase_ == 0) {
        phase_ = 1;
        const std::uint32_t cellsX = static_cast<std::uint32_t>(g_host->City()->CellCountX());
        const std::uint32_t cellsZ = static_cast<std::uint32_t>(g_host->City()->CellCountZ());
        std::int32_t maxX = static_cast<std::int32_t>((cellsX >> 3) - 1);
        std::int32_t maxZ = static_cast<std::int32_t>((cellsZ >> 3) - 1);
        tractRect_ = SC4Rect<int>(0, 0, maxX, maxZ);
        if ((cellsX >> 3) == 0) {
            tractRect_.x1 = 0;
            maxX = 0;
        }
        if ((cellsZ >> 3) == 0) {
            tractRect_.z1 = 0;
            maxZ = 0;
        }
        cursorX_ = 0;
        cursorZ_ = 0;
        // The original writes `((maxX | maxZ) >> 30) & 2` here; the clamping above makes the top bit
        // unreachable, so the byte is always 0 (0x100121d7a).
        cursorState_ = 0;
    }

    // Simulator agents: idle-time (10) and day (4) intervals, name "cSC4PropDeveloper", flags 2.
    g_host->AddSimulatorAgent(static_cast<cIGZMessageTarget2*>(this), kPropMessageAnimationInterval, kAgentName,
                              kPropAgentFlags);
    g_host->AddSimulatorAgent(static_cast<cIGZMessageTarget2*>(this), kPropMessageDayInterval, kAgentName,
                              kPropAgentFlags);

    if (cIGZMessageServer2* messages = g_host->MessageServer()) {
        for (std::uint32_t type : kRegisteredMessages) {
            messages->AddNotification(static_cast<cIGZMessageTarget2*>(this), type);   // vslot 0x28
        }
    }
    g_host->AddTickNotification(static_cast<cIGZMessageTarget2*>(this), kPropAnimationChannel);   // SL::ATM()
    return true;
}

// @ 0x100121f7e.
bool cSC4PropDeveloper::Shutdown() {
    if (!initialized_) return true;
    initialized_ = false;

    ReleaseEffectProps();
    ReleaseList(timeProps_, true);
    ReleaseList(dateProps_, true);
    ReleaseList(powerProps_, false);   // kept: the original releases the occupants but leaves the nodes
    for (cISC4PropOccupant* prop : stateProps_) {
        if (prop) prop->Release();
    }
    stateProps_.clear();

    if (IPropSimulatorApi* simulator = g_host ? g_host->Simulator() : nullptr) {
        simulator->RemoveAgent(static_cast<cIGZMessageTarget2*>(this));   // vslot 0xa8
    }
    if (cIGZMessageServer2* messages = g_host ? g_host->MessageServer() : nullptr) {
        for (std::uint32_t type : kRegisteredMessages) {
            messages->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), type);   // vslot 0x30
        }
    }
    if (g_host) g_host->RemoveTickNotification(static_cast<cIGZMessageTarget2*>(this), kPropAnimationChannel);

    phase_ = 0;
    if (lotManager_) {
        lotManager_->Release();
        lotManager_ = nullptr;
    }
    if (propManager_) {
        propManager_->Release();
        propManager_ = nullptr;
    }
    if (networkLotManager_) {
        networkLotManager_->Release();
        networkLotManager_ = nullptr;
    }
    if (powerSimulator_) {
        powerSimulator_->Release();
        powerSimulator_ = nullptr;
    }
    if (requesterCreated_) {
        for (cISC4PropRequester* requester : requesters_) {
            if (requester) requester->Release();
        }
        requesters_.clear();
        requesterCreated_ = false;
    }
    return true;
}

void cSC4PropDeveloper::ReleaseList(std::list<cISC4PropOccupant*>& list, bool clear) {
    for (cISC4PropOccupant* prop : list) {
        if (prop) prop->Release();
    }
    if (clear) list.clear();
}

void cSC4PropDeveloper::ReleaseEffectProps() {
    for (auto& entry : effectProps_) {
        for (cISC4PropOccupant* prop : entry.second) {
            if (prop) prop->Release();
        }
    }
    effectProps_.clear();
}

// ---------------------------------------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------------------------------------

// @ 0x1001222c8. Version 5, then the walk state, the requesters, the three stored managers, the animation
// state, the night flag, the state props, the effect map (key, list size, props), the four prop lists and the
// sign flag/list. Returns true when the stream reported no error.
bool cSC4PropDeveloper::Write(cIGZOStream& stream) const {
    stream.SetUint16(5);
    stream.SetSint8(static_cast<std::int8_t>(phase_));
    {
        // +0x24: the tract walk's scanning iterator, streamed whole (state, x, z, step, the tract rectangle in memory order)
        ScanningIterator cursor;
        cursor.state = static_cast<std::int8_t>(cursorState_);
        cursor.x = cursorX_;
        cursor.z = cursorZ_;
        cursor.remaining = static_cast<std::uint32_t>(step_);
        cursor.rect = cRZRect{tractRect_.x0, tractRect_.z0, tractRect_.x1, tractRect_.z1};
        stream << cursor;
    }
    stream.SetUint8(requesterCreated_ ? 1 : 0);
    stream.SetUint32(static_cast<std::uint32_t>(requesters_.size()));
    for (cISC4PropRequester* requester : requesters_) {
        SC4WriteGZSerializable(stream, requester);
    }
    if (g_host) {
        g_host->WriteService(stream, lotManager_);
        g_host->WriteService(stream, propManager_);
        g_host->WriteService(stream, networkLotManager_);
    } else {   // no host: the three service references are still part of the record (written as absent)
        for (int i = 0; i < 3; ++i) SC4WriteGZSerializable(stream, nullptr);
    }
    stream.SetUint32(animationIndex_);
    stream.SetUint32(static_cast<std::uint32_t>(animationMode_));
    stream.SetUint32(animationBatch_);
    stream.SetUint32(animationSeconds_);
    stream << night_;
    stream.SetUint32(static_cast<std::uint32_t>(stateProps_.size()));
    for (cISC4PropOccupant* prop : stateProps_) {
        SC4WriteGZSerializable(stream, prop);
    }
    stream.SetUint32(static_cast<std::uint32_t>(effectProps_.size()));
    for (const auto& entry : effectProps_) {
        stream.SetUint32(entry.first);
        stream.SetUint32(static_cast<std::uint32_t>(entry.second.size()));
        for (cISC4PropOccupant* prop : entry.second) {
            SC4WriteGZSerializable(stream, prop);
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(timeProps_.size()));
    for (cISC4PropOccupant* prop : timeProps_) {
        SC4WriteGZSerializable(stream, prop);
    }
    stream.SetUint32(static_cast<std::uint32_t>(dateProps_.size()));
    for (cISC4PropOccupant* prop : dateProps_) {
        SC4WriteGZSerializable(stream, prop);
    }
    stream.SetUint32(static_cast<std::uint32_t>(powerProps_.size()));
    for (cISC4PropOccupant* prop : powerProps_) {
        SC4WriteGZSerializable(stream, prop);
    }
    stream << signsVisible_;
    stream.SetUint32(static_cast<std::uint32_t>(signProps_.size()));
    for (cISC4PropOccupant* prop : signProps_) {
        SC4WriteGZSerializable(stream, prop);
    }
    return !stream.GetError();
}

// @ 0x1001225d0. Only version >= 4 is accepted. The original overwrites the manager pointers without
// releasing the Init-time ones (a leak); the port keeps that.
bool cSC4PropDeveloper::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    if (!stream.GetUint16(version)) return false;
    if (version < 4) return false;

    std::int8_t phase = 0;
    if (!stream.GetSint8(phase)) return false;
    phase_ = static_cast<std::uint8_t>(phase);
    ScanningIterator cursor;                                  // +0x24: the tract walk's scanning iterator
    stream >> cursor;
    cursorState_ = static_cast<std::uint8_t>(cursor.state);
    cursorX_ = cursor.x;
    cursorZ_ = cursor.z;
    step_ = static_cast<std::int32_t>(cursor.remaining);
    tractRect_ = SC4Rect<int>(cursor.rect.left, cursor.rect.top, cursor.rect.right, cursor.rect.bottom);
    if (std::getenv("SC4_PROPDEVDEBUG"))
        std::fprintf(stderr, "prop developer: read phase %d, walk state %d at %d,%d step %d, tracts %d,%d..%d,%d\n", phase_,
                     cursorState_, cursorX_, cursorZ_, step_, tractRect_.x0, tractRect_.z0, tractRect_.x1, tractRect_.z1);
    std::uint8_t created = 0;
    if (!stream.GetUint8(created)) return false;
    requesterCreated_ = created != 0;

    std::uint32_t count = 0;
    if (!stream.GetUint32(count)) return false;
    requesters_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cISC4PropRequester, &raw, false);
        requesters_[i] = static_cast<cISC4PropRequester*>(raw);
        if (std::getenv("SC4_PROPDEVDEBUG"))
            std::fprintf(stderr, "prop developer: read requester %u of %u -> %08x\n", i, count,
                         raw ? requesters_[i]->GetRequesterSatisfactionId() : 0u);
    }
    if (g_host) {
        lotManager_ = static_cast<IPropLotManagerApi*>(g_host->ReadService(stream, 0x681905bd));
        propManager_ = static_cast<IPropManagerApi*>(g_host->ReadService(stream, 0x28326f3e));
        networkLotManager_ = static_cast<IPropNetworkLotManagerApi*>(g_host->ReadService(stream, 0x89a5ba3f));
    } else {   // no host: the three service references are still part of the record
        for (std::uint32_t iid : {0x681905bdu, 0x28326f3eu, 0x89a5ba3fu}) {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, iid, &raw, false);
            if (raw) static_cast<cIGZUnknown*>(raw)->Release();
        }
    }
    std::uint32_t index = 0, mode = 0, batch = 0, seconds = 0;
    if (!stream.GetUint32(index) || !stream.GetUint32(mode) || !stream.GetUint32(batch) ||
        !stream.GetUint32(seconds)) {
        return false;
    }
    animationIndex_ = index;
    animationMode_ = static_cast<std::int32_t>(mode);
    animationBatch_ = batch;
    animationSeconds_ = seconds;
    stream >> night_;

    if (!stream.GetUint32(count)) return false;
    stateProps_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kGZIID_cISC4PropOccupant, &raw, false);
        stateProps_[i] = static_cast<cISC4PropOccupant*>(raw);
    }

    std::uint32_t effectCount = 0;
    if (!stream.GetUint32(effectCount)) return false;
    for (std::uint32_t i = 0; i < effectCount; ++i) {
        std::uint32_t key = 0, propCount = 0;
        if (!stream.GetUint32(key) || !stream.GetUint32(propCount)) return false;
        std::list<cISC4PropOccupant*>& list = effectProps_[key];
        for (std::uint32_t j = 0; j < propCount; ++j) {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cISC4PropOccupant, &raw, false);
            if (raw) list.push_back(static_cast<cISC4PropOccupant*>(raw));
        }
    }

    std::list<cISC4PropOccupant*>* lists[3] = {&timeProps_, &dateProps_, &powerProps_};
    for (std::list<cISC4PropOccupant*>* list : lists) {
        std::uint32_t propCount = 0;
        if (!stream.GetUint32(propCount)) return false;
        for (std::uint32_t j = 0; j < propCount; ++j) {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cISC4PropOccupant, &raw, false);
            if (raw) list->push_back(static_cast<cISC4PropOccupant*>(raw));
        }
    }

    if (version > 4) {
        stream >> signsVisible_;
        std::uint32_t propCount = 0;
        if (!stream.GetUint32(propCount)) return false;
        for (std::uint32_t j = 0; j < propCount; ++j) {
            void* raw = nullptr;
            SC4ReadGZSerializable(stream, kGZIID_cISC4PropOccupant, &raw, false);
            if (raw) signProps_.push_back(static_cast<cISC4PropOccupant*>(raw));
        }
    }
    return !stream.GetError();
}

// ---------------------------------------------------------------------------------------------------------
// requesters
// ---------------------------------------------------------------------------------------------------------

// @ 0x100122d70.
void cSC4PropDeveloper::AddPropRequester(cISC4PropRequester* requester) {
    if (!requester) return;
    requester->AddRef();          // vslot 0x08
    requesters_.push_back(requester);
}

// @ 0x100122dc8.
void cSC4PropDeveloper::RemovePropRequester(cISC4PropRequester* requester) {
    const auto it = std::find(requesters_.begin(), requesters_.end(), requester);
    if (it == requesters_.end()) return;
    requesters_.erase(it);
    requester->Release();         // vslot 0x10
}

// ---------------------------------------------------------------------------------------------------------
// messages
// ---------------------------------------------------------------------------------------------------------

// @ 0x100122e36.
bool cSC4PropDeveloper::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();            // slot 0x20
    cIGZMessage2Standard& payload = StandardMessage(message);

    static const bool trace = std::getenv("SC4_PROPDEVDEBUG") != nullptr;
    if (trace && type != kPropMessageAnimationTick && type != kPropMessageOccupantInserted && type != kPropMessageOccupantRemoved &&
        type != kPropMessageIdleTime)
        std::fprintf(stderr, "prop developer: message %08x data1 %llu data2 %llu (mode %d, night %d)\n", type,
                     static_cast<unsigned long long>(payload.GetData1()), static_cast<unsigned long long>(payload.GetData2()),
                     animationMode_, night_ ? 1 : 0);
    if (type == kPropMessageAnimationTick) {
        if (trace && animationMode_ != 0)
            std::fprintf(stderr, "prop developer: tick %llu us, mode %d, index %u of %zu\n",
                         static_cast<unsigned long long>(payload.GetData2()), animationMode_, animationIndex_, stateProps_.size());
        if (animationMode_ == 0) return false;
        // slot 0x70 = data2, the elapsed milliseconds; the original divides by 1000.
        const std::uint32_t elapsed = static_cast<std::uint32_t>(payload.GetData2());
        DoAnimationTick(elapsed / 1000);
        return true;
    }
    if (type == kPropMessagePowerChanged) {
        const SC4Rect<int> rect{static_cast<std::int32_t>(static_cast<std::uint32_t>(payload.GetData1())),
                                static_cast<std::int32_t>(static_cast<std::uint32_t>(payload.GetData2())),
                                static_cast<std::int32_t>(static_cast<std::uint32_t>(payload.GetData3())),
                                static_cast<std::int32_t>(static_cast<std::uint32_t>(payload.GetData4()))};
        DoPowerChanged(rect);
        return true;
    }
    if (type == kPropMessageCityInit) {
        night_ = g_host && g_host->Lighting() && g_host->Lighting()->IsNight();
        SwitchNightChangeProps(night_);
        return true;
    }
    if (type == kPropMessageDay) {
        DoIdle(0);   // the original calls DoIdle without setting its (ignored) argument
        if (g_host && g_host->Simulator()) {
            const cGZDate today = g_host->Simulator()->Date();   // vslot 0x30
            for (cISC4PropOccupant* prop : dateProps_) {
                if (prop) prop->TickSimulatorDate(today);        // prop vslot 0x80
            }
        }
        return true;
    }
    if (type == kPropMessageGlobalEffectChanged) {
        // data1 = the global effect id, data2 = on. Props that wait for that effect get timer bit 4.
        const std::uint32_t key = static_cast<std::uint32_t>(payload.GetData1());
        const bool on = static_cast<std::uint32_t>(payload.GetData2()) != 0;
        const auto it = effectProps_.find(key);
        if (it != effectProps_.end()) {
            for (auto entry = it->second.begin(); entry != it->second.end();) {
                cISC4PropOccupant* prop = *entry;
                if (!prop) {
                    entry = it->second.erase(entry);
                    continue;
                }
                prop->SetPropTimerState(kPropTimerBitEffect, on);   // prop vslot 0xa8
                ++entry;
            }
        }
        return true;
    }
    if (type == kPropMessagePropsChanged) {
        // The message this class posts for itself after a lot state change: data1/data2 = the cell.
        if (g_host && g_host->LotManager()) {
            cSC4Lot* lot = g_host->LotManager()->GetLot(static_cast<int>(payload.GetData1()),
                                                        static_cast<int>(payload.GetData2()), false);
            if (lot) UpdateLotStateProps(lot);
        }
        return true;
    }
    if (type == kPropMessageLotStateChanged) {
        cSC4Lot* snapshot = g_host ? g_host->MessageLot(message) : nullptr;   // message slot 0x100
        if (!snapshot) return true;
        int x = 0, z = 0;
        snapshot->GetLocation(x, z);       // lot vslot 0xa8
        std::uint8_t sizeX = 0, sizeZ = 0;
        snapshot->GetSize(sizeX, sizeZ);   // lot vslot 0xb0
        cSC4Lot* lot = g_host->LotManager() ? g_host->LotManager()->GetLot(x, z, true) : nullptr;
        if (!lot) return true;
        int lotX = 0, lotZ = 0;
        lot->GetLocation(lotX, lotZ);
        std::uint8_t lotSizeX = 0, lotSizeZ = 0;
        lot->GetSize(lotSizeX, lotSizeZ);
        if (x != lotX || z != lotZ || sizeX != lotSizeX || sizeZ != lotSizeZ) return true;
        if (!snapshot->IsInLivableState()) RemoveOptionalPropsInLot(lot);
        const std::uint32_t oldState = static_cast<std::uint32_t>(payload.GetData1());
        const std::uint32_t newState = static_cast<std::uint32_t>(payload.GetData2());
        // Quirk kept (0x100123470): the flag is "the lot just became livable" (old 2 -> new 3), read from the
        // incoming message's low data bytes, not from the lot.
        const bool livable = (oldState & 0xff) == 2 && (newState & 0xff) == 3;
        auto* reply = new cRZMessage2Standard();
        reply->AddRef();
        reply->SetType(kPropMessagePropsChanged);
        reply->SetData1(static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)));
        reply->SetData2(static_cast<std::uint64_t>(static_cast<std::uint32_t>(z)));
        reply->SetData3(livable ? 1u : 0u);
        if (cIGZMessageServer2* messages = g_host->MessageServer()) {
            messages->MessagePost(reply, false);   // vslot 0x20
        }
        reply->Release();
        return true;
    }
    if (type == kPropMessageDayNightChanged) {
        if (!g_host || !g_host->Clock()) return true;
        const std::uint32_t night = static_cast<std::uint32_t>(payload.GetData2());   // i386 0x000d9f9e: message slot 14 GetData2
        const int mode = g_host->Clock()->VisualTimeMode();   // clock vslot 0x30
        if (night == 1) {
            if (mode == 1) SwitchNightChangeProps(false);      // pinned to noon
        } else if (night == 0) {
            if (mode == 2) SwitchNightChangeProps(true);       // pinned to midnight
        }
        return true;
    }
    if (type == kPropMessageMinuteChanged) {
        if (!g_host || !g_host->Clock() || !g_host->City()) return true;
        const float time = static_cast<float>(g_host->Clock()->Hour()) +          // clock vslot 0x50
                           static_cast<float>(g_host->Clock()->Minute()) / kPropMinutesPerHour;   // vslot 0x68
        if (std::fabs(lastTimeOfDay_ - time) < minuteThreshold_) return true;
        lastTimeOfDay_ = time;
        ApplyTimeOfDay(time);
        return true;
    }
    if (type == kPropMessageTerrainChanged || type == kPropMessageTerrainChanged2) {
        // The original reads data1/data2 and drops them, then updates the sign heights.
        (void)payload.GetData1();
        (void)payload.GetData2();
        UpdateSignpostHeights();
        return true;
    }
    if (type == kPropMessageOccupantInserted) {
        if (static_cast<std::uint32_t>(payload.GetData3()) == kPropOccupantType) {
            HandleInsertOccupant(static_cast<cISC4Occupant*>(payload.GetVoid1()));
        }
        return true;
    }
    if (type == kPropMessageOccupantRemoved) {
        const std::uint32_t occupantType = static_cast<std::uint32_t>(payload.GetData3());
        if (occupantType == kPropOccupantType) {
            HandleRemoveOccupant(static_cast<cISC4Occupant*>(payload.GetVoid1()));
        } else if (occupantType == kPropNetworkOccupantType) {
            HandleRemoveNetworkOccupant(static_cast<cISC4Occupant*>(payload.GetVoid1()));
        }
        return true;
    }
    if (type == kPropMessageLightsOff || type == kPropMessageLightsOn) {
        night_ = type == kPropMessageLightsOn;
        animationMode_ = night_ ? 1 : 2;
        ShuffleProps(stateProps_);
        const std::size_t count = stateProps_.size();
        animationBatch_ = static_cast<std::uint32_t>(count / 8 + (count % 8 != 0 ? 1 : 0));
        animationIndex_ = 0;
        return true;
    }
    if (type == kPropMessageIdleTime) {
        DoIdle(static_cast<std::uint32_t>(payload.GetData3()));   // slot 0x98
        return true;
    }
    return true;
}

// @ 0x100122d.. The minute-changed body: props whose time-of-day window contains `time` get timer bit 1.
void cSC4PropDeveloper::ApplyTimeOfDay(float time) {
    for (auto entry = timeProps_.begin(); entry != timeProps_.end();) {
        cISC4PropOccupant* prop = *entry;
        if (!prop) {
            entry = timeProps_.erase(entry);
            continue;
        }
        float start = 0.0f, end = 0.0f;
        prop->GetTimeOfDayRange(start, end);   // prop vslot 0x70
        bool on;
        if (start <= end) {
            on = start <= time && time <= end;
        } else {
            on = time >= start || time <= end;
        }
        prop->SetPropTimerState(kPropTimerBitTimeOfDay, on);   // prop vslot 0xa8
        ++entry;
    }
}

// ---------------------------------------------------------------------------------------------------------
// the tract walk
// ---------------------------------------------------------------------------------------------------------

// @ 0x1001235a0. Three-state cursor over the tract rectangle: the first call arms the walk (phase 1 -> 2), then
// each call updates one tract and advances. At the end of the rectangle the cursor wraps and restarts in the
// same call (quirk). The `elapsed` argument is ignored.
void cSC4PropDeveloper::DoIdle(std::uint32_t elapsed) {
    (void)elapsed;
    if (phase_ != 2) {
        if (phase_ == 1) phase_ = 2;
        return;
    }
    std::int32_t x;
    std::int32_t z = cursorZ_;
    if (cursorState_ == 0) {
        cursorState_ = 1;
        x = cursorX_;
    } else if (cursorState_ == 2) {
        x = tractRect_.x0;
        cursorX_ = 0;
        cursorZ_ = 0;
        cursorState_ = 0;
        if (x < 1 && tractRect_.z0 < 1 && tractRect_.x1 >= 0 && tractRect_.z1 >= 0) {
            cursorState_ = 1;
            x = 0;
            z = 0;
        } else {
            cursorState_ = 2;
            z = 0;
        }
        UpdateTract(x, z);
        return;
    } else {
        x = cursorX_ + step_;
        cursorX_ = x;
        if (x > tractRect_.x1) {
            x = tractRect_.x0;
            cursorX_ = x;
            z = cursorZ_ + step_;
            cursorZ_ = z;
            if (z > tractRect_.z1) {
                cursorState_ = 2;
                cursorX_ = 0;
                cursorZ_ = 0;
                cursorState_ = 0;
                if (x > 0 || tractRect_.z0 > 0) {
                    cursorState_ = 2;
                } else if (tractRect_.x1 < 0) {
                    cursorState_ = 2;
                } else if (tractRect_.z1 < 0) {
                    cursorState_ = 2;
                } else {
                    cursorState_ = 1;
                    x = 0;
                    z = 0;
                }
                UpdateTract(x, z);
                return;
            }
            z = cursorZ_;
        }
    }
    UpdateTract(x, z);
}

// @ 0x1001243ae. The night power sweep: every 200 accumulated seconds, update one eighth of the state props.
void cSC4PropDeveloper::DoAnimationTick(std::uint32_t seconds) {
    animationSeconds_ += seconds;
    if (animationSeconds_ <= kPropAnimationStepSeconds) return;
    animationSeconds_ = 0;
    const std::int32_t mode = animationMode_;
    const float cellX = g_host->City()->CellSizeX();   // city vslot 0x2d8
    const float cellZ = g_host->City()->CellSizeZ();   // city vslot 0x2e0
    std::uint32_t index = animationIndex_;
    const std::uint32_t count = static_cast<std::uint32_t>(stateProps_.size());
    std::uint32_t end = animationBatch_ + index;
    if (static_cast<std::int32_t>(count) < static_cast<std::int32_t>(end)) {
        animationMode_ = 0;
        end = count;
    }
    if (index >= end) return;
    // The original truncates the cell size to an integer before dividing (0x1001250a1-style sequence).
    const float cellXInt = static_cast<float>(static_cast<std::int32_t>(cellX));
    const float cellZInt = static_cast<float>(static_cast<std::int32_t>(cellZ));
    do {
        cISC4PropOccupant* prop = stateProps_[index];
        std::uint8_t state = 0;
        if (mode == 1 && prop) {
            float pos[3] = {0.0f, 0.0f, 0.0f};
            prop->AsOccupant()->GetPosition(pos);   // occupant vslot 0x40
            state = powerSimulator_->IsPowered(static_cast<int>(pos[0] / cellXInt),
                                               static_cast<int>(pos[2] / cellZInt)) ? 1 : 0;
        }
        if (prop) prop->SetState(state);            // prop vslot 0x58
        index = ++animationIndex_;
    } while (index < end);
}

// @ 0x1001245e6. Sets every state prop to the night/day state (at night only when the cell has power) and
// resets the animation cursor *and* mode to 0.
void cSC4PropDeveloper::SwitchNightChangeProps(bool night) {
    const float cellX = g_host->City()->CellSizeX();
    const float cellZ = g_host->City()->CellSizeZ();
    const float invX = 1.0f / cellX;
    const float invZ = 1.0f / cellZ;
    for (auto entry = stateProps_.begin(); entry != stateProps_.end();) {
        cISC4PropOccupant* prop = *entry;
        if (!prop) {
            entry = stateProps_.erase(entry);
            continue;
        }
        std::uint8_t state = night ? 1 : 0;
        if (night) {
            float pos[3] = {0.0f, 0.0f, 0.0f};
            prop->AsOccupant()->GetPosition(pos);   // prop vslot 0x18 -> occupant vslot 0x40
            state = powerSimulator_->IsPowered(static_cast<int>(pos[0] * invX),
                                               static_cast<int>(pos[2] * invZ)) ? 1 : 0;
        }
        prop->SetState(state);
        ++entry;
    }
    // The 8-byte store at +0xa70 clears the animation cursor and the animation mode (0x1001247c8).
    animationIndex_ = 0;
    animationMode_ = 0;
}

// ---------------------------------------------------------------------------------------------------------
// lots
// ---------------------------------------------------------------------------------------------------------

// @ 0x10012471c. Removes the props that fill an *optional* configuration slot (object type 1, flags bit 0)
// from a lot that is no longer livable.
void cSC4PropDeveloper::RemoveOptionalPropsInLot(cSC4Lot* lot) {
    if (!lot || !g_host || !g_host->PropManager()) return;
    const std::vector<cSC4LotConfigurationObject>* objects = g_host->LotConfigurationObjects(lot);
    if (!objects) return;
    std::vector<cISC4Occupant*> props;
    g_host->PropManager()->PropsInLot(props, lot);   // vslot 0xb8
    for (cISC4Occupant* occupant : props) {
        if (!occupant) continue;
        std::uint32_t tag = 0;
        if (!objects->empty() && occupant->GetLotTag(tag)) {
            for (const cSC4LotConfigurationObject& object : *objects) {
                if (object.id != tag) continue;
                if ((object.flags & 1) != 0) {
                    g_host->PropManager()->RemoveProp(occupant);   // vslot 0xe8
                }
                break;
            }
        }
        occupant->Release();   // the list's reference
    }
}

// @ 0x100124842. Power changed inside a cell rectangle: state props toggle state 1/0, power-timer props get
// timer bit 8. The cell size is truncated to an integer before dividing (like the original).
void cSC4PropDeveloper::DoPowerChanged(const SC4Rect<int>& rect) {
    float cellX = g_host->City()->CellSizeX();
    float cellZ = g_host->City()->CellSizeZ();
    std::vector<cISC4Occupant*> props;
    if (g_host->PropManager()->PropsInRect(props, rect) && !props.empty()) {
        cellX = static_cast<float>(static_cast<std::int32_t>(cellX));
        cellZ = static_cast<float>(static_cast<std::int32_t>(cellZ));
        for (cISC4Occupant* occupant : props) {
            if (!occupant) continue;
            cISCPropertyHolder* holder = occupant->AsPropertyHolder();   // occupant vslot 0x30
            std::uint8_t hasState = 0;
            if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyNightState, hasState) &&
                hasState == kPropPropertyLotStateValue) {
                cISC4PropOccupant* prop = nullptr;
                if (occupant->QueryInterface(kGZIID_cISC4PropOccupant, reinterpret_cast<void**>(&prop)) && prop) {
                    float pos[3] = {0.0f, 0.0f, 0.0f};
                    occupant->GetPosition(pos);   // occupant vslot 0x40
                    const bool powered = powerSimulator_->IsPowered(static_cast<int>(pos[0] / cellX),
                                                                    static_cast<int>(pos[2] / cellZ));
                    if (!powered) {
                        if (prop->GetState() == 1) prop->SetState(0);
                    } else if (night_ && prop->GetState() == 0) {
                        prop->SetState(1);
                    }
                    prop->Release();
                }
            }
            occupant->Release();
        }
    }
    for (auto entry = powerProps_.begin(); entry != powerProps_.end();) {
        cISC4PropOccupant* prop = *entry;
        if (!prop) {
            entry = powerProps_.erase(entry);
            continue;
        }
        float pos[3] = {0.0f, 0.0f, 0.0f};
        prop->AsOccupant()->GetPosition(pos);
        const int cellXC = static_cast<int>(pos[0] * kPropCellSizeReciprocal);
        const int cellZC = static_cast<int>(pos[2] * kPropCellSizeReciprocal);
        if (rect.x0 <= cellXC && cellXC <= rect.x1 && rect.z0 <= cellZC && cellZC <= rect.z1) {
            prop->SetPropTimerState(kPropTimerBitPower,
                                    powerSimulator_->IsPowered(cellXC, cellZC));   // vslot 0x128
        }
        ++entry;
    }
}

// @ 0x100124b06. Props carrying property 0x6a5e0727 get state = (property value != lot state), inverted.
void cSC4PropDeveloper::UpdateLotStateProps(cSC4Lot* lot) {
    if (!lot) return;
    const std::uint8_t lotState = lot->GetState();   // lot vslot 0x108
    std::vector<cISC4Occupant*> props;
    g_host->PropManager()->PropsInLot(props, lot);
    for (cISC4Occupant* occupant : props) {
        if (!occupant) continue;
        cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        std::uint8_t value = 0;
        if (holder && cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyLotState, value)) {
            cISC4PropOccupant* prop = nullptr;
            if (occupant->QueryInterface(kGZIID_cISC4PropOccupant, reinterpret_cast<void**>(&prop)) && prop) {
                prop->SetState(value != lotState ? 1 : 0);
                prop->Release();
            }
        }
        occupant->Release();
    }
}

// @ 0x100124bfe. Puts the sign props on top of the higher of the terrain and the water surface.
void cSC4PropDeveloper::UpdateSignpostHeights() {
    if (!g_host || !g_host->Terrain() || !g_host->OccupantManager()) return;
    for (cISC4PropOccupant* prop : signProps_) {
        if (!prop) continue;
        if (prop->GetPropType() != kPropTypeSignpost) continue;   // prop vslot 0x38
        cISC4Occupant* occupant = prop->AsOccupant();
        float pos[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(pos);
        const float terrain = g_host->Terrain()->Height(pos[0], pos[2]);
        const float water = g_host->Terrain()->WaterHeight(pos[0], pos[2]);
        const float base = std::max(terrain, water);
        if (std::fabs(pos[1] - base) <= kPropSignHeightEpsilon) continue;
        pos[1] = base;
        occupant->SetPosition(pos);                                  // occupant vslot 0x48
        g_host->OccupantManager()->UpdateOccupant(occupant, true);   // occupant manager vslot 0xc0
    }
}

// ---------------------------------------------------------------------------------------------------------
// the registry slice
// ---------------------------------------------------------------------------------------------------------

// @ 0x100124d32. One tract (8x8 cells). The algorithm, in the original's order:
//   1. ask every requester for its share of the tract (cISC4PropRequester slot 0x18),
//   2. walk the cells; for every lot / network lot take the optional prop slots of its configuration and match
//      the props already in the cell to them by lot tag, keeping/reusing or removing them,
//   3. if at least one lot was seen, scale the total slot count by the average lot occupancy and split the
//      result over the requesters (remainder handed out round-robin from requester 1),
//   4. remove one excess prop per over-filled requester, then create props for the remaining slots.
void cSC4PropDeveloper::UpdateTract(int tractX, int tractZ) {
    const std::size_t requesterCount = requesters_.size();
    const SC4Rect<int> tract{tractX * kPropTractCells, tractZ * kPropTractCells,
                             tractX * kPropTractCells + kPropTractCells - 1,
                             tractZ * kPropTractCells + kPropTractCells - 1};

    ++stats_.tracts;
    std::vector<std::list<PropSlot>> pending(requesterCount);
    std::list<PropSlot> unslotted;
    std::vector<std::uint32_t> wanted(requesterCount, 0);
    std::uint32_t totalWanted = 0;
    for (std::size_t i = 0; i < requesterCount; ++i) {
        wanted[i] = requesters_[i] ? requesters_[i]->GetRequestedShare(&tract) : 0;   // requester vslot 0x18
        totalWanted += wanted[i];
    }
    // The original also builds a vector<bool> of (wanted[i] != 0) here and never reads it (0x100124e1c).
    std::vector<std::uint32_t> existing(requesterCount, 0);
    std::uint32_t slotTotal = 0;      // +0xe0: props in the configurations (kept + missing)
    std::uint32_t createdTotal = 0;   // +0x48: props kept
    float occupancySum = 0.0f;
    int lotCount = 0;

    for (int cellX = tract.x0; cellX <= tract.x1; ++cellX) {
        for (int cellZ = tract.z0; cellZ <= tract.z1; ++cellZ) {
            cSC4Lot* lot = g_host->LotManager()->GetLot(cellX, cellZ, true);   // lot manager vslot 0x28
            const std::vector<cSC4LotConfigurationObject>* objects = nullptr;
            if (lot) {
                ++stats_.lotsFound;
                if (lot->IsInLivableState() && lot->GetBuildingType(false) != 0) {
                    ++stats_.lotsLivable;
                    occupancySum += lot->GetOccupancy();   // lot vslot 0x1e8 (a float)
                    ++lotCount;
                    objects = g_host->LotConfigurationObjects(lot);   // lot 0x48 -> config 0xb8
                }
                if (!objects) continue;
            } else {
                cSC4NetworkLot* networkLot = g_host->NetworkLotManager()->GetNetworkLot(cellX, cellZ);
                if (!networkLot) continue;
                ++stats_.networkLots;
                objects = &networkLot->GetLotConfigurationObjectArray();   // network lot vslot 0x28
            }

            const std::size_t objectCount = objects->size();
            std::vector<bool> unfilled(objectCount, false);
            std::uint32_t optionalCount = 0;
            for (std::size_t i = 0; i < objectCount; ++i) {
                const cSC4LotConfigurationObject& object = (*objects)[i];
                if (object.type == 1 && (object.flags & 1) != 0) {   // object type 1 = prop, bit 0 = optional
                    unfilled[i] = true;
                    ++optionalCount;
                }
            }

            std::vector<cISC4Occupant*> props;
            if (lot) {
                g_host->PropManager()->PropsInLot(props, lot);              // vslot 0xb8
            } else {
                g_host->PropManager()->PropsInCell(props, cellX, cellZ);    // vslot 0xf0
            }
            int createdHere = 0;
            for (cISC4Occupant* occupant : props) {
                if (!occupant) continue;
                std::uint32_t tag = 0;
                std::size_t index = 0;
                bool matched = false;
                if (occupant->GetLotTag(tag)) {   // occupant vslot 0xd8
                    while (index < objectCount && (*objects)[index].id != tag) ++index;
                    matched = index < objectCount;
                }
                if (!matched) {
                    // Quirk: a prop that does not belong to the configuration *decrements* the count.
                    --createdHere;
                    occupant->Release();
                    continue;
                }
                bool keep = false;
                bool removed = false;
                if (unfilled[index]) {
                    unfilled[index] = false;
                    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
                    std::uint32_t requesterIndex = 0;
                    const bool hasRequester =
                        holder && cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyRequester, requesterIndex);
                    if (!hasRequester) {
                        keep = true;
                    } else if (requesterIndex < requesterCount) {
                        cISC4PropRequester* requester = requesters_[requesterIndex];
                        // Port: a saved requester whose class does not resolve reads as null (kept in place: props
                        // store requester indices); the original never meets one.
                        if (!requester || !requester->UsesCustomPropSelector()) {   // requester vslot 0x28
                            keep = true;
                        } else {
                            cISC4PropOccupant* prop = nullptr;
                            if (occupant->QueryInterface(kGZIID_cISC4PropOccupant,
                                                         reinterpret_cast<void**>(&prop)) &&
                                prop) {
                                const std::uint32_t propType = prop->GetPropType();   // prop vslot 0x38
                                std::vector<std::uint32_t> candidates{propType};
                                const std::uint32_t chosen = requester->SelectProp(candidates, cellX, cellZ);
                                prop->Release();
                                keep = chosen == propType;
                            }
                        }
                    } else {
                        keep = true;
                    }
                    if (keep) {
                        occupant->AddRef();   // the pending list owns a reference
                        PropSlot slot;
                        slot.x = static_cast<std::uint8_t>(cellX);
                        slot.z = static_cast<std::uint8_t>(cellZ);
                        slot.objects = const_cast<std::vector<cSC4LotConfigurationObject>*>(objects);
                        slot.objectIndex = static_cast<std::uint32_t>(index);
                        slot.occupant = occupant;
                        pending[requesterIndex].push_back(slot);
                        ++existing[requesterIndex];
                        ++createdHere;
                        if (optionalCount > 0) --optionalCount;
                    } else {
                        removed = true;
                    }
                }
                if (removed) {
                    g_host->PropManager()->RemoveProp(occupant);   // vslot 0xe8
                    unfilled[index] = true;
                }
                occupant->Release();
            }

            for (std::size_t i = 0; i < objectCount; ++i) {
                if (!unfilled[i]) continue;
                PropSlot slot;
                slot.x = static_cast<std::uint8_t>(cellX);
                slot.z = static_cast<std::uint8_t>(cellZ);
                slot.objects = const_cast<std::vector<cSC4LotConfigurationObject>*>(objects);
                slot.objectIndex = static_cast<std::uint32_t>(i);
                slot.occupant = nullptr;
                unslotted.push_back(slot);
            }
            slotTotal += static_cast<std::uint32_t>(createdHere + static_cast<int>(optionalCount));
            createdTotal += static_cast<std::uint32_t>(createdHere);
        }
    }

    if (lotCount != 0) {
        ++stats_.tractsWithLots;
        stats_.slots += static_cast<std::int32_t>(slotTotal);   // the original's counts can go below zero
        stats_.kept += static_cast<std::int32_t>(createdTotal);
        const float averageOccupancy = occupancySum / static_cast<float>(lotCount);
        if (totalWanted == 0) {
            ++stats_.noShare;
            // The original divides by zero here when every requester asks for nothing (0x1001255ea).
            // The port skips the distribution instead of trapping.
        } else {
            const int targetTotal = static_cast<int>(std::floor(static_cast<float>(slotTotal) * averageOccupancy));
            std::vector<std::uint32_t> target(requesterCount, 0);
            const std::uint32_t share = static_cast<std::uint32_t>(targetTotal) / totalWanted;
            std::uint32_t remainder = static_cast<std::uint32_t>(targetTotal) % totalWanted;
            for (std::size_t i = 0; i < requesterCount; ++i) {
                target[i] = wanted[i] * share;
            }
            // Round-robin remainder, starting at requester 1 (quirk 0x100125625) and skipping zero shares.
            std::size_t i = 0;
            while (remainder > 0 && requesterCount > 0) {
                i = (i + 1 == requesterCount) ? 0 : i + 1;
                if (wanted[i] == 0) continue;
                --wanted[i];
                ++target[i];
                --remainder;
            }

            // Remove one excess prop per over-filled requester.
            std::uint32_t removed = 0;
            for (std::size_t r = 0; r < requesterCount; ++r) {
                if (target[r] >= existing[r] || pending[r].empty()) continue;
                const std::int32_t pick = random_.RandomSint32RangeUniform(
                    0, static_cast<std::int32_t>(existing[r]));
                auto node = pending[r].begin();
                std::advance(node, pick);
                PropSlot copy = *node;
                g_host->PropManager()->RemoveProp(copy.occupant);
                if (copy.occupant) copy.occupant->Release();
                pending[r].erase(node);
                copy.occupant = nullptr;
                unslotted.push_back(copy);
                ++removed;
                ++stats_.removed;
            }

            std::int32_t available = static_cast<std::int32_t>(slotTotal + removed - createdTotal);
            for (std::size_t r = 0; r < requesterCount && available > 0; ++r) {
                std::int32_t need = static_cast<std::int32_t>(target[r]) - static_cast<std::int32_t>(existing[r]);
                while (need > 0 && available > 0 && !unslotted.empty()) {
                    const std::int32_t pick = random_.RandomSint32RangeUniform(0, available);
                    auto node = unslotted.begin();
                    std::advance(node, pick < static_cast<std::int32_t>(unslotted.size()) ? pick
                                                                                          : static_cast<std::int32_t>(unslotted.size()) - 1);
                    const std::uint32_t created = SelectAndCreateProp(&*node, static_cast<std::uint32_t>(r));
                    ++stats_.createCalls;
                    if (created != 0) ++stats_.created;
                    unslotted.erase(node);
                    need -= created != 0 ? 1 : 0;
                    --available;
                }
            }
        }
    }

    // Release the extra references the pending slots hold (the props stay in the city).
    for (std::list<PropSlot>& list : pending) {
        for (const PropSlot& slot : list) {
            if (slot.occupant) slot.occupant->Release();
        }
    }
}

// @ 0x100125d30. Creates the prop of one configuration object: gather the prop types of the object's `extra`
// ids that satisfy the requester, let the requester pick one, create it through the lot developer or the
// network lot manager, and tag it with the requester index.
std::uint32_t cSC4PropDeveloper::SelectAndCreateProp(PropSlot* slot, std::uint32_t requesterIndex) {
    if (!slot || !slot->objects || slot->objectIndex >= slot->objects->size()) return 0;
    const cSC4LotConfigurationObject& object = (*slot->objects)[slot->objectIndex];
    const std::size_t idCount = object.extra.size();
    if (idCount == 0) return 0;
    if (requesterIndex >= requesters_.size() || !requesters_[requesterIndex]) return 0;
    cISC4PropRequester* requester = requesters_[requesterIndex];
    const std::uint32_t satisfaction = requester->GetRequesterSatisfactionId();   // requester vslot 0x20

    std::vector<std::uint32_t> candidates;
    for (std::uint32_t id : object.extra) {
        // i386 0x000d79fc: an id that is not a valid prop type is expanded as a family (prop manager i386 slot 8 = IsPropTypeValid)
        if (!g_host->PropManager()->IsPropTypeValid(id)) {           // prop manager vslot 0x40
            std::vector<std::uint32_t> members;
            if (g_host->PropManager()->GetPropTypesByFamily(members, id) <= 0) continue;   // vslot 0x48
            for (std::uint32_t member : members) {
                const PropProfile* profile = g_host->PropManager()->GetPropProfile(member);   // vslot 0x68
                if (profile && profile->valueA == satisfaction) candidates.push_back(member);
            }
        } else {
            const PropProfile* profile = g_host->PropManager()->GetPropProfile(id);
            if (profile && profile->valueA == satisfaction) candidates.push_back(id);
        }
    }
    if (candidates.empty()) return 0;

    std::uint32_t propType = 0;
    if (!requester->UsesCustomPropSelector()) {   // requester vslot 0x28
        propType = candidates[random_.RandomUint32Uniform(static_cast<std::uint32_t>(candidates.size()))];
    } else {
        propType = requester->SelectProp(candidates, slot->x, slot->z);   // requester vslot 0x30
    }
    if (propType == 0) return 0;

    cISC4Occupant* occupant = nullptr;
    cSC4Lot* lot = g_host->LotManager()->GetLot(slot->x, slot->z, false);
    if (lot) {
        IPropLotDeveloperApi* developer = g_host->BuildingDevelopment()->GetLotDeveloper();   // vslot 0x120
        occupant = developer ? developer->CreateLotProp(lot, static_cast<int>(slot->objectIndex), propType)
                             : nullptr;                                                       // vslot 0x1f8
    } else {
        cSC4NetworkLot* networkLot = g_host->NetworkLotManager()->GetNetworkLot(slot->x, slot->z);
        if (!networkLot) return 0;   // the original returns 0 when the cell has no lot at all
        occupant = g_host->NetworkLotManager()->CreateProp(networkLot, static_cast<int>(slot->objectIndex),
                                                           propType);   // vslot 0x40
    }
    if (!occupant) return 0;
    occupant->AddRef();   // occupant vslot 0x08
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();   // vslot 0x30
    if (holder) cSCPropertyHelp::SetPropertyValue(holder, kPropPropertyRequester, requesterIndex);
    occupant->Release();  // vslot 0x10
    // Quirk kept: the original only writes `bl` (0x100126035), so the value is (idCount & ~0xff) | 1.
    return (static_cast<std::uint32_t>(idCount) & ~0xffu) | 1u;
}

// @ 0x1001260b2. Re-runs the registry for every tract of the city.
void cSC4PropDeveloper::UpdateAllOptionalProps() {
    if (!g_host || !g_host->City()) return;
    const std::uint32_t cellsX = static_cast<std::uint32_t>(g_host->City()->CellCountX());
    const std::uint32_t cellsZ = static_cast<std::uint32_t>(g_host->City()->CellCountZ());
    const std::uint32_t tractsX = cellsX >> 3;
    const std::uint32_t tractsZ = cellsZ >> 3;
    for (std::uint32_t x = 0; x < tractsX; ++x) {
        for (std::uint32_t z = 0; z < tractsZ; ++z) {
            UpdateTract(static_cast<int>(x), static_cast<int>(z));
        }
    }
}

// @ 0x10012612a. Shows or hides the sign props by setting / clearing occupant flag 2.
void cSC4PropDeveloper::ShowSignsAndLabels(bool show) {
    if (signsVisible_ == show) return;
    signsVisible_ = show;
    for (cISC4PropOccupant* prop : signProps_) {
        if (!prop) continue;
        cISC4Occupant* occupant = prop->AsOccupant();   // prop vslot 0x18
        if (show) {
            occupant->SetFlag(kPropSignVisibleFlag);     // occupant vslot 0xe8
        } else {
            occupant->ClearFlag(kPropSignVisibleFlag);   // occupant vslot 0xf8
        }
    }
}

// @ 0x100126448: re-enters the vtable's ShowSignsAndLabels slot with the inverted flag.
void cSC4PropDeveloper::ToggleSignsAndLabels() { ShowSignsAndLabels(!signsVisible_); }

// ---------------------------------------------------------------------------------------------------------
// HandleInsertOccupant
// ---------------------------------------------------------------------------------------------------------

// @ 0x100123632.
void cSC4PropDeveloper::HandleInsertOccupant(cISC4Occupant* occupant) {
    if (!occupant || !g_host) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();   // occupant vslot 0x30
    if (!holder) return;
    // Props that cast no shadow are not prop-developer props at all.
    if (holder->HasProperty(kPropPropertyNoShadow)) return;

    std::uint8_t hasState = 0;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyNightState, hasState) &&
        hasState == kPropPropertyLotStateValue) {
        cISC4PropOccupant* prop = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4PropOccupant, reinterpret_cast<void**>(&prop)) && prop) {
            stateProps_.push_back(prop);   // the QI reference moves into the vector
            if (prop->GetState() != (night_ ? 1u : 0u)) {   // prop vslot 0x60
                bool apply = true;
                if (night_ && powerSimulator_) {
                    float pos[3] = {0.0f, 0.0f, 0.0f};
                    occupant->GetPosition(pos);   // occupant vslot 0x40
                    apply = powerSimulator_->IsPowered(static_cast<int>(pos[0] * kPropCellSizeReciprocal),
                                                       static_cast<int>(pos[2] * kPropCellSizeReciprocal));
                }
                if (apply) prop->SetState(night_ ? 1 : 0);     // prop vslot 0x58
            }
        }
    }

    cISC4PropOccupant* prop = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4PropOccupant, reinterpret_cast<void**>(&prop)) || !prop) return;
    const bool interesting = holder->HasProperty(kPropPropertyEffectId) ||
                             holder->HasProperty(kPropPropertyTimeWindow) ||
                             holder->HasProperty(kPropPropertyDatePropType) ||
                             holder->HasProperty(kPropPropertyPowerTimer);
    // The sign check at 0x100123e11 is reached both from the end of the property work and from the
    // "none of the four properties" branch, so a sign is listed even without them.
    if (interesting) {
    prop->SetState(1);   // slot 0x58
    std::uint8_t randomChance = 100;
    cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyRandomChance, randomChance);
    prop->SetRandomChance(randomChance);   // slot 0x88

    std::uint32_t effectId = 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyEffectId, effectId) || effectId == 0) {
        prop->SetPropTimerState(kPropTimerBitEffect, true);   // slot 0xa8
    } else {
        IPropView3DApi* view = g_host->View3D();
        if (view) {
            std::list<cISC4PropOccupant*>& list = effectProps_[effectId];
            list.push_back(prop);
            prop->AddRef();
            prop->SetPropTimerState(kPropTimerBitEffect, view->GlobalEffectTriggered(effectId));   // view vslot 0x170
            view->Release();
        }
    }

    // Time-of-day window: property 0x4a149631 holds a two-float curve (start, end).
    cISCProperty* property = holder->GetProperty(kPropPropertyTimeWindow);   // holder vslot 0x28
    if (!property) {
        prop->SetPropTimerState(kPropTimerBitTimeOfDay, true);
    } else {
        const cIGZVariant* value = property->GetPropertyValue();   // property vslot 0x30 (non-const)
        const float* times = value ? value->RefFloat32() : nullptr;   // variant vslot 0x3a8
        const std::uint32_t count = value ? value->GetCount() : 0;    // variant vslot 0x20
        if (times && count >= 2 && (times[0] != times[1] || std::isnan(times[0]) || std::isnan(times[1]))) {
            timeProps_.push_back(prop);
            prop->AddRef();
            prop->SetTimeOfDayRange(times[0], times[1]);   // slot 0x68
        }
    }

    // Date range: property 0xa751675 holds the prop type of the dated variant, 0xca7515cc the month/day and
    // 0x4a764564 the span in days (default 1).
    property = holder->GetProperty(kPropPropertyDatePropType);
    if (!property) {
        prop->SetPropTimerState(kPropTimerBitDate, true);
    } else {
        const std::uint32_t datePropType = Uint32Of(property->GetPropertyValue());
        std::uint8_t month = 0, day = 0;
        if (cISCProperty* monthDay = holder->GetProperty(kPropPropertyDateMonthDay)) {
            const cIGZVariant* value = monthDay->GetPropertyValue();
            const std::uint8_t* bytes = value ? value->RefUint8() : nullptr;   // variant vslot 0x318
            if (bytes && value->GetCount() >= 2) {
                month = bytes[0];
                day = bytes[1];
            }
        }
        std::uint32_t span = 1;
        cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyDateSpan, span);
        prop->SetSimulatorDateRange(datePropType, span, month, day);   // slot 0x78
        dateProps_.push_back(prop);
        prop->AddRef();
    }

    // Power-timer props follow the power grid.
    if (holder->HasProperty(kPropPropertyPowerTimer)) {
        float pos[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(pos);
        const bool powered = powerSimulator_ &&
                             powerSimulator_->IsPowered(static_cast<int>(pos[0] * kPropCellSizeReciprocal),
                                                        static_cast<int>(pos[2] * kPropCellSizeReciprocal));
        prop->SetPropTimerState(kPropTimerBitPower, powered);   // slot 0xa8
        powerProps_.push_back(prop);
        prop->AddRef();
    }

    }

    // Signs sit on top of the terrain and can be hidden as a group.
    const std::uint32_t propType = prop->GetPropType();   // slot 0x38
    if (propType == kPropTypeSignpost || propType == kPropTypeNetworkSign) {
        signProps_.push_back(prop);
        prop->AddRef();
    }
    prop->Release();   // the QI reference
}

// @ 0x100123edc.
void cSC4PropDeveloper::HandleRemoveOccupant(cISC4Occupant* occupant) {
    if (!occupant || !g_host) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (!holder) return;
    if (holder->HasProperty(kPropPropertyNoShadow)) return;

    std::uint8_t hasState = 0;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyNightState, hasState) &&
        hasState == kPropPropertyLotStateValue) {
        cISC4PropOccupant* prop = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4PropOccupant, reinterpret_cast<void**>(&prop)) && prop) {
            const auto it = std::find(stateProps_.begin(), stateProps_.end(), prop);
            if (it != stateProps_.end()) {
                stateProps_.erase(it);
                prop->Release();   // the vector's reference
            }
            prop->Release();       // the QI reference
        }
    }

    cISC4PropOccupant* prop = nullptr;
    if (!occupant->QueryInterface(kGZIID_cISC4PropOccupant, reinterpret_cast<void**>(&prop)) || !prop) return;

    std::uint32_t effectId = 0;
    if (cSCPropertyHelp::GetPropertyValue(holder, kPropPropertyEffectId, effectId) && effectId != 0) {
        if (IPropView3DApi* view = g_host->View3D()) {
            const auto mapIt = effectProps_.find(effectId);
            if (mapIt != effectProps_.end()) {
                for (auto it = mapIt->second.begin(); it != mapIt->second.end(); ++it) {
                    if (*it != prop) continue;
                    mapIt->second.erase(it);
                    if (mapIt->second.empty()) effectProps_.erase(mapIt);
                    prop->Release();   // the list's reference
                    break;
                }
            }
            view->Release();
        }
    }

    if (holder->HasProperty(kPropPropertyTimeWindow)) {
        const auto it = std::find(timeProps_.begin(), timeProps_.end(), prop);
        if (it != timeProps_.end()) {
            timeProps_.erase(it);
            prop->Release();
        }
    }
    if (holder->HasProperty(kPropPropertyDatePropType)) {
        const auto it = std::find(dateProps_.begin(), dateProps_.end(), prop);
        if (it != dateProps_.end()) {
            dateProps_.erase(it);
            prop->Release();
        }
    }
    if (holder->HasProperty(kPropPropertyPowerTimer)) {
        const auto it = std::find(powerProps_.begin(), powerProps_.end(), prop);
        if (it != powerProps_.end()) {
            powerProps_.erase(it);
            prop->Release();
        }
    }

    const std::uint32_t propType = prop->GetPropType();
    if (propType == kPropTypeSignpost || propType == kPropTypeNetworkSign) {
        const auto it = std::find(signProps_.begin(), signProps_.end(), prop);
        if (it != signProps_.end()) {
            signProps_.erase(it);
            prop->Release();
        }
    }
    prop->Release();   // the QI reference
}

// @ 0x10012428c. A network sign is removed from the city when its cell no longer supports it: the sign prop
// whose position converts to the removed occupant's first bounding cell is handed to the prop manager.
void cSC4PropDeveloper::HandleRemoveNetworkOccupant(cISC4Occupant* occupant) {
    if (!occupant || !g_host) return;
    SC4CellRect rect{};
    occupant->GetBoundingCityCells(rect);   // occupant vslot 0x58 (only the first cell is used)
    const int cellX = rect.x0;
    const int cellZ = rect.z0;
    for (cISC4PropOccupant* prop : signProps_) {
        if (!prop) continue;
        if (prop->GetPropType() != kPropTypeNetworkSign) continue;
        cISC4Occupant* signOccupant = prop->AsOccupant();
        float pos[3] = {0.0f, 0.0f, 0.0f};
        signOccupant->GetPosition(pos);
        int signX = 0, signZ = 0;
        g_host->City()->PositionToCell(pos[0], pos[2], signX, signZ);   // city vslot 0x2f8
        if (signX != cellX || signZ != cellZ) continue;
        g_host->PropManager()->RemoveProp(signOccupant);
    }
}

}  // namespace sc4
