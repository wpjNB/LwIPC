// ai_subscriber.cpp
// Example: simulates an AI inference process that subscribes to LiDAR data
// and publishes control commands.
//
// Run alongside sensor_publisher in separate terminals.

#include "lwipc/lwipc.hpp"

#include <chrono>
#include <cstdio>
#include <csignal>
#include <thread>
#include <atomic>

using namespace lwipc;

static std::atomic<bool> g_stop{false};
static void sig_handler(int) { g_stop.store(true); }

int main() {
    std::signal(SIGINT, sig_handler);

    const char* lidar_ch = "/lwipc_lidar";
    const char* ctrl_ch  = "/lwipc_ctrl";

    // Subscribe to LiDAR
    Subscriber<PointXYZI, 128> lidar_sub(lidar_ch);
    // Publish control commands
    Publisher<ControlCmd, 32>  ctrl_pub(ctrl_ch, MsgType::CONTROL_CMD, 0);

    std::printf("[AISubscriber] Listening on '%s', publishing ctrl on '%s'\n",
                lidar_ch, ctrl_ch);

    auto cursor = lidar_sub.make_cursor();
    int processed = 0;

    while (!g_stop.load()) {
        IPCHeader   hdr{};
        PointXYZI   pt{};
        if (lidar_sub.try_receive(cursor, hdr, pt)) {
            // "AI inference": simple heuristic steer toward origin
            float steer = -pt.x / 20.f;  // normalize
            if (steer >  1.f) steer =  1.f;
            if (steer < -1.f) steer = -1.f;

            ControlCmd cmd{steer, 0.5f, 0.0f, 3};
            ctrl_pub.publish(cmd);
            ++processed;

            if (processed % 50 == 0) {
                std::printf("[AISubscriber] Processed %d pts | last steer=%.3f "
                            "seq=%lu\n",
                            processed, steer, (unsigned long)hdr.sequence);
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    std::printf("[AISubscriber] Exiting. Processed %d frames.\n", processed);
    return 0;
}
