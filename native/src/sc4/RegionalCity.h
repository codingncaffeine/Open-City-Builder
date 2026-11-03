// cSC4RegionalCity — the city as the region sees it: its place and size in the region grid, the name and
// identity record of the save file, the population / jobs / tax / commodity statistics the region view shows,
// the road/rail/... connections to the neighbouring cities with their pending symmetric-change queue, and the
// per-commodity trade deals with the neighbours.
//
// Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (vtable 0x100aa3220 primary group, 0x100aa34a8 message
// target, 0x100aa34d8 serializable; 90 symbols 0x1003ad932..0x1003b2e84 plus 10 non-virtual thunks; class id
// 0x6a037c24, interface 0x69d2be1d). The class is created through GZCOM by SC4ReadRegionalCity
// (src/sc4/SaveGame.cpp).
//
// Original layout (0x2d8 bytes):
//   +0x00 vptr (cISC4RegionalCity), +0x08 vptr (cIGZMessageTarget2), +0x10 vptr (cIGZSerializable)
//   +0x18 initialised byte, +0x1c refcount (one count, the two secondary views are non-virtual thunks)
//   +0x20 region cell x, +0x24 region cell z, +0x28 city width, +0x2c city height (in region cells; 1 = the
//         small 64-cell city, 2 = medium, 4 = large — SetupCity shifts them left 10 to get metres)
//   +0x30 population, +0x34 commercial jobs, +0x38 industrial jobs, +0x3c workforce percentage (float),
//         +0x40 mayor rating (signed byte), +0x44 difficulty level
//   +0x48 std::map<u32,int> population per purpose code, +0x60 extrapolated population, +0x78 potential
//         population (GetAllowableExtrapolation = potential - extrapolated, clamped at 0)
//   +0x90 float[12] tax rates (default 7.0 each, 0x100a1e650), indexed by GetTaxRate's purpose-code table
//   +0xc0 std::list<ref<cISC4NeighborConnection>> connections (sentinel +0xc0, size +0xd0)
//   +0xd8 std::vector<ConnectionChange> pending symmetric-connection changes (16 bytes each)
//   +0xf0 connections-changed flag, +0xf4 serial number, +0xf8/+0xfc original language/country,
//         +0x100/+0x104 last language/country, +0x108 birth date
//   +0x110, +0x138, +0x160, +0x188, +0x1b0, +0x1d8, +0x200, +0x238 eight cRZStrings (0x28 each): save file
//         path, city name, city name (original), mayor name, mayor name (original), description, description
//         (original), utility advisor name
//   +0x228 world position x/z and base elevation (the three floats sit between the seventh string and the
//         utility advisor name)
//   +0x260 established byte, +0x264 tutorial GUID (-1 = not a tutorial city)
//   +0x268 double budget, +0x270 double income
//   +0x278 std::map<int,cCommodityStats> (5 doubles per entry: produced, demanded, exported, imported,
//         cost per unit; the constructor pre-inserts keys 1, 2 and 3 with zeroed stats)
//   +0x290 std::list<ref<cISC4NeighborDeal>> deals (sentinel +0x290, size +0x2a0)
//   +0x2a8 std::list<ref<cISC4NeighborDeal>> local deals (sentinel +0x2a8, size +0x2b8)
//   +0x2c0 std::map<PointOfInterestType, std::vector<PointOfInterest>> points of interest, +0x2d0 size
//
// Serialization (Write version 13, Read accepts < 14): the identity/geometry/statistics block, the three
// population maps, the tax rates, the connections, the pending changes, budget/income, the commodity stats,
// the deals, and (version > 11) the points of interest. Read then looks the tutorial value up in the tutorial
// registry, loads the default city name when the city is not established, and overrides the city and mayor
// names from the localized string records.
//
// Quirks kept (asserted by scripts/tests/sc4_regionalcity_test.cpp):
//  * Release calls Shutdown and deletes the object *without* the last reference check that other classes use
//    (it destroys when the count is below 2) and returns 0 (0x1003ae2fe).
//  * Read stores *both* deal lists in the deals list at +0x290: the second block was meant for the local-deals
//    list at +0x2a8 but pushes into the same list (0x1003af48e), so after a load the local list is empty.
//  * Read only stores the difficulty level when the city is established (0x1003aeec8); a non-established city
//    keeps the difficulty it was constructed with.
//  * Read converts the stored tutorial value through cSC4TutorialRegistry::WhichToGUID (0x1003aeee2).
//  * GetExtrapolatedPopulation falls back to the virtual GetPopulation() when the purpose code has no entry
//    (0x1003b1d30).
//  * GetPointsOfInterest leaves the caller's vector untouched when the type has no entry (0x1003b2a46).
//  * SetPosition(true) (used after a region import) renumbers the connection/deal cells by the position delta
//    before queuing the repairs (0x1003b2aa2).
//  * UpdateImportExport books a deal for types 0x4773ffe0/e2/e4 as imported when its other side is another city
//    and as exported when its other side is this city; e1/e3/e5 swap the two (0x1003b2826). Deals with the
//    silent flag 0x80 are skipped, and every deal starts silent (0x81) until UpdateLocalDeals clears it.
//  * SetupPreferences copies the names with strcpy (unbounded) (0x1003af8f8).
//  * The const commodity getters and GetAllowableExtrapolation index their maps with operator[], so a query for
//    a missing key inserts a zeroed entry (0x1003b2214, 0x1003b2152).
//  * ExtractLocalDeals always drops the deals whose two region cells have no city, even with removeEmpty false
//    (0x1003b2534); removeEmpty only controls the surviving local deals.
//  * Read releases the database-stream reference the QueryInterface added *before* it asks that stream for its
//    segment (0x1003af66c): the stream survives only because the caller owns another reference.
//  * Read looks the localized names up when it has a segment; the port routes them through the host, which
//    owns that segment call (the original dereferences a null segment).
//
// Hook-up left to the lead (the class itself is complete): IRegionHost's city virtuals (Region.h), the view's
// IRegionCityView (WinRegionView.h) and the screen's IRegionalCity (WinRegionScreen.h) still describe the
// original object; SaveGame's createRegionalCity factory and GZCOM_CreatecSC4RegionalCity still answer null.
//
// Not recovered (see the PARTIAL/SKIPPED sections of notes/ledger_pending/cSC4RegionalCity.txt):
//  * CreateSymmetricConnections (0x1003b0ae0, 4168 bytes): needs the neighbour simulator's connection
//    create/delete path, the occupant manager's network-mask occupants, the demolition utility, the zone
//    manager and the network manager. DoMessage is PARTIAL for this reason: it handles the message and the
//    pending-change queue exactly as the original, then stops instead of calling CreateSymmetricConnections.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZPersistDBSegment.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZAutoRefCount.h"
#include "rz/cRZString.h"
#include "sc4/ISC4City.h"
#include "sc4/ISC4RegionalCity.h"
#include "sc4/NeighborConnection.h"
#include "sc4/NeighborDeal.h"

class cSC4RegionalCity;
class IRegionalCityHost;

// The "new city" record the region screen fills in and hands to SetupPreferences / UpdateCityCache
// (0x404 bytes: two 0x200-byte names and the difficulty level).
struct SC4NewCityPreferences {
    char cityName[0x200] = {};
    char mayorName[0x200] = {};
    std::uint32_t difficultyLevel = 0;   // +0x400
};
static_assert(sizeof(SC4NewCityPreferences) == 0x404, "SC4NewCityPreferences is 0x404 bytes");

constexpr std::uint32_t kGZCLSID_cSC4RegionalCity = 0x6a037c24;
constexpr std::uint16_t kRegionalCityWriteVersion = 13;   // Read accepts any version below 14
constexpr std::uint32_t kRegionalCityRecordIdCityName = 0x6aaee931;   // localized name string record
constexpr std::uint32_t kRegionalCityRecordIdMayorName = 0x6aaee932;
constexpr std::uint32_t kRegionalCityDefaultCityNameGroup = 0x6a231eaa;      // SC4LoadString arguments
constexpr std::uint32_t kRegionalCityDefaultCityNameInstance = 0xaa738c4e;
// The five messages Init subscribes to and DoMessage handles.
constexpr std::uint32_t kRegionalCityMessageDealsChanged = 0x26c63342;
constexpr std::uint32_t kRegionalCityMessageCityStatistics = 0x26c63343;
constexpr std::uint32_t kRegionalCityMessageCityInit = 0x26d31ec1;
constexpr std::uint32_t kRegionalCityMessageCityInit2 = 0x26d31ec3;
constexpr std::uint32_t kRegionalCityMessageCityChanged = 0xca63d517;

using NeighborConnectionList = std::list<cRZAutoRefCount<sc4::cISC4NeighborConnection>>;
using NeighborDealList = std::list<cRZAutoRefCount<sc4::cISC4NeighborDeal>>;

// Every call cSC4RegionalCity makes outside itself. Each method mirrors one original call; the slot or the
// free-function address is in the comment. Assembled the same way as IRegionHost (Region.h).
class IRegionalCityHost {
public:
    virtual ~IRegionalCityHost() = default;

    // --- construction / framework -------------------------------------------------------------------
    // SL::spPreferences + 0x46c: the difficulty level a new region city starts with.
    virtual std::uint32_t DefaultDifficultyLevel() = 0;
    // SL::spMS2 vslot 0x28 (Init) and 0x30 (Shutdown).
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t messageType) = 0;
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t messageType) = 0;
    // SL::spMS2 vslot 0x10 MessageSend of a plain message of this type.
    virtual void SendMessage(std::uint32_t type) { (void)type; }
    // SL::spSC4 (cISC4App) vslot 0x120: the regional city the application is currently in.
    virtual cISC4RegionalCity* CurrentRegionalCity() = 0;
    // SL::spCity: the city object being simulated (null outside the city).
    virtual cISC4City* CurrentCity() = 0;
    // SL::spSC4 vslot 0x118 (the region) then cISC4Region vslot 0x58 (GetCity(x, z)): the city there, or null.
    virtual cISC4RegionalCity* RegionCityAt(int x, int z) = 0;

    // --- neighbour simulator ------------------------------------------------------------------------
    // cSC4NeighborsSimulator::ConvertConnectionCellToNeighborCell @ 0x1000707f4 (static).
    virtual void ConvertConnectionCellToNeighborCell(int x, int z, int& neighborX, int& neighborZ,
                                                     cISC4RegionalCity*& city, int& cellX, int& cellZ) = 0;
    // cSC4NeighborsSimulator::ConvertNeighborCellToConnectionCell @ 0x100070a70 (static).
    virtual void ConvertNeighborCellToConnectionCell(cISC4RegionalCity* city, int x, int z, int& outX,
                                                     int& outZ) = 0;
    // SL::spNeighborsSim != null (ExtractLocalDeals bails out without it) and vslot 0x60
    // FindCityOnOtherSideOfConnection(x, z).
    virtual bool HasNeighborsSimulator() = 0;
    virtual cISC4RegionalCity* FindCityOnOtherSideOfConnection(int x, int z) = 0;
    // cISC4RegionalCity vslot 0x158 (GetEstablished) and 0xa8 (GetAllConnections) of a neighbouring city.
    virtual bool RegionalCityEstablished(const cISC4RegionalCity* city) = 0;
    virtual void RegionalCityConnections(const cISC4RegionalCity* city, NeighborConnectionList& out) = 0;
    // SL::spNeighborsSim vslot 0xa0 GetAdjacentCities(list, flag).
    virtual void GetAdjacentCities(std::list<cRZAutoRefCount<cISC4RegionalCity>>& out, bool connected) = 0;

    // --- save-file helper / tutorial registry / string service --------------------------------------
    // SC4CreateSavePathForCity(cityName, serial, out) @ 0x1001e98a4.
    virtual void CreateSavePathForCity(const cIGZString& cityName, std::uint32_t serial, cRZString& out) = 0;
    // cSC4TutorialRegistry::WhichToGUID @ 0x100643874, reached through SL::spSC4 vslot 0x178.
    virtual int TutorialGUIDFromStoredValue(int value) = 0;
    // RZ::LM() vslot 0x38 (GetCurrentLanguage), used for the localized string records.
    virtual int UILanguage() = 0;
    // SC4ReadStringRecord(segment, group, instance, out) @ 0x1001e9453.
    virtual bool ReadStringRecord(cIGZPersistDBSegment* segment, std::uint32_t group, std::uint32_t instance,
                                  cIGZString& out) = 0;
    // SC4LoadString(group, instance, out) @ 0x100120e10.
    virtual bool LoadString(std::uint32_t group, std::uint32_t instance, cIGZString& out) = 0;

    // --- the city object, as SetupCity / UpdateCityCache(cISC4City*) reach it ------------------------
    virtual void CityName(const cISC4City* city, cIGZString& out) = 0;               // city vslot 0x60
    virtual void CityMayorName(const cISC4City* city, cIGZString& out) = 0;          // 0x80
    virtual void SetCityName(cISC4City* city, const cIGZString& name) = 0;           // 0x68
    virtual void SetCityMayorName(cISC4City* city, const cIGZString& name) = 0;      // 0x88
    virtual std::uint32_t CityDifficulty(const cISC4City* city) = 0;                 // 0xc0
    virtual void SetCityDifficulty(cISC4City* city, std::uint32_t level) = 0;        // 0xc8
    virtual void SetCitySize(cISC4City* city, float sizeX, float sizeZ) = 0;         // 0x2c0
    virtual std::uint32_t CitySerialNumber(const cISC4City* city) = 0;               // 0x28
    virtual bool CityEstablished(const cISC4City* city) = 0;                         // 0xb0
    // city 0x198 (aura simulator) then its vslot 0x40 GetMayorRating.
    virtual int CityMayorRating(const cISC4City* city) = 0;
    // city 0x1a0 (budget simulator) vslot 0x120 GetTaxRate(i) for i = 0..11.
    virtual void CityTaxRates(const cISC4City* city, float (&rates)[12]) = 0;
    // city 0x1f0 (neighbours simulator) vslot 0x80 GetAllConnections / 0x110 UpdateCommodityStats(city).
    virtual void CityConnections(const cISC4City* city, NeighborConnectionList& out) = 0;
    virtual void CityUpdateCommodityStats(cISC4City* city) = 0;
    // city 0x290 (tutorial system) vslot 0x60 GetTutorialGUID; false when the system is absent.
    virtual bool CityTutorialGUID(const cISC4City* city, int& guid) = 0;
    // city 0x220 (residential simulator) vslot 0x80 GetPopulation / 0xf8 GetGlobalWorkforcePercent;
    // false when the simulator is absent (the fields are then left alone).
    virtual bool CityPopulationStats(const cISC4City* city, int& population, float& workforce) = 0;
    // city 0x1c0 (demand simulator) vslot 0x58 GetLocalPopulationSummary.
    virtual void CityLocalPopulationSummary(const cISC4City* city, std::map<std::uint32_t, int>& out) = 0;
    // city 0x1c0 vslot 0x40 CalculateJobsPerUnitOfDemand(out, purpose, 0).
    virtual void CityJobsPerUnitOfDemand(const cISC4City* city, float* out, std::uint32_t purpose) = 0;
    // SL::spAirportDeveloper / SL::spSeaportDeveloper vslot 0x40 (the active lots), then per lot vslot
    // 0x128 (in use) and 0x78 (its point of interest).
    virtual void AirportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>& out) = 0;
    virtual void SeaportPointsOfInterest(std::vector<cISC4RegionalCity::PointOfInterest>& out) = 0;
};
IRegionalCityHost* RegionalCityHost();
void SetRegionalCityHost(IRegionalCityHost* host);

// cSC4RegionalCity. The vtable methods are declared in the original's slot order; the slots that the partial
// cISC4RegionalCity interface above does not declare are plain virtuals here (their original offset is in the
// comment), so the interface's five methods keep the slot order the rest of the port builds against.
class cSC4RegionalCity : public cISC4RegionalCity, public cIGZMessageTarget2, public cIGZSerializable {
public:
    // cSC4RegionalCity::mbStaticInitialized @ 0x100b6ddd0: set by the first Init, never cleared.
    static inline bool mbStaticInitialized = false;

    // 16 bytes in the original: purpose code, cell x, cell z, flag at +0xc.
    struct ConnectionChange {
        std::uint32_t type = 0;   // +0x0
        int x = 0;                // +0x4
        int z = 0;                // +0x8
        bool flag = false;        // +0xc
    };
    static_assert(sizeof(ConnectionChange) == 16, "ConnectionChange is 16 bytes");

    // The per-commodity trade statistics (five doubles in the original, +0x0 +0x8 +0x10 +0x18 +0x20).
    struct cCommodityStats {
        double produced = 0;
        double demanded = 0;
        double exported = 0;
        double imported = 0;
        double costPerUnit = 0;
    };

    cSC4RegionalCity();
    explicit cSC4RegionalCity(IRegionalCityHost* host);
    virtual ~cSC4RegionalCity();

    // cIGZUnknown / cIGZSerializable / cIGZMessageTarget2
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override;
    std::uint32_t Release() override;
    bool DoMessage(cIGZMessage2* message) override;                 // +0x258
    bool Write(cIGZOStream& stream) const override;                // +0x260
    bool Read(cIGZIStream& stream) override;                       // +0x268
    std::uint32_t GetGZCLSID() const override;                     // +0x270

    // cISC4RegionalCity
    bool Init() override;                                          // +0x18
    bool Shutdown() override;                                      // +0x20
    void SetupCity(cISC4City* city) override;                      // +0xc8
    std::uint32_t GetCitySerialNumber() const override;            // +0xd8
    void GetCitySaveFilePath(cIGZString& out) const override;      // +0xf8

    virtual void GetPosition(int& x, int& z) const;                // +0x28
    virtual void SetPosition(int x, int z, bool fromImport);       // +0x30
    virtual void GetCitySize(int& width, int& height) const;       // +0x38
    virtual void SetCitySize(int width, int height);               // +0x40
    virtual int GetPopulation() const;                             // +0x48
    virtual int GetCommercialJobs() const;                         // +0x50
    virtual int GetIndustrialJobs() const;                         // +0x58
    virtual const float& GetWorkforcePercentage() const;           // +0x60 (returns the field's address)
    virtual int GetMayorRating() const;                            // +0x68 (sign-extended byte)
    virtual std::uint32_t GetDifficultyLevel() const;              // +0x70
    virtual float GetTaxRate(std::uint32_t purpose) const;         // +0x78
    virtual int GetPopulation(std::uint32_t purpose) const;        // +0x80
    virtual int GetExtrapolatedPopulation(std::uint32_t purpose) const;   // +0x88
    virtual int GetAllowableExtrapolation(std::uint32_t purpose) const;   // +0x90
    virtual void ExtrapolateGrowth(std::uint32_t purpose, float amount);  // +0x98
    virtual sc4::cISC4NeighborConnection* FindConnection(int type, int x, int z) const;   // +0xa0
    virtual void GetAllConnections(NeighborConnectionList& out) const;                    // +0xa8
    virtual void ChangeSymmetricConnection(sc4::cISC4NeighborConnection* connection, bool add);   // +0xb0
    virtual void SetupPreferences(SC4NewCityPreferences* preferences);                   // +0xb8
    virtual void UpdateCityCache(SC4NewCityPreferences* preferences);                    // +0xc0
    virtual void UpdateCityCache(cISC4City* city);                                       // +0xd0
    virtual void SetCitySerialNumber(std::uint32_t serial);                              // +0xe0
    virtual bool GetOriginalLanguageAndCountry(int& language, int& country) const;       // +0xe8
    virtual bool GetLastLanguageAndCountry(int& language, int& country);                 // +0xf0
    virtual void SetCitySaveFilePath(const cIGZString& path);                            // +0x100
    virtual void GetCityName(cIGZString& out) const;                                     // +0x108
    virtual void SetCityName(const cIGZString& name);                                    // +0x110
    virtual void GetMayorName(cIGZString& out) const;                                    // +0x118
    virtual void SetMayorName(const cIGZString& name);                                   // +0x120
    virtual void GetUtilityAdvisorName(cIGZString& out) const;                           // +0x128
    virtual void SetUtilityAdvisorName(const cIGZString& name);                          // +0x130
    virtual void GetCityDescription(cIGZString& out) const;                              // +0x138
    virtual void SetCityDescription(const cIGZString& description);                      // +0x140
    virtual std::uint32_t GetBirthDate() const;                                          // +0x148
    virtual void SetBirthDate(std::uint32_t date);                                       // +0x150
    virtual bool GetEstablished() const;                                                 // +0x158
    virtual void SetEstablished(bool established);                                       // +0x160
    virtual void GetWorldPosition(float& longitude, float& latitude) const;              // +0x168
    virtual void SetWorldPosition(float longitude, float latitude);                      // +0x170
    virtual float GetWorldBaseElevation() const;                                         // +0x178
    virtual void SetWorldBaseElevation(float elevation);                                 // +0x180
    virtual int GetWorldHemisphere() const;                                              // +0x188
    virtual double GetBudget() const;                                                    // +0x190
    virtual void SetBudget(double budget);                                               // +0x198
    virtual double GetIncome() const;                                                    // +0x1a0
    virtual void SetIncome(double income);                                               // +0x1a8
    virtual double GetExported(int commodity) const;                                     // +0x1b0
    virtual void SetExported(int commodity, double amount);                              // +0x1b8
    virtual double GetImported(int commodity) const;                                     // +0x1c0
    virtual void SetImported(int commodity, double amount);                              // +0x1c8
    virtual double GetProduced(int commodity) const;                                     // +0x1d0
    virtual void SetProduced(int commodity, double amount);                              // +0x1d8
    virtual double GetDemanded(int commodity) const;                                     // +0x1e0
    virtual void SetDemanded(int commodity, double amount);                              // +0x1e8
    virtual double GetCostPerUnit(int commodity) const;                                  // +0x1f0
    virtual void SetCostPerUnit(int commodity, double cost);                             // +0x1f8
    virtual double GetCommodityBalance(int commodity) const;                             // +0x200
    virtual int GetTutorialGUID() const;                                                 // +0x208
    virtual void SetTutorialGUID(int guid);                                              // +0x210
    virtual bool IsTutorial() const;                                                     // +0x218
    virtual bool UpdateLocalDeals();                                                     // +0x220
    virtual bool SetLocalDeals(NeighborDealList& deals);                                 // +0x228
    virtual bool GetLocalDeals(NeighborDealList& out);                                   // +0x230
    virtual bool UpdateImportExport();                                                   // +0x238
    virtual void GetPointsOfInterest(PointOfInterestType type,
                                     std::vector<PointOfInterest>& out);                 // +0x240

    // Present only so the test can look inside; the original keeps these private as well.
    IRegionalCityHost* Host() const { return host_; }
    const std::vector<ConnectionChange>& ConnectionChangesForTest() const { return connectionChanges_; }
    const NeighborConnectionList& ConnectionsForTest() const { return connections_; }
    const NeighborDealList& DealsForTest() const { return deals_; }
    const NeighborDealList& LocalDealsForTest() const { return localDeals_; }
    const std::map<int, cCommodityStats>& CommodityStatsForTest() const { return commodityStats_; }
    const std::map<std::uint32_t, int>& PopulationForTest() const { return populationByType_; }
    const std::map<std::uint32_t, int>& PotentialPopulationForTest() const { return potentialPopulation_; }
    const std::map<std::uint32_t, int>& ExtrapolatedPopulationForTest() const { return extrapolatedPopulation_; }
    void UpdateImportExportForTest(const sc4::cISC4NeighborDeal* deal) { UpdateImportExport(deal); }

private:
    // Not in the vtable in the original.
    void RepairConnectionsAfterImport();                        // 0x1003b1efe
    void MarkConnectionsForRemoval();                           // 0x1003b0824
    void TreatAllIncomingConnectionsAsNew();                    // 0x1003b0948
    // The original's signature has a phantom cISC4City* parameter that actually receives `this`
    // (0x1003b05fb); the port works on `this` directly.
    void CalculatePotentialPopulations();                       // 0x1003b06e4
    void UpdateExtrapolatedPopulations();                       // 0x1003b079a
    void ExtractLocalDeals(NeighborDealList& deals, NeighborDealList& out, bool removeEmpty);   // 0x1003b2534
    void UpdateImportExport(const sc4::cISC4NeighborDeal* deal);                    // 0x1003b2826

    IRegionalCityHost* host_ = nullptr;

    bool initialized_ = false;                   // +0x18
    std::uint32_t refCount_ = 0;                 // +0x1c
    int x_ = 0;                                  // +0x20
    int z_ = 0;                                  // +0x24
    int sizeX_ = 1;                              // +0x28
    int sizeZ_ = 1;                             // +0x2c
    int population_ = 0;                         // +0x30
    int commercialJobs_ = 0;                     // +0x34
    int industrialJobs_ = 0;                     // +0x38
    float workforcePercentage_ = 0.5f;           // +0x3c
    std::int8_t mayorRating_ = 0;                // +0x40
    std::uint32_t difficultyLevel_ = 0;          // +0x44
    std::map<std::uint32_t, int> populationByType_;           // +0x48
    // The original's const accessors index these two maps with operator[] (GetAllowableExtrapolation), so a
    // query inserts a zero entry; the port keeps that side effect (hence mutable).
    mutable std::map<std::uint32_t, int> extrapolatedPopulation_;   // +0x60
    mutable std::map<std::uint32_t, int> potentialPopulation_;      // +0x78
    float taxRates_[12] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};   // +0x90
    NeighborConnectionList connections_;         // +0xc0
    std::vector<ConnectionChange> connectionChanges_;   // +0xd8
    bool connectionsChanged_ = false;            // +0xf0
    std::uint32_t serialNumber_ = 0;             // +0xf4
    int originalLanguage_ = 0;                   // +0xf8
    int originalCountry_ = 0;                    // +0xfc
    int lastLanguage_ = 0;                       // +0x100
    int lastCountry_ = 0;                        // +0x104
    std::uint32_t birthDate_ = 0;                // +0x108
    cRZString saveFilePath_;                     // +0x110
    cRZString cityName_;                         // +0x138
    cRZString cityNameOriginal_;                 // +0x160 (no accessor of its own)
    cRZString mayorName_;                        // +0x188
    cRZString mayorNameOriginal_;                // +0x1b0 (no accessor of its own)
    cRZString description_;                      // +0x1d8
    cRZString descriptionOriginal_;              // +0x200 (no accessor of its own)
    float worldPositionX_ = 0;                   // +0x228
    float worldPositionZ_ = 0;                   // +0x22c
    float worldBaseElevation_ = 0;               // +0x230
    cRZString utilityAdvisorName_;               // +0x238
    bool established_ = false;                   // +0x260
    int tutorialGUID_ = -1;                      // +0x264
    double budget_ = 0;                          // +0x268
    double income_ = 0;                          // +0x270
    // Same side effect: the const commodity getters insert a zeroed record (0x1003b2214).
    mutable std::map<int, cCommodityStats> commodityStats_;   // +0x278
    NeighborDealList deals_;                     // +0x290
    NeighborDealList localDeals_;                // +0x2a8
    std::map<PointOfInterestType, std::vector<PointOfInterest>> pointsOfInterest_;   // +0x2c0
};
