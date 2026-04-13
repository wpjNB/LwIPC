// benchmark.cpp
// Latency and throughput benchmark for the shared-memory ring buffer.
//   Publisher and subscriber in the same process with nanosecond timestamps.

#include "lwipc/lwipc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>
#include <atomic>

using namespace lwipc;
using namespace std::chrono;

int main() {
    const char* ch = "/lwipc_bench";
    static constexpr int N = 100'000;

    Publisher<PointXYZI, 1024>  pub(ch, MsgType::POINTCLOUD, 0);
    Subscriber<PointXYZI, 1024> sub(ch);

    std::vector<int64_t> latencies;
    latencies.reserve(N);

    auto cur = sub.make_cursor();

    for (int i = 0; i < N; ++i) {
        auto t0 = steady_clock::now();
        PointXYZI pt{float(i), 0, 0, 0};
        pub.publish(pt);

        PointXYZI got{};
        while (!sub.try_receive(cur, got)) { /* spin */ }
        auto t1 = steady_clock::now();

        latencies.push_back(duration_cast<nanoseconds>(t1 - t0).count());
    }

    std::sort(latencies.begin(), latencies.end());
    double avg = double(std::accumulate(latencies.begin(), latencies.end(), int64_t{0}))
                 / double(N);
    double throughput_gbps =
        (double(N) * sizeof(PointXYZI)) / 1e9
        / (double(latencies.back()) / 1e9);

    std::printf("=== LwIPC Benchmark (%d messages) ===\n", N);
    std::printf("  avg latency : %.1f ns\n", avg);
    std::printf("  p50 latency : %ld ns\n", latencies[N/2]);
    std::printf("  p99 latency : %ld ns\n", latencies[N*99/100]);
    std::printf("  min latency : %ld ns\n", latencies.front());
    std::printf("  max latency : %ld ns\n", latencies.back());
    std::printf("  est. throughput : %.2f GB/s (single point per msg)\n",
                throughput_gbps);
    return 0;
}
