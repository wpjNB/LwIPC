#pragma once

#include "sync_policy.hpp"

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace lwipc {

// ---------------------------------------------------------------------------
// MpmcRingBuffer<T, Capacity>
//
//  Lock-free, Multiple-Producer Multiple-Consumer (MPMC) ring buffer backed
//  by heap memory (intra-process).
//
//  Design
//  ------
//  Each slot has a sequence number (std::atomic<uint64_t>).  To claim a slot
//  for writing, a producer performs a CAS on write_pos_ (fetch-add).  The
//  consumer claims slots similarly on read_pos_.  The sequence number is used
//  to detect whether the slot belongs to the current round and whether it has
//  been fully written.
//
//  This is the classic Dmitry Vyukov MPMC queue adapted for ring semantics:
//    - Producer claims a slot: seq == pos (ready-to-write)
//    - Producer marks ready:  seq = pos + 1
//    - Consumer claims slot:  seq == pos + 1 (ready-to-read)
//    - Consumer marks free:   seq = pos + Capacity
//
//  Template parameters
//  -------------------
//  T        — payload type; must be trivially copyable.
//  Capacity — number of slots; must be a power of two >= 2.
//
//  Usage
//  -----
//    MpmcRingBuffer<MyMsg, 64> rb;
//
//    // Producer (any thread)
//    if (rb.push(msg)) { /* published */ }
//
//    // Consumer (any thread)
//    MyMsg out;
//    if (rb.pop(out)) { /* consumed */ }
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity = 64>
class MpmcRingBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "MpmcRingBuffer T must be trivially copyable");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "MpmcRingBuffer Capacity must be a power of two >= 2");

public:
    MpmcRingBuffer() noexcept {
        // Pre-seed each slot's sequence to its initial index so the first
        // producer at position i can immediately claim slot i.
        for (std::size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(static_cast<uint64_t>(i),
                                     std::memory_order_relaxed);
        }
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

    // Non-copyable, non-movable (contains atomics / embedded storage)
    MpmcRingBuffer(const MpmcRingBuffer&)            = delete;
    MpmcRingBuffer& operator=(const MpmcRingBuffer&) = delete;

    // -----------------------------------------------------------------------
    // push — non-blocking attempt to enqueue one item.
    // Returns true on success, false if the ring is full.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool push(const T& item) noexcept {
        uint64_t pos = write_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            const int64_t  diff = static_cast<int64_t>(seq) - static_cast<int64_t>(pos);

            if (diff == 0) {
                // Slot is free and belongs to this round: try to claim it.
                if (write_pos_.compare_exchange_weak(pos, pos + 1,
                                                     std::memory_order_relaxed)) {
                    std::memcpy(&slot.data, &item, sizeof(T));
                    // Publish: set sequence to pos+1 so consumers can see it.
                    slot.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
                // Another producer won the CAS; retry with fresh pos.
            } else if (diff < 0) {
                // Ring is full.
                return false;
            } else {
                // Another producer is ahead; reload pos and retry.
                pos = write_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // -----------------------------------------------------------------------
    // pop — non-blocking attempt to dequeue one item.
    // Returns true and copies into @p out on success; false if the ring is empty.
    // -----------------------------------------------------------------------
    [[nodiscard]] bool pop(T& out) noexcept {
        uint64_t pos = read_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & kMask];
            const uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            const int64_t  diff = static_cast<int64_t>(seq)
                                - static_cast<int64_t>(pos + 1);

            if (diff == 0) {
                // Slot is ready to read: try to claim it.
                if (read_pos_.compare_exchange_weak(pos, pos + 1,
                                                    std::memory_order_relaxed)) {
                    std::memcpy(&out, &slot.data, sizeof(T));
                    // Mark slot as free for the next round.
                    slot.sequence.store(pos + Capacity, std::memory_order_release);
                    return true;
                }
                // Another consumer won the CAS; retry.
            } else if (diff < 0) {
                // Ring is empty.
                return false;
            } else {
                // Another consumer is ahead; reload pos and retry.
                pos = read_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    /// Approximate number of items currently in the ring (may be stale).
    [[nodiscard]] std::size_t approx_size() const noexcept {
        const uint64_t w = write_pos_.load(std::memory_order_relaxed);
        const uint64_t r = read_pos_.load(std::memory_order_relaxed);
        return (w >= r) ? static_cast<std::size_t>(w - r) : 0;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        T                     data{};
        static constexpr std::size_t kRaw = sizeof(std::atomic<uint64_t>) + sizeof(T);
        static constexpr std::size_t kRem = kRaw % 64u;
        static constexpr std::size_t kPad = (kRem == 0u) ? 1u : (64u - kRem);
        uint8_t _pad[kPad]{};
    };

    static constexpr std::size_t kMask = Capacity - 1u;

    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> read_pos_{0};
    alignas(64) std::array<Slot, Capacity> slots_{};
};

} // namespace lwipc
