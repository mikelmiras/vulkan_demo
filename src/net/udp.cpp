#include "net/udp.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace net {

namespace {

bool startup() {
#if defined(_WIN32)
    static bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

sockaddr_in toSockaddr(const Address& a) {
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(a.ip);
    s.sin_port = htons(a.port);
    return s;
}

}  // namespace

std::string Address::str() const {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u:%u", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255, port);
    return buf;
}

bool resolve(const std::string& hostPort, uint16_t defaultPort, Address& out, std::string& error) {
    if (!startup()) { error = "networking is unavailable"; return false; }
    std::string host = hostPort;
    uint16_t port = defaultPort;
    if (size_t colon = hostPort.rfind(':'); colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        int p = std::atoi(hostPort.c_str() + colon + 1);
        if (p <= 0 || p > 65535) { error = "not a port: " + hostPort.substr(colon + 1); return false; }
        port = (uint16_t)p;
    }
    if (host.empty() || host == "localhost") host = "127.0.0.1";
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        error = "can't find " + host;
        return false;
    }
    out.ip = ntohl(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(res);
    return true;
}

bool UdpSocket::open(uint16_t port, std::string& error) {
    close();
    if (!startup()) { error = "networking is unavailable"; return false; }
    Handle h = (Handle)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h == kInvalid) { error = "can't make a socket"; return false; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(h, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
        closesocket(h);
#else
        ::close(h);
#endif
        error = "port " + std::to_string(port) + " is in use";
        return false;
    }
#if defined(_WIN32)
    u_long nonBlocking = 1;
    ioctlsocket(h, FIONBIO, &nonBlocking);
#else
    fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in bound{};
#if defined(_WIN32)
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    getsockname(h, reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    handle_ = h;
    return true;
}

void UdpSocket::close() {
    if (handle_ == kInvalid) return;
#if defined(_WIN32)
    closesocket(handle_);
#else
    ::close(handle_);
#endif
    handle_ = kInvalid;
    port_ = 0;
}

bool UdpSocket::send(const Address& to, const void* data, size_t size) {
    if (handle_ == kInvalid) return false;
    sockaddr_in addr = toSockaddr(to);
    auto sent = sendto(handle_, static_cast<const char*>(data), (int)size, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == (decltype(sent))size;
}

size_t UdpSocket::receive(Address& from, void* buffer, size_t capacity) {
    if (handle_ == kInvalid) return 0;
    sockaddr_in addr{};
#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    auto n = recvfrom(handle_, static_cast<char*>(buffer), (int)capacity, 0, reinterpret_cast<sockaddr*>(&addr), &len);
    if (n <= 0) return 0;   // (nothing waiting, or an error: either way, nothing)
    from.ip = ntohl(addr.sin_addr.s_addr);
    from.port = ntohs(addr.sin_port);
    return (size_t)n;
}

}  // namespace net
