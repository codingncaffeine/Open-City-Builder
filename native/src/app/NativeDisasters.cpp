// The disasters in a native city: the real IDisasterHost (the layer's and the managers' city services), the instance factory,
// the fire instance's services (IFireInstanceHost), the flame effects of cSC4FireOccupant and the flammability simulator's
// host (the simulator that starts the random fires). Installed after the stub hosts (NativeCityHosts.cpp).
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeDemolition.h"
#include "app/NativePlumbing.h"
#include "gz/cGZPersistResourceManager.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/AnimationTickManager.h"
#include "sc4/AuraSimulator.h"
#include "sc4/Demolition.h"
#include "sc4/Disaster.h"
#include "sc4/DisasterInstanceFire.h"
#include "sc4/DisasterInstanceRiot.h"
#include "sc4/DisasterManagers.h"
#include "sc4/Exemplar.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/HistoryWarehouse.h"
#include "sc4/Lot.h"
#include "sc4/Ordinance.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TutorialSystem.h"
#include "sc4/WeatherSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"
#include "tools/city_probe_plain.h"

namespace native {
const sc4::cSC4SimGrid<std::uint8_t>* NativeFlammabilityGrid();   // app/NativeAdvisorFlammability.cpp
void NativeMoveCameraTo(float x, float y, float z);                // app/NativeCitySystems.cpp
int NativeCurrentViewLevel();
void NativeStartEffectOnOccupant(const char* name, sc4::cISC4Occupant* occupant);   // app/NativeFireEffects.cpp
void NativeStartEffectAt(const char* name, const float position[3]);
void InstallFireServices();
void InstallRiotHost();   // app/NativeRiot.cpp
void NativeRiotCityClosed();
bool NativeFireSkip(const char* what);
void NativeKillFloraOccupant(sc4::cISC4Occupant* occupant);   // app/NativeFlora.cpp

namespace {
using namespace sc4;

bool Trace() {
    static const bool on = std::getenv("SC4_FIREDEBUG") != nullptr;
    return on;
}

template <class T> T* CityAs(CityComponent component) {
    cSC4City* city = HostCity();
    return city ? dynamic_cast<T*>(city->GetComponent(component)) : nullptr;
}
cISC4DisasterLayer* Layer() { return CityAs<cISC4DisasterLayer>(CityComponent::DisasterLayer); }
cISC4OccupantManager* OccupantManager() { return CityAs<cISC4OccupantManager>(CityComponent::OccupantManager); }
cSC4ZoneDeveloper* ZoneDeveloper() { return CityAs<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper); }
cSTETerrain* Terrain() { return CityAs<cSTETerrain>(CityComponent::Terrain); }

cSC4Lot* LotOf(cISC4Occupant* occupant) {
    cSC4ZoneDeveloper* lots = ZoneDeveloper();
    return lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
}

const cISCPropertyHolder* HeldExemplar(const cGZPersistResourceKey& key) {
    static std::map<std::uint32_t, cISCResExemplar*> held;   // by instance: the tuning exemplars live for the program
    auto it = held.find(key.instance);
    if (it == held.end()) {
        void* raw = nullptr;
        cISCResExemplar* exemplar =
            (SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) && raw) ? static_cast<cISCResExemplar*>(raw) : nullptr;
        it = held.emplace(key.instance, exemplar).first;
    }
    return it->second ? it->second->AsISCPropertyHolder() : nullptr;
}

void PostCityMessage(std::uint32_t type) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

// ---------------------------------------------------------------------------------------------- the disaster host

class DisasterHostImpl final : public IDisasterHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cISC4DisasterLayer* DisasterLayer() override { return Layer(); }
    cISC4OccupantManager* Occupants() override { return OccupantManager(); }
    // The automata are not in the native occupant manager: one that still has its in-city flag counts.
    bool IsOccupantInCity(cISC4Occupant* occupant) override {
        if (IDisasterHost::IsOccupantInCity(occupant)) return true;
        void* automaton = nullptr;
        if (!occupant || !occupant->QueryInterface(0xa9b40f05, &automaton) || !automaton) return false;
        occupant->Release();
        return occupant->IsFlagSet(1);
    }
    bool CityEstablished() override { cSC4City* city = HostCity(); return city && city->GetEstablished(); }
    void EmergencyPause() override { if (cSC4Simulator* s = CitySimulatorObject()) s->EmergencyPause(); }
    void EmergencyResume() override { if (cSC4Simulator* s = CitySimulatorObject()) s->EmergencyResume(); }
    void DisableSave() override { if (cSC4City* city = HostCity()) city->DisableSave(); }
    void EnableSave() override { if (cSC4City* city = HostCity()) city->EnableSave(); }
    std::uint32_t FireDispatchCount() override {
        auto* fire = CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
        return fire ? static_cast<std::uint32_t>(fire->GetTotalDispatchCount()) : 0;
    }
    std::uint32_t PoliceDispatchCount() override {
        auto* police = CityAs<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
        return police ? static_cast<std::uint32_t>(police->GetTotalDispatchCount()) : 0;
    }
    bool PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        city->PositionToCell(x, z, cellX, cellZ);
        return true;
    }
    bool CellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        city->CellCenterToPosition(cellX, cellZ, x, z);
        return true;
    }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    int CityCellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CityCellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    void MoveCamera(float x, float y, float z, int zoomLevel) override {
        NativeMoveCameraTo(x, y, z);
        (void)zoomLevel;   // PARTIAL: the jump keeps the player's zoom
    }
    bool AutoJumpToDisasters() override { return SL::spPreferences && SL::spPreferences[0xf08] != 0; }
    cISC4DisasterManager* CreateManager(std::uint32_t classId) override {
        cISC4DisasterManager* manager = nullptr;
        switch (classId) {
            case kGZCLSID_cSC4DisasterManagerVolcano: manager = new cSC4DisasterManagerVolcano(); break;
            case kGZCLSID_cSC4DisasterManagerFire: manager = new cSC4DisasterManagerFire(); break;
            case kGZCLSID_cSC4DisasterManagerEarthquake: manager = new cSC4DisasterManagerEarthquake(); break;
            case kGZCLSID_cSC4DisasterManagerPipeBurst: manager = new cSC4DisasterManagerPipeBurst(); break;
            case kGZCLSID_cSC4DisasterManagerRiot: manager = new cSC4DisasterManagerRiot(); break;
            case kGZCLSID_cSC4DisasterManagerEffect: manager = new cSC4DisasterManagerEffect(); break;
            case kGZCLSID_cSC4DisasterManagerToxic: manager = new cSC4DisasterManagerToxic(); break;
            default: return nullptr;
        }
        manager->AddRef();
        return manager;
    }
} g_disasterHost;

// ------------------------------------------------------------------------------------------ the instance factory

class InstanceFactory final : public IDisasterInstanceFactory {
public:
    cISC4DisasterInstanceFire* CreateFire() override { return new cSC4DisasterInstanceFire(); }
    cISC4DisasterInstanceVolcano* CreateVolcano() override { return nullptr; }
    cISC4DisasterInstanceRiot* CreateRiot() override { return new cSC4DisasterInstanceRiot(); }
    cISC4DisasterInstancePipeBurst* CreatePipeBurst() override { return nullptr; }
    cISC4DisasterInstanceEarthquake* CreateEarthquake() override { return nullptr; }
    IPlumbingGrid* PlumbingGrid() override { return nullptr; }
    // Mac i386 0x0033a55c: how many riots may run at once and how many leaders one may have
    bool ReadRiotTuning(int& maxInstances, int& maxLeaders) override {
        const cISCPropertyHolder* tuning = HeldExemplar(cGZPersistResourceKey{0x6534284a, kRiotTuningGroup, kRiotTuningInstance});
        if (!tuning) return false;
        std::uint32_t instances = static_cast<std::uint32_t>(maxInstances), leaders = static_cast<std::uint32_t>(maxLeaders);
        cSCPropertyHelp::GetPropertyValue(tuning, 0xcadb6b3e, instances);
        cSCPropertyHelp::GetPropertyValue(tuning, 0x2adb6b6d, leaders);
        maxInstances = static_cast<int>(instances);
        maxLeaders = static_cast<int>(leaders);
        return true;
    }
} g_factory;

// -------------------------------------------------------------------------------------- the fire instance's services

class FireInstanceHostImpl final : public IFireInstanceHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cISC4DisasterLayer* DisasterLayer() override { return Layer(); }
    cISC4OccupantManager* Occupants() override { return OccupantManager(); }
    bool HasCity() override { return HostCity() != nullptr; }
    bool HasSimulator() override { return CitySimulatorObject() != nullptr; }
    bool SimulatorPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsPaused(); }
    bool SimulatorEmergencyPaused() override { cSC4Simulator* s = CitySimulatorObject(); return s && s->IsEmergencyPaused(); }
    void RemoveSimulatorAgent(cIGZMessageTarget2* target) override { RemoveCitySimulatorAgent(target); }
    int CellCountX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    int CellCountZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    float CellWidthX() override { cSC4City* city = HostCity(); return city ? city->CellWidthX() : 16.0f; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override {
        if (cSC4City* city = HostCity()) city->PositionToCell(x, z, cellX, cellZ);
    }
    void CellCenterToPosition(int cellX, int cellZ, float& x, float& z) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(cellX, cellZ, x, z);
    }
    bool TutorialRunning() override;
    std::uint8_t Flammability(int x, int z) override {
        const cSC4SimGrid<std::uint8_t>* grid = NativeFlammabilityGrid();
        if (!grid || x < 0 || z < 0) return 0;
        if (!grid->TractIsInBounds(x >> grid->GetTractShift(), z >> grid->GetTractShift())) return 0;
        return grid->GetCellValue(x, z);
    }
    std::uint8_t FireProtection(int x, int z, int width, int height) override {
        auto* fire = CityAs<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
        const cSC4SimGrid<std::uint8_t>* grid = fire ? fire->GetFireProtectionGrid() : nullptr;
        return grid ? static_cast<std::uint8_t>(grid->GetAverageValueInCellRect(x, z, width, height)) : 0;
    }
    void AmbientWind(float& x, float& z) override {
        x = z = 0.0f;
        auto* weather = CityAs<cSC4WeatherSimulator>(CityComponent::WeatherSimulator);
        if (!weather) return;
        float direction[2] = {};
        weather->GetAmbientWind(direction);
        x = direction[0];
        z = direction[1];
    }
    void AddTransientAuraEffect(float x, float z, int effect) override {
        if (auto* aura = CityAs<cISC4AuraSimulator>(CityComponent::AuraSimulator))
            aura->AddTransientEffect(x, z, static_cast<TransientAuraEffect>(effect));
    }
    void StartEffectOnOccupant(const char* name, cISC4Occupant* occupant) override { NativeStartEffectOnOccupant(name, occupant); }
    void StartEffectAt(const char* name, const float position[3]) override { NativeStartEffectAt(name, position); }
    void KillFloraOccupant(cISC4Occupant* occupant) override {
        NativeKillFloraOccupant(occupant);
    }
    bool OccupantLotRect(cISC4Occupant* occupant, SC4CellRect& rect) override {
        cSC4Lot* lot = LotOf(occupant);
        if (!lot) return false;
        std::int32_t r[4] = {};
        lot->GetBoundingRect(r);
        rect.x0 = r[0];
        rect.z0 = r[1];
        rect.x1 = r[2];
        rect.z1 = r[3];
        return true;
    }
    void SetOccupantLotCondition(cISC4Occupant* occupant, int condition) override {
        if (NativeFireSkip("condition")) return;
        if (cSC4Lot* lot = LotOf(occupant)) lot->SetCondition(static_cast<std::uint32_t>(condition));
    }
    void DemolishRegion(const SC4CellRect& rect, std::uint32_t flags) override {
        DemolitionRegion region;
        region.bounds = {rect.x0, rect.z0, rect.x1, rect.z1};
        std::int64_t cost = 0;
        native::DemolishRegion(true, region, 0, flags, false, &cost);
        if (Trace()) std::fprintf(stderr, "fire: demolished %d,%d..%d,%d flags %x\n", rect.x0, rect.z0, rect.x1, rect.z1, flags);
    }
    void AddRadiation(const float position[3], float radius, float amount) override {
        if (auto* pollution = CityAs<cSC4PollutionSimulator>(CityComponent::PollutionSimulator))
            pollution->AddRadiationToCity(position, radius, amount);
    }
    void RecordHistory(std::uint32_t id, std::int32_t value) override {
        if (auto* history = CityAs<cSC4HistoryWarehouse>(CityComponent::HistoryWarehouse)) history->RecordHistoryData(id, value);
    }
    void AddTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->AddTickableObject(target, priority);
    }
    void RemoveTickable(cIGZMessageTarget2* target, std::uint32_t priority) override {
        if (cSC4AnimationTickManager* atm = AnimationTickManager()) atm->RemoveTickableObject(target, priority);
    }
    int CurrentViewLevel() override { return NativeCurrentViewLevel(); }
    const cISCPropertyHolder* TuningExemplar() override { return HeldExemplar(kTuningKey); }
    bool LoadTuningCurve(cSC4ResponseCurve& curve, std::uint32_t property) override {
        const cISCPropertyHolder* tuning = TuningExemplar();
        std::vector<float> points;
        if (!tuning || !cSCPropertyHelp::GetPropertyValue(tuning, property, points) || points.size() < 2) return false;
        return curve.SetResponseCurveData(points.data(), points.size());
    }

private:
    static constexpr cGZPersistResourceKey kTuningKey{0x6534284a, 0x29b6c670, 0xc9b6b73d};
} g_fireHost;

}  // namespace

bool NativeTutorialRunning();
bool FireInstanceHostImpl::TutorialRunning() { return NativeTutorialRunning(); }

void InstallDisasterHosts() {
    sc4::SetDisasterHost(&g_disasterHost);
    sc4::SetDisasterInstanceFactory(&g_factory);
    sc4::SetFireInstanceHost(&g_fireHost);
    InstallFireServices();
    InstallRiotHost();
}

// ---- what the flammability simulator's host (app/NativeFlammabilityHost.cpp) asks of the city: its header cannot be combined with
// the lot and disaster headers.
const sc4::cISCPropertyHolder* NativeHeldExemplar(const cGZPersistResourceKey& key) { return HeldExemplar(key); }
bool NativeTutorialRunning() {
    auto* tutorial = CityAs<sc4::cSC4TutorialSystem>(sc4::CityComponent::TutorialSystem);
    return tutorial && tutorial->IsTutorial();
}
// lot manager vslot 0x34 (a random lot) -> its building's position as a cell
bool NativeRandomLotBuildingCell(int& x, int& z) {
    sc4::cSC4ZoneDeveloper* lots = ZoneDeveloper();
    sc4::cSC4Lot* lot = lots ? lots->GetRandomLot() : nullptr;
    cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
    void* view = nullptr;
    if (!building || !building->QueryInterface(sc4::kGZIID_cISC4Occupant, &view) || !view) return false;
    auto* occupant = static_cast<sc4::cISC4Occupant*>(view);
    float position[3] = {};
    occupant->GetPosition(position);
    occupant->Release();
    HostCity()->PositionToCell(position[0], position[2], x, z);
    return true;
}
int NativeLotZoneTypeOfOccupant(sc4::cISC4Occupant* occupant) {
    sc4::cSC4Lot* lot = LotOf(occupant);
    return lot ? lot->GetZoneType() : -1;
}
// The building of the lot a "lot changed" message carries (iid 0xe818fced), one reference; null when it has none.
sc4::cISC4Occupant* NativeLotMessageBuilding(cIGZUnknown* unknown) {
    void* plain = nullptr;   // the message's object answers the plain lot (app/NativeZoneDeveloperHost.cpp PostLotMessage)
    sc4::cSC4Lot* lot = unknown && unknown->QueryInterface(sc4::kGZIID_PlainObject, &plain) ? static_cast<sc4::cSC4Lot*>(plain) : nullptr;
    cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
    void* view = nullptr;
    if (!building || !building->QueryInterface(sc4::kGZIID_cISC4Occupant, &view)) return nullptr;
    return static_cast<sc4::cISC4Occupant*>(view);
}
// disaster layer -> the fire manager's interface: IgniteCell(x, z, roll on a random lot, false, 0), then SetCause
void NativeStartRandomFire(int x, int z, bool onRandomLot, int cause) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire)) || !fire) return;
    const bool lit = fire->IgniteCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), onRandomLot, false, 0);
    fire->SetCause(static_cast<std::uint32_t>(cause));
    fire->Release();
    if (Trace()) std::fprintf(stderr, "fire: random start at cell %d,%d (%s) -> %s\n", x, z, onRandomLot ? "random lot" : "hotspot", lit ? "lit" : "nothing lit");
}
float NativeOrdinanceFactor(std::uint32_t property, std::uint32_t index) {
    auto* ordinances = CityAs<sc4::cSC4OrdinanceSimulator>(sc4::CityComponent::OrdinanceSimulator);
    return ordinances ? ordinances->GetCumulativeFactor(property, index) : 1.0f;
}
int NativeCurrentSeason() {
    auto* weather = CityAs<sc4::cSC4WeatherSimulator>(sc4::CityComponent::WeatherSimulator);
    return weather ? static_cast<int>(weather->GetCurrentSeason()) : 0;
}
int NativeZoneTypeAt(int x, int z) {
    auto* zones = CityAs<sc4::cSC4ZoneManager>(sc4::CityComponent::ZoneManager);
    return zones ? zones->GetZoneType(x, z) : 0;
}
void NativeRecordHistory(std::uint32_t id, std::int32_t value) {
    if (auto* history = CityAs<sc4::cSC4HistoryWarehouse>(sc4::CityComponent::HistoryWarehouse)) history->RecordHistoryData(id, value);
}

// The disaster layer's part of the city's component lifecycle (cSC4City::Init / Shutdown slot 0x18 / 0x20).
bool CityComponentInit_cSC4DisasterLayer(cIGZUnknown* object, bool init) {
    auto* layer = dynamic_cast<sc4::cSC4DisasterLayer*>(object);
    if (!layer) return false;
    if (init) {
        layer->Init();
    } else {
        layer->Shutdown();
        NativeRiotCityClosed();   // the riot host's views of the old city's road tiles
    }
    return true;
}

// The fires burning in the city (the fire engines' host: fire manager -> fire instance -> GetFireCount).
int NativeFireCount() {
    sc4::cISC4DisasterLayer* layer = Layer();
    auto* manager = dynamic_cast<sc4::cSC4DisasterManagerFire*>(layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr);
    sc4::cISC4DisasterInstanceFire* fire = manager ? manager->GetFireInstance() : nullptr;
    if (!fire) return 0;
    const int count = fire->GetFireCount();
    fire->Release();
    return count;
}

// A crime of the arson kind (crime simulator, Mac i386 0x00172cea): the fire manager's IgniteOccupant, then SetCause(2).
void NativeStartArson(sc4::cISC4Occupant* building) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!building || !manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire)) || !fire) return;
    const bool lit = fire->IgniteOccupant(building);
    fire->SetCause(2);
    fire->Release();
    if (Trace()) std::fprintf(stderr, "fire: arson -> %s\n", lit ? "lit" : "nothing lit");
}

// The view's "go to the next disaster" button and key.
void GoToNextDisaster() {
    if (sc4::cISC4DisasterLayer* layer = Layer()) layer->GoToNextDisaster();
}

// Kit command `ignite X Z [force]`: sets the occupants of a city cell alight through the fire manager (the torch's call).
void NativeIgniteCell(int x, int z, bool force) {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    sc4::cISC4DisasterManagerFire* fire = nullptr;
    if (!manager || !manager->QueryInterface(sc4::kGZIID_cISC4DisasterManagerFire, reinterpret_cast<void**>(&fire)) || !fire) {
        std::printf("ignite: no fire manager\n");
        return;
    }
    const bool lit = fire->IgniteCell(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(z), false, force, 0);
    fire->Release();
    std::printf("ignite %d,%d -> %s\n", x, z, lit ? "lit" : "nothing lit");
}

// Kit command `fires`: the fire instance's state, one line per fire.
void NativeFireReport() {
    sc4::cISC4DisasterLayer* layer = Layer();
    sc4::cISC4DisasterManager* manager = layer ? layer->GetDisasterManager(sc4::kDisasterManagerIdFire) : nullptr;
    auto* fireManager = dynamic_cast<sc4::cSC4DisasterManagerFire*>(manager);
    sc4::cISC4DisasterInstanceFire* view = fireManager ? fireManager->GetFireInstance() : nullptr;
    auto* fire = dynamic_cast<sc4::cSC4DisasterInstanceFire*>(view);
    if (!fire) {
        std::printf("fires: no fire instance (layer %p manager %p)\n", static_cast<void*>(layer), static_cast<void*>(manager));
        if (view) view->Release();
        return;
    }
    std::printf("fires: %d burning, phase %d, severity %u, cause %u, damaged %d, toast %zu, ongoing %d\n", fire->GetFireCount(),
                fire->GetPhase(), fire->Severity(), fire->GetCause(), fire->GetDamagedOccupantsCount(), fire->ToastCount(),
                layer->IsADisasterOngoing() ? 1 : 0);
    for (sc4::cSC4FireOccupant* f : fire->Fires()) {
        float p[3] = {};
        f->GetPosition(p);
        std::printf("  fire at %.0f,%.0f,%.0f target type %08x stage %u/%u counter %u hosed %u flames %zu family %08x\n", p[0], p[1], p[2],
                    f->GetTarget() ? f->GetTarget()->GetType() : 0, f->GetStage(), f->GetMaxStage(), f->GetCounter(), f->GetHosed(),
                    f->Flames().size(), f->GetFamily());
    }
    view->Release();
}

}  // namespace native
