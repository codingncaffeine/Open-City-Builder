// cSC4PropOccupant — a prop (small decoration: street lights, signs, chimney smoke, ...) placed in the city.
// 76 functions at 0x10015e4b4..0x100163330. Class id 0x2977aa47, version 6 record:
//   occupant data, u32 prop type (stored through a global type table), bbox, s8 orientation, u8 state,
//   u8 timeOfDay start (x10), u8 end, bool hasDateRange {u32 period, u32 span, date, date},
//   u8 random chance, u8 usage, u32 lot tag, u8 timer bitfield (version >= 6).
#pragma once
#include <cstdint>
#include <memory>

#include "gz/cGZDate.h"
#include "sc4/OccupantManager.h"
#include "sc4/PropOccupantInterface.h"

namespace sc4 {

constexpr std::uint32_t kGZCLSID_cSC4PropOccupant = 0x2977aa47;
constexpr std::uint32_t kGZIID_cISC4PropOccupant = 0xe9793a65;
constexpr std::uint32_t kOccupantTypeProp = 0xa823821e;
constexpr std::uint32_t kMessagePropStateChanged = 0xe9c86ddb;

// the prop type table: ids are interned into a process-wide list and stored as 16-bit indices
std::uint16_t GetPropTypeIndex(std::uint32_t propType);
std::uint32_t GetPropTypeFromIndex(std::uint16_t index);

enum PropTimerMask : std::uint8_t { kPropTimerTimeOfDay = 1, kPropTimerDate = 2 };   // bits 0..3 must all be set to show

// services a prop uses
class IPropHost {
public:
    virtual ~IPropHost() = default;
    virtual int ClockHour() = 0;                    // sp24HourClock vslot 0x50
    virtual int ClockMinute() = 0;                  // vslot 0x68
    virtual void SimulatorDate(cGZDate& out) = 0;   // spSimulator vslot 0x30
    virtual std::uint32_t RandomPercent() = 0;      // uniform 0..99
    virtual cISC4OccupantManager* OccupantManager() = 0;
    virtual float AnimationTimeDilation() { return 1.0f; }   // spSimulator vslot 0x80 (GetAnimationTimeDilation)
};
IPropHost* PropHost();
void SetPropHost(IPropHost* host);

// The original's views: the prop interface (cISC4PropOccupant, iid 0xe9793a65), cIGZMessageTarget2 at +8 (the animation
// coordinator registers a prop with the animation tick manager through it), the occupant base. The occupant methods the
// prop interface repeats (Init .. SetLotTag) are one override each, shared by both tables as in the original.
class cSC4PropOccupant : public cSC4Occupant, public cIGZMessageTarget2, public cISC4PropOccupant {
public:
    cSC4PropOccupant() { type_ = kOccupantTypeProp; }

    bool QueryInterface(std::uint32_t iid, void** out) override;
    std::uint32_t AddRef() override { return cSC4Occupant::AddRef(); }
    std::uint32_t Release() override { return cSC4Occupant::Release(); }
    // i386 0x001057e0: on the animation tick (0x86ad10ee) every animated model of the placeable object advances by the
    // tick's milliseconds (data3, float bits) x the simulator's animation time dilation.
    bool DoMessage(cIGZMessage2* message) override;

    // cSC4Occupant overrides
    bool Init() override;
    bool Shutdown() override;
    void GetPosition(float* xyz) override;
    void SetPosition(const float* xyz) override;
    void GetBoundingBox(float* minXyz, float* maxXyz) override;
    bool GetLotTag(std::uint32_t& tag) const override;
    void SetLotTag(std::uint32_t tag) override { lotTag_ = tag; }
    bool Write(cIGZOStream& stream) const override;
    bool Read(cIGZIStream& stream) override;
    std::uint32_t GetGZCLSID() const override { return kGZCLSID_cSC4PropOccupant; }

    // cISC4PropOccupant
    cISC4Occupant* AsOccupant() override { return static_cast<cISC4Occupant*>(this); }
    void SetBoundingBox(const float* min, const float* max) override;
    // i386 0x0065e524 returns 0 and 0x00102764 does nothing: a prop keeps no placement properties.
    void* GetPropPlacementProperties() override { return nullptr; }
    void SetPropPlacementProperties(void*) override {}
    std::uint32_t GetPropType() const override { return GetPropTypeFromIndex(propTypeIndex_); }
    void SetPropType(std::uint32_t type) override { propTypeIndex_ = GetPropTypeIndex(type); }
    std::int32_t GetOrientation() const override { return orientation_; }
    bool SetOrientation(std::int32_t orientation) override;
    void SetState(std::uint32_t state) override;
    // Raw state byte: no change message and no model notification. cSC4PropOccupantVisualEffect::SetState
    // (@ 0x100161482) writes prop +0x9b directly and restarts its effect instead of going through SetState.
    void SetStateRaw(std::uint8_t state) { state_ = state; }
    std::uint32_t GetState() const override { return state_; }
    void SetTimeOfDayRange(float start, float end) override;
    void GetTimeOfDayRange(float& start, float& end) const override;
    void SetSimulatorDateRange(std::uint32_t period, std::uint32_t span, std::uint8_t month, std::uint8_t day) override;
    void TickSimulatorDate(const cIGZDate& today) override;
    void SetRandomChance(std::uint8_t percent) override { randomChance_ = percent; }
    std::uint8_t GetRandomChance() const override { return randomChance_; }
    std::uint8_t GetPropTimerBitfield() const override { return timerBits_; }
    bool GetPropTimerState(std::uint32_t mask) const override { return (timerBits_ & mask) != 0; }
    void SetPropTimerState(std::uint32_t mask, bool on) override;
    std::uint8_t GetPropUsage() const override { return usage_; }
    void SetPropUsage(std::uint8_t usage) override { usage_ = usage; }
    bool PlaceInOccupantManager(bool place);
    bool PlaceInRenderer(bool place);
    // true when a date range is attached (the range's own state lives in the record)
    bool HasDateRange() const { return dateRange_ != nullptr; }

private:
    struct DateRange {
        std::uint32_t period = 0;        // days between appearances (365 = every year on the same date)
        std::uint32_t span = 1;          // days the prop stays visible
        cGZDate showDate;                // +0x08 next date it appears
        cGZDate hideDate;                // +0x28 next date it disappears (showDate + span)
    };
    float min_[3] = {}, max_[3] = {};       // +0x70
    std::unique_ptr<DateRange> dateRange_;  // +0x88
    std::uint32_t lotTag_ = 0xffffffff;     // +0x90
    std::uint16_t propTypeIndex_ = 0;       // +0x94
    std::uint8_t timeStart_ = 0, timeEnd_ = 0;   // +0x96/+0x97 (tenths of an hour)
    std::uint8_t timerBits_ = 0;            // +0x98
    std::int8_t orientation_ = 0;           // +0x99
    std::uint8_t randomChance_ = 100;       // +0x9a
    std::uint8_t state_ = 0;                // +0x9b
    std::uint8_t usage_ = 0;                // +0x9c
};

}  // namespace sc4
