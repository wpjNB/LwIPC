#pragma once

#include <cstdint>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace lwipc {

// ---------------------------------------------------------------------------
// Message type identifiers
// ---------------------------------------------------------------------------
enum class MsgType : uint16_t {
    UNKNOWN     = 0,
    IMAGE       = 1,   // Camera frame  (raw bytes, width/height in header)
    POINTCLOUD  = 2,   // LiDAR points  (array of float[4]: x,y,z,intensity)
    IMU         = 3,   // IMU sample    (accel xyz + gyro xyz + orientation)
    TENSOR      = 4,   // NN inference result (flat float array)
    CONTROL_CMD = 5,   // Control output (steering / throttle / brake)
};

// ---------------------------------------------------------------------------
// CRC-32 (ISO 3309) — lightweight, no external dependency
// ---------------------------------------------------------------------------
namespace detail {
inline uint32_t crc32_update(uint32_t crc, const void* data, size_t len) noexcept {
    static constexpr uint32_t kTable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    crc ^= 0xFFFFFFFFu;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        crc = (crc >> 4) ^ kTable[(crc ^ (p[i] >>  0)) & 0x0F];
        crc = (crc >> 4) ^ kTable[(crc ^ (p[i] >>  4)) & 0x0F];
    }
    return crc ^ 0xFFFFFFFFu;
}
} // namespace detail

// ---------------------------------------------------------------------------
// IPCHeader — fixed-size header prepended to every message
// ---------------------------------------------------------------------------
struct alignas(64) IPCHeader {
    static constexpr uint32_t kMagic   = 0x4C574950u;  // "LWIP"
    static constexpr uint8_t  kVersion = 1;

    uint32_t magic      = kMagic;    // sanity / framing
    uint8_t  version    = kVersion;
    uint8_t  _pad0[3]   = {};
    MsgType  msg_type   = MsgType::UNKNOWN;
    uint16_t sensor_id  = 0;         // source sensor / producer ID
    uint64_t timestamp_ns = 0;       // Unix timestamp in nanoseconds
    uint64_t sequence   = 0;         // monotonically increasing per producer
    uint32_t data_length = 0;        // byte length of the payload that follows
    uint32_t checksum   = 0;         // CRC-32 over payload bytes
    uint8_t  _pad1[24]  = {};        // pad to 64 bytes total

    [[nodiscard]] bool valid() const noexcept {
        return magic == kMagic && version == kVersion;
    }
};
static_assert(sizeof(IPCHeader) == 64, "IPCHeader must be exactly 64 bytes");

// ---------------------------------------------------------------------------
// Typed payload structs for common autonomous-driving messages
// ---------------------------------------------------------------------------

struct PointXYZI {
    float x, y, z, intensity;
};

struct ImuSample {
    float accel[3];     // m/s²
    float gyro[3];      // rad/s
    float orientation[4]; // quaternion wxyz
};

struct ControlCmd {
    float steering;     // [-1, 1]  left…right
    float throttle;     // [ 0, 1]
    float brake;        // [ 0, 1]
    uint8_t gear;       // 0=park,1=reverse,2=neutral,3=drive
    uint8_t _pad[3]     = {};
};
static_assert(sizeof(ControlCmd) == 16, "ControlCmd must be 16 bytes");

// ---------------------------------------------------------------------------
// ImageEncoding — pixel layout / format tag
// ---------------------------------------------------------------------------
enum class ImageEncoding : uint8_t {
    UNKNOWN = 0,
    GRAY8   = 1,   // 8-bit grayscale
    RGB8    = 2,   // 8-bit packed RGB
    BGR8    = 3,   // 8-bit packed BGR (OpenCV default)
    RGBA8   = 4,   // 8-bit RGBA
    YUV420  = 5,   // planar YUV 4:2:0
};

// ---------------------------------------------------------------------------
// ImageFrame<MaxBytes> — fixed-capacity camera frame
//
// MaxBytes sets the compile-time buffer size.  A typical use case:
//   ImageFrame<1920*1080*3>  — uncompressed RGB Full-HD (≈6 MB)
//   ImageFrame<640*480*3>    — VGA RGB            (≈921 kB)
//
// Both are trivially copyable, which is required for Publisher<T,N>.
// For very large images the zero-copy loan() API avoids stack copies.
// ---------------------------------------------------------------------------
template <uint32_t MaxBytes>
struct ImageFrame {
    uint32_t      width       = 0;
    uint32_t      height      = 0;
    uint32_t      stride      = 0;      // bytes per row
    uint32_t      byte_count  = 0;      // actual populated bytes (≤ MaxBytes)
    uint8_t       channels    = 0;
    ImageEncoding encoding    = ImageEncoding::UNKNOWN;
    uint8_t       _pad[2]     = {};
    uint8_t       data[MaxBytes] = {};

    static constexpr uint32_t kMaxBytes = MaxBytes;
};

// Convenience alias for common resolutions
using ImageFrameVGA  = ImageFrame<640u * 480u * 4u>;   // RGBA VGA
using ImageFrameHD   = ImageFrame<1280u * 720u * 3u>;  // RGB 720p

// ---------------------------------------------------------------------------
// Tensor<MaxElems> — fixed-capacity flat float tensor
//
// MaxElems is the maximum number of float elements the tensor can hold.
// Typical NN output: classification (1000), bounding boxes (300*7≈2100),
// segmentation masks (512*512≈262 144).
// ---------------------------------------------------------------------------
template <uint32_t MaxElems>
struct Tensor {
    uint32_t ndim         = 0;       // number of dimensions (≤ 8)
    uint32_t dims[8]      = {};      // size along each dimension
    uint32_t num_elements = 0;       // actual populated elements (≤ MaxElems)
    uint8_t  _pad[4]      = {};
    float    data[MaxElems] = {};

    static constexpr uint32_t kMaxElems = MaxElems;
};

// Convenience aliases
using TensorSmall  = Tensor<1024u>;      // small classification outputs
using TensorMedium = Tensor<8192u>;      // detection / small feature maps
using TensorLarge  = Tensor<262144u>;    // segmentation masks (512×512)

// ---------------------------------------------------------------------------
// IPCMessage — header + owned heap payload
// ---------------------------------------------------------------------------
class IPCMessage {
public:
    // Default-constructed message is explicitly INVALID
    IPCMessage() = default;

    // Construct from header + raw payload bytes (copied). Marks as valid.
    IPCMessage(const IPCHeader& hdr, const void* payload, uint32_t length)
        : header_(hdr), valid_(true)
    {
        header_.data_length = length;
        if (length > 0 && payload) {
            data_.resize(length);
            std::memcpy(data_.data(), payload, length);
        }
        header_.checksum = compute_checksum();
    }

    // Convenience: construct and sign a typed payload
    template <typename T>
    static IPCMessage make(MsgType type, uint16_t sensor_id,
                           uint64_t timestamp_ns, uint64_t sequence,
                           const T& payload)
    {
        IPCHeader hdr{};
        hdr.msg_type      = type;
        hdr.sensor_id     = sensor_id;
        hdr.timestamp_ns  = timestamp_ns;
        hdr.sequence      = sequence;
        IPCMessage msg(hdr, &payload, static_cast<uint32_t>(sizeof(T)));
        return msg;
    }

    // Construct from contiguous buffer (header followed by payload).
    // Returns an invalid message on any parse error.
    static IPCMessage from_buffer(const void* buf, size_t buf_len) {
        if (buf_len < sizeof(IPCHeader)) return {};
        IPCMessage msg;
        std::memcpy(&msg.header_, buf, sizeof(IPCHeader));
        if (!msg.header_.valid()) return {};           // bad magic / version
        const uint32_t dlen = msg.header_.data_length;
        if (dlen > 0) {
            if (buf_len < sizeof(IPCHeader) + dlen) return {};
            msg.data_.resize(dlen);
            std::memcpy(msg.data_.data(),
                        static_cast<const uint8_t*>(buf) + sizeof(IPCHeader),
                        dlen);
        }
        msg.valid_ = true;
        return msg;
    }

    // Serialise to flat byte buffer (header + payload)
    [[nodiscard]] std::vector<uint8_t> to_buffer() const {
        const size_t dat_size = data_.size();
        std::vector<uint8_t> buf(sizeof(IPCHeader) + dat_size);
        // Use std::copy to avoid spurious compiler alias/restrict warnings
        const auto* hdr_bytes = reinterpret_cast<const uint8_t*>(&header_);
        std::copy(hdr_bytes, hdr_bytes + sizeof(IPCHeader), buf.begin());
        if (dat_size > 0)
            std::copy(data_.begin(), data_.end(), buf.begin() + sizeof(IPCHeader));
        return buf;
    }

    [[nodiscard]] bool verify_checksum() const noexcept {
        return valid_ && (header_.checksum == compute_checksum());
    }

    // Accessors
    [[nodiscard]] const IPCHeader&             header() const noexcept { return header_; }
    [[nodiscard]] const std::vector<uint8_t>&  data()   const noexcept { return data_;   }
    /// Returns true only for messages successfully constructed or parsed.
    [[nodiscard]] bool                         valid()  const noexcept { return valid_;  }

    template <typename T>
    [[nodiscard]] const T* as() const noexcept {
        if (data_.size() < sizeof(T)) return nullptr;
        return reinterpret_cast<const T*>(data_.data());
    }

private:
    [[nodiscard]] uint32_t compute_checksum() const noexcept {
        if (data_.empty()) return 0;
        return detail::crc32_update(0, data_.data(), data_.size());
    }

    IPCHeader             header_{};
    std::vector<uint8_t>  data_;
    bool                  valid_{false};   // false for default-constructed (error) state
};

} // namespace lwipc
