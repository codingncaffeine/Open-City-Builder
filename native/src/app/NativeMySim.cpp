// The MySim on the native application: the services cSC4MySim (IMySimHost), cSC4MySimDispatch (IMySimDispatchHost),
// the MySim occupant filters (IMySimFilterHost) and cSC4MySimAdvice (IMySimAdviceHost) reach. Each method is the city
// component the original reaches through its SL:: global; the slot comments name the original call (Mac i386 decompile).
//
// Not native yet (the methods report SC4_UNWIRED with the missing class): the MySim UI neighbours (cSC4WinCalloutBox over the
// residence, cSC4MySimBalloonWinProc, the four MySim input controls, the portrait bitmaps), the home sounds, the ghost
// effect, the trip automata (cSC4MySimDispatch drives a car / pedestrian through the vehicle and pedestrian managers) and
// the U-Drive-It view. A MySim without them still lives: it is stored and read back with its city, keeps its residence,
// job and commute state, ages, and runs its advices.
#include <cstdio>
#include <cstring>
#include <list>
#include <string>
#include <vector>

#include "app/NativeAnimationTicks.h"
#include "app/NativeCity.h"
#include "app/NativeGameData.h"
#include "app/NativeUi.h"
#include "gz/CommandParameterSet.h"
#include "gz/cIGZVariant.h"
#include "platform/Unwired.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZVariant.h"
#include "sc4/AdviceSubject.h"
#include "sc4/AdvisorSystem.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/DBSegment.h"
#include "sc4/GameDataRegistryHelpers.h"
#include "sc4/Lot.h"
#include "sc4/MySim.h"
#include "sc4/MySimAdvice.h"
#include "sc4/MySimAgentSimulator.h"
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
#include "sc4/TrafficSimulator.h"
#include "sc4/ZoneDeveloper.h"
#include "sc4/cSC4City.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSTETerrain.h"

namespace native {
const sc4::cISCPropertyHolder* NativeSimulatorExemplar(std::uint32_t instance);   // app/NativeSimHosts.cpp
bool NativeProjectToViewport(const float* xyz, float out[3]);                     // app/NativeAutomataManagerHost.cpp

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
        cISC4OccupantManager* manager = Occupants();
        out.clear();
        if (!manager) return;
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        manager->GetOccupantManagerCellBounds(x0, z0, x1, z1);
        const int a[2] = {x0, z0}, b[2] = {x1, z1};
        FilteredBuildings(a, b, true, out);
    }
    bool IsResidenceCandidate(cISC4BuildingOccupant* building) override {
        cMySimBuildingFilter filter(true);
        return building && filter.IsOccupantIncluded(building->AsOccupant());
    }
    void BuildingsNearResidence(int cellX, int cellZ, int radius, std::vector<cISC4BuildingOccupant*>& out) override {
        const int a[2] = {cellX - radius, cellZ - radius}, b[2] = {cellX + radius, cellZ + radius};
        FilteredBuildings(a, b, false, out);
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
    void CityOccupants(std::vector<cISC4Occupant*>& out) override {
        out.clear();
        cISC4OccupantManager* manager = Occupants();
        if (!manager) return;
        int x0 = 0, z0 = 0, x1 = 0, z1 = 0;
        manager->GetOccupantManagerCellBounds(x0, z0, x1, z1);
        const int a[2] = {x0, z0}, b[2] = {x1, z1};
        OccupantList list;
        manager->GetOccupantsByOccupantManagerCells(list, a, b, kOccupantTypeBuilding, 0xffffffffu);
        for (cISC4Occupant* occupant : list) {
            out.push_back(occupant);
            occupant->Release();   // borrowed: the occupant manager keeps them
        }
    }
    void BuildingOccupantGroups(std::uint32_t buildingType, std::vector<std::uint32_t>& out) override {
        out.clear();
        (void)buildingType;
        SC4_UNWIRED("IMySimHost::BuildingOccupantGroups (building development simulator exemplar of a building type)");
    }
    bool GetLocalSchoolGrade(std::uint32_t buildingType, float x, float z, int& grade) override {
        return LocalGrade(true, buildingType, x, z, grade);
    }
    bool GetLocalHospitalGrade(std::uint32_t buildingType, float x, float z, int& grade) override {
        return LocalGrade(false, buildingType, x, z, grade);
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
    bool PickRandomWalkModelKey(cGZPersistResourceKey&) override { SC4_UNWIRED("IMySimHost::PickRandomWalkModelKey (pedestrian manager walk model)"); return false; }
    IMySimHomeSound* CreateHomeSound(std::uint32_t) override { SC4_UNWIRED("IMySimHost::CreateHomeSound (MySim home sounds)"); return nullptr; }
    void DestroyHomeSound(IMySimHomeSound*) override {}
    void PlayCitySound(IMySimHomeSound*, const float*) override {}
    IMySimEffect* CreateGhostEffect() override { SC4_UNWIRED("IMySimHost::CreateGhostEffect (the MySimGhost effect)"); return nullptr; }
    bool DispatchIconRect(int, int*) override { SC4_UNWIRED("IMySimHost::DispatchIconRect (dispatch view icon of a tracked MySim)"); return false; }
    // --- objects ---
    IMySimDispatch* CreateDispatch(cSC4MySim* owner) override {   // cSC4MySimDispatch ctor (i386 0x0047...; owner = the sim)
        auto* dispatch = new cSC4MySimDispatch(static_cast<IMySimDispatchOwner*>(owner));
        dispatch->AddRef();
        return dispatch;
    }
    IMySimBalloonWinProc* CreateBalloonWinProc(cISC4MySim*) override { SC4_UNWIRED("IMySimHost::CreateBalloonWinProc (cSC4MySimBalloonWinProc)"); return nullptr; }
    IMySimPylon* CreatePylon() override { SC4_UNWIRED("IMySimHost::CreatePylon (MySim dispatch tool call box)"); return nullptr; }
    // --- view input controls (the MySim tools are not native) ---
    cIGZUnknown* CurrentViewInputControl() override { return nullptr; }
    void SetCurrentViewInputControl(cIGZUnknown*) override {}
    cIGZUnknown* ViewCurrentInputControl() override { return nullptr; }
    void ViewSetInputControl(cIGZUnknown*) override {}
    void ViewPushInputControl(cIGZUnknown*) override {}
    cIGZUnknown* CreateMoveInTool(cSC4MySim*) override { SC4_UNWIRED("IMySimHost::CreateMoveInTool (cSC4MySimInputControlMoveIn)"); return nullptr; }
    cIGZUnknown* CreateDispatchToolControl(cSC4MySim*) override { SC4_UNWIRED("IMySimHost::CreateDispatchToolControl (cSC4MySimInputControlDispatch)"); return nullptr; }
    cIGZUnknown* CreateChangeHouseTool(cSC4MySim*) override { SC4_UNWIRED("IMySimHost::CreateChangeHouseTool (cSC4MySimInputControlChangeHouse)"); return nullptr; }
    cIGZUnknown* CreateChangeJobTool(cSC4MySim*) override { SC4_UNWIRED("IMySimHost::CreateChangeJobTool (cSC4MySimInputControlChangeJob)"); return nullptr; }
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

private:
    // GetLocalSchoolGrade / GetLocalHospitalGrade (spResidentialSim 0x178 / 0x188 school / hospital lists, then the grade of
    // the nearest one of the building type through vslots 0xd0 / 0xd8): the residential simulator's per-building grade query
    // is not exposed natively yet.
    bool LocalGrade(bool, std::uint32_t, float, float, int& grade) {
        SC4_UNWIRED("IMySimHost::GetLocalSchoolGrade / GetLocalHospitalGrade (residential vslots 0x178 / 0x188 -> 0xd0 / 0xd8)");
        grade = 0;
        return false;
    }
};

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
    IMySimDispatchVehicleView* DispatchVehicleView() override { SC4_UNWIRED("IMySimDispatchHost::DispatchVehicleView (dispatch view tracking of a MySim trip)"); return nullptr; }
    IMySimDispatchAutomataManager* AutomataManager(std::uint32_t) override { SC4_UNWIRED("IMySimDispatchHost::AutomataManager (MySim trip automata)"); return nullptr; }
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
    bool TileHasNetwork(int cellX, int cellZ, std::uint32_t mask) override { (void)cellX; (void)cellZ; (void)mask; SC4_UNWIRED("IMySimDispatchHost::TileHasNetwork (MySim trips)"); return false; }
    bool NetworkTilesInRect(const int rect[4], std::deque<std::pair<int, int>>& tiles) override { (void)rect; (void)tiles; SC4_UNWIRED("IMySimDispatchHost::NetworkTilesInRect (MySim trips)"); return false; }
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
    bool UDriveViewAvailable() override { return false; }
    void EnterUDriveView(IMySimDispatchAutomaton*, cIGZMessageTarget2*) override { SC4_UNWIRED("IMySimDispatchHost::EnterUDriveView (U-Drive-It with a MySim)"); }
    void ShowUDriveAdvice() override {}
    void ClearVehiclePlayerDrive() override {}
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

}  // namespace

void InstallMySimHosts() {
    static MySimHost sim;
    static DispatchHost dispatch;
    static FilterHost filter;
    static AdviceHost advice;
    sc4::SetMySimHost(&sim);
    sc4::SetMySimDispatchHost(&dispatch);
    sc4::SetMySimFilterHost(&filter);
    sc4::SetMySimAdviceHost(&advice);
}

// The agent simulator's get_sim / get_count values (cSC4MySimAgentSimulator::Init, i386 0x0054d538), registered under the
// "mysims" node when the node is already there (the first sim's init_balloons adds it otherwise).
void NativeRegisterMySimAgentGameData(sc4::cSC4MySimAgentSimulator& agent) {
    sc4::cISC4GameDataRegistry* registry = NativeGameDataRegistry();
    if (!registry) return;
    MySimHost::RegisterAgentValues(agent, registry);
}

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
    }
    std::printf("mysims: %d active of %d\n", active, kMySimAgentSlots);
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
    std::printf("mysimmovein: \"%s\" moved into %s (%zu candidates), state %d\n", name, chosen->GetExemplarName().Data(),
                candidates.size(), target->State());
}

}  // namespace native
