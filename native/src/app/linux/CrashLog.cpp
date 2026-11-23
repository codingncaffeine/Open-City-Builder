// The fatal-signal handler of the Linux game (platform/CrashLog.h). Everything the handler does after a crash is
// async-signal-safe: the report path, the executable's load address and the version line are prepared at install time,
// the text is written with write(2), and numbers are formatted by hand. glibc's backtrace() is loaded once at install
// (its first call can allocate); on a C library without <execinfo.h> (musl) the report has no backtrace, the rest stays.
#include "platform/CrashLog.h"

#include <dlfcn.h>
#include <link.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define OCB_HAVE_BACKTRACE 1
#endif

namespace platform {
namespace {

constexpr int kKeepReports = 20;          // older crash reports are removed at start
constexpr std::size_t kAltStackSize = 64 * 1024;

char gReportPath[1024] = {};
char gExePath[1024] = {};
std::uintptr_t gExeBase = 0;
std::uintptr_t gExeEnd = 0;   // the end of the executable's highest loaded segment
const int kSignals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};

void Put(int fd, const char* text) {
    std::size_t left = std::strlen(text);
    while (left > 0) {
        const ssize_t n = write(fd, text, left);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return;
        }
        text += n;
        left -= static_cast<std::size_t>(n);
    }
}

// Writes `value` in hex (0x...) into `out` (at least 19 bytes).
void Hex(char* out, std::uintptr_t value) {
    char digits[16];
    int count = 0;
    do {
        digits[count++] = "0123456789abcdef"[value & 0xf];
        value >>= 4;
    } while (value && count < 16);
    *out++ = '0';
    *out++ = 'x';
    while (count) *out++ = digits[--count];
    *out = '\0';
}

void Decimal(char* out, long value) {
    char digits[24];
    int count = 0;
    const bool negative = value < 0;
    unsigned long v = negative ? static_cast<unsigned long>(-value) : static_cast<unsigned long>(value);
    do {
        digits[count++] = static_cast<char>('0' + v % 10);
        v /= 10;
    } while (v);
    if (negative) *out++ = '-';
    while (count) *out++ = digits[--count];
    *out = '\0';
}

const char* SignalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (invalid memory access)";
        case SIGBUS: return "SIGBUS (bus error)";
        case SIGFPE: return "SIGFPE (arithmetic error)";
        case SIGILL: return "SIGILL (illegal instruction)";
        case SIGABRT: return "SIGABRT (abort)";
        default: return "signal";
    }
}

void WriteReport(int fd, int sig, const siginfo_t* info) {
    char number[32];
    Put(fd, "==== Open City Builder crashed ====\n");
    Put(fd, "signal: ");
    Put(fd, SignalName(sig));
    Put(fd, " (");
    Decimal(number, sig);
    Put(fd, number);
    Put(fd, ")\nfault address: ");
    Hex(number, info ? reinterpret_cast<std::uintptr_t>(info->si_addr) : 0);
    Put(fd, number);
    Put(fd, "\nexecutable: ");
    Put(fd, gExePath);
    Put(fd, "\nload address: ");
    Hex(number, gExeBase);
    Put(fd, number);
    Put(fd, "\n\nbacktrace (offsets: addr2line -f -C -i -e <executable> <offset>):\n");
#ifdef OCB_HAVE_BACKTRACE
    void* frames[64];
    const int count = backtrace(frames, 64);
    for (int i = 0; i < count; ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(frames[i]);
        Put(fd, "  #");
        Decimal(number, i);
        Put(fd, number);
        Put(fd, "  ");
        Hex(number, address);
        Put(fd, number);
        if (gExeBase && address >= gExeBase && address < gExeEnd) {
            Put(fd, "  offset ");
            Hex(number, address - gExeBase);
            Put(fd, number);
        }
        Put(fd, "\n");
    }
    Put(fd, "symbols:\n");
    backtrace_symbols_fd(frames, count, fd);
#else
    Put(fd, "  (no backtrace on this C library; the core dump has it: coredumpctl debug)\n");
#endif
    Put(fd, "\nlast actions (oldest first):\n");
    BreadcrumbRing& ring = Breadcrumbs();
    const unsigned next = ring.next.load(std::memory_order_relaxed);
    const unsigned first = next > kBreadcrumbCount ? next - kBreadcrumbCount : 0;
    for (unsigned i = first; i < next; ++i) {
        const char* line = ring.lines[i % kBreadcrumbCount];
        if (!line[0]) continue;
        Put(fd, "  ");
        Put(fd, line);
        Put(fd, "\n");
    }
    Put(fd, "==== end of crash report ====\n");
}

void Handler(int sig, siginfo_t* info, void*) {
    const int fd = gReportPath[0] ? open(gReportPath, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644) : -1;
    if (fd >= 0) {
        WriteReport(fd, sig, info);
        close(fd);
    }
    WriteReport(STDERR_FILENO, sig, info);
    if (fd >= 0) {
        Put(STDERR_FILENO, "crash report written to ");
        Put(STDERR_FILENO, gReportPath);
        Put(STDERR_FILENO, "\n");
    }
    // SA_RESETHAND put the default action back: raising again ends the process with the core dump as before.
    raise(sig);
}

std::string StateDirectory() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && state[0] == '/') return std::string(state) + "/open-city-builder";
    if (const char* home = std::getenv("HOME"); home && home[0]) return std::string(home) + "/.local/state/open-city-builder";
    return {};
}

// Keeps the newest kKeepReports - 1 reports so this run's report makes kKeepReports.
void PruneOldReports(const std::string& directory) {
    std::error_code error;
    std::vector<std::filesystem::path> reports;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("crash-", 0) == 0 && entry.path().extension() == ".log") reports.push_back(entry.path());
    }
    std::sort(reports.begin(), reports.end());   // crash-YYYYMMDD-HHMMSS-...: name order is time order
    while (reports.size() >= static_cast<std::size_t>(kKeepReports)) {
        std::filesystem::remove(reports.front(), error);
        reports.erase(reports.begin());
    }
}

}  // namespace

void InstallCrashHandler() {
    BreadcrumbEpoch();
    const std::string directory = StateDirectory();
    if (!directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (!error) {
            PruneOldReports(directory);
            char stamp[32];
            const std::time_t now = std::time(nullptr);
            std::tm local{};
            localtime_r(&now, &local);
            std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
            std::snprintf(gReportPath, sizeof gReportPath, "%s/crash-%s-%ld.log", directory.c_str(), stamp, static_cast<long>(getpid()));
        }
    }
    Dl_info self{};
    if (dladdr(reinterpret_cast<void*>(&InstallCrashHandler), &self)) {
        gExeBase = reinterpret_cast<std::uintptr_t>(self.dli_fbase);
        if (self.dli_fname) std::snprintf(gExePath, sizeof gExePath, "%s", self.dli_fname);
    }
    dl_iterate_phdr(
        [](dl_phdr_info* info, std::size_t, void*) -> int {
            if (info->dlpi_name && info->dlpi_name[0]) return 0;   // the main program has the empty name
            for (int i = 0; i < info->dlpi_phnum; ++i)
                if (info->dlpi_phdr[i].p_type == PT_LOAD)
                    gExeEnd = std::max<std::uintptr_t>(gExeEnd, info->dlpi_addr + info->dlpi_phdr[i].p_vaddr + info->dlpi_phdr[i].p_memsz);
            return 1;
        },
        nullptr);
    const ssize_t length = readlink("/proc/self/exe", gExePath, sizeof gExePath - 1);
    if (length > 0) gExePath[length] = '\0';
#ifdef OCB_HAVE_BACKTRACE
    void* warm[2];
    backtrace(warm, 2);   // loads the unwinder now, outside any signal
#endif
    static std::vector<char> altStack(kAltStackSize);
    stack_t stack{};
    stack.ss_sp = altStack.data();
    stack.ss_size = altStack.size();
    sigaltstack(&stack, nullptr);   // a stack overflow still gets its report
    struct sigaction action {};
    action.sa_sigaction = Handler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int sig : kSignals) sigaction(sig, &action, nullptr);
}

}  // namespace platform
