// city_probe_bindings - probe tooling, not part of the game: stand-ins for the engine seams (the I*Host interfaces the
// recovered classes reach the rest of the engine through) that cSC4App::LoadCity touches while it reads a real city.
//
// Nothing in the repo installs these seams for a running game yet - every Set*Host call outside the unit tests is
// missing - so a real load reaches the first recovered class whose Read() asks its host for something and stops on a
// null host. The probe installs these adapters to get past that and to list which seams the load path really uses:
// Read/WriteSerializable go to the recovered SC4Read/WriteGZSerializable helpers (the original calls), every other
// method answers neutrally (null / false / nothing) and is reported the first time it is called.
#include <cstdio>
#include <set>
#include <string>
#include "gz/SimHostRegistry.h"
#include "sc4/RetainedReference.h"
#include "sc4/DBSegment.h"
#include "sc4/CommercialSimulator.h"
#include "sc4/CrimeSimulator.h"
#include "sc4/Demolition.h"
#include "sc4/DispatchManager.h"
#include "sc4/FireProtectionSimulator.h"
#include "sc4/LandValueSimulator.h"
#include "sc4/LotDeveloper.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PoliceSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PowerSimulator.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/TrafficNetworkMap.h"
#include "sc4/TrafficSimulator.h"
#include "sc4/WealthRequester.h"
#include "sc4/WeatherSimulator.h"
#include "sc4/cSC424HourClock.h"
#include "sc4/cSC4Demand.h"
#include "sc4/cSC4FlammabilitySimulator.h"
#include "sc4/cSC4ZoneManager.h"
#include "sc4/cSTETerrainView3D.h"
#include "tools/city_probe_hosts.h"


namespace {
void Seam(const char* what) {
    static std::set<std::string> seen;
    if (seen.insert(what).second) std::printf("  seam used: %s\n", what);
}
}  // namespace

namespace sc4 {
namespace {
// Stream seams of the simulators' hosts. The game's host implements these with SC4Read/WriteGZSerializable; nested
// objects whose class the probe does not model are read and dropped so the stream stays in step.
void* ReadNested(cIGZIStream& stream, std::uint32_t iid) {
    void* raw = nullptr;
    SC4ReadGZSerializable(stream, iid, &raw, false);
    return raw;
}
void DropNested(cIGZIStream& stream) {
    if (void* raw = ReadNested(stream, 1)) static_cast<cIGZUnknown*>(raw)->Release();
}
}  // namespace
namespace {
class Probe0 final : public IDispatchHost {
public:
    cIGZMessageServer2* MessageServer() override { Seam("IDispatchHost::MessageServer"); return nullptr; }
    IDispatchCity* City() override { Seam("IDispatchHost::City"); return nullptr; }
    IDispatchVehicleView* CreateDispatchVehicleView() override { Seam("IDispatchHost::CreateDispatchVehicleView"); return nullptr; }
    IPylonOccupant* CreatePylonOccupant() override { Seam("IDispatchHost::CreatePylonOccupant"); return nullptr; }
    IAutomataDispatchManager* GetAutomataManagerForGroup(std::uint32_t group) override { Seam("IDispatchHost::GetAutomataManagerForGroup"); return nullptr; }
    void AddDispatchViewLayer(IDispatchVehicleView* view) override { Seam("IDispatchHost::AddDispatchViewLayer");}
    void RemoveDispatchViewLayer(IDispatchVehicleView* view) override { Seam("IDispatchHost::RemoveDispatchViewLayer");}
    bool WriteSerializable(cIGZOStream& stream, const cIGZUnknown* object) override { return SC4WriteGZSerializable(stream, object); }
    bool ReadSerializable(cIGZIStream& stream, std::uint32_t iid, void** out) override { return SC4ReadGZSerializable(stream, iid, out, false); }
};
Probe0 g_probe0;
}  // namespace
}

namespace sc4 {
class Probe1 final : public IPowerHost {
public:
    int CityCellsX() override { Seam("IPowerHost::CityCellsX"); return {}; }
    int CityCellsZ() override { Seam("IPowerHost::CityCellsZ"); return {}; }
    float CellSizeX() override { Seam("IPowerHost::CellSizeX"); return 0; }
    float CellSizeZ() override { Seam("IPowerHost::CellSizeZ"); return 0; }
    void OccupantsInRect(const PowerRect& rect, std::vector<IPowerOccupant*>& out) override { Seam("IPowerHost::OccupantsInRect");}
    bool IsPowerLineCell(int x, int z) override { Seam("IPowerHost::IsPowerLineCell"); return false; }
    void SetSupplyValue(int x, int z, std::uint8_t value) override { Seam("IPowerHost::SetSupplyValue");}
    float OrdinancePowerFactor() override { Seam("IPowerHost::OrdinancePowerFactor"); return 0; }
    PowerPlantBudgetEntry GetPlantBudgetEntry(IPowerOccupant* plant) override { Seam("IPowerHost::GetPlantBudgetEntry"); return {}; }
    float DepartmentFundingFraction() override { Seam("IPowerHost::DepartmentFundingFraction"); return 0; }
    int CityDay() override { Seam("IPowerHost::CityDay"); return {}; }
    // The city file's grid, plant and pole references are not resolved by the port's power simulator: they are kept as read
    // and written back in the same order (Write calls these in Read's order, with null plants and poles).
    bool ReadPowerGrid(cIGZIStream& stream) override {
        powerGrid_ = cRetainedReference();
        plants_.clear();
        poles_.clear();
        return powerGrid_.Read(stream);
    }
    bool ReadSupplyGrid(cIGZIStream& stream) override { supplyGrid_ = cRetainedReference(); return supplyGrid_.Read(stream); }
    bool ReadPlant(cIGZIStream& stream, IPowerOccupant** out) override { *out = nullptr; plants_.emplace_back(); return plants_.back().Read(stream); }
    bool ReadPole(cIGZIStream& stream, IPowerPole** out) override { *out = nullptr; poles_.emplace_back(); return poles_.back().Read(stream); }
    void WritePowerGrid(cIGZOStream& stream) override { plantAt_ = poleAt_ = 0; powerGrid_.Write(stream); }
    void WriteSupplyGrid(cIGZOStream& stream) override { supplyGrid_.Write(stream); }
    void WritePlant(cIGZOStream& stream, IPowerOccupant*) override {
        if (plantAt_ < plants_.size()) plants_[plantAt_++].Write(stream); else SC4WriteGZSerializable(stream, nullptr);
    }
    void WritePole(cIGZOStream& stream, IPowerPole*) override {
        if (poleAt_ < poles_.size()) poles_[poleAt_++].Write(stream); else SC4WriteGZSerializable(stream, nullptr);
    }
private:
    cRetainedReference powerGrid_, supplyGrid_;
    std::vector<cRetainedReference> plants_, poles_;
    std::size_t plantAt_ = 0, poleAt_ = 0;
};
Probe1 g_IPowerHost;
}

namespace sc4 {
class Probe2 final : public IPlumbingHost {
public:
    int CityCellsX() override { Seam("IPlumbingHost::CityCellsX"); return {}; }
    int CityCellsZ() override { Seam("IPlumbingHost::CityCellsZ"); return {}; }
    IPlumbingWaterMap* WaterMap() override { Seam("IPlumbingHost::WaterMap"); return nullptr; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { Seam("IPlumbingHost::PositionToCell");}
    float DepartmentFundingFraction() override { Seam("IPlumbingHost::DepartmentFundingFraction"); return 0; }
    std::uint32_t PollutionAt(int x, int z) override { Seam("IPlumbingHost::PollutionAt"); return {}; }
    std::vector<IPlumbingOccupant*> BuildingsAtCell(int x, int z) override { Seam("IPlumbingHost::BuildingsAtCell"); return {}; }
    PlumbingBudgetEntry GetBudgetEntry(cISCPropertyHolder* holder) override { Seam("IPlumbingHost::GetBudgetEntry"); return {}; }
    void SetPipeInfrastructureCost(int cost) override { Seam("IPlumbingHost::SetPipeInfrastructureCost");}
    void ApplyBuildingBudget(IPlumbingOccupant* building, bool added) override { Seam("IPlumbingHost::ApplyBuildingBudget");}
    bool GetLotRect(int cellX, int cellZ, PlumbingRect& out) override { Seam("IPlumbingHost::GetLotRect"); return false; }
    std::int32_t LotWaterCapacity(IPlumbingOccupant* building) override { Seam("IPlumbingHost::LotWaterCapacity"); return {}; }
    void SetLotCondition(int cellX, int cellZ, int state) override { Seam("IPlumbingHost::SetLotCondition");}
    void BurstPipeAt(int cellX, int cellZ) override { Seam("IPlumbingHost::BurstPipeAt");}
    bool ProducerLotDisabled(IPlumbingOccupant* occupant) override { Seam("IPlumbingHost::ProducerLotDisabled"); return false; }
    std::vector<IPlumbingNeighborConnection*> NeighborConnections(void* regionalCity) override { Seam("IPlumbingHost::NeighborConnections"); return {}; }
    bool ReadGrid(cIGZIStream& stream, cSC4SimGrid<std::uint8_t>** out) override { *out = SimGridCast<std::uint8_t>(ReadNested(stream, 1)); return true; }
    void WriteGrid(cIGZOStream& stream, cSC4SimGrid<std::uint8_t>* grid) override { SC4WriteGZSerializable(stream, static_cast<cISC4SimGridBase*>(grid)); }
    bool ReadWaterGrid(cIGZIStream& stream) override { DropNested(stream); return true; }
    bool ReadOccupant(cIGZIStream& stream, IPlumbingOccupant** out) override { *out = nullptr; DropNested(stream); return true; }
    bool ReadPipe(cIGZIStream& stream, IPlumbingPipe** out) override { *out = nullptr; DropNested(stream); return true; }
};
Probe2 g_IPlumbingHost;
}

namespace sc4 {
class Probe3 final : public IDemolitionHost {
public:
    void AddNotification(std::uint32_t message) override { Seam("IDemolitionHost::AddNotification");}
    void RemoveNotification(std::uint32_t message) override { Seam("IDemolitionHost::RemoveNotification");}
    void PostOccupantMessage(std::uint32_t message, IDemolishable* occupant, bool sendNow) override { Seam("IDemolitionHost::PostOccupantMessage");}
    cISCPropertyHolder* TuningExemplar(std::uint32_t group, std::uint32_t instance) override { Seam("IDemolitionHost::TuningExemplar"); return nullptr; }
    void CityCells(int& x, int& z) override { Seam("IDemolitionHost::CityCells");}
    IDemolishFilter* ViewOccupantFilter() override { Seam("IDemolitionHost::ViewOccupantFilter"); return nullptr; }
    void ViewBeginSweep() override { Seam("IDemolitionHost::ViewBeginSweep");}
    bool OccupantAllowedByFilter(IDemolishable* occupant, const DemolitionFilterState& state) override { Seam("IDemolitionHost::OccupantAllowedByFilter"); return false; }
    bool LocalFilterIncludes(IDemolishable* occupant) override { Seam("IDemolitionHost::LocalFilterIncludes"); return false; }
    std::vector<IDemolishable*> OccupantsInRect(const DemolitionCellRect& cells, const DemolitionFilterState& state) override { Seam("IDemolitionHost::OccupantsInRect"); return {}; }
    std::vector<IDemolishable*> OccupantsForTerrain(const DemolitionCellRect& cells, int mode) override { Seam("IDemolitionHost::OccupantsForTerrain"); return {}; }
    bool RemoveOccupant(IDemolishable* occupant, bool refund, bool deferred) override { Seam("IDemolitionHost::RemoveOccupant"); return false; }
    void ReleaseOccupantList(std::vector<IDemolishable*>& list) override { Seam("IDemolitionHost::ReleaseOccupantList");}
    void OccupantManagerCentre(int x, int z, float& a, float& b) override { Seam("IDemolitionHost::OccupantManagerCentre");}
    IDemolishableLot* LotAtCell(int x, int z) override { Seam("IDemolitionHost::LotAtCell"); return nullptr; }
    IDemolishableLot* LotOfOccupant(IDemolishable* occupant) override { Seam("IDemolitionHost::LotOfOccupant"); return nullptr; }
    std::vector<IDemolishableLot*> LotsInRect(const DemolitionCellRect& cells) override { Seam("IDemolitionHost::LotsInRect"); return {}; }
    void RemoveLot(IDemolishableLot* lot) override { Seam("IDemolitionHost::RemoveLot");}
    void CreateLotAt(int x, int z, std::uint8_t orientation) override { Seam("IDemolitionHost::CreateLotAt");}
    int ZoneTypeAt(int x, int z) override { Seam("IDemolitionHost::ZoneTypeAt"); return {}; }
    void ClearZones(const DemolitionCellRect& cells) override { Seam("IDemolitionHost::ClearZones");}
    void LotPrepareRemoval(IDemolishableLot* lot) override { Seam("IDemolitionHost::LotPrepareRemoval");}
    void LotFinishRemoval(IDemolishableLot* lot) override { Seam("IDemolitionHost::LotFinishRemoval");}
    void LotSkipRemoval(IDemolishableLot* lot, bool flag) override { Seam("IDemolitionHost::LotSkipRemoval");}
    void NetworkBatch(bool begin) override { Seam("IDemolitionHost::NetworkBatch");}
    void UpdateNetworkManager(const std::vector<IDemolishable*>& networks) override { Seam("IDemolitionHost::UpdateNetworkManager");}
    void NotifyNeighborsNetworkChange(IDemolishable* network) override { Seam("IDemolitionHost::NotifyNeighborsNetworkChange");}
    float TerrainHeight(int x, int z) override { Seam("IDemolitionHost::TerrainHeight"); return 0; }
    bool TerrainCellSkipped(int x, int z) override { Seam("IDemolitionHost::TerrainCellSkipped"); return false; }
    void TerrainLevelCell(int x, int z) override { Seam("IDemolitionHost::TerrainLevelCell");}
    void TerrainLevelRect(const DemolitionCellRect& cells, std::uint32_t reason) override { Seam("IDemolitionHost::TerrainLevelRect");}
    void TerrainUpdateRect(const DemolitionCellRect& cells, std::uint32_t reason) override { Seam("IDemolitionHost::TerrainUpdateRect");}
    float TerrainHeightAtMetres(float x, float z) override { Seam("IDemolitionHost::TerrainHeightAtMetres"); return 0; }
    bool PropManagerAvailable() override { Seam("IDemolitionHost::PropManagerAvailable"); return false; }
    bool PropManagerRemove(IDemolishable* prop) override { Seam("IDemolitionHost::PropManagerRemove"); return false; }
    bool NetworkCost(IDemolishable* network, std::int64_t& cost) override { Seam("IDemolitionHost::NetworkCost"); return false; }
    bool BuildingCost(IDemolishable* building, std::int64_t& cost) override { Seam("IDemolitionHost::BuildingCost"); return false; }
    bool FloraCost(IDemolishable* flora, std::int64_t& cost) override { Seam("IDemolitionHost::FloraCost"); return false; }
    bool PoleCost(IDemolishable* pole, std::int64_t& cost) override { Seam("IDemolitionHost::PoleCost"); return false; }
    std::vector<IDemolishable*> PoleConnections(IDemolishable* pole) override { Seam("IDemolitionHost::PoleConnections"); return {}; }
    void PoleDetach(IDemolishable* pole) override { Seam("IDemolitionHost::PoleDetach");}
    bool IsPlainOccupant(IDemolishable* occupant) override { Seam("IDemolitionHost::IsPlainOccupant"); return false; }
    IDemolitionEffect* CreateEffect(std::uint32_t effectSet, std::uint32_t index) override { Seam("IDemolitionHost::CreateEffect"); return nullptr; }
    void ReleaseEffect(IDemolitionEffect* effect) override { Seam("IDemolitionHost::ReleaseEffect");}
    void PlayGroupEffect(std::uint32_t index, float x, float height, float z) override { Seam("IDemolitionHost::PlayGroupEffect");}
    void PlaySound(std::uint32_t soundId) override { Seam("IDemolitionHost::PlaySound");}
    float RenderMinimumSize() override { Seam("IDemolitionHost::RenderMinimumSize"); return 0; }
    float ViewScale() override { Seam("IDemolitionHost::ViewScale"); return 0; }
};
Probe3 g_IDemolitionHost;
}

namespace sc4 {
class Probe4 final : public IZoneManagerHost {
public:
    int CityCellsX() override { Seam("IZoneManagerHost::CityCellsX"); return {}; }
    int CityCellsZ() override { Seam("IZoneManagerHost::CityCellsZ"); return {}; }
    cISCPropertyHolder* GetTunables(const cGZPersistResourceKey& key) override { Seam("IZoneManagerHost::GetTunables"); return nullptr; }
};
Probe4 g_IZoneManagerHost;
}

namespace sc4 {
class Probe5 final : public IFireHost {
public:
    int CurrentDay() override { Seam("IFireHost::CurrentDay"); return {}; }
    void PostMessage(std::uint32_t message, IFireOccupant* occupant) override { Seam("IFireHost::PostMessage");}
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { Seam("IFireHost::AddHistory");}
    bool FundingFraction(IFireOccupant* occupant, std::uint32_t propertyId, float& fraction) override { Seam("IFireHost::FundingFraction"); return false; }
    bool FundingInfo(IFireOccupant* occupant, std::uint32_t propertyId, int& cost, float& fraction) override { Seam("IFireHost::FundingInfo"); return false; }
    bool SetFundingFraction(IFireOccupant* occupant, std::uint32_t propertyId, const SC4Percentage& fraction) override { Seam("IFireHost::SetFundingFraction"); return false; }
    float DepartmentBudgetFraction() override { Seam("IFireHost::DepartmentBudgetFraction"); return 0; }
    void* DepartmentBudget() override { Seam("IFireHost::DepartmentBudget"); return nullptr; }
    void SetStrikerGroup(IFireOccupant* occupant, bool add) override { Seam("IFireHost::SetStrikerGroup");}
    void RegisterFundedStation(IFireOccupant* occupant, std::uint32_t propertyId, bool add) override { Seam("IFireHost::RegisterFundedStation");}
    float PoliceCoverageShare() override { Seam("IFireHost::PoliceCoverageShare"); return 0; }
    int AvailableDispatchCount(std::uint32_t purpose) override { Seam("IFireHost::AvailableDispatchCount"); return {}; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) override { Seam("IFireHost::ActiveDispatches"); return {}; }
    bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) override { Seam("IFireHost::GetDispatch"); return false; }
    bool LotAllowsDispatch(IProtectionStation* station) override { Seam("IFireHost::LotAllowsDispatch"); return false; }
    bool IsAboveWater(float x, float z) override { Seam("IFireHost::IsAboveWater"); return false; }
    bool IsZonedAt(int x, int z) override { Seam("IFireHost::IsZonedAt"); return false; }
    void TakeStationDispatch(IProtectionStation* station) override { Seam("IFireHost::TakeStationDispatch");}
};
Probe5 g_IFireHost;
}

namespace sc4 {
class Probe6 final : public IWeatherHost {
public:
    cISCPropertyHolder* SimulatorTunables(const cGZPersistResourceKey& key,
                                                  std::uint32_t filter) override { return nullptr; }
    float CitySizeX() override { Seam("IWeatherHost::CitySizeX"); return 0; }
    float CitySizeZ() override { Seam("IWeatherHost::CitySizeZ"); return 0; }
    float CityCellSizeX() override { Seam("IWeatherHost::CityCellSizeX"); return 0; }
    float CityCellSizeZ() override { Seam("IWeatherHost::CityCellSizeZ"); return 0; }
    int CityHemisphere() override { Seam("IWeatherHost::CityHemisphere"); return {}; }
    int SimulatorMonth() override { Seam("IWeatherHost::SimulatorMonth"); return {}; }
    void RegisterAgent(cIGZMessageTarget2* target, int interval, const char* name) override { Seam("IWeatherHost::RegisterAgent");}
    void UnregisterAgent(cIGZMessageTarget2* target) override { Seam("IWeatherHost::UnregisterAgent");}
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { Seam("IWeatherHost::AddNotification");}
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { Seam("IWeatherHost::RemoveNotification");}
    void PostMessage(cIGZMessage2* message) override { Seam("IWeatherHost::PostMessage");}
    void MoistureValueRange(std::uint8_t& low, std::uint8_t& high) override { Seam("IWeatherHost::MoistureValueRange");}
    IWeatherTerrain* Terrain() override { Seam("IWeatherHost::Terrain"); return nullptr; }
    void MakeWaterAreaList(IWeatherTerrain& terrain, std::vector<cRZRect>& out, int minWidth,
                                   int minDepth, int cellsX, int cellsZ, int cellWidth, int cellDepth,
                                   const cRZRect* area) override {}
};
Probe6 g_IWeatherHost;
}

namespace sc4 {
class Probe7 final : public IResidentialHost {
public:
    std::uint32_t CurrentDay() override { Seam("IResidentialHost::CurrentDay"); return {}; }
    void PostStrikeMessage(std::uint32_t messageId, IResidentialOccupant* occupant, bool begin, std::uint32_t days) override { Seam("IResidentialHost::PostStrikeMessage");}
    void SetStrikerGroup(bool education, IResidentialOccupant* occupant, bool add) override { Seam("IResidentialHost::SetStrikerGroup");}
    bool CityHasOccupantManager() override { Seam("IResidentialHost::CityHasOccupantManager"); return false; }
    bool IsBudgeted(IResidentialOccupant* occupant, std::uint32_t propertyId) override { Seam("IResidentialHost::IsBudgeted"); return false; }
    IBudgetRecord* GetBudgetRecord(IResidentialOccupant* occupant, std::uint32_t propertyId) override { Seam("IResidentialHost::GetBudgetRecord"); return nullptr; }
    void GetSchoolQueryData(IResidentialOccupant* occupant, ResidentialQueryData& out) override { Seam("IResidentialHost::GetSchoolQueryData");}
    void GetHospitalQueryData(IResidentialOccupant* occupant, ResidentialQueryData& out) override { Seam("IResidentialHost::GetHospitalQueryData");}
    void GetOccupantEfficiencyRatings(IResidentialOccupant* occupant, float& staff, bool education) override { Seam("IResidentialHost::GetOccupantEfficiencyRatings");}
    std::uint8_t CellWorkforce(unsigned x, unsigned z) override { Seam("IResidentialHost::CellWorkforce"); return {}; }
    std::uint8_t GlobalWorkforce() override { Seam("IResidentialHost::GlobalWorkforce"); return {}; }
    float CellLifeExpectancyBase(unsigned x, unsigned z) override { Seam("IResidentialHost::CellLifeExpectancyBase"); return 0; }
};
Probe7 g_IResidentialHost;
}

namespace sc4 {
class Probe8 final : public IPoliceHost {
public:
    int CurrentDay() override { Seam("IPoliceHost::CurrentDay"); return {}; }
    void PostMessage(std::uint32_t message, IPoliceOccupant* occupant) override { Seam("IPoliceHost::PostMessage");}
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { Seam("IPoliceHost::AddHistory");}
    bool FundingFraction(IPoliceOccupant* occupant, std::uint32_t propertyId, float& fraction) override { Seam("IPoliceHost::FundingFraction"); return false; }
    bool FundingInfo(IPoliceOccupant* occupant, std::uint32_t propertyId, int& cost, float& fraction) override { Seam("IPoliceHost::FundingInfo"); return false; }
    float DepartmentBudgetFraction() override { Seam("IPoliceHost::DepartmentBudgetFraction"); return 0; }
    void SetStrikerGroup(IPoliceOccupant* occupant, bool add) override { Seam("IPoliceHost::SetStrikerGroup");}
    void SetJailbreakGroup(IPoliceOccupant* occupant, bool add) override { Seam("IPoliceHost::SetJailbreakGroup");}
    void RegisterFundedStation(IPoliceOccupant* occupant, std::uint32_t propertyId, bool add) override { Seam("IPoliceHost::RegisterFundedStation");}
    int AvailableDispatchCount(std::uint32_t purpose) override { Seam("IPoliceHost::AvailableDispatchCount"); return {}; }
    std::vector<std::uint32_t> ActiveDispatches(std::uint32_t purpose) override { Seam("IPoliceHost::ActiveDispatches"); return {}; }
    bool GetDispatch(std::uint32_t id, ProtectionDispatchInfo& info) override { Seam("IPoliceHost::GetDispatch"); return false; }
    bool LotAllowsDispatch(IProtectionStation* station) override { Seam("IPoliceHost::LotAllowsDispatch"); return false; }
    bool IsAboveWater(float x, float z) override { Seam("IPoliceHost::IsAboveWater"); return false; }
    bool IsZonedAt(int x, int z) override { Seam("IPoliceHost::IsZonedAt"); return false; }
    void TakeStationDispatch(IProtectionStation* station) override { Seam("IPoliceHost::TakeStationDispatch");}
};
Probe8 g_IPoliceHost;
}

namespace sc4 {
class Probe9 final : public ITrafficHost {
public:
    void PostMessage(std::uint32_t message) override { Seam("ITrafficHost::PostMessage");}
    void PostPotholeMessage(int x, int z, bool potholed) override { Seam("ITrafficHost::PostPotholeMessage");}
    void SetRoadBudgetExpense(int line, int amount) override { Seam("ITrafficHost::SetRoadBudgetExpense");}
    void SetMassTransitBudgetExpense(int line, int amount) override { Seam("ITrafficHost::SetMassTransitBudgetExpense");}
    float MassTransitFundingFraction() override { Seam("ITrafficHost::MassTransitFundingFraction"); return 0; }
    float CityCellSize() override { Seam("ITrafficHost::CityCellSize"); return 0; }
    bool IsRightHandTraffic() override { Seam("ITrafficHost::IsRightHandTraffic"); return false; }
    short DemandGridValue(std::uint32_t gridId, int x, int z) override { Seam("ITrafficHost::DemandGridValue"); return {}; }
    void ActiveOrdinanceStrategyAdjustments(std::vector<std::vector<std::int32_t>>& out) override { Seam("ITrafficHost::ActiveOrdinanceStrategyAdjustments");}
    ITrafficLot* LotAt(int x, int z, int filter) override { Seam("ITrafficHost::LotAt"); return nullptr; }
    void* NeighborFindConnection(int connectionType, int x, int z) override { Seam("ITrafficHost::NeighborFindConnection"); return nullptr; }
    void* NeighborFindConnectionOnOtherSide(void* connection) override { Seam("ITrafficHost::NeighborFindConnectionOnOtherSide"); return nullptr; }
    void NeighborGetAllConnections(std::vector<void*>& out) override { Seam("ITrafficHost::NeighborGetAllConnections");}
    void NeighborGetConnectedCities(std::vector<void*>& out) override { Seam("ITrafficHost::NeighborGetConnectedCities");}
    cISCPropertyHolder* NeighborConnectionProperties(void* connection) override { Seam("ITrafficHost::NeighborConnectionProperties"); return nullptr; }
    void NeighborConnectionCell(void* connection, int& x, int& z) override { Seam("ITrafficHost::NeighborConnectionCell");}
    void* NeighborConnectionCity(void* connection) override { Seam("ITrafficHost::NeighborConnectionCity"); return nullptr; }
    bool NeighborCityEstablished(void* city) override { Seam("ITrafficHost::NeighborCityEstablished"); return false; }
    void NeighborCityRegionCell(void* city, int& x, int& z) override { Seam("ITrafficHost::NeighborCityRegionCell");}
    int NeighborCityExtrapolatedPopulation(void* city, std::uint32_t purpose) override { Seam("ITrafficHost::NeighborCityExtrapolatedPopulation"); return {}; }
    int RegionDemandTotal(std::uint32_t gridId) override { Seam("ITrafficHost::RegionDemandTotal"); return {}; }
    SC4RectInt32 TrafficSourceStartRect(void* source) override { Seam("ITrafficHost::TrafficSourceStartRect"); return {}; }
};
Probe9 g_ITrafficHost;
}

namespace sc4 {
class Probe10 final : public ITrafficNetworkMapHost {
public:
    int CityCellsX() override { Seam("ITrafficNetworkMapHost::CityCellsX"); return {}; }
    int CityCellsZ() override { Seam("ITrafficNetworkMapHost::CityCellsZ"); return {}; }
    void PositionToCell(float x, float z, int& cellX, int& cellZ) override { Seam("ITrafficNetworkMapHost::PositionToCell");}
    bool SimulationActive() override { Seam("ITrafficNetworkMapHost::SimulationActive"); return false; }
    ITrafficNetworkMapLot* LotAtCell(int cellX, int cellZ) override { Seam("ITrafficNetworkMapHost::LotAtCell"); return nullptr; }
    ITrafficNetworkMapLot* LotOf(cISC4Occupant* occupant) override { Seam("ITrafficNetworkMapHost::LotOf"); return nullptr; }
    std::vector<cISC4Occupant*> NetworkOccupantsInBox(const float boxMin[3], const float boxMax[3]) override { Seam("ITrafficNetworkMapHost::NetworkOccupantsInBox"); return {}; }
    void ReleaseOccupants(const std::vector<cISC4Occupant*>& occupants) override { Seam("ITrafficNetworkMapHost::ReleaseOccupants");}
    std::vector<ITrafficNetworkMapProp*> PropsAtCell(int cellX, int cellZ) override { Seam("ITrafficNetworkMapHost::PropsAtCell"); return {}; }
    bool HasTerrain() override { Seam("ITrafficNetworkMapHost::HasTerrain"); return false; }
    float TerrainHeightAt(float x, float z) override { Seam("ITrafficNetworkMapHost::TerrainHeightAt"); return 0; }
    ITrafficNetworkMapBuilding* LotBuilding(ITrafficNetworkMapLot* lot) override { Seam("ITrafficNetworkMapHost::LotBuilding"); return nullptr; }
    int ActiveEdgesForBuilding(std::uint32_t purpose, std::uint8_t wealth) override { Seam("ITrafficNetworkMapHost::ActiveEdgesForBuilding"); return {}; }
    void GetViewPosition(float& x, float& y, float& z) override { Seam("ITrafficNetworkMapHost::GetViewPosition");}
    void GetViewTarget(float& x, float& y, float& z) override { Seam("ITrafficNetworkMapHost::GetViewTarget");}
    void SubscribeMessage(std::uint32_t message) override { Seam("ITrafficNetworkMapHost::SubscribeMessage");}
    void UnsubscribeMessage(std::uint32_t message) override { Seam("ITrafficNetworkMapHost::UnsubscribeMessage");}
    void MakeTunnelPaths(ITrafficNetworkMapPathInfo* pathInfo, ITrafficNetworkMapOccupant* from,
                                 ITrafficNetworkMapOccupant* to) override {}
};
Probe10 g_ITrafficNetworkMapHost;
}

namespace sc4 {
class Probe11 final : public IPollutionHost {
public:
    bool PowerAvailable() override { Seam("IPollutionHost::PowerAvailable"); return false; }
    bool OccupantPowered(IPollutionOccupant* occupant) override { Seam("IPollutionHost::OccupantPowered"); return false; }
    float SanitationFunding() override { Seam("IPollutionHost::SanitationFunding"); return 0; }
    void LandfillRange(int& low, int& high) override { Seam("IPollutionHost::LandfillRange");}
    int LandfillTransportLoad() override { Seam("IPollutionHost::LandfillTransportLoad"); return {}; }
    void PostMessage(std::uint32_t message, std::uint32_t arg) override { Seam("IPollutionHost::PostMessage");}
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { Seam("IPollutionHost::AddHistory");}
    float RecyclingCapacitySlots() override { Seam("IPollutionHost::RecyclingCapacitySlots"); return 0; }
};
Probe11 g_IPollutionHost;
}

namespace sc4 {
class Probe12 final : public ICommercialHost {
public:
    void AddIdleClient(cIGZMessageTarget2* target, int kind, const char* name, int priority) override { Seam("ICommercialHost::AddIdleClient");}
    void RemoveIdleClient(cIGZMessageTarget2* target) override { Seam("ICommercialHost::RemoveIdleClient");}
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { Seam("ICommercialHost::AddNotification");}
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { Seam("ICommercialHost::RemoveNotification");}
    bool AcquireServices() override { Seam("ICommercialHost::AcquireServices"); return false; }
    void ReleaseServices() override { Seam("ICommercialHost::ReleaseServices");}
    void FloodFillDistance(cSC4SimGrid<std::uint8_t>& grid, int sourceKind) override { Seam("ICommercialHost::FloodFillDistance");}
};
Probe12 g_ICommercialHost;
}

namespace sc4 {
class Probe13 final : public ICrimeHost {
public:
    ICrimeLot* RandomLot() override { Seam("ICrimeHost::RandomLot"); return nullptr; }
    ICrimeLot* LotAt(int x, int z) override { Seam("ICrimeHost::LotAt"); return nullptr; }
    int ZoneTypeAt(int x, int z) override { Seam("ICrimeHost::ZoneTypeAt"); return {}; }
    int PropCrimeTypeId(std::uint32_t propId) override { Seam("ICrimeHost::PropCrimeTypeId"); return {}; }
    int PoliceJailPopulation() override { Seam("ICrimeHost::PoliceJailPopulation"); return {}; }
    int PoliceResetUnjailedCriminals() override { Seam("ICrimeHost::PoliceResetUnjailedCriminals"); return {}; }
    void PoliceProcessMonthlyCrimeStats() override { Seam("ICrimeHost::PoliceProcessMonthlyCrimeStats");}
    const cSC4SimGrid<std::uint8_t>* PoliceCoverageGrid() override { Seam("ICrimeHost::PoliceCoverageGrid"); return {}; }
    void DemandValues(int demandId, float& a, float& b, float& c) override { Seam("ICrimeHost::DemandValues");}
    float OrdinanceFactor(std::uint32_t ordinanceId) override { Seam("ICrimeHost::OrdinanceFactor"); return 0; }
    void ResidentialGrids(int wealth, ICrimeCensusGrid*& population, ICrimeCensusGrid*& value) override { Seam("ICrimeHost::ResidentialGrids");}
    std::vector<cGZPersistResourceKey> ListCrimeTypeExemplars() override { Seam("ICrimeHost::ListCrimeTypeExemplars"); return {}; }
    cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& key) override { Seam("ICrimeHost::LoadExemplar"); return nullptr; }
    void ReleaseExemplar(cISCPropertyHolder* exemplar) override { Seam("ICrimeHost::ReleaseExemplar");}
    bool LoadExemplarString(std::uint32_t group, std::uint32_t instance, std::uint32_t stringGroup, std::string& out) override { Seam("ICrimeHost::LoadExemplarString"); return false; }
    void PostCrimeEvent(std::uint32_t typeId, ICrimeLot* lot, const std::string& description) override { Seam("ICrimeHost::PostCrimeEvent");}
    void PostCrimesUpdated() override { Seam("ICrimeHost::PostCrimesUpdated");}
    void StartArson(ICrimeLot* lot, ICrimeBuilding* building) override { Seam("ICrimeHost::StartArson");}
    bool WriteLot(cIGZOStream& stream, ICrimeLot* lot) override { Seam("ICrimeHost::WriteLot"); return false; }
    ICrimeLot* ReadLot(cIGZIStream& stream) override { Seam("ICrimeHost::ReadLot"); return nullptr; }
    void LotPositionToIncidentXZ(float x, float z, float& outX, float& outZ) override { Seam("ICrimeHost::LotPositionToIncidentXZ");}
    std::vector<ICrimeOccupant*> OccupantsInBox(const float min[3], const float max[3]) override { Seam("ICrimeHost::OccupantsInBox"); return {}; }
    void ReleaseOccupants(std::vector<ICrimeOccupant*>& list) override { Seam("ICrimeHost::ReleaseOccupants");}
    double RandomDouble() override { Seam("ICrimeHost::RandomDouble"); return 0; }
    double RandomDoubleRange(double lo, double hi) override { Seam("ICrimeHost::RandomDoubleRange"); return 0; }
    std::uint32_t RandomUint32(std::uint32_t bound) override { Seam("ICrimeHost::RandomUint32"); return {}; }
};
Probe13 g_ICrimeHost;
}

namespace sc4 {
class Probe14 final : public IDemandHost {
public:
    void PostMessage(std::uint32_t message, std::uint32_t data1, std::int32_t data2) override { Seam("IDemandHost::PostMessage");}
};
Probe14 g_IDemandHost;
}

namespace sc4 {
class Probe15 final : public IWealthHost {
public:
    bool HasLot(int x, int z) override { Seam("IWealthHost::HasLot"); return false; }
    int LotWealth(int x, int z) override { Seam("IWealthHost::LotWealth"); return {}; }
    int CityCellWealth(int x, int z) override { Seam("IWealthHost::CityCellWealth"); return {}; }
    unsigned ZoneType(int x, int z) override { Seam("IWealthHost::ZoneType"); return {}; }
    int PropWealth(std::uint32_t propId) override { Seam("IWealthHost::PropWealth"); return {}; }
};
Probe15 g_IWealthHost;
}

namespace sc4 {
class Probe16 final : public IClockHost {
public:
    void PostMessage(std::uint32_t message, std::uint32_t data1, std::uint32_t data2) override { Seam("IClockHost::PostMessage");}
};
Probe16 g_IClockHost;
}

namespace sc4 {
class Probe17 final : public IBaseLotDeveloperHost {
public:
    float VertexHeight(int vertexIndex) override { Seam("IBaseLotDeveloperHost::VertexHeight"); return 0; }
    float VertexWaterHeight(int vertexIndex) override { Seam("IBaseLotDeveloperHost::VertexWaterHeight"); return 0; }
    float HeightAt(float x, float z) override { Seam("IBaseLotDeveloperHost::HeightAt"); return 0; }
    float WaterHeightAt(float x, float z) override { Seam("IBaseLotDeveloperHost::WaterHeightAt"); return 0; }
    bool VertexIsLevelled(int vx, int vz) override { Seam("IBaseLotDeveloperHost::VertexIsLevelled"); return false; }
    int VerticesPerRow() override { Seam("IBaseLotDeveloperHost::VerticesPerRow"); return {}; }
    float CityWidthMetres() override { Seam("IBaseLotDeveloperHost::CityWidthMetres"); return 0; }
    float CityDepthMetres() override { Seam("IBaseLotDeveloperHost::CityDepthMetres"); return 0; }
    int LevelVertices(float height, bool a, bool b, int x0, int z0, int x1, int z1, void* lot, int mode,
                              int extra) override { return {}; }
    void CellOfPosition(float x, float z, int& cellX, int& cellZ) override { Seam("IBaseLotDeveloperHost::CellOfPosition");}
    void OccupantBounds(void* occupant, float lo[3], float hi[3]) override { Seam("IBaseLotDeveloperHost::OccupantBounds");}
};
Probe17 g_IBaseLotDeveloperHost;
}

namespace sc4 {
class Probe18 final : public INeighborsHost {
public:
    cIGZMessageServer2* MessageServer() override { Seam("INeighborsHost::MessageServer"); return nullptr; }
    cISC4City* City() override { Seam("INeighborsHost::City"); return nullptr; }
    cISC4RegionalCity* CurrentRegionalCity() override { Seam("INeighborsHost::CurrentRegionalCity"); return nullptr; }
    cISCResExemplar* TuningExemplar() override { Seam("INeighborsHost::TuningExemplar"); return nullptr; }
    void AddSimulatorAgent(cIGZMessageTarget2* agent, int interval, const char* name) override { Seam("INeighborsHost::AddSimulatorAgent");}
    void RemoveSimulatorAgent(cIGZMessageTarget2* agent) override { Seam("INeighborsHost::RemoveSimulatorAgent");}
    void SimDate(cIGZDate& out) override { Seam("INeighborsHost::SimDate");}
    void PostMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2,
                             std::uint32_t data3, std::uint32_t data4, cIGZUnknown* unknown) override {}
    void PostDealStatusMessage(std::uint32_t status, cIGZUnknown* unknown) override { Seam("INeighborsHost::PostDealStatusMessage");}
    cISC4RegionalCity* RegionCityAt(int x, int z) override { Seam("INeighborsHost::RegionCityAt"); return nullptr; }
    void ReloadCityAt(int x, int z) override { Seam("INeighborsHost::ReloadCityAt");}
    int CityCellCountX() override { Seam("INeighborsHost::CityCellCountX"); return {}; }
    int CityCellCountZ() override { Seam("INeighborsHost::CityCellCountZ"); return {}; }
    void CityCellCenterToPosition(int x, int z, float& outX, float& outZ) override { Seam("INeighborsHost::CityCellCenterToPosition");}
    void CityPositionToCell(float x, float z, int& outX, int& outZ) override { Seam("INeighborsHost::CityPositionToCell");}
    float TerrainAltitudeAtCell(int x, int z) override { Seam("INeighborsHost::TerrainAltitudeAtCell"); return 0; }
    float TerrainAltitudeAtPosition(float x, float z) override { Seam("INeighborsHost::TerrainAltitudeAtPosition"); return 0; }
    void RecordHistory(std::uint32_t series, int value) override { Seam("INeighborsHost::RecordHistory");}
    double BudgetTotal() override { Seam("INeighborsHost::BudgetTotal"); return 0; }
    double BudgetIncome() override { Seam("INeighborsHost::BudgetIncome"); return 0; }
    bool BudgetLineItemAmount(std::uint32_t lineId, double& out) override { Seam("INeighborsHost::BudgetLineItemAmount"); return false; }
    bool BudgetSubLineItemAmount(std::uint32_t lineId, std::uint32_t subId, double& out) override { Seam("INeighborsHost::BudgetSubLineItemAmount"); return false; }
    int PowerCapacity() override { Seam("INeighborsHost::PowerCapacity"); return {}; }
    int PowerDemand() override { Seam("INeighborsHost::PowerDemand"); return {}; }
    int WaterCapacity() override { Seam("INeighborsHost::WaterCapacity"); return {}; }
    int WaterDemand() override { Seam("INeighborsHost::WaterDemand"); return {}; }
    void LandfillRange(int& start, int& end) override { Seam("INeighborsHost::LandfillRange");}
    int PollutionValue(int which) override { Seam("INeighborsHost::PollutionValue"); return {}; }
    void OccupantsInRect(int x0, int z0, int x1, int z1, std::vector<cISC4Occupant*>& out) override { Seam("INeighborsHost::OccupantsInRect");}
    void DeleteProp(cISC4Occupant* occupant) override { Seam("INeighborsHost::DeleteProp");}
    bool CreateProp(std::uint32_t propType, void** out) override { Seam("INeighborsHost::CreateProp"); return false; }
    void FinishProp(void* prop) override { Seam("INeighborsHost::FinishProp");}
    void SetPropPosition(void* prop, const float xyz[3]) override { Seam("INeighborsHost::SetPropPosition");}
    void SetPropEdgeFlags(void* prop, std::uint32_t flags) override { Seam("INeighborsHost::SetPropEdgeFlags");}
    void AddPropConnectionProperties(void* prop, std::uint32_t connType) override { Seam("INeighborsHost::AddPropConnectionProperties");}
    void ReleaseProp(void* prop) override { Seam("INeighborsHost::ReleaseProp");}
    void ReleaseOccupantList(std::vector<cISC4Occupant*>& list) override { Seam("INeighborsHost::ReleaseOccupantList");}
    void DeactivateOccupant(cISC4Occupant* occupant) override { Seam("INeighborsHost::DeactivateOccupant");}
    const cISCPropertyHolder* OccupantProperties(cISC4Occupant* occupant) override { Seam("INeighborsHost::OccupantProperties"); return nullptr; }
    void OccupantPosition(cISC4Occupant* occupant, float& x, float& z) override { Seam("INeighborsHost::OccupantPosition");}
    void SetOccupantPosition(cISC4Occupant* occupant, const float xyz[3]) override { Seam("INeighborsHost::SetOccupantPosition");}
    std::uint32_t OccupantType(cISC4Occupant* occupant) override { Seam("INeighborsHost::OccupantType"); return {}; }
    bool OccupantHasNetworkType(cISC4Occupant* occupant, std::uint32_t networkType) override { Seam("INeighborsHost::OccupantHasNetworkType"); return false; }
    bool OccupantRetained(cISC4Occupant* occupant) override { Seam("INeighborsHost::OccupantRetained"); return false; }
    void CityPosition(const cISC4RegionalCity* city, int& x, int& z) override { Seam("INeighborsHost::CityPosition");}
    void CitySize(const cISC4RegionalCity* city, int& width, int& height) override { Seam("INeighborsHost::CitySize");}
    bool CityEstablished(const cISC4RegionalCity* city) override { Seam("INeighborsHost::CityEstablished"); return false; }
    void CityName(const cISC4RegionalCity* city, cIGZString& out) override { Seam("INeighborsHost::CityName");}
    double CityGetBudget(const cISC4RegionalCity* city) override { Seam("INeighborsHost::CityGetBudget"); return 0; }
    void CitySetBudget(cISC4RegionalCity* city, double value) override { Seam("INeighborsHost::CitySetBudget");}
    void CitySetIncome(cISC4RegionalCity* city, double value) override { Seam("INeighborsHost::CitySetIncome");}
    cISC4NeighborConnection* CityFindConnection(const cISC4RegionalCity* city, int type, int x, int z) override { Seam("INeighborsHost::CityFindConnection"); return nullptr; }
    void CityAllConnections(const cISC4RegionalCity* city, ConnectionList& out) override { Seam("INeighborsHost::CityAllConnections");}
    void CityChangeSymmetricConnection(cISC4RegionalCity* city, cISC4NeighborConnection* connection, bool add) override { Seam("INeighborsHost::CityChangeSymmetricConnection");}
    void CityUpdateLocalDeals(cISC4RegionalCity* city) override { Seam("INeighborsHost::CityUpdateLocalDeals");}
    void CitySetLocalDeals(cISC4RegionalCity* city, const DealList& deals) override { Seam("INeighborsHost::CitySetLocalDeals");}
    void CityGetLocalDeals(cISC4RegionalCity* city, DealList& out) override { Seam("INeighborsHost::CityGetLocalDeals");}
    void CityUpdateImportExport(cISC4RegionalCity* city) override { Seam("INeighborsHost::CityUpdateImportExport");}
    double CityGetExported(const cISC4RegionalCity* city, int commodity) override { Seam("INeighborsHost::CityGetExported"); return 0; }
    void CitySetExported(cISC4RegionalCity* city, int commodity, double value) override { Seam("INeighborsHost::CitySetExported");}
    double CityGetImported(const cISC4RegionalCity* city, int commodity) override { Seam("INeighborsHost::CityGetImported"); return 0; }
    void CitySetImported(cISC4RegionalCity* city, int commodity, double value) override { Seam("INeighborsHost::CitySetImported");}
    double CityGetProduced(const cISC4RegionalCity* city, int commodity) override { Seam("INeighborsHost::CityGetProduced"); return 0; }
    void CitySetProduced(cISC4RegionalCity* city, int commodity, double value) override { Seam("INeighborsHost::CitySetProduced");}
    double CityGetDemanded(const cISC4RegionalCity* city, int commodity) override { Seam("INeighborsHost::CityGetDemanded"); return 0; }
    void CitySetDemanded(cISC4RegionalCity* city, int commodity, double value) override { Seam("INeighborsHost::CitySetDemanded");}
    double CityGetCostPerUnit(const cISC4RegionalCity* city, int commodity) override { Seam("INeighborsHost::CityGetCostPerUnit"); return 0; }
    void CitySetCostPerUnit(cISC4RegionalCity* city, int commodity, double value) override { Seam("INeighborsHost::CitySetCostPerUnit");}
    bool ConfirmationDialog(std::uint32_t titleId, std::uint32_t formatId, std::uint32_t okId,
                                    const cIGZString& arg) override { return false; }
};
Probe18 g_INeighborsHost;
}

namespace sc4 {
class Probe19 final : public ITerrainViewHost {
public:
};
Probe19 g_ITerrainViewHost;
}

namespace sc4 {
class Probe20 final : public IFlammabilityHost {
public:
    int CitySizeXInCells() override { Seam("IFlammabilityHost::CitySizeXInCells"); return {}; }
    int CitySizeZInCells() override { Seam("IFlammabilityHost::CitySizeZInCells"); return {}; }
    bool RandomDisastersBlocked() override { Seam("IFlammabilityHost::RandomDisastersBlocked"); return false; }
    bool PickRandomLotCell(int& x, int& z) override { Seam("IFlammabilityHost::PickRandomLotCell"); return false; }
    void StartFire(int x, int z, bool onRandomLot, int intensity) override { Seam("IFlammabilityHost::StartFire");}
    float GetOrdinanceFlameResistance() override { Seam("IFlammabilityHost::GetOrdinanceFlameResistance"); return 0; }
    int GetCurrentWeather() override { Seam("IFlammabilityHost::GetCurrentWeather"); return {}; }
    bool HasWaterAt(int x, int z) override { Seam("IFlammabilityHost::HasWaterAt"); return false; }
    int GetZoneTypeAt(int x, int z) override { Seam("IFlammabilityHost::GetZoneTypeAt"); return {}; }
    int GetLotZoneTypeOfOccupant(ISimOccupant* occupant) override { Seam("IFlammabilityHost::GetLotZoneTypeOfOccupant"); return {}; }
    std::vector<ISimOccupant*> GatherOccupants(const SC4Rect& rect, const std::uint32_t* types, int typeCount) override { Seam("IFlammabilityHost::GatherOccupants"); return {}; }
    cISCPropertyHolder* GetSimulatorTunables(const cGZPersistResourceKey& key) override { Seam("IFlammabilityHost::GetSimulatorTunables"); return nullptr; }
    void PostFlammabilityMessage() override { Seam("IFlammabilityHost::PostFlammabilityMessage");}
    void AddHistoryValue(std::uint32_t historyId, std::uint8_t value) override { Seam("IFlammabilityHost::AddHistoryValue");}
    void RegisterSimulator(const char* name) override { Seam("IFlammabilityHost::RegisterSimulator");}
    void UnregisterSimulator() override { Seam("IFlammabilityHost::UnregisterSimulator");}
};
Probe20 g_IFlammabilityHost;
}

namespace sc4 {
class Probe21 final : public ILandValueHost {
public:
    void AddIdleClient(cIGZMessageTarget2* target, int kind, const char* name, int priority) override { Seam("ILandValueHost::AddIdleClient");}
    void RemoveIdleClient(cIGZMessageTarget2* target) override { Seam("ILandValueHost::RemoveIdleClient");}
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { Seam("ILandValueHost::AddNotification");}
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { Seam("ILandValueHost::RemoveNotification");}
    void PostMessage(std::uint32_t message) override { Seam("ILandValueHost::PostMessage");}
    bool AcquireServices() override { Seam("ILandValueHost::AcquireServices"); return false; }
    const cISCPropertyHolder* SimulatorExemplar() override { Seam("ILandValueHost::SimulatorExemplar"); return nullptr; }
    void ReleaseServices() override { Seam("ILandValueHost::ReleaseServices");}
    ILandValueTerrain* Terrain() override { Seam("ILandValueHost::Terrain"); return nullptr; }
    int CellZoneType(int x, int z) override { Seam("ILandValueHost::CellZoneType"); return {}; }
    void CreateHistory(std::uint32_t historyId, int kind) override { Seam("ILandValueHost::CreateHistory");}
    void AddHistory(std::uint32_t historyId, std::int64_t value) override { Seam("ILandValueHost::AddHistory");}
    const void* GrowthDeveloperForType(std::uint32_t developerType) override { Seam("ILandValueHost::GrowthDeveloperForType"); return nullptr; }
    cSC4SimGrid<std::int16_t>* DesirabilityMap(const void* developer) override { Seam("ILandValueHost::DesirabilityMap"); return {}; }
    float AverageSlope(int x0, int z0, int x1, int z1) override { Seam("ILandValueHost::AverageSlope"); return 0; }
    void AddDistanceEffect(std::vector<std::vector<std::uint8_t>>& grid, int countX, int countZ,
                                   float inner, float outer) override {}
    std::uint64_t BeginSlice(std::uint32_t milliseconds) override { Seam("ILandValueHost::BeginSlice"); return {}; }
    bool SliceExpired(std::uint64_t slice) override { Seam("ILandValueHost::SliceExpired"); return false; }
};
Probe21 g_ILandValueHost;
}

// The probe creators (city_probe_creators.cpp) construct the host-required city classes against these same
// stub hosts; the concrete Probe* types stay private to this file, so the creators reach them through the
// interface-typed getters declared in tools/city_probe_hosts.h.
namespace sc4 {
IDemolitionHost* CityProbeDemolitionHost() { return &g_IDemolitionHost; }
ICrimeHost* CityProbeCrimeHost() { return &g_ICrimeHost; }
IWealthHost* CityProbeWealthHost() { return &g_IWealthHost; }
INeighborsHost* CityProbeNeighborsHost() { return &g_INeighborsHost; }
IWeatherHost* CityProbeWeatherHost() { return &g_IWeatherHost; }
ICommercialHost* CityProbeCommercialHost() { return &g_ICommercialHost; }
ILandValueHost* CityProbeLandValueHost() { return &g_ILandValueHost; }
IDemandHost* CityProbeDemandHost() { return &g_IDemandHost; }
IClockHost* CityProbeClockHost() { return &g_IClockHost; }
}  // namespace sc4

void InstallCityProbeBindings() {
    sc4::SetDispatchHost(&sc4::g_probe0);
    // The GZCOM creators hand these to the simulators they construct; without them every Read() that touches its
    // host dereferences null.
    gzcom::SimHosts& hosts = gzcom::GetSimHosts();
    hosts.power = &sc4::g_IPowerHost;
    hosts.plumbing = &sc4::g_IPlumbingHost;
    hosts.fire = &sc4::g_IFireHost;
    hosts.police = &sc4::g_IPoliceHost;
    hosts.traffic = &sc4::g_ITrafficHost;
    hosts.pollution = &sc4::g_IPollutionHost;
    hosts.flammability = &sc4::g_IFlammabilityHost;
    hosts.landValue = &sc4::g_ILandValueHost;
    hosts.wealth = &sc4::g_IWealthHost;
    hosts.zoneManager = &sc4::g_IZoneManagerHost;
    hosts.commercial = &sc4::g_ICommercialHost;
    hosts.residential = &sc4::g_IResidentialHost;
    hosts.lotDeveloper = &sc4::g_IBaseLotDeveloperHost;
    hosts.terrainView = &sc4::g_ITerrainViewHost;
}
