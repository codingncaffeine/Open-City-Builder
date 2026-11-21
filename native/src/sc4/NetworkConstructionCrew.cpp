// cSC4NetworkConstructionCrew (Mac i386 0x00163e54..0x0016722a): see NetworkConstructionCrew.h.
#include "sc4/NetworkConstructionCrew.h"

#include <cmath>
#include <cstdlib>

#include "gz/StreamOps.h"
#include "rz/RZTime.h"
#include "rz/cRZRandom.h"
#include "rz/cRZRandomSuper.h"
#include "s3d/S3DMath.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/NetworkTool.h"
#include "sc4/PathInfo.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {
INetworkConstructionCrewHost* g_host = nullptr;
bool g_staticInited = false;   // sbInited

constexpr std::uint32_t kGZIID_cIGZMessageTarget2Crew = 0x452294aa;
constexpr std::uint32_t kCrewMessageAnimationTick = 0x86ad10ee;
constexpr std::uint32_t kCrewMessageOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kCrewMessageReset = 0x26d31ec2;
constexpr std::uint32_t kVehicleGroup = 0x4002;
constexpr std::uint32_t kVehicleFlags = 0x3b000;
constexpr std::uint32_t kWorkerGroup = 0x4101;
constexpr std::uint32_t kWorkerFlags = 0x3a000;
constexpr std::uint32_t kVehicleModelType = 0x5ad0e817;    // the vehicle's model key: type, group, the network's model id
constexpr std::uint32_t kVehicleModelGroup = 0xbadb57f1;
constexpr float kOrientationEpsilon = 1.0e-5f;             // cSC4TrafficAutomaton::kfAutomatonOrientationEpsilon (0x00a70830)
constexpr float kArrivalDistance = 0.6f;                   // FLOAT_00a707a8
constexpr float kCellsPerUnit = 0.0625f;                   // FLOAT_00a70700
// CreateConstructionAutomata::pathDistances (0x0084600c): where along each path the worker spots are taken.
constexpr float kPathDistances[4] = {0.0f, 0.25f, 0.5f, 0.75f};

cISC4Occupant* AsOccupant(cSC4NetworkOccupant* piece) { return piece; }   // cISC4NetworkOccupant vslot 0xc0

float Length(const float* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

// The float -> unsigned conversion of the original (negative values clamp to 0).
std::uint32_t ToUnsigned(float value) { return value <= 0.0f ? 0u : static_cast<std::uint32_t>(value); }
}  // namespace

void SetNetworkConstructionCrewHost(INetworkConstructionCrewHost* host) { g_host = host; }
INetworkConstructionCrewHost* NetworkConstructionCrewHost() { return g_host; }

// ==== 0x00165600 ===========================================================================================================
cSC4NetworkConstructionCrew::cSC4NetworkConstructionCrew() = default;

// ==== 0x0016553e ===========================================================================================================
cSC4NetworkConstructionCrew::~cSC4NetworkConstructionCrew() = default;

std::size_t cSC4NetworkConstructionCrew::StepCount() const { return steps_.size(); }

// ==== 0x00164354 ===========================================================================================================
bool cSC4NetworkConstructionCrew::QueryInterface(std::uint32_t iid, void** out) {
    if (iid != kGZIID_cIGZMessageTarget2Crew) return false;
    *out = static_cast<cIGZMessageTarget2*>(this);
    AddRef();
    return true;
}

// ==== 0x00164342 ===========================================================================================================
std::uint32_t cSC4NetworkConstructionCrew::AddRef() { return ++refs_; }

// ==== 0x001655d8 / 0x001655b0 (FinalRelease: Shutdown, then delete) ========================================================
std::uint32_t cSC4NetworkConstructionCrew::Release() {
    if (refs_ < 2) {
        Shutdown();
        delete this;
        return 0;
    }
    return --refs_;
}

// ==== 0x0016430a / 0x00164330 ==============================================================================================
bool cSC4NetworkConstructionCrew::StaticInit() {
    g_staticInited = true;
    return true;
}
bool cSC4NetworkConstructionCrew::StaticShutdown() {
    g_staticInited = false;
    return true;
}

// ==== 0x001657ca ===========================================================================================================
// Durations come in seconds (kept in ms: x FLOAT_00a706f8 = 1000) and speeds in units per second (kept per microsecond:
// / FLOAT_00a708e0 = 1e6, the animation tick's unit).
bool cSC4NetworkConstructionCrew::Init(std::uint32_t penWidthX, float durationSeconds, float speed, std::uint32_t leadModel,
                                       std::uint32_t followModel, std::uint32_t pathType) {
    if (!g_staticInited) StaticInit();
    if (inited_) return true;
    if (g_host) g_host->RegisterCrew(this);
    penWidthX_ = penWidthX;
    duration_ = ToUnsigned(durationSeconds * 1000.0f);
    speed_ = speed / 1000000.0f;
    leadModel_ = leadModel;
    followModel_ = followModel;
    pathType_ = pathType;
    soundSystem_ = g_host && g_host->SoundSystemPresent();
    pending_.clear();
    steps_.clear();
    cells_.clear();
    crews_.clear();
    if (g_host) {
        g_host->AddNotification(this, kCrewMessageOccupantRemoved);
        g_host->AddNotification(this, kCrewMessageReset);
    }
    inited_ = true;
    return true;
}

// ==== 0x001656b0 ===========================================================================================================
bool cSC4NetworkConstructionCrew::Shutdown() {
    if (!inited_) return true;
    inited_ = false;
    if (g_host) {
        g_host->RemoveNotification(this, kCrewMessageOccupantRemoved);
        g_host->RemoveNotification(this, kCrewMessageReset);
    }
    if (ticking_ && g_host) {
        ticking_ = false;
        g_host->RemoveTickable(this, kNetworkCrewTickPriority);
    }
    for (std::size_t i = 0; i < crews_.size(); ++i) StopConstruction(crews_[i], true);
    MarkOccupantsUsable(pending_, true);
    pending_.clear();
    steps_.clear();
    cells_.clear();
    crews_.clear();
    soundSystem_ = false;
    // The manager drops its reference last: this may delete the crew (Release -> FinalRelease).
    if (g_host) g_host->UnregisterCrew(this);
    return true;
}

// ==== 0x0016722a ===========================================================================================================
bool cSC4NetworkConstructionCrew::DoMessage(cIGZMessage2* message) {
    const std::uint32_t type = message->GetType();
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    if (type == kCrewMessageAnimationTick) {
        DoAnimationTick(static_cast<std::uint32_t>(standard->GetData2()));   // the elapsed microseconds
    } else if (type == kCrewMessageOccupantRemoved) {
        DoOccupantRemoved(static_cast<cISC4Occupant*>(standard->GetVoid1()));
    } else if (type == kCrewMessageReset) {
        Shutdown();
    }
    return true;
}

// ==== 0x0016429e ===========================================================================================================
// Opaque in the normal view (view level 1), visible, usable.
void cSC4NetworkConstructionCrew::MarkOccupantUsable(cSC4NetworkOccupant* piece) {
    if (!piece) return;
    if (g_host && g_host->CurrentViewLevel() == 1) piece->ClearNetworkFlag(0x4000);
    AsOccupant(piece)->SetVisibility(true, true);
    piece->SetNetworkFlag(kNetworkFlagUsable);
}

// ==== 0x00164690 ===========================================================================================================
void cSC4NetworkConstructionCrew::MarkOccupantsUsable(std::list<cSC4NetworkOccupant*>& pieces, bool release) {
    for (cSC4NetworkOccupant* piece : pieces) {
        MarkOccupantUsable(piece);
        if (release && piece) piece->Release();
    }
}

// ==== 0x0016437e / 0x001643c4 / 0x0016445c: the crew count on the piece's property holder ==================================
std::uint32_t cSC4NetworkConstructionCrew::GetNetworkOccupantCrewCount(cSC4NetworkOccupant* piece) {
    std::uint32_t count = 0;
    if (cISCPropertyHolder* holder = AsOccupant(piece)->AsPropertyHolder())
        cSCPropertyHelp::GetPropertyValue(holder, kNetworkCrewCountProperty, count);
    return count;
}

std::uint32_t cSC4NetworkConstructionCrew::RemoveCrewFromNetworkOccupant(cSC4NetworkOccupant* piece) {
    std::uint32_t count = 0;
    cISCPropertyHolder* holder = AsOccupant(piece)->AsPropertyHolder();
    if (!holder) return 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kNetworkCrewCountProperty, count))
        cSCPropertyHelp::SetPropertyValue(holder, kNetworkCrewCountProperty, std::uint32_t{0});   // vslot 0x3c: a new property
    if (count == 0) return 0;
    --count;
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkCrewCountProperty, count);
    return count;
}

std::uint32_t cSC4NetworkConstructionCrew::AddCrewToNetworkOccupant(cSC4NetworkOccupant* piece) {
    std::uint32_t count = 0;
    cISCPropertyHolder* holder = AsOccupant(piece)->AsPropertyHolder();
    if (!holder) return 0;
    if (!cSCPropertyHelp::GetPropertyValue(holder, kNetworkCrewCountProperty, count))
        cSCPropertyHelp::SetPropertyValue(holder, kNetworkCrewCountProperty, std::uint32_t{0});
    cSCPropertyHelp::SetPropertyValue(holder, kNetworkCrewCountProperty, count + 1);
    return count;   // the count before the crew was added
}

// ==== 0x00163e54 ===========================================================================================================
// The side of `from` that faces the neighbouring piece `to` (0 -x, 1 -z, 2 +x, 3 +z); 0xffffffff when they are not
// edge neighbours.
std::uint32_t cSC4NetworkConstructionCrew::GetEdge(cSC4NetworkOccupant* from, cSC4NetworkOccupant* to) {
    std::uint32_t ax = 0, az = 0, bx = 0, bz = 0;
    from->GetOccupiedCell(ax, az);
    to->GetOccupiedCell(bx, bz);
    const int dx = static_cast<int>(bx) - static_cast<int>(ax);
    const int dz = static_cast<int>(bz) - static_cast<int>(az);
    if (std::abs(dx) + std::abs(dz) != 1) return 0xffffffffu;
    if (dz < 0) return 1;
    if (dz > 0) return 3;
    return dx < 0 ? 0 : 2;
}

// ==== 0x00163ee4 ===========================================================================================================
// A side to pair with `edge` that has a path on the piece: the opposite side first, then the two neighbours, then the same
// side; car paths (type 1) before the crew's own path type, lanes 0 and 1. `asEntry` makes the found side the entry.
std::uint32_t cSC4NetworkConstructionCrew::SearchForEdge(cSC4NetworkOccupant* piece, std::uint32_t edge, bool asEntry) {
    cISC4PathInfo* paths = piece->GetPathInfo();
    if (!paths) return 0xffffffffu;
    static constexpr int kOffsets[4] = {2, 3, 5, 0};
    const std::uint32_t types[2] = {1, pathType_};
    for (int offset : kOffsets) {
        const std::uint32_t candidate = (edge + static_cast<std::uint32_t>(offset)) & 3u;
        for (std::uint32_t type : types) {
            for (std::uint8_t lane = 0; lane != 2; ++lane) {
                const std::uint8_t entry = static_cast<std::uint8_t>(asEntry ? candidate : edge);
                const std::uint8_t exit = static_cast<std::uint8_t>(asEntry ? edge : candidate);
                if (paths->GetPath(type, entry, exit, lane, nullptr)) return candidate;
            }
        }
    }
    return 0xffffffffu;
}

// ==== 0x00164006 ===========================================================================================================
// The middle of the piece's path from `previous` to `next` (car paths first, then the crew's path type): the centre lane
// when both sides are known, a random path otherwise. Without a path the piece's own position is answered with false.
bool cSC4NetworkConstructionCrew::GetPathMidpoint(cSC4NetworkOccupant* piece, cSC4NetworkOccupant* previous,
                                                  cSC4NetworkOccupant* next, float* out, std::uint32_t& pathKey, bool reverse) {
    if (!piece) return false;
    cISC4PathInfo* paths = piece->GetPathInfo();
    if (paths && pathType_ == 5) paths->CopyPaths(0xff, 1, 5, true);   // vslot 0x7c: the type 5 paths are the car paths
    std::uint32_t entry = previous ? GetEdge(piece, previous) : 0xffu;
    std::uint32_t exit = next ? GetEdge(piece, next) : 0xffu;
    // Quirk kept: "no side" is tested as 0xff although GetEdge and SearchForEdge answer 0xffffffff for it, so a search that
    // finds nothing still counts as found (and asks for the 0xff wildcard side below).
    bool found;
    if (entry == 0xffu) {
        if (exit != 0xffu) {
            entry = SearchForEdge(piece, exit, true);
            found = entry != 0xffu;
        } else {
            found = false;
        }
    } else if (exit == 0xffu) {
        exit = SearchForEdge(piece, entry, false);
        found = exit != 0xffu;
    } else {
        found = true;
    }
    if (reverse) std::swap(entry, exit);
    float position[3] = {0, 0, 0};
    if (!paths) {
        piece->GetPosition(position);   // vslot 0x148 (the original also formats "Network piece with ID %08x has no path info!")
        out[0] = position[0];
        out[1] = position[1];
        out[2] = position[2];
        pathKey = 0;
        return false;
    }
    const std::uint32_t types[2] = {1, pathType_};
    const tPath* path = nullptr;
    for (int i = 0; i < 2; ++i) {
        if (found) pathKey = paths->GetCenterLane(types[i], static_cast<std::uint8_t>(entry), static_cast<std::uint8_t>(exit));
        else paths->GetRandomPathWildcard(types[i], static_cast<std::uint8_t>(entry), static_cast<std::uint8_t>(exit),
                                          g_host ? g_host->Random() : nullptr, &pathKey, nullptr);
        path = paths->GetPath(pathKey, nullptr);
        if (path) break;
    }
    if (!path) {
        piece->GetPosition(position);
        out[0] = position[0];
        out[1] = position[1];
        out[2] = position[2];
        pathKey = 0;
        return false;
    }
    cS3DVector3 middle;
    paths->GetPathMidpoint(&path->points, middle);
    out[0] = middle.x;
    out[1] = middle.y;
    out[2] = middle.z;
    return true;
}

// ==== 0x001644ec ===========================================================================================================
void cSC4NetworkConstructionCrew::DestroyVehicle(tVehicleInfo& info) {
    if (info.vehicle) {
        if (g_host && g_host->VehicleManagerPresent()) {
            info.vehicle->RemoveCallback(this);
            g_host->RemoveVehicle(info.vehicle);
        }
        info.vehicle->Release();
    }
    if (info.piece) info.piece->Release();
    info.piece = nullptr;
    info.vehicle = nullptr;
}

// ==== 0x00164576 ===========================================================================================================
void cSC4NetworkConstructionCrew::SendConstructionMessage(std::uint32_t type, cISC4Occupant* occupant, std::uint32_t data2,
                                                          std::uint32_t data3) {
    if (g_host) g_host->PostConstructionMessage(type, occupant, this, data2, data3);
}

// ==== 0x00164642 ===========================================================================================================
std::list<cSC4NetworkOccupant*>::iterator cSC4NetworkConstructionCrew::FindNetworkOccupantForCell(std::list<cSC4NetworkOccupant*>& pieces,
                                                                                                std::uint32_t x, std::uint32_t z) {
    for (auto it = pieces.begin(); it != pieces.end(); ++it) {
        std::uint32_t px = 0, pz = 0;
        (*it)->GetOccupiedCell(px, pz);
        if (px == x && pz == z) return it;
    }
    return pieces.end();
}

// ==== 0x001646d6 ===========================================================================================================
// The piece after `piece` in the list (with a reference), and whether the two are edge neighbours.
bool cSC4NetworkConstructionCrew::GetNextNetworkOccupant(std::list<cSC4NetworkOccupant*>& pieces, cSC4NetworkOccupant* piece,
                                                         cSC4NetworkOccupant*& next, bool& adjacent) {
    auto it = pieces.begin();
    for (; it != pieces.end(); ++it)
        if (*it == piece) break;
    if (it == pieces.end()) return false;
    auto after = std::next(it);
    if (after == pieces.end()) {
        next = nullptr;
        adjacent = false;
    } else {
        next = *after;
        std::uint32_t ax = 0, az = 0, bx = 0, bz = 0;
        piece->GetOccupiedCell(ax, az);
        next->GetOccupiedCell(bx, bz);
        adjacent = std::abs(static_cast<int>(bx) - static_cast<int>(ax)) + std::abs(static_cast<int>(bz) - static_cast<int>(az)) == 1;
    }
    if (next) next->AddRef();
    return next != nullptr;
}

// ==== 0x0016479c ===========================================================================================================
// One stair step of a diagonal side walk: true once (x, z) is the end cell; otherwise x and z advance alternately.
bool cSC4NetworkConstructionCrew::AdvanceConstructionCrewSearchCell(int& x, int& z, int dx, int dz, bool& stepX, bool& stepZ,
                                                                   std::uint32_t endCell) {
    const SC4PointU& end = cells_[endCell];
    if (static_cast<std::uint32_t>(x) == end.x && static_cast<std::uint32_t>(z) == end.z) return true;
    if (stepX) x += dx;
    else if (stepZ) z += dz;
    stepX = !stepX;
    stepZ = !stepZ;
    return false;
}

// ==== 0x0016480e ===========================================================================================================
void cSC4NetworkConstructionCrew::CreateConstructionVehicle(cSC4NetworkOccupant* piece, tVehicleInfo& info) {
    float position[3] = {0, 0, 0};
    if (piece) AsOccupant(piece)->GetPosition(position);
    const std::uint32_t key[3] = {kVehicleModelType, kVehicleModelGroup, info.modelId};
    if (!g_host || !g_host->VehicleManagerPresent()) return;
    INetworkCrewVehicle* vehicle = g_host->CreateVehicle(kVehicleGroup, position, kVehicleFlags, key, 0x32);
    if (!vehicle) return;
    vehicle->SetAutomataFlag(2, false);
    info.vehicle = vehicle;
    vehicle->AddRef();
    info.onPath = true;
    info.piece = piece;
    if (piece) piece->AddRef();
}

// ==== 0x001648f4 ===========================================================================================================
// The crew leaves its piece (the piece becomes usable). `final` also takes the vehicles away and hands every piece left in
// the crew's list over as usable.
void cSC4NetworkConstructionCrew::StopConstruction(tConstructionCrewData& crew, bool final) {
    if (crew.effect) {
        crew.effect->Stop(0);
        crew.effect->Release();
        crew.effect = nullptr;
    }
    if (crew.current) {
        MarkOccupantUsable(crew.current);
        crew.current->Release();
        crew.current = nullptr;
    }
    for (INetworkCrewWorker* worker : crew.workers) {
        if (g_host) g_host->RemoveWorker(worker);
        worker->Release();
    }
    crew.workers.clear();
    if (final) {
        DestroyVehicle(crew.lead);
        DestroyVehicle(crew.follow);
        MarkOccupantsUsable(crew.pieces, true);
        crew.pieces.clear();
    }
    crew.duration = 0;
    crew.timer.Stop();
}

// ==== 0x00164a4c ===========================================================================================================
void cSC4NetworkConstructionCrew::InitConstructionCrew(tConstructionCrewData& crew) {
    crew.count = 0;
    crew.duration = duration_;
    crew.pieces.clear();
    crew.current = nullptr;
    crew.effect = nullptr;
    crew.lead.follower = false;
    crew.lead.modelId = leadModel_;
    crew.lead.vehicle = nullptr;
    crew.lead.piece = nullptr;
    crew.lead.arrived = true;
    crew.follow.follower = true;
    crew.follow.modelId = followModel_;
    crew.follow.vehicle = nullptr;
    crew.follow.piece = nullptr;
    crew.follow.arrived = true;
}

// ==== 0x00164abe ===========================================================================================================
// "road_construction_dust" on top of the piece (its position, raised to the top of its bounding box).
INetworkCrewEffect* cSC4NetworkConstructionCrew::CreateConstructionParticles(cSC4NetworkOccupant* piece) {
    INetworkCrewEffect* effect = g_host ? g_host->CreateEffect("road_construction_dust") : nullptr;
    if (!effect) return nullptr;
    float position[3] = {0, 0, 0};
    AsOccupant(piece)->GetPosition(position);
    float boxMin[3] = {0, 0, 0}, boxMax[3] = {0, 0, 0};
    AsOccupant(piece)->GetBoundingBox(boxMin, boxMax);
    position[1] = boxMax[1];
    effect->SetSourcePosition(position);
    effect->Start(0);
    return effect;
}

// ==== 0x00164fec ===========================================================================================================
// Four to seven workers on the piece: candidate spots at 0, 1/4, 1/2 and 3/4 of every path of the piece (on the terrain
// of the piece unless the paths carry their own heights), each pushed up to 2.5 units in a random direction; the workers
// take random spots, a random frame of their model and a random heading.
void cSC4NetworkConstructionCrew::CreateConstructionAutomata(tConstructionCrewData& crew) {
    if (!crew.current || !g_host) return;
    cISC4PathInfo* paths = crew.current->GetPathInfo();
    if (!paths) return;
    cRZRandomSuper* random = g_host->Random();
    std::vector<cS3DVector3> spots;
    const PathMap map = *paths->GetPathMap();   // the original iterates a copy of the table
    for (const auto& entry : map) {
        const std::vector<cS3DVector3>& points = entry.second.points;
        const cS3DVector3 first = points[0];
        const cS3DVector3 last = points[points.size() - 1];
        for (float t : kPathDistances) {
            cS3DVector3 point{(last.x - first.x) * t + first.x, (last.y - first.y) * t + first.y, t * (last.z - first.z) + first.z};
            if (!paths->UseElevation()) point.y = crew.current->GetAltitude(point.x, point.z);
            // (1, 0, 0) times cS3DMatrix3::RotateY(angle) (0x0062d1f6): the matrix's first row (cos, 0, sin).
            const double degrees = random ? random->RandomDoubleRangeUniform(0.0, 360.0) : 0.0;
            const float angle = static_cast<float>(static_cast<double>(static_cast<float>(degrees)) * 0.017453292519943295);
            const float dx = std::cos(angle), dz = std::sin(angle);
            const float push = random ? static_cast<float>(random->RandomDoubleRangeUniform(0.0, 2.5)) : 0.0f;
            spots.push_back(cS3DVector3{push * dx + point.x, push * 0.0f + point.y, push * dz + point.z});
        }
    }
    const std::uint32_t extra = random ? random->RandomUint32Uniform(4) : 0;
    for (std::uint32_t i = 0; i < extra + 4; ++i) {
        if (spots.empty()) break;
        const std::uint32_t pick = random ? random->RandomUint32Uniform(static_cast<std::uint32_t>(spots.size())) : 0;
        const cS3DVector3 spot = spots[pick];
        spots.erase(spots.begin() + pick);
        const float position[3] = {spot.x, spot.y, spot.z};
        INetworkCrewWorker* worker = g_host->CreateWorker(kWorkerGroup, position, kWorkerFlags);
        if (!worker) continue;
        worker->SetAutomataFlag(2, false);
        worker->AddRef();
        const int frames = worker->ModelFrameCount();
        if (frames >= 0 && random) worker->SetModelFrame(static_cast<int>(random->RandomUint32Uniform(static_cast<std::uint32_t>(frames))));
        cRZRandom headingRandom(cRZTime::GetMillisecondSystemTime());   // a generator seeded from the current time (cRZTime)
        const cS3DVector2 heading = nS3D::RandomDirection2(headingRandom);
        const float direction[3] = {heading.x, 0.0f, heading.y};
        worker->SetOrientation(direction);
        crew.workers.push_back(worker);
    }
}

// ==== 0x0016590a ===========================================================================================================
// Put the vehicle at the middle of `piece`'s path, heading for the middle of `next` (or standing when there is none); a
// piece without a path is marked usable at once and the vehicle waits there, hidden over water.
void cSC4NetworkConstructionCrew::PlaceVehicle(cSC4NetworkOccupant* piece, cSC4NetworkOccupant* next, tVehicleInfo& info, bool reverse) {
    if (!info.vehicle) {
        CreateConstructionVehicle(piece, info);
        if (!info.vehicle) return;
    }
    if (!g_host || !g_host->VehicleManagerPresent()) return;
    float from[3] = {0, 0, 0}, to[3] = {0, 0, 0};
    std::uint32_t fromKey = 0, toKey = 0;
    bool found = GetPathMidpoint(piece, nullptr, next, from, fromKey, reverse);
    if (!next) {
        to[0] = from[0];
        to[1] = from[1];
        to[2] = from[2];
    } else {
        found = GetPathMidpoint(next, piece, nullptr, to, toKey, reverse);
    }
    if (info.piece) info.piece->Release();
    info.piece = piece;
    if (piece) piece->AddRef();
    if (!found) {
        bool terrain = false;
        const bool water = g_host->CellHasWater(ToUnsigned(from[0] * kCellsPerUnit), ToUnsigned(from[2] * kCellsPerUnit), terrain);
        info.vehicle->SetOpacity(!terrain || water ? 0.0f : 1.0f, 0.0f);
    }
    info.endPoint[0] = to[0];
    info.endPoint[1] = to[1];
    info.endPoint[2] = to[2];
    float direction[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
    const float length = Length(direction);
    if (length <= kOrientationEpsilon) {
        info.vehicle->GetOrientation(direction);
    } else {
        direction[0] /= length;
        direction[1] /= length;
        direction[2] /= length;
    }
    info.vehicle->MoveTo(from, direction);
    if (!found) {
        if (info.piece) MarkOccupantUsable(info.piece);
        info.arrived = true;
        info.onPath = false;
    } else {
        info.vehicle->SetPath(piece, fromKey, true);
        info.vehicle->SetPathOffset(0.0f);
        info.arrived = next == nullptr;
        info.onPath = true;
    }
}

// ==== 0x00165c3e ===========================================================================================================
// Aim the vehicle at the middle of the piece after its current one (placing it there when the pieces do not join or the
// next piece has no path). The follower stops (is taken away) when its next piece is the crew's.
bool cSC4NetworkConstructionCrew::SetVehicleEndPoint(std::list<cSC4NetworkOccupant*>& pieces, cSC4NetworkOccupant* crewPiece,
                                                     tVehicleInfo& info, bool reverse) {
    if (!info.vehicle || !info.piece) return false;
    cSC4NetworkOccupant* next = nullptr;
    bool adjacent = false;
    if (!GetNextNetworkOccupant(pieces, info.piece, next, adjacent)) {
        DestroyVehicle(info);
        return false;
    }
    bool result;
    bool place = true;
    if (!info.follower || next != crewPiece) {
        result = true;
        if (adjacent) {
            float point[3] = {0, 0, 0};
            std::uint32_t key = 0;
            if (!GetPathMidpoint(next, info.piece, nullptr, point, key, reverse)) {
                PlaceVehicle(next, nullptr, info, reverse);
                info.arrived = true;
            } else {
                info.endPoint[0] = point[0];
                info.endPoint[1] = point[1];
                info.endPoint[2] = point[2];
                info.arrived = false;
            }
            place = false;
        }
    } else {
        DestroyVehicle(info);
        result = false;
    }
    if (place) {
        PlaceVehicle(next, nullptr, info, reverse);
        info.arrived = true;
    }
    if (!info.follower) AsOccupant(next)->SetVisibility(true, true);
    next->Release();
    return result;
}

// ==== 0x00165dc8 ===========================================================================================================
// The vehicle reached the end of its piece's path: continue on the next piece's path from the side it came from.
bool cSC4NetworkConstructionCrew::MoveVehicleToNextCell(std::list<cSC4NetworkOccupant*>& pieces, tVehicleInfo& info,
                                                        cSC4NetworkOccupant* crewPiece, bool reverse) {
    if (!info.vehicle || !info.piece) return false;
    cSC4NetworkOccupant* next = nullptr;
    bool adjacent = false;
    if (!GetNextNetworkOccupant(pieces, info.piece, next, adjacent)) {
        DestroyVehicle(info);
        return false;
    }
    bool result;
    bool place = true;
    if (!info.follower || next != crewPiece) {
        result = true;
        if (adjacent) {
            float point[3] = {0, 0, 0};
            std::uint32_t key = 0;
            if (!GetPathMidpoint(next, info.piece, nullptr, point, key, reverse)) {
                PlaceVehicle(next, nullptr, info, reverse);
                info.arrived = true;
            } else {
                info.vehicle->SetPath(next, key, true);
                info.vehicle->SetPathOffset(0.0f);
                if (info.piece) info.piece->Release();
                info.piece = next;
                next->AddRef();
            }
            place = false;
        }
    } else {
        DestroyVehicle(info);
        result = false;
    }
    if (place) PlaceVehicle(next, nullptr, info, reverse);
    next->Release();
    return result;
}

// ==== 0x00165f72 ===========================================================================================================
void cSC4NetworkConstructionCrew::PlaceLeadVehicle(std::list<cSC4NetworkOccupant*>& pieces, cSC4NetworkOccupant* piece,
                                                   tVehicleInfo& info, bool reverse) {
    cSC4NetworkOccupant* next = nullptr;
    bool adjacent = false;
    if (GetNextNetworkOccupant(pieces, piece, next, adjacent) && !adjacent && next) {
        next->Release();
        next = nullptr;
    }
    PlaceVehicle(piece, next, info, reverse);
    info.arrived = next == nullptr;
    if (info.piece) AsOccupant(info.piece)->SetVisibility(true, true);
    if (next) next->Release();
}

// ==== 0x00166046 ===========================================================================================================
void cSC4NetworkConstructionCrew::StartLeadVehicleMoving(tConstructionCrewData& crew) {
    bool place = !crew.lead.vehicle;
    if (!place && crew.current == crew.lead.piece) {
        DestroyVehicle(crew.lead);
        place = !crew.lead.vehicle;
    }
    if (place) PlaceLeadVehicle(crew.pieces, crew.current, crew.lead, crew.reverse);
    else SetVehicleEndPoint(crew.pieces, crew.current, crew.lead, crew.reverse);
}

// ==== 0x001660d0 ===========================================================================================================
// The crew's piece is done: it becomes usable and the crew moves to the next piece of its list, or finishes (count -1).
bool cSC4NetworkConstructionCrew::MoveConstructionCrewToNextCell(tConstructionCrewData& crew) {
    cSC4NetworkOccupant* previous = crew.current;
    if (previous) previous->AddRef();
    StopConstruction(crew, false);
    cSC4NetworkOccupant* next = nullptr;
    bool adjacent = false;
    bool result;
    if (!GetNextNetworkOccupant(crew.pieces, previous, next, adjacent)) {
        const std::uint32_t flags = previous ? previous->GetNetworkFlag() : 0;   // cISC4NetworkOccupant vslot 0x30
        if (soundSystem_) SendConstructionMessage(kNetworkCrewMessageDone, nullptr, 0, flags);
        StopConstruction(crew, true);
        crew.count = -1;
        result = false;
    } else {
        if (crew.current) crew.current->Release();
        crew.current = next;
        crew.effect = CreateConstructionParticles(next);
        CreateConstructionAutomata(crew);
        crew.timer.Restart();
        crew.duration = duration_;
        StartLeadVehicleMoving(crew);
        if (!adjacent) {
            DestroyVehicle(crew.follow);
        } else if (!crew.follow.vehicle) {
            PlaceVehicle(previous, crew.current, crew.follow, crew.reverse);
            crew.follow.arrived = true;
        } else {
            SetVehicleEndPoint(crew.pieces, crew.current, crew.follow, crew.reverse);
        }
        if (soundSystem_) SendConstructionMessage(kNetworkCrewMessageNextPiece, AsOccupant(crew.current), 0, crew.current->GetNetworkFlag());
        result = true;
    }
    if (previous) previous->Release();
    return result;
}

// ==== 0x001662a6 ===========================================================================================================
// A piece was bulldozed: its vehicles go, a crew standing on it moves on, and it leaves the lists.
void cSC4NetworkConstructionCrew::DoOccupantRemoved(cISC4Occupant* occupant) {
    const std::size_t count = crews_.size();
    for (std::size_t i = 0; i < count; ++i) {
        tConstructionCrewData& crew = crews_[i];
        for (auto it = crew.pieces.begin(); it != crew.pieces.end(); ++it) {
            cSC4NetworkOccupant* piece = *it;
            if (AsOccupant(piece) != occupant) continue;
            if (piece == crew.lead.piece) DestroyVehicle(crew.lead);
            if (piece == crew.follow.piece) DestroyVehicle(crew.follow);
            bool remove = crew.count != -1;
            if (piece == crew.current) remove = MoveConstructionCrewToNextCell(crew);
            if (remove) {
                piece->Release();
                crew.pieces.erase(it);
            }
            break;
        }
    }
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
        if (AsOccupant(*it) != occupant) continue;
        (*it)->Release();
        pending_.erase(it);
        return;
    }
}

// ==== 0x001663c4 ===========================================================================================================
void cSC4NetworkConstructionCrew::CreateConstructionCrewParticleEffect(tConstructionCrewData& crew) {
    if (crew.pieces.empty()) return;
    crew.current = crew.pieces.front();
    if (!crew.current) return;
    crew.current->AddRef();
    crew.effect = CreateConstructionParticles(crew.current);
    CreateConstructionAutomata(crew);
    if (soundSystem_) SendConstructionMessage(kNetworkCrewMessageDone, AsOccupant(crew.current), 1, crew.current->GetNetworkFlag());
    PlaceLeadVehicle(crew.pieces, crew.current, crew.lead, crew.reverse);
}

// ==== 0x00164b88 ===========================================================================================================
void cSC4NetworkConstructionCrew::AddNetworkSegment(std::list<cSC4NetworkOccupant*>& pieces, std::vector<tDraggedStep>& steps,
                                                    const std::vector<SC4PointU>& cells) {
    if (!steps.empty() || pieces.empty()) {
        const std::uint32_t base = static_cast<std::uint32_t>(cells_.size());
        const std::size_t stepCount = steps.size();
        // Deviation: the original reads steps[0] again for every step, after the first iteration has rebased it, which runs
        // past the caller's cells from the second segment on; the port keeps the step as it came.
        const tDraggedStep lineStep = steps[0];
        for (std::size_t s = 0; s < stepCount; ++s) {
            tDraggedStep& step = steps[s];
            const std::size_t known = steps_.size();
            const std::uint32_t firstIndex = step.firstCell;
            std::uint32_t lastIndex = step.lastCell;
            const SC4PointU& first = cells[firstIndex];
            const SC4PointU& last = cells[lastIndex];
            // A step the crew already has (same end cells and direction) adds its cells again but not the step.
            bool duplicate = false;
            for (std::size_t k = 0; k < known; ++k) {
                const tDraggedStep& mine = steps_[k];
                const SC4PointU& a = cells_[mine.firstCell];
                const SC4PointU& b = cells_[mine.lastCell];
                if (a == first && b == last && mine.direction == step.direction) {
                    duplicate = true;
                    break;
                }
            }
            // The cells run from the end nearer the line's first step.
            const SC4PointU& lineFirst = cells[lineStep.firstCell];
            const SC4PointU& lineLast = cells[lineStep.lastCell];
            const auto manhattan = [](const SC4PointU& p, const SC4PointU& q) {
                return static_cast<int>(std::fabs(static_cast<float>(p.x) - static_cast<float>(q.x)) +
                                        std::fabs(static_cast<float>(p.z) - static_cast<float>(q.z)));
            };
            int direction;
            std::uint32_t from;
            if (manhattan(last, lineLast) < manhattan(first, lineFirst)) {
                direction = -1;
                from = lastIndex;
                lastIndex = firstIndex;
            } else {
                direction = 1;
                from = firstIndex;
            }
            for (std::uint32_t i = from; i != lastIndex + static_cast<std::uint32_t>(direction); i += static_cast<std::uint32_t>(direction))
                cells_.push_back(cells[i]);
            if (!duplicate) {
                // Quirk kept: the caller's step is rebased in place before it is copied.
                step.firstCell += base;
                step.lastCell += base;
                steps_.push_back(step);
            }
        }
        // The pieces on the crew's cells move to the pending list (a later piece on a cell replaces the earlier one).
        const std::size_t cellCount = cells_.size();
        for (std::size_t c = 0; c < cellCount; ++c) {
            auto found = FindNetworkOccupantForCell(pieces, cells_[c].x, cells_[c].z);
            if (found == pieces.end()) continue;
            auto mine = FindNetworkOccupantForCell(pending_, cells_[c].x, cells_[c].z);
            if (mine == pending_.end()) {
                pending_.push_back(*found);
            } else {
                pending_.insert(mine, *found);
                (*mine)->Release();
                pending_.erase(mine);
            }
            pieces.erase(found);
        }
    }
    MarkOccupantsUsable(pieces, false);
}

// ==== 0x00166488 ===========================================================================================================
// A wide line (pen width X != 1): one crew walks each side of the steps first..last. A straight line takes each step's
// first and last cell; a diagonal one walks the stair steps of both sides from the first step.
int cSC4NetworkConstructionCrew::CreateCrewsForSegment(int firstStep, int lastStep, std::list<cSC4NetworkOccupant*>& pending) {
    tConstructionCrewData a, b;
    bool aStepX = true, aStepZ = false, bStepX = false, bStepZ = true;
    InitConstructionCrew(a);
    InitConstructionCrew(b);
    const SC4PointU& start = cells_[steps_[firstStep].firstCell];
    const SC4PointU& end = cells_[steps_[lastStep].firstCell];
    int dx = static_cast<int>(end.x) - static_cast<int>(start.x);
    int dz = static_cast<int>(end.z) - static_cast<int>(start.z);
    if (dx != 0) dx /= std::abs(dx);
    if (dz != 0) dz /= std::abs(dz);
    if (dx == 0 || dz == 0) {
        for (int s = firstStep; s <= lastStep; ++s) {
            const SC4PointU& left = cells_[steps_[s].firstCell];
            auto found = FindNetworkOccupantForCell(pending, left.x, left.z);
            if (found != pending.end()) {
                AddCrewToNetworkOccupant(*found);
                a.pieces.push_back(*found);
                ++a.count;
            }
            const SC4PointU& right = cells_[steps_[s].lastCell];
            found = FindNetworkOccupantForCell(pending, right.x, right.z);
            if (found != pending.end()) {
                AddCrewToNetworkOccupant(*found);
                b.pieces.push_back(*found);
                ++b.count;
            }
        }
        // Quirk kept: the pending list's references go to the crews' lists as they are.
        pending.clear();
    } else {
        const std::uint32_t from = dx < 0 ? steps_[firstStep].lastCell : steps_[firstStep].firstCell;
        const std::uint32_t endCell = steps_[lastStep].lastCell;
        int ax = static_cast<int>(cells_[from].x), az = static_cast<int>(cells_[from].z);
        int bx = ax, bz = az;
        bool again = true;
        while (again) {
            bool aDone = false, bDone = false;
            again = false;
            for (int s = firstStep; s <= lastStep && !again; ++s) {
                for (std::uint32_t c = steps_[s].firstCell; c <= steps_[s].lastCell && !again; ++c) {
                    const SC4PointU& cell = cells_[c];
                    auto found = FindNetworkOccupantForCell(pending, cell.x, cell.z);
                    if (found == pending.end()) continue;
                    cSC4NetworkOccupant* piece = *found;
                    bool taken = false;
                    if (cell.x == static_cast<std::uint32_t>(ax) && cell.z == static_cast<std::uint32_t>(az)) {
                        piece->AddRef();
                        AddCrewToNetworkOccupant(piece);
                        a.pieces.push_back(piece);
                        ++a.count;
                        aDone = AdvanceConstructionCrewSearchCell(ax, az, dx, dz, aStepX, aStepZ, endCell);
                        taken = true;
                    }
                    if (cell.x == static_cast<std::uint32_t>(bx) && cell.z == static_cast<std::uint32_t>(bz)) {
                        piece->AddRef();
                        AddCrewToNetworkOccupant(piece);
                        b.pieces.push_back(piece);
                        ++b.count;
                        bDone = AdvanceConstructionCrewSearchCell(bx, bz, dx, dz, bStepX, bStepZ, endCell);
                        taken = true;
                    }
                    if (!taken) continue;
                    piece->Release();
                    pending.erase(found);
                    again = true;   // rescan from the first step
                }
            }
            if (!again) {
                if (aDone && bDone) break;
                aDone = AdvanceConstructionCrewSearchCell(ax, az, dx, dz, aStepX, aStepZ, endCell);
                bDone = AdvanceConstructionCrewSearchCell(bx, bz, dx, dz, bStepX, bStepZ, endCell);
            }
            if (aDone || bDone) break;
        }
    }
    a.reverse = false;
    b.reverse = false;
    CreateConstructionCrewParticleEffect(a);
    CreateConstructionCrewParticleEffect(b);
    crews_.push_back(a);
    crews_.push_back(b);
    return lastStep + 1;
}

// ==== 0x00166b66 ===========================================================================================================
// Lines of fewer than five steps are usable at once; longer ones are split over (2n / (n + 1) =) one group of crews.
void cSC4NetworkConstructionCrew::BeginConstruction() {
    const int stepCount = static_cast<int>(steps_.size());
    if (stepCount < 5) {
        MarkOccupantsUsable(pending_, false);
        Shutdown();
        return;
    }
    const int groups = std::max(1, (stepCount + stepCount) / (stepCount + 1));
    const int perGroup = std::max(1, stepCount / groups);
    int remainder = stepCount % groups;
    crews_.clear();
    int step = 0;
    for (int g = 0; g < groups; ++g) {
        int last;
        if (step + stepCount + 1 < stepCount) {
            last = step + perGroup - 1;
            if (remainder != 0) {
                --remainder;
                last = step + perGroup;
            }
        } else {
            last = stepCount - 1;
        }
        tConstructionCrewData crew;
        InitConstructionCrew(crew);
        if (penWidthX_ == 1) {
            for (; step <= last; ++step) crew.count += static_cast<int>(steps_[step].lastCell - steps_[step].firstCell) + 1;
            crew.reverse = false;
            crew.count = std::min(static_cast<int>(pending_.size()), crew.count);
            for (int i = 0; i < crew.count; ++i) {
                cSC4NetworkOccupant* piece = pending_.front();
                pending_.pop_front();
                AddCrewToNetworkOccupant(piece);
                crew.pieces.push_back(piece);
            }
            CreateConstructionCrewParticleEffect(crew);
            crews_.push_back(crew);
        } else {
            CreateCrewsForSegment(step, last, pending_);
        }
        step = last + 1;
    }
    if (!ticking_ && g_host && g_host->AddTickable(this, kNetworkCrewTickPriority)) ticking_ = true;
    MarkOccupantsUsable(pending_, true);
    // Deviation (memory safety): the original keeps the released entries in the list.
    pending_.clear();
    steps_.clear();
    cells_.clear();
    for (tConstructionCrewData& crew : crews_) crew.timer.Restart();
}

// ==== 0x00166e94 ===========================================================================================================
// Drive a vehicle along its path at the crew's speed, on to the next piece's path at the end of one, and stop it within
// 0.6 units of its end point.
void cSC4NetworkConstructionCrew::AnimateVehicle(std::uint32_t microseconds, tVehicleInfo& info, tConstructionCrewData& crew) {
    const float speed = speed_;
    if (!info.vehicle || info.arrived || !g_host) return;
    g_host->VehicleManagerPresent();   // the original fetches the manager and does not test it
    if (!info.vehicle) return;
    if (!info.onPath) {
        MoveVehicleToNextCell(crew.pieces, info, crew.current, crew.reverse);
        if (!info.vehicle) return;
    }
    const float step = static_cast<float>(microseconds) * speed;
    float position[3] = {0, 0, 0};
    info.vehicle->GetPosition(position);
    info.onPath = info.vehicle->InterpolateAlongPath(step, position, position);
    float current[3] = {0, 0, 0};
    info.vehicle->GetPosition(current);
    const float toEnd[3] = {info.endPoint[0] - current[0], info.endPoint[1] - current[1], info.endPoint[2] - current[2]};
    if (Length(toEnd) < step) {
        position[0] = info.endPoint[0];
        position[1] = info.endPoint[1];
        position[2] = info.endPoint[2];
    }
    float direction[3] = {info.endPoint[0] - position[0], info.endPoint[1] - position[1], info.endPoint[2] - position[2]};
    const float distance = Length(direction);
    if (distance <= kOrientationEpsilon) {
        direction[0] = direction[1] = direction[2] = 0.0f;
    } else {
        direction[0] /= distance;
        direction[1] /= distance;
        direction[2] /= distance;
    }
    info.vehicle->MoveTo(position, direction);
    if (distance < kArrivalDistance) info.arrived = true;
}

// ==== 0x001670c6 ===========================================================================================================
void cSC4NetworkConstructionCrew::DoAnimationTick(std::uint32_t microseconds) {
    bool working = false;
    for (std::size_t i = 0; i < crews_.size(); ++i) {
        tConstructionCrewData& crew = crews_[i];
        if (crew.count <= 0 || crew.duration == 0) continue;
        bool moving = false;
        if (crew.lead.vehicle && !crew.lead.arrived) {
            AnimateVehicle(microseconds, crew.lead, crew);
            moving = !crew.lead.arrived;
        }
        if (crew.follow.vehicle && !crew.follow.arrived) {
            AnimateVehicle(microseconds, crew.follow, crew);
            moving = moving || !crew.follow.arrived;
        }
        // A piece takes the crew's duration, twice that while a vehicle is still on its way.
        const std::uint32_t elapsed = crew.timer.GetElapsedTimeU32();
        const bool expired = moving ? elapsed >= crew.duration * 2 : elapsed >= crew.duration;
        if (expired) RemoveCrewFromNetworkOccupant(crew.current);
        if (GetNetworkOccupantCrewCount(crew.current) == 0) {
            working = MoveConstructionCrewToNextCell(crew) || working;
        } else {
            if (crew.lead.arrived && crew.current == crew.lead.piece) StartLeadVehicleMoving(crew);
            working = true;
        }
    }
    if (!working) Shutdown();
}

}  // namespace sc4
