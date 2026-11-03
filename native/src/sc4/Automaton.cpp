#include "sc4/Automaton.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "rz/cRZMessage2Standard.h"
#include "s3d/S3DMath.h"
#include "sc4/AutomataGenerator.h"   // AutomataFloorIdiom
#include "sc4/DBSegment.h"
#include "sc4/SCPropertyHelp.h"

namespace sc4 {

namespace {

IAutomatonHost* g_host = nullptr;

constexpr std::uint32_t kControllerIid = 0x6a1857f8;   // cISC4AutomataController
constexpr std::uint32_t kSerializableIid = 0xe4fda3d4;  // cIGZSerializable
constexpr std::uint32_t kMessageTargetIid = 0x452294aa; // cIGZMessageTarget2
constexpr std::uint16_t kAutomatonStreamVersion = 8;
constexpr float kDirectionEpsilon = 1e-5f;
constexpr float kDestinationEpsilon = 0.01f;

bool IsFinite(float v) { return !std::isnan(v) && !std::isinf(v); }

// The original's and-mask clamp: 0 for a negative ratio, otherwise min(ratio, 1).
float ClampRatio(float ratio) {
    if (!(ratio > 0.0f)) return 0.0f;
    return ratio <= 1.0f ? ratio : 1.0f;
}

cS3DVector3 MakeVector(float x, float y, float z) { return cS3DVector3{x, y, z}; }

void StoreVector(float* out, const cS3DVector3& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}

cS3DVector3 LoadVector(const float* in) { return cS3DVector3{in[0], in[1], in[2]}; }

float Length(const cS3DVector3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

// The 16 m cell the "avoid lots / avoid roads" tests use.
int CellOfWorldCoordinate(float world) { return AutomataFloorIdiom(world * 0.0625f); }

}  // namespace

IAutomatonHost* AutomatonHost() { return g_host; }
void SetAutomatonHost(IAutomatonHost* host) { g_host = host; }

// ---- TweenStruct -----------------------------------------------------------------------------------------------------
void AutomatonTween::SetDesired(float value, float seconds) {
    if (desired != value || std::isnan(desired) || std::isnan(value)) {
        float start = desired;
        if (duration > 0.0f) start = ClampRatio(elapsed / duration) * (desired - current) + current;
        current = start;
        desired = value;
        duration = seconds;
        elapsed = 0.0f;
    }
}

float AutomatonTween::Ratio() const { return ClampRatio(elapsed / duration); }

void AutomatonTween::Write(cIGZOStream& stream) const {
    stream.SetFloat32(current);
    stream.SetFloat32(desired);
    stream.SetFloat32(duration);
    stream.SetFloat32(elapsed);
}

void AutomatonTween::Read(cIGZIStream& stream) {
    stream.GetFloat32(current);
    stream.GetFloat32(desired);
    stream.GetFloat32(duration);
    stream.GetFloat32(elapsed);
}

// ---- AutomataOBB -----------------------------------------------------------------------------------------------------
AutomataOBB::AutomataOBB(const cSC4Automaton& automaton, const cS3DVector3& position, float scale)
    : centerX(position.x), centerZ(position.z) {
    IAutomatonModel* model = automaton.GetModelInstance();
    if (!model) return;
    float bounds[6] = {0, 0, 0, 0, 0, 0};
    model->GetBounds(bounds, bounds + 3);
    const float minX = bounds[0], minZ = bounds[2], maxX = bounds[3], maxZ = bounds[5];
    // The original asks the automaton for interface 0x4a416254 (wide vehicles) and shrinks the box less.
    const bool wide = false;   // the port's automata do not answer that interface
    const float shrinkX = wide ? 1.0f : 0.5f;
    const float shrinkZ = wide ? 2.0f : 0.5f;
    halfX = (maxX - minX - shrinkX) * 0.5f * scale;
    halfZ = (maxZ - minZ - shrinkZ) * 0.5f * scale;
    cS3DTransform transform;
    model->GetTransform(transform);
    centerX = transform.translation[0] + position.x;
    centerZ = transform.translation[2] + position.z;
    const float length = std::sqrt(transform.rotation[0][0] * transform.rotation[0][0] +
                                   transform.rotation[0][2] * transform.rotation[0][2]);
    if (length > 0.0f) {
        dirX = transform.rotation[0][0] / length;
        dirZ = -transform.rotation[0][2] / length;
    }
    valid = true;
}

bool AutomataOBB::TestForCollision(const AutomataOBB& other) const {
    if (!valid || !other.valid) return false;
    const float dx = other.centerX - centerX;
    const float dz = other.centerZ - centerZ;
    const float axisDot = dirX * other.dirX + dirZ * other.dirZ;
    const float axisCross = dirX * other.dirZ - other.dirX * dirZ;
    const float projected = dz * dirZ + dirX * dx;
    const float negCross = -axisCross;
    if (std::fabs(projected) - (std::fabs(other.halfZ * negCross) + std::fabs(other.halfX * axisDot) + halfX) > 0.0f)
        return false;
    const float projectedZ = dirX * dz - dx * dirZ;
    if (std::fabs(projectedZ) - (std::fabs(axisDot * other.halfZ) + std::fabs(other.halfX * axisCross) + halfZ) > 0.0f)
        return false;
    if (std::fabs(projectedZ * axisCross + projected * axisDot) -
            (std::fabs(axisCross) * halfZ + other.halfX + halfX * std::fabs(axisDot)) > 0.0f)
        return false;
    return std::fabs(projectedZ * axisDot + projected * negCross) -
               (std::fabs(axisDot) * halfZ + other.halfZ + halfX * std::fabs(negCross)) <= 0.0f;
}

// ---- ControllerLink --------------------------------------------------------------------------------------------------
// The original hands `this` to the controller. The port's cSC4AutomataController takes its IControllerAutomaton seam
// instead, whose slot names differ from the automaton's vtable: its "SetController" is slot 0x68 = Remove(bool) and its
// "SetPosition" is slot 0x50 = SetDestination (verified in the controller's HandleStateTick: *0x50(automaton) with the
// queue point). GetLeader/SetLeader are slots 0x180/0x188 and GetPosition/GetLength are the leader's 0x58/0x88.
class cSC4Automaton::ControllerLink final : public IControllerAutomaton, public IControllerLeader {
public:
    explicit ControllerLink(cSC4Automaton& owner) : owner_(owner) {}
    cSC4Automaton& owner() const { return owner_; }

    void AddRef() override { owner_.AddRef(); }
    void Release() override { owner_.Release(); }
    void SetController(void* controller) override { owner_.Remove(controller != nullptr); }
    void SetPosition(const float* xyz) override { owner_.SetDestination(xyz); }
    IControllerLeader* GetLeader() override {
        cSC4Automaton* leader = owner_.GetLeader();
        return leader ? static_cast<ControllerLink*>(leader->ControllerLinkFor()) : nullptr;
    }
    void SetLeader(IControllerAutomaton* leader) override {
        owner_.SetLeader(leader ? &static_cast<ControllerLink*>(leader)->owner() : nullptr);
    }
    void GetPosition(float* xyz) override { owner_.GetPosition(xyz); }
    float GetLength() override { return owner_.Radius(); }

private:
    cSC4Automaton& owner_;
};

// ---- construction ----------------------------------------------------------------------------------------------------
cSC4Automaton::cSC4Automaton(IAutomatonHost* host, const cGZPersistResourceKey& modelKey) : host_(host) {
    type_ = kAutomatonOccupantType;            // +0x50
    flags_ = static_cast<std::uint8_t>(flags_ | 0x10);   // +0x5b: the automaton's own occupant flag bit
    saved_ = false;                            // +0x70
    position_ = MakeVector(0.0f, 0.0f, 0.0f);
    destination_ = MakeVector(0.0f, 0.0f, 0.0f);
    orientation_ = MakeVector(1.0f, 0.0f, 0.0f);   // +0x8c = (1, 0, 0)
    for (float& value : unset_) value = kAutomatonUnsetCoordinate;
    radius_ = 0.0f;
    defaultSpeed_ = kAutomatonInitialSpeed;
    automataFlags_ = 0;
    dispatchInfoId_ = 0;
    soundInstanceId_ = 0;
    priority_ = 0;
    actualSpeed_ = 0.0f;
    attractionRadius_ = 0.0f;
    attractionDistance_ = 0.0f;
    speedTween_ = AutomatonTween{kAutomatonInitialSpeed, kAutomatonInitialSpeed, 0.0f, 0.0f};
    opacityTween_ = AutomatonTween{1.0f, 1.0f, 0.0f, 0.0f};
    if (modelKey.instance != 0) AddModelResourceKey(modelKey);
}

cSC4Automaton::~cSC4Automaton() {
    ClearStateStack();
    if (auxiliaryModel_) {
        Host() ? Host()->DestroyModel(auxiliaryModel_) : auxiliaryModel_->Release();
        auxiliaryModel_ = nullptr;
    }
    for (IAutomatonModel* model : models_) {
        if (Host()) Host()->DestroyModel(model);
        else if (model) model->Release();
    }
    models_.clear();
    if (attractionController_) attractionController_->Release();
    if (leader_) leader_->Release();
    if (effects_) {
        effects_->SetModel(nullptr);
        effects_->Shutdown();
        if (Host()) Host()->DestroyEffects(effects_);
    }
    effects_ = nullptr;
    delete controllerLink_;
    delete extension_;
    controllerLink_ = nullptr;
}

bool cSC4Automaton::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == 1 || iid == kAutomatonIid) {
        *out = this;
    } else if (iid == kMessageTargetIid) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else {
        return cSC4Occupant::QueryInterface(iid, out);
    }
    AddRef();
    return true;
}

// 0x10010dfc2: a reference count below two runs Shutdown (slot 0x20) and FinalRelease (slot 0x238) without decrementing.
std::uint32_t cSC4Automaton::Release() {
    if (RefCount() > 1) return cSC4Occupant::Release();
    Shutdown();
    return cSC4Occupant::Release();
}

IControllerAutomaton* cSC4Automaton::ControllerLinkFor() {
    // Allocated lazily and kept alive by the automaton: the controller's queue stores the pointer it is given.
    if (!controllerLink_) controllerLink_ = new ControllerLink(*this);
    return controllerLink_;
}

IAutomatonEffects* cSC4Automaton::EnsureEffects() {
    if (!effects_ && Host()) effects_ = Host()->CreateEffects();
    return effects_;
}

// ---- Init / Shutdown -------------------------------------------------------------------------------------------------
bool cSC4Automaton::Init() {
    bool result = true;
    if (!IsInitialized()) {
        result = LoadModels();
        if (result) InitEffectsList();
        cSC4Occupant::Init();
    }
    return result;
}

void cSC4Automaton::InitEffectsList() {
    if (models_.empty()) return;
    if (IAutomatonEffects* effects = EnsureEffects()) {
        effects->SetModel(models_.front());
        effects->Init();
        effects->Update(position_, orientation_, animTimer_);
        StartAllEffects();
    }
}

bool cSC4Automaton::Shutdown() {
    UnregisterInGameDataRegistry();
    if (effects_) {
        effects_->StopAll(false);
        effects_->SetModel(nullptr);
    }
    if (IsInitialized()) {
        HandleExitState(GetState());
        ClearStateStack();
        if (leader_) {
            cSC4Automaton* old = leader_;
            leader_ = nullptr;
            old->Release();
        }
        if (IsFlagSet(kAutomatonOccupantInManager)) PlaceInOccupantManager(false);
        if (IsFlagSet(kAutomatonOccupantInRenderer)) PlaceInRenderer(false);
        ShutdownGhostModel();
        UnloadModels();
        callbacks_.clear();
        if (attractionController_) {
            cSC4AutomataController* old = attractionController_;
            attractionController_ = nullptr;
            old->Release();
        }
        automataFlags_ = 0;
        radius_ = 0.0f;
        if (activeSoundHandle_ != 0 && Host()) Host()->StopSoundLoop(activeSoundHandle_);
        activeSoundHandle_ = 0;
        cSC4Occupant::Shutdown();
    }
    return true;
}

// ---- models ----------------------------------------------------------------------------------------------------------
bool cSC4Automaton::AddModelResourceKey(const cGZPersistResourceKey& key) {
    if (key.instance != 0 && primaryKey_.instance == 0) {
        primaryKey_ = key;
        primaryInstanceId_ = key.instance;
    }
    modelKeys_.push_back(key);
    bool result = true;
    if (IsInitialized()) result = AddModelInstance(key) != nullptr;
    return result;
}

IAutomatonModel* cSC4Automaton::AddModelInstance(const cGZPersistResourceKey& key) {
    if (!Host()) return nullptr;
    IAutomatonModel* model = Host()->CreateModel(key);
    if (!model) return nullptr;
    models_.push_back(model);
    PositionModelInstance(model);   // the original places a new instance at the automaton's position/heading
    return model;
}

bool cSC4Automaton::RemoveModelInstance(IAutomatonModel* model) {
    if (model == nullptr) return false;
    if (!models_.empty() && effects_ && models_.front() == model) effects_->SetModel(nullptr);
    const bool inRenderer = IsFlagSet(kAutomatonOccupantInRenderer);
    if (inRenderer && !models_.empty() && Host()) PlaceInRenderer(false);
    auto it = std::find(models_.begin(), models_.end(), model);
    if (it != models_.end()) {
        models_.erase(it);
        if (Host()) Host()->DestroyModel(model);
        else model->Release();
    }
    if (inRenderer && !models_.empty() && Host()) PlaceInRenderer(true);
    UpdateEffects();
    return true;
}

bool cSC4Automaton::LoadModels() {
    // The model system (cS3DModelProcessor and the animated-instance loader) is unrecovered: the host turns each stored
    // key into a model instance and the bounding box comes from the first one (what the original does in-process).
    for (const cGZPersistResourceKey& key : modelKeys_) AddModelInstance(key);
    if (!models_.empty()) {
        float bounds[6] = {1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
        models_.front()->GetBounds(bounds, bounds + 3);
        std::memcpy(boundingBox_, bounds, sizeof(boundingBox_));
        PositionModelInstance(models_.front());
    }
    return true;
}

bool cSC4Automaton::UnloadModels() {
    if (effects_) effects_->SetModel(nullptr);
    for (IAutomatonModel* model : models_) {
        if (Host()) Host()->DestroyModel(model);
        else if (model) model->Release();
    }
    models_.clear();
    return true;
}

bool cSC4Automaton::LoadAuxiliaryModel(std::uint32_t specifier, IAutomatonModel** out) {
    (void)specifier;
    if (out) *out = nullptr;
    // The ghost model depends on the renderer's model creation (unrecovered): no auxiliary model is produced.
    return false;
}

cGZPersistResourceKey cSC4Automaton::GetModelResourceKey(int index) const {
    if (index >= 0 && static_cast<std::size_t>(index) < modelKeys_.size()) return modelKeys_[static_cast<std::size_t>(index)];
    return cGZPersistResourceKey{};
}

IAutomatonModel* cSC4Automaton::GetModelInstance() const { return models_.empty() ? nullptr : models_.front(); }

void cSC4Automaton::PositionModelInstance(IAutomatonModel* model) {
    if (!model) return;
    cS3DTransform transform;
    model->GetTransform(transform);
    nS3D::Matrix3 rotation;
    nS3D::AlignZToDirWithYUp(orientation_, rotation);
    std::memcpy(transform.rotation, rotation.m, sizeof(rotation.m));
    transform.translation[0] = position_.x;
    transform.translation[1] = position_.y;
    transform.translation[2] = position_.z;
    transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformRotation | nS3D::kTransformTranslation);
    ++transform.changeCounter;
    model->SetTransform(transform);
}

bool cSC4Automaton::GetTransform(cS3DTransform& transform) const {
    if (models_.empty()) return false;
    models_.front()->GetTransform(transform);
    return true;
}

void cSC4Automaton::SetTransform(const cS3DTransform& transform) {
    for (IAutomatonModel* model : models_) {
        if (model) model->SetTransform(transform);
    }
}

void cSC4Automaton::SetZoomAndRotation(int zoom, int rotation) {
    for (IAutomatonModel* model : models_) {
        if (model) model->SetZoomAndRotation(zoom, rotation);
    }
}

// ---- serialization ---------------------------------------------------------------------------------------------------
bool cSC4Automaton::Write(cIGZOStream& stream) const {
    stream.SetUint16(kAutomatonStreamVersion);
    if (!cSC4Occupant::Write(stream)) return false;
    stream << position_ << destination_ << orientation_;
    stream.SetFloat32(radius_);
    stream.SetFloat32(defaultSpeed_);
    stream.SetUint32(automataFlags_);
    stream.SetUint32(dispatchInfoId_);
    const cIGZUnknown* controller = nullptr;
    if (attractionController_ && !attractionController_->CheckFlag(kControllerFlagTransient))
        controller = static_cast<const cIGZSerializable*>(attractionController_);
    SC4WriteGZSerializable(stream, controller);
    stream.SetFloat32(attractionRadius_);
    stream.SetFloat32(attractionDistance_);
    stream << attraction_;
    SC4WriteGZSerializable(stream, static_cast<const cIGZSerializable*>(leader_));
    stream.SetUint32(static_cast<std::uint32_t>(states_.size()));
    for (const tAutomataState* state : states_) {
        stream.SetUint32(state->stateType);
        state->Write(stream);
    }
    stream.SetUint32(static_cast<std::uint32_t>(modelKeys_.size()));
    for (const cGZPersistResourceKey& key : modelKeys_) stream << key;
    stream << primaryKey_;
    stream.SetUint32(primaryInstanceId_);
    // Only the callbacks that are serializable are saved (the original queries 0xe4fda3d4 on each).
    std::vector<cIGZSerializable*> savedCallbacks;
    for (const cRZAutoRefCount<cIGZMessageTarget2>& callback : callbacks_) {
        if (!callback.get()) continue;
        void* raw = nullptr;
        if (callback.get()->QueryInterface(kSerializableIid, &raw) && raw) {
            savedCallbacks.push_back(static_cast<cIGZSerializable*>(raw));
        }
    }
    stream.SetUint32(static_cast<std::uint32_t>(savedCallbacks.size()));
    for (cIGZSerializable* callback : savedCallbacks) SC4WriteGZSerializable(stream, callback);
    for (cIGZSerializable* callback : savedCallbacks) callback->Release();
    speedTween_.Write(stream);
    opacityTween_.Write(stream);
    stream.SetUint8(priority_);
    stream.SetUint32(automataGroupId_);
    return !stream.GetError();
}

bool cSC4Automaton::Read(cIGZIStream& stream) {
    saved_ = true;   // +0x70: a record has been loaded
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 9 || !cSC4Occupant::Read(stream)) return false;
    stream >> position_ >> destination_ >> orientation_;
    stream.GetFloat32(radius_);
    stream.GetFloat32(defaultSpeed_);
    stream.GetUint32(automataFlags_);
    stream.GetUint32(dispatchInfoId_);
    void* rawController = nullptr;
    SC4ReadGZSerializable(stream, kControllerIid, &rawController, false);
    if (attractionController_) attractionController_->Release();
    attractionController_ = static_cast<cSC4AutomataController*>(rawController);
    stream.GetFloat32(attractionRadius_);
    stream.GetFloat32(attractionDistance_);
    stream >> attraction_;
    void* rawLeader = nullptr;
    SC4ReadGZSerializable(stream, kAutomatonIid, &rawLeader, false);
    if (leader_) {
        cSC4Automaton* old = leader_;
        leader_ = static_cast<cSC4Automaton*>(rawLeader);
        old->Release();
    } else {
        leader_ = static_cast<cSC4Automaton*>(rawLeader);
    }
    std::uint32_t stateCount = 0;
    stream.GetUint32(stateCount);
    for (std::uint32_t i = 0; i < stateCount; ++i) {
        std::uint32_t stateType = 0;
        stream.GetUint32(stateType);
        tAutomataState* state = CreateAutomataState(stateType);
        if (state) {
            state->stateType = stateType;
            state->stateId = static_cast<std::uint32_t>(states_.size());
            state->Read(stream);
        }
        states_.push_back(state);
    }
    std::uint32_t keyCount = 0;
    stream.GetUint32(keyCount);
    for (std::uint32_t i = 0; i < keyCount; ++i) {
        cGZPersistResourceKey key;
        stream >> key;
        modelKeys_.push_back(key);
    }
    stream >> primaryKey_;
    stream.GetUint32(primaryInstanceId_);
    std::uint32_t callbackCount = 0;
    stream.GetUint32(callbackCount);
    for (std::uint32_t i = 0; i < callbackCount; ++i) {
        void* raw = nullptr;
        SC4ReadGZSerializable(stream, kMessageTargetIid, &raw, false);
        callbacks_.emplace_back(static_cast<cIGZMessageTarget2*>(raw));
        if (raw) static_cast<cIGZMessageTarget2*>(raw)->Release();
    }
    speedTween_.Read(stream);
    opacityTween_.Read(stream);
    stream.GetUint8(priority_);
    if (version > 7) stream.GetUint32(automataGroupId_);
    return !stream.GetError();
}

// ---- leader ----------------------------------------------------------------------------------------------------------
cSC4Automaton* cSC4Automaton::GetLeader() const { return leader_; }

void cSC4Automaton::SetLeader(cSC4Automaton* leader) {
    if (leader_ == leader) return;
    if (leader) leader->AddRef();
    cSC4Automaton* previous = leader_;
    leader_ = leader;
    if (previous) previous->Release();
}

// 0x100106ae4: the pursuit target of a chase state reports that it is gone (data2 2 or 3).
bool cSC4Automaton::DoMessage(cIGZMessage2* message) {
    if (!message || message->GetType() != kAutomatonMsgAutomaton) return false;
    // The original reads the payload through the cIGZMessage2Standard slots; the port already has the concrete class,
    // so no reference is taken here (a message may be a stack object with no owner reference).
    auto* standard = dynamic_cast<cIGZMessage2Standard*>(message);
    if (!standard) return false;
    const std::uint64_t data2 = standard->GetData2();
    if ((data2 & ~static_cast<std::uint64_t>(1)) != 2) return false;
    tAutomataState* found = FindState(kStatePursuit);
    if (found) {
        auto* chase = static_cast<tAutomataStateChase*>(found);
        if (chase->target != nullptr && chase->target == standard->GetVoid1()) {
            chase->target->Release();
            chase->target = nullptr;
            PopState();
        }
    }
    return true;
}

// ---- position / heading ----------------------------------------------------------------------------------------------
void cSC4Automaton::GetPosition(float* xyz) {
    if (!xyz) return;
    StoreVector(xyz, position_);
}

void cSC4Automaton::SetPosition(const float* xyz) {
    if (!xyz) return;
    // Occupant flags bits 0 and 1 set means "placed": the move then goes through MoveTo so the models follow.
    if ((flags_ & 3) == 3) {
        MoveTo(xyz, nullptr);
        return;
    }
    position_ = LoadVector(xyz);
    for (IAutomatonModel* model : models_) {
        if (!model) continue;
        cS3DTransform transform;
        model->GetTransform(transform);
        transform.translation[0] = position_.x;
        transform.translation[1] = position_.y;
        transform.translation[2] = position_.z;
        transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
        ++transform.changeCounter;
        model->SetTransform(transform);
    }
}

void cSC4Automaton::SetOrientation(const float* xyz) {
    if (!xyz) return;
    if ((flags_ & 3) == 3) {
        MoveTo(nullptr, xyz);
        return;
    }
    const cS3DVector3 direction = LoadVector(xyz);
    float length = Length(direction);
    const float inverse = length > 0.0f ? 1.0f / length : 0.0f;
    orientation_ = MakeVector(direction.x * inverse, direction.y * inverse, direction.z * inverse);
    for (IAutomatonModel* model : models_) PositionModelInstance(model);
}

bool cSC4Automaton::GetOrientation(float* xyz) const {
    if (!xyz) return false;
    StoreVector(xyz, orientation_);
    return true;
}

void cSC4Automaton::SetDestination(const float* xyz) {
    if (!xyz) return;
    destination_ = LoadVector(xyz);
}

void cSC4Automaton::GetDestination(float* xyz) const {
    if (!xyz) return;
    StoreVector(xyz, destination_);
}

void cSC4Automaton::GetBoundingBox(float* minXyz, float* maxXyz) {
    if (minXyz) {
        minXyz[0] = boundingBox_[0];
        minXyz[1] = boundingBox_[1];
        minXyz[2] = boundingBox_[2];
    }
    if (maxXyz) {
        maxXyz[0] = boundingBox_[3];
        maxXyz[1] = boundingBox_[4];
        maxXyz[2] = boundingBox_[5];
    }
}

void cSC4Automaton::SetVisible(bool visible) {
    cSC4Occupant::SetVisibility(visible, false);
    if (!IsInitialized()) return;
    PlaceInRenderer(visible);
    if (visible) {
        StartAllEffects();
    } else if (IAutomatonEffects* effects = EnsureEffects()) {
        effects->StopAll(true);
    }
}

// ---- placement -------------------------------------------------------------------------------------------------------
bool cSC4Automaton::PlaceInOccupantManager(bool add) {
    const bool inManager = IsFlagSet(kAutomatonOccupantInManager);
    if (add == inManager) return true;
    if (!Host()) return false;
    if (add) {
        if (!models_.empty()) {
            float bounds[6];
            models_.front()->GetBounds(bounds, bounds + 3);
            std::memcpy(boundingBox_, bounds, sizeof(boundingBox_));
        }
        if (!Host()->InsertOccupant(this)) return false;
        SetFlag(kAutomatonOccupantInManager);
        return true;
    }
    if (IsFlagSet(kAutomatonOccupantInRenderer)) {
        for (IAutomatonModel* model : models_) {
            if (model) Host()->RemoveModelFromRenderer(model);
        }
        ClearFlag(kAutomatonOccupantInRenderer);
    }
    Host()->RemoveOccupant(this);
    ClearFlag(kAutomatonOccupantInManager);
    return true;
}

namespace {
// The value a tween shows right now.
float TweenValue(const AutomatonTween& tween) {
    if (tween.duration <= 0.0f) return tween.desired;
    return ClampRatio(tween.elapsed / tween.duration) * (tween.desired - tween.current) + tween.current;
}
}  // namespace

// 0x100107370. Quirk kept: the remove path (and the no-op case) answer true, the add path answers false even when it
// registered the models.
bool cSC4Automaton::PlaceInRenderer(bool add) {
    const bool inRenderer = IsFlagSet(kAutomatonOccupantInRenderer);
    if (add == inRenderer) return true;
    if (!Host()) return false;
    if (!add) {
        for (IAutomatonModel* model : models_) {
            if (model) Host()->RemoveModelFromRenderer(model);
        }
        ClearFlag(kAutomatonOccupantInRenderer);
        return true;
    }
    if (!IsFlagSet(kAutomatonOccupantInManager)) PlaceInOccupantManager(true);
    if (!IsFlagSet(kAutomatonOccupantInManager)) return false;
    const float opacity = TweenValue(opacityTween_);
    bool registered = true;
    for (IAutomatonModel* model : models_) {
        if (!model) continue;
        model->SetOpacity(opacity <= 1.0f ? opacity : 1.0f, opacity < 1.0f);
        if (!Host()->AddModelToRenderer(model)) {
            registered = false;
            break;
        }
    }
    if (!registered) return false;
    SetFlag(kAutomatonOccupantInRenderer);
    return false;
}

void cSC4Automaton::FinishMove(bool fadeIn, bool fadeOut) {
    if (!models_.empty()) {
        float bounds[6];
        models_.front()->GetBounds(bounds, bounds + 3);
        std::memcpy(boundingBox_, bounds, sizeof(boundingBox_));
    }
    if (IsFlagSet(kOccupantFlagVisible) && Host()) {
        if (!IsFlagSet(kAutomatonOccupantInRenderer)) {
            if (!IsFlagSet(kAutomatonOccupantInManager)) PlaceInOccupantManager(true);
            else Host()->RefreshOccupant(this);
            if (IsFlagSet(kAutomatonOccupantInManager)) PlaceInRenderer(true);
        } else {
            for (IAutomatonModel* model : models_) {
                if (!model) continue;
                if (fadeIn) Host()->FadeModelIn(model);
                else if (fadeOut) Host()->FadeModelOut(model, 3);
            }
            if (IsFlagSet(kAutomatonOccupantInManager)) Host()->RefreshOccupant(this);
        }
    }
    if (animTimer_ > 100.0f) {
        SendCallbackMessage(kAutomataEventMoveFinished, 0);
        animTimer_ = 0.0f;
    }
}

// 0x100107572: heading first (direction), then the position; both may be null. Quirk kept: the city bounds are only
// checked when a coordinate is NaN, and FinishMove always runs.
void cSC4Automaton::MoveTo(const float* position, const float* direction) {
    bool moved = false;
    if (direction && IsFinite(direction[0]) && IsFinite(direction[1]) && IsFinite(direction[2]) &&
        !(std::fabs(direction[0]) < kDirectionEpsilon && std::fabs(direction[1]) < kDirectionEpsilon &&
          std::fabs(direction[2]) < kDirectionEpsilon)) {
        const cS3DVector3 hint = LoadVector(direction);
        const float inverse = 1.0f / Length(hint);
        orientation_ = MakeVector(hint.x * inverse, hint.y * inverse, hint.z * inverse);
        nS3D::Matrix3 rotation;
        nS3D::AlignZToDirWithYUp(orientation_, rotation);
        for (IAutomatonModel* model : models_) {
            if (!model) continue;
            cS3DTransform transform;
            model->GetTransform(transform);
            std::memcpy(transform.rotation, rotation.m, sizeof(rotation.m));
            transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformRotation);
            ++transform.changeCounter;
            model->SetTransform(transform);
        }
        moved = true;
    }
    bool positioned = false;
    if (position != nullptr) {
        if (!IsFinite(position[0]) || !IsFinite(position[1]) || !IsFinite(position[2])) {
            if (position[0] < 0.0f || position[2] < 0.0f) {
                Remove(false);
                return;
            }
            if (Host() && (position[0] >= Host()->CitySizeX() || position[2] >= Host()->CitySizeZ())) {
                Remove(false);
                return;
            }
        }
        position_ = LoadVector(position);
        for (IAutomatonModel* model : models_) {
            if (!model) continue;
            cS3DTransform transform;
            model->GetTransform(transform);
            transform.translation[0] = position_.x;
            transform.translation[1] = position_.y;
            transform.translation[2] = position_.z;
            transform.flags = static_cast<std::uint8_t>(transform.flags | nS3D::kTransformTranslation);
            ++transform.changeCounter;
            model->SetTransform(transform);
        }
        positioned = true;
        if (!GetMarkedForRemove() && (CheckAutomataFlag(kAutomataFlagAvoidLots) || CheckAutomataFlag(kAutomataFlagAvoidRoads))) {
            const int cellX = CellOfWorldCoordinate(position_.x);
            const int cellZ = CellOfWorldCoordinate(position_.z);
            if ((CheckAutomataFlag(kAutomataFlagAvoidLots) && Host() && Host()->LotAt(cellX, cellZ)) ||
                (CheckAutomataFlag(kAutomataFlagAvoidRoads) && Host() && Host()->NetworkAt(cellX, cellZ))) {
                Remove(false);
            }
        }
    }
    UpdateLighting();
    UpdateEffects();
    FinishMove(positioned, moved);
}

// ---- speed / opacity -------------------------------------------------------------------------------------------------
float cSC4Automaton::GetCurrentSpeed() const { return TweenValue(speedTween_); }

float cSC4Automaton::GetOpacity() const { return TweenValue(opacityTween_); }

float cSC4Automaton::GetMaxSpeed() const { return Host() ? Host()->MaxSpeedUnits() * kAutomatonSpeedScale : 0.0f; }

void cSC4Automaton::SetSpeed(float speed, float seconds) { speedTween_.SetDesired(speed, seconds); }

void cSC4Automaton::SetOpacity(float opacity, float seconds) {
    opacityTween_.SetDesired(opacity, seconds);
    if (seconds < 0.001f) {
        for (IAutomatonModel* model : models_) {
            if (!model) continue;
            model->SetOpacity(opacity <= 1.0f ? opacity : 1.0f, opacity < 1.0f);
        }
    }
}

void cSC4Automaton::UpdateOpacity(float milliseconds) {
    const float duration = opacityTween_.duration;
    float value = 0.0f;      // the value the models end up with
    float previous = 0.0f;   // the value they showed before
    if (duration <= 0.0f) {
        previous = opacityTween_.desired;
        value = previous;
    } else {
        const float ratio = opacityTween_.elapsed / duration;
        previous = ClampRatio(ratio) * (opacityTween_.desired - opacityTween_.current) + opacityTween_.current;
        value = opacityTween_.desired;
    }
    if (duration <= opacityTween_.elapsed) {
        if (duration > 0.0f) {
            value = ClampRatio(opacityTween_.elapsed / duration) * (value - opacityTween_.current) + opacityTween_.current;
        }
    } else {
        opacityTween_.elapsed += milliseconds / 1000.0f;
        if (opacityTween_.elapsed < duration) {
            value = ClampRatio(opacityTween_.elapsed / duration) * (value - opacityTween_.current) + opacityTween_.current;
        } else {
            opacityTween_.duration = 0.0f;
            opacityTween_.current = value;
        }
    }
    if (value == previous && !std::isnan(value) && !std::isnan(previous)) return;
    for (IAutomatonModel* model : models_) {
        if (!model) continue;
        model->SetOpacity(value <= 1.0f ? value : 1.0f, value < 1.0f);
    }
}

// ---- flags and small accessors ---------------------------------------------------------------------------------------
void cSC4Automaton::SetAutomataFlag(std::uint32_t flag, bool on) {
    automataFlags_ = on ? (automataFlags_ | flag) : (automataFlags_ & ~flag);
}

bool cSC4Automaton::CheckAutomataFlag(std::uint32_t flag) const { return (automataFlags_ & flag) != 0; }

bool cSC4Automaton::GetMarkedForRemove() const { return CheckAutomataFlag(kAutomataFlagMarkedForRemove); }
void cSC4Automaton::SetMarkedForRemove(bool on) { SetAutomataFlag(kAutomataFlagMarkedForRemove, on); }
bool cSC4Automaton::GetSelfControlling() const { return CheckAutomataFlag(kAutomataFlagSelfControlling); }
void cSC4Automaton::SetSelfControlling(bool on) { SetAutomataFlag(kAutomataFlagSelfControlling, on); }
bool cSC4Automaton::GetReachedDestination() const { return CheckAutomataFlag(kAutomataFlagReachedDestination); }
bool cSC4Automaton::GetSpecialRouteFlag() const { return CheckAutomataFlag(kAutomataFlagSpecialRoute); }
void cSC4Automaton::SetSpecialRouteFlag(bool on) { SetAutomataFlag(kAutomataFlagSpecialRoute, on); }
bool cSC4Automaton::GetDisabled() const { return CheckAutomataFlag(kAutomataFlagDisabled); }

float cSC4Automaton::GetSpeedModifier() const {
    const float modifier = Host() ? Host()->SimSpeedModifier() : 0.0f;
    if (CheckAutomataFlag(kAutomataFlagIgnoreSpeedModifier)) return 0.0f < modifier ? 1.0f : 0.0f;
    return modifier;
}

// ---- effects, lighting, attraction -----------------------------------------------------------------------------------
void cSC4Automaton::StartAllEffects() {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->StartAll();
}

void cSC4Automaton::UpdateEffects() {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->Update(position_, orientation_, animTimer_);
}

void cSC4Automaton::AddRegEffect(const cRZString& effect, const cRZString& point) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->AddRegEffect(effect, point);
}

void cSC4Automaton::FindRegEffect(const cRZString& effect) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->FindRegEffect(effect);
}

void cSC4Automaton::RemoveRegEffect(const cRZString& effect) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->RemoveRegEffect(effect);
}

void cSC4Automaton::StartNamedEffects(const cRZString& effect, int transition, bool loop) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->StartNamed(effect, transition, loop);
}

void cSC4Automaton::StopNamedEffects(const cRZString& effect, int transition) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->StopNamed(effect, transition);
}

void cSC4Automaton::ToggleNamedEffects(const cRZString& effect, int transition, bool on) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->ToggleNamed(effect, transition, on);
}

void cSC4Automaton::RecreateNamedEffects(const cRZString& effect, int transition, bool on) {
    if (IAutomatonEffects* effects = EnsureEffects()) effects->RecreateNamed(effect, transition, on);
}

void cSC4Automaton::StopKeyEffects() {
    // The key effects are the registered effects of the automata group's behaviour templates; the effect registry is a
    // seam, so the group's snapshot (the effect name of each behaviour) is what gets stopped.
    if (!effects_) return;
    const auto* behavior = GetState() ? reinterpret_cast<const tBehaviorTemplate*>(GetState()->runtime30) : nullptr;
    (void)behavior;
    effects_->StopAll(true);
}

void cSC4Automaton::InitKeyEffects() {
    if (!effects_) return;
    const auto* behavior = GetState() ? reinterpret_cast<const tBehaviorTemplate*>(GetState()->runtime30) : nullptr;
    (void)behavior;
    // The behaviour templates carry effect names; the group's templates are recovered but the effect creation is a seam.
    effects_->Init();
}

void cSC4Automaton::InitLighting() {}
void cSC4Automaton::UpdateLighting() {}

void cSC4Automaton::UpdateAttraction() {
    if (CheckAutomataFlag(kAutomataFlagIgnoreAttractors) || !Host()) return;
    cSC4AutomataController* controller = Host()->FindAttraction(this, &attraction_, &attractionRadius_, &attractionDistance_);
    if (controller) controller->AddRef();
    if (controller != attractionController_) {
        if (attractionController_) {
            while (true) {
                tAutomataState* top = GetState();
                if (!top || top->controller != attractionController_) break;
                PopState();
            }
        }
        if (attractionController_) attractionController_->Release();
        attractionController_ = controller;
    }
    if (attractionController_) {
        const tBehaviorTemplate* behavior = attractionController_->GetBehaviorTemplate(attractionDistance_, &position_.x);
        if (behavior && Host()->RandomUnit() <= behavior->percentage) {
            while (true) {
                tAutomataState* top = GetState();
                if (!top) break;
                if (top->controller == nullptr) break;
                if (top->controller->GetOccupant() != attractionController_->GetOccupant() ||
                    top->stateType == behavior->state)
                    break;
                PopState();
            }
            tAutomataState* top = GetState();
            if (!top || top->runtime30 != reinterpret_cast<std::uint64_t>(behavior))
                PushBehavior(behavior, attractionController_);
        }
    }
    if (controller) controller->Release();
}

// ---- callbacks -------------------------------------------------------------------------------------------------------
void cSC4Automaton::AddCallback(cIGZMessageTarget2* target) {
    if (!target) return;
    for (const cRZAutoRefCount<cIGZMessageTarget2>& callback : callbacks_) {
        if (callback.get() == target) return;
    }
    callbacks_.emplace_back(target);
}

void cSC4Automaton::RemoveCallback(cIGZMessageTarget2* target) {
    for (auto it = callbacks_.begin(); it != callbacks_.end(); ++it) {
        if (it->get() == target) {
            callbacks_.erase(it);
            return;
        }
    }
}

void cSC4Automaton::SendCallbackMessage(std::uint32_t event, std::uint32_t data) {
    if (callbacks_.empty()) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kAutomatonMsgAutomaton);
    message->SetVoid1(this);
    message->SetData2(event);
    message->SetData3(data);
    for (const cRZAutoRefCount<cIGZMessageTarget2>& callback : callbacks_) {
        if (callback.get()) callback.get()->DoMessage(message);
    }
    message->Release();
}

// ---- sound -----------------------------------------------------------------------------------------------------------
bool cSC4Automaton::StartSFX(std::uint32_t sfx) {
    if (sfx == 0 || !Host()) return false;
    constexpr std::uint32_t kSfxStartMessage = 0x8a385dd8;
    soundInstanceId_ = Host()->GenerateSoundInstanceId();
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kSfxStartMessage);
    message->SetVoid1(this);
    message->SetData1(sfx);
    message->SetData2(soundInstanceId_);
    Host()->PostMessage(message);
    message->Release();
    return true;
}

bool cSC4Automaton::StopSFX(std::uint32_t sfx) {
    (void)sfx;
    if (soundInstanceId_ == 0 || !Host()) return true;
    constexpr std::uint32_t kSfxStopMessage = 0xaa396960;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(kSfxStopMessage);
    message->SetVoid1(this);
    message->SetData2(soundInstanceId_);
    Host()->PostMessage(message);
    message->Release();
    return true;
}

// 0x10010ea18: the active sound loop reads its parameters from occupant property 0x6c044bb5 once, when first enabled.
void cSC4Automaton::EnableActiveSound(bool on) {
    if (!activeSoundEnabled_ && on) {
        activeSoundData_.clear();
        cSCPropertyHelp::GetPropertyValue(AsPropertyHolder(), kAutomatonPropertyActiveSound, activeSoundData_);
    }
    activeSoundEnabled_ = on;
}

void cSC4Automaton::UpdateActiveSound(float milliseconds) {
    // PARTIAL: the original starts / moves / stops a sound loop through the sound manager (SFX::SoundManager,
    // cSC4AudioSoundManager::GenerateNewInstanceId) and the 0x6c044bb5 property data. The active-sound loop is not in
    // the port's sound layer, so only the handle bookkeeping and the stop message are kept here.
    if (!activeSoundEnabled_) {
        if (activeSoundHandle_ != 0 && Host()) {
            Host()->StopSoundLoop(activeSoundHandle_);
            activeSoundHandle_ = 0;
        }
        return;
    }
    (void)milliseconds;
}

// ---- states ----------------------------------------------------------------------------------------------------------
tAutomataState* cSC4Automaton::GetState() { return states_.empty() ? nullptr : states_.back(); }

tAutomataState* cSC4Automaton::FindState(std::uint32_t state) const {
    for (auto it = states_.rbegin(); it != states_.rend(); ++it) {
        if (*it && (*it)->stateType == state) return *it;
    }
    return nullptr;
}

// 0x10010d0d4: the original answers the state whose stored stack index (+0x18) is one less; the port's state record
// stores the state id there, so the predecessor is found by position in the stack (the same answer while the stack is
// well formed).
tAutomataState* cSC4Automaton::PrevState(const tAutomataState* state) const {
    if (state == nullptr || states_.empty() || states_.front() == state) return nullptr;
    for (std::size_t i = 1; i < states_.size(); ++i) {
        if (states_[i] == state) return states_[i - 1];
    }
    return nullptr;
}

std::uint32_t cSC4Automaton::GetBehavior() const { return states_.empty() || !states_.back() ? 0 : states_.back()->stateType; }

tAutomataState* cSC4Automaton::CreateAutomataState(std::uint32_t state) {
    tAutomataState* created = nullptr;
    switch (state) {
        case 8: {
            auto* value = new tAutomataStateRiotLeader();
            value->value38 = -1;
            value->value3c = -1;
            created = value;
            break;
        }
        case kStateRandomWalk: created = new tAutomataStateRandomWalk(); break;
        case kStatePlummet: created = new tAutomataStatePlummet(); break;
        case kStatePlopJump: created = new tAutomataStatePlopJump(); break;
        case kStatePlayerDrive: created = new tAutomataStatePlayerDrive(); break;
        case kStateBumped: created = new tAutomataStateBumped(); break;
        case kStateAttachAnim: created = new tAutomataStateAttachAnim(); break;
        case kStateCollide: created = new tAutomataStateCollide(); break;
        case kStatePullOver: created = new tAutomataStatePullOver(); break;
        case kStatePursuit: created = new tAutomataStateChase(); break;
        default: created = new tAutomataState(); break;
    }
    if (created) created->stateType = state;
    return created;
}

void cSC4Automaton::ClearStateStack() {
    for (tAutomataState* state : states_) {
        if (!state) continue;
        state->Shutdown(this);
        delete state;
    }
    states_.clear();
}

bool cSC4Automaton::SetState(std::uint32_t state, cSC4AutomataController* controller, float timer, bool flag,
                             std::uint32_t animOffset, std::uint32_t sfx, const tBehaviorTemplate* behavior) {
    while (!states_.empty()) {
        tAutomataState* top = states_.back();
        if (top && top->flag) return false;
        if (!PopState()) break;
    }
    return PushState(state, controller, timer, flag, animOffset, sfx, behavior);
}

bool cSC4Automaton::PushState(std::uint32_t stateType, cSC4AutomataController* controller, float timer, bool flag,
                              std::uint32_t animOffset, std::uint32_t sfx, const tBehaviorTemplate* behavior) {
    if (stateType == 0 && controller == nullptr) {
        if (states_.empty()) return false;
        return SetState(0, nullptr, timer, flag, animOffset, sfx, behavior);
    }
    if (!states_.empty()) {
        tAutomataState* top = states_.back();
        if (top && top->stateType == stateType && top->controller == controller) return false;
    }
    HandleExitState(states_.empty() ? nullptr : states_.back());
    tAutomataState* state = CreateAutomataState(stateType);
    if (!state) return false;
    states_.push_back(state);
    state->stateType = stateType;
    state->controller = controller;
    state->timer = timer;
    state->flag = flag;
    state->value28 = animOffset;
    state->soundHandle = sfx;
    state->runtime30 = reinterpret_cast<std::uint64_t>(behavior);
    if (controller) controller->AddRef();
    state->stateId = static_cast<std::uint32_t>(states_.size() - 1);
    HandleEnterState(state);
    return true;
}

bool cSC4Automaton::PopState() {
    if (states_.empty()) return false;
    tAutomataState* state = states_.back();
    HandleExitState(state);
    if (!state->flag || state->stateType == kStateIdle) {
        if (state->controller) state->controller->Release();
        states_.pop_back();
        delete state;
        if (!GetSelfControlling() && !states_.empty()) HandleEnterState(states_.back());
        return true;
    }
    PushState(kStateIdle, nullptr, -1.0f, false, 0, 0, nullptr);
    return true;
}

void cSC4Automaton::SetBehavior(const tBehaviorTemplate* behavior, cSC4AutomataController* controller) {
    HandleExitState(GetState());
    ClearStateStack();
    PushBehavior(behavior, controller);
}

bool cSC4Automaton::PushBehavior(const tBehaviorTemplate* behavior, cSC4AutomataController* controller) {
    if (!behavior) return false;
    std::uint32_t sfx = 0;
    std::uint32_t animOffset = 0;
    if (!behavior->sfx.empty() && Host()) sfx = behavior->sfx[Host()->RandomBelow(static_cast<std::uint32_t>(behavior->sfx.size()))];
    if (!behavior->anims.empty() && Host())
        animOffset = GetAnimOffsetByName(behavior->anims[Host()->RandomBelow(static_cast<std::uint32_t>(behavior->anims.size()))]);
    // GetTimeoutMilliseconds answers the float bits of -1.0 for "no timeout"; they are handed on as the timer.
    const std::uint32_t timeoutBits = behavior->GetTimeoutMilliseconds();
    float timer = 0.0f;
    std::memcpy(&timer, &timeoutBits, sizeof(timer));
    return PushState(behavior->state, controller, timer, behavior->final, animOffset, sfx, behavior);
}

void cSC4Automaton::SetSpeedFromBehaviorTemplate(const tBehaviorTemplate* behavior) {
    if (!behavior) return;
    float speed = defaultSpeed_;
    std::int32_t kph = behavior->kph[0];
    if (kph > 0 && behavior->kph[1] > 0) {
        if (kph != behavior->kph[1] && Host()) kph = Host()->RandomInt(kph, behavior->kph[1]);
        speed = static_cast<float>(kph) * kAutomatonSpeedScale;
    }
    const float multiplyLow = behavior->kphMultiply[0];
    const float multiplyHigh = behavior->kphMultiply[1];
    if (multiplyLow != multiplyHigh || std::isnan(multiplyLow) || std::isnan(multiplyHigh)) {
        if (Host()) speed = static_cast<float>(static_cast<double>(speed) * Host()->RandomDouble(multiplyLow, multiplyHigh));
    } else {
        speed *= multiplyLow;
    }
    SetSpeed(speed, behavior->acceleration);
}

void cSC4Automaton::SetAutomataGroup(IAutomatonGroupTemplate* group) {
    if (!group) return;
    automataGroupId_ = group->GroupId();
    if (group->SpeedMin() > 0.0f) {
        float high = group->SpeedMax();
        IAutomatonGroupTemplate* current = group;
        while (high <= 0.0f) {
            current = current->Parent();
            if (!current) {
                high = 0.0f;
                break;
            }
            high = current->SpeedMax();
        }
        if (Host()) {
            const float speed = static_cast<float>(Host()->RandomDouble(group->SpeedMin(), high)) * kAutomatonSpeedScale;
            defaultSpeed_ = speed;
            SetSpeed(speed, 0.0f);
        }
    }
    StopKeyEffects();
    InitKeyEffects();
}

IAutomatonGroupTemplate* cSC4Automaton::GetAutomataGroup() const {
    if (automataGroupId_ == 0 || !Host()) return nullptr;
    return Host()->FindAutomataGroup(automataGroupId_);
}

// 0x100108016: the group of a loaded automaton is the largest occupant group; the original calls
// GetGroupHierarchyDepth for every group but the first and discards the answer (quirk kept).
bool cSC4Automaton::PostInit() {
    if (!saved_ || automataGroupId_ != 0 || !Host()) return true;
    std::set<std::uint32_t> groups;
    if (!GetOccupantGroups(groups)) return true;
    std::uint32_t groupId = 0;
    for (std::uint32_t id : groups) {
        const bool hadPrevious = groupId != 0;
        groupId = id;
        if (hadPrevious) {
            if (IAutomatonGroupTemplate* group = Host()->FindAutomataGroup(groupId)) group->GetGroupHierarchyDepth();
        }
        automataGroupId_ = groupId;
    }
    return true;
}

bool cSC4Automaton::HandleEnterState(tAutomataState* state) {
    if (!state) {
        PlayAnim(static_cast<std::uint32_t>(0));
        SendCallbackMessage(kAutomataEventStateEntered, 0);
        return false;
    }
    state->value2c = automataFlags_;
    PlayAnim(state->value28);
    if (state->soundHandle != 0) StartSFX(state->soundHandle);
    switch (state->stateType) {
        case kStateRandomWalk: {
            auto* walk = static_cast<tAutomataStateRandomWalk*>(state);
            walk->speed = 0.0f;
            walk->turnRate = -0.01f;   // 0xbc23d70a
            walk->target = orientation_;
            break;
        }
        case kStatePlummet: {
            auto* plummet = static_cast<tAutomataStatePlummet*>(state);
            plummet->value48 = 0.0f;
            if (Host()) plummet->value44 = static_cast<float>(Host()->RandomDouble(1.0, 2.5) * 1000.0);
            plummet->value40 = orientation_.y;
            plummet->value38 = -0.0041666669f;   // 0xbb88888a
            plummet->value3c = 0.5f;
            plummet->runtime50 = 0;
            break;
        }
        case kStatePlopJump: {
            auto* jump = static_cast<tAutomataStatePlopJump*>(state);
            jump->value38 = position_.y;
            if (Host()) jump->value3c = static_cast<float>(Host()->RandomDouble(30.0, 50.0) * 0.0002777778);
            break;
        }
        case kStatePlayerDrive: {
            auto* drive = static_cast<tAutomataStatePlayerDrive*>(state);
            drive->value38 = 0.0f;
            for (float& value : drive->history) value = -1.0f;
            SetMarkedForRemove(false);
            SetOpacity(1.0f, 0.0f);
            SetAutomataFlag(kAutomataFlagPlayerDrive, true);
            SetAutomataFlag(kAutomataFlagSelfControlling, true);
            SetAutomataFlag(kAutomataFlagNoCollision, true);
            speedTween_ = AutomatonTween{0.0f, 0.0f, 0.0f, 0.0f};
            opacityTween_ = AutomatonTween{1.0f, 1.0f, 0.0f, 0.0f};
            break;
        }
        case kStateMagnet:
            SetAutomataFlag(kAutomataFlagPlayerDrive, true);
            SetAutomataFlag(kAutomataFlagIgnorePaths, true);
            break;
        case kStateCollide:
            AddToCollisionSystem(static_cast<tAutomataStateCollide*>(state));
            break;
        default: break;
    }
    const auto* behavior = reinterpret_cast<const tBehaviorTemplate*>(state->runtime30);
    if (behavior) {
        if (behavior->ignorePaths) SetAutomataFlag(kAutomataFlagIgnorePaths, true);
        if (behavior->ignoreRoads) SetAutomataFlag(kAutomataFlagIgnoreRoads, true);
        if (behavior->ignoreTerrain) SetAutomataFlag(kAutomataFlagIgnoreTerrain, true);
        if (behavior->ignoreAttractors) SetAutomataFlag(kAutomataFlagIgnoreAttractors, true);
        if (behavior->avoidLots) SetAutomataFlag(kAutomataFlagAvoidLots, true);
        if (behavior->avoidRoads) SetAutomataFlag(kAutomataFlagAvoidRoads, true);
        SetSpeedFromBehaviorTemplate(behavior);
    }
    std::uint32_t stateType = state->stateType;
    if (stateType == kStateIdle) {
        Remove(false);
        stateType = state->stateType;
    }
    SendCallbackMessage(kAutomataEventStateEntered, stateType);
    if (state->controller) return state->controller->HandleEnterState(ControllerLinkFor(), state->stateType);
    return false;
}

bool cSC4Automaton::HandleExitState(const tAutomataState* state) {
    if (!state) return false;
    const std::uint32_t stateType = state->stateType;
    if (stateType == kStatePlayerDrive) {
        StopKeyEffects();
    } else if (stateType != kStateCollide) {
        if (stateType == kStateAttachAnim) {
            auto* attach = const_cast<tAutomataStateAttachAnim*>(static_cast<const tAutomataStateAttachAnim*>(state));
            if (attach->animation) {
                attach->animation->Release();
                attach->animation = nullptr;
            }
        }
    } else {
        const_cast<tAutomataState*>(state)->Shutdown(this);
    }
    if (state->soundHandle != 0) StopSFX(state->soundHandle);
    automataFlags_ = (automataFlags_ & 0x600c001u) | state->value2c;
    SendCallbackMessage(kAutomataEventStateExited, stateType);
    if (state->controller) return state->controller->HandleExitState(ControllerLinkFor(), state->stateType);
    return true;
}

// 0x10010c00a / 0x10010cc76: the game data registry entries (one value per behaviour, the four s* callbacks and the
// script-system node). The registry bridge is unrecovered, so both are no-ops that keep the lifecycle balance.
void cSC4Automaton::RegisterInGameDataRegistry() {}
void cSC4Automaton::UnregisterInGameDataRegistry() {}

// ---- animation -------------------------------------------------------------------------------------------------------
bool cSC4Automaton::PlayAnim(const cGZPersistResourceKey& key) {
    if (key.instance == 0 || models_.empty() || !Host()) return true;
    // PARTIAL: cSC4Automaton::ReplaceModelInstance (0x10010ab5e) builds a new animated instance from the key, transfers
    // the zoom / rotation, the effects model and the bounding box. The model layer is a seam here, so this only swaps
    // the first model instance and repositions it.
    IAutomatonModel* model = Host()->CreateModel(key);
    if (!model) return true;
    IAutomatonModel* old = models_.front();
    if (effects_) effects_->SetModel(model);
    models_.front() = model;
    PositionModelInstance(model);
    Host()->DestroyModel(old);
    return true;
}

bool cSC4Automaton::PlayAnim(std::uint32_t animId) {
    if (primaryKey_.instance != 0 && !models_.empty()) {
        cGZPersistResourceKey key = primaryKey_;
        key.instance = primaryInstanceId_ + animId;
        PlayAnim(key);
    }
    return true;
}

bool cSC4Automaton::PlayAnimByName(const cRZString& name) {
    PlayAnim(GetAnimOffsetByName(name));
    return true;
}

// 0x10010aee2: resolves the animation offset through the occupant's group templates. The script system's group
// templates are a seam and their animation tables are not recovered, so no offset is produced (PARTIAL).
std::uint32_t cSC4Automaton::GetAnimOffsetByName(const cRZString& name) const {
    (void)name;
    return 0;
}

bool cSC4Automaton::AttachAnim(const cGZPersistResourceKey& key, s3d::PlayMode mode, bool flag, float rate) {
    (void)key;
    (void)mode;
    (void)flag;
    (void)rate;
    // PARTIAL: creates an auxiliary animated instance (AddModelInstance) and an attach-animation state; both need the
    // uncaptured animated-instance layer.
    return false;
}

void cSC4Automaton::UpdateAnimation(float milliseconds) {
    if (Host() && !Host()->AnimationUpdateAllowed()) return;
    for (IAutomatonModel* model : models_) {
        if (model) model->UpdateAnimation(milliseconds);
    }
    if (!callbacks_.empty() && !models_.empty()) {
        const int frame = models_.front()->GetFrame();
        const int frameCount = models_.front()->GetFrameCount();
        if (frameCount - 1 <= frame) SendCallbackMessage(kAutomataEventAnimationDone, 0);
    }
    animTimer_ += milliseconds;
}

// ---- the simulated step ----------------------------------------------------------------------------------------------
// 0x100107ce2: advance the tweens and, while the automaton controls itself, run the state machine or walk towards the
// destination.
bool cSC4Automaton::DoAnimationTick(std::uint32_t unknown, float milliseconds) {
    (void)unknown;
    if (speedTween_.elapsed < speedTween_.duration) {
        speedTween_.elapsed += milliseconds / 1000.0f;
        if (speedTween_.elapsed >= speedTween_.duration) {
            speedTween_.duration = 0.0f;
            speedTween_.current = speedTween_.desired;
        }
    }
    UpdateOpacity(milliseconds);
    UpdateAnimation(milliseconds);
    if (GetSelfControlling()) {
        if (!DoStateTick(milliseconds)) {
            if (!GetReachedDestination()) {
                if (destination_.x < 0.0f || destination_.y < 0.0f || destination_.z < 0.0f) destination_ = position_;
                const cS3DVector3 delta = destination_ - position_;
                const float distance = Length(delta);
                if (distance <= kDestinationEpsilon) {
                    SetAutomataFlag(kAutomataFlagReachedDestination, true);
                    SendCallbackMessage(kAutomataEventDestinationReached, 0);
                } else {
                    orientation_ = delta * (1.0f / distance);
                    const cS3DVector3 velocity = orientation_ * GetCurrentSpeed();
                    const float modifier = GetSpeedModifier();
                    cS3DVector3 next = position_ + velocity * (modifier * milliseconds);
                    if (!CheckAutomataFlag(kAutomataFlagIgnoreRoads) && Host())
                        Host()->PlaceOnNetwork(position_, next, kAutomataMoveNetworkMask);
                    if (!CheckAutomataFlag(kAutomataFlagIgnoreTerrain) && Host())
                        next.y = Host()->TerrainHeight(next.x, next.z);
                    const cS3DVector3 previous = position_;
                    MoveTo(&next.x, &orientation_.x);
                    actualSpeed_ = Length(next - previous) / milliseconds;
                }
            }
        }
    }
    SendCallbackMessage(kAutomataEventTick, 0);
    return true;
}

bool cSC4Automaton::DoStateTick(float milliseconds) {
    if (states_.empty()) return false;
    tAutomataState* state = states_.back();
    if (state->timer > 0.0f) {
        state->timer -= milliseconds;
        if (state->timer < 0.0f) {
            PopState();
            if (states_.empty()) return true;
            state = states_.back();
        }
    }
    if (state->controller &&
        state->controller->HandleStateTick(ControllerLinkFor(), state->stateType, milliseconds)) {
        return true;
    }
    switch (state->stateType) {
        case kStateIdle:
            if (!GetMarkedForRemove()) Remove(false);
            return true;
        case kStateIdleTick: return DoIdleTick(milliseconds);
        case kStateScripted: return DoScriptedTick(milliseconds);
        case kStateRandomWalk: return DoRandomWalkTick(milliseconds);
        case kStatePlopJump: return DoPlopJumpTick(milliseconds);
        case kStatePlayerDrive: return DoPlayerDriveTick(milliseconds);
        case kStateMagnet: return DoMagnetTick(milliseconds);
        case kStateAttachAnim: return DoAttachAnimTick(milliseconds);
        case kStateCollide: return DoCollideTick(milliseconds);
        default: return false;
    }
}

bool cSC4Automaton::DoIdleTick(float milliseconds) {
    (void)milliseconds;
    if (!GetMarkedForRemove()) UpdateAttraction();
    return !GetMarkedForRemove();
}

// PARTIAL: the original turns by a random amount drawn from the behaviour template's delay/turn ranges (with the
// atan2/sincos pair), picks a new speed through SetSpeedFromBehaviorTemplate and can drop back to a "bumped" state.
// The port keeps the per-state timer bookkeeping and the forward step through MoveTo.
bool cSC4Automaton::DoRandomWalkTick(float milliseconds) {
    tAutomataState* generic = GetState();
    if (!generic) return true;
    auto* state = static_cast<tAutomataStateRandomWalk*>(generic);
    const float seconds = milliseconds / 1000.0f;
    const auto* behavior = reinterpret_cast<const tBehaviorTemplate*>(state->runtime30);
    if (state->turnRate >= 0.0f) {
        state->turnRate -= seconds;
        state->speed -= seconds;
        if (state->speed <= 0.0f) {
            if (Host() && behavior) state->speed += static_cast<float>(Host()->RandomDouble(behavior->delay[0], behavior->delay[1]));
            SetSpeedFromBehaviorTemplate(behavior);
            PlayAnim(static_cast<std::uint32_t>(0));
        }
        const float speed = GetCurrentSpeed();
        cS3DVector3 next = position_ + orientation_ * (speed * GetSpeedModifier() * milliseconds);
        if (!CheckAutomataFlag(kAutomataFlagIgnoreTerrain) && Host()) next.y = Host()->TerrainHeight(next.x, next.z);
        MoveTo(&next.x, &orientation_.x);
    } else {
        if (Host() && behavior) state->turnRate = -static_cast<float>(Host()->RandomDouble(behavior->idleDelay[0], behavior->idleDelay[1]));
        UpdateAttraction();
    }
    return true;
}

bool cSC4Automaton::DoPlopJumpTick(float milliseconds) {
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return false;
    tAutomataState* generic = GetState();
    if (!generic) return true;
    auto* state = static_cast<tAutomataStatePlopJump*>(generic);
    cS3DVector3 next = position_;
    const float risen = state->value3c * milliseconds + position_.y;
    float height = state->value38;
    if (height <= risen) {
        state->value3c += milliseconds * -5.5555563e-05f;
        height = risen;
    }
    next.y = height;
    MoveTo(&next.x, nullptr);
    return true;
}

// PARTIAL: advances the 13 command timers and then runs TranslatePlayerDriveCommands; the occupant-manager vehicle
// query that the original performs after it (slot 0x88) is not in the port's occupant-manager seam.
bool cSC4Automaton::DoPlayerDriveTick(float milliseconds) {
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return false;
    tAutomataState* generic = GetState();
    if (!generic) return true;
    auto* state = static_cast<tAutomataStatePlayerDrive*>(generic);
    for (float& value : state->history) {
        if (value < 0.0f) continue;
        float next = 3.4028235e+38f;
        if (value < 3.4028235e+38f - milliseconds) {
            next = value + milliseconds;
            value = next;
        }
        value = next;
    }
    TranslatePlayerDriveCommands(state, milliseconds);
    return true;
}

bool cSC4Automaton::DoBumpedTick(float milliseconds) {
    (void)milliseconds;
    return !CheckAutomataFlag(kAutomataFlagMarkedForRemove);
}

// PARTIAL: the heading is turned towards the attraction controller's position and the step goes through the network;
// the original's terrain/network flag test (slot 0xf8, argument dropped by the decompiler) is taken to be 0x20000.
bool cSC4Automaton::DoMagnetTick(float milliseconds) {
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return false;
    tAutomataState* state = GetState();
    if (state && state->controller) {
        cS3DVector3 target{0.0f, 0.0f, 0.0f};
        state->controller->GetPosition(&target.x);
        target = target - position_;
        const float distance = Length(target);
        if (distance > 0.0001f) {
            const cS3DVector3 direction = target * (1.0f / distance);
            const float speed = GetCurrentSpeed();
            cS3DVector3 next = position_ + direction * (speed * milliseconds);
            if (!CheckAutomataFlag(kAutomataFlagIgnoreTerrain) && Host()) next.y = Host()->TerrainHeight(next.x, next.z);
            MoveTo(&next.x, &direction.x);
        }
    }
    UpdateAttraction();
    return true;
}

bool cSC4Automaton::DoScriptedTick(float milliseconds) {
    (void)milliseconds;
    return !CheckAutomataFlag(kAutomataFlagMarkedForRemove);
}

bool cSC4Automaton::DoCollideTick(float milliseconds) {
    (void)milliseconds;
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return false;
    if (IsFlagSet(kAutomatonOccupantInRenderer)) SetVisible(false);
    if (!models_.empty()) {
        float bounds[6];
        models_.front()->GetBounds(bounds, bounds + 3);
        std::memcpy(boundingBox_, bounds, sizeof(boundingBox_));
    }
    if (IsFlagSet(kAutomatonOccupantInManager) && Host()) Host()->RefreshOccupant(this);
    if (animTimer_ > 100.0f) {
        SendCallbackMessage(kAutomataEventMoveFinished, 0);
        animTimer_ = 0.0f;
    }
    return true;
}

bool cSC4Automaton::DoAttachAnimTick(float milliseconds) {
    (void)milliseconds;
    if (CheckAutomataFlag(kAutomataFlagMarkedForRemove)) return false;
    tAutomataState* state = GetState();
    if (!state) return true;
    auto* attach = static_cast<tAutomataStateAttachAnim*>(state);
    // PARTIAL: the attached animated instance's frame count drives the exit; without the animated-instance layer only
    // the state's own flag decides.
    return attach->flag40;
}

bool cSC4Automaton::TranslatePlayerDriveCommands(tAutomataStatePlayerDrive* state, float milliseconds) {
    (void)state;
    (void)milliseconds;
    // PARTIAL: the player-drive record's 0x2a0 bytes hold seven 13-entry command tables (offsets +0xdc, +0x120,
    // +0x154, +0x188, +0x1bc, +0x1f0 and the two byte tables at +0x224/+0x231); the port's tAutomataStatePlayerDrive
    // only models the history table at +0xa8, so the translation cannot run yet.
    return true;
}

bool cSC4Automaton::HandlePlayerDriveCommand(std::uint32_t command, bool on) {
    tAutomataState* found = FindState(kStatePlayerDrive);
    if (!found || command >= 13) return false;
    auto* drive = static_cast<tAutomataStatePlayerDrive*>(found);
    const float current = drive->history[command];
    if (!on) {
        if (current >= 0.0f) SendCallbackMessage(kAutomataEventPlayerDriveStop, command);
        drive->history[command] = -1.0f;
        return true;
    }
    if (current < 0.0f) {
        drive->history[command] = 0.0f;
        SendCallbackMessage(kAutomataEventPlayerDriveStart, command);
    }
    return true;
}

// ---- removal / route / collision -------------------------------------------------------------------------------------
void cSC4Automaton::Remove(bool immediate) {
    SetAutomataFlag(kAutomataFlagMarkedForRemove, true);
    SetAutomataFlag(kAutomataFlagRemoved, true);
    if (immediate) {
        SetOpacity(0.0f, 0.0f);
        if (IAutomatonEffects* effects = EnsureEffects()) effects->StopAll(false);
        return;
    }
    SendCallbackMessage(kAutomataEventRemoving, 0);
    SetOpacity(0.0f, Host() ? Host()->FadeDurationSeconds() : 0.0f);
    if (IAutomatonEffects* effects = EnsureEffects()) {
        effects->StopAll(true);
        effects->SetModel(nullptr);
    }
}

void cSC4Automaton::HandleReachedRouteDestination() {
    SetAutomataFlag(kAutomataFlagReachedDestination, true);
    SetAutomataFlag(0x20, true);   // the route-reached flag
    SendCallbackMessage(kAutomataEventRouteReached, 0);
    if (!CheckAutomataFlag(kAutomataFlagKeepAfterRoute)) Remove(false);
}

void cSC4Automaton::HandleDispatched(const cS3DVector3& position, cISC4Occupant* occupant) {
    (void)position;
    (void)occupant;
    SendCallbackMessage(kAutomataEventDispatched, 0);
}

bool cSC4Automaton::HandleRemoveController(cSC4AutomataController* controller) {
    if (!controller) return true;
    while (true) {
        tAutomataState* top = GetState();
        if (!top || top->controller != controller) break;
        PopState();
    }
    for (auto it = states_.begin(); it != states_.end();) {
        tAutomataState* state = *it;
        if (state && state->controller == controller) {
            controller->Release();
            delete state;
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
    if (attractionController_ == controller) {
        attractionController_ = nullptr;
        controller->Release();
    }
    return true;
}

bool cSC4Automaton::ShouldCollide(cSC4Automaton* other) const {
    if (!other || other == this) return false;
    if (!other->CheckAutomataFlag(kAutomataFlagCollideCandidate)) return false;
    if (other->GetMarkedForRemove()) return false;
    if (!CheckAutomataFlag(kAutomataFlagForceCollision)) return true;
    if (!other->CheckAutomataFlag(kAutomataFlagForceCollision)) {
        return (other->GetFlags() & kOccupantFlagVisible) != 0;
    }
    return true;
}

void cSC4Automaton::GetCollidePosition(cS3DVector3& out) const {
    out = position_ - orientation_ * radius_;
}

bool cSC4Automaton::GetChaseVector(cS3DVector3& out) const {
    if (GetBehavior() != kStatePursuit) return false;
    const tAutomataState* state = states_.empty() ? nullptr : states_.back();
    if (!state) return false;
    auto* chase = static_cast<const tAutomataStateChase*>(state);
    if (!chase->target) return false;
    auto* automaton = dynamic_cast<cSC4Automaton*>(chase->target);
    if (!automaton) return false;
    cS3DVector3 target;
    automaton->GetPosition(&target.x);
    out = target - position_;
    return true;
}

bool cSC4Automaton::HandleCollision(cSC4Automaton* other, const cS3DVector3& point) {
    (void)other;
    (void)point;
    return true;
}

bool cSC4Automaton::AddToCollisionSystem(tAutomataStateCollide* state) {
    (void)state;
    // PARTIAL: creates the collision effect (cSC4EffectsManager slot 0x30 with the vehicle/train collide effect) and
    // hands it the model instance and a velocity. Neither the effects manager nor the model layer is recovered.
    return false;
}

// ---- debug text ------------------------------------------------------------------------------------------------------
// PARTIAL: the original builds a long line listing every set automata flag and the extra state data.
const char* cSC4Automaton::GetDebugFlagsText() const { return ""; }
const char* cSC4Automaton::GetDebugExtraText() const { return ""; }

// ---- ghost model (the faint copy shown while an automaton is being moved) --------------------------------------------
// PARTIAL: the ghost model is created through the renderer's model layer (cS3DModelProcessor at +0x1d8).
bool cSC4Automaton::InitGhostModel() { return false; }
bool cSC4Automaton::ShutdownGhostModel() { return false; }
bool cSC4Automaton::UpdateGhostModel() { return false; }

}  // namespace sc4
