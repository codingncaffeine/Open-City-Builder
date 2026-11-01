// Loading a city file in the native application: the application's city hooks (the recovered cSC4City, cSC4DBSegment and the
// save-game helpers) and the serializable factory that creates every object of a city file from its class id.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "rz/cRZAutoRefCount.h"
#include "sc4/ISC4RegionalCity.h"

class cSC4App;
class cIGZUnknown;
class cIGZMessageTarget2;
namespace sc4 { enum class CityComponent : std::uint8_t; class cSC4City; }

namespace native {

// Installs a neutral stub for every host interface the recovered classes reach (the real hosts replace them as they are written).
void InstallStubHosts();

// The window changes LoadCity asks for once the file is read (the application window's DestroyCurrentScreen / CreateCityScreen).
struct CityScreenCallbacks {
    std::function<void()> destroyCurrentScreen, createCityScreen, createRegionScreen;   // the last: leaving a city
};
void SetCityScreenCallbacks(CityScreenCallbacks callbacks);

// Installs the city hooks once and loads the city file through cSC4App::LoadCity; true when the city came up.
// The exemplar and cohort factories of the resource manager (idempotent; boot and the first city load call it).
void RegisterExemplarFactories();
// holder = the region's slot of the city (its regional record is read into it on load and written from it on save)
bool LoadCityFile(cSC4App& app, const std::string& path, cRZAutoRefCount<cISC4RegionalCity>* holder = nullptr);
// One framework tick for the services the simulator host registered (NativeSimulatorHost.cpp): cSC4Simulator::OnTick.
void TickSimulatorServices(std::uint32_t tickNumber);
// SL::spSimulator vslot 0x98 / 0xa8 (AddAgent / RemoveAgent) on the city's cSC4Simulator: what every simulator's Init / Shutdown
// reaches through its own host seam. False when the city has no simulator yet.
bool AddCitySimulatorAgent(cIGZMessageTarget2* agent, int interval, const char* name, std::uint32_t flags);
void RemoveCitySimulatorAgent(cIGZMessageTarget2* agent);
bool CitySimulatorPresent();   // SL::spSimulator != nullptr
// cSC4City::Init / Shutdown for one component: its recovered class's Init / Shutdown (NativeCityComponents.cpp).
sc4::cSC4City* HostCity();   // the city being loaded or played (NativeCity.cpp)
void RunCityComponentLifecycle(sc4::CityComponent component, cIGZUnknown* object, bool init);

}  // namespace native
