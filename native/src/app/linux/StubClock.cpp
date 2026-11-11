// The game's SDL clock and timer as the recovered code sees them: the test stub SDL.h (scripts/tests/stubs), whose timers never
// fire by themselves. The frame loop drives them through these two functions; this file is apart from game_main_linux.cpp
// because the stub's names clash with the real SDL3 the window uses.
#include <SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace linuxapp {

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// SDL_GetTicks / SDL_GetTicksNS for the stub SDL clock: time since start-up, never 0 (a cRZTimer started at tick 0 counts as
// stopped). The nanoseconds give the city view's per-frame scroll step its real frame time instead of whole milliseconds.
void UpdateStubTicks() {
    static const double start = NowMs();
    const double ms = NowMs() - start;
    g_ticks = 1u + static_cast<std::uint32_t>(static_cast<std::uint64_t>(ms));
    g_ticksNS = 1'000'000ull + static_cast<std::uint64_t>(ms * 1'000'000.0);
}

// Fires every armed timer whose interval has passed (a timer is armed from the first pump after it was added). The callback
// answers the next interval; 0 ends the timer. Callbacks may add or remove timers, so each one is looked up again by id.
void PumpTimers() {
    const double now = NowMs();
    UpdateStubTicks();
    std::vector<SDL_TimerID> due;
    for (StubSDLTimer& t : g_timers) {
        if (t.armedAt < 0) t.armedAt = now;
        else if (now - t.armedAt >= t.interval) due.push_back(t.id);
    }
    for (SDL_TimerID id : due) {
        StubSDLTimer timer{};
        bool live = false;
        for (const StubSDLTimer& t : g_timers) if (t.id == id) { timer = t; live = true; break; }
        if (!live) continue;   // removed by an earlier callback of this pump
        if (std::getenv("SC4_DEBUG")) std::printf("debug: timer %d fires\n", id);
        const std::uint32_t next = timer.callback(timer.interval, timer.param);
        for (auto it = g_timers.begin(); it != g_timers.end(); ++it) {
            if (it->id != id) continue;
            if (next == 0) {
                g_timers.erase(it);
                if (id == g_activeTimerId) { g_timerCallback = nullptr; g_activeTimerId = 0; }
            } else {
                it->interval = next;
                it->armedAt = NowMs();
            }
            break;
        }
    }
}

}  // namespace linuxapp
