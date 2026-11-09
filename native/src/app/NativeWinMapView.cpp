// The data-view map window's services on the native application (sc4::IWinMapViewHost). The generated stub this replaces
// answered every call neutrally: no message server (so cSC4WinMapView::Init failed) and FilterUICommand false (so every
// button of the Data Views panel was vetoed before the view saw it).
//   PARTIAL: CollectHighlights views 6 and 9 (see there) and the pothole decal answer
//   "nothing"; the tutorial
//   command filter always passes (no tutorial runs natively).
#include <chrono>
#include <set>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeCityView.h"
#include "app/NativeEffects.h"
#include "app/NativePower.h"
#include "app/NativeZoneDeveloperHost.h"
#include "gz/WinFont.h"
#include "gz/Buffer.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/AuraSimulator.h"
#include "sc4/CommonIIDs.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/Lot.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/SC4UI.h"
#include "sc4/VisualEffect.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/CameraControl.h"
#include "sc4/Exemplar.h"
#include "sc4/Render3D.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/SLString.h"
#include "sc4/ServiceLocator.h"
#include "sc4/View3DWin.h"
#include "sc4/WinMapView.h"
#include "sc4/WinMiniMap.h"
#include "sc4/PowerSimulator.h"
#include "sc4/cSC4City.h"
#include "sc4/cSTETerrain.h"

namespace native {
const sc4::cSC4SimGrid<std::uint8_t>* NativeFlammabilityGrid();   // app/NativeAdvisorFlammability.cpp
const sc4::cSC4SimGrid<std::uint8_t>* NativePoweredGrid();        // app/NativePower.cpp
const sc4::cISC4GrowthDeveloper* GrowthDeveloperInterfaceOfType(std::uint32_t type);   // app/NativeGrowthDevelopers.cpp
sc4::IWinMapViewMarkedCells* NativeCreateMarkedCells();          // app/NativeMarkedCells.cpp
bool NativeAddMarkedCells(sc4::IWinMapViewMarkedCells* cells);
bool NativeRemoveMarkedCells(sc4::IWinMapViewMarkedCells* cells);
sc4::cISC4Occupant* NativeStationOccupant(sc4::IProtectionStation* station);   // app/NativeSimHosts.cpp
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_MAPVIEWDEBUG") != nullptr;
    return on;
}

// DisplayLegend's exemplar: {0x6534284a, 0x690f693f, view + 0x4a0b6800}; colours 0x4a0b47f2, label string ids 0x4a0b47f3, title
// string id 0x4a0b47f4.
constexpr std::uint32_t kExemplarType = 0x6534284a, kViewExemplarGroup = 0x690f693f, kViewExemplarBase = 0x4a0b6800;
constexpr std::uint32_t kLegendColors = 0x4a0b47f2, kLegendLabels = 0x4a0b47f3, kLegendTitle = 0x4a0b47f4;
constexpr std::uint32_t kLegendFontStyle = 0xe9c86b5f;

// The map panel's child 0x4203 is a cSC4WinMiniMap; its iid 0xca318385 view is cISC4WinMiniMap. The port's colour-sink seam
// names its calls by the vslots it saw: 0x40 = SetFlags(mask, value), 0x30 = SetDefaultZoneColorToTrans, 0x38 = SetZoneColor.
class MiniMapSink final : public IWinMapViewColorSink {
public:
    cISC4WinMiniMap* map = nullptr;   // borrowed: the panel owns the window
    void SetBaseColor(std::uint32_t mask, std::uint32_t value) override { map->SetFlags(mask, value); }
    void ResetColors() override { map->SetDefaultZoneColorToTrans(); }
    void SetColor(int index, std::uint32_t rgb) override { map->SetZoneColor(static_cast<std::uint32_t>(index), rgb); }
    void SetOverlay(cIGZBuffer* overlay) override { map->SetOverlay(overlay); }
    void Refresh() override { map->Update(); }
    void Invalidate() override { if (cIGZWin* win = map->AsIGZWin()) win->InvalidateSelf(); }
    bool MapSize(int& width, int& height) override {
        cIGZWin* win = map->AsIGZWin();
        if (!win) return false;
        width = static_cast<int>(win->GetW());
        height = static_cast<int>(win->GetH());
        return width > 0 && height > 0;
    }
};

// A highlight's visual effect: the named effect from the effects manager. Place = the source transform (translation, uniform
// scale, flags | 3, change counter + 2: UpdateHighlights' crime case, i386 0x0027bb0a), Activate = Start(0). When the highlight
// lets go of it, the effect is stopped.
class HighlightEffect final : public IWinMapViewEffect {
public:
    explicit HighlightEffect(cSC4VisualEffect* effect) : effect_(effect) {}
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (--refs_ != 0) return refs_;
        effect_->Stop(0);
        effect_->Release();
        delete this;
        return 0;
    }
    void Place(const float position[3], float size) override {
        cS3DTransform source;
        source.flags |= 3;
        source.changeCounter += 2;
        source.translation[0] = position[0];
        source.translation[1] = position[1];
        source.translation[2] = position[2];
        source.scale = size;
        effect_->SetSourceTransform(source);
    }
    void Activate(bool) override { effect_->Start(0); }

private:
    cSC4VisualEffect* effect_;   // one reference
    std::uint32_t refs_ = 1;
};

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
cSTETerrain* CityTerrainOf(cSC4City* city) {
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();
    return terrain;
}
template <class T> bool Fill(const cSC4SimGrid<T>* grid, const std::vector<std::vector<T>>*& out, WinMapGrid& g) {
    if (!grid) return false;
    out = &grid->GetGridData();
    g.cellSizeX = grid->GetTractWidthX();
    g.cellSizeZ = grid->GetTractWidthZ();
    return true;
}

class Host final : public IWinMapViewHost {
public:
    std::uint32_t NowMilliseconds() override {
        using namespace std::chrono;
        return static_cast<std::uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
    }
    cIGZMessageServer2* MessageServer2() override { return SL::spMS2; }
    // SL::ATM() vslots 0x28 / 0x30 / 0x40: AddTickableObject, RemoveTickableObject (any priority), SetNewPriority.
    void AddAnimationTarget(cIGZMessageTarget2* target) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, 0);
    }
    void RemoveAnimationTarget(cIGZMessageTarget2* target) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, 0xffffffffu);
    }
    void SetAnimationNotifications(cIGZMessageTarget2* target, std::uint32_t flags) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->SetNewPriority(target, flags);
    }
    // UpdateHighlights' simulator switch (i386 0x0027bb0a). Coverage radii come from each simulator's GetOccupantCoverage at level
    // 0.5 (its second output). PARTIAL: view 6 (garbage buildings + landfill-zone occupants) and view 9 (the traffic simulator's
    // transit switches, not recovered) collect nothing.
    void CollectHighlights(std::uint32_t view, std::vector<WinMapHighlight>& out) override {
        switch (view) {
            case 1:
            case 2:
                if (auto* res = CityAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator)) {
                    cSC4ResidentialSimulator::OccupantList list;
                    if (view == 1) res->GetSchoolBuildings(list, nullptr);
                    else res->GetHospitalBuildings(list, nullptr);
                    for (IResidentialOccupant* building : list) {
                        float distance = 0.0f, radius = 0.0f;
                        res->GetOccupantCoverage(building, 0.5f, distance, radius);
                        void* raw = nullptr;
                        cIGZUnknown* object = building->Object();
                        if (object && object->QueryInterface(kGZIID_cISC4Occupant, &raw) && raw) {
                            out.push_back({static_cast<sc4::cISC4Occupant*>(raw), radius});
                            static_cast<sc4::cISC4Occupant*>(raw)->Release();   // the city keeps the occupant
                        }
                        building->Release();   // the list's reference
                    }
                }
                break;
            case 3:
                if (auto* fire = CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator)) {
                    std::list<IFireOccupant*> stations;
                    fire->GetFireStations(stations);
                    for (IFireOccupant* station : stations) {
                        float covered = 0.0f, radius = 0.0f;
                        fire->GetOccupantCoverage(station, SC4Percentage(0.5f), covered, radius);
                        if (sc4::cISC4Occupant* occupant = NativeStationOccupant(station)) out.push_back({occupant, radius});
                    }
                }
                break;
            case 4:
                if (auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator)) {
                    std::list<IProtectionStation*> stations;
                    police->GetPoliceStations(stations);
                    for (IProtectionStation* station : stations) {
                        auto* officer = dynamic_cast<IPoliceOccupant*>(station);
                        if (!officer) continue;
                        float covered = 0.0f, radius = 0.0f;
                        police->GetOccupantCoverage(officer, SC4Percentage(0.5f), covered, radius);
                        if (sc4::cISC4Occupant* occupant = NativeStationOccupant(station)) out.push_back({occupant, radius});
                    }
                }
                break;
            case 5: {   // the crime hot spot: a cell, 100 m
                cSC4City* city = HostCity();
                cIGZUnknown* object = city ? city->GetComponent(CityComponent::CrimeSimulator) : nullptr;
                void* raw = nullptr;
                std::uint8_t value = 0;
                int x = 0, z = 0;
                if (object && object->QueryInterface(kGZIID_PlainObject, &raw) && raw && static_cast<cSC4CrimeSimulator*>(raw)->GetCrimeHotspot(&value, &x, &z)) {
                    WinMapHighlight spot;
                    spot.scale = 100.0f;
                    spot.x = static_cast<float>((x + 0.5) * 16.0);
                    spot.z = static_cast<float>((z + 0.5) * 16.0);
                    if (cSTETerrain* terrain = CityTerrainOf(city)) spot.y = terrain->GetAltitude(spot.x, spot.z);
                    out.push_back(spot);
                }
                break;
            }
            case 7:
                if (cSC4PowerSimulator* power = NativePowerSimulator()) {
                    std::list<sc4::cISC4Occupant*> plants;
                    power->GetPowerPlants(plants);   // borrowed
                    for (sc4::cISC4Occupant* plant : plants) out.push_back({plant, 0.0f});
                }
                break;
            case 8:
                if (auto* plumbing = CityAs<cSC4PlumbingSimulator>(CityComponent::PlumbingSimulator)) {
                    std::list<sc4::cISC4Occupant*> producers;
                    plumbing->GetWaterProducers(producers);   // one reference each
                    for (sc4::cISC4Occupant* producer : producers) {
                        out.push_back({producer, 0.0f});
                        producer->Release();
                    }
                }
                break;
            default: break;
        }
        if (Trace() && !out.empty()) std::fprintf(stderr, "map view: highlight view %u: %zu highlights\n", view, out.size());
    }
    IWinMapViewMarkedCells* CreateMarkedCells() override { return NativeCreateMarkedCells(); }
    bool AddMarkedCells(IWinMapViewMarkedCells* cells) override { return NativeAddMarkedCells(cells); }
    bool RemoveMarkedCells(IWinMapViewMarkedCells* cells) override { return NativeRemoveMarkedCells(cells); }
    bool CreateEffect(const cRZString& name, IWinMapViewEffect** out) override {
        cSC4VisualEffect* effect = CreateNamedEffect(name.ToChar());
        if (Trace()) std::fprintf(stderr, "map view: highlight effect \"%s\" -> %s\n", name.ToChar(), effect ? "made" : "none");
        if (!effect) return false;
        *out = new HighlightEffect(effect);
        return true;
    }
    bool LoadLegendData(std::uint32_t view, std::vector<std::uint32_t>& colors, std::vector<std::uint32_t>& labelStrings,
                        std::uint32_t& titleString) override {
        void* resource = nullptr;
        const cGZPersistResourceKey key{kExemplarType, kViewExemplarGroup, view + kViewExemplarBase};
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &resource, 0, nullptr) || !resource) {
            if (Trace()) std::fprintf(stderr, "map view: no legend exemplar for view 0x%x\n", view);
            return false;
        }
        auto* exemplar = static_cast<cISCResExemplar*>(resource);
        cISCPropertyHolder* holder = exemplar->AsISCPropertyHolder();
        cSCPropertyHelp::GetPropertyValue(holder, kLegendColors, colors);
        cSCPropertyHelp::GetPropertyValue(holder, kLegendLabels, labelStrings);
        cSCPropertyHelp::GetPropertyValue(holder, kLegendTitle, titleString);
        exemplar->Release();
        if (Trace()) std::fprintf(stderr, "map view: legend 0x%x: %zu colours, %zu labels, title 0x%08x\n", view, colors.size(), labelStrings.size(), titleString);
        return true;
    }
    void LoadString(std::uint32_t group, std::uint32_t instance, cRZString& out) override {
        out = cSLString(group, instance, true);
    }
    cIGZFont* LegendFont() override {
        gz::IWinFontSource* source = gz::WinFontSource();
        gz::IWinFont* font = source ? source->Font(kLegendFontStyle) : nullptr;
        return font ? font->AsFont() : nullptr;
    }
    IWinMapViewColorSink* ViewColorSink(cIGZWin* parent) override {
        void* raw = nullptr;
        if (!parent || !parent->GetChildAsRecursive(0x4203, kMiniMapIID, &raw) || !raw) return nullptr;
        sink_.map = static_cast<cISC4WinMiniMap*>(raw);
        sink_.map->Release();   // borrowed for the caller's few calls; the window keeps its own reference
        return &sink_;
    }
    bool OccupantLotPosition(sc4::cISC4Occupant*, float[3]) override { return false; }
    bool IsTransportationCell(int x, int z) override {
        return x >= 0 && z >= 0 && x < CitySizeXInCells() && z < CitySizeZInCells() && NativeNetworkTypesAt(x, z) != 0;
    }
    int CitySizeXInCells() override { cSC4City* city = HostCity(); return city ? static_cast<int>(city->CellCountX()) : 0; }
    int CitySizeZInCells() override { cSC4City* city = HostCity(); return city ? static_cast<int>(city->CellCountZ()) : 0; }
    cIGZBuffer* CreatePotholeDecal() override { return nullptr; }
    int MapRotation() override {
        cSC43DRender* renderer = CityRenderer();
        cSC4CameraControl* camera = renderer ? renderer->GetCameraControl() : nullptr;
        return camera ? camera->Rotation() : 0;
    }
    // SL::spCommandServer vslot 0x98: the city view handles the commands natively (as NativeView3DWinHost::ExecuteCommand).
    void SendCommand(std::uint32_t command, std::uint32_t, std::uint32_t) override {
        if (cSC4View3DWin* view = CityView3DWin()) view->ProcessViewCommand(command);
    }
    bool FilterUICommand(const cGZMessage&) override { return true; }

    bool LoadDataViewExemplar(std::uint32_t key, WinMapViewExemplar& out) override {
        void* resource = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(cGZPersistResourceKey{kExemplarType, kViewExemplarGroup, key}, 0xa52160f5, &resource, 0, nullptr) ||
            !resource)
            return false;
        auto* exemplar = static_cast<cISCResExemplar*>(resource);
        const cISCPropertyHolder* h = exemplar->AsISCPropertyHolder();
        using P = cSCPropertyHelp;
        std::uint32_t u = 0;
        bool b = false;
        out.hasOccupantGroups = P::GetPropertyValue(h, 0x4a0b47e0, out.occupantGroups);
        if ((out.hasInvertGroups = P::GetPropertyValue(h, 0x4a0b47e1, b))) out.invertGroups = b;
        if ((out.hasHidePlain = P::GetPropertyValue(h, 0x4a0b47e2, b))) out.hidePlain = b;
        if ((out.hasUnderground = P::GetPropertyValue(h, 0x4a0b47e3, u))) out.underground = u;
        out.hasRamp = P::GetPropertyValue(h, 0x4a0b47e4, out.ramp);
        if ((out.hasDataView = P::GetPropertyValue(h, 0x4a0b47e5, u))) out.dataView = u;
        if ((out.hasZoneDisplay = P::GetPropertyValue(h, 0x4a0b47e6, b))) out.zoneDisplay = b;
        if ((out.hasClampToCity = P::GetPropertyValue(h, 0x4a0b47e7, b))) out.clampToCity = b;
        if ((out.hasLotTypeMask = P::GetPropertyValue(h, 0x4a0b47e8, u))) out.lotTypeMask = u;
        if ((out.hasHighlightView = P::GetPropertyValue(h, 0x4a0b47e9, u))) out.highlightView = u;
        out.hasEffectName = P::GetPropertyValue(h, 0x4a0b47ea, out.effectName);
        if ((out.hasCustomView = P::GetPropertyValue(h, 0x4a0b47eb, u))) out.customView = u;
        if ((out.hasSteps = P::GetPropertyValue(h, 0x4a0b47ec, u))) out.steps = u;
        if ((out.hasLayerStyle = P::GetPropertyValue(h, 0x4a0b47ed, u))) out.layerStyle = u;
        if ((out.hasCircleColor = P::GetPropertyValue(h, 0x4a0b47ee, u))) out.circleColor = u;
        if ((out.hasLotShadeColor = P::GetPropertyValue(h, 0x4a0b47ef, u))) out.lotShadeColor = u;
        std::uint8_t dev = 0;
        if ((out.hasDevelopmentView = P::GetPropertyValue(h, 0x4a0b47f0, dev))) out.developmentView = dev;
        if ((out.hasNetworkList = P::GetPropertyValue(h, 0x4a0b47f1, u))) out.networkList = u;
        if ((out.hasShowZones = P::GetPropertyValue(h, 0x4a0b47f5, b))) out.showZones = b;
        out.hasNetworkTypes = P::GetPropertyValue(h, 0x6b7c5cdd, out.networkTypes);
        exemplar->Release();
        if (Trace())
            std::fprintf(stderr, "map view: data view exemplar 0x%08x: data view %u (%d), ramp %zu, steps %u, lot mask %d/0x%x, clamp %d, highlight %u\n", key,
                         out.dataView, out.hasDataView, out.ramp.size(), out.steps, out.hasLotTypeMask, out.lotTypeMask, out.clampToCity, out.highlightView);
        return true;
    }
    void InterpolateRamp(std::uint32_t* out, int count, const std::uint32_t* stops, int stopCount) override {
        nSC4UI::InterpolateRampToPalette(out, count, stops, stopCount);
    }
    cIGZBuffer* CreateMapBuffer() override {
        auto* buffer = new cGZBuffer();
        buffer->AddRef();
        return buffer;
    }
    bool SimulatorGrid(int source, int argument, WinMapGrid& out) override {
        bool ok = false;
        switch (source) {
            case kGridAuraMayorRating:
                if (auto* aura = CityAs<cISC4AuraSimulator>(CityComponent::AuraSimulator)) ok = Fill<std::int8_t>(aura->GetAuraGrid(), out.s8, out);
                break;
            case kGridAura:
                if (auto* aura = CityAs<cISC4AuraSimulator>(CityComponent::AuraSimulator)) ok = Fill<std::int16_t>(aura->GetParkMap(), out.s16, out);
                break;
            case kGridLandValue:
                if (auto* lv = CityAs<cISC4LandValueSimulator>(CityComponent::LandValueSimulator)) ok = Fill<std::uint8_t>(lv->GetTotalLandValueMap(), out.u8, out);
                break;
            case kGridPolice:
                if (auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator))
                    ok = Fill<std::uint8_t>(police->Protection().AdjustedGrid(), out.u8, out);
                break;
            case kGridCrime: {
                cSC4City* city = HostCity();
                cIGZUnknown* object = city ? city->GetComponent(CityComponent::CrimeSimulator) : nullptr;
                void* raw = nullptr;
                if (object && object->QueryInterface(kGZIID_PlainObject, &raw) && raw)   // a plain class behind its adapter
                    ok = Fill<std::uint8_t>(static_cast<cSC4CrimeSimulator*>(raw)->GetCrimeGrid(), out.u8, out);
                break;
            }
            case kGridFireProtection:
                if (auto* fire = CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator))
                    ok = Fill<std::uint8_t>(fire->GetFireProtectionGrid(), out.u8, out);
                break;
            case kGridFlammability: ok = Fill<std::uint8_t>(NativeFlammabilityGrid(), out.u8, out); break;
            case kGridPower: ok = Fill<std::uint8_t>(NativePoweredGrid(), out.u8, out); break;
            case kGridPollution:
                if (auto* pollution = CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator))
                    ok = Fill<std::int16_t>(pollution->GetPollutionGrid(static_cast<PollutionType>(argument)), out.s16, out);
                break;
            case kGridTraffic:
                if (auto* traffic = CityAs<cSC4TrafficSimulator>(CityComponent::TrafficSimulator)) ok = Fill<std::uint8_t>(traffic->GetCongestionMap(), out.u8, out);
                break;
            case kGridPlumbing:
                if (auto* plumbing = CityAs<cSC4PlumbingSimulator>(CityComponent::PlumbingSimulator)) ok = Fill<std::uint8_t>(plumbing->GetWateredGrid(), out.u8, out);
                break;
            case kGridEducation:
            case kGridHealth:
                if (auto* res = CityAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator)) {
                    cSC4SimGrid<float>* grid = source == kGridEducation ? res->GetAverageEQGrid(nullptr, nullptr) : res->GetAverageHQGrid(nullptr, nullptr);
                    out.owner = static_cast<cISC4SimGridBase*>(grid);
                    ok = Fill<float>(grid, out.f32, out);
                }
                break;
            case kGridResidentialAge:
                if (auto* res = CityAs<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator)) ok = Fill<std::uint8_t>(res->GetAgeGrid(), out.u8, out);
                break;
            default: break;
        }
        if (Trace()) std::fprintf(stderr, "map view: grid source %d (%d) -> %s\n", source, argument, ok ? "found" : "MISSING");
        return ok;
    }
    const cISC4GrowthDeveloper* GrowthDeveloper(std::uint32_t type) override { return GrowthDeveloperInterfaceOfType(type); }
    void CityLots(std::vector<WinMapLot>& out) override {
        cSC4City* city = HostCity();
        auto* lots = city ? dynamic_cast<cSC4ZoneDeveloper*>(city->GetComponent(CityComponent::ZoneDeveloper)) : nullptr;
        if (!lots) return;
        std::set<cSC4Lot*> seen;
        for (int x = 0; x < CitySizeXInCells(); ++x) {
            for (int z = 0; z < CitySizeZInCells(); ++z) {
                cSC4Lot* lot = lots->GetLot(x, z, false);
                if (!lot || !seen.insert(lot).second) continue;
                std::int32_t rect[4] = {};
                lot->GetBoundingRect(rect);
                out.push_back({rect[0], rect[1], rect[2], rect[3], lot->GetZoneType()});
            }
        }
    }

private:
    MiniMapSink sink_;
};

}  // namespace

void InstallWinMapViewHost() {
    static Host host;
    sc4::SetWinMapViewHost(&host);
}

// cSC4WinMapView::InitWindow on the map window the city view made from its script (NativeView3DWinHost::InitMapView).
void NativeInitMapView(cIGZWin* window) {
    if (auto* map = dynamic_cast<sc4::cSC4WinMapView*>(window)) {
        map->InitWindow();
        if (Trace()) std::fprintf(stderr, "map view: InitWindow on %p\n", static_cast<void*>(map));
    } else if (Trace()) {
        std::fprintf(stderr, "map view: InitMapView got %p, not a cSC4WinMapView\n", static_cast<void*>(window));
    }
}

}  // namespace native
