// vkrelay: the relay server multiplayer games meet through (net/relay.h): lobbies, NAT traversal, and forwarding
// between players whose NATs won't let them talk directly. Runs anywhere with a public UDP port; no GPU, no engine.
//
//   vkrelay [port]     (27100 by default; logs lobbies and joins; Ctrl+C stops it)

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "net/relay.h"

int main(int argc, char** argv) {
    uint16_t port = argc > 1 ? (uint16_t)std::atoi(argv[1]) : relay::kDefaultPort;
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (its log as it happens, also when it goes to a file)
    relay::Server server;
    server.verbose = true;
    std::string error;
    if (!server.open(port, error)) {
        std::fprintf(stderr, "vkrelay: %s\n", error.c_str());
        return 1;
    }
    std::printf("vkrelay: listening on UDP port %u\n", server.port());
    auto start = std::chrono::steady_clock::now();
    for (;;) {   // (a millisecond's sleep between rounds: packets wait in the socket meanwhile)
        server.update(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
