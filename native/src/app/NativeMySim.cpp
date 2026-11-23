// The MySim on the native application: the services cSC4MySim (IMySimHost), cSC4MySimDispatch (IMySimDispatchHost),
// the MySim occupant filters (IMySimFilterHost) and cSC4MySimAdvice (IMySimAdviceHost) reach. Each method is the city
// component the original reaches through its SL:: global; the slot comments name the original call (Mac i386 decompile).
//
// Not native yet (the methods report SC4_UNWIRED with the missing class): the MySim UI neighbours (cSC4WinCalloutBox over the
// residence, the four MySim input controls, the portrait bitmaps), the ghost
// effect and the U-Drive-It view. A MySim without them still lives: it is stored and read back with its city, keeps its
// residence, job and commute state, ages, rides its trips and runs its advices.
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeAutomataViews.h"
#include "app/NativeBuildingDevelopment.h"
#include "app/NativeCitySystems.h"
#include "audio/AudioServices.h"
#include "audio/interfaces/cIGZSndSys.h"
#include "gz/GZCOMDirector.h"
#include "gz/WinGen.h"
#include "app/NativeCity.h"
#include "app/NativeGameData.h"
#include "app/NativeGrowthDevelopers.h"
#include "app/NativePlumbing.h"
#include "app/NativeUi.h"
#include "gz/WinCtrlFactory.h"
#include "gz/WinCtrlMgr.h"
#include "gz/WinFont.h"
#include "sc4/AuraSimulator.h"
#include "sc4/AutomataManager.h"
#include "sc4/BudgetSimulator.h"
#include "sc4/AutomataStates.h"
#include "sc4/Automaton.h"
#include "sc4/BuildingDevelopmentSimulator.h"
#include "sc4/DispatchManager.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/Disaster.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/ISC4RegionalCity.h"
#include "sc4/Occupant.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PropManager.h"
#include "sc4/PropOccupant.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/RegionalCity.h"
#include "gz/CommandParameterSet.h"
#include "gz/cIGZVariant.h"
#include "platform/Unwired.h"
#include "sc4/CitySituationEntities.h"
#include "sc4/AdvisorCitySituations.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/AdviceSubject.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/DBSegment.h"
#include "sc4/GameDataRegistryHelpers.h"
#include "sc4/Lot.h"
#include "sc4/MySim.h"
#include "sc4/MyStreetSim.h"
#include "sc4/MySimInputControls.h"
#include "sc4/MySimAdvice.h"
#include "sc4/MySimAgentSimulator.h"
#include "sc4/MySimBalloonWinProc.h"
#include "sc4/MySimDispatch.h"
#include "sc4/MySimFilters.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/OccupantManager.h"
#include "sc4/PathFinder.h"
#include "sc4/PylonOccupant.h"
#include "sc4/Region.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/ServiceLocator.h"
#include "sc4/Simulator.h"
#include "sc4/TrafficAutomaton.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSTETerrain.h"

namespace native {
sc4::cISC4View3DWin* NativeStreetSimView();   // app/NativeView3DWinHost.cpp: the city view as cISC4View3DWin
sc4::cSC4ViewInputControl* NativeViewCurrentControl();                        // app/NativeView3DWinHost.cpp
void NativeViewSetControl(sc4::cSC4ViewInputControl* control, int operation);
void NativeViewRemoveControl();

cIGZWin* NativeCityView3DWindow();                                                // app/NativeCityUi.cpp
const sc4::cISCPropertyHolder* NativeSimulatorExemplar(std::uint32_t instance);   // app/NativeSimHosts.cpp
bool NativeProjectToViewport(const float* xyz, float out[3]);                     // app/NativeAutomataManagerHost.cpp
sc4::cSC4CrimeSimulator* NativeCrimeSimulator();                                  // app/NativeCrime.cpp
sc4::IResidentialOccupant* NativeResidentialOccupant(cIGZUnknown* building);      // app/NativeSimHosts.cpp
sc4::cSC4AutomataManager* NativeAutomataManagerAt(int index);                     // app/NativeAutomataManagerHost.cpp

namespace {
using namespace sc4;
constexpr std::uint32_t kResidentialSimulatorExemplar = 0x69189c84;

cIGZUnknown* Component(CityComponent c) {
    cSC4City* city = HostCity();
    return city ? city->GetComponent(c) : nullptr;
}
template <class T> T* Comp(CityComponent c) {   // a serializable component as its concrete class
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* out = dynamic_cast<T*>(serializable);
    serializable->Release();
    return out;
}
template <class T> T* Plain(CityComponent c) {   // a component the port keeps as a plain C++ object
    cIGZUnknown* object = Component(c);
    void* raw = nullptr;
    return object && object->QueryInterface(kGZIID_PlainObject, &raw) ? static_cast<T*>(raw) : nullptr;
}
template <class T> T* As(CityComponent c) {
    cIGZUnknown* object = Component(c);
    return object ? dynamic_cast<T*>(object) : nullptr;
}

cSC4ZoneDeveloper* LotManager() { return As<cSC4ZoneDeveloper>(CityComponent::ZoneDeveloper); }
cISC4OccupantManager* Occupants() { return As<cISC4OccupantManager>(CityComponent::OccupantManager); }
cSC4ResidentialSimulator* Residential() { return Comp<cSC4ResidentialSimulator>(CityComponent::ResidentialSimulator); }
cSC4TrafficSimulator* Traffic() { return Comp<cSC4TrafficSimulator>(CityComponent::TrafficSimulator); }
cSC4MySimAgentSimulator* Agent() { return Comp<cSC4MySimAgentSimulator>(CityComponent::MySimAgentSimulator); }
cSC4Simulator* Simulator() { return Comp<cSC4Simulator>(CityComponent::Simulator); }
cSC424HourClock* Clock24() { return Plain<cSC424HourClock>(CityComponent::Clock24Hour); }
cSTETerrain* Terrain() { return As<cSTETerrain>(CityComponent::Terrain); }
cISC4NeighborsSimulator* Neighbors() { return As<cISC4NeighborsSimulator>(CityComponent::NeighborsSimulator); }

// The MySim home sound (PlayHomeSound, i386 0x00543126): a prop occupant the prop manager makes from the sound prop
// exemplar {0x6534284a, 0xc977c536, sound id} (vslot 0x40 CreateGenericProp); the sim keeps it while it plays.
class HomeSound final : public IMySimHomeSound {
public:
    explicit HomeSound(cSC4PropOccupant* prop) : prop_(prop) {}   // takes the prop's one reference
    ~HomeSound() { if (prop_) prop_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid != kGZIID_cIGZUnknown) return false;
        *out = static_cast<cIGZUnknown*>(this);
        AddRef();
        return true;
    }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        delete this;
        return 0;
    }
    void SetPosition(const float* xyz) override { prop_->SetPosition(xyz); }        // occupant 0x24
    bool IsPlaying() const override { return prop_->IsFlagSet(1); }                  // occupant 0x80(1): in the city
    cISC4Occupant* Occupant() const { return static_cast<cISC4Occupant*>(prop_); }

private:
    cSC4PropOccupant* prop_ = nullptr;
    std::uint32_t refs_ = 0;
};
cSC4PropManager* PropManager() { return Plain<cSC4PropManager>(CityComponent::PropManager); }

cSC4Lot* LotOf(cISC4Occupant* occupant) {   // spLotManager GetOccupantLot (borrowed)
    cSC4ZoneDeveloper* lots = LotManager();
    return lots && occupant ? lots->GetOccupantLot(occupant) : nullptr;
}

void Post(std::uint32_t type, std::uint64_t d1, std::uint64_t d2, std::uint64_t d3, std::uint64_t d4, cIGZUnknown* unknown) {
    if (!SL::spMS2) return;
    auto* message = new cRZMessage2Standard();
    message->AddRef();
    message->SetType(type);
    message->SetData1(d1);
    message->SetData2(d2);
    message->SetData3(d3);
    message->SetData4(d4);
    if (unknown) message->SetIGZUnknown(unknown);
    SL::spMS2->MessagePost(message, false);
    message->Release();
}

// Every building occupant cMySimBuildingFilter(flag) accepts in the cell rectangle (inclusive, occupant manager cells), borrowed:
// the original keeps the occupant manager's list only while it walks it.
void FilteredBuildings(const int a[2], const int b[2], bool onlyKindOne, std::vector<cISC4BuildingOccupant*>& out) {
    out.clear();
    cISC4OccupantManager* manager = Occupants();
    if (!manager) return;
    cMySimBuildingFilter filter(onlyKindOne);
    filter.AddRef();
    OccupantList list;
    manager->GetOccupantsByOccupantManagerCells(list, a, b, &filter, 0xffffffffu);
    for (cISC4Occupant* occupant : list) {
        cISC4BuildingOccupant* building = nullptr;
        if (occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building)) && building) {
            out.push_back(building);
            building->Release();   // the occupant manager keeps the building alive
        }
        occupant->Release();
    }
}

// AnyTravelConnectionExists (i386 0x0047da24): the regional city at the region cell (spSC4 GetRegion -> GetCity), then the
// neighbours simulator's connections to it for each of the valid connection types until one is found.
bool AnyTravelConnectionTo(int regionX, int regionZ) {
    cISC4Region* region = SL::spSC4 ? SL::spSC4->GetRegion() : nullptr;
    cRZAutoRefCount<cISC4RegionalCity>* slot = region ? region->GetCity(regionX, regionZ) : nullptr;
    if (!slot || !slot->get()) return false;
    cISC4NeighborsSimulator* neighbors = Neighbors();
    if (!neighbors) return false;
    for (std::uint32_t type = 0; type < 13; ++type) {   // kValidNeighborConnectionTypeArray, ended by 13
        ConnectionList list;
        neighbors->GetConnectionsOfTypeToCity(type, slot->get(), list);
        if (!list.empty()) return true;
    }
    return false;
}

// The traffic simulator's path finder the dispatch keeps (spTrafficSim CreatePathFinder; the host owns the one reference and
// hands it out borrowed, the dispatch takes its own).
cISC4PathFinder* SharedPathFinder() {
    static cISC4PathFinder* finder = nullptr;
    static cSC4TrafficSimulator* finderFor = nullptr;
    cSC4TrafficSimulator* traffic = Traffic();
    if (traffic != finderFor) {
        if (finder) finder->Release();
        finder = traffic ? traffic->CreatePathFinder() : nullptr;
        finderFor = traffic;
    }
    return finder;
}

// The adapter that shows a cISC4PylonOccupant (iid 0xc9b6d7d6) through the IMySimPylon seam, slot for slot.
class PylonView final : public IMySimPylon {
public:
    explicit PylonView(cISC4PylonOccupant* pylon) : pylon_(pylon) { pylon_->AddRef(); }
    ~PylonView() { pylon_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return pylon_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { const std::uint32_t left = --refs_; if (left == 0) delete this; return left; }
    bool Init() override { return pylon_->Init(); }
    bool Shutdown() override { return pylon_->Shutdown(); }
    void SetState(int state) override { pylon_->SetUIState(state); }
    void SetBitmap(int bitmapId, const cIGZString& name) override { pylon_->SetBitmapInfo(bitmapId, name); }
    void SetFlags(int flags) override { pylon_->SetDispatchType(flags); }
    void SetKind(int kind) override { pylon_->SetDispatchState(kind); }
    void SetLinkedObject(cISC4Occupant* occupant) override { pylon_->SetSourceOccupant(occupant); }
    cISC4Occupant* GetLinkedOccupant() override { return pylon_->GetSourceOccupant(); }
    void SetLinkedAgent(cIGZUnknown* agent) override { pylon_->SetPylonOwner(agent); }
    bool IsDead() const override { return pylon_->GetDragState(); }
    cISC4Occupant* AsOccupant() override { return pylon_->AsOccupant(); }
    void SetVisibility(bool visible) override { if (cISC4Occupant* o = pylon_->AsOccupant()) o->SetVisibility(visible, true); }
    cISC4PylonOccupant* Pylon() const { return pylon_; }

private:
    cISC4PylonOccupant* pylon_;
    std::uint32_t refs_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The game data registry entries of the sims (cSC4MySim::init_balloons i386 0x0054572e) and of the agent simulator
// (cSC4MySimAgentSimulator::Init i386 0x0054d538, s_get_sim 0x0054d168, s_get_sim_count 0x0054d238).
// ---------------------------------------------------------------------------------------------------------------------
// s_sim_name_exch: read only, publishes the sim's personal name.
bool SimNameExchange(void* context, cIGZVariant& value, bool write, sc4::cISC4GameDataRegistry*) {
    if (write || !context) return false;
    cIGZString& name = static_cast<cSC4MySim*>(context)->GetPersonalName();
    value.SetValString(name);
    return true;
}
// s_get_sim(index): the agent's sim at the index (0..5) or null.
bool GetSimCommand(void* handle, cIGZCommandParameterSet* in, cIGZCommandParameterSet* out, sc4::cISC4GameDataRegistry* registry) {
    auto* agent = static_cast<cSC4MySimAgentSimulator*>(registry->GetEntryContext(registry->GetEntryID(handle)));
    cIGZUnknown* sim = nullptr;
    if (agent && in) {
        if (cIGZVariant* index = in->GetParameter(in->GetFirstParameterID())) {
            const std::int32_t i = index->GetValSint32();
            if (i >= 0 && i < kMySimAgentSlots) sim = agent->Sim(i);
        }
    }
    cRZVariant result;
    result.SetValUnknown(sim);
    if (out) out->SetParameter(1, result);
    return sim != nullptr;
}
bool GetSimCountCommand(void* handle, cIGZCommandParameterSet*, cIGZCommandParameterSet* out, sc4::cISC4GameDataRegistry* registry) {
    auto* agent = static_cast<cSC4MySimAgentSimulator*>(registry->GetEntryContext(registry->GetEntryID(handle)));
    const std::int32_t count = agent ? agent->SimCount() : 0;
    cRZVariant result;
    result.SetValSint32(count);
    if (out) out->SetParameter(1, result);
    return count > 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// IMySimHost
// ---------------------------------------------------------------------------------------------------------------------
IMySimDispatchVehicleView* NativeMySimVehicleView();   // below: the dispatch manager's view, borrowed

class MySimHost final : public IMySimHost {
public:
    // --- script state ---
    cISCLua* Lua() override {   // spCity GetAdvisorSystem (0x288) -> GetScriptingContext (0x50)
        cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
        return system ? system->GetScriptingContext() : nullptr;
    }
    int LuaUserDataTag(bool stringValue) override {   // advisor system GetDataTag (0xb0)
        cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
        return system ? system->GetDataTag(stringValue ? 1 : 0) : 0;
    }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override { return LoadLocalizedString(group, instance, out); }
    // --- city ---
    void ResidenceCandidates(std::vector<cISC4BuildingOccupant*>& out) override {
        // FindNewResidence (i386 0x0054a5f8): occupant manager 0x9c (occupant-manager cells) with no corners, i.e. the
        // whole city, through cMySimBuildingFilter(true).
        FilteredBuildings(nullptr, nullptr, true, out);
    }
    bool IsResidenceCandidate(cISC4BuildingOccupant* building) override {
        cMySimBuildingFilter filter(true);
        return building && filter.IsOccupantIncluded(building->AsOccupant());
    }
    void BuildingsNearResidence(int cellX, int cellZ, int radius, std::vector<cISC4BuildingOccupant*>& out) override {
        // UpdateLocalBuildings (i386 0x00547eea): GetOccupantsByStandardCityCells with cMySimBuildingFilter(false).
        out.clear();
        cISC4OccupantManager* manager = Occupants();
        if (!manager) return;
        const int a[2] = {cellX - radius, cellZ - radius}, b[2] = {cellX + radius, cellZ + radius};
        cMySimBuildingFilter filter(false);
        filter.AddRef();
        OccupantList list;
        manager->GetOccupantsByStandardCityCells(list, a, b, &filter, 0xffffffffu);
        for (cISC4Occupant* occupant : list) {
            cISC4BuildingOccupant* building = nullptr;
            if (occupant->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building)) && building) {
                out.push_back(building);
                building->Release();   // the occupant manager keeps the building alive
            }
            occupant->Release();
        }
    }
    cISC4Occupant* OccupantAtCell(int, int) override {
        // spSC4 GetRegion (0x118) -> GetCity (0x58): the original's "job across a neighbour connection" is the regional city
        // at the job's region cell, not an occupant; cSC4MySim::Init asks it for the building interface, which a regional
        // city does not answer, so no building comes back.
        return nullptr;
    }
    cSC4Lot* LotByBuilding(cISC4BuildingOccupant* building) override { return building ? LotOf(building->AsOccupant()) : nullptr; }
    cSC4Lot* LotAtCell(int x, int z) override { cSC4ZoneDeveloper* lots = LotManager(); return lots ? lots->GetLot(x, z, false) : nullptr; }
    cSC4MySim* MySimAtBuilding(cISC4BuildingOccupant* building) override {   // spCity GetMySimAgentSimulator -> MySimLivesHere
        cSC4MySimAgentSimulator* agent = Agent();
        cISC4MySim* sim = agent ? agent->MySimLivesHere(building) : nullptr;
        return sim ? dynamic_cast<cSC4MySim*>(sim) : nullptr;
    }
    bool AnyTravelConnection(int x, int z) override { return AnyTravelConnectionTo(x, z); }
    // cSC4MySim::CheckCommute (i386 0x005490f4): a new path finder from the traffic simulator, SetFlag(1),
    // SetTripDestinationFilter(3), SetTrafficDensity(1), the home and job rectangles, FindPath; a found path (1) gives the
    // trip length scaled for display, clamped at 0, as the commute minutes.
    bool FindCommutePath(const int homeRect[4], const int jobRect[4], int& minutes) override {
        cSC4TrafficSimulator* traffic = Traffic();
        cISC4PathFinder* finder = traffic ? traffic->CreatePathFinder() : nullptr;
        if (!finder) return false;
        finder->SetFlag(1);
        finder->SetTripDestinationFilter(3);
        finder->SetTrafficDensity(1);
        finder->SetStartRect(SC4RectInt32{homeRect[0], homeRect[1], homeRect[2], homeRect[3]});
        finder->SetEndRect(SC4RectInt32{jobRect[0], jobRect[1], jobRect[2], jobRect[3]});
        const bool found = finder->FindPath() == 1;
        if (found) {
            float length = finder->GetTripLengthScaledForDisplay();
            if (length <= 0.0f) length = 0.0f;
            minutes = static_cast<int>(static_cast<std::uint32_t>(length));
        }
        finder->Release();
        return found;
    }
    float TerrainHeight(float x, float z) override { cSTETerrain* t = Terrain(); return t ? t->GetAltitude(x, z) : 0.0f; }
    float TerrainSeaLevel() override { cSTETerrain* t = Terrain(); return t ? t->GetSeaLevel() : 0.0f; }
    void StandardCellToPosition(int x, int z, float& px, float& pz) override {
        if (cSC4City* city = HostCity()) city->CellCenterToPosition(x, z, px, pz);
    }
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    int ResidentialAgeForWealth(int wealth) override {   // residential simulator: the average age a new resident of the wealth has
        return cSC4ResidentialSimulator::GetAverageNewAgeByWealth(NativeSimulatorExemplar(kResidentialSimulatorExemplar), wealth);
    }
    // GetCongestion (i386 0x0054150a): the congestion map's value where the cell has road capacity, else 0.
    int TrafficCongestion(int x, int z) override {
        cSC4TrafficSimulator* traffic = Traffic();
        if (!traffic || traffic->GetCapacity(1, x, z) == 0.0f) return 0;
        cSC4SimGrid<std::uint8_t>* map = traffic->GetCongestionMap();
        return map ? map->GetCellValue(x, z) : 0;
    }
    int ResidentialAgeAtCell(int x, int z) override {   // residential simulator age grid (0x148) -> cell value (0x38)
        cSC4ResidentialSimulator* r = Residential();
        cSC4SimGrid<std::uint8_t>* grid = r ? r->GetAgeGrid() : nullptr;
        return grid ? grid->GetCellValue(x, z) : 0;
    }
    float ResidentialEducationQuotient(int x, int z, int wealth) override {   // slot 37
        cSC4ResidentialSimulator* r = Residential();
        return r ? r->GetCellEQByWealth(static_cast<unsigned>(x), static_cast<unsigned>(z), wealth) : 0.0f;
    }
    float ResidentialHealthQuotient(int x, int z, int wealth) override {      // slot 38
        cSC4ResidentialSimulator* r = Residential();
        return r ? r->GetCellHQByWealth(static_cast<unsigned>(x), static_cast<unsigned>(z), wealth) : 0.0f;
    }
    int HourOfDay() override { cSC424HourClock* c = Clock24(); return c ? static_cast<int>(c->GetHour()) : 12; }
    bool SimulatorTickEnabled() override { cSC4Simulator* s = Simulator(); return s && !s->IsAnyPaused(); }
    bool SimulatorAnyPaused() override { cSC4Simulator* s = Simulator(); return s && s->IsAnyPaused(); }
    IMySimDispatchVehicleView* DispatchVehicleView() override { return NativeMySimVehicleView(); }
    // spCity GetBuildingDevelopmentSimulator -> GetSpecialBuildings: the civic occupants, borrowed.
    void SpecialBuildings(std::vector<cISC4Occupant*>& out) override {
        out.clear();
        cSC4BuildingDevelopmentSimulator* development = NativeBuildingDevelopmentSimulator();
        if (!development) return;
        for (cIGZUnknown* object : development->GetSpecialBuildings()) {
            cISC4Occupant* occupant = nullptr;
            if (object && object->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occupant)) && occupant) {
                out.push_back(occupant);
                occupant->Release();   // the simulator keeps the occupant alive
            }
        }
    }
    // UpdateLocalBuildings: spBuildingDevelopmentSim GetBuildingKeyFromType, then cSC4Occupant::GetOccupantGroups(key).
    void BuildingOccupantGroups(std::uint32_t buildingType, std::vector<std::uint32_t>& out) override {
        out.clear();
        cSC4BuildingDevelopmentSimulator* development = NativeBuildingDevelopmentSimulator();
        cGZPersistResourceKey key;
        if (development) development->GetBuildingKeyFromType(buildingType, key);
        cSC4Occupant::GetOccupantGroups(key, out);
    }
    // GetSchoolBuildings / GetHospitalBuildings(list, null): the residential simulator AddRefs each, the host hands them
    // out borrowed.
    void ResidentialServiceBuildings(bool hospital, std::vector<cISC4Occupant*>& out) override {
        out.clear();
        cSC4ResidentialSimulator* r = Residential();
        if (!r) return;
        cSC4ResidentialSimulator::OccupantList list;
        if (hospital) r->GetHospitalBuildings(list, nullptr);
        else r->GetSchoolBuildings(list, nullptr);
        for (IResidentialOccupant* entry : list) {
            cIGZUnknown* object = entry ? entry->Object() : nullptr;
            cISC4Occupant* occupant = nullptr;
            if (object && object->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occupant)) && occupant) {
                out.push_back(occupant);
                occupant->Release();
            }
            if (entry) entry->Release();   // the reference the simulator added
        }
    }
    bool ResidentialServiceGrade(bool hospital, cISC4Occupant* building, float& grade) override {
        cSC4ResidentialSimulator* r = Residential();
        IResidentialOccupant* occupant = r && building ? NativeResidentialOccupant(building) : nullptr;
        ResidentialQueryData data;
        const bool ok = occupant && (hospital ? r->GetHospitalQueryData(occupant, data) : r->GetSchoolQueryData(occupant, data));
        grade = data.grade;
        return ok;
    }
    int ResidentialLifeExpectancy(int x, int z) override {   // GetCellLifeExpectancy
        cSC4ResidentialSimulator* r = Residential();
        return r ? r->GetCellLifeExpectancy(static_cast<unsigned>(x), static_cast<unsigned>(z)) : 0;
    }
    std::uint8_t BuildingCondition(cISC4BuildingOccupant* building) override {   // GetBuildingCondition (i386 0x005422a6)
        cSC4Lot* lot = building ? LotOf(building->AsOccupant()) : nullptr;
        return lot ? lot->GetCondition() : 0;
    }
    // cSC4PowerSimulator::IsOccupantPowered (i386 0x0018087a): the occupant's lot; powered when it has power or does not
    // need it; no lot is not powered.
    bool OccupantPowered(cISC4Occupant* occupant) override {
        cSC4Lot* lot = LotOf(occupant);
        if (!lot) return false;
        return lot->GetHasPower() || !lot->GetNeedsPower();
    }
    bool BuildingWatered(cISC4Occupant* occupant) override {   // cSC4PlumbingSimulator::IsBuildingWatered
        cSC4PlumbingSimulator* plumbing = NativePlumbingSimulator();
        return plumbing && occupant && plumbing->IsBuildingWatered(occupant);
    }
    int CrimeAtCell(int x, int z) override {
        cSC4CrimeSimulator* crime = NativeCrimeSimulator();
        const cSC4SimGrid<std::uint8_t>* grid = crime ? crime->GetCrimeGrid() : nullptr;
        return grid ? grid->GetCellValue(x, z) : 0;
    }
    // --- UpdateTripVariables: the surroundings of a trip ---
    bool TripSimulatorsPresent() override {
        return Traffic() && Comp<cSC4PollutionSimulator>(CityComponent::PollutionSimulator) && NativeCrimeSimulator();
    }
    // The grid's tract at the world position (PositionToTract, then GetTractValue). A position outside the grid answers 0.
    template <class T> static double TractValue(const cSC4SimGrid<T>* grid, float x, float z) {
        if (!grid) return 0.0;
        std::int32_t tx = 0, tz = 0;
        grid->PositionToTract(x, z, tx, tz);
        return grid->TractIsInBounds(tx, tz) ? static_cast<double>(grid->GetTractValue(tx, tz)) : 0.0;
    }
    double TripGridValue(TripGrid which, float x, float z) override {
        switch (which) {
        case TripGrid::Congestion: {
            cSC4TrafficSimulator* traffic = Traffic();
            return TractValue(traffic ? traffic->GetCongestionMap() : nullptr, x, z);
        }
        case TripGrid::PollutionAir:
        case TripGrid::PollutionWater:
        case TripGrid::PollutionGarbage: {
            auto* pollution = Comp<cSC4PollutionSimulator>(CityComponent::PollutionSimulator);
            const int type = static_cast<int>(which) - static_cast<int>(TripGrid::PollutionAir);
            return TractValue(pollution ? pollution->GetPollutionGrid(static_cast<PollutionType>(type)) : nullptr, x, z);
        }
        case TripGrid::Crime: {
            cSC4CrimeSimulator* crime = NativeCrimeSimulator();
            return TractValue(crime ? crime->GetCrimeGrid() : nullptr, x, z);
        }
        case TripGrid::MayorRating: {
            auto* aura = Comp<cSC4AuraSimulator>(CityComponent::AuraSimulator);
            return TractValue(aura ? aura->GetAuraGrid() : nullptr, x, z);
        }
        case TripGrid::EducationQuotient:
        case TripGrid::HealthQuotient: {
            cSC4ResidentialSimulator* r = Residential();
            if (!r) return 0.0;
            cSC4SimGrid<float>* grid = which == TripGrid::EducationQuotient ? r->GetAverageEQGrid(nullptr, nullptr)
                                                                            : r->GetAverageHQGrid(nullptr, nullptr);
            const double value = TractValue(grid, x, z);
            if (grid) grid->Release();   // the new grid's one reference
            return value;
        }
        }
        return 0.0;
    }
    float TravelTimeRatio(int cellX, int cellZ, int travelType) override {
        cSC4TrafficSimulator* traffic = Traffic();
        return traffic ? traffic->GetTravelTimeRatio(cellX, cellZ, travelType) : 1.0f;
    }
    void LotsInCellRect(int x0, int z0, int x1, int z1, std::vector<TripLot>& out) override {
        out.clear();
        cSC4ZoneDeveloper* lots = LotManager();
        if (!lots) return;
        std::list<cRZAutoRefCount<cSC4Lot>> list;
        lots->GetLotsInCellRect(list, x0, z0, x1, z1, false);
        for (const cRZAutoRefCount<cSC4Lot>& lot : list) {
            if (!lot) continue;
            TripLot entry{nullptr, lot->GetCondition(), lot->GetOccupantWealth()};
            if (cIGZUnknown* object = lot->GetBuilding()) {   // borrowed
                cISC4BuildingOccupant* building = nullptr;
                if (object->QueryInterface(kGZIID_cISC4BuildingOccupant, reinterpret_cast<void**>(&building)) && building) {
                    entry.building = building;
                    building->Release();   // the lot keeps the building alive
                }
            }
            out.push_back(entry);
        }
    }
    float BuildingLocalFunding(cISC4BuildingOccupant* building, std::uint32_t purpose) override {
        auto* budget = Comp<cSC4BudgetSimulator>(CityComponent::BudgetSimulator);
        if (!budget || !building) return 0.0f;
        BudgetItem item;
        budget->GetBudgetItemForPurpose(building->AsOccupant()->AsPropertyHolder(), purpose, item);
        return item.localFunding;
    }
    void DisasterLocations(std::vector<TripDisaster>& out) override {
        out.clear();
        cISC4DisasterLayer* layer = As<cISC4DisasterLayer>(CityComponent::DisasterLayer);
        if (!layer) layer = Comp<cISC4DisasterLayer>(CityComponent::DisasterLayer);
        if (!layer) return;
        const int count = layer->GetDisasterLocationCount();
        for (int i = 0; i < count; ++i) {
            cISC4DisasterInstance* instance = nullptr;
            int x = 0, z = 0;
            if (!layer->GetDisasterLocationByIndex(i, &instance, &x, &z) || !instance) continue;
            out.push_back({instance->GetID(), x, z});
            instance->Release();
        }
    }
    // GetPolicePowerGrid / GetFireProtectionGrid: the protection simulator's adjusted grid, averaged over the cell rect.
    int ProtectionCoverage(bool police, int x, int z, int width, int height) override {
        const cSC4SimGrid<std::uint8_t>* grid = nullptr;
        if (police) {
            if (auto* p = Comp<cSC4PoliceSimulator>(CityComponent::PoliceSimulator)) grid = p->Protection().AdjustedGrid();
        } else if (auto* f = Comp<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator)) {
            grid = f->GetFireProtectionGrid();
        }
        return grid ? AverageInCellRectU8(*grid, x, z, width, height) : 0;
    }
    int PollutionAtCell(int type, int x, int z) override {
        auto* pollution = Comp<cSC4PollutionSimulator>(CityComponent::PollutionSimulator);
        cSC4SimGrid<std::int16_t>* grid = pollution ? pollution->GetPollutionGrid(static_cast<PollutionType>(type)) : nullptr;
        return grid ? grid->GetCellValue(x, z) : 0;
    }
    bool DesirabilityAtCell(std::uint32_t developerType, int x, int z, int& value) override {
        cSC4SimGrid<std::int16_t>* map = GrowthDeveloperDesirabilityMap(developerType);
        if (!map) return false;
        value = map->GetCellValue(x, z);
        return true;
    }
    void ProtectionStations(bool police, std::vector<Station>& out) override {
        out.clear();
        auto add = [&out](IProtectionStation* station, bool funded, const SC4Percentage& fraction) {
            Station s{};
            station->Position(s.position);
            s.funded = funded;
            s.funding = fraction.value;
            out.push_back(s);
        };
        if (police) {
            auto* p = Comp<cSC4PoliceSimulator>(CityComponent::PoliceSimulator);
            if (!p) return;
            std::list<IProtectionStation*> stations;
            p->GetPoliceStations(stations);
            for (IProtectionStation* station : stations) {
                SC4Percentage fraction;
                const bool funded = station && p->GetStationFunding(static_cast<IPoliceOccupant*>(station), nullptr, &fraction);
                if (station) add(station, funded, fraction);
            }
        } else {
            auto* f = Comp<cSC4FireProtectionSimulator>(CityComponent::FireProtectionSimulator);
            if (!f) return;
            std::list<IFireOccupant*> stations;
            f->GetFireStations(stations);
            for (IFireOccupant* station : stations) {
                SC4Percentage fraction;
                const bool funded = station && f->GetStationFunding(station, nullptr, &fraction);
                if (station) add(station, funded, fraction);
            }
        }
    }
    bool NeighborJobCityName(cIGZUnknown* city, cIGZString& out) override {   // the regional city's GetCityName
        void* raw = nullptr;
        if (!city || !city->QueryInterface(kGZIID_cISC4RegionalCity, &raw) || !raw) return false;
        auto* unknown = static_cast<cIGZUnknown*>(raw);
        auto* regional = dynamic_cast<cSC4RegionalCity*>(unknown);
        if (regional) regional->GetCityName(out);
        unknown->Release();
        return regional != nullptr;
    }
    // The job building's name ellipsized to the width in the system font 0x68963c4c (RZ::WCM() SystemFont, its line height,
    // DefaultText(0x5377be31) as the ellipsis, FormatForEllipsis(text, ellipsis, {0, 0, width, height}, false)).
    void FitTextToWidth(cIGZString& text, int width) override {
        gz::cIGZWinCtrlMgr* controls = gz::WinCtrlMgr();
        gz::IWinFont* font = controls ? controls->SystemFont(0x68963c4c) : nullptr;
        if (!font) return;
        const cRZRect area{0, 0, width, font->GlyphHeight()};
        cRZString ellipsis;
        if (const cIGZString* e = controls->DefaultText(0x5377be31)) ellipsis.Copy(*e);
        cRZString fitted(text);
        font->FormatForEllipsis(fitted, ellipsis, area, false);
        text.Copy(fitted);
    }
    bool GamePaused() override { cSC4Simulator* s = Simulator(); return s && s->IsAnyPaused(); }
    // --- UI / effects ---
    bool CameraLookAt(const float pos[3], bool zoomIfFar) override { (void)pos; (void)zoomIfFar; SC4_UNWIRED("IMySimHost::CameraLookAt (MySim Find)"); return false; }
    void NotificationDialog(const cIGZString& title, const cIGZString& body) override {
        std::fprintf(stderr, "mysim: notification \"%s\": %s\n", title.ToChar(), body.ToChar());
    }
    bool CityName(cIGZString& out) override { cSC4City* c = HostCity(); if (!c) return false; c->GetCityName(out); return true; }
    IMySimCallout* CreateCallout() override { SC4_UNWIRED("IMySimHost::CreateCallout (cSC4WinCalloutBox over the MySim residence)"); return nullptr; }
    bool LoadMySimBitmap(std::uint32_t, const cIGZString*, cIGZBuffer**) override { SC4_UNWIRED("IMySimHost::LoadMySimBitmap (MySim portraits)"); return false; }
    bool CalloutImageAvailable() override { return false; }   // the callout is not native (CreateCallout)
    void ProjectPointToScreen(float xyz[3]) override { float out[3]; if (NativeProjectToViewport(xyz, out)) { xyz[0] = out[0]; xyz[1] = out[1]; xyz[2] = out[2]; } }
    void ReportSimEvent(const cIGZString& text, const float*) override { std::fprintf(stderr, "mysim: event %s\n", text.ToChar()); }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    void PostMessage2(std::uint32_t type, std::uint32_t d1, std::uint32_t d2, std::uint32_t d3, std::uint32_t d4, cIGZUnknown* unknown) override {
        Post(type, d1, d2, d3, d4, unknown);
    }
    bool LoadExemplar(const cGZPersistResourceKey& key, std::uint32_t iid, cIGZUnknown** out) override {
        *out = nullptr;
        return SL::spRM && SL::spRM->GetResource(key, iid, reinterpret_cast<void**>(out), 0, nullptr) && *out;
    }
    // GetRandomWalkModelKey (i386 0x00542410): a walking pedestrian (group 0x4149, flags 2) made by spPedestrianManager at
    // (0, -100, 0) with no model key and no listener, its model resource key, then the pedestrian is removed at once.
    bool PickRandomWalkModelKey(cGZPersistResourceKey& key) override {
        key = cGZPersistResourceKey();
        cSC4AutomataManager* pedestrians = NativeAutomataManagerAt(1);
        if (!pedestrians) return false;
        IAutomataAutomaton* made = pedestrians->CreateAutomaton(0x4149, cS3DVector3{0.0f, -100.0f, 0.0f}, 2, cGZPersistResourceKey(),
                                                                nullptr, 0xff, nullptr);
        cSC4Automaton* automaton = made ? native::AutomatonOf(made) : nullptr;
        if (!automaton) return false;
        automaton->AddRef();
        key = automaton->GetModelResourceKey(0);
        automaton->Remove(true);
        automaton->Release();
        return true;
    }
    IMySimHomeSound* CreateHomeSound(std::uint32_t soundId) override {   // spPropManager 0x40 CreateGenericProp
        cSC4PropManager* props = PropManager();
        if (!props) return nullptr;
        cSC4PropOccupant* prop = nullptr;
        if (!props->CreateGenericProp(cGZPersistResourceKey(0x6534284au, 0xc977c536u, soundId), prop) || !prop) return nullptr;
        auto* sound = new HomeSound(prop);
        sound->AddRef();
        return sound;
    }
    void DestroyHomeSound(IMySimHomeSound* sound) override {   // spPropManager 0x74 RemovePropA(the prop)
        cSC4PropManager* props = PropManager();
        if (props && sound) props->RemovePropA(static_cast<HomeSound*>(sound)->Occupant());
    }
    void PlayCitySound(IMySimHomeSound* sound, const float*) override {   // spCity 0x88 GetOccupantManager -> 0x50 InsertOccupant
        cISC4OccupantManager* occupants = Occupants();
        if (occupants && sound) occupants->InsertOccupant(static_cast<HomeSound*>(sound)->Occupant(), 0);
    }
    IMySimEffect* CreateGhostEffect() override { SC4_UNWIRED("IMySimHost::CreateGhostEffect (the MySimGhost effect)"); return nullptr; }
    bool DispatchIconRect(int, int*) override { SC4_UNWIRED("IMySimHost::DispatchIconRect (dispatch view icon of a tracked MySim)"); return false; }
    // --- objects ---
    IMySimDispatch* CreateDispatch(cSC4MySim* owner) override {   // cSC4MySimDispatch ctor (i386 0x0047...; owner = the sim)
        auto* dispatch = new cSC4MySimDispatch(static_cast<IMySimDispatchOwner*>(owner));
        dispatch->AddRef();
        return dispatch;
    }
    IMySimBalloonWinProc* CreateBalloonWinProc(cISC4MySim* owner) override {   // cSC4MySimBalloonWinProc (i386 0x0047ceb8)
        auto* proc = new cSC4MySimBalloonWinProc(owner);
        proc->AddRef();
        return proc;
    }
    IMySimPylon* CreatePylon() override { SC4_UNWIRED("IMySimHost::CreatePylon (MySim dispatch tool call box)"); return nullptr; }
    // --- view input controls ---
    // cSC4MySim::mspViewInputControl (the MySim tool the sims last put up) and the city view's control stack (spView3D vslots
    // 0x60 current, 0x64 SetCurrentViewInputControl(control, 2), 0x68 RemoveCurrentViewInputControl(false)).
    cIGZUnknown* toolControl_ = nullptr;   // not counted: the view's stack holds the tool
    cIGZUnknown* CurrentViewInputControl() override { return toolControl_; }
    void SetCurrentViewInputControl(cIGZUnknown* control) override { toolControl_ = control; }
    cIGZUnknown* ViewCurrentInputControl() override { return static_cast<cIGZUnknown*>(NativeViewCurrentControl()); }
    void ViewSetInputControl(cIGZUnknown* control) override {
        if (!control) NativeViewRemoveControl();
        else if (auto* tool = dynamic_cast<sc4::cSC4ViewInputControl*>(control)) NativeViewSetControl(tool, 0);
    }
    void ViewPushInputControl(cIGZUnknown* control) override {
        if (auto* tool = control ? dynamic_cast<sc4::cSC4ViewInputControl*>(control) : nullptr) NativeViewSetControl(tool, 2);
    }
    // A new tool with no reference of its own: the view's stack takes one when the MySim pushes it.
    template <class Tool> static cIGZUnknown* MakeTool(cSC4MySim* sim) {
        auto* tool = new Tool();
        if (!tool->Init(sim)) {
            tool->AddRef();
            tool->Release();
            return nullptr;
        }
        return static_cast<cIGZUnknown*>(tool);
    }
    cIGZUnknown* CreateMoveInTool(cSC4MySim* sim) override { return MakeTool<sc4::cSC4MySimInputControlMoveIn>(sim); }
    cIGZUnknown* CreateDispatchToolControl(cSC4MySim*) override { SC4_UNWIRED("IMySimHost::CreateDispatchToolControl (cSC4MySimInputControlDispatch)"); return nullptr; }
    cIGZUnknown* CreateChangeHouseTool(cSC4MySim* sim) override { return MakeTool<sc4::cSC4MySimInputControlChangeHouse>(sim); }
    cIGZUnknown* CreateChangeJobTool(cSC4MySim* sim) override { return MakeTool<sc4::cSC4MySimInputControlChangeJob>(sim); }
    // --- message plumbing ---
    void RegisterInputControlTarget(cIGZMessageTarget2* target) override { if (cIGZMessageServer2* atm = AnimationTickServerView()) atm->AddNotification(target, 0xff); }
    void UnregisterInputControlTarget(cIGZMessageTarget2* target) override { if (cIGZMessageServer2* atm = AnimationTickServerView()) atm->RemoveNotification(target, 0xff); }
    bool AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { return SL::spMS2 && SL::spMS2->AddNotification(target, type); }
    bool RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { return SL::spMS2 && SL::spMS2->RemoveNotification(target, type); }
    // --- game data registry (init_balloons): the "mysims" node of the agent simulator (added when missing), the sim's node
    // under it and its "age" (uint8, writable) and "name" (read only) values ---
    void AddToGameDataRegistry(cSC4MySim& sim) override {
        sc4::cISC4GameDataRegistry* registry = NativeGameDataRegistry();
        cSC4MySimAgentSimulator* agent = Agent();
        if (!registry || !agent) return;
        cIGZUnknown* agentHandle = nullptr;
        agent->QueryInterface(1, reinterpret_cast<void**>(&agentHandle));
        const EntryId agentEntry = registry->GetEntryID(agentHandle);
        if (agentHandle) agentHandle->Release();
        if (agentEntry == 0) {
            nsISC4GameDataRegistry::AddNode<cSC4MySimAgentSimulator, cIGZUnknown>(agent, "mysims", nullptr, 0, nullptr, registry);
            RegisterAgentValues(*agent, registry);
        }
        nsISC4GameDataRegistry::AddNode<cSC4MySim, cSC4MySimAgentSimulator>(&sim, "", agent, 0, nullptr, registry);
        nsISC4GameDataRegistry::AddValue<cSC4MySim>(sim.AgeField(), "age", nsISC4GameDataRegistry::GenericValueExchnageFuncUint8, &sim, 0, nullptr, registry);
        nsISC4GameDataRegistry::AddValue<cSC4MySim>(sim.NameHandle(), "name", &SimNameExchange, &sim, 2, &sim, registry);
    }
    void RemoveFromGameDataRegistry() override {}   // registry vslot 0x30(0): RemoveChildEntry(null) removes nothing
    // --- occupant serialization ---
    void WriteOccupant(cIGZOStream& stream, cISC4BuildingOccupant* occupant) override { SC4WriteGZSerializable(stream, occupant); }
    bool ReadOccupant(cIGZIStream& stream, cISC4BuildingOccupant** out) override {
        void* object = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, kGZIID_cISC4BuildingOccupant, &object, false);
        *out = static_cast<cISC4BuildingOccupant*>(object);
        return ok;
    }
    bool ReadPylon(cIGZIStream& stream, IMySimPylon** out) override {
        void* object = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, kGZIID_cISC4PylonOccupant, &object, false);
        *out = nullptr;
        if (object) {
            auto* pylon = static_cast<cISC4PylonOccupant*>(object);
            *out = new PylonView(pylon);
            (*out)->AddRef();
            pylon->Release();
        }
        return ok;
    }
    void WritePylon(cIGZOStream& stream, IMySimPylon* pylon) override {
        auto* view = dynamic_cast<PylonView*>(pylon);
        SC4WriteGZSerializable(stream, view ? static_cast<cIGZUnknown*>(view->Pylon()) : nullptr);
    }
    void WriteDispatch(cIGZOStream& stream, IMySimDispatch* dispatch) override { SC4WriteGZSerializable(stream, dispatch); }
    bool ReadDispatch(cIGZIStream& stream, IMySimDispatch** out) override {
        void* object = nullptr;
        const bool ok = SC4ReadGZSerializable(stream, kGZIID_cISC4MySimDispatch, &object, false);
        *out = static_cast<IMySimDispatch*>(object);
        return ok;
    }
    // GZCOM CreateInstance(0x6a26d049 cSC4AdviceSubject, 0x6a26d050): a new subject with one reference; the caller sets its object.
    cISC4AdviceSubject* CreateLuaSubject(cISC4Occupant*, bool) override {
        auto* subject = new cSC4AdviceSubject();
        cISC4AdviceSubject* out = nullptr;
        subject->AddRef();
        if (!subject->QueryInterface(kGZIID_cISC4AdviceSubject, reinterpret_cast<void**>(&out))) out = nullptr;
        subject->Release();
        return out;
    }

    static void RegisterAgentValues(cSC4MySimAgentSimulator& agent, sc4::cISC4GameDataRegistry* registry) {
        nsISC4GameDataRegistry::AddValue<cSC4MySimAgentSimulator>(reinterpret_cast<void*>(&GetSimCommand), "get_sim", &GetSimCommand, &agent, 0, &agent, registry);
        nsISC4GameDataRegistry::AddValue<cSC4MySimAgentSimulator>(reinterpret_cast<void*>(&GetSimCountCommand), "get_count", &GetSimCountCommand, &agent, 0, &agent, registry);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// The trip automata: the dispatch's views of a cSC4Automaton (the cISC4Automaton slots it calls), of its traffic route
// (cISC4TrafficAutomaton 0x4b8 / 0x4c0) and of the vehicle and pedestrian managers. In the original they are the same
// objects; here each view holds one reference on the automaton and forwards to its method.
// ---------------------------------------------------------------------------------------------------------------------
class RouteView final : public IMySimDispatchRoute {
public:
    explicit RouteView(cSC4TrafficAutomaton* automaton) : automaton_(automaton) { automaton_->AddRef(); }
    ~RouteView() { automaton_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return automaton_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { const std::uint32_t left = --refs_; if (left == 0) delete this; return left; }
    bool GetRouteInfo(std::deque<RouteData>& out) const override { out = automaton_->GetRouteInfo(); return true; }
    bool SetRouteInfo(const std::deque<RouteData>& route, bool begin, bool onlyStart) override {
        return automaton_->SetRouteInfo(route, begin, onlyStart);
    }
    void SetCellLifespan(std::uint8_t lifespan) override { automaton_->SetCellLifespan(lifespan); }
private:
    cSC4TrafficAutomaton* automaton_;
    std::uint32_t refs_ = 0;
};

class DispatchAutomatonView final : public IMySimDispatchAutomaton {
public:
    explicit DispatchAutomatonView(cSC4Automaton* automaton) : automaton_(automaton) { automaton_->AddRef(); }
    ~DispatchAutomatonView() { automaton_->Release(); }
    bool QueryInterface(std::uint32_t iid, void** out) override { return automaton_->QueryInterface(iid, out); }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override { const std::uint32_t left = --refs_; if (left == 0) delete this; return left; }
    cISC4Occupant* AsOccupant() override { return automaton_->AsOccupant(); }
    void Remove(bool fade) override { automaton_->Remove(fade); }
    bool PlayAnim(std::uint32_t animId) override { return automaton_->PlayAnim(animId); }
    void SetAutomataFlag(std::uint32_t flag, bool on) override { automaton_->SetAutomataFlag(flag, on); }
    bool CheckAutomataFlag(std::uint32_t flag) override { return automaton_->CheckAutomataFlag(flag); }
    void AddCallback(cIGZMessageTarget2* target) override { automaton_->AddCallback(target); }
    void RemoveCallback(cIGZMessageTarget2* target) override { automaton_->RemoveCallback(target); }
    // 0x1c0 GetState -> +8: the state's behavior type (an automaton always has its base state; -1 without one)
    std::uint32_t GetBehavior() const override { return automaton_->GetBehavior(); }
    int GetStateId() const override {
        tAutomataState* state = automaton_->GetState();
        return state ? static_cast<int>(state->stateType) : -1;
    }
    // 0x198 SetState: the dispatch never passes a controller (i386 0x004807aa passes null).
    bool SetState(std::uint32_t state, IMySimDispatchAutomaton*, float timer, bool flag, std::uint32_t animOffset,
                  std::uint32_t sfx, const void* behavior) override {
        return automaton_->SetState(state, nullptr, timer, flag, animOffset, sfx, static_cast<const tBehaviorTemplate*>(behavior));
    }
    void SetPriority(std::uint8_t priority) override { automaton_->SetPriority(priority); }
    IMySimDispatchRoute* RouteInterface() override {   // QueryInterface(0x8b99ee7c): only a traffic automaton answers
        auto* traffic = dynamic_cast<cSC4TrafficAutomaton*>(automaton_);
        if (!traffic) return nullptr;
        auto* route = new RouteView(traffic);
        route->AddRef();
        return route;
    }
private:
    cSC4Automaton* automaton_;
    std::uint32_t refs_ = 0;
};

IMySimDispatchAutomaton* ViewOf(cSC4Automaton* automaton) {   // one reference
    if (!automaton) return nullptr;
    auto* view = new DispatchAutomatonView(automaton);
    view->AddRef();
    return view;
}
cSC4Automaton* AutomatonBehind(cISC4Occupant* occupant) {   // QueryInterface(0xa9b40f05), borrowed
    void* raw = nullptr;
    if (!occupant || !occupant->QueryInterface(kAutomatonIid, &raw) || !raw) return nullptr;
    auto* automaton = static_cast<cSC4Automaton*>(raw);
    automaton->Release();   // the occupant itself keeps the automaton
    return automaton;
}

// spVehicleManager (index 0) / spPedestrianManager (index 1) as the dispatch uses them.
class AutomataManagerView final : public IMySimDispatchAutomataManager {
public:
    explicit AutomataManagerView(int index) : index_(index) {}
    // 0x78 CreateAutomaton(group, (x, 0, z), flags, key, target, 0xff, no velocity) (i386 0x00480624); the manager keeps the
    // automaton, the view handed out holds one reference of its own.
    IMySimDispatchAutomaton* CreateAutomaton(int group, const float position[2], std::uint32_t flags, const cGZPersistResourceKey& key,
                                            cIGZMessageTarget2* target, std::uint8_t priority, int) override {
        cSC4AutomataManager* manager = NativeAutomataManagerAt(index_);
        if (!manager) return nullptr;
        const cS3DVector3 where{position[0], 0.0f, position[1]};
        IAutomataAutomaton* made = manager->CreateAutomaton(static_cast<std::uint32_t>(group), where, flags, key, target, priority, nullptr);
        return ViewOf(made ? native::AutomatonOf(made) : nullptr);
    }
    bool PlayerDriveActive() override { cSC4AutomataManager* m = NativeAutomataManagerAt(index_); return m && m->IsCameraFollowUpdating(); }   // 0x118
    bool DriveViewActive() override { cSC4AutomataManager* m = NativeAutomataManagerAt(index_); return m && m->IsDrivingMode(); }             // 0x68
    void SetPlayerDriveAutomaton(IMySimDispatchAutomaton* automaton) override {   // 0x110 SetCameraFollow
        cSC4AutomataManager* m = NativeAutomataManagerAt(index_);
        if (!m) return;
        cSC4Automaton* behind = automaton ? AutomatonBehind(automaton->AsOccupant()) : nullptr;
        m->SetCameraFollow(behind ? native::AutomatonView(behind) : nullptr);
    }
private:
    int index_;
};

// SL::spDispatchManager GetDispatchVehicleView (0xb0) as the dispatch tracks the sim's automaton.
class VehicleViewForward final : public IMySimDispatchVehicleView {
public:
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return 1; }
    std::uint32_t Release() override { return 1; }
    int TrackOccupant(cISC4Occupant* occupant, int trackerType, std::uint32_t callNumber, const cRZString* label) override {
        IDispatchVehicleView* view = View();
        return view ? view->TrackOccupant(occupant, trackerType, callNumber, label) : -1;
    }
    void UntrackOccupant(int trackId, bool force) override { if (IDispatchVehicleView* view = View()) view->UntrackOccupant(trackId, force); }
    static IDispatchVehicleView* View() {
        auto* manager = As<cSC4DispatchManager>(CityComponent::DispatchManager);
        return manager ? manager->GetDispatchVehicleView() : nullptr;
    }
};

IMySimDispatchVehicleView* NativeMySimVehicleView() {
    static VehicleViewForward view;
    return VehicleViewForward::View() ? &view : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// IMySimDispatchHost
// ---------------------------------------------------------------------------------------------------------------------
class DispatchHost final : public IMySimDispatchHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    void RegisterInputControlTarget(cIGZMessageTarget2* target) override { if (cIGZMessageServer2* atm = AnimationTickServerView()) atm->AddNotification(target, 0xff); }
    void UnregisterInputControlTarget(cIGZMessageTarget2* target) override { if (cIGZMessageServer2* atm = AnimationTickServerView()) atm->RemoveNotification(target, 0xff); }
    cISC4PathFinder* PathFinder() override { return SharedPathFinder(); }
    int FindPath(cISC4PathFinder* pathFinder) override { return pathFinder ? pathFinder->FindPath() : 0; }
    cIGZUnknown* Clock() override { return Component(CityComponent::Clock24Hour); }
    IMySimDispatchVehicleView* DispatchVehicleView() override {   // spDispatchManager GetDispatchVehicleView
        static VehicleViewForward view;
        return VehicleViewForward::View() ? &view : nullptr;
    }
    // pickAutomataManager: the dispatch has chosen the vehicle or the pedestrian manager by the group; the pedestrian groups
    // (0x4149 / 0x414a / 0x4107) go to spPedestrianManager, the rest to spVehicleManager.
    IMySimDispatchAutomataManager* AutomataManager(std::uint32_t group) override {
        static AutomataManagerView vehicles(0), pedestrians(1);
        const bool pedestrian = group == kAutomataGroupFootMale || group == kAutomataGroupFootFemale || group == kAutomataGroupTransit;
        AutomataManagerView& view = pedestrian ? pedestrians : vehicles;
        return NativeAutomataManagerAt(pedestrian ? 1 : 0) ? &view : nullptr;
    }
    int CityCellsX() override { cSC4City* c = HostCity(); return c ? c->CellCountX() : 0; }
    int CityCellsZ() override { cSC4City* c = HostCity(); return c ? c->CellCountZ() : 0; }
    bool AnyTravelConnection(int cellX, int cellZ) override { return AnyTravelConnectionTo(cellX, cellZ); }
    // updateNeighborLocation (i386 0x0047ddc8) walks a connection list it never fills, so it never finds a connection.
    bool NeighborConnection(int, int, int&, int&) override { return false; }
    cISC4Occupant* BuildingAtCell(int cellX, int cellZ) override {   // spLotManager GetLot(x, z, 0) -> its building -> AsOccupant
        cSC4ZoneDeveloper* lots = LotManager();
        cSC4Lot* lot = lots ? lots->GetLot(cellX, cellZ, false) : nullptr;
        cIGZUnknown* building = lot ? lot->GetBuilding() : nullptr;
        cISC4Occupant* occupant = nullptr;
        if (building && building->QueryInterface(kGZIID_cISC4Occupant, reinterpret_cast<void**>(&occupant)) && occupant) occupant->Release();
        return occupant;
    }
    // getLocationCellRect (i386 0x0047d648): the lot at the cell (GetLot(x, z, 0)) or of the occupant (GetOccupantLot), its rect.
    bool LotCellRect(int cellX, int cellZ, int outRect[4]) override {
        cSC4ZoneDeveloper* lots = LotManager();
        cSC4Lot* lot = lots ? lots->GetLot(cellX, cellZ, false) : nullptr;
        if (!lot) return false;
        lot->GetBoundingRect(outRect);
        return true;
    }
    bool LotCellRectForOccupant(cISC4Occupant* occupant, int outRect[4]) override {
        cSC4Lot* lot = LotOf(occupant);
        if (!lot) return false;
        lot->GetBoundingRect(outRect);
        return true;
    }
    // spTrafficNetworkMap FindNetworkOccupant(x, z, mask, false) (i386 0x28; snapToNearestRoadTile 0x0047df78)
    bool TileHasNetwork(int cellX, int cellZ, std::uint32_t mask) override {
        auto* map = As<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
        return map && map->FindNetworkOccupant(cellX, cellZ, mask, false) != nullptr;
    }
    // spTrafficNetworkMap GetSurroundingRoads(rect, 1, 1, 0x849) (i386 0x68), each road's occupied cell; the list's
    // references are released.
    bool NetworkTilesInRect(const int rect[4], std::deque<std::pair<int, int>>& tiles) override {
        auto* map = As<cSC4TrafficNetworkMap>(CityComponent::TrafficNetwork);
        if (!map) return false;
        std::vector<ITrafficNetworkMapOccupant*> roads;
        const bool found = map->GetSurroundingRoads(roads, SC4RectInt32{rect[0], rect[1], rect[2], rect[3]}, 1, 1, 0x849);
        for (ITrafficNetworkMapOccupant* road : roads) {
            if (!road) continue;
            if (found) {
                std::uint32_t x = 0, z = 0;
                road->GetOccupiedCell(x, z);
                tiles.emplace_back(static_cast<int>(x), static_cast<int>(z));
            }
            road->Release();
        }
        return found;
    }
    void OccupantsInCellRect(const int rect[4], std::deque<cISC4Occupant*>& out) override {
        cISC4OccupantManager* manager = Occupants();
        if (!manager) return;
        const int a[2] = {rect[0], rect[1]}, b[2] = {rect[2], rect[3]};
        OccupantList list;
        manager->GetOccupantsByStandardCityCells(list, a, b, 0u, 0xffffffffu);
        for (cISC4Occupant* occupant : list) out.push_back(occupant);   // one reference each, ClearOccupantList returns them
    }
    void ClearOccupantList(std::deque<cISC4Occupant*>& list) override {
        for (cISC4Occupant* occupant : list) if (occupant) occupant->Release();
        list.clear();
    }
    IMySimDispatchAutomaton* AutomatonOf(cISC4Occupant* occupant) override { return ViewOf(AutomatonBehind(occupant)); }
    IMySimDispatchRoute* RouteOf(cISC4Occupant* occupant) override {
        IMySimDispatchAutomaton* view = AutomatonOf(occupant);
        IMySimDispatchRoute* route = view ? view->RouteInterface() : nullptr;
        if (view) view->Release();
        return route;
    }
    // The automaton event message names the cSC4Automaton that sent it (cSC4Automaton::SendCallbackMessage).
    cISC4Occupant* CallbackOccupant(void* automaton) override {
        return automaton ? static_cast<cSC4Automaton*>(automaton)->AsOccupant() : nullptr;
    }
    bool UDriveViewAvailable() override { return false; }
    void EnterUDriveView(IMySimDispatchAutomaton*, cIGZMessageTarget2*) override { SC4_UNWIRED("IMySimDispatchHost::EnterUDriveView (U-Drive-It with a MySim)"); }
    void ShowUDriveAdvice() override {}
    void ClearVehiclePlayerDrive() override {   // spVehicleManager SetCameraFollow(null)
        if (cSC4AutomataManager* vehicles = NativeAutomataManagerAt(0)) vehicles->SetCameraFollow(nullptr);
    }
    bool WriteSerializable(cIGZOStream& stream, const cIGZUnknown* object) override { return SC4WriteGZSerializable(stream, object); }
    bool ReadSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override { return SC4ReadGZSerializable(stream, iid, out, false); }
};

class FilterHost final : public IMySimFilterHost {
public:
    std::uint8_t BuildingCondition(cISC4BuildingOccupant* building) override {   // GetBuildingCondition (i386 0x005422a6)
        cSC4Lot* lot = building ? LotOf(building->AsOccupant()) : nullptr;
        return lot ? lot->GetCondition() : 0;
    }
};

class AdviceHost final : public IMySimAdviceHost {
public:
    // spCity GetAdvisorSystem -> GetAdvisor(id) -> the MySim, then its ActivateLuaContext (0x2d8) before the link runs.
    bool NotifySimOfLink(int simId) override {
        cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
        cISC4Advisor* advisor = system ? system->GetAdvisor(simId) : nullptr;
        cISC4MySim* sim = nullptr;
        if (!advisor || !advisor->QueryInterface(kGZIID_cISC4MySimLink, reinterpret_cast<void**>(&sim)) || !sim) return false;
        sim->ActivateLuaContext();
        sim->Release();
        return true;
    }
};

const char* StateName(int state) {
    switch (state) {
        case kMySimStateNone: return "none";
        case kMySimStateAtHome: return "at-home";
        case kMySimStateIdle: return "idle";
        case kMySimStateAtWork: return "at-work";
        case kMySimStateGoingToWork: return "going-to-work";
        case kMySimStateGoingHome: return "going-home";
        case kMySimStateAtPylon: return "at-pylon";
        default: return "?";
    }
}

// The balloon's services (IMySimBalloonHost): the generic window it draws into, the city view it lives in, the message
// server that ticks it, the advisor system's random generator for the sound chance and the sound system.
class BalloonHost final : public IMySimBalloonHost {
public:
    gz::cIGZWinGen* CreateWinGen() override {   // GZCOM()->CreateInstance(0x4386d516, 0x5386d516)
        void* raw = nullptr;
        return GZCOM() && GZCOM()->CreateInstance(gz::kGZCLSID_cGZWinGen, gz::kGZIID_cIGZWinGen, &raw) && raw
                   ? static_cast<gz::cIGZWinGen*>(raw) : nullptr;
    }
    cIGZWin* View3DWin() override {   // SL::spView3DWin as its window
        return native::NativeCityView3DWindow();
    }
    cIGZMessageServer2* Messages() override { return SL::spMS2; }
    double RandomPercent() override {   // spCity GetAdvisorSystem (0x144) -> GetRandom (0x24)
        cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
        cRZRandom* random = system ? system->GetRandom() : nullptr;
        return random ? random->RandomDoubleRangeUniform(0.0, 100.0) : 100.0;
    }
    // The sound system events take the sim as their object (the 32-bit original passes the pointer in an integer
    // field of the same event).
    bool SoundSystemPresent() override { return Audio::SoundSystem() != nullptr; }
    void StartBalloonSound(std::uint32_t soundId, cISC4MySim* sim) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->PostEventUnknown(1, sim, soundId, 0, 0, 0);
    }
    void PlaceBalloonSound(cISC4MySim* sim, int x, int y, int z) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) {
            system->PostEventUnknown(0xca45190f, sim, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                                     static_cast<std::uint32_t>(z), 0);
        }
    }
    void StopBalloonSound(cISC4MySim* sim, bool immediate) override {
        if (cIGZSndSys* system = Audio::SoundSystem()) system->PostEventUnknown(2, sim, immediate ? 1 : 0, 0, 0, 0);
    }
};
BalloonHost g_balloonHost;

// The street-sim tool: the city view, the occupant manager's box query and the traffic-automaton view of an occupant.
// The MySim building tools' city services (sc4::IMySimToolHost).
class BuildingToolHost final : public sc4::IMySimToolHost {
public:
    sc4::cISC4View3DWin* View() override { return NativeStreetSimView(); }
    cSC4MySimAgentSimulator* Agents() override { return Agent(); }
    cSC4Lot* OccupantLot(cISC4Occupant* occupant) override { cSC4ZoneDeveloper* lots = LotManager(); return lots && occupant ? lots->GetOccupantLot(occupant) : nullptr; }
    cSC4Lot* LotAt(const cS3DVector3& position) override { cSC4ZoneDeveloper* lots = LotManager(); return lots ? lots->GetLot(position) : nullptr; }
} g_buildingToolHost;

class StreetToolHost final : public sc4::IMyStreetSimToolHost {
public:
    sc4::cISC4View3DWin* View() override { return NativeStreetSimView(); }
    void OccupantsInBox(const float min[3], const float max[3], std::vector<cISC4Occupant*>& out) override {
        out.clear();
        cSC4City* city = HostCity();
        auto* manager = city ? dynamic_cast<cISC4OccupantManager*>(city->GetComponent(CityComponent::OccupantManager)) : nullptr;
        if (!manager) return;
        sc4::OccupantList list;
        manager->GetOccupantsByBBox(list, min, max, 0u, 0xffffffffu);   // vslot 0x90 (type 0, no limit)
        out.assign(list.begin(), list.end());
        manager->ReleaseOccupantList(list);                             // vslot 0xc0: the caller only borrows them
    }
    IMySimDispatchAutomaton* StreetAutomaton(cISC4Occupant* occupant) override {
        auto* automaton = dynamic_cast<cSC4TrafficAutomaton*>(occupant);   // QueryInterface(0x8b99ee7c)
        return automaton ? ViewOf(automaton) : nullptr;
    }
};
StreetToolHost g_streetToolHost;

}  // namespace

sc4::ISituationOccupant* SituationViewOfOccupant(void* engineOccupant);   // app/NativeSituationAutomata.cpp
// The sim of the open MySim panel as a MySim mission binds it: its residence (vslot 0x58, as an occupant) and its Lua context
// (vslot 0x220 ActivateLuaContext) before the mission's script calls.
namespace {
class SituationMySimView final : public sc4::ISituationMySim {
public:
    explicit SituationMySimView(cISC4MySim* sim) : sim_(sim) { sim_->AddRef(); }
    bool QueryInterface(std::uint32_t, void** out) override { *out = nullptr; return false; }
    std::uint32_t AddRef() override { return ++refs_; }
    std::uint32_t Release() override {
        if (refs_ > 1) return --refs_;
        sim_->Release();
        delete this;
        return 0;
    }
    sc4::ISituationOccupant* FindMissionOccupant() override {
        cISC4BuildingOccupant* home = sim_->GetResidence();
        void* raw = nullptr;
        if (!home || !home->QueryInterface(sc4::kGZIID_cISC4Occupant, &raw) || !raw) return nullptr;
        auto* occupant = static_cast<sc4::cISC4Occupant*>(raw);
        occupant->Release();   // the city keeps the building
        return SituationViewOfOccupant(occupant);
    }
    void PrepareScript() override { sim_->ActivateLuaContext(); }
private:
    cISC4MySim* sim_;
    std::uint32_t refs_ = 1;
};
}  // namespace

sc4::ISituationMySim* NativeActiveSituationMySim() {
    cSC4MySimAgentSimulator* agent = Agent();
    int slot = 0;
    cISC4MySim* sim = agent ? agent->GetActiveUIPanelSim(slot) : nullptr;
    return sim ? new SituationMySimView(sim) : nullptr;
}

void InstallMySimHosts() {
    sc4::SetMySimBalloonHost(&g_balloonHost);
    static MySimHost sim;
    static DispatchHost dispatch;
    static FilterHost filter;
    static AdviceHost advice;
    sc4::SetMySimHost(&sim);
    sc4::SetMySimDispatchHost(&dispatch);
    sc4::SetMySimFilterHost(&filter);
    sc4::SetMySimAdviceHost(&advice);
    sc4::SetMyStreetSimToolHost(&g_streetToolHost);
    sc4::SetMySimToolHost(&g_buildingToolHost);
    // cSC4AdvisorCitySituations::UpdateMySimAdvices (i386): SL::spCity 0x11c GetMySimAgentSimulator, vslot 0x30
    // GetActiveUIPanelSim(slot), then the sim's vslot 0x110 ActivateLuaContext before the MySim advices are tested.
    sc4::SetAdvisorCitySituationsRefresh([] {
        cSC4MySimAgentSimulator* agent = Agent();
        int slot = 0;
        if (cISC4MySim* sim = agent ? agent->GetActiveUIPanelSim(slot) : nullptr) sim->ActivateLuaContext();
    });
}

// The agent simulator's get_sim / get_count values (cSC4MySimAgentSimulator::Init, i386 0x0054d538), registered under the
// "mysims" node when the node is already there (the first sim's init_balloons adds it otherwise).
void NativeRegisterMySimAgentGameData(sc4::cSC4MySimAgentSimulator& agent) {
    sc4::cISC4GameDataRegistry* registry = NativeGameDataRegistry();
    if (!registry) return;
    MySimHost::RegisterAgentValues(agent, registry);
}

// The opinion fields cSC4MySim::UpdateVariables writes into the sim's script table, as the scripts read them.
static void PrintOpinionFields(cSC4MySim& sim, int slot) {
    static const char* const kNumbers[] = {
        "eq", "hq", "delta_eq", "delta_hq", "le", "home_condition", "home_has_road", "home_has_power", "home_has_water",
        "local_crime", "home_police_coverage", "home_fire_coverage", "local_air_pollution", "local_water_pollution",
        "local_garbage", "local_radiation", "last_player_trip_day", "desirability", "home_occupant_wealth",
        "local_police_funding", "local_fire_funding", "local_traffic_congestion", "last_commute_time", "employed",
        "unemployed_days", "job_is_neighbor", "job_condition", "job_has_power", "job_has_water", "trip_in_transit",
        "trip_at_location"};
    static const char* const kTexts[] = {"home_building", "job_building", "job_title", "job_city"};
    cSC4AdvisorSystem* system = cSC4AdvisorSystem::Instance();
    cISCLua* lua = system ? system->GetScriptingContext() : nullptr;
    if (!lua || sim.LuaTableRef() < 1) { std::printf("mysims: slot %d opinions: no table\n", slot); return; }
    const int top = lua->GetTop();
    lua->GetRef(sim.LuaTableRef());
    std::string line;
    char buffer[96];
    int missing = 0;
    for (const char* key : kNumbers) {
        lua->PushString(key);
        lua->GetTable(-2);
        if (lua->IsNumber(-1)) std::snprintf(buffer, sizeof(buffer), " %s=%g", key, lua->ToNumber(-1));
        else { std::snprintf(buffer, sizeof(buffer), " %s=nil", key); ++missing; }
        line += buffer;
        lua->Pop(1);
    }
    for (const char* key : kTexts) {
        lua->PushString(key);
        lua->GetTable(-2);
        auto* text = lua->IsUserData(-1) ? static_cast<cIGZString*>(lua->ToUserTag(-1)) : nullptr;
        line += " ";
        line += key;
        line += "=\"";
        line += text ? text->ToChar() : "<nil>";
        line += "\"";
        lua->Pop(1);
    }
    // the trip fields UpdateTripVariables writes (state, flag word and the surroundings of the current location)
    static const char* const kTrip[] = {
        "trip_idle", "trip_at_transit_switch", "last_trip_type", "trip_traffic_congestion", "trip_time_ratio",
        "trip_wait_length", "trip_pollution_air", "trip_pollution_water", "trip_pollution_garbage", "trip_crime",
        "trip_mayor_rating", "trip_eq", "trip_hq", "trip_distress1_p", "trip_distress2_p", "trip_distress3_p",
        "trip_wealth_low_p", "trip_wealth_med_p", "trip_wealth_high_p", "trip_landmark_nearby",
        "trip_landmark_funding_p", "trip_fire_nearby", "trip_riot_nearby", "trip_any_disaster_nearby"};
    std::string trip;
    int tripMissing = 0;
    for (const char* key : kTrip) {
        lua->PushString(key);
        lua->GetTable(-2);
        if (lua->IsNumber(-1)) std::snprintf(buffer, sizeof(buffer), " %s=%g", key, lua->ToNumber(-1));
        else { std::snprintf(buffer, sizeof(buffer), " %s=nil", key); ++tripMissing; }
        trip += buffer;
        lua->Pop(1);
    }
    lua->PushString("trip_landmark_name");
    lua->GetTable(-2);
    auto* landmark = lua->IsUserData(-1) ? static_cast<cIGZString*>(lua->ToUserTag(-1)) : nullptr;
    trip += " landmark_name=\"";
    trip += landmark ? landmark->ToChar() : "<nil>";
    trip += "\"";
    lua->Pop(1);
    lua->SetTop(top);
    std::printf("mysims: slot %d opinions (%d nil):%s\n", slot, missing, line.c_str());
    std::printf("mysims: slot %d trip fields (%d nil):%s\n", slot, tripMissing, trip.c_str());
}

// The city's MySim agent simulator (borrowed; null without a city).
sc4::cSC4MySimAgentSimulator* NativeMySimAgentSimulator() { return Agent(); }

// Kit command `mysims`: the agent simulator's sims and their state (read only).
void NativeMySimReport() {
    cSC4MySimAgentSimulator* agent = Agent();
    if (!agent) { std::printf("mysims: no agent simulator\n"); return; }
    int active = 0;
    for (int i = 0; i < kMySimAgentSlots; ++i) {
        cISC4MySim* view = agent->Sim(i);
        cSC4MySim* sim = view ? dynamic_cast<cSC4MySim*>(view) : nullptr;
        if (!sim) { std::printf("mysims: slot %d empty\n", i); continue; }
        if (sim->IsActive()) ++active;
        float xyz[3] = {};
        sim->GetLocation(xyz);
        cISC4BuildingOccupant* home = sim->GetResidence();
        cISC4BuildingOccupant* job = sim->GetJob();
        cSC4Lot* homeLot = home ? LotOf(home->AsOccupant()) : nullptr;
        int hx = -1, hz = -1;
        if (homeLot) homeLot->GetLocation(hx, hz);
        IMySimDispatch* dispatch = sim->Dispatch();
        std::printf("mysims: slot %d advisor %d \"%s\" state %d (%s) active %d uislot %u male %d wealth %d age %u sign %u gen %u "
                    "home %s cell %d,%d job %s at %.0f,%.0f,%.0f retired %d dispatch %s state %d flags 0x%x\n",
                    i, sim->GetID(), sim->GetPersonalName().ToChar(), sim->State(), StateName(sim->State()), sim->IsActive() ? 1 : 0,
                    sim->GetSlot(), sim->IsMale() ? 1 : 0, sim->GetWealth(), sim->GetAge(), sim->GetSign(), sim->Generation(),
                    home ? home->GetExemplarName().Data() : "-", hx, hz, job ? job->GetExemplarName().Data() : "-",
                    xyz[0], xyz[1], xyz[2], sim->Retired() ? 1 : 0, dispatch ? "yes" : "no", dispatch ? dispatch->GetState() : 0,
                    dispatch ? static_cast<cSC4MySimDispatch*>(dispatch)->Flags() : 0u);
        if (sim->IsActive()) PrintOpinionFields(*sim, i);
        if (sim->IsActive()) {   // the speech balloon: its state, advice, icons and window
            auto* balloon = dynamic_cast<cSC4MySimBalloonWinProc*>(sim->BalloonWinProc());
            cIGZWin* window = balloon && balloon->Window() ? balloon->Window()->AsIGZWin() : nullptr;
            int sx = 0, sy = 0;
            const bool onScreen = sim->GetCurrentScreenLocationXY(&sx, &sy);
            std::printf("mysims: slot %d balloon %s state %d advice %s icons %zu window %s visible %d at %d,%d size %dx%d (sim on screen %d at %d,%d)\n",
                        i, balloon ? "yes" : "no", balloon ? balloon->State() : -1,
                        balloon && balloon->CurrentAdvice() ? "yes" : "no", balloon ? balloon->IconCount() : 0u,
                        window ? "yes" : "no", window && window->IsVisible() ? 1 : 0, window ? window->GetL() : 0,
                        window ? window->GetT() : 0, window ? window->GetW() : 0, window ? window->GetH() : 0, onScreen ? 1 : 0, sx, sy);
        }
        if (sim->IsActive() && dispatch) {   // the automaton the sim rides, if any
            auto* trip = static_cast<cSC4MySimDispatch*>(dispatch);
            cISC4Occupant* rider = trip->AutomataOccupant();
            float where[3] = {};
            if (rider) rider->GetPosition(where);
            void* automaton = nullptr;
            const bool isAutomaton = rider && rider->QueryInterface(0xa9b40f05, &automaton) && automaton;
            if (isAutomaton) static_cast<cIGZUnknown*>(automaton)->Release();
            std::printf("mysims: slot %d trip travel %d route %zu automaton %s%s at %.0f,%.0f,%.0f groups 0x4149:%d 0x414a:%d 0x420b:%d\n", i,
                        trip->GetTravelType(), trip->RouteSize(), rider ? "yes" : "no", isAutomaton ? "" : " (not an automaton)",
                        where[0], where[1], where[2], rider && rider->IsOccupantGroup(0x4149) ? 1 : 0,
                        rider && rider->IsOccupantGroup(0x414a) ? 1 : 0, rider && rider->IsOccupantGroup(0x420b) ? 1 : 0);
            const MySimLocation& here = trip->GetCurrentLocation();
            const MySimLocation& goal = trip->GetCurrentDestination();
            std::printf("mysims: slot %d trip time %d at cell %d,%d (home %d work %d) leave %d destination %d,%d (home %d work %d)\n", i,
                        trip->Time(), here.cellX, here.cellZ, here == trip->GetCurrentDefaultDestination() ? 1 : 0,
                        here == trip->GetCurrentCommuteDestination() ? 1 : 0, here.data28, goal.cellX, goal.cellZ,
                        goal == trip->GetCurrentDefaultDestination() ? 1 : 0, goal == trip->GetCurrentCommuteDestination() ? 1 : 0);
        }
    }
    cSC424HourClock* clock = Clock24();
    std::printf("mysims: %d active of %d, hour %d\n", active, kMySimAgentSlots, clock ? static_cast<int>(clock->GetHour()) : -1);
}

// Kit command `mysimmovein NAME`: what the pick-sim window and the move-in tool do for the player, without input. The agent
// simulator's MoveIn (i386 0x0054cbd8) gives the name to the first inactive sim; the move-in tool's hover test
// (cSC4MySimInputControlMoveIn::OnMouseMove, i386 0x00486e26: a building nobody lives in, 100% complete, residential,
// TestForResidence(building, true, false)) picks the residence and its click (OnMouseDownL, i386 0x00486c7e) calls
// SetResidence. Here the residence is the first building of the city's residence candidates that passes the same test.
void NativeMySimMoveIn(const char* name, bool male, unsigned zodiac) {
    cSC4MySimAgentSimulator* agent = Agent();
    if (!agent) { std::printf("mysimmovein: no agent simulator\n"); return; }
    cSC4MySim* target = nullptr;
    for (int i = 0; i < kMySimAgentOrdinarySlots && !target; ++i) {
        cISC4MySim* view = agent->Sim(i);
        if (view && !view->IsActive()) target = dynamic_cast<cSC4MySim*>(view);
    }
    if (!target) { std::printf("mysimmovein: every sim is active\n"); return; }
    cRZString simName(name), bio, file;
    agent->MoveIn(static_cast<std::uint8_t>(target->GetID()), simName, male, static_cast<std::uint8_t>(zodiac), bio, 0, file);
    std::vector<cISC4BuildingOccupant*> candidates;
    MySimHost host;
    host.ResidenceCandidates(candidates);
    cISC4BuildingOccupant* chosen = nullptr;
    for (cISC4BuildingOccupant* building : candidates) {
        if (agent->MySimLivesHere(building)) continue;
        if (building->GetCompletionPercent() < 1.0f || building->GetBuildingProfile().purpose != 1) continue;
        if (!target->TestForResidence(building, true, false)) continue;
        chosen = building;
        break;
    }
    if (!chosen) { std::printf("mysimmovein: \"%s\" found no residence among %zu candidates\n", name, candidates.size()); return; }
    target->SetResidence(chosen);
    // The move-in runs from the MySim panel, whose sim list refresh (cSC4MySimWinProc::GetCurrentSims) ends with
    // TrackLocation(nullptr): every sim shows its residence and location, which starts the dispatch tracking.
    agent->TrackLocation(nullptr);
    std::printf("mysimmovein: \"%s\" moved into %s (%zu candidates), state %d\n", name, chosen->GetExemplarName().Data(),
                candidates.size(), target->State());
}

}  // namespace native
