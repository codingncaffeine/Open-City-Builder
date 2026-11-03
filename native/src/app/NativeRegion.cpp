#include "app/NativeRegion.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gz/cIGZMessage2.h"
#include "rz/cRZMessage2Standard.h"
#include "rz/cRZRandom.h"
#include "sc4/App.h"
#include "sc4/DBSegment.h"
#include "sc4/SaveGame.h"
#include "sc4/cSC4City.h"
#include "sc4/ServiceLocator.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/Region.h"
#include "sc4/RegionalCity.h"
#include "gz/cGZPersistResourceManager.h"

namespace native {

namespace {

cSC4App* g_app = nullptr;
RegionCallbacks g_callbacks;

cSC4RegionalCity* Concrete(const cISC4RegionalCity* city) { return static_cast<cSC4RegionalCity*>(const_cast<cISC4RegionalCity*>(city)); }

// Reads the regional city record of a city file (the region's view of the city: place, size, name, population).
bool ReadRegionalCityFile(const cIGZString& path, cISC4RegionalCity*& out) {
    out = nullptr;
    cIGZPersistDBSegment* segment = sc4::SC4OpenSaveGame(path, false, true);
    if (!segment) { if (std::getenv("SC4_DEBUG")) std::printf("region: cannot open %s\n", path.ToChar()); return false; }
    cRZAutoRefCount<cISC4RegionalCity> city;
    const bool ok = sc4::SC4ReadRegionalCity(segment, city);
    if (!ok && std::getenv("SC4_DEBUG")) std::printf("region: no regional city record in %s\n", path.ToChar());
    sc4::SC4CloseSaveGame(segment);
    if (!ok || !city) return false;
    out = city.get();
    out->AddRef();
    if (std::getenv("SC4_DEBUG")) {
        int x = 0, z = 0, w = 0, h = 0;
        Concrete(out)->GetPosition(x, z);
        Concrete(out)->GetCitySize(w, h);
        std::printf("region: city at (%d,%d) size %dx%d from %s\n", x, z, w, h, path.ToChar());
    }
    return true;
}

// ---- cSC4RegionalCity -----------------------------------------------------------------------------------------------------
// The live city (name, mayor, difficulty, serial, established, save path) is the app's open city: on the save-started message
// the application's regional city refreshes itself from it (UpdateCityCache) before cSC4App::Write stores it. What needs
// the simulators (rating, taxes, connections, populations, jobs) still answers neutrally and keeps the record's values.
sc4::cSC4City* LiveCity(const cISC4City* city) { return dynamic_cast<sc4::cSC4City*>(const_cast<cISC4City*>(city)); }

cISC4RegionalCity* RegionCity(int x, int z);
// SL::spNeighborsSim: the city's neighbours simulator, bound before the simulators' Init (SL::OnCityInit).
sc4::cSC4NeighborsSimulator* NeighborsSim() { return dynamic_cast<sc4::cSC4NeighborsSimulator*>(SL::spNeighborsSim); }

class CityHost final : public IRegionalCityHost {
public:
    std::uint32_t DefaultDifficultyLevel() override { return 0; }
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->AddNotification(target, type); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t type) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, type); }
    void SendMessage(std::uint32_t type) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        SL::spMS2->MessageSend(message);
        message->Release();
    }
    cISC4RegionalCity* CurrentRegionalCity() override { return g_app ? g_app->GetRegionalCity() : nullptr; }
    cISC4City* CurrentCity() override { return g_app ? g_app->GetCity() : nullptr; }
    cISC4RegionalCity* RegionCityAt(int x, int z) override { return RegionCity(x, z); }
    void ConvertConnectionCellToNeighborCell(int x, int z, int& neighborX, int& neighborZ, cISC4RegionalCity*& city, int& cellX, int& cellZ) override {
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighborCell(x, z, neighborX, neighborZ, &city, cellX, cellZ);
    }
    void ConvertNeighborCellToConnectionCell(cISC4RegionalCity* city, int x, int z, int& outX, int& outZ) override {
        sc4::cSC4NeighborsSimulator::ConvertNeighborCellToConnectionCell(city, x, z, outX, outZ);
    }
    bool HasNeighborsSimulator() override { return NeighborsSim() != nullptr; }
    cISC4RegionalCity* FindCityOnOtherSideOfConnection(int x, int z) override {
        const sc4::cSC4NeighborsSimulator* neighbors = NeighborsSim();
        return neighbors ? neighbors->FindCityOnOtherSideOfConnection(x, z) : nullptr;
    }
    bool RegionalCityEstablished(const cISC4RegionalCity* city) override { return Concrete(city)->GetEstablished(); }
    void RegionalCityConnections(const cISC4RegionalCity* city, NeighborConnectionList& out) override { Concrete(city)->GetAllConnections(out); }
    void GetAdjacentCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool includeUnestablished) override {
        if (const sc4::cSC4NeighborsSimulator* neighbors = NeighborsSim()) neighbors->GetAdjacentCities(out, includeUnestablished);
    }
    void CreateSavePathForCity(const cIGZString& cityName, std::uint32_t serial, cRZString& out) override {
        out = cRZString(cityName.ToChar());
        char tail[24];
        std::snprintf(tail, sizeof tail, " %08x.sc4", serial);
        out.AppendChars(tail, std::strlen(tail));
    }
    int TutorialGUIDFromStoredValue(int value) override { return value; }
    int UILanguage() override { return 0; }
    bool ReadStringRecord(cIGZPersistDBSegment* segment, std::uint32_t group, std::uint32_t instance, cIGZString& out) override {
        return segment && sc4::SC4ReadStringRecord(segment, group, instance, out);
    }
    bool LoadString(std::uint32_t, std::uint32_t, cIGZString&) override { return false; }
    void CityName(const cISC4City* city, cIGZString& out) override { if (auto* live = LiveCity(city)) live->GetCityName(out); else out.AssignFromCString(""); }
    void CityMayorName(const cISC4City* city, cIGZString& out) override { if (auto* live = LiveCity(city)) live->GetMayorName(out); else out.AssignFromCString(""); }
    void SetCityName(cISC4City* city, const cIGZString& name) override { if (auto* live = LiveCity(city)) live->SetCityName(name); }
    void SetCityMayorName(cISC4City* city, const cIGZString& name) override { if (auto* live = LiveCity(city)) live->SetMayorName(name); }
    std::uint32_t CityDifficulty(const cISC4City* city) override { auto* live = LiveCity(city); return live ? std::uint32_t(live->GetDifficultyLevel()) : 0; }
    void SetCityDifficulty(cISC4City* city, std::uint32_t level) override { if (auto* live = LiveCity(city)) live->SetDifficultyLevel(sc4::cSC4City::DifficultyLevel(level)); }
    void SetCitySize(cISC4City*, float, float) override {}
    std::uint32_t CitySerialNumber(const cISC4City* city) override { auto* live = LiveCity(city); return live ? live->GetCitySerialNumber() : 0; }
    bool CityEstablished(const cISC4City* city) override { auto* live = LiveCity(city); return live && live->GetEstablished(); }
    // (no live mayor rating yet: the record's value is kept)
    int CityMayorRating(const cISC4City*) override { cISC4RegionalCity* current = CurrentRegionalCity(); return current ? Concrete(current)->GetMayorRating() : 0; }
    void CityTaxRates(const cISC4City*, float (&rates)[12]) override { for (float& r : rates) r = 7.0f; }
    void CityConnections(const cISC4City*, NeighborConnectionList&) override {}
    void CityUpdateCommodityStats(cISC4City*) override {}
    bool CityTutorialGUID(const cISC4City*, int&) override { return false; }
    bool CityPopulationStats(const cISC4City*, int&, float&) override { return false; }
    void CityLocalPopulationSummary(const cISC4City*, std::map<std::uint32_t, int>&) override {}
    void CityJobsPerUnitOfDemand(const cISC4City*, float*, std::uint32_t) override {}
    void AirportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>&) override {}
    void SeaportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>&) override {}
} g_cityHost;

// ---- cSC4Region -----------------------------------------------------------------------------------------------------------
class RegionHost final : public IRegionHost {
public:
    void GetRegionDirectory(cRZString& out) override { out = cSC4Nation::msRegionDirectory; }
    void GetRegionDownloadDirectory(cRZString& out) override { out = cSC4Nation::msRegionDownloadDirectory; }
    bool ReadRegionalCity(const cIGZString& path, cISC4RegionalCity** out) override { return ReadRegionalCityFile(path, *out); }
    bool WriteRegionalCity(const cIGZString&, cISC4RegionalCity*) override { return false; }
    void CreateSavePathForCity(const cIGZString& directory, const cIGZString& cityName, std::uint32_t serial, cRZString& out) override {
        out = cRZString(directory.ToChar());
        out.AppendChars(cityName.ToChar(), std::strlen(cityName.ToChar()));
        char tail[24];
        std::snprintf(tail, sizeof tail, " %08x.sc4", serial);
        out.AppendChars(tail, std::strlen(tail));
    }
    void CreateSavePathForCity(const cIGZString& cityName, std::uint32_t serial, cRZString& out) override {
        out = cRZString(cityName.ToChar());
        char tail[24];
        std::snprintf(tail, sizeof tail, " %08x.sc4", serial);
        out.AppendChars(tail, std::strlen(tail));
    }
    std::uint32_t CreateCitySerialNumber() override { return sc4::SC4CreateCitySerialNumber(); }
    int ReadBinaryRecordToFile(const cGZPersistResourceKey&, const cIGZString&) override { return -1; }
    bool HasResource(const cGZPersistResourceKey& key) override { cGZPersistResourceKey copy = key; return SL::spRM && SL::spRM->TestForKey(copy); }
    std::uint32_t RandomUint32UniformBelow(std::uint32_t range) override {
        static cRZRandom random(0x5c4);
        return random.RandomUint32Uniform(range);
    }
    void SetTutorialActive(int, bool) override {}
    void GetCityPosition(const cISC4RegionalCity* city, int& x, int& z) override { Concrete(city)->GetPosition(x, z); }
    void GetCitySize(const cISC4RegionalCity* city, int& w, int& h) override { Concrete(city)->GetCitySize(w, h); }
    void SetCityPosition(cISC4RegionalCity* city, int x, int z, bool fromImport) override { Concrete(city)->SetPosition(x, z, fromImport); }
    std::uint32_t GetCitySerialNumber(const cISC4RegionalCity* city) override { return Concrete(city)->GetCitySerialNumber(); }
    void SetCitySerialNumber(cISC4RegionalCity* city, std::uint32_t serial) override { Concrete(city)->SetCitySerialNumber(serial); }
    void GetCitySaveFilePath(const cISC4RegionalCity* city, cRZString& out) override {
        cRZString path;
        Concrete(city)->GetCitySaveFilePath(path);
        out = path;
    }
    void SetCitySaveFilePath(cISC4RegionalCity* city, const cIGZString& path) override { Concrete(city)->SetCitySaveFilePath(path); }
    void GetCityName(const cISC4RegionalCity* city, cRZString& out) override {
        cRZString name;
        Concrete(city)->GetCityName(name);
        out = name;
    }
    std::uint32_t GetTutorialGUID(const cISC4RegionalCity* city) override { return static_cast<std::uint32_t>(Concrete(city)->GetTutorialGUID()); }
    void SetTutorialGUID(cISC4RegionalCity* city, std::uint32_t guid) override { Concrete(city)->SetTutorialGUID(static_cast<int>(guid)); }
} g_regionHost;

// ---- cSC4Nation -----------------------------------------------------------------------------------------------------------
class NationHost final : public INationHost {
public:
    bool ApplicationPresent() override { return g_app != nullptr; }
    void RegionsDirectory(cIGZString& out) override { g_app->GetRegionsDirectory(out); }
    std::uint8_t* Preferences() override { return g_app->GetPreferences(); }
    bool IsRunFirstTimeAfterInstall() override { return false; }
    int TutorialLanguageIndex() override { return 0; }
    void TutorialRegionDirectoryNames(std::list<cRZString>&) override {}
    cRZString TutorialRegionDefaultDirName(int) override { return cRZString(); }
    void TutorialItems(std::list<NationTutorialItem>&, cISC4Region*) override {}
    void SetTutorialActive(std::int32_t, bool) override {}
    bool IsRegionScreenCurrent() override { return g_callbacks.isRegionScreenCurrent && g_callbacks.isRegionScreenCurrent(); }
    void DestroyCurrentScreen() override { if (g_callbacks.destroyCurrentScreen) g_callbacks.destroyCurrentScreen(); }
    void CreateRegionScreen() override { if (g_callbacks.createRegionScreen) g_callbacks.createRegionScreen(); }
    void SimStartDate(cIGZDate&) override {}
    cIGZDate* SimDate() override { return nullptr; }
    bool CityPresent() override { return false; }
    int CityDifficultyLevel() override { return 0; }
    sc4::cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey&) override { return nullptr; }
    void ReleaseExemplar(sc4::cISCPropertyHolder*) override {}
    void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->AddNotification(target, message); }
    void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) override { if (SL::spMS2) SL::spMS2->RemoveNotification(target, message); }
    void DataDirectory(cIGZString& out) override { out.AssignFromCString(g_callbacks.dataDirectory.c_str()); }
    bool LanguageDirectoryName(cIGZString&, std::uint32_t) override { return false; }
    int ReadBinaryRecord(const cGZPersistResourceKey&, const cIGZString&) override { return -1; }
    bool ReadRegionalCity(const cIGZString& path, cISC4RegionalCity*& out) override { return ReadRegionalCityFile(path, out); }
    void CityPosition(cISC4RegionalCity* city, int& x, int& z) override { Concrete(city)->GetPosition(x, z); }
    void SetCitySaveFilePath(cISC4RegionalCity* city, const cIGZString& path) override { Concrete(city)->SetCitySaveFilePath(path); }
    std::int32_t CityTutorialGUID(cISC4RegionalCity* city) override { return Concrete(city)->GetTutorialGUID(); }
} g_nationHost;


// ---- the neighbours simulator and its connections -----------------------------------------------------------------------------
// The region side of the city's border connections: the cities around this one (the region's cities by region cell), their saved
// connections and the cell conversions, so the neighbours simulator and the traffic simulator find the city and the connection on
// the other side. PARTIAL: the deal pass (power / water / garbage trades) is not run natively (no simulator agent is registered and
// every budget / utility seam answers neutrally), and no connection occupants are created or moved.
cISC4RegionalCity* RegionCity(int x, int z) {
    cISC4Region* region = g_app ? g_app->GetRegion() : nullptr;
    cRZAutoRefCount<cISC4RegionalCity>* entry = region ? region->GetCity(x, z) : nullptr;
    return entry ? entry->get() : nullptr;
}
int CurrentCellsX() { cISC4City* city = g_app ? g_app->GetCity() : nullptr; auto* live = LiveCity(city); return live ? live->CellCountX() : 0; }
int CurrentCellsZ() { cISC4City* city = g_app ? g_app->GetCity() : nullptr; auto* live = LiveCity(city); return live ? live->CellCountZ() : 0; }

class NeighborsHost final : public sc4::INeighborsHost {
public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cISC4City* City() override { return g_app ? g_app->GetCity() : nullptr; }
    cISC4RegionalCity* CurrentRegionalCity() override { return g_app ? g_app->GetRegionalCity() : nullptr; }
    sc4::cISCResExemplar* TuningExemplar() override {
        void* raw = nullptr;
        const cGZPersistResourceKey key{0x6534284a, 0x7a4a8458, 0x6a469021};
        return SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) ? static_cast<sc4::cISCResExemplar*>(raw) : nullptr;
    }
    void AddSimulatorAgent(cIGZMessageTarget2*, int, const char*) override {}   // PARTIAL: the deal pass does not run natively
    void RemoveSimulatorAgent(cIGZMessageTarget2*) override {}
    void SimDate(cIGZDate&) override {}
    void PostMessage(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, cIGZUnknown*) override {}
    void PostDealStatusMessage(std::uint32_t, cIGZUnknown*) override {}
    cISC4RegionalCity* RegionCityAt(int x, int z) override { return RegionCity(x, z); }
    void ReloadCityAt(int, int) override {}
    int CityCellCountX() override { return CurrentCellsX(); }
    int CityCellCountZ() override { return CurrentCellsZ(); }
    void CityCellCenterToPosition(int x, int z, float& outX, float& outZ) override { outX = (float(x) + 0.5f) * 16.0f; outZ = (float(z) + 0.5f) * 16.0f; }
    void CityPositionToCell(float x, float z, int& outX, int& outZ) override { outX = int(std::floor(x / 16.0f)); outZ = int(std::floor(z / 16.0f)); }
    float TerrainAltitudeAtCell(int, int) override { return 0.0f; }
    float TerrainAltitudeAtPosition(float, float) override { return 0.0f; }
    void RecordHistory(std::uint32_t, int) override {}
    double BudgetTotal() override { return 0.0; }
    double BudgetIncome() override { return 0.0; }
    bool BudgetLineItemAmount(std::uint32_t, double&) override { return false; }
    bool BudgetSubLineItemAmount(std::uint32_t, std::uint32_t, double&) override { return false; }
    int PowerCapacity() override { return 0; }
    int PowerDemand() override { return 0; }
    int WaterCapacity() override { return 0; }
    int WaterDemand() override { return 0; }
    void LandfillRange(int& start, int& end) override { start = end = 0; }
    int PollutionValue(int) override { return 0; }
    void OccupantsInRect(int, int, int, int, std::vector<sc4::cISC4Occupant*>&) override {}
    void DeleteProp(sc4::cISC4Occupant*) override {}
    bool CreateProp(std::uint32_t, void** out) override { if (out) *out = nullptr; return false; }
    void FinishProp(void*) override {}
    void SetPropPosition(void*, const float[3]) override {}
    void SetPropEdgeFlags(void*, std::uint32_t) override {}
    void AddPropConnectionProperties(void*, std::uint32_t) override {}
    void ReleaseProp(void*) override {}
    void ReleaseOccupantList(std::vector<sc4::cISC4Occupant*>&) override {}
    void DeactivateOccupant(sc4::cISC4Occupant*) override {}
    const sc4::cISCPropertyHolder* OccupantProperties(sc4::cISC4Occupant*) override { return nullptr; }
    void OccupantPosition(sc4::cISC4Occupant*, float& x, float& z) override { x = z = 0.0f; }
    void SetOccupantPosition(sc4::cISC4Occupant*, const float[3]) override {}
    std::uint32_t OccupantType(sc4::cISC4Occupant*) override { return 0; }
    bool OccupantHasNetworkType(sc4::cISC4Occupant*, std::uint32_t) override { return false; }
    bool OccupantRetained(sc4::cISC4Occupant*) override { return true; }
    void CityPosition(const cISC4RegionalCity* city, int& x, int& z) override { Concrete(city)->GetPosition(x, z); }
    void CitySize(const cISC4RegionalCity* city, int& w, int& h) override { Concrete(city)->GetCitySize(w, h); }
    bool CityEstablished(const cISC4RegionalCity* city) override { return Concrete(city)->GetEstablished(); }
    void CityName(const cISC4RegionalCity* city, cIGZString& out) override {
        cRZString name;
        Concrete(city)->GetCityName(name);
        out.AssignFromCString(name.ToChar());
    }
    double CityGetBudget(const cISC4RegionalCity*) override { return 0.0; }
    void CitySetBudget(cISC4RegionalCity*, double) override {}
    void CitySetIncome(cISC4RegionalCity*, double) override {}
    sc4::cISC4NeighborConnection* CityFindConnection(const cISC4RegionalCity* city, int type, int x, int z) override {
        sc4::cISC4NeighborConnection* found = Concrete(city)->FindConnection(type, x, z);
        static const bool trace = std::getenv("SC4_TRAFFICDEBUG") != nullptr;
        if (trace) {
            NeighborConnectionList all;
            Concrete(city)->GetAllConnections(all);
            std::fprintf(stderr, "neighbours: other city has %zu connections; type %d at (%d,%d): %s", all.size(), type, x, z, found ? "found" : "none");
            for (auto& ref : all) {
                int cx = 0, cz = 0;
                ref->GetConnectionCell(cx, cz);
                std::fprintf(stderr, " [%u (%d,%d)]", ref->GetType(), cx, cz);
            }
            std::fprintf(stderr, "\n");
        }
        return found;
    }
    void CityAllConnections(const cISC4RegionalCity* city, sc4::ConnectionList& out) override { Concrete(city)->GetAllConnections(out); }
    void CityChangeSymmetricConnection(cISC4RegionalCity*, sc4::cISC4NeighborConnection*, bool) override {}
    // The region city's own deal list (cSC4RegionalCity vslots 0x220 / 0x228 / 0x230).
    void CityUpdateLocalDeals(cISC4RegionalCity* city) override { if (city) Concrete(city)->UpdateLocalDeals(); }
    void CitySetLocalDeals(cISC4RegionalCity* city, const sc4::DealList& deals) override {
        if (!city) return;
        NeighborDealList copy(deals.begin(), deals.end());
        Concrete(city)->SetLocalDeals(copy);
    }
    void CityGetLocalDeals(cISC4RegionalCity* city, sc4::DealList& out) override {
        if (!city) return;
        NeighborDealList deals;
        Concrete(city)->GetLocalDeals(deals);
        out.assign(deals.begin(), deals.end());
    }
    void CityUpdateImportExport(cISC4RegionalCity*) override {}
    double CityGetExported(const cISC4RegionalCity*, int) override { return 0.0; }
    void CitySetExported(cISC4RegionalCity*, int, double) override {}
    double CityGetImported(const cISC4RegionalCity*, int) override { return 0.0; }
    void CitySetImported(cISC4RegionalCity*, int, double) override {}
    double CityGetProduced(const cISC4RegionalCity*, int) override { return 0.0; }
    void CitySetProduced(cISC4RegionalCity*, int, double) override {}
    double CityGetDemanded(const cISC4RegionalCity*, int) override { return 0.0; }
    void CitySetDemanded(cISC4RegionalCity*, int, double) override {}
    double CityGetCostPerUnit(const cISC4RegionalCity*, int) override { return 0.0; }
    void CitySetCostPerUnit(cISC4RegionalCity*, int, double) override {}
    bool ConfirmationDialog(std::uint32_t, std::uint32_t, std::uint32_t, const cIGZString&) override { return false; }
} g_neighborsHost;

class ConnectionHost final : public sc4::INeighborConnectionHost {
public:
    int CityCellsX() override { return CurrentCellsX(); }
    int CityCellsZ() override { return CurrentCellsZ(); }
    bool CityEntryAtCell(int x, int z, void*& city) override {
        city = RegionCity(x, z);
        return city != nullptr;
    }
    void ConvertConnectionCell(int x, int z, int& outX, int& outZ, void* regionalCity) override {
        sc4::cSC4NeighborsSimulator::ConvertConnectionCellToNeighboringRegionCell(x, z, outX, outZ, static_cast<cISC4RegionalCity*>(regionalCity));
    }
} g_connectionHost;

}  // namespace

cISC4RegionalCity* NativeCurrentRegionalCity() { return g_app ? g_app->GetRegionalCity() : nullptr; }

void InstallNeighborHosts() {
    sc4::SetNeighborsHost(&g_neighborsHost);
    sc4::SetNeighborConnectionHost(&g_connectionHost);
}

namespace {
}  // namespace

void InstallRegionServices(cSC4App& app, const RegionCallbacks& callbacks) {
    g_app = &app;
    // cSC4App::GetRegion (i386 0x005ee07a): the nation's active region (nation slot 8).
    app.SetNationRegionSource(+[](cISC4Nation* nation) -> cISC4Region* { return nation ? nation->GetActiveRegion() : nullptr; });
    g_callbacks = callbacks;
    SetNationHost(&g_nationHost);
    SetRegionHost(&g_regionHost);
    SetRegionalCityHost(&g_cityHost);
    auto& services = sc4::GetSaveGameServices();
    services.createCitySegment = []() -> cIGZPersistDBSegment* {
        auto* segment = new sc4::cSC4DBSegment();
        segment->AddRef();
        return segment->AsIGZPersistDBSegment();
    };
    services.createRegionalCity = []() -> cISC4RegionalCity* {
        auto* city = new cSC4RegionalCity(&g_cityHost);
        city->AddRef();
        return city;
    };
}

cISC4Nation* CreateNation() {
    auto* nation = new cSC4Nation();
    nation->AddRef();
    nation->Init();
    return nation;
}

}  // namespace native
