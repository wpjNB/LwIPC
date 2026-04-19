#pragma once

#include "ipc_message.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lwipc {

// ---------------------------------------------------------------------------
// ChannelEntry — one record inside the shared registry table
// ---------------------------------------------------------------------------
struct ChannelEntry {
    char     name[64]      = {};    // POSIX shm name, e.g. "/lidar"
    MsgType  msg_type      = MsgType::UNKNOWN;
    uint16_t sensor_id     = 0;
    uint8_t  _pad[6]       = {};
    int32_t  producer_pid  = 0;     // PID of the registering process
    uint64_t created_ns    = 0;     // Unix timestamp (ns) of registration
    uint64_t heartbeat_ns  = 0;     // last-seen heartbeat (updated by producer)

    [[nodiscard]] bool active() const noexcept { return name[0] != '\0'; }
};
static_assert(sizeof(ChannelEntry) == 96, "ChannelEntry layout changed");

// ---------------------------------------------------------------------------
// ChannelRegistry
//
//  A shared-memory table that lets publishers register themselves and
//  consumers enumerate active channels.
//
//  Layout:  [RegistryHeader (64 B)] [ChannelEntry × kMaxChannels]
//
//  Thread / process safety:
//    - Registration and removal are guarded by an atomic spinlock in the header.
//    - list() and find() take a snapshot without holding the lock.
//
//  Usage (publisher side)
//  ----------------------
//    ChannelRegistry reg;
//    reg.register_channel("/lidar", MsgType::POINTCLOUD, 1);
//    // … publish …
//    reg.unregister_channel("/lidar");
//
//  Usage (consumer side)
//  ---------------------
//    ChannelRegistry reg;
//    auto channels = reg.list();
//    for (auto& e : channels) printf("%s\n", e.name);
//
//  Convenience free function
//  -------------------------
//    auto names = lwipc::discover(MsgType::POINTCLOUD);
// ---------------------------------------------------------------------------
class ChannelRegistry {
public:
    static constexpr std::size_t kMaxChannels = 64;
    static constexpr const char* kShmName     = "/lwipc_registry";

    ChannelRegistry() { open_or_create(); }

    ~ChannelRegistry() {
        if (header_) {
            munmap(header_, total_size());
            header_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }

    // Non-copyable
    ChannelRegistry(const ChannelRegistry&)            = delete;
    ChannelRegistry& operator=(const ChannelRegistry&) = delete;

    // -----------------------------------------------------------------------
    // Publisher-side: register / unregister
    // -----------------------------------------------------------------------

    /// Register a channel.  Silently succeeds if already registered (updates heartbeat).
    /// Returns false if the table is full.
    bool register_channel(const std::string& channel_name,
                          MsgType            msg_type,
                          uint16_t           sensor_id = 0) noexcept
    {
        if (!header_) return false;
        SpinLockGuard g(header_->lock);

        ChannelEntry* entries = table();
        // Update existing entry if present
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (entries[i].active() &&
                std::strncmp(entries[i].name, channel_name.c_str(), 63) == 0)
            {
                entries[i].heartbeat_ns = now_ns();
                return true;
            }
        }
        // Insert into first free slot
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (!entries[i].active()) {
                std::strncpy(entries[i].name, channel_name.c_str(), 63);
                entries[i].name[63]       = '\0';
                entries[i].msg_type       = msg_type;
                entries[i].sensor_id      = sensor_id;
                entries[i].producer_pid   = static_cast<int32_t>(::getpid());
                entries[i].created_ns     = now_ns();
                entries[i].heartbeat_ns   = entries[i].created_ns;
                return true;
            }
        }
        return false;  // table full
    }

    /// Remove a channel entry by name.
    void unregister_channel(const std::string& channel_name) noexcept {
        if (!header_) return;
        SpinLockGuard g(header_->lock);
        ChannelEntry* entries = table();
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (entries[i].active() &&
                std::strncmp(entries[i].name, channel_name.c_str(), 63) == 0)
            {
                entries[i] = ChannelEntry{};  // clear
                return;
            }
        }
    }

    /// Touch heartbeat of an existing channel (call periodically from publisher).
    void touch(const std::string& channel_name) noexcept {
        if (!header_) return;
        ChannelEntry* entries = table();
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (entries[i].active() &&
                std::strncmp(entries[i].name, channel_name.c_str(), 63) == 0)
            {
                entries[i].heartbeat_ns = now_ns();
                return;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Consumer-side: list / find
    // -----------------------------------------------------------------------

    /// Return a snapshot of all active channel entries.
    [[nodiscard]] std::vector<ChannelEntry> list() const {
        if (!header_) return {};
        std::vector<ChannelEntry> result;
        result.reserve(kMaxChannels);
        const ChannelEntry* entries = table();
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (entries[i].active()) result.push_back(entries[i]);
        }
        return result;
    }

    /// Return entries whose msg_type matches @p type.
    [[nodiscard]] std::vector<ChannelEntry> list(MsgType type) const {
        auto all = list();
        std::vector<ChannelEntry> filtered;
        for (auto& e : all) {
            if (e.msg_type == type) filtered.push_back(e);
        }
        return filtered;
    }

    /// Return true if @p channel_name is currently registered.
    [[nodiscard]] bool contains(const std::string& channel_name) const noexcept {
        if (!header_) return false;
        const ChannelEntry* entries = table();
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            if (entries[i].active() &&
                std::strncmp(entries[i].name, channel_name.c_str(), 63) == 0)
                return true;
        }
        return false;
    }

private:
    // -----------------------------------------------------------------------
    // Shared memory layout
    // -----------------------------------------------------------------------
    struct alignas(64) RegistryHeader {
        std::atomic<uint32_t> lock{0};      // spinlock: 0=free, 1=locked
        uint8_t               _pad[60];
    };
    static_assert(sizeof(RegistryHeader) == 64);

    static constexpr std::size_t total_size() noexcept {
        return sizeof(RegistryHeader) + sizeof(ChannelEntry) * kMaxChannels;
    }

    ChannelEntry* table() noexcept {
        return reinterpret_cast<ChannelEntry*>(
            reinterpret_cast<uint8_t*>(header_) + sizeof(RegistryHeader));
    }
    const ChannelEntry* table() const noexcept {
        return reinterpret_cast<const ChannelEntry*>(
            reinterpret_cast<const uint8_t*>(header_) + sizeof(RegistryHeader));
    }

    // -----------------------------------------------------------------------
    // Spinlock guard (busy-wait; acceptable for very short critical sections)
    // -----------------------------------------------------------------------
    struct SpinLockGuard {
        std::atomic<uint32_t>& lock_;
        explicit SpinLockGuard(std::atomic<uint32_t>& l) noexcept : lock_(l) {
            uint32_t expected = 0;
            while (!lock_.compare_exchange_weak(expected, 1u,
                                                std::memory_order_acquire,
                                                std::memory_order_relaxed))
                expected = 0;
        }
        ~SpinLockGuard() noexcept { lock_.store(0, std::memory_order_release); }
    };

    // -----------------------------------------------------------------------
    // Open or create the registry shm segment
    // -----------------------------------------------------------------------
    void open_or_create() noexcept {
        // Try to create first; if it already exists, just open it.
        fd_ = ::shm_open(kShmName, O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return;

        // ftruncate is idempotent if size matches; safe to call even if
        // another process already set the size.
        if (::ftruncate(fd_, static_cast<off_t>(total_size())) != 0) {
            close(fd_); fd_ = -1; return;
        }

        void* mem = ::mmap(nullptr, total_size(),
                           PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mem == MAP_FAILED) {
            close(fd_); fd_ = -1; return;
        }
        header_ = static_cast<RegistryHeader*>(mem);
    }

    static uint64_t now_ns() noexcept {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<nanoseconds>(
                system_clock::now().time_since_epoch()).count());
    }

    int              fd_{-1};
    RegistryHeader*  header_{nullptr};
};

// ---------------------------------------------------------------------------
// discover() — free function convenience wrapper
//
//  Returns the names of all currently-registered channels with the given type.
//
//  Example:
//    for (auto& name : lwipc::discover(MsgType::POINTCLOUD))
//        printf("lidar channel: %s\n", name.c_str());
// ---------------------------------------------------------------------------
inline std::vector<std::string> discover(MsgType type) {
    ChannelRegistry reg;
    auto entries = reg.list(type);
    std::vector<std::string> names;
    names.reserve(entries.size());
    for (auto& e : entries) names.emplace_back(e.name);
    return names;
}

} // namespace lwipc
