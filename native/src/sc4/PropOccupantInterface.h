// cISC4PropOccupant — the prop occupant interface (iid 0xe9793a65). Implemented by cSC4PropOccupant.
#pragma once
#include <cstdint>

#include "gz/cGZDate.h"
#include "gz/cIGZIStream.h"
#include "gz/cIGZMessage2.h"
#include "gz/cIGZOStream.h"
#include "sc4/Occupant.h"

namespace sc4 {

// ---------------------------------------------------------------------------------------------------------
// The prop occupant interface (cISC4PropOccupant). Slot order from the original's primary vtable
// 0x100a76950. cSC4PropOccupant (PropOccupant.h) derives from it, so QueryInterface(0xe9793a65) on any prop
// returns this view of the same object. Offsets are vtable byte offsets.
// ---------------------------------------------------------------------------------------------------------
class cISC4PropOccupant : public cIGZUnknown {
public:
    virtual cISC4Occupant* AsOccupant() = 0;                                      // [3]  0x18
    virtual void SetBoundingBox(const float* minXyz, const float* maxXyz) = 0;    // [4]  0x20
    virtual void* GetPropPlacementProperties() = 0;                               // [5]  0x28
    virtual void SetPropPlacementProperties(void* properties) = 0;                // [6]  0x30
    virtual std::uint32_t GetPropType() const = 0;                                // [7]  0x38
    virtual void SetPropType(std::uint32_t type) = 0;                             // [8]  0x40
    virtual std::int32_t GetOrientation() const = 0;                              // [9]  0x48
    virtual bool SetOrientation(std::int32_t orientation) = 0;                    // [10] 0x50
    virtual void SetState(std::uint32_t state) = 0;                               // [11] 0x58
    virtual std::uint32_t GetState() const = 0;                                   // [12] 0x60
    virtual void SetTimeOfDayRange(float start, float end) = 0;                   // [13] 0x68
    virtual void GetTimeOfDayRange(float& start, float& end) const = 0;           // [14] 0x70
    virtual void SetSimulatorDateRange(std::uint32_t propType, std::uint32_t span, std::uint8_t month,
                                       std::uint8_t day) = 0;                     // [15] 0x78
    virtual void TickSimulatorDate(const cIGZDate& today) = 0;                    // [16] 0x80
    virtual void SetRandomChance(std::uint8_t percent) = 0;                       // [17] 0x88
    virtual std::uint8_t GetRandomChance() const = 0;                             // [18] 0x90
    virtual std::uint8_t GetPropTimerBitfield() const = 0;                        // [19] 0x98
    virtual bool GetPropTimerState(std::uint32_t mask) const = 0;                 // [20] 0xa0
    virtual void SetPropTimerState(std::uint32_t mask, bool on) = 0;              // [21] 0xa8
    virtual std::uint8_t GetPropUsage() const = 0;                                // [22] 0xb0
    virtual void SetPropUsage(std::uint8_t usage) = 0;                            // [23] 0xb8
    virtual bool Init() = 0;                                                      // [24] 0xc0
    virtual bool Shutdown() = 0;                                                  // [25] 0xc8
    virtual bool Write(cIGZOStream& stream) const = 0;                            // [26] 0xd0
    virtual bool Read(cIGZIStream& stream) = 0;                                   // [27] 0xd8
    virtual std::uint32_t GetGZCLSID() const = 0;                                 // [28] 0xe0
    virtual bool DoMessage(cIGZMessage2* message) = 0;                            // [29] 0xe8
    virtual void GetPosition(float* xyz) = 0;                                     // [30] 0xf0
    virtual void SetPosition(const float* xyz) = 0;                               // [31] 0xf8
    virtual void GetBoundingBox(float* minXyz, float* maxXyz) = 0;                // [32] 0x100
    virtual bool GetLotTag(std::uint32_t& tag) const = 0;                         // [33] 0x108
    virtual void SetLotTag(std::uint32_t tag) = 0;                                // [34] 0x110
    virtual ~cISC4PropOccupant() = default;                                       // [35]/[36]
};

}  // namespace sc4
