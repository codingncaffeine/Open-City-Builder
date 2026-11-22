#include <algorithm>
#include <tuple>
#include <set>
#include <cstdarg>
// cSC4RegionalCity — tests the region's view of a city against the recovered behaviour: the constructor's
// defaults, COM plumbing, the statistics/tax/population accessors, the connection and deal bookkeeping, the
// city/preferences cache updates, the message handler and the version-13 save record (round trip, older
// versions and the quirks listed in src/sc4/RegionalCity.h).
//
// Every neighbour is a fake IRegionalCityHost; connections and deals are either fakes (logic) or the real
// recovered records (serialization).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gz/StreamOps.h"
#include "rz/PortMemoryStreams.h"
#include "rz/cRZMessage2Standard.h"
#include "sc4/RegionalCity.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {

//---------------------------------------------------------------------------------------------------------------------
// fakes
//---------------------------------------------------------------------------------------------------------------------

struct FakeRegionalCity final : cISC4RegionalCity {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4RegionalCity) {
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
    std::uint32_t GetCitySerialNumber() const override { return 0; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromCString(""); }

    int refs = 1;
};

struct FakeCity final : cISC4City {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown) {
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
    void SetCitySerialNumber(std::uint32_t serial) override { serialNumber = serial; }
    void GetCitySaveFilePath(cIGZString& out) const override { out.AssignFromIGZString(savePath); }
    void SetCitySaveFilePath(const cIGZString& path) override {
        savePath.AssignFromIGZString(path);
        if (log) log->push_back("city:SetCitySaveFilePath");
    }
    void SetCityNameChanged(bool changed) override { nameChanged = changed; }
    bool IsSaveDisabled() const override { return false; }

    cRZString savePath;
    std::uint32_t serialNumber = 0;
    bool nameChanged = false;
    std::vector<std::string>* log = nullptr;
    int refs = 1;
};

struct FakeConnection final : sc4::cISC4NeighborConnection {
    bool QueryInterface(std::uint32_t iid, void** out) override {
        if (iid == kGZIID_cIGZUnknown || iid == sc4::kGZIID_cISC4NeighborConnection) {
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
    std::uint32_t GetType() const override { return type; }
    void GetConnectionCell(int& x, int& z) const override { x = cellX; z = cellZ; }
    void GetRegionCellOnOtherSideOfConnection(int& x, int& z) const override { x = otherX; z = otherZ; }
    bool GetConnectionInnerCell(int& x, int& z) const override { x = innerX; z = innerZ; return innerValid; }
    void* GetCityOnOtherSideOfConnection() const override { return otherCity; }
    void Repair(void*) override { ++repairs; }
    bool GetVisitedThisCycle() const override { return false; }
    void SetVisitedThisCycle(bool) override {}
    bool GetIsActive() const override { return true; }
    void SetIsActive(bool) override {}

    std::uint32_t type = 1;
    int cellX = 0, cellZ = 0;
    int otherX = 0, otherZ = 0;
    int innerX = 0, innerZ = 0;
    bool innerValid = true;
    void* otherCity = nullptr;
    int repairs = 0;
    int refs = 1;
};

// The neighbour-deal seam the deals use to find their other side.
struct TestDealHost final : sc4::INeighborDealHost {
    bool CityHasBudget() override { return true; }
    double DealCost(double, std::uint32_t, void*) override { return 0.0; }
    bool CityEntryAtCell(int, int, void*& city) override {
        city = cityAtCell;
        return cityAtCell != nullptr;
    }
    void* CurrentCity() override { return nullptr; }
    double CurrentCityFunds() override { return 0.0; }
    void ReportDealStatus(bool, sc4::cSC4NeighborDeal*) override {}

    void* cityAtCell = nullptr;
};

TestDealHost g_dealHost;

struct FakeHost final : IRegionalCityHost {
    // -- CreateSymmetricConnections: every call logged; connections, cell contents and edge words kept here
    std::vector<std::string> pass;
    int cellsX = 0, cellsZ = 0;
    bool toolOk = false, driveSide = true;
    std::set<std::tuple<std::uint32_t, int, int>> links;
    std::set<std::pair<int, int>> trafficCells, laidCells;
    std::map<std::pair<int, int>, std::uint32_t> edges;   // the edge word of the network piece in a cell (one piece per laid cell)
    std::uint32_t laidNetwork = 0;
    void Log(const char* fmt, ...) {
        char buffer[160];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buffer, sizeof buffer, fmt, args);
        va_end(args);
        pass.push_back(buffer);
    }
    void CityCellCounts(int& x, int& z) override { x = cellsX; z = cellsZ; }
    bool DriveSideFlag() override { return driveSide; }
    bool NeighborsHasConnection(std::uint32_t type, int x, int z) override { return links.count({type, x, z}) != 0; }
    void NeighborsCreateConnection(std::uint32_t type, int x, int z, bool flag) override { Log("create %u %d,%d dir %d", type, x, z, flag ? 1 : 0); links.insert({type, x, z}); }
    void NeighborsDeleteConnection(std::uint32_t type, int x, int z) override { Log("delete %u %d,%d", type, x, z); links.erase({type, x, z}); }
    bool BeginNetworkTool(std::uint32_t network) override { Log("tool %u", network); laidNetwork = network; return toolOk; }
    void InitNetworkTool() override { Log("init"); }
    void PlaceToolOccupants(int x0, int z0, int x1, int z1) override {
        Log("place %d,%d-%d,%d", x0, z0, x1, z1);
        for (int x = std::min(x0, x1); x <= std::max(x0, x1); ++x)
            for (int z = std::min(z0, z1); z <= std::max(z0, z1); ++z) { laidCells.insert({x, z}); edges.emplace(std::make_pair(x, z), 0u); }
    }
    void EndNetworkTool() override { Log("end"); }
    bool TrafficMapCellOccupied(int x, int z) override { return trafficCells.count({x, z}) != 0; }
    void DemolishCellsOnLayer(const std::vector<std::pair<int, int>>& cells, std::uint8_t layer) override {
        std::string list;
        for (auto& c : cells) list += " " + std::to_string(c.first) + "," + std::to_string(c.second);
        Log("demolish layer %u:%s", layer, list.c_str());
    }
    void DezoneCells(const std::vector<std::pair<int, int>>& cells) override { Log("dezone %zu", cells.size()); }
    int NetworkOccupantsAt(int x, int z, std::uint32_t, int, std::vector<void*>& out) override {
        out.clear();
        auto it = edges.find({x, z});
        if (it != edges.end()) out.push_back(&it->second);
        return static_cast<int>(out.size());
    }
    std::uint32_t EdgeWord(void* occupant, int) override { return *static_cast<std::uint32_t*>(occupant); }
    void SetEdgeWord(void* occupant, int, std::uint32_t word) override { *static_cast<std::uint32_t*>(occupant) = word; }

    static std::string Hex(std::uint32_t value) {
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "%x", value);
        return buffer;
    }

    // -- framework -------------------------------------------------------------------------------------
    std::uint32_t DefaultDifficultyLevel() override { return defaultDifficulty; }
    void AddNotification(cIGZMessageTarget2*, std::uint32_t id) override {
        notifications.push_back("add:" + Hex(id));
    }
    void RemoveNotification(cIGZMessageTarget2*, std::uint32_t id) override {
        notifications.push_back("remove:" + Hex(id));
    }
    cISC4RegionalCity* CurrentRegionalCity() override { return currentRegionalCity; }
    cISC4City* CurrentCity() override { return currentCity; }
    cISC4RegionalCity* RegionCityAt(int x, int z) override {
        auto it = regionCities.find({x, z});
        return it == regionCities.end() ? nullptr : it->second;
    }
    bool HasNeighborsSimulator() override { return hasNeighbors; }
    cISC4RegionalCity* FindCityOnOtherSideOfConnection(int, int) override { return findCityResult; }
    bool RegionalCityEstablished(const cISC4RegionalCity* city) override {
        return city == findCityResult && regionalEstablished;
    }
    void RegionalCityConnections(const cISC4RegionalCity*, NeighborConnectionList& out) override {
        out = regionalConnections;
    }
    void GetAdjacentCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool) override {
        out = adjacentCities;
    }
    void ConvertConnectionCellToNeighborCell(int x, int z, int& neighborX, int& neighborZ,
                                             cISC4RegionalCity*& city, int& cellX, int& cellZ) override {
        neighborX = x + 1;
        neighborZ = z + 2;
        city = convertedCity;
        cellX = x + 100;
        cellZ = z + 200;
    }
    void ConvertNeighborCellToConnectionCell(cISC4RegionalCity*, int x, int z, int& outX, int& outZ) override {
        outX = x - 100;
        outZ = z - 200;
    }
    void CreateSavePathForCity(const cIGZString& cityName, std::uint32_t serial, cRZString& out) override {
        callLog.push_back("CreateSavePathForCity");
        out.AssignFromCString("C:/Cities/");
        out.AppendChars(cityName.Data(), cityName.Strlen());
        out.AppendChars("_", 1);
        char buffer[16];
        std::snprintf(buffer, sizeof buffer, "%u", serial);
        out.AppendChars(buffer, static_cast<std::uint32_t>(std::strlen(buffer)));
        out.AppendChars(".sc4", 4);
    }
    int TutorialGUIDFromStoredValue(int value) override {
        return tutorialMapping == 0 ? value : value * 2 + 1;
    }
    int UILanguage() override { return uiLanguage; }
    bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        loadStringCalls.emplace_back(group, instance);
        out.AssignFromIGZString(loadedString);
        return loadStringResult;
    }
    bool ReadStringRecord(cIGZPersistDBSegment*, std::uint32_t group, std::uint32_t instance,
                          cIGZString& out) override {
        ++stringRecordCalls;
        lastStringRecord = {group, instance};
        if (group == kRegionalCityRecordIdCityName) {
            out.AssignFromIGZString(stringRecordCityName);
            return true;
        }
        if (group == kRegionalCityRecordIdMayorName) {
            out.AssignFromIGZString(stringRecordMayorName);
            return true;
        }
        return false;
    }

    // -- the city object -------------------------------------------------------------------------------
    void CityName(const cISC4City*, cIGZString& out) override {
        callLog.push_back("CityName");
        out.AssignFromIGZString(cityName);
    }
    void CityMayorName(const cISC4City*, cIGZString& out) override {
        callLog.push_back("CityMayorName");
        out.AssignFromIGZString(cityMayorName);
    }
    void SetCityName(cISC4City*, const cIGZString& name) override {
        callLog.push_back("SetCityName");
        cityName.AssignFromIGZString(name);
    }
    void SetCityMayorName(cISC4City*, const cIGZString& name) override {
        callLog.push_back("SetCityMayorName");
        cityMayorName.AssignFromIGZString(name);
    }
    std::uint32_t CityDifficulty(const cISC4City*) override {
        callLog.push_back("CityDifficulty");
        return cityDifficulty;
    }
    void SetCityDifficulty(cISC4City*, std::uint32_t level) override {
        callLog.push_back("SetCityDifficulty");
        cityDifficulty = level;
    }
    void SetCitySize(cISC4City*, float sizeX, float sizeZ) override {
        callLog.push_back("SetCitySize");
        setCitySizeX = sizeX;
        setCitySizeZ = sizeZ;
    }
    std::uint32_t CitySerialNumber(const cISC4City*) override {
        callLog.push_back("CitySerialNumber");
        return citySerial;
    }
    bool CityEstablished(const cISC4City*) override {
        callLog.push_back("CityEstablished");
        return cityEstablished;
    }
    int CityMayorRating(const cISC4City*) override {
        callLog.push_back("CityMayorRating");
        return cityMayorRating;
    }
    void CityTaxRates(const cISC4City*, float (&rates)[12]) override {
        callLog.push_back("CityTaxRates");
        for (int i = 0; i < 12; ++i) rates[i] = cityTaxRates[i];
    }
    void CityConnections(const cISC4City*, NeighborConnectionList& out) override {
        callLog.push_back("CityConnections");
        out = cityConnections;
    }
    void CityUpdateCommodityStats(cISC4City*) override {
        callLog.push_back("CityUpdateCommodityStats");
        ++commodityStatsUpdates;
    }
    bool CityTutorialGUID(const cISC4City*, int& guid) override {
        callLog.push_back("CityTutorialGUID");
        if (!tutorialSystemPresent) return false;
        guid = tutorialGUID;
        return true;
    }
    bool CityPopulationStats(const cISC4City*, int& population, float& workforce) override {
        callLog.push_back("CityPopulationStats");
        if (!populationStatsPresent) return false;
        population = cityPopulation;
        workforce = cityWorkforce;
        return true;
    }
    void CityLocalPopulationSummary(const cISC4City*, std::map<std::uint32_t, int>& out) override {
        callLog.push_back("CityLocalPopulationSummary");
        out = localPopulationSummary;
    }
    void CityJobsPerUnitOfDemand(const cISC4City*, float* out, std::uint32_t purpose) override {
        callLog.push_back("CityJobsPerUnitOfDemand");
        lastJobsPurpose = purpose;
        for (int i = 0; i < 4; ++i) out[i] = jobsPerUnit[i];
    }
    void AirportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>& out) override {
        callLog.push_back("AirportPointsOfInterest");
        out = airports;
    }
    void SeaportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>& out) override {
        callLog.push_back("SeaportPointsOfInterest");
        out = seaports;
    }

    std::uint32_t defaultDifficulty = 1;
    std::vector<std::string> notifications;
    cISC4RegionalCity* currentRegionalCity = nullptr;
    cISC4City* currentCity = nullptr;
    std::map<std::pair<int, int>, cISC4RegionalCity*> regionCities;
    bool hasNeighbors = true;
    cISC4RegionalCity* findCityResult = nullptr;
    bool regionalEstablished = true;
    NeighborConnectionList regionalConnections;
    std::list<cRZAutoRefCount<cISC4RegionalCity>> adjacentCities;
    cISC4RegionalCity* convertedCity = nullptr;
    int tutorialMapping = 0;
    int uiLanguage = 0x13;
    cRZString loadedString;
    bool loadStringResult = true;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> loadStringCalls;
    cRZString stringRecordCityName;
    cRZString stringRecordMayorName;
    int stringRecordCalls = 0;
    std::pair<std::uint32_t, std::uint32_t> lastStringRecord{0, 0};
    cRZString cityName;
    cRZString cityMayorName;
    std::uint32_t cityDifficulty = 2;
    std::uint32_t citySerial = 0xabcd;
    bool cityEstablished = true;
    int cityMayorRating = -5;
    float cityTaxRates[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    NeighborConnectionList cityConnections;
    int commodityStatsUpdates = 0;
    bool tutorialSystemPresent = true;
    int tutorialGUID = 0x1234;
    bool populationStatsPresent = true;
    int cityPopulation = 4321;
    float cityWorkforce = 0.77f;
    std::map<std::uint32_t, int> localPopulationSummary;
    float jobsPerUnit[4] = {9, 2, -1, 0.5f};   // the last three add up to 1.5
    std::uint32_t lastJobsPurpose = 0;
    std::vector<cISC4RegionalCity::PointOfInterest> airports;
    std::vector<cISC4RegionalCity::PointOfInterest> seaports;
    std::vector<std::string> callLog;
    float setCitySizeX = 0, setCitySizeZ = 0;
};

//---------------------------------------------------------------------------------------------------------------------
// helpers
//---------------------------------------------------------------------------------------------------------------------

// A real connection record built from its own serialized form (the class has no setters for its fields).
cRZAutoRefCount<sc4::cISC4NeighborConnection> MakeConnection(std::uint32_t type, int cellX, int cellZ,
                                                             int otherX, int otherZ, bool visited) {
    PortMemoryOStream os;
    os.SetUint16(1);                    // record version
    os.SetUint32(type);
    os.SetSint32(cellX);
    os.SetSint32(cellZ);
    os.SetSint32(otherX);
    os.SetSint32(otherZ);
    os.SetUint8(visited ? 1 : 0);
    os.SetUint16(2);                    // empty property holder
    os.SetUint32(0);
    PortMemoryIStream is(os.bytes().data(), os.bytes().size());
    auto* connection = new sc4::cSC4NeighborConnection();
    cRZAutoRefCount<sc4::cISC4NeighborConnection> holder(connection);
    connection->Read(is);
    return holder;
}

// The recovered deal starts silent (flags 0x81); the tests clear that bit so the booking code runs, which is
// what UpdateLocalDeals does to the deals it keeps.
cRZAutoRefCount<sc4::cISC4NeighborDeal> MakeDeal(std::uint32_t type, int cityX, int cityZ, int otherX, int otherZ,
                                                 int amount) {
    auto* deal = new sc4::cSC4NeighborDeal();
    cRZAutoRefCount<sc4::cISC4NeighborDeal> holder(deal);
    deal->SetType(type);
    deal->SetRegionCellOnCitySideOfConnection(cityX, cityZ);
    deal->SetRegionCellOnOtherSideOfConnection(otherX, otherZ);
    deal->SetAmount(amount);
    deal->SetFlags(sc4::kDealFlagActive, true);
    deal->SetFlags(sc4::kDealFlagSilent, false);   // a live deal is not silent
    return holder;
}

std::vector<unsigned char> Serialize(const cSC4RegionalCity& city) {
    PortMemoryOStream os;
    if (!city.Write(os)) return {};
    return os.bytes();
}

//---------------------------------------------------------------------------------------------------------------------
// tests
//---------------------------------------------------------------------------------------------------------------------

void testConstructionAndCom() {
    FakeHost host;
    host.defaultDifficulty = 1;
    cSC4RegionalCity city(&host);

    CHECK(city.GetPopulation() == 0);
    CHECK(city.GetCommercialJobs() == 0);
    CHECK(city.GetIndustrialJobs() == 0);
    CHECK(city.GetWorkforcePercentage() == 0.5f);
    CHECK(city.GetMayorRating() == 0);
    CHECK(city.GetDifficultyLevel() == 1);       // from SL::spPreferences + 0x46c
    CHECK(city.GetEstablished() == false);
    CHECK(city.GetTutorialGUID() == -1);
    CHECK(city.GetBudget() == 0.0);
    CHECK(city.GetIncome() == 0.0);
    int width = 0, height = 0;
    city.GetCitySize(width, height);
    CHECK(width == 1 && height == 1);
    int x = -1, z = -1;
    city.GetPosition(x, z);
    CHECK(x == 0 && z == 0);
    // The constructor inserts the three commodity records 1, 2 and 3.
    CHECK(city.CommodityStatsForTest().size() == 3);
    CHECK(city.CommodityStatsForTest().count(1) == 1);
    CHECK(city.CommodityStatsForTest().count(2) == 1);
    CHECK(city.CommodityStatsForTest().count(3) == 1);
    CHECK(city.GetTaxRate(0x1010) == 7.0f);      // the constructor's 7.0 pattern
    CHECK(city.GetTaxRate(0x7777) == 7.0f);

    // QueryInterface: iid 1 and the regional-city iid answer the primary view, 0x452294aa the message target and
    // 0xe4fda3d4 the serializable view; each successful answer AddRefs.
    void* out = nullptr;
    CHECK(city.QueryInterface(1, &out) && out == static_cast<cISC4RegionalCity*>(&city));
    CHECK(city.AddRef() == 2);
    CHECK(city.Release() == 1);
    CHECK(city.QueryInterface(kGZIID_cISC4RegionalCity, &out) && out == static_cast<cISC4RegionalCity*>(&city));
    CHECK(city.Release() == 1);
    CHECK(city.QueryInterface(0x452294aa, &out) && out == static_cast<cIGZMessageTarget2*>(&city));
    CHECK(city.Release() == 1);
    CHECK(city.QueryInterface(0xe4fda3d4, &out) && out == static_cast<cIGZSerializable*>(&city));
    CHECK(city.Release() == 1);
    CHECK(!city.QueryInterface(0x12345678, &out));
    CHECK(city.GetGZCLSID() == 0x6a037c24);

    // Release destroys below a count of 2 and answers 0 (quirk).
    auto* heapCity = new cSC4RegionalCity(&host);
    CHECK(heapCity->AddRef() == 1);
    CHECK(heapCity->Release() == 0);
}

void testInitShutdown() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    cSC4RegionalCity::mbStaticInitialized = false;
    CHECK(city.Init());
    CHECK(city.Init());     // the second call does not subscribe again
    CHECK(cSC4RegionalCity::mbStaticInitialized);
    CHECK(host.notifications.size() == 5);
    CHECK(host.notifications[0] == "add:26c63342");
    CHECK(host.notifications[1] == "add:26c63343");
    CHECK(host.notifications[2] == "add:26d31ec1");
    CHECK(host.notifications[3] == "add:26d31ec3");
    CHECK(host.notifications[4] == "add:ca63d517");
    CHECK(city.Shutdown());
    CHECK(host.notifications.size() == 10);
    CHECK(host.notifications[5] == "remove:26c63342");
    CHECK(host.notifications[9] == "remove:ca63d517");
    CHECK(city.Shutdown());   // already shut down: no further removals
    CHECK(host.notifications.size() == 10);
}

void testAccessors() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    city.SetCitySize(2, 4);
    city.SetPosition(7, 9, false);
    city.SetCitySerialNumber(0x12345678);
    city.SetBirthDate(1234);
    city.SetEstablished(true);
    city.SetTutorialGUID(0x99);
    city.SetBudget(1234.5);
    city.SetIncome(-3.25);
    city.SetWorldPosition(12.5f, -7.25f);
    city.SetWorldBaseElevation(46.0f);
    city.SetCityName(cRZString("City"));
    city.SetMayorName(cRZString("Mayor"));
    city.SetUtilityAdvisorName(cRZString("Advisor"));
    city.SetCityDescription(cRZString("Description"));
    city.SetCitySaveFilePath(cRZString("C:/Cities/test.sc4"));
    int language = 1, country = 2;
    CHECK(city.GetOriginalLanguageAndCountry(language, country) && language == 0 && country == 0);
    CHECK(city.GetLastLanguageAndCountry(language, country) && language == 0 && country == 0);

    int width = 0, height = 0;
    city.GetCitySize(width, height);
    CHECK(width == 2 && height == 4);
    int x = 0, z = 0;
    city.GetPosition(x, z);
    CHECK(x == 7 && z == 9);
    CHECK(city.GetCitySerialNumber() == 0x12345678u);
    CHECK(city.GetBirthDate() == 1234u);
    CHECK(city.GetEstablished());
    CHECK(city.GetTutorialGUID() == 0x99);
    CHECK(city.IsTutorial());
    city.SetTutorialGUID(-1);
    CHECK(!city.IsTutorial());
    CHECK(city.GetBudget() == 1234.5);
    CHECK(city.GetIncome() == -3.25);
    float longitude = 0, latitude = 0;
    city.GetWorldPosition(longitude, latitude);
    CHECK(longitude == 12.5f && latitude == -7.25f);
    CHECK(city.GetWorldHemisphere() == 2);       // southern hemisphere for a negative latitude
    city.SetWorldPosition(1.0f, 2.0f);
    CHECK(city.GetWorldHemisphere() == 1);
    city.SetWorldPosition(1.0f, 0.0f);
    CHECK(city.GetWorldHemisphere() == 2);       // the original's `0.0 < latitude` test
    CHECK(city.GetWorldBaseElevation() == 46.0f);
    cRZString text;
    city.GetCitySaveFilePath(text);
    CHECK(std::string(text.ToChar()) == "C:/Cities/test.sc4");
    city.GetCityName(text);
    CHECK(std::string(text.ToChar()) == "City");
    city.GetMayorName(text);
    CHECK(std::string(text.ToChar()) == "Mayor");
    city.GetUtilityAdvisorName(text);
    CHECK(std::string(text.ToChar()) == "Advisor");
    city.GetCityDescription(text);
    CHECK(std::string(text.ToChar()) == "Description");
}

void testPopulationAndTax() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    CHECK(city.GetPopulation(0x3111) == 0);
    CHECK(city.GetExtrapolatedPopulation(0x3111) == city.GetPopulation());   // falls back to the no-arg getter
    CHECK(city.PopulationForTest().empty());

    // GetAllowableExtrapolation uses operator[] on both maps, so it inserts zero entries (quirk).
    CHECK(city.GetAllowableExtrapolation(0x3111) == 0);
    CHECK(city.PotentialPopulationForTest().count(0x3111) == 1);
    CHECK(city.ExtrapolatedPopulationForTest().count(0x3111) == 1);

    // A non-empty population goes through CalculatePotentialPopulations (max(1000, pop * 1.1)).
    host.localPopulationSummary = {{0x3111, 100}, {0x3121, 2000}, {0x3131, 0}};
    FakeCity fakeCity;
    host.currentCity = &fakeCity;
    city.UpdateCityCache(&fakeCity);
    CHECK(city.GetPopulation(0x3111) == 100);
    CHECK(city.GetPopulation(0x3121) == 2000);
    CHECK(city.GetPopulation(0x3131) == 0);
    cRZMessage2Standard message;
    message.SetType(kRegionalCityMessageCityStatistics);
    host.currentRegionalCity = &city;
    CHECK(city.DoMessage(&message));
    CHECK(city.PotentialPopulationForTest().at(0x3111) == 1000);
    CHECK(city.PotentialPopulationForTest().at(0x3121) == 2200);
    CHECK(city.PotentialPopulationForTest().at(0x3131) == 0);
    CHECK(city.GetExtrapolatedPopulation(0x3121) == 2000);
    CHECK(city.GetAllowableExtrapolation(0x3121) == 200);
    city.ExtrapolateGrowth(0x3121, 100.9f);
    CHECK(city.GetExtrapolatedPopulation(0x3121) == 2100);   // truncated toward zero
    city.ExtrapolateGrowth(0x3121, 100.9f);
    CHECK(city.GetAllowableExtrapolation(0x3121) == 0);       // clamped at zero
    city.ExtrapolateGrowth(0x3121, -5000.0f);
    CHECK(city.GetAllowableExtrapolation(0x3121) == 5000);   // 2200 - (-2800)

    // The tax-rate table.
    city.UpdateCityCache(&fakeCity);
    CHECK(city.GetTaxRate(0x1010) == 0);
    CHECK(city.GetTaxRate(0x1030) == 2);
    CHECK(city.GetTaxRate(0x3110) == 3);
    CHECK(city.GetTaxRate(0x3320) == 6);
    CHECK(city.GetTaxRate(0x4400) == 11);
    CHECK(city.GetTaxRate(0x4401) == 7.0f);
    CHECK(city.GetTaxRate(0x0) == 7.0f);

    // The statistics copied from the city object.
    CHECK(city.GetPopulation() == 4321);
    CHECK(city.GetWorkforcePercentage() == 0.77f);
    CHECK(city.GetMayorRating() == -5);
    CHECK(city.GetDifficultyLevel() == 2);
    CHECK(city.GetCitySerialNumber() == 0xabcdu);
    CHECK(city.GetEstablished());
    // 1.5 jobs per unit for every purpose code: 100 + 2000 + 0 population on the commercial codes.
    CHECK(city.GetCommercialJobs() == static_cast<int>(100 * 1.5f) + static_cast<int>(2000 * 1.5f));
    // The tutorial GUID comes from the tutorial system only while the field is still -1.
    CHECK(city.GetTutorialGUID() == 0x1234);
    city.SetTutorialGUID(7);
    city.UpdateCityCache(&fakeCity);
    CHECK(city.GetTutorialGUID() == 7);
    host.tutorialSystemPresent = false;
    city.SetTutorialGUID(-1);
    city.UpdateCityCache(&fakeCity);
    CHECK(city.GetTutorialGUID() == -1);
    host.tutorialSystemPresent = true;
    // An absent residential simulator leaves the two fields alone.
    host.populationStatsPresent = false;
    city.GetPopulation();
    const int before = city.GetPopulation();
    city.UpdateCityCache(&fakeCity);
    CHECK(city.GetPopulation() == before);
    host.populationStatsPresent = true;
}

void testCommodityStats() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    city.SetProduced(4, 10.0);
    city.SetDemanded(4, 20.0);
    city.SetExported(4, 5.0);
    city.SetImported(4, 7.0);
    city.SetCostPerUnit(4, 2.5);
    CHECK(city.GetProduced(4) == 10.0);
    CHECK(city.GetDemanded(4) == 20.0);
    CHECK(city.GetExported(4) == 5.0);
    CHECK(city.GetImported(4) == 7.0);
    CHECK(city.GetCostPerUnit(4) == 2.5);
    CHECK(city.GetCommodityBalance(4) == (10.0 + 7.0) - (20.0 + 5.0));   // -8
    // Negative amounts are clamped to zero.
    city.SetProduced(4, -1.0);
    city.SetDemanded(4, -1.0);
    city.SetExported(4, -1.0);
    city.SetImported(4, -1.0);
    city.SetCostPerUnit(4, -1.0);
    CHECK(city.GetProduced(4) == 0.0 && city.GetCostPerUnit(4) == 0.0);
    // Reading a missing commodity inserts a zeroed record (quirk).
    CHECK(city.CommodityStatsForTest().count(9) == 0);
    CHECK(city.GetExported(9) == 0.0);
    CHECK(city.CommodityStatsForTest().count(9) == 1);

    // The deals' other side comes from the neighbour-deal seam.
    cSC4RegionalCity other(&host);
    g_dealHost.cityAtCell = &other;

    // Lower types book the amount as imported when the other side is not this city.
    city.UpdateImportExportForTest(MakeDeal(0x4773ffe0, 0, 0, 1, 1, 40).get());
    CHECK(city.GetImported(1) == 40.0);
    CHECK(city.GetExported(1) == 0.0);
    // Lower type whose other side is this city: exported.
    g_dealHost.cityAtCell = &city;
    city.UpdateImportExportForTest(MakeDeal(0x4773ffe2, 0, 0, 1, 1, 5).get());
    CHECK(city.GetExported(2) == 5.0);
    // Upper types swap the two figures.
    g_dealHost.cityAtCell = &other;
    city.UpdateImportExportForTest(MakeDeal(0x4773ffe3, 0, 0, 1, 1, 6).get());
    CHECK(city.GetExported(2) == 11.0);
    // A silent deal is ignored (flag 0x80).
    auto silent = MakeDeal(0x4773ffe0, 0, 0, 1, 1, 99);
    silent->SetFlags(sc4::kDealFlagSilent, true);
    city.UpdateImportExportForTest(silent.get());
    CHECK(city.GetImported(1) == 40.0);
    // Types outside the deal range are ignored.
    city.UpdateImportExportForTest(MakeDeal(0x4773ffe6, 0, 0, 1, 1, 5).get());
    CHECK(city.GetImported(1) == 40.0);

    // UpdateImportExport() zeroes imported/exported for commodities 1..3 and then rebooks every deal.
    CHECK(city.UpdateImportExport());
    CHECK(city.GetImported(1) == 0.0);
    CHECK(city.GetExported(1) == 0.0);
}

void testConnections() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    auto* fake = new FakeConnection();
    fake->type = 7;
    fake->cellX = 3;
    fake->cellZ = 4;
    NeighborConnectionList list;
    list.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(fake));
    host.cityConnections = list;
    FakeCity fakeCity;
    host.currentCity = &fakeCity;
    city.UpdateCityCache(&fakeCity);
    CHECK(city.ConnectionsForTest().size() == 1);

    CHECK(city.FindConnection(7, 3, 4) != nullptr);
    CHECK(city.FindConnection(7, 4, 3) == nullptr);
    CHECK(city.FindConnection(8, 3, 4) == nullptr);

    NeighborConnectionList all;
    all.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(fake));
    all.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(fake));
    city.GetAllConnections(all);     // clears first
    CHECK(all.size() == 1);

    // RepairConnectionsAfterImport repairs every connection.
    fake->repairs = 0;
    city.SetPosition(10, 20, true);
    CHECK(fake->repairs == 1);
    CHECK(city.ConnectionChangesForTest().empty());

    // ChangeSymmetricConnection queues the converted cell.
    fake->cellX = 5;
    fake->cellZ = 6;
    city.ChangeSymmetricConnection(fake, true);
    CHECK(city.ConnectionChangesForTest().size() == 1);
    CHECK(city.ConnectionChangesForTest()[0].type == 7);
    CHECK(city.ConnectionChangesForTest()[0].x == 105);
    CHECK(city.ConnectionChangesForTest()[0].z == 206);
    CHECK(city.ConnectionChangesForTest()[0].flag == true);

    // MarkConnectionsForRemoval queues the inner cell of every connection whose other side is established.
    fake->innerX = 1;
    fake->innerZ = 2;
    host.findCityResult = &city;              // any non-null regional city
    host.regionalEstablished = true;
    host.currentRegionalCity = &city;
    cRZMessage2Standard changed;
    changed.SetType(kRegionalCityMessageCityChanged);
    changed.SetData3(1);                      // data3 != 0 takes the first branch
    host.pass.clear();
    CHECK(city.DoMessage(&changed));
    // The handler clears the pending list first, then MarkConnectionsForRemoval queues one entry (type 7 at 1,2, a removal), which
    // CreateSymmetricConnections applies at once: it asks for the light rail tool (network 8) and, with none, skips the entry.
    CHECK(city.ConnectionChangesForTest().empty());
    CHECK(host.pass.size() == 1 && host.pass[0] == "tool 8");

    // TreatAllIncomingConnectionsAsNew records the connections of adjacent cities whose other side is this one.
    auto* adjacency = new FakeRegionalCity();
    host.adjacentCities.clear();
    host.adjacentCities.emplace_back(adjacency);
    auto* incoming = new FakeConnection();
    incoming->type = 3;
    incoming->otherCity = &city;
    incoming->otherX = 130;
    incoming->otherZ = 240;
    NeighborConnectionList incomingList;
    incomingList.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(incoming));
    host.regionalConnections = incomingList;
    host.pass.clear();
    CHECK(city.DoMessage(&changed));
    // ...and the incoming highway (type 3 at 30,40, an add) after it, along the edge: the highway tool (network 2) is asked next.
    CHECK(city.ConnectionChangesForTest().empty());
    CHECK(host.pass.size() == 2 && host.pass[0] == "tool 8" && host.pass[1] == "tool 2");
    host.regionalConnections.clear();

    // Not established, or no city: MarkConnectionsForRemoval queues nothing.
    host.regionalEstablished = false;
    CHECK(city.DoMessage(&changed));
    CHECK(city.ConnectionChangesForTest().empty());
    host.regionalEstablished = true;
    host.currentCity = nullptr;
    CHECK(city.DoMessage(&changed));
    CHECK(city.ConnectionChangesForTest().empty());
    fake->Release();   // the fakes start with the test's own reference
    adjacency->Release();
    incoming->Release();
}

// CreateSymmetricConnections (spec regional-city-records.md 4, i386 0x002cc2d0) on a 64 x 64 city with the tools present.
void testSymmetricConnections() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    host.cellsX = host.cellsZ = 64;
    host.toolOk = true;
    using Change = cSC4RegionalCity::ConnectionChange;
    auto join = [&] { std::string all; for (auto& line : host.pass) all += line + "|"; return all; };
    // a west road: its strip (0..2, 5) cleared on layer 1, the connection made, the strip laid, the border byte (West) := 2, laid again
    city.RunSymmetricConnectionsForTest({Change{2, -1, 5, true}});
    CHECK(join() == "tool 0|demolish layer 1: 0,5 1,5 2,5|init|create 2 -1,5 dir 0|place 0,5-2,5|place 0,5-2,5|end|");
    CHECK(host.edges[std::make_pair(0, 5)] == 0x02u && city.ConnectionChangesForTest().empty());
    // an existing connection is not added twice; cells the traffic map already holds are not demolished
    host.pass.clear();
    city.RunSymmetricConnectionsForTest({Change{2, -1, 5, true}});
    CHECK(join() == "tool 0|end|");
    host.pass.clear();
    host.trafficCells = {{1, 9}};
    city.RunSymmetricConnectionsForTest({Change{2, -1, 9, true}});
    CHECK(host.pass.size() >= 2 && host.pass[1] == "demolish layer 1: 0,9 2,9");
    // a highway pair on the west edge: the first half only makes its connection (direction false), the second gets the strip (true)
    host.pass.clear();
    city.RunSymmetricConnectionsForTest({Change{3, -1, 11, true}, Change{3, -1, 10, true}});
    CHECK(join() == "tool 2|create 3 -1,10 dir 0|end|tool 2|demolish layer 1: 0,11 1,11 2,11|init|create 3 -1,11 dir 1|place 0,11-2,11|place 0,11-2,11|end|");
    // the last change of a cell wins: add then remove leaves the removal, which deletes the link, closes the border byte, re-solves the inner cell
    host.pass.clear();
    host.edges[{0, 5}] = 0x00000402u;   // West active, North grouped
    host.edges[{0, 4}] = 0x00000002u;
    city.RunSymmetricConnectionsForTest({Change{2, -1, 5, true}, Change{2, -1, 5, false}});
    CHECK(join() == "tool 0|delete 2 -1,5|init|place 0,5-0,5|end|");
    CHECK(host.edges[std::make_pair(0, 5)] == 0x00000400u && host.edges[std::make_pair(0, 4)] == 0x0u);   // the North group carries the West byte to 0,4
    // an east pipe (layer 3) and a north subway (layer 2); a ferry (no network) only creates its connection, even when removed
    host.pass.clear();
    city.RunSymmetricConnectionsForTest({Change{5, 64, 20, true}, Change{6, 30, -1, true}, Change{9, -1, 40, false}});
    CHECK(join() == "create 9 -1,40 dir 0|tool 7|demolish layer 2: 30,0 30,1 30,2|init|create 6 30,-1 dir 0|place 30,0-30,2|place 30,0-30,2|end|"
                    "tool 4|demolish layer 3: 61,20 62,20 63,20|init|create 5 64,20 dir 0|place 63,20-61,20|place 63,20-61,20|end|");
    // a power pole connection dezones the cleared cells; the queue ends empty either way
    host.pass.clear();
    city.RunSymmetricConnectionsForTest({Change{4, 10, 64, true}});
    CHECK(join().find("dezone 3|create 4 10,64") != std::string::npos && city.ConnectionChangesForTest().empty());
}

void testDeals() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    FakeCity fakeCity;
    FakeRegionalCity neighbour;
    host.currentCity = &fakeCity;
    host.currentRegionalCity = &city;
    host.regionCities[{0, 0}] = &city;
    host.regionCities[{1, 1}] = &neighbour;

    // SetLocalDeals extracts the deals that involve this city and drops the ones whose cells have no city.
    NeighborDealList list;
    list.push_back(MakeDeal(0x4773ffe0, 0, 0, 1, 1, 12));   // this <-> neighbour: local
    list.push_back(MakeDeal(0x4773ffe0, 5, 5, 6, 6, 99));   // no cities at those cells: dropped
    CHECK(city.SetLocalDeals(list));
    CHECK(city.LocalDealsForTest().size() == 1);
    CHECK(city.LocalDealsForTest().front()->GetAmount() == 12);
    // removeEmpty == false keeps the surviving deal in the caller's list, but the dangling one is always dropped.
    CHECK(list.size() == 1);

    NeighborDealList out;
    CHECK(city.GetLocalDeals(out));
    CHECK(out.size() == 1);

    // DoMessage 0x26c63342 moves the incoming deals into the main list, back to front.
    NeighborDealList incoming;
    incoming.push_back(MakeDeal(0x4773ffe0, 0, 0, 1, 1, 1));
    incoming.push_back(MakeDeal(0x4773ffe1, 0, 0, 1, 1, 2));
    city.SetLocalDeals(incoming);
    CHECK(city.LocalDealsForTest().size() == 2);
    cRZMessage2Standard message;
    message.SetType(kRegionalCityMessageDealsChanged);
    host.callLog.clear();
    CHECK(city.DoMessage(&message));
    CHECK(city.LocalDealsForTest().empty());
    CHECK(city.DealsForTest().size() == 2);
    if (city.DealsForTest().size() == 2) {
        CHECK(city.DealsForTest().front()->GetAmount() == 2);   // back() first
        CHECK(city.DealsForTest().back()->GetAmount() == 1);
    }
    CHECK(host.callLog.empty());   // the handler does not touch the city object

    // UpdateLocalDeals moves the local deals out of the main list and clears the silent flag on what is left.
    CHECK(city.UpdateLocalDeals());
    CHECK(city.DealsForTest().empty());
    CHECK(city.LocalDealsForTest().size() == 2);

    // Without the city or the neighbours simulator ExtractLocalDeals does nothing.
    host.currentCity = nullptr;
    city.SetLocalDeals(list);
    CHECK(city.LocalDealsForTest().empty());
    host.currentCity = &fakeCity;
    host.hasNeighbors = false;
    city.SetLocalDeals(list);
    CHECK(city.LocalDealsForTest().empty());
    host.hasNeighbors = true;

    // 0x26d31ec3 moves the deals only when its void-1 is the current city.
    city.SetLocalDeals(incoming);
    CHECK(city.LocalDealsForTest().size() == 2);
    cRZMessage2Standard init2;
    init2.SetType(kRegionalCityMessageCityInit2);
    init2.SetVoid1(&neighbour);
    CHECK(city.DoMessage(&init2));
    CHECK(city.LocalDealsForTest().size() == 2);
    init2.SetVoid1(&fakeCity);
    CHECK(city.DoMessage(&init2));
    CHECK(city.LocalDealsForTest().empty());
    CHECK(city.DealsForTest().size() == 2);
}

void testPreferencesAndSetupCity() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    host.cityName = "Host City";
    host.cityMayorName = "Host Mayor";
    FakeCity fakeCity;
    host.currentCity = &fakeCity;
    city.UpdateCityCache(&fakeCity);
    city.SetCityName(cRZString("New City"));
    city.SetMayorName(cRZString("New Mayor"));
    host.cityDifficulty = 2;

    // The preferences round trip (strcpy, so the buffers must hold the names).
    SC4NewCityPreferences preferences{};
    city.SetupPreferences(&preferences);
    CHECK(std::string(preferences.cityName) == "New City");
    CHECK(std::string(preferences.mayorName) == "New Mayor");
    CHECK(preferences.difficultyLevel == 2);

    // UpdateCityCache(preferences) copies them back and rebuilds the save path from the old directory.
    city.SetCitySaveFilePath(cRZString("C:/Old/oldcity.sc4"));
    preferences.difficultyLevel = 1;
    std::strcpy(preferences.cityName, "Fresh City");
    std::strcpy(preferences.mayorName, "Fresh Mayor");
    city.UpdateCityCache(&preferences);
    cRZString text;
    city.GetCityName(text);
    CHECK(std::string(text.ToChar()) == "Fresh City");
    city.GetMayorName(text);
    CHECK(std::string(text.ToChar()) == "Fresh Mayor");
    CHECK(city.GetDifficultyLevel() == 1);
    city.GetCitySaveFilePath(text);
    CHECK(std::string(text.ToChar()) == "C:/Old/Fresh City.sc4");

    // SetupCity hands the record to a new city object.
    FakeCity target;
    host.callLog.clear();
    target.log = &host.callLog;
    city.SetCitySize(2, 4);
    city.SetupCity(&target);
    CHECK(std::string(target.savePath.ToChar()) == "C:/Cities/Fresh City_0.sc4");
    CHECK(host.setCitySizeX == 2048.0f);
    CHECK(host.setCitySizeZ == 4096.0f);
    const std::vector<std::string> expected = {"SetCityMayorName", "SetCityName", "SetCityDifficulty",
                                               "CreateSavePathForCity", "city:SetCitySaveFilePath",
                                               "SetCitySize"};
    CHECK(host.callLog == expected);
    CHECK(std::string(host.cityName.ToChar()) == "Fresh City");
    CHECK(std::string(host.cityMayorName.ToChar()) == "Fresh Mayor");
    CHECK(host.cityDifficulty == 1);
}

void testCityCache() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    FakeCity fakeCity;
    host.cityName = "Cache City";
    host.cityMayorName = "Cache Mayor";
    host.cityDifficulty = 0;
    host.citySerial = 0x11223344;
    host.cityEstablished = true;
    host.cityMayorRating = 12;
    host.cityTaxRates[0] = 1.5f;
    host.cityPopulation = 500;
    host.cityWorkforce = 0.25f;
    host.tutorialGUID = 0x77;
    host.localPopulationSummary = {{0x3111, 10}, {0x4101, 20}};
    host.jobsPerUnit[0] = 0;
    host.jobsPerUnit[1] = 1;
    host.jobsPerUnit[2] = 0;
    host.jobsPerUnit[3] = 0;
    host.airports.push_back({1, 2, 3});
    host.seaports.push_back({4, 5, 6});
    host.seaports.push_back({7, 8, 9});
    host.currentCity = &fakeCity;
    city.UpdateCityCache(&fakeCity);

    cRZString text;
    city.GetCityName(text);
    CHECK(std::string(text.ToChar()) == "Cache City");
    city.GetMayorName(text);
    CHECK(std::string(text.ToChar()) == "Cache Mayor");
    CHECK(city.GetDifficultyLevel() == 0);
    CHECK(city.GetCitySerialNumber() == 0x11223344u);
    CHECK(city.GetEstablished());
    CHECK(city.GetMayorRating() == 12);
    CHECK(city.GetTaxRate(0x1010) == 1.5f);
    CHECK(city.GetTaxRate(0x1020) == 1.0f);
    CHECK(city.GetPopulation() == 500);
    CHECK(city.GetWorkforcePercentage() == 0.25f);
    CHECK(city.GetTutorialGUID() == 0x77);
    CHECK(city.GetCommercialJobs() == 10);
    CHECK(city.GetIndustrialJobs() == 20);
    std::vector<cISC4RegionalCity::PointOfInterest> points;
    city.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestAirport, points);
    CHECK(points.size() == 1 && points[0].x == 1 && points[0].y == 2 && points[0].z == 3);
    points.clear();
    city.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestSeaport, points);
    CHECK(points.size() == 2);
    // Both types are inserted even when empty; an unknown type leaves the caller's vector untouched.
    FakeHost emptyHost;
    cSC4RegionalCity emptyCity(&emptyHost);
    emptyHost.currentCity = &fakeCity;
    emptyCity.UpdateCityCache(&fakeCity);
    points.assign(1, {});
    emptyCity.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestAirport, points);
    CHECK(points.empty());
    points.push_back({});
    emptyCity.GetPointsOfInterest(static_cast<cISC4RegionalCity::PointOfInterestType>(9), points);
    CHECK(points.size() == 1);
    CHECK(host.commodityStatsUpdates == 1);
    // The calls reached the host in the original's order.
    std::vector<std::string> expected = {"CityMayorName", "CityName", "CityDifficulty", "CitySerialNumber",
                                         "CityEstablished", "CityMayorRating", "CityTaxRates", "CityConnections",
                                         "CityUpdateCommodityStats", "CityTutorialGUID", "CityPopulationStats",
                                         "CityLocalPopulationSummary"};
    for (int i = 0; i < 9; ++i) expected.push_back("CityJobsPerUnitOfDemand");
    expected.push_back("AirportPointsOfInterest");
    expected.push_back("SeaportPointsOfInterest");
    CHECK(host.callLog == expected);
}

void testMessageHandler() {
    FakeHost host;
    cSC4RegionalCity city(&host);
    FakeCity fakeCity;
    host.currentCity = &fakeCity;
    host.currentRegionalCity = &city;
    host.cityName = "Message City";
    host.localPopulationSummary = {{0x3111, 50}};

    // 0x26d31ec1 refreshes the cache only for the current city.
    cRZMessage2Standard init;
    init.SetType(kRegionalCityMessageCityInit);
    host.callLog.clear();
    CHECK(city.DoMessage(&init));
    CHECK(host.callLog.size() == 23);
    cRZString text;
    city.GetCityName(text);
    CHECK(std::string(text.ToChar()) == "Message City");
    // Not the current city: nothing happens.
    host.currentRegionalCity = nullptr;
    host.callLog.clear();
    CHECK(city.DoMessage(&init));
    CHECK(host.callLog.empty());
    // No city at all: nothing happens either.
    host.currentRegionalCity = &city;
    host.currentCity = nullptr;
    CHECK(city.DoMessage(&init));
    CHECK(host.callLog.empty());
    host.currentCity = &fakeCity;

    // 0x26c63343 also recomputes the potential and extrapolated populations.
    cRZMessage2Standard statistics;
    statistics.SetType(kRegionalCityMessageCityStatistics);
    host.callLog.clear();
    CHECK(city.DoMessage(&statistics));
    CHECK(city.GetPopulation(0x3111) == 50);
    CHECK(city.PotentialPopulationForTest().at(0x3111) == 1000);
    CHECK(city.GetExtrapolatedPopulation(0x3111) == 50);

    // An unhandled message does nothing.
    cRZMessage2Standard unknownMessage;
    unknownMessage.SetType(0x12345678);
    host.callLog.clear();
    CHECK(city.DoMessage(&unknownMessage));
    CHECK(host.callLog.empty());

    // 0x26d31ec3 with the city as void-1 moves the incoming deals; with another pointer it only reads the
    // current regional city.
    host.regionCities[{0, 0}] = &city;
    FakeRegionalCity neighbour;
    host.regionCities[{1, 1}] = &neighbour;
    NeighborDealList local;
    local.push_back(MakeDeal(0x4773ffe0, 0, 0, 1, 1, 3));
    city.SetLocalDeals(local);
    CHECK(city.LocalDealsForTest().size() == 1);
    cRZMessage2Standard init2;
    init2.SetType(kRegionalCityMessageCityInit2);
    init2.SetVoid1(&fakeCity);
    CHECK(city.DoMessage(&init2));
    CHECK(city.LocalDealsForTest().empty());
    CHECK(city.DealsForTest().size() == 1);
    cRZMessage2Standard init3;
    init3.SetType(kRegionalCityMessageCityInit2);
    init3.SetVoid1(&neighbour);
    CHECK(city.DoMessage(&init3));
    CHECK(city.DealsForTest().size() == 1);
}

// Builds a fully populated city (through the cache path plus the setters) for the serialization tests.
void BuildFullCity(cSC4RegionalCity& city, FakeHost& host, FakeCity& fakeCity) {
    host.cityName = "Serial City";
    host.cityMayorName = "Serial Mayor";
    host.cityDifficulty = 3;
    host.citySerial = 0x0badf00d;
    host.cityEstablished = true;
    host.cityMayorRating = 8;
    for (int i = 0; i < 12; ++i) host.cityTaxRates[i] = static_cast<float>(i) * 0.5f;
    host.cityPopulation = 1234;
    host.cityWorkforce = 0.6f;
    host.tutorialGUID = 0x42;
    host.localPopulationSummary = {{0x3111, 100}, {0x3121, 2000}};
    host.jobsPerUnit[1] = 1.0f;
    host.jobsPerUnit[2] = 0.0f;
    host.jobsPerUnit[3] = 0.0f;
    host.airports.push_back({1.5f, 2.5f, 3.5f});
    host.seaports.push_back({-1.0f, -2.0f, -3.0f});
    host.stringRecordCityName = "Serial City";
    host.stringRecordMayorName = "Serial Mayor";
    host.currentCity = &fakeCity;
    city.UpdateCityCache(&fakeCity);
    city.SetCitySize(2, 4);
    city.SetPosition(6, 7, false);
    city.SetBirthDate(0x55667788);
    city.SetBudget(999.5);
    city.SetIncome(-12.25);
    city.SetWorldPosition(11.5f, -22.5f);
    city.SetWorldBaseElevation(33.0f);
    city.SetEstablished(true);
    city.SetUtilityAdvisorName(cRZString("Advisor"));
    city.SetCityDescription(cRZString("Description"));
    city.SetCitySaveFilePath(cRZString("C:/Saves/serial.sc4"));
    for (int commodity = 1; commodity <= 3; ++commodity) {
        city.SetProduced(commodity, 4.0);
        city.SetDemanded(commodity, 6.0);
        city.SetImported(commodity, 2.0);
        city.SetExported(commodity, 1.0);
        city.SetCostPerUnit(commodity, 0.75);
    }
    auto connection = MakeConnection(4, 1, 2, 130, 240, true);
    NeighborConnectionList list;
    list.push_back(connection);
    host.cityConnections = list;
    city.UpdateCityCache(&fakeCity);
    auto* fake = new FakeConnection();
    fake->type = 9;
    fake->cellX = 2;
    fake->cellZ = 3;
    city.ChangeSymmetricConnection(fake, false);
    fake->Release();
    city.ChangeSymmetricConnection(connection.get(), true);
    // The statistics message fills the potential/extrapolated maps.
    host.currentRegionalCity = &city;
    cRZMessage2Standard statistics;
    statistics.SetType(kRegionalCityMessageCityStatistics);
    city.DoMessage(&statistics);
    city.ExtrapolateGrowth(0x3121, 12.5f);
    city.GetAllowableExtrapolation(0x3111);
}

void testSerializationRoundTrip() {
    FakeHost host;
    host.tutorialMapping = 0;   // identity so the tutorial guid survives the read
    cSC4RegionalCity city(&host);
    city.Init();
    FakeCity fakeCity;
    BuildFullCity(city, host, fakeCity);
    CHECK(city.DealsForTest().empty());
    CHECK(city.LocalDealsForTest().empty());

    const std::vector<unsigned char> bytes = Serialize(city);
    CHECK(!bytes.empty());
    CHECK(bytes.size() > 40 && bytes[0] == 13 && bytes[1] == 0);    // version 13
    // The exact offset of every field of the identity block (see the layout comment in RegionalCity.h).
    const auto readSint32 = [&bytes](std::size_t offset) {
        std::int32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof value);
        return value;
    };
    const auto readFloat = [&bytes](std::size_t offset) {
        float value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof value);
        return value;
    };
    CHECK(readSint32(2) == 6);         // +0x20 x
    CHECK(readSint32(6) == 7);         // +0x24 z
    CHECK(readSint32(10) == 2);        // +0x28 width
    CHECK(readSint32(14) == 4);        // +0x2c height
    CHECK(readSint32(18) == 1234);     // +0x30 population
    CHECK(readSint32(22) == 2100);     // +0x34 commercial jobs
    CHECK(readSint32(26) == 0);        // +0x38 industrial jobs
    CHECK(readFloat(30) == 0.6f);      // +0x3c workforce
    CHECK(static_cast<std::int8_t>(bytes[34]) == 8);   // +0x40 mayor rating
    CHECK(bytes[35] == 3);             // +0x44 difficulty
    CHECK(bytes[36] == 0);             // +0xf0 connections-changed flag
    CHECK(static_cast<std::uint32_t>(readSint32(37)) == 0x0badf00du);   // +0xf4 serial number

    cSC4RegionalCity loaded(&host);
    PortMemoryIStream is(bytes.data(), bytes.size());
    CHECK(loaded.Read(is));
    CHECK(!is.GetError());

    // The serialized state of both objects must be identical: the record carries the whole state except the
    // save file path, which is not serialized.
    const std::vector<unsigned char> second = Serialize(loaded);
    CHECK(bytes == second);

    // Spot checks through the public interface.
    int x = 0, z = 0;
    loaded.GetPosition(x, z);
    CHECK(x == 6 && z == 7);
    int width = 0, height = 0;
    loaded.GetCitySize(width, height);
    CHECK(width == 2 && height == 4);
    CHECK(loaded.GetPopulation() == 1234);
    CHECK(loaded.GetWorkforcePercentage() == 0.6f);
    CHECK(loaded.GetMayorRating() == 8);
    CHECK(loaded.GetDifficultyLevel() == 3);
    CHECK(loaded.GetCitySerialNumber() == 0x0badf00du);
    CHECK(loaded.GetEstablished());
    CHECK(loaded.GetBirthDate() == 0x55667788u);
    CHECK(loaded.GetBudget() == 999.5);
    CHECK(loaded.GetIncome() == -12.25);
    CHECK(loaded.GetTutorialGUID() == 0x42);
    CHECK(loaded.GetTaxRate(0x1030) == 1.0f);
    cRZString text;
    loaded.GetCityName(text);
    CHECK(std::string(text.ToChar()) == "Serial City");
    loaded.GetMayorName(text);
    CHECK(std::string(text.ToChar()) == "Serial Mayor");
    loaded.GetUtilityAdvisorName(text);
    CHECK(std::string(text.ToChar()) == "Advisor");
    loaded.GetCityDescription(text);
    CHECK(std::string(text.ToChar()) == "Description");
    loaded.GetCitySaveFilePath(text);                 // not serialized
    CHECK(std::string(text.ToChar()).empty());
    CHECK(loaded.ConnectionsForTest().size() == 1);
    CHECK(loaded.FindConnection(4, 1, 2) != nullptr);
    CHECK(loaded.ConnectionChangesForTest().size() == 2);
    CHECK(loaded.ConnectionChangesForTest()[0].type == 9);
    CHECK(loaded.ConnectionChangesForTest()[0].flag == false);
    CHECK(loaded.ConnectionChangesForTest()[1].type == 4);
    CHECK(loaded.ConnectionChangesForTest()[1].flag == true);
    CHECK(loaded.GetCommercialJobs() == city.GetCommercialJobs());
    CHECK(loaded.GetIndustrialJobs() == city.GetIndustrialJobs());
    CHECK(loaded.PotentialPopulationForTest() == city.PotentialPopulationForTest());
    CHECK(loaded.ExtrapolatedPopulationForTest() == city.ExtrapolatedPopulationForTest());
    std::vector<cISC4RegionalCity::PointOfInterest> points;
    loaded.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestAirport, points);
    CHECK(points.size() == 1 && points[0].z == 3.5f);
    points.clear();
    loaded.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestSeaport, points);
    CHECK(points.size() == 1 && points[0].x == -1.0f);
    CHECK(loaded.GetCommodityBalance(1) == city.GetCommodityBalance(1));
    // Reading an empty stream fails (the version read leaves the error flag set).
    PortMemoryIStream empty(nullptr, 0);
    cSC4RegionalCity failed(&host);
    CHECK(!failed.Read(empty));
}

// Splits a version-13 record into the layout of an older version: `< 11` has no workforce float (version 10
// stores it as percent/100), `< 13` has no difficulty byte, `<= 11` has no points-of-interest block.
std::vector<unsigned char> RebuildAsVersion(const std::vector<unsigned char>& bytes, std::uint16_t version) {
    std::vector<unsigned char> out;
    out.push_back(static_cast<unsigned char>(version & 0xff));
    out.push_back(static_cast<unsigned char>(version >> 8));
    out.insert(out.end(), bytes.begin() + 2, bytes.begin() + 30);        // the seven 32-bit fields
    if (version == 10) {                                                 // workforce written as percent
        const float percent = 60.0f;                                     // 0.6
        const auto* raw = reinterpret_cast<const unsigned char*>(&percent);
        out.insert(out.end(), raw, raw + 4);
    } else if (version >= 11) {
        out.insert(out.end(), bytes.begin() + 30, bytes.begin() + 34);   // the workforce float
    }
    out.push_back(bytes[34]);                                            // the mayor rating byte
    if (version > 12) out.push_back(bytes[35]);                          // the difficulty byte
    out.insert(out.end(), bytes.begin() + 36, bytes.end());
    return out;
}

void testReadVersions() {
    FakeHost host;
    host.tutorialMapping = 0;
    host.loadedString = "Default City Name";
    cSC4RegionalCity source(&host);
    FakeCity fakeCity;
    BuildFullCity(source, host, fakeCity);
    host.stringRecordCityName = "Old City";
    host.stringRecordMayorName = "Old Mayor";
    const std::vector<unsigned char> bytes = Serialize(source);

    // Version 9: no workforce field (0.5), no difficulty byte (2 when established), no points of interest.
    {
        const std::vector<unsigned char> old = RebuildAsVersion(bytes, 9);
        cSC4RegionalCity city(&host);
        PortMemoryIStream is(old.data(), old.size());
        CHECK(city.Read(is));
        CHECK(city.GetWorkforcePercentage() == 0.5f);
        CHECK(city.GetDifficultyLevel() == 2);       // the version <= 12 default
        std::vector<cISC4RegionalCity::PointOfInterest> points;
        city.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestAirport, points);
        CHECK(points.empty());
        // The city name comes from the string record of the host.
        cRZString text;
        city.GetCityName(text);
        CHECK(std::string(text.ToChar()) == "Old City");
    }
    // Version 10: the workforce is stored as percent and scaled by 0.01.
    {
        const std::vector<unsigned char> old = RebuildAsVersion(bytes, 10);
        cSC4RegionalCity city(&host);
        PortMemoryIStream is(old.data(), old.size());
        CHECK(city.Read(is));
        CHECK(std::fabs(city.GetWorkforcePercentage() - 0.6f) < 1e-6f);
        CHECK(city.GetDifficultyLevel() == 2);
    }
    // Version 12: the workforce is a fraction, the difficulty byte is not read (< 13, so it defaults to 2) and
    // the points of interest are read (> 11).
    {
        const std::vector<unsigned char> old = RebuildAsVersion(bytes, 12);
        cSC4RegionalCity city(&host);
        PortMemoryIStream is(old.data(), old.size());
        CHECK(city.Read(is));
        CHECK(city.GetWorkforcePercentage() == 0.6f);
        CHECK(city.GetDifficultyLevel() == 2);
        std::vector<cISC4RegionalCity::PointOfInterest> points;
        city.GetPointsOfInterest(cISC4RegionalCity::kPointOfInterestSeaport, points);
        CHECK(points.size() == 1);
    }
    // Version 14 is not readable.
    {
        const std::vector<unsigned char> newer = RebuildAsVersion(bytes, 14);
        cSC4RegionalCity city(&host);
        PortMemoryIStream is(newer.data(), newer.size());
        CHECK(!city.Read(is));
    }
}

void testReadQuirks() {
    // Version 13 with an unestablished city: the difficulty byte does not stick, the default name loads and the
    // tutorial value goes through the registry's WhichToGUID.
    {
        FakeHost host;
        host.tutorialMapping = 1;              // value * 2 + 1
        host.loadedString = "Default City Name";
        host.cityEstablished = false;
        host.cityDifficulty = 3;                // deliberately different from the host's default (1)
        cSC4RegionalCity source(&host);
        FakeCity fakeCity;
        BuildFullCity(source, host, fakeCity);
        host.stringRecordCityName = "Loaded City";
        host.stringRecordMayorName = "Loaded Mayor";
        source.SetEstablished(false);
        source.SetTutorialGUID(5);
        const std::vector<unsigned char> bytes = Serialize(source);
        cSC4RegionalCity city(&host);
        CHECK(city.GetDifficultyLevel() == 1);  // the host's default
        PortMemoryIStream is(bytes.data(), bytes.size());
        CHECK(city.Read(is));
        CHECK(!city.GetEstablished());
        CHECK(city.GetDifficultyLevel() == 1);  // unchanged: only an established city takes the difficulty
        CHECK(city.GetTutorialGUID() == 11);    // 5 * 2 + 1 through the tutorial registry
        // The default name is loaded (not established) and then overridden by the string records.
        cRZString text;
        city.GetCityName(text);
        CHECK(std::string(text.ToChar()) == "Loaded City");
        city.GetMayorName(text);
        CHECK(std::string(text.ToChar()) == "Loaded Mayor");
        CHECK(host.loadStringCalls.size() == 1);
        if (host.loadStringCalls.size() == 1) {
            CHECK(host.loadStringCalls[0].first == kRegionalCityDefaultCityNameGroup);
            CHECK(host.loadStringCalls[0].second == kRegionalCityDefaultCityNameInstance);
        }
        CHECK(host.stringRecordCalls == 2);
        CHECK(host.lastStringRecord.first == kRegionalCityRecordIdMayorName);
        CHECK(host.lastStringRecord.second == static_cast<std::uint32_t>(host.uiLanguage));
    }
    // Both deal blocks are read into the main deal list: the local list is empty after a load.
    {
        FakeHost host;
        FakeCity fakeCity;
        FakeRegionalCity neighbour;
        host.currentCity = &fakeCity;
        cSC4RegionalCity source(&host);
        host.currentRegionalCity = &source;
        host.regionCities[{0, 0}] = &source;
        host.regionCities[{1, 1}] = &neighbour;
        NeighborDealList local;
        local.push_back(MakeDeal(0x4773ffe0, 0, 0, 1, 1, 7));
        local.push_back(MakeDeal(0x4773ffe1, 0, 0, 1, 1, 8));
        CHECK(source.SetLocalDeals(local));
        CHECK(source.LocalDealsForTest().size() == 2);
        const std::vector<unsigned char> bytes = Serialize(source);
        cSC4RegionalCity city(&host);
        PortMemoryIStream is(bytes.data(), bytes.size());
        CHECK(city.Read(is));
        CHECK(city.LocalDealsForTest().empty());
        CHECK(city.DealsForTest().size() == 2);
        if (city.DealsForTest().size() == 2) {
            CHECK(city.DealsForTest().front()->GetAmount() == 7);
            CHECK(city.DealsForTest().back()->GetAmount() == 8);
        }
    }
}

}  // namespace

int main() {
    sc4::SetNeighborDealHost(&g_dealHost);
    testConstructionAndCom();
    testInitShutdown();
    testAccessors();
    testPopulationAndTax();
    testCommodityStats();
    testConnections();
    testSymmetricConnections();
    testDeals();
    testPreferencesAndSetupCity();
    testCityCache();
    testMessageHandler();
    testSerializationRoundTrip();
    testReadVersions();
    testReadQuirks();
    sc4::SetNeighborDealHost(nullptr);
    if (failures == 0) {
        std::printf("sc4_regionalcity: all checks passed\n");
        return 0;
    }
    std::printf("sc4_regionalcity: %d check(s) failed\n", failures);
    return 1;
}
