// The small vehicles derived straight from cSC4Vehicle: cSC4CarjackedVehicle (0x1006d6864.., class id 0xcbb32b0a),
// cSC4GetawayVan (0x100507244.., id 0xea863423) and cSC4MayorLimo (0x1006f3580.., id 0x4c0dd224). RECOVERED here: constructors
// and the stream / identity members, the PostInit of the first two (Mac i386 0x0049d9a8 / 0x0039189c) and the limo's Init
// (0x004b1536). NOT recovered: the limo's TranslatePlayerDriveCommands (0x004b142c: the player throws money, which costs
// the budget `cost` and raises the aura around the limo by `auraMagnitude` within `auraRadius`).
// Also cSC4Bus (Mac i386 0x003913ec..0x00391774, class id 0xaa9f4a3d): a vehicle that takes waiting pedestrians on at a
// station.
#pragma once
#include "sc4/Vehicle.h"

namespace sc4 {

constexpr std::uint32_t kCarjackedVehicleGZCLSID = 0xcbb32b0a;
constexpr std::uint32_t kGetawayVanGZCLSID = 0xea863423;
constexpr std::uint32_t kMayorLimoGZCLSID = 0x4c0dd224;
constexpr std::uint32_t kBusGZCLSID = 0xaa9f4a3d;
constexpr std::uint32_t kMayorLimoTuningProperty = 0xec15654a;   // three floats: cost, aura radius, aura magnitude
constexpr std::uint32_t kBusStationGroup = 0x1301;               // the building a bus stops at
constexpr float kBusStationStopMilliseconds = 4800.0f;           // 0x459c4000
constexpr float kBusNearbyPedRadius = 16.0f;                     // DoStopAtStationTick's kfNearbyPedRadius

// The animated figures the two crime vehicles carry (model resources, group 0xbadb57f1).
constexpr cGZPersistResourceKey kCarjackingSimsModelKey{0x5ad0e817, 0xbadb57f1, 0x112a0000};   // _simsModelKey (0x00c18a40)
constexpr cGZPersistResourceKey kGetawayEscapeModelKey{0x5ad0e817, 0xbadb57f1, 0x117c0000};
constexpr cGZPersistResourceKey kGetawayCaughtModelKey{0x5ad0e817, 0xbadb57f1, 0x117b0000};
constexpr std::uint32_t kCarjackingSfx = 0xcbc17ddf;

// What the getaway van asks the city when it appears (SL::spCity, spPoliceSim, spCrimeSim).
class IGetawayVanHost {
public:
    virtual ~IGetawayVanHost() = default;
    // city vslot 0xc4 GetCitySituationManager, its vslot 0x24 GetCurrentTarget: is this van the mission's target
    virtual bool IsSituationTarget(cISC4Occupant* occupant) = 0;
    // the police power grid and the crime grid at a cell; false without the two simulators, their grids or when the cell
    // lies outside the city
    virtual bool PoliceAndCrimeAt(int cellX, int cellZ, std::uint8_t& police, std::uint8_t& crime) = 0;
};
IGetawayVanHost* GetawayVanHost();
void SetGetawayVanHost(IGetawayVanHost* host);

// Version 1 around the vehicle record.
class cSC4CarjackedVehicle : public cSC4Vehicle {
public:
    using cSC4Vehicle::cSC4Vehicle;                                           // @ 0x1006d6864
    bool PostInit() override;                                                 // 0x360 (i386 0x0049d9a8)
    bool Write(cIGZOStream& stream) const override;                           // 0x240 @ 0x1006d694a
    bool Read(cIGZIStream& stream) override;                                  // 0x248 @ 0x1006d69fe
    std::uint32_t GetGZCLSID() const override { return kCarjackedVehicleGZCLSID; }   // 0x250 @ 0x1006d6c26
};

// Version 3: the vehicle record, a bool (always written false) and a signed 32-bit value (always written 0), both read and dropped.
class cSC4GetawayVan : public cSC4Vehicle {
public:
    using cSC4Vehicle::cSC4Vehicle;                                           // @ 0x100507244
    bool PostInit() override;                                                 // 0x360 (i386 0x0039189c)
    bool Escapes() const { return escapes_; }
    bool Write(cIGZOStream& stream) const override;                           // 0x240 @ 0x10050744e
    bool Read(cIGZIStream& stream) override;                                  // 0x248 @ 0x1005074c4
    std::uint32_t GetGZCLSID() const override { return kGetawayVanGZCLSID; }  // 0x250 @ 0x10050759c

private:
    bool escapes_ = false;   // port only: which of the two animations PostInit chose
};

// Version 1 around the vehicle record. Its three fields after the vehicle's (cost 50, aura radius 48, aura magnitude 10) are
// set by the constructor and replaced in Init by the exemplar's property 0xec15654a when it has that many floats.
class cSC4MayorLimo : public cSC4Vehicle {
public:
    cSC4MayorLimo(ITrafficAutomatonHost* host = nullptr, IAutomatonHost* automatonHost = nullptr,
                  IVehicleHost* vehicleHost = nullptr, const cGZPersistResourceKey& modelKey = {})
        : cSC4Vehicle(host, automatonHost, vehicleHost, modelKey) {}          // @ 0x1006f3580
    bool Init() override;                                                     // @ i386 0x004b1536
    bool HandlePlayerDriveCommand(std::uint32_t command, bool on) override {   // 0x1e0 @ 0x1006f3706 (just the vehicle's)
        return cSC4Vehicle::HandlePlayerDriveCommand(command, on);
    }
    bool HandleExitState(const tAutomataState* state) override {              // 0x3b0 @ 0x1006f3714 (the traffic automaton's, skipping the vehicle's)
        return cSC4TrafficAutomaton::HandleExitState(state);
    }
    bool Write(cIGZOStream& stream) const override;                           // 0x240 @ 0x1006f3814
    bool Read(cIGZIStream& stream) override;                                  // 0x248 @ 0x1006f38c8
    std::uint32_t GetGZCLSID() const override { return kMayorLimoGZCLSID; }   // 0x250 @ 0x1006f3a30
    std::uint32_t Cost() const { return cost_; }
    float AuraRadius() const { return auraRadius_; }
    float AuraMagnitude() const { return auraMagnitude_; }

private:
    std::uint32_t cost_ = 50;            // +0x424 (i386 +0x2c8)
    float auraRadius_ = 48.0f;           // +0x428 (i386 +0x2cc)
    float auraMagnitude_ = 10.0f;        // +0x42c (i386 +0x2d0)
};

// Version 2 around the vehicle record.
class cSC4Bus : public cSC4Vehicle {
public:
    using cSC4Vehicle::cSC4Vehicle;                                           // @ 0x00391774
    bool Write(cIGZOStream& stream) const override;                           // @ 0x003914f0
    bool Read(cIGZIStream& stream) override;                                  // @ 0x00391522
    std::uint32_t GetGZCLSID() const override { return kBusGZCLSID; }
    bool DoStopAtStationTick(float milliseconds) override;                    // @ 0x0039155e
    bool HandleExitNetwork(ITrafficAutomatonOccupant* occupant, std::uint32_t key, void* lot, const SC4RectInt32& rect) override;   // @ 0x00391418
};

}  // namespace sc4
