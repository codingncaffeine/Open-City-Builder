// cRZTimer — Maxis/EA RZ framework stopwatch, recovered from Sim City 4 Deluxe Editionsub 1.2.1.
// Object layout (0x18 bytes, no vtable): +0x00 units, +0x08 startTick, +0x10 accumulated.
// Internally everything is stored in microseconds (SDL_GetTicksNS); Units only selects how
// values are scaled on the way in/out. (The Mac build kept SDL_GetTicks milliseconds, so a
// microsecond timer advanced in steps of 1000: the city view's scroll moved by whole-millisecond
// frame times, unevenly at high refresh rates. The Windows build timed with the performance counter.)
#pragma once
#include <cstdint>

class cRZTimer {
public:
    // Values recovered from the scaling switch in GetElapsedTime/SetElapsedTime.
    enum Units : std::uint32_t {
        kNanoseconds  = 2,  // write-only in the original (SetElapsedTime(u64) only)
        kMicroseconds = 3,
        kMilliseconds = 4,
        kSeconds      = 5,
        kMinutes      = 6,
    };

    explicit cRZTimer(Units units) noexcept : units_(units) {}
    ~cRZTimer() = default;

    Units GetUnits() const noexcept { return units_; }
    // Changing units also clears the timer, as in the original.
    void SetUnits(Units units) noexcept { units_ = units; Reset(); }

    // Clears accumulated time and stops the timer.
    void Reset() noexcept { startTick_ = 0; accumulated_ = 0; }
    bool IsTimerRunning() const noexcept { return startTick_ != 0; }

    // Returns elapsed time truncated to 32 bits; 0 for unsupported units.
    std::uint32_t GetElapsedTimeU32() const noexcept;
    void GetElapsedTimeU64(std::uint64_t& out) const noexcept;  // leaves `out` untouched for unsupported units
    void GetElapsedTimeFloat(float& out) const noexcept;          // 0.0f for unsupported units

    void SetElapsedTimeFloat(float value) noexcept;
    void SetElapsedTimeU64(std::uint64_t value) noexcept;

    void Restart() noexcept;  // zero accumulated, start now
    void Start() noexcept;    // start now without clearing accumulated
    void Stop() noexcept;     // fold running time into accumulated

    // Blocks until at least `duration` (in this timer's units) has elapsed.
    void Delay(std::uint32_t duration) const noexcept;

    static void GetTimerCycle(std::uint64_t* cycle) noexcept;
    static double GetSecondsToTimerCyclesRatio() noexcept;
    static double GetMillisecondsToCPUCyclesRatio() noexcept;

private:
    // Microseconds elapsed including the currently running interval.
    std::uint64_t ElapsedMicroseconds() const noexcept;

    Units units_;
    std::uint64_t startTick_ = 0;     // clock (microseconds) at Start(); 0 = stopped
    std::uint64_t accumulated_ = 0;   // microseconds banked by Stop()
};

static_assert(sizeof(cRZTimer) == 0x18);
