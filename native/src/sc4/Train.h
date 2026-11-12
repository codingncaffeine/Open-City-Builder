// The rail automata: cSC4TrainCar (class id 0x4a416254, Mac i386 0x003930be..0x003943da, 0x2d4 bytes there), cSC4Train
// (0x8a41625a, 0x00393292..0x00397524, 0x308 bytes) and cSC4MassTransitTrain (0x4c22ae53, 0x004b1008..0x004b12fc).
//
// A train car is a vehicle that does not drive itself: its engine puts it on the track behind its leader each tick. A
// train is the engine, itself a train car: it drives along the path with its nose (the vehicle's path position) and its
// tail end (the back point) both on the track, and it pulls a chain of cars it creates one by one at the place it started
// from. The mass-transit train (subway, elevated rail, monorail) is a train that also stops at stations.
//
// Original layout after the cSC4Vehicle fields (i386 offsets):
//   cSC4TrainCar  +0x2c5 bool        elevated (occupant group 0x4217 or 0x4208): fades in and out with the track's
//                                    underground pieces
//                 +0x2c8 cS3DVector3 back point: where the car's rear end sits on the track
//   cSC4Train     +0x2d4 u32         how many cars it should pull
//                 +0x2d8 u32         the automata group of its cars (0 = none)
//                 +0x2dc std::vector<cSC4TrainCar*> the cars, counted; the last one is the tail
//                 +0x2e8 cS3DVector3 the place new cars appear at (-1, -1, -1 until the first tick fills it)
//                 +0x2f4 cISC4NetworkOccupant* the network occupant there, counted
//                 +0x2f8 u32         the path key there
//                 +0x2fc float       seconds it has stood still
//                 +0x300 float       the player-drive derail amount (NOT recovered: see the player-drive list below)
//                 +0x304 bool        (written by Init only)
//
// Records: the train car is u16 2 + the vehicle record. The train is u16 2 + the train car record, u32 n + n car refs (read
// with iid 0x4a416254, null refs dropped), u32 desired cars, the appear place, a ref to its occupant (iid 0xa821ef94) and its
// path key. The mass-transit train is u16 1 + the train record.
//
// NOT recovered (the player drives a train only in the U-Drive-It missions): cSC4Train::DoPlayerDriveTick (0x00396432),
// ChooseNewPlayerDriveDestination (0x003971d6), PlayerDriveCheckForDerail (0x00394abe), TranslatePlayerDriveCommands
// (0x00394ce4) and the derail wobble at the end of DoAnimationTick. A train car entering the collide state also tells the
// state's accident object which occupants crashed (slot 0x34 with 0x10 / 0x11); the port's accident has no such call.
#pragma once
#include <cstdint>
#include <vector>

#include "sc4/Vehicle.h"

namespace sc4 {

constexpr std::uint32_t kTrainCarGZCLSID = 0x4a416254;
constexpr std::uint32_t kTrainGZCLSID = 0x8a41625a;
constexpr std::uint32_t kMassTransitTrainGZCLSID = 0x4c22ae53;

// Occupant groups the trains test themselves for.
constexpr std::uint32_t kTrainGroupFreightEngine = 0x4216, kTrainGroupFreightCar = 0x4203, kTrainGroupFreightLastCar = 0x421a;
constexpr std::uint32_t kTrainGroupPassengerEngine = 0x4215, kTrainGroupPassengerCar = 0x4201;
constexpr std::uint32_t kTrainGroupElevatedEngine = 0x4217, kTrainGroupElevatedCar = 0x4208;
constexpr std::uint32_t kTrainGroupMonorailEngine = 0x422c, kTrainGroupMonorailCar = 0x422d;
constexpr std::uint32_t kTrainGroupGroup422e = 0x422e, kTrainGroupGroup422f = 0x422f;
// The buildings a train stops at (the HandleExitNetwork overrides).
constexpr std::uint32_t kStationGroup1300 = 0x1300, kStationGroup1302 = 0x1302, kStationGroup1509 = 0x1509;
constexpr std::uint32_t kStationGroup1303 = 0x1303, kStationGroup1307 = 0x1307;
// Path types SnapToPath asks the tile for.
constexpr std::uint32_t kTrainPathTypeRail = 3, kTrainPathTypeElevated = 4, kTrainPathTypeMonorail = 6, kTrainPathType7 = 7;

constexpr std::uint32_t kAutomataFlagPulled = 0x10000000;       // a car its engine moves (the trailer cars' flag)
constexpr float kTrainStationStopMilliseconds = 4800.0f;        // 0x459c4000
constexpr std::uint32_t kMassTransitStationSfx = 0x0c16dc50;    // the state's sound
constexpr float kTrainCollideMilliseconds = 20000.0f;           // 0x469c4000
constexpr float kTrainCarWreckMilliseconds = 10000.0f;          // 0x00a70824
constexpr float kTrainStandStillSeconds = 10.0f;                // 0x00a707d4

class cSC4Train;

class cSC4TrainCar : public cSC4Vehicle {
public:
    explicit cSC4TrainCar(ITrafficAutomatonHost* host = nullptr, IAutomatonHost* automatonHost = nullptr,
                          IVehicleHost* vehicleHost = nullptr, const cGZPersistResourceKey& modelKey = {});   // @ 0x003942de

    bool Init() override;                                                    // @ 0x003930be
    bool QueryInterface(std::uint32_t iid, void** out) override;             // @ 0x003931fc
    bool Write(cIGZOStream& stream) const override;                          // @ 0x00393c0c
    bool Read(cIGZIStream& stream) override;                                 // @ 0x00393a0e
    std::uint32_t GetGZCLSID() const override { return kTrainCarGZCLSID; }   // @ 0x0074bda4
    cSC4Automaton* FindDrivingAutomaton() override;                          // @ 0x0039369c
    bool HandleCollision(cSC4Automaton* other, const cS3DVector3& point) override;   // @ 0x00393242
    bool HandleEnterState(tAutomataState* state) override;                   // @ 0x003936c2
    bool DoPlayerDriveTick(float) override { return false; }                 // @ 0x003931f4
    bool DoCollideTick(float milliseconds) override;                         // @ 0x003931a0
    bool DoPathTick(float) override { return true; }                         // @ 0x00393196: the engine moves it
    void SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey,
                 const std::vector<cS3DVector3>* path, bool keepDestination) override;   // @ 0x003938de
    void HandleNetworkOccupantRemoved() override { cSC4TrafficAutomaton::HandleNetworkOccupantRemoved(); }   // @ 0x00393238
    bool IsTrain() const override { return true; }                           // @ 0x0074bdae
    void RecalculatePathOffset() override { pathOffset_ = 0.0f; }            // @ 0x0074bdc0
    bool CheckIntersectionDensity() override { return true; }                // @ 0x0074bdd2
    void ShowHeadlights(bool on, bool instant) override { cSC4Vehicle::ShowHeadlights(on, instant); }   // @ 0x0039317e
    virtual bool IsEngine() const { return false; }                          // 0x31c @ 0x0074bdb8

    bool IsElevated() const { return elevated_; }
    const cS3DVector3& BackPoint() const { return backPoint_; }
    void SetBackPoint(const cS3DVector3& point) { backPoint_ = point; }

protected:
    friend class cSC4Train;
    bool elevated_ = false;                                                  // +0x2c5
    cS3DVector3 backPoint_{0.0f, 0.0f, 0.0f};                                // +0x2c8
};

class cSC4Train : public cSC4TrainCar {
public:
    explicit cSC4Train(ITrafficAutomatonHost* host = nullptr, IAutomatonHost* automatonHost = nullptr,
                       IVehicleHost* vehicleHost = nullptr, const cGZPersistResourceKey& modelKey = {});   // @ 0x003944d6
    ~cSC4Train() override;                                                   // @ 0x0074c56c

    bool Init() override;                                                    // @ 0x003940b2
    bool Shutdown() override;                                                // @ 0x00393e62
    bool QueryInterface(std::uint32_t iid, void** out) override;             // @ 0x003933a0
    bool Write(cIGZOStream& stream) const override;                          // @ 0x00393c3e
    bool Read(cIGZIStream& stream) override;                                 // @ 0x00393d30
    std::uint32_t GetGZCLSID() const override { return kTrainGZCLSID; }      // @ 0x0074bdf0
    cSC4Automaton* FindDrivingAutomaton() override { return this; }          // @ 0x00393398
    bool HandleCollision(cSC4Automaton* other, const cS3DVector3& point) override;   // @ 0x00393a4a
    bool DoAnimationTick(std::uint32_t unknown, float milliseconds) override;   // @ 0x00394646 (PARTIAL: no derail wobble)
    bool ShouldCollide(cSC4Automaton* other) const override;                 // @ 0x003933dc
    bool HandleEnterState(tAutomataState* state) override;                   // @ 0x0039499e
    bool HandleExitState(const tAutomataState* state) override;              // @ 0x0039355a
    bool DoPlayerDriveTick(float) override { return false; }                 // 0x00396432 NOT recovered (see the header note)
    float GetSpeedModifier() const override;                                 // @ 0x00393444
    bool DoPathTick(float milliseconds) override;                            // @ 0x00395654
    bool StartTrip(ITrafficAutomatonOccupant* occupant, std::uint32_t key, bool param3) override;   // @ 0x00395f54
    void SetPath(ITrafficAutomatonOccupant* occupant, std::uint32_t pathKey,
                 const std::vector<cS3DVector3>* path, bool keepDestination) override;   // @ 0x003939a6
    bool SnapToPath() override;                                              // @ 0x00395a98
    bool ChooseNewDestination() override;                                    // @ 0x00393f9c
    bool HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect) override;   // @ 0x00393292
    std::uint8_t GetDesiredLookaheadCount() const override { return 6; }     // @ 0x0074be14
    bool IsEngine() const override { return true; }                          // @ 0x0074be0a
    virtual void SetNumCars(std::uint8_t count);                             // 0x320 @ 0x00393814
    virtual std::uint32_t GetNumCars() const { return static_cast<std::uint32_t>(cars_.size()); }   // 0x324 @ 0x0074c1d0
    virtual void SetDesiredNumCars(std::uint8_t count);                      // 0x328 @ 0x003937d6
    virtual std::uint8_t GetDesiredNumCars() const { return static_cast<std::uint8_t>(desiredCars_); }   // 0x32c @ 0x0074bdfa
    virtual void TickFollowingCars(float milliseconds);                      // 0x334 @ 0x00394da6
    virtual bool AddCarAtEnd();                                              // 0x338 @ 0x00397524

    cSC4TrainCar* GetTailCar();                                              // @ 0x003938a6
    void MarkTrainInCell(const tLookaheadNode& node, bool mark);             // @ 0x00393618

    std::uint32_t CarGroup() const { return carGroup_; }
    const std::vector<cSC4TrainCar*>& Cars() const { return cars_; }
    const cS3DVector3& TailPosition() const { return tailPos_; }
    ITrafficAutomatonOccupant* TailOccupant() const { return tailOccupant_; }
    std::uint32_t TailPathKey() const { return tailPathKey_; }
    float StandStillSeconds() const { return standStill_; }

protected:
    std::uint32_t desiredCars_ = 0;                                          // +0x2d4
    std::uint32_t carGroup_ = 0;                                             // +0x2d8
    std::vector<cSC4TrainCar*> cars_;                                        // +0x2dc
    cS3DVector3 tailPos_{-1.0f, -1.0f, -1.0f};                               // +0x2e8
    ITrafficAutomatonOccupant* tailOccupant_ = nullptr;                      // +0x2f4
    std::uint32_t tailPathKey_ = 0;                                          // +0x2f8
    float standStill_ = 0.0f;                                                // +0x2fc
    float derail_ = 0.0f;                                                    // +0x300
    bool flag304_ = false;                                                   // +0x304
};

class cSC4MassTransitTrain : public cSC4Train {
public:
    using cSC4Train::cSC4Train;                                              // @ 0x004b12fc
    bool Write(cIGZOStream& stream) const override;                          // @ 0x004b1144
    bool Read(cIGZIStream& stream) override;                                 // @ 0x004b1176
    std::uint32_t GetGZCLSID() const override { return kMassTransitTrainGZCLSID; }   // @ 0x00801224
    bool DoStopAtStationTick(float milliseconds) override { return cSC4TrafficAutomaton::DoStopAtStationTick(milliseconds); }   // @ 0x004b1126
    bool ChooseNewDestination() override;                                    // @ 0x004b11b2
    bool HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect) override;   // @ 0x004b1034
};

}  // namespace sc4
