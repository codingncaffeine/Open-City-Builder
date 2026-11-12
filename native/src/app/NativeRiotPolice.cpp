// The figures a patrol car puts down in a native city: the services of cSC4RiotPolice (IRiotPoliceHost) and what the patrol
// cars' host (app/NativeDispatch.cpp) needs to put a squad down and take it back.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "app/NativeAutomataViews.h"
#include "app/NativeCity.h"
#include "app/NativeCityView.h"
#include "sc4/AutomataManager.h"
#include "sc4/PedestrianManager.h"
#include "sc4/Render3D.h"
#include "sc4/RiotPolice.h"
#include "sc4/ViewUtilities.h"
#include "sc4/cSC4City.h"

namespace sc4 { extern IPedestrianHost* g_loadPedestrianHost; }   // tools/city_probe_hostglobals.cpp (the native pedestrian host)

namespace native {
cIGZUnknown* NativeAddController(sc4::cISC4Occupant* occupant, const char* templateName);   // app/NativeAutomataControllerHost.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_RIOTDEBUG") != nullptr;
    return on;
}

cSC4PedestrianManager* PedestrianManager() {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<cSC4PedestrianManager*>(city->GetComponent(CityComponent::PedestrianManager)) : nullptr;
}

class RiotPoliceHost final : public IRiotPoliceHost {
public:
    bool CityCells(int& countX, int& countZ) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        countX = city->CellCountX();
        countZ = city->CellCountZ();
        return true;
    }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
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
    float CompetenceThreshold(int level) override {
        const AutomataTuningGlobals& t = AutomataTuning();
        return level == 0 ? t.f21a0c : level == 1 ? t.f21a10 : t.f21a14;
    }
    void AddController(cISC4Occupant* occupant, const char* templateName) override {
        cIGZUnknown* made = NativeAddController(occupant, templateName);
        if (Trace()) std::fprintf(stderr, "riot police: controller %s -> %s\n", templateName, made ? "added" : "none");
    }
    cISCResExemplar* ModelExemplar(const cGZPersistResourceKey& key) override {
        cSC4PedestrianManager* manager = PedestrianManager();
        tExemplarData* data = manager ? manager->FindExemplarFromModelKey(key) : nullptr;
        return data ? data->exemplar : nullptr;
    }
} g_riotPoliceHost;

cSC4RiotPolice* SquadOf(cIGZUnknown* riot) { return riot ? dynamic_cast<cSC4RiotPolice*>(riot) : nullptr; }

}  // namespace

void InstallRiotPoliceHost() { SetRiotPoliceHost(&g_riotPoliceHost); }

// ---- the patrol car host's calls (IPatrolCarHost, app/NativeDispatch.cpp) ----
cIGZUnknown* NativeNewRiotPolice(sc4::cSC4PatrolCar& car) {
    if (Trace()) std::fprintf(stderr, "riot police: new figure for a car in dispatch state %u\n", car.GetDispatchState());
    return static_cast<sc4::cISC4Occupant*>(new sc4::cSC4RiotPolice(&car, sc4::g_loadPedestrianHost));
}
void NativeSetupRiotPolice(cIGZUnknown* riot, float competence) {
    sc4::cSC4RiotPolice* squad = SquadOf(riot);
    if (!squad) return;
    squad->SetCompetence(competence);
    if (sc4::cSC4PedestrianManager* manager = PedestrianManager()) manager->AddAutomaton(AutomatonView(static_cast<sc4::cSC4Automaton*>(squad)));
    squad->SetupRiotPolice();
    if (Trace()) {
        float p[3] = {};
        squad->GetPosition(p);
        std::fprintf(stderr, "riot police: set up at (%.0f, %.0f, %.0f), competence %.2f, level %u, figure %u, %zu models, thresholds %.2f %.2f %.2f\n",
                     p[0], p[1], p[2], competence, squad->Level(), squad->CallForHelpFigure(), squad->Models().size(),
                     g_riotPoliceHost.CompetenceThreshold(0), g_riotPoliceHost.CompetenceThreshold(1), g_riotPoliceHost.CompetenceThreshold(2));
    }
}
void NativeRemoveRiotPolice(cIGZUnknown* riot) {
    sc4::cSC4RiotPolice* squad = SquadOf(riot);
    sc4::cSC4PedestrianManager* manager = PedestrianManager();
    if (Trace() && squad) std::fprintf(stderr, "riot police: removed\n");
    if (squad && manager) manager->RemoveAutomaton(AutomatonView(static_cast<sc4::cSC4Automaton*>(squad)), false);
}

}  // namespace native
