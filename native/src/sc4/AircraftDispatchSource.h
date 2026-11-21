// cSC4FirePlaneDispatchSource and cSC4PoliceHelicopterDispatchSource - the dispatch sources of the aircraft (dispatch types 5
// and 6). The fire protection simulator makes and owns the fire plane source (i386 +200), the police simulator the helicopter
// source (i386 +0xc0); each registers itself with the dispatch manager in Init and keeps the list of the city's buildings that
// launch its aircraft. The dispatch manager asks the source for the nearest station with a free unit and for the automata group
// to send (the aircraft manager then sends the aircraft parked on that station's lot).
//
// Recovered from the Mac build: cSC4FirePlaneDispatchSource i386 0x004d1d3e..0x004d2aca, cSC4PoliceHelicopterDispatchSource
// i386 0x004d2aca..0x004d3852 (class ids 0x4c0ae114 / 0x0c0b06e8, objects 0x2c bytes on i386, 0x58 on 64-bit). The two classes
// are the same code; they differ only in their dispatch type, the three station properties, the automata group they send, their
// class id and the record versions Read accepts. The port writes that code once (cSC4AircraftDispatchSource) and gives each
// class its constants.
//
// Original layout (i386): +0x00 cISC4DispatchSource vtable (23 slots), +0x04 cIGZMessageTarget2, +0x08 cIGZSerializable, +0x0c
// initialized, +0x10 refcount, +0x14 dispatch type, +0x18 SL::spCity (held), +0x1c ilist<cISC4Occupant*> stations (one reference
// each), +0x24 SL::spMS2 (held), +0x28 next free call number (starts at 1).
//
// Stations: a building occupant (type 0x278128a0) whose unit-count property is above 0. Each station's units have call numbers
// base .. base+count-1 (the base property, picked so no two stations overlap) and a mask property with one bit per unit at home.
//
// Saved state (record version 1): the station list (count + occupant references) and the next call number.
//
// Quirks kept:
//  * CanDispatch answers true whenever any station has units, even when every unit is out; the "all out" answer goes in the flag
//    the dispatch manager drops (cSC4DispatchManager::CanDispatch).
//  * DispatchFromStation ignores GetDispatchesAtStation's answer, and with no unit at home it still reports success (the call
//    number is then base + count, no bit changes).
//  * DoOccupantInserted's call-number search reads the "in use" table past its end when a station's units would run beyond the
//    next free call number; the port reads those slots as free (the original reads whatever the bit vector's last word holds).
//  * Read does not release the stations it replaces. (It also keeps null entries for references that did not resolve, which
//    later crash it; the port leaves those out.)
#pragma once
#include <cstdint>
#include <list>

#include "gz/cIGZMessage2.h"
#include "gz/cIGZSerializable.h"
#include "sc4/DispatchManager.h"

namespace sc4 {

constexpr std::uint32_t kGZIID_cISC4DispatchSource = 0xc9c00435;
constexpr std::uint32_t kGZCLSID_cSC4FirePlaneDispatchSource = 0x4c0ae114;
constexpr std::uint32_t kGZCLSID_cSC4PoliceHelicopterDispatchSource = 0x0c0b06e8;

constexpr std::uint32_t kDispatchSourceBuildingOccupantType = 0x278128a0;
constexpr std::uint32_t kDispatchSourceNoAutomataProperty = 0x029244db;
constexpr std::uint32_t kDispatchSourceAutomataFlags = 0x11882;

// The constants that tell the two classes apart.
struct AircraftDispatchSourceTraits {
    tDispatchType type;               // +0x14
    std::uint32_t countProperty;      // the station's unit count (a building with a count above 0 is a station)
    std::uint32_t maskProperty;       // the units at home, one bit each
    std::uint32_t baseProperty;       // the first unit's call number
    std::uint32_t automataGroup;      // the group GetDispatchAutomataProperties hands the dispatch manager
    std::uint32_t classId;
    std::uint16_t maxReadVersion;     // Read accepts versions 1..max (fire plane: exactly 1; helicopter: below 2)
    std::uint16_t minReadVersion;
};

// The fire plane: dispatch type 5, the airport kind 2 aircraft (group 0x4303).
inline constexpr AircraftDispatchSourceTraits kFirePlaneDispatchTraits{kDispatchTypeFirePlane, 0x0c0afdd7, 0xcc0afddb, 0x0c0aff3f,
                                                                0x4303, kGZCLSID_cSC4FirePlaneDispatchSource, 1, 1};
// The police helicopter: dispatch type 6, the helipad kind 1 aircraft (group 0x4301).
inline constexpr AircraftDispatchSourceTraits kPoliceHelicopterDispatchTraits{kDispatchTypePoliceHelicopter, 0xcc0b0705, 0xec0b0709,
                                                                       0xac0b070e, 0x4301,
                                                                       kGZCLSID_cSC4PoliceHelicopterDispatchSource, 1, 0};

// What the sources reach through SL::sp* globals. Every pointer is borrowed.
class IAircraftDispatchSourceHost {
public:
    virtual ~IAircraftDispatchSourceHost() = default;
    // SL::spCity: held (AddRef in Init, Release in Shutdown).
    virtual cIGZUnknown* City() = 0;
    // SL::spMS2: held while initialized; the source registers its notifications on it.
    virtual cIGZMessageServer2* MessageServer() = 0;
    // SL::spDispatchManager (Init, CanDispatch, the dispatch messages) and spCity vslot 0x9c (Shutdown): the same object.
    virtual cISC4DispatchManager* DispatchManager() = 0;
    // spCity vslot 0x88 (occupant manager) vslot 0xc0 ReleaseOccupantList: releases each occupant and empties the list.
    virtual void ReleaseOccupantList(std::list<cISC4Occupant*>& occupants) = 0;
};
IAircraftDispatchSourceHost* AircraftDispatchSourceHost();
void SetAircraftDispatchSourceHost(IAircraftDispatchSourceHost* host);

class cSC4AircraftDispatchSource : public IDispatchSource, public cIGZMessageTarget2, public cIGZSerializable {
public:
    // cIGZUnknown, one count for the three views (QueryInterface 0x004d1d3e, AddRef 0x004d1d86, Release 0x004d256a).
    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return ++refCount_; }
    std::uint32_t Release() override;

    // cISC4DispatchSource (the fire plane's addresses; the helicopter's are the same functions 0xd8c bytes on).
    bool CanHandleDispatchType(std::uint32_t type) const override { return type == traits_.type; }   // 0x004d1fb2
    bool CanDispatch(float x, float z, bool* alreadyDispatched) override;                            // 0x004d2108
    int GetMaxPossibleDispatchCount();                                                               // 0x004d21d8
    bool GetStations(std::list<cISC4Occupant*>& out) override;                                       // 0x004d2802
    bool GetDispatchesAtStation(cISC4Occupant* station, std::uint32_t* mask, std::uint32_t* base,
                                std::uint32_t* count) override;                                     // 0x004d1fc6
    cISC4Occupant* DispatchFromNearestStation(float x, float z, std::uint32_t* callNumber) override; // 0x004d2232
    bool DispatchFromStation(cISC4Occupant* station, std::uint32_t* callNumber) override;            // 0x004d2590
    void RevokeDispatchFromStation(cISC4Occupant* station, std::uint32_t callNumber) override;       // 0x004d272e
    bool GetDispatchAutomataProperties(cISC4Occupant* station, std::uint32_t& group, cGZPersistResourceKey& key,
                                       std::uint32_t& flags, const float position[3]) override;     // 0x004d20d4
    bool Init();                                                                                     // 0x004d2322
    bool Shutdown();                                                                                 // 0x004d241e
    void SetStationAvailabilityFlags(cISC4Occupant* station, std::uint32_t mask);                    // 0x004d2076
    void SetStationBaseCallNumber(cISC4Occupant* station, std::uint32_t base);                       // 0x004d20a2
    void DoOccupantInserted(cISC4Occupant* occupant);                                                // 0x004d28c6
    void DoOccupantRemoved(cISC4Occupant* occupant);                                                 // 0x004d217e
    void DoDispatchBegin(std::uint32_t id) { (void)id; }                                             // 0x004d20ce
    void DoDispatchActivated(std::uint32_t id, bool active);                                         // 0x004d2154
    void DoDispatchEnd(std::uint32_t id);                                                            // 0x004d26e8
    bool ReturnDispatchToStation(cISC4Occupant* station, std::uint32_t callNumber);                  // 0x004d264a

    // cIGZMessageTarget2 / cIGZSerializable
    bool DoMessage(cIGZMessage2* message) override;                                                  // 0x004d1e44
    bool Write(cIGZOStream& stream) const override;                                                  // 0x004d1d98
    bool Read(cIGZIStream& stream) override;                                                         // 0x004d1de8
    std::uint32_t GetGZCLSID() const override { return traits_.classId; }                            // 0x0080ea10

    const AircraftDispatchSourceTraits& Traits() const { return traits_; }
    bool IsInitialized() const { return initialized_; }
    const std::list<cISC4Occupant*>& Stations() const { return stations_; }
    std::uint32_t NextCallNumber() const { return nextCallNumber_; }

protected:
    explicit cSC4AircraftDispatchSource(const AircraftDispatchSourceTraits& traits) : traits_(traits) {}
    virtual ~cSC4AircraftDispatchSource();

private:
    void FinalRelease();                                                                             // 0x004d253e

    const AircraftDispatchSourceTraits& traits_;
    bool initialized_ = false;                  // +0x0c
    std::uint32_t refCount_ = 0;                // +0x10
    cIGZUnknown* city_ = nullptr;               // +0x18
    std::list<cISC4Occupant*> stations_;        // +0x1c
    cIGZMessageServer2* messageServer_ = nullptr;   // +0x24
    std::uint32_t nextCallNumber_ = 1;          // +0x28
};

class cSC4FirePlaneDispatchSource final : public cSC4AircraftDispatchSource {
public:
    cSC4FirePlaneDispatchSource() : cSC4AircraftDispatchSource(kFirePlaneDispatchTraits) {}   // 0x004d282c
};

class cSC4PoliceHelicopterDispatchSource final : public cSC4AircraftDispatchSource {
public:
    cSC4PoliceHelicopterDispatchSource() : cSC4AircraftDispatchSource(kPoliceHelicopterDispatchTraits) {}   // 0x004d35b8
};

}  // namespace sc4
