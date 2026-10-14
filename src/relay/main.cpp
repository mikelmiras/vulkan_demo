// vkrelay: the relay server multiplayer games meet through (net/relay.h): lobbies, NAT traversal, and forwarding
// between players whose NATs won't let them talk directly. Runs anywhere with a public UDP port; no GPU, no engine.
//
//   vkrelay [port] [--secret S]   (27100 by default; logs lobbies and joins; Ctrl+C stops it)
//
// With a secret (or VKRELAY_SECRET in the environment) only games that know it can host lobbies (project.json's
// "relaySecret", vkgame --relay-secret); anyone can still list and join them. Every ip is rate limited.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "net/relay.h"

int main(int argc, char** argv) {
    uint16_t port = relay::kDefaultPort;
    const char* env = std::getenv("VKRELAY_SECRET");
    std::string secret = env ? env : "";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--secret") && i + 1 < argc) secret = argv[++i];
        else port = (uint16_t)std::atoi(argv[i]);
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // (its log as it happens, also when it goes to a file)
    relay::Server server;
    server.verbose = true;
    server.setSecret(secret);
    std::string error;
    if (!server.open(port, error)) {
        std::fprintf(stderr, "vkrelay: %s\n", error.c_str());
        return 1;
    }
    std::printf("vkrelay: listening on UDP port %u%s\n", server.port(), secret.empty() ? "" : " (hosts need the secret)");
    auto start = std::chrono::steady_clock::now();
    for (;;) {   // (a millisecond's sleep between rounds: packets wait in the socket meanwhile)
        server.update(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
