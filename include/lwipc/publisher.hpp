#pragma once

#include "ipc_message.hpp"
#include "object_pool.hpp"
#include "loaned_sample.hpp"
#include "shm_ring_buffer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>

namespace lwipc {

// ---------------------------------------------------------------------------
// Publisher<T, Capacity>
//
//  RAII wrapper that creates a named shared-memory channel and allows the
//  owner process to publish messages into it.
//
//  Zero-copy loan API (new)
//  ------------------------
//  Instead of constructing T on the stack and copying it in, callers may
//  borrow a slot from the publisher's internal ObjectPool, fill it in place,
//  and then hand it back via publish(LoanedSample&&).  The payload is copied
//  once from the pool slot into the ring buffer and the loan is immediately
//  returned to the pool.
//
//  Template parameters
//  -------------------
//  T        — payload type (trivially copyable).
//  Capacity — ring-buffer slot count (power of two).
//
//  Example (classic copy API)
//  --------------------------
//    Publisher<PointXYZI, 128> pub("/lidar_channel", MsgType::POINTCLOUD, 1);
//    PointXYZI pt{1.f, 2.f, 3.f, 0.5f};
//    pub.publish(pt);
//
//  Example (loan API — zero-copy write path)
//  ------------------------------------------
//    auto loan = pub.loan();
//    if (loan) {
//        loan->x = 1.f;  loan->y = 2.f;  loan->z = 3.f;
//        pub.publish(std::move(loan));
//    }
// ---------------------------------------------------------------------------
template <typename T, std::size_t Capacity = 64>
class Publisher {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Publisher payload T must be trivially copyable");
public:
    // Slot type that is stored in the ring buffer: header + payload
    struct Slot {
        IPCHeader header;
        T         payload;
    };
    static_assert(std::is_trivially_copyable<Slot>::value);

    // Pool and loan types
    using PoolType = ObjectPool<T, Capacity>;
    using LoanType = LoanedSample<T, PoolType>;

    // @param channel_name  POSIX shm name (must start with /)
    // @param msg_type      MsgType tag for all messages on this channel
    // @param sensor_id     Source sensor / producer identifier
    Publisher(const std::string& channel_name,
              MsgType            msg_type,
              uint16_t           sensor_id)
        : channel_name_(channel_name)
        , msg_type_(msg_type)
        , sensor_id_(sensor_id)
        , ring_(channel_name, /*create=*/true)
    {}

    ~Publisher() = default;

    // Non-copyable
    Publisher(const Publisher&)            = delete;
    Publisher& operator=(const Publisher&) = delete;

    // -----------------------------------------------------------------------
    // Classic publish API (copies item once into ring slot)
    // -----------------------------------------------------------------------

    /// Publish one item.  Item is copied once into the ring slot.
    void publish(const T& item) {
        Slot s{};
        s.header.msg_type     = msg_type_;
        s.header.sensor_id    = sensor_id_;
        s.header.timestamp_ns = now_ns();
        s.header.sequence     = ++sequence_;
        s.header.data_length  = static_cast<uint32_t>(sizeof(T));
        s.header.checksum     = detail::crc32_update(0, &item, sizeof(T));
        s.payload             = item;
        ring_.push(s);
    }

    /// Publish with explicit timestamp (useful when timestamp comes from HW)
    void publish(const T& item, uint64_t timestamp_ns) {
        Slot s{};
        s.header.msg_type     = msg_type_;
        s.header.sensor_id    = sensor_id_;
        s.header.timestamp_ns = timestamp_ns;
        s.header.sequence     = ++sequence_;
        s.header.data_length  = static_cast<uint32_t>(sizeof(T));
        s.header.checksum     = detail::crc32_update(0, &item, sizeof(T));
        s.payload             = item;
        ring_.push(s);
    }

    // -----------------------------------------------------------------------
    // Loan / zero-copy write API
    // -----------------------------------------------------------------------

    /// Borrow a T slot from the internal object pool.
    /// Returns an empty LoanedSample (operator bool() == false) if the pool
    /// is exhausted.  The caller fills the T in place, then calls publish().
    [[nodiscard]] LoanType loan() noexcept {
        T* p = pool_.acquire();
        return LoanType(p, p ? &pool_ : nullptr);
    }

    /// Zero-copy publish: consume a loan, copy once into the ring, release
    /// the loan back to the pool.  The LoanedSample is left empty after this
    /// call regardless of success.
    ///
    /// If the loan is empty (pool was exhausted), the call is a no-op.
    void publish(LoanType&& loan_sample) noexcept {
        if (!loan_sample) return;
        publish(*loan_sample.get());    // one copy: pool slot → ring slot
        loan_sample.reset();            // return pool slot immediately
    }

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    [[nodiscard]] const std::string& channel_name() const noexcept { return channel_name_; }
    [[nodiscard]] uint64_t           published()    const noexcept { return sequence_; }

    /// Pool availability (useful for back-pressure monitoring).
    /// TODO: expose as a proper QoS / statistics API in a future milestone.
    [[nodiscard]] std::size_t pool_available() const noexcept {
        return pool_.available();
    }

private:
    static uint64_t now_ns() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count());
    }

    std::string                        channel_name_;
    MsgType                            msg_type_;
    uint16_t                           sensor_id_;
    uint64_t                           sequence_{0};
    ShmRingBuffer<Slot, Capacity>      ring_;
    PoolType                           pool_;   // internal object pool for loan()
};

// ---------------------------------------------------------------------------
// IntraPublisher<T, Capacity>
//
//  Lightweight, heap-allocated intra-process publisher (no POSIX shm).
//  Designed for SPSC/MPSC within the same process; avoids the overhead of
//  shm_open / mmap for purely in-process communication paths.
//
//  Usage
//  -----
//    auto pub = std::make_shared<IntraChannel<PointXYZI, 64>>();
//    IntraPublisher<PointXYZI, 64> pub_handle(pub, MsgType::POINTCLOUD, 1);
//    IntraSubscriber<PointXYZI, 64> sub_handle(pub);
//
//  Or via the factory helper:
//    auto [pub, sub] = make_intra_pubsub<PointXYZI, 64>(MsgType::POINTCLOUD, 1);
//
//  See subscriber.hpp for IntraSubscriber definition.
// ---------------------------------------------------------------------------

// Shared ring-buffer control block for intra-process channels.
// Both IntraPublisher and IntraSubscriber hold a shared_ptr to this.
template <typename T, std::size_t Capacity>
struct IntraRingBuffer {
    static_assert(std::is_trivially_copyable<T>::value,
                  "IntraRingBuffer T must be trivially copyable");
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "IntraRingBuffer Capacity must be a power-of-two >= 2");

    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        T                     data{};
        // Pad to the next 64-byte boundary to avoid false sharing between slots.
        // When kRaw is already a multiple of 64, no extra bytes are needed (the
        // alignas(64) on the struct accounts for alignment); we keep a minimum of
        // 1 byte to satisfy the array declaration.
        static constexpr std::size_t kRaw = sizeof(std::atomic<uint64_t>) + sizeof(T);
        static constexpr std::size_t kRem = kRaw % 64u;
        static constexpr std::size_t kPad = (kRem == 0u) ? 1u : (64u - kRem);
        uint8_t _pad[kPad]{};
    };

    struct alignas(64) ControlBlock {
        std::atomic<uint64_t> write_pos{0};
        uint8_t _pad[56]{};
    };

    static constexpr std::size_t kMask = Capacity - 1u;

    ControlBlock ctrl{};
    std::array<Slot, Capacity> slots{};

    /// Producer: push one item, never blocks. Returns write position.
    uint64_t push(const T& item) noexcept {
        const uint64_t pos = ctrl.write_pos.fetch_add(1, std::memory_order_acq_rel);
        Slot& s = slots[pos & kMask];
        // Mark slot as "being written" (even sequence).  Relaxed is sufficient
        // here; the release on the second store provides the necessary ordering.
        s.sequence.store(pos * 2u, std::memory_order_relaxed);
        std::memcpy(&s.data, &item, sizeof(T));
        // Release: makes the memcpy result visible to consumers before they see
        // the odd (ready) sequence number.
        s.sequence.store(pos * 2u + 1u, std::memory_order_release); // mark: ready
        return pos;
    }

    struct Cursor { uint64_t pos{0}; };

    [[nodiscard]] Cursor make_cursor() const noexcept {
        return Cursor{ctrl.write_pos.load(std::memory_order_acquire)};
    }

    bool pop(Cursor& cursor, T& out) noexcept {
        const uint64_t wp = ctrl.write_pos.load(std::memory_order_acquire);
        if (cursor.pos >= wp) return false;
        if (wp - cursor.pos > Capacity) cursor.pos = wp - Capacity;

        Slot& s = slots[cursor.pos & kMask];
        const uint64_t expected = cursor.pos * 2u + 1u;
        if (s.sequence.load(std::memory_order_acquire) != expected) return false;

        std::memcpy(&out, &s.data, sizeof(T));

        if (s.sequence.load(std::memory_order_acquire) != expected) return false;
        ++cursor.pos;
        return true;
    }
};

template <typename T, std::size_t Capacity = 64>
class IntraPublisher {
    static_assert(std::is_trivially_copyable<T>::value,
                  "IntraPublisher T must be trivially copyable");
public:
    using RingType = IntraRingBuffer<T, Capacity>;
    using PoolType = ObjectPool<T, Capacity>;
    using LoanType = LoanedSample<T, PoolType>;

    IntraPublisher(std::shared_ptr<RingType> ring,
                   MsgType                   msg_type,
                   uint16_t                  sensor_id) noexcept
        : ring_(std::move(ring))
        , msg_type_(msg_type)
        , sensor_id_(sensor_id)
    {}

    // Non-copyable
    IntraPublisher(const IntraPublisher&)            = delete;
    IntraPublisher& operator=(const IntraPublisher&) = delete;

    /// Borrow a T from the internal pool (zero-copy write path).
    [[nodiscard]] LoanType loan() noexcept {
        T* p = pool_.acquire();
        return LoanType(p, p ? &pool_ : nullptr);
    }

    /// Zero-copy publish: copy once from pool slot into ring, return loan.
    void publish(LoanType&& loan_sample) noexcept {
        if (!loan_sample) return;
        ring_->push(*loan_sample.get());
        ++sequence_;
        loan_sample.reset();
    }

    /// Classic copy publish.
    void publish(const T& item) noexcept {
        ring_->push(item);
        ++sequence_;
    }

    [[nodiscard]] MsgType  msg_type()  const noexcept { return msg_type_; }
    [[nodiscard]] uint16_t sensor_id() const noexcept { return sensor_id_; }
    [[nodiscard]] uint64_t published() const noexcept { return sequence_; }

    [[nodiscard]] std::shared_ptr<RingType> ring() const noexcept { return ring_; }

    [[nodiscard]] std::size_t pool_available() const noexcept {
        return pool_.available();
    }

private:
    std::shared_ptr<RingType> ring_;
    MsgType                   msg_type_;
    uint16_t                  sensor_id_;
    uint64_t                  sequence_{0};
    PoolType                  pool_;
};

} // namespace lwipc
