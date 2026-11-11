// Stub SDL.h used to build/test recovered units without SDL installed:
//   clang++ -std=c++20 -Isrc -Iscripts/tests/stubs <test.cpp> <sources...>
#pragma once
#include <cstdint>

inline std::uint32_t g_ticks = 0;
inline std::uint32_t SDL_GetTicks() { return g_ticks; }
// Nanoseconds for the timers that need finer steps than a millisecond (cRZTimer): the frame loop sets g_ticksNS; a test that
// only sets g_ticks gets its milliseconds.
inline std::uint64_t g_ticksNS = 0;
inline std::uint64_t SDL_GetTicksNS() { return g_ticksNS ? g_ticksNS : std::uint64_t(g_ticks) * 1'000'000ull; }
inline std::uint64_t SDL_GetPerformanceCounter() { return 12345; }
inline std::uint64_t SDL_GetPerformanceFrequency() { return 1'000'000'000ull; }

// Timers are never fired by themselves: tests invoke g_timerCallback (the most recently added timer) by hand; the native game's
// frame loop fires every armed timer in g_timers when its interval has passed (linuxapp::PumpTimers), on the loop's thread.
// A callback's return value is the next interval, 0 removes the timer (SDL's contract).
#include <vector>
using SDL_TimerID = int;
using SDL_TimerCallback = std::uint32_t (*)(std::uint32_t, void*);
struct StubSDLTimer { SDL_TimerID id; SDL_TimerCallback callback; void* param; std::uint32_t interval; double armedAt; };
inline std::vector<StubSDLTimer> g_timers;
inline SDL_TimerCallback g_timerCallback = nullptr;
inline void* g_timerParam = nullptr;
inline std::uint32_t g_timerInterval = 0;
inline SDL_TimerID g_nextTimerId = 1;
inline SDL_TimerID g_activeTimerId = 0;
inline bool g_timerCreationFails = false;
inline SDL_TimerID SDL_AddTimer(std::uint32_t interval, SDL_TimerCallback cb, void* param) {
    if (g_timerCreationFails) return 0;
    g_timerCallback = cb; g_timerParam = param; g_timerInterval = interval;
    g_activeTimerId = g_nextTimerId++;
    g_timers.push_back({g_activeTimerId, cb, param, interval, -1.0});
    return g_activeTimerId;
}
inline bool SDL_RemoveTimer(SDL_TimerID id) {
    bool found = false;
    for (auto it = g_timers.begin(); it != g_timers.end(); ++it) {
        if (it->id == id) { g_timers.erase(it); found = true; break; }
    }
    if (id == g_activeTimerId) { g_timerCallback = nullptr; g_activeTimerId = 0; found = true; }
    return found;
}
