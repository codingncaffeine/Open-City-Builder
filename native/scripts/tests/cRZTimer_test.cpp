#include "rz/cRZTimer.h"
#include <SDL.h>
#include <cassert>
#include <cstdio>
int main() {
    cRZTimer t(cRZTimer::kMilliseconds);
    assert(!t.IsTimerRunning());
    g_ticks = 1000; t.Start(); assert(t.IsTimerRunning());
    g_ticks = 4500;
    assert(t.GetElapsedTimeU32() == 3500);
    t.SetUnits(cRZTimer::kSeconds); assert(!t.IsTimerRunning() && t.GetElapsedTimeU32() == 0);
    g_ticks = 1000; t.Start(); g_ticks = 7000;
    assert(t.GetElapsedTimeU32() == 6);
    float f = -1; t.GetElapsedTimeFloat(f); assert(f == 6.0f);
    std::uint64_t u = 0; t.GetElapsedTimeU64(u); assert(u == 6);
    t.Stop(); assert(!t.IsTimerRunning()); g_ticks = 99999; assert(t.GetElapsedTimeU32() == 6);
    t.SetElapsedTimeFloat(120.0f); assert(t.GetElapsedTimeU32() == 120);          // seconds
    cRZTimer m(cRZTimer::kMinutes); m.SetElapsedTimeU64(std::uint64_t(2)); assert(m.GetElapsedTimeU32() == 2);
    cRZTimer us(cRZTimer::kMicroseconds); us.SetElapsedTimeU64(std::uint64_t(5000)); assert(us.GetElapsedTimeU32() == 5000);
    cRZTimer ns(cRZTimer::kNanoseconds); ns.SetElapsedTimeU64(std::uint64_t(3'000'000)); assert(ns.GetElapsedTimeU32() == 0 /*unsupported on read*/);
    // sub-millisecond steps reach a microsecond timer (the frame loop's nanosecond clock)
    cRZTimer f2(cRZTimer::kMicroseconds); g_ticksNS = 5'000'000'000ull; f2.Start(); g_ticksNS += 6'944'444ull;
    assert(f2.GetElapsedTimeU32() == 6944);
    cRZTimer m2(cRZTimer::kMilliseconds); m2.Start(); g_ticksNS += 2'500'000ull; assert(m2.GetElapsedTimeU32() == 2);
    // a clock that steps back reads as no time, not as a huge interval
    cRZTimer b(cRZTimer::kMilliseconds); b.Start(); g_ticksNS -= 1'000'000ull; assert(b.GetElapsedTimeU32() == 0);
    g_ticksNS = 0;
    std::puts("cRZTimer ok");
}
