#include "app/NativeCitySystems.h"

#include <cstdio>

#include "sc4/ServiceLocator.h"
#include "sc4/Render3D.h"
#include "sc4/View3DWin.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4SimGrid.h"

namespace native {
namespace {
using sc4::CityComponent;

class CitySystems final : public SL::ICitySystems {
public:
    explicit CitySystems(sc4::cSC4City* city) : city_(city) {}
    cIGZUnknown* HistoryWarehouse() override { return C(CityComponent::HistoryWarehouse); }
    cIGZUnknown* Demolition() override { return C(CityComponent::DemolitionUtility); }
    cIGZUnknown* LotManager() override { return C(CityComponent::LotManager); }
    cIGZUnknown* OccupantManager() override { return C(CityComponent::OccupantManager); }
    cIGZUnknown* PropManager() override { return C(CityComponent::PropManager); }
    cIGZUnknown* ZoneManager() override { return C(CityComponent::ZoneManager); }
    cIGZUnknown* LotConfigurationManager() override { return C(CityComponent::LotConfigurationManager); }
    cIGZUnknown* NetworkManager() override { return C(CityComponent::NetworkManager); }
    cIGZUnknown* DispatchManager() override { return C(CityComponent::DispatchManager); }
    cIGZUnknown* TrafficNetworkMap() override { return C(CityComponent::TrafficNetwork); }
    cIGZUnknown* PropDeveloper() override { return C(CityComponent::PropDeveloper); }
    cIGZUnknown* NetworkLotManager() override { return C(CityComponent::NetworkLotManager); }
    cIGZUnknown* VehicleManager() override { return C(CityComponent::VehicleManager); }
    cIGZUnknown* PedestrianManager() override { return C(CityComponent::PedestrianManager); }
    cIGZUnknown* AircraftManager() override { return C(CityComponent::AircraftManager); }
    cIGZUnknown* WatercraftManager() override { return C(CityComponent::WatercraftManager); }
    cIGZUnknown* Simulator() override { return C(CityComponent::Simulator); }
    cIGZUnknown* AuraSim() override { return C(CityComponent::AuraSimulator); }
    cIGZUnknown* BudgetSim() override { return C(CityComponent::BudgetSimulator); }
    cIGZUnknown* BuildingDevelopmentSim() override { return C(CityComponent::BuildingDevelopmentSimulator); }
    cIGZUnknown* CommercialSim() override { return C(CityComponent::CommercialSimulator); }
    cIGZUnknown* CrimeSim() override { return C(CityComponent::CrimeSimulator); }
    cIGZUnknown* DemandSim() override { return C(CityComponent::DemandSimulator); }
    cIGZUnknown* FireProtectionSim() override { return C(CityComponent::FireProtectionSimulator); }
    cIGZUnknown* FlammabilitySim() override { return C(CityComponent::FlammabilitySimulator); }
    cIGZUnknown* FloraSim() override { return C(CityComponent::FloraSimulator); }
    cIGZUnknown* IndustrialSim() override { return C(CityComponent::IndustrialSimulator); }
    cIGZUnknown* LandValueSim() override { return C(CityComponent::LandValueSimulator); }
    cIGZUnknown* NeighborsSim() override { return C(CityComponent::NeighborsSimulator); }
    cIGZUnknown* OrdinanceSim() override { return C(CityComponent::OrdinanceSimulator); }
    cIGZUnknown* PlumbingSim() override { return C(CityComponent::PlumbingSimulator); }
    cIGZUnknown* PoliceSim() override { return C(CityComponent::PoliceSimulator); }
    cIGZUnknown* PollutionSim() override { return C(CityComponent::PollutionSimulator); }
    cIGZUnknown* PowerSim() override { return C(CityComponent::PowerSimulator); }
    cIGZUnknown* ResidentialSim() override { return C(CityComponent::ResidentialSimulator); }
    cIGZUnknown* TrafficSim() override { return C(CityComponent::TrafficSimulator); }
    cIGZUnknown* WeatherSim() override { return C(CityComponent::WeatherSimulator); }
    cIGZUnknown* DisasterLayer() override { return C(CityComponent::DisasterLayer); }
    cIGZUnknown* CivicBuildingSim() override { return C(CityComponent::CivicBuildingSimulator); }
    cIGZUnknown* ParkManager() override { return C(CityComponent::ParkManager); }
    cIGZUnknown* ZoneDeveloper() override { return C(CityComponent::ZoneDeveloper); }
    cIGZUnknown* SeaportDeveloper() override { return C(CityComponent::SeaportDeveloper); }
    cIGZUnknown* AirportDeveloper() override { return C(CityComponent::AirportDeveloper); }
    cIGZUnknown* LandfillDeveloper() override { return C(CityComponent::LandfillDeveloper); }
    cIGZUnknown* LotDeveloper() override { return C(CityComponent::LotDeveloper); }
    cIGZUnknown* TractDeveloper() override { return C(CityComponent::TractDeveloper); }
    cIGZUnknown* AdvisorSystem() override { return C(CityComponent::AdvisorSystem); }
    cIGZUnknown* TutorialSystem() override { return C(CityComponent::TutorialSystem); }
    cIGZUnknown* SurfaceWater() override { return C(CityComponent::SurfaceWater); }
    cIGZUnknown* Terrain() override { return C(CityComponent::Terrain); }
    cIGZUnknown* EffectsManager() override { return C(CityComponent::EffectsManager); }
    cIGZUnknown* Clock24Hour() override { return C(CityComponent::Clock24Hour); }

private:
    cIGZUnknown* C(CityComponent c) { return city_ ? city_->GetComponent(c) : nullptr; }   // borrowed, as the original's locator holds them
    sc4::cSC4City* city_;
};

}  // namespace

namespace {
// The city geometry cSC4SimGrid reads through SL::spCity (cISC4City cell counts and cell sizes).
class SimGridCity final : public sc4::ISimGridCity {
public:
    std::int32_t CitySizeXInCells() const override { return city_ ? city_->CellCountX() : 0; }
    std::int32_t CitySizeZInCells() const override { return city_ ? city_->CellCountZ() : 0; }
    float CellSizeXMeters() const override { return city_ ? city_->CellWidthX() : 16.0f; }
    float CellSizeZMeters() const override { return city_ ? city_->CellWidthZ() : 16.0f; }
    sc4::cSC4City* city_ = nullptr;
};
}  // namespace

// cSC4SimGrid's city dimensions (SetTractSize reads them): bound before anything in the city view builds a grid.
void BindSimGridCity(cISC4City* cityInterface) {
    auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface);
    if (!city) return;
    static SimGridCity gridCity;
    gridCity.city_ = city;
    sc4::SetSimGridCity(&gridCity);
}

void BindCityServices(cISC4City* cityInterface) {
    auto* city = dynamic_cast<sc4::cSC4City*>(cityInterface);
    if (!city) return;
    BindSimGridCity(cityInterface);
    static CitySystems* systems = nullptr;
    delete systems;
    systems = new CitySystems(city);
    SL::OnCityCreate(cityInterface, systems);
    SL::OnCityInit();
}

namespace {
class View3DSystems final : public SL::IView3DSystems, public SL::IWinSC4App {
public:
    SL::IView3DSystems* View3D() override { return this; }
    cIGZUnknown* View3DWin() override { return view_ ? static_cast<cIGZUnknown*>(static_cast<sc4::cISC4View3DWin*>(view_)) : nullptr; }
    cIGZUnknown* Renderer() override { return render_ ? static_cast<cIGZUnknown*>(static_cast<::cISC43DRender*>(render_)) : nullptr; }
    cIGZUnknown* ModelMaker() override { return nullptr; }         // the model maker object is not published natively yet
    cIGZUnknown* ViewManager() override { return nullptr; }
    cIGZUnknown* RendererViewUtilities() override { return nullptr; }   // SC4ViewUtilities is a plain class, not a COM object
    cIGZUnknown* RendererLightingManager() override { return SL::spLightingManager; }
    sc4::cSC4View3DWin* view_ = nullptr;
    sc4::cSC43DRender* render_ = nullptr;
};
}  // namespace

sc4::cSC4View3DWin* g_cityView3DWin = nullptr;
sc4::cSC4View3DWin* CityView3DWin() { return g_cityView3DWin; }
int CityViewLevel() { return g_cityView3DWin ? static_cast<int>(g_cityView3DWin->GetCurrentViewLevel()) : 1; }

void BindView3DServices(sc4::cSC4View3DWin* view, sc4::cSC43DRender* render) {
    static View3DSystems systems;
    g_cityView3DWin = view;
    systems.view_ = view;
    systems.render_ = render;
    SL::spWinSC4App = &systems;
    SL::OnView3DCreate(&systems);
    SL::OnView3DInit();
}

}  // namespace native

namespace native {
void AnnounceCameraScroll(bool begin);   // app/NativeAutomataViews.cpp
sc4::cSC43DRender* CityRenderer();      // app/NativeCityView.cpp
// The camera's target to a world position (the disaster layer's jump): the automata managers hear the scroll.
void NativeMoveCameraTo(float x, float y, float z) {
    sc4::cSC43DRender* render = CityRenderer();
    sc4::cSC4CameraControl* camera = render ? render->GetCameraControl() : nullptr;
    if (!camera) return;
    cS3DVector3 target;
    target.x = x;
    target.y = y;
    target.z = z;
    AnnounceCameraScroll(true);
    camera->SetViewTargetPosition(target);
    AnnounceCameraScroll(false);
}
// 3D view vslot 0x58: 1 = the surface; 1 without a view.
int NativeCurrentViewLevel() {
    sc4::cSC4View3DWin* view = CityView3DWin();
    return view ? static_cast<int>(view->GetCurrentViewLevel()) : 1;
}
}  // namespace native
