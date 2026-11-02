#include "rz/cRZTimer.h"

#include "platform/MacTools.h"

#include <SDL.h>

namespace {

constexpr float kUsPerMillisecondF = 1000.0f;
constexpr float kUsPerSecondF = 1000000.0f;
constexpr float kUsPerMinuteF = 60000000.0f;

// The clock in microseconds, never 0 (0 marks a stopped timer).
std::uint64_t NowMicroseconds() noexcept {
    const std::uint64_t us = SDL_GetTicksNS() / 1000;
    return us ? us : 1;
}

}  // namespace

// Shared by all three GetElapsedTime overloads (each repeats this inline in the original).
std::uint64_t cRZTimer::ElapsedMicroseconds() const noexcept {
    if (startTick_ == 0) {
        return accumulated_;
    }
    const std::uint64_t now = NowMicroseconds();
    return accumulated_ + (now > startTick_ ? now - startTick_ : 0);
}

// cRZTimer::GetElapsedTime() const @ 0x1007b6d54
std::uint32_t cRZTimer::GetElapsedTimeU32() const noexcept {
    const std::uint64_t us = ElapsedMicroseconds();
    switch (units_) {
        case kMicroseconds: return static_cast<std::uint32_t>(us);
        case kMilliseconds: return static_cast<std::uint32_t>(us / 1000);
        case kSeconds:      return static_cast<std::uint32_t>(us / 1000000);
        case kMinutes:      return static_cast<std::uint32_t>(us / 60000000);
        default:            return 0;
    }
}

// cRZTimer::GetElapsedTime(unsigned long long&) const @ 0x1007b6df0
void cRZTimer::GetElapsedTimeU64(std::uint64_t& out) const noexcept {
    const std::uint64_t us = ElapsedMicroseconds();
    switch (units_) {
        case kMicroseconds: out = us; break;
        case kMilliseconds: out = us / 1000; break;
        case kSeconds:      out = us / 1000000; break;
        case kMinutes:      out = us / 60000000; break;
        default: break;
    }
}

// cRZTimer::GetElapsedTime(float&) const @ 0x1007b6e94
void cRZTimer::GetElapsedTimeFloat(float& out) const noexcept {
    const auto us = static_cast<float>(ElapsedMicroseconds());
    switch (units_) {
        case kMicroseconds: out = us; break;
        case kMilliseconds: out = us / kUsPerMillisecondF; break;
        case kSeconds:      out = us / kUsPerSecondF; break;
        case kMinutes:      out = us / kUsPerMinuteF; break;
        default:            out = 0.0f; break;
    }
}

// cRZTimer::SetElapsedTime(float) @ 0x1007b6fa4
void cRZTimer::SetElapsedTimeFloat(float value) noexcept {
    switch (units_) {
        case kMicroseconds: break;
        case kMilliseconds: value *= kUsPerMillisecondF; break;
        case kSeconds:      value *= kUsPerSecondF; break;
        case kMinutes:      value *= kUsPerMinuteF; break;
        default: return;  // original leaves the timer untouched
    }
    // The original's float->u64 conversion handles values >= 2^63; negative input is UB there too.
    accumulated_ = static_cast<std::uint64_t>(value);
}

// cRZTimer::SetElapsedTime(unsigned long long) @ 0x1007b7024
void cRZTimer::SetElapsedTimeU64(std::uint64_t value) noexcept {
    switch (units_) {
        case kNanoseconds:  value /= 1000; break;
        case kMicroseconds: break;
        case kMilliseconds: value *= 1000; break;
        case kSeconds:      value *= 1000000; break;
        case kMinutes:      value *= 60000000; break;
        default: return;
    }
    accumulated_ = value;
}

// cRZTimer::Restart() @ 0x1007b70a4
void cRZTimer::Restart() noexcept {
    startTick_ = NowMicroseconds();
    accumulated_ = 0;
}

// cRZTimer::Start() @ 0x1007b70c8
void cRZTimer::Start() noexcept {
    startTick_ = NowMicroseconds();
}

// cRZTimer::Stop() @ 0x1007b70e4
void cRZTimer::Stop() noexcept {
    if (startTick_ != 0) {
        const std::uint64_t now = NowMicroseconds();
        accumulated_ += now > startTick_ ? now - startTick_ : 0;
        startTick_ = 0;
    }
}

// cRZTimer::Delay(unsigned int) @ 0x1007b7112
// Original polls a stack-local timer and sleeps duration/1000 ms between polls.
void cRZTimer::Delay(std::uint32_t duration) const noexcept {
    cRZTimer local(units_);
    local.Start();
    while (local.GetElapsedTimeU32() < duration) {
        MacTools::Sleep(duration / 1000);
    }
}

// cRZTimer::GetTimerCycle(unsigned long long*) @ 0x1007b717a
void cRZTimer::GetTimerCycle(std::uint64_t* cycle) noexcept {
    *cycle = SDL_GetPerformanceCounter();
}

// cRZTimer::GetSecondsToTimerCyclesRatio() @ 0x1007b7192
double cRZTimer::GetSecondsToTimerCyclesRatio() noexcept {
    return 1.0 / static_cast<double>(SDL_GetPerformanceFrequency());
}

// cRZTimer::GetMillisecondsToCPUCyclesRatio() @ 0x1007b71c2
double cRZTimer::GetMillisecondsToCPUCyclesRatio() noexcept {
    return 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
}
