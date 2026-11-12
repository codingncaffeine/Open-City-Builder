// The small vehicles derived straight from cSC4Vehicle: cSC4CarjackedVehicle (0x1006d6864.., class id 0xcbb32b0a),
// cSC4GetawayVan (0x100507244.., id 0xea863423) and cSC4MayorLimo (0x1006f3580.., id 0x4c0dd224). RECOVERED here: constructors
// and the stream / identity members, and the PostInit of the first two (Mac i386 0x0049d9a8 / 0x0039189c). NOT recovered: the
// limo's Init / Shutdown / TranslatePlayerDriveCommands / HandleExitState-adjacent work (see the ledger).
#pragma once
#include "sc4/Vehicle.h"

namespace sc4 {

constexpr std::uint32_t kCarjackedVehicleGZCLSID = 0xcbb32b0a;
constexpr std::uint32_t kGetawayVanGZCLSID = 0xea863423;
constexpr std::uint32_t kMayorLimoGZCLSID = 0x4c0dd224;

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

// Version 1 around the vehicle record. Its three fields after the vehicle's (+0x424: 50, +0x428: 48.0f, +0x42c: 10.0f) are
// initialised by the constructor; their meaning is not known yet.
class cSC4MayorLimo : public cSC4Vehicle {
public:
    cSC4MayorLimo(ITrafficAutomatonHost* host = nullptr, IAutomatonHost* automatonHost = nullptr,
                  IVehicleHost* vehicleHost = nullptr, const cGZPersistResourceKey& modelKey = {})
        : cSC4Vehicle(host, automatonHost, vehicleHost, modelKey) {}          // @ 0x1006f3580
    bool HandlePlayerDriveCommand(std::uint32_t command, bool on) override {   // 0x1e0 @ 0x1006f3706 (just the vehicle's)
        return cSC4Vehicle::HandlePlayerDriveCommand(command, on);
    }
    bool HandleExitState(const tAutomataState* state) override {              // 0x3b0 @ 0x1006f3714 (the traffic automaton's, skipping the vehicle's)
        return cSC4TrafficAutomaton::HandleExitState(state);
    }
    bool Write(cIGZOStream& stream) const override;                           // 0x240 @ 0x1006f3814
    bool Read(cIGZIStream& stream) override;                                  // 0x248 @ 0x1006f38c8
    std::uint32_t GetGZCLSID() const override { return kMayorLimoGZCLSID; }   // 0x250 @ 0x1006f3a30
    std::uint32_t Value424() const { return value424_; }
    float Value428() const { return value428_; }
    float Value42c() const { return value42c_; }

private:
    std::uint32_t value424_ = 50;        // +0x424 (0x32)
    float value428_ = 48.0f;             // +0x428 (0x42400000)
    float value42c_ = 10.0f;             // +0x42c (0x41200000)
};

}  // namespace sc4
