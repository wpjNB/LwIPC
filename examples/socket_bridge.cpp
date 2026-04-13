// socket_bridge.cpp
// Example: bridges a local shm channel to a remote host via TCP.
//   - Runs as a *server* waiting for a remote peer to connect
//   - Reads ControlCmd messages off the socket and prints them
//
// Pair with a client that sends messages (e.g. the AI subscriber extended
// to use SocketClient instead of / in addition to shm).

#include "lwipc/lwipc.hpp"

#include <csignal>
#include <cstdio>
#include <atomic>
#include <chrono>
#include <thread>

using namespace lwipc;

static std::atomic<bool> g_stop{false};
static void sig_handler(int) { g_stop.store(true); }

int main(int argc, char* argv[]) {
    std::signal(SIGINT, sig_handler);

    uint16_t port = 7788;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));

    std::printf("[SocketBridge] Listening on TCP port %u …\n", port);

    std::atomic<int> total{0};
    SocketServer srv(port);
    srv.start([&](const IPCMessage& msg) {
        if (!msg.valid()) return;
        ++total;
        const auto& h = msg.header();
        std::printf("[SocketBridge] Received msg type=%u sensor=%u seq=%lu "
                    "len=%u crc_ok=%d\n",
                    (unsigned)h.msg_type, (unsigned)h.sensor_id,
                    (unsigned long)h.sequence, h.data_length,
                    msg.verify_checksum() ? 1 : 0);
    });

    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    srv.stop();
    std::printf("[SocketBridge] Done. Total messages received: %d\n", total.load());
    return 0;
}
