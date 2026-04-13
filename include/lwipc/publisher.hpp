#pragma once

#include "ipc_message.hpp"
#include "shm_ring_buffer.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

namespace lwipc {

// ---------------------------------------------------------------------------
// Publisher<T, Capacity>
//
//  RAII wrapper that creates a named shared-memory channel and allows the
//  owner process to publish messages into it.
//
//  Template parameters
//  -------------------
//  T        — payload type (trivially copyable).
//  Capacity — ring-buffer slot count (power of two).
//
//  Example
//  -------
//    Publisher<PointXYZI, 128> pub("/lidar_channel", MsgType::POINTCLOUD, 1);
//    PointXYZI pt{1.f, 2.f, 3.f, 0.5f};
//    pub.publish(pt);
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

    /// Publish one item.  Zero-copy: item is copied once into the ring slot.
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

    [[nodiscard]] const std::string& channel_name() const noexcept { return channel_name_; }
    [[nodiscard]] uint64_t           published()    const noexcept { return sequence_; }

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
};

} // namespace lwipc
