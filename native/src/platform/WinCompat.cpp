#include "platform/WinCompat.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

namespace wincompat {

namespace {
IWinCompatHost* g_host = nullptr;
constexpr int kVkLButton = 1, kVkRButton = 2, kVkMButton = 4, kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12, kVkPause = 0x13,
              kVkCapital = 0x14, kVkNumLock = 0x90;
constexpr std::uint32_t kButtonLeft = 1, kButtonMiddle = 2, kButtonRight = 4;
constexpr std::uint32_t kModShift = 0x3, kModCtrl = 0xc0, kModAlt = 0x300, kModGui = 0xc00;
}  // namespace

IWinCompatHost* Host() { return g_host; }
void SetHost(IWinCompatHost* host) { g_host = host; }

std::int32_t InterlockedIncrement(volatile std::int32_t* value) { return __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST); }   // @ 0x10064214c
std::int32_t InterlockedDecrement(volatile std::int32_t* value) { return __atomic_sub_fetch(value, 1, __ATOMIC_SEQ_CST); }   // @ 0x100642156

char* fcvt(double value, int digits, int* decimalPoint, int* sign) {   // @ 0x100642162
    static char buffer[512];
    std::snprintf(buffer, sizeof buffer, "%.*e", digits, value);
    *sign = value < 0.0;
    const char* dot = std::strchr(buffer, '.');
    *decimalPoint = dot ? static_cast<int>(dot - buffer) : -1;
    return buffer;
}

int setenv(const char*, const char*, int) { return 1; }   // @ 0x1006421cd

namespace {
// Shared by GetKeyState / GetAsyncKeyState; `altMask` is the one place they differ (the synchronous one answers to Command).
int KeyState(int vk, std::uint32_t altMask, bool async) {
    IWinCompatHost* host = Host();
    if (!host) return 0;
    std::uint16_t state = 0;
    if (vk < 0x10) {
        switch (vk) {
            case kVkLButton: state = static_cast<std::uint16_t>((host->MouseButtons() & kButtonLeft) << 15); break;
            case kVkRButton: state = static_cast<std::uint16_t>((host->MouseButtons() & kButtonRight) << 13); break;
            case kVkMButton:
                if (async) state = static_cast<std::uint16_t>((host->MouseButtons() & kButtonMiddle) << 14);
                else state = static_cast<std::uint16_t>((host->MouseButtons() & (kButtonMiddle | kButtonRight)) == (kButtonMiddle | kButtonRight)) << 15;
                break;
            default: break;
        }
        return static_cast<std::int16_t>(state);
    }
    switch (vk) {
        case kVkShift: state = static_cast<std::uint16_t>((host->ModState() & kModShift) != 0) << 15; break;
        case kVkControl: state = static_cast<std::uint16_t>((host->ModState() & kModCtrl) != 0) << 15; break;
        case kVkMenu: state = static_cast<std::uint16_t>((host->ModState() & altMask) != 0) << 15; break;
        case kVkPause: break;
        case kVkCapital: state = static_cast<std::uint16_t>((host->ModState() >> 13) & 1); break;
        case kVkNumLock: state = static_cast<std::uint16_t>((host->ModState() >> 12) & 1); break;
        default: break;
    }
    return static_cast<std::int16_t>(state);
}
}  // namespace

int GetKeyState(int virtualKey) { return KeyState(virtualKey, kModGui, false); }       // @ 0x10064225b
int GetAsyncKeyState(int virtualKey) { return KeyState(virtualKey, kModAlt, true); }    // @ 0x10064232c

void GetClientRect(void* window, RECT* out) { if (IWinCompatHost* host = Host()) host->ClientRect(window, out); }   // @ 0x1006423f3
void GetWindowRect(void* window, RECT* out) { GetClientRect(window, out); }                                         // @ 0x1006423fd
int ClientToScreen(void*, void*) { return 1; }                                                                      // @ 0x100642407

int SetCursorPos(short x, short y) {   // @ 0x100642414
    if (IWinCompatHost* host = Host()) {
        host->WarpCursor(static_cast<double>(x), static_cast<double>(y));
        host->Alert("SetCursorPos not implemented");
    }
    return 1;
}

int closesocket(int socket) { return ::close(socket); }                                                   // @ 0x100642440
int ioctlsocket(int socket, unsigned long request, void* argument) { return ::ioctl(socket, request, argument); }   // @ 0x10064244a

namespace {
// Winsock codes are 10000 + the BSD errno; the original adds that base to the Darwin errno. Linux numbers the socket errors
// differently, so they are first turned into their BSD numbers (macOS: unchanged).
int BsdErrno(int error) {
#ifdef __APPLE__
    return error;
#else
    switch (error) {
        case EWOULDBLOCK: return 35;     case EINPROGRESS: return 36;     case EALREADY: return 37;
        case ENOTSOCK: return 38;        case EDESTADDRREQ: return 39;    case EMSGSIZE: return 40;
        case EPROTOTYPE: return 41;      case ENOPROTOOPT: return 42;     case EPROTONOSUPPORT: return 43;
        case ESOCKTNOSUPPORT: return 44; case EOPNOTSUPP: return 45;      case EPFNOSUPPORT: return 46;
        case EAFNOSUPPORT: return 47;    case EADDRINUSE: return 48;      case EADDRNOTAVAIL: return 49;
        case ENETDOWN: return 50;        case ENETUNREACH: return 51;     case ENETRESET: return 52;
        case ECONNABORTED: return 53;    case ECONNRESET: return 54;      case ENOBUFS: return 55;
        case EISCONN: return 56;         case ENOTCONN: return 57;        case ESHUTDOWN: return 58;
        case ETOOMANYREFS: return 59;    case ETIMEDOUT: return 60;       case ECONNREFUSED: return 61;
        case ELOOP: return 62;           case ENAMETOOLONG: return 63;    case EHOSTDOWN: return 64;
        case EHOSTUNREACH: return 65;
        default: return error;
    }
#endif
}
}  // namespace

int WSAGetLastError() {   // @ 0x100642456
    const int error = BsdErrno(errno);
    return (error >= 1 && error <= 0x57) ? error + kWSABase : error;
}

}  // namespace wincompat
