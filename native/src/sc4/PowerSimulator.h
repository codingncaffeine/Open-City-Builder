// cSC4PowerSimulator (class id 0x0990c005) — the power grid: which cells carry power (the wire bits: buildings, poles, power
// carrying network pieces and pylons, each spread over a neighbourhood), which of them are powered (a flood from each plant,
// limited by what the plants and the neighbour deals supply, block by block), the supply grid the lots and the data view read,
// plant output, wear and funding, pole funding and the monthly power statistics.
//
// Ported from the Mac i386 build (field map: scratch/s41/power-notes.md); the i386 offset is given per field. The city services the
// simulator reaches (occupant manager, budget, neighbours, lots, history, messages, the simulator clock) go through IPowerHost; the
// occupants themselves are the game's cISC4Occupant.
#pragma once
#include <array>
#include <cstdint>
#include <list>
#include <vector>

#include "gz/Bit1.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "rz/cRZRandom.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"

class cIGZIStream;
class cIGZOStream;
class cIGZMessageTarget2;

namespace sc4 {

class cISC4Occupant;
class cISC4NeighborDeal;
class cISC4NeighborConnection;

constexpr std::uint32_t kGZCLSID_cSC4PowerSimulator = 0x0990c005;
constexpr std::uint32_t kGZIID_cISC4PowerSimulator = 0xe9e7bff5;
constexpr std::uint32_t kPowerDepartmentIid = 0x223e2b67;  // the power department's budget (+0x20)
constexpr std::uint32_t kSupplyGridIid = 0xc9bd939a;       // uint8 sim grid (the supply grid, +0x80)
constexpr std::uint32_t kPowerPoleIid = 0xa8c86142;        // cISC4PowerPoleOccupant
constexpr std::uint32_t kPowerConsumed = 0x27812854;       // building exemplar: power consumed
constexpr std::uint32_t kPowerPlantMaxProduced = 0x27812852;      // exemplar property: max output
constexpr std::uint32_t kPowerPlantPowerType = 0x27812853;
constexpr std::uint32_t kPowerPlantCurrentProduced = 0x08d9763d;
constexpr std::uint32_t kPowerPlantWear = 0xc910b4cf;              // 0..1+ age / wear
constexpr std::uint32_t kPowerPlantBudgetProperty = 0xca58e540;
constexpr std::uint32_t kPowerPlantConditionCurve = 0x8a1e07ee;
constexpr std::uint32_t kPowerPoleUpkeep = 0x09132de8;             // int64 monthly cost of a pole

// occupant types that feed the wire map
constexpr std::uint32_t kPowerOccupantBuilding = 0x278128a0, kPowerOccupantPole = 0x2890d4de,
                        kPowerOccupantNetwork = 0x0990b2ea, kPowerOccupantPylon = 0xc772bf98;

// neighbour deals: power the city buys (0x4773ffe0) and sells (0x4773ffe1)
constexpr std::uint32_t kPowerDealImport = 0x4773ffe0, kPowerDealExport = 0x4773ffe1;

// messages
constexpr std::uint32_t kMsgPowerCityEstablished = 0x6ac284f3;   // the simulator starts working
constexpr std::uint32_t kMsgPowerOccupantInserted = 0x99ef1142, kMsgPowerOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kMsgPowerTick = 0x66956814, kMsgPowerTickFast = 0x6695681a, kMsgPowerMonth = 0x66956816;
constexpr std::uint32_t kMsgPowerTunablesChanged = 0xca1b151c, kMsgPowerLotStateChanged = 0xe8bda2e7;
constexpr std::uint32_t kMsgPowerDealsChanged = 0xea418d8f, kMsgPowerPoleCosts = 0x26d31ec1, kMsgPowerNetworkCells = 0x07699455;
constexpr std::uint32_t kMsgPowerTerrain = 0x278e6003, kMsgPowerCheat = 0xc2c09554;
constexpr std::uint32_t kMsgPowerCellsChanged = 0x8a07ae00, kMsgPowerGridChanged = 0x6a07c2ee;

struct PowerRect { int left = 0, top = 0, right = 0, bottom = 0; };   // inclusive

// What the budget simulator tracks for a power plant (budget vslot 0xcc with 0xca58e540).
struct PowerPlantBudgetEntry {
    bool tracked = false;
    float funding = 0.0f;           // fraction of full funding
    bool hasExplicitFunding = false;
    std::int64_t monthlyCost = 0;
};

// The primary view the city holds for this simulator (cISC4PowerSimulator, iid 0xe9e7bff5). The port only
// answers its identity through it; the query surface lives on the concrete class.
class cISC4PowerSimulator : public cIGZUnknown {};

class IPowerHost {
public:
    virtual ~IPowerHost() = default;
    virtual int CityCellsX() = 0;                  // city vslot 0x174
    virtual int CityCellsZ() = 0;                  // city vslot 0x178
    virtual float CellSizeX() = 0;
    virtual float CellSizeZ() = 0;
    // occupant manager vslot 0xa4 (cells, any type): the occupants over the cell rectangle (borrowed)
    virtual void OccupantsInRect(const PowerRect& rect, std::vector<cISC4Occupant*>& out) = 0;
    // The cell a pole (iid 0xa8c86142 vslot 0x20, not while its vslot 0x34 holds), a network piece (iid 0xc98f3b24 vslot 0x14,
    // not while its vslot 0x18 holds) or a pylon (iid 0xa821ef94 vslot 0xec, flagged 0x40000 and not 0x80000000) wires.
    virtual bool WireCell(cISC4Occupant* occupant, int& x, int& z) = 0;
    virtual bool IsZoned(int x, int z) = 0;                               // zone manager vslot 0x30 GetZoneType != 0 (zones conduct)
    virtual void SetSupplyValue(int x, int z, std::uint8_t value) = 0;    // supply grid (+0x80)
    virtual float OrdinancePowerFactor() = 0;                             // ordinance 0x0911e117 (+0x2b0)
    virtual PowerPlantBudgetEntry GetPlantBudgetEntry(cISC4Occupant* plant) = 0;
    virtual float DepartmentFundingFraction() = 0;                        // the power department's funding (+0x20 vslot 0x28)
    virtual const cISCPropertyHolder* SimulatorExemplar() { return nullptr; }   // {0x6534284a, 0xe7e2c2db, 0xc911e35b}
    // The power department's generation line item (+0x20 vslot 0x50(1) -> vslot 0x30) and the department total (budget 0x88).
    virtual void SetGenerationCost(std::int64_t cost) { (void)cost; }
    virtual std::int64_t DepartmentExpense() { return 0; }
    // The plant's budget item (0xca58e540): its monthly expense follows 0x2a58d350 scaled by the plant's 0x0911e2e7 wear curve.
    virtual void SetPlantExpense(cISC4Occupant* plant, std::int64_t expense) { (void)plant; (void)expense; }
    virtual void AddPlantToBudget(cISC4Occupant* plant) { (void)plant; }        // DoOccupantInserted's budget item
    virtual void RemovePlantFromBudget(cISC4Occupant* plant) { (void)plant; }   // DoOccupantRemoved's
    // lot manager vslot 0x38 (the lot of an occupant) / 0x14 (the lot at a cell), then lot SetHasPower(true) (vslot 0x10c)
    virtual void SetOccupantLotPowered(cISC4Occupant* building) { (void)building; }
    virtual void SetLotPoweredAt(int x, int z) { (void)x; (void)z; }
    // 0xe8bda2e7's lot (QI 0xe818fced) when its state is 1: the centre cell of its bounding rectangle (lot vslots 0x84 / 0x38)
    virtual bool LotCentreIfStateOne(cIGZUnknown* object, int& x, int& z) { (void)object; (void)x; (void)z; return false; }
    // AgePowerPlant past the plant's second wear limit: its lot gets condition 2
    virtual void DistressPlantLot(cISC4Occupant* plant) { (void)plant; }
    // neighbours (+0x30): deals of a type (vslot 0x74), power connections (0x44, or 0x48 to one city), an unvisited connection to a
    // city (0x3c) and whether the connection's other side is live (0x38 -> 0x34); the deal's city is its vslot [7]
    virtual void Deals(std::uint32_t type, std::vector<cISC4NeighborDeal*>& out) { (void)type; (void)out; }
    virtual void* DealCity(cISC4NeighborDeal* deal) { (void)deal; return nullptr; }
    virtual void Connections(void* city, std::vector<cISC4NeighborConnection*>& out) { (void)city; (void)out; }   // null = all
    virtual cISC4NeighborConnection* UnvisitedConnection(void* city) { (void)city; return nullptr; }
    virtual bool OtherSideActive(cISC4NeighborConnection* connection) { (void)connection; return false; }
    virtual void RefreshNeighborDeals() {}                                // spNeighborsSim vslot 0x7c
    // services
    virtual void PostMessage(std::uint32_t type, std::uint32_t data, const PowerRect* rect) { (void)type; (void)data; (void)rect; }
    virtual void AddHistory(std::uint32_t id, std::int64_t value) { (void)id; (void)value; }
    virtual void CreateHistory(std::uint32_t id, std::uint32_t length) { (void)id; (void)length; }
    virtual void AddAgent(cIGZMessageTarget2* target, int interval, const char* name, std::uint32_t flags) { (void)target; (void)interval; (void)name; (void)flags; }
    virtual void RemoveAgent(cIGZMessageTarget2* target) { (void)target; }
    virtual void AddNotification(cIGZMessageTarget2* target, std::uint32_t message) { (void)target; (void)message; }
    virtual void RemoveNotification(cIGZMessageTarget2* target, std::uint32_t message) { (void)target; (void)message; }
    // poles (pole funding switches them on and off: pole vslots 0x58 / 0x60)
    virtual bool PoleEnabled(cISC4Occupant* pole) { (void)pole; return true; }
    virtual void SetPoleEnabled(cISC4Occupant* pole, bool enabled) { (void)pole; (void)enabled; }
    // ---- stream seams: the nested serializables, through SC4Write/ReadGZSerializable (kept out of the simulator's translation unit)
    virtual void WriteDepartment(cIGZOStream&) {}
    virtual bool ReadDepartment(cIGZIStream&) { return false; }
    virtual void WriteSupplyGrid(cIGZOStream&) {}
    virtual bool ReadSupplyGrid(cIGZIStream&) { return false; }
    virtual void WriteOccupant(cIGZOStream&, cISC4Occupant*) {}
    // one reference to the occupant (null when the reference is empty)
    virtual bool ReadOccupant(cIGZIStream&, std::uint32_t iid, cISC4Occupant** out) { (void)iid; *out = nullptr; return false; }
};

struct PowerPlantEntry { cISC4Occupant* occupant = nullptr; int state = 0; };   // list node: +8 state, +0xc occupant (one reference)

class cSC4PowerSimulator : public cISC4PowerSimulator,
                           public cIGZMessageTarget2,
                           public cIGZSerializable {
public:
    cSC4PowerSimulator(IPowerHost* host, cRZRandom* random);
    virtual ~cSC4PowerSimulator();

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PowerSimulator; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    bool DoMessage(cIGZMessage2* message) override;

    bool Init();                                                     // i386 0x00184be4
    bool Shutdown();                                                 // i386 0x0018493a
    bool IsInitialized() const { return initialised_; }
    // Loads defaults and then the exemplar {0x6534284a, 0xe7e2c2db, 0xc911e35b}; null = defaults (i386 0x00181b3c).
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);

    // ---- coordinates ----
    void PositionToCell(float x, float z, int& cellX, int& cellZ) const;
    void CellToPosition(int cellX, int cellZ, float& x, float& z) const;
    int NeighborhoodRadius() const { return neighborhoodRadius_; }
    void ExpandRectToIncludeNeighborhood(PowerRect& rect) const;
    void GetNeighborhoodOfPoint(int x, int z, PowerRect& out) const;

    // ---- bit maps ----
    void InitMarkAndWireBits();        // allocates the four maps (cells^2 bits each)
    void ShutdownMarkAndWireBits();
    bool IsPowered(int x, int z) const;
    bool IsWired(int x, int z) const;
    bool IsMarked(int x, int z) const;
    bool IsMarked(const PowerRect& cells) const;           // any marked cell of the rectangle
    void SetMarkBit(int x, int z);
    static bool SetsWireBits(cISC4Occupant* occupant);
    void SetWireBits(const PowerRect& rect);
    void ResetWireBits(const PowerRect& rect);
    void UpdateWireBits(const PowerRect& rect, cISC4Occupant* ignore);
    bool CellChanged(unsigned x0, unsigned z0, unsigned x1, unsigned z1);
    std::uint32_t* PoweredBits() { return poweredBits_; }
    std::uint32_t* WireBits() { return wireBits_; }

    // ---- the grid ----
    void DoIdle();                                                   // the five-state pass (i386 0x00182ec8)
    int IdleState() const { return state_; }
    bool Enabled() const { return enabled_; }
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    void UpdatePowerGrid(int x, int z);                              // i386 0x00182616
    void UpdatePoweredLots();                                        // i386 0x00181012
    bool IsNeighborConnectedToCurrentGrid(void* city, bool markVisited, bool powerConnectionCell);   // i386 0x001812e0
    void GetConnectionCell(cISC4NeighborConnection* connection, int& x, int& z) const;                 // i386 0x00180a0c
    void DoOccupantInserted(cISC4Occupant* occupant);                // i386 0x00183b7a
    void DoOccupantRemoved(cISC4Occupant* occupant);                 // i386 0x00185bba
    void AgePowerPlant(cISC4Occupant* plant);                        // i386 0x001821de
    bool RefreshRequested() const { return refresh_; }
    void RequestRefresh() { refresh_ = true; }

    // ---- plants ----
    std::list<PowerPlantEntry>& Plants() { return plants_; }
    std::uint32_t GetCurrentPowerProduced(cISC4Occupant* plant);
    float GetCurrentPlantConditionPercent(cISC4Occupant* plant);
    float GetCurrentPlantEfficiencyPercent(cISC4Occupant* plant);
    bool GetPowerPlantFundingPercentage(cISC4Occupant* plant, SC4Percentage& out);
    bool GetMonthlyPowerGenerationCost(cISC4Occupant* plant, std::int64_t& cost);
    // Best (least funded, tracked) and most worn plant: plant with the lowest funding below 2.0 and the plant
    // with the highest wear ratio; optional outputs. wearPercent receives wear * 100 of the worn plant.
    void FindPowerPlants(cISC4Occupant** leastFunded, SC4Percentage* funding, cISC4Occupant** mostWorn, int* wearPercent);
    void MarkPowerPlantsAsVisited(std::uint32_t used, std::uint32_t capacity);   // i386 0x00180be2
    bool GetOccupantPowerProduced(cISC4Occupant* plant, std::uint32_t& produced, std::uint32_t& currentProduced);
    static bool GetOccupantMaxPowerProduced(cISC4Occupant* plant, std::uint32_t& out);
    void GetPowerPlants(std::list<cISC4Occupant*>& out) const;

    // ---- poles and funding ----
    std::vector<cISC4Occupant*>& Poles() { return poles_; }
    void ApplyPoleFunding(const SC4Percentage& funding);             // i386 0x00184118
    std::uint32_t GetPowerPoleCount() const { return std::uint32_t(poles_.size()); }

    // ---- monthly statistics ----
    void DoMonthlySimulation();                                      // i386 0x001842b0
    static constexpr int kMonthlySlots = 12, kPowerTypes = 10;
    std::array<std::array<int, kPowerTypes>, kMonthlySlots>& MonthlyPower() { return monthlyPower_; }   // +0x90
    std::array<int, kPowerTypes>& AnnualPower() { return annualPower_; }                                // +0x270
    int CurrentMonth() const { return month_; }
    int GetAnnualConsumption() const;
    int GetAnnualConsumptionByPowerType(int type) const { return annualPower_[std::size_t(type)]; }

    // the pass counters (i386 +0x3c..+0x4c) and their last finished values (+0x50..+0x60)
    struct PassCounters { std::int32_t produced = 0, consumed = 0, imported = 0, exported = 0, demand = 0; };
    PassCounters pass, last;
    float shortage = 0.0f;                  // +0x64
    std::int32_t consumers = 0;             // +0x68
    std::int32_t poweredConsumers = 0;      // +0x6c
    std::int32_t unpoweredConsumers = 0;    // +0x70
    std::int32_t totalConsumption = 0;      // +0x74
    std::int64_t poleUpkeep = 0;            // +0x78
    std::int32_t plantCount = 0;            // +0x2e0
    std::array<std::int32_t, kPowerTypes> plantCountByType{};   // +0x2e4
    std::int32_t overworkedPlants = 0, agedPlants = 0, distressedPlants = 0;   // +0x30c / +0x310 / +0x314

    // cISC4PowerSimulator statistics getters
    std::uint32_t GetMonthlyProduction() const { return std::uint32_t(last.produced); }
    std::uint32_t GetMonthlyConsumption() const { return std::uint32_t(last.consumed); }
    std::uint32_t GetMonthlyImport() const { return std::uint32_t(last.imported); }
    std::uint32_t GetMonthlyExport() const { return std::uint32_t(last.exported); }
    std::uint32_t GetMonthlyDemand() const { return std::uint32_t(last.demand); }
    std::uint32_t GetUnpoweredConsumerCount() const { return std::uint32_t(unpoweredConsumers); }
    std::uint32_t GetPoweredConsumerCount() const { return std::uint32_t(poweredConsumers); }
    std::uint32_t GetPowerPlantCount() const { return std::uint32_t(plantCount); }
    std::uint32_t GetPowerPlantCount(std::uint32_t type) const { return type < kPowerTypes ? std::uint32_t(plantCountByType[type]) : 0; }
    std::uint32_t GetOverworkedPowerPlantCount() const { return std::uint32_t(overworkedPlants); }
    std::uint32_t GetAgedPowerPlantCount() const { return std::uint32_t(agedPlants); }
    std::uint32_t GetDistressedPowerPlantCount() const { return std::uint32_t(distressedPlants); }

    // ---- tunables (ReadSimulatorTunableValues) ----
    float unitsPerPower = 1.0f;             // +0x2b0 (the ordinance factor, refreshed monthly)
    float value2b8 = 1.1f;                  // +0x2b8 (0xe911e2fc / 100)
    float conditionSafe = 0.6f;             // +0x358 (0x29121e8b / 100)
    float value35c = 1.2f;                  // +0x35c (0xe911e2d1 / 100)
    float strikeEfficiency = 0.5f;          // +0x360 (0x09122027 / 100)
    std::int32_t value364 = 10;             // +0x364 (0x2912202e)
    float value368 = 0.9f;                  // +0x368 (0x49122036 / 100)
    cSC4ResponseCurve fundingEfficiencyCurve;   // +0x36c (funding % -> efficiency %, 0x6911e2df)
    cSC4ResponseCurve curve37c;                 // +0x37c (0x0911e2e6)
    cSC4ResponseCurve wearByFundingCurve;       // +0x38c (department funding -> wear rate, 0x0911e2e8)
    cSC4ResponseCurve wearByUsageCurve;         // +0x39c (use of capacity -> wear rate, 0x0911e2e9)
    cSC4ResponseCurve poleFundingCurve;         // +0x3ac (0xaa713782)
    static constexpr bool IsOnStrike() { return false; }
    static constexpr float GetChanceOfStrike() { return 0.0f; }

private:
    void GrowPendingRect(const PowerRect& rect);   // the pending wire rectangle (+0x336 / +0x338)
    int Size() const { return host_->CityCellsX(); }

    IPowerHost* host_;
    cRZRandom* random_;
    std::uint32_t refCount_ = 0;
    bool initialised_ = false;                 // +0x0c / +0x0d
    bool enabled_ = false;                     // +0x14
    int neighborhoodRadius_ = 2;               // +0x2b4
    std::uint32_t* markBits_ = nullptr;        // +0x2c8
    std::uint32_t* wireBits_ = nullptr;        // +0x2cc
    std::uint32_t* workBits_ = nullptr;        // +0x2d4
    std::uint32_t* poweredBits_ = nullptr;     // +0x2d8
    std::uint32_t bitWords_ = 0;               // +0x2d0
    std::vector<std::vector<std::int16_t>> consumptionMap_;   // +0x84 [x][z]
    std::uint8_t flag2dc_ = 0;                 // +0x2dc
    bool gridChanged_ = false;                 // +0x2dd
    bool refresh_ = false;                     // +0x334
    bool agePlants_ = false;                   // +0x335
    bool pending_ = false;                     // +0x336
    PowerRect pendingRect_{};                  // +0x338
    int state_ = 4;                            // +0x348
    std::list<PowerPlantEntry> plants_;        // +0x298
    std::vector<cISC4Occupant*> poles_;        // +0x2a0 (one reference each)
    int month_ = 0;                            // +0x38
    std::array<std::array<int, kPowerTypes>, kMonthlySlots> monthlyPower_{};   // +0x90
    std::array<int, kPowerTypes> annualPower_{};                               // +0x270
    float poleFunding_ = 0.0f;                 // +0x2c4 (ApplyPoleFunding's accumulator)
    std::uint32_t enabledPoles_ = 0;           // +0x2c0
    std::int32_t value330_ = 0;                // +0x330 (stream only)
    std::vector<std::uint32_t> loadedPowered_; // the powered map the city file brought (SC4_POWERDEBUG)
};

}  // namespace sc4
