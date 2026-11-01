// The native city's component lifecycle: cSC4City::Init / Shutdown call slot 0x18 / 0x20 of every component in kInitOrder /
// kShutdownOrder (cSC4City::Init @ 0x1006bcaa2). The port keeps components as cIGZUnknown, so this file is the dispatch: the component
// is matched to its recovered class with dynamic_cast (a city-probe wrapper or a neutral creator object simply does not match) and that
// class's Init / Shutdown runs. A component with no recovered class, or whose class has no recovered Init, is reported unwired.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/citycomp/CityComponentInit.h"
#include "platform/Unwired.h"
#include "sc4/cSC4City.h"

namespace native {
namespace {
using namespace sc4;

// Components whose class and Init are recovered but whose host in the native game is still the city probe's neutral binding
// (src/tools/city_probe_bindings.cpp): their Init dereferences services the probe does not answer, so they stay uninitialised until a
// native host replaces the probe. Each entry is a to-do (SIM-HOST-*).
constexpr const char* kProbeHosted = ",TrafficNetwork,";
// Components the native city view initialises itself, after its render stack is up (NativeCityView.cpp: the terrain needs the view's
// services for its first surface pass, the occupant manager needs the automata host installed first). Initialised here they come up
// half-wired and their init guard then stops the second call (the terrain drew black). Ordering divergence until those services exist
// before cSC4City::Init.
constexpr const char* kInitByCityView = ",Terrain,OccupantManager,";

// Dev switch: SC4_COMPINIT_SKIP=Name,Name leaves more components uninitialised (bisecting a component whose Init breaks);
// SC4_COMPINIT_PROBES=1 initialises the probe-hosted ones anyway.
bool Skipped(const char* name) {
    const std::string key = std::string(",") + name + ",";
    static const bool probes = std::getenv("SC4_COMPINIT_PROBES") != nullptr;
    if (!probes && std::string(kProbeHosted).find(key) != std::string::npos) return true;
    if (std::string(kInitByCityView).find(key) != std::string::npos) return true;
    static const char* list = std::getenv("SC4_COMPINIT_SKIP");
    if (!list) return false;
    return (std::string(",") + list + ",").find(key) != std::string::npos;
}

// Components whose recovered class has a no-argument Init (the list in citycomp/CityComponentInit.h, one TU per class). Not there
// because their Init / Shutdown are not recovered (each is a to-do): cSC4LotDeveloper, cSC4FireProtectionSimulator, cSC4FloraSimulator,
// cSC4PlumbingSimulator (Init), cSC4PoliceSimulator, cSC4PollutionSimulator, cSC4PowerSimulator, cSC4ResidentialSimulator,
// cSC4TrafficSimulator, cSC4CrimeSimulator (Init takes the tunables: the slot-0x18 wrapper is not recovered), the effects manager,
// the disaster layer and the demolition utility.
bool Dispatch(cIGZUnknown* object, bool init) {
#define SC4_TRY_CITY_COMPONENT(c) if (CityComponentInit_##c(object, init)) return true;
    SC4_CITY_COMPONENTS(SC4_TRY_CITY_COMPONENT)
#undef SC4_TRY_CITY_COMPONENT
    return false;
}

}  // namespace

void RunCityComponentLifecycle(CityComponent component, cIGZUnknown* object, bool init) {
    const char* name = GetCityComponentInfo(component).name;
    static const bool trace = std::getenv("SC4_SIMDEBUG") != nullptr;
    if (!object) return;
    if (Skipped(name)) {
        if (trace) std::fprintf(stderr, "city: %s %s skipped (probe host)\n", init ? "init" : "shutdown", name);
        if (init) SC4_UNWIRED("cSC4City::Init component left uninitialised (its host is still a city-probe binding)");
        return;
    }
    const bool done = Dispatch(object, init);
    if (trace) std::fprintf(stderr, "city: %s %s %s\n", init ? "init" : "shutdown", name, done ? "ok" : "not wired");
    if (!done && init) SC4_UNWIRED("cSC4City::Init component slot 0x18 (class or Init not recovered)");
}

}  // namespace native
