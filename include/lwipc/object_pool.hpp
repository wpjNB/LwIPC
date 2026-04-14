#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace lwipc {

// ---------------------------------------------------------------------------
// ObjectPool<T, Capacity>
//
//  A fixed-size, lock-free, thread-safe object pool that pre-allocates
//  Capacity objects at construction time and never calls malloc/free during
//  steady-state operation.
//
//  Design
//  ------
//  - All storage lives inside the ObjectPool object itself (no heap allocation).
//  - A lock-free LIFO free-list (Treiber stack) tracks available slots.
//  - Each free-list node stores a tagged pointer (index + ABA tag) packed into
//    a single 64-bit atomic to prevent the ABA problem.
//  - T need not be trivially copyable; objects are constructed in-place and
//    their lifetime is managed explicitly via placement-new / explicit destructor
//    calls.
//
//  Thread safety
//  -------------
//  - acquire() and release() are safe to call concurrently from any thread.
//  - The same pointer must not be release()'d more than once.
//
//  Usage
//  -----
//    ObjectPool<PointXYZI, 64> pool;
//    PointXYZI* p = pool.acquire();   // may return nullptr if exhausted
//    if (p) {
//        p->x = 1.f;
//        pool.release(p);             // returns p to the free list
//    }
//
//  Capacity must be a power of two and <= 65535.
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity>
class ObjectPool {
    static_assert(Capacity >= 1,
                  "ObjectPool Capacity must be at least 1");
    static_assert(Capacity <= 65535u,
                  "ObjectPool Capacity must fit in uint16_t index");
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "ObjectPool Capacity must be a power of two");

public:
    // Sentinel: acquire() returns nullptr when the pool is empty.
    static constexpr uint16_t kNullIndex = 0xFFFFu;

    ObjectPool() noexcept {
        // Build the initial free-list: head → 0 → 1 → … → Capacity-1 → null
        for (uint16_t i = 0; i < static_cast<uint16_t>(Capacity); ++i) {
            next_[i] = (i + 1 < static_cast<uint16_t>(Capacity)) ?
                       static_cast<uint16_t>(i + 1) : kNullIndex;
        }
        // head_ = index 0, tag = 0
        head_.store(pack(0, 0), std::memory_order_relaxed);
    }

    ~ObjectPool() noexcept {
        // Destroy any in-use objects would require tracking; the caller is
        // responsible for releasing all loans before the pool is destroyed.
        // TODO: add optional debug-mode check for unreleased slots.
    }

    // Non-copyable, non-movable (embedded storage)
    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // -----------------------------------------------------------------------
    // acquire — borrow one slot from the free-list.
    // Returns a non-null T* on success; nullptr when the pool is exhausted.
    // The returned T* points to uninitialized storage; the caller is
    // responsible for constructing the object (or simply writing its fields).
    // -----------------------------------------------------------------------
    [[nodiscard]] T* acquire() noexcept {
        uint64_t old_head = head_.load(std::memory_order_acquire);
        for (;;) {
            const uint16_t idx = index_of(old_head);
            if (idx == kNullIndex) {
                return nullptr;  // pool exhausted
            }
            // Prepare new head: next slot in the chain, bumped ABA tag
            const uint64_t new_head = pack(next_[idx], tag_of(old_head) + 1u);
            if (head_.compare_exchange_weak(old_head, new_head,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
                return reinterpret_cast<T*>(&storage_[idx]);
            }
        }
    }

    // -----------------------------------------------------------------------
    // release — return a previously acquired pointer back to the pool.
    // Behavior is undefined if p was not obtained from this pool or has
    // already been released.
    // -----------------------------------------------------------------------
    void release(T* p) noexcept {
        assert(p != nullptr);
        const uint16_t idx = index_of_ptr(p);
        assert(idx < static_cast<uint16_t>(Capacity));

        uint64_t old_head = head_.load(std::memory_order_relaxed);
        for (;;) {
            next_[idx] = index_of(old_head);
            const uint64_t new_head = pack(idx, tag_of(old_head) + 1u);
            if (head_.compare_exchange_weak(old_head, new_head,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // available — approximate count of free slots (non-atomic snapshot).
    // -----------------------------------------------------------------------
    [[nodiscard]] std::size_t available() const noexcept {
        std::size_t count = 0;
        uint16_t idx = index_of(head_.load(std::memory_order_relaxed));
        while (idx != kNullIndex && count <= Capacity) {
            ++count;
            idx = next_[idx];
        }
        return count;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    // ABA-safe tagged pointer: bits [63:16] = ABA tag, bits [15:0] = slot index
    static constexpr uint64_t pack(uint16_t idx, uint64_t tag) noexcept {
        return (tag << 16u) | static_cast<uint64_t>(idx);
    }
    static constexpr uint16_t index_of(uint64_t v) noexcept {
        return static_cast<uint16_t>(v & 0xFFFFu);
    }
    static constexpr uint64_t tag_of(uint64_t v) noexcept {
        return v >> 16u;
    }

    uint16_t index_of_ptr(const T* p) const noexcept {
        const auto* base = reinterpret_cast<const StorageType*>(&storage_[0]);
        const auto* slot = reinterpret_cast<const StorageType*>(p);
        return static_cast<uint16_t>(slot - base);
    }

    // -----------------------------------------------------------------------
    // Internal storage
    // -----------------------------------------------------------------------
    using StorageType = std::aligned_storage_t<sizeof(T), alignof(T)>;

    alignas(64) std::array<StorageType, Capacity> storage_{};

    // Free-list next pointers (one per slot)
    alignas(64) std::array<uint16_t, Capacity> next_{};

    // Free-list head: packed (tag | index)
    alignas(64) std::atomic<uint64_t> head_{0};
};

} // namespace lwipc
