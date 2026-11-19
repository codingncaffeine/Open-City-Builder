// cSC4FloraSimulator — trees, bushes and rocks: the process-wide catalogue of flora types (cSC4StaticFloraData, see
// FloraOccupant.h) loaded from flora exemplars, their placement rules, and the simulation of the flora occupants of a city:
// aging and rebirth, seeding (around mature plants and "wild" seeds while propagation is on), burnt trees cleared at a
// daily rate, removal under new buildings and networks and on terrain changes, the "Flora" cheat, the PlaceFlora command,
// the city-file record and the flora kept across an obliterated city. Behavior spec: OpenSC4 docs/specs/flora.md; details
// settled against the Mac i386 build (64 functions, 0x1000bd5e6..0x1000c2454 in the 64-bit build).
//
// Identity: one object answers iid 1 / 0x06c66177 (the simulator), 0x452294aa (cIGZMessageTarget2, the original's +4
// view) and 0xe4fda3d4 (cIGZSerializable, its +8 view), with one reference count. The services the original takes from
// the service locator are the IFloraSimulatorHost seam.
#pragma once
#include <list>
#include <cstdint>
#include <map>
#include <unordered_set>
#include <vector>

#include "gz/cGZPersistResourceKey.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/Exemplar.h"
#include "rz/cRZRandom.h"
#include "sc4/RandomIterator.h"
#include "sc4/RecordTape.h"
#include "sc4/FloraOccupant.h"
#include "sc4/OccupantManager.h"
#include "sc4/SCProperty.h"

class cIGZPersistDBSegment;

namespace sc4 {

constexpr std::uint32_t kFloraSimulatorClassId = 0xc990c034;           // GZCOM_cSC4FloraSimulatorCLSID
constexpr std::uint32_t kFloraSimulatorInterfaceId = 0x06c66177;       // cISC4FloraSimulator
constexpr std::uint32_t kFloraDirectoryTypeId = 0x6534284a;            // resource-directory id scanned by StaticInit
constexpr std::uint32_t kFloraExemplarTypeValue = 0xf;                 // exemplar type (property 0x10) of a flora type

// property ids of a flora exemplar
constexpr std::uint32_t kFloraPropTypeId = 0x21;
constexpr std::uint32_t kFloraPropAquatic = 0xea346888;                // presence: elevation measured below the sea level
constexpr std::uint32_t kFloraPropTraits = 0x83dc35b;                  // 14 floats -> +0x10..+0x44
constexpr std::uint32_t kFloraPropFamilyKey = 0x2a0348ba;              // -> +4
constexpr std::uint32_t kFloraPropValue48 = 0x2a0348bb;
constexpr std::uint32_t kFloraPropValue4c = 0x2a0348bc;
constexpr std::uint32_t kFloraPropSuitability = 0x83dc487;             // 16x16 floats (0..1) -> bytes at +0x50
constexpr std::uint32_t kFloraPropValue150 = 0x83de16f;
constexpr std::uint32_t kFloraPropFlagExcluded = 0xe9d117c3;           // bit0 of +0x180
constexpr std::uint32_t kFloraPropFlagBit = 0x6a37ebb6;                // clears bit1 (false) or bit2 (true)
constexpr std::uint32_t kFloraPropFamilies = 0xa8f149c5;
constexpr std::uint32_t kFloraPropCost = 0x49cac341;

// The simulator's messages, command and cheat (flora.md 4.1, 5, 9).
constexpr std::uint32_t kFloraMsgTerrainHeightsChanged = 0x278e6003;
constexpr std::uint32_t kFloraMsgInsertOccupant = 0x99ef1142;
constexpr std::uint32_t kFloraMsgRemoveOccupant = 0x99ef1143;
constexpr std::uint32_t kFloraMsgChangeOccupant = 0x99ef1145;
constexpr std::uint32_t kFloraMsgExemplarChanged = 0xc53d10aa;
constexpr std::uint32_t kFloraMsgCityEstablished = 0x26d31ec4;
constexpr std::uint32_t kFloraMsgCityObliterated = 0x26d31ec5;
constexpr std::uint32_t kFloraMsgSimNewDay = 0x66956814;
constexpr std::uint32_t kFloraMsgSimNewMonth = 0x66956816;
constexpr std::uint32_t kFloraMsgCheatCode = 0x230e27ac;
constexpr std::uint32_t kFloraMsgTypesUpdated = 0x87a6277f;            // posted after every (re)load of the flora types
constexpr std::uint32_t kFloraCommandPlaceFlora = 0xab99a64b;          // a command id, subscribed as a message too
constexpr std::uint32_t kFloraCheatCode = 0x095bdc9c;                  // "Flora"
constexpr std::uint32_t kFloraTuningInstance = 0x1a2fdb6a;             // {0x6534284a, 0x7a4a8458, 0x1a2fdb6a}
constexpr std::uint32_t kBurntFloraType = 0x29cff3cb;
// SaveObliterated / LoadObliterated records (flora.md 10.3): the count at {type, count group, 1}, occupant i at
// {type, occupant group, i}.
constexpr std::uint32_t kObliteratedFloraRecordType = 0xc9bb3fc6;
constexpr std::uint32_t kObliteratedFloraCountGroup = 0xc9bb3fc7;
constexpr std::uint32_t kObliteratedFloraOccupantGroup = 0x4a664f83;

class cISC4FloraTypeFilter {
public:
    virtual ~cISC4FloraTypeFilter() = default;
    virtual bool Accepts(std::uint32_t floraType) = 0;                 // vslot 0x18
};

class IFloraSimulatorHost {
public:
    virtual ~IFloraSimulatorHost() = default;
    // Loads an exemplar by key (returned with a reference), or nullptr.
    virtual cISCResExemplar* LoadExemplar(const cGZPersistResourceKey& key) = 0;
    // Enumerates the resources of the flora resource directory (kFloraDirectoryTypeId).
    virtual void EnumerateFloraResources(void (*callback)(const cGZPersistResourceKey&, void*), void* context) = 0;
    virtual float SeaLevel() = 0;                                      // terrain vslot 0xe8
    virtual float TerrainHeight(float x, float z) = 0;                 // terrain vslot 0x60
    virtual int TerrainClass(float x, float z) = 0;                    // weather GetMoistureType (0 without a weather simulator)
    // AddNewFloraOccupant / LocationIsOKForNewFlora: the simulator's occupant manager (+0x14), its lot manager (+0x24
    // vslot 0x48: the ground height and whether the point is on a lot), the terrain's face normal (+0x20 vslot 0x68),
    // the city's size in metres (SL::spCity vslots 0x164 / 0x168) and the simulator's day number (+0x1c vslot 0x20).
    virtual cISC4OccupantManager* OccupantManager() = 0;
    virtual float GroundHeight(float x, float z, bool& onLot) = 0;
    virtual float TerrainNormalY(float x, float z) = 0;
    virtual void CitySizeMeters(float& x, float& z) = 0;
    virtual std::uint32_t SimDate() = 0;

    // Init / Shutdown / DoMessage (flora.md 4, 5).
    // SL::spMS2: subscriptions, FloraTypesUpdated; null skips the subscriptions and the command, as in the original.
    virtual cIGZMessageServer2* MessageServer() = 0;
    // SL::spCommandServer slot 6 RegisterCommand(id, name, parameter info) / slot 9 UnregisterCommand(id).
    virtual bool RegisterCommand(std::uint32_t id, const char* name, const char* parameterInfo) = 0;
    virtual bool UnregisterCommand(std::uint32_t id) = 0;
    // SL::spSimulator AddAgent(target, level, name, flags) / RemoveAgent(target) (every level).
    virtual bool AddSimulatorAgent(cIGZMessageTarget2* target, int level, const char* name, std::uint32_t flags) = 0;
    virtual void RemoveSimulatorAgent(cIGZMessageTarget2* target) = 0;
    // SL::spCCM (may be null: every call is then skipped): slot 3 RegisterCheatCode / slot 4 UnregisterCheatCode, slot 7
    // AddNotification2 / slot 8 RemoveNotification2.
    virtual bool CheatCodeManagerPresent() = 0;
    virtual void RegisterCheatCode(std::uint32_t id, const char* text) = 0;
    virtual void UnregisterCheatCode(std::uint32_t id) = 0;
    virtual void AddCheatNotification(cIGZMessageTarget2* target, std::uint32_t id) = 0;
    virtual void RemoveCheatNotification(cIGZMessageTarget2* target, std::uint32_t id) = 0;
    // SL::spCity: present, its cell counts (slots 93 / 94) and GetEstablished (slot 22).
    virtual bool CityPresent() = 0;
    virtual void CityCellCounts(std::int32_t& x, std::int32_t& z) = 0;
    virtual bool CityEstablished() = 0;
    // the terrain's "initialized" flag (slot 5); false without a terrain
    virtual bool TerrainInitialized() = 0;
    // cISC4App::GetNewCitySpecification()->obliteratedCityDBSegment (borrowed; null when the city is not a replacement)
    virtual cIGZPersistDBSegment* ObliteratedCitySegment() = 0;
};
IFloraSimulatorHost* FloraSimulatorHost();
void SetFloraSimulatorHost(IFloraSimulatorHost* host);

class cSC4FloraSimulator : public cIGZSerializable, public cIGZMessageTarget2 {
public:
    cSC4FloraSimulator() = default;
    virtual ~cSC4FloraSimulator();

    // cIGZUnknown (one count for the three views)
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;
    // cIGZSerializable (flora.md 10.1)
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kFloraSimulatorClassId; }
    // cIGZMessageTarget2 (flora.md 5): always true
    bool DoMessage(cIGZMessage2* message) override;

    bool Init();                                                        // flora.md 4.1
    bool Shutdown();                                                    // flora.md 4.2
    bool IsInitialized() const { return initialized_; }

    static void MakeDefaultStaticFloraData(cSC4StaticFloraData& data);
    static bool StaticInit(cSC4FloraSimulator* simulator = nullptr);   // the simulator rides on FloraTypesUpdated
    static bool StaticShutdown();
    static void LoadAllStandardFloraTypes(cSC4FloraSimulator* simulator = nullptr);
    static void PostFloraTypesUpdatedMessage(cSC4FloraSimulator* simulator);
    static void AddStaticFloraDataFromResource(const cGZPersistResourceKey& key);   // AddStaticFloraDataStatic
    static bool AddStaticFloraData(cSC4StaticFloraData* data);      // takes a reference; false when the type exists
    static bool AddStaticFloraData(const cGZPersistResourceKey& key, cISCResExemplar* exemplar);

    int GetElevationIndex(float x, float z, bool belowSeaLevel) const;
    float GetPropagationScoreForFlora(std::uint32_t type, float x, float z) const;
    std::uint32_t GetBestFloraTypeForLocation(float x, float z, float& score) const;
    cSC4StaticFloraData* GetStaticFloraData(std::uint32_t type) const;
    std::uint32_t GetAllFloraTypes(std::uint32_t* out, std::uint32_t& count) const;
    void GetAllFloraTypes(std::vector<std::uint32_t>& out) const;
    void GetAllFloraFamilyTypes(std::vector<std::uint32_t>& out) const;
    bool IsFloraTypeValid(std::uint32_t type) const;
    bool IsFloraFamilyTypeValid(std::uint32_t family) const;       // a family key, or (quirk) a flora type
    int GetFloraTypesByFilter(std::unordered_set<std::uint32_t>& out, cISC4FloraTypeFilter* filter) const;
    int GetFloraTypesByFamily(std::unordered_set<std::uint32_t>& out, std::uint32_t family) const;
    void SetElevationScale(float scale) { elevationScale_ = scale; }

    // Planting (the terrain's flora brush, cSTETerrain::BrushFnFlora): a new occupant of `type` (0 = the best type for the
    // spot) at xyz; unless `skipClusters`, the trees around it that cluster with it are merged into the next cluster type
    // first. Returns the occupant (one reference held by the occupant manager) or null.
    cISC4Occupant* AddNewFloraOccupant(std::uint32_t type, float* xyz, bool skipClusters);   // vslot 0x48 (0x1000c1f6c)
    bool AddNewFloraOccupant(cISC4Occupant* occupant);                                         // vslot 0x44
    bool LocationIsOKForNewFlora(std::uint32_t type, float x, float z, cISC4Occupant* ignore) const;   // vslot 0x3c
    // The terrain-changed message 0x278e6003 (data1 = x0 << 16 | x1, data2 = z0 << 16 | z1, cells): DoMessage's decoding,
    // then DoTerrainAltitudeChange (0x8c9e2 in the Mac i386 build) over the flora of the cells.
    void DoTerrainChangedMessage(std::uint32_t data1, std::uint32_t data2);
    void DoTerrainAltitudeChange(const std::int32_t rect[4]);
    void AdjustFloraPositionFromTerrainChange(cISC4Occupant* occupant, cISC4FloraOccupant* flora);
    // Flora burnt by a fire (vslot 0x5c, Mac i386 0x0008ad00): the plant becomes the burnt type 0x29cff3cb in state 3 and joins
    // the burnt list; false when the occupant is not flora in the city.
    bool KillFloraOccupant(cISC4Occupant* occupant);
    bool DemolishFloraOccupant(cISC4Occupant* occupant, int unused);              // vslot 0x4c (Mac i386 0x00089bfc)
    bool DemolishFloraInCellArea(const std::int32_t rect[4], int unused);         // vslot 0x50 (Mac i386 0x0008a9d2)
    bool DemolishFloraInBoundingBox(const float* min, const float* max, int unused);   // vslot 0x54
    std::int64_t GetCostOfOccupantDemolition(cISC4Occupant*, int) const { return 0; }   // vslot 0x58: always 0
    bool SaveObliterated(cIGZPersistDBSegment* segment) const;                   // vslot 0x60 (flora.md 10.3)
    bool LoadObliterated(cIGZPersistDBSegment* segment);                         // Init step 12 (flora.md 10.3)
    static std::uint32_t NextClusterType(std::uint32_t type);
    static std::uint32_t AdvanceClusterType(std::uint32_t a, std::uint32_t b);

    // The simulation (flora.md 6.6 - 6.10), public for the tests.
    void DoDailySimulation();                                           // 6.8
    void RemoveBurntFlora(std::int32_t count);
    void DoMonthlySimulation();                                         // 6.7
    void SimulateExistingFloraOccupant(cISC4FloraOccupant* flora);      // 6.7.2
    bool DropSeed(std::uint32_t type, float x, float z);                // 6.6
    void DoOccupantInserted(cISC4Occupant* occupant, std::uint32_t occupantType);   // 6.10.1
    void DoOccupantRemoved(cISC4Occupant* occupant, std::uint32_t occupantType);
    void EnableFlora(bool enable);                                      // 6.10.4
    void ReadSimulatorTunableValues();                                  // 7

    // State (flora.md 3.1), read by the tests.
    bool Enabled() const { return enabled_; }
    bool Propagation() const { return propagation_; }
    bool PropagationForced() const { return propagationForced_; }
    bool Preserve() const { return preserve_; }
    std::uint32_t Today() const { return today_; }
    std::uint32_t Period() const { return period_; }
    std::uint32_t BlocksPerMonth() const { return blocksPerMonth_; }
    std::uint32_t WildSeedsPerMonth() const { return wildSeedsPerMonth_; }
    float ElevationScale() const { return elevationScale_; }
    float BurnRate() const { return burnRate_; }
    float BurntRemovalStep() const { return burntRemovalStep_; }
    float BurnAccumulator() const { return burnAccumulator_; }
    float BurntRemovalAccumulator() const { return burntRemovalAccumulator_; }
    cRZRandom& Random() { return random_; }
    const RandomIterator& BlockIterator() const { return blockWalk_; }
    const std::list<cISC4Occupant*>& BurntFlora() const { return burnt_; }

private:
    void DoCheat(const char* line);
    void PlaceFloraCommand(cIGZUnknown* parameters);
    void ReloadFloraTypes(std::uint32_t instance);
    cIGZMessageTarget2* AsMessageTarget() { return static_cast<cIGZMessageTarget2*>(this); }

    std::uint32_t refCount_ = 0;
    bool initialized_ = false;                                      // +0xc
    bool simulatorHeld_ = false;                                    // +0x1c held: the day and month agents are registered
    bool enabled_ = true;                                           // +0x2c
    bool propagation_ = true;                                       // +0x2d
    bool propagationForced_ = false;                                // +0x2e
    bool preserve_ = false;                                         // +0x2f (cheat "preserve": move, never remove)
    float sizeX_ = 1.0f, sizeZ_ = 1.0f;                             // +0x30, +0x34 metres
    std::int32_t cellsX_ = 1, cellsZ_ = 1;                          // +0x38, +0x3c
    std::uint32_t today_ = 0;                                       // +0x40 data1 of the last SimNewMonth
    std::uint32_t period_ = 6;                                      // +0x44 months per full pass
    std::uint32_t blocksPerMonth_ = 1;                              // +0x48
    std::uint32_t wildSeedsPerMonth_ = 5;                           // +0x4c
    std::uint8_t keepIteratorFlag_ = 0;                             // +0x50 (never set, only read)
    RandomIterator blockWalk_;                                      // +0x54 2x2-cell blocks
    cRZRandom random_{0xffffffffu};                                 // +0x84 seeded from the clock
    float elevationScale_ = 0.08f;                                  // +0x98 (16 / tuning 0xc9dbdd02)
    std::list<cISC4Occupant*> burnt_;                               // +0x9c burnt flora, a reference each
    float burnAccumulator_ = 0.0f;                                  // +0xa4
    float burntRemovalAccumulator_ = 0.0f;                          // +0xa8
    float burnRate_ = 1.0f;                                         // +0xac (1 / tuning 0xc9dbdd00)
    float burntRemovalStep_ = 4.0f;                                 // +0xb0 (tuning 0xc9dbdd01)
};

}  // namespace sc4
