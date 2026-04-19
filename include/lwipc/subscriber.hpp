#pragma once

#include "channel_stats.hpp"
#include "publisher.hpp"   // for Publisher<T,Cap>::Slot and IntraRingBuffer
#include "shm_ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace lwipc {

// ---------------------------------------------------------------------------
// Subscriber<T, Capacity>
//
//  RAII wrapper that attaches to an existing named shared-memory channel and
//  allows one or more consumer threads to receive messages.
//
//  Two usage modes
//  ---------------
//  1. Polling  — call try_receive() in your own loop.
//  2. Callback — call spin(callback) or start_async(callback) and the
//                subscriber drives the loop.
//
//  Example (polling)
//  -----------------
//    Subscriber<PointXYZI, 128> sub("/lidar_channel");
//    auto cur = sub.make_cursor();
//    PointXYZI pt;
//    while (sub.try_receive(cur, pt)) { /* process pt */ }
//
//  Example (callback)
//  ------------------
//    Subscriber<PointXYZI, 128> sub("/lidar_channel");
//    sub.start_async([](const IPCHeader& h, const PointXYZI& p) {
//        // runs in background thread
//    });
//    // … later …
//    sub.stop();
//
//  Dead-producer detection
//  -----------------------
//    sub.producer_alive(1'000'000'000)  // true if heard within last 1 s
//
//  Retry construction
//  ------------------
//    auto sub = Subscriber<PointXYZI,128>::try_attach("/lidar", 5000ms);
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity = 64>
class Subscriber {
public:
    using SlotType   = typename Publisher<T, Capacity>::Slot;
    using CursorType = typename ShmRingBuffer<SlotType, Capacity>::Cursor;
    using Callback   = std::function<void(const IPCHeader&, const T&)>;

    explicit Subscriber(const std::string& channel_name)
        : channel_name_(channel_name)
        , ring_(channel_name, /*create=*/false)
    {}

    ~Subscriber() { stop(); }

    // Non-copyable
    Subscriber(const Subscriber&)            = delete;
    Subscriber& operator=(const Subscriber&) = delete;

    // -----------------------------------------------------------------------
    // Factory: try_attach — retry construction with a timeout
    //
    //  Returns nullptr on timeout; throws on unexpected errors.
    //  Useful when the producer might not yet have created the shm segment.
    // -----------------------------------------------------------------------
    static std::unique_ptr<Subscriber> try_attach(
            const std::string& channel_name,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(5000),
            std::chrono::milliseconds backoff = std::chrono::milliseconds(10))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (shm_exists(channel_name)) {
                try {
                    return std::make_unique<Subscriber>(channel_name);
                } catch (...) { /* shm vanished between check and open; retry */ }
            }
            std::this_thread::sleep_for(backoff);
        }
        return nullptr;
    }

    // -----------------------------------------------------------------------
    // Polling API
    // -----------------------------------------------------------------------

    [[nodiscard]] CursorType make_cursor() const noexcept {
        return ring_.make_cursor();
    }

    /// Non-blocking receive.
    /// Returns true and populates @p header / @p payload if a new slot is ready.
    bool try_receive(CursorType& cursor, IPCHeader& header, T& payload) noexcept {
        const uint64_t pre_pos = cursor.pos;
        SlotType slot{};
        if (!ring_.pop(cursor, slot)) return false;

        // Account for fast-forward drops
        if (cursor.pos > pre_pos + 1) {
            stats_dropped_.fetch_add(cursor.pos - pre_pos - 1, std::memory_order_relaxed);
        }

        header  = slot.header;
        payload = slot.payload;
        stats_received_.fetch_add(1, std::memory_order_relaxed);

        // Rolling latency estimate (exponential moving average, α ≈ 1/16)
        const uint64_t now = now_ns();
        if (header.timestamp_ns > 0 && now >= header.timestamp_ns) {
            const double lat_us = double(now - header.timestamp_ns) / 1000.0;
            // α = 1/16 EMA
            const double prev = avg_latency_us_.load(std::memory_order_relaxed);
            avg_latency_us_.store(prev + (lat_us - prev) / 16.0,
                                  std::memory_order_relaxed);
        }

        return true;
    }

    /// Convenience overload that skips the header output.
    bool try_receive(CursorType& cursor, T& payload) noexcept {
        IPCHeader hdr;
        return try_receive(cursor, hdr, payload);
    }

    // -----------------------------------------------------------------------
    // Callback / async API
    // -----------------------------------------------------------------------

    /// Block the calling thread, invoking @p cb for each new message.
    /// Returns when stop() is called from another thread.
    void spin(Callback cb,
              std::chrono::microseconds poll_us = std::chrono::microseconds(100)) {
        auto cursor = ring_.make_cursor();
        while (!stop_flag_.load(std::memory_order_acquire)) {
            IPCHeader hdr;
            T         payload{};
            if (try_receive(cursor, hdr, payload)) {
                cb(hdr, payload);
            } else {
                std::this_thread::sleep_for(poll_us);
            }
        }
    }

    /// Launch spin() in a background thread.
    void start_async(Callback cb,
                     std::chrono::microseconds poll_us = std::chrono::microseconds(100)) {
        stop_flag_.store(false, std::memory_order_release);
        worker_ = std::thread([this, cb, poll_us]() {
            spin(cb, poll_us);
        });
    }

    /// Signal the background thread to stop and join it.
    void stop() {
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

    // -----------------------------------------------------------------------
    // Dead-producer detection
    // -----------------------------------------------------------------------

    /// Returns true if the producer has published at least one message within
    /// the last @p timeout_ns nanoseconds.
    [[nodiscard]] bool producer_alive(uint64_t timeout_ns) const noexcept {
        return ring_.producer_alive(timeout_ns);
    }

    // -----------------------------------------------------------------------
    // QoS / Statistics
    // -----------------------------------------------------------------------

    [[nodiscard]] ChannelStats stats() const noexcept {
        ChannelStats s;
        s.received        = stats_received_.load(std::memory_order_relaxed);
        s.dropped         = stats_dropped_.load(std::memory_order_relaxed);
        s.avg_latency_us  = avg_latency_us_.load(std::memory_order_relaxed);
        return s;
    }

    [[nodiscard]] const std::string& channel_name() const noexcept { return channel_name_; }

private:
    static uint64_t now_ns() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count());
    }

    std::string                       channel_name_;
    ShmRingBuffer<SlotType, Capacity> ring_;
    std::atomic<bool>                 stop_flag_{false};
    std::thread                       worker_;

    // Stats counters (all updated by consumer thread(s); relaxed is sufficient)
    mutable std::atomic<uint64_t> stats_received_{0};
    mutable std::atomic<uint64_t> stats_dropped_{0};
    mutable std::atomic<double>   avg_latency_us_{0.0};
};

// ---------------------------------------------------------------------------
// IntraSubscriber<T, Capacity>
//
//  Intra-process subscriber that reads from a shared IntraRingBuffer.
//  Designed to pair with IntraPublisher<T, Capacity>.
//
//  Multiple IntraSubscriber instances can independently consume from the
//  same IntraRingBuffer (SPMC — single producer, multiple consumers).
//  Each subscriber maintains its own read cursor.
//
//  Usage (polling)
//  ---------------
//    auto ring = std::make_shared<IntraRingBuffer<PointXYZI, 64>>();
//    IntraPublisher<PointXYZI, 64> pub(ring, MsgType::POINTCLOUD, 1);
//    IntraSubscriber<PointXYZI, 64> sub(ring);
//    auto cur = sub.make_cursor();
//    PointXYZI pt;
//    while (sub.try_receive(cur, pt)) { /* process */ }
//
//  Usage (async callback)
//  ----------------------
//    sub.start_async([](const PointXYZI& p) { /* ... */ });
//    // … later …
//    sub.stop();
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity = 64>
class IntraSubscriber {
public:
    using RingType   = IntraRingBuffer<T, Capacity>;
    using CursorType = typename RingType::Cursor;
    using Callback   = std::function<void(const T&)>;

    explicit IntraSubscriber(std::shared_ptr<RingType> ring) noexcept
        : ring_(std::move(ring))
    {}

    ~IntraSubscriber() { stop(); }

    // Non-copyable
    IntraSubscriber(const IntraSubscriber&)            = delete;
    IntraSubscriber& operator=(const IntraSubscriber&) = delete;

    // -----------------------------------------------------------------------
    // Polling API
    // -----------------------------------------------------------------------

    /// Create a cursor positioned at the current write head.
    /// Messages published *before* make_cursor() are not visible to this cursor.
    [[nodiscard]] CursorType make_cursor() const noexcept {
        return ring_->make_cursor();
    }

    /// Non-blocking receive. Returns true and copies into @p out if new data available.
    bool try_receive(CursorType& cursor, T& out) noexcept {
        return ring_->pop(cursor, out);
    }

    // -----------------------------------------------------------------------
    // Callback / async API
    // -----------------------------------------------------------------------

    /// Block the calling thread, invoking @p cb for each new message.
    /// Returns when stop() is called from another thread.
    void spin(Callback cb,
              std::chrono::microseconds poll_us = std::chrono::microseconds(100)) {
        auto cursor = ring_->make_cursor();
        while (!stop_flag_.load(std::memory_order_acquire)) {
            T payload{};
            if (try_receive(cursor, payload)) {
                cb(payload);
            } else {
                std::this_thread::sleep_for(poll_us);
            }
        }
    }

    /// Launch spin() in a background thread.
    void start_async(Callback cb,
                     std::chrono::microseconds poll_us = std::chrono::microseconds(100)) {
        stop_flag_.store(false, std::memory_order_release);
        worker_ = std::thread([this, cb = std::move(cb), poll_us]() mutable {
            spin(std::move(cb), poll_us);
        });
    }

    /// Signal the background thread to stop and join it.
    void stop() {
        stop_flag_.store(true, std::memory_order_release);
        if (worker_.joinable()) worker_.join();
    }

private:
    std::shared_ptr<RingType> ring_;
    std::atomic<bool>         stop_flag_{false};
    std::thread               worker_;
};

// ---------------------------------------------------------------------------
// IntraPubSubPair — result type for make_intra_pubsub()
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity = 64>
struct IntraPubSubPair {
    std::unique_ptr<IntraPublisher<T, Capacity>>  publisher;
    std::unique_ptr<IntraSubscriber<T, Capacity>> subscriber;
};

// ---------------------------------------------------------------------------
// make_intra_pubsub — convenience factory
//
//  Creates a matching IntraPublisher / IntraSubscriber pair sharing the same
//  internal ring buffer.  Both are allocated on the heap and returned as
//  unique_ptrs inside IntraPubSubPair.
//
//  Usage:
//    auto ps = make_intra_pubsub<PointXYZI, 64>(MsgType::POINTCLOUD, 1);
//    ps.publisher->publish(pt);
//    auto cur = ps.subscriber->make_cursor();
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity = 64>
IntraPubSubPair<T, Capacity>
make_intra_pubsub(MsgType msg_type, uint16_t sensor_id = 0) {
    auto ring = std::make_shared<IntraRingBuffer<T, Capacity>>();
    return IntraPubSubPair<T, Capacity>{
        std::make_unique<IntraPublisher<T, Capacity>>(ring, msg_type, sensor_id),
        std::make_unique<IntraSubscriber<T, Capacity>>(ring)
    };
}

} // namespace lwipc
