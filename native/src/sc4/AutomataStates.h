// tAutomataState and its ten subclasses — the per-state records of an automaton (what it is doing right now: random walk,
// riot leading, chase, bumped, collide, plop-jump, plummet, pull over, player drive, attach animation). Each state is saved
// with the automaton: base record (u16 version 3; the controller unless its flag 8 is set; state id; timer; flag; sound handle;
// two values) then the subclass fields. 3 functions for the base (0x10010e128 Shutdown, 0x10010e164 Write, 0x10010e214 Read) and
// Write/Read (plus Shutdown for Collide / PlayerDrive) for each subclass, 0x10010e2e2..0x10010e984.
// Base layout: +0x10 controller* (counted), +0x18 u32 state id, +0x1c float, +0x20 bool, +0x24 u32 sound handle,
// +0x28 u32, +0x2c u32 (since version 3), +0x30 u64 runtime pointer (cleared by Read). Subclass fields start at +0x38.
// Quirks kept: Read accepts versions 0..3 only (a newer record answers false without reading anything); the controller read from a
// stream replaces the pointer without releasing the old one; PlayerDrive::Read refills the 13 floats at +0xa8 with -1.0;
// Plummet::Read clears the u64 at +0x50; Collide::Shutdown detaches and releases its object first.
#pragma once
#include <cstdint>

#include "gz/StreamOps.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "sc4/AutomataController.h"

namespace sc4 {

constexpr std::uint16_t kAutomataStateVersion = 3;
constexpr std::uint32_t kControllerFlagTransient = 0x8;   // controllers with this flag are saved as null
constexpr std::uint32_t kAutomatonIid = 0xa9b40f05;
constexpr std::uint32_t kAttachAnimIid = 0xa818edd8;

// The automaton as the states see it (vslot 0x1f0).
class IStateAutomaton {
public:
    virtual ~IStateAutomaton() = default;
    virtual void StopSound(std::uint32_t handle) = 0;   // vslot 0x1f0
};
// The object a Collide state holds (vslot 0x20 is called with `true` before it is released).
class IAutomataStateAttachment : public cIGZUnknown {
public:
    virtual void Slot3() {}
    virtual void Detach(bool final) = 0;   // vslot 0x20
};

class tAutomataState {
public:
    virtual ~tAutomataState() = default;
    virtual void Shutdown(IStateAutomaton* automaton);      // @ 0x10010e128 (vslot 0x00)
    virtual bool Write(cIGZOStream& stream) const;          // @ 0x10010e164 (vslot 0x08)
    virtual bool Read(cIGZIStream& stream);                 // @ 0x10010e214 (vslot 0x10)

    std::uint32_t stateType = 0;                    // +0x08: the behaviour state id the automaton dispatches on
    cSC4AutomataController* controller = nullptr;   // +0x10
    std::uint32_t stateId = 0;                      // +0x18: index of this record in the automaton's state stack
    float timer = 0.0f;                             // +0x1c
    bool flag = false;                              // +0x20
    std::uint32_t soundHandle = 0;                  // +0x24
    std::uint32_t value28 = 0;                      // +0x28
    std::uint32_t value2c = 0;                      // +0x2c
    std::uint64_t runtime30 = 0;                    // +0x30
};

class tAutomataStateRandomWalk : public tAutomataState {   // @ Write 0x10010e2e2, Read 0x10010e33a
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    cS3DVector3 direction;     // +0x38 the heading the walk steers toward
    float turnTimer = 0.0f;    // +0x44 seconds until the next turn
    float walkTimer = 0.0f;    // +0x48 seconds left to walk (>= 0) or, negative, to stand
    bool idleAnimStarted = false;   // +0x4c the stand's idle animation plays (not saved)
};
class tAutomataStateRiotLeader : public tAutomataState {   // @ Write 0x10010e390, Read 0x10010e3e6
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::int32_t value38 = 0, value3c = 0;   // +0x38, +0x3c
    std::uint8_t value40 = 0;                // +0x40
};
class tAutomataStatePlopJump : public tAutomataState {      // @ Write 0x10010e43e, Read 0x10010e48a
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    float value38 = 0.0f, value3c = 0.0f;
};
class tAutomataStatePlummet : public tAutomataState {       // @ Write 0x10010e4d4, Read 0x10010e54c
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    float value38 = 0, value3c = 0, value40 = 0, value44 = 0, value48 = 0;
    std::uint64_t runtime50 = 0;   // +0x50 (cleared by Read)
};
class tAutomataStatePullOver : public tAutomataState {      // @ Write 0x10010e5c2, Read 0x10010e5fe
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    float value38 = 0.0f;
    // Run-time fields the vehicle's DoPullOverTick uses (original +0x3c..+0x50; not serialized).
    cS3DVector3 orientation3c;   // +0x3c (unit heading)
    cS3DVector3 direction48;     // +0x48 (unit steering target, flipped when it points away from the occupant)
};
// The player-drive record is larger than its saved fields: Write stores only value38 and the 13-entry history, while
// the run-time tables after it are read by the player-drive code of the vehicle family and of cSC4Watercraft (the
// original offsets are in the comments; the port does not keep byte-compatible padding). cSC4Watercraft's
// HandleEnterState / TranslatePlayerDriveCommands are the users recovered so far.
class tAutomataStatePlayerDrive : public tAutomataState {   // @ Shutdown 0x10010e63c, Write 0x10010e678, Read 0x10010e6b4
public:
    void Shutdown(IStateAutomaton* automaton) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    float value38 = 0.0f;                  // +0x38
    float value3c = 0.0f;                  // +0x3c
    std::uint32_t value40 = 0;             // +0x40
    float groupSpeed = 1.0f;               // +0x44: the automata group's speed (HandleEnterState)
    float steering6c = 0.0f;               // +0x6c: the watercraft's rudder command
    float history[13];                     // +0xa8: refilled with -1.0f by Read
    float effectTimerC4 = 0.0f;            // +0xc4
    std::uint8_t hint117 = 0;              // +0x117
    std::uint8_t hint119 = 0;              // +0x119
    std::uint32_t value170 = 0;            // +0x170
    std::int32_t animStage1a4 = 0;         // +0x1a4
    std::uint32_t timeout1d8 = 0;          // +0x1d8 (bits of 2000.0f)
    std::uint32_t effectDelay20c = 0;      // +0x20c
    std::uint8_t wakeStarted22b = 0;       // +0x22b
    // The helicopter's spotlight effect state (cSC4Helicopter::TranslatePlayerDriveCommands / UpdateSpotlight).
    float effectParam240 = 0.0f;           // +0x240
    float effectParam244 = 0.0f;           // +0x244
    cS3DVector3 effectPosition248;         // +0x248
    std::uint8_t spotlightStarted255 = 0;  // +0x255
    float steerRate260 = 0.0f;             // +0x260
    std::int32_t cellX298 = 0;             // +0x298 (occupant manager slot 0x88 result, cSC4Helicopter::DoPlayerDriveTick)
    std::int32_t cellZ29c = 0;             // +0x29c
    float steerVisual264 = 0.0f;           // +0x264
    float wakeAngle268 = 0.0f;             // +0x268
    float wakeTurn26c = 6.2831855f;        // +0x26c
    float heading270 = 0.0f;               // +0x270
    // The vehicle player-drive run-time block (original +0x48..+0x70 and +0x254; not serialized, cSC4Watercraft never
    // reads it). The effect / particle / occupant / accident pointers the original also keeps in this record live on
    // cSC4Vehicle in the port (see Vehicle.h).
    cS3DVector3 steer48;                   // +0x48 (second copy of the steering direction)
    cS3DVector3 drivePos54;                // +0x54 (the driving position, integrated per sub-step)
    cS3DVector3 driveDir60;                // +0x60 (unit driving direction)
    std::int32_t damage70 = 0;             // +0x70 (accumulated damage; the params' +0x5c is the crash threshold)
    std::uint8_t onRoad254 = 0;            // +0x254 (1 = off the network, reported through callback event 10)
    tAutomataStatePlayerDrive() { for (float& v : history) v = -1.0f; }
};
class tAutomataStateBumped : public tAutomataState {        // @ Write 0x10010e70a, Read 0x10010e76a
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    cS3DVector3 v38, v44, v50, v5c;
};
class tAutomataStateCollide : public tAutomataState {       // @ Write 0x10010e7ca, Read 0x10010e7f6, Shutdown 0x10010e822
public:
    ~tAutomataStateCollide() override = default;
    void Shutdown(IStateAutomaton* automaton) override;
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    IAutomataStateAttachment* attachment = nullptr;   // +0x38
};
class tAutomataStateAttachAnim : public tAutomataState {    // @ Write 0x10010e884, Read 0x10010e8ec
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    cIGZUnknown* animation = nullptr;   // +0x38 (counted; the original keeps the 0xa818edd8 interface pointer)
    bool flag40 = false;                // +0x40
    std::uint32_t value44 = 0;          // +0x44 (saved as 16 bit)
};
class tAutomataStateChase : public tAutomataState {         // @ Write 0x10010e984, Read 0x10010e9be
public:
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    cIGZUnknown* target = nullptr;   // +0x38 (counted automaton)
};

}  // namespace sc4
