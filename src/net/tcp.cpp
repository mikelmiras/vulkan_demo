#include "net/tcp.h"

#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

namespace {

#if defined(_WIN32)
bool startup() {
    static bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
void closeHandle(uintptr_t h) { closesocket((SOCKET)h); }
void setNonBlocking(uintptr_t h) {
    u_long on = 1;
    ioctlsocket((SOCKET)h, FIONBIO, &on);
}
bool wouldBlock() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
bool startup() { return true; }
void closeHandle(int h) { ::close(h); }
void setNonBlocking(int h) { fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK); }
bool wouldBlock() { return errno == EAGAIN || errno == EWOULDBLOCK; }
#endif

void noDelay(decltype(socket(0, 0, 0)) h) {   // (small request / answer messages: no batching delay)
    int one = 1;
    setsockopt(h, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
}

}  // namespace

TcpConnection::~TcpConnection() { close(); }

void TcpConnection::close() {
    if (!open_) return;
    closeHandle(handle_);
    open_ = false;
}

std::unique_ptr<TcpConnection> TcpConnection::connect(const Address& to, std::string& error) {
    if (!startup()) { error = "networking is unavailable"; return nullptr; }
    auto h = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (h == INVALID_SOCKET) { error = "can't make a socket"; return nullptr; }
#else
    if (h < 0) { error = "can't make a socket"; return nullptr; }
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(to.ip);
    addr.sin_port = htons(to.port);
    if (::connect(h, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeHandle((decltype(TcpConnection::handle_))h);
        error = "can't connect to " + to.str();
        return nullptr;
    }
    noDelay(h);
    setNonBlocking((decltype(TcpConnection::handle_))h);
    return std::unique_ptr<TcpConnection>(new TcpConnection((decltype(TcpConnection::handle_))h));
}

bool TcpConnection::send(const void* data, size_t size) {
    const char* p = static_cast<const char*>(data);
    while (open_ && size > 0) {
        auto n = ::send(handle_, p, (int)size, 0);
        if (n > 0) {
            p += n;
            size -= (size_t)n;
            continue;
        }
        if (n < 0 && wouldBlock()) {   // (its buffer is full: wait a little for room)
#if defined(_WIN32)
            Sleep(1);
#else
            pollfd pf{handle_, POLLOUT, 0};
            ::poll(&pf, 1, 10);
#endif
            continue;
        }
        close();
    }
    return open_;
}

bool TcpConnection::receive(std::string& into) {
    char buf[4096];
    while (open_) {
        auto n = ::recv(handle_, buf, sizeof(buf), 0);
        if (n > 0) {
            into.append(buf, (size_t)n);
            continue;
        }
        if (n < 0 && wouldBlock()) break;
        close();   // (0: closed by the other side; < 0: an error)
    }
    return open_;
}

bool TcpListener::listen(uint16_t port, std::string& error) {
    close();
    if (!startup()) { error = "networking is unavailable"; return false; }
    auto h = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#if defined(_WIN32)
    if (h == INVALID_SOCKET) { error = "can't make a socket"; return false; }
#else
    if (h < 0) { error = "can't make a socket"; return false; }
    int one = 1;
    setsockopt(h, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));   // (restarting soon after: the port is free again)
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(h, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(h, 4) != 0) {
        closeHandle(h);
        error = "port " + std::to_string(port) + " is in use";
        return false;
    }
    setNonBlocking(h);
    sockaddr_in bound{};
#if defined(_WIN32)
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    getsockname(h, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    handle_ = h;
    open_ = true;
    return true;
}

void TcpListener::close() {
    if (!open_) return;
    closeHandle(handle_);
    open_ = false;
    port_ = 0;
}

std::unique_ptr<TcpConnection> TcpListener::accept() {
    if (!open_) return nullptr;
    auto h = ::accept(handle_, nullptr, nullptr);
#if defined(_WIN32)
    if (h == INVALID_SOCKET) return nullptr;
#else
    if (h < 0) return nullptr;
#endif
    noDelay(h);
    setNonBlocking(h);
    return std::unique_ptr<TcpConnection>(new TcpConnection(h));
}

}  // namespace net
