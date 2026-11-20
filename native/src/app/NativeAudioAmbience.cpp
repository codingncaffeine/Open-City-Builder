// The ambience system's questions about the city on the native application: the ambience manager's host
// (sc4audio/AudioAmbienceManager.h), the ambience layers' host (sc4audio/AmbienceLayer.h) and the three filter families'
// (sc4/AmbienceStateFilters.h, AmbienceOccupantFilters.h, AmbienceCellFilters.h). Each answer is the call the original makes
// on the service locator's objects; the slot of each call was read from the Mac build's vtables:
//   lot manager GetLot (0x28) / GetOccupantLot (0x70); lot GetLotConfiguration (0x48) -> GetGrowthStage, GetZoneType (0xf8),
//   GetState (0x108), GetCondition (0x118), GetOccupantWealth (0x148), GetBuildingType (0x170), GetBuilding (0x180),
//   GetLotOccupants (0x188); zone developer ExistsNetworkOfType (0x40) / IsCellWater (0x48); zone manager GetZoneType (0x60);
//   crime simulator GetCrimeGrid (0x28); land value simulator GetTotalLandValueMap (0x40); occupant manager
//   GetFirstOccupantByStandardCityCell (0x100); building development simulator GetBuildingProfile (0x138);
//   simulator IsPaused (0x78) / IsEmergencyPaused (0x88) / GetSimSpeed (0xd8); lighting manager IsNight (0x48);
//   3D view GetCurrentViewLevel (0xb0); automaton AsOccupant (0x30) / GetPosition (0x38) / AddCallback (0x138) /
//   RemoveCallback (0x140) / FindState (0x1c8).
// The seams name a lot's condition "state" and its state "construction phase"; the slots above are what they call.
//
// The game's lots and automata are plain classes here, where the ambience code expects small interfaces: a view object per
// lot / automaton answers them. A view holds only the pointer it stands for; the views are dropped when the city's ambience
// stops (the manager leaves the tick list).
#include <cstdio>
#include <cstdlib>
#include <list>
#include <unordered_map>
#include <vector>

#include "app/NativeCity.h"
#include "app/NativeCitySystems.h"
#include "app/NativeLotConstruction.h"
#include "audio/AudioServices.h"
#include "gz/GZCOMDirector.h"
#include "gz/GZFramework.h"
#include "gz/RZServices.h"
#include "sc4/AmbienceCellFilters.h"
#include "sc4/AmbienceOccupantFilters.h"
#include "sc4/AmbienceStateFilters.h"
#include "sc4/Automaton.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/Exemplar.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/LightingManager.h"
#include "sc4/Lot.h"
#include "sc4/LotConfiguration.h"
#include "sc4/OccupantManager.h"
#include "sc4/RenderProperties.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/ViewUtilities.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrain.h"
#include "sc4audio/AmbienceLayer.h"
#include "sc4audio/AudioAmbienceManager.h"
#include "sc4audio/AudioCOMDirector.h"
#include "sc4audio/AudioScapeManager.h"
#include "sc4audio/SoundManager.h"
#include "tools/city_probe_plain.h"

namespace native {
sc4::cSC4BuildingDevelopmentSimulator* NativeBuildingDevelopmentSimulator();   // app/NativeBuildingDevelopment.cpp
sc4::cSC4CrimeSimulator* NativeCrimeSimulator();                               // app/NativeCrime.cpp
int NativeResidentialPopulation();                                             // app/NativeSimHosts.cpp
sc4::cSC4LightingManager* LightingManager();                                   // app/NativeLighting.cpp

namespace {
using namespace sc4;

template <class T> T* Serializable(CityComponent c, std::uint32_t clsid) {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(c) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = serializable->GetGZCLSID() == clsid ? static_cast<T*>(serializable) : nullptr;
    serializable->Release();
    return out;
}
cSC4ZoneDeveloper* Lots() { return Serializable<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper, cSC4ZoneDeveloper::kZoneDeveloperClassId); }
cSC4ZoneManager* Zones() { return Serializable<cSC4ZoneManager>(CityComponent::ZoneManager, kZoneManagerClassId); }
cSC4LandValueSimulator* LandValue() { return Serializable<cSC4LandValueSimulator>(CityComponent::LandValueSimulator, kGZCLSID_cSC4LandValueSimulator); }
cSC424HourClock* Clock() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Clock24Hour) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
    return static_cast<cSC424HourClock*>(raw);
}
cISC4OccupantManager* Occupants() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::OccupantManager) : nullptr;
    void* raw = nullptr;
    if (!object || !object->QueryInterface(kGZIID_cISC4OccupantManager, &raw) || !raw) return nullptr;
    auto* manager = static_cast<cISC4OccupantManager*>(raw);
    manager->Release();   // borrowed: the city keeps it
    return manager;
}
cSTETerrain* Terrain() {
    cSC4City* city = HostCity();
    cIGZUnknown* object = city ? city->GetComponent(CityComponent::Terrain) : nullptr;
    void* serializable = nullptr;
    if (!object || !object->QueryInterface(0xab953253, &serializable)) return nullptr;
    auto* terrain = static_cast<cSTETerrain*>(static_cast<cIGZSerializable*>(serializable));
    static_cast<cIGZSerializable*>(serializable)->Release();
    return terrain;
}

// The building of a lot as a building occupant (lot vslot 0x180), borrowed.
cISC4BuildingOccupant* BuildingOf(const cSC4Lot* lot) { return static_cast<cISC4BuildingOccupant*>(lot->GetBuilding()); }

// ---- a lot, as the three ambience seams see it -----------------------------------------------------------------------------
// The lots belong to the zone developer and are not counted, so the views' reference counts do nothing.
class AmbienceLot final : public SC4Audio::IAmbienceLot {
public:
    const cSC4Lot* lot = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    std::int32_t State() override { return lot->GetCondition(); }
    void GetOccupants(std::list<cISC4Occupant*>& out) override {   // one reference each: the caller releases the list
        std::vector<cIGZUnknown*> found;
        lot->GetLotOccupants(found);
        for (cIGZUnknown* object : found) {
            void* raw = nullptr;
            if (object && object->QueryInterface(kGZIID_cISC4Occupant, &raw) && raw) out.push_back(static_cast<cISC4Occupant*>(raw));
        }
    }
};
class LotInfo final : public ISC4LotInfo {
public:
    const cSC4Lot* lot = nullptr;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    std::int32_t GetState() override { return lot->GetCondition(); }
    std::int32_t GetWealth() override { return lot->GetOccupantWealth(); }
};
class CellLot final : public ISC4CellLot {
public:
    const cSC4Lot* lot = nullptr;
    std::int32_t GetState() override { return lot->GetCondition(); }
    std::int32_t GetWealth() override { return lot->GetOccupantWealth(); }
    std::int32_t GetConstructionPhase() override { return lot->GetState(); }
    bool GetBuildingZone(std::int32_t& zone) override {   // the building's profile: its first field
        cISC4BuildingOccupant* building = BuildingOf(lot);
        if (!building) return false;
        zone = static_cast<std::int32_t>(building->GetBuildingProfile().purpose);
        return true;
    }
    cISC4Occupant* GetBuildingOccupant() override { cISC4BuildingOccupant* building = BuildingOf(lot); return building ? building->AsOccupant() : nullptr; }
    bool GetGrowthStage(std::uint8_t& stage) override {
        cSC4LotConfiguration* configuration = NativeLotConfiguration(lot->GetLotConfiguration());
        if (!configuration) return false;
        stage = configuration->GetGrowthStage();
        return true;
    }
    std::int32_t GetZoneCode() override { return lot->GetZoneType(); }
    std::int32_t GetBuildingTypeId(int fromConfiguration) override { return static_cast<std::int32_t>(lot->GetBuildingType(fromConfiguration != 0)); }
};
struct LotViews {
    AmbienceLot ambience;
    LotInfo info;
    CellLot cell;
};

// ---- an automaton, as the ambience manager sees it --------------------------------------------------------------------------
// The view's references are the automaton's own.
class AutomatonView final : public SC4Audio::IAmbienceAutomaton {
public:
    cSC4Automaton* automaton = nullptr;
    bool QueryInterface(std::uint32_t iid, void** out) override { return automaton->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return automaton->AddRef(); }
    std::uint32_t Release() override { return automaton->Release(); }
    cISC4Occupant* AsOccupant() override { return automaton->AsOccupant(); }
    void GetPosition(float* xyz) override { automaton->GetPosition(xyz); }
    void AddCallback(cIGZMessageTarget2* target) override { automaton->AddCallback(target); }
    void RemoveCallback(cIGZMessageTarget2* target) override { automaton->RemoveCallback(target); }
    void* FindState(std::uint32_t state) override { return automaton->FindState(state); }
};

std::unordered_map<const cSC4Lot*, LotViews> g_lotViews;
std::unordered_map<cSC4Automaton*, AutomatonView> g_automatonViews;

LotViews* ViewsOf(const cSC4Lot* lot) {
    if (!lot) return nullptr;
    LotViews& views = g_lotViews[lot];
    views.ambience.lot = views.info.lot = views.cell.lot = lot;
    return &views;
}
AutomatonView* ViewOf(cSC4Automaton* automaton) {
    if (!automaton) return nullptr;
    AutomatonView& view = g_automatonViews[automaton];
    view.automaton = automaton;
    return &view;
}
cSC4Lot* LotOf(cISC4Occupant* occupant) { cSC4ZoneDeveloper* lots = Lots(); return lots && occupant ? lots->GetOccupantLot(occupant) : nullptr; }
cSC4Lot* LotAtCell(int x, int z) { cSC4ZoneDeveloper* lots = Lots(); return lots ? lots->GetLot(x, z, false) : nullptr; }

// ---- the ambience manager ---------------------------------------------------------------------------------------------------
class ManagerHost final : public SC4Audio::IAmbienceManagerHost {
public:
    bool HasFramework() override { return cGZFramework::FrameWork() != nullptr; }
    void AddSystemService(cIGZSystemService* service) override { cGZFramework::FrameWork()->AddSystemService(service); }
    void RemoveSystemService(cIGZSystemService* service) override { cGZFramework::FrameWork()->RemoveSystemService(service); }
    void AddToTick(cIGZSystemService* service) override { cGZFramework::FrameWork()->AddToTick(service); }
    void RemoveFromTick(cIGZSystemService* service) override {
        cGZFramework::FrameWork()->RemoveFromTick(service);
        g_lotViews.clear();          // the city's ambience stops: its lots and automata go with the city
        g_automatonViews.clear();
    }
    cIGZUnknown* AudioScapeManager() override {
        void* found = nullptr;
        if (!cGZFramework::FrameWork()->GetSystemService(kGZSID_cISC4AudioScapeManager, kGZIID_cISC4AudioScapeManager, &found) || !found) return nullptr;
        auto* scape = static_cast<cISC4AudioScapeManager*>(found);
        scape->Release();   // borrowed: the manager takes its own reference
        return scape;
    }
    cIGZSndSys* SoundSystem() override { return Audio::SoundSystem(); }
    cIGZMessageServer2* MessageServer() override { return RZ::MS2(); }
    cISC4OccupantManager* OccupantManager() override { return Occupants(); }
    SC4Audio::IAmbienceLot* LotOfOccupant(cISC4Occupant* occupant) override {
        LotViews* views = ViewsOf(LotOf(occupant));
        return views ? &views->ambience : nullptr;
    }
    void CityCellExtent(int& cellsX, int& cellsZ) override {
        cSC4City* city = HostCity();
        cellsX = city ? city->CellCountX() : 0;
        cellsZ = city ? city->CellCountZ() : 0;
    }
    bool CreateInstance(std::uint32_t classId, std::uint32_t iid, void** out) override {
        cIGZCOM* com = GZCOM();
        return com && com->CreateInstance(classId, iid, out);
    }
    bool HasTuning() override { Tuning tuning; return tuning.properties != nullptr; }
    bool GetTuningFloats(std::uint32_t propertyId, std::vector<float>& out) override {
        Tuning tuning;
        return tuning.properties && cSCPropertyHelp::GetPropertyValue(tuning.properties, propertyId, out);
    }
    bool GetTuningUints(std::uint32_t propertyId, std::vector<std::uint32_t>& out) override {
        Tuning tuning;
        return tuning.properties && cSCPropertyHelp::GetPropertyValue(tuning.properties, propertyId, out);
    }
    bool ClockTime(int& hour, int& minute) override {
        cSC424HourClock* clock = Clock();
        if (!clock) return false;
        hour = static_cast<int>(clock->GetHour());
        minute = static_cast<int>(clock->GetMinute());
        return true;
    }
    std::uint32_t GenerateNewInstanceId() override { return cSC4AudioSoundManager::GenerateNewInstanceId(); }
    void LogError(const char* message) override { if (std::getenv("SC4_AUDIODEBUG")) std::fprintf(stderr, "ambience: %s\n", message); }

    SC4Audio::IAmbienceAutomaton* AutomatonOfObject(cIGZUnknown* object) override {   // one reference: the automaton's
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kAutomatonIid, &raw) || !raw) return nullptr;
        return ViewOf(static_cast<cSC4Automaton*>(raw));
    }
    SC4Audio::IAmbienceAutomaton* AutomatonOfCallbackPayload(void* payload) override { return ViewOf(static_cast<cSC4Automaton*>(payload)); }
    SC4Audio::IAmbienceLot* LotOfObject(cIGZUnknown* object) override {   // a lot message carries the lot behind its plain view
        void* raw = nullptr;
        if (!object || !object->QueryInterface(kGZIID_PlainObject, &raw) || !raw) return nullptr;
        LotViews* views = ViewsOf(static_cast<cSC4Lot*>(raw));
        return views ? &views->ambience : nullptr;
    }

private:
    struct Tuning {   // SL::spRM->GetResource({0x6534284a, 0xca25875d, 0x6a359c5c}, 0xa52160f5)
        Tuning() {
            void* raw = nullptr;
            const cGZPersistResourceKey key{SC4Audio::kAmbienceManagerTuningType, SC4Audio::kAmbienceManagerTuningGroup, SC4Audio::kAmbienceManagerTuningInstance};
            if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return;
            exemplar = static_cast<cISCResExemplar*>(raw);
            properties = exemplar->AsISCPropertyHolder();
        }
        ~Tuning() { if (exemplar) exemplar->Release(); }
        cISCResExemplar* exemplar = nullptr;
        cISCPropertyHolder* properties = nullptr;
    };
};

// ---- the ambience layers ----------------------------------------------------------------------------------------------------
class LayerHost final : public SC4Audio::IAmbienceLayerHost {
public:
    bool HasResourceManager() override { return SL::spRM != nullptr; }
    cISCPropertyHolder* GetTuningExemplar(const cGZPersistResourceKey& key) override {   // an AddRef'd holder, or null
        void* raw = nullptr;
        if (!SL::spRM || !SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) || !raw) return nullptr;
        return static_cast<cISCResExemplar*>(raw)->AsISCPropertyHolder();
    }
    SC4Audio::IAmbienceSndSink* AcquireSoundSink() override {
        cIGZSndSys* system = Audio::SoundSystem();
        if (!system) return nullptr;
        if (!sink_ || sinkSystem_ != system) {
            delete sink_;
            sink_ = new SC4Audio::cAmbienceSndSysAdapter(system);
            sinkSystem_ = system;
        }
        return sink_;
    }
    bool CreateObject(std::uint32_t classId, std::uint32_t iid, void** out) override {
        cIGZCOM* com = GZCOM();
        return com && com->CreateInstance(classId, iid, out);
    }
    // SL::spRenderProperties' int property 19, "kSoundDetail" (the vector at +0x30, 0x48 bytes an entry: +0x590 is its value).
    int RenderAudioQuality() override {
        constexpr std::uint32_t kSoundDetail = 19;
        if (auto* properties = dynamic_cast<cSC4RenderProperties*>(SL::spRenderProperties);
            properties && properties->NumIntProperties() > static_cast<std::int32_t>(kSoundDetail))
            return properties->IntValue(kSoundDetail);
        return DefaultRenderProperties().ints[kSoundDetail].value;
    }
    bool CellToWorld(int cellX, int cellZ, float& x, float& z) override {
        cSC4City* city = HostCity();
        if (!city) return false;
        city->CellCenterToPosition(cellX, cellZ, x, z);
        return true;
    }
    float TerrainAltitude(float x, float z) override { cSTETerrain* terrain = Terrain(); return terrain ? terrain->GetAltitude(x, z) : 0.0f; }
    int ResidentialPopulation() override { return NativeResidentialPopulation(); }
    std::uint32_t IndustrialPopulation() override {   // SFX::AmbienceManager()->GetIndustrialPopulation()
        SC4Audio::cSC4AudioAmbienceManager* manager = GetSC4AudioCOMDirector()->AmbienceManager();
        return manager ? manager->GetIndustrialPopulation() : 0;
    }
    std::uint32_t GenerateNewInstanceId() override { return cSC4AudioSoundManager::GenerateNewInstanceId(); }
    void LogError(const char* message) override { if (std::getenv("SC4_AUDIODEBUG")) std::fprintf(stderr, "ambience layer: %s\n", message); }

private:
    SC4Audio::cAmbienceSndSysAdapter* sink_ = nullptr;
    cIGZSndSys* sinkSystem_ = nullptr;
};

// ---- the filters -------------------------------------------------------------------------------------------------------------
class StateHost final : public IAmbienceStateHost {
public:
    bool IsNight() override { cSC4LightingManager* lighting = LightingManager(); return lighting && lighting->IsNight(); }
    bool IsDrivingMode() override { return false; }   // the vehicle manager's driving mode (U-Drive-It) is not in the build yet
    int View3DLevel() override { return CityViewLevel(); }
    int SimSpeed() override { cSC4Simulator* simulator = CitySimulatorObject(); return simulator ? simulator->GetSimSpeed() : 0; }
    // The seam's names for simulator vslots 0x78 and 0x88: IsPaused and IsEmergencyPaused.
    bool SimIsActive() override { cSC4Simulator* simulator = CitySimulatorObject(); return simulator && simulator->IsPaused(); }
    bool SimIsRunning() override { cSC4Simulator* simulator = CitySimulatorObject(); return simulator && simulator->IsEmergencyPaused(); }
    int ZoomLevel() override {
        int zoom = 0, rotation = 0;
        if (SL::spViewUtilities) SL::spViewUtilities->GetZoomAndRotation(zoom, rotation);
        return zoom;
    }
};

class OccupantWorld final : public IAmbienceWorld {
public:
    std::int32_t CityCellsX() override { cSC4City* city = HostCity(); return city ? city->CellCountX() : 0; }
    std::int32_t CityCellsZ() override { cSC4City* city = HostCity(); return city ? city->CellCountZ() : 0; }
    ISC4LotInfo* LotAtCell(int x, int z) override { LotViews* views = ViewsOf(native::LotAtCell(x, z)); return views ? &views->info : nullptr; }
    ISC4LotInfo* LotOfOccupant(cISC4Occupant* occupant) override { LotViews* views = ViewsOf(LotOf(occupant)); return views ? &views->info : nullptr; }
};

class CellWorld final : public IAmbienceCellWorld {
public:
    int ZoneTypeAt(int x, int z) override { cSC4ZoneManager* zones = Zones(); return zones ? zones->GetZoneType(x, z) : 0; }
    ISC4CellLot* LotAt(int x, int z) override { LotViews* views = ViewsOf(native::LotAtCell(x, z)); return views ? &views->cell : nullptr; }
    bool ZoneDeveloperHas(int x, int z, int mask) override {
        cSC4ZoneDeveloper* developer = Lots();
        return developer && developer->ExistsNetworkOfType(x, z, static_cast<std::uint32_t>(mask));
    }
    bool ZoneDeveloperIsWater(int x, int z) override { cSC4ZoneDeveloper* developer = Lots(); return developer && developer->IsCellWater(x, z); }
    bool HasCrimeSim() override { return NativeCrimeSimulator() != nullptr; }
    std::uint8_t CrimeAt(int x, int z) override {
        cSC4CrimeSimulator* crime = NativeCrimeSimulator();
        const cSC4SimGrid<std::uint8_t>* grid = crime ? crime->GetCrimeGrid() : nullptr;
        return grid ? grid->GetCellValue(x, z) : 0;
    }
    bool HasLandValueSim() override { return LandValue() != nullptr; }
    std::uint8_t DesirabilityAt(int x, int z) override {
        cSC4LandValueSimulator* landValue = LandValue();
        const cSC4SimGrid<std::uint8_t>* grid = landValue ? landValue->GetTotalLandValueMap() : nullptr;
        return grid ? grid->GetCellValue(x, z) : 0;
    }
    bool CellHasOccupant(int x, int z) override {
        cISC4OccupantManager* occupants = Occupants();
        cISC4Occupant* occupant = nullptr;
        if (!occupants || !occupants->GetFirstOccupantByStandardCityCell(occupant, x, z, 0u) || !occupant) return false;
        occupant->Release();
        return true;
    }
    bool BuildingZoneById(std::int32_t id, std::int32_t& zone) override {
        cSC4BuildingDevelopmentSimulator* simulator = NativeBuildingDevelopmentSimulator();
        const BuildingProfile* profile = simulator ? simulator->GetBuildingProfilePtr(static_cast<std::uint32_t>(id)) : nullptr;
        if (!profile) return false;
        zone = static_cast<std::int32_t>(profile->purpose);
        return true;
    }
};

}  // namespace

void InstallAmbienceHosts() {
    static ManagerHost manager;
    static LayerHost layers;
    static StateHost state;
    static OccupantWorld occupants;
    static CellWorld cells;
    SC4Audio::SetAmbienceManagerHost(&manager);
    SC4Audio::SetAmbienceLayerHost(&layers);
    sc4::SetAmbienceStateHost(&state);
    sc4::SetAmbienceWorld(&occupants);
    sc4::SetAmbienceCellWorld(&cells);
}

}  // namespace native
