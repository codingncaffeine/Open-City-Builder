// cSC424HourClock — the game's 24-hour clock service (service id 0xa7e7f92a, priority -2000000,
// class id 0xa7e7f929). Recovered from Sim City 4 Deluxe Editionsub 1.2.1 (vtable 0x100a7a5d0,
// 42 functions).
//
// Time is kept as seconds since midnight (0..86399). While neither pause counter is raised,
// OnIdle converts the elapsed real microseconds into game seconds with the time magnification
// (default 100x), clamps one step to 1200 game seconds, wraps at one day and notifies the world
// when the hour (0x44778230) or the 5-minute slot (0x44778231) changes. The "visual" time follows
// the real time unless the visual mode pins it to noon (1) or midnight (2).
//
// Services the original reaches through globals are injected through IClockHost: the message
// server (SL::spMS2), the resource manager tunables (exemplar 0xe7e2c2db:6534284a:e9590e52) and
// the cheat-code manager. Night is the span from `nightStartHour` (default 20) for
// (24 - start + end) % 24 hours, i.e. 20:00-04:00 by default.
//
// Deviation: stream version 1 data is read like version 2 (as the original does); the original's
// no-op legacy timer conversion is dropped.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include "gz/cIGZIStream.h"
#include "gz/cIGZOStream.h"
#include "rz/cRZTimer.h"

namespace sc4 {

// Message ids posted by the clock. The host posts them with the clock as void1 and object and the two values in data2 and data3
// (i386 PostMessageServerMessage 0x00122002); "data1/data2" below name the two values in that order.
enum ClockMessage : std::uint32_t {
    kMsgHourChanged = 0x44778230,        // data1 = hour
    kMsgMinuteChanged = 0x44778231,      // data1 = hour, data2 = minute (5-minute steps)
    kMsgVisualHourChanged = 0x44778232,  // data1 = visual hour
    kMsgVisualMinuteChanged = 0x44778233,
    kMsgDayNightChanged = 0x44778234,    // data1 = 1 for night, 0 for day
    kMsgPauseChanged = 0x44778235,       // data1 = 1 when paused
    kMsgHiddenPauseChanged = 0x44778236,
};

constexpr std::uint32_t kClockServiceId = 0xa7e7f92a;
constexpr std::int32_t kClockServicePriority = -2000000;
constexpr std::uint32_t kClockClassId = 0xa7e7f929;
constexpr std::uint32_t kGZIID_cISC424HourClock = 0x681bd53a;
constexpr std::uint32_t kClockCheatSetTime = 0x281f7858;     // "<cheat> <hour>"
constexpr std::uint32_t kClockCheatControl = 0xc9b73fbb;     // "<cheat> pause | resume | magnify <n>"

class IClockHost {
public:
    virtual ~IClockHost() = default;
    virtual void PostMessage(std::uint32_t message, std::uint32_t data1, std::uint32_t data2) = 0;
    // Simulator tunables; false when the exemplar is missing. Values left untouched on absent properties.
    struct Tunables {
        float timeMagnification;
        std::uint32_t nightStartHour, nightEndHour;
    };
    virtual bool ReadTunables(Tunables& inOut) { (void)inOut; return false; }
};

class cSC424HourClock {
public:
    enum VisualTimeMode : std::int32_t { kVisualReal = 0, kVisualNoon = 1, kVisualMidnight = 2 };

    explicit cSC424HourClock(IClockHost* host = nullptr);

    bool Init();        // starts the timer, reads tunables
    bool Shutdown();

    float GetTimePercentage() const { return static_cast<float>(timeSeconds_) / 86400.0f; }
    void SetTimePercentage(float fraction);
    float GetVisualTimePercentage() const { return static_cast<float>(visualSeconds_) / 86400.0f; }
    std::uint32_t GetHour() const { return timeSeconds_ / 3600; }
    std::uint32_t GetVisualHour() const { return visualSeconds_ / 3600; }
    void SetHour(std::uint32_t hour);
    std::uint32_t GetMinute() const { return (timeSeconds_ - GetHour() * 3600) / 60; }
    std::uint32_t GetVisualMinute() const { return (visualSeconds_ - GetVisualHour() * 3600) / 60; }
    void SetMinute(std::uint32_t minute);
    std::uint32_t GetSeconds() const { return timeSeconds_ - GetHour() * 3600 - GetMinute() * 60; }
    std::uint32_t GetVisualSeconds() const { return visualSeconds_ - GetVisualHour() * 3600 - GetVisualMinute() * 60; }
    void SetSeconds(std::uint32_t seconds);
    std::uint32_t GetEventHour(int which) const { return which == 0 ? nightStart_ : which == 1 ? nightEnd_ : 0; }

    void GetTimeMagnification(float& value) const { value = magnification_; }
    bool SetTimeMagnification(float value);
    void GetTimeMagnificationMinMax(float& lo, float& hi) const { lo = magMin_; hi = magMax_; }
    bool SetTimeMagnificationMinMax(float lo, float hi) { magMin_ = lo; magMax_ = lo <= hi ? hi : lo; return true; }

    void Pause(bool pause);
    bool IsPaused() const { return pauseCount_ > 0; }
    void HiddenPause(bool pause);
    bool IsHiddenPaused() const { return hiddenPauseCount_ > 0; }
    bool IsNight() const;
    void EnableDayNightTransitionNotifications(bool enable);
    void SetVisualTimeMode(VisualTimeMode mode);
    VisualTimeMode GetVisualTimeMode() const { return visualMode_; }

    bool OnIdle(std::uint32_t elapsedTicks);
    // Handles a cheat-code message: `cheat` is kClockCheatSetTime / kClockCheatControl and
    // `commandLine` the text typed after the cheat string (first token is the cheat itself).
    bool DoCheat(std::uint32_t cheat, const std::string& commandLine);

    bool Write(cIGZOStream& stream) const;
    bool Read(cIGZIStream& stream);
    std::uint32_t GetGZCLSID() const { return kClockClassId; }

    // Test hook: replaces the real-time source (microseconds since the last restart).
    void SetElapsedSource(std::function<std::uint32_t()> source) { elapsedSource_ = std::move(source); }

private:
    void ChangeVisualTime(std::uint32_t seconds);
    void Post(std::uint32_t message, std::uint32_t d1 = 0, std::uint32_t d2 = 0) {
        if (host_) host_->PostMessage(message, d1, d2);
    }
    void ReadSimulatorTunableValues();
    std::uint32_t Elapsed() const { return elapsedSource_ ? elapsedSource_() : timer_.GetElapsedTimeU32(); }
    void RestartTimer() { timer_.Restart(); }

    IClockHost* host_;
    bool initialized_ = false;
    std::int32_t pauseCount_ = 0, hiddenPauseCount_ = 0;
    float magnification_ = 100.0f, magMin_ = 0.05f, magMax_ = 2000.0f;
    std::uint32_t timeSeconds_ = 0xa8c0;     // noon
    std::uint32_t visualSeconds_ = 0xa8c0;
    cRZTimer timer_{cRZTimer::kMicroseconds};
    std::uint32_t nightStart_ = 20, nightEnd_ = 4;
    bool dayNightNotifications_ = true;
    VisualTimeMode visualMode_ = kVisualReal;
    std::function<std::uint32_t()> elapsedSource_;
};

}  // namespace sc4
