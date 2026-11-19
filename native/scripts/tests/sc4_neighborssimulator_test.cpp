// cSC4NeighborsSimulator — tests the recovered regional coupling against fake cities/engine services:
//   * construction defaults and COM plumbing (three views, AddRef/Release);
//   * Init (exemplar response curve, simulator agents, message subscriptions, city/message-server references,
//     connection repair, adjacent-city reloads, local-deal cloning and re-dating) and Shutdown;
//   * the connection bookkeeping and the cell geometry conversions (including the range-check quirk);
//   * the record framing of Write/Read (version 2, count) and the version gate;
//   * DoMessage: established, terrain change (occupant re-placement), day/idle flush, month deal pass, reset,
//     occupant-removed, the re-entrancy guard;
//   * the deal pass: propose -> activate, cancelled removal, expiry -> renewal, frozen removal, lock timing;
//   * deal queries/pricing (FindDeals*, monthly income/expense, GetDealCost/GetDealMaxAmount) and
//     UpdateCommodityStats (budget/income, power/water/garbage statistics, history series).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "gz/cGZDate.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZString.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/SCPropertyHelp.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

using sc4::cISC4NeighborConnection;
using sc4::cISC4Occupant;
using sc4::cISCPropertyHolder;
using sc4::cSC4NeighborDeal;
using sc4::cSCPropertyHolder;
using sc4::cSCResExemplar;
using sc4::cISCResExemplar;
using sc4::ConnectionList;
using sc4::DealList;

//---------------------------------------------------------------------------------------------------------------------
// fakes
//---------------------------------------------------------------------------------------------------------------------

struct FakeRegionalCity final : cISC4RegionalCity {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4RegionalCity) {
            *out = this;
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) {
            delete this;
            return 0;
        }
        return refs;
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetupCity(cISC4City*) override {}
    std::uint32_t GetCitySerialNumber() const override { return serial; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString(""); }

    int refs = 1;
    int x = 0, z = 0;
    int width = 1, height = 1;
    bool established = true;
    double budget = 0.0;
    double income = 0.0;
    double exported[4] = {0, 0, 0, 0};
    double imported[4] = {0, 0, 0, 0};
    double produced[4] = {0, 0, 0, 0};
    double demanded[4] = {0, 0, 0, 0};
    double costPerUnit[4] = {0, 0, 0, 0};
    std::uint32_t serial = 0;
    std::string name;
    ConnectionList connections;
    DealList localDeals;
};

struct FakeCity final : cISC4City {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cIGZUnknown) {
            *out = this;
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) {
            delete this;
            return 0;
        }
        return refs;
    }
    bool Init() override { return true; }
    bool Shutdown() override { return true; }
    void SetCitySerialNumber(std::uint32_t) override {}
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString(""); }
    void SetCitySaveFilePath(const cIGZString&) override {}
    void SetCityNameChanged(bool) override {}
    bool IsSaveDisabled() const override { return false; }

    int refs = 1;
};

struct FakeMessageServer final : cIGZMessageServer2 {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == 1 || iid == kGZIID_cIGZUnknown || iid == kGZIID_cIGZMessageServer2) {
            *out = this;
            AddRef();
            return true;
        }
        return false;
    }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override {
        if (--refs == 0) {
            delete this;
            return 0;
        }
        return refs;
    }
    bool MessageSend(cIGZMessage2*) override { return true; }
    bool MessagePost(cIGZMessage2* message, bool) override {
        posted.push_back(message ? message->GetType() : 0);
        return true;
    }
    bool AddNotification(cIGZMessageTarget2*, std::uint32_t id) override {
        ++notifications[id];
        return true;
    }
    bool RemoveNotification(cIGZMessageTarget2*, std::uint32_t id) override {
        --notifications[id];
        return true;
    }
    bool GeneralMessagePostToTarget(cIGZMessage2*, cIGZMessageTarget2*) override { return true; }
    bool CancelGeneralMessagePostsToTarget(cIGZMessageTarget2*) override { return true; }
    bool OnTick(std::uint32_t) override { return true; }
    std::uint32_t GetMessageQueueSize() override { return 0; }
    void SetAlwaysClearQueueOnTick(bool) override {}
    std::uint32_t GetRefCount() const override { return static_cast<std::uint32_t>(refs); }
    bool CreateMessage(std::uint32_t, std::uint32_t, void** out) override {
        if (out) *out = nullptr;
        return false;
    }

    int refs = 1;
    std::map<std::uint32_t, int> notifications;
    std::vector<std::uint32_t> posted;
};

struct FakeProp {
    float position[3] = {0, 0, 0};
    std::uint32_t flags = 0;
    std::vector<std::uint32_t> properties;
};

struct FakeOccupant {
    static cISC4Occupant* Make(std::uintptr_t id) { return reinterpret_cast<cISC4Occupant*>(id); }
};

struct TestHost final : sc4::INeighborsHost {
    // --- framework services ---
    cIGZMessageServer2* MessageServer() override { return messages; }
    cISC4City* City() override { return city; }
    cISC4RegionalCity* CurrentRegionalCity() override { return current; }
    cISCResExemplar* TuningExemplar() override {
        if (!exemplar) return nullptr;
        exemplar->AddRef();
        return exemplar;
    }
    void AddSimulatorAgent(cIGZMessageTarget2*, int interval, const char* name) override {
        agentIntervals.push_back(interval);
        agentName = name ? name : "";
    }
    void RemoveSimulatorAgent(cIGZMessageTarget2*) override { ++agentRemovals; }
    void SimDate(cIGZDate& out) override { out.Set(simDate.DayNumber()); }
    void PostMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3,
                     std::uint32_t, cIGZUnknown*) override {
        posts.push_back({type, data1, data2, data3});
    }
    void PostDealStatusMessage(std::uint32_t status, cIGZUnknown*) override {
        dealStatuses.push_back(status);
        if (onDealStatus) onDealStatus();
    }
    cISC4RegionalCity* RegionCityAt(int x, int z) override {
        auto it = region.find({x, z});
        return it == region.end() ? nullptr : it->second;
    }
    void ReloadCityAt(int x, int z) override { reloads.push_back({x, z}); }

    // --- the current city ---
    int CityCellCountX() override { return cellsX; }
    int CityCellCountZ() override { return cellsZ; }
    void CityCellCenterToPosition(int x, int z, float& outX, float& outZ) override {
        outX = static_cast<float>(x) + 0.5f;
        outZ = static_cast<float>(z) + 0.5f;
    }
    void CityPositionToCell(float x, float z, int& outX, int& outZ) override {
        outX = static_cast<int>(x);
        outZ = static_cast<int>(z);
    }
    float TerrainAltitudeAtCell(int x, int z) override {
        auto it = altitudeCells.find({x, z});
        return it == altitudeCells.end() ? 10.0f : it->second;
    }
    float TerrainAltitudeAtPosition(float, float) override { return terrainAltitude; }
    void RecordHistory(std::uint32_t series, int value) override { history.push_back({series, value}); }
    double BudgetTotal() override { return budgetTotal; }
    double BudgetIncome() override { return budgetIncome; }
    bool BudgetLineItemAmount(std::uint32_t lineId, double& out) override {
        auto it = budgetLines.find(lineId);
        if (it == budgetLines.end()) return false;
        out = it->second;
        return true;
    }
    bool BudgetSubLineItemAmount(std::uint32_t lineId, std::uint32_t subId, double& out) override {
        auto it = budgetSubLines.find({lineId, subId});
        if (it == budgetSubLines.end()) return false;
        out = it->second;
        return true;
    }
    int PowerCapacity() override { return powerCapacity; }
    int PowerDemand() override { return powerDemand; }
    int WaterCapacity() override { return waterCapacity; }
    int WaterDemand() override { return waterDemand; }
    void LandfillRange(int& start, int& end) override { start = landfillStart; end = landfillEnd; }
    int PollutionValue(int which) override { return pollution[which]; }

    // --- occupants / props ---
    void OccupantsInRect(int x0, int z0, int x1, int z1, std::vector<cISC4Occupant*>& out) override {
        for (const auto& entry : occupantsByCell) {
            const auto& cell = entry.first;
            if (cell.first >= x0 && cell.second >= z0 && cell.first <= x1 && cell.second <= z1) {
                out.push_back(entry.second);
            }
        }
    }
    void DeleteProp(cISC4Occupant* occupant) override { deletedProps.push_back(occupant); }
    bool CreateProp(std::uint32_t propType, void** out) override {
        if (propType != sc4::kConnectionOccupantPropType) return false;
        ++createdProps;
        if (!createPropSucceeds) return false;
        auto* prop = new FakeProp();
        props.push_back(prop);
        *out = prop;
        return true;
    }
    void FinishProp(void* prop) override { finishedProps.push_back(prop); }
    void SetPropPosition(void* prop, const float xyz[3]) override {
        auto* fake = static_cast<FakeProp*>(prop);
        for (int i = 0; i < 3; ++i) fake->position[i] = xyz[i];
    }
    void SetPropEdgeFlags(void* prop, std::uint32_t flags) override {
        static_cast<FakeProp*>(prop)->flags = flags;
    }
    void AddPropConnectionProperties(void* prop, std::uint32_t) override {
        static_cast<FakeProp*>(prop)->properties.push_back(0xcaa46595);
    }
    void ReleaseProp(void* prop) override { releasedProps.push_back(prop); }
    void ReleaseOccupantList(std::vector<cISC4Occupant*>& list) override { list.clear(); }
    void DeactivateOccupant(cISC4Occupant* occupant) override { deactivated.push_back(occupant); }
    const cISCPropertyHolder* OccupantProperties(cISC4Occupant* occupant) override {
        auto it = occupantHolders.find(occupant);
        return it == occupantHolders.end() ? nullptr : it->second.get();
    }
    void OccupantPosition(cISC4Occupant* occupant, float& x, float& z) override {
        auto it = occupantPositions.find(occupant);
        x = it == occupantPositions.end() ? 0.0f : it->second.first;
        z = it == occupantPositions.end() ? 0.0f : it->second.second;
    }
    void SetOccupantPosition(cISC4Occupant* occupant, const float xyz[3]) override {
        occupantPositions[occupant] = {xyz[0], xyz[2]};
        occupantAltitude[occupant] = xyz[1];
    }
    std::uint32_t OccupantType(cISC4Occupant* occupant) override {
        auto it = occupantTypes.find(occupant);
        return it == occupantTypes.end() ? 0 : it->second;
    }
    bool OccupantHasNetworkType(cISC4Occupant* occupant, std::uint32_t networkType) override {
        return occupantNetworkTypes.count({occupant, networkType}) != 0;
    }
    bool OccupantRetained(cISC4Occupant*) override { return occupantRetained; }

    // --- regional cities ---
    void CityPosition(const cISC4RegionalCity* cityPtr, int& x, int& z) override {
        const auto* fake = static_cast<const FakeRegionalCity*>(cityPtr);
        x = fake->x;
        z = fake->z;
    }
    void CitySize(const cISC4RegionalCity* cityPtr, int& width, int& height) override {
        const auto* fake = static_cast<const FakeRegionalCity*>(cityPtr);
        width = fake->width;
        height = fake->height;
    }
    bool CityEstablished(const cISC4RegionalCity* cityPtr) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->established;
    }
    void CityName(const cISC4RegionalCity* cityPtr, cIGZString& out) override {
        out.AssignFromCString(static_cast<const FakeRegionalCity*>(cityPtr)->name.c_str());
    }
    double CityGetBudget(const cISC4RegionalCity* cityPtr) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->budget;
    }
    void CitySetBudget(cISC4RegionalCity* cityPtr, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->budget = value;
    }
    void CitySetIncome(cISC4RegionalCity* cityPtr, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->income = value;
    }
    cISC4NeighborConnection* CityFindConnection(const cISC4RegionalCity* cityPtr, int type, int x, int z) override {
        for (const auto& ref : static_cast<const FakeRegionalCity*>(cityPtr)->connections) {
            cISC4NeighborConnection* connection = ref.get();
            if (!connection || connection->GetType() != static_cast<std::uint32_t>(type)) continue;
            int cellX = 0, cellZ = 0;
            connection->GetConnectionCell(cellX, cellZ);
            if (cellX == x && cellZ == z) return connection;
        }
        return nullptr;
    }
    void CityAllConnections(const cISC4RegionalCity* cityPtr, ConnectionList& out) override {
        out = static_cast<const FakeRegionalCity*>(cityPtr)->connections;
    }
    void CityChangeSymmetricConnection(cISC4RegionalCity* cityPtr, cISC4NeighborConnection* connection, bool add) override {
        symmetricChanges.push_back({static_cast<FakeRegionalCity*>(cityPtr), connection, add});
    }
    void CityUpdateLocalDeals(cISC4RegionalCity* cityPtr) override {
        localDealUpdates.push_back(static_cast<FakeRegionalCity*>(cityPtr));
    }
    void CitySetLocalDeals(cISC4RegionalCity* cityPtr, const DealList& deals) override {
        lastSetLocalDeals[static_cast<FakeRegionalCity*>(cityPtr)] = deals.size();
        ++setLocalDealsCalls;
    }
    void CityGetLocalDeals(cISC4RegionalCity* cityPtr, DealList& out) override {
        out = static_cast<FakeRegionalCity*>(cityPtr)->localDeals;
    }
    void CityUpdateImportExport(cISC4RegionalCity* cityPtr) override {
        importExportUpdates.push_back(static_cast<FakeRegionalCity*>(cityPtr));
    }
    double CityGetExported(const cISC4RegionalCity* cityPtr, int commodity) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->exported[commodity];
    }
    void CitySetExported(cISC4RegionalCity* cityPtr, int commodity, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->exported[commodity] = value;
    }
    double CityGetImported(const cISC4RegionalCity* cityPtr, int commodity) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->imported[commodity];
    }
    void CitySetImported(cISC4RegionalCity* cityPtr, int commodity, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->imported[commodity] = value;
    }
    double CityGetProduced(const cISC4RegionalCity* cityPtr, int commodity) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->produced[commodity];
    }
    void CitySetProduced(cISC4RegionalCity* cityPtr, int commodity, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->produced[commodity] = value;
    }
    double CityGetDemanded(const cISC4RegionalCity* cityPtr, int commodity) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->demanded[commodity];
    }
    void CitySetDemanded(cISC4RegionalCity* cityPtr, int commodity, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->demanded[commodity] = value;
    }
    double CityGetCostPerUnit(const cISC4RegionalCity* cityPtr, int commodity) override {
        return static_cast<const FakeRegionalCity*>(cityPtr)->costPerUnit[commodity];
    }
    void CitySetCostPerUnit(cISC4RegionalCity* cityPtr, int commodity, double value) override {
        static_cast<FakeRegionalCity*>(cityPtr)->costPerUnit[commodity] = value;
    }
    bool ConfirmationDialog(std::uint32_t titleId, std::uint32_t textId, std::uint32_t okId,
                            const cIGZString& arg) override {
        dialogTitle = titleId;
        dialogText = textId;
        dialogOk = okId;
        dialogArg = arg.ToChar();
        return confirmResult;
    }

    struct Post {
        std::uint32_t type, data1, data2, data3;
    };
    struct SymmetricChange {
        FakeRegionalCity* city;
        cISC4NeighborConnection* connection;
        bool add;
    };

    FakeCity* city = nullptr;
    FakeRegionalCity* current = nullptr;
    FakeMessageServer* messages = nullptr;
    cSCResExemplar* exemplar = nullptr;
    std::map<std::pair<int, int>, FakeRegionalCity*> region;
    int cellsX = 64, cellsZ = 64;
    cGZDate simDate{1, 1, 2000};
    std::vector<int> agentIntervals;
    std::string agentName;
    int agentRemovals = 0;
    std::vector<Post> posts;
    std::vector<std::uint32_t> dealStatuses;
    std::function<void()> onDealStatus;
    std::vector<std::pair<int, int>> reloads;
    std::vector<std::pair<std::uint32_t, int>> history;
    double budgetTotal = 5000.0, budgetIncome = 1000.0;
    std::map<std::uint32_t, double> budgetLines;
    std::map<std::pair<std::uint32_t, std::uint32_t>, double> budgetSubLines;
    int powerCapacity = 100, powerDemand = 80, waterCapacity = 50, waterDemand = 40;
    int landfillStart = 0, landfillEnd = 10;
    int pollution[5] = {1, 2, 3, 4, 5};
    float terrainAltitude = 10.0f;
    std::map<std::pair<int, int>, float> altitudeCells;
    std::map<std::pair<int, int>, cISC4Occupant*> occupantsByCell;
    std::map<cISC4Occupant*, std::unique_ptr<cSCPropertyHolder>> occupantHolders;
    std::map<cISC4Occupant*, std::pair<float, float>> occupantPositions;
    std::map<cISC4Occupant*, float> occupantAltitude;
    std::map<cISC4Occupant*, std::uint32_t> occupantTypes;
    std::set<std::pair<cISC4Occupant*, std::uint32_t>> occupantNetworkTypes;
    std::vector<cISC4Occupant*> deletedProps;
    std::vector<cISC4Occupant*> deactivated;
    std::vector<FakeProp*> props;
    ~TestHost() { for (FakeProp* p : props) delete p; }
    std::vector<void*> finishedProps, releasedProps;
    int createdProps = 0;
    bool createPropSucceeds = true;
    bool occupantRetained = false;
    std::vector<SymmetricChange> symmetricChanges;
    std::vector<FakeRegionalCity*> localDealUpdates;
    std::vector<FakeRegionalCity*> importExportUpdates;
    std::map<FakeRegionalCity*, std::size_t> lastSetLocalDeals;
    int setLocalDealsCalls = 0;
    bool confirmResult = false;
    std::uint32_t dialogTitle = 0, dialogText = 0, dialogOk = 0;
    std::string dialogArg;

    bool hasSuitableConnection = true;
    double dealCostFactor = 1.0;
};

// The neighbour-connection seam: the geometry comes from the recovered static conversions.
struct ConnHost final : sc4::INeighborConnectionHost {
    TestHost* host = nullptr;
    int CityCellsX() override { return host->cellsX; }
    int CityCellsZ() override { return host->cellsZ; }
    bool CityEntryAtCell(int x, int z, void*& city) override {
        auto* found = host->RegionCityAt(x, z);
        city = found;
        return found != nullptr;
    }
    void ConvertConnectionCell(int x, int z, int& outX, int& outZ, void*) override {
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(x, z, outX, outZ, nullptr);
    }
};

struct DealHost final : sc4::INeighborDealHost {
    TestHost* host = nullptr;
    bool CityHasBudget() override { return true; }
    double DealCost(double amount, std::uint32_t type, void*) override {
        const double factor = (type == sc4::kDealTypePowerBuy || type == sc4::kDealTypePowerSell)   ? 2.0
                              : (type == sc4::kDealTypeWaterBuy || type == sc4::kDealTypeWaterSell) ? 3.0
                                                                                                   : 4.0;
        return amount * factor * host->dealCostFactor;
    }
    bool CityEntryAtCell(int x, int z, void*& city) override {
        auto* found = host->RegionCityAt(x, z);
        city = found;
        return found != nullptr;
    }
    void* CurrentCity() override { return host->current; }
    double CurrentCityFunds() override { return 100000.0; }
    void ReportDealStatus(bool, cSC4NeighborDeal*) override {}
    double CurrentRegionalCityBudget() override { return host->current ? host->current->budget : 0.0; }
    bool HasSuitableConnection(std::uint32_t, void*) override { return host->hasSuitableConnection; }
};

//---------------------------------------------------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------------------------------------------------

cRZAutoRefCount<sc4::cISC4NeighborDeal> MakeDeal(std::uint32_t type, int cityX, int cityZ, int otherX, int otherZ,
                                                 int amount) {
    auto* deal = new sc4::cSC4NeighborDeal();
    deal->AddRef();   // the original constructor leaves the count at 1
    deal->SetType(type);
    deal->SetRegionCellOnCitySideOfConnection(cityX, cityZ);
    deal->SetRegionCellOnOtherSideOfConnection(otherX, otherZ);
    deal->SetAmount(amount);
    cRZAutoRefCount<sc4::cISC4NeighborDeal> owned(deal);
    deal->Release();   // drop the constructor's count; the smart pointer holds the only reference
    return owned;
}

std::unique_ptr<cRZMessage2Standard> MakeMessage(std::uint32_t type, std::uint64_t data1 = 0,
                                                 std::uint64_t data2 = 0) {
    auto message = std::make_unique<cRZMessage2Standard>();
    message->SetType(type);
    message->SetData1(data1);
    message->SetData2(data2);
    return message;
}

struct Fixture {
    std::unique_ptr<FakeCity> city{new FakeCity()};
    std::unique_ptr<FakeMessageServer> messages{new FakeMessageServer()};
    std::unique_ptr<cSCResExemplar> exemplar{new cSCResExemplar()};
    std::unique_ptr<FakeRegionalCity> current{new FakeRegionalCity()};
    TestHost host;
    ConnHost connHost;
    DealHost dealHost;
    std::vector<std::unique_ptr<FakeRegionalCity>> cities;

    Fixture() {
        exemplar->AddRef();
        host.city = city.get();
        host.current = current.get();
        host.messages = messages.get();
        host.exemplar = exemplar.get();
        host.region[{current->x, current->z}] = current.get();
        connHost.host = &host;
        dealHost.host = &host;
        // Default exemplar: a one-year deal term (so the monthly conversion never divides by zero).
        sc4::cSCPropertyHelp::SetPropertyValue(exemplar->AsISCPropertyHolder(), sc4::kNeighborsPropertyDealYears,
                                               1.0f);
        sc4::SetNeighborsHost(&host);
        sc4::SetNeighborConnectionHost(&connHost);
        sc4::SetNeighborDealHost(&dealHost);
    }
    ~Fixture() {
        sc4::SetNeighborConnectionHost(nullptr);
        sc4::SetNeighborDealHost(nullptr);
        sc4::SetNeighborsHost(nullptr);
    }

    void PlaceCurrent(int x, int z, int width = 1, int height = 1) {
        current->x = x;
        current->z = z;
        current->width = width;
        current->height = height;
        host.region[{x, z}] = current.get();
    }
    FakeRegionalCity* AddCity(int x, int z, int width = 1, int height = 1) {
        auto item = std::make_unique<FakeRegionalCity>();
        FakeRegionalCity* raw = item.get();
        raw->x = x;
        raw->z = z;
        raw->width = width;
        raw->height = height;
        host.region[{x, z}] = raw;
        cities.push_back(std::move(item));
        return raw;
    }
};

}  // namespace

int main() {
    //-----------------------------------------------------------------------------------------------------------------
    // construction / COM
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        CHECK(sim->DealsDirty());
        CHECK(!sim->ConnectionsDirty());
        CHECK(!sim->UIDisabled());
        CHECK(!sim->Initialized());
        CHECK(!sim->CityEstablished_());
        CHECK(!sim->InsideSimulation());
        CHECK(sim->Connections().empty() && sim->Deals().empty());
        CHECK(sim->GetGZCLSID() == sc4::kGZCLSID_cSC4NeighborsSimulator);
        CHECK(!sc4::cSC4NeighborsSimulator::mbStaticInitialized);

        void* view = nullptr;
        CHECK(sim->QueryInterface(1, &view) && view == static_cast<sc4::cISC4NeighborsSimulator*>(sim));
        CHECK(sim->QueryInterface(sc4::kGZIID_cISC4NeighborsSimulator, &view) &&
              view == static_cast<sc4::cISC4NeighborsSimulator*>(sim));
        CHECK(sim->QueryInterface(0x452294aa, &view) && view == static_cast<cIGZMessageTarget2*>(sim));
        CHECK(sim->QueryInterface(0xe4fda3d4, &view) && view == static_cast<cIGZSerializable*>(sim));
        CHECK(!sim->QueryInterface(0x12345678, &view));
        // The four successful QIs added references; the last Release deletes the object.
        CHECK(sim->Release() == 3);
        CHECK(sim->Release() == 2);
        CHECK(sim->Release() == 1);
        CHECK(sim->Release() == 0);
        sc4::cSC4NeighborsSimulator::ResetStaticInitialized();
    }

    //-----------------------------------------------------------------------------------------------------------------
    // Init / Shutdown
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        FakeRegionalCity* north = f.AddCity(1, 0);
        north->established = true;
        FakeRegionalCity* east = f.AddCity(2, 1);
        east->established = true;

        std::vector<float> curve = {0.0f, 0.0f, 1.0f, 1.0f};
        sc4::cSCPropertyHelp::SetPropertyValue(f.exemplar->AsISCPropertyHolder(),
                                               sc4::kNeighborsPropertyResponseCurve, curve);

        // A local deal on the eastern city, seen from that city: its city side is (2,1), ours is (1,1).
        east->localDeals.push_back(MakeDeal(sc4::kDealTypePowerSell, 2, 1, 1, 1, 5));
        sc4::cISC4NeighborDeal* eastDeal = east->localDeals.front().get();
        eastDeal->SetStartDate(cGZDate(f.host.simDate.DayNumber() - 30));
        eastDeal->SetExpirationDate(cGZDate(f.host.simDate.DayNumber() + 30));
        eastDeal->SetCurrentDate(cGZDate(f.host.simDate.DayNumber() - 10));
        eastDeal->SetSuspensionDate(cGZDate(0));

        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        // A connection created before Init has its region cell recomputed by the repair pass.
        sim->CreateConnection(0, -1, 5, false);
        static_cast<sc4::cSC4NeighborConnection*>(sim->FindConnection(0, -1, 5))->Initialize(0, -1, 5, 99, 99);
        CHECK(sim->Init());
        int repairedX = 0, repairedZ = 0;
        sim->FindConnection(0, -1, 5)->GetRegionCellOnOtherSideOfConnection(repairedX, repairedZ);
        CHECK(repairedX == 0 && repairedZ == 1);
        CHECK(sim->Initialized());
        CHECK(sim->StaticInitializedFlag());
        CHECK(sim->Init());   // the second Init is a no-op that still returns true
        CHECK(f.host.agentIntervals == std::vector<int>({6, 4, 10}));
        CHECK(f.host.agentName == "cSC4NeighborsSimulator");
        CHECK(f.messages->notifications.size() == 7);
        for (const auto& entry : f.messages->notifications) CHECK(entry.second == 1);
        CHECK(f.city->refs == 2);        // Init keeps a city reference
        CHECK(f.messages->refs == 2);    // ... and a message-server reference
        CHECK(f.host.reloads.size() == 2);
        CHECK(std::fabs(sim->CurveValue(0.5f) - 0.5f) < 1e-6);

        CHECK(f.host.localDealUpdates.size() == 3);   // current + two adjacent
        CHECK(sim->Deals().size() == 1);
        sc4::cISC4NeighborDeal* clone = sim->Deals().front().get();
        CHECK(clone != nullptr);
        CHECK(!clone->CheckFlagsOr(sc4::kDealFlagSilent));
        CHECK(clone->GetAmount() == 5);
        CHECK(clone->GetType() == sc4::kDealTypePowerBuy);   // seen from our side: sell becomes buy
        cGZDate cloneStart, cloneExpiration, cloneSuspension;
        clone->GetStartDate(cloneStart);
        clone->GetExpirationDate(cloneExpiration);
        clone->GetSuspensionDate(cloneSuspension);
        CHECK(cloneStart.DayNumber() == f.host.simDate.DayNumber() - 20);
        CHECK(cloneExpiration.DayNumber() == f.host.simDate.DayNumber() + 40);
        CHECK(cloneSuspension.DayNumber() == 10);
        CHECK(f.host.setLocalDealsCalls >= 3);

        CHECK(sim->Shutdown());
        CHECK(!sim->Initialized());
        CHECK(f.city->refs == 1);
        CHECK(f.messages->refs == 1);
        CHECK(f.host.agentRemovals == 1);
        for (const auto& entry : f.messages->notifications) CHECK(entry.second == 0);
        CHECK(sim->Connections().empty() && sim->Deals().empty());
        CHECK(sim->Release() == 0);
        sc4::cSC4NeighborsSimulator::ResetStaticInitialized();
    }

    //-----------------------------------------------------------------------------------------------------------------
    // connection bookkeeping and cell geometry
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        FakeRegionalCity* west = f.AddCity(0, 1);
        FakeRegionalCity* east = f.AddCity(2, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);

        CHECK(sc4::cSC4NeighborsSimulator::IsValidConnectionCell(-1, 5, f.current.get()));
        CHECK(sc4::cSC4NeighborsSimulator::IsValidConnectionCell(64, 5, f.current.get()));
        CHECK(sc4::cSC4NeighborsSimulator::IsValidConnectionCell(5, -1, f.current.get()));
        CHECK(sc4::cSC4NeighborsSimulator::IsValidConnectionCell(5, 64, f.current.get()));
        CHECK(!sc4::cSC4NeighborsSimulator::IsValidConnectionCell(-1, -1, f.current.get()));
        CHECK(!sc4::cSC4NeighborsSimulator::IsValidConnectionCell(64, 64, f.current.get()));
        CHECK(!sc4::cSC4NeighborsSimulator::IsValidConnectionCell(0, 0, f.current.get()));
        // Quirk: the coordinate that is not on an edge is not range-checked.
        CHECK(sc4::cSC4NeighborsSimulator::IsValidConnectionCell(-1, 1000, f.current.get()));
        CHECK(sc4::cSC4NeighborsSimulator::IsValidConnectionCell(1000, -1, f.current.get()));

        int outX = 0, outZ = 0;
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(-1, 5, outX, outZ, nullptr);
        CHECK(outX == 0 && outZ == 1);
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(64, 5, outX, outZ, nullptr);
        CHECK(outX == 2 && outZ == 1);
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(5, -1, outX, outZ, nullptr);
        CHECK(outX == 1 && outZ == 0);
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(5, 64, outX, outZ, nullptr);
        CHECK(outX == 1 && outZ == 2);

        cISC4RegionalCity* neighbor = nullptr;
        int neighborX = 0, neighborZ = 0;
        CHECK(sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighborCell(-1, 5, outX, outZ, &neighbor,
                                                                              neighborX, neighborZ));
        CHECK(neighbor == west && outX == 0 && outZ == 1 && neighborX == 64 && neighborZ == 5);
        int backX = 0, backZ = 0;
        CHECK(sc4::cSC4NeighborsSimulator::ConvertNeighborCellToConnectionCell(west, 64, 5, backX, backZ));
        CHECK(backX == -1 && backZ == 5);
        CHECK(sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighborCell(64, 5, outX, outZ, &neighbor,
                                                                              neighborX, neighborZ));
        CHECK(neighbor == east && outX == 2 && outZ == 1 && neighborX == -1 && neighborZ == 5);
        CHECK(sc4::cSC4NeighborsSimulator::ConvertNeighborCellToConnectionCell(east, -1, 5, backX, backZ));
        CHECK(backX == 64 && backZ == 5);

        sim->CreateConnection(0, -1, 5, false);
        CHECK(sim->Connections().size() == 1);
        CHECK(sim->HasConnection(0, -1, 5));
        CHECK(sim->FindConnection(0, -1, 5) != nullptr);
        CHECK(sim->FindConnection(0, 0, 0) == nullptr);
        CHECK(sim->FindCityOnOtherSideOfConnection(-1, 5) == west);
        CHECK(sim->FindCityOnOtherSideOfConnection(64, 5) == east);
        CHECK(f.host.symmetricChanges.size() == 1 && f.host.symmetricChanges[0].city == west &&
              f.host.symmetricChanges[0].add);
        CHECK(f.host.posts.size() == 1 && f.host.posts[0].type == sc4::kNeighborsMessageConnectionCreated &&
              f.host.posts[0].data1 == 0 && f.host.posts[0].data2 == static_cast<std::uint32_t>(-1) &&
              f.host.posts[0].data3 == 5);
        CHECK(f.host.createdProps == 1 && f.host.props.size() == 1);
        CHECK(f.host.props[0]->flags == 3);                       // x < 0, not from the other side
        CHECK(f.host.props[0]->position[0] == 0.5f);              // cell (0, 5) centre
        CHECK(f.host.props[0]->position[2] == 5.5f);
        CHECK(f.host.props[0]->position[1] == 10.0f);             // type 0 is not a raised type
        CHECK(f.host.finishedProps.size() == 1 && f.host.releasedProps.size() == 1);

        sim->CreateConnection(3, 64, 5, true);                    // raised type, east side
        CHECK(sim->Connections().size() == 2);
        CHECK(f.host.props[1]->flags == 3);
        CHECK(f.host.props[1]->position[1] == 25.5f);

        ConnectionList list;
        sim->GetConnectionsOfType(0, list);
        CHECK(list.size() == 1);
        sim->GetConnectionsToCity(west, list);
        CHECK(list.size() == 1);
        sim->GetConnectionsOfTypeToCity(3, east, list);
        CHECK(list.size() == 1);
        sim->GetAllConnections(list);
        CHECK(list.size() == 2);

        auto* westMirror = new sc4::cSC4NeighborConnection();
        westMirror->AddRef();
        westMirror->Initialize(0, 64, 5, -1, 5);
        west->connections.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(westMirror));
        westMirror->Release();
        auto* eastMirror = new sc4::cSC4NeighborConnection();
        eastMirror->AddRef();
        eastMirror->Initialize(0, -1, 5, 64, 5);
        east->connections.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(eastMirror));
        eastMirror->Release();
        CHECK(sim->FindConnectionOnOtherSide(0, 64, 5) == eastMirror);
        CHECK(sim->FindConnectionOnOtherSide(sim->FindConnection(0, -1, 5)) == westMirror);
        CHECK(sim->FindUnvisitedConnection(0, west) == sim->FindConnection(0, -1, 5));
        CHECK(sim->FindUnvisitedConnection(0, east) == nullptr);

        auto* occupant = FakeOccupant::Make(0x1000);
        f.host.occupantHolders[occupant] = std::make_unique<cSCPropertyHolder>();
        sc4::cSCPropertyHelp::SetPropertyValue(f.host.occupantHolders[occupant].get(),
                                               sc4::kNeighborsPropertyConnectionFlag, true);
        f.host.occupantsByCell[{0, 5}] = occupant;
        sim->DeleteConnection(0, -1, 5);
        CHECK(sim->Connections().size() == 1);
        CHECK(f.host.deletedProps.size() == 1);
        CHECK(f.host.posts.back().type == sc4::kNeighborsMessageConnectionDeleted);
        CHECK(f.host.symmetricChanges.back().city == west && !f.host.symmetricChanges.back().add);

        sim->CreateConnection(4, -1, 6, false);
        sim->CreateConnection(4, -1, 7, false);
        sim->CreateConnection(5, -1, 8, false);
        sim->GetConnectionsOfType(4, list);
        CHECK(list.size() == 2);
        sim->DeleteAllConnectionsOfType(4);
        sim->GetConnectionsOfType(4, list);
        CHECK(list.empty());
        CHECK(sim->Connections().size() == 2);   // type 3 and type 5 remain

        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // RequestCreateConnection
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        FakeRegionalCity* west = f.AddCity(0, 1);
        west->name = "Springfield";
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        f.host.confirmResult = false;
        CHECK(!sim->RequestCreateConnection(0, -1, 5, false));
        CHECK(f.host.dialogTitle == 0xaa8e151b);
        CHECK(f.host.dialogText == 0x0a8e152d);
        CHECK(f.host.dialogOk == 0x0a8e15f9);
        CHECK(f.host.dialogArg == "Springfield");
        f.host.confirmResult = true;
        CHECK(sim->RequestCreateConnection(0, -1, 5, false));
        sim->CreateConnection(0, -1, 5, false);
        CHECK(!sim->RequestCreateConnection(0, -1, 5, false));   // already connected
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // serialization framing
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        f.AddCity(0, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        sim->CreateConnection(0, -1, 5, false);
        sim->CreateConnection(3, 64, 5, true);

        PortMemoryOStream os;
        CHECK(sim->Write(os));
        const auto& bytes = os.bytes();
        CHECK(bytes.size() >= 6);
        std::uint16_t version = 0;
        std::uint32_t count = 0;
        std::memcpy(&version, bytes.data(), 2);
        std::memcpy(&count, bytes.data() + 2, 4);
        CHECK(version == 2);
        CHECK(count == 2);

        std::vector<unsigned char> record;
        const auto put16 = [&record](std::uint16_t v) {
            record.push_back(static_cast<unsigned char>(v & 0xff));
            record.push_back(static_cast<unsigned char>((v >> 8) & 0xff));
        };
        const auto put32 = [&record](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) record.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xff));
        };
        put16(2);
        put32(0);
        PortMemoryIStream reader(record.data(), record.size());
        CHECK(sim->Read(reader));
        record.clear();
        put16(3);
        put32(0);
        PortMemoryIStream badReader(record.data(), record.size());
        CHECK(!sim->Read(badReader));
        record.clear();
        put16(1);
        put32(0);
        PortMemoryIStream oldReader(record.data(), record.size());
        CHECK(!sim->Read(oldReader));
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // messages and the deal pass
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        f.AddCity(0, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        CHECK(sim->Init());

        auto month = MakeMessage(sc4::kNeighborsMessageMonth);
        CHECK(sim->DoMessage(month.get()));
        CHECK(f.host.dealStatuses.empty());   // the city is not established yet

        auto established = MakeMessage(sc4::kNeighborsMessageCityEstablished);
        CHECK(sim->DoMessage(established.get()));
        CHECK(sim->CityEstablished_());

        sim->MarkDealsDirty();
        CHECK(sim->DealsDirty());
        auto day = MakeMessage(sc4::kNeighborsMessageDay);
        CHECK(sim->DoMessage(day.get()));
        CHECK(!sim->DealsDirty());
        CHECK(!sim->ConnectionsDirty());

        sim->CreateConnection(0, -1, 5, false);
        CHECK(sim->DoMessage(month.get()));
        CHECK(f.current->budget == 5000.0);
        CHECK(f.current->income == 1000.0);
        CHECK(f.current->produced[1] == 100.0 && f.current->demanded[1] == 80.0);
        CHECK(f.current->produced[2] == 50.0 && f.current->demanded[2] == 40.0);
        CHECK(f.current->costPerUnit[1] == 1e-05 && f.current->costPerUnit[2] == 1e-05);
        CHECK(std::fabs(f.current->produced[3] - (1.0 + 10.0 / 12.0 + 2.0 + 3.0 + 4.0)) < 1e-6);
        CHECK(f.current->demanded[3] == 5.0);
        CHECK(f.current->costPerUnit[3] == 1e-05);
        CHECK(!f.host.history.empty());
        CHECK(f.host.dealStatuses.empty());

        // Propose -> activate.
        auto proposed = MakeDeal(sc4::kDealTypePowerSell, 1, 1, 0, 1, 10);
        proposed->SetFlags(sc4::kDealFlagProposed, true);
        f.host.dealStatuses.clear();
        sim->AddDeal(proposed.get());
        CHECK(sim->Deals().size() == 1);
        CHECK(sim->DoMessage(month.get()));
        CHECK(proposed->CheckFlagsAnd(sc4::kDealFlagActive));
        CHECK(!proposed->CheckFlagsOr(sc4::kDealFlagProposed));
        CHECK(std::find(f.host.dealStatuses.begin(), f.host.dealStatuses.end(), 1) != f.host.dealStatuses.end());

        // Cancelled -> removed with status 2.
        proposed->SetFlags(sc4::kDealFlagCancelled, true);
        f.host.dealStatuses.clear();
        CHECK(sim->DoMessage(month.get()));
        CHECK(sim->Deals().empty());
        CHECK(std::find(f.host.dealStatuses.begin(), f.host.dealStatuses.end(), 2) != f.host.dealStatuses.end());

        // Expiry -> status 6; the next pass renews (status 7) because the city can pay.
        auto expiring = MakeDeal(sc4::kDealTypePowerBuy, 1, 1, 0, 1, 10);
        expiring->SetFlags(sc4::kDealFlagProposed, false);
        expiring->SetFlags(sc4::kDealFlagActive, true);
        expiring->SetExpirationDate(cGZDate(f.host.simDate.DayNumber() - 1));
        expiring->SetCurrentDate(cGZDate(f.host.simDate.DayNumber() - 40));
        sim->AddDeal(expiring.get());
        f.host.dealStatuses.clear();
        CHECK(sim->Deals().size() == 1);
        CHECK(sim->DoMessage(month.get()));
        CHECK(expiring->CheckFlagsAnd(sc4::kDealFlagExpired));
        CHECK(std::find(f.host.dealStatuses.begin(), f.host.dealStatuses.end(), 6) != f.host.dealStatuses.end());
        f.host.dealStatuses.clear();
        CHECK(sim->DoMessage(month.get()));
        CHECK(!expiring->CheckFlagsOr(sc4::kDealFlagExpired));
        CHECK(std::find(f.host.dealStatuses.begin(), f.host.dealStatuses.end(), 7) != f.host.dealStatuses.end());
        cGZDate newStart, newExpiration;
        expiring->GetStartDate(newStart);
        expiring->GetExpirationDate(newExpiration);
        CHECK(newStart.DayNumber() == f.host.simDate.DayNumber() - 40);   // start = the old current date
        // Quirk: the renewal sets the expiration to (today - today) + sim date.
        CHECK(newExpiration.DayNumber() == f.host.simDate.DayNumber() ||
              newExpiration.DayNumber() == f.host.simDate.DayNumber() + 1);
        CHECK(expiring->CheckFlagsOr(sc4::kDealFlagActive));

        // Frozen + affordable -> removed with status 3.
        expiring->SetFlags(sc4::kDealStateMask, false);
        expiring->SetFlags(sc4::kDealFlagActive, true);
        expiring->SetFlags(sc4::kDealFlagFrozenBit, true);
        expiring->SetFlags(sc4::kDealFlagAffordable, true);
        f.host.dealStatuses.clear();
        CHECK(sim->DoMessage(month.get()));
        CHECK(sim->Deals().empty());
        CHECK(std::find(f.host.dealStatuses.begin(), f.host.dealStatuses.end(), 3) != f.host.dealStatuses.end());

        // Locked with the suspension date in the past -> the lock turns into the frozen state.
        auto locked = MakeDeal(sc4::kDealTypePowerSell, 1, 1, 0, 1, 4);
        locked->SetFlags(sc4::kDealFlagProposed, false);
        locked->SetFlags(sc4::kDealFlagActive, true);
        locked->SetFlags(sc4::kDealFlagLockedState, true);
        locked->SetSuspensionDate(cGZDate(f.host.simDate.DayNumber() - 1));
        sim->AddDeal(locked.get());
        f.host.dealStatuses.clear();
        CHECK(sim->DoMessage(month.get()));
        CHECK(!locked->CheckFlagsOr(sc4::kDealFlagLockedState));
        CHECK(locked->CheckFlagsOr(sc4::kDealFlagFrozenBit));

        // The UI disable flag skips the deal pass.
        sim->SetUIDisableFlag(true);
        CHECK(sim->UIDisabled());
        f.host.dealStatuses.clear();
        CHECK(sim->DoMessage(month.get()));
        CHECK(f.host.dealStatuses.empty());
        CHECK(!sim->Deals().empty());
        sim->SetUIDisableFlag(false);

        // Re-entrancy: a nested simulate_deals (from the status callback) posts data1 = 8.
        auto nested = MakeDeal(sc4::kDealTypePowerSell, 1, 1, 0, 1, 3);
        nested->SetFlags(sc4::kDealFlagProposed, true);
        sim->AddDeal(nested.get());
        f.host.posts.clear();
        bool nestedCall = false;
        f.host.onDealStatus = [&]() {
            if (nestedCall) return;
            nestedCall = true;
            sim->simulate_deals();
        };
        CHECK(sim->DoMessage(month.get()));
        bool foundGuard = false;
        for (const auto& post : f.host.posts) {
            if (post.type == sc4::kNeighborsMessageDealStatus && post.data1 == 8) foundGuard = true;
        }
        CHECK(foundGuard);
        f.host.onDealStatus = nullptr;

        // City reset clears the deal list.
        auto reset = MakeMessage(sc4::kNeighborsMessageCityReset);
        CHECK(sim->DoMessage(reset.get()));
        CHECK(sim->Deals().empty());
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // deal queries and pricing
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        FakeRegionalCity* west = f.AddCity(0, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        std::vector<float> curve = {0.0f, 0.0f, 1.0f, 1.0f, 2.0f, 4.0f};
        sc4::cSCPropertyHelp::SetPropertyValue(f.exemplar->AsISCPropertyHolder(),
                                               sc4::kNeighborsPropertyResponseCurve, curve);
        CHECK(sim->Init());

        auto dealA = MakeDeal(sc4::kDealTypePowerSell, 1, 1, 9, 9, 100);   // income
        dealA->SetFlags(sc4::kDealFlagProposed, false);
        dealA->SetFlags(sc4::kDealFlagActive, true);
        auto dealB = MakeDeal(sc4::kDealTypePowerBuy, 1, 1, 9, 9, 100);    // expense
        dealB->SetFlags(sc4::kDealFlagProposed, false);
        dealB->SetFlags(sc4::kDealFlagActive, true);
        auto dealC = MakeDeal(sc4::kDealTypeWaterSell, 1, 1, 9, 9, 100);   // income (water)
        dealC->SetFlags(sc4::kDealFlagProposed, false);
        dealC->SetFlags(sc4::kDealFlagActive, true);
        sim->AddDeal(dealA.get());
        sim->AddDeal(dealB.get());
        sim->AddDeal(dealC.get());

        DealList list;
        sim->FindDealsAll(list);
        CHECK(list.size() == 3);
        sim->FindDeals(sc4::kDealTypePowerSell, list);
        CHECK(list.size() == 1);
        sim->FindDeals(sc4::kDealTypePowerSell, west, list);
        CHECK(list.empty());   // the deals point at the empty cell (9, 9)
        sim->FindDeals(sc4::kDealTypeWaterSell, list);
        CHECK(list.size() == 1);
        CHECK(sim->GetTotalMonthlyNeighborDealIncome() == 500);    // power sell 200 + water sell 300
        CHECK(sim->GetTotalMonthlyNeighborDealExpense() == 200);   // power buy 200

        // Pricing. The curve is {0,0},{1,1},{2,4}; the exemplar factors widen the cost band 6..9.
        sc4::cSCPropertyHelp::SetPropertyValue(f.exemplar->AsISCPropertyHolder(),
                                               sc4::kNeighborsPropertyDealCostFactor, 2.0f);
        sc4::cSCPropertyHelp::SetPropertyValue(f.exemplar->AsISCPropertyHolder(),
                                               sc4::kNeighborsPropertyDealCostFactor2, 1.0f);
        f.current->demanded[1] = 100.0;
        f.current->exported[1] = 10.0;
        f.current->produced[1] = 60.0;
        f.current->imported[1] = 5.0;
        west->demanded[1] = 40.0;
        west->exported[1] = 0.0;
        west->produced[1] = 70.0;
        west->imported[1] = 0.0;
        west->costPerUnit[1] = 3.0;

        CHECK(sim->GetDealMaxAmount(sc4::kDealTypePowerBuy, west) == 30.0);
        CHECK(sim->GetDealMaxAmount(sc4::kDealTypePowerBuy, nullptr) == 0.0);
        const double cost = sim->GetDealCost(sc4::kDealTypePowerBuy, 10.0, west);
        const double expected = 6.0 + (9.0 - 6.0) * (20.0 / 70.0) * 0.5;
        CHECK(std::fabs(cost - expected * 10.0) < 1e-6);
        CHECK(sim->GetDealCost(sc4::kDealTypePowerBuy, 0.0, west) == 0.0);
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // adjacent / connected cities
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1, 2, 1);
        f.AddCity(1, 0);
        FakeRegionalCity* north2 = f.AddCity(2, 0);
        north2->established = false;
        FakeRegionalCity* east = f.AddCity(3, 1);
        FakeRegionalCity* shared = f.AddCity(1, 2);
        f.host.region[{2, 2}] = shared;   // the same city reaches two border cells
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);

        std::list<cRZAutoRefCount<cISC4RegionalCity>> cities;
        sim->GetAdjacentCities(cities, false);
        CHECK(cities.size() == 3);   // north, east, shared (deduplicated)
        sim->GetAdjacentCities(cities, true);
        CHECK(cities.size() == 4);   // north2 is back

        sim->CreateConnection(2, -1, 5, false);    // west of the city -> (0,1), no city there
        sim->CreateConnection(2, 128, 5, false);   // east of the 2-cell-wide city -> east
        sim->CreateConnection(0, 5, -1, false);    // north of the city -> north (type 0 is not connectable)
        ConnectionList all;
        sim->GetAllConnections(all);
        CHECK(all.size() == 3);
        sim->GetConnectedCities(cities, false);
        CHECK(cities.size() == 1 && cities.front().get() == east);

        auto* chain = new sc4::cSC4NeighborConnection();
        chain->AddRef();
        chain->Initialize(2, 0, 0, 3, 2);
        east->connections.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(chain));
        chain->Release();
        f.host.region[{3, 2}] = shared;
        sim->GetAllConnectedCities(cities, false);
        CHECK(cities.size() == 2);   // east and shared, the current city is removed
        sim->GetAllConnectedCities(cities, true);
        CHECK(cities.size() == 2);
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // DeleteOccupantConnection and the occupant-removed message
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        f.AddCity(0, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        sim->CreateConnection(4, -1, 10, false);
        CHECK(sim->HasConnection(4, -1, 10));
        auto* pole = FakeOccupant::Make(0x2000);
        f.host.occupantTypes[pole] = sc4::kOccupantTypePowerPole;
        f.host.occupantPositions[pole] = {0.0f, 10.5f};   // cell (0, 10): x == 0 -> the west border
        sim->DeleteOccupantConnection(pole);
        CHECK(!sim->HasConnection(4, -1, 10));

        sim->CreateConnection(0xb, -1, 20, false);
        auto* network = FakeOccupant::Make(0x2001);
        f.host.occupantTypes[network] = sc4::kOccupantTypeNetworkOccupant;
        f.host.occupantNetworkTypes.insert({network, 9});   // network type 9 -> connection type 0xb
        f.host.occupantPositions[network] = {0.0f, 20.5f};
        sim->DeleteOccupantConnection(network);
        CHECK(!sim->HasConnection(0xb, -1, 20));

        // The occupant-removed message path only reacts to the power-pole type and only when not retained.
        f.host.occupantRetained = false;
        sim->CreateConnection(4, -1, 12, false);
        f.host.occupantPositions[pole] = {0.0f, 12.5f};
        auto message = MakeMessage(sc4::kNeighborsMessageOccupantRemoved);
        message->SetVoid1(pole);
        CHECK(sim->DoMessage(message.get()));
        CHECK(!sim->HasConnection(4, -1, 12));

        sim->CreateConnection(4, -1, 13, false);
        f.host.occupantPositions[pole] = {0.0f, 13.5f};
        f.host.occupantRetained = true;
        CHECK(sim->DoMessage(message.get()));
        CHECK(sim->HasConnection(4, -1, 13));
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // DoTerrainChanged through the terrain-changed message
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        f.AddCity(0, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        sim->CreateConnection(3, -1, 5, false);   // the raised type
        auto* occupant = FakeOccupant::Make(0x3000);
        f.host.occupantsByCell[{0, 5}] = occupant;
        f.host.occupantHolders[occupant] = std::make_unique<cSCPropertyHolder>();
        sc4::cSCPropertyHelp::SetPropertyValue(f.host.occupantHolders[occupant].get(),
                                               sc4::kNeighborsPropertyConnectionFlag, true);
        f.host.altitudeCells[{0, 5}] = 12.0f;

        // data1 = (0 << 16) | 5, data2 = (5 << 16) | 5 -> rectangle x 0..5, z 4..5.
        auto message = MakeMessage(sc4::kNeighborsMessageTerrainChanged, 5u, (5u << 16) | 5u);
        CHECK(sim->DoMessage(message.get()));
        CHECK(f.host.deactivated.size() == 1 && f.host.deactivated[0] == occupant);
        CHECK(f.host.occupantAltitude[occupant] == 12.0f + 15.5f);

        auto outside = MakeMessage(sc4::kNeighborsMessageTerrainChanged, (10u << 16) | 15u, (10u << 16) | 15u);
        CHECK(sim->DoMessage(outside.get()));
        CHECK(f.host.deactivated.size() == 1);
        CHECK(sim->Release() == 0);
    }

    //-----------------------------------------------------------------------------------------------------------------
    // UpdateCommodityStats with budget line items and the garbage curve
    //-----------------------------------------------------------------------------------------------------------------
    {
        Fixture f;
        f.PlaceCurrent(1, 1);
        auto* sim = new sc4::cSC4NeighborsSimulator(&f.host);
        CHECK(sim->Init());
        // A power deal cost (type e0, amount 10, factor 2 -> 20) enters the power price numerator.
        auto deal = MakeDeal(sc4::kDealTypePowerBuy, 1, 1, 9, 9, 10);
        deal->SetFlags(sc4::kDealFlagProposed, false);
        deal->SetFlags(sc4::kDealFlagActive, true);
        sim->AddDeal(deal.get());
        f.current->imported[1] = 5.0;
        f.host.budgetLines[sc4::kNeighborsBudgetPower] = 80.0;
        f.host.budgetSubLines[{sc4::kNeighborsBudgetPower, sc4::kNeighborsBudgetPowerSub}] = 40.0;
        f.host.budgetLines[sc4::kNeighborsBudgetGarbage] = 10.0;
        std::vector<float> curve = {0.0f, 0.0f, 10.0f, 1.0f};
        sc4::cSCPropertyHelp::SetPropertyValue(f.exemplar->AsISCPropertyHolder(),
                                               sc4::kNeighborsPropertyGarbageCurve, curve);
        sim->UpdateCommodityStats(f.city.get());
        CHECK(std::fabs(f.current->costPerUnit[1] - (20.0 + 80.0) / (5.0 + 100.0 + 1.0)) < 1e-9);
        const double produced = 1.0 + 10.0 / 12.0 + 2.0 + 3.0 + 4.0;
        const double input = (40.0 + 10.0) / (0.0 + produced + 1.0);
        CHECK(std::fabs(f.current->costPerUnit[3] - input / 10.0) < 1e-6);
        f.current->costPerUnit[1] = -1.0;
        FakeCity other;
        sim->UpdateCommodityStats(&other);
        CHECK(f.current->costPerUnit[1] == -1.0);
        CHECK(sim->Release() == 0);
    }

    sc4::cSC4NeighborsSimulator::ResetStaticInitialized();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("sc4_neighborssimulator: ok\n");
    return 0;
}
