// cSC4WatercraftManager — see WatercraftManager.h for the class notes, the original layout and the quirks.
#include "sc4/WatercraftManager.h"
#include "platform/Unwired.h"

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "sc4/SCPropertyHelp.h"
#include "sc4/SpiralIterator.h"

namespace sc4 {
namespace {

float VectorLength(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The largest of a bounding box's three extents (the original's maxss chain over |max - min|).
float MaxBoxExtent(const float* minXyz, const float* maxXyz) {
    float extent = std::fabs(maxXyz[0] - minXyz[0]);
    const float y = std::fabs(maxXyz[1] - minXyz[1]);
    if (extent < y) extent = y;
    const float z = std::fabs(maxXyz[2] - minXyz[2]);
    if (extent < z) extent = z;
    return extent;
}

// The three dock orientations (@ 0x100a23540, GetPropPositionAndOrientation::kPossibleOrientations).
const float kPossibleOrientations[3][3] = {
    {0.0f, 0.0f, 1.0f},
    {-1.0f, 0.0f, 0.0f},
    {0.0f, 0.0f, -1.0f},
};

// The push the manager uses to fade a boat out: state 0x49, no controller, timer -1 (original call shape:
// PushState(state = 0x49, controller = nullptr, timer = -1, flag = false, animOffset = 0, sfx = 0, behavior = null)).
void FadeOutBoat(cSC4Watercraft* boat) {
    if (boat) boat->PushState(kWatercraftStateRemoveFade, nullptr, -1.0f, false, 0, 0, nullptr);
}

// The ten marina/seaport occupant properties, probed in the original's order.
bool HasMarinaBuildingProperty(cISCPropertyHolder* holder) {
    if (!holder) return false;
    for (std::uint32_t id : kMarinaBuildingProperties)
        if (holder->HasProperty(id)) return true;
    return false;
}

template <class T>
void ReleaseCounted(T*& pointer) {
    if (pointer) {
        pointer->Release();
        pointer = nullptr;
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// COM / identity
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004ef596. The inherited views come from the parent (the original's four vtables); the class id is answered
// with the primary view. Both paths add a reference.
bool cSC4WatercraftManager::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZCLSID_cSC4WatercraftManager) {
        *out = this;
        AddRef();
        return true;
    }
    if (base_ && base_->QueryInterface(iid, out)) {
        base_->AddRef();
        return true;
    }
    return false;
}

// @ 0x1001045f4 (the inherited Release): the last reference shuts the manager down and deletes it.
std::uint32_t cSC4WatercraftManager::Release() {
    if (refCount_ > 1) return --refCount_;
    Shutdown();
    delete this;
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004eddb6.
bool cSC4WatercraftManager::Init() {
    if (!base_ || base_->Initialized()) return true;
    base_->Init();
    if (host_) host_->AddMessageNotification(this, kMessageWatercraftRoutesChanged);
    // The original stores the manager's own random (base +0x78) into SL::mpRandom here; the port asks the parent.
    if (base_->PostCityInitPending()) base_->DestroyAutomata();
    base_->SetMaxCount(kWatercraftMaxCount);
    HandleTuningParametersChanged();
    return true;
}

// @ 0x1004ede32. Every registry is cleared (the counted values release their objects).
bool cSC4WatercraftManager::Shutdown() {
    if (!base_ || !base_->Initialized()) return true;
    if (host_) host_->RemoveMessageNotification(this, kMessageWatercraftRoutesChanged);
    terminals_.clear();
    marinaOccupants_.clear();
    marinaLots_.clear();
    boats_.clear();
    ambientBoats_.clear();
    base_->Shutdown();
    return true;
}

// @ 0x1004edee6. The watercraft-specific occupant notifications are handled here and the parent still sees the
// message (its own occupant handlers maintain the generic automata registries); the result is the OR of both.
bool cSC4WatercraftManager::DoMessage(cIGZMessage2* message) {
    const bool baseResult = base_ ? base_->DoMessage(message) : false;
    if (!message) return baseResult;
    const std::uint32_t type = message->GetType();
    bool handled = false;
    if (type == kMessageWatercraftRoutesChanged) {
        routesChanged_ = true;
        handled = true;
    } else if (type == kMessageOccupantRemoved || type == kMessageOccupantInserted) {
        handled = true;
        void* raw = nullptr;
        if (message->QueryInterface(kGZIID_cIGZMessage2Standard, &raw) && raw) {
            auto* standard = static_cast<cIGZMessage2Standard*>(raw);
            auto* occupant = static_cast<cISC4Occupant*>(standard->GetVoid1());
            standard->Release();
            // The original dereferences the removed occupant without a null check; the port guards it.
            if (occupant) {
                cISCPropertyHolder* holder = occupant->AsPropertyHolder();
                if (!holder || !holder->HasProperty(kPropertyNoAutomata)) {
                    if (type == kMessageOccupantInserted) HandleOccupantInserted(occupant);
                    else HandleOccupantRemoved(occupant);
                }
            }
        }
    }
    return baseResult || handled;
}

// @ 0x1004ef4c2. Sets the maximum count from the inherited tuning slot (the original loads the raw 32 bits of
// cSC4AutomataManager::sTuningParams+0x28 and passes them to SetMaxCount), then reads the per-kind minimum zoom from
// the automata tuning exemplar's property 0x4998c91b and stores it minus one.
bool cSC4WatercraftManager::HandleTuningParametersChanged() {
    if (!base_) return true;
    base_->SetMaxCount(std::bit_cast<std::int32_t>(base_->TuningMaxCountField()));
    cISCResExemplar* exemplar = host_ ? host_->GetAutomataTuningExemplar() : nullptr;
    if (exemplar) {
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        if (holder) {
            std::uint8_t minZoom = 1;
            cSCPropertyHelp::GetPropertyValue(holder, 0x4998c91b, minZoom);
            base_->SetMinZoom(static_cast<int>(minZoom) - 1);
        }
        exemplar->Release();
    }
    return true;
}

// @ 0x1004f29a2. PARTIAL: only the paused shortcut, the ambient spawner call and the parent tick are recovered here.
// The rest of the original is the 60 s docking-rights sweep, the marina-boat sweep (CreateMarinaBoat over the +0xb60
// registry), and the ferry rebuild when message 0x2c06ba82 arrived (SL::spCity vslot 0x228 -> vslot 0xf8 returns the
// traffic simulator's ferry routes -> CreateFerry); those need cISC4TrafficSimulator::GetFerryRoutesInUse, which is
// unrecovered.
void cSC4WatercraftManager::DoAnimationTick(std::uint32_t data, float milliseconds) {
    if (!base_) return;
    base_->CalculateSimSpeedModifier();
    if (base_->SimSpeedPaused()) {
        base_->DoAnimationTick(data, milliseconds);
        return;
    }
    PopulateVisibleArea(milliseconds);
    SC4_UNWIRED("cSC4WatercraftManager::DoAnimationTick: docking-rights sweep, marina-boat sweep and ferry rebuild (message 0x2c06ba82) not ported");
    base_->DoAnimationTick(data, milliseconds);
}

// ---------------------------------------------------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004ef220. A null automaton means "make one": the original allocates a cSC4Watercraft (0xf88 bytes) with its
// default constructor and adds it. The base AddAutomaton's work is replicated here (init, hold a reference, place it).
bool cSC4WatercraftManager::AddAutomaton(cSC4Watercraft* automaton) {
    if (!automaton) automaton = new cSC4Watercraft();
    if (!automaton) return false;
    // The base's AddAutomaton adds the reference before Init: Init posts messages that hold (and release) a reference
    // to the occupant, so the automaton must be alive while it runs.
    automaton->AddRef();
    automaton->Init();
    boats_.push_front(cRZAutoRefCount<cSC4Watercraft>(automaton));
    automaton->Release();
    automaton->PlaceInOccupantManager(true);
    automaton->PlaceInRenderer(true);
    automaton->SetVisible(true);
    return true;
}

// @ 0x1004ef274. The watercraft registries are cleared of the automaton before the base removal. The base's own
// removal (the +0x58 list, the tracked groups and the retire path) has no port equivalent for cSC4Watercraft, so the
// list side is replicated here with the base semantics.
bool cSC4WatercraftManager::RemoveAutomaton(cSC4Watercraft* automaton, bool force) {
    if (!automaton) return false;
    // Keep the automaton alive while the registries drop their own references (the original always has a caller
    // reference here; the port's list may be the last owner).
    automaton->AddRef();
    // The original reads the group template back through GetAutomataGroup() and its +0x20 id; the port uses the id
    // the automaton cached when SetAutomataGroup ran (the same value).
    const std::uint32_t groupId = automaton->AutomataGroupId();
    if (groupId == kWatercraftManagerGroupFerryA || groupId == kWatercraftManagerGroupFerryB) {
        // A ferry: clear the route slot that points at it (the original only ever clears one).
        bool cleared = false;
        for (auto& entry : terminals_) {
            for (FerryRouteInformation& route : entry.second.routes) {
                if (route.boat.get() == automaton) {
                    route.boat = nullptr;
                    cleared = true;
                    break;
                }
            }
            if (cleared) break;
        }
    } else {
        cISC4Occupant* occupant = automaton->AsOccupant();
        const bool marinaBoat = occupant && (occupant->IsOccupantGroup(kWatercraftManagerGroupMarinaBoat) ||
                                             occupant->IsOccupantGroup(kWatercraftGroupTravelBoat3) ||
                                             occupant->IsOccupantGroup(kWatercraftGroupTravelBoat2) ||
                                             occupant->IsOccupantGroup(kWatercraftGroupTravelBoat1));
        if (marinaBoat) {
            for (auto& entry : marinaOccupants_) {
                if (entry.second.get() == automaton) {
                    entry.second = nullptr;
                    break;
                }
            }
        }
        if (occupant && occupant->IsOccupantGroup(kWatercraftManagerGroupAmbientBoat)) {
            for (auto it = boats_.begin(); it != boats_.end(); ++it) {
                if (it->get() == automaton) {
                    boats_.erase(it);   // releases the list's reference
                    break;
                }
            }
            for (auto it = ambientBoats_.begin(); it != ambientBoats_.end(); ++it) {
                if (it->get() == automaton) {
                    ambientBoats_.erase(it);
                    break;
                }
            }
        }
    }
    // The base RemoveAutomaton: force removes (event 3, marker, shutdown, release); otherwise it retires the automaton
    // (event 2, marker, opacity 0, effects cleared). cSC4Automaton has no StopEffectsAndClearModel in the port, so the
    // retire path hides the boat instead (hook-up item).
    if (force) {
        automaton->SendCallbackMessage(3, 0);
        automaton->SetMarkedForRemove(true);
        for (auto it = boats_.begin(); it != boats_.end(); ++it) {
            if (it->get() == automaton) {
                boats_.erase(it);
                break;
            }
        }
        automaton->Shutdown();
    } else {
        automaton->SendCallbackMessage(2, 0);
        automaton->SetMarkedForRemove(true);
        automaton->SetOpacity(0.0f, base_ ? base_->RetireOpacityDuration() : 0.0f);
        automaton->SetVisible(false);
    }
    automaton->Release();
    return true;
}

// @ 0x1004f2054. Refuses to create a boat on top of another one and outside the city, then hands over to the parent's
// creation path. The port seams that call (CreateWatercraft) because the recovered cSC4Watercraft is not an
// IAutomataAutomaton, which is what the recovered base CreateAutomaton returns; like the base, the seam registers the
// boat and returns a borrowed pointer.
cSC4Watercraft* cSC4WatercraftManager::CreateAutomaton(std::uint32_t group, const cS3DVector3& position,
                                                       std::uint32_t flags, const cGZPersistResourceKey& key,
                                                       cIGZMessageTarget2* listener, std::uint8_t argument,
                                                       const cS3DVector3* velocity) {
    if (LookForBoatsAtPosition(position, nullptr)) return nullptr;
    if (!host_) return nullptr;
    int cellX = 0, cellZ = 0;
    host_->WorldToCell(position.x, position.z, cellX, cellZ);
    if (!host_->CellInCity(cellX, cellZ)) return nullptr;
    return host_->CreateWatercraft(group, position, flags, key, listener, argument, velocity);
}

// @ 0x1004ef14e. The first boat within twice its radius of the position (excluding the caller's boat).
cSC4Watercraft* cSC4WatercraftManager::LookForBoatsAtPosition(const cS3DVector3& position, cSC4Watercraft* exclude) {
    for (const cRZAutoRefCount<cSC4Watercraft>& ref : boats_) {
        cSC4Watercraft* boat = ref.get();
        if (!boat || boat == exclude) continue;
        float xyz[3] = {0.0f, 0.0f, 0.0f};
        boat->GetPosition(xyz);
        const float dx = position.x - xyz[0];
        const float dy = position.y - xyz[1];
        const float dz = position.z - xyz[2];
        const float radius = boat->GetRadius();
        if (std::sqrt(dx * dx + dy * dy + dz * dz) < radius + radius) return boat;
    }
    return nullptr;
}

// @ 0x1004f1f2e. True when the cell is water and no boat overlaps its centre. The original also converts the cell to
// world space and samples the terrain height before the conversion (the arguments of that height query are implicit);
// the port samples the height at the cell centre, which is what the comparison needs.
bool cSC4WatercraftManager::ClearToLaunchBoat(int cellX, int cellZ, int unknown) {
    (void)unknown;
    if (!host_ || !host_->IsWaterCell(cellX, cellZ)) return false;
    float worldX = 0.0f, worldZ = 0.0f;
    host_->CellToWorld(cellX, cellZ, worldX, worldZ);
    const float worldY = host_->TerrainHeight(worldX, worldZ);
    for (const cRZAutoRefCount<cSC4Watercraft>& ref : boats_) {
        cSC4Watercraft* boat = ref.get();
        if (!boat) continue;
        float xyz[3] = {0.0f, 0.0f, 0.0f};
        boat->GetPosition(xyz);
        const cS3DVector3 delta{xyz[0] - worldX, xyz[1] - worldY, xyz[2] - worldZ};
        if (VectorLength(delta) < boat->GetRadius()) return false;
    }
    return true;
}

// @ 0x1004f3802. Ferries have priority 0, the two larger boat kinds 1 and 2, everything else 3.
int cSC4WatercraftManager::BoatPriority(cSC4Watercraft* boat) {
    if (!boat) return 3;
    // The cached group id (see RemoveAutomaton).
    const std::uint32_t groupId = boat->AutomataGroupId();
    if (groupId == kWatercraftManagerGroupFerryA || groupId == kWatercraftManagerGroupFerryB) return 0;
    cISC4Occupant* occupant = boat->AsOccupant();
    if (occupant && occupant->IsOccupantGroup(kWatercraftGroupLargeBoat)) return 1;
    if (occupant && occupant->IsOccupantGroup(kWatercraftGroupMediumBoat)) return 2;
    return 3;
}

// @ 0x1004f3f88. The first of the lot's four corners (queried as (z, x), the original's argument order) that lies in a
// body of water; 0 when the lot is not on water. Quirk kept: the corner queries pass the bounding rect's z first.
std::int16_t cSC4WatercraftManager::GetLotBodyOfWater(cSC4Lot* lot) {
    if (!lot || !host_) return 0;
    IWatercraftBodyOfWater* water = host_->BodyOfWater();
    if (!water) return 0;
    std::int32_t rect[4] = {0, 0, 0, 0};
    lot->GetBoundingRect(rect);
    const std::int32_t x0 = rect[0], z0 = rect[1], x1 = rect[2], z1 = rect[3];
    std::int16_t body = water->BodyOfWaterAt(z0, x0);
    if (body != 0) return body;
    body = water->BodyOfWaterAt(z0, x1);
    if (body != 0) return body;
    body = water->BodyOfWaterAt(z1, x0);
    if (body != 0) return body;
    return water->BodyOfWaterAt(z1, x1);
}

// @ 0x1004f0e16. The occupant's position (height replaced by the terrain height) and the direction its prop facing
// points at. The original queries the occupant for cISC4PropOccupant (0xe9793a65), uses vslot 0x48 for the facing and
// the occupant's bounding box to measure how far the dock sits; `offsetVariant` selects the original's second formula.
bool cSC4WatercraftManager::GetPropPositionAndOrientation(cISC4Occupant* occupant, cS3DVector3& position,
                                                          cS3DVector3& direction, bool offsetVariant) {
    if (!occupant) return false;
    occupant->GetPosition(&position.x);
    if (host_) position.y = host_->TerrainHeight(position.x, position.z);
    int facing = 0;
    if (!host_ || !host_->PropOccupantFacing(occupant, facing)) return false;
    // The original indexes the three-entry table with the raw facing value; the port clamps it.
    if (facing < 0 || facing > 2) facing = 0;
    const float ox = kPossibleOrientations[facing][0];
    const float oy = kPossibleOrientations[facing][1];
    const float oz = kPossibleOrientations[facing][2];
    // The dominant axis chooses which bounding-box extent measures the dock offset.
    float minXyz[3] = {0.0f, 0.0f, 0.0f};
    float maxXyz[3] = {0.0f, 0.0f, 0.0f};
    occupant->GetBoundingBox(minXyz, maxXyz);
    const float delta = std::fabs(ox) <= std::fabs(oz) ? minXyz[2] - maxXyz[2] : minXyz[0] - maxXyz[0];
    const float length = std::sqrt(ox * ox + oy * oy + oz * oz);
    if (length <= 0.0f) return false;
    const float inverse = 1.0f / length;
    direction.x = ox * inverse;
    direction.y = oy * inverse;
    direction.z = oz * inverse;
    if (!offsetVariant) {
        const float scale = delta * 90.0f;   // @ 0x100a158c8
        position.x += direction.x * scale;
        position.y += direction.y * scale;
        position.z += direction.z * scale;
    } else {
        // The original's second branch only moves x (by dir.z / |dir_xz| * delta / 3); its y and z additions are
        // -0.0f and +-0.0f, i.e. no-ops. Kept as it is.
        const float lengthXZ = std::sqrt(direction.x * direction.x + direction.z * direction.z);
        if (lengthXZ > 0.0f) position.x += direction.z / lengthXZ * (delta / 3.0f);
    }
    return true;
}

// @ 0x1004f4908. Builds the caller's oriented bounding box, then walks the boats: a boat within the larger of the two
// bounding-box extents (and not the caller) is tested OBB against OBB; the first hit is returned. The other box is
// built with scale 1 and a zero position, exactly like the original's constant.
cSC4Watercraft* cSC4WatercraftManager::CollisionDetection(cSC4Watercraft* self, const cS3DVector3& position,
                                                          float scale) {
    if (!self) return nullptr;
    AutomataOBB selfBox(*self, position, scale);
    for (const cRZAutoRefCount<cSC4Watercraft>& ref : boats_) {
        cSC4Watercraft* other = ref.get();
        if (!other || other == self) continue;
        const cS3DVector3& selfPosition = self->SimulatedPosition();
        const cS3DVector3& otherPosition = other->SimulatedPosition();
        const float distance = std::sqrt((selfPosition.x - otherPosition.x) * (selfPosition.x - otherPosition.x) +
                                         (selfPosition.y - otherPosition.y) * (selfPosition.y - otherPosition.y) +
                                         (selfPosition.z - otherPosition.z) * (selfPosition.z - otherPosition.z));
        float minXyz[3] = {0.0f, 0.0f, 0.0f};
        float maxXyz[3] = {0.0f, 0.0f, 0.0f};
        self->GetBoundingBox(minXyz, maxXyz);
        const float selfExtent = MaxBoxExtent(minXyz, maxXyz);
        other->GetBoundingBox(minXyz, maxXyz);
        const float otherExtent = MaxBoxExtent(minXyz, maxXyz);
        cSC4Watercraft* chosen = selfExtent > otherExtent ? self : other;
        chosen->GetBoundingBox(minXyz, maxXyz);
        if (MaxBoxExtent(minXyz, maxXyz) > distance) {
            const cS3DVector3 origin{0.0f, 0.0f, 0.0f};
            AutomataOBB otherBox(*other, origin, 1.0f);
            if (selfBox.TestForCollision(otherBox)) return other;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Docking state machine
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004f4690. One docking slot per terminal lot. Quirk: a null boat still reports success when the slot is free.
bool cSC4WatercraftManager::RequestPermissionToDock(cSC4Lot* lot, cSC4Watercraft* boat) {
    if (!lot) return false;
    auto it = terminals_.find(cRZAutoRefCount<cSC4Lot>(lot));
    if (it == terminals_.end() || it->second.dockBoat) return false;
    if (boat) it->second.dockBoat = boat;
    return true;
}

// @ 0x1004f472e. One queue slot per terminal lot (same null-boat quirk).
bool cSC4WatercraftManager::RequestPermissionToWait(cSC4Lot* lot, cSC4Watercraft* boat) {
    if (!lot) return false;
    auto it = terminals_.find(cRZAutoRefCount<cSC4Lot>(lot));
    if (it == terminals_.end() || it->second.waitBoat) return false;
    if (boat) it->second.waitBoat = boat;
    return true;
}

// @ 0x1004f47cc. Only the boat that holds the slot can give it back.
void cSC4WatercraftManager::DoneDocking(cSC4Lot* lot, cSC4Watercraft* boat) {
    if (!lot) return;
    auto it = terminals_.find(cRZAutoRefCount<cSC4Lot>(lot));
    if (it == terminals_.end()) return;
    if (it->second.dockBoat.get() == boat && it->second.dockBoat) it->second.dockBoat = nullptr;
}

// @ 0x1004f4864.
void cSC4WatercraftManager::DoneQueueing(cSC4Lot* lot, cSC4Watercraft* boat) {
    if (!lot) return;
    auto it = terminals_.find(cRZAutoRefCount<cSC4Lot>(lot));
    if (it == terminals_.end()) return;
    if (it->second.waitBoat.get() == boat && it->second.waitBoat) it->second.waitBoat = nullptr;
}

// The counts the boats read from the base layout's +0xb28 / +0xb78 (the terminal map and the marina-occupant map sizes).
void cSC4WatercraftManager::FleetCounts(std::int32_t& docks, std::int32_t& queues) const {
    docks = TerminalCount();
    queues = MarinaOccupantCount();
}

// ---------------------------------------------------------------------------------------------------------------------
// Occupant notifications
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004edfb4. Three independent blocks, run in this order: a ferry terminal (0xec39dd14), a marina/seaport building
// (the ten properties) and a marina terminal lot (0x8c392d39). A ferry terminal is only added when the prop position
// query succeeds and the lot is not known yet; the marina lot registry ignores the query result.
void cSC4WatercraftManager::HandleOccupantInserted(cISC4Occupant* occupant) {
    if (!occupant) return;
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (holder && holder->HasProperty(kPropertyWatercraftFerryTerminal)) {
        float xyz[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(xyz);
        cSC4Lot* lot = host_ ? host_->LotAtPosition(xyz) : nullptr;
        if (lot) {
            if (terminals_.find(cRZAutoRefCount<cSC4Lot>(lot)) == terminals_.end()) {
                cS3DVector3 position{};
                cS3DVector3 direction{};
                if (GetPropPositionAndOrientation(occupant, position, direction, false)) {
                    FerryTerminalInformation info;
                    info.occupant = occupant;
                    terminals_.emplace(cRZAutoRefCount<cSC4Lot>(lot), std::move(info));
                }
            }
            lot->Release();
        }
    }
    if (HasMarinaBuildingProperty(holder)) {
        if (marinaOccupants_.find(cRZAutoRefCount<cISC4Occupant>(occupant)) == marinaOccupants_.end())
            marinaOccupants_.emplace(cRZAutoRefCount<cISC4Occupant>(occupant), nullptr);
    }
    if (holder && holder->HasProperty(kPropertyWatercraftMarinaLot)) {
        cS3DVector3 position{};
        cS3DVector3 direction{};
        GetPropPositionAndOrientation(occupant, position, direction, false);
        float xyz[3] = {0.0f, 0.0f, 0.0f};
        occupant->GetPosition(xyz);
        cSC4Lot* lot = host_ ? host_->LotAtPosition(xyz) : nullptr;
        if (lot) {
            if (marinaLots_.find(cRZAutoRefCount<cSC4Lot>(lot)) == marinaLots_.end())
                marinaLots_.emplace(cRZAutoRefCount<cSC4Lot>(lot), cRZAutoRefCount<cISC4Occupant>(occupant));
            lot->Release();
        }
    }
}

// @ 0x1004eee5e. The ferry-terminal registry is matched on the terminal occupant; every ferry boat of the entry is
// faded out (state 0x49) before the routes are cleared and the entry erased. The marina registries are matched by value.
void cSC4WatercraftManager::HandleOccupantRemoved(cISC4Occupant* occupant) {
    if (!occupant) return;
    for (auto it = terminals_.begin(); it != terminals_.end(); ++it) {
        if (it->second.occupant.get() != occupant) continue;
        for (FerryRouteInformation& route : it->second.routes) FadeOutBoat(route.boat.get());
        it->second.routes.clear();
        terminals_.erase(it);
        return;
    }
    cISCPropertyHolder* holder = occupant->AsPropertyHolder();
    if (HasMarinaBuildingProperty(holder)) {
        auto it = marinaOccupants_.find(cRZAutoRefCount<cISC4Occupant>(occupant));
        if (it != marinaOccupants_.end()) {
            FadeOutBoat(it->second.get());
            marinaOccupants_.erase(it);
        }
    }
    if (holder && holder->HasProperty(kPropertyWatercraftMarinaLot)) {
        for (auto it = marinaLots_.begin(); it != marinaLots_.end(); ++it) {
            if (it->second.get() != occupant) continue;
            marinaLots_.erase(it);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Ambient spawner
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004f3d66. The four city edges around the visible rectangle, one edge per call (the index advances modulo 4). The
// centre world position the original computes here is never used (kept as a quirk note, the port skips the call).
void cSC4WatercraftManager::GetEdgeCells(int& index, std::vector<std::pair<int, int>>& out) {
    index %= 4;
    if (!host_) return;
    int rect[4] = {0, 0, 0, 0};
    host_->VisibleCellRect(rect);
    const int x0 = rect[0], z0 = rect[1], x1 = rect[2], z1 = rect[3];
    switch (index) {
        case 0:
            for (int x = x0; x < x1; ++x) out.emplace_back(x, z0);
            break;
        case 1:
            for (int z = z0; z < z1; ++z) out.emplace_back(x1, z);
            break;
        case 2:
            for (int x = x0; x < x1; ++x) out.emplace_back(x, z1);
            break;
        case 3:
            for (int z = z0; z < z1; ++z) out.emplace_back(x0, z);
            break;
        default:
            break;
    }
}

// @ 0x1004f3900. Spawns at most one ambient boat every 10 s while fewer than six ambient boats exist and the marina
// terminal registry is not empty. The random gate depends on the player-drive mode (10 % normally, 5 % while driving);
// while driving, a second roll picks the towable group (0x4417) instead of the ambient one (0x4418).
void cSC4WatercraftManager::PopulateVisibleArea(float milliseconds) {
    static float timeSinceLastLaunch = 10001.0f;
    if (timeSinceLastLaunch < 10000.0f) {
        timeSinceLastLaunch += milliseconds;
        return;
    }
    if (AmbientBoatCount() > 5) return;
    if (marinaLots_.empty()) return;
    if (!host_ || !base_) return;
    bool towable = false;
    if (!base_->IsDrivingMode()) {
        if (base_->Random().RandomDoubleUniform() > 0.1) {
            (void)milliseconds;
            return;
        }
    } else {
        if (base_->Random().RandomDoubleUniform() > 0.05) {
            (void)milliseconds;
            return;
        }
        if (base_->Random().RandomDoubleUniform() >= 0.5) towable = true;
    }
    int rect[4] = {0, 0, 0, 0};
    host_->VisibleCellRect(rect);
    const int centerX = static_cast<int>((static_cast<float>(rect[2]) - static_cast<float>(rect[0])) * 0.5f +
                                         static_cast<float>(rect[0]));
    const int centerZ = static_cast<int>((static_cast<float>(rect[3]) - static_cast<float>(rect[1])) * 0.5f +
                                         static_cast<float>(rect[1]));
    float centerWorldX = 0.0f, centerWorldZ = 0.0f;
    host_->CellToWorld(centerX, centerZ, centerWorldX, centerWorldZ);
    const cS3DVector3 center{centerWorldX, host_->TerrainHeight(centerWorldX, centerWorldZ), centerWorldZ};
    static int lastEdgeChecked = 0;
    std::vector<std::pair<int, int>> cells;
    GetEdgeCells(lastEdgeChecked, cells);
    ++lastEdgeChecked;
    for (const std::pair<int, int>& cell : cells) {
        if (!host_->CellInCity(cell.first, cell.second)) continue;
        if (!ClearToLaunchBoat(cell.first, cell.second, 0)) continue;
        float cellX = 0.0f, cellZ = 0.0f;
        host_->CellToWorld(cell.first, cell.second, cellX, cellZ);
        const cS3DVector3 position{cellX, host_->TerrainHeight(cellX, cellZ), cellZ};
        const std::uint32_t group = towable ? kWatercraftGroupTowable : kWatercraftManagerGroupAmbientBoat;
        const cGZPersistResourceKey key{};
        cSC4Watercraft* boat = CreateAutomaton(group, position, kWatercraftCreateFlags, key, nullptr, 0, nullptr);
        if (!boat) continue;
        timeSinceLastLaunch = 0.0f;
        const cS3DVector3 delta{center.x - position.x, center.y - position.y, center.z - position.z};
        const float inverse = 1.0f / VectorLength(delta);
        const cS3DVector3 direction{delta.x * inverse, delta.y * inverse, delta.z * inverse};
        boat->MoveTo(&position.x, &direction.x);
        boat->SetTowWaitThreshold(kWatercraftTowWaitThreshold);
        boat->SetAnimationHold(towable);
        ambientBoats_.push_back(cRZAutoRefCount<cSC4Watercraft>(boat));
        break;
    }
}

// @ 0x1004f4024. The creation point for a watercraft of `group` near `center`: the terminal's own dock when the centre
// lot is a known ferry terminal/marina lot, otherwise a spiral search over the city for a water cell in the same body
// of water. Quirk kept: the final "inside the city" test passes the *cell indices* as floats to PointInCity.
bool cSC4WatercraftManager::GetRandomCreationPoint(std::uint32_t group, const cS3DVector3& center, float minDistance,
                                                   float maxDistance, cS3DVector3& outPosition, std::uint32_t flags,
                                                   cS3DVector3* outDirection) {
    (void)maxDistance;
    (void)flags;
    if (!host_ || !base_ || !base_->CanCreateGroup(group)) return false;
    IWatercraftBodyOfWater* water = host_->BodyOfWater();
    cSC4Lot* lot = host_->LotAtPosition(&center.x);
    std::int16_t desiredBody = 0;
    if (lot) {
        cISC4Occupant* terminalOccupant = nullptr;
        auto it = terminals_.find(cRZAutoRefCount<cSC4Lot>(lot));
        if (it != terminals_.end()) {
            terminalOccupant = it->second.occupant.get();
        } else {
            auto marina = marinaLots_.find(cRZAutoRefCount<cSC4Lot>(lot));
            if (marina != marinaLots_.end()) terminalOccupant = marina->second.get();
        }
        if (terminalOccupant) {
            cS3DVector3 position{};
            cS3DVector3 direction{};
            GetPropPositionAndOrientation(terminalOccupant, position, direction, false);
            const float cellSize = host_->CellSize();
            position.x += direction.x * 2.0f * cellSize;
            position.y += direction.y * 2.0f * cellSize;
            position.z += direction.z * 2.0f * cellSize;
            desiredBody = GetLotBodyOfWater(lot);
            int cellX = 0, cellZ = 0;
            host_->WorldToCell(position.x, position.z, cellX, cellZ);
            if (ClearToLaunchBoat(cellX, cellZ, 0)) {
                float worldX = 0.0f, worldZ = 0.0f;
                host_->CellToWorld(cellX, cellZ, worldX, worldZ);
                outPosition.x = worldX;
                outPosition.y = host_->TerrainHeight(worldX, worldZ);
                outPosition.z = worldZ;
                if (outDirection) {
                    outDirection->x = direction.x * 2.0f;
                    outDirection->y = direction.y * 2.0f;
                    outDirection->z = direction.z * 2.0f;
                }
                lot->Release();
                return true;
            }
        }
        lot->Release();
    }
    int centerCellX = 0, centerCellZ = 0;
    host_->WorldToCell(center.x, center.z, centerCellX, centerCellZ);
    int sizeX = 0, sizeZ = 0;
    host_->CityCellCount(sizeX, sizeZ);
    // The original's hand-built iterator spans [0, size] (one past the last cell), not [0, size-1].
    SpiralIterator spiral = SpiralIterator::Over(centerCellX, centerCellZ, sizeX + 1, sizeZ + 1);
    int cellX = centerCellX, cellZ = centerCellZ;
    int candidateX = -1, candidateZ = 0;
    bool found = false;
    while (spiral.GetNext(cellX, cellZ)) {
        float worldX = 0.0f, worldZ = 0.0f;
        host_->CellToWorld(cellX, cellZ, worldX, worldZ);
        if (!lot) {
            const cS3DVector3 delta{worldX - center.x, 0.0f, worldZ - center.z};
            if (minDistance > VectorLength(delta)) continue;
        }
        if (!host_->CellInCity(cellX, cellZ)) continue;
        if (desiredBody == 0 && water) {
            const std::int16_t body = water->BodyOfWaterAt(cellX, cellZ);
            desiredBody = body != 0 ? body : 0;
        }
        if (!ClearToLaunchBoat(cellX, cellZ, 0)) continue;
        const std::int16_t body = water ? water->BodyOfWaterAt(cellX, cellZ) : 0;
        if (desiredBody == body) {
            found = true;
            break;
        }
        if (candidateX == -1) {
            candidateX = cellX;
            candidateZ = cellZ;
        }
    }
    if (!found) {
        if (candidateX == -1) return false;
        cellX = candidateX;
        cellZ = candidateZ;
    }
    if (!host_->PointInCity(static_cast<float>(cellX), static_cast<float>(cellZ))) return false;
    float worldX = 0.0f, worldZ = 0.0f;
    host_->CellToWorld(cellX, cellZ, worldX, worldZ);
    outPosition.x = worldX;
    outPosition.y = host_->TerrainHeight(worldX, worldZ);
    outPosition.z = worldZ;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Persistence (stream version 6; versions below 6 fall through to the parent reader)
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004ef61a. Writes the marina lot registry, the marina occupant registry and the ferry terminal registry (with the
// ferry routes). The runtime-only fields of a route (its boat and the "at station" flag) are not written, and the route
// state is clamped to {0, 1} (anything else becomes -1).
bool cSC4WatercraftManager::Write(cIGZOStream& stream) const {
    stream.SetUint16(6);
    if (!host_ || !base_) return false;
    if (!base_->Write(stream)) return false;
    stream.SetSint32(static_cast<std::int32_t>(marinaLots_.size()));
    for (const auto& entry : marinaLots_) {
        host_->WriteLot(stream, entry.first.get());
        host_->WriteSerializable(stream, entry.second.get());
    }
    stream.SetSint32(static_cast<std::int32_t>(marinaOccupants_.size()));
    for (const auto& entry : marinaOccupants_) host_->WriteSerializable(stream, entry.first.get());
    stream.SetSint32(static_cast<std::int32_t>(terminals_.size()));
    for (const auto& entry : terminals_) {
        const FerryTerminalInformation& info = entry.second;
        host_->WriteLot(stream, entry.first.get());
        host_->WriteSerializable(stream, info.occupant.get());
        stream.SetSint32(static_cast<std::int32_t>(info.routes.size()));
        for (const FerryRouteInformation& routeInfo : info.routes) {
            const FerryRouteInfo& route = routeInfo.route;
            host_->WriteLot(stream, route.fromLot.get());
            host_->WriteLot(stream, route.toLot.get());
            const bool stateKept = route.state == 0 || route.state == 1;
            stream.SetSint32(stateKept ? route.state : -1);
            stream.SetUint16(route.f14);
            stream.SetUint16(route.f16);
            stream.SetUint16(route.f18);
            stream.SetUint16(route.f1a);
            stream.SetUint8(route.f1c);
            stream.SetSint16(route.f1e);
            stream.SetSint16(route.f20);
            stream << route.route;
            stream << route.route2;
        }
    }
    return !stream.GetError();
}

// @ 0x1004ef802. Reads the three registries back. Version 6 clears every registry after the parent read; a route gets
// no boat and the "at station" flag is set (the original stores true).
bool cSC4WatercraftManager::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    if (!stream.GetUint16(version)) return false;
    if (!base_ || !host_) return false;
    if (version < 6) return base_->ReadInternal(stream, version);
    if (version != 6) return false;
    if (!base_->Read(stream)) return false;
    boats_.clear();
    ambientBoats_.clear();
    marinaLots_.clear();
    std::int32_t count = 0;
    if (!stream.GetSint32(count)) return false;
    for (std::int32_t i = 0; i < count; ++i) {
        cSC4Lot* lot = host_->ReadLot(stream);
        auto* occupant = static_cast<cISC4Occupant*>(host_->ReadSerializable(stream, kGZIID_cISC4Occupant));
        marinaLots_.emplace(cRZAutoRefCount<cSC4Lot>(lot), cRZAutoRefCount<cISC4Occupant>(occupant));
        ReleaseCounted(lot);
        ReleaseCounted(occupant);
    }
    marinaOccupants_.clear();
    if (!stream.GetSint32(count)) return false;
    for (std::int32_t i = 0; i < count; ++i) {
        auto* occupant = static_cast<cISC4Occupant*>(host_->ReadSerializable(stream, kGZIID_cISC4Occupant));
        marinaOccupants_.emplace(cRZAutoRefCount<cISC4Occupant>(occupant), nullptr);
        ReleaseCounted(occupant);
    }
    terminals_.clear();
    if (!stream.GetSint32(count)) return false;
    for (std::int32_t i = 0; i < count; ++i) {
        cSC4Lot* lot = host_->ReadLot(stream);
        auto* occupant = static_cast<cISC4Occupant*>(host_->ReadSerializable(stream, kGZIID_cISC4Occupant));
        FerryTerminalInformation info;
        info.occupant = occupant;
        std::int32_t routeCount = 0;
        if (!stream.GetSint32(routeCount)) routeCount = 0;
        for (std::int32_t r = 0; r < routeCount; ++r) {
            FerryRouteInformation routeInfo;
            FerryRouteInfo& route = routeInfo.route;
            cSC4Lot* from = host_->ReadLot(stream);   // counted: the route's auto references take their own
            cSC4Lot* to = host_->ReadLot(stream);
            route.fromLot = from;
            route.toLot = to;
            ReleaseCounted(from);
            ReleaseCounted(to);
            std::int32_t state = -1;
            stream.GetSint32(state);
            route.state = state;
            stream.GetUint16(route.f14);
            stream.GetUint16(route.f16);
            stream.GetUint16(route.f18);
            stream.GetUint16(route.f1a);
            stream.GetUint8(route.f1c);
            stream.GetSint16(route.f1e);
            stream.GetSint16(route.f20);
            stream >> route.route;
            stream >> route.route2;
            routeInfo.atStation = true;
            info.routes.push_back(std::move(routeInfo));
        }
        terminals_.emplace(cRZAutoRefCount<cSC4Lot>(lot), std::move(info));
        ReleaseCounted(lot);
        ReleaseCounted(occupant);
    }
    return !stream.GetError();
}

// ---------------------------------------------------------------------------------------------------------------------
// Ferry / marina creation (PARTIAL: the traffic simulator's ferry routes and neighbour connections are unrecovered)
// ---------------------------------------------------------------------------------------------------------------------

// @ 0x1004f1144. PARTIAL: the recovered prefix is the "station A's terminal occupant must sit at least three cells away
// from the city border" check (the original returns 0 outside it); the boat creation group selection (0x4400 / 0x4402
// from the route state) and the station wiring that follow need the cSC4CompressedRouteReader conversions that
// CreateFerryStruct performs, which are unrecovered.
FerryRouteInformation* cSC4WatercraftManager::CreateFerry(FerryTerminalInformation* stationA,
                                                          FerryTerminalInformation* stationB,
                                                          FerryRouteInformation* route) {
    (void)stationB;
    (void)route;
    if (!stationA || !stationA->occupant || !host_) return nullptr;
    float xyz[3] = {0.0f, 0.0f, 0.0f};
    stationA->occupant->GetPosition(xyz);
    int cellX = 0, cellZ = 0;
    host_->WorldToCell(xyz[0], xyz[2], cellX, cellZ);
    int sizeX = 0, sizeZ = 0;
    host_->CityCellCount(sizeX, sizeZ);
    if (cellX < 3 || sizeX - 3 < cellX || cellZ < 3 || sizeZ - 3 < cellZ) return nullptr;
    SC4_UNWIRED("cSC4WatercraftManager::CreateFerry: ferry boat creation and station wiring not ported");
    return nullptr;
}

// @ 0x1004f180e. PARTIAL: builds the two FerryStationInformation records for a new ferry from a terminal's occupant
// (position, dock direction, lot and compressed route). The route half runs the cSC4CompressedRouteReader
// (SetData/GetNext) and the ClearToLaunchBoat scan, which is where the recovery stops.
void cSC4WatercraftManager::CreateFerryStruct(FerryTerminalInformation* station, FerryRouteInformation* route,
                                              FerryStationInformation& stationA, FerryStationInformation& stationB) {
    (void)station;
    (void)route;
    (void)stationA;
    (void)stationB;
    SC4_UNWIRED("cSC4WatercraftManager::CreateFerryStruct: ferry station records from the compressed route not ported");
}

// @ 0x1004f212e. PARTIAL: creates the boat a marina/seaport occupant owns. The recovered prefix is the occupant
// position query and the "three cells from the border" test; the rest walks cISC4NeighborConnection routes
// (IsNeighborTravelBoat / SetNbrRouteData) and the neighbour-city travel data, which are unrecovered.
bool cSC4WatercraftManager::CreateMarinaBoat(cISC4Occupant* occupant, std::uint32_t group) {
    (void)group;
    if (!occupant || !host_) return false;
    cS3DVector3 position{};
    cS3DVector3 direction{};
    if (!GetPropPositionAndOrientation(occupant, position, direction, false)) return false;
    int cellX = 0, cellZ = 0;
    host_->WorldToCell(position.x, position.z, cellX, cellZ);
    int sizeX = 0, sizeZ = 0;
    host_->CityCellCount(sizeX, sizeZ);
    if (cellX <= 2 || sizeX - 3 < cellX || cellZ <= 2 || sizeZ - 3 < cellZ) return false;
    SC4_UNWIRED("cSC4WatercraftManager::CreateMarinaBoat: marina / seaport boat creation (neighbor travel routes) not ported");
    return false;
}

}  // namespace sc4
