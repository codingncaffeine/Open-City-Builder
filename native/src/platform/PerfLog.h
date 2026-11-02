// SC4_PERFLOG=<file>: a timing log for chasing stalls on a player's machine. The frame loop writes one line per frame; anything
// else can add a note with PerfNote (stamped with the same clock). Off (one cached getenv) when the variable is not set.
#pragma once
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace platform {

inline std::FILE* PerfLogFile() {
    static std::FILE* file = [] {
        const char* path = std::getenv("SC4_PERFLOG");
        std::FILE* f = path && *path ? std::fopen(path, "w") : nullptr;
        if (f) std::setvbuf(f, nullptr, _IOLBF, 1 << 16);
        return f;
    }();
    return file;
}

inline bool PerfLogOn() { return PerfLogFile() != nullptr; }

// Milliseconds since the first call.
inline double PerfNowMs() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double, std::milli>(steady_clock::now() - start).count();
}

inline void PerfNote(const char* format, ...) {
    std::FILE* f = PerfLogFile();
    if (!f) return;
    std::fprintf(f, "%10.1f  ", PerfNowMs());
    va_list args;
    va_start(args, format);
    std::vfprintf(f, format, args);
    va_end(args);
    std::fputc('\n', f);
}

}  // namespace platform
