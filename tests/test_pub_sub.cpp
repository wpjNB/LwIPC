// test_pub_sub.cpp
// Integration tests for Publisher / Subscriber using shared memory.

#include "lwipc/publisher.hpp"
#include "lwipc/subscriber.hpp"
#include "lwipc/ipc_message.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>

using namespace lwipc;

static int g_tests = 0, g_pass = 0;

#define CHECK(cond)  do { \
    ++g_tests; \
    if (cond) { ++g_pass; } \
    else { std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while(0)

// ---------------------------------------------------------------------------
void test_pointcloud_pub_sub() {
    const char* ch = "/lwipc_ps_lidar";

    Publisher<PointXYZI, 64>  pub(ch, MsgType::POINTCLOUD, /*sensor_id=*/1);
    Subscriber<PointXYZI, 64> sub(ch);
    auto cur = sub.make_cursor();

    PointXYZI pt{1.f, 2.f, 3.f, 0.5f};
    pub.publish(pt);

    IPCHeader hdr{};
    PointXYZI got{};
    CHECK(sub.try_receive(cur, hdr, got));
    CHECK(hdr.msg_type  == MsgType::POINTCLOUD);
    CHECK(hdr.sensor_id == 1);
    CHECK(hdr.sequence  == 1);
    CHECK(hdr.data_length == sizeof(PointXYZI));
    CHECK(got.x == 1.f && got.y == 2.f && got.z == 3.f);
}

void test_imu_pub_sub() {
    const char* ch = "/lwipc_ps_imu";

    Publisher<ImuSample, 32>  pub(ch, MsgType::IMU, 2);
    Subscriber<ImuSample, 32> sub(ch);
    auto cur = sub.make_cursor();

    ImuSample imu{};
    imu.accel[0] = 9.8f;
    imu.gyro[1]  = 0.05f;
    pub.publish(imu);

    ImuSample got{};
    CHECK(sub.try_receive(cur, got));
    CHECK(got.accel[0] == 9.8f);
    CHECK(got.gyro[1]  == 0.05f);
}

void test_control_cmd_pub_sub() {
    const char* ch = "/lwipc_ps_ctrl";

    Publisher<ControlCmd, 16>  pub(ch, MsgType::CONTROL_CMD, 0);
    Subscriber<ControlCmd, 16> sub(ch);
    auto cur = sub.make_cursor();

    ControlCmd cmd{0.3f, 0.7f, 0.0f, 3};
    pub.publish(cmd);

    IPCHeader   hdr{};
    ControlCmd  got{};
    CHECK(sub.try_receive(cur, hdr, got));
    CHECK(hdr.msg_type == MsgType::CONTROL_CMD);
    CHECK(got.steering == 0.3f);
    CHECK(got.throttle == 0.7f);
    CHECK(got.gear     == 3);
}

void test_async_subscriber() {
    const char* ch = "/lwipc_ps_async";

    Publisher<PointXYZI, 64>  pub(ch, MsgType::POINTCLOUD, 7);
    Subscriber<PointXYZI, 64> sub(ch);

    std::atomic<int> received{0};
    sub.start_async([&](const IPCHeader&, const PointXYZI& p) {
        (void)p;
        received.fetch_add(1, std::memory_order_relaxed);
    }, std::chrono::microseconds(50));

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    for (int i = 0; i < 10; ++i) {
        PointXYZI pt{float(i), 0, 0, 0};
        pub.publish(pt);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    sub.stop();

    CHECK(received.load() >= 5);  // at minimum 5 of 10 (some may be missed due to timing)
}

void test_multiple_subscribers() {
    const char* ch = "/lwipc_ps_multi";

    Publisher<PointXYZI, 64>   pub(ch, MsgType::POINTCLOUD, 9);
    Subscriber<PointXYZI, 64>  sub1(ch);
    Subscriber<PointXYZI, 64>  sub2(ch);

    auto cur1 = sub1.make_cursor();
    auto cur2 = sub2.make_cursor();

    PointXYZI pt{5.f, 6.f, 7.f, 1.f};
    pub.publish(pt);

    PointXYZI g1{}, g2{};
    CHECK(sub1.try_receive(cur1, g1));
    CHECK(sub2.try_receive(cur2, g2));
    CHECK(g1.x == 5.f && g2.x == 5.f);
}

void test_high_frequency_publish() {
    const char* ch = "/lwipc_ps_hf";

    Publisher<PointXYZI, 256>  pub(ch, MsgType::POINTCLOUD, 1);
    Subscriber<PointXYZI, 256> sub(ch);
    auto cur = sub.make_cursor();

    static constexpr int N = 200;
    for (int i = 0; i < N; ++i) {
        PointXYZI pt{float(i), 0, 0, 0};
        pub.publish(pt);
    }

    int cnt = 0;
    PointXYZI got{};
    while (sub.try_receive(cur, got)) ++cnt;
    CHECK(cnt == N);
    CHECK(pub.published() == N);
}

// ---------------------------------------------------------------------------
int main() {
    test_pointcloud_pub_sub();
    test_imu_pub_sub();
    test_control_cmd_pub_sub();
    test_async_subscriber();
    test_multiple_subscribers();
    test_high_frequency_publish();

    std::printf("Publisher/Subscriber tests: %d/%d passed\n", g_pass, g_tests);
    return (g_pass == g_tests) ? 0 : 1;
}
