// Crash reports: a ring of the last things the game did ("breadcrumbs": input, tool changes, tutorial steps) and, in the
// Linux game, a fatal-signal handler (app/linux/CrashLog.cpp) that writes them with the signal, the fault address and a
// backtrace to ~/.local/state/open-city-builder/crash-<time>-<pid>.log (and to stderr) before the default action (the core
// dump) runs.
//
// Header-only on purpose: any code (including the unit-tested classes) can leave a breadcrumb without a link dependency; the
// ring is one object for the whole program (an inline function's static).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>

namespace platform {

constexpr unsigned kBreadcrumbCount = 64;
constexpr unsigned kBreadcrumbLength = 160;

struct BreadcrumbRing {
    char lines[kBreadcrumbCount][kBreadcrumbLength] = {};
    std::atomic<unsigned> next{0};
};

inline BreadcrumbRing& Breadcrumbs() {
    static BreadcrumbRing ring;
    return ring;
}

inline std::chrono::steady_clock::time_point BreadcrumbEpoch() {
    static const auto start = std::chrono::steady_clock::now();
    return start;
}

// One line, printf-style, stamped with the seconds since the first breadcrumb. Cheap enough for every click and key.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((format(printf, 1, 2)))
#endif
inline void Breadcrumb(const char* format, ...) {
    BreadcrumbRing& ring = Breadcrumbs();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - BreadcrumbEpoch()).count();
    char* line = ring.lines[ring.next.fetch_add(1, std::memory_order_relaxed) % kBreadcrumbCount];
    const int stamp = std::snprintf(line, kBreadcrumbLength, "%9.3f  ", seconds);
    if (stamp < 0 || static_cast<unsigned>(stamp) >= kBreadcrumbLength) return;
    va_list args;
    va_start(args, format);
    std::vsnprintf(line + stamp, kBreadcrumbLength - static_cast<unsigned>(stamp), format, args);
    va_end(args);
}

// Installs the handler (Linux game only; app/linux/CrashLog.cpp). Call once, early in main.
void InstallCrashHandler();

}  // namespace platform
