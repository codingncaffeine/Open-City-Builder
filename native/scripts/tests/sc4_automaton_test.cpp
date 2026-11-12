// cSC4Automaton — construction/QueryInterface/refcounting, the occupant plumbing, position/heading/speed state, the
// controller handshake and the behaviour state machine, the simulated step (DoAnimationTick) and the placement path.
// The model, effect, city, sound and controller-manager neighbours are fakes (the automaton only ever reaches them
// through IAutomatonHost / IAutomatonModel / IAutomatonEffects and the recovered cSC4AutomataController).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

#include "gz/CommandParameterSet.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZVariant.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/Automaton.h"
#include "sc4/GameDataRegistry.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)
#define CHECK_NEAR(a, b) do { if (std::fabs((a) - (b)) > 1e-4f) { \
    std::printf("FAIL line %d: %s == %f, expected %f\n", __LINE__, #a, static_cast<double>(a), static_cast<double>(b)); ++failures; } } while (0)

using namespace sc4;

namespace {

// ---- fakes ---------------------------------------------------------------------------------------------------------
struct FakeModel final : IAutomatonModel {
    int refs = 1;
    int addRefs = 0, releases = 0, transformSets = 0, opacitySets = 0, rendererUpdates = 0, frame = 0, frameCount = 1;
    float opacity = 1.0f;
    bool translucent = false;
    float minXyz[3] = {-1.0f, 0.0f, -2.0f};
    float maxXyz[3] = {1.0f, 3.0f, 2.0f};
    cS3DTransform transform;
    cGZPersistResourceKey playedKey{};

    void AddRef() override { ++refs; ++addRefs; }
    void Release() override { --refs; ++releases; }
    void GetBounds(float* minOut, float* maxOut) override { std::memcpy(minOut, minXyz, sizeof(minXyz)); std::memcpy(maxOut, maxXyz, sizeof(maxXyz)); }
    void GetTransform(cS3DTransform& out) override { out = transform; }
    void SetTransform(const cS3DTransform& in) override { transform = in; ++transformSets; }
    void SetOpacity(float value, bool isTranslucent) override { opacity = value; translucent = isTranslucent; ++opacitySets; }
    void UpdateAnimation(float) override { ++rendererUpdates; }
    int GetFrame() const override { return frame; }
    int GetFrameCount() const override { return frameCount; }
    void SetZoomAndRotation(int, int) override {}
    void SetPlayMode(s3d::PlayMode mode) override { playMode = static_cast<int>(mode); }
    float GetOpacity() const override { return opacity; }
    int playMode = -1;
};

struct FakeEffects final : IAutomatonEffects {
    int setModel = 0, inits = 0, updates = 0, starts = 0, stops = 0, shutdowns = 0;
    IAutomatonModel* model = nullptr;
    bool lastFade = false;
    void SetModel(IAutomatonModel* value) override { model = value; ++setModel; }
    bool Init() override { ++inits; return true; }
    void Update(const cS3DVector3&, const cS3DVector3&, float) override { ++updates; }
    void StartAll() override { ++starts; }
    void StopAll(bool fade) override { ++stops; lastFade = fade; }
    void StartNamed(const cRZString&, int, bool) override {}
    void StopNamed(const cRZString&, int) override {}
    void ToggleNamed(const cRZString&, int, bool) override {}
    void RecreateNamed(const cRZString&, int, bool) override {}
    bool AddRegEffect(const cRZString&, const cRZString&) override { return true; }
    void* FindRegEffect(const cRZString&) override { return nullptr; }
    void RemoveRegEffect(const cRZString&) override {}
    void Shutdown() override { ++shutdowns; }
};

struct FakeGroup final : IAutomatonGroupTemplate {
    std::uint32_t id = 0;
    FakeGroup* parent = nullptr;
    float speedMin = 0.0f, speedMax = 0.0f;
    int depthCalls = 0;
    std::uint32_t GroupId() const override { return id; }
    IAutomatonGroupTemplate* Parent() const override { return parent; }
    float SpeedMin() const override { return speedMin; }
    float SpeedMax() const override { return speedMax; }
    void GetGroupHierarchyDepth() override { ++depthCalls; }
};

struct FakeMessageTarget final : cIGZMessageTarget2 {
    int refs = 1;
    std::vector<std::uint32_t> events;
    std::vector<std::uint32_t> data;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cIGZMessage2* message) override {
        auto* standard = dynamic_cast<cIGZMessage2Standard*>(message);
        if (!standard) return false;
        events.push_back(static_cast<std::uint32_t>(standard->GetData2()));
        data.push_back(static_cast<std::uint32_t>(standard->GetData3()));
        return true;
    }
};

struct FakeHost final : IAutomatonHost {
    FakeModel* nextModel = nullptr;
    FakeModel* queuedModel = nullptr;
    std::vector<std::uint32_t> createdInstances;
    FakeEffects effects;
    int createdModels = 0, destroyedModels = 0;
    int inserted = 0, removed = 0, refreshed = 0, rendererAdded = 0, rendererRemoved = 0, fadesIn = 0, fadesOut = 0;
    int posted = 0;
    std::vector<std::uint32_t> messageTypes;
    bool city = true;
    float sizeX = 4096.0f, sizeZ = 4096.0f, terrainY = -3.5f;
    bool lotHere = false, networkHere = false;
    int lotCalls = 0, networkCalls = 0;
    float maxSpeedUnits = 3600.0f;   // -> 1 m/s
    float simSpeed = 1.0f;
    float fadeSeconds = 0.5f;
    bool animationAllowed = true;
    cSC4AutomataController* attraction = nullptr;
    FakeGroup group;
    FakeGroup secondary;
    double randomUnit = 0.0;
    std::uint32_t soundIds = 100;
    std::uint32_t stoppedLoops = 0;
    std::vector<cS3DVector3> networkMoves;
    double randomFraction = 0.0;   // RandomDouble answers lo + (hi - lo) * this
    int constrainCalls = 0, roadAltitudeCalls = 0;
    cS3DVector3 constrainFrom{0, 0, 0};
    std::uint32_t constrainFlags = 0;
    float constrainX = 55.0f, roadY = 7.25f;

    // answers nextModel; a queued model takes its place for the calls after
    IAutomatonModel* CreateModel(const cGZPersistResourceKey& key) override {
        ++createdModels;
        createdInstances.push_back(key.instance);
        IAutomatonModel* model = nextModel;
        if (queuedModel) {
            nextModel = queuedModel;
            queuedModel = nullptr;
        }
        return model;
    }
    IAutomatonModel* CreateAnimatedModel(const cGZPersistResourceKey& key) override { ++createdAnimated; return CreateModel(key); }
    int createdAnimated = 0;
    void DestroyModel(IAutomatonModel* model) override { ++destroyedModels; if (model) model->Release(); }
    IAutomatonEffects* CreateEffects() override { return &effects; }
    void DestroyEffects(IAutomatonEffects*) override {}
    bool AddModelToRenderer(IAutomatonModel*) override { ++rendererAdded; return true; }
    bool RemoveModelFromRenderer(IAutomatonModel*) override { ++rendererRemoved; return true; }
    void MoveModelInRenderer(IAutomatonModel*) override { ++fadesIn; }
    void ChangeModelInRenderer(IAutomatonModel*, int) override { ++fadesOut; }
    bool InsertOccupant(cISC4Occupant*) override { ++inserted; return true; }
    void RemoveOccupant(cISC4Occupant*) override { ++removed; }
    bool RefreshOccupant(cISC4Occupant*) override { ++refreshed; return true; }
    bool HasCity() override { return city; }
    float CitySizeX() override { return sizeX; }
    float CitySizeZ() override { return sizeZ; }
    float TerrainHeight(float, float) override { return terrainY; }
    bool NetworkAt(int, int) override { ++networkCalls; return networkHere; }
    bool LotAt(int, int) override { ++lotCalls; return lotHere; }
    void PlaceOnNetwork(const cS3DVector3& from, cS3DVector3& to, std::uint32_t) override { networkMoves.push_back(from); networkMoves.push_back(to); }
    cSC4AutomataController* FindAttraction(cISC4Occupant*, cS3DVector3*, float* radius, float* distance) override {
        if (!attraction) return nullptr;
        *radius = 10.0f;
        *distance = 2.0f;
        return attraction;
    }
    IAutomatonGroupTemplate* FindAutomataGroup(std::uint32_t id) override {
        if (id == group.id) return &group;
        if (id == secondary.id) return &secondary;
        return nullptr;
    }
    bool AnimationUpdateAllowed() override { return animationAllowed; }
    float SimSpeedModifier() override { return simSpeed; }
    float MaxSpeedUnits() override { return maxSpeedUnits; }
    float FadeDurationSeconds() override { return fadeSeconds; }
    void PostMessage(cIGZMessage2* message) override { ++posted; messageTypes.push_back(message->GetType()); }
    std::uint32_t GenerateSoundInstanceId() override { return ++soundIds; }
    void StopSoundLoop(std::uint32_t) override { ++stoppedLoops; }
    void PostAudioEvent(std::uint32_t) override {}
    std::uint32_t RandomBelow(std::uint32_t range) override { return range ? 0 : 0; }
    std::int32_t RandomInt(std::int32_t lo, std::int32_t) override { return lo; }
    double RandomDouble(double lo, double hi) override { return lo + (hi - lo) * randomFraction; }
    bool ConstrainVectorToNetworkTile(const cS3DVector3& from, cS3DVector3& point, std::uint32_t flags) override {
        ++constrainCalls;
        constrainFrom = from;
        constrainFlags = flags;
        point.x = constrainX;
        return true;
    }
    float RoadAltitude(float, float) override { ++roadAltitudeCalls; return roadY; }
    double RandomUnit() override { return randomUnit; }
};

// The recovered controller class, stubbed at the slots the automaton calls.
struct FakeController final : cSC4AutomataController {
    int refs = 1;
    int ticks = 0, enters = 0, exits = 0, released = 0;
    bool consumeTick = false;
    std::uint32_t lastState = 0;
    const tBehaviorTemplate* behavior = nullptr;
    cISC4Occupant* occupant = nullptr;

    std::uint32_t AddRef() override { return static_cast<std::uint32_t>(++refs); }
    std::uint32_t Release() override { ++released; return static_cast<std::uint32_t>(--refs); }
    bool CheckFlag(std::uint32_t) const override { return false; }
    void GetPosition(float* xyz) const override { xyz[0] = 100.0f; xyz[1] = 0.0f; xyz[2] = 0.0f; }
    cISC4Occupant* GetOccupant() const override { return occupant; }
    bool HandleStateTick(IControllerAutomaton*, std::uint32_t state, float) override { ++ticks; lastState = state; return consumeTick; }
    bool HandleEnterState(IControllerAutomaton*, std::uint32_t state) override { ++enters; lastState = state; return false; }
    bool HandleExitState(IControllerAutomaton*, std::uint32_t state) override { ++exits; lastState = state; return false; }
    const tBehaviorTemplate* GetBehaviorTemplate(float, const float*) const override { return behavior; }
};

cSC4Automaton* MakeAutomaton(FakeHost& host) {
    auto* automaton = new cSC4Automaton(&host);
    automaton->AddRef();
    return automaton;
}

}  // namespace

int main() {
    // ---- construction and QueryInterface --------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        CHECK(automaton->GetType() == kAutomatonOccupantType);
        CHECK(automaton->IsFlagSet(kAutomatonOccupantInManager) == false);
        CHECK_NEAR(automaton->GetRadius(), 0.0f);
        CHECK_NEAR(automaton->GetDefaultSpeed(), 1.0f / 36.0f);
        float orientation[3] = {9, 9, 9};
        CHECK(automaton->GetOrientation(orientation));
        CHECK_NEAR(orientation[0], 1.0f);
        CHECK_NEAR(orientation[1], 0.0f);
        CHECK_NEAR(orientation[2], 0.0f);
        CHECK(automaton->AutomataFlags() == 0);
        CHECK(!automaton->IsSavedFlagSet());
        CHECK(automaton->StateStack().empty());
        CHECK(automaton->GetState() == nullptr);
        CHECK(automaton->GetBehavior() == 0);
        CHECK(automaton->FindState(7) == nullptr);
        CHECK(automaton->PrevState(nullptr) == nullptr);
        CHECK(automaton->GetPriority() == 0);
        CHECK(automaton->GetCallNumber() == 0);
        CHECK(automaton->GetDispatchState() == 3);
        CHECK(automaton->GetDispatchInfoID() == 0);
        automaton->SetDispatchInfoID(0x1234);
        CHECK(automaton->GetDispatchInfoID() == 0x1234);
        automaton->SetPriority(7);
        CHECK(automaton->GetPriority() == 7);
        automaton->SetCallNumber(12);   // a no-op in the original
        CHECK(automaton->GetCallNumber() == 0);
        CHECK(automaton->FindDrivingAutomaton() == automaton);
        CHECK(automaton->GetCollidingAutomaton(1.0f) == nullptr);
        automaton->RecalculateLeaders();
        CHECK(automaton->HandleCollision(nullptr, cS3DVector3{}));

        cISC4Occupant* occupant = nullptr;
        CHECK(automaton->QueryInterface(1, reinterpret_cast<void**>(&occupant)));
        CHECK(occupant == automaton);
        occupant->Release();
        cIGZMessageTarget2* target = nullptr;
        CHECK(automaton->QueryInterface(0x452294aa, reinterpret_cast<void**>(&target)));
        CHECK(target == automaton);
        target->Release();
        cIGZUnknown* automatonInterface = nullptr;
        CHECK(automaton->QueryInterface(kAutomatonIid, reinterpret_cast<void**>(&automatonInterface)));
        CHECK(automatonInterface != nullptr);
        automatonInterface->Release();
        void* unknown = nullptr;
        CHECK(!automaton->QueryInterface(0xdeadbeef, &unknown));
        CHECK(automaton->Release() == 0);
    }

    // ---- position / heading / destination ---------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        const float pos[3] = {10.0f, 2.0f, -3.0f};
        automaton->SetPosition(pos);
        float out[3] = {};
        automaton->GetPosition(out);
        CHECK_NEAR(out[0], 10.0f);
        CHECK_NEAR(out[1], 2.0f);
        CHECK_NEAR(out[2], -3.0f);
        const float dest[3] = {1.0f, 0.0f, 2.0f};
        automaton->SetDestination(dest);
        automaton->GetDestination(out);
        CHECK_NEAR(out[2], 2.0f);
        const float heading[3] = {0.0f, 0.0f, 5.0f};
        automaton->SetOrientation(heading);
        automaton->GetOrientation(out);
        CHECK_NEAR(out[2], 1.0f);   // normalised
        cS3DTransform transform;
        CHECK(!automaton->GetTransform(transform));   // no model instance
        // A model instance follows the position and the heading.
        FakeModel* model = new FakeModel();
        host.nextModel = model;
        CHECK(automaton->AddModelResourceKey(cGZPersistResourceKey{0x1111, 0x2222, 0x3333}));
        CHECK(automaton->AddModelInstance(cGZPersistResourceKey{0x1111, 0x2222, 0x3333}) == model);
        CHECK(automaton->GetModelInstance() == model);
        automaton->FinishMove(false, false);   // refreshes the bounding box from the model
        CHECK(automaton->AddModelResourceKey(cGZPersistResourceKey{0x4444, 0x5555, 0x6666}));
        CHECK(automaton->ModelKeys().size() == 2);
        // the first key with an instance id becomes the primary one
        CHECK(automaton->PrimaryModelKey().type == 0x1111);
        CHECK(automaton->PrimaryModelInstanceId() == 0x3333);
        CHECK(automaton->GetModelResourceKey(1).type == 0x4444);
        CHECK(automaton->GetModelResourceKey(5).type == 0);
        CHECK(automaton->GetModelResourceKey(-1).type == 0);
        CHECK(automaton->GetTransform(transform));
        automaton->SetZoomAndRotation(2, 3);
        float minBox[3] = {}, maxBox[3] = {};
        automaton->GetBoundingBox(minBox, maxBox);
        CHECK_NEAR(minBox[0], -1.0f);
        CHECK_NEAR(maxBox[2], 2.0f);
        // GetPosition goes through the model: the occupant flags are not both set yet
        automaton->GetBoundingBox(minBox, maxBox);
        cS3DVector3 modelPosition;
        modelPosition.x = model->transform.translation[0];
        modelPosition.z = model->transform.translation[2];
        CHECK_NEAR(modelPosition.x, 10.0f);
        CHECK_NEAR(modelPosition.z, -3.0f);
        // SetTransform goes to every model
        cS3DTransform replacement;
        replacement.translation[0] = 7.0f;
        automaton->SetTransform(replacement);
        CHECK_NEAR(model->transform.translation[0], 7.0f);
        // GetCollidePosition steps back along the heading by the radius
        automaton->SetRadius(4.0f);
        cS3DVector3 collide;
        automaton->GetCollidePosition(collide);
        CHECK_NEAR(collide.z, -3.0f - 4.0f);
        CHECK(automaton->Models().size() == 1);
        CHECK(automaton->Release() == 0);
    }

    // ---- MoveTo: heading + position, model placement, NaN bounds check, avoid lots/roads ----------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        const float direction[3] = {0.0f, 0.0f, 2.0f};
        const float position[3] = {100.0f, 5.0f, 200.0f};
        automaton->MoveTo(position, direction);
        CHECK_NEAR(automaton->Position().x, 100.0f);
        CHECK_NEAR(automaton->Orientation().z, 1.0f);
        // a zero direction leaves the heading alone
        const float zero[3] = {0.0f, 0.0f, 0.0f};
        automaton->MoveTo(position, zero);
        CHECK_NEAR(automaton->Orientation().z, 1.0f);
        // NaN position outside the city removes the automaton
        const float outside[3] = {std::nanf(""), 0.0f, 5000.0f};
        automaton->MoveTo(outside, nullptr);
        CHECK(automaton->GetMarkedForRemove());
        CHECK(automaton->CheckAutomataFlag(kAutomataFlagRemoved));
        CHECK(automaton->Release() == 0);
    }
    {
        FakeHost host;
        host.lotHere = true;
        cSC4Automaton* automaton = MakeAutomaton(host);
        automaton->SetAutomataFlag(kAutomataFlagAvoidLots, true);
        const float position[3] = {100.0f, 0.0f, 100.0f};   // 100 / 16 = 6.25 -> cell 6
        automaton->MoveTo(position, nullptr);
        CHECK(host.lotCalls == 1);
        CHECK(automaton->GetMarkedForRemove());
        CHECK(automaton->Release() == 0);
    }
    {
        FakeHost host;
        host.networkHere = true;
        cSC4Automaton* automaton = MakeAutomaton(host);
        automaton->SetAutomataFlag(kAutomataFlagAvoidRoads, true);
        const float position[3] = {160.0f, 0.0f, 96.0f};
        automaton->MoveTo(position, nullptr);
        CHECK(host.networkCalls == 1);
        CHECK(automaton->GetMarkedForRemove());
        CHECK(automaton->Release() == 0);
    }

    // ---- speed / opacity tweens ------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        CHECK_NEAR(automaton->GetCurrentSpeed(), 1.0f / 36.0f);
        CHECK_NEAR(automaton->GetOpacity(), 1.0f);
        CHECK_NEAR(automaton->GetMaxSpeed(), 1.0f);   // 3600 units * 1/3600
        CHECK_NEAR(automaton->GetSpeedModifier(), 1.0f);
        automaton->SetAutomataFlag(kAutomataFlagIgnoreSpeedModifier, true);
        CHECK_NEAR(automaton->GetSpeedModifier(), 1.0f);
        host.simSpeed = -2.0f;
        CHECK_NEAR(automaton->GetSpeedModifier(), 0.0f);
        host.simSpeed = 1.0f;
        host.maxSpeedUnits = 0.0f;
        CHECK_NEAR(automaton->GetMaxSpeed(), 0.0f);
        // a 1 s tween from 1/36 to 1: half way after 500 ms
        automaton->SetSpeed(1.0f, 1.0f);
        CHECK_NEAR(automaton->GetCurrentSpeed(), 1.0f / 36.0f);
        automaton->UpdateOpacity(0.0f);
        automaton->DoAnimationTick(0, 500.0f);
        CHECK_NEAR(automaton->GetCurrentSpeed(), (1.0f / 36.0f + 1.0f) * 0.5f);
        automaton->DoAnimationTick(0, 600.0f);
        CHECK_NEAR(automaton->GetCurrentSpeed(), 1.0f);
        // opacity tween: SetOpacity drives the models immediately for short durations
        FakeModel* model = new FakeModel();
        host.nextModel = model;
        automaton->AddModelInstance(cGZPersistResourceKey{1, 2, 3});
        automaton->SetOpacity(0.5f, 0.0f);
        CHECK_NEAR(model->opacity, 0.5f);
        CHECK(model->translucent);
        automaton->SetOpacity(1.0f, 2.0f);
        CHECK_NEAR(automaton->GetOpacity(), 0.5f);
        automaton->UpdateOpacity(1000.0f);
        CHECK_NEAR(automaton->GetOpacity(), 0.75f);
        automaton->UpdateOpacity(2000.0f);
        CHECK_NEAR(automaton->GetOpacity(), 1.0f);
        CHECK(automaton->Release() == 0);
    }

    // ---- the behaviour state stack ---------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeController controller;
        CHECK(automaton->PushState(kStateIdleTick, &controller, 10.0f, false, 0, 0, nullptr));
        CHECK(automaton->StateStack().size() == 1);
        tAutomataState* top = automaton->GetState();
        CHECK(top != nullptr);
        CHECK(top->stateType == kStateIdleTick);
        CHECK(top->stateId == 0);
        CHECK(top->controller == &controller);
        CHECK_NEAR(top->timer, 10.0f);
        CHECK(controller.enters == 1);
        // pushing the same state from the same controller answers false
        CHECK(!automaton->PushState(kStateIdleTick, &controller, 10.0f, false, 0, 0, nullptr));
        // a second state on top
        CHECK(automaton->PushState(kStateScripted, nullptr, 0.0f, false, 0, 0, nullptr));
        CHECK(automaton->StateStack().size() == 2);
        CHECK(automaton->FindState(kStateIdleTick) == automaton->StateStack().front());
        CHECK(automaton->PrevState(automaton->GetState()) == automaton->StateStack().front());
        CHECK(automaton->PrevState(automaton->StateStack().front()) == nullptr);
        CHECK(automaton->GetBehavior() == kStateScripted);
        // SetState pops every unflagged state (both) and then pushes the new one
        CHECK(automaton->SetState(kStateMagnet, nullptr, 0.0f, false, 0, 0, nullptr));
        CHECK(automaton->StateStack().size() == 1);
        CHECK(automaton->GetState()->stateType == kStateMagnet);
        // a flagged top state stops SetState
        automaton->GetState()->flag = true;
        CHECK(!automaton->SetState(kStateScripted, nullptr, 0.0f, false, 0, 0, nullptr));
        automaton->GetState()->flag = false;
        // PopState runs the exit path and releases the controller of the popped record
        CHECK(automaton->PopState());
        CHECK(automaton->StateStack().empty());
        CHECK(controller.exits >= 1);
        // ClearStateStack releases the controllers
        automaton->ClearStateStack();
        CHECK(automaton->StateStack().empty());
        CHECK(automaton->GetState() == nullptr);
        CHECK(controller.released >= 1);

        // SetState with a flagged top answers false; pop it to get past
        automaton->ClearStateStack();
        tAutomataState* flagged = automaton->CreateAutomataState(kStateScripted);
        CHECK(flagged && flagged->stateType == kStateScripted);
        flagged->flag = true;
        automaton->StateStack().push_back(flagged);
        CHECK(!automaton->SetState(kStateMagnet, nullptr, 0.0f, false, 0, 0, nullptr));
        // PopState of a flagged, non-idle state pushes an idle state instead of deleting it (quirk: the flagged record
        // stays in the stack under the idle one)
        CHECK(automaton->PopState());
        CHECK(automaton->StateStack().size() == 2);
        CHECK(automaton->GetState() != nullptr);
        CHECK(automaton->GetState()->stateType == kStateIdle);
        automaton->ClearStateStack();
        CHECK(automaton->Release() == 0);
    }

    // ---- the attraction controller is a counted member ----------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeController attractor;
        host.attraction = &attractor;
        automaton->UpdateAttraction();
        CHECK(attractor.refs == 2);            // the automaton keeps its own reference while it is attracted
        automaton->UpdateAttraction();
        CHECK(attractor.refs == 2);            // the same controller again: nothing changes
        host.attraction = nullptr;
        automaton->UpdateAttraction();         // out of reach: only the automaton's reference goes
        CHECK(attractor.refs == 1);
        host.attraction = &attractor;
        automaton->UpdateAttraction();
        CHECK(attractor.refs == 2);
        CHECK(automaton->Release() == 0);      // the destructor gives it back
        CHECK(attractor.refs == 1);
    }

    // ---- the random walk (i386 0x000c4b58) ----------------------------------------------------------------------
    {   // walking without an idle delay: the turn, the steering blend, the step and the road height
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        tBehaviorTemplate walk;
        walk.delay[0] = walk.delay[1] = 2.0f;
        walk.turn[0] = 0.25f;
        walk.turn[1] = 0.0f;
        walk.idleDelay[0] = walk.idleDelay[1] = 0.0f;
        walk.ignoreRoads = true;
        walk.ignoreAttractors = true;
        const float start[3] = {100.0f, 0.0f, 100.0f};
        automaton->SetPosition(start);
        CHECK(automaton->PushState(kStateRandomWalk, nullptr, -1.0f, false, 0, 0, &walk));
        auto* state = dynamic_cast<tAutomataStateRandomWalk*>(automaton->GetState());
        CHECK(state != nullptr);
        if (state) {
            CHECK_NEAR(state->walkTimer, -0.01f);
            CHECK_NEAR(state->turnTimer, 0.0f);
            CHECK_NEAR(state->direction.x, 1.0f);
            CHECK(!state->idleAnimStarted);
            CHECK(automaton->DoStateTick(16.0f));      // the instant of standing ends: the walk begins, nothing moves yet
            CHECK_NEAR(state->walkTimer, 0.0f);
            float p[3];
            automaton->GetPosition(p);
            CHECK_NEAR(p[0], 100.0f);
            automaton->SetSpeed(0.01f, 0.0f);
            const float speed = automaton->GetCurrentSpeed() * automaton->GetSpeedModifier();
            CHECK(speed > 0.0f);
            host.randomFraction = 1.0;                  // the longest wait, the widest turn to the positive side
            CHECK(automaton->DoStateTick(100.0f));
            CHECK_NEAR(state->walkTimer, 0.0f);         // no idle delay: the walk timer is left alone
            CHECK_NEAR(state->turnTimer, 1.9f);
            CHECK(std::fabs(state->direction.x) < 1e-3f && std::fabs(state->direction.z - 1.0f) < 1e-3f && state->direction.y == 0.0f);
            const float length = std::sqrt(1.0f + 0.2f * 0.2f);   // (1, 0, 0) + 100 / 500 * (0, 0, 1)
            float o[3];
            automaton->GetOrientation(o);
            CHECK(std::fabs(o[0] - 1.0f / length) < 1e-3f && std::fabs(o[2] - 0.2f / length) < 1e-3f);
            automaton->GetPosition(p);
            CHECK(std::fabs(p[0] - (100.0f + speed * 100.0f / length)) < 1e-3f);
            CHECK(std::fabs(p[2] - (100.0f + speed * 100.0f * 0.2f / length)) < 1e-3f);
            CHECK_NEAR(p[1], 7.25f);                    // ignoring roads: the road (or ground) height
            CHECK(host.roadAltitudeCalls == 1 && host.constrainCalls == 0);
            // a turn to the negative side also takes the least turn (turn[1]) that way: -(0.25 + 0.125) of a circle
            walk.turn[1] = 0.125f;
            host.randomFraction = 0.0;
            state->direction = {1.0f, 0.0f, 0.0f};
            state->turnTimer = 0.05f;
            CHECK(automaton->DoStateTick(100.0f));
            CHECK_NEAR(state->turnTimer, 1.95f);
            CHECK(std::fabs(state->direction.x - std::cos(-0.375f * 6.2831855f)) < 1e-3f);
            CHECK(std::fabs(state->direction.z - std::sin(-0.375f * 6.2831855f)) < 1e-3f);
            // not ignoring roads: the step is kept on the network tile (flags 0xc49), from the old position
            automaton->SetAutomataFlag(kAutomataFlagIgnoreRoads, false);
            automaton->GetPosition(p);
            const float beforeX = p[0];
            CHECK(automaton->DoStateTick(100.0f));
            CHECK(host.constrainCalls == 1 && host.constrainFlags == 0xc49 && host.roadAltitudeCalls == 2);
            CHECK_NEAR(host.constrainFrom.x, beforeX);
            automaton->GetPosition(p);
            CHECK_NEAR(p[0], 55.0f);
        }
        automaton->Shutdown();
        automaton->Release();
    }
    {   // the walk / stand cycle: idle_delay seconds of walking, then idle_time seconds standing with the idle animation
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        tBehaviorTemplate walk;
        walk.idleDelay[0] = walk.idleDelay[1] = 3.0f;
        walk.idleTime[0] = walk.idleTime[1] = 1.0f;
        walk.kph[0] = walk.kph[1] = 36;
        walk.acceleration = 0.0f;
        walk.deceleration = 0.0f;
        walk.ignoreRoads = true;
        CHECK(automaton->PushState(kStateRandomWalk, nullptr, -1.0f, false, 0, 0, &walk));
        auto* state = dynamic_cast<tAutomataStateRandomWalk*>(automaton->GetState());
        CHECK(state != nullptr);
        if (state) {
            CHECK(automaton->DoStateTick(16.0f));
            CHECK_NEAR(state->walkTimer, 3.0f);         // the walk's length, and the behavior's speed
            CHECK_NEAR(automaton->GetCurrentSpeed(), 36.0f / 3600.0f);
            CHECK(automaton->DoStateTick(1000.0f));
            CHECK_NEAR(state->walkTimer, 2.0f);
            CHECK(automaton->DoStateTick(2500.0f));     // the walk is over: stand for idle_time, slowing to a stop
            CHECK_NEAR(state->walkTimer, -1.0f);
            CHECK_NEAR(automaton->GetCurrentSpeed(), 0.0f);
            float before[3], after[3];
            automaton->GetPosition(before);
            CHECK(automaton->DoStateTick(100.0f));
            CHECK_NEAR(state->walkTimer, -0.9f);
            CHECK(state->idleAnimStarted);              // slower than 0.01: the idle animation starts once
            automaton->GetPosition(after);
            CHECK(before[0] == after[0] && before[2] == after[2]);
            CHECK(automaton->DoStateTick(1000.0f));     // the stand is over: the next walk
            CHECK_NEAR(state->walkTimer, 3.0f);
            CHECK(!state->idleAnimStarted);
            CHECK_NEAR(automaton->GetCurrentSpeed(), 36.0f / 3600.0f);
        }
        automaton->Shutdown();
        automaton->Release();
    }

    // ---- the state machine tick ------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeController controller;
        // an idle state (7) runs DoIdleTick -> UpdateAttraction
        CHECK(automaton->PushState(kStateIdleTick, nullptr, 0.0f, false, 0, 0, nullptr));
        CHECK(automaton->DoStateTick(16.0f));
        // the timer expiry pops the state (the timer is in milliseconds, like the tick)
        automaton->ClearStateStack();
        CHECK(automaton->PushState(kStateScripted, nullptr, 2000.0f, false, 0, 0, nullptr));
        CHECK(automaton->DoStateTick(500.0f));
        CHECK(automaton->StateStack().size() == 1);
        CHECK(automaton->DoStateTick(2000.0f));
        CHECK(automaton->StateStack().empty());
        // a behavior's timeout (seconds) becomes the state's timer in milliseconds; none is -1
        {
            tBehaviorTemplate timed;
            timed.state = kStateScripted;
            timed.timeout = 60.0f;
            CHECK(automaton->PushBehavior(&timed, nullptr));
            CHECK(automaton->GetState() && automaton->GetState()->timer == 60000.0f);
            CHECK(automaton->DoStateTick(500.0f));
            CHECK(automaton->StateStack().size() == 1);   // a minute does not end on the first tick
            automaton->ClearStateStack();
            tBehaviorTemplate untimed;
            untimed.state = kStateScripted;
            CHECK(untimed.GetTimeoutMilliseconds() == -1.0f);
            CHECK(automaton->PushBehavior(&untimed, nullptr));
            CHECK(automaton->GetState() && automaton->GetState()->timer == -1.0f);
            automaton->ClearStateStack();
        }
        // the controller gets HandleStateTick first and can consume the tick
        CHECK(automaton->PushState(kStateQueue, &controller, 0.0f, false, 0, 0, nullptr));
        controller.consumeTick = true;
        CHECK(automaton->DoStateTick(16.0f));
        CHECK(controller.ticks == 1);
        controller.consumeTick = false;
        CHECK(!automaton->DoStateTick(16.0f));   // the queue state has no tick function
        // an unknown state id answers false
        automaton->ClearStateStack();
        CHECK(automaton->PushState(0x77, nullptr, 0.0f, false, 0, 0, nullptr));
        CHECK(!automaton->DoStateTick(16.0f));
        automaton->ClearStateStack();
        // HandleRemoveController drops the controller's states and releases it
        CHECK(automaton->PushState(kStateQueue, &controller, 0.0f, false, 0, 0, nullptr));
        CHECK(automaton->HandleRemoveController(&controller));
        CHECK(automaton->StateStack().empty());
        CHECK(controller.released >= 1);
        CHECK(automaton->Release() == 0);
    }

    // ---- CreateAutomataState ---------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        struct Case { std::uint32_t state; bool randomWalk, plummet, plop, drive, collide, chase; };
        const Case cases[] = {
            {kStateRandomWalk, true, false, false, false, false, false},
            {kStatePlummet, false, true, false, false, false, false},
            {kStatePlopJump, false, false, true, false, false, false},
            {kStatePlayerDrive, false, false, false, true, false, false},
            {kStateCollide, false, false, false, false, true, false},
            {kStatePursuit, false, false, false, false, false, true},
            {kStateScripted, false, false, false, false, false, false},
        };
        for (const Case& entry : cases) {
            tAutomataState* state = automaton->CreateAutomataState(entry.state);
            CHECK(state != nullptr);
            CHECK(state->stateType == entry.state);
            CHECK((dynamic_cast<tAutomataStateRandomWalk*>(state) != nullptr) == entry.randomWalk);
            CHECK((dynamic_cast<tAutomataStatePlummet*>(state) != nullptr) == entry.plummet);
            CHECK((dynamic_cast<tAutomataStatePlopJump*>(state) != nullptr) == entry.plop);
            CHECK((dynamic_cast<tAutomataStatePlayerDrive*>(state) != nullptr) == entry.drive);
            CHECK((dynamic_cast<tAutomataStateCollide*>(state) != nullptr) == entry.collide);
            CHECK((dynamic_cast<tAutomataStateChase*>(state) != nullptr) == entry.chase);
            delete state;
        }
        CHECK(automaton->Release() == 0);
    }

    // ---- the simulated step: walking to the destination ------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeMessageTarget callback;
        callback.AddRef();
        automaton->AddCallback(&callback);
        CHECK(automaton->Callbacks().size() == 1);
        automaton->AddCallback(&callback);   // deduplicated
        CHECK(automaton->Callbacks().size() == 1);
        automaton->SetSelfControlling(true);
        automaton->SetAutomataFlag(kAutomataFlagIgnoreRoads, true);
        automaton->SetAutomataFlag(kAutomataFlagIgnoreTerrain, false);
        automaton->SetSpeed(1.0f, 0.0f);
        automaton->SetRadius(2.0f);
        const float position[3] = {0.0f, 0.0f, 0.0f};
        automaton->SetPosition(position);
        const float destination[3] = {1000.0f, 0.0f, 0.0f};
        automaton->SetDestination(destination);
        CHECK(automaton->DoAnimationTick(0, 1000.0f));
        // the original steps position += heading * speed * dt, with dt in milliseconds (so 1 m/s * 1000 ms)
        CHECK_NEAR(automaton->Position().x, 1000.0f);
        CHECK_NEAR(automaton->GetActualSpeed(), 1.0f);
        CHECK_NEAR(automaton->Position().y, -3.5f);   // terrain height applied
        CHECK(callback.events.size() >= 2);   // the tick plus the state events
        CHECK(callback.events[callback.events.size() - 1] == kAutomataEventTick);
        // jump to the destination: the next tick marks it reached
        automaton->SetPosition(destination);
        automaton->DoAnimationTick(0, 100.0f);
        CHECK(automaton->GetReachedDestination());
        CHECK(automaton->CheckAutomataFlag(kAutomataFlagReachedDestination));
        // the whole flow sends the destination-reached event
        bool sawReached = false;
        for (std::uint32_t event : callback.events) sawReached = sawReached || event == kAutomataEventDestinationReached;
        CHECK(sawReached);
        // RemoveCallback stops the delivery
        const std::size_t before = callback.events.size();
        automaton->RemoveCallback(&callback);
        CHECK(automaton->Callbacks().empty());
        automaton->SendCallbackMessage(kAutomataEventTick, 0);
        CHECK(callback.events.size() == before);
        callback.Release();
        CHECK(automaton->Release() == 0);
    }

    // ---- placement in the occupant manager / renderer ---------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeModel* model = new FakeModel();
        host.nextModel = model;
        automaton->AddModelInstance(cGZPersistResourceKey{1, 2, 3});
        automaton->SetRadius(1.0f);
        // the renderer path first inserts into the occupant manager
        CHECK(!automaton->PlaceInRenderer(true));   // quirk: the add path answers false
        CHECK(host.inserted == 1);
        CHECK(host.rendererAdded == 1);
        CHECK(automaton->IsFlagSet(kAutomatonOccupantInManager));
        CHECK(automaton->IsFlagSet(kAutomatonOccupantInRenderer));
        // adding again is a no-op that answers true
        CHECK(automaton->PlaceInRenderer(true));
        CHECK(host.inserted == 1);
        // removing goes through the renderer and the occupant manager
        CHECK(automaton->PlaceInOccupantManager(false));
        CHECK(host.removed == 1);
        CHECK(host.rendererRemoved == 1);
        CHECK(!automaton->IsFlagSet(kAutomatonOccupantInManager));
        // FinishMove of a visible automaton registers it and fades the model in
        automaton->SetVisible(true);
        host.fadesIn = 0;
        automaton->FinishMove(true, false);
        CHECK(host.fadesIn >= 0);
        CHECK(automaton->Release() == 0);
    }

    // ---- PlayAnim swaps the first model, and swaps it in the renderer when the automaton is placed there ----------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeModel* first = new FakeModel();
        host.nextModel = first;
        automaton->AddModelInstance(cGZPersistResourceKey{1, 2, 3});
        // not in the renderer: the model is swapped and nothing reaches the renderer
        FakeModel* second = new FakeModel();
        host.nextModel = second;
        CHECK(automaton->PlayAnim(cGZPersistResourceKey{1, 2, 4}));
        CHECK(automaton->GetModelInstance() == second);
        CHECK(host.rendererAdded == 0);
        CHECK(host.rendererRemoved == 0);
        CHECK(host.destroyedModels == 1);
        // in the renderer: the old instance leaves it and the new one enters
        automaton->SetRadius(1.0f);
        automaton->PlaceInRenderer(true);
        CHECK(host.rendererAdded == 1);
        FakeModel* third = new FakeModel();
        host.nextModel = third;
        CHECK(automaton->PlayAnim(cGZPersistResourceKey{1, 2, 5}));
        CHECK(automaton->GetModelInstance() == third);
        CHECK(host.rendererRemoved == 1);
        CHECK(host.rendererAdded == 2);
        CHECK(host.destroyedModels == 2);
        CHECK(automaton->Release() == 0);
    }

    // ---- callbacks and sound ---------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        CHECK(!automaton->StartSFX(0));
        CHECK(automaton->StartSFX(0x1234));
        CHECK(host.messageTypes.size() == 1);
        CHECK(host.messageTypes[0] == 0x8a385dd8);
        CHECK(automaton->StopSFX(0x1234));
        CHECK(host.messageTypes.size() == 2);
        CHECK(host.messageTypes[1] == 0xaa396960);
        automaton->EnableActiveSound(true);
        CHECK(automaton->IsActiveSoundEnabled());
        automaton->EnableActiveSound(false);
        CHECK(!automaton->IsActiveSoundEnabled());
        CHECK(automaton->Release() == 0);
    }

    // ---- collision / OBB -------------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        cSC4Automaton* other = MakeAutomaton(host);
        other->SetAutomataFlag(kAutomataFlagCollideCandidate, true);
        CHECK(!automaton->ShouldCollide(nullptr));
        CHECK(!automaton->ShouldCollide(automaton));
        CHECK(automaton->ShouldCollide(other));
        other->SetMarkedForRemove(true);
        CHECK(!automaton->ShouldCollide(other));
        other->SetMarkedForRemove(false);
        automaton->SetAutomataFlag(kAutomataFlagForceCollision, true);
        CHECK(automaton->ShouldCollide(other));
        other->SetAutomataFlag(kAutomataFlagForceCollision, true);
        CHECK(automaton->ShouldCollide(other));
        // chase vector needs a pursuit state
        cS3DVector3 out;
        CHECK(!automaton->GetChaseVector(out));
        CHECK(other->Release() == 0);
        CHECK(automaton->Release() == 0);
    }
    {
        FakeModel first;
        FakeModel second;
        first.minXyz[0] = -2.0f; first.minXyz[2] = -2.0f;
        first.maxXyz[0] = 2.0f;  first.maxXyz[2] = 2.0f;
        second.minXyz[0] = 0.0f; second.minXyz[2] = 0.0f;
        second.maxXyz[0] = 4.0f; second.maxXyz[2] = 4.0f;
        AutomataOBB a;
        a.valid = true;
        a.centerX = 0.0f; a.centerZ = 0.0f; a.halfX = 2.0f; a.halfZ = 2.0f; a.dirX = 1.0f; a.dirZ = 0.0f;
        AutomataOBB b = a;
        CHECK(a.TestForCollision(b));
        b.centerX = 10.0f;
        CHECK(!a.TestForCollision(b));
        AutomataOBB invalid;
        CHECK(!invalid.TestForCollision(a));
    }

    // ---- message target: the chase state's target is gone -----------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        auto* chase = static_cast<tAutomataStateChase*>(automaton->CreateAutomataState(kStatePursuit));
        auto* target = new FakeMessageTarget();
        target->AddRef();
        chase->target = target;
        automaton->StateStack().push_back(chase);
        // a message of another type is ignored
        cRZMessage2Standard otherMessage;
        otherMessage.SetType(0x12345678);
        CHECK(!automaton->DoMessage(&otherMessage));
        // build the message through the standard class: data2 == 2 and void1 == the chase target
        cRZMessage2Standard message;
        message.SetType(0x09d92fc3);
        message.SetData2(2);
        message.SetVoid1(target);
        CHECK(automaton->DoMessage(&message));
        CHECK(automaton->StateStack().empty());
        CHECK(target->refs == 1);   // the released reference
        target->Release();
        CHECK(automaton->Release() == 0);
    }

    // ---- Write / Read round trip -----------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        const float position[3] = {10.0f, 1.0f, 20.0f};
        const float destination[3] = {30.0f, 0.0f, 40.0f};
        const float orientation[3] = {0.0f, 1.0f, 0.0f};
        automaton->SetPosition(position);
        automaton->SetDestination(destination);
        automaton->SetOrientation(orientation);
        automaton->SetRadius(3.0f);
        automaton->SetAutomataFlag(kAutomataFlagSpecialRoute, true);
        automaton->SetDispatchInfoID(0xabcdef);
        automaton->SetPriority(9);
        automaton->SetSpeed(2.0f, 0.0f);
        automaton->AddModelResourceKey(cGZPersistResourceKey{7, 8, 9});
        automaton->PushState(kStateScripted, nullptr, 4.0f, false, 0, 0, nullptr);
        PortMemoryOStream os;
        CHECK(automaton->Write(os));
        PortMemoryIStream is(os.bytes().data(), os.bytes().size());
        cSC4Automaton* loaded = MakeAutomaton(host);
        CHECK(loaded->Read(is));
        float out[3] = {};
        loaded->GetPosition(out);
        CHECK_NEAR(out[0], 10.0f);
        loaded->GetDestination(out);
        CHECK_NEAR(out[2], 40.0f);
        CHECK_NEAR(loaded->GetRadius(), 3.0f);
        CHECK(loaded->CheckAutomataFlag(kAutomataFlagSpecialRoute));
        CHECK(loaded->GetDispatchInfoID() == 0xabcdef);
        CHECK(loaded->GetPriority() == 9);
        CHECK(loaded->IsSavedFlagSet());
        CHECK(loaded->StateStack().size() == 1);
        CHECK(loaded->GetState()->stateType == kStateScripted);
        CHECK_NEAR(loaded->GetState()->timer, 4.0f);
        CHECK(loaded->Release() == 0);
        // version 9 is refused
        PortMemoryOStream newer;
        newer.SetUint16(9);
        PortMemoryIStream newerIn(newer.bytes().data(), newer.bytes().size());
        cSC4Automaton* refused = MakeAutomaton(host);
        CHECK(!refused->Read(newerIn));
        CHECK(refused->Release() == 0);
        CHECK(automaton->Release() == 0);
    }

    // ---- automata group / PostInit ---------------------------------------------------------------------------------
    {
        FakeHost host;
        host.group.id = 0x20;
        host.group.speedMin = 8.0f;
        host.group.speedMax = 16.0f;
        cSC4Automaton* automaton = MakeAutomaton(host);
        automaton->SetAutomataGroup(&host.group);
        CHECK(automaton->AutomataGroupId() == 0x20);
        CHECK(automaton->GetAutomataGroup() == &host.group);
        CHECK(automaton->GetDefaultSpeed() > 0.0f);
        // PostInit picks the largest occupant group of a loaded automaton and asks the templates for their depth
        cSC4Automaton* loaded = MakeAutomaton(host);
        loaded->SetSavedFlag(true);
        host.group.id = 0x20;
        host.group.depthCalls = 0;
        // the occupant's groups come from the occupant itself; the automaton is one, so use its group API
        loaded->AddOccupantGroup(0x10);
        loaded->AddOccupantGroup(0x20);
        CHECK(loaded->PostInit());
        CHECK(loaded->AutomataGroupId() == 0x20);
        CHECK(host.group.depthCalls == 1);
        CHECK(loaded->Release() == 0);
        CHECK(automaton->Release() == 0);
    }

    // ---- removal --------------------------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeModel* model = new FakeModel();
        host.nextModel = model;
        automaton->AddModelInstance(cGZPersistResourceKey{1, 2, 3});
        automaton->InitEffectsList();
        automaton->Remove(true);
        CHECK(automaton->GetMarkedForRemove());
        CHECK(host.effects.stops == 1);
        CHECK(!host.effects.lastFade);
        CHECK(automaton->Release() == 0);
    }
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeMessageTarget callback;
        callback.AddRef();
        automaton->AddCallback(&callback);
        automaton->Remove(false);
        CHECK(host.effects.stops == 1);
        CHECK(host.effects.lastFade);
        CHECK(!callback.events.empty() && callback.events[0] == kAutomataEventRemoving);
        callback.Release();
        CHECK(automaton->Release() == 0);
    }

    // ---- Init / Shutdown lifecycle ---------------------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        CHECK(automaton->Init());   // no model keys: nothing to load, the effects list is not built
        CHECK(automaton->IsInitialized());
        CHECK(host.effects.inits == 0);
        CHECK(automaton->PlaceInOccupantManager(true));
        CHECK(automaton->Shutdown());
        CHECK(!automaton->IsInitialized());
        CHECK(host.removed == 1);
        CHECK(automaton->Release() == 0);
    }
    {   // LoadModels: the radius is half the first model's length; a model longer than 64 gets 32
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeModel* model = new FakeModel();
        model->minXyz[2] = -2.0f;
        model->maxXyz[2] = 4.0f;
        host.nextModel = model;
        CHECK(automaton->AddModelResourceKey(cGZPersistResourceKey{1, 2, 3}));   // not initialized yet: only the key is kept
        CHECK(automaton->GetModelInstance() == nullptr);
        CHECK(automaton->LoadModels());
        CHECK(automaton->GetModelInstance() == model);
        CHECK_NEAR(automaton->GetRadius(), 3.0f);
        automaton->UnloadModels();
        FakeModel* longModel = new FakeModel();
        longModel->minXyz[2] = -40.0f;
        longModel->maxXyz[2] = 40.0f;
        host.nextModel = longModel;
        CHECK(automaton->LoadModels());
        CHECK_NEAR(automaton->GetRadius(), 32.0f);
        automaton->UnloadModels();
        CHECK(automaton->Release() == 0);
    }
    {   // LoadModels: the auxiliary models the exemplar lists (here 3, the trailer part) join the instances
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        cSCPropertyHelp::SetPropertyValue(automaton->AsPropertyHolder(), 0xcc21420d, std::vector<std::uint32_t>{3, 7});   // 7 is no specifier
        cSCPropertyHelp::SetPropertyValue(automaton->AsPropertyHolder(), kAutomatonPropertyTrailerModel,
                                          std::vector<std::uint32_t>{0x5ad0e817, 0xbadb57f1, 0x11490000});
        CHECK(automaton->AddModelResourceKey(cGZPersistResourceKey{1, 2, 3}));
        FakeModel* body = new FakeModel();
        FakeModel* part = new FakeModel();
        host.nextModel = body;
        host.queuedModel = part;
        CHECK(automaton->LoadModels());
        CHECK(host.createdModels == 2);                 // the body and the listed part
        CHECK(host.createdInstances.size() == 2 && host.createdInstances[0] == 3 && host.createdInstances[1] == 0x11490000);
        CHECK(automaton->GetModelInstance() == body);
        automaton->UnloadModels();
        CHECK(automaton->Release() == 0);
    }

    // ---- debug text / ghost (seam placeholders) --------------------------------------------------------------------
    {
        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        CHECK(automaton->GetDebugFlagsText() != nullptr);
        CHECK(automaton->GetDebugExtraText() != nullptr);
        CHECK(!automaton->InitGhostModel());
        CHECK(!automaton->ShutdownGhostModel());
        CHECK(!automaton->UpdateGhostModel());
        CHECK(!automaton->AttachAnim(cGZPersistResourceKey{}, s3d::kPlayLoop, true, 1.0f));
        CHECK(automaton->Release() == 0);
    }

    // ---- AttachAnim (i386 0x000c1b92), its tick (0x000bbf9c) and its exit (0x000c0418) ---------------------------------
    {
        FakeHost host;
        host.fadeSeconds = 1.0f;
        cSC4Automaton* automaton = MakeAutomaton(host);
        FakeModel figures;
        figures.frameCount = 4;
        host.nextModel = &figures;
        const cGZPersistResourceKey key{0x5ad0e817, 0xbadb57f1, 0x112a0000};
        CHECK(automaton->AttachAnim(key, static_cast<s3d::PlayMode>(1), true, 5.0f));
        tAutomataState* state = automaton->GetState();
        CHECK(state && state->stateType == kStateAttachAnim && state->timer == 5.0f);
        CHECK(host.createdAnimated == 1 && host.createdInstances.back() == 0x112a0000);
        CHECK(figures.opacity == 0.0f && figures.translucent && figures.playMode == 1);   // joins invisible
        CHECK(figures.refs == 2);                                                          // the instance list and the state
        // each tick adds ms / (half the fade time); the answer is the flag AttachAnim was given
        CHECK(automaton->DoAttachAnimTick(0.125f));
        CHECK_NEAR(figures.opacity, 0.25f);
        CHECK(automaton->DoAttachAnimTick(0.5f));
        CHECK_NEAR(figures.opacity, 1.0f);
        const int sets = figures.opacitySets;
        CHECK(automaton->DoAttachAnimTick(0.5f) && figures.opacitySets == sets);           // opaque: left alone
        CHECK(automaton->GetState() == state);
        // the last frame of a one-shot animation ends the state, and the instance leaves with it
        figures.frame = 3;
        CHECK(automaton->DoAttachAnimTick(0.5f));
        CHECK(automaton->GetState() != state);
        CHECK(host.destroyedModels == 1 && figures.refs == 0);
        CHECK(automaton->GetModelInstance() == nullptr);

        // a looping animation (mode 3) and a ping-pong one (2) never end by themselves; the flag is what the tick answers
        for (int mode : {2, 3}) {
            FakeModel loop;
            loop.frameCount = 4;
            loop.frame = 3;
            host.nextModel = &loop;
            CHECK(automaton->AttachAnim(key, static_cast<s3d::PlayMode>(mode), false, 5.0f));
            tAutomataState* looping = automaton->GetState();
            CHECK(!automaton->DoAttachAnimTick(0.5f));
            CHECK(automaton->GetState() == looping && loop.refs == 2);
            CHECK(automaton->PopState());
            CHECK(loop.refs == 0);
        }

        // no model for the key: the state is pushed all the same (as in the original) and the call answers false
        host.nextModel = nullptr;
        CHECK(!automaton->AttachAnim(key, s3d::kPlayOnce, true, 5.0f));
        CHECK(automaton->GetState() && automaton->GetState()->stateType == kStateAttachAnim);
        CHECK(automaton->DoAttachAnimTick(0.5f));
        CHECK(automaton->Release() == 0);
    }

    // ---- the game-data registry node (RegisterInGameDataRegistry, i386 0x000c29a2, and its commands) --------------------
    {
        struct SystemNode final : cIGZUnknown {   // stands in for the automata script system's node
            bool QueryInterface(std::uint32_t iid, void** out) override {
                if (iid != 1 || !out) return false;
                *out = this;
                return true;
            }
            std::uint32_t AddRef() override { return 2; }
            std::uint32_t Release() override { return 1; }
        };
        static cSC4GameDataRegistry* s_registry = nullptr;
        static SystemNode s_system;
        auto* registry = new cSC4GameDataRegistry();
        registry->AddRef();
        s_registry = registry;
        const EntryId systemId = registry->AddChildEntry(&s_system, "automata", registry->GetRootNodeEntry(), 0, nullptr);
        AutomatonRegistrySource source;
        source.registry = [] { return static_cast<cISC4GameDataRegistry*>(s_registry); };
        source.scriptSystem = []() -> cIGZUnknown* { return &s_system; };
        SetAutomatonRegistrySource(source);

        FakeHost host;
        cSC4Automaton* automaton = MakeAutomaton(host);
        const float where[3] = {12.0f, 34.0f, 56.0f};
        automaton->SetPosition(where);
        automaton->RegisterInGameDataRegistry();
        cIGZUnknown* self = static_cast<cISC4Occupant*>(automaton);
        const EntryId node = registry->GetEntryID(self);
        CHECK(node != 0);
        CHECK(registry->GetParentNodeEntry(node) == systemId);
        CHECK(registry->GetEntryName(node) && registry->GetEntryName(node)->Strlen() == 0);
        automaton->RegisterInGameDataRegistry();   // once only
        struct Children { std::vector<EntryId> ids; } children;
        registry->VisitImmediateChildren(node, [](EntryId e, void* c) { static_cast<Children*>(c)->ids.push_back(e); return true; }, &children);
        CHECK(children.ids.size() == 9);
        auto find = [&](const char* name) -> EntryId {
            for (EntryId e : children.ids)
                if (const cRZString* n = registry->GetEntryName(e); n && std::strcmp(n->ToChar(), name) == 0) return e;
            return 0;
        };
        // the position as values
        cRZVariant value;
        CHECK(registry->DoValueExchange(find("z"), value, false));
        CHECK_NEAR(value.GetValFloat32(), 56.0f);
        CHECK(registry->CheckEntryFlags(find("x"), 2) && !registry->CheckEntryFlags(find("x"), 1));
        // push_state(state, seconds): seconds become milliseconds; without them no timeout (-1)
        cGZCommandParameterSet in, out;
        in.AppendParameter(cRZVariant(static_cast<std::uint32_t>(kStateIdleTick)));
        in.AppendParameter(cRZVariant(2.5f));
        CHECK(registry->CheckEntryFlags(find("push_state"), 1));
        CHECK(registry->DoValueCall(find("push_state"), &in, &out));
        CHECK(automaton->GetState() && automaton->GetState()->stateType == kStateIdleTick);
        CHECK(automaton->GetState() && automaton->GetState()->controller == nullptr);
        CHECK_NEAR(automaton->GetState() ? automaton->GetState()->timer : 0.0f, 2500.0f);
        cGZCommandParameterSet one;
        one.AppendParameter(cRZVariant(static_cast<std::uint32_t>(kStateScripted)));
        CHECK(registry->DoValueCall(find("push_state"), &one, &out));
        CHECK_NEAR(automaton->GetState() ? automaton->GetState()->timer : 0.0f, -1.0f);
        // fade_out marks the automaton removed and fades it; remove takes the opacity to 0 at once
        CHECK(!automaton->CheckAutomataFlag(kAutomataFlagRemoved));
        cGZCommandParameterSet none;
        CHECK(registry->DoValueCall(find("remove"), &none, &out));
        CHECK(automaton->CheckAutomataFlag(kAutomataFlagRemoved));
        CHECK_NEAR(automaton->GetOpacity(), 0.0f);
        // the node goes with its subtree
        automaton->UnregisterInGameDataRegistry();
        CHECK(registry->GetEntryID(self) == 0);
        Children left;
        registry->VisitImmediateChildren(systemId, [](EntryId e, void* c) { static_cast<Children*>(c)->ids.push_back(e); return true; }, &left);
        CHECK(left.ids.empty());
        CHECK(automaton->Release() == 0);
        SetAutomatonRegistrySource({});
        registry->Release();
    }

    if (failures == 0) std::printf("PASS sc4_automaton\n");
    else std::printf("FAIL sc4_automaton (%d checks)\n", failures);
    return failures == 0 ? 0 : 1;
}
