// TCP (IPv4), non-blocking: a listening socket and the connections it accepts, for tools talking to a running game
// (the Lua debugger, script/lua_debugger.h). Loopback and LAN use; no TLS.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "net/udp.h"

namespace net {

class TcpConnection {
public:
    ~TcpConnection();
    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    static std::unique_ptr<TcpConnection> connect(const Address& to, std::string& error);   // (blocking; tools, tests)
    bool open() const { return open_; }
    bool send(const void* data, size_t size);   // all of it (waits while the other side is slow); false: closed
    // What's arrived, appended to `into` (never waits); false once the other side has closed
    bool receive(std::string& into);
    void close();

private:
    friend class TcpListener;
#if defined(_WIN32)
    using Handle = uintptr_t;
#else
    using Handle = int;
#endif
    explicit TcpConnection(Handle h) : handle_(h) {}
    Handle handle_;
    bool open_ = true;
};

class TcpListener {
public:
    TcpListener() = default;
    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    ~TcpListener() { close(); }

    bool listen(uint16_t port, std::string& error);   // 0: any free port (port() tells which)
    void close();
    bool isOpen() const { return open_; }
    uint16_t port() const { return port_; }
    std::unique_ptr<TcpConnection> accept();          // a waiting connection, or null (never waits)

private:
#if defined(_WIN32)
    uintptr_t handle_ = 0;
#else
    int handle_ = -1;
#endif
    bool open_ = false;
    uint16_t port_ = 0;
};

}  // namespace net
