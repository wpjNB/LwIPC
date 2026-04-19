#pragma once

#include <cstdint>

namespace lwipc {

// ---------------------------------------------------------------------------
// ChannelStats — atomic QoS counters for a publisher or subscriber
// ---------------------------------------------------------------------------
struct ChannelStats {
    uint64_t published        = 0;  // total messages sent by publisher
    uint64_t received         = 0;  // total messages received by this subscriber
    uint64_t dropped          = 0;  // messages skipped due to ring-buffer fast-forward
    uint64_t pool_exhaustions = 0;  // times loan() returned nullptr (publisher side)
    double   avg_latency_us   = 0.0;// rolling average publish→receive latency (µs)
};

} // namespace lwipc
