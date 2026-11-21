#include "sc4/AircraftManager.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandomSuper.h"
#include "sc4/DBSegment.h"
#include "sc4/Occupant.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
IAircraftManagerHost* g_host = nullptr;
cRZRandomSuper* g_random = nullptr;   // cSC4Aircraft::mpRandom

constexpr std::uint32_t kPropertyNoAutomataHere = 0x029244db;
constexpr float kVectorEpsilon = 0.0001f;   // FLOAT_00a7078c, the cS3DVector3 comparison tolerance

// kInvalidVector of this translation unit (i386 0x00c11980, set by its static initializer to (-1, -2, -3)): the value
// GetRunwayList pads the list with.
constexpr cS3DVector3 kRunwayInvalidVector{-1.0f, -2.0f, -3.0f};

// cS3DVector3::operator== / operator!= (i386 0x0062d38a / 0x006217be): every component within the tolerance. The
// comparison is UCOMISS + JBE, so an unordered (NaN) component counts as different.
bool VectorsEqual(const cS3DVector3& a, const cS3DVector3& b) {
    const float d[3] = {std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)};
    for (float v : d)
        if (!(kVectorEpsilon > v)) return false;
    return true;
}

cS3DVector3 OccupantPosition(cISC4Occupant* occupant) {
    float p[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetPosition(p);
    return cS3DVector3{p[0], p[1], p[2]};
}

// The dispatch target's altitude: the terrain under it (or the off-terrain height) plus 200.
float DispatchAltitude(float x, float z) {
    if (!g_host) return kAircraftDispatchAltitude;
    const float ground = g_host->TerrainPointInBounds(x, z) ? g_host->TerrainHeight(x, z) : g_host->DefaultAltitude();
    return ground + kAircraftDispatchAltitude;
}
}  // namespace

IAircraftManagerHost* AircraftManagerHost() { return g_host; }
void SetAircraftManagerHost(IAircraftManagerHost* host) { g_host = host; }

cRZRandomSuper* cSC4AircraftManager::AircraftRandom() { return g_random; }

std::uint32_t cSC4AircraftManager::RandomWeightedChoice(std::uint32_t count, const double* weights) {
    return g_random ? g_random->RandomUint32WeightedChoice(count, weights) : 0;
}

double cSC4AircraftManager::RandomDoubleRange(double lo, double hi) {
    return g_random ? g_random->RandomDoubleRangeUniform(lo, hi) : lo;
}

cSC4AircraftManager::cSC4AircraftManager() {
    SetExemplarGroup(kAircraftManagerExemplarGroup);
    SetDefaultGroup(kAircraftManagerDefaultGroup);
    SetFallbackGroup(kAircraftManagerFallbackGroup);
}

cSC4AircraftManager::~cSC4AircraftManager() { ReleaseAirports(); }

void cSC4AircraftManager::ReleaseAirports() {
    for (auto& entry : airports_) {
        if (entry.first) entry.first->Release();
        if (entry.second) entry.second->Release();
    }
    airports_.clear();
}

bool cSC4AircraftManager::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZCLSID_cSC4AircraftManager) {
        *out = static_cast<cSC4AircraftManager*>(this);
        AddRef();
        return true;
    }
    return cSC4AutomataManager::QueryInterface(iid, out);
}

// The base Init, then the building/prop filter the aircraft's collision query uses, mpRandom and the tuning values.
bool cSC4AircraftManager::Init() {
    if (!Initialized()) {
        cSC4AutomataManager::Init();
        buildingFilter_ = g_host ? g_host->NewBuildingPropFilter() : nullptr;
        g_random = &Random();
        HandleTuningParametersChanged();
    }
    return true;
}

bool cSC4AircraftManager::Shutdown() {
    if (Initialized()) {
        ReleaseAirports();
        runways_.clear();
        if (buildingFilter_ && g_host) g_host->ReleaseBuildingPropFilter(buildingFilter_);
        buildingFilter_ = nullptr;
        cSC4AutomataManager::Shutdown();
        g_random = nullptr;
    }
    return true;
}

// The maximum count is the first tuning value (property 0x4998c89f); the minimum zoom comes from the automata tuning
// exemplar's property 0x4998c91a (default 1), stored minus one.
bool cSC4AircraftManager::HandleTuningParametersChanged() {
    SetMaxCount(AutomataTuning().params.f000);
    IAutomataManagerHost* host = sc4::AutomataManagerHost();
    cISCPropertyHolder* holder = nullptr;
    if (host && host->GetTuningProperties(&holder) && holder) {
        std::uint8_t minZoom = 1;
        cSCPropertyHelp::GetPropertyValue(holder, kAircraftPropertyMinZoom, minZoom);
        SetMinZoom(static_cast<int>(static_cast<std::uint8_t>(minZoom - 1)));
        holder->Release();
    }
    return true;
}

// A null automaton means "a random ambient aircraft": a helicopter for draws below 0x7fffffff, a blimp otherwise.
bool cSC4AircraftManager::AddAutomaton(IAutomataAutomaton* automaton) {
    if (!automaton) {
        const std::uint32_t draw = Random().RandomUint32Uniform();
        IAutomataAutomaton* made = nullptr;
        if (g_host) made = draw < 0x7fffffffu ? g_host->NewHelicopter() : g_host->NewBlimp();
        if (!made) return false;
        const bool added = cSC4AutomataManager::AddAutomaton(made);
        made->Release();   // the base list holds its own reference
        return added;
    }
    return cSC4AutomataManager::AddAutomaton(automaton);
}

bool cSC4AircraftManager::DoMessage(cIGZMessage2* message) {
    bool handled = false;
    if (message) {
        const std::uint32_t type = message->GetType();
        if (type == kMessageOccupantInserted || type == kMessageOccupantRemovedFromManager) {
            void* raw = nullptr;
            cISC4Occupant* occupant = nullptr;
            if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) && raw) {
                auto* standard = static_cast<cIGZMessage2Standard*>(raw);
                occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
                standard->Release();
            }
            if (occupant) {
                cISCPropertyHolder* holder = occupant->AsPropertyHolder();
                if (!holder || !holder->HasProperty(kPropertyNoAutomataHere)) {
                    if (type == kMessageOccupantInserted) HandleOccupantInserted(occupant);
                    else HandleOccupantRemoved(occupant);
                }
            }
            handled = type == kMessageOccupantInserted;   // the removed message still reports the base's answer only
        }
    }
    const bool base = cSC4AutomataManager::DoMessage(message);
    return base || handled;
}

void cSC4AircraftManager::HandleOccupantInserted(cISC4Occupant* occupant) {
    if (!occupant) return;
    // Creates the occupant's aircraft through CreateAutomaton and records it (one reference each on the occupant and the
    // automaton; CreateAutomaton itself hands back a borrowed pointer).
    auto create = [&](std::uint32_t specificGroup, std::uint32_t baseGroup, std::uint32_t initialState) {
        tExemplarData* exemplar = ChooseRandomExemplar(specificGroup, PopulateLayer::kLayerNone, -1, -1);
        cS3DVector3 position = OccupantPosition(occupant);
        float boxMin[3] = {0.0f, 0.0f, 0.0f}, boxMax[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetBoundingBox(boxMin, boxMax);
        position.y = boxMin[1];
        std::int32_t orientation = g_host ? g_host->PropOrientation(occupant) : 0;
        if (orientation < 0 || orientation > 3) orientation = 0;
        const cS3DVector3 facing{kAircraftFacing[orientation][0], kAircraftFacing[orientation][1], kAircraftFacing[orientation][2]};
        if (!exemplar) return;
        IAutomataAutomaton* automaton = CreateAutomaton(baseGroup, position, kAircraftCreateFlags, exemplar->modelKey, nullptr, 0, &facing);
        if (!automaton) return;
        occupant->AddRef();
        automaton->AddRef();
        airports_[occupant] = automaton;
        if (initialState != 0) automaton->PushState(initialState, nullptr, -1.0f, false, 0, 0, nullptr);
    };

    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    std::uint8_t helipad = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kAircraftPropertyHelipad, helipad);
    if (helipad >= 1 && helipad <= 5) {
        static constexpr std::uint32_t kGroups[5] = {0x4301, 0x4305, 0x4304, 0x4307, 0x4308};
        static constexpr std::uint32_t kStates[5] = {0x36, 0x38, 0x37, 0, 0};
        create(kGroups[helipad - 1], kAircraftGroupHelicopterBase, kStates[helipad - 1]);
    }

    holder = occupant->AsPropertyHolder();
    std::uint8_t airport = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kAircraftPropertyAirport, airport);
    if (airport >= 1 && airport <= 5) {
        static constexpr std::uint32_t kGroups[5] = {0x4306, 0x4303, 0x4311, 0x4312, 0x4313};
        create(kGroups[airport - 1], kAircraftGroupAirplaneBase, 0);
    }

    holder = occupant->AsPropertyHolder();
    std::uint8_t runway = 0;
    cSCPropertyHelp::GetPropertyValue(holder, kAircraftPropertyRunway, runway);
    if (runway != 0) {
        const cS3DVector3 position = OccupantPosition(occupant);
        runways_.push_back(AircraftRunwayMarker{position.x, position.y, position.z, runway});
    }
}

void cSC4AircraftManager::HandleOccupantRemoved(cISC4Occupant* occupant) {
    if (!occupant) return;
    const auto it = airports_.find(occupant);
    if (it != airports_.end()) {
        IAutomataAutomaton* automaton = it->second;
        if (automaton) automaton->Remove(false);
        airports_.erase(it);
        if (automaton) automaton->Release();
        occupant->Release();
    }
    std::uint8_t runway = 0;
    cSCPropertyHelp::GetPropertyValue(occupant->AsPropertyHolder(), kAircraftPropertyRunway, runway);
    if (runway == 0) return;
    const cS3DVector3 position = OccupantPosition(occupant);
    const auto marker = std::find_if(runways_.begin(), runways_.end(), [&](const AircraftRunwayMarker& m) {
        return m.flag == runway && VectorsEqual(cS3DVector3{m.x, m.y, m.z}, position);
    });
    if (marker != runways_.end()) runways_.erase(marker);
}

// The runway ends on the lot under `position`: runway n fills the pair (2n-2, 2n-1), the slots it does not reach keep
// kInvalidVector. A marker goes to the pair's second slot when the first already differs from kInvalidVector.
void cSC4AircraftManager::GetRunwayList(const cS3DVector3& position, std::vector<cS3DVector3>& out) {
    out.clear();
    const void* lot = g_host ? g_host->LotAt(position) : nullptr;
    if (!lot) return;
    const cS3DVector3 invalid = kRunwayInvalidVector;
    for (const AircraftRunwayMarker& marker : runways_) {
        const cS3DVector3 point{marker.x, marker.y, marker.z};
        if (g_host->LotAt(point) != lot) continue;
        const std::size_t pairEnd = static_cast<std::size_t>(marker.flag) * 2;
        if (out.size() < pairEnd) out.resize(pairEnd, invalid);
        const std::size_t slot = VectorsEqual(out[pairEnd - 2], invalid) ? pairEnd - 2 : pairEnd - 1;
        out[slot] = point;
    }
}

// 0x90: the aircraft flies to `to`; a police helicopter (occupant group 0x4301) holds for 10000 ms (state 0x35), then goes
// (state 0x33); any other aircraft goes with state 0x39. The station that sent it gives its efficiency.
bool cSC4AircraftManager::Dispatch(IAutomataAutomaton* automaton, const tAutomataLocation& from, const tAutomataLocation& to,
                                   std::uint32_t flags, std::uint8_t type) {
    (void)flags;
    (void)type;
    if (!automaton) return false;
    cS3DVector3 destination;
    if (!TranslateDispatchLocation(to, destination)) return false;
    destination.y = DispatchAltitude(destination.x, destination.z);
    if (g_host) g_host->SetDestination(automaton, destination);
    IAutomataOccupant* self = automaton->Occupant();
    const bool police = self && self->IsOccupantGroup(kAircraftGroupPoliceHelicopter);
    float efficiency = 1.0f;   // kSC4PercentageOneHundred
    if (from.kind == kLocationOccupant && from.occupant && g_host) {
        if (!police) {
            if (g_host->IsFireStation(from.occupant)) efficiency = g_host->FireStationEfficiency(from.occupant);
        } else if (g_host->IsPoliceStation(from.occupant)) {
            efficiency = g_host->PoliceStationEfficiency(from.occupant);
        }
    }
    if (g_host) g_host->SetStationEfficiency(automaton, efficiency);
    if (police) {
        automaton->PushState(0x35, nullptr, kAircraftDispatchHoldTimer, false, 0, 0, nullptr);
        automaton->PushState(0x33, nullptr, -1.0f, false, 0, 0, nullptr);
    } else {
        automaton->PushState(0x39, nullptr, -1.0f, false, 0, 0, nullptr);
    }
    return true;
}

// 0x88: no automaton is created; the aircraft of the airport map whose occupant stands on the station's lot is sent. A
// police station sends with the police states, any other station with the fire state.
IAutomataAutomaton* cSC4AircraftManager::Dispatch(std::uint32_t group, const tAutomataLocation& from, const tAutomataLocation& to,
                                                  std::uint32_t flags, std::uint8_t type, const cGZPersistResourceKey& key,
                                                  std::uint8_t argument) {
    (void)group;
    (void)flags;
    (void)type;
    (void)key;
    (void)argument;
    if (from.kind != kLocationOccupant || !from.occupant || !g_host) return nullptr;
    float efficiency = 1.0f;   // kSC4PercentageOneHundred
    bool police = false;
    if (g_host->IsPoliceStation(from.occupant)) {
        efficiency = g_host->PoliceStationEfficiency(from.occupant);
        police = true;
    } else if (g_host->IsFireStation(from.occupant)) {
        efficiency = g_host->FireStationEfficiency(from.occupant);
    }
    std::vector<cISC4Occupant*> onLot;
    if (!g_host->StationLotOccupants(from.occupant, onLot)) return nullptr;
    IAutomataAutomaton* automaton = nullptr;
    for (const auto& entry : airports_) {
        if (std::find(onLot.begin(), onLot.end(), entry.first) != onLot.end()) {
            automaton = entry.second;
            break;
        }
    }
    if (!automaton) return nullptr;
    cS3DVector3 destination;
    if (!TranslateDispatchLocation(to, destination)) return nullptr;
    destination.y = DispatchAltitude(destination.x, destination.z);
    g_host->SetDestination(automaton, destination);
    g_host->SetStationEfficiency(automaton, efficiency);
    if (police) {
        automaton->PushState(0x35, nullptr, kAircraftDispatchHoldTimer, false, 0, 0, nullptr);
        automaton->PushState(0x33, nullptr, -1.0f, false, 0, 0, nullptr);
    } else {
        automaton->PushState(0x39, nullptr, -1.0f, false, 0, 0, nullptr);
    }
    return automaton;
}

bool cSC4AircraftManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(kAircraftManagerRecordVersion);
    if (cSC4AutomataManager::Write(stream)) {
        stream.SetUint32(static_cast<std::uint32_t>(airports_.size()));
        for (const auto& entry : airports_) {
            SC4WriteGZSerializable(stream, static_cast<const cIGZUnknown*>(entry.first));
            SC4WriteGZSerializable(stream, static_cast<const cIGZUnknown*>(entry.second));
        }
        stream.SetUint32(static_cast<std::uint32_t>(runways_.size()));
        for (const AircraftRunwayMarker& marker : runways_) {
            stream.SetFloat32(marker.x);
            stream.SetFloat32(marker.y);
            stream.SetFloat32(marker.z);
            stream.SetUint8(marker.flag);
        }
    }
    return !stream.GetError();
}

bool cSC4AircraftManager::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version < kAircraftManagerRecordVersion) return ReadInternal(stream, version);
    if (version != kAircraftManagerRecordVersion) return false;
    if (cSC4AutomataManager::Read(stream)) {
        IAutomataManagerHost* host = sc4::AutomataManagerHost();
        std::uint32_t count = 0;
        stream.GetUint32(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            void* occupant = nullptr;
            void* automaton = nullptr;
            SC4ReadGZSerializable(stream, kAircraftIIDOccupant, &occupant, false);
            SC4ReadGZSerializable(stream, kAircraftIIDAutomaton, &automaton, false);
            // The automaton read for cISC4Automaton is the object itself; the manager keeps the base's view of it (the
            // read reference becomes the map's). Without a view the reference is dropped and the entry keeps null.
            IAutomataAutomaton* view = automaton && host ? host->LoadedAutomaton(automaton) : nullptr;
            if (automaton && !view) static_cast<cIGZUnknown*>(automaton)->Release();
            airports_[static_cast<cISC4Occupant*>(occupant)] = view;
        }
        stream.GetUint32(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            AircraftRunwayMarker marker;
            stream.GetFloat32(marker.x);
            stream.GetFloat32(marker.y);
            stream.GetFloat32(marker.z);
            stream.GetUint8(marker.flag);
            runways_.push_back(marker);
        }
    }
    return !stream.GetError();
}

}  // namespace sc4
