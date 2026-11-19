// SL: the stages of the service locator against the real framework and a fake city.
#include <cstdio>
#include <cstring>

#include "gz/GZFramework.h"
#include "sc4/App.h"
#include "sc4/ServiceLocator.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
struct Object final : cIGZUnknown {
    bool QueryInterface(std::uint32_t iid, void** out) override { if (iid == 0xc47b747c || iid == 1) { *out = this; AddRef(); return true; } return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    int refs = 1;
};
// a system service that answers every interface with itself
struct Service final : cRZSystemService {
    explicit Service(std::uint32_t id) : cRZSystemService(id, 100) {}
    bool QueryInterface(std::uint32_t, void** out) override { *out = static_cast<cIGZSystemService*>(this); AddRef(); return true; }
    std::uint32_t AddRef() override { return cRZSystemService::AddRef(); }
    std::uint32_t Release() override { return cRZSystemService::Release(); }
};
struct City final : SL::ICitySystems {
    Object o[56]; int next = 0;
    cIGZUnknown* Get() { return &o[next++ % 56]; }
#define S(name) cIGZUnknown* name() override { return &o[__COUNTER__ - base]; }
    static constexpr int base = __COUNTER__ + 1;
    S(HistoryWarehouse) S(Demolition) S(LotManager) S(OccupantManager) S(PropManager) S(ZoneManager) S(LotConfigurationManager) S(NetworkManager)
    S(DispatchManager) S(TrafficNetworkMap) S(PropDeveloper) S(NetworkLotManager) S(VehicleManager) S(PedestrianManager) S(AircraftManager)
    S(WatercraftManager) S(Simulator) S(AuraSim) S(BudgetSim) S(BuildingDevelopmentSim) S(CommercialSim) S(CrimeSim) S(DemandSim)
    S(FireProtectionSim) S(FlammabilitySim) S(FloraSim) S(IndustrialSim) S(LandValueSim) S(NeighborsSim) S(OrdinanceSim) S(PlumbingSim)
    S(PoliceSim) S(PollutionSim) S(PowerSim) S(ResidentialSim) S(TrafficSim) S(WeatherSim) S(DisasterLayer) S(CivicBuildingSim) S(ParkManager)
    S(ZoneDeveloper) S(SeaportDeveloper) S(AirportDeveloper) S(LandfillDeveloper) S(LotDeveloper) S(TractDeveloper) S(AdvisorSystem)
    S(TutorialSystem) S(SurfaceWater) S(Terrain) S(EffectsManager) S(Clock24Hour)
};
struct View final : SL::IView3DSystems {
    Object o[6];
    cIGZUnknown* View3DWin() override { return &o[0]; }
    cIGZUnknown* Renderer() override { return &o[1]; }
    cIGZUnknown* ModelMaker() override { return &o[2]; }
    cIGZUnknown* ViewManager() override { return &o[3]; }
    SC4ViewUtilities* RendererViewUtilities() override { return reinterpret_cast<SC4ViewUtilities*>(&o[4]); }
    cIGZUnknown* RendererLightingManager() override { return &o[5]; }
};
struct Window final : SL::IWinSC4App { View view; SL::IView3DSystems* View3D() override { return &view; } };
struct TerrainViewSource final : SL::ITerrainViewSource { Object view; cIGZUnknown* TerrainView() override { return &view; } };
}  // namespace

int main() {
    auto* f = new cGZFramework();
    cGZFramework::sSetFrameWork(f);
    SL::SetFramework(f);
    auto* app = new cSC4App("T", f->AppView()); app->AddRef();
    CHECK(SL::spSC4 == static_cast<cISC4App*>(app));           // the constructor announces the application
    app->SetDirectories("/g/", "/g/Data/", "/u/");
    cRZString dir;
    SL::AppDirectory(dir); CHECK(std::strcmp(dir.ToChar(), "/g/") == 0);
    SL::DataDirectory(dir); CHECK(std::strcmp(dir.ToChar(), "/g/Data/") == 0);
    SL::UserDataDirectory(dir); CHECK(std::strcmp(dir.ToChar(), "/u/") == 0);

    // services by id; the locator keeps bare pointers (no reference held)
    auto* sound = new Service(0x23cecba0); f->AddSystemService(sound);
    auto* rm = new Service(0x056b906e); f->AddSystemService(rm);
    auto* graphics = new Service(0xc416025c); f->AddSystemService(graphics);
    auto* ms2 = new Service(0x04fa845b); f->AddSystemService(ms2);
    std::uint32_t refsBefore = sound->RefCount();
    CHECK(SL::OnAppInit1());
    CHECK(static_cast<void*>(SL::spSS) == static_cast<void*>(static_cast<cIGZSystemService*>(sound)) && sound->RefCount() == refsBefore);
    CHECK(static_cast<void*>(SL::spRM) == static_cast<void*>(static_cast<cIGZSystemService*>(rm)));
    CHECK(SL::spMS2 != nullptr && SL::spMS == nullptr && SL::spCommandServer == nullptr);   // not registered: untouched / null
    CHECK(SL::spGS != nullptr && SL::spGS2 != nullptr);
    CHECK(SL::ATM() == nullptr && SL::AEMS() == nullptr);

    // the city
    City city;
    int cityToken = 0;
    auto* fakeCity = reinterpret_cast<cISC4City*>(&cityToken);
    CHECK(SL::OnCityCreate(fakeCity, &city) && SL::City() == fakeCity);
    CHECK(SL::OnCityInit());
    CHECK(SL::spTerrain == &city.o[49] && SL::spTerrainMap == SL::spTerrain && SL::Terrain() == &city.o[49]);
    CHECK(SL::spHistoryWarehouse == &city.o[0] && SL::spSimulator == &city.o[16] && SL::sp24HourClock == &city.o[51]);
    CHECK(SL::NetworkManager() == &city.o[7] && SL::OccupantManager() == &city.o[3]);
    TerrainViewSource tv; SL::SetTerrainViewSource(&tv);
    CHECK(SL::TerrainView() == &tv.view);
    CHECK(SL::OnCityShutdown() && SL::spTerrain == nullptr && SL::spSimulator == nullptr && SL::TerrainView() == nullptr);
    CHECK(SL::City() == fakeCity);                                  // shutdown leaves the city pointer alone
    int other = 0;
    CHECK(SL::OnCityDestroy(reinterpret_cast<cISC4City*>(&other)) && SL::City() == fakeCity);
    CHECK(SL::OnCityDestroy(fakeCity) && SL::City() == nullptr);

    // the 3D view needs the application window
    CHECK(!SL::OnView3DInit() && SL::spRenderer == nullptr);
    Window window;
    app->SetWinSC4App(&window);
    f->SetApplication(static_cast<cIGZApp*>(app));
    CHECK(SL::OnAppInit2() && SL::spWinSC4App == &window);
    CHECK(SL::OnView3DInit());
    CHECK(SL::spView3DWin == &window.view.o[0] && SL::spRenderer == &window.view.o[1] && SL::spModelMaker == &window.view.o[2]);
    CHECK(SL::ViewManager() == &window.view.o[3] && SL::ViewUtilities() == reinterpret_cast<SC4ViewUtilities*>(&window.view.o[4]) && SL::spLightingManager == &window.view.o[5]);
    CHECK(SL::OnView3DShutdown() && SL::spRenderer == nullptr && SL::spView3D == nullptr);
    std::printf(failures ? "FAILED\n" : "OK\n");
    return failures ? 1 : 0;
}
