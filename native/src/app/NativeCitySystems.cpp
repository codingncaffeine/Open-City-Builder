#include "app/NativeCitySystems.h"

#include <cstdio>

#include "sc4/ServiceLocator.h"
#include "sc4/Render3D.h"
#include "sc4/View3DWin.h"
#include "sc4/RenderProperties.h"
#include "sc4/ServiceLocator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4SimGrid.h"
#include "sc4/NetworkOccupant.h"
#include "sc4/OccupantFilters2.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/Lot.h"
#include "app/NativeCity.h"
#include "app/NativeZoneDeveloperHost.h"

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
    cIGZUnknown* ModelMaker() override { return view_ ? view_->GetModelMaker() : nullptr; }   // the view's (+0x190)
    cIGZUnknown* ViewManager() override { return nullptr; }
    SC4ViewUtilities* RendererViewUtilities() override { return render_ ? render_->GetViewUtilities() : nullptr; }
    cIGZUnknown* RendererLightingManager() override { return SL::spLightingManager; }
    sc4::cSC4View3DWin* view_ = nullptr;
    sc4::cSC43DRender* render_ = nullptr;
};
}  // namespace

sc4::cSC4View3DWin* g_cityView3DWin = nullptr;
sc4::cSC4View3DWin* CityView3DWin() { return g_cityView3DWin; }
bool CityViewPickTerrain(int x, int y, float out[3], bool flag) { return g_cityView3DWin && g_cityView3DWin->PickTerrain(x, y, out, flag); }
void SetCityView3DWin(sc4::cSC4View3DWin* view) { g_cityView3DWin = view; }
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
// cSC4DisasterLayer::GoToNextDisaster's zoom (i386 0x0034a65c): after the target moved, with the render property bool 0x25 on,
// a camera more than one level further out than the disaster's zoom goes to that zoom.
void NativeZoomInForDisaster(int zoom) {
    sc4::cSC43DRender* render = CityRenderer();
    sc4::cSC4CameraControl* camera = render ? render->GetCameraControl() : nullptr;
    if (!camera) return;
    const int current = camera->Zoom();   // +0x108
    if (!(static_cast<float>(current) - 1.0f < static_cast<float>(zoom))) return;
    auto* properties = dynamic_cast<sc4::cSC4RenderProperties*>(SL::spRenderProperties);
    if (!properties) return;
    constexpr std::uint32_t kZoomInOnFind = 0x25;
    const bool zoomIn = properties->NumBoolProperties() > kZoomInOnFind ? properties->BoolValue(kZoomInOnFind)
                                                                         : sc4::DefaultRenderProperties().bools[kZoomInOnFind].value;
    if (zoomIn && static_cast<float>(current) < static_cast<float>(zoom - 1)) camera->SetZoom(static_cast<float>(zoom));
}
// cSC4MySim::Find's camera half (i386 0x00544fee): with the render property bool 0x25 on, a camera zoomed out past 3 goes to
// zoom 4; then the camera's target is the point.
bool NativeCameraLookAt(const float pos[3], bool zoomIfFar) {
    sc4::cSC43DRender* render = CityRenderer();
    sc4::cSC4CameraControl* camera = render ? render->GetCameraControl() : nullptr;   // spRenderer vslot 0x40
    if (!camera) return false;
    constexpr std::uint32_t kZoomInOnFind = 0x25;
    bool zoomIn = sc4::DefaultRenderProperties().bools[kZoomInOnFind].value;
    if (auto* properties = dynamic_cast<sc4::cSC4RenderProperties*>(SL::spRenderProperties);
        properties && properties->NumBoolProperties() > kZoomInOnFind)
        zoomIn = properties->BoolValue(kZoomInOnFind);
    if (zoomIfFar && zoomIn && static_cast<float>(camera->Zoom()) < 3.0f) camera->SetZoom(4.0f);
    cS3DVector3 target;
    target.x = pos[0];
    target.y = pos[1];
    target.z = pos[2];
    camera->SetViewTargetPosition(target);
    return true;
}
// 3D view vslot 0x58: 1 = the surface; 1 without a view.
int NativeCurrentViewLevel() {
    sc4::cSC4View3DWin* view = CityView3DWin();
    return view ? static_cast<int>(view->GetCurrentViewLevel()) : 1;
}
}  // namespace native

namespace native {
namespace {
// The world the occupant filters of sc4/OccupantFilters2 ask (cSC4ViewLevelOccupantFilter, cSC4NetworkOccupantFilter, ...).
class OccupantFilterWorld final : public sc4::IOccupantFilterWorld {
public:
    // QueryInterface(0xa821ef94) then vslot 0x60 (the network flags); cSC4NetworkOccupant does not answer that QI natively, its
    // class stands for the interface.
    bool NetworkFlags(sc4::cISC4Occupant* occupant, std::uint32_t& flags) override {
        auto* piece = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        if (!piece) return false;
        flags = piece->GetNetworkFlag();
        return true;
    }
    bool NetworkIsPlaced(sc4::cISC4Occupant* occupant) override {
        auto* piece = dynamic_cast<sc4::cSC4NetworkOccupant*>(occupant);
        return piece && piece->IsPlaced();
    }
    // SC4GetOccupantViewLevel (PC 0x005E9CA0, spec occupant-manager.md 13.2.4): 1 the surface, 2 the subway layer, 3 the pipe layer.
    int OccupantViewLevel(sc4::cISC4Occupant* occupant) override {
        if (!occupant) return 1;
        auto answers = [occupant](std::uint32_t iid) {
            void* raw = nullptr;
            if (!occupant->QueryInterface(iid, &raw) || !raw) return false;
            static_cast<cIGZUnknown*>(raw)->Release();
            return true;
        };
        const sc4::cISCPropertyHolder* holder = occupant->AsPropertyHolder();
        int level = 1;
        if (answers(0x49c05b9f) || answers(0xea0a5acf)) level = 3;          // a pipe, a pipe connection
        if (holder && holder->HasProperty(0x49512a09)) level = 3;
        if (answers(0xca16374f) || answers(0xca15ed6b)) level = 2;          // a subway, a subway connection
        if (occupant->IsOccupantGroup(0x4217) || occupant->IsOccupantGroup(0x4208) || occupant->IsOccupantGroup(0x421d)) level = 2;
        if (holder && holder->HasProperty(0x8a15f1bd)) return 2;           // final
        if (level == 1 && holder) {
            bool p72 = false, p71 = false, p70 = false;
            sc4::cSCPropertyHelp::GetPropertyValue(holder, 0xcaa45672, p72);
            sc4::cSCPropertyHelp::GetPropertyValue(holder, 0xcaa45671, p71);
            sc4::cSCPropertyHelp::GetPropertyValue(holder, 0xcaa45670, p70);
            if (p72) level = 3;
            if (p71) level = 2;
            if (!p70) level = 1;
        }
        return level;
    }
    int LotStateAtPosition(float x, float z) override {
        sc4::cSC4City* city = HostCity();
        if (!city) return -1;
        int cellX = 0, cellZ = 0;
        city->PositionToCell(x, z, cellX, cellZ);
        sc4::cSC4Lot* lot = NativeLotAtCell(cellX, cellZ, false);
        return lot ? lot->GetState() : -1;
    }
};
}  // namespace

void InstallOccupantFilterWorld() {
    static OccupantFilterWorld world;
    sc4::SetOccupantFilterWorld(&world);
}
}  // namespace native
