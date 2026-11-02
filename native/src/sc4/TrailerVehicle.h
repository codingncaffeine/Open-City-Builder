// cSC4TrailerVehicle — a road vehicle that pulls a chain of trailer cars (class id 0x4a5b69ed; Mac i386 0x00391a90..0x00392fc6,
// 0x2f4 bytes there). Each trailer car is an ordinary cSC4Vehicle the lead vehicle creates behind its tail and steers along the
// lead's path history; the lead also remembers where its tail is (position, network occupant, path key) so the next car can be
// placed there.
//
// Original layout after the cSC4Vehicle fields (i386 offsets):
//   +0x2c8 std::vector<u32>          the model ids of the cars it should pull (AddTrailer; GetDesiredNumCars = its size)
//   +0x2d4 std::vector<cSC4Vehicle*> the cars it pulls, counted (GetNumCars = its size; the last one is the tail car)
//   +0x2e0 cS3DVector3               the tail position (-1, -1, -1 until the first tick fills it)
//   +0x2ec cISC4NetworkOccupant*     the network occupant under the tail, counted
//   +0x2f0 u32                       the tail's path key
//
// Record (Write 0x00391dcc / Read 0x00391fae, Windows 0x005562A0 / 0x00556C30): u16 2, the vehicle record, u32 n + n refs to the
// cars (read with iid 0x896e714a, null refs dropped), u32 m + m model ids (zeros dropped), the tail position, a ref to the tail
// occupant (iid 0xa821ef94) and the tail path key.
//
// PARTIAL: the car-following half of DoAnimationTick (AddCarAtEnd 0x00392116, TickFollowingCars 0x003929ca,
// TickFollowingCarsOffRoad 0x003924e8, FindPointOnPathSegment 0x00392372) and SetAutomataGroup's model-id list
// (0x00391d80) need the automata manager's CreateAutomaton and the group template's model list, which the native game
// does not run yet. A loaded trailer keeps its cars and its record.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/Vehicle.h"

namespace sc4 {

constexpr std::uint32_t kTrailerVehicleGZCLSID = 0x4a5b69ed;
constexpr std::uint32_t kAutomataFlagTrailer = 0x10000000;   // Init sets it before the vehicle's Init

class cSC4TrailerVehicle : public cSC4Vehicle {
public:
    explicit cSC4TrailerVehicle(ITrafficAutomatonHost* host = nullptr, IAutomatonHost* automatonHost = nullptr,
                                IVehicleHost* vehicleHost = nullptr, const cGZPersistResourceKey& modelKey = {});
    ~cSC4TrailerVehicle() override;                                          // @ 0x0074bc30

    bool Init() override;                                                    // @ 0x00391a90
    bool Shutdown() override;                                                // @ 0x00391f16
    bool QueryInterface(std::uint32_t iid, void** out) override;             // @ 0x00391adc
    bool Write(cIGZOStream& stream) const override;                          // @ 0x00391dcc
    bool Read(cIGZIStream& stream) override;                                 // @ 0x00391fae
    std::uint32_t GetGZCLSID() const override { return kTrailerVehicleGZCLSID; }   // @ 0x0074b522
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // @ 0x00391c7e (PARTIAL)
    bool ShouldCollide(cSC4Automaton* other) const override;                 // @ 0x00391b18
    void SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey,
                 const std::vector<cS3DVector3>* path, bool keepDestination) override;   // @ 0x00391b80
    void ShowHeadlights(bool on, bool instant) override;                     // @ 0x00391ac4

    void AddTrailer(std::uint32_t modelId) { desiredCars_.push_back(modelId); }   // @ 0x00391ef6
    cSC4Vehicle* GetTailCar();                                               // @ 0x00391c4c
    void GetCollidePosition(cS3DVector3& out);                               // @ 0x00391bf8
    std::uint32_t GetDesiredNumCars() const { return static_cast<std::uint32_t>(desiredCars_.size()); }   // @ 0x0074b83a
    std::uint32_t GetNumCars() const { return static_cast<std::uint32_t>(cars_.size()); }                 // @ 0x0074b89c

    const std::vector<std::uint32_t>& DesiredCars() const { return desiredCars_; }
    const std::vector<cSC4Vehicle*>& Cars() const { return cars_; }
    const cS3DVector3& TailPosition() const { return tailPos_; }
    ITrafficAutomatonOccupant* TailOccupant() const { return tailOccupant_; }
    std::uint32_t TailPathKey() const { return tailPathKey_; }

private:
    std::vector<std::uint32_t> desiredCars_;                                 // +0x2c8
    std::vector<cSC4Vehicle*> cars_;                                         // +0x2d4
    cS3DVector3 tailPos_{-1.0f, -1.0f, -1.0f};                               // +0x2e0
    ITrafficAutomatonOccupant* tailOccupant_ = nullptr;                      // +0x2ec
    std::uint32_t tailPathKey_ = 0;                                          // +0x2f0
};

}  // namespace sc4
