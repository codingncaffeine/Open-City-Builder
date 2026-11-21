// cSC4AircraftManager - the automata manager of the aircraft (class id / interface 0xc98f9d4a, object 0xb40 bytes). A
// cSC4AutomataManager that keeps one automaton per occupant that launches aircraft (airports, helipads, police and fire
// stations: the properties 0xabb90e58 / 0xbfa15a5 select the kind) and the list of runway markers the planes take off from
// and land on (property 0x8bfb1072 on the runway marker props). Recovered from the Mac build (i386 0x0038c5fc..0x0038dd78;
// 64-bit 0x10050044c..0x10050234e).
//
// Original layout (64-bit; the i386 offsets are 0x70 lower): the cSC4AutomataManager base (0xb08), then the airport map
// std::map<cISC4Occupant*, cISC4Automaton*> at +0xb08 (both held), the std::list<RunwayMarker> at +0xb20 and the
// cSC4BuildingPropFilter* at +0xb38 (made by Init, the filter the aircraft's collision query passes).
//
// Saved state (record version 5): the base manager, then the count and the (occupant, automaton) pairs of the airport map,
// then the count and the runway markers (three floats and a byte). Versions below 5 are the base manager's own legacy record.
//
// Behavior (each method names its i386 address):
//  * DoMessage: an occupant inserted / removed message (0x99ef1142 / 0x99ef1143) for an occupant without property
//    0x29244db ("no automata") runs HandleOccupantInserted / HandleOccupantRemoved before the base manager sees it.
//  * HandleOccupantInserted: property 0xabb90e58 = 1..5 makes a helicopter-pad automaton of group 0x4301 / 0x4305 / 0x4304 /
//    0x4307 / 0x4308 (the first three start in state 0x36 / 0x38 / 0x37); property 0xbfa15a5 = 1..5 makes an airport
//    automaton of group 0x4306 / 0x4303 / 0x4311 / 0x4312 / 0x4313. Both create through CreateAutomaton with the base group
//    (0x4300 / 0x4302), the model key of a random exemplar of the specific group, flags 0x10002, the occupant's position on
//    the bottom of its bounding box and the facing of the prop's orientation. Property 0x8bfb1072 (the runway number) adds
//    a runway marker at the occupant's position.
//  * GetRunwayList: the runway marker positions on the lot under `position`, two per runway number (index 2n-2 / 2n-1).
//  * Dispatch (both overloads): a police helicopter (group 0x4301 / a police station's aircraft) is sent with states 0x35
//    (hold, 10000 ms) and 0x33; anything else with state 0x39. The destination is the dispatch position 200 above the
//    terrain, and the dispatching station's efficiency (police / fire protection simulator) is stored on the aircraft.
//  * AddAutomaton(nullptr) adds a new helicopter or blimp (an even draw of the manager's random stream).
//
// Quirks kept:
//  * A second airport property on the same occupant overwrites the map entry of the first (whose references are not
//    released), as the original's map operator[] does.
//  * HandleOccupantInserted dereferences the exemplar ChooseRandomExemplar returned; the port skips the automaton when the
//    group has no exemplar (the original would read the key from address 0x10).
//  * A prop orientation outside 0..3 indexes the facing table out of range in the original; the port takes facing 0.
//  * GetRunwayList pads with kInvalidVector (-1, -2, -3) and puts a marker in its pair's second slot whenever the first
//    already differs from it (cS3DVector3::operator!=, tolerance 0.0001), so a third marker of one runway overwrites the
//    second.
//  * HandleOccupantRemoved finds the runway marker by its position and runway number with an exact-number, epsilon-position
//    comparison (cS3DVector3::operator==).
//
// Seams: the lot manager, the prop occupant's orientation, the police / fire protection simulators, the terrain and the
// aircraft classes themselves go through IAircraftManagerHost below (each method names the original call).
#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <vector>

#include "sc4/AutomataManager.h"

class cRZRandomSuper;

namespace sc4 {

class cISC4Occupant;
class cSC4BuildingPropFilter;

constexpr std::uint32_t kGZCLSID_cSC4AircraftManager = 0xc98f9d4a;
constexpr std::uint32_t kAircraftManagerFallbackGroup = 0xc96e75dc;   // the aircraft class id
constexpr std::uint32_t kAircraftManagerDefaultGroup = 0x4003;
constexpr std::uint32_t kAircraftManagerExemplarGroup = 0x18;
constexpr std::uint16_t kAircraftManagerRecordVersion = 5;
constexpr std::uint32_t kAircraftIIDOccupant = 0x26bb767e;
constexpr std::uint32_t kAircraftIIDAutomaton = 0xa9b40f05;

// The occupant properties the manager reads.
constexpr std::uint32_t kAircraftPropertyHelipad = 0xabb90e58;     // uint8 1..5: the helicopter kind of the occupant
constexpr std::uint32_t kAircraftPropertyAirport = 0x0bfa15a5;     // uint8 1..5: the airplane kind of the occupant
constexpr std::uint32_t kAircraftPropertyRunway = 0x8bfb1072;      // uint8: the runway number of a runway marker prop
constexpr std::uint32_t kAircraftPropertyMinZoom = 0x4998c91a;     // uint8 in the automata tuning exemplar (minus one)

constexpr std::uint32_t kAircraftGroupHelicopterBase = 0x4300;     // CreateAutomaton's group for the helipad kinds
constexpr std::uint32_t kAircraftGroupAirplaneBase = 0x4302;       // ... and for the airport kinds
constexpr std::uint32_t kAircraftGroupPoliceHelicopter = 0x4301;
constexpr std::uint32_t kAircraftCreateFlags = 0x10002;
constexpr float kAircraftDispatchAltitude = 200.0f;                // FLOAT_00a708e4: above the terrain at the target
constexpr float kAircraftDispatchHoldTimer = 10000.0f;             // 0x461c4000: the police helicopter's state 0x35 timer

// The facing per prop orientation (kFacing @ i386 0x0084c1c0).
constexpr float kAircraftFacing[4][3] = {{0.0f, 0.0f, 1.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}};

struct AircraftRunwayMarker {
    float x = 0, y = 0, z = 0;   // +0x10 / +0x14 / +0x18 of the list node
    std::uint8_t flag = 0;       // +0x1c the runway number
};

class IAircraftManagerHost {
public:
    virtual ~IAircraftManagerHost() = default;
    // GZCOM_Interface<cISC4Occupant, cISC4PropOccupant>(0xe9793a65) -> vslot 0x48 GetOrientation (0..3); 0 for an occupant
    // that is not a prop.
    virtual std::int32_t PropOrientation(cISC4Occupant* occupant) = 0;
    // SL::spLotManager vslot 0x30 GetLot(position): compared only by identity (null = no lot).
    virtual const void* LotAt(const cS3DVector3& position) = 0;
    // SL::spLotManager vslot 0x70 GetOccupantLot(station), the lot's vslot 0x188 GetLotOccupants and the occupant manager's
    // vslot 0x180 ReleaseOccupantList: the occupants on the station's lot (borrowed). False when the station has no lot.
    virtual bool StationLotOccupants(IAutomataOccupant* station, std::vector<cISC4Occupant*>& out) = 0;
    // SL::spPoliceSim vslot 0xb0 IsPoliceStation / vslot 0xc8 GetStationEfficiency.
    virtual bool IsPoliceStation(IAutomataOccupant* station) = 0;
    virtual float PoliceStationEfficiency(IAutomataOccupant* station) = 0;
    // SL::spFireProtectionSim vslot 0x38 IsFireStation / vslot 0x50 GetStationEfficiency.
    virtual bool IsFireStation(IAutomataOccupant* station) = 0;
    virtual float FireStationEfficiency(IAutomataOccupant* station) = 0;
    // SL::spTerrain vslot 0x58 (point in bounds), 0x60 (height), 0xe8 (the height outside the terrain).
    virtual bool TerrainPointInBounds(float x, float z) = 0;
    virtual float TerrainHeight(float x, float z) = 0;
    virtual float DefaultAltitude() = 0;
    // The automaton's vslot 0x50 SetDestination.
    virtual void SetDestination(IAutomataAutomaton* automaton, const cS3DVector3& destination) = 0;
    // cSC4Aircraft +0x308 (i386 +0x1f4): the dispatching station's efficiency.
    virtual void SetStationEfficiency(IAutomataAutomaton* automaton, float efficiency) = 0;
    // operator new + the cSC4Helicopter / cSC4Blimp constructor (AddAutomaton(nullptr)); one reference, null on failure.
    virtual IAutomataAutomaton* NewHelicopter() = 0;
    virtual IAutomataAutomaton* NewBlimp() = 0;
    // operator new(0xc) + cSC4BuildingPropFilter::cSC4BuildingPropFilter + AddRef (Init); Release in Shutdown.
    virtual cSC4BuildingPropFilter* NewBuildingPropFilter() = 0;
    virtual void ReleaseBuildingPropFilter(cSC4BuildingPropFilter* filter) = 0;
};
IAircraftManagerHost* AircraftManagerHost();
void SetAircraftManagerHost(IAircraftManagerHost* host);

class cSC4AircraftManager : public cSC4AutomataManager {
public:
    cSC4AircraftManager();                                    // i386 0x0038d128
    ~cSC4AircraftManager() override;                          // 0x100502244

    bool QueryInterface(std::uint32_t iid, void** out) override;   // i386 0x0038c68c
    bool Init() override;                                     // i386 0x0038cf38
    bool Shutdown() override;                                 // i386 0x0038d66e
    bool Write(cIGZOStream& stream) const override;           // i386 0x0038d478
    bool Read(cIGZIStream& stream) override;                  // i386 0x0038d716
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4AircraftManager; }   // i386 0x0074a0ee

    bool AddAutomaton(IAutomataAutomaton* automaton) override;   // i386 0x0038c604
    bool HandleTuningParametersChanged() override;               // i386 0x0038cacc
    bool DoMessage(cIGZMessage2* message) override;              // i386 0x0038dcba
    bool DoMessage(cGZMessage& message) override { (void)message; return false; }   // i386 0x0038c5fc
    // 0x88: the aircraft already parked at the dispatching station is sent to `to` (no new automaton is made).
    IAutomataAutomaton* Dispatch(std::uint32_t group, const tAutomataLocation& from, const tAutomataLocation& to,
                                 std::uint32_t flags, std::uint8_t type, const cGZPersistResourceKey& key,
                                 std::uint8_t argument) override;   // i386 0x0038d1a6
    // 0x90: re-dispatch an aircraft.
    bool Dispatch(IAutomataAutomaton* automaton, const tAutomataLocation& from, const tAutomataLocation& to,
                  std::uint32_t flags, std::uint8_t type) override;   // i386 0x0038cc28

    void HandleOccupantInserted(cISC4Occupant* occupant);     // i386 0x0038d886
    void HandleOccupantRemoved(cISC4Occupant* occupant);      // i386 0x0038d586
    void GetRunwayList(const cS3DVector3& position, std::vector<cS3DVector3>& out);   // i386 0x0038dd78

    // cSC4Aircraft::mpRandom: Init points it at the manager's random stream (the base's +0x78), Shutdown clears it.
    static cRZRandomSuper* AircraftRandom();
    // The aircraft's two draws on mpRandom (IAircraftHost::RandomWeightedChoice / RandomDoubleRange).
    static std::uint32_t RandomWeightedChoice(std::uint32_t count, const double* weights);
    static double RandomDoubleRange(double lo, double hi);

    std::size_t AirportCount() const { return airports_.size(); }
    const std::map<cISC4Occupant*, IAutomataAutomaton*>& Airports() const { return airports_; }
    const std::list<AircraftRunwayMarker>& RunwayMarkers() const { return runways_; }
    cSC4BuildingPropFilter* BuildingPropFilter() const { return buildingFilter_; }

private:
    void ReleaseAirports();

    std::map<cISC4Occupant*, IAutomataAutomaton*> airports_;   // +0xb08 occupant -> automaton (both held)
    std::list<AircraftRunwayMarker> runways_;                 // +0xb20
    cSC4BuildingPropFilter* buildingFilter_ = nullptr;        // +0xb38 (counted)
};

}  // namespace sc4
