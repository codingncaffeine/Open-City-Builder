// cSC4Lot — one zoned lot of the city (108 functions, 0x10017ed76..0x100183c30). A lot owns the footprint
// rectangle, altitude/slope, state machine (HabitationState), per-developer-type populations, travel
// populations and jobs, road-access caches and the trip list used by the traffic simulator.
// The simulators it talks to (zone manager, building development, traffic, land value, message server)
// are reached through ILotHost, replacing the SL::sp* globals of the original.
//
// Original layout (0xf8 bytes): +0x40 rect bytes (x0,z0,x1,z1), +0x44 altitude, +0x48 slope, +0x4c slope at
// construction, +0x50 saved lot-configuration id, +0x58 configuration, +0x60 last state change day,
// +0x64 building type, +0x68 road-access map, +0x80 business unit, +0x88 travel designate, +0x90 populations,
// +0xa8 travel populations, +0xc0 jobs[3], +0xcc initialised, +0xcd flags, +0xce/+0xcf cell, +0xd0/+0xd1 size,
// +0xd2 facing, +0xd3 state, +0xd4 condition, +0xd5 zone type, +0xd6 wealth, +0xd7 vacate reason,
// +0xd8 trips, +0xf0 random percent.
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include "gz/cIGZSerializable.h"
#include "sc4/BuildingOccupant.h"
#include "sc4/Exemplar.h"
#include "sc4/SCProperty.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4Lot = 0xc9bd5d4a;
constexpr std::uint32_t kGZIID_cISC4Lot = 0xe818fced;
constexpr std::uint32_t kGZIID_cISC4TrafficSource = 0xe4fda3d4;
constexpr std::uint32_t kMessageLotStateChanged = 0xe8bda2e7;
constexpr std::uint32_t kMessageLotConditionChanged = 0xe99b3d4d;

// cISC4Lot::HabitationState
enum LotState : std::uint8_t {
    kLotStateUnbuilt = 0, kLotStateUnderConstruction = 1, kLotStateOccupied = 2, kLotStateLivable = 3,
    kLotStateAbandoned = 4, kLotStateReserved = 5, kLotStateDemolishable = 6,
};
// cISC4Lot flag bits (+0xcd)
enum LotFlag : std::uint8_t {
    kLotFlagLocked = 1, kLotFlagNeedsPower = 8, kLotFlagHasPower = 0x10, kLotFlagHistorical = 0x20,
    kLotFlagVerticesLocked = 0x40,
};

// One trip descriptor of the lot's traffic list (cSC4TripData, 0x44 bytes in the original).
struct cSC4TripData {
    std::vector<std::uint8_t> route;          // +0x00
    std::vector<std::uint8_t> routeReversed;  // +0x18
    std::uint32_t count = 0;                  // +0x30
    std::uint8_t type = 0;                    // +0x34
    std::uint8_t index = 0;                   // +0x35
    std::uint8_t flags = 0;                   // +0x36 bit 1 = placeholder
    std::uint8_t kind = 0;                    // +0x37 4 = overflow, 5 = overflow of another kind
    std::int16_t sourceX = 0, sourceZ = 0;    // +0x38, +0x3a
    float distance = 0;                       // +0x3c
    std::uint32_t extra = 0;                  // +0x40 (version 2)

    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);
};

class cISC4LotConfigurationRef : public cIGZUnknown {
public:
    virtual std::uint32_t GetID() const = 0;                                 // vslot 0x18
};

using LotBuildingProfile = BuildingProfile;      // the simulator's per building-type record

class cSC4Lot;
class cISC4BusinessUnit;

class ILotHost {
public:
    virtual ~ILotHost() = default;
    virtual std::uint8_t ZoneTypeAt(int x, int z) = 0;                       // zone manager vslot 0x60
    virtual std::int32_t SimulatorDay() = 0;                                 // simulator clock vslot 0xd0
    virtual float TerrainAltitude(float x, float z) = 0;                     // terrain vslot 0x60
    virtual const LotBuildingProfile* BuildingProfile(std::uint32_t buildingType) = 0;  // sim vslot 0x138
    virtual float SlopeOf(const cSC4Lot& lot) = 0;                           // sim vslot 0x120 -> 0xf0
    virtual std::uint32_t DeveloperType(std::uint32_t purpose, std::int32_t wealth) = 0;  // sim vslot 0xf0
    virtual void OnDeveloperLotAbandoned(std::uint16_t wealthIndex) = 0;     // sim vslot 0x198
    virtual std::uint8_t LandValueAt(int x, int z) = 0;                      // land value sim vslot 0x30
    virtual std::uint32_t RandomPercent() = 0;                               // sRandom uniform(100)
    virtual cISC4LotConfigurationRef* LotConfigurationById(std::uint32_t id) = 0;   // lot cfg manager 0x40
    virtual void PostLotMessage(std::uint32_t message, const cSC4Lot& snapshot, std::uint32_t oldValue,
                                std::uint32_t newValue, std::uint32_t buildingType, std::int32_t wealth) = 0;
    // Cells the lot reacts on (terrain view colouring when the lot gains/loses the "locked" flag).
    virtual void SetLotHighlight(int x, int z, bool on, float altitude) = 0;
    // Traffic simulator job counts per kind 0..2 (vslot 0x98 set / 0xa0 get).
    virtual std::int32_t TrafficJobs(const cSC4Lot& lot, int kind) = 0;
    virtual void SetTrafficJobs(const cSC4Lot& lot, int kind, std::int32_t jobs) = 0;
    virtual float ResidentialTravelFactor(int x, int z) = 0;                 // residential sim vslot 0xf0
    virtual float TravelVolumeFactor() = 0;                                  // traffic sim vslot 0xd8
    virtual void TravelDistribution(int kind, float out[3]) = 0;             // traffic sim vslot 0xd0
    // Per-occupant jobs of a livable commercial/industrial lot for one developer type (demand sim 0x40).
    virtual void JobsPerOccupant(float out[4], int developerType) = 0;
    // Jobs listed in the building exemplar when the building is not a developer type (0x27812841 table).
    virtual void ExemplarJobs(const cSC4Lot& lot, float out[4]) = 0;
    // Building exemplar property (UpdateNeedsPower reads 0x27812852 and 0x27812854); false if absent.
    virtual bool ExemplarProperty(const cSC4Lot& lot, std::uint32_t id, std::int32_t& value) = 0;
    // sim record for (purpose, wealthLevels) vs lot configuration; available=false when either is missing.
    virtual bool PowerRequirementExceeded(const cSC4Lot& lot, const LotBuildingProfile& profile,
                                          bool& available) = 0;
    virtual bool TrafficHasJobAccess(const cSC4Lot& lot, int kind) = 0;     // traffic sim vslot 0x130
    virtual void NeighborLots(const cSC4Lot& lot, int direction, std::vector<cSC4Lot*>& out) = 0;
    virtual void GetLotOccupants(const cSC4Lot& lot, std::vector<cIGZUnknown*>& out) = 0;
    // Road access scans of the lot manager / occupant manager.
    virtual bool RoadAccessCells(const cSC4Lot& lot, int rotation) = 0;      // network occupant on an edge cell
    virtual bool IsFerryTerminal(const cSC4Lot& lot) = 0;                    // building is in group 0x1304
    virtual bool FerryTerminalRoadAccess(const cSC4Lot& lot) = 0;
    virtual cIGZUnknown* FindBuildingOccupant(const cSC4Lot& lot) = 0;      // occupant manager, type 0x278128a0
    virtual void WriteSerializable(cIGZOStream& stream, cIGZUnknown* object) = 0;
    virtual cIGZUnknown* ReadSerializable(cIGZIStream& stream, std::uint32_t clsid) = 0;
    // The serializable a city file knows the lot by (borrowed); null when the host keeps none.
    virtual cIGZUnknown* LotSerializable(cSC4Lot* lot) { (void)lot; return nullptr; }
};
ILotHost* LotHost();
void SetLotHost(ILotHost* host);

// A lot reference in a city file, through the lot host's serializable for the lot. The read lot is borrowed (the city's lot
// table keeps it); null when the reference is empty or does not name a lot.
bool WriteLotReference(cIGZOStream& stream, const cSC4Lot* lot);
cSC4Lot* ReadLotReference(cIGZIStream& stream);

class cSC4Lot {
public:
    cSC4Lot() = default;
    // @ 0x100183988: the original returns its cISC4TrafficSource view (this+0x38); the port has no separate view object,
    // so the lot itself stands for it (the traffic simulator's host takes it as an opaque source).
    void* AsTrafficSource() { return this; }
    cSC4Lot(int x0, int z0, int sizeX, int sizeZ, int facing);
    cSC4Lot(const cSC4Lot& other);
    cSC4Lot& operator=(const cSC4Lot&) = delete;
    ~cSC4Lot();

    std::uint32_t AddRef() { return ++refCount_; }
    std::uint32_t Release();
    bool QueryInterface(std::uint32_t iid, void** out);

    bool Init();
    bool Shutdown();
    bool IsInitialized() const { return initialized_; }
    void PostCityInit();

    // geometry
    void GetLocation(int& x, int& z) const { x = cellX_; z = cellZ_; }
    void GetSize(std::uint8_t& sx, std::uint8_t& sz) const { sx = sizeX_; sz = sizeZ_; }
    void GetBoundingRect(std::int32_t rect[4]) const;
    void GetCenter(float* xyz) const;
    float GetAltitude() const { return altitude_; }
    void SetAltitude(float altitude);
    float GetSlope() const { return slope_; }
    float GetSlopeAtTimeOfConstruction() const { return slopeAtConstruction_; }
    void UpdateSlope();
    std::uint8_t GetFacing() const { return facing_; }
    void SetFacing(int facing) { facing_ = static_cast<std::uint8_t>(facing); }
    std::uint8_t RectByte(int i) const { return rect_[i]; }
    void GetSourceCells(std::int32_t out[4]) const;

    // flags / state
    std::uint8_t GetFlags() const { return flags_; }
    void SetFlags(int flags);
    bool IsHistorical() const { return (flags_ & kLotFlagHistorical) != 0; }
    void SetHistorical(bool on) { SetFlagBit(kLotFlagHistorical, on); }
    bool GetNeedsPower() const { return (flags_ & kLotFlagNeedsPower) != 0; }
    bool GetHasPower() const { return (flags_ & kLotFlagHasPower) != 0; }
    void SetHasPower(bool on) { SetFlagBit(kLotFlagHasPower, on); }
    bool GetVerticesAreLocked() const { return (flags_ & kLotFlagVerticesLocked) != 0; }
    void SetVerticesAreLocked(bool on) { SetFlagBit(kLotFlagVerticesLocked, on); }
    std::uint8_t GetState() const { return state_; }
    void SetState(std::uint32_t state, std::uint32_t wealth, std::uint8_t vacateReason);
    std::uint8_t GetCondition() const { return condition_; }
    void SetCondition(std::uint32_t condition);
    bool IsInLivableState() const { return state_ == kLotStateLivable; }
    bool HasCapacity() const { return (state_ & 0xfe) == 2; }
    bool IsInDemolishableState() const { return state_ == kLotStateDemolishable || static_cast<std::uint8_t>(state_ - 2) < 3; }
    std::int32_t GetDaysSinceLastStateChange() const;
    std::uint8_t GetOccupantWealth() const { return wealth_; }
    std::uint8_t GetVacateReason() const { return vacateReason_; }
    std::uint8_t GetZoneType() const { return zoneType_; }
    void UpdateZoneType();
    std::int64_t GetLandValue() const;

    // building / configuration
    std::uint32_t GetBuildingType(bool fromConfiguration) const;
    void SetBuildingType(std::uint32_t type) { buildingType_ = type; wealth_ = 0; }
    cIGZUnknown* GetBuilding() const;
    void GetLotOccupants(std::vector<cIGZUnknown*>& out) const;
    cISC4LotConfigurationRef* GetLotConfiguration() const { return configuration_; }
    void SetLotConfiguration(cISC4LotConfigurationRef* configuration);
    cISC4BusinessUnit* GetUnit() const { return unit_; }
    void SetUnit(cISC4BusinessUnit* unit);
    cSC4Lot* GetTravelDesignate() const { return travelDesignate_; }
    void SetTravelDesignate(cSC4Lot* designate);
    cSCPropertyHolder& Properties() { return properties_; }

    // populations
    std::uint16_t GetPopulation(std::uint32_t developerType) const;
    const std::vector<std::map<std::uint32_t, std::uint16_t>>& GetPopulations() const { return populations_; }
    bool SetPopulation(std::uint32_t developerType, std::uint16_t population);
    std::uint16_t GetCapacity(std::uint32_t developerType, bool ignoreState) const;
    std::uint16_t GetCapacityForWealth(std::uint32_t wealth) const;
    std::uint16_t GetAvailableCapacity(std::uint32_t developerType, bool includeAbandoned) const;
    float GetOccupancy() const;
    void GetOccupancyByType(std::map<std::uint32_t, float>& out) const;
    void UpdatePrimaryOccupantWealth();
    void UpdateNeedsPower();

    // travel
    std::uint16_t GetTravelPopulation(std::uint32_t developerType) const;
    void ResetTravelPopulation();
    void AddTravelPopulation(const cSC4Lot& other);
    std::int32_t GetTravelJobs(int wealth) const;
    void GetJobs(float* jobs) const;      // jobs[1..3] by wealth (jobs[0] unused)
    void UpdateJobs();
    bool HasJobAccess() const;
    void UpdateTrafficVolume();
    std::list<cSC4TripData>& GetTripDataList() { return trips_; }

    // road access
    bool HasRoadAccess() const { return HasOrganicRoadAccess(0); }
    bool HasOrganicRoadAccess(std::uint32_t purpose) const;
    void UpdateRoadAccess();

    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);
    std::uint32_t GetGZCLSID() const { return kGZCLSID_cSC4Lot; }

private:
    void SetFlagBit(std::uint8_t bit, bool on) { flags_ = on ? (flags_ | bit) : (flags_ & ~bit); }
    bool CalculateRoadAccess() const;
    bool CalculateOrganicRoadAccess(std::uint32_t purpose) const;
    bool CalculateFerryTerminalRoadAccess() const;
    std::uint32_t DeveloperTypeFor(std::uint32_t purpose, std::int32_t wealth) const;

    std::uint32_t refCount_ = 0;
    std::uint8_t rect_[4] = {};                 // +0x40 x0, z0, x1, z1
    float altitude_ = 0;                        // +0x44
    float slope_ = 0;                           // +0x48
    float slopeAtConstruction_ = 0;             // +0x4c
    std::uint32_t savedConfigurationId_ = 0;    // +0x50
    cISC4LotConfigurationRef* configuration_ = nullptr;   // +0x58
    std::uint32_t lastStateChange_ = 0;         // +0x60
    std::uint32_t buildingType_ = 0;            // +0x64
    mutable std::map<std::int32_t, bool> roadAccess_;      // +0x68
    cISC4BusinessUnit* unit_ = nullptr;         // +0x80
    cSC4Lot* travelDesignate_ = nullptr;        // +0x88
    std::vector<std::map<std::uint32_t, std::uint16_t>> populations_;   // +0x90
    std::map<std::uint32_t, std::uint16_t> travelPopulations_;          // +0xa8
    float jobs_[3] = {};                        // +0xc0
    bool initialized_ = false;                  // +0xcc
    std::uint8_t flags_ = 0;                    // +0xcd
    std::uint8_t cellX_ = 0, cellZ_ = 0;        // +0xce, +0xcf
    std::uint8_t sizeX_ = 0, sizeZ_ = 0;       // +0xd0, +0xd1
    std::uint8_t facing_ = 0;                   // +0xd2
    std::uint8_t state_ = 0;                    // +0xd3
    std::uint8_t condition_ = 0;                // +0xd4
    std::uint8_t zoneType_ = 0;                 // +0xd5
    std::uint8_t wealth_ = 0;                   // +0xd6
    std::uint8_t vacateReason_ = 0;             // +0xd7
    std::list<cSC4TripData> trips_;             // +0xd8
    std::uint8_t randomPercent_ = 0xff;         // +0xf0
    cSCPropertyHolder properties_;              // +0x10
};

}  // namespace sc4
