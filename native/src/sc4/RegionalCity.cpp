#include "sc4/RegionalCity.h"

#include <cstdio>
#include <cstdlib>

#include <cstring>
#include <iterator>
#include <string>

#include "gz/StreamOps.h"
#include "rz/cRZPath.h"
#include "sc4/DBSegment.h"

// cSC4RegionalCity — see RegionalCity.h for the object layout and the list of quirks.
//
// Every call outside the object goes through IRegionalCityHost (one method per original call, with its slot);
// the only recovered neighbours called directly are the serializable views of the connection and deal records.

namespace {

IRegionalCityHost* g_host = nullptr;

constexpr const char* kCitySaveExtension = ".sc4";
constexpr std::uint32_t kIID_cIGZMessageTarget2 = 0x452294aa;   // cIGZMessageTarget2 (sc4/OccupantManager.h)

// Purpose code -> tax-rate slot (GetTaxRate 0x1003b2040); every other code answers 7.0 (0x40e00000).
struct TaxPurpose {
    std::uint32_t code;
    int index;
};
constexpr TaxPurpose kTaxPurposes[] = {
    {0x1010, 0}, {0x1020, 1}, {0x1030, 2}, {0x3110, 3}, {0x3120, 4}, {0x3130, 5},
    {0x3320, 6}, {0x3330, 7}, {0x4100, 8}, {0x4200, 9}, {0x4300, 10}, {0x4400, 11},
};
constexpr float kDefaultTaxRate = 7.0f;          // 0x40e00000, the memset pattern at 0x100a1e650

// The limits the original uses while reading the variable-length blocks (0x1003aef46..0x1003af059).
constexpr std::uint32_t kReadMapCountLimit = 0x20000001;
constexpr std::uint32_t kReadTaxCountLimit = 0xd;
constexpr std::uint32_t kReadListCountLimit = 0x100001;

// The five message ids of Init / Shutdown, in the original's subscription order.
constexpr std::uint32_t kMessages[] = {
    kRegionalCityMessageDealsChanged, kRegionalCityMessageCityStatistics, kRegionalCityMessageCityInit,
    kRegionalCityMessageCityInit2,    kRegionalCityMessageCityChanged,
};

}  // namespace

IRegionalCityHost* RegionalCityHost() { return g_host; }
void SetRegionalCityHost(IRegionalCityHost* host) { g_host = host; }

//---------------------------------------------------------------------------------------------------------------------
// construction / COM plumbing
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1003ad948. The three commodity records are inserted by the constructor itself (keys 2, 1, 3 in that order;
// their statistics are zero) and the difficulty level comes from the preferences blob.
cSC4RegionalCity::cSC4RegionalCity(IRegionalCityHost* host) : host_(host) {
    if (!host_) host_ = RegionalCityHost();
    commodityStats_[2] = cCommodityStats{};
    commodityStats_[1] = cCommodityStats{};
    commodityStats_[3] = cCommodityStats{};
    difficultyLevel_ = host_ ? host_->DefaultDifficultyLevel() : 0u;   // SL::spPreferences + 0x46c
}

cSC4RegionalCity::cSC4RegionalCity() : cSC4RegionalCity(RegionalCityHost()) {}

// @ 0x1003ae08c: the members' own destructors do the work the original spells out.
cSC4RegionalCity::~cSC4RegionalCity() = default;

// @ 0x1003ae26e. iid 1 and the regional-city iid answer the primary view, 0x452294aa the message target and
// 0xe4fda3d4 the serializable view; everything else fails.
bool cSC4RegionalCity::QueryInterface(std::uint32_t iid, void** out) {
    if (!out) return false;
    if (iid == kGZIID_cIGZUnknown || iid == kGZIID_cISC4RegionalCity) {
        *out = static_cast<cISC4RegionalCity*>(this);
    } else if (iid == kIID_cIGZMessageTarget2) {
        *out = static_cast<cIGZMessageTarget2*>(this);
    } else if (iid == sc4::kGZIID_cIGZSerializable) {
        *out = static_cast<cIGZSerializable*>(this);
    } else {
        return false;
    }
    AddRef();
    return true;
}

// @ 0x1003ae2d4. One count at +0x1c shared by the three views (their AddRef thunks use the adjusted offsets).
std::uint32_t cSC4RegionalCity::AddRef() { return ++refCount_; }

// @ 0x1003ae2fe. Quirk: the original destroys the object when the count is below 2 (it never checks for zero)
// and always answers 0 in that case.
std::uint32_t cSC4RegionalCity::Release() {
    if (static_cast<std::int32_t>(refCount_) < 2) {
        Shutdown();      // vslot 0x20
        delete this;     // vslot 0x250 (the deleting destructor)
        return 0;
    }
    return --refCount_;
}

// @ 0x1003ae3b6. Subscribes the message-target view to the five message ids; the second Init does nothing.
bool cSC4RegionalCity::Init() {
    if (!initialized_) {
        initialized_ = true;
        mbStaticInitialized = true;      // 0x100b6ddd0
        if (host_) {
            for (std::uint32_t id : kMessages)
                host_->AddNotification(static_cast<cIGZMessageTarget2*>(this), id);
        }
    }
    return true;
}

// @ 0x1003ae444. Unsubscribes and drops the connection and deal lists.
bool cSC4RegionalCity::Shutdown() {
    if (initialized_) {
        initialized_ = false;
        if (host_) {
            for (std::uint32_t id : kMessages)
                host_->RemoveNotification(static_cast<cIGZMessageTarget2*>(this), id);
        }
        connections_.clear();
        deals_.clear();
        localDeals_.clear();
    }
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
// identity / geometry / statistics accessors
//---------------------------------------------------------------------------------------------------------------------

void cSC4RegionalCity::GetPosition(int& x, int& z) const { x = x_; z = z_; }             // @ 0x1003b2c20
void cSC4RegionalCity::SetPosition(int x, int z, bool fromImport) {                       // @ 0x1003b2aa2
    const int oldX = x_, oldZ = z_;
    x_ = x;
    z_ = z;
    if (!fromImport) return;
    connectionsChanged_ = true;             // +0xf0
    RepairConnectionsAfterImport();
    const int deltaX = x_ - oldX;
    const int deltaZ = z_ - oldZ;
    // Every deal's two cells move with the city.
    for (NeighborDealList* list : {&deals_, &localDeals_}) {
        for (cRZAutoRefCount<sc4::cISC4NeighborDeal>& ref : *list) {
            sc4::cISC4NeighborDeal* deal = ref.get();
            int cellX = 0, cellZ = 0;
            deal->GetRegionCellOnCitySideOfConnection(cellX, cellZ);
            deal->SetRegionCellOnCitySideOfConnection(cellX + deltaX, cellZ + deltaZ);
            deal->GetRegionCellOnOtherSideOfConnection(cellX, cellZ);
            deal->SetRegionCellOnOtherSideOfConnection(cellX + deltaX, cellZ + deltaZ);
        }
    }
}

void cSC4RegionalCity::GetCitySize(int& width, int& height) const { width = sizeX_; height = sizeZ_; }   // @ 0x1003b2c30
void cSC4RegionalCity::SetCitySize(int width, int height) { sizeX_ = width; sizeZ_ = height; }           // @ 0x1003b2c40
int cSC4RegionalCity::GetPopulation() const { return population_; }                                     // @ 0x1003b2c4c
int cSC4RegionalCity::GetCommercialJobs() const { return commercialJobs_; }                             // @ 0x1003b2c56
int cSC4RegionalCity::GetIndustrialJobs() const { return industrialJobs_; }                             // @ 0x1003b2c60

// @ 0x1003b2c6a: the original returns the field's address, not its value.
const float& cSC4RegionalCity::GetWorkforcePercentage() const { return workforcePercentage_; }

int cSC4RegionalCity::GetMayorRating() const { return mayorRating_; }                                   // @ 0x1003b2c74
std::uint32_t cSC4RegionalCity::GetDifficultyLevel() const { return difficultyLevel_; }                 // @ 0x1003b2c7e
std::uint32_t cSC4RegionalCity::GetBirthDate() const { return birthDate_; }                             // @ 0x1003b2db6
void cSC4RegionalCity::SetBirthDate(std::uint32_t date) { birthDate_ = date; }                          // @ 0x1003b2dc2
bool cSC4RegionalCity::GetEstablished() const { return established_; }                                  // @ 0x1003b2dce
void cSC4RegionalCity::SetEstablished(bool established) { established_ = established; }                 // @ 0x1003b2dde
int cSC4RegionalCity::GetTutorialGUID() const { return tutorialGUID_; }                                 // @ 0x1003b2e54
void cSC4RegionalCity::SetTutorialGUID(int guid) { tutorialGUID_ = guid; }                              // @ 0x1003b2e60

// @ 0x1003b2c88 / 0x1003b2c94
std::uint32_t cSC4RegionalCity::GetCitySerialNumber() const { return serialNumber_; }
void cSC4RegionalCity::SetCitySerialNumber(std::uint32_t serial) { serialNumber_ = serial; }

// @ 0x1003b2ca0 / 0x1003b2cb8 (both always answer true)
bool cSC4RegionalCity::GetOriginalLanguageAndCountry(int& language, int& country) const {
    language = originalLanguage_;
    country = originalCountry_;
    return true;
}
bool cSC4RegionalCity::GetLastLanguageAndCountry(int& language, int& country) {
    language = lastLanguage_;
    country = lastCountry_;
    return true;
}

// The eight strings: save file path 0x1003b2cd0/0x1003b2cec, city name 0x1003b2cfe/0x1003b2d1a, mayor name
// 0x1003b2d2c/0x1003b2d48, utility advisor 0x1003b2d5a/0x1003b2d76, description 0x1003b2d88/0x1003b2da4.
void cSC4RegionalCity::GetCitySaveFilePath(cIGZString& out) const { out.AssignFromIGZString(saveFilePath_); }
void cSC4RegionalCity::SetCitySaveFilePath(const cIGZString& path) { saveFilePath_.AssignFromIGZString(path); }
void cSC4RegionalCity::GetCityName(cIGZString& out) const { out.AssignFromIGZString(cityName_); }
void cSC4RegionalCity::SetCityName(const cIGZString& name) { cityName_.AssignFromIGZString(name); }
void cSC4RegionalCity::GetMayorName(cIGZString& out) const { out.AssignFromIGZString(mayorName_); }
void cSC4RegionalCity::SetMayorName(const cIGZString& name) { mayorName_.AssignFromIGZString(name); }
void cSC4RegionalCity::GetUtilityAdvisorName(cIGZString& out) const { out.AssignFromIGZString(utilityAdvisorName_); }
void cSC4RegionalCity::SetUtilityAdvisorName(const cIGZString& name) { utilityAdvisorName_.AssignFromIGZString(name); }
void cSC4RegionalCity::GetCityDescription(cIGZString& out) const { out.AssignFromIGZString(description_); }
void cSC4RegionalCity::SetCityDescription(const cIGZString& description) { description_.AssignFromIGZString(description); }

// @ 0x1003b2dec / 0x1003b2e02
void cSC4RegionalCity::GetWorldPosition(float& longitude, float& latitude) const {
    longitude = worldPositionX_;
    latitude = worldPositionZ_;
}
void cSC4RegionalCity::SetWorldPosition(float longitude, float latitude) {
    worldPositionX_ = longitude;
    worldPositionZ_ = latitude;
}

// @ 0x1003b2e18 / 0x1003b2e26 / 0x1003b2e34 (1 = north of the equator, 2 = south).
float cSC4RegionalCity::GetWorldBaseElevation() const { return worldBaseElevation_; }
void cSC4RegionalCity::SetWorldBaseElevation(float elevation) { worldBaseElevation_ = elevation; }
int cSC4RegionalCity::GetWorldHemisphere() const { return 2 - (worldPositionZ_ > 0.0f ? 1 : 0); }

// @ 0x1003b21dc .. 0x1003b2206
double cSC4RegionalCity::GetBudget() const { return budget_; }
void cSC4RegionalCity::SetBudget(double budget) { budget_ = budget; }
double cSC4RegionalCity::GetIncome() const { return income_; }
void cSC4RegionalCity::SetIncome(double income) { income_ = income; }

// @ 0x1003b2c0a
bool cSC4RegionalCity::IsTutorial() const { return GetTutorialGUID() != -1; }

//---------------------------------------------------------------------------------------------------------------------
// population / tax statistics
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1003b1cf2
int cSC4RegionalCity::GetPopulation(std::uint32_t purpose) const {
    auto it = populationByType_.find(purpose);
    return it == populationByType_.end() ? 0 : it->second;
}

// @ 0x1003b1d30. Quirk: without an entry the original answers the virtual no-argument GetPopulation().
int cSC4RegionalCity::GetExtrapolatedPopulation(std::uint32_t purpose) const {
    auto it = extrapolatedPopulation_.find(purpose);
    if (it != extrapolatedPopulation_.end()) return it->second;
    return GetPopulation();
}

// @ 0x1003b2152. Quirk: both maps are indexed with operator[], so a missing purpose code inserts a zero entry.
int cSC4RegionalCity::GetAllowableExtrapolation(std::uint32_t purpose) const {
    const int potential = potentialPopulation_[purpose];
    const int current = extrapolatedPopulation_[purpose];
    const int available = potential - current;
    return available < 0 ? 0 : available;
}

// @ 0x1003b2198 (the float sum is truncated toward zero)
void cSC4RegionalCity::ExtrapolateGrowth(std::uint32_t purpose, float amount) {
    int& value = extrapolatedPopulation_[purpose];
    value = static_cast<int>(static_cast<float>(value) + amount);
}

// @ 0x1003b2040
float cSC4RegionalCity::GetTaxRate(std::uint32_t purpose) const {
    for (const TaxPurpose& entry : kTaxPurposes) {
        if (purpose == entry.code) return taxRates_[entry.index];
    }
    return kDefaultTaxRate;
}

// @ 0x1003b06e4. The original's signature carries a phantom cISC4City* parameter that receives `this`
// (0x1003b05fb); the port works on the object directly.
void cSC4RegionalCity::CalculatePotentialPopulations() {
    for (const auto& entry : populationByType_) {
        int potential = 0;
        if (entry.second >= 1) {
            float value = static_cast<float>(entry.second) * 1.1f;
            if (value < 1000.0f) value = 1000.0f;
            potential = static_cast<int>(value);
        }
        potentialPopulation_[entry.first] = potential;
    }
}

// @ 0x1003b079a
void cSC4RegionalCity::UpdateExtrapolatedPopulations() {
    for (const auto& entry : populationByType_) extrapolatedPopulation_[entry.first] = entry.second;
}

//---------------------------------------------------------------------------------------------------------------------
// commodity statistics
//---------------------------------------------------------------------------------------------------------------------
// The const getters index the map with operator[], so reading a missing commodity inserts a zeroed record; the
// port keeps that side effect (hence the mutable map).

double cSC4RegionalCity::GetProduced(int commodity) const { return commodityStats_[commodity].produced; }      // @ 0x1003b239e
double cSC4RegionalCity::GetDemanded(int commodity) const { return commodityStats_[commodity].demanded; }      // @ 0x1003b23fe
double cSC4RegionalCity::GetExported(int commodity) const { return commodityStats_[commodity].exported; }      // @ 0x1003b2214
double cSC4RegionalCity::GetImported(int commodity) const { return commodityStats_[commodity].imported; }      // @ 0x1003b233e
double cSC4RegionalCity::GetCostPerUnit(int commodity) const { return commodityStats_[commodity].costPerUnit; } // @ 0x1003b245e

void cSC4RegionalCity::SetProduced(int commodity, double amount) {         // @ 0x1003b23c6
    commodityStats_[commodity].produced = amount < 0.0 ? 0.0 : amount;
}
void cSC4RegionalCity::SetDemanded(int commodity, double amount) {         // @ 0x1003b2426
    commodityStats_[commodity].demanded = amount < 0.0 ? 0.0 : amount;
}
void cSC4RegionalCity::SetExported(int commodity, double amount) {         // @ 0x1003b2306
    commodityStats_[commodity].exported = amount < 0.0 ? 0.0 : amount;
}
void cSC4RegionalCity::SetImported(int commodity, double amount) {         // @ 0x1003b2366
    commodityStats_[commodity].imported = amount < 0.0 ? 0.0 : amount;
}
void cSC4RegionalCity::SetCostPerUnit(int commodity, double cost) {        // @ 0x1003b2486
    commodityStats_[commodity].costPerUnit = cost < 0.0 ? 0.0 : cost;
}

// @ 0x1003b24be: (produced + imported) - (demanded + exported)
double cSC4RegionalCity::GetCommodityBalance(int commodity) const {
    const double imported = GetImported(commodity);
    const double produced = GetProduced(commodity);
    const double exported = GetExported(commodity);
    const double demanded = GetDemanded(commodity);
    return (produced + imported) - (demanded + exported);
}

// @ 0x1003b2826
void cSC4RegionalCity::UpdateImportExport(const sc4::cISC4NeighborDeal* deal) {
    if (!deal) return;
    void* otherCity = deal->GetCityOnOtherSideOfConnection();
    if (!otherCity) return;
    if (deal->CheckFlagsOr(sc4::kDealFlagSilent | 0x3c)) return;    // mask 0xbc
    const int amount = deal->GetAmount();
    const std::uint32_t type = deal->GetType();
    if (type - sc4::kDealTypeBase > 5) return;                       // only 0x4773ffe0..0x4773ffe5
    const std::uint32_t offset = type - sc4::kDealTypeBase;
    const int commodity = 1 + static_cast<int>(offset) / 2;
    const bool lowerType = (offset & 1u) == 0;
    // A lower type records a deal seen from our side (its other city is the neighbour); the upper type is the
    // neighbour's copy of the same deal.
    const bool addToExported = lowerType ? (static_cast<void*>(this) == otherCity)
                                         : (static_cast<void*>(this) != otherCity);
    cCommodityStats& stats = commodityStats_[commodity];
    double& target = addToExported ? stats.exported : stats.imported;
    target += static_cast<double>(amount);
}

// @ 0x1003b296c
bool cSC4RegionalCity::UpdateImportExport() {
    for (int commodity = 1; commodity <= 3; ++commodity) {      // the original zeroes imported before exported
        SetImported(commodity, 0.0);
        SetExported(commodity, 0.0);
    }
    for (cRZAutoRefCount<sc4::cISC4NeighborDeal>& ref : deals_) UpdateImportExport(ref.get());
    for (cRZAutoRefCount<sc4::cISC4NeighborDeal>& ref : localDeals_) UpdateImportExport(ref.get());
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
// connections
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1003b1d78
sc4::cISC4NeighborConnection* cSC4RegionalCity::FindConnection(int type, int x, int z) const {
    for (const cRZAutoRefCount<sc4::cISC4NeighborConnection>& ref : connections_) {
        sc4::cISC4NeighborConnection* connection = ref.get();
        int cellX = 0, cellZ = 0;
        connection->GetConnectionCell(cellX, cellZ);
        if (static_cast<int>(connection->GetType()) == type && cellX == x && cellZ == z) return connection;
    }
    return nullptr;
}

// @ 0x1003b1df6
void cSC4RegionalCity::GetAllConnections(NeighborConnectionList& out) const {
    out.clear();
    out.insert(out.end(), connections_.begin(), connections_.end());
}

// @ 0x1003b1e34
void cSC4RegionalCity::ChangeSymmetricConnection(sc4::cISC4NeighborConnection* connection, bool add) {
    if (!connection) return;
    int cellX = 0, cellZ = 0;
    connection->GetConnectionCell(cellX, cellZ);
    cISC4RegionalCity* neighborCity = nullptr;
    int neighborX = 0, neighborZ = 0, connectionCellX = 0, connectionCellZ = 0;
    if (host_) {
        host_->ConvertConnectionCellToNeighborCell(cellX, cellZ, neighborX, neighborZ, neighborCity,
                                                   connectionCellX, connectionCellZ);
    }
    ConnectionChange change;
    change.type = connection->GetType();
    change.x = connectionCellX;
    change.z = connectionCellZ;
    change.flag = add;
    connectionChanges_.push_back(change);
}

// @ 0x1003b1efe
void cSC4RegionalCity::RepairConnectionsAfterImport() {
    NeighborConnectionList all;
    GetAllConnections(all);
    for (cRZAutoRefCount<sc4::cISC4NeighborConnection>& ref : all) ref->Repair(this);
    all.clear();
}

// @ 0x1003b0824
void cSC4RegionalCity::MarkConnectionsForRemoval() {
    NeighborConnectionList all;
    GetAllConnections(all);
    for (cRZAutoRefCount<sc4::cISC4NeighborConnection>& ref : all) {
        sc4::cISC4NeighborConnection* connection = ref.get();
        int cellX = 0, cellZ = 0;
        connection->GetConnectionInnerCell(cellX, cellZ);
        cISC4RegionalCity* otherCity = host_ ? host_->FindCityOnOtherSideOfConnection(cellX, cellZ) : nullptr;
        if (!otherCity || !host_->RegionalCityEstablished(otherCity)) continue;
        ConnectionChange change;
        change.type = connection->GetType();
        change.x = cellX;
        change.z = cellZ;
        change.flag = false;
        connectionChanges_.push_back(change);
    }
}

// @ 0x1003b0948
void cSC4RegionalCity::TreatAllIncomingConnectionsAsNew() {
    if (!host_) return;
    std::list<cRZAutoRefCount<cISC4RegionalCity>> adjacent;
    host_->GetAdjacentCities(adjacent, true);
    for (cRZAutoRefCount<cISC4RegionalCity>& cityRef : adjacent) {
        cISC4RegionalCity* city = cityRef.get();
        NeighborConnectionList incoming;
        host_->RegionalCityConnections(city, incoming);
        for (cRZAutoRefCount<sc4::cISC4NeighborConnection>& ref : incoming) {
            sc4::cISC4NeighborConnection* connection = ref.get();
            if (connection->GetCityOnOtherSideOfConnection() != this) continue;
            int neighborX = 0, neighborZ = 0;
            connection->GetRegionCellOnOtherSideOfConnection(neighborX, neighborZ);
            int connectionX = 0, connectionZ = 0;
            host_->ConvertNeighborCellToConnectionCell(city, neighborX, neighborZ, connectionX, connectionZ);
            ConnectionChange change;
            change.type = connection->GetType();
            change.x = connectionX;
            change.z = connectionZ;
            change.flag = true;
            connectionChanges_.push_back(change);
        }
    }
}

//---------------------------------------------------------------------------------------------------------------------
// local deals
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1003b2534. A deal is local when it involves the city the application is in; the deals whose two region
// cells have no city are always dropped from `deals` (whatever `removeEmpty` says). `removeEmpty` additionally
// moves the surviving local deals out of `deals`.
void cSC4RegionalCity::ExtractLocalDeals(NeighborDealList& deals, NeighborDealList& out, bool removeEmpty) {
    static const bool trace = std::getenv("SC4_NEIGHBORDEBUG") != nullptr;
    if (trace) std::fprintf(stderr, "regionalcity %p: extract from %zu deals, host %d city %d neighbours %d\n", static_cast<void*>(this), deals.size(),
                            host_ != nullptr, host_ && host_->CurrentCity(), host_ && host_->HasNeighborsSimulator());
    if (!host_ || !host_->CurrentCity() || !host_->HasNeighborsSimulator()) return;
    cISC4RegionalCity* current = host_->CurrentRegionalCity();
    std::vector<NeighborDealList::iterator> toErase;
    for (auto it = deals.begin(); it != deals.end(); ++it) {
        sc4::cISC4NeighborDeal* deal = it->get();
        int cellX = 0, cellZ = 0;
        deal->GetRegionCellOnCitySideOfConnection(cellX, cellZ);
        cISC4RegionalCity* cityA = host_->RegionCityAt(cellX, cellZ);
        deal->GetRegionCellOnOtherSideOfConnection(cellX, cellZ);
        cISC4RegionalCity* cityB = host_->RegionCityAt(cellX, cellZ);
        if (!cityA || !cityB) {
            if (trace) std::fprintf(stderr, "regionalcity %p:   deal type %08x dropped: cities %p %p\n", static_cast<void*>(this), deal->GetType(),
                                    static_cast<void*>(cityA), static_cast<void*>(cityB));
            toErase.push_back(it);
            continue;
        }
        bool local;
        if (this == current) {
            local = cityA == current || cityB == current;
        } else {
            local = (cityA == current && cityB == this) || (cityA == this && cityB == current);
        }
        if (trace) std::fprintf(stderr, "regionalcity %p:   deal type %08x cities %p %p current %p\n", static_cast<void*>(this), deal->GetType(),
                                static_cast<void*>(cityA), static_cast<void*>(cityB), static_cast<void*>(current));
        if (!local) continue;
        out.push_back(*it);
        if (removeEmpty) toErase.push_back(it);
    }
    for (auto it = toErase.rbegin(); it != toErase.rend(); ++it) deals.erase(*it);
}

// @ 0x1003b27ca
bool cSC4RegionalCity::UpdateLocalDeals() {
    ExtractLocalDeals(deals_, localDeals_, true);
    for (cRZAutoRefCount<sc4::cISC4NeighborDeal>& ref : deals_)
        ref->SetFlags(sc4::kDealFlagSilent, false);
    return true;
}

// @ 0x1003b275a
bool cSC4RegionalCity::SetLocalDeals(NeighborDealList& deals) {
    localDeals_.clear();
    ExtractLocalDeals(deals, localDeals_, false);
    UpdateImportExport();
    return true;
}

// @ 0x1003b27a2
bool cSC4RegionalCity::GetLocalDeals(NeighborDealList& out) {
    if (&localDeals_ != &out) out.assign(localDeals_.begin(), localDeals_.end());
    return true;
}

// @ 0x1003b2a46. Quirk: an unknown type leaves the caller's vector untouched.
void cSC4RegionalCity::GetPointsOfInterest(PointOfInterestType type, std::vector<PointOfInterest>& out) {
    auto it = pointsOfInterest_.find(type);
    if (it == pointsOfInterest_.end()) return;
    if (&it->second == &out) return;
    out.assign(it->second.begin(), it->second.end());
}

//---------------------------------------------------------------------------------------------------------------------
// preferences / city cache
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1003af8f8. Quirk: both names are copied with strcpy, so the preference arrays must already be big enough.
void cSC4RegionalCity::SetupPreferences(SC4NewCityPreferences* preferences) {
    if (!preferences) return;
    std::strcpy(preferences->cityName, cityName_.Data());
    std::strcpy(preferences->mayorName, mayorName_.Data());
    preferences->difficultyLevel = difficultyLevel_;
}

// @ 0x1003af960
void cSC4RegionalCity::UpdateCityCache(SC4NewCityPreferences* preferences) {
    if (!preferences) return;
    cityName_.AssignFromCString(preferences->cityName);
    mayorName_.AssignFromCString(preferences->mayorName);
    difficultyLevel_ = preferences->difficultyLevel;
    // The new save file keeps the drive/directory of the previous path and takes the city name.
    const cRZPath path(std::string(saveFilePath_.Data(), saveFilePath_.Strlen()));
    const std::string newPath = path.GetDriveAndDirectory() + preferences->cityName + kCitySaveExtension;
    saveFilePath_.AssignFromCString(newPath.c_str());
}

// @ 0x1003afd9a
void cSC4RegionalCity::SetupCity(cISC4City* city) {
    if (!city) return;
    if (host_) {
        host_->SetCityMayorName(city, mayorName_);            // city vslot 0x88
        host_->SetCityName(city, cityName_);                  // 0x68
        host_->SetCityDifficulty(city, difficultyLevel_);     // 0xc8
    }
    cRZString path;
    if (host_) host_->CreateSavePathForCity(cityName_, 0, path);   // SC4CreateSavePathForCity(name, 0, out)
    city->SetCitySaveFilePath(path);                          // 0x58
    // The region stores the size in region cells; the city takes metres (one cell is 1024 m).
    if (host_) host_->SetCitySize(city, static_cast<float>(sizeX_ << 10), static_cast<float>(sizeZ_ << 10));
}

// @ 0x1003afe7a
void cSC4RegionalCity::UpdateCityCache(cISC4City* city) {
    if (!city || !host_) return;
    host_->CityMayorName(city, mayorName_);                  // 0x80
    host_->CityName(city, cityName_);                        // 0x60
    difficultyLevel_ = host_->CityDifficulty(city);          // 0xc0
    city->GetCitySaveFilePath(saveFilePath_);                // 0x50
    SetCitySerialNumber(host_->CitySerialNumber(city));      // 0x28 -> vslot 0xe0
    SetEstablished(host_->CityEstablished(city));            // 0xb0 -> vslot 0x160
    mayorRating_ = static_cast<std::int8_t>(host_->CityMayorRating(city));   // 0x198 -> 0x40
    host_->CityTaxRates(city, taxRates_);                    // 0x1a0 -> 0x120 (12 rates)
    host_->CityConnections(city, connections_);              // 0x1f0 -> 0x80
    host_->CityUpdateCommodityStats(city);                   // 0x1f0 -> 0x110
    int tutorialGUID = 0;
    if (host_->CityTutorialGUID(city, tutorialGUID) && tutorialGUID_ == -1)   // 0x290 -> 0x60
        tutorialGUID_ = tutorialGUID;
    int population = 0;
    float workforce = 0;
    if (host_->CityPopulationStats(city, population, workforce)) {           // 0x220 -> 0x80 / 0xf8
        population_ = population;
        workforcePercentage_ = workforce;
    }
    host_->CityLocalPopulationSummary(city, populationByType_);              // 0x1c0 -> 0x58

    // Jobs from the demand simulator: each purpose code contributes the population of that purpose times the
    // sum of the last three entries of CalculateJobsPerUnitOfDemand's four-float answer.
    auto jobsFromPurposes = [&](const std::uint32_t* purposes, std::size_t count) {
        int jobs = 0;
        for (std::size_t i = 0; i < count; ++i) {
            float units[4] = {0, 0, 0, 0};
            host_->CityJobsPerUnitOfDemand(city, units, purposes[i]);          // 0x1c0 -> 0x40
            const float perUnit = units[1] + units[2] + units[3];
            jobs += static_cast<int>(static_cast<float>(GetPopulation(purposes[i])) * perUnit);
        }
        return jobs;
    };
    static constexpr std::uint32_t kCommercialPurposes[] = {0x3111, 0x3121, 0x3131, 0x3321, 0x3331};
    static constexpr std::uint32_t kIndustrialPurposes[] = {0x4101, 0x4201, 0x4301, 0x4401};
    commercialJobs_ = jobsFromPurposes(kCommercialPurposes, std::size(kCommercialPurposes));
    industrialJobs_ = jobsFromPurposes(kIndustrialPurposes, std::size(kIndustrialPurposes));

    // Points of interest: both types are inserted even when the developer has no active lot.
    pointsOfInterest_.clear();
    host_->AirportPointsOfInterest(pointsOfInterest_[kPointOfInterestAirport]);   // SL::spAirportDeveloper
    host_->SeaportPointsOfInterest(pointsOfInterest_[kPointOfInterestSeaport]);   // SL::spSeaportDeveloper
}

//---------------------------------------------------------------------------------------------------------------------
// message handling
//---------------------------------------------------------------------------------------------------------------------

namespace {

// Moves the incoming-deal list (+0x2a8) into the deal list (+0x290), back to front.
void MoveIncomingDeals(NeighborDealList& from, NeighborDealList& to) {
    while (!from.empty()) {
        to.push_back(from.back());
        from.pop_back();
    }
}

}  // namespace

// @ 0x1003b04f8. PARTIAL: the original ends this handler by calling CreateSymmetricConnections (0x1003b0ae0), of which only the
// closing message is ported (see the header note); everything before that call is ported.
bool cSC4RegionalCity::DoMessage(cIGZMessage2* message) {
    if (!message) return true;
    auto* standard = static_cast<cIGZMessage2Standard*>(message);
    const std::uint32_t type = message->GetType();
    cISC4City* city = host_ ? host_->CurrentCity() : nullptr;
    if (type == kRegionalCityMessageDealsChanged) {
        MoveIncomingDeals(localDeals_, deals_);
        return true;
    }
    if (type == kRegionalCityMessageCityInit2) {
        if (standard->GetVoid1() != city) {
            if (host_) host_->CurrentRegionalCity();
            return true;
        }
        MoveIncomingDeals(localDeals_, deals_);
        return true;
    }
    cISC4RegionalCity* current = host_ ? host_->CurrentRegionalCity() : nullptr;
    if (type != kRegionalCityMessageCityChanged) {
        if (type == kRegionalCityMessageCityInit) {
            if (!city || current != this) return true;
            UpdateCityCache(city);
            return true;
        }
        if (type != kRegionalCityMessageCityStatistics) return true;
        if (!city || current != this) return true;
        UpdateCityCache(city);
        CalculatePotentialPopulations();
        UpdateExtrapolatedPopulations();
        return true;
    }
    if (!city || current != this) return true;
    // The original reads data 1 and data 2 and drops both answers (0x1003b0625, 0x1003b062e).
    (void)standard->GetData1();
    (void)standard->GetData2();
    const std::uint64_t data3 = standard->GetData3();
    const bool established = host_ && host_->CityEstablished(city);
    if (data3 != 0 || !established) {
        connectionChanges_.clear();
        if (connectionsChanged_) MarkConnectionsForRemoval();
        TreatAllIncomingConnectionsAsNew();
    } else if (connectionsChanged_) {
        connectionChanges_.clear();
        MarkConnectionsForRemoval();
        if (connectionsChanged_) TreatAllIncomingConnectionsAsNew();
    }
    // CreateSymmetricConnections (i386 0x002cc2d0): PARTIAL, the connections are not made (the changes stay queued); its last step is
    // ported, the 0x6ac284f3 it sends once the city's connections stand (the power, plumbing and neighbours simulators start on it).
    if (host_) host_->SendMessage(0x6ac284f3);
    return true;
}

//---------------------------------------------------------------------------------------------------------------------
// persistence
//---------------------------------------------------------------------------------------------------------------------

// @ 0x1003ae4f8 (version 13)
bool cSC4RegionalCity::Write(cIGZOStream& stream) const {
    stream.SetUint16(kRegionalCityWriteVersion);
    stream.SetSint32(x_);
    stream.SetSint32(z_);
    stream.SetSint32(sizeX_);
    stream.SetSint32(sizeZ_);
    stream.SetSint32(population_);
    stream.SetSint32(commercialJobs_);
    stream.SetSint32(industrialJobs_);
    stream.SetFloat32(workforcePercentage_);
    stream.SetSint8(mayorRating_);
    stream.SetUint8(static_cast<std::uint8_t>(difficultyLevel_));
    stream << connectionsChanged_;
    stream.SetUint32(serialNumber_);
    stream.SetSint32(originalLanguage_);
    stream.SetSint32(originalCountry_);
    stream.SetSint32(lastLanguage_);
    stream.SetSint32(lastCountry_);
    stream.SetUint32(birthDate_);
    stream.SetUint8(established_ ? 1 : 0);
    stream.SetGZStr(cityName_);
    stream.SetGZStr(cityNameOriginal_);
    stream.SetGZStr(mayorName_);
    stream.SetGZStr(mayorNameOriginal_);
    stream.SetGZStr(utilityAdvisorName_);
    stream.SetGZStr(description_);
    stream.SetGZStr(descriptionOriginal_);
    stream.SetFloat32(worldPositionX_);
    stream.SetFloat32(worldPositionZ_);
    stream.SetFloat32(worldBaseElevation_);
    stream.SetSint32(tutorialGUID_);

    const auto writePopulationMap = [&stream](const std::map<std::uint32_t, int>& map) {
        stream.SetUint32(static_cast<std::uint32_t>(map.size()));
        for (const auto& entry : map) {
            stream.SetUint32(entry.first);
            stream.SetSint32(entry.second);
        }
    };
    writePopulationMap(populationByType_);
    writePopulationMap(extrapolatedPopulation_);
    writePopulationMap(potentialPopulation_);

    stream.SetUint32(12);
    for (float rate : taxRates_) stream.SetFloat32(rate);

    stream.SetUint32(static_cast<std::uint32_t>(connections_.size()));
    for (const cRZAutoRefCount<sc4::cISC4NeighborConnection>& ref : connections_) {
        cIGZSerializable* serializable = nullptr;
        if (ref->QueryInterface(sc4::kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable)))
            serializable->Write(stream);
        if (serializable) serializable->Release();
    }

    stream.SetUint32(static_cast<std::uint32_t>(connectionChanges_.size()));
    for (const ConnectionChange& change : connectionChanges_) {
        stream.SetUint32(change.type);
        stream.SetSint32(change.x);
        stream.SetSint32(change.z);
        stream << change.flag;
    }

    stream.SetFloat64(budget_);
    stream.SetFloat64(income_);

    stream.SetSint32(static_cast<std::int32_t>(commodityStats_.size()));
    for (const auto& entry : commodityStats_) {
        stream.SetSint32(entry.first);
        stream.SetFloat64(entry.second.produced);
        stream.SetFloat64(entry.second.demanded);
        stream.SetFloat64(entry.second.imported);
        stream.SetFloat64(entry.second.exported);
        stream.SetFloat64(entry.second.costPerUnit);
    }

    const auto writeDealList = [&stream](const NeighborDealList& list, bool checked) {
        stream.SetSint32(static_cast<std::int32_t>(list.size()));
        for (const cRZAutoRefCount<sc4::cISC4NeighborDeal>& ref : list) {
            cIGZSerializable* serializable = nullptr;
            if (ref->QueryInterface(sc4::kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable))) {
                serializable->Write(stream);
            } else if (!checked) {
                // (sic) the original does not check the second list's QueryInterface answer.
                serializable->Write(stream);
            }
            if (serializable) serializable->Release();
        }
    };
    writeDealList(deals_, true);
    writeDealList(localDeals_, false);

    stream.SetUint32(static_cast<std::uint32_t>(pointsOfInterest_.size()));
    for (const auto& entry : pointsOfInterest_) {
        stream.SetUint32(static_cast<std::uint32_t>(entry.first));
        stream.SetUint32(static_cast<std::uint32_t>(entry.second.size()));
        for (const PointOfInterest& point : entry.second) {
            stream.SetFloat32(point.x);
            stream.SetFloat32(point.y);
            stream.SetFloat32(point.z);
        }
    }
    return !stream.GetError();
}

// @ 0x1003aec4a
bool cSC4RegionalCity::Read(cIGZIStream& stream) {
    std::uint16_t version = 0;
    stream.GetUint16(version);
    if (version >= 14) return false;
    stream.GetSint32(x_);
    stream.GetSint32(z_);
    stream.GetSint32(sizeX_);
    stream.GetSint32(sizeZ_);
    stream.GetSint32(population_);
    stream.GetSint32(commercialJobs_);
    stream.GetSint32(industrialJobs_);
    if (version < 11) {
        if (version == 10) {
            float percent = 0;
            stream.GetFloat32(percent);
            workforcePercentage_ = percent * 0.01f;
        } else {
            workforcePercentage_ = 0.5f;
        }
    } else {
        stream.GetFloat32(workforcePercentage_);
    }
    std::int8_t mayorRating = 0;
    stream.GetSint8(mayorRating);
    mayorRating_ = mayorRating;
    std::uint8_t difficulty = 2;
    if (version > 12) stream.GetUint8(difficulty);
    stream >> connectionsChanged_;
    stream.GetUint32(serialNumber_);
    stream.GetSint32(originalLanguage_);
    stream.GetSint32(originalCountry_);
    stream.GetSint32(lastLanguage_);
    stream.GetSint32(lastCountry_);
    stream.GetUint32(birthDate_);
    std::uint8_t established = 0;
    stream.GetUint8(established);
    established_ = established != 0;
    stream.GetGZStr(cityName_);
    stream.GetGZStr(cityNameOriginal_);
    stream.GetGZStr(mayorName_);
    stream.GetGZStr(mayorNameOriginal_);
    stream.GetGZStr(utilityAdvisorName_);
    stream.GetGZStr(description_);
    stream.GetGZStr(descriptionOriginal_);
    stream.GetFloat32(worldPositionX_);
    stream.GetFloat32(worldPositionZ_);
    stream.GetFloat32(worldBaseElevation_);
    stream.GetSint32(tutorialGUID_);
    // Quirk: the difficulty level read from the record only sticks for an established city.
    if (established_) difficultyLevel_ = difficulty;
    if (host_) tutorialGUID_ = host_->TutorialGUIDFromStoredValue(tutorialGUID_);

    const auto readPopulationMap = [&stream](std::map<std::uint32_t, int>& map) {
        std::uint32_t count = 0;
        stream.GetUint32(count);
        if (count >= kReadMapCountLimit) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            std::uint32_t key = 0;
            std::int32_t value = 0;
            stream.GetUint32(key);
            stream.GetSint32(value);
            map[key] = value;
        }
        return true;
    };
    if (!readPopulationMap(populationByType_)) return false;
    if (!readPopulationMap(extrapolatedPopulation_)) return false;
    if (!readPopulationMap(potentialPopulation_)) return false;

    std::uint32_t taxCount = 0;
    stream.GetUint32(taxCount);
    if (taxCount >= kReadTaxCountLimit) return false;
    for (std::uint32_t i = 0; i < taxCount; ++i) stream.GetFloat32(taxRates_[i]);

    std::uint32_t connectionCount = 0;
    stream.GetUint32(connectionCount);
    if (connectionCount >= kReadListCountLimit) return false;
    for (std::uint32_t i = 0; i < connectionCount; ++i) {
        auto* connection = new sc4::cSC4NeighborConnection();
        connection->AddRef();
        connections_.push_back(cRZAutoRefCount<sc4::cISC4NeighborConnection>(connection));
        connection->Release();
        cIGZSerializable* serializable = nullptr;
        if (connection->QueryInterface(sc4::kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable))) {
            serializable->Read(stream);
            serializable->Release();
        }
    }

    std::uint32_t changeCount = 0;
    stream.GetUint32(changeCount);
    if (changeCount >= kReadListCountLimit) return false;
    for (std::uint32_t i = 0; i < changeCount; ++i) {
        ConnectionChange change;
        stream.GetUint32(change.type);
        stream.GetSint32(change.x);
        stream.GetSint32(change.z);
        stream >> change.flag;
        connectionChanges_.push_back(change);
    }

    stream.GetFloat64(budget_);
    stream.GetFloat64(income_);

    std::int32_t commodityCount = 0;
    stream.GetSint32(commodityCount);
    if (static_cast<std::uint32_t>(commodityCount) >= kReadListCountLimit) return false;
    for (std::int32_t i = 0; i < commodityCount; ++i) {
        std::int32_t commodity = 0;
        stream.GetSint32(commodity);
        cCommodityStats& stats = commodityStats_[commodity];
        stream.GetFloat64(stats.produced);
        stream.GetFloat64(stats.demanded);
        stream.GetFloat64(stats.imported);
        stream.GetFloat64(stats.exported);
        stream.GetFloat64(stats.costPerUnit);
    }

    // Quirk: both deal blocks are appended to the deals list; the local-deals list stays empty after a load.
    const auto readDeals = [&stream, this](std::int32_t count) {
        for (std::int32_t i = 0; i < count; ++i) {
            auto* deal = new sc4::cSC4NeighborDeal();
            deal->AddRef();
            bool read = false;
            cIGZSerializable* serializable = nullptr;
            if (deal->QueryInterface(sc4::kGZIID_cIGZSerializable, reinterpret_cast<void**>(&serializable))) {
                read = serializable->Read(stream);
                serializable->Release();
            }
            if (read) {
                deal->AddRef();
                deals_.push_back(cRZAutoRefCount<sc4::cISC4NeighborDeal>(deal));
                deal->Release();
            }
            deal->SetFlags(sc4::kDealFlagSilent, true);
            deal->Release();
        }
    };
    std::int32_t dealCount = 0;
    stream.GetSint32(dealCount);
    if (static_cast<std::uint32_t>(dealCount) >= kReadListCountLimit) return false;
    readDeals(dealCount);
    // (sic) the second block is the local-deals list in the writer but is read into the same list here; the
    // original does not range-check this count either.
    stream.GetSint32(dealCount);
    readDeals(dealCount);

    if (version > 11) {
        std::uint32_t typeCount = 0;
        stream.GetUint32(typeCount);
        for (std::uint32_t i = 0; i < typeCount; ++i) {
            std::uint32_t type = 0;
            std::uint32_t pointCount = 0;
            stream.GetUint32(type);
            stream.GetUint32(pointCount);
            std::vector<PointOfInterest>& points =
                pointsOfInterest_[static_cast<PointOfInterestType>(type)];
            for (std::uint32_t j = 0; j < pointCount; ++j) {
                PointOfInterest point;
                stream.GetFloat32(point.x);
                stream.GetFloat32(point.y);
                stream.GetFloat32(point.z);
                points.push_back(point);
            }
        }
    }

    if (!established_ && host_) host_->LoadString(kRegionalCityDefaultCityNameGroup,
                                                  kRegionalCityDefaultCityNameInstance, cityName_);

    const int language = host_ ? host_->UILanguage() : 0;      // RZ::LM() vslot 0x38
    cIGZPersistDBSegment* segment = nullptr;
    void* raw = nullptr;
    if (stream.QueryInterface(sc4::kGZIID_cISC4DBSegmentIStream, &raw)) {
        auto* db = static_cast<sc4::cISC4DBSegmentIStream*>(raw);
        // (sic) the original drops the reference the query added *before* asking the stream for its segment;
        // it stays alive because the caller owns another reference.
        db->Release();
        if (sc4::cISC4DBSegment* owner = db->GetSegment()) segment = owner->AsIGZPersistDBSegment();
    }
    cRZString text;
    if (host_) {   // the original calls SC4ReadStringRecord, which dereferences the segment
        if (host_->ReadStringRecord(segment, kRegionalCityRecordIdCityName,
                                    static_cast<std::uint32_t>(language), text))
            cityName_.AssignFromIGZString(text);
        if (host_->ReadStringRecord(segment, kRegionalCityRecordIdMayorName,
                                    static_cast<std::uint32_t>(language), text))
            mayorName_.AssignFromIGZString(text);
    }
    return !stream.GetError();
}

// @ 0x1003b2e6c
std::uint32_t cSC4RegionalCity::GetGZCLSID() const { return kGZCLSID_cSC4RegionalCity; }
