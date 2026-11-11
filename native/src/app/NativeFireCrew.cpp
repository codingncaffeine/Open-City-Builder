// The fire crews in a native city: the services of cSC4SimFireCrew (ISimFireCrewHost) and what the fire engines' host
// (app/NativeDispatch.cpp) needs to put a crew down and take it back.
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "app/NativeEffects.h"
#include "rz/cRZRandom.h"
#include "sc4/AutomataManager.h"
#include "sc4/PedestrianManager.h"
#include "sc4/Render3D.h"
#include "sc4/ServiceLocator.h"
#include "sc4/SimFireCrew.h"
#include "sc4/ViewUtilities.h"
#include "sc4/VisualEffect.h"
#include "sc4/cSC4City.h"

namespace sc4 { extern IPedestrianHost* g_loadPedestrianHost; }   // tools/city_probe_hostglobals.cpp (the native pedestrian host)

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_FIREDEBUG") != nullptr;
    return on;
}

class CrewEffect final : public ICrewEffect {
public:
    explicit CrewEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    void AddRef() override { ++refs_; }
    void Release() override {
        if (--refs_ > 0) return;
        effect_->Release();
        delete this;
    }
    void Start(int transition) override { effect_->Start(transition); }
    void Stop(int transition) override { effect_->Stop(transition); }
    bool IsStarted() override { return effect_->IsStarted(); }
    void SetSourceTransform(const cS3DTransform& transform) override { effect_->SetSourceTransform(transform); }
    void SetTarget(const cS3DVector3& target) override { effect_->SetVectorParams(5, &target, 1); }
private:
    cSC4VisualEffect* effect_;
    int refs_ = 1;
};

cISC4OccupantManager* OccupantManager() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cISC4OccupantManager*>(city->GetComponent(CityComponent::OccupantManager)) : nullptr;
}
cSC4PedestrianManager* PedestrianManager() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSC4PedestrianManager*>(city->GetComponent(CityComponent::PedestrianManager)) : nullptr;
}

class CrewHost final : public ISimFireCrewHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    ICrewEffect* CreateEffect(const char* name) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name);
        return effect ? new CrewEffect(effect) : nullptr;
    }
    bool OccupantsInBox(const float min[3], const float max[3], cSC4OccupantFilter* filter, OccupantList& out) override {
        cISC4OccupantManager* manager = OccupantManager();
        const bool any = manager && manager->GetOccupantsByBBox(out, min, max, filter, 0xffffffffu) && !out.empty();
        if (Trace()) std::fprintf(stderr, "crew: search (%.0f..%.0f, %.0f..%.0f) -> %zu\n", min[0], max[0], min[2], max[2], out.size());
        return any;
    }
    bool CityCells(int& countX, int& countZ) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        countX = city->CellCountX();
        countZ = city->CellCountZ();
        return true;
    }
    void ViewDirection(float xyz[3]) override {
        xyz[0] = xyz[1] = 0.0f;
        xyz[2] = 1.0f;
        cSC43DRender* render = CityRenderer();
        if (SC4ViewUtilities* view = render ? render->GetViewUtilities() : nullptr) {
            const cS3DVector3& d = view->ViewDirection();
            xyz[0] = d.x;
            xyz[1] = d.y;
            xyz[2] = d.z;
        }
    }
    std::uint32_t RandomBelow(std::uint32_t range) override { return range ? random_.RandomUint32Uniform(range) : 0; }
    std::uint32_t RandomUint32() override { return random_.RandomUint32Uniform(); }
    double RandomRange(double lo, double hi) override { return random_.RandomDoubleRangeUniform(lo, hi); }
    float CrewDistance() override { return AutomataTuning().f21a04; }
    float DirectorDistance() override { return AutomataTuning().f21a08; }
    float CompetenceThreshold(int level) override {
        const AutomataTuningGlobals& t = AutomataTuning();
        return level == 0 ? t.f21a0c : level == 1 ? t.f21a10 : t.f21a14;
    }
    float HoseRadius() override { return AutomataTuning().f21a18; }
    void MoveModelInRenderer(IAutomatonModel*) override {}   // the automata host places a model when its transform is set
    // PARTIAL: the hose registration point of the crew model (the stream leaves from the model's origin).
    bool RegPointTranslations(IAutomatonModel*, const char*, std::vector<cS3DVector3>&) override { return false; }
private:
    cRZRandom random_{0x5f1cu};
} g_crewHost;

cSC4SimFireCrew* CrewOf(cIGZUnknown* crew) { return crew ? dynamic_cast<cSC4SimFireCrew*>(crew) : nullptr; }


}  // namespace

void InstallFireCrewHost() { SetSimFireCrewHost(&g_crewHost); }

// ---- the fire engine host's crew calls (IFireTruckHost, app/NativeDispatch.cpp) ----
cIGZUnknown* NativeNewFireFilter() { return static_cast<sc4::cISC4OccupantFilter*>(new sc4::cSC4CrewFireOccupantFilter()); }
cIGZUnknown* NativeNewSpillFilter() { return static_cast<sc4::cISC4OccupantFilter*>(new sc4::cSC4ActiveToxicOccupantFilter()); }
bool NativeCountOccupantsInBox(const float* min, const float* max, cIGZUnknown* filter, int& count) {
    count = 0;
    sc4::cISC4OccupantManager* manager = OccupantManager();
    auto* occupantFilter = filter ? dynamic_cast<sc4::cISC4OccupantFilter*>(filter) : nullptr;
    if (!manager || !occupantFilter) return false;
    sc4::OccupantList list;
    if (!manager->GetOccupantsByBBox(list, min, max, occupantFilter, 0xffffffffu)) return false;
    count = static_cast<int>(list.size());
    manager->ReleaseOccupantList(list);
    if (Trace()) std::fprintf(stderr, "crew: %d occupants pass the engine's filter in (%.0f..%.0f, %.0f..%.0f)\n", count, min[0], max[0], min[2], max[2]);
    return count > 0;
}
cIGZUnknown* NativeNewFireCrew(sc4::cSC4FireTruck& truck) {
    if (Trace()) std::fprintf(stderr, "crew: new crew for an engine in dispatch state %u\n", truck.GetDispatchState());
    return static_cast<sc4::cISC4Occupant*>(new sc4::cSC4SimFireCrew(&truck, sc4::g_loadPedestrianHost));
}
void NativeSetFireCrewCompetence(cIGZUnknown* crew, float competence) {
    if (sc4::cSC4SimFireCrew* c = CrewOf(crew)) c->SetCompetence(competence);
}
void NativeRegisterFireCrew(cIGZUnknown* crew) {
    sc4::cSC4SimFireCrew* c = CrewOf(crew);
    sc4::cSC4PedestrianManager* manager = PedestrianManager();
    if (c && manager) manager->AddAutomaton(AutomatonView(static_cast<sc4::cSC4Automaton*>(c)));
}
void NativeSetupFireCrew(cIGZUnknown* crew) {
    sc4::cSC4SimFireCrew* c = CrewOf(crew);
    if (!c) return;
    c->SetupFireCrew();
    if (Trace()) {
        float p[3] = {};
        c->GetPosition(p);
        std::fprintf(stderr, "crew: set up at (%.0f, %.0f, %.0f), level %u, %zu models, hose radius %.1f, distances %.1f / %.1f, thresholds %.2f %.2f %.2f\n",
                     p[0], p[1], p[2], c->Level(), c->Models().size(), g_crewHost.HoseRadius(), g_crewHost.CrewDistance(),
                     g_crewHost.DirectorDistance(), g_crewHost.CompetenceThreshold(0), g_crewHost.CompetenceThreshold(1),
                     g_crewHost.CompetenceThreshold(2));
    }
}
void NativeRemoveFireCrew(cIGZUnknown* crew) {
    sc4::cSC4SimFireCrew* c = CrewOf(crew);
    sc4::cSC4PedestrianManager* manager = PedestrianManager();
    if (Trace() && c) std::fprintf(stderr, "crew: removed after %d ticks with a target\n", c->FoundCount());
    if (c && manager) manager->RemoveAutomaton(AutomatonView(static_cast<sc4::cSC4Automaton*>(c)), false);
}

}  // namespace native
