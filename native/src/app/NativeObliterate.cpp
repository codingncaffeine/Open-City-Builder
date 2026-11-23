// The "Obliterate City" scene's services on the native application (sc4/ObliterateCity.h's IObliterateCityHost): the tuning
// exemplar, the city's save switch, the simulator's hidden pause, the situation indicator, the animation ticks, demolition, the
// occupant manager, the zone manager and the named particle effects.
#include "sc4/Render3D.h"   // first: its filter forward declaration must not meet sc4/OccupantManager.h's first

#include <cstdio>
#include <cstdlib>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "app/NativeDisasterTools.h"
#include "gz/cGZPersistResourceManager.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/CitySituationManager.h"
#include "sc4/Demolition.h"
#include "sc4/Exemplar.h"
#include "sc4/ObliterateCity.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/cSC4City.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/View3DWin.h"
#include "sc4/ViewUtilities.h"
#include "sc4/App.h"
#include "app/NativeBoot.h"
#include "app/NativeCitySystems.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"

namespace native {
namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_OBLITERATEDEBUG") != nullptr;
    return on;
}

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
template <class T> T* Serializable(CityComponent c, std::uint32_t clsid) {
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? dynamic_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cISC4OccupantManager* Occupants() {
    cIGZUnknown* object = Component(CityComponent::OccupantManager);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();   // the city keeps it
    return manager;
}
cSC4CitySituationManager* Situations() { return HostCitySituations(); }

sc4::cSTETerrain* CityTerrainComponent() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Terrain) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &raw) || !raw) return nullptr;
    auto* terrain = static_cast<sc4::cSTETerrain*>(static_cast<cIGZSerializable*>(raw));
    static_cast<cIGZSerializable*>(raw)->Release();   // the city keeps it
    return terrain;
}

// The tool's occupant filter seen by demolition (the demolition service asks through its own occupant view).
class DemolishFilter final : public IDemolishFilter {
public:
    explicit DemolishFilter(sc4::cISC4OccupantFilter* filter) : filter_(filter) {}
    bool IsOccupantIncluded(IDemolishable* occupant) override {
        sc4::cISC4Occupant* raw = DemolitionOccupant(occupant);
        return !filter_ || (raw && filter_->IsOccupantIncluded(raw));
    }
    bool IsOccupantTypeIncluded(std::uint32_t type) override { return !filter_ || filter_->IsOccupantTypeIncluded(type); }

private:
    sc4::cISC4OccupantFilter* filter_;
};

// A named effect handle (app/NativeDisasterTools.cpp).
class Effect final : public IObliterateEffect {
public:
    explicit Effect(void* handle) : handle_(handle) {}
    void Stop(bool hard) override { NativeStopEffectOnly(handle_, hard ? 1 : 0); }
    void Release() override {
        NativeReleaseEffectHandle(handle_);
        delete this;
    }

private:
    void* handle_;
};

struct ObliterateHost final : IObliterateCityHost {
    Tuning ReadTuning() override {
        Tuning tuning;
        if (!SL::spRM) return tuning;
        void* resource = nullptr;
        if (!SL::spRM->GetResource(cGZPersistResourceKey{0x6534284a, 0x7a4a8458, 0xca5a576f}, 0xa52160f5, &resource, 0, nullptr) || !resource)
            return tuning;
        cISCPropertyHolder* holder = static_cast<cISCResExemplar*>(resource)->AsISCPropertyHolder();
        std::uint32_t size = 0;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x6a5a6700, size)) tuning.blockSize = size;
        std::vector<float> curve;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x6a5a6701, curve))
            for (std::size_t i = 0; i + 1 < curve.size(); i += 2) tuning.curve.push_back({curve[i], curve[i + 1]});
        float smoke = 0.0f;
        if (cSCPropertyHelp::GetPropertyValue(holder, 0x6a5a6702, smoke)) tuning.smokeSeconds = smoke;
        static_cast<cIGZUnknown*>(resource)->Release();
        if (Trace()) std::fprintf(stderr, "obliterate: tuning block %u, %zu curve points, smoke %.2f s\n", tuning.blockSize, tuning.curve.size(), tuning.smokeSeconds);
        return tuning;
    }
    void CityCellCount(int& x, int& z) override {
        cSC4City* city = HostCity();
        x = city ? city->CellCountX() : 0;
        z = city ? city->CellCountZ() : 0;
    }
    void DisableSave() override { if (cSC4City* city = HostCity()) city->DisableSave(); }
    void EnableSave() override { if (cSC4City* city = HostCity()) city->EnableSave(); }
    void SimulatorHiddenPause() override { if (cSC4Simulator* simulator = CitySimulatorObject()) simulator->HiddenPause(); }
    void SimulatorHiddenResume() override { if (cSC4Simulator* simulator = CitySimulatorObject()) simulator->HiddenResume(); }
    bool SituationIndicatorVisible() override { cSC4CitySituationManager* s = Situations(); return s && s->GetVisible(); }
    void SetSituationIndicatorVisible(bool visible) override { if (cSC4CitySituationManager* s = Situations()) s->SetVisible(visible); }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void AddTick(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTick(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    void DemolishCells(int x0, int z0, int x1, int z1, sc4::cISC4OccupantFilter* filter) override {
        DemolitionRegion region;
        region.bounds = DemolitionCellRect{x0, z0, x1, z1};
        DemolishFilter adapter(filter);
        std::int64_t cost = 0;
        const bool ok = native::DemolishRegion(true, region, 0, 0x2000, false, &cost, nullptr, 0, 0, &adapter);
        if (Trace()) std::fprintf(stderr, "obliterate: demolish %d,%d..%d,%d -> %d\n", x0, z0, x1, z1, ok);
    }
    bool CellsOccupied(int x0, int z0, int x1, int z1, sc4::cISC4OccupantFilter* filter) override {
        cISC4OccupantManager* manager = Occupants();
        if (!manager) return false;
        OccupantList found;
        const int a[2] = {x0, z0}, b[2] = {x1, z1};
        manager->GetOccupantsByStandardCityCells(found, a, b, filter, 1);
        const bool occupied = !found.empty();
        for (sc4::cISC4Occupant* occupant : found) occupant->Release();
        return occupied;
    }
    void RemoveOccupants(sc4::cISC4OccupantFilter* filter) override {
        if (cISC4OccupantManager* manager = Occupants()) manager->RemoveOccupants(filter, true, 0);
    }
    // the dezoning helper's PlaceZone(whole city, type 0, commit, skip funds, check slope, skip size check, no lot test)
    void DezoneCity(int maxX, int maxZ) override {
        cSC4ZoneManager* zones = Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId);
        if (!zones) return;
        SC4CellRegion<int> region = SC4CellRegion<int>::Make(SC4Rect<int>{0, 0, maxX, maxZ}, true);
        zones->PlaceZone(region, 0, true, true, true, true, false, nullptr, nullptr, nullptr);
    }
    IObliterateEffect* CreateEffect(const char* name, float x, float z, bool keep, std::uint32_t endMessage) override {
        cSTETerrain* terrain = CityTerrainComponent();
        if (!terrain) return nullptr;
        const float xyz[3] = {x, terrain->GetAltitude(x, z), z};
        void* handle = endMessage ? NativeCreateEffectWithFinishMessage(name, xyz, endMessage, 0) : NativeCreateEffectAtPoint(name, xyz);
        if (Trace()) std::fprintf(stderr, "obliterate: effect %s at %.0f,%.0f -> %s\n", name, x, z, handle ? "made" : "none");
        if (!handle) return nullptr;
        if (!keep) {
            NativeReleaseEffectHandle(handle);
            return nullptr;
        }
        return new Effect(handle);
    }
};

}  // namespace

void InstallObliterateCityHost() {
    static ObliterateHost host;
    SetObliterateCityHost(&host);
    // cSC4App::DoCityObliteration keeps the camera: the zoom and rotation from the view utilities (+0x14 / +0x18) before the
    // old city closes, and the new city's view set to them afterwards (spView3D: cISC4View3DWin::SetZoomAndRotation).
    if (cSC4App* app = BootApplication()) {
        app->CityHooks().viewZoomAndRotation = [](int& zoom, int& rotation) {
            if (SC4ViewUtilities* utilities = SL::spViewUtilities) utilities->GetZoomAndRotation(zoom, rotation);
        };
        app->CityHooks().setViewZoomAndRotation = [](int zoom, int rotation) {
            if (sc4::cSC4View3DWin* view = CityView3DWin()) view->SetZoomAndRotation(zoom, rotation);
        };
    }
}

sc4::cSC4ViewInputControl* CreateObliterateCityTool() { return new sc4::cSC4ObliterateCity(); }

}  // namespace native
