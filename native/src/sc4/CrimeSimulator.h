// cSC4CrimeSimulator — where crimes happen. Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (constructor
// 0x1001fcae8, class id 0x8990c09a, simulator type 0x27567222, object size 0x250; code 0x1001fc910..0x1001ffffa).
// The simulator keeps two byte grids (tract size from the tunables, default 2):
//   * the crime grid (+0x90, instance 0x89d5b895): every crime adds `severity` to the tracts under the lot, decayed
//     each month by +0x248; the hottest tract is tracked (+0xcc/+0xd0/+0xd4) for the hotspot query;
//   * the effect grid (+0x98, instance 0x2a5ba1b3): buildings with property 0xca5b9306 {strength, radius} raise or
//     lower the chance of crime around them (radius effects added/removed as occupants come and go).
// A monthly cycle (state byte +0xdc, one step per idle message): 0 wait, 1 census + economic factors, 2 criminal
// base, 3 crimes to commit, 4 decay + add crimes, 5 post the "crimes updated" message, 6 idle until the next month.
//   census (+0xe8..): residents per wealth (3) bucketed by 50-point value bands (4);
//   factors (+0x118): per wealth, 2 * (demandA - demandC) / (demandB - demandC) - 1 summed over a demand-id list;
//   criminal base = sum over wealth of (residents*curve148[wealth](band*25) + n * curve1a8[wealth](factor)).
// Crime types come from exemplars of type 0x6534284a, group 0xea12f32c (static list shared by the class); an event
// picks a type whose four filter masks (zone, building class, building subclass, lot wealth) accept the lot, weighted by
// property 0xaa12fcb8 (default 10).
//
// Quirks kept: ParseFilterData stores 0xffffffff for an empty list and then overwrites it with 0 (so an empty filter
// accepts nothing; a zero entry means "any"); census bands above 3 spill into the next row's counters (here confined to
// the 12 counters); an average over zero zoned tracts is 0/0 = NaN, and GetAverageCrime truncates it like cvttss2si.
// Services (city, lot/zone managers, police simulator, demand, ordinances, resources, message server) are injected
// through ICrimeHost; the original reaches them through the SL::sp* globals.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZRandom.h"
#include "sc4/RandomIterator.h"
#include "sc4/SCPropertyHelp.h"
#include "sc4/cSC4ResponseCurve.h"
#include "sc4/cSC4SimGrid.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4CrimeSimulator = 0x8990c09a;
constexpr std::uint32_t kCrimeSimulatorType = 0x27567222;
constexpr std::uint32_t kCrimeRequesterSatisfactionId = 0x49a1e996;
constexpr std::uint32_t kCrimeGridInstance = 0x89d5b895, kCrimeEffectGridInstance = 0x2a5ba1b3;
constexpr std::uint32_t kCrimeTypeExemplarType = 0x6534284a, kCrimeTypeExemplarGroup = 0xea12f32c;
constexpr std::uint32_t kCrimeTunableGroup = 0xe7e2c2db, kCrimeTunableInstance = 0xc8e8f6c5;
// messages routed by DoMessage
constexpr std::uint32_t kCrimeMsgIdle = 0x66956814, kCrimeMsgIdleAlt = 0x6695681a, kCrimeMsgMonthDone = 0x66956816;
constexpr std::uint32_t kCrimeMsgOccupantAdded = 0x99ef1142, kCrimeMsgOccupantRemoved = 0x99ef1143;
constexpr std::uint32_t kCrimeMsgOrdinanceChanged = 0xc2c09554, kCrimeMsgExemplarReloaded = 0xc53d10aa;
constexpr std::uint32_t kCrimeMsgCrimesUpdated = 0x88ea4dff, kCrimeMsgCrimeEvent = 0x0a159d95;
constexpr std::uint32_t kCrimeOrdinanceId = 0x28ed0380;
// exemplar properties
constexpr std::uint32_t kCrimeTypeName = 0xaa12fcb0, kCrimeTypeVisual = 0xaa12fcb1, kCrimeTypeZoneFilter = 0xaa12fcb2,
                        kCrimeTypeWealthFilter = 0xaa12fcb3, kCrimeTypePurposeFilter = 0xaa12fcb4,
                        kCrimeTypeSeverity = 0xaa12fcb5, kCrimeTypeArson = 0xaa12fcb6,
                        kCrimeTypeBuildingWealthFilter = 0xaa12fcb7, kCrimeTypeWeight = 0xaa12fcb8;
constexpr std::uint32_t kCrimeTypeNameStringGroup = 0x8a416b2e;
constexpr std::uint32_t kCrimeBuildingEffectProperty = 0xca5b9306;   // uint8 {strength, radius}
constexpr std::uint32_t kCrimeNonCrimeOccupantProperty = 0x029244db; // occupants with it are ignored by DoMessage
constexpr std::uint32_t kCrimePropFilterProperty = 0x8aa864d5;
constexpr std::uint32_t kCrimePropOccupantType = 0xa823821e, kCrimeSimOccupantType = 0x496e5783;

// A lot as the crime simulator sees it (cISC4Lot vslots 0x70, 0xa8, 0x118, 0x180). Events hold a reference.
class ICrimeBuilding {
public:
    virtual ~ICrimeBuilding() = default;
    virtual const std::int32_t* ClassAndSubclass() = 0;   // vslot 0x58: [0] subclass (0 = none), [1] class (both 1-based)
    virtual std::uint32_t OccupantId() = 0;               // vslot 0x18
};
class ICrimeLot {
public:
    virtual ~ICrimeLot() = default;
    virtual void AddRef() = 0;
    virtual void Release() = 0;
    virtual void GetBounds(int& x0, int& z0, int& x1, int& z1) = 0;   // vslot 0x70, inclusive cell rectangle
    virtual void GetCell(int& x, int& z) = 0;                          // vslot 0xa8, the lot's reference cell
    virtual std::uint32_t Wealth() = 0;                                // vslot 0x118 (valid 0..6)
    virtual ICrimeBuilding* Building() = 0;                            // vslot 0x180
    virtual void GetPosition(float& x, float& z) = 0;                  // vslot 0x78 -> city cell to world
};

class ICrimeOccupant {
public:
    virtual ~ICrimeOccupant() = default;
    virtual std::uint32_t OccupantType() = 0;                 // vslot 0x38
    virtual cISCPropertyHolder* PropertyHolder() = 0;         // vslot 0x30
    virtual void Position(float xyz[3]) = 0;                  // vslot 0x40
    virtual bool IsIdlePropOccupant() = 0;         // QueryInterface(0xe9793a65) succeeds and its vslot 0x60 == 0
    virtual bool IsCrimeResponder() = 0;                      // QueryInterface(0xa9b40f05) succeeds
    virtual bool HasFlags(std::uint32_t flags) = 0;           // vslot 0xb0
};

// One of the residential grids used by the census (cISC4SimGrid vslots 0x60/0x68/0xd8).
class ICrimeCensusGrid {
public:
    virtual ~ICrimeCensusGrid() = default;
    virtual int CountX() = 0;
    virtual int CountZ() = 0;
    virtual std::uint16_t Population(int x, int z) = 0;       // ushort grid
    virtual float Value(int x, int z) = 0;                    // float grid (value per resident band)
};

struct CrimeIncident {   // cISC4CrimeSimulator::tCrimeIncident, 0x1c bytes
    float x = 0, z = 0;
    std::uint32_t typeId = 0;
    std::uint8_t visualsCreated = 0;
    cGZPersistResourceKey visual;
};

class ICrimeHost {
public:
    virtual ~ICrimeHost() = default;
    virtual ICrimeLot* RandomLot() = 0;                                     // lot manager vslot 0x68
    virtual ICrimeLot* LotAt(int x, int z) = 0;                              // lot manager vslot 0x28 (x, z, 0)
    virtual int ZoneTypeAt(int x, int z) = 0;                                // zone manager vslot 0x60 (0 = unzoned)
    virtual int PropCrimeTypeId(std::uint32_t propId) = 0;                   // prop manager vslot 0x68 -> record +0x18 (0 = any crime)
    virtual int PoliceJailPopulation() = 0;                                  // police simulator vslot 0xe8
    virtual int PoliceResetUnjailedCriminals() = 0;                          // police simulator vslot 0xd8
    virtual void PoliceProcessMonthlyCrimeStats() = 0;                       // police simulator vslot 0x130
    virtual const cSC4SimGrid<std::uint8_t>* PoliceCoverageGrid() = 0;       // police simulator vslot 0x28
    // demand source `demandId`: the three float getters at vslots 0x60 / 0x70 / 0x78
    virtual void DemandValues(int demandId, float& a, float& b, float& c) = 0;
    virtual float OrdinanceFactor(std::uint32_t ordinanceId) = 0;            // ordinance simulator vslot 0xc8
    virtual void ResidentialGrids(int wealth, ICrimeCensusGrid*& population, ICrimeCensusGrid*& value) = 0;
    // resources
    virtual std::vector<cGZPersistResourceKey> ListCrimeTypeExemplars() = 0;
    virtual cISCPropertyHolder* LoadExemplar(const cGZPersistResourceKey& key) = 0;   // null when missing; caller Release()s
    virtual void ReleaseExemplar(cISCPropertyHolder* exemplar) = 0;
    virtual bool LoadExemplarString(std::uint32_t group, std::uint32_t instance, std::uint32_t stringGroup, std::string& out) = 0;
    // events
    virtual void PostCrimeEvent(std::uint32_t typeId, ICrimeLot* lot, const std::string& description) = 0;
    virtual void PostCrimesUpdated() = 0;                                    // message 0x88ea4dff
    virtual void StartArson(ICrimeLot* lot, ICrimeBuilding* building) = 0;   // disaster manager (fire) at the building
    // lot streaming (SC4Write/ReadGZSerializable with iid 0xe818fced); ReadLot may return null
    virtual bool WriteLot(cIGZOStream& stream, ICrimeLot* lot) = 0;
    virtual ICrimeLot* ReadLot(cIGZIStream& stream) = 0;
    virtual void LotPositionToIncidentXZ(float x, float z, float& outX, float& outZ) = 0;   // city vslot 0x2f8
    virtual std::vector<ICrimeOccupant*> OccupantsInBox(const float min[3], const float max[3]) = 0;   // city occupant manager 0x128
    virtual void ReleaseOccupants(std::vector<ICrimeOccupant*>& list) = 0;
    virtual double RandomDouble() = 0;                                       // sRandom.RandomDoubleUniform
    virtual double RandomDoubleRange(double lo, double hi) = 0;              // sRandom.RandomDoubleRangeUniform
    virtual std::uint32_t RandomUint32(std::uint32_t bound) = 0;             // sRandom.RandomUint32Uniform
    // A new city: the simulator's requester view (0x89c498be, +8) joins the prop developer (spPropDeveloper vslot 0x14).
    // The view lives on the simulator's COM adapter, so the host finds it. A loaded city gets it back from the save.
    virtual void RegisterPropRequester() {}
};

// One crime type (tCrimeType, 0x58 bytes). Filter masks accept a lot when (mask & lotBits) != 0 for all four.
struct CrimeType {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t filter[4] = {0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};   // zone, building class, subclass, lot wealth
    cGZPersistResourceKey visual;
    std::uint8_t severity = 0x40;
    bool arson = false;
    float weight = 10.0f;
};

struct CrimeEvent {   // tCrimeEvent, 0x48 bytes
    ICrimeLot* lot = nullptr;
    std::uint32_t typeId = 0;
    std::string description;
    cGZPersistResourceKey visual;
    bool visualsCreated = false;
};

struct CrimeTunables {
    int tractSize = 2;           // +0x124 (property 0x47e2c200)
    float decay = 0.5f;          // +0x248 (0x8a0cbaac): crime grid cells are multiplied by this each month
    float crimeFactor = 1.0f;    // +0x24c (0x8a0cbaad)
};

// Shared crime-type list (the original keeps it in the class statics mCrimeTypes / mbStaticInitialized).
class CrimeTypeRegistry {
public:
    void Clear() { types_.clear(); }
    void LoadAll(ICrimeHost& host);
    void LoadFromExemplar(ICrimeHost& host, const cGZPersistResourceKey& key);
    const std::vector<std::unique_ptr<CrimeType>>& Types() const { return types_; }
    bool initialized = false;
private:
    std::vector<std::unique_ptr<CrimeType>> types_;
};

// ParseFilterData @ 0x1001ff54d: list of 1-based bit numbers -> mask (see quirks above).
std::uint32_t ParseCrimeFilterData(const std::vector<std::uint8_t>& values);
// Truncation as x86 cvttss2si does it (out-of-range and NaN give INT_MIN).
int TruncateFloatX86(float v);
// (int)(x + 12582912.0f) - 0x4b400000: round to nearest even, the compiler's magic-number idiom.
int RoundFloatMagic(float v);
// cSC4SimGrid<unsigned char>::GetAverageValueInCellRect @ 0x1001fc860: the byte grid returns T, not float: a rectangle
// inside one tract gives that tract's value, otherwise (int)(sum / (double)(w*h)) truncated to a byte.
std::uint8_t AverageInCellRectU8(const cSC4SimGrid<std::uint8_t>& grid, int x, int z, int w, int h);
// cSC4MapUtilities::AddRadiusEffect, byte grid overload @ 0x1000d7098 (column-major grid[x][z]).
void AddRadiusEffectU8(std::vector<std::vector<std::uint8_t>>& grid, int width, int height, int cx, int cz, float radius,
                       float peak, float edge);

// tCrimePropOccupantFilter / tCrimeSimOccupantFilter IsOccupantIncluded (0x1001fff04 / 0x1001fff82).
bool IsCrimePropOccupant(ICrimeOccupant* occupant);
bool IsCrimeSimOccupant(ICrimeOccupant* occupant);

class cSC4CrimeSimulator {
public:
    // state machine step values (+0xdc)
    enum Step : std::uint8_t { kWaiting = 0, kCensus = 1, kCriminalBase = 2, kCrimesToCommit = 3, kApplyCrimes = 4, kPost = 5, kIdle = 6 };

    cSC4CrimeSimulator(ICrimeHost* host, CrimeTypeRegistry* registry);
    ~cSC4CrimeSimulator();   // releases the grids and events
    cSC4CrimeSimulator(const cSC4CrimeSimulator&) = delete;
    cSC4CrimeSimulator& operator=(const cSC4CrimeSimulator&) = delete;

    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4CrimeSimulator; }
    static std::uint32_t GetSimulatorType() { return kCrimeSimulatorType; }
    static std::uint32_t GetRequesterSatisfactionId() { return kCrimeRequesterSatisfactionId; }
    static bool UsesCustomPropSelector() { return true; }

    // lifecycle
    bool Init(const cISCPropertyHolder* tunables);   // StaticInit + ReadSimulatorTunableValues + grids; false when already running
    bool Shutdown();
    void ReadSimulatorTunableValues(const cISCPropertyHolder* exemplar);
    void ClearCrimeEvents();

    // messages
    bool DoMessage(std::uint32_t message, std::uint32_t data1 = 0, std::uint32_t data2 = 0, ICrimeOccupant* occupant = nullptr);
    void DoIdle();
    void DoOccupantChanged(ICrimeOccupant* occupant, bool added);

    // monthly steps
    void DoCensus();
    void CalculateEconomicFactors();
    int ComputeCriminalBase() const;
    void DecayCrimeGrid();
    int AddCrimes(int count);
    void ComputeAverageCrime();
    // Picks a crime type for the lot and records the event; returns the type's severity (0 when no event was created).
    std::uint8_t AddCrimeEvent(ICrimeLot* lot);

    // queries
    int GetAverageCrime() const { return TruncateFloatX86(averageCrime_) & 0xff; }
    int GetMonthlyCrimeCount() const { return monthlyCrimes_; }
    int GetRequestedShare(int x0, int z0, int x1, int z1) const;   // rectangle [x0,x1) x [z0,z1)
    std::uint32_t SelectProp(const std::vector<std::uint32_t>& candidates, int x, int z);
    bool GetCrimeHotspot(std::uint8_t* value, int* x, int* z) const;
    bool GetCrimeIncidentArray(std::vector<CrimeIncident>& out) const;
    bool SetVisualsCreated(std::uint32_t index, bool created);
    bool GetVisualsCreated(std::uint32_t index) const;
    ICrimeOccupant* GetNearestCrimeProp(const float position[3], float radius);
    ICrimeOccupant* GetNearestCrimeSim(const float position[3], float radius);
    const cSC4SimGrid<std::uint8_t>* GetCrimeGrid() const { return crimeGrid_; }
    const cSC4SimGrid<std::uint8_t>* GetCrimeEffectGrid() const { return effectGrid_; }
    Step CurrentStep() const { return step_; }
    void ForceStep(Step s) { step_ = s; }
    const std::vector<CrimeEvent*>& Events() const { return events_; }
    const CrimeTunables& Tunables() const { return tunables_; }
    int CriminalBase() const { return criminalBase_; }
    int CrimesToAdd() const { return crimesToAdd_; }
    float CrimeMultiplier() const { return crimeMultiplier_; }
    const std::uint32_t (&Census() const)[3][4] { return census_; }
    const float (&Factors() const)[3] { return factors_; }
    cSC4ResponseCurve& ResidentCurve(int wealth) { return residentCurve_[wealth]; }     // +0x148
    cSC4ResponseCurve& FactorCurve(int wealth) { return factorCurve_[wealth]; }         // +0x1a8
    cSC4ResponseCurve& ShareCurve() { return shareCurve_; }                             // +0x128
    cSC4ResponseCurve& CrimeChanceCurve() { return chanceCurve_; }                      // +0x208
    cSC4ResponseCurve& CriminalCurve() { return criminalCurve_; }                       // +0x228

    // lots
    // The four bit sets the type filters are tested against: zone type, building class ([1]), building subclass ([0], 0x100
    // when none or no building) and lot wealth. A lot without a building leaves the building class at 0x80000000.
    void GetLotBits(ICrimeLot* lot, std::uint32_t& zone, std::uint32_t& buildingClass, std::uint32_t& buildingSubclass,
                    std::uint32_t& lotWealth);

    // streams (version 6)
    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);

private:
    ICrimeHost* host_;
    CrimeTypeRegistry* registry_;
    bool initialized_ = false;      // +0x20
    bool loaded_ = false;           // +0x21: Read ran, keep the iterator
    RandomIterator iterator_;       // +0x28
    cSC4SimGrid<std::uint8_t>* crimeGrid_ = nullptr;    // +0x90, reference counted
    cSC4SimGrid<std::uint8_t>* effectGrid_ = nullptr;   // +0x98
    std::uint32_t unknownA0_ = 0, unknownA4_ = 0;   // +0xa0/+0xa4 streamed, never written by recovered code
    float averageCrime_ = 0;        // +0xa8
    std::vector<CrimeEvent*> events_;   // +0xb0
    int monthlyCrimes_ = 0;         // +0xc8
    int hotspotX_ = 0, hotspotZ_ = 0, hotspotValue_ = 0;   // +0xcc, +0xd0, +0xd4
    float crimeMultiplier_ = 1.0f;  // +0xd8
    Step step_ = kIdle;             // +0xdc
    int criminalBase_ = 0, crimesToAdd_ = 0;   // +0xe0, +0xe4
    std::uint32_t census_[3][4] = {};          // +0xe8
    float factors_[3] = {};                    // +0x118
    CrimeTunables tunables_;
    cSC4ResponseCurve shareCurve_, residentCurve_[3], factorCurve_[3], chanceCurve_, criminalCurve_;
};

}  // namespace sc4
