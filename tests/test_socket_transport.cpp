// test_socket_transport.cpp
// Integration tests for SocketServer / SocketClient using loopback TCP.

#include "lwipc/socket_transport.hpp"
#include "lwipc/ipc_message.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace lwipc;

static int g_tests = 0, g_pass = 0;

#define CHECK(cond)  do { \
    ++g_tests; \
    if (cond) { ++g_pass; } \
    else { std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

// ---------------------------------------------------------------------------
void test_single_message_roundtrip() {
    std::atomic<bool>  received{false};
    std::atomic<float> recv_x{0.f};

    SocketServer srv(17788);
    srv.start([&](const IPCMessage& msg) {
        CHECK(msg.valid());
        CHECK(msg.header().msg_type == MsgType::POINTCLOUD);
        const PointXYZI* pt = msg.as<PointXYZI>();
        if (pt) recv_x.store(pt->x, std::memory_order_relaxed);
        received.store(true, std::memory_order_release);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    SocketClient cli("127.0.0.1", 17788);
    cli.connect();

    PointXYZI pt{9.9f, 0, 0, 0};
    auto msg = IPCMessage::make(MsgType::POINTCLOUD, 1, 0, 1, pt);
    CHECK(cli.send(msg));

    // Wait for server to process
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (!received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    CHECK(received.load());
    CHECK(std::abs(recv_x.load() - 9.9f) < 1e-5f);

    cli.disconnect();
    srv.stop();
}

void test_multiple_messages() {
    static constexpr int N = 20;
    std::atomic<int> count{0};

    SocketServer srv(17789);
    srv.start([&](const IPCMessage& msg) {
        if (msg.valid()) count.fetch_add(1, std::memory_order_relaxed);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    SocketClient cli("127.0.0.1", 17789);
    cli.connect();

    for (int i = 0; i < N; ++i) {
        ControlCmd cmd{float(i)/N, 0.5f, 0.f, 3};
        auto msg = IPCMessage::make(MsgType::CONTROL_CMD, 0, 0, uint64_t(i), cmd);
        CHECK(cli.send(msg));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (count.load() < N && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(count.load() == N);

    cli.disconnect();
    srv.stop();
}

void test_checksum_integrity_over_socket() {
    std::atomic<bool> valid{false};
    std::atomic<bool> checksum_ok{false};

    SocketServer srv(17790);
    srv.start([&](const IPCMessage& msg) {
        valid.store(msg.valid(), std::memory_order_relaxed);
        checksum_ok.store(msg.verify_checksum(), std::memory_order_relaxed);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    SocketClient cli("127.0.0.1", 17790);
    cli.connect();

    ImuSample imu{};
    imu.accel[1] = -9.8f;
    imu.orientation[0] = 1.f;  // unit quaternion
    auto msg = IPCMessage::make(MsgType::IMU, 5, 123456789ULL, 42, imu);
    cli.send(msg);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    CHECK(valid.load());
    CHECK(checksum_ok.load());

    cli.disconnect();
    srv.stop();
}

// ---------------------------------------------------------------------------
int main() {
    test_single_message_roundtrip();
    test_multiple_messages();
    test_checksum_integrity_over_socket();

    std::printf("SocketTransport tests: %d/%d passed\n", g_pass, g_tests);
    return (g_pass == g_tests) ? 0 : 1;
}
