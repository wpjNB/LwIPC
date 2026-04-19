#pragma once

#include "sync_policy.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <type_traits>

// POSIX shared memory
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lwipc {

// ---------------------------------------------------------------------------
// shm_exists() — non-throwing probe: returns true if the named POSIX shm
//                segment exists and is accessible.
// ---------------------------------------------------------------------------
inline bool shm_exists(const std::string& name) noexcept {
    int fd = ::shm_open(name.c_str(), O_RDONLY, 0);
    if (fd < 0) return false;
    ::close(fd);
    return true;
}

// ---------------------------------------------------------------------------
// ShmRingBuffer<T, Capacity, Policy>
//
//  A lock-free, SPMC (Single Producer, Multiple Consumer) ring buffer that
//  lives in POSIX shared memory so that multiple processes can share it.
//
//  Template parameters
//  -------------------
//  T        — slot payload type; must be trivially copyable.
//  Capacity — number of slots; must be a power of two (checked at compile time).
//  Policy   — SyncPolicy (default: AcqRelPolicy).
//
//  Shared-memory layout
//  --------------------
//    [ControlBlock | Slot[0] … Slot[Capacity-1]]
//
//  ControlBlock holds:
//    write_pos  — next slot index to be written (producer advances this)
//    _padding   — cache-line isolation
//
//  Each Slot holds:
//    sequence   — even = empty/being-written; odd = ready-to-read
//    data       — T payload (zero-copy read)
//
//  Consumer reads
//  --------------
//  Every consumer independently tracks its own local read position.
//  It reads whichever slot matches its current position by checking the
//  sequence number.  If the slot is too far behind (overwritten), it
//  fast-forwards to the latest write position so it always sees fresh data
//  (autonomous-driving "latest-only" semantics).
//
//  Usage
//  -----
//    // Producer process
//    ShmRingBuffer<MyMsg, 64> rb("/my_shm", true);   // create=true
//    rb.push(msg);
//
//    // Consumer process
//    ShmRingBuffer<MyMsg, 64> rb("/my_shm", false);  // create=false
//    auto cursor = rb.make_cursor();
//    MyMsg m;
//    if (rb.pop(cursor, m)) { /* use m */ }
// ---------------------------------------------------------------------------

template <typename T,
          std::size_t Capacity  = 64,
          typename Policy       = DefaultSyncPolicy>
class ShmRingBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "ShmRingBuffer slot type T must be trivially copyable");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "ShmRingBuffer Capacity must be a power of two >= 2");

public:
    // Public cursor type – consumers hold one per subscription
    struct Cursor {
        uint64_t pos = 0;
    };

    // -----------------------------------------------------------------------
    // Construction / destruction
    // -----------------------------------------------------------------------

    /// @param shm_name  POSIX shm name, e.g. "/my_channel" (must start with /)
    /// @param create    true  → create (producer side, O_CREAT|O_TRUNC)
    ///                  false → attach (consumer side, O_RDONLY or O_RDWR)
    explicit ShmRingBuffer(const std::string& shm_name, bool create)
        : shm_name_(shm_name), is_owner_(create)
    {
        open_shm(create);
    }

    ~ShmRingBuffer() {
        if (ctrl_) {
            munmap(ctrl_, total_size());
            ctrl_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (is_owner_) {
            shm_unlink(shm_name_.c_str());
        }
    }

    // Non-copyable, non-movable (holds raw pointers to mmap'd memory)
    ShmRingBuffer(const ShmRingBuffer&)            = delete;
    ShmRingBuffer& operator=(const ShmRingBuffer&) = delete;

    // -----------------------------------------------------------------------
    // Producer API  (single producer)
    // -----------------------------------------------------------------------

    /// Write one item to the next slot.  Never blocks.
    /// Returns the write position used (useful for testing).
    uint64_t push(const T& item) noexcept {
        const uint64_t pos  = Policy::fetch_add(ctrl_->write_pos, uint64_t{1});
        Slot& slot          = slots_[pos & kMask];

        // Mark slot as "being written" (even sequence)
        Policy::store(slot.sequence, pos * 2);

        // Copy payload
        std::memcpy(&slot.data, &item, sizeof(T));

        // Mark slot as "ready" (odd sequence = pos*2+1)
        Policy::store(slot.sequence, pos * 2 + 1);

        // Update heartbeat so consumers can detect a live producer
        ctrl_->last_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);

        return pos;
    }

    // -----------------------------------------------------------------------
    // Consumer API  (multiple consumers, each with independent Cursor)
    // -----------------------------------------------------------------------

    /// Create a cursor positioned at the current write head.
    /// The consumer will start receiving messages published *after* this call.
    [[nodiscard]] Cursor make_cursor() const noexcept {
        Cursor c;
        c.pos = Policy::load(ctrl_->write_pos);
        return c;
    }

    /// Create a cursor at position 0 – consumer will try to read from the
    /// very beginning (subject to ring-buffer wrap-around).
    [[nodiscard]] static Cursor make_cursor_from_start() noexcept {
        return Cursor{0};
    }

    /// Non-blocking pop.
    /// Returns true and copies item into @p out if a new item is available.
    /// Returns false if no new item is available.
    /// If the consumer has fallen behind by more than Capacity slots the
    /// cursor is fast-forwarded to the latest position (data is lost for
    /// the stale slots, but we never block).
    bool pop(Cursor& cursor, T& out) noexcept {
        const uint64_t write_pos = Policy::load(ctrl_->write_pos);

        if (cursor.pos >= write_pos) {
            return false;  // nothing new
        }

        // Fast-forward if too far behind (overwrite happened)
        if (write_pos - cursor.pos > Capacity) {
            cursor.pos = write_pos - Capacity;
        }

        Slot& slot = slots_[cursor.pos & kMask];

        // The expected "ready" sequence for this position
        const uint64_t expected_seq = cursor.pos * 2 + 1;
        const uint64_t seq = Policy::load(slot.sequence);

        if (seq != expected_seq) {
            // Slot not yet ready (or already overwritten by fast-forward adjustment)
            return false;
        }

        std::memcpy(&out, &slot.data, sizeof(T));

        // Verify the slot wasn't overwritten while we were reading
        if (Policy::load(slot.sequence) != expected_seq) {
            return false;  // torn read; caller should retry
        }

        ++cursor.pos;
        return true;
    }

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    [[nodiscard]] uint64_t write_pos() const noexcept {
        return Policy::load(ctrl_->write_pos);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(Capacity);
    }

    [[nodiscard]] const std::string& name() const noexcept { return shm_name_; }

    /// Returns true if the producer has written at least one message within
    /// the last @p timeout_ns nanoseconds.  Returns false if no message has
    /// ever been written or the heartbeat is stale.
    [[nodiscard]] bool producer_alive(uint64_t timeout_ns) const noexcept {
        const uint64_t hb = ctrl_->last_heartbeat_ns.load(std::memory_order_relaxed);
        if (hb == 0) return false;
        const uint64_t now = now_ns();
        return (now >= hb) && ((now - hb) <= timeout_ns);
    }

private:
    // -----------------------------------------------------------------------

    // Internal layout
    // -----------------------------------------------------------------------

    struct alignas(64) ControlBlock {
        std::atomic<uint64_t> write_pos{0};
        // last_heartbeat_ns — updated by the producer on every push().
        // Consumers use this to detect dead producers.
        std::atomic<uint64_t> last_heartbeat_ns{0};
        uint8_t _pad[48];   // pad to 64 bytes (cache-line)
    };
    static_assert(sizeof(ControlBlock) == 64);

    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        T                     data{};
        // Pad so the total slot size is at least 64 bytes to avoid false sharing.
        // If sizeof(sequence)+sizeof(T) already >= 64 the compiler will
        // implicitly add alignment padding via alignas(64); no extra member needed.
        static constexpr std::size_t kPayload = sizeof(std::atomic<uint64_t>) + sizeof(T);
        static constexpr std::size_t kPad     = (kPayload < 64) ? (64 - kPayload) : 1;
        uint8_t _pad[kPad];
    };

    static constexpr std::size_t kMask = Capacity - 1;

    static constexpr std::size_t total_size() noexcept {
        return sizeof(ControlBlock) + sizeof(Slot) * Capacity;
    }

    // -----------------------------------------------------------------------
    // POSIX shm helpers
    // -----------------------------------------------------------------------
    void open_shm(bool create) {
        if (create) {
            // Remove any stale segment first
            shm_unlink(shm_name_.c_str());
            fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd_ < 0)
                throw std::runtime_error("shm_open(create) failed for " + shm_name_);
            if (ftruncate(fd_, static_cast<off_t>(total_size())) != 0)
                throw std::runtime_error("ftruncate failed for " + shm_name_);
        } else {
            fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0666);
            if (fd_ < 0)
                throw std::runtime_error("shm_open(attach) failed for " + shm_name_);
        }

        void* mem = mmap(nullptr, total_size(),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mem == MAP_FAILED)
            throw std::runtime_error("mmap failed for " + shm_name_);

        ctrl_  = static_cast<ControlBlock*>(mem);
        slots_ = reinterpret_cast<Slot*>(
                     static_cast<uint8_t*>(mem) + sizeof(ControlBlock));

        if (create) {
            // Zero-initialise control block and all slots
            new (ctrl_) ControlBlock{};
            for (std::size_t i = 0; i < Capacity; ++i) {
                new (&slots_[i]) Slot{};
                // Pre-seed each slot's sequence to a "written" value that no
                // consumer at position 0 would see as valid
                slots_[i].sequence.store(0, std::memory_order_relaxed);
            }
        }
    }

    std::string    shm_name_;
    bool           is_owner_{false};
    int            fd_{-1};
    ControlBlock*  ctrl_{nullptr};
    Slot*          slots_{nullptr};

    static uint64_t now_ns() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count());
    }
};

} // namespace lwipc
