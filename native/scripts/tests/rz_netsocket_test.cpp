#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <vector>

#include "platform/WinCompat.h"
#include "rz/NetSocket.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++failures; } } while (0)

namespace {
// A BSD loopback address (length byte and AF_INET, which cRZINetSocketAddress does not write on this platform).
cRZSocketAddress Loopback(std::uint16_t port) {
    sockaddr_in in{};
#ifdef __APPLE__   // the BSD length byte (Linux's sockaddr_in has none)
    in.sin_len = sizeof in;
#endif
    in.sin_family = AF_INET;
    in.sin_port = htons(port);
    in.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cRZSocketAddress address;
    std::memcpy(address.bytes, &in, 16);
    return address;
}
std::uint16_t PortOf(const cRZSocketAddress& address) {
    sockaddr_in in{};
    std::memcpy(&in, address.bytes, 16);
    return ntohs(in.sin_port);
}

struct Target final : cIGZMessageTarget {
    int refs = 1;
    std::vector<cGZMessage> got;
    bool QueryInterface(std::uint32_t, void**) override { return false; }
    std::uint32_t AddRef() override { return ++refs; }
    std::uint32_t Release() override { return --refs; }
    bool DoMessage(cGZMessage& message) override { got.push_back(message); return true; }
};

struct Host final : wincompat::IWinCompatHost {
    std::uint32_t buttons = 0, mods = 0;
    std::vector<std::string> events;
    std::uint32_t MouseButtons() override { return buttons; }
    std::uint32_t ModState() override { return mods; }
    void ClientRect(void* window, wincompat::RECT* out) override { events.push_back("rect"); *out = {1, 2, 3, window ? 4 : 5}; }
    void WarpCursor(double x, double y) override { events.push_back("warp " + std::to_string(int(x)) + "," + std::to_string(int(y))); }
    void Alert(const char* text) override { events.push_back(text); }
};
}  // namespace

int main() {
    // ---- the Winsock error numbers
    CHECK(cRZSocketError::GetErrorFromPlatformSpecificError(0x2714) == 2 && cRZSocketError::GetErrorFromPlatformSpecificError(0x2733) == 8);
    CHECK(cRZSocketError::GetErrorFromPlatformSpecificError(0x274d) == 0x22 && cRZSocketError::GetErrorFromPlatformSpecificError(0x2775) == 0x2d);
    CHECK(cRZSocketError::GetErrorFromPlatformSpecificError(0x2af9) == 0x31 && cRZSocketError::GetErrorFromPlatformSpecificError(0x2afc) == 0x34);
    CHECK(cRZSocketError::GetErrorFromPlatformSpecificError(0x2715) == 1 && cRZSocketError::GetErrorFromPlatformSpecificError(12345) == 1);
    cRZSocketError error;
    CHECK(error.GetErrorValue() == 1 && std::string(error.GetErrorStringCStr()) == "(\?\?\?): Unknown error or no error");
    error.Init(0x274d);
    CHECK(error.GetErrorValue() == 0x22 && error.platformError_ == 0x274d);
    CHECK(std::string(error.GetErrorStringCStr()) == "(WSAECONNREFUSED): The connection was refused");
    cRZString text;
    error.GetErrorString(text);
    CHECK(std::string(text.Data()) == "Winsock error 10061 (WSAECONNREFUSED): The connection was refused");
    error.Init(0x2af9);
    CHECK(std::string(error.GetErrorStringCStr()) == "(WSAHOST_NOT_FOUND): The host was not found");

    // ---- addresses
    cRZSocketAddress blank;
    CHECK(blank.bytes[0] == 0 && blank.bytes[1] == 0);
    cRZINetSocketAddress inet(0x64c3, 0x0100007f);
    std::uint16_t family, port;
    std::uint32_t ip;
    std::memcpy(&family, inet.bytes, 2); std::memcpy(&port, inet.bytes + 2, 2); std::memcpy(&ip, inet.bytes + 4, 4);
    CHECK(family == 2 && port == 0x64c3 && ip == 0x0100007f && inet.bytes[8] == 0 && inet.bytes[15] == 0);

    // ---- the compatibility shims
    {
        Host host;
        wincompat::SetHost(&host);
        std::int32_t counter = 5;
        CHECK(wincompat::InterlockedIncrement(&counter) == 6 && wincompat::InterlockedDecrement(&counter) == 5 && counter == 5);
        int point = 0, sign = 0;
        char* digits = wincompat::fcvt(-1.5, 3, &point, &sign);
        CHECK(std::string(digits) == "-1.500e+00" && point == 2 && sign == 1);
        digits = wincompat::fcvt(25.0, 0, &point, &sign);
        CHECK(std::string(digits) == "2e+01" && point == -1 && sign == 0);          // no decimal point: -1
        CHECK(wincompat::setenv("A", "B", 1) == 1);
        host.buttons = 1;
        CHECK(wincompat::GetKeyState(1) == -32768 && wincompat::GetAsyncKeyState(1) == -32768 && wincompat::GetKeyState(2) == 0);
        host.buttons = 4;
        CHECK(wincompat::GetKeyState(2) == -32768 && wincompat::GetAsyncKeyState(2) == -32768);
        host.buttons = 2;
        CHECK(wincompat::GetAsyncKeyState(4) == -32768 && wincompat::GetKeyState(4) == 0);        // synchronous middle needs middle + right
        host.buttons = 6;
        CHECK(wincompat::GetKeyState(4) == -32768);
        host.buttons = 0;
        host.mods = 0x1;
        CHECK(wincompat::GetKeyState(0x10) == -32768 && wincompat::GetAsyncKeyState(0x10) == -32768 && wincompat::GetKeyState(0x11) == 0);
        host.mods = 0x40;
        CHECK(wincompat::GetKeyState(0x11) == -32768 && wincompat::GetAsyncKeyState(0x11) == -32768);
        host.mods = 0x100;   // left Alt
        CHECK(wincompat::GetKeyState(0x12) == 0 && wincompat::GetAsyncKeyState(0x12) == -32768);
        host.mods = 0x400;   // Command counts as Alt for the synchronous query only
        CHECK(wincompat::GetKeyState(0x12) == -32768 && wincompat::GetAsyncKeyState(0x12) == 0);
        host.mods = 0x2000;  // caps lock: the toggle bit, not the pressed bit
        CHECK(wincompat::GetKeyState(0x14) == 1 && wincompat::GetAsyncKeyState(0x14) == 1 && wincompat::GetKeyState(0x90) == 0);
        host.mods = 0x1000;
        CHECK(wincompat::GetKeyState(0x90) == 1 && wincompat::GetKeyState(0x13) == 0 && wincompat::GetKeyState(0x41) == 0);
        wincompat::RECT rect{};
        wincompat::GetClientRect(reinterpret_cast<void*>(1), &rect);
        CHECK(rect.left == 1 && rect.bottom == 4);
        wincompat::GetWindowRect(nullptr, &rect);
        CHECK(rect.bottom == 5);
        CHECK(wincompat::ClientToScreen(nullptr, nullptr) == 1);
        host.events.clear();
        CHECK(wincompat::SetCursorPos(10, 20) == 1 && host.events.size() == 2 && host.events[0] == "warp 10,20" && host.events[1] == "SetCursorPos not implemented");
        errno = ENOENT;
        CHECK(wincompat::WSAGetLastError() == 10002);
        errno = EWOULDBLOCK;
        CHECK(wincompat::WSAGetLastError() == 0x2733);
        errno = 200;   // beyond the mapped range on every platform: passed through
        CHECK(wincompat::WSAGetLastError() == 200);
        wincompat::SetHost(nullptr);
        CHECK(wincompat::GetKeyState(1) == 0);
    }

    // ---- a stream socket pair over the loopback interface
    {
        cRZNetStreamSocket server;
        CHECK(!server.IsSocketCreated() && server.GetSocket() == -1 && server.GetConnectStatus() == 0 && server.GetLastError() == 1);
        server.SetID(7);
        CHECK(server.GetID() == 7);
        CHECK(server.CreateSocket() && server.IsSocketCreated() && server.GetSocket() >= 0);
        CHECK(server.SetReuseAddressOption(true));
        bool on = false;
        CHECK(server.GetReuseAddressOption(on) && on);
        CHECK(server.SetKeepAliveOption(true) && server.GetKeepAliveOption(on) && on);
        CHECK(server.SetOOBOption(true) && server.GetOOBOption(on) && on);
        CHECK(server.SetRouteOption(false) && server.GetRouteOption(on) && !on);              // inverted on the wire, restored here
        CHECK(server.SetRouteOption(true) && server.GetRouteOption(on) && on);
        CHECK(server.SetBroadcastOption(true) && server.GetBroadcastOption(on) && on);
        CHECK(server.SetDebugOption(false) && server.GetDebugOption(on) && !on);
        CHECK(server.SetSendDelayOption(false) && server.GetSendDelayOption(on) && !on);      // SO_DEBUG, as shipped
        CHECK(server.SetLingerOption(true, 3));
        std::uint32_t seconds = 0;
        CHECK(server.GetLingerOption(on, seconds) && on && seconds == 3);
        CHECK(server.SetReceiveBufferOption(65536) && server.SetSendBufferOption(65536));
        std::uint32_t size = 0;
        CHECK(server.GetReceiveBufferOption(size) && size >= 65536 && server.GetSendBufferOption(size) && size >= 65536);
        CHECK(!server.SetSendTimeout(5000));                                                  // a 4-byte value where the kernel wants a timeval
        CHECK(server.GetLastError() != 1);
        CHECK(server.BindSocket(cRZINetSocketAddress(0, htonl(INADDR_LOOPBACK))));              // the kernel rewrites the length byte and ignores the family on bind
        cRZSocketAddress mine;
        server.GetMyAddress(mine);
        const std::uint16_t port = PortOf(mine);
        CHECK(port != 0);
        CHECK(server.StartListen(5) && server.GetConnectStatus() == cRZNetStreamSocket::kListening);
        CHECK(!server.IsWaitingDataAvailable(0));
        CHECK(server.IsSocketConnected());

        cRZNetStreamSocket client;
        CHECK(client.CreateSocket());
        bool connected = false;
        CHECK(client.Connect(Loopback(port), connected, 1000) && connected && client.GetConnectStatus() == cRZNetStreamSocket::kConnected);
        CHECK(server.IsWaitingDataAvailable(2000));
        cRZNetStreamSocket accepted;
        bool done = false;
        CHECK(server.Accept(accepted, done) && done && accepted.GetConnectStatus() == cRZNetStreamSocket::kConnected && accepted.IsSocketCreated());
        cRZSocketAddress peer;
        std::int64_t acceptedSocket = accepted.GetSocket();
        CHECK(accepted.GetPeerAddress(peer, acceptedSocket) && PortOf(peer) != 0);

        // data both ways
        int count = 5;
        CHECK(client.Write("hello", count, 0) && count == 5);
        CHECK(accepted.IsWaitingDataAvailable(2000));
        std::uint32_t waiting = 0;
        CHECK(accepted.GetWaitingDataSize(waiting, 0) && waiting == 5);
        CHECK(accepted.GetWaitingDataSize(waiting, 1000) && waiting == 5);
        char buffer[16] = {};
        count = 3;
        CHECK(accepted.Peek(buffer, count) && count == 3 && std::string(buffer, 3) == "hel");
        count = 16;
        CHECK(accepted.Read(buffer, count) && count == 5 && std::string(buffer, 5) == "hello");
        count = 0;
        CHECK(!accepted.Read(buffer, count));                                                  // a zero-length read answers false
        CHECK(accepted.GetWaitingDataSize(waiting, 50) && waiting == 0);
        CHECK(accepted.IsWritable(1000));
        CHECK(accepted.IsReadableAndOrWritable(cRZNetSocket::kWaitWrite, 0) != cRZNetSocket::kWaitFailed);
        int none = 0;
        CHECK(accepted.Write("x", none, 0) && none == 0);                                       // nothing to send
        CHECK(accepted.IsSocketConnected());

        // out of band data and the shutdown / close handling
        count = 1;
        CHECK(client.WriteOOB("!", count, 0) && count == 1);
        count = 3;
        CHECK(accepted.IsWaitingDataAvailable(2000));
        accepted.ReadOOB(buffer, count);

        CHECK(client.ShutDownSocket(cRZNetSocket::kShutWrite) && client.GetConnectStatus() == cRZNetStreamSocket::kShutDown);
        count = 16;
        accepted.Read(buffer, count);                                                          // drains the urgent byte or the end of stream
        count = 16;
        accepted.IsWaitingDataAvailable(2000);
        bool read = accepted.Read(buffer, count);
        if (!read && count == 0 && accepted.Error().value_ == 0x1e) {
            CHECK(accepted.Error().platformError_ == 0x2749 && !accepted.IsSocketCreated() && accepted.GetConnectStatus() == 0);
        }
        CHECK(client.CloseSocket() && client.GetConnectStatus() == 0 && !client.IsSocketCreated());
        CHECK(server.StopListen() && !server.IsSocketCreated() && server.GetConnectStatus() == 0);
        CHECK(server.StopListen());                                                            // already stopped
        CHECK(server.CloseSocket());
        CHECK(!server.IsSocketConnected());
    }

    // ---- notifications
    {
        cRZNetSocket socket;
        Target target;
        CHECK(socket.SetAsyncNotificationTarget(&target) && socket.GetAsyncNotificationTarget() == &target && target.refs == 2);
        CHECK(!socket.StartAsyncCustomNotification(1) && !socket.StartAsyncStandardNotification() && !socket.StartAsyncAcceptNotification() && !socket.StopAsyncNotification());
        CHECK(socket.CreateSocket());
        const std::uint32_t handle = static_cast<std::uint32_t>(socket.GetSocket());
        CHECK(!socket.Notification(handle + 1, 1, 0) && target.got.empty());                  // another socket's event
        CHECK(socket.Notification(handle, 1, 0) && target.got.size() == 1 && target.got[0].type == 0xab2045c3 && target.got[0].param2 == 0);
        CHECK(target.got[0].param1 == reinterpret_cast<std::uint64_t>(&socket));
        CHECK(socket.Notification(handle, 2, 0) && target.got.back().type == 0xab2045c4);
        CHECK(socket.Notification(handle, 4, 0) && target.got.back().type == 0xab2045c6);
        CHECK(socket.Notification(handle, 8, 0) && target.got.back().type == 0xab2045ca);
        CHECK(socket.Notification(handle, 0x10, 0) && target.got.back().type == 0xab2045d2);
        CHECK(socket.Notification(handle, 0x20, 0) && target.got.back().type == 0xab2045e2);
        CHECK(socket.Notification(handle, 3, 0) && target.got.back().type == 0);              // a combination has no message of its own
        CHECK(socket.Notification(handle, 0x40, 0) && target.got.back().type == 0);
        CHECK(socket.Notification(handle, 2, 10054) && target.got.back().param2 == 1 && socket.GetLastError() == 0x1b);
        CHECK(socket.Error().platformError_ == 10054);
        socket.SetAsyncNotificationTarget(nullptr);
        CHECK(target.refs == 1);
        CHECK(socket.SetSaveSocketOnDelete(true));
        const int raw = static_cast<int>(socket.GetSocket());
        socket.SetSaveSocketOnDelete(false);
        (void)raw;

        // the stream socket's own state follows connect and close messages
        cRZNetStreamSocket stream;
        cGZMessage message;
        message.type = 0x10;
        message.param2 = 0;
        stream.DoMessage(message);
        CHECK(stream.GetConnectStatus() == 2);
        message.param2 = 1;
        stream.DoMessage(message);
        CHECK(stream.GetConnectStatus() == 0 && stream.Error().platformError_ == 1);
        stream.DoMessage(message);
        message.type = 0x20;
        stream.DoMessage(message);
        CHECK(stream.GetConnectStatus() == 0);
    }

    std::printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
