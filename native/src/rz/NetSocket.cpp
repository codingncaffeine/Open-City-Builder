#include "rz/NetSocket.h"

#include <cstring>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "platform/WinCompat.h"

#include "rz/SocketErrorTable.inc"

namespace {
// The platform's own socket constants: the original's numbers (SOL_SOCKET 0xffff, SO_REUSEADDR 4, FIONREAD 0x4004667f...) are
// the BSD ones and equal these on macOS; Linux numbers them differently.
constexpr int kSolSocket = SOL_SOCKET;
constexpr int kSoDebug = SO_DEBUG, kSoReuseAddr = SO_REUSEADDR, kSoKeepAlive = SO_KEEPALIVE, kSoDontRoute = SO_DONTROUTE,
              kSoBroadcast = SO_BROADCAST, kSoLinger = SO_LINGER, kSoOobInline = SO_OOBINLINE, kSoSndBuf = SO_SNDBUF,
              kSoRcvBuf = SO_RCVBUF, kSoSndTimeo = SO_SNDTIMEO, kSoRcvTimeo = SO_RCVTIMEO;
constexpr unsigned long kFionRead = FIONREAD;
constexpr int kWsaEWouldBlock = 0x2733, kWsaENotConn = 0x2749;
constexpr std::uint32_t kEventRead = 0xab2045c3, kEventWrite = 0xab2045c4, kEventOob = 0xab2045c6, kEventAccept = 0xab2045ca,
                        kEventConnect = 0xab2045d2, kEventClose = 0xab2045e2;
constexpr std::uint32_t kInfinite = 0xffffffffu;

const SocketErrorEntry* FindError(int platformError) {
    for (const SocketErrorEntry& entry : kSocketErrors) {
        if (entry.platform == platformError) return &entry;
    }
    return nullptr;
}

// Waits on one descriptor; `read` / `write` select the sets. Returns select's result.
int Wait(std::int64_t socket, bool read, bool write, std::uint32_t timeoutMs) {
    fd_set readSet, writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    timeval timeout{};
    timeval* pointer = nullptr;
    if (timeoutMs != kInfinite) {
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = static_cast<int>((timeoutMs % 1000) * 1000);
        pointer = &timeout;
    }
    if (read) FD_SET(static_cast<int>(socket), &readSet);
    if (write) FD_SET(static_cast<int>(socket), &writeSet);
    return ::select(static_cast<int>(socket) + 1, read ? &readSet : nullptr, write ? &writeSet : nullptr, nullptr, pointer);
}
}  // namespace

// ---------------------------------------------------------------------------------------------------- addresses

cRZINetSocketAddress::cRZINetSocketAddress(std::uint16_t port, std::uint32_t address) {   // @ 0x10064eb2a
    const std::uint16_t family = 2;
    std::memcpy(bytes + 0, &family, 2);
    std::memcpy(bytes + 2, &port, 2);
    std::memcpy(bytes + 4, &address, 4);
    std::memset(bytes + 8, 0, 8);
}

// ---------------------------------------------------------------------------------------------------- errors

int cRZSocketError::GetErrorFromPlatformSpecificError(int platformError) {   // @ 0x10079ce26
    const SocketErrorEntry* entry = FindError(platformError);
    return entry ? entry->value : 1;
}

void cRZSocketError::Init(int platformError) {   // @ 0x10079d1c4
    value_ = GetErrorFromPlatformSpecificError(platformError);
    platformError_ = platformError;
}

const char* cRZSocketError::GetErrorStringCStr() const {   // @ 0x10079d1e6
    const SocketErrorEntry* entry = FindError(platformError_);
    return entry ? entry->message : "(\?\?\?): Unknown error or no error";
}

void cRZSocketError::GetErrorString(cRZString& out) const {   // @ 0x10079d5f8
    out.Sprintf("Winsock error %d %s", platformError_, GetErrorStringCStr());
}

// ---------------------------------------------------------------------------------------------------- cRZNetSocket

cRZNetSocket::cRZNetSocket() = default;   // @ 0x10079b690

cRZNetSocket::~cRZNetSocket() {   // @ 0x10079b704
    if (socket_ != -1 && !saveOnDelete_) cRZNetSocket::CloseSocket();
    ShutdownWindow();
    if (target_) {
        target_->Release();
        target_ = nullptr;
    }
}

int cRZNetSocket::WSAError() { return wincompat::WSAGetLastError(); }

void cRZNetSocket::RecordError(int platformError) {
    error_.value_ = cRZSocketError::GetErrorFromPlatformSpecificError(platformError);
    error_.platformError_ = platformError;
}

bool cRZNetSocket::Notification(std::uint32_t socket, int event, int error) {   // @ 0x10079b78e
    if (error != 0) RecordError(error);
    cGZMessage message;
    message.param1 = reinterpret_cast<std::uint64_t>(this);
    message.param2 = error != 0;
    if (static_cast<std::uint64_t>(socket_) != socket) return false;
    switch (event) {
        case 1: message.type = kEventRead; break;
        case 2: message.type = kEventWrite; break;
        case 4: message.type = kEventOob; break;
        case 8: message.type = kEventAccept; break;
        case 0x10: message.type = kEventConnect; break;
        case 0x20: message.type = kEventClose; break;
        default: break;   // 3, 5, 6, 7 and anything else: the message keeps type 0
    }
    DoMessage(message);
    if (target_) target_->DoMessage(message);
    return true;
}

bool cRZNetSocket::SetupWindow() { return true; }      // @ 0x10079b886
bool cRZNetSocket::ShutdownWindow() { return true; }   // @ 0x10079b88e
bool cRZNetSocket::DoMessage(cGZMessage&) { return true; }   // @ 0x10079bf5a

bool cRZNetSocket::SetSaveSocketOnDelete(bool keep) { saveOnDelete_ = keep; return keep; }   // @ 0x10079b898 (returns its argument)
cIGZMessageTarget* cRZNetSocket::GetAsyncNotificationTarget() { return target_; }               // @ 0x10079b8a4

bool cRZNetSocket::SetAsyncNotificationTarget(cIGZMessageTarget* target) {   // @ 0x10079b8ae
    if (target) target->AddRef();
    if (target_) target_->Release();
    target_ = target;
    return true;
}

bool cRZNetSocket::StartAsyncStandardNotification() { return StartAsyncCustomNotification(0x37); }   // @ 0x10079b8e4
bool cRZNetSocket::StartAsyncAcceptNotification() { return StartAsyncCustomNotification(8); }         // @ 0x10079b8fa
bool cRZNetSocket::StartAsyncCustomNotification(int) { return false; }                                // @ 0x10079b910
bool cRZNetSocket::StopAsyncNotification() { return StartAsyncCustomNotification(0); }                // @ 0x10079b918

bool cRZNetSocket::SetSocketStyle(int domain, int type, int protocol) {   // @ 0x10079b92c
    domain_ = domain;
    type_ = type;
    protocol_ = protocol;
    return true;
}

bool cRZNetSocket::CreateSocket() {   // @ 0x10079b93e
    const int created = ::socket(domain_, type_, protocol_);
    socket_ = created;
    if (created == -1) return Fail();
    return true;
}

bool cRZNetSocket::IsSocketCreated() const { return socket_ != -1; }   // @ 0x10079b982

bool cRZNetSocket::CloseSocket() {   // @ 0x10079b990
    bool ok = true;
    if (socket_ != -1) {
        const int result = wincompat::closesocket(static_cast<int>(socket_));
        socket_ = -1;
        if (result != 0) {
            Fail();
            ok = false;
        }
    }
    bound_ = false;
    return ok;
}

bool cRZNetSocket::ShutDownSocket(int mode) {   // @ 0x10079b9de
    if (socket_ != -1 && ::shutdown(static_cast<int>(socket_), mode) != 0) return Fail();
    return true;
}

bool cRZNetSocket::BindSocket(const cRZSocketAddress& address) {   // @ 0x10079ba20
    myAddress_ = address;
    if (::bind(static_cast<int>(socket_), reinterpret_cast<const sockaddr*>(&myAddress_), 16) == 0) {
        bound_ = true;
        return true;
    }
    bound_ = false;
    return Fail();
}

bool cRZNetSocket::BindSocket() { return BindSocket(myAddress_); }   // @ 0x10079ba7c

bool cRZNetSocket::GetSocketAddress(cRZSocketAddress& out, std::int64_t& socket) {   // @ 0x10079ba8e
    socklen_t length = 16;
    if (::getsockname(static_cast<int>(socket), reinterpret_cast<sockaddr*>(&out), &length) == 0) return true;
    return Fail();
}

void cRZNetSocket::GetMyAddress(cRZSocketAddress& out) { GetSocketAddress(out, socket_); }   // @ 0x10079bada

bool cRZNetSocket::GetPeerAddress(cRZSocketAddress& out, std::int64_t& socket) {   // @ 0x10079baec
    socklen_t length = 16;
    if (::getpeername(static_cast<int>(socket), reinterpret_cast<sockaddr*>(&out), &length) == 0) return true;
    return Fail();
}

void cRZNetSocket::GetMyPeerAddress(cRZSocketAddress& out) { GetPeerAddress(out, socket_); }   // @ 0x10079bb38

bool cRZNetSocket::SetMyAddress(const cRZSocketAddress& address) { myAddress_ = address; return true; }          // @ 0x10079bb4a
bool cRZNetSocket::SetPeerSocketAddress(const cRZSocketAddress& address) { peerAddress_ = address; return true; }   // @ 0x10079bb62

void cRZNetSocket::SetID(int id) { id_ = id; }               // @ 0x10079d638
int cRZNetSocket::GetID() { return id_; }                    // @ 0x10079d642
int cRZNetSocket::GetLastError() { return error_.value_; }   // @ 0x10079d64c
std::int64_t cRZNetSocket::GetSocket() const { return socket_; }   // @ 0x10079d62e

bool cRZNetSocket::GetWaitingDataSize(std::uint32_t& size, std::uint32_t timeoutMs) {   // @ 0x10079bb7a
    if (timeoutMs != 0) {
        const int ready = Wait(socket_, true, false, timeoutMs);
        if (ready == 0) {
            size = 0;
            return true;
        }
        if (ready == -1) return Fail();
    }
    if (wincompat::ioctlsocket(static_cast<int>(socket_), kFionRead, &size) == 0) return true;
    size = 0;
    return Fail();
}

bool cRZNetSocket::IsWaitingDataAvailable(std::uint32_t timeoutMs) {   // @ 0x10079bc88
    const int ready = Wait(socket_, true, false, timeoutMs);
    if (ready == 0) return false;
    if (ready != -1) return true;
    Fail();
    socket_ = -1;
    return false;
}

bool cRZNetSocket::IsWritable(std::uint32_t timeoutMs) {   // @ 0x10079bd50
    const int ready = Wait(socket_, false, true, timeoutMs);
    if (ready == 0) return false;
    if (ready != -1) return true;
    Fail();
    return false;
}

std::uint32_t cRZNetSocket::IsReadableAndOrWritable(std::uint32_t waitMode, std::uint32_t timeoutMs) {   // @ 0x10079be10
    fd_set readSet, writeSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    timeval timeout{};
    timeval* pointer = nullptr;
    if (timeoutMs != kInfinite) {
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = static_cast<int>((timeoutMs % 1000) * 1000);
        pointer = &timeout;
    }
    const int fd = static_cast<int>(socket_);
    if (waitMode & kWaitRead) FD_SET(fd, &readSet);
    if (waitMode & kWaitWrite) FD_SET(fd, &writeSet);
    // Like the original this passes nfds = 0 and looks at the sets afterwards.
    if (::select(0, (waitMode & kWaitRead) ? &readSet : nullptr, (waitMode & kWaitWrite) ? &writeSet : nullptr, nullptr, pointer) == -1) return kWaitFailed;
    const bool readable = FD_ISSET(fd, &readSet);
    const bool writable = FD_ISSET(fd, &writeSet);
    return writable ? readable + 2u : readable;
}

bool cRZNetSocket::SetIntOption(int level, int option, int value) {
    if (::setsockopt(static_cast<int>(socket_), level, option, &value, 4) == 0) return true;
    return Fail();
}

bool cRZNetSocket::GetIntOption(int level, int option, int& value) {
    socklen_t length = 4;
    if (::getsockopt(static_cast<int>(socket_), level, option, &value, &length) == 0) return true;
    return false;
}

bool cRZNetSocket::SetBroadcastOption(bool on) { return SetIntOption(kSolSocket, kSoBroadcast, on); }   // @ 0x10079bf62
bool cRZNetSocket::SetDebugOption(bool on) { return SetIntOption(kSolSocket, kSoDebug, on); }           // @ 0x10079bfbc

bool cRZNetSocket::SetLingerOption(bool on, std::uint32_t seconds) {   // @ 0x10079c016
    std::uint32_t option[2] = {on, seconds & 0xffff};
    if (::setsockopt(static_cast<int>(socket_), kSolSocket, kSoLinger, option, 8) == 0) return true;
    return Fail();
}

bool cRZNetSocket::SetRouteOption(bool on) { return SetIntOption(kSolSocket, kSoDontRoute, !on); }     // @ 0x10079c076
bool cRZNetSocket::SetKeepAliveOption(bool on) { return SetIntOption(kSolSocket, kSoKeepAlive, on); }  // @ 0x10079c0d4
bool cRZNetSocket::SetOOBOption(bool on) { return SetIntOption(kSolSocket, kSoOobInline, on); }        // @ 0x10079c12e
bool cRZNetSocket::SetReceiveBufferOption(std::uint32_t size) { return SetIntOption(kSolSocket, kSoRcvBuf, static_cast<int>(size)); }   // @ 0x10079c188
bool cRZNetSocket::SetSendBufferOption(std::uint32_t size) { return SetIntOption(kSolSocket, kSoSndBuf, static_cast<int>(size)); }      // @ 0x10079c1de
bool cRZNetSocket::SetReuseAddressOption(bool on) { return SetIntOption(kSolSocket, kSoReuseAddr, on); }   // @ 0x10079c234
bool cRZNetSocket::SetSendDelayOption(bool on) { return SetIntOption(kSolSocket, kSoDebug, on); }          // @ 0x10079c28e (SO_DEBUG, as shipped)
bool cRZNetSocket::SetSendTimeout(std::uint32_t ms) { return SetIntOption(kSolSocket, kSoSndTimeo, static_cast<int>(ms)); }   // @ 0x10079c2e8
bool cRZNetSocket::SetReceiveTimeout(std::uint32_t ms) { return SetIntOption(kSolSocket, kSoRcvTimeo, static_cast<int>(ms)); }   // @ 0x10079c33e

namespace {
bool Truth(int value, bool& out) { out = value != 0; return true; }
}  // namespace

bool cRZNetSocket::GetBroadcastOption(bool& on) { int v; return GetIntOption(kSolSocket, kSoBroadcast, v) && Truth(v, on); }   // @ 0x10079c394
bool cRZNetSocket::GetDebugOption(bool& on) { int v; return GetIntOption(kSolSocket, kSoDebug, v) && Truth(v, on); }           // @ 0x10079c3f8

bool cRZNetSocket::GetLingerOption(bool& on, std::uint32_t& seconds) {   // @ 0x10079c45c
    std::uint32_t option[2] = {};
    socklen_t length = 8;
    if (::getsockopt(static_cast<int>(socket_), kSolSocket, kSoLinger, option, &length) != 0) return false;
    on = option[0] != 0;
    seconds = option[1];
    return true;
}

bool cRZNetSocket::GetRouteOption(bool& on) { int v; if (!GetIntOption(kSolSocket, kSoDontRoute, v)) return false; on = v == 0; return true; }   // @ 0x10079c4ce
bool cRZNetSocket::GetKeepAliveOption(bool& on) { int v; return GetIntOption(kSolSocket, kSoKeepAlive, v) && Truth(v, on); }   // @ 0x10079c532
bool cRZNetSocket::GetOOBOption(bool& on) { int v; return GetIntOption(kSolSocket, kSoOobInline, v) && Truth(v, on); }          // @ 0x10079c596

bool cRZNetSocket::GetReceiveBufferOption(std::uint32_t& size) {   // @ 0x10079c5fa
    int v = 0;
    if (!GetIntOption(kSolSocket, kSoRcvBuf, v)) return false;
    size = static_cast<std::uint32_t>(v);
    return true;
}
bool cRZNetSocket::GetSendBufferOption(std::uint32_t& size) {   // @ 0x10079c656
    int v = 0;
    if (!GetIntOption(kSolSocket, kSoSndBuf, v)) return false;
    size = static_cast<std::uint32_t>(v);
    return true;
}
bool cRZNetSocket::GetReuseAddressOption(bool& on) { int v; return GetIntOption(kSolSocket, kSoReuseAddr, v) && Truth(v, on); }   // @ 0x10079c6b2
bool cRZNetSocket::GetSendDelayOption(bool& on) { int v; return GetIntOption(kSolSocket, kSoDebug, v) && Truth(v, on); }          // @ 0x10079c716
bool cRZNetSocket::GetSendTimeout(std::uint32_t& ms) {   // @ 0x10079c77a
    int v = 0;
    if (!GetIntOption(kSolSocket, kSoSndTimeo, v)) return false;
    ms = static_cast<std::uint32_t>(v);
    return true;
}
bool cRZNetSocket::GetReceiveTimeout(std::uint32_t& ms) {   // @ 0x10079c7d6
    int v = 0;
    if (!GetIntOption(kSolSocket, kSoRcvTimeo, v)) return false;
    ms = static_cast<std::uint32_t>(v);
    return true;
}

// ---------------------------------------------------------------------------------------------------- cRZNetStreamSocket

cRZNetStreamSocket::cRZNetStreamSocket() { SetSocketStyle(2, 1, 0); }   // @ 0x10079c834

cRZNetStreamSocket::~cRZNetStreamSocket() = default;   // @ 0x10079d662

bool cRZNetStreamSocket::ReceiveWith(void* buffer, int& size, int flags) {
    if (size == 0) return true;
    const ssize_t received = ::recv(static_cast<int>(socket_), buffer, static_cast<std::size_t>(static_cast<std::int64_t>(size)), flags);
    if (received == 0) {   // the peer closed the connection
        size = 0;
        error_.value_ = 0x1e;
        error_.platformError_ = kWsaENotConn;
        CloseSocket();
        return false;
    }
    if (received == -1) {
        const int error = WSAError();
        size = 0;
        RecordError(error);
        return static_cast<unsigned>(error - kWsaEWouldBlock) < 2;
    }
    size = static_cast<int>(received);
    return true;
}

bool cRZNetStreamSocket::Read(void* buffer, int& size) {   // @ 0x10079c86c
    if (size <= 0) return false;
    const ssize_t received = ::recv(static_cast<int>(socket_), buffer, static_cast<std::size_t>(size), 0);
    if (received == 0) {
        size = 0;
        error_.value_ = 0x1e;
        error_.platformError_ = kWsaENotConn;
        CloseSocket();
        return false;
    }
    if (received == -1) {
        const int error = WSAError();
        size = 0;
        RecordError(error);
        return static_cast<unsigned>(error - kWsaEWouldBlock) < 2;
    }
    size = received < 0 ? 0 : static_cast<int>(received);
    return true;
}

bool cRZNetStreamSocket::Peek(void* buffer, int& size) { return ReceiveWith(buffer, size, MSG_PEEK); }   // @ 0x10079c8fe
bool cRZNetStreamSocket::ReadOOB(void* buffer, int& size) { return ReceiveWith(buffer, size, MSG_OOB); }   // @ 0x10079c9f4

bool cRZNetStreamSocket::Write(const void* buffer, int& size, int flags) {   // @ 0x10079c990
    if (size == 0) return true;
    const ssize_t sent = ::send(static_cast<int>(socket_), buffer, static_cast<std::size_t>(static_cast<std::int64_t>(size)), flags);
    if (sent == -1) {
        const int error = WSAError();
        size = 0;
        RecordError(error);
        return static_cast<unsigned>(error - kWsaEWouldBlock) < 2;
    }
    size = static_cast<int>(sent);
    return true;
}

bool cRZNetStreamSocket::WriteOOB(const void* buffer, int& size, int flags) { return Write(buffer, size, flags | MSG_OOB); }   // @ 0x10079ca86

bool cRZNetStreamSocket::IsSocketConnected() {   // @ 0x10079caec
    if (socket_ == -1) return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<int>(socket_), &set);
    timeval none{};   // the original leaves the timeout argument unset
    if (::select(1, &set, nullptr, nullptr, &none) != -1) return true;
    Fail();
    socket_ = -1;
    connectStatus_ = kNotConnected;
    return false;
}

bool cRZNetStreamSocket::CloseSocket() {   // @ 0x10079cbaa
    connectStatus_ = kNotConnected;
    return cRZNetSocket::CloseSocket();
}

bool cRZNetStreamSocket::ShutDownSocket(int mode) {   // @ 0x10079cbbe
    connectStatus_ = kShutDown;
    return cRZNetSocket::ShutDownSocket(mode);
}

bool cRZNetStreamSocket::StartListen(int backlog) {   // @ 0x10079cbd2
    if (::listen(static_cast<int>(socket_), backlog) == 0) {
        connectStatus_ = kListening;
        return true;
    }
    return Fail();
}

bool cRZNetStreamSocket::StopListen() {   // @ 0x10079cc14
    if (socket_ == -1) return true;
    const int result = wincompat::closesocket(static_cast<int>(socket_));
    socket_ = -1;
    if (result == 0) {
        connectStatus_ = kNotConnected;
        return true;
    }
    Fail();
    return false;
}

bool cRZNetStreamSocket::Connect(const cRZSocketAddress& address, bool& connected, std::uint32_t timeoutMs) {   // @ 0x10079cc6a
    SetPeerSocketAddress(address);
    return Connect(connected, timeoutMs);
}

bool cRZNetStreamSocket::Connect(bool& connected, std::uint32_t) {   // @ 0x10079cc98
    if (::connect(static_cast<int>(socket_), reinterpret_cast<const sockaddr*>(&peerAddress_), 16) == 0) {
        connected = true;
        connectStatus_ = kConnected;
        return true;
    }
    const int error = WSAError();
    connected = false;
    RecordError(error);
    return error == kWsaEWouldBlock;
}

bool cRZNetStreamSocket::Accept(cRZNetStreamSocket& accepted, bool& done) {   // @ 0x10079cd00
    const bool ok = Accept(accepted.socket_, accepted.peerAddress_, done);
    if (done && ok) {
        accepted.connectStatus_ = kConnected;
        accepted.SetSocketStyle(domain_, type_, protocol_);
    }
    return ok;
}

bool cRZNetStreamSocket::Accept(std::int64_t& socket, cRZSocketAddress& peer, bool& done) {   // @ 0x10079cd60
    socklen_t length = 16;
    const int accepted = ::accept(static_cast<int>(socket_), reinterpret_cast<sockaddr*>(&peer), &length);
    socket = accepted;
    if (accepted == -1) {
        const int error = WSAError();
        done = false;
        RecordError(error);
        return error == kWsaEWouldBlock;
    }
    done = true;
    return true;
}

bool cRZNetStreamSocket::DoMessage(cGZMessage& message) {   // @ 0x10079cdcc
    if (message.type == 0x20) {
        connectStatus_ = kNotConnected;
    } else if (message.type == 0x10) {
        if (message.param2 == 0) {
            connectStatus_ = kConnected;
        } else {
            connectStatus_ = kNotConnected;
            RecordError(static_cast<int>(message.param2));
        }
    }
    return true;
}
