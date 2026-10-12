// UDP sockets (IPv4), non-blocking: what multiplayer games send their packets through (net/net_session.h)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

struct Address {
    uint32_t ip = 0;     // host byte order (127.0.0.1 = 0x7F000001)
    uint16_t port = 0;
    bool operator==(const Address& o) const { return ip == o.ip && port == o.port; }
    bool operator!=(const Address& o) const { return !(*this == o); }
    std::string str() const;   // "192.168.1.5:27015"
};

// "host", "host:port", "1.2.3.4:5" -> an address (the port is `defaultPort` when there's none); false if unknown
bool resolve(const std::string& hostPort, uint16_t defaultPort, Address& out, std::string& error);

class UdpSocket {
public:
    UdpSocket() = default;
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket() { close(); }

    bool open(uint16_t port, std::string& error);   // 0: any free port (port() tells which)
    void close();
    bool isOpen() const { return handle_ != kInvalid; }
    uint16_t port() const { return port_; }
    bool send(const Address& to, const void* data, size_t size);
    // A waiting packet into `buffer` (its size), or 0 when there's none (never blocks)
    size_t receive(Address& from, void* buffer, size_t capacity);

private:
#if defined(_WIN32)
    using Handle = uintptr_t;
#else
    using Handle = int;
#endif
    static constexpr Handle kInvalid = (Handle)-1;
    Handle handle_ = kInvalid;
    uint16_t port_ = 0;
};

}  // namespace net
