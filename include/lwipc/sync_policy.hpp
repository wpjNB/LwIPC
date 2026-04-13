#pragma once

#include <atomic>

namespace lwipc {

// ---------------------------------------------------------------------------
// SyncPolicy — encapsulates std::memory_order choices for the ring buffer.
//
//  Three pre-defined policies:
//    RelaxedPolicy   — no ordering guarantees (fastest, only for single-thread)
//    AcqRelPolicy    — acquire on load, release on store (lock-free SPMC)
//    SeqCstPolicy    — sequentially consistent (safest, slowest)
//
//  Each policy provides:
//    store(atom, val)   → atom.store(val, order)
//    load(atom)         → atom.load(order)
//    fetch_add(atom, n) → atom.fetch_add(n, order)
//    compare_exchange_weak(atom, expected, desired) → bool
// ---------------------------------------------------------------------------

struct RelaxedPolicy {
    template <typename T>
    static void store(std::atomic<T>& a, T v) noexcept {
        a.store(v, std::memory_order_relaxed);
    }
    template <typename T>
    static T load(const std::atomic<T>& a) noexcept {
        return a.load(std::memory_order_relaxed);
    }
    template <typename T>
    static T fetch_add(std::atomic<T>& a, T n) noexcept {
        return a.fetch_add(n, std::memory_order_relaxed);
    }
    template <typename T>
    static bool compare_exchange_weak(std::atomic<T>& a, T& expected, T desired) noexcept {
        return a.compare_exchange_weak(expected, desired,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed);
    }
};

struct AcqRelPolicy {
    template <typename T>
    static void store(std::atomic<T>& a, T v) noexcept {
        a.store(v, std::memory_order_release);
    }
    template <typename T>
    static T load(const std::atomic<T>& a) noexcept {
        return a.load(std::memory_order_acquire);
    }
    template <typename T>
    static T fetch_add(std::atomic<T>& a, T n) noexcept {
        return a.fetch_add(n, std::memory_order_acq_rel);
    }
    template <typename T>
    static bool compare_exchange_weak(std::atomic<T>& a, T& expected, T desired) noexcept {
        return a.compare_exchange_weak(expected, desired,
                                       std::memory_order_acq_rel,
                                       std::memory_order_acquire);
    }
};

struct SeqCstPolicy {
    template <typename T>
    static void store(std::atomic<T>& a, T v) noexcept {
        a.store(v, std::memory_order_seq_cst);
    }
    template <typename T>
    static T load(const std::atomic<T>& a) noexcept {
        return a.load(std::memory_order_seq_cst);
    }
    template <typename T>
    static T fetch_add(std::atomic<T>& a, T n) noexcept {
        return a.fetch_add(n, std::memory_order_seq_cst);
    }
    template <typename T>
    static bool compare_exchange_weak(std::atomic<T>& a, T& expected, T desired) noexcept {
        return a.compare_exchange_weak(expected, desired,
                                       std::memory_order_seq_cst,
                                       std::memory_order_seq_cst);
    }
};

// Default policy for the ring buffer
using DefaultSyncPolicy = AcqRelPolicy;

} // namespace lwipc
