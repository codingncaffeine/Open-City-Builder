// cSC4MySimDispatch — see MySimDispatch.h for the recovered layout, the seam inventory and the quirks kept. Each
// definition below names its original address.
#include "sc4/MySimDispatch.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "gz/cIGZSerializable.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/GeometryUtilities.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {
namespace {

IMySimDispatchHost* g_host = nullptr;
IMySimDispatchHost* Host() { return g_host; }

// The original rounds world coordinates to standard cells with the float add-0.5 / 0x4b400000 (1.5*2^23) magic-number
// trick followed by a bias subtraction, which is floor(x / 16) for every finite value (see the comment on
// cSC4MySimDispatch::cellFromPosition).
int CellFromPosition(float value) {
    return static_cast<int>(std::floor(value * 0.0625f));
}

// The automaton's traffic automaton view (the original QIs the occupant for 0x8b99ee7c at the call site); the host answers
// it because the port's automaton classes and the dispatch seams are separate objects. One counted reference.
IMySimDispatchRoute* AcquireRoute(cISC4Occupant* occupant) {
    IMySimDispatchHost* host = MySimDispatchHost();
    return host && occupant ? host->RouteOf(occupant) : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// cLocation (0x1006ac7da..0x1006acaac) — the halves cSC4MySim does not already own. The type is sc4::MySimLocation
// (MySim.h) so cSC4MySim, the path segment code and the dispatch share one layout.
// ---------------------------------------------------------------------------------------------------------------
bool MySimLocationIsValid(const MySimLocation& location) {
    IMySimDispatchHost* host = Host();
    if (!location.hasOccupant) return MySimLocationAnyTravelConnection(location, location.lotCellX, location.lotCellZ);
    if (location.cellX < 0 || location.cellZ < 0) return false;
    if (!host) return true;
    return location.cellX < host->CityCellsX() && location.cellZ < host->CityCellsZ();
}

// 0x1006ac84e AnyTravelConnectionExists: the occupant manager's neighbour list for the (lot) cell, filtered through
// the 13 valid connection kinds; the original returns true on the first connection the neighbours simulator reports.
bool MySimLocationAnyTravelConnection(const MySimLocation&, int cellX, int cellZ) {
    IMySimDispatchHost* host = Host();
    if (!host) return false;
    return host->AnyTravelConnection(cellX, cellZ);
}

// 0x1006ac930 cLocation::Write. The occupant is a serialized object (SC4WriteGZSerializable), the two positions are
// floats and everything else is 32-bit except the two booleans.
bool MySimLocationWrite(const MySimLocation& location, cIGZOStream& stream) {
    IMySimDispatchHost* host = Host();
    if (host) host->WriteSerializable(stream, location.occupant.get());
    stream.SetFloat32(location.x);
    stream.SetFloat32(location.z);
    stream.SetUint32(static_cast<std::uint32_t>(location.cellX));
    stream.SetUint32(static_cast<std::uint32_t>(location.cellZ));
    stream.SetUint32(static_cast<std::uint32_t>(location.lotCellX));
    stream.SetUint32(static_cast<std::uint32_t>(location.lotCellZ));
    stream << location.hasOccupant;
    stream << location.commuteLocation;
    stream.SetUint32(static_cast<std::uint32_t>(location.data24));
    stream.SetUint32(static_cast<std::uint32_t>(location.data28));
    return !stream.GetError();
}

// 0x1006ac9e2 cLocation::Read: releases the occupant it held before reading the new one (the original clears the slot
// first and lets SC4ReadGZSerializable fill it).
bool MySimLocationRead(MySimLocation& location, cIGZIStream& stream) {
    IMySimDispatchHost* host = Host();
    location.occupant = nullptr;
    if (host) {
        cIGZUnknown* object = nullptr;
        host->ReadSerializable(stream, kGZIID_cISC4OccupantSaved, reinterpret_cast<void**>(&object));
        if (object) {
            void* occupant = nullptr;
            if (object->QueryInterface(kGZIID_cISC4Occupant, &occupant)) {
                location.occupant = static_cast<cISC4Occupant*>(occupant);
                location.occupant->Release();   // QueryInterface took a reference; the auto-ref has its own
            }
            object->Release();
        }
    }
    stream.GetFloat32(location.x);
    stream.GetFloat32(location.z);
    std::uint32_t value = 0;
    stream.GetUint32(value); location.cellX = static_cast<std::int32_t>(value);
    stream.GetUint32(value); location.cellZ = static_cast<std::int32_t>(value);
    stream.GetUint32(value); location.lotCellX = static_cast<std::int32_t>(value);
    stream.GetUint32(value); location.lotCellZ = static_cast<std::int32_t>(value);
    stream >> location.hasOccupant;
    stream >> location.commuteLocation;
    stream.GetUint32(value); location.data24 = static_cast<std::int32_t>(value);
    stream.GetUint32(value); location.data28 = static_cast<std::int32_t>(value);
    return !stream.GetError();
}

IMySimDispatchHost* MySimDispatchHost() { return g_host; }
void SetMySimDispatchHost(IMySimDispatchHost* host) { g_host = host; }

int cSC4MySimDispatch::cellFromPosition(float value) const { return CellFromPosition(value); }

// ---------------------------------------------------------------------------------------------------------------
// Construction / destruction / interfaces (0x1006acb52, 0x1006ace62, 0x1006ad7f0..0x1006ad89c)
// ---------------------------------------------------------------------------------------------------------------
cSC4MySimDispatch::cSC4MySimDispatch(IMySimDispatchOwner* owner) : owner_(owner) {
    // The cLocation members are default-constructed to the cLocation ctor values (x/z = -1000, cells = -1,
    // hasOccupant = true). The ctor also pre-sets the tuning values the original stores (0xa/0xa/5/0/5).
    if (owner_) owner_->AddRef();
}

cSC4MySimDispatch::~cSC4MySimDispatch() {
    // Original order: owner (+0x170) first, then the route/model/destination containers, then path finder (+0x180),
    // clock (+0x178), the (now null) owner test, the automaton occupant (+0x168), the label string, the tracked
    // occupant (+0x130) and the four locations.
    if (owner_) {
        IMySimDispatchOwner* owner = owner_;
        owner_ = nullptr;
        owner->Release();
    }
    if (pathFinder_) {
        cISC4PathFinder* pathFinder = pathFinder_;
        pathFinder_ = nullptr;
        pathFinder->Release();
    }
    if (clock_) {
        cIGZUnknown* clock = clock_;
        clock_ = nullptr;
        clock->Release();
    }
    if (owner_) {   // always null: the original leaves the same redundant test in place
        owner_->Release();
        owner_ = nullptr;
    }
    automataOccupant_ = nullptr;
    trackedOccupant_ = nullptr;
}

bool cSC4MySimDispatch::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kGZIID_cISC4MySimDispatch) {
        *out = static_cast<IMySimDispatch*>(this);
    } else if (iid == kGZIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == kGZIID_cIGZSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4MySimDispatch::Release() {
    if (refCount_ > 1) return --refCount_;
    delete this;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Init / Shutdown / Reset (0x1006ad11a / 0x1006ad48a / 0x1006ad584)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::Init() {
    IMySimDispatchHost* host = Host();
    if (!pathFinder_ && host) {
        if (cIGZMessageServer2* server = host->MessageServer()) {
            server->AddNotification(this, kMySimDispatchMsgFiveMinuteTick);
            server->AddNotification(this, kMySimDispatchMsgFollow);
            server->AddNotification(this, kMySimDispatchMsgView1);
            server->AddNotification(this, kMySimDispatchMsgView2);
        }
        host->RegisterInputControlTarget(this);
        if (cISC4PathFinder* pathFinder = host->PathFinder()) {
            pathFinder->AddRef();
            pathFinder_ = pathFinder;
        }
    }
    if (!initPending_) {
        if (host) {
            cIGZUnknown* clock = host->Clock();
            if (clock != clock_) {
                if (clock) clock->AddRef();
                cIGZUnknown* old = clock_;
                clock_ = clock;
                if (old) old->Release();
            }
        }
        // Reset the default destination (+0xa0) and the commute destination (+0x100), then start the sim at the
        // commute destination with its commute flag set.
        defaultDestination_ = cLocation();
        commuteDestination_ = cLocation();
        commuteDestination_.commuteLocation = true;
        currentLocation_ = commuteDestination_;
        setState(kMySimTripIdle);
        resetPath();
        follow_ = false;
    }
    return true;
}

bool cSC4MySimDispatch::Shutdown() {
    IMySimDispatchHost* host = Host();
    if (pathFinder_ && host) {
        host->UnregisterInputControlTarget(this);
        if (cIGZMessageServer2* server = host->MessageServer()) {
            server->RemoveNotification(this, kMySimDispatchMsgFiveMinuteTick);
            server->RemoveNotification(this, kMySimDispatchMsgFollow);
            server->RemoveNotification(this, kMySimDispatchMsgView1);
            server->RemoveNotification(this, kMySimDispatchMsgView2);
        }
    }
    const std::uint32_t savedFlags = flags_;
    StopTracking();
    setFlags(kMySimDispatchFlagTracking, (savedFlags >> 8) & 1);
    Reset();
    if (clock_) {
        cIGZUnknown* clock = clock_;
        clock_ = nullptr;
        clock->Release();
    }
    if (pathFinder_) {
        cISC4PathFinder* pathFinder = pathFinder_;
        pathFinder_ = nullptr;
        pathFinder->Release();
    }
    return true;
}

bool cSC4MySimDispatch::Reset() {
    StopTracking();
    if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
        automaton->RemoveCallback(this);
        automaton->Remove(false);
        automaton->Release();
    }
    setFlags(0xffffffffu, false);   // the original passes -1, i.e. clears every bit
    destinationQueue_.clear();

    currentLocation_ = cLocation();
    currentDestination_ = cLocation();
    defaultDestination_ = cLocation();

    if (travelType_ != kTravelTypeNone) ++changeCounter_;
    travelType_ = kTravelTypeNone;
    setState(kMySimTripNone);
    resetPath();
    automataOccupant_ = nullptr;
    return true;
}

// 0x1006ad3c8 setState: remembers the previous state, flags the idle state and tells the owner about the change.
bool cSC4MySimDispatch::setState(int state) {
    const int oldState = state_;
    previousState_ = oldState;
    state_ = state;
    if (state == kMySimTripIdle) setFlags(kMySimDispatchFlagDriveView, true);
    if (oldState != state_) {
        notifyMySim(kDispatchNotifyTripLeg, oldState, state_, 0);
        ++changeCounter_;
    }
    return true;
}

// 0x1006ad41c resetPath: drop the route and let the path finder write into it again.
void cSC4MySimDispatch::resetPath() {
    route_.clear();
    setFlags(0x5800, false);
    pathSegmentEnd_ = 0;
    pathSegmentStart_ = 0;
    if (pathFinder_) {
        pathFinder_->Reset();                 // 0xb0
        pathFinder_->ClearAllTravelTypeFlags();   // 0x78
        pathFinder_->SetRouteInfo(&route_);   // 0x40
    }
}

// 0x1006afa56 setFlags: every change is reported to the owner (event 0x4bb2d020, old and new flag word).
bool cSC4MySimDispatch::setFlags(std::uint32_t flags, bool on) {
    const std::uint32_t oldFlags = flags_;
    flags_ = on ? (oldFlags | flags) : (oldFlags & ~flags);
    if (oldFlags != flags_) {
        notifyMySim(kDispatchNotifyTripEnd, static_cast<int>(oldFlags), static_cast<int>(flags_), 0);
        ++changeCounter_;
    }
    return true;
}

// 0x1006aea60 notifyMySim: build a cRZMessage2Standard of type 0x2ba06b99 and call the owner's DoMessage directly.
void cSC4MySimDispatch::notifyMySim(std::uint32_t event, int data2, int data3, int data4) {
    if (!owner_) return;
    static const bool debug = std::getenv("SC4_DEBUG") != nullptr;
    if (debug) {
        std::fprintf(stderr, "mysimdispatch: notify %08x %d %d (state %d travel %d time %d at %d,%d)\n", event, data2, data3,
                     state_, travelType_, time_, currentLocation_.cellX, currentLocation_.cellZ);
    }
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kMySimDispatchMsgNotify);
    message->SetData1(event);
    message->SetData2(static_cast<std::uint64_t>(data2));
    message->SetData3(static_cast<std::uint64_t>(data3));
    message->SetData4(static_cast<std::uint64_t>(data4));
    owner_->DoMessage(message);
    message->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// Destinations (0x1006afdd8, 0x1006b00de, 0x1006afb8c, 0x1006b26fe, 0x1006b2860)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::AddDestinationLocation(const cLocation& location, int flags) {
    if ((flags & 4) != 0) destinationQueue_.clear();
    if ((flags & 1) != 0) {
        destinationQueue_.push_front(location);
    } else {
        destinationQueue_.push_back(location);
    }
    return true;
}

void cSC4MySimDispatch::SetDefaultDestination(const cLocation& location) {
    const bool valid = MySimLocationIsValid(location) && location.hasOccupant && !location.commuteLocation;
    if (valid) {
        if (currentLocation_ == defaultDestination_ && currentLocation_.occupant) currentLocation_.occupant = nullptr;
        defaultDestination_ = location;
        ++changeCounter_;
    }
    setFlags(kMySimDispatchFlagHasDefault, valid);
}

bool cSC4MySimDispatch::GoTo(const cLocation& location) {
    cLocation target = location;
    if (target.occupant && target.data24 < minTimeAtLocation_) target.data24 = minTimeAtLocation_;

    if (state_ != kMySimTripTravelling) {
        currentLocation_.data28 = time_ - 1;
        destinationQueue_.clear();
        destinationQueue_.push_back(target);
        return true;
    }

    destinationQueue_.clear();
    currentDestination_ = target;

    bool result = true;
    if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
        const bool markedForRemove = automaton->CheckAutomataFlag(1);
        automaton->Release();
        if (markedForRemove) {
            setFlags(0x5800, false);
            setFlags(kMySimDispatchFlagNeedPath, false);
            return true;
        }
    }
    setFlags(0x5800, false);
    setFlags(kMySimDispatchFlagNeedPath, true);
    updateState();
    if (route_.empty() || pathSegmentStart_ == pathSegmentEnd_) {
        setStuck();
        result = false;
    }
    return result;
}

bool cSC4MySimDispatch::SetCurrentCommuteDestination(const cLocation& location, bool notify) {
    cLocation target = location;
    if (!MySimLocationIsValid(target)) {
        if (!notify) return false;
        if (state_ != kMySimTripTravelling) {
            if (!validateCommuteLocation(target)) return false;
        }
    }
    commuteDestination_ = target;
    if (notify) notifyMySim(kDispatchNotifyArrived, 0, 0, 0);
    return true;
}

void cSC4MySimDispatch::TeleportToCommuteLocation() {
    currentLocation_ = commuteDestination_;
    if (travelType_ != kTravelTypeWalk) ++changeCounter_;
    travelType_ = kTravelTypeWalk;
    ++changeCounter_;
    currentLocation_.data28 = currentLocation_.data24 + time_;
    setState(kMySimTripIdle);
}

// ---------------------------------------------------------------------------------------------------------------
// Model keys / offsets (0x1006aff0e, 0x1006b000a)
// ---------------------------------------------------------------------------------------------------------------
void cSC4MySimDispatch::SetModelBaseResKey(int modelType, const cGZPersistResourceKey& key, int offset) {
    modelKeys_[modelType] = std::make_pair(key, offset);
}

void cSC4MySimDispatch::getModelKey(int modelType, cGZPersistResourceKey& key, int& offset) const {
    key = cGZPersistResourceKey();
    offset = -1;
    auto it = modelKeys_.find(modelType);
    if (it == modelKeys_.end()) return;
    key = it->second.first;
    offset = it->second.second;
}

void cSC4MySimDispatch::SetModelOffset(int modelType, int offset, bool snap) {
    auto it = modelKeys_.find(modelType);
    if (it == modelKeys_.end()) return;
    it->second.second = offset;
    if (!snap) return;
    if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
        automaton->PlayAnim(static_cast<std::uint32_t>(offset));
        automaton->Release();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Tracking through the dispatch view (0x1006b0266, 0x1006b01d2, 0x1006b03ec, 0x1006b0486)
// ---------------------------------------------------------------------------------------------------------------
cISC4Occupant* cSC4MySimDispatch::getOccupantToTrack() {
    if ((flags_ & 0x104) != 0x100) return nullptr;
    cISC4Occupant* result = automataOccupant_.get();
    if (((flags_ & kMySimDispatchFlagAtBuilding) != 0) || state_ == kMySimTripIdle) {
        cISC4Occupant* occupant = currentLocation_.occupant.get();
        if (occupant) {
            void* pylon = nullptr;
            const bool isPylon = occupant->QueryInterface(kMySimDispatchIidPylon, &pylon);
            if (pylon) static_cast<cIGZUnknown*>(pylon)->Release();
            if (!isPylon) result = occupant;
        }
    }
    return result;
}

void cSC4MySimDispatch::StopTracking() {
    setFlags(kMySimDispatchFlagTracking, false);
    if (trackHandle_ != -1) {
        if (IMySimDispatchHost* host = Host()) {
            if (IMySimDispatchVehicleView* view = host->DispatchVehicleView()) view->UntrackOccupant(trackHandle_, false);
        }
    }
    trackHandle_ = -1;
    trackedOccupant_ = nullptr;
}

void cSC4MySimDispatch::StartTracking(int bitmapId, const cRZString& text) {
    setFlags(kMySimDispatchFlagTracking, true);
    trackingBitmapId_ = static_cast<std::uint32_t>(bitmapId);
    trackingText_ = text;
    const std::uint32_t savedFlags = flags_;
    StopTracking();
    setFlags(kMySimDispatchFlagTracking, (savedFlags >> 8) & 1);
    if ((savedFlags & 4) == 0 && trackHandle_ == -1) {
        IMySimDispatchHost* host = Host();
        IMySimDispatchVehicleView* view = host ? host->DispatchVehicleView() : nullptr;
        cISC4Occupant* occupant = getOccupantToTrack();
        if (view && occupant) {
            const cRZString* label = bitmapId == 0 ? &text : nullptr;
            const int handle = view->TrackOccupant(occupant, 3, trackingBitmapId_, label);
            trackHandle_ = handle == 0 ? -1 : handle;
            if (handle != 0) {
                trackedOccupant_ = occupant;
                if (occupant == automataOccupant_.get() && follow_) {
                    if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) automaton->Release();
                    if (host) host->ClearVehiclePlayerDrive();
                }
            }
        }
    }
}

bool cSC4MySimDispatch::updateTrackingOccupant() {
    if (trackedChangeCounter_ == changeCounter_ && trackedOccupant_.get() == getOccupantToTrack()) return true;
    const std::uint32_t savedFlags = flags_;
    const bool tracking = ((savedFlags >> 8) & 1) != 0;
    if (trackedOccupant_ || trackHandle_ != -1) {
        StopTracking();
        setFlags(kMySimDispatchFlagTracking, tracking);
    }
    if ((savedFlags & kMySimDispatchFlagTracking) != 0) StartTracking(static_cast<int>(trackingBitmapId_), trackingText_);
    setFlags(kMySimDispatchFlagTracking, tracking);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Occupant / automaton helpers (0x1006b052c, 0x1006b059c, 0x1006b05e8)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::isBuilding(cLocation& location) const {
    cISC4Occupant* occupant = location.occupant.get();
    if (!occupant) return false;
    void* building = nullptr;
    const bool ok = occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, &building);
    if (building) static_cast<cIGZUnknown*>(building)->Release();
    return ok;
}

IMySimDispatchAutomataManager* cSC4MySimDispatch::pickAutomataManager(int automataGroup) {
    const unsigned offset = static_cast<unsigned>(automataGroup) - 0x4200u;
    bool vehicle = offset < 0x2fu && ((0x500000a00801ULL >> (offset & 0x3f)) & 1ULL) != 0;
    const unsigned footOffset = static_cast<unsigned>(automataGroup) - 0x4149u;
    bool pedestrian = footOffset <= 1u || automataGroup == static_cast<int>(kAutomataGroupTransit);
    if (!vehicle && !pedestrian) return nullptr;
    IMySimDispatchHost* host = Host();
    return host ? host->AutomataManager(static_cast<std::uint32_t>(automataGroup)) : nullptr;
}

bool cSC4MySimDispatch::setAutomataOccupant(cISC4Occupant* occupant) {
    if (trackedOccupant_.get() != occupant) {
        const std::uint32_t savedFlags = flags_;
        StopTracking();
        setFlags(kMySimDispatchFlagTracking, (savedFlags >> 8) & 1);
    }
    cISC4Occupant* current = automataOccupant_.get();
    if (current) {
        if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
            if (automaton->AsOccupant() != occupant) {
                automaton->RemoveCallback(this);
                automaton->Remove(false);
            }
            automaton->Release();
        }
    }
    if (current != occupant) automataOccupant_ = occupant;
    ++changeCounter_;
    return true;
}

IMySimDispatchAutomaton* cSC4MySimDispatch::acquireAutomaton() const {
    IMySimDispatchHost* host = Host();
    return host && automataOccupant_ ? host->AutomatonOf(automataOccupant_.get()) : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Commute validation / cell rectangles (0x1006b074c, 0x1006b0aac)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::validateCommuteLocation(cLocation& location) {
    resetPath();
    location = cLocation();
    location.commuteLocation = true;

    if ((flags_ & kMySimDispatchFlagHasDefault) == 0) return false;
    if (!pathFinder_) return false;
    IMySimDispatchHost* host = Host();
    if (!host) return false;

    pathFinder_->SetFlag(1);                    // 0x28
    pathFinder_->SetFlag(0x10);
    pathFinder_->SetTripDestinationFilter(0);   // 0x48
    const int wealth = owner_ ? owner_->GetWealth() : 0;
    pathFinder_->SetDestinationFlag(static_cast<std::uint8_t>(wealth == 3 ? 4 : wealth == 2 ? 2 : 1));   // 0x50
    pathFinder_->SetTrafficDensity(1);          // 0x90
    int defaultRect[4];
    getLocationCellRect(defaultDestination_, defaultRect);
    pathFinder_->SetStartRect(SC4RectInt32{defaultRect[0], defaultRect[1], defaultRect[2], defaultRect[3]});   // 0x98
    pathFinder_->SetTravelTypeFlag(0x1cf, true);   // 0x68: the commute masks every travel type

    if (host->FindPath(pathFinder_) != 1) return false;
    int cellX = 0, cellZ = 0;
    pathFinder_->GetEndCell(cellX, cellZ);      // 0xd8

    cISC4Occupant* building = host->BuildingAtCell(cellX, cellZ);
    if (building) {
        const bool commute = location.commuteLocation;
        location.SetByOccupant(building);
        location.commuteLocation = commute;
        location.hasOccupant = true;
        return true;
    }

    int neighborX = 0, neighborZ = 0;
    if (host->NeighborConnection(cellX, cellZ, neighborX, neighborZ)) {
        if (cellX < 0) cellX = 0;
        if (cellZ < 0) cellZ = 0;
        const int cityX = host->CityCellsX();
        const int cityZ = host->CityCellsZ();
        if (cityX <= cellX) cellX = cityX - 1;
        if (cityZ <= cellZ) cellZ = cityZ - 1;
        location.hasOccupant = false;
        location.occupant = nullptr;
        location.SetCellPosition(cellX, cellZ);
        location.lotCellX = neighborX;
        location.lotCellZ = neighborZ;
        return true;
    }
    return false;
}

void cSC4MySimDispatch::getLocationCellRect(const cLocation& location, int outRect[4]) {
    IMySimDispatchHost* host = Host();
    if (!host) {
        outRect[0] = outRect[2] = location.cellX;
        outRect[1] = outRect[3] = location.cellZ;
        return;
    }
    int rect[4] = {location.cellX, location.cellZ, location.cellX, location.cellZ};
    const bool found = location.occupant ? host->LotCellRectForOccupant(location.occupant, rect)
                                         : host->LotCellRect(location.cellX, location.cellZ, rect);
    if (!found) {
        rect[0] = rect[2] = location.cellX;
        rect[1] = rect[3] = location.cellZ;
    }
    for (int i = 0; i < 4; ++i) outRect[i] = rect[i];
}


// ---------------------------------------------------------------------------------------------------------------
// Path segmentation (0x1006afa98, 0x1006b29a6, 0x1006b2a4a, 0x1006b25f4, 0x1006b2668, 0x1006b2442)
// ---------------------------------------------------------------------------------------------------------------
// 0x1006afa98 setNextPathLeg: advance pathSegmentStart_ to the old end and walk pathSegmentEnd_ through the route until
// the travel type changes. An empty run is retried from the next element; the walk type (0xfe) always ends the search.
void cSC4MySimDispatch::setNextPathLeg() {
    int index = pathSegmentEnd_;
    const int size = static_cast<int>(route_.size());
    for (;;) {
        pathSegmentStart_ = index;
        if (index < 0 || index >= size) return;
        const std::uint8_t travelType = route_[std::size_t(index)].travelType;
        int probe = index;   // the original's lVar5: compared before it advances
        do {
            ++index;
            if (travelType != route_[std::size_t(probe)].travelType) break;
            ++probe;
            pathSegmentEnd_ = index;
        } while (probe < size);
        if (snapCurrentPathSegmentToNearestNetworkTile()) return;
        if (travelType == 0xfe) return;
        if (pathSegmentStart_ != pathSegmentEnd_) return;
        if (size <= pathSegmentStart_) return;
    }
}

// 0x1006b29a6 getNextSwitchPathSegment: copy route[pathSegmentStart_..pathSegmentEnd_) into `out` (or append to it).
bool cSC4MySimDispatch::getNextSwitchPathSegment(std::deque<RouteData>& out, bool append) {
    if (!append) out.clear();
    if (pathSegmentStart_ < 0 || pathSegmentEnd_ > static_cast<int>(route_.size())) return false;
    bool any = false;
    for (int index = pathSegmentStart_; index < pathSegmentEnd_; ++index) {
        out.push_back(route_[std::size_t(index)]);
        any = true;
    }
    return any;
}

// 0x1006b2a4a snapCurrentPathSegmentToNearestNetworkTile: move pathSegmentStart_ forward until the route cell has a
// network tile of any travel type (the 0x849 mask).
bool cSC4MySimDispatch::snapCurrentPathSegmentToNearestNetworkTile() {
    IMySimDispatchHost* host = Host();
    if (!host) return false;
    int index = pathSegmentStart_;
    while (index < pathSegmentEnd_) {
        const RouteData& data = route_[std::size_t(index)];
        if (host->TileHasNetwork(data.x, data.z, 0x1fcf)) return true;
        ++index;
        pathSegmentStart_ = index;
    }
    return false;
}

// 0x1006b25f4 jumpToNextSwitch: skip to the next leg and report whether it is not the walking leg.
bool cSC4MySimDispatch::jumpToNextSwitch() {
    if (pathSegmentStart_ >= pathSegmentEnd_) return false;
    setNextPathLeg();
    if (pathSegmentStart_ >= pathSegmentEnd_) return false;
    if (pathSegmentStart_ < 0 || pathSegmentStart_ >= static_cast<int>(route_.size())) return false;
    return route_[std::size_t(pathSegmentStart_)].travelType != 0xfe;
}

// 0x1006b2668 setStuck: mark the sim stuck (and the trip impossible) and tell the owner the pylon was not reached.
void cSC4MySimDispatch::setStuck() {
    const std::uint32_t savedFlags = flags_;
    bool apply = false;
    if ((savedFlags & 0x12) == 0) {
        apply = (savedFlags & kMySimDispatchFlagPlayerDrive) != 0 || travelType_ > 1;
    } else {
        apply = (savedFlags & kMySimDispatchFlagPlayerDrive) == 0;
    }
    if (apply) setFlags(0x2401, true);
    setFlags(0x440, true);
    notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
}

// 0x1006b2442 distance_between_locations: the smallest distance from one of the second location's cell-rectangle
// corners to the first location's rectangle (floats, truncated to int like the original).
int cSC4MySimDispatch::distance_between_locations(const cLocation& a, const cLocation& b) {
    int rectA[4], rectB[4];
    getLocationCellRect(a, rectA);
    getLocationCellRect(b, rectB);
    float boxMin[4] = {static_cast<float>(rectA[0]), static_cast<float>(rectA[1]),
                       static_cast<float>(rectA[2]), static_cast<float>(rectA[3])};
    const float corners[4][2] = {{static_cast<float>(rectB[0]), static_cast<float>(rectB[1])},
                                 {static_cast<float>(rectB[2]), static_cast<float>(rectB[1])},
                                 {static_cast<float>(rectB[0]), static_cast<float>(rectB[3])},
                                 {static_cast<float>(rectB[2]), static_cast<float>(rectB[3])}};
    float best = SC4GeometryUtilities::PointDistanceToRectangle(corners[0], boxMin, boxMin + 2);
    for (int i = 1; i < 4; ++i) {
        const float distance = SC4GeometryUtilities::PointDistanceToRectangle(corners[i], boxMin, boxMin + 2);
        if (distance < best) best = distance;
    }
    return static_cast<int>(best);
}

// 0x1006b1d76 snapToNearestRoadTile: if the location's cell has no road tile, pick the tile in the lot rectangle that
// matches the current path cell (or the first one) and move the location onto it.
bool cSC4MySimDispatch::snapToNearestRoadTile(cLocation& location) {
    IMySimDispatchHost* host = Host();
    if (!host) return false;
    if (host->TileHasNetwork(location.cellX, location.cellZ, 0x849)) return true;

    int rect[4];
    getLocationCellRect(location, rect);
    std::deque<std::pair<int, int>> tiles;
    if (!host->NetworkTilesInRect(rect, tiles) || tiles.empty()) return false;

    std::pair<int, int> chosen = tiles.front();
    if (!route_.empty() && pathSegmentStart_ < pathSegmentEnd_ &&
        pathSegmentStart_ < static_cast<int>(route_.size())) {
        const RouteData& data = route_[std::size_t(pathSegmentStart_)];
        for (const auto& tile : tiles) {
            if (tile.first == data.x && tile.second == data.z) {
                chosen = tile;
                break;
            }
        }
    }
    location.SetCellPosition(chosen.first, chosen.second);
    return true;
}

// 0x1006b2aea getAutoGroupAndModelType: travel type -> automata group + model type. Walking (0xfe, and 0 which shares
// its branch) picks a pedestrian group from the sim's sex unless a walking model key was set; a set model key selects
// the "ride a transit vehicle" group.
void cSC4MySimDispatch::getAutoGroupAndModelType(int travelType, int* automataGroup, int* modelType) {
    int group = 0;
    int model = -1;
    if (travelType == 0 || travelType == 0xfe) {
        model = ((travelType == 0xfe) | (flags_ & 1)) + 1;
        auto it = modelKeys_.find(model);
        bool haveModel = false;
        if (it != modelKeys_.end()) {
            const cGZPersistResourceKey& key = it->second.first;
            haveModel = key.type != 0 || key.group != 0 || key.instance != 0;
        }
        group = static_cast<int>(haveModel ? kAutomataGroupTransit
                                           : (owner_ && owner_->IsMale() ? kAutomataGroupFootMale : kAutomataGroupFootFemale));
    } else {
        switch (travelType) {
            case 1: group = 0x420b; model = 0; break;
            case 2: group = 0x4200; break;
            case 3: group = 0x4215; break;
            case 4:
            case 5: break;
            case 6: group = 0x4217; break;
            case 7: group = 0x422c; break;
            case 8: group = 0x422e; break;
            default: group = 0; break;
        }
    }
    if (modelType) *modelType = model;
    if (automataGroup) *automataGroup = group;
}
// ---------------------------------------------------------------------------------------------------------------
// Automaton creation / dispatch (0x1006b2c08, 0x1006b1fa2, 0x1006b256a)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::createAutomaton(int travelType, IMySimDispatchAutomaton** automaton, int* modelType) {
    int group = 0;
    int model = kModelNone;
    getAutoGroupAndModelType(travelType, &group, &model);
    if (modelType) *modelType = model;

    cGZPersistResourceKey key;
    int offset = -1;
    getModelKey(model, key, offset);        // GetModelBaseResKey / GetModelOffset of the model type (i386 0x00480624)

    float position[2] = {0.0f, 0.0f};
    if (MySimLocationIsValid(currentLocation_)) {
        position[0] = currentLocation_.x;
        position[1] = currentLocation_.z;
    }
    IMySimDispatchAutomataManager* manager = pickAutomataManager(group);
    if (!manager) return false;
    IMySimDispatchAutomaton* created =
        manager->CreateAutomaton(group, position, kAutomataCreateFlags, key, this, 0xff, 0);
    if (!created) return false;
    *automaton = created;   // one reference for the caller, like the original's two AddRefs and single Release

    cISC4Occupant* occupant = created->AsOccupant();
    if (occupant) {
        if (cISCPropertyHolder* holder = occupant->AsPropertyHolder()) {
            cSCPropertyHelp::SetPropertyValue(holder, 0x1bc42c49, true);   // "is a my-sim automaton"
        }
    }
    return true;
}

// 0x1006b1fa2 basicDispatch: create (or re-use) the automaton that rides `path`, hand it the route and place the sim on
// it. Returns the travel type of the path, or 0xff when no automaton could be created.
std::uint8_t cSC4MySimDispatch::basicDispatch(std::deque<RouteData>& path, IMySimDispatchAutomaton* automatonArg,
                                              bool noRoute) {
    if (path.empty()) return 0xff;
    IMySimDispatchAutomaton* automaton = automatonArg;
    if (automaton) automaton->AddRef();

    const int travelType = path.front().travelType;
    int group = 0;
    int modelType = kModelNone;
    getAutoGroupAndModelType(travelType, &group, &modelType);

    bool created = false;
    if (!automaton) {
        created = createAutomaton(travelType, &automaton, &modelType);
    } else {
        cISC4Occupant* occupant = automaton->AsOccupant();
        const bool matches = occupant && occupant->IsOccupantGroup(static_cast<std::uint32_t>(group));
        if (!matches || automaton->CheckAutomataFlag(0x1)) {   // kAutomataFlagMarkedForRemove
            automaton->Release();
            automaton = nullptr;
            created = createAutomaton(travelType, &automaton, &modelType);
        } else {
            created = true;
        }
    }
    if (!created || !automaton) {
        if (automaton) automaton->Release();
        return 0xff;
    }

    automaton->SetAutomataFlag(0x10000, true);
    automaton->SetAutomataFlag(0x40, true);
    automaton->SetAutomataFlag(0x200000, true);
    automaton->SetAutomataFlag(0x1000000, true);
    automaton->SetAutomataFlag(0x200, false);
    automaton->SetAutomataFlag(0x2000, false);
    automaton->SetAutomataFlag(0x100, noRoute);
    automaton->SetAutomataFlag(0x40, !noRoute);
    automaton->SetAutomataFlag(0x20, !noRoute);
    automaton->SetPriority(0xff);

    cGZPersistResourceKey modelKey;
    int offset = -1;
    getModelKey(modelType, modelKey, offset);

    bool success = true;
    IMySimDispatchRoute* route = automaton->RouteInterface();
    if (!route) {
        success = false;
    } else if (noRoute) {
        // SetState(7, no controller, -1, false, the model offset (0 when none), no sound, no behavior) (i386 0x004807aa)
        automaton->SetState(7, nullptr, -1.0f, false, offset < 0 ? 0u : static_cast<std::uint32_t>(offset), 0, nullptr);
    } else {
        const bool routed = route->SetRouteInfo(path, true, false);
        if (offset >= 0) automaton->PlayAnim(static_cast<std::uint32_t>(offset));
        automaton->SetState(0, nullptr, -1.0f, false, 0, 0, nullptr);
        if (!routed) success = false;
    }
    if (route) route->Release();
    if (!success) {
        automaton->RemoveCallback(this);
        automaton->Remove(false);
        automaton->Release();
        return 0xff;
    }

    setAutomataOccupant(automaton->AsOccupant());
    defaultModelType_ = modelType;
    float position[3] = {0.0f, 0.0f, 0.0f};
    if (automataOccupant_) automataOccupant_->GetPosition(position);
    currentLocation_.x = position[0];
    currentLocation_.z = position[2];
    currentLocation_.cellX = cellFromPosition(position[0]);
    currentLocation_.cellZ = cellFromPosition(position[2]);
    automaton->Release();
    return static_cast<std::uint8_t>(travelType);
}

// 0x1006b256a basicDispatchOnNewPathLeg: dispatch only when the current leg holds more than one route element.
std::uint8_t cSC4MySimDispatch::basicDispatchOnNewPathLeg(IMySimDispatchAutomaton* automaton, bool flag) {
    std::deque<RouteData> segment;
    const bool haveSegment = getNextSwitchPathSegment(segment, false);
    std::uint8_t result = 0xff;
    if (haveSegment && segment.size() > 1) result = basicDispatch(segment, automaton, flag);
    return result;
}

// ---------------------------------------------------------------------------------------------------------------
// updateNeighborLocation (0x1006b06ec) — PARTIAL: the recovered body is a Ghidra stub that always returns 0; only the
// neighbour-connection lookup is visible (spSC4 0x118 -> 0x58 -> spNeighborsSim 0x60). The connection -> cell mapping
// the decompiler dropped at 0x1006b0740 is still missing, so the function keeps the original's "no neighbour location"
// result.
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::updateNeighborLocation(int attempt, cLocation& location) {
    (void)attempt;
    IMySimDispatchHost* host = Host();
    if (!host) return false;
    if (location.hasOccupant) return false;
    int cellX = 0, cellZ = 0;
    if (!host->NeighborConnection(location.lotCellX, location.lotCellZ, cellX, cellZ)) return false;
    return false;   // see PARTIAL above: the original's success path was dropped by the decompiler
}

// ---------------------------------------------------------------------------------------------------------------
// updateState (0x1006aeb5a) — the state machine.
// ---------------------------------------------------------------------------------------------------------------
std::uint32_t cSC4MySimDispatch::updateState() {
    if (state_ == kMySimTripIdle) {   // 3: waiting at the current location
        currentDestination_ = cLocation();
        if (currentLocation_.occupant) setAutomataOccupant(nullptr);

        if (time_ <= currentLocation_.data28 && MySimLocationIsValid(currentLocation_) && getOccupantToTrack()) {
            return updateTrackingOccupant();
        }
        if (!MySimLocationIsValid(currentLocation_)) {
            std::uint32_t event = 0;
            if (!currentLocation_.commuteLocation ||
                (!MySimLocationIsValid(currentLocation_) && !validateCommuteLocation(currentLocation_))) {
                if (MySimLocationIsValid(defaultDestination_)) {
                    currentLocation_ = defaultDestination_;
                    event = kDispatchNotifyPylon;
                }
            } else {
                commuteDestination_ = currentLocation_;
                event = kDispatchNotifyArrived;
            }
            if (event) notifyMySim(event, 0, 0, 0);
        }

        setFlags(kMySimDispatchFlagCellOnly, !currentLocation_.hasOccupant);

        while (!destinationQueue_.empty() && !MySimLocationIsValid(destinationQueue_.front())) {
            destinationQueue_.pop_front();
        }
        cLocation next;
        if (destinationQueue_.empty()) {
            if ((flags_ & kMySimDispatchFlagHasDefault) != 0 && !(currentLocation_ == defaultDestination_) &&
                MySimLocationIsValid(defaultDestination_)) {
                next = defaultDestination_;
            }
        } else {
            next = destinationQueue_.front();
            destinationQueue_.pop_front();
        }
        neighborAttempts_ = 0;

        if (next.commuteLocation && !MySimLocationIsValid(next)) {
            if (validateCommuteLocation(next)) {
                commuteDestination_ = next;
                notifyMySim(kDispatchNotifyArrived, 0, 0, 0);
            }
        }
        if (!MySimLocationIsValid(next)) {
            if ((flags_ & kMySimDispatchFlagHasDefault) == 0 || !(currentLocation_ == defaultDestination_)) {
                setState(kMySimTripNone);
            }
        } else if (!(currentLocation_ == next)) {
            currentDestination_ = next;
            currentDestination_.data28 = time_ + currentDestination_.data24;
            setState(kMySimTripTravelling);
            setFlags(0x5800, false);
        }
        return updateTrackingOccupant();
    }

    if (state_ == kMySimTripTravelling) {   // 2: following the route
        const std::uint32_t savedFlags = flags_;
        if ((savedFlags & kMySimDispatchFlagTravelling) != 0) {
            if ((savedFlags & kMySimDispatchFlagUDriveIt) != 0) return updateTrackingOccupant();
            if ((savedFlags & kMySimDispatchFlagStuck) != 0) {
                setState(kMySimTripNone);
            } else if ((savedFlags & kMySimDispatchFlagSwitchTravel) != 0) {
                switchTravel();
            } else if ((savedFlags & kMySimDispatchFlagPathLeg) == 0) {
                const std::uint8_t travelType = basicDispatchOnNewPathLeg(nullptr, false);
                if (travelType == 0xff) {
                    setStuck();
                } else {
                    setFlags(kMySimDispatchFlagPathLeg, true);
                    setFlags(kMySimDispatchFlagSwitchTravel, false);
                    if (travelType_ != travelType) ++changeCounter_;
                    travelType_ = travelType;
                }
            }
            return updateTrackingOccupant();
        }

        const bool resumedTravel = (savedFlags & kMySimDispatchFlagNeedPath) != 0 && automataOccupant_ &&
                                   (travelType_ | 1) != 0xff;
        resetPath();
        if (!pathFinder_) return updateTrackingOccupant();
        pathFinder_->SetRouteInfo(&route_);   // 0x40
        pathFinder_->SetFlag(1);              // 0x28
        pathFinder_->SetFlag(0x10);
        pathFinder_->SetTrafficDensity(1);    // 0x90
        if ((savedFlags & kMySimDispatchFlagNeedPath) == 0 || (travelType_ & ~1) == kTravelTypeWalk) {
            const bool startAtBuilding = !(flags_ & kMySimDispatchFlagUDriveItPending);
            pathFinder_->SetTravelTypeFlag(2, true);
            pathFinder_->SetTravelTypeFlag(1, startAtBuilding);
            const bool cellOnly = !currentLocation_.hasOccupant && startAtBuilding;
            pathFinder_->SetTravelTypeFlag(4, cellOnly);
            pathFinder_->SetTravelTypeFlag(8, cellOnly);
            pathFinder_->SetTravelTypeFlag(0x40, cellOnly);
            pathFinder_->SetTravelTypeFlag(0x80, cellOnly);
            pathFinder_->SetTravelTypeFlag(0x100, cellOnly);
        } else {
            pathFinder_->SetTravelTypeFlag(2, travelType_ == 1);
            pathFinder_->SetTravelTypeFlag(1, travelType_ == 0);
            pathFinder_->SetTravelTypeFlag(4, travelType_ == 2);
            pathFinder_->SetTravelTypeFlag(8, travelType_ == 3);
            pathFinder_->SetTravelTypeFlag(0x40, travelType_ == 6);
            pathFinder_->SetTravelTypeFlag(0x80, travelType_ == 7);
            pathFinder_->SetTravelTypeFlag(0x100, travelType_ == 8);
        }
        pathFinder_->SetTripDestinationFilter(3);   // 0x48
        // a trip to the commute location makes it the commute destination (i386 0x004827ec: commute = destination), so
        // the destination keeps its dwell time
        if (currentDestination_.commuteLocation) commuteDestination_ = currentDestination_;
        int startRect[4], endRect[4];
        getLocationCellRect(currentLocation_, startRect);
        getLocationCellRect(currentDestination_, endRect);
        pathFinder_->SetStartRect(SC4RectInt32{startRect[0], startRect[1], startRect[2], startRect[3]});   // 0x98
        pathFinder_->SetEndRect(SC4RectInt32{endRect[0], endRect[1], endRect[2], endRect[3]});             // 0xa0

        const int result = Host() ? Host()->FindPath(pathFinder_) : 0;
        if (result == 1) {
            setFlags(0x820, false);
            setFlags(kMySimDispatchFlagTravelling, true);
            bool switchFlag = true;
            if (resumedTravel) switchFlag = (flags_ & kMySimDispatchFlagPlayerDrive) != 0;
            setFlags(kMySimDispatchFlagSwitchTravel, switchFlag);
            pathSegmentStart_ = 0;
            pathSegmentEnd_ = 0;
            if (!route_.empty()) {
                const RouteData tail = route_.back();
                if (startRect[0] == startRect[2] && startRect[1] == startRect[3]) {
                    RouteData corner{static_cast<std::uint8_t>(startRect[0] & 0xff),
                                     static_cast<std::uint8_t>(startRect[1] & 0xff), tail.travelType};
                    route_.push_back(corner);
                }
                RouteData walk{tail.x, tail.z, 0xfe};
                route_.push_back(walk);
            }
            setNextPathLeg();
            if (!resumedTravel) notifyMySim(kDispatchNotifyNeighbor, 0, 0, 0);
            return updateTrackingOccupant();
        }
        if (result == 2) return updateTrackingOccupant();   // still searching

        if (!currentDestination_.hasOccupant) {
            if (updateNeighborLocation(neighborAttempts_ + 1, currentDestination_)) {
                int rect[4];
                getLocationCellRect(currentDestination_, rect);
                pathFinder_->SetEndRect(SC4RectInt32{rect[0], rect[1], rect[2], rect[3]});
                return updateTrackingOccupant();
            }
        } else if (!currentLocation_.hasOccupant) {
            if (updateNeighborLocation(neighborAttempts_ + 1, currentLocation_)) {
                int rect[4];
                getLocationCellRect(currentLocation_, rect);
                pathFinder_->SetStartRect(SC4RectInt32{rect[0], rect[1], rect[2], rect[3]});
                return updateTrackingOccupant();
            }
        }
        setState(kMySimTripNone);
        notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
        return updateTrackingOccupant();
    }

    if (state_ == kMySimTripNone) {   // -1: no trip; fall back to the default destination once
        setFlags(0xfe4d, false);
        if ((flags_ & kMySimDispatchFlagHasDefault) != 0) {
            if (!(currentLocation_ == defaultDestination_)) {
                setFlags(0x32, false);
                if ((flags_ & kMySimDispatchFlagHasDefault) != 0 && !(currentLocation_ == defaultDestination_)) {
                    currentLocation_ = defaultDestination_;
                    if (travelType_ != kTravelTypeWalk) ++changeCounter_;
                    travelType_ = kTravelTypeWalk;
                    ++changeCounter_;
                    currentLocation_.data28 = currentLocation_.data24 + time_;
                    notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
                    if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
                        automaton->RemoveCallback(this);
                        automaton->Remove(false);
                        automaton->Release();
                    }
                    automataOccupant_ = nullptr;
                    setState(kMySimTripIdle);
                    return updateTrackingOccupant();
                }
            } else {
                setState(kMySimTripIdle);
            }
        }
        if (!destinationQueue_.empty()) setState(kMySimTripIdle);
    }
    return updateTrackingOccupant();
}

// ---------------------------------------------------------------------------------------------------------------
// switchTravel (0x1006b0b16, i386 0x00480c7c): runs when the sim reached the end of a path leg (flag 0x400). It starts
// from the current state and travel type and picks one of four branches by the flag word:
//  * player drive (0x1): the U-Drive-It hand-off (re-dispatch on foot, or arrive when the drive ended);
//  * stuck (0x40): the trip ends (state -1);
//  * at a transit stop (0x10): find the stop occupant near the route cell, wait there, then board the transit automaton
//    that stops on the cell (or walk on when the wait ran out);
//  * a cell-only destination (0x4): wait until the dwell time ran out, then dispatch the next leg;
//  * a leg ended (0x20): the walking leg reached the destination (arrival: state 3), the leg continues with the same
//    travel type, or the sim gets off at a transit switch;
//  * otherwise: dispatch the next leg (or jump to the next switch).
// The flags are applied at the end in the original's order: setStuck, 0x2401, clear mask, set mask.
// ---------------------------------------------------------------------------------------------------------------
namespace {
// The transit-stop occupant group (and the automaton group riding it) for the travel type of the next leg
// (i386 0x00481358 switch); travel types without an entry leave the automaton group unchanged and search group 0.
std::uint32_t TransitStopGroupFor(std::uint8_t travelType, std::uint32_t& automataGroup) {
    const unsigned index = static_cast<unsigned>(travelType) - 2u;
    if (index >= 7u || ((kTransitMask >> index) & 1u) == 0) return 0;
    automataGroup = kTransitGroupTable[index];
    return kTransitStopTable[index];
}
}  // namespace

void cSC4MySimDispatch::switchTravel() {
    IMySimDispatchHost* host = Host();
    const std::uint32_t flags = flags_;
    std::uint32_t travelType = static_cast<std::uint32_t>(travelType_);
    int newState = state_;
    std::uint32_t setMask = 0;      // flags set at the end
    std::uint32_t clearMask = 0;    // flags cleared at the end
    std::uint32_t stuckMask = 0;    // 0x40 -> setStuck, 0x1 -> setFlags(0x2401)

    auto routeAt = [this](int index) -> RouteData {
        return index >= 0 && index < static_cast<int>(route_.size()) ? route_[std::size_t(index)] : RouteData{};
    };
    // The route cell of the next switch becomes the current location (after jumpToNextSwitch).
    auto moveToSwitchCell = [this, &routeAt](std::uint32_t& outTravelType) {
        const RouteData data = routeAt(pathSegmentStart_ - 1);
        outTravelType = data.travelType;
        currentLocation_.cellX = data.x;
        currentLocation_.cellZ = data.z;
        currentLocation_.x = static_cast<float>(data.x << 4);
        currentLocation_.z = static_cast<float>(data.z << 4);
    };

    if ((flags & kMySimDispatchFlagPlayerDrive) != 0) {
        // --- U-Drive-It (i386 0x00480cb5) ---
        IMySimDispatchAutomaton* automaton = acquireAutomaton();
        const int automatonState = automaton ? automaton->GetStateId() : -1;
        bool teleported = (flags & kMySimDispatchFlagTeleported) != 0;
        int animOffset = -1;
        if (teleported) {
            notifyMySim(kDispatchNotifyIdleOffset, 0, 0, 0);
            cGZPersistResourceKey key;
            getModelKey(defaultModelType_, key, animOffset);   // GetModelOffset(defaultModelType_)
        }
        if (!automaton || defaultModelType_ != kModelWalk || automatonState != 7 ||
            (travelType_ != 0 && travelType_ != kTravelTypeWalk)) {
            cLocation location = currentLocation_;
            const RouteData data = routeAt(pathSegmentStart_);
            if (data.travelType != 0xfe) {
                location.cellX = data.x;
                location.x = static_cast<float>(data.x << 4);
                location.z = static_cast<float>(data.z << 4);
                location.cellZ = data.z;
            }
            if (!snapToNearestRoadTile(currentLocation_) && !snapToNearestRoadTile(location)) {
                notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
                newState = kMySimTripNone;
                teleported = false;
            } else {
                currentLocation_ = location;
                std::deque<RouteData> path;
                path.push_back(RouteData{data.x, data.z, 0});
                notifyMySim(kDispatchNotifyIdleOffset, 0, 0, 0);
                const std::uint8_t result = basicDispatch(path, nullptr, true);
                if (result == 0xff) {
                    notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
                    newState = kMySimTripNone;
                    travelType = 0xff;
                } else if (travelType_ != kTravelTypeWalk) {
                    travelType = result;
                }
            }
        }
        if (!teleported) {
            clearMask = 0x2002;
        } else {
            if (automaton) automaton->Release();
            automaton = acquireAutomaton();
            if (currentLocation_.data28 < time_ || !automaton) {
                setFlags(kMySimDispatchFlagPlayerDrive, false);
                const int limit = destinationProximityMin_ * destinationProximityMin_;
                const bool near = distance_between_locations(currentDestination_, currentLocation_) < limit;
                bool arrive = false;
                if (travelType_ != kTravelTypeWalk || (flags_ & kMySimDispatchFlagCommute) == 0) {
                    const std::uint8_t result = basicDispatchOnNewPathLeg(nullptr, false);
                    travelType = result;
                    if (result != 0xff) {
                        clearMask = 0x2437;
                        setMask = 0x200;
                    } else if (jumpToNextSwitch()) {
                        moveToSwitchCell(travelType);
                        clearMask = 0x2002;
                        setMask = 0x220;
                    } else if (!near) {
                        notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
                        clearMask = 0x2002;
                        newState = kMySimTripNone;
                    } else {
                        if (isBuilding(currentDestination_)) currentLocation_ = currentDestination_;
                        arrive = true;
                    }
                } else {
                    if (isBuilding(currentDestination_) && near) currentLocation_ = currentDestination_;
                    arrive = true;
                }
                if (arrive) {
                    currentLocation_.data28 = currentLocation_.data24 + time_;
                    clearMask = 0xa403;
                    newState = kMySimTripIdle;
                }
            } else {
                if (animOffset >= 1) automaton->PlayAnim(static_cast<std::uint32_t>(animOffset));
                clearMask = 0x2002;
            }
        }
        if (automaton) automaton->Release();
    } else if ((flags & kMySimDispatchFlagStuck) != 0) {
        newState = kMySimTripNone;   // i386 0x00481d06
    } else if (travelType == 0xfe && (flags & kMySimDispatchFlagCellOnly) == 0 &&
               !(currentLocation_ == defaultDestination_) && !(currentLocation_ == commuteDestination_)) {
        // walking somewhere that is neither home nor work: idle there
        currentLocation_.data28 = time_ + minTimeIdling_;
        travelType = 0xfe;
        setMask = 1;
        stuckMask = 1;
    } else if ((flags & kMySimDispatchFlagAtBuilding) != 0) {
        // --- at a transit stop (i386 0x004812dc) ---
        const RouteData next = routeAt(pathSegmentStart_);
        if (!currentLocation_.occupant && host) {
            const std::uint32_t stopGroup = TransitStopGroupFor(next.travelType, transitGroup_);
            const int minX = std::min<int>(currentLocation_.cellX, next.x), maxX = std::max<int>(currentLocation_.cellX, next.x);
            const int minZ = std::min<int>(currentLocation_.cellZ, next.z), maxZ = std::max<int>(currentLocation_.cellZ, next.z);
            const int rect[4] = {minX - 1, minZ - 1, maxX + 1, maxZ + 1};
            std::deque<cISC4Occupant*> occupants;
            host->OccupantsInCellRect(rect, occupants);
            for (cISC4Occupant* occupant : occupants) {
                if (!occupant || !occupant->IsOccupantGroup(stopGroup)) continue;
                currentLocation_.occupant = occupant;
                float position[3] = {0.0f, 0.0f, 0.0f};
                occupant->GetPosition(position);
                currentLocation_.x = position[0];
                currentLocation_.z = position[2];
                currentLocation_.cellX = cellFromPosition(position[0]);
                currentLocation_.cellZ = cellFromPosition(position[2]);
                currentLocation_.data28 = minTimeAtSwitch_ + time_;
                transitSwitchStartTime_ = time_;
                notifyMySim(kDispatchNotifyTransitStop, static_cast<int>(stopGroup), 0, 0);
                setAutomataOccupant(nullptr);
                break;
            }
            host->ClearOccupantList(occupants);
        }
        if (!currentLocation_.occupant) {
            setMask = 0x40;
        } else if (currentLocation_.data28 < time_) {
            // the wait ran out: go on with the next leg without the transit automaton
            const std::uint8_t result = basicDispatchOnNewPathLeg(nullptr, false);
            travelType = result;
            if (result == 0xff) {
                setMask = 0x40;
            } else {
                clearMask = 0x436;
                setMask = 0x200;
            }
        } else if (host) {
            // board the transit automaton of the stop's group standing on the sim's cell
            const int rect[4] = {currentLocation_.cellX, currentLocation_.cellZ, currentLocation_.cellX, currentLocation_.cellZ};
            std::deque<cISC4Occupant*> occupants;
            host->OccupantsInCellRect(rect, occupants);
            IMySimDispatchAutomaton* transit = nullptr;
            for (cISC4Occupant* occupant : occupants) {
                if (occupant && occupant->IsOccupantGroup(transitGroup_)) transit = host->AutomatonOf(occupant);
                if (transit) break;
            }
            if (transit) {
                transit->AddCallback(this);
                transit->SetAutomataFlag(0x1000000, true);
                transit->SetAutomataFlag(0x10000, true);
                transit->SetPriority(0xff);
                const std::uint8_t result = basicDispatchOnNewPathLeg(transit, false);
                travelType = result;
                if (result == 0xff) {
                    setMask = 0x40;
                } else {
                    clearMask = 0x436;
                    setMask = 0x200;
                }
            }
            host->ClearOccupantList(occupants);
            if (transit) transit->Release();
        }
        stuckMask = setMask;
    } else if ((flags & kMySimDispatchFlagCellOnly) != 0) {
        // --- a cell-only location: leave once the dwell time ran out (i386 0x004817df) ---
        if (time_ > currentLocation_.data28) {
            const std::uint8_t result = basicDispatchOnNewPathLeg(nullptr, false);
            travelType = result;
            if (result == 0xff) {
                setMask = 0x40;
                stuckMask = 0x40;
            } else {
                clearMask = 0x436;
                setMask = 0x200;
            }
        }
    } else if ((flags & kMySimDispatchFlagCommute) != 0) {
        // --- the leg ended (i386 0x0048185c) ---
        const std::uint8_t legType = routeAt(pathSegmentStart_).travelType;
        if (legType == 0xfe) {
            // the walking leg: the sim reached the destination's lot
            std::uint32_t arrival = 2;
            if (!currentLocation_.hasOccupant || !currentDestination_.hasOccupant) {
                currentLocation_.occupant = nullptr;
                setAutomataOccupant(nullptr);
                arrival = 6;
            }
            const int limit = destinationProximityMin_ * destinationProximityMin_;
            std::uint32_t event = 0;
            bool arrived = false;
            if (limit < distance_between_locations(currentDestination_, currentLocation_)) {
                // still too far from the destination: idle where the walk ended
                currentLocation_.data28 = minTimeIdling_ + time_;
                event = kDispatchNotifyFarFromDestination;
            } else if ((flags_ & kMySimDispatchFlagCellOnly) != 0 || (arrival & 4) != 0 ||
                       (flags_ & kMySimDispatchFlagPlayerDrive) != 0 || currentDestination_ == defaultDestination_ ||
                       currentDestination_ == commuteDestination_) {
                currentLocation_.data24 += time_;
                if (isBuilding(currentDestination_)) currentLocation_ = currentDestination_;
                notifyMySim(kDispatchNotifyLocation, 0, 0, 0);
                travelType = 0xfe;
                clearMask = 0x8460;
                newState = kMySimTripIdle;
                setMask = arrival;
                arrived = true;
            } else {
                currentLocation_.data28 = minTimeIdling_ + time_;
                event = kDispatchNotifyLocation;
            }
            if (!arrived) {
                setMask = arrival | 0x421;
                notifyMySim(event, 0, 0, 0);
                travelType = 0xfe;
                clearMask = 0x460;
            }
        } else {
            const std::uint32_t current = static_cast<std::uint32_t>(travelType_);
            clearMask = 0x20;
            if (legType == current) {
                // the next leg keeps the travel type: continue on the same automaton
                IMySimDispatchAutomaton* automaton = acquireAutomaton();
                const std::uint8_t result = basicDispatchOnNewPathLeg(automaton, false);
                if (result == 0xff) {
                    setStuck();
                } else {
                    setFlags(kMySimDispatchFlagPathLeg, true);
                    setFlags(kMySimDispatchFlagSwitchTravel, false);
                    if (static_cast<int>(result) != travelType_) ++changeCounter_;
                    travelType_ = result;
                }
                if (automaton) automaton->Release();
            } else if (static_cast<int>(current) < 9) {
                if (current > 5 || current < 4) {
                    // get off the vehicle: wait at the switch for a transit leg, idle before a car or walking leg
                    if (legType < 9) {
                        const std::uint32_t bit = 1u << legType;
                        if ((bit & 0x1cc) != 0) {
                            currentLocation_.occupant = nullptr;
                            currentLocation_.data28 = minTimeAtSwitch_ + time_;
                            setMask = 0x10;
                        } else if ((bit & 3) != 0) {
                            currentLocation_.occupant = nullptr;
                            currentLocation_.data28 = minTimeIdling_ + time_;
                            setMask = 1;
                        }
                    }
                    setAutomataOccupant(nullptr);
                }
            } else if (current - 0xfe < 2) {
                newState = kMySimTripNone;
            }
        }
        stuckMask = setMask;
        if ((setMask & 0x53) == 0) {
            // nothing else to do at this switch: idle here
            stuckMask = setMask | 3;
            destinationQueue_.clear();
            currentLocation_.data28 = minTimeIdling_ + time_;
            travelType = 0xfe;
            setMask = stuckMask;
        }
    } else {
        // --- dispatch the next leg (i386 0x00481c4e) ---
        const std::uint8_t result = basicDispatchOnNewPathLeg(nullptr, false);
        travelType = result;
        if (result != 0xff) {
            clearMask = 0x436;
            setMask = 0x200;
        } else if (jumpToNextSwitch()) {
            moveToSwitchCell(travelType);
            clearMask = 2;
            setMask = 0x220;
        } else {
            notifyMySim(kDispatchNotifyPylon, 0, 0, 0);
            newState = kMySimTripNone;
        }
    }

    if (static_cast<std::uint32_t>(travelType_) != travelType) ++changeCounter_;
    travelType_ = static_cast<int>(travelType);
    setState(newState);
    if ((stuckMask & kMySimDispatchFlagStuck) != 0) setStuck();
    if ((stuckMask & 1) != 0) setFlags(0x2401, true);
    setFlags(clearMask, false);
    setFlags(setMask, true);
}
// ---------------------------------------------------------------------------------------------------------------
// U-Drive-It (0x1006b2dd6, 0x1006b2e9c, 0x1006b30a6)
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::CanUDriveIt() {
    if (state_ == kMySimTripTravelling) {
        if (automataOccupant_ && travelType_ == 1) {
            if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
                automaton->Release();
                return true;
            }
        }
    }
    if (state_ == kMySimTripIdle) {
        if (currentLocation_ == defaultDestination_ || currentLocation_ == commuteDestination_) return true;
    }
    return false;
}

bool cSC4MySimDispatch::EnterUDriveIt() {
    if (!CanUDriveIt()) return false;
    if (state_ == kMySimTripIdle) {
        if (currentLocation_ == defaultDestination_) {
            setFlags(kMySimDispatchFlagUDriveItPending, true);
            GoTo(commuteDestination_);
        } else if (currentLocation_ == commuteDestination_) {
            setFlags(kMySimDispatchFlagUDriveItPending, true);
            GoTo(defaultDestination_);
        } else {
            return false;
        }
    } else if (state_ == kMySimTripTravelling) {
        if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
            IMySimDispatchHost* host = Host();
            if (host) {
                host->ShowUDriveAdvice();
                host->EnterUDriveView(automaton, this);
            }
            setFlags(kMySimDispatchFlagUDriveIt, true);
            automaton->Release();
        }
    } else {
        return false;
    }
    return true;
}

bool cSC4MySimDispatch::ExitUDriveIt() {
    if ((flags_ & kMySimDispatchFlagUDriveIt) != 0) {
        if (IMySimDispatchAutomaton* automaton = acquireAutomaton()) {
            automaton->RemoveCallback(this);
            automaton->Remove(false);
            automaton->Release();
        }
        destinationQueue_.clear();
        currentLocation_ = defaultDestination_;
        currentDestination_ = defaultDestination_;
        if (travelType_ != kTravelTypeNone) ++changeCounter_;
        travelType_ = kTravelTypeNone;
        setState(kMySimTripNone);
        resetPath();
        automataOccupant_ = nullptr;
    }
    setFlags(0x30000, false);
    return true;
}

// 0x1006b2dae GetTransitSwitchWaitTimeMunutes: minutes since the sim reached the transit switch, reset after 15 minutes
// unless it is at a transit stop.
int cSC4MySimDispatch::GetTransitSwitchWaitTimeMunutes() {
    int value = transitSwitchStartTime_;
    if ((flags_ & kMySimDispatchFlagAtBuilding) == 0) {
        if (time_ - value > 15) {
            transitSwitchStartTime_ = 0;
            value = 0;
        }
    }
    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// DoMessage (0x1006ae2d6) — the notifications registered in Init, the simulator tick and the automaton events.
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    const std::uint32_t type = message->GetType();
    IMySimDispatchHost* host = Host();

    if (type == kMySimDispatchMsgFiveMinuteTick) {
        if (pathFinder_) time_ += 5;
        return true;
    }
    if (type == kMySimDispatchMsgSimulate) {
        if (pathFinder_) updateState();
        return true;
    }
    if (type == kMySimDispatchMsgFollow) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        standard->GetIGZUnknown();   // the original queries the view and discards it
        follow_ = standard->GetData1() != 0;
        return true;
    }
    if (type == kMySimDispatchMsgView1 || type == kMySimDispatchMsgView2) {
        if (follow_) {
            bool driving = false;
            if (host) {
                if (IMySimDispatchAutomataManager* manager = host->AutomataManager(kAutomataGroupVehicleDefault)) {
                    driving = manager->PlayerDriveActive() || manager->DriveViewActive();
                }
            }
            if (!driving) {
                if (host) host->ClearVehiclePlayerDrive();
                follow_ = false;
            }
        }
        return true;
    }
    if (type == kMySimDispatchMsgReadPending) {
        if (initPending_) {
            Init();
            initPending_ = false;
            if (host) {
                if (cIGZMessageServer2* server = host->MessageServer()) server->RemoveNotification(this, type);
            }
            notifyMySim(kDispatchNotifyTripEnd, 0, static_cast<int>(flags_), 0);
            notifyMySim(kDispatchNotifyTripLeg, -2, state_, 0);
        }
        return true;
    }
    if (type == kMySimDispatchMsgExitPlayerDrive) {
        auto* standard = static_cast<cIGZMessage2Standard*>(message);
        if ((flags_ & kMySimDispatchFlagUDriveIt) != 0 && standard->GetData2() == 1) ExitUDriveIt();
        return true;
    }
    if (type != kMySimDispatchMsgAutomaton) return true;

    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t event = static_cast<std::uint32_t>(standard->GetData2());
    switch (event) {
        case kDispatchEventAnimationDone:
            if ((flags_ & kMySimDispatchFlagPlayerDrive) != 0) {
                setFlags(kMySimDispatchFlagTeleported, true);
                updateState();
            }
            break;
        case kDispatchEventRemoving: {
            if (!automataOccupant_) break;
            cISC4Occupant* sender = host ? host->CallbackOccupant(standard->GetVoid1()) : nullptr;
            if (!sender || sender != automataOccupant_.get()) break;
            if ((flags_ & kMySimDispatchFlagUDriveIt) == 0) {
                GoTo(currentDestination_);
                setFlags(0x401, true);
            } else {
                ExitUDriveIt();
            }
            break;
        }
        case kDispatchEventRouteReached: {
            if (state_ != kMySimTripTravelling || (flags_ & kMySimDispatchFlagStuck) != 0) break;
            IMySimDispatchRoute* route = AcquireRoute(automataOccupant_.get());
            if (!route) break;
            std::deque<RouteData> path;
            route->GetRouteInfo(path);
            if (path.size() < 2) {
                if (automataOccupant_) {
                    float position[3] = {0.0f, 0.0f, 0.0f};
                    automataOccupant_->GetPosition(position);
                    currentLocation_.x = position[0];
                    currentLocation_.z = position[2];
                    currentLocation_.cellX = cellFromPosition(position[0]);
                    currentLocation_.cellZ = cellFromPosition(position[2]);
                }
                setFlags(0x420, true);
                setNextPathLeg();
                updateState();
            } else {
                path.pop_front();
                route->SetRouteInfo(path, true, false);
            }
            route->Release();
            break;
        }
        case kDispatchEventReturned: {
            if (!automataOccupant_) break;
            cISC4Occupant* sender = host ? host->CallbackOccupant(standard->GetVoid1()) : nullptr;
            if (!sender || sender != automataOccupant_.get()) break;
            currentLocation_.occupant = nullptr;
            ++changeCounter_;
            if ((flags_ & kMySimDispatchFlagUDriveItPending) != 0) {
                setFlags(kMySimDispatchFlagUDriveItPending, false);
                ExitUDriveIt();
            }
            if (!automataOccupant_) break;   // ExitUDriveIt released it (the original would dereference null here)
            float position[3] = {0.0f, 0.0f, 0.0f};
            automataOccupant_->GetPosition(position);
            const int cellX = cellFromPosition(position[0]);
            const int cellZ = cellFromPosition(position[2]);
            if ((cellX != currentLocation_.cellX || cellZ != currentLocation_.cellZ) &&
                (flags_ & kMySimDispatchFlagPlayerDrive) == 0) {
                const int dx = cellX - currentDestination_.cellX;
                const int dz = cellZ - currentDestination_.cellZ;
                const int limit = destinationApproachDistance_ * destinationApproachDistance_;
                setFlags(kMySimDispatchFlagNearDestination,
                         static_cast<std::uint32_t>(dx * dx + dz * dz) < static_cast<std::uint32_t>(limit));
            }
            currentLocation_.x = position[0];
            currentLocation_.z = position[2];
            currentLocation_.cellX = cellX;
            currentLocation_.cellZ = cellZ;
            break;
        }
        default:
            break;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Persistence (0x1006ad8aa, 0x1006adcee). The record is a version word, the scalar fields +0x24..+0x68, the four
// locations, the tracked occupant, the label, the automaton occupant, the owner, the clock, the route deque, the model
// key map, the default model type and the destination queue.
// ---------------------------------------------------------------------------------------------------------------
bool cSC4MySimDispatch::Write(cIGZOStream& stream) const {
    IMySimDispatchHost* host = Host();
    stream.SetUint32(1);
    stream << follow_;
    const std::uint32_t scalars[18] = {
        static_cast<std::uint32_t>(destinationApproachDistance_), static_cast<std::uint32_t>(destinationProximityMin_),
        static_cast<std::uint32_t>(minTimeIdling_), static_cast<std::uint32_t>(minTimeAtLocation_),
        static_cast<std::uint32_t>(minTimeAtSwitch_), transitGroup_, static_cast<std::uint32_t>(pathSegmentEnd_),
        static_cast<std::uint32_t>(pathSegmentStart_), static_cast<std::uint32_t>(changeCounter_),
        static_cast<std::uint32_t>(trackedChangeCounter_), static_cast<std::uint32_t>(trackHandle_),
        static_cast<std::uint32_t>(state_), static_cast<std::uint32_t>(previousState_), flags_,
        static_cast<std::uint32_t>(time_), static_cast<std::uint32_t>(travelType_),
        static_cast<std::uint32_t>(transitSwitchStartTime_), static_cast<std::uint32_t>(neighborAttempts_)};
    for (std::uint32_t value : scalars) stream.SetUint32(value);

    MySimLocationWrite(currentLocation_, stream);
    MySimLocationWrite(defaultDestination_, stream);
    MySimLocationWrite(currentDestination_, stream);
    MySimLocationWrite(commuteDestination_, stream);

    if (host) host->WriteSerializable(stream, trackedOccupant_.get());
    stream.SetUint32(trackingBitmapId_);
    stream.SetGZStr(trackingText_);
    if (host) host->WriteSerializable(stream, automataOccupant_.get());
    // the original writes the owner's +0x148 serializable view (iid 0x8bc01ffe); the game adapter maps the owner seam
    if (host) host->WriteSerializable(stream, owner_);
    if (host) host->WriteSerializable(stream, clock_);

    stream.SetUint32(static_cast<std::uint32_t>(route_.size()));
    for (const RouteData& data : route_) {
        stream.SetUint8(data.x);
        stream.SetUint8(data.z);
        stream.SetUint8(data.travelType);
    }

    stream.SetUint32(static_cast<std::uint32_t>(modelKeys_.size()));
    for (const auto& entry : modelKeys_) {
        stream.SetUint32(static_cast<std::uint32_t>(entry.first));
        stream << entry.second.first;
        stream.SetUint32(static_cast<std::uint32_t>(entry.second.second));
    }

    stream.SetUint32(static_cast<std::uint32_t>(defaultModelType_));
    stream.SetUint32(static_cast<std::uint32_t>(destinationQueue_.size()));
    for (const cLocation& location : destinationQueue_) MySimLocationWrite(location, stream);
    return !stream.GetError();
}

bool cSC4MySimDispatch::Read(cIGZIStream& stream) {
    IMySimDispatchHost* host = Host();
    std::uint32_t version = 0;
    stream.GetUint32(version);
    stream >> follow_;

    std::uint32_t value = 0;
    std::uint32_t scalars[18] = {};
    for (std::uint32_t& scalar : scalars) stream.GetUint32(scalar);
    destinationApproachDistance_ = static_cast<int>(scalars[0]);
    destinationProximityMin_ = static_cast<int>(scalars[1]);
    minTimeIdling_ = static_cast<int>(scalars[2]);
    minTimeAtLocation_ = static_cast<int>(scalars[3]);
    minTimeAtSwitch_ = static_cast<int>(scalars[4]);
    transitGroup_ = scalars[5];
    pathSegmentEnd_ = static_cast<int>(scalars[6]);
    pathSegmentStart_ = static_cast<int>(scalars[7]);
    changeCounter_ = static_cast<int>(scalars[8]);
    trackedChangeCounter_ = static_cast<int>(scalars[9]);
    trackHandle_ = static_cast<int>(scalars[10]);
    state_ = static_cast<int>(scalars[11]);
    previousState_ = static_cast<int>(scalars[12]);
    flags_ = scalars[13];
    time_ = static_cast<int>(scalars[14]);
    travelType_ = static_cast<int>(scalars[15]);
    transitSwitchStartTime_ = static_cast<int>(scalars[16]);
    neighborAttempts_ = static_cast<int>(scalars[17]);

    MySimLocationRead(currentLocation_, stream);
    MySimLocationRead(defaultDestination_, stream);
    MySimLocationRead(currentDestination_, stream);
    MySimLocationRead(commuteDestination_, stream);

    trackedOccupant_ = nullptr;
    if (host) {
        cIGZUnknown* object = nullptr;
        if (host->ReadSerializable(stream, kGZIID_cISC4OccupantSaved, reinterpret_cast<void**>(&object)) && object) {
            void* occupant = nullptr;
            if (object->QueryInterface(kGZIID_cISC4Occupant, &occupant)) {
                trackedOccupant_ = static_cast<cISC4Occupant*>(occupant);
                static_cast<cISC4Occupant*>(occupant)->Release();
            }
            object->Release();
        }
    }
    stream.GetUint32(trackingBitmapId_);
    stream.GetGZStr(trackingText_);

    automataOccupant_ = nullptr;
    if (host) {
        cIGZUnknown* object = nullptr;
        if (host->ReadSerializable(stream, kGZIID_cISC4OccupantSaved, reinterpret_cast<void**>(&object)) && object) {
            void* occupant = nullptr;
            if (object->QueryInterface(kGZIID_cISC4Occupant, &occupant)) {
                automataOccupant_ = static_cast<cISC4Occupant*>(occupant);
                static_cast<cISC4Occupant*>(occupant)->Release();
            }
            object->Release();
        }
    }

    if (owner_) {
        owner_->Release();
        owner_ = nullptr;
    }
    if (host) {
        cIGZUnknown* object = nullptr;
        if (host->ReadSerializable(stream, kGZIID_cISC4MySimOwner, reinterpret_cast<void**>(&object)) && object) {
            owner_ = reinterpret_cast<IMySimDispatchOwner*>(object);
        }
    }
    if (clock_) {
        clock_->Release();
        clock_ = nullptr;
    }
    if (host) {
        cIGZUnknown* object = nullptr;
        if (host->ReadSerializable(stream, kGZIID_cISC4GameClock, reinterpret_cast<void**>(&object)) && object) {
            clock_ = object;
        }
    }

    stream.GetUint32(value);
    for (std::uint32_t i = 0; i < value; ++i) {
        RouteData data;
        std::uint8_t byte = 0;
        stream.GetUint8(byte); data.x = byte;
        stream.GetUint8(byte); data.z = byte;
        stream.GetUint8(byte); data.travelType = byte;
        route_.push_back(data);
    }

    stream.GetUint32(value);
    for (std::uint32_t i = 0; i < value; ++i) {
        std::uint32_t modelType = 0;
        cGZPersistResourceKey key;
        std::int32_t offset = 0;
        stream.GetUint32(modelType);
        stream >> key;
        stream.GetUint32(reinterpret_cast<std::uint32_t&>(offset));
        if (modelKeys_.find(static_cast<int>(modelType)) == modelKeys_.end()) {
            modelKeys_[static_cast<int>(modelType)] = std::make_pair(key, static_cast<int>(offset));
        }
    }

    std::uint32_t defaultModelType = 0;
    stream.GetUint32(defaultModelType);
    defaultModelType_ = static_cast<int>(defaultModelType);

    stream.GetUint32(value);
    for (std::uint32_t i = 0; i < value; ++i) {
        cLocation location;
        MySimLocationRead(location, stream);
        destinationQueue_.push_back(location);
    }

    if (host) {
        if (cIGZMessageServer2* server = host->MessageServer()) {
            server->AddNotification(this, kMySimDispatchMsgReadPending);
        }
    }
    initPending_ = !stream.GetError();
    return !stream.GetError();
}

}  // namespace sc4
