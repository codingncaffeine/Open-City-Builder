#include "sc4/DispatchManager.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cmath>

#include "rz/cRZMessage2Standard.h"

namespace sc4 {
namespace {

// Messages the manager registers in Init and handles in DoMessage (0x1002159c8 / 0x100216066).
constexpr std::uint32_t kDispatchMsgCameraMoved = 0xa6b79602;
constexpr std::uint32_t kDispatchMsgOccupantRemovedFromManager = 0x99ef1143;
constexpr std::uint32_t kDispatchMsgReturnDispatchedAutomata = 0xaa1b2002;   // name not recovered (see the header)
constexpr std::uint32_t kDispatchMsgAutomatonRemoved = 0x09d92fc3;

// Messages it posts (a dispatch source consumes them; cSC4FirePlaneDispatchSource::DoMessage 0x10072331c).
constexpr std::uint32_t kDispatchMsgBegin = 0xe94671eb;
constexpr std::uint32_t kDispatchMsgActivated = 0xe94671ec;
constexpr std::uint32_t kDispatchMsgDeactivated = 0xe94671ed;
constexpr std::uint32_t kDispatchMsgEnd = 0x894671f6;

// Occupant groups that select the dispatch-view tracker type (cISC4Occupant::IsOccupantGroup, slot 0xb0).
constexpr std::uint32_t kOccupantGroupVehicle = 0x4202;
constexpr std::uint32_t kOccupantGroupPoliceHelicopter = 0x4303;
constexpr std::uint32_t kOccupantGroupPedestrian = 0x4301;

// Dispatch-view tracker type per dispatch type (table at 0x100a1b2a0).
constexpr std::uint32_t kTrackerTypeForDispatchType[7] = {1, 0, 1, 1, 1, 0, 1};

IDispatchHost* g_host = nullptr;

// Builds the two end points of the station's dispatch line and its unit direction; returns the length.
// spCity vslot 0x300 converts the two cell corners, view vslot 0x60 gives the heights (0x10021667c /
// 0x100216dd0); dir stays zero when the corners coincide.
float PrepareDispatchSegment(IDispatchCity* city, const SC4CellRect& rect, float a[3], float b[3], float dir[3]) {
    float ax = 0.0f, az = 0.0f, bx = 0.0f, bz = 0.0f;
    if (city) {
        city->CellToWorld(rect.x0, rect.z0, ax, az);
        city->CellToWorld(rect.x1 + 1, rect.z1 + 1, bx, bz);
    }
    a[0] = ax;
    a[2] = az;
    b[0] = bx;
    b[2] = bz;
    a[1] = city ? city->CellHeight(a[0], a[2]) : 0.0f;
    b[1] = city ? city->CellHeight(b[0], b[2]) : 0.0f;
    const float dx = b[0] - a[0];
    const float dy = b[1] - a[1];
    const float dz = b[2] - a[2];
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    dir[0] = dir[1] = dir[2] = 0.0f;
    if (0.0f < length) {
        const float inv = 1.0f / length;
        dir[0] = dx * inv;
        dir[1] = dy * inv;
        dir[2] = dz * inv;
    }
    return length;
}

}  // namespace

IDispatchHost* DispatchHost() { return g_host; }
void SetDispatchHost(IDispatchHost* host) { g_host = host; }

cSC4DispatchManager::~cSC4DispatchManager() = default;

// ---- cIGZUnknown (one count for the three interface views) ----------------------------------------------------------------

bool cSC4DispatchManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4DispatchManager) {
        *out = static_cast<cISC4DispatchManager*>(this);
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

std::uint32_t cSC4DispatchManager::Release() {
    if (refCount_ > 1) return --refCount_;
    Shutdown();
    delete this;
    return 0;
}

// ---- lifecycle ------------------------------------------------------------------------------------------------------------

bool cSC4DispatchManager::Init() {
    if (initialized_) return true;

    IDispatchHost* host = DispatchHost();
    if (!messageServer_) {
        messageServer_ = host->MessageServer();
        if (messageServer_) {
            messageServer_->AddRef();
            messageServer_->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchMsgCameraMoved);
            messageServer_->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchMsgOccupantRemovedFromManager);
            messageServer_->AddNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchMsgReturnDispatchedAutomata);
        }
    }
    if (!vehicleView_) {
        vehicleView_ = host->CreateDispatchVehicleView();   // counted, like new + AddRef
        if (vehicleView_) {
            vehicleView_->Init();
            host->AddDispatchViewLayer(vehicleView_);        // spRenderer 0x100(view, 5, 1000)
        }
    }
    if (!city_) {
        city_ = host->City();
        if (city_) city_->AddRef();
    }
    if (restored_) {
        toolType_ = kDispatchTypeNone;
        SetActivePylon(nullptr);
        ClearAvailablePylonList();
        if (vehicleView_) {
            for (auto& info : dispatches_) {
                if (!info.automaton) continue;
                const std::uint32_t type = static_cast<std::uint32_t>(info.type);
                if (type < 7 && ((99u >> type) & 1u) != 0) {
                    info.trackId = vehicleView_->TrackOccupant(info.automaton->Occupant(),
                                                              static_cast<int>(kTrackerTypeForDispatchType[type]),
                                                              info.callNumber, nullptr);
                }
            }
        }
    }
    initialized_ = true;
    return true;
}

bool cSC4DispatchManager::Shutdown() {
    if (!initialized_) return true;
    IDispatchHost* host = DispatchHost();
    if (messageServer_) {
        messageServer_->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchMsgCameraMoved);
        messageServer_->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchMsgOccupantRemovedFromManager);
        messageServer_->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), kDispatchMsgReturnDispatchedAutomata);
        messageServer_->Release();
        messageServer_ = nullptr;
    }
    if (activePylon_) {
        activePylon_->Shutdown();
        activePylon_->Release();
        activePylon_ = nullptr;
    }
    ClearAvailablePylonList();
    if (vehicleView_) {
        host->RemoveDispatchViewLayer(vehicleView_);   // spRenderer 0x108(view)
        vehicleView_->Shutdown();
        vehicleView_->Release();
        vehicleView_ = nullptr;
    }
    if (city_) {
        city_->Release();
        city_ = nullptr;
    }
    initialized_ = false;
    return true;
}

// ---- dispatch-source registry ---------------------------------------------------------------------------------------------

void cSC4DispatchManager::RegisterDispatchSource(IDispatchSource* source) {
    // The original AddRefs into a cRZAutoRefCount slot, so even a null source becomes a (null) entry.
    sources_.emplace_back(source);
}

void cSC4DispatchManager::UnregisterDispatchSource(IDispatchSource* source) {
    for (auto it = sources_.begin(); it != sources_.end(); ++it) {
        if (it->get() == source) {
            sources_.erase(it);   // releases the removed entry, moves the rest (no net refcount change)
            return;
        }
    }
}

IDispatchSource* cSC4DispatchManager::GetDispatchSource(std::uint32_t type) const {
    for (const auto& source : sources_) {
        IDispatchSource* candidate = source.get();
        // the original has no null check; a null registration would crash here
        if (candidate && candidate->CanHandleDispatchType(type)) return candidate;
    }
    return nullptr;
}

// ---- queries --------------------------------------------------------------------------------------------------------------

int cSC4DispatchManager::GetDispatchCount(tDispatchType type) {
    int count = 0;
    for (const auto& info : dispatches_) {
        if (info.type == type) ++count;
    }
    return count;
}

bool cSC4DispatchManager::GetDispatchInfo(std::uint32_t id, DispatchInfo& out) {
    const auto it = FindByID(id);
    if (it == dispatches_.end()) return false;
    out = *it;
    return true;
}

void cSC4DispatchManager::GetDispatchIDArray(tDispatchType type, std::uint32_t* ids, std::uint32_t& count) {
    // Quirk kept: the limit is tested only after the id is written, so count == 0 never stops the loop.
    std::uint32_t written = 0;
    for (const auto& info : dispatches_) {
        if (info.type != type) continue;
        ids[written] = info.id;
        ++written;
        if (written == count) return;
    }
}

bool cSC4DispatchManager::RevokeAllDispatches(tDispatchType type) {
    (void)type;   // Quirk kept: the original ignores the dispatch type and revokes every dispatch.
    while (dispatchCount_ != 0 && !dispatches_.empty()) {
        RevokeDispatchByID(dispatches_.front().id);
    }
    return true;
}

IDispatchAutomaton* cSC4DispatchManager::GetAutomatonFromPylon(IPylonOccupant* pylon) const {
    for (const auto& info : dispatches_) {
        if (info.pylon == pylon) return info.automaton;
    }
    return nullptr;
}

// ---- pylons ---------------------------------------------------------------------------------------------------------------

IPylonOccupant* cSC4DispatchManager::GetActivePylon() const { return activePylon_; }
IDispatchVehicleView* cSC4DispatchManager::GetDispatchVehicleView() const { return vehicleView_; }

void cSC4DispatchManager::SetActivePylon(IPylonOccupant* pylon) {
    if (activePylon_ == pylon) return;
    if (activePylon_) {
        activePylon_->SetUIState(1);
        activePylon_->Release();
    }
    activePylon_ = pylon;
    if (pylon) {
        pylon->AddRef();
        pylon->SetUIState(2);
    }
}

void cSC4DispatchManager::ClearAvailablePylonList() {
    for (IPylonOccupant* pylon : availablePylons_) {
        if (!pylon) continue;
        pylon->Shutdown();
        pylon->Release();
    }
    availablePylons_.clear();
}

void cSC4DispatchManager::AddPylonToAvailableList(IPylonOccupant* pylon) {
    if (!pylon) return;
    if (std::find(availablePylons_.begin(), availablePylons_.end(), pylon) != availablePylons_.end()) return;
    availablePylons_.push_back(pylon);
    pylon->AddRef();
}

int cSC4DispatchManager::GetAvailablePylonCount(tDispatchType type) const {
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(type));
    if (!source) return 0;
    int count = 0;
    std::list<cISC4Occupant*> stations;
    if (source->GetStations(stations)) {
        for (cISC4Occupant* station : stations) {
            if (!station) continue;
            std::uint32_t callMask = 0, baseCall = 0, dispatchCount = 0;
            if (source->GetDispatchesAtStation(station, &callMask, &baseCall, &dispatchCount) && dispatchCount != 0) {
                for (std::uint32_t i = 0; i < dispatchCount; ++i) count += static_cast<int>((callMask >> i) & 1u);
            }
        }
        // spCity vslot 0x110 (occupant manager) -> 0x180: give the station references back.
        if (city_) city_->ReleaseStationList(stations);
    }
    stations.clear();
    return count;
}

// ---- dispatch tool --------------------------------------------------------------------------------------------------------

bool cSC4DispatchManager::BeginDispatchTool(tDispatchType type) {
    if (toolType_ == type) return true;
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(type));
    if (!source) return false;
    toolType_ = type;
    ClearAvailablePylonList();

    std::list<cISC4Occupant*> stations;
    if (!source->GetStations(stations)) {
        stations.clear();
        return true;
    }
    for (cISC4Occupant* station : stations) {
        if (!station) continue;
        std::uint32_t callMask = 0, baseCall = 0, dispatchCount = 0;
        if (!source->GetDispatchesAtStation(station, &callMask, &baseCall, &dispatchCount)) continue;
        float a[3] = {0.0f, 0.0f, 0.0f};
        float b[3] = {0.0f, 0.0f, 0.0f};
        float dir[3] = {0.0f, 0.0f, 0.0f};
        if (dispatchCount < 2) {
            // a single call box sits on the station itself (position, no direction)
            station->GetPosition(a);
        } else {
            SC4CellRect rect;
            station->GetBoundingCityCells(rect);
            const float length = PrepareDispatchSegment(city_, rect, a, b, dir);
            const float step = length / static_cast<float>(dispatchCount + 1);
            dir[0] *= step;
            dir[1] *= step;
            dir[2] *= step;
        }
        for (std::uint32_t i = 0; i < dispatchCount; ++i) {
            if (((callMask >> i) & 1u) == 0) continue;
            IDispatchHost* host = DispatchHost();
            IPylonOccupant* pylon = host->CreatePylonOccupant();   // counted (new + AddRef)
            pylon->Init();
            pylon->SetCallNumber(static_cast<std::uint16_t>(i + baseCall));
            pylon->SetDispatchState(kDispatchStateIdle);
            pylon->SetDispatchType(toolType_);
            pylon->SetUIState(1);
            pylon->SetSourceOccupant(station);
            const float k = static_cast<float>(i + 1);
            const float position[3] = {a[0] + dir[0] * k, a[1] + dir[1] * k, a[2] + dir[2] * k};
            pylon->AsOccupant()->SetPosition(position);
            availablePylons_.push_back(pylon);
        }
    }
    if (city_) city_->ReleaseStationList(stations);
    stations.clear();
    return true;
}

bool cSC4DispatchManager::EndDispatchTool(tDispatchType type) {
    if (toolType_ != type) return false;
    ClearAvailablePylonList();
    toolType_ = kDispatchTypeNone;
    return true;
}

// ---- dispatch creation ----------------------------------------------------------------------------------------------------

bool cSC4DispatchManager::CanDispatch(tDispatchType type, float x, float z) {
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(type));
    if (!source) return false;
    bool alreadyDispatched = false;   // Quirk kept: the source's flag is dropped.
    return source->CanDispatch(x, z, &alreadyDispatched);
}

std::uint32_t cSC4DispatchManager::Dispatch(tDispatchType type, float x, float z) {
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(type));
    if (!source) return 0;
    bool allBusy = false;   // the source's "every unit is out" flag (protection.md 8.2)
    if (!source->CanDispatch(x, z, &allBusy)) return 0;

    // i386 0x0018829c: without a call box in the player's hand, a dispatch needs a free unit.
    IPylonOccupant* active = GetActivePylon();
    if (!active && allBusy) return 0;

    if (active) {
        // Re-use the dispatch that already belongs to the active pylon (the id and type stay).
        for (auto& info : dispatches_) {
            if (info.pylon != active) continue;
            info.x = x;
            info.z = z;
            info.y = city_ ? city_->CellHeight(x, z) : 0.0f;
            if (!BeginDispatch(info)) {
                RevokeDispatchByID(info.id);
                return 0;
            }
            return info.id;
        }
    }

    DispatchInfo info;
    info.type = type;
    info.id = ++nextDispatchID_;
    info.x = x;
    info.z = z;
    info.y = city_ ? city_->CellHeight(x, z) : 0.0f;
    if (!InitDispatch(info)) return 0;
    dispatches_.push_back(info);
    ++dispatchCount_;
    DispatchInfo& stored = dispatches_.back();
    if (!BeginDispatch(stored)) {
        RevokeDispatchByID(stored.id);
        return 0;
    }
    return stored.id;
}

bool cSC4DispatchManager::InitDispatch(DispatchInfo& info) {
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(info.type));
    if (!source) return false;

    IPylonOccupant* active = GetActivePylon();
    if (active) {
        cISC4Occupant* station = active->GetSourceOccupant();
        if (station) station->AddRef();
        info.station = station;
        active->AddRef();
        info.pylon = active;
        info.callNumber = active->GetCallNumber();
        source->DispatchFromStation(station, &info.callNumber);
        const auto it = std::find(availablePylons_.begin(), availablePylons_.end(), active);
        if (it != availablePylons_.end()) {
            active->Release();   // the list's reference; the dispatch keeps its own
            availablePylons_.erase(it);
        }
    } else {
        cISC4Occupant* station = source->DispatchFromNearestStation(info.x, info.z, &info.callNumber);
        if (!station) return false;
        station->AddRef();
        info.station = station;
        // When the dispatch tool is running for this type its pre-created call boxes are used; the one with
        // the station's call number is taken out of the list.
        IPylonOccupant* found = nullptr;
        if (info.type == toolType_) {
            for (auto it = availablePylons_.begin(); it != availablePylons_.end(); ++it) {
                IPylonOccupant* candidate = *it;
                if (candidate && static_cast<std::uint32_t>(candidate->GetCallNumber()) == info.callNumber) {
                    candidate->Release();   // transfer the list's reference to the dispatch
                    availablePylons_.erase(it);
                    found = candidate;
                    break;
                }
            }
        }
        if (!found) {
            // Quirk kept (0x100217567..0x1002175a5): with no matching call box the original creates a pylon,
            // initialises it, then drops it without a Release while reporting success with info.pylon == null.
            IDispatchHost* host = DispatchHost();
            IPylonOccupant* created = host->CreatePylonOccupant();
            created->Init();
            created->SetSourceOccupant(info.station);
        }
        info.pylon = found;
    }

    if (info.pylon) {
        const float position[3] = {info.x, info.y, info.z};
        info.pylon->AsOccupant()->SetPosition(position);
        info.pylon->SetDispatchType(info.type);
    }
    return true;
}

bool cSC4DispatchManager::BeginDispatch(DispatchInfo& info) {
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(info.type));
    if (!source) return false;

    float position[3] = {info.x, info.y, info.z};
    if (city_) position[1] = city_->CellHeight(position[0], position[2]);

    // i386 0x00187eae BeginDispatch: a new vehicle leaves from the station (kind 0, the station occupant); a re-target passes a zeroed
    // first location (the vehicle keeps its return source). The second location is the clicked world position.
    tDispatchLocation from;
    if (!info.automaton) from.occupant = info.station;
    tDispatchLocation to;
    to.type = 1;   // tDispatchLocation::kWorldPosition
    to.x = position[0];
    to.y = position[1];
    to.z = position[2];

    std::uint32_t group = 0;
    cGZPersistResourceKey key;
    std::uint32_t flags = 0;
    source->GetDispatchAutomataProperties(info.station, group, key, flags, position);

    IDispatchHost* host = DispatchHost();
    IAutomataDispatchManager* manager = host->GetAutomataManagerForGroup(group);
    if (!manager) return false;

    if (info.automaton) {
        info.automaton->SetDispatchState(kDispatchStateDispatched);
        manager->Dispatch(info.automaton, from, to, flags, 2);
        info.automaton->AddCallback(static_cast<cIGZMessageTarget2*>(this));
    } else {
        IDispatchAutomaton* automaton = manager->Dispatch(group, from, to, flags, 0, key, 0xff);
        if (!automaton) return false;
        automaton->AddRef();
        automaton->AddCallback(static_cast<cIGZMessageTarget2*>(this));
        info.automaton = automaton;
        automaton->SetDispatchInfoID(info.id);
        automaton->SetCallNumber(static_cast<std::uint16_t>(info.callNumber));
        automaton->SetDispatchState(kDispatchStateDispatched);
        if (vehicleView_) {
            cISC4Occupant* occupant = automaton->Occupant();
            if (occupant) occupant->AddRef();
            int trackerType = 0;
            if (occupant && !occupant->IsOccupantGroup(kOccupantGroupVehicle)) {
                if (!occupant->IsOccupantGroup(kOccupantGroupPoliceHelicopter)) {
                    trackerType = 1;
                    occupant->IsOccupantGroup(kOccupantGroupPedestrian);   // original calls it, result unused
                }
            }
            info.trackId = vehicleView_->TrackOccupant(occupant, trackerType, automaton->GetCallNumber(), nullptr);
            if (occupant) occupant->Release();
        }
        if (info.pylon) info.pylon->SetCallNumber(automaton->GetCallNumber());
    }

    if (info.automaton && info.pylon) info.pylon->SetDispatchState(info.automaton->GetDispatchState());
    PostDispatchMessage(kDispatchMsgBegin, static_cast<std::uint32_t>(info.type), info.id);
    return true;
}

// ---- ending a dispatch ----------------------------------------------------------------------------------------------------

bool cSC4DispatchManager::EndDispatch(DispatchInfo& info) {
    info.active = false;
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(info.type));
    if (!source) return false;   // Quirk kept: the pylon/automaton/station stay attached in this path.

    source->RevokeDispatchFromStation(info.station, info.callNumber);

    if (info.pylon) {
        if (GetActivePylon() == info.pylon) SetActivePylon(nullptr);
        if (info.type == toolType_) {
            ReturnPylonToStation(source, info.pylon);
        } else {
            info.pylon->Shutdown();
        }
        info.pylon->Release();
        info.pylon = nullptr;
    }
    if (info.automaton) {
        if (vehicleView_) vehicleView_->UntrackOccupant(info.trackId, false);
        info.automaton->Release();
        info.automaton = nullptr;
    }
    if (info.station) {
        info.station->Release();
        info.station = nullptr;
    }
    PostDispatchMessage(kDispatchMsgEnd, static_cast<std::uint32_t>(info.type), info.id);
    return true;
}

bool cSC4DispatchManager::RevokeDispatchByID(std::uint32_t id) {
    const auto it = FindByID(id);
    if (it == dispatches_.end()) return false;
    const bool result = EndDispatch(*it);
    EraseDispatch(it);
    return result;
}

bool cSC4DispatchManager::RevokeDispatchByPylon(IPylonOccupant* pylon) {
    if (!pylon) return false;
    bool result = false;
    const auto it = FindByPylon(pylon);
    if (it != dispatches_.end()) result = RevokeDispatchByID(it->id);
    // The original returns the pylon to its station even when no dispatch matched. It reads the dispatch type
    // back from the pylon, so the caller has to keep a reference (the dispatch tool does).
    const tDispatchType type = pylon->GetDispatchType();
    IDispatchSource* source = GetDispatchSource(static_cast<std::uint32_t>(type));
    ReturnPylonToStation(source, pylon);
    return result;
}

void cSC4DispatchManager::ReturnPylonToStation(IDispatchSource* source, IPylonOccupant* pylon) {
    if (!source || !pylon) return;
    cISC4Occupant* occupant = pylon->AsOccupant();
    if (!occupant) {
        // no occupant: the pylon is dropped (0x100216828)
        pylon->Shutdown();
        pylon->Release();
        return;
    }
    std::uint32_t callMask = 0, baseCall = 0, dispatchCount = 0;
    if (source->GetDispatchesAtStation(occupant, &callMask, &baseCall, &dispatchCount)) {
        SC4CellRect rect;
        occupant->GetBoundingCityCells(rect);
        float a[3] = {0.0f, 0.0f, 0.0f};
        float b[3] = {0.0f, 0.0f, 0.0f};
        float dir[3] = {0.0f, 0.0f, 0.0f};
        const float length = PrepareDispatchSegment(city_, rect, a, b, dir);
        const float step = length / static_cast<float>(dispatchCount + 1);
        // Call numbers run along the line from the station; slot 0 is baseCall.
        const float k = static_cast<float>(((static_cast<int>(pylon->GetCallNumber()) - static_cast<int>(baseCall)) & 0xff) + 1);
        const float position[3] = {a[0] + dir[0] * step * k, a[1] + dir[1] * step * k, a[2] + dir[2] * step * k};
        pylon->AsOccupant()->SetPosition(position);
        pylon->SetDispatchState(kDispatchStateIdle);
        pylon->SetUIState(1);
    }
    AddPylonToAvailableList(pylon);
}

// ---- messages -------------------------------------------------------------------------------------------------------------

void cSC4DispatchManager::PostDispatchMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2) {
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetIGZUnknown(static_cast<cISC4DispatchManager*>(this));
    message->SetData1(data1);
    message->SetData2(data2);
    // the original posts without a null check; the manager is initialised before it posts
    if (messageServer_) messageServer_->MessagePost(message, false);
    message->Release();
}

bool cSC4DispatchManager::UpdateDispatchState(DispatchInfo& info, tDispatchState state) {
    const bool active = (state == kDispatchStateActive);
    if (info.active != active) {
        info.active = active;
        PostDispatchMessage(active ? kDispatchMsgActivated : kDispatchMsgDeactivated,
                            static_cast<std::uint32_t>(info.type), info.id);
    }
    if (info.pylon) info.pylon->SetDispatchState(state);
    return true;
}

bool cSC4DispatchManager::HandleAutomataEventMessage(cIGZMessage2Standard* message) {
    if (!message) return false;
    IDispatchHost* host = DispatchHost();
    void* object = message->GetVoid1();
    IDispatchAutomaton* automaton = object && host ? host->AutomatonFromMessage(object) : static_cast<IDispatchAutomaton*>(object);
    if (!automaton) return false;
    const std::uint32_t event = static_cast<std::uint32_t>(message->GetData2());
    static const bool trace = std::getenv("SC4_DISPATCHDEBUG") != nullptr;
    if (trace) std::fprintf(stderr, "dispatch: automaton event %u data %u\n", event, static_cast<std::uint32_t>(message->GetData3()));
    if ((event | 4u) != 7u) return false;   // only 3 (removed) and 7 (reached its destination)
    for (auto& info : dispatches_) {
        if (info.automaton != automaton) continue;
        if (event == 7) {
            const std::uint32_t state = static_cast<std::uint32_t>(message->GetData3());
            UpdateDispatchState(info, static_cast<tDispatchState>(state));
            if (state != 0) break;
        } else if (event != 3) {
            break;
        }
        RevokeDispatchByID(info.id);
        break;
    }
    return true;
}

bool cSC4DispatchManager::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    if (type == kDispatchMsgOccupantRemovedFromManager) {
        auto* occupant = static_cast<cISC4Occupant*>(static_cast<cIGZMessage2Standard*>(message)->GetVoid1());
        for (auto it = dispatches_.begin(); it != dispatches_.end();) {
            if (it->station == occupant) {
                if (it->automaton) {
                    if (vehicleView_) vehicleView_->UntrackOccupant(it->trackId, false);
                    it->automaton->Remove(false);
                    it->automaton->Release();
                    it->automaton = nullptr;
                }
                if (it->pylon) {
                    if (GetActivePylon() == it->pylon) SetActivePylon(nullptr);
                    it->pylon->Shutdown();
                    it->pylon->Release();
                    it->pylon = nullptr;
                }
                if (it->station) {
                    it->station->Release();
                    it->station = nullptr;
                }
                it = dispatches_.erase(it);
                --dispatchCount_;
                continue;
            }
            ++it;
        }
        // Call boxes that were waiting for a call from that station go away too.
        for (auto it = availablePylons_.begin(); it != availablePylons_.end();) {
            IPylonOccupant* pylon = *it;
            if (!pylon || pylon->GetSourceOccupant() != occupant) {
                ++it;
                continue;
            }
            pylon->Shutdown();
            pylon->Release();
            if (GetActivePylon() == pylon) SetActivePylon(nullptr);
            it = availablePylons_.erase(it);
        }
        return true;
    }
    if (type == kDispatchMsgCameraMoved) {
        if (vehicleView_) vehicleView_->HandleViewChanged();
    } else if (type == kDispatchMsgReturnDispatchedAutomata) {
        for (auto& info : dispatches_) {
            if (info.automaton) info.automaton->ReturnToSourceOccupant();
        }
    } else if (type == kDispatchMsgAutomatonRemoved) {
        return HandleAutomataEventMessage(static_cast<cIGZMessage2Standard*>(message));
    }
    return true;
}

// ---- persistence ----------------------------------------------------------------------------------------------------------

void cSC4DispatchManager::WriteDispatchInfo(cIGZOStream& stream, const DispatchInfo& info) const {
    stream.SetUint32(static_cast<std::uint32_t>(info.type));
    stream.SetSint32(static_cast<std::int32_t>(info.id));
    stream.SetSint32(static_cast<std::int32_t>(info.callNumber));
    stream.SetUint8(info.active ? 1 : 0);   // operator<<(cIGZOStream&, bool)
    stream.SetFloat32(info.x);
    stream.SetFloat32(info.y);
    stream.SetFloat32(info.z);
    IDispatchHost* host = DispatchHost();
    host->WriteSerializable(stream, info.station);
    host->WriteSerializable(stream, info.automaton);
    host->WriteSerializable(stream, info.pylon);
}

// The original record is: u32 type, s32 id, s32 call number, u8 active, f32 x/y/z, station, automaton, pylon.
// Quirk kept: the dispatch-view track id (+0x0c) is written by neither side, so a loaded dispatch has 0.
void cSC4DispatchManager::ReadDispatchInfo(cIGZIStream& stream, DispatchInfo& info) {
    std::uint32_t type = 0;
    stream.GetUint32(type);
    info.type = static_cast<tDispatchType>(type);
    std::int32_t value = 0;
    stream.GetSint32(value);
    info.id = static_cast<std::uint32_t>(value);
    stream.GetSint32(value);
    info.callNumber = static_cast<std::uint32_t>(value);
    std::uint8_t active = 0;
    stream.GetUint8(active);
    info.active = active != 0;
    stream.GetFloat32(info.x);
    stream.GetFloat32(info.y);
    stream.GetFloat32(info.z);
    IDispatchHost* host = DispatchHost();
    void* raw = nullptr;
    host->ReadSerializable(stream, kGZIID_cISC4Occupant, &raw);
    info.station = static_cast<cISC4Occupant*>(raw);
    raw = nullptr;
    host->ReadSerializable(stream, kDispatchAutomatonIid, &raw);
    info.automaton = static_cast<IDispatchAutomaton*>(raw);
    raw = nullptr;
    host->ReadSerializable(stream, kDispatchPylonIid, &raw);
    info.pylon = static_cast<IPylonOccupant*>(raw);
}

bool cSC4DispatchManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(kDispatchRecordVersion);
    stream.SetSint32(static_cast<std::int32_t>(dispatchCount_));
    for (const auto& info : dispatches_) WriteDispatchInfo(stream, info);
    // Quirk kept: the next-dispatch-id counter is written between the list and the active pylon.
    stream.SetSint32(static_cast<std::int32_t>(nextDispatchID_));
    IDispatchHost* host = DispatchHost();
    host->WriteSerializable(stream, activePylon_);
    stream.SetSint32(static_cast<std::int32_t>(availablePylons_.size()));
    for (IPylonOccupant* pylon : availablePylons_) host->WriteSerializable(stream, pylon);
    stream.SetSint32(static_cast<std::int32_t>(toolType_));
    return !stream.GetError();
}

bool cSC4DispatchManager::Read(cIGZIStream& stream) {
    restored_ = true;
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version != kDispatchRecordVersion) return false;
    std::int32_t count = 0;
    stream.GetSint32(count);
    // Quirk kept: no clear, the record is appended to whatever the manager already holds.
    for (std::int32_t i = 0; i < count; ++i) {
        DispatchInfo info;
        ReadDispatchInfo(stream, info);
        dispatches_.push_back(info);
        ++dispatchCount_;
    }
    std::int32_t nextID = 0;
    stream.GetSint32(nextID);
    nextDispatchID_ = static_cast<std::uint32_t>(nextID);
    IDispatchHost* host = DispatchHost();
    void* raw = nullptr;
    host->ReadSerializable(stream, kDispatchPylonIid, &raw);
    activePylon_ = static_cast<IPylonOccupant*>(raw);   // Quirk kept: the previous pylon is not released.
    std::int32_t pylonCount = 0;
    stream.GetSint32(pylonCount);
    for (std::int32_t i = 0; i < pylonCount; ++i) {
        raw = nullptr;
        host->ReadSerializable(stream, kDispatchPylonIid, &raw);
        availablePylons_.push_back(static_cast<IPylonOccupant*>(raw));
    }
    std::int32_t tool = 0;
    stream.GetSint32(tool);
    toolType_ = static_cast<tDispatchType>(tool);
    return !stream.GetError();
}

// ---- helpers --------------------------------------------------------------------------------------------------------------

cSC4DispatchManager::DispatchList::iterator cSC4DispatchManager::FindByID(std::uint32_t id) {
    for (auto it = dispatches_.begin(); it != dispatches_.end(); ++it) {
        if (it->id == id) return it;
    }
    return dispatches_.end();
}

cSC4DispatchManager::DispatchList::iterator cSC4DispatchManager::FindByPylon(IPylonOccupant* pylon) {
    for (auto it = dispatches_.begin(); it != dispatches_.end(); ++it) {
        if (it->pylon == pylon) return it;
    }
    return dispatches_.end();
}

void cSC4DispatchManager::EraseDispatch(DispatchList::iterator it) {
    dispatches_.erase(it);
    --dispatchCount_;
}

}  // namespace sc4
