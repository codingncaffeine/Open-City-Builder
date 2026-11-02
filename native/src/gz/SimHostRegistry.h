// Where the GZCOM creators of the simulators find the services the originals reached through globals (SL::spCity,
// RZ::WM(), the shared random generator). The application installs the hosts once at startup; creators hand them to the
// constructors. A missing host stays null, which the classes tolerate until their first use.
#pragma once
#include "rz/cRZRandom.h"

class cIGZWinMgr;

namespace sc4 {
class ICityHost;
class ITrafficHost;
class IPollutionHost;
class IPoliceHost;
class IPlumbingHost;
class IFlammabilityHost;
class ILandValueHost;
class IWealthHost;
class IBaseLotDeveloperHost;
class IZoneManagerHost;
class ICommercialHost;
class IResidentialHost;
class IPowerHost;
class IFireHost;
class ITerrainViewHost;
class IDemolitionHost;
}  // namespace sc4

namespace gzcom {

struct SimHosts {
    sc4::ICityHost* city = nullptr;
    sc4::ITrafficHost* traffic = nullptr;
    sc4::IPollutionHost* pollution = nullptr;
    sc4::IPoliceHost* police = nullptr;
    sc4::IPlumbingHost* plumbing = nullptr;
    sc4::IFlammabilityHost* flammability = nullptr;
    sc4::ILandValueHost* landValue = nullptr;
    sc4::IWealthHost* wealth = nullptr;
    sc4::IBaseLotDeveloperHost* lotDeveloper = nullptr;
    sc4::IZoneManagerHost* zoneManager = nullptr;
    sc4::ICommercialHost* commercial = nullptr;
    sc4::IResidentialHost* residential = nullptr;
    sc4::IPowerHost* power = nullptr;
    sc4::IFireHost* fire = nullptr;
    sc4::ITerrainViewHost* terrainView = nullptr;
    sc4::IDemolitionHost* demolition = nullptr;   // SL::spDemolition (the probe's stub host when null)
    cIGZWinMgr* windowManager = nullptr;     // RZ::WM()
    cRZRandom* random = nullptr;             // the shared generator; a private one is used when null
};

SimHosts& GetSimHosts();
cRZRandom& SharedRandom();   // the registry's generator, or a process-wide default seeded from the clock

}  // namespace gzcom
