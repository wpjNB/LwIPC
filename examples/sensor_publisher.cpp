// sensor_publisher.cpp
// Example: simulates a LiDAR sensor producer publishing PointCloud frames
// to a shared-memory channel at ~100 Hz.
//
// Run alongside ai_subscriber in separate terminals.

#include "lwipc/lwipc.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace lwipc;
using namespace std::chrono;

int main() {
    const char* ch = "/lwipc_lidar";
    Publisher<PointXYZI, 128> pub(ch, MsgType::POINTCLOUD, /*sensor_id=*/1);

    std::printf("[SensorPublisher] Publishing PointCloud on channel '%s'\n", ch);

    for (int frame = 1; frame <= 500; ++frame) {
        // Simulate a rotating LiDAR point (polar coords → Cartesian)
        float angle = float(frame) * 0.02f;  // radians
        PointXYZI pt{
            std::cos(angle) * 10.f,
            std::sin(angle) * 10.f,
            float(frame % 32) * 0.1f,
            0.8f
        };
        pub.publish(pt);

        if (frame % 100 == 0) {
            std::printf("[SensorPublisher] Published %d frames, write_pos=%lu\n",
                        frame, (unsigned long)pub.published());
        }

        // ~100 Hz
        std::this_thread::sleep_for(microseconds(10'000));
    }

    std::printf("[SensorPublisher] Done. Total published: %lu\n",
                (unsigned long)pub.published());
    return 0;
}
