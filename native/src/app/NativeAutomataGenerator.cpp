// The automata generator as a real controller: cSC4AutomataController (the recovered base: occupant, template, flags, the
// automata list, age and lifetime, activation, the save record) carrying the recovered generator (sc4/AutomataGenerator.*:
// the spawn clock, the count modes, the ring of network cells, the spawn itself). In the original the generator IS a subclass
// of the controller; the port recovered it apart, behind IAutomataGeneratorHost, so this class is that host: every "Base"
// call lands in the base object, and the base's virtuals (the ones the controller manager calls) land in the generator.
// The city's answers (population, schools and hospitals, lots, networks, terrain, the automata managers) come from the
// loaded city.
//   PARTIAL: the network manager's own position test (its vslot 0x158) is not recovered; a spawn point on a network cell of
//   the wanted kind is accepted. The watercraft manager is not asked for a group.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>

#include "app/NativeAutomataViews.h"
#include "sc4/TrailerVehicle.h"
#include "app/NativeZoneDeveloperHost.h"
#include "sc4/AutomataController.h"
#include "sc4/AutomataGenerator.h"
#include "sc4/AutomataManager.h"
#include "sc4/Automaton.h"
#include "sc4/Lot.h"

namespace sc4 {
extern cIGZSerializable* (*g_createLoadedGenerator)();   // tools/city_probe_hostglobals.cpp; the load-time factory asks it for class 0x4a1628c5
}

namespace native {
sc4::cSC4AutomataManager* NativeAutomataManagerForGroup(std::uint32_t group);   // app/NativeAutomataManagerHost.cpp
bool NativeViewportHolds(const float* viewport);                                   // app/NativeAutomataManagerHost.cpp
long NativeSpritesDrawn();                                                         // app/NativeSprites.cpp
bool NativeAutomataLayerVisible(std::uint32_t layer);
cIGZMessageTarget2* NativeControllerManagerTarget();                           // app/NativeAutomataControllerHost.cpp
bool NativeControllerManagerNow(float& time, std::int32_t& day);
int NativeResidentialPopulation();                                             // app/NativeSimHosts.cpp
bool NativeResidentialServed(cIGZUnknown* building, bool school, std::uint32_t& served);
bool NativeProjectToViewport(const float* xyz, float out[3]);                  // app/NativeAutomataManagerHost.cpp

namespace {
using namespace sc4;

bool Trace() { static const bool on = std::getenv("SC4_GENERATORDEBUG") != nullptr; return on; }

class NativeGenerator;
std::set<NativeGenerator*> g_live;   // for the kit runner's listing

class NativeGenerator final : public cSC4AutomataController, private IAutomataGeneratorHost {
public:
    NativeGenerator() : generator_(this) { flags_ = kCtrlFlagReady; g_live.insert(this); }   // the original's constructor stores 2 at +0x6c
    ~NativeGenerator() override { g_live.erase(this); }

    // One line per automaton this generator made and still holds: where it is in the city and on screen, and its state.
    void Report() const {
        for (IControllerAutomaton* link : automata_) {
            cSC4Automaton* a = cSC4Automaton::OfControllerLink(link);
            float p[3] = {0, 0, 0}, s[3] = {0, 0, 0};
            a->GetPosition(p);
            const bool projected = NativeProjectToViewport(p, s);
            const tAutomataState* state = a->GetState();
            std::printf("generators: %s group 0x%x class 0x%08x at (%.0f, %.1f, %.0f) on screen (%.0f, %.0f)%s state %d speed %.4f flags 0x%x model %p opacity %.2f\n",
                        GetTemplateName().ToChar(), generator_.Group(), a->GetGZCLSID(), p[0], p[1], p[2], s[0], s[1], projected ? "" : " (no view)",
                        state ? static_cast<int>(state->stateType) : -1, a->GetCurrentSpeed(), static_cast<unsigned>(a->GetFlags()),
                        static_cast<void*>(a->GetModelInstance()), a->GetOpacity());
        }
    }

    // ---- the controller's virtuals -> the generator ----------------------------------------------------------------------
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AutomataGenerator; }
    std::uint32_t GetControllerType() const override { return kControllerTypeGenerator; }
    bool Init() override {
        const bool ok = generator_.Init();
        if (Trace())
            std::fprintf(stderr, "generator %s: %s, init %d at (%.0f, %.0f) flags 0x%x radius %.0f..%.0f groups %zu mode %d rate %.2f unit %d max %u; holds %zu (count %u) spawned %u group 0x%x (%u of it) age %.0f of %.0f\n",
                         GetTemplateName().ToChar(), loaded_ ? "from the save" : "new", ok ? 1 : 0, Where(0), Where(2), flags_, mirror_.minRadius, mirror_.radius, mirror_.groups.size(),
                         mirror_.countMode, mirror_.rate, mirror_.periodUnit, mirror_.maxAutomata, automata_.size(), automataCount_, generator_.SpawnedCount(), generator_.Group(),
                         generator_.GroupSpawned(), age_, lifetime_);
        return ok;
    }
    bool Shutdown() override { return generator_.Shutdown(); }
    void Activate(bool on) override { generator_.Activate(on); }
    float GetRadius() const override { return generator_.GetRadius(); }
    void SetTemplate(tControllerTemplate* tmpl) override {
        pending_ = tmpl;
        if (!tmpl) {
            cSC4AutomataController::SetTemplate(nullptr);
            generator_.SetTemplate(nullptr);
            return;
        }
        Mirror(*tmpl);
        generator_.SetTemplate(&mirror_);
    }
    void SetAutomataGroups(const std::vector<std::uint32_t>& groups) override {
        // the original writes into the shared template (kept); the generator reads its mirror
        if (template_ && template_->controllerType == kControllerTypeGenerator && &template_->groups != &groups) template_->groups = groups;
        generator_.SetAutomataGroups(groups);
    }
    void CheckTimeOfDay(const ControllerTime& now, const ControllerTime& delta) override { generator_.CheckTimeOfDay(now, delta); }
    bool IsExpired() const override { return generator_.IsExpired(); }
    void DoAnimationTick(float dtMilliseconds, std::uint32_t arg, const ControllerTime& time) override {
        const std::uint32_t before = automataCount_;
        generator_.DoAnimationTick(dtMilliseconds, arg, time);
        if (Trace() && automataCount_ != before)
            std::fprintf(stderr, "generator %s: %u -> %u automata (group 0x%x) at (%.0f, %.0f)\n", GetTemplateName().ToChar(), before, automataCount_,
                         generator_.Group(), Where(0), Where(2));
    }
    bool Write(cIGZOStream& stream) const override { return generator_.Write(stream); }
    bool Read(cIGZIStream& stream) override { loaded_ = true; return generator_.Read(stream); }
    bool HandleRemoveAutomaton(IControllerAutomaton* automaton) override {
        if (Trace()) {
            for (IControllerAutomaton* link : automata_) {
                if (link != automaton) continue;
                cSC4Automaton* a = cSC4Automaton::OfControllerLink(link);
                float p[3] = {0, 0, 0};
                a->GetPosition(p);
                const tAutomataState* state = a->GetState();
                std::fprintf(stderr, "generator %s: automaton %p removed at (%.0f, %.1f, %.0f) state %d\n", GetTemplateName().ToChar(), static_cast<void*>(a),
                             p[0], p[1], p[2], state ? static_cast<int>(state->stateType) : -1);
            }
        }
        return cSC4AutomataController::HandleRemoveAutomaton(automaton);
    }

private:
    float Where(int axis) const { float p[3] = {0, 0, 0}; cSC4AutomataController::GetPosition(p); return p[axis]; }

    // tControllerTemplate / tGeneratorTemplate -> the generator's mirror of the fields it reads
    void Mirror(const tControllerTemplate& t) {
        mirror_ = GeneratorTemplate();
        mirror_.type = t.controllerType;
        mirror_.minRadius = t.radius;
        mirror_.spawnCurve = t.curve;
        mirror_.dayMask = static_cast<std::uint32_t>(t.field60);
        mirror_.lifetime = t.lifetime;
        mirror_.maxAutomata = t.maxAutomata;
        mirror_.field_bc = t.flags;
        mirror_.groups = t.groups;
        if (const auto* g = dynamic_cast<const tGeneratorTemplate*>(&t)) {
            mirror_.countScale = g->countScale;
            mirror_.countMode = g->countMode;
            mirror_.groupSizeMin = g->groupSizeMin;
            mirror_.groupSizeMax = g->groupSizeMax;
            mirror_.rate = g->rate;
            mirror_.periodUnit = g->periodUnit;
            mirror_.probability = g->probability;
            mirror_.radius = g->radius2;
            mirror_.lifetimeAutomataLimit = g->lifetimeAutomataLimit;
            mirror_.spawnArgument = g->spawnArgument;
        }
    }

    // ---- IAutomataGeneratorHost: the base class -------------------------------------------------------------------------
    bool BaseInit() override { const bool ok = cSC4AutomataController::Init(); generator_.initialized = initialized_; return ok; }
    bool BaseShutdown() override { const bool ok = cSC4AutomataController::Shutdown(); generator_.initialized = initialized_; return ok; }
    void BaseActivate(bool active) override { cSC4AutomataController::Activate(active); }
    void BaseDoAnimationTick(float dt, std::uint32_t arg, const ControllerTime& time) override { cSC4AutomataController::DoAnimationTick(dt, arg, time); }
    bool BaseWrite(cIGZOStream& stream) override { return cSC4AutomataController::Write(stream); }
    bool BaseRead(cIGZIStream& stream) override { return cSC4AutomataController::Read(stream); }
    void BaseSetTemplate() override { cSC4AutomataController::SetTemplate(pending_); }
    std::uint32_t BaseLiveCount() override { return automataCount_; }
    void BaseAddAutomaton(IAutomaton* automaton) override {
        IControllerAutomaton* link = static_cast<Spawned*>(automaton)->automaton->ControllerLinkFor();
        automata_.push_back(link);
        link->AddRef();
        ++automataCount_;
    }
    void BaseReleaseAutomata() override {
        for (IControllerAutomaton* link : automata_) {
            link->SetController(nullptr);   // automaton vslot 0x68 with 0
            link->Release();
        }
        automata_.clear();
    }
    void BaseSyncLiveCount() override { automataCount_ = static_cast<std::uint32_t>(automata_.size()); }
    float BaseAge() override { return age_; }
    float BaseLifetime() override { return lifetime_; }
    bool CheckFlag(std::uint32_t flags) override { return cSC4AutomataController::CheckFlag(flags); }
    void GetPosition(float out[3]) override { cSC4AutomataController::GetPosition(out); }
    void GetLotFacingPoint(float out[3]) override { cSC4AutomataController::GetLotFacingPoint(out); }
    void GetOccupantBoundingBox(float outMin[3], float outMax[3]) override { cSC4AutomataController::GetOccupantBoundingBox(outMin, outMax); }
    bool GetBoundingBoxIntersection(const float from[2], const float to[2], float out[2]) override {
        return cSC4AutomataController::GetBoundingBoxIntersection(from, to, out);
    }
    const void* GetBehaviorTemplate(float distance, const float position[3]) override { return cSC4AutomataController::GetBehaviorTemplate(distance, position); }

    // ---- the occupant -----------------------------------------------------------------------------------------------------
    bool HasOccupant() override { return occupant_ != nullptr; }
    std::uint32_t OccupantType() override { return occupant_ ? occupant_->GetType() : 0; }
    bool OccupantHasFlag(std::uint32_t group) override { return occupant_ && occupant_->IsOccupantGroup(group); }
    void OccupantLotCell(int& x, int& z) override {
        SC4CellRect cells;
        if (occupant_) occupant_->GetBoundingCityCells(cells);
        x = cells.x0;
        z = cells.z0;
    }
    int OccupantFacing() override {   // the lot manager's lot of the occupant, its facing; -1 without one
        SC4CellRect rect;
        std::uint32_t facing = 0;
        return occupant_ && AutomataControllerHost()->GetOccupantLot(occupant_, rect, facing) ? static_cast<int>(facing) : -1;
    }

    // ---- the city ---------------------------------------------------------------------------------------------------------
    const void* Clock() override { return NativeControllerManagerTarget(); }
    void ClockNow(float& time, std::int32_t& day) override { time = 0.0f; day = 0; NativeControllerManagerNow(time, day); }
    bool IsAutomataLayerVisible() override { return NativeAutomataLayerVisible(7); }
    int CityCount() override { return NativeResidentialPopulation(); }
    float OccupantMeasureA() override { std::uint32_t served = 0; NativeResidentialServed(occupant_, true, served); return static_cast<float>(served); }
    float OccupantMeasureB() override { std::uint32_t served = 0; NativeResidentialServed(occupant_, false, served); return static_cast<float>(served); }
    bool LotMeasure(int cellX, int cellZ, float& out) override {
        cSC4Lot* lot = NativeLotAtCell(cellX, cellZ, false);
        out = lot ? lot->GetOccupancy() : 0.0f;
        return lot != nullptr;
    }
    bool NetworkCellHasType(int x, int z, std::uint32_t mask) override { return (NativeNetworkTypesAt(x, z) & mask) != 0; }
    bool NetworkPositionAccepts(const float position[3], std::uint32_t mask) override {
        return NetworkCellHasType(static_cast<int>(position[0] / 16.0f), static_cast<int>(position[2] / 16.0f), mask);
    }
    float NetworkHeight(float x, float z) override { return AutomataControllerHost()->GroundHeight(x, z); }
    bool TerrainHeightEnabled(float x, float z) override { return AutomataControllerHost()->PointInCity(x, z); }
    float TerrainHeight(float x, float z) override { return AutomataControllerHost()->TerrainHeight(x, z); }

    // ---- spawning: the automata manager of the group, and the new automaton as the generator drives it -----------------------
    struct Spawned final : IAutomaton {
        NativeGenerator* owner = nullptr;
        cSC4Automaton* automaton = nullptr;
        void AddRef() override { automaton->AddRef(); }
        void Release() override { automaton->Release(); }
        void DetachController(void*) override { automaton->Remove(false); }
        void AddCallback(const void* target) override { automaton->AddCallback(static_cast<cIGZMessageTarget2*>(const_cast<void*>(target))); }
        void StartBehavior(const void* behavior, cSC4AutomataGenerator*) override {
            const auto* b = static_cast<const tBehaviorTemplate*>(behavior);
            const bool pushed = automaton->PushBehavior(b, owner);
            if (Trace()) std::fprintf(stderr, "generator %s: automaton %p starts behavior state %u (radius %.0f, timeout %.0f, final %d) -> %d\n",
                                      owner->GetTemplateName().ToChar(), static_cast<void*>(automaton), b->state, b->radius, b->timeout, b->final ? 1 : 0, pushed ? 1 : 0);
        }
        void StartDefault(float timer, cSC4AutomataGenerator*) override {
            const bool pushed = automaton->PushState(0, owner, timer, false, 0, 0, nullptr);
            if (Trace()) std::fprintf(stderr, "generator %s: automaton %p starts the default state -> %d\n", owner->GetTemplateName().ToChar(),
                                      static_cast<void*>(automaton), pushed ? 1 : 0);
        }
    };
    struct Manager final : IAutomatonManager {
        NativeGenerator* owner = nullptr;
        cSC4AutomataManager* manager = nullptr;
        Spawned spawned;   // one at a time: the generator finishes with an automaton before it asks for the next
        IAutomaton* CreateAutomaton(std::uint32_t group, const float position[3], std::uint32_t flags, const float[3], std::uint32_t,
                                    std::uint8_t argument) override {
            const cS3DVector3 where{position[0], position[1], position[2]};
            IAutomataAutomaton* view = manager->CreateAutomaton(group, where, flags, cGZPersistResourceKey{}, nullptr, argument, nullptr);
            cSC4Automaton* automaton = view ? AutomatonOf(view) : nullptr;
            if (Trace() && !automaton) std::fprintf(stderr, "generator %s: no automaton for group 0x%x\n", owner->GetTemplateName().ToChar(), group);
            if (!automaton) return nullptr;
            if (Trace()) std::fprintf(stderr, "generator %s: spawned %p group 0x%x class 0x%08x at (%.0f, %.1f, %.0f)\n", owner->GetTemplateName().ToChar(),
                                      static_cast<void*>(automaton), group, automaton->GetGZCLSID(), position[0], position[1], position[2]);
            spawned.owner = owner;
            spawned.automaton = automaton;
            return &spawned;
        }
    };
    IAutomatonManager* ManagerForGroup(std::uint32_t group) override {
        manager_.owner = this;
        manager_.manager = NativeAutomataManagerForGroup(group);
        if (Trace() && !manager_.manager) std::fprintf(stderr, "generator %s: no automata manager takes group 0x%x\n", GetTemplateName().ToChar(), group);
        return manager_.manager ? &manager_ : nullptr;
    }

    cSC4AutomataGenerator generator_;
    GeneratorTemplate mirror_;
    tControllerTemplate* pending_ = nullptr;
    bool loaded_ = false;   // read from a save (for the trace)
    Manager manager_;
};

cIGZSerializable* CreateLoaded() {
    auto* generator = new NativeGenerator();
    generator->AddRef();
    return generator;
}

}  // namespace

// GZCOM()->CreateInstance(0x4a1628c5, 0x6a1857f8): one reference.
sc4::cSC4AutomataController* NativeCreateAutomataGenerator() {
    auto* generator = new NativeGenerator();
    generator->AddRef();
    return generator;
}

void InstallAutomataGenerator() { sc4::g_createLoadedGenerator = &CreateLoaded; }

// The kit's `pedestrians`: how many the pedestrian manager holds, how many have a model, and the first `limit` that are on
// screen (city position, viewport position, state, model, its animation frame).
void NativePedestrianReport(int limit) {
    cSC4AutomataManager* manager = NativeAutomataManagerForGroup(0x4001);   // the pedestrians' base group
    if (!manager) { std::printf("pedestrians: no manager\n"); return; }
    int total = 0, withModel = 0, onScreen = 0, shown = 0;
    for (IAutomataAutomaton* view : manager->Automata()) {
        cSC4Automaton* a = view ? AutomatonOf(view) : nullptr;
        if (!a) continue;
        ++total;
        IAutomatonModel* model = a->GetModelInstance();
        if (model) ++withModel;
        float p[3] = {0, 0, 0}, s[3] = {0, 0, 0};
        a->GetPosition(p);
        if (!NativeProjectToViewport(p, s)) continue;
        if (!NativeViewportHolds(s)) continue;
        ++onScreen;
        if (shown >= limit) continue;
        ++shown;
        const tAutomataState* state = a->GetState();
        std::printf("pedestrians: at (%.0f, %.1f, %.0f) on screen (%.0f, %.0f) state %d speed %.4f flags 0x%x model %p frame %d/%d opacity %.2f\n", p[0], p[1],
                    p[2], s[0], s[1], state ? static_cast<int>(state->stateType) : -1, a->GetCurrentSpeed(), static_cast<unsigned>(a->GetFlags()),
                    static_cast<void*>(model), model ? model->GetFrame() : -1, model ? model->GetFrameCount() : -1, a->GetOpacity());
    }
    std::printf("pedestrians: %d held, %d with a model, %d on screen, %ld sprite quads drawn so far\n", total, withModel, onScreen, NativeSpritesDrawn());
}

// The kit's `trailers`: every trailer vehicle the vehicle manager holds that is on screen, with the cars it pulls.
void NativeTrailerReport(int limit) {
    cSC4AutomataManager* manager = NativeAutomataManagerForGroup(0x4000);   // the vehicles' base group
    if (!manager) { std::printf("trailers: no manager\n"); return; }
    int leads = 0, withCars = 0, cars = 0, shown = 0;
    for (IAutomataAutomaton* view : manager->Automata()) {
        auto* lead = view ? dynamic_cast<cSC4TrailerVehicle*>(AutomatonOf(view)) : nullptr;
        if (!lead) continue;
        ++leads;
        if (lead->GetNumCars() != 0) ++withCars;
        cars += static_cast<int>(lead->GetNumCars());
        float p[3] = {0, 0, 0}, s[3] = {0, 0, 0};
        lead->GetPosition(p);
        if (!NativeProjectToViewport(p, s) || !NativeViewportHolds(s) || shown >= limit) continue;
        ++shown;
        std::printf("trailers: lead at (%.0f, %.1f, %.0f) on screen (%.0f, %.0f) radius %.1f opacity %.2f pulls %u of %u", p[0], p[1], p[2], s[0], s[1],
                    lead->GetRadius(), lead->GetOpacity(), lead->GetNumCars(), lead->GetDesiredNumCars());
        for (cSC4Vehicle* car : lead->Cars()) {
            float c[3] = {0, 0, 0}, cs[3] = {0, 0, 0};
            car->GetPosition(c);
            NativeProjectToViewport(c, cs);
            std::printf("; car at (%.0f, %.1f, %.0f) on screen (%.0f, %.0f) model %p opacity %.2f", c[0], c[1], c[2], cs[0], cs[1],
                        static_cast<void*>(car->GetModelInstance()), car->GetOpacity());
        }
        std::printf("\n");
    }
    std::printf("trailers: %d leads, %d with cars, %d cars\n", leads, withCars, cars);
}

void NativeGeneratorReport() {
    std::size_t automata = 0;
    for (NativeGenerator* generator : g_live) { generator->Report(); automata += generator->Automata().size(); }
    std::printf("generators: %zu controllers, %zu automata\n", g_live.size(), automata);
}

}  // namespace native
