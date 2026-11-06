#include "sc4/AutomataController.h"

#include <cmath>

#include "gz/StreamOps.h"
#include "sc4/AutomataAttractor.h"   // tAttractorTemplate (constructed by CreateTemplateInstance, as in the original)
#include "sc4/DBSegment.h"
#include "sc4/GeometryUtilities.h"

namespace sc4 {

// The answer is a float (it goes straight into the state timer). The port once answered an integer whose bits were then
// read as a float: 60 s became a denormal, and every timed behavior ended on its first tick.
float tBehaviorTemplate::GetTimeoutMilliseconds() const { return timeout > 0.0f ? timeout * 1000.0f : -1.0f; }

namespace {
IAutomataControllerHost* g_host = nullptr;
constexpr std::uint16_t kControllerStreamVersion = 5;
constexpr std::uint32_t kOccupantIid = 0x26bb767e, kAutomatonIid = 0xa9b40f05;

// Cell rect -> metre box (16 m cells; the max edge is the far side of the last cell).
void LotBox(const SC4CellRect& r, float& x0, float& z0, float& x1, float& z1) {
    x0 = static_cast<float>(r.x0 * 16);
    z0 = static_cast<float>(r.z0 * 16);
    x1 = static_cast<float>(r.x1 * 16 + 16);
    z1 = static_cast<float>(r.z1 * 16 + 16);
}
}  // namespace

IAutomataControllerHost* AutomataControllerHost() { return g_host; }
void SetAutomataControllerHost(IAutomataControllerHost* host) { g_host = host; }

cSC4AutomataController::~cSC4AutomataController() {
    if (city_) city_->Release();
    automata_.clear();   // the automata are not released here (Shutdown does that)
    if (occupant_) occupant_->Release();
}

bool cSC4AutomataController::QueryInterface(std::uint32_t iid, void** out) {
    if (iid == kGZIID_cIGZSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
    } else if (iid == 1 || iid == kGZIID_cISC4AutomataController) {
        *out = static_cast<void*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

std::uint32_t cSC4AutomataController::Release() {
    if (refCount_ > 1) return --refCount_;
    FinalRelease();   // Shutdown + delete; the count is not decremented
    return 0;
}

void cSC4AutomataController::FinalRelease() {
    Shutdown();
    delete this;
}

bool cSC4AutomataController::Init() {
    if (initialized_) return true;
    initialized_ = true;
    tractRect_[0] = tractRect_[1] = tractRect_[2] = tractRect_[3] = 0;   // (the original clears the 4-byte rect)
    IAutomataControllerHost* host = g_host;

    cIGZUnknown* city = host->CityContext();
    if (city_ != city) {
        cIGZUnknown* old = city_;
        if (city) city->AddRef();
        city_ = city;
        if (old) old->Release();
    }
    if (CheckFlag(kCtrlFlagFixedPosition)) {
        // fixed controllers sit in the middle of their lot (at ground level) instead of following the occupant
        SC4CellRect rect;
        std::uint32_t facing = 0;
        bool placed = false;
        if (occupant_ && host->GetOccupantLot(occupant_, rect, facing)) {
            float x0, z0, x1, z1;
            LotBox(rect, x0, z0, x1, z1);
            pos_[0] = (x1 - x0) * 0.5f + x0;
            pos_[2] = (z1 - z0) * 0.5f + z0;
            if (host->PointInCity(pos_[0], pos_[2])) {
                pos_[1] = host->TerrainHeight(pos_[0], pos_[2]);
            } else if (occupant_) {
                float p[3] = {0, 0, 0};
                occupant_->GetPosition(p);
                pos_[1] = p[1];
            } else {
                pos_[1] = 0.0f;
            }
            placed = true;
        }
        if (!placed && occupant_) occupant_->GetPosition(pos_);
    }
    Activate(true);
    CheckEffectTriggers();
    PlaySFX(true);
    return true;
}

void cSC4AutomataController::PlaySFX(bool start) {
    IAutomataControllerHost* host = g_host;
    if (!host->SoundAvailable()) return;
    if (start) {
        if (!template_ || template_->sounds.empty()) return;
        const std::uint32_t pick = host->RandomBelow(static_cast<std::uint32_t>(template_->sounds.size()));
        soundHandle_ = template_->sounds[pick];
        float p[3] = {0, 0, 0};
        GetPosition(p);
        host->PostSound(kSoundMsgStartLoop, soundHandle_, ControllerId(), 0, 0);
        host->PostSound(kSoundMsgSetPosition, ControllerId(), static_cast<long>(p[0]), static_cast<long>(p[1]), static_cast<long>(p[2]));
    } else {
        if (soundHandle_ == 0) return;
        host->PostSound(kSoundMsgStopLoop, ControllerId(), 0, 0, 0);
    }
}

bool cSC4AutomataController::Shutdown() {
    Activate(false);
    if (initialized_) {
        initialized_ = false;
        for (IControllerAutomaton* automaton : automata_) {
            if (CheckFlag(kCtrlFlagDetachOnShutdown)) automaton->SetController(nullptr);
            automaton->Release();
        }
        automata_.clear();
        automataCount_ = 0;
        if (occupant_) {
            cISC4Occupant* old = occupant_;
            occupant_ = nullptr;
            old->Release();
        }
        if (city_) {
            cIGZUnknown* old = city_;
            city_ = nullptr;
            old->Release();
        }
        template_ = nullptr;
    }
    return true;
}

void cSC4AutomataController::Activate(bool on) {
    if (!initialized_) return;
    if (CheckFlag(kCtrlFlagActive) == on) return;
    SetFlag(kCtrlFlagActive, on);
    if (on) g_host->RegisterActiveController(this);
    PlaySFX(on);
    HandleViewChanged();
    animTimer_ = 0.0f;
    SetFlag(kCtrlFlagDelayed, false);
    if (on && template_ && template_->startDelay > 0.0f) SetFlag(kCtrlFlagDelayed, true);
}

void cSC4AutomataController::SetOccupant(cISC4Occupant* occupant) {
    cISC4Occupant* old = occupant_;
    if (old == occupant) return;
    if (occupant) occupant->AddRef();
    occupant_ = occupant;
    if (old) old->Release();
}

void cSC4AutomataController::SetTemplate(tControllerTemplate* tmpl) {
    template_ = tmpl;
    if (tmpl) {
        name_.Erase(0, 0xffffffffu);
        lifetime_ = tmpl->lifetime;
        flags_ = tmpl->flags;   // replaces all flags
    }
}

void cSC4AutomataController::DoAnimationTick(float dtMilliseconds, std::uint32_t, const ControllerTime&) {
    if (!CheckFlag(kCtrlFlagActive)) return;
    if (CheckFlag(kCtrlFlagDelayed)) {
        animTimer_ += dtMilliseconds * 0.001f;
        if (template_ && template_->startDelay <= animTimer_) SetFlag(kCtrlFlagDelayed, false);
    }
    if (!CheckFlag(kCtrlFlagDelayed) && lifetime_ > 0.0f) {
        age_ += dtMilliseconds * 0.001f;
        if (lifetime_ < age_) Activate(false);
    }
}

void cSC4AutomataController::CheckEffectTriggers() {
    if (!template_) return;
    IAutomataControllerHost* host = g_host;
    if (!host->BeginEffectQuery()) return;
    bool result = true, haveOn = false;
    if (!template_->effectsOn.empty()) {
        haveOn = true;
        for (std::uint32_t effect : template_->effectsOn) {
            result = host->IsEffectActive(effect);
            if (!result) break;
        }
    }
    bool apply = true;
    if (template_->effectsOff.empty()) {
        if (!haveOn) apply = false;   // no triggers at all: leave the controller alone
    } else if (result) {
        for (std::uint32_t effect : template_->effectsOff) {
            result = !host->IsEffectActive(effect);
            if (!result) break;
        }
    }
    if (apply) Activate(result);
    host->EndEffectQuery();
}

void cSC4AutomataController::NotifySourceDataChanged() {
    if (!template_) return;
    SetFlag(template_->flags, true);
    SetFlag(kCtrlFlagReady, true);
    lifetime_ = template_->lifetime;
    animTimer_ = 0.0f;
}

const tBehaviorTemplate* cSC4AutomataController::GetBehaviorTemplate(float distance, const float* position) const {
    for (const tBehaviorTemplate& entry : template_->behaviors) {
        if (distance > entry.radius) continue;
        if (entry.state != kBehaviorTypeQueue) return &entry;
        IControllerQueue* queue = QueueOf(true);
        queue->InitQueuePoint(occupant_);
        if (queue->IsValid()) {
            const float* q = queue->QueuePoint();
            const float dx = position[0] - q[0], dz = position[2] - q[2];
            const float length = std::sqrt(dx * dx + 0.0f + dz * dz);
            if (length <= entry.radius) return &entry;
        }
    }
    return nullptr;
}

IControllerQueue* cSC4AutomataController::QueueOf(bool create) const { return g_host->GetQueue(occupant_, create); }

bool cSC4AutomataController::HandleStateTick(IControllerAutomaton* automaton, std::uint32_t state, float) {
    if (state != kBehaviorTypeQueue) return false;
    IControllerQueue* queue = QueueOf(true);
    queue->InitQueuePoint(occupant_);
    if (!queue->IsValid()) {
        automaton->SetController(nullptr);
        return true;
    }
    const float* point = queue->QueuePoint();
    float position[3] = {0, 0, 0};
    if (IControllerLeader* leader = automaton->GetLeader()) {
        leader->GetPosition(position);
        float length = leader->GetLength();
        if (length <= 1.0f) length = 1.0f;
        const float* dir = queue->QueueDirection();
        position[0] += dir[0] * length;
        position[1] += dir[1] * length;
        position[2] += dir[2] * length;
    } else {
        position[0] = point[0];
        position[1] = point[1];
        position[2] = point[2];
    }
    position[1] = g_host->GroundHeight(position[0], position[2]);
    automaton->SetPosition(position);
    return false;
}

bool cSC4AutomataController::HandleEnterState(IControllerAutomaton* automaton, std::uint32_t state) {
    if (state == kBehaviorTypeQueue) {
        if (IControllerQueue* queue = QueueOf(true)) queue->AddToQueue(automaton);
    }
    return false;
}

bool cSC4AutomataController::HandleExitState(IControllerAutomaton* automaton, std::uint32_t state) {
    if (state == kBehaviorTypeQueue) {
        if (IControllerQueue* queue = QueueOf(false)) queue->RemoveFromQueue(automaton);
    }
    return false;
}

bool cSC4AutomataController::HandleRemoveAutomaton(IControllerAutomaton* automaton) {
    if (IControllerQueue* queue = QueueOf(false)) queue->RemoveFromQueue(automaton);
    for (auto it = automata_.begin(); it != automata_.end(); ++it) {
        if (*it == automaton) {
            automaton->Release();
            automata_.erase(it);
            --automataCount_;
            break;
        }
    }
    return true;
}

bool cSC4AutomataController::HandleViewChanged() {
    IAutomataControllerHost* host = g_host;
    if (!host->ViewAvailable()) return true;
    bool visible = true;
    if (!CheckFlag(kCtrlFlagAlwaysVisible)) {
        float lo[3], hi[3];
        GetBoundingBox(lo, hi);
        lo[1] = host->PointInCity(lo[0], lo[2]) ? host->TerrainHeight(lo[0], lo[2]) : 0.0f;
        hi[1] = host->PointInCity(hi[0], hi[2]) ? host->TerrainHeight(hi[0], hi[2]) : 0.0f;
        float p[3] = {0, 0, 0};
        GetPosition(p);
        visible = host->IsPointInViewFrustum(p);
        if (!visible) {
            const float c1[3] = {lo[0], lo[1], lo[2]};
            visible = host->IsPointInViewFrustum(c1);
        }
        if (!visible) {
            const float c2[3] = {hi[0], lo[1], lo[2]};
            visible = host->IsPointInViewFrustum(c2);
        }
        if (!visible) {
            const float c3[3] = {hi[0], hi[1], hi[2]};
            visible = host->IsPointInViewFrustum(c3);
        }
        if (!visible) {
            const float c4[3] = {lo[0], hi[1], hi[2]};
            visible = host->IsPointInViewFrustum(c4);
        }
    }
    SetFlag(kCtrlFlagInView, visible);
    return true;
}

void cSC4AutomataController::GetPosition(float* xyz) const {
    if (occupant_ && !CheckFlag(kCtrlFlagFixedPosition)) {
        occupant_->GetPosition(xyz);
        return;
    }
    xyz[0] = pos_[0];
    xyz[1] = pos_[1];
    xyz[2] = pos_[2];
}

void cSC4AutomataController::GetLotFacingPoint(float* xyz) const {
    if (!occupant_) {
        xyz[0] = pos_[0];
        xyz[1] = pos_[1];
        xyz[2] = pos_[2];
        return;
    }
    SC4CellRect rect;
    std::uint32_t facing = 0;
    if (!g_host->GetOccupantLot(occupant_, rect, facing)) {
        occupant_->GetPosition(xyz);
        return;
    }
    float x0, z0, x1, z1;
    LotBox(rect, x0, z0, x1, z1);
    // The edge the lot faces: (ax, az) .. (bx, bz), a point one metre outside the lot.
    float ax = 0, az = 0, bx = 0, bz = 0;
    switch (facing) {
        case 0: ax = x0; az = z0 - 1.0f; bx = x1; bz = z0 - 1.0f; break;
        case 1: ax = x1 + 1.0f; az = z0; bx = x1 + 1.0f; bz = z1; break;
        case 2: ax = x1; az = z1 + 1.0f; bx = x0; bz = z1 + 1.0f; break;
        case 3: ax = x0 - 1.0f; az = z1; bx = x0 - 1.0f; bz = z0; break;
        default: break;   // unknown facing: (0, 0, 0)
    }
    xyz[0] = (bx - ax) * 0.5f + ax;
    xyz[1] = 0.0f;
    xyz[2] = (bz - az) * 0.5f + az;
}

bool cSC4AutomataController::IsPointWithinRadius(const float* point, float* distance) {
    float lo[3], hi[3];
    GetOccupantBoundingBox(lo, hi);
    const float radius = GetRadius();
    if (point[0] < lo[0] - radius || point[2] < lo[2] - radius || hi[0] + radius < point[0] || radius + hi[2] < point[2]) return false;
    if (CheckFlag(kCtrlFlagTerrainBound)) {
        if (g_host->TerrainHeight(point[0], point[2]) + 5.0f < point[1]) return false;
    }
    if (distance) {
        float centre[3];
        if (!CheckFlag(kCtrlFlagLotFacing)) GetPosition(centre); else GetLotFacingPoint(centre);
        const float from[2] = {centre[0], centre[2]};
        const float to[2] = {point[0], point[2]};
        float hit[2];
        const float* start = from;
        // GetBoundingBoxIntersection is a virtual-free helper in the original too (called directly)
        if (GetBoundingBoxIntersection(from, to, hit)) start = hit;
        *distance = SC4GeometryUtilities::LengthOfLine2D(start, to);
    }
    return true;
}

bool cSC4AutomataController::GetBoundingBoxIntersection(const float* a, const float* b, float* out) const {
    float lo[3], hi[3];
    GetOccupantBoundingBox(lo, hi);
    const float bottomLeft[2] = {lo[0], lo[2]}, bottomRight[2] = {hi[0], lo[2]};
    const float topRight[2] = {hi[0], hi[2]}, topLeft[2] = {lo[0], hi[2]};
    namespace G = SC4GeometryUtilities;
    return G::LinesIntersect2D(a, b, bottomLeft, bottomRight, out) || G::LinesIntersect2D(a, b, bottomRight, topRight, out) ||
           G::LinesIntersect2D(a, b, topRight, topLeft, out) || G::LinesIntersect2D(a, b, topLeft, bottomLeft, out);
}

void cSC4AutomataController::GetOccupantBoundingBox(float* minXyz, float* maxXyz) const {
    if (!CheckFlag(kCtrlFlagFixedPosition) && !CheckFlag(kCtrlFlagBoxFromPosition) && occupant_) {
        occupant_->GetBoundingBox(minXyz, maxXyz);
        SC4CellRect rect;
        std::uint32_t facing = 0;
        if (g_host->GetOccupantLot(occupant_, rect, facing)) {
            float x0, z0, x1, z1;
            LotBox(rect, x0, z0, x1, z1);
            minXyz[0] = x0;
            minXyz[2] = z0;
            maxXyz[0] = x1;
            maxXyz[2] = z1;
        }
        return;
    }
    GetPosition(minXyz);
    GetPosition(maxXyz);
}

void cSC4AutomataController::GetBoundingBox(float* minXyz, float* maxXyz) const {
    IAutomataControllerHost* host = g_host;
    GetOccupantBoundingBox(minXyz, maxXyz);
    const float radius = GetRadius();
    minXyz[0] -= radius;
    maxXyz[0] += radius;
    minXyz[2] -= radius;
    maxXyz[2] += radius;
    const float width = host->CityWidth(), depth = host->CityDepth();
    auto clamp = [](float v, float limit) { return v < 0.0f ? 0.0f : (v <= limit ? v : limit); };
    minXyz[0] = clamp(minXyz[0], width);
    minXyz[2] = clamp(minXyz[2], depth);
    maxXyz[0] = clamp(maxXyz[0], width);
    maxXyz[2] = clamp(maxXyz[2], depth);
    minXyz[1] = host->CityTerrainHeight(minXyz[0], minXyz[2]);
    maxXyz[1] = host->CityTerrainHeight(maxXyz[0], maxXyz[2]);
}

const cIGZString& cSC4AutomataController::GetTemplateName() const { return template_ ? template_->name : name_; }

bool cSC4AutomataController::IsActive() const {
    return CheckFlag(kCtrlFlagActive) && CheckFlag(kCtrlFlagReady) && CheckFlag(kCtrlFlagInView) && !CheckFlag(kCtrlFlagDelayed);
}

void cSC4AutomataController::SetFlag(std::uint32_t flag, bool on) { flags_ = on ? (flags_ | flag) : (flags_ & ~flag); }

bool cSC4AutomataController::IsFull() const {
    if (!template_ || template_->maxAutomata == 0) return false;
    return template_->maxAutomata <= automataCount_;
}

bool cSC4AutomataController::Write(cIGZOStream& stream) const {
    stream.SetUint16(kControllerStreamVersion);
    SC4WriteGZSerializable(stream, occupant_);
    stream.SetGZStr(GetTemplateName());
    stream << cS3DVector3{pos_[0], pos_[1], pos_[2]};
    stream.SetFloat32(age_);
    stream.SetFloat32(lifetime_);
    stream.SetUint32(soundHandle_);
    stream.SetUint32(flags_);
    stream.SetUint32(automataCount_);
    for (IControllerAutomaton* automaton : automata_) SC4WriteGZSerializable(stream, g_host->AutomatonObject(automaton));
    return !stream.GetError();
}

bool cSC4AutomataController::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (occupant_) {
        cISC4Occupant* old = occupant_;
        occupant_ = nullptr;
        old->Release();
    }
    SC4ReadGZSerializable(stream, kOccupantIid, reinterpret_cast<void**>(&occupant_), false);
    stream.GetGZStr(name_);
    cS3DVector3 position;
    stream >> position;
    pos_[0] = position.x;
    pos_[1] = position.y;
    pos_[2] = position.z;
    stream.GetFloat32(age_);
    stream.GetFloat32(lifetime_);
    stream.GetUint32(soundHandle_);
    stream.GetUint32(flags_);
    stream.GetUint32(automataCount_);
    for (std::uint32_t i = 0; i < automataCount_; ++i) {
        void* automaton = nullptr;
        SC4ReadGZSerializable(stream, kAutomatonIid, &automaton, false);
        // A null (an automaton class that could not be made) is dropped: every later walk of the list calls through it.
        if (IControllerAutomaton* link = automaton ? g_host->AutomatonLink(automaton) : nullptr) automata_.push_back(link);
    }
    return !stream.GetError();
}

tControllerTemplate* cSC4AutomataController::CreateTemplateInstance(const cRZString& name, std::uint32_t type) {
    tControllerTemplate* created = nullptr;
    if (type == kControllerTypeGenerator) {
        created = new tGeneratorTemplate();          // `new(0x158)` + the inlined tGeneratorTemplate ctor
    } else if (type == kControllerTypeAttractor) {
        created = new tAttractorTemplate();          // `new(0x148)` + cSC4AutomataAttractor::tTemplate @ 0x1000fbe8e
    } else {
        // The original formats "Couldn't create a controller template for controller type %d!" into a temporary
        // cRZString and drops it again (nothing consumes the message), then answers null.
        return nullptr;
    }
    created->classKey = type;
    created->name.Copy(name);
    created->controllerType = type;
    created->radius = 0.0f;
    created->field60 = -1;
    created->curve.SetResponseCurveData(std::vector<SC4Point>{{0.0f, 1.0f}, {24.0f, 1.0f}});
    return created;
}

}  // namespace sc4
