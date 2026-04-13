#pragma once

#include "publisher.hpp"   // for Publisher<T,Cap>::Slot definition
#include "shm_ring_buffer.hpp"

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>

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
    // Polling API
    // -----------------------------------------------------------------------

    [[nodiscard]] CursorType make_cursor() const noexcept {
        return ring_.make_cursor();
    }

    /// Non-blocking receive.
    /// Returns true and populates @p header / @p payload if a new slot is ready.
    bool try_receive(CursorType& cursor, IPCHeader& header, T& payload) noexcept {
        SlotType slot{};
        if (!ring_.pop(cursor, slot)) return false;
        header  = slot.header;
        payload = slot.payload;
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

    [[nodiscard]] const std::string& channel_name() const noexcept { return channel_name_; }

private:
    std::string                    channel_name_;
    ShmRingBuffer<SlotType, Capacity> ring_;
    std::atomic<bool>              stop_flag_{false};
    std::thread                    worker_;
};

} // namespace lwipc
