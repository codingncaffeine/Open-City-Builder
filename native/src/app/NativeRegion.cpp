#include "app/NativeRegionScreen.h"
#include "platform/Unwired.h"
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
#include "sc4/NeighborDeal.h"
#include "sc4/BudgetSimulator.h"
#include "app/NativeCity.h"
#include "sc4/NeighborsSimulator.h"
#include "sc4/AirportDeveloper.h"
#include "sc4/AuraSimulator.h"
#include "sc4/DemandSimulator.h"
#include "sc4/Lot.h"
#include "sc4/ResidentialSimulator.h"
#include "sc4/SeaportDeveloper.h"
#include "sc4/TutorialSystem.h"
#include "sc4/Simulator.h"
#include "sc4/Region.h"
#include "sc4/RegionalCity.h"
#include "sc4/PlumbingSimulator.h"
#include "sc4/PollutionSimulator.h"
#include "sc4/PowerSimulator.h"
#include "sc4/SC4UI.h"
#include "sc4/SLString.h"
#include "gz/cGZPersistResourceManager.h"

namespace native {
void NativeLandfillUsage(int& used, int& capacity);   // app/NativeLandfill.cpp

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
// the application's regional city refreshes itself from it (UpdateCityCache) before cSC4App::Write stores it, including what the
// city's simulators know (rating, taxes, connections, populations, jobs, ports), through the city's component slots as the original.
sc4::cSC4City* LiveCity(const cISC4City* city) { return dynamic_cast<sc4::cSC4City*>(const_cast<cISC4City*>(city)); }

// The city's component `c` as its recovered class (city vslots 0xcc aura, 0xd0 budget, 0xe0 demand, 0xf8 neighbors, 0x110
// residential, 0x148 tutorial system); null when the city has no such component or it is not that class.
template <class T> T* CityPart(const cISC4City* city, sc4::CityComponent c) {
    sc4::cSC4City* live = LiveCity(city);
    cIGZUnknown* object = live ? live->GetComponent(c) : nullptr;
    if (!object) return nullptr;
    if (T* direct = dynamic_cast<T*>(object)) return direct;
    void* raw = nullptr;
    if (!object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;   // cIGZSerializable: the component's own object
    auto* serializable = static_cast<cIGZSerializable*>(raw);
    T* concrete = dynamic_cast<T*>(serializable);
    serializable->Release();
    return concrete;
}

// SL::spAirportDeveloper / spSeaportDeveloper vslot 0x20 GetAllPortLots: every lot in its livable state adds its centre
// (lot vslots 0x94 IsInLivableState, 0x3c GetCenter); each lot's reference is released (vslot 8).
template <class Developer> void PortPointsOfInterest(cIGZUnknown* service, std::vector<cISC4RegionalCity::PointOfInterest>& out) {
    auto* developer = dynamic_cast<Developer*>(service);
    if (!developer) return;
    std::vector<sc4::cSC4Lot*> lots;
    developer->GetAllPortLots(lots);
    for (sc4::cSC4Lot* lot : lots) {
        if (lot->IsInLivableState()) {
            float xyz[3] = {0.0f, 0.0f, 0.0f};
            lot->GetCenter(xyz);
            out.push_back(cISC4RegionalCity::PointOfInterest{xyz[0], xyz[1], xyz[2]});
        }
        lot->Release();
    }
}

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
    // city vslot 0x2c0 SetSize: a city made from the region (NewCity, e.g. after an obliteration) takes the tile's size here
    void SetCitySize(cISC4City* city, float sizeX, float sizeZ) override { if (auto* live = LiveCity(city)) live->SetSize(sizeX, sizeZ); }
    std::uint32_t CitySerialNumber(const cISC4City* city) override { auto* live = LiveCity(city); return live ? live->GetCitySerialNumber() : 0; }
    bool CityEstablished(const cISC4City* city) override { auto* live = LiveCity(city); return live && live->GetEstablished(); }
    // city vslot 0xcc -> aura simulator vslot 0x20 GetMayorRating
    int CityMayorRating(const cISC4City* city) override {
        if (auto* aura = CityPart<sc4::cSC4AuraSimulator>(city, sc4::CityComponent::AuraSimulator)) return aura->GetMayorRating();
        cISC4RegionalCity* current = CurrentRegionalCity();
        return current ? Concrete(current)->GetMayorRating() : 0;
    }
    // city vslot 0xd0 -> budget simulator vslot 0x90 GetTaxRate(i) for the twelve tax types
    void CityTaxRates(const cISC4City* city, float (&rates)[12]) override {
        auto* budget = CityPart<sc4::cSC4BudgetSimulator>(city, sc4::CityComponent::BudgetSimulator);
        if (!budget) return;
        for (int i = 0; i < 12; ++i) rates[i] = budget->GetTaxRate(i);
    }
    // city vslot 0xf8 -> neighbors simulator vslot 0x40 GetAllConnections / vslot 0x88 UpdateCommodityStats
    void CityConnections(const cISC4City* city, NeighborConnectionList& out) override {
        if (auto* neighbors = CityPart<sc4::cSC4NeighborsSimulator>(city, sc4::CityComponent::NeighborsSimulator)) neighbors->GetAllConnections(out);
    }
    void CityUpdateCommodityStats(cISC4City* city) override {
        if (auto* neighbors = CityPart<sc4::cSC4NeighborsSimulator>(city, sc4::CityComponent::NeighborsSimulator)) neighbors->UpdateCommodityStats(city);
    }
    // city vslot 0x148 -> tutorial system vslot 0x30 GetTutorialGUID (no tutorial system: nothing)
    bool CityTutorialGUID(const cISC4City* city, int& guid) override {
        auto* tutorials = CityPart<sc4::cSC4TutorialSystem>(city, sc4::CityComponent::TutorialSystem);
        if (!tutorials) return false;
        guid = tutorials->GetTutorialGUID();
        return true;
    }
    // city vslot 0x110 -> residential simulator vslot 0x40 GetPopulation and vslot 0x7c GetGlobalWorkforcePercent
    bool CityPopulationStats(const cISC4City* city, int& population, float& workforce) override {
        auto* residential = CityPart<sc4::cSC4ResidentialSimulator>(city, sc4::CityComponent::ResidentialSimulator);
        if (!residential) return false;
        population = residential->GetPopulation();
        workforce = residential->GetGlobalWorkforcePercent();
        return true;
    }
    // city vslot 0xe0 -> demand simulator vslot 0x2c GetLocalPopulationSummary / vslot 0x20 CalculateJobsPerUnitOfDemand(units, id, 0)
    void CityLocalPopulationSummary(const cISC4City* city, std::map<std::uint32_t, int>& out) override {
        if (auto* demand = CityPart<sc4::cSC4DemandSimulator>(city, sc4::CityComponent::DemandSimulator)) demand->GetLocalPopulationSummary(out);
    }
    void CityJobsPerUnitOfDemand(const cISC4City* city, float* units, std::uint32_t purpose) override {
        if (auto* demand = CityPart<sc4::cSC4DemandSimulator>(city, sc4::CityComponent::DemandSimulator)) demand->CalculateJobsPerUnitOfDemand(units, purpose, 0);
    }
    void AirportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>& out) override { PortPointsOfInterest<sc4::cSC4AirportDeveloper>(SL::spAirportDeveloper, out); }
    void SeaportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>& out) override { PortPointsOfInterest<sc4::cSC4SeaportDeveloper>(SL::spSeaportDeveloper, out); }
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
    void SetTutorialActive(int guid, bool active) override { native::SetTutorialActive(guid, active); }
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
    cRZString TutorialRegionDefaultDirName(int id) override {   // the tutorial registry's default region (a first start opens it)
        sc4::cSC4TutorialRegistry* registry = native::TutorialRegistry();
        return cRZString(registry ? registry->GetTutorialRegionDefaultDirName(id).c_str() : "");
    }
    void TutorialItems(std::list<NationTutorialItem>&, cISC4Region*) override {}
    void SetTutorialActive(std::int32_t guid, bool active) override { native::SetTutorialActive(guid, active); }
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
// the other side. PARTIAL: the deal pass (power / water / garbage trades) runs on the simulator agent the neighbours simulator
// registers, but every budget / utility seam below answers neutrally, and no connection occupants are created or moved.
cISC4RegionalCity* RegionCity(int x, int z) {
    cISC4Region* region = g_app ? g_app->GetRegion() : nullptr;
    cRZAutoRefCount<cISC4RegionalCity>* entry = region ? region->GetCity(x, z) : nullptr;
    return entry ? entry->get() : nullptr;
}
int CurrentCellsX() { cISC4City* city = g_app ? g_app->GetCity() : nullptr; auto* live = LiveCity(city); return live ? live->CellCountX() : 0; }
int CurrentCellsZ() { cISC4City* city = g_app ? g_app->GetCity() : nullptr; auto* live = LiveCity(city); return live ? live->CellCountZ() : 0; }

class NeighborsHost final : public sc4::INeighborsHost {
    // the open city's component `c` as its recovered class (null when missing)
    template <class T> static T* Part(sc4::CityComponent c) { return CityPart<T>(HostCity(), c); }
    static sc4::cSC4Simulator* Simulator() {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::Simulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0x8695664e, &raw) || !raw) return nullptr;
        auto* simulator = static_cast<sc4::cSC4Simulator*>(static_cast<sc4::cISC4Simulator*>(raw));
        simulator->Release();   // the city keeps it
        return simulator;
    }

public:
    cIGZMessageServer2* MessageServer() override { return SL::spMS2; }
    cISC4City* City() override { return g_app ? g_app->GetCity() : nullptr; }
    cISC4RegionalCity* CurrentRegionalCity() override { return g_app ? g_app->GetRegionalCity() : nullptr; }
    sc4::cISCResExemplar* TuningExemplar() override {
        void* raw = nullptr;
        const cGZPersistResourceKey key{0x6534284a, 0x7a4a8458, 0x6a469021};
        return SL::spRM && SL::spRM->GetResource(key, 0xa52160f5, &raw, 0, nullptr) ? static_cast<sc4::cISCResExemplar*>(raw) : nullptr;
    }
    // neighbors.md 11.2: the month, day and idle agents, flags 2 (an established city only)
    void AddSimulatorAgent(cIGZMessageTarget2* agent, int interval, const char* name) override { AddCitySimulatorAgent(agent, interval, name, 2); }
    void RemoveSimulatorAgent(cIGZMessageTarget2* agent) override { RemoveCitySimulatorAgent(agent); }
    void SimDate(cIGZDate& out) override {
        if (sc4::cSC4Simulator* simulator = Simulator()) out.Copy(simulator->GetSimDate());
    }
    void PostMessage(std::uint32_t type, std::uint32_t data1, std::uint32_t data2, std::uint32_t data3, std::uint32_t data4,
                     cIGZUnknown* unknown) override {
        if (!SL::spMS2) return;
        auto* message = new cRZMessage2Standard();
        message->AddRef();
        message->SetType(type);
        message->SetData1(data1);
        message->SetData2(data2);
        message->SetData3(data3);
        message->SetData4(data4);
        message->SetIGZUnknown(unknown);
        SL::spMS2->MessagePost(message, false);
        message->Release();
    }
    // a status report (neighbors.md 5.3): data1 = the status, the deal as the message object
    void PostDealStatusMessage(std::uint32_t status, cIGZUnknown* deal) override { PostMessage(sc4::kNeighborsMessageDealStatus, status, 0, 0, 0, deal); }
    cISC4RegionalCity* RegionCityAt(int x, int z) override { return RegionCity(x, z); }
    void ReloadCityAt(int, int) override {}
    int CityCellCountX() override { return CurrentCellsX(); }
    int CityCellCountZ() override { return CurrentCellsZ(); }
    void CityCellCenterToPosition(int x, int z, float& outX, float& outZ) override { outX = (float(x) + 0.5f) * 16.0f; outZ = (float(z) + 0.5f) * 16.0f; }
    void CityPositionToCell(float x, float z, int& outX, int& outZ) override { outX = int(std::floor(x / 16.0f)); outZ = int(std::floor(z / 16.0f)); }
    float TerrainAltitudeAtCell(int, int) override { return 0.0f; }
    float TerrainAltitudeAtPosition(float, float) override { return 0.0f; }
    void RecordHistory(std::uint32_t, int) override {}
    // The budget simulator (i386 0x20 GetTotalFunds, 0x48 GetTotalMonthlyIncome; a department's 0x44 GetTotalExpenses; its line
    // item's 0x30 GetFullExpenses): false when the department (line item) does not exist.
    double BudgetTotal() override { auto* b = Part<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator); return b ? double(b->GetTotalFunds()) : 0.0; }
    double BudgetIncome() override { auto* b = Part<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator); return b ? double(b->GetTotalMonthlyIncome()) : 0.0; }
    bool BudgetLineItemAmount(std::uint32_t department, double& out) override {
        auto* b = Part<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator);
        sc4::cSC4DepartmentBudget* d = b ? b->GetDepartmentBudget(department) : nullptr;
        if (!d) return false;
        out = double(d->GetTotalExpenses());
        return true;
    }
    bool BudgetSubLineItemAmount(std::uint32_t department, std::uint32_t line, double& out) override {
        auto* b = Part<sc4::cSC4BudgetSimulator>(sc4::CityComponent::BudgetSimulator);
        sc4::cSC4DepartmentBudget* d = b ? b->GetDepartmentBudget(department) : nullptr;
        sc4::cSC4LineItem* item = d ? d->GetLineItem(line) : nullptr;
        if (!item) return false;
        out = double(item->GetFullExpenses());
        return true;
    }
    // power i386 0x38 GetMonthlyProduction / 0x34 GetMonthlyRequirement; plumbing 0x40 GetWaterProductionCapacity / GetWaterDemand
    int PowerCapacity() override { auto* p = Part<sc4::cSC4PowerSimulator>(sc4::CityComponent::PowerSimulator); return p ? int(p->GetMonthlyProduction()) : 0; }
    int PowerDemand() override { auto* p = Part<sc4::cSC4PowerSimulator>(sc4::CityComponent::PowerSimulator); return p ? int(p->GetMonthlyDemand()) : 0; }
    int WaterCapacity() override { auto* p = Part<sc4::cSC4PlumbingSimulator>(sc4::CityComponent::PlumbingSimulator); return p ? int(p->GetWaterProductionCapacity()) : 0; }
    int WaterDemand() override { auto* p = Part<sc4::cSC4PlumbingSimulator>(sc4::CityComponent::PlumbingSimulator); return p ? int(p->GetWaterDemand()) : 0; }
    // the landfill developer's GetLandfillUsage (used, capacity): the deal pass reads capacity - used as the free space
    void LandfillRange(int& start, int& end) override { NativeLandfillUsage(start, end); }
    // pollution i386 0x84 incinerator capacity, 0x88 waste-to-energy capacity, 0xa0 recycled, 0x90 abducted, 0x94 produced
    int PollutionValue(int which) override {
        auto* p = Part<sc4::cSC4PollutionSimulator>(sc4::CityComponent::PollutionSimulator);
        if (!p) return 0;
        switch (which) {
            case 0: return int(p->GetMonthlyIncineratorCapacity());
            case 1: return int(p->GetMonthlyWasteToEnergyCapacity());
            case 2: return int(p->GetGarbageRecycledPrevMonth());
            case 3: return int(p->GetGarbageAbductedPrevMonth());
            case 4: return int(p->GetGarbageProducedPrevMonth());
            default: return 0;
        }
    }
    void OccupantsInRect(int, int, int, int, std::vector<sc4::cISC4Occupant*>&) override {}
    void DeleteProp(sc4::cISC4Occupant*) override {}
    bool CreateProp(std::uint32_t, void** out) override {
        SC4_UNWIRED("cSC4NeighborsSimulator: the neighbor connection occupants (border props) are not created");
        if (out) *out = nullptr;
        return false;
    }
    void FinishProp(void*) override {}
    void SetPropPosition(void*, const float[3]) override {}
    void SetPropEdgeFlags(void*, std::uint32_t) override {}
    void AddPropConnectionProperties(void*, std::uint32_t) override {}
    void ReleaseProp(void*) override {}
    void ReleaseOccupantList(std::vector<sc4::cISC4Occupant*>&) override {}
    void DeactivateOccupant(sc4::cISC4Occupant*) override {}
    const sc4::cISCPropertyHolder* OccupantProperties(sc4::cISC4Occupant*) override { return nullptr; }
    void OccupantPosition(sc4::cISC4Occupant*, float& x, float& z) override { x = z = 0.0f; }
    void SetOccupantPosition(sc4::cISC4Occupant*, const float[3]) override {
        SC4_UNWIRED("cSC4NeighborsSimulator: the neighbor connection occupants are not moved");
    }
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
    double CityGetBudget(const cISC4RegionalCity* city) override { return Concrete(city)->GetBudget(); }
    void CitySetBudget(cISC4RegionalCity* city, double value) override { Concrete(city)->SetBudget(value); }
    void CitySetIncome(cISC4RegionalCity* city, double value) override { Concrete(city)->SetIncome(value); }
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
    void CityChangeSymmetricConnection(cISC4RegionalCity* city, sc4::cISC4NeighborConnection* connection, bool add) override {
        if (city) Concrete(city)->ChangeSymmetricConnection(connection, add);
    }
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
    // The regional city's commodity record (cSC4RegionalCity vslots 0x1b0..0x1f8) and its import / export pass (0x238).
    void CityUpdateImportExport(cISC4RegionalCity* city) override { if (city) Concrete(city)->UpdateImportExport(); }
    double CityGetExported(const cISC4RegionalCity* city, int c) override { return Concrete(city)->GetExported(c); }
    void CitySetExported(cISC4RegionalCity* city, int c, double v) override { Concrete(city)->SetExported(c, v); }
    double CityGetImported(const cISC4RegionalCity* city, int c) override { return Concrete(city)->GetImported(c); }
    void CitySetImported(cISC4RegionalCity* city, int c, double v) override { Concrete(city)->SetImported(c, v); }
    double CityGetProduced(const cISC4RegionalCity* city, int c) override { return Concrete(city)->GetProduced(c); }
    void CitySetProduced(cISC4RegionalCity* city, int c, double v) override { Concrete(city)->SetProduced(c, v); }
    double CityGetDemanded(const cISC4RegionalCity* city, int c) override { return Concrete(city)->GetDemanded(c); }
    void CitySetDemanded(cISC4RegionalCity* city, int c, double v) override { Concrete(city)->SetDemanded(c, v); }
    double CityGetCostPerUnit(const cISC4RegionalCity* city, int c) override { return Concrete(city)->GetCostPerUnit(c); }
    void CitySetCostPerUnit(cISC4RegionalCity* city, int c, double v) override { Concrete(city)->SetCostPerUnit(c, v); }
    // i386 0x0004ce32: title, the format Sprintf'd with the neighbor's name (else the default name string), no OK / cancel text
    bool ConfirmationDialog(std::uint32_t titleId, std::uint32_t formatId, std::uint32_t defaultNameId, const cIGZString* name) override {
        sc4::cSLString title(0x6a231eaa, titleId, true), format(0x6a231eaa, formatId, true), fallback(0x6a231eaa, defaultNameId, true);
        cRZString text;
        text.Sprintf(format.ToChar(), name ? name->ToChar() : fallback.ToChar());
        return nSC4UI::GenericConfirmationDialog(title, text, nullptr, nullptr);
    }
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

// cSC4NeighborDeal's services: which city a region cell holds (a deal finds the city on each side of it this way; a clone whose cells
// hold no city is cancelled), the current regional city and the city's funds.
class DealHost final : public sc4::INeighborDealHost {
public:
    bool CityHasBudget() override { return Budget() != nullptr; }
    // cSC4NeighborDeal::UpdateCost: the CURRENT city's neighbors simulator prices the deal (neighbors.md 8.1, vslot 0x118
    // GetDealCost(type, amount, the city on the other side)). Without a city or simulator the original crashes; 0 here (the deal
    // then costs 1).
    double DealCost(double amount, std::uint32_t type, void* otherCity) override {
        sc4::cSC4City* city = HostCity();
        auto* neighbors = city ? dynamic_cast<sc4::cSC4NeighborsSimulator*>(city->GetComponent(sc4::CityComponent::NeighborsSimulator)) : nullptr;
        return neighbors ? neighbors->GetDealCost(type, amount, static_cast<const cISC4RegionalCity*>(otherCity)) : 0.0;
    }
    bool CityEntryAtCell(int x, int z, void*& city) override {
        city = RegionCity(x, z);
        return city != nullptr;
    }
    void* CurrentCity() override { return NativeCurrentRegionalCity(); }
    double CurrentCityFunds() override { sc4::cSC4BudgetSimulator* b = Budget(); return b ? double(b->GetTotalFunds()) : 0.0; }
    void ReportDealStatus(bool, sc4::cSC4NeighborDeal*) override {}
    double CurrentRegionalCityBudget() override {
        cISC4RegionalCity* city = NativeCurrentRegionalCity();
        return city ? Concrete(city)->GetBudget() : 0.0;
    }
    bool HasSuitableConnection(std::uint32_t type, void* city) override {
        sc4::cSC4NeighborsSimulator* neighbors = NeighborsSim();
        return neighbors && neighbors->FindConnectionSuitableForDeal(type, static_cast<const cISC4RegionalCity*>(city)) != nullptr;
    }
private:
    static sc4::cSC4BudgetSimulator* Budget() {
        sc4::cSC4City* city = HostCity();
        cIGZUnknown* object = city ? city->GetComponent(sc4::CityComponent::BudgetSimulator) : nullptr;
        void* raw = nullptr;
        if (!object || !object->QueryInterface(0xe4fda3d4, &raw) || !raw) return nullptr;
        auto* serializable = static_cast<cIGZSerializable*>(raw);
        serializable->Release();
        return serializable->GetGZCLSID() == sc4::kGZCLSID_cSC4BudgetSimulator ? static_cast<sc4::cSC4BudgetSimulator*>(serializable) : nullptr;
    }
} g_dealHost;

}  // namespace

cISC4RegionalCity* NativeCurrentRegionalCity() { return g_app ? g_app->GetRegionalCity() : nullptr; }

// SL::spNeighborsSim vslot 0x7c MarkDealsDirty: the simulators that change what the neighbor deals carry ask for a recount.
void NativeMarkNeighborDealsDirty() {
    if (sc4::cSC4NeighborsSimulator* neighbors = NeighborsSim()) neighbors->MarkDealsDirty();
}

void InstallNeighborHosts() {
    sc4::SetNeighborsHost(&g_neighborsHost);
    sc4::SetNeighborConnectionHost(&g_connectionHost);
    sc4::SetNeighborDealHost(&g_dealHost);
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

// kit command `neighbors`: the open city's deals (type, amount, cost, flags) and its regional commodity record (power, water,
// garbage), after the neighbors simulator refreshed it (UpdateCommodityStats); with `propose`, the deals ProposeDeals would offer.
void NativeNeighborsReport(bool propose) {
    sc4::cSC4City* city = HostCity();
    auto* neighbors = city ? CityPart<sc4::cSC4NeighborsSimulator>(city, sc4::CityComponent::NeighborsSimulator) : nullptr;
    if (!neighbors) { std::printf("neighbors: no simulator\n"); return; }
    neighbors->UpdateCommodityStats(city);
    sc4::DealList deals;
    neighbors->FindDealsAll(deals);
    std::printf("neighbors: %zu deals\n", deals.size());
    for (auto& deal : deals)
        if (deal) std::printf("neighbors deal %08x amount %d cost %lld flags %08x\n", deal->GetType(), deal->GetAmount(),
                              static_cast<long long>(deal->GetCost()), deal->GetFlags());
    if (cISC4RegionalCity* current = g_app ? g_app->GetRegionalCity() : nullptr) {
        auto* r = Concrete(current);
        for (int c = 1; c <= 3; ++c)
            std::printf("neighbors commodity %d produced %.0f demanded %.0f imported %.0f exported %.0f price %g budget %.0f\n", c, r->GetProduced(c),
                        r->GetDemanded(c), r->GetImported(c), r->GetExported(c), r->GetCostPerUnit(c), r->GetBudget());
    }
    if (propose) {
        sc4::DealList offers;
        neighbors->ProposeDeals(offers, false);
        std::printf("neighbors: %zu proposals\n", offers.size());
        for (auto& deal : offers) if (deal) std::printf("neighbors proposal %08x cost %lld\n", deal->GetType(), static_cast<long long>(deal->GetCost()));
    }
}

cISC4Nation* CreateNation() {
    auto* nation = new cSC4Nation();
    nation->AddRef();
    nation->Init();
    return nation;
}

}  // namespace native
