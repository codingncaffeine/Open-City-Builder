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
bool CityComponentInit_cSC4DisasterLayer(cIGZUnknown* object, bool init);
bool CityComponentInit_cSC4CrimeSimulator(cIGZUnknown* object, bool init);
bool CityComponentInit_cSC4EffectsManager(cIGZUnknown* object, bool init);   // NativeEffects.cpp
namespace {
using namespace sc4;

// Components whose class and Init are recovered but whose host in the native game is still the city probe's neutral binding
// (src/tools/city_probe_bindings.cpp): their Init dereferences services the probe does not answer, so they stay uninitialised until a
// native host replaces the probe. Each entry is a to-do (SIM-HOST-*).
constexpr const char* kProbeHosted = ",";

// Dev switch: SC4_COMPINIT_SKIP=Name,Name leaves more components uninitialised (bisecting a component whose Init breaks);
// SC4_COMPINIT_PROBES=1 initialises the probe-hosted ones anyway.
bool Skipped(const char* name, bool init) {
    const std::string key = std::string(",") + name + ",";
    static const bool probes = std::getenv("SC4_COMPINIT_PROBES") != nullptr;
    if (!probes && std::string(kProbeHosted).find(key) != std::string::npos) return true;
    static const char* list = std::getenv("SC4_COMPINIT_SKIP");
    if (!list) return false;
    return (std::string(",") + list + ",").find(key) != std::string::npos;
}

// Components whose recovered class has a no-argument Init (the list in citycomp/CityComponentInit.h, one TU per class), plus the
// disaster layer, the crime simulator and the effects manager (their own files). Not dispatched yet (each is a to-do, PLAN.md 3a
// item A): the lot developer and the demolition utility.
bool Dispatch(cIGZUnknown* object, bool init) {
    if (CityComponentInit_cSC4DisasterLayer(object, init)) return true;   // NativeDisasters.cpp
    if (CityComponentInit_cSC4CrimeSimulator(object, init)) return true;  // NativeCrime.cpp
    if (CityComponentInit_cSC4EffectsManager(object, init)) return true;  // NativeEffects.cpp
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
    if (Skipped(name, init)) {
        if (trace) std::fprintf(stderr, "city: %s %s skipped (probe host)\n", init ? "init" : "shutdown", name);
        if (init) SC4_UNWIRED("cSC4City::Init component left uninitialised (its host is still a city-probe binding)");
        return;
    }
    const bool done = Dispatch(object, init);
    if (trace) std::fprintf(stderr, "city: %s %s %s\n", init ? "init" : "shutdown", name, done ? "ok" : "not wired");
    if (!done && init) SC4_UNWIRED("cSC4City::Init component slot 0x18 (class or Init not recovered)");
}

}  // namespace native
