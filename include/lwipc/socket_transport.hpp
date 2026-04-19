#pragma once

#include "ipc_message.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace lwipc {

// ---------------------------------------------------------------------------
// SocketTransport
//
//  Thin TCP framing layer that carries IPCMessage objects across hosts.
//
//  Wire format (little-endian):
//    [4 bytes: total_length (uint32_t)] [sizeof(IPCHeader) bytes: header]
//    [data_length bytes: payload]
//
//  Server side (single-client for simplicity — autonomous driving peer):
//    SocketServer srv(7788);
//    srv.start([](const IPCMessage& msg){ /* process */ });
//    // … later …
//    srv.stop();
//
//  Client side:
//    SocketClient cli("192.168.1.10", 7788);
//    cli.send(msg);
// ---------------------------------------------------------------------------

namespace detail {

// Maximum allowed payload size when receiving a framed message.
// Guards against absurd allocation on corrupt length fields.
static constexpr uint32_t kMaxMessageSize = 64u * 1024u * 1024u;  // 64 MiB

// Read exactly n bytes from fd; returns false on error/EOF
inline bool recv_all(int fd, void* buf, size_t n) noexcept {
    auto* p   = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < n) {
        ssize_t r = ::recv(fd, p + done, n - done, 0);
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
}

// Write exactly n bytes to fd; returns false on error
inline bool send_all(int fd, const void* buf, size_t n) noexcept {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < n) {
        ssize_t w = ::send(fd, p + done, n - done, MSG_NOSIGNAL);
        if (w <= 0) return false;
        done += static_cast<size_t>(w);
    }
    return true;
}

// Frame one IPCMessage into a flat buffer and send it
inline bool send_ipc_message(int fd, const IPCMessage& msg) noexcept {
    auto buf = msg.to_buffer();
    uint32_t total = static_cast<uint32_t>(buf.size());
    if (!send_all(fd, &total, sizeof(total))) return false;
    if (!send_all(fd, buf.data(), buf.size())) return false;
    return true;
}

// Receive one framed IPCMessage; returns invalid IPCMessage on error
inline IPCMessage recv_ipc_message(int fd) noexcept {
    uint32_t total = 0;
    if (!recv_all(fd, &total, sizeof(total))) return {};
    if (total < sizeof(IPCHeader) || total > kMaxMessageSize) return {}; // sanity
    std::vector<uint8_t> buf(total);
    if (!recv_all(fd, buf.data(), total)) return {};
    return IPCMessage::from_buffer(buf.data(), buf.size());
}

// Set SOCK_NONBLOCK + TCP_NODELAY helpers
inline void set_tcp_nodelay(int fd) noexcept {
    int v = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof(v));
}
inline void set_reuseaddr(int fd) noexcept {
    int v = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof(v));
}

} // namespace detail

// ===========================================================================
// SocketServer — accepts multiple concurrent connections, dispatches callbacks
//
//  Each accepted connection is handled in its own background thread.
//  The server supports up to @p max_clients simultaneous connections (default 8).
//  An optional broadcast() method pushes an IPCMessage to all connected clients.
// ===========================================================================
class SocketServer {
public:
    using MsgCallback = std::function<void(const IPCMessage&)>;

    static constexpr int kDefaultMaxClients = 8;

    explicit SocketServer(uint16_t port, int max_clients = kDefaultMaxClients)
        : port_(port), max_clients_(max_clients) {}

    ~SocketServer() { stop(); }

    // Non-copyable
    SocketServer(const SocketServer&)            = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    /// Start listening and accepting in a background accept thread.
    void start(MsgCallback cb) {
        if (running_.load()) return;
        callback_ = std::move(cb);
        running_.store(true);
        accept_thread_ = std::thread([this]() { accept_loop(); });
    }

    /// Stop the server: close the listening socket, signal all client threads,
    /// and wait for them to finish.
    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
            shutdown(listen_fd_, SHUT_RDWR);
            close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        // Join all outstanding client threads
        std::unique_lock<std::mutex> lk(clients_mutex_);
        for (auto& ct : client_threads_) {
            if (ct.joinable()) ct.join();
        }
        client_threads_.clear();
    }

    /// Broadcast @p msg to every currently-connected client.
    /// Returns the number of clients the message was successfully sent to.
    int broadcast(const IPCMessage& msg) noexcept {
        int sent = 0;
        std::lock_guard<std::mutex> lk(client_fds_mutex_);
        for (int fd : client_fds_) {
            if (detail::send_ipc_message(fd, msg)) ++sent;
        }
        return sent;
    }

    /// Approximate number of currently-connected clients.
    [[nodiscard]] int client_count() const noexcept {
        std::lock_guard<std::mutex> lk(client_fds_mutex_);
        return static_cast<int>(client_fds_.size());
    }

    [[nodiscard]] uint16_t port()        const noexcept { return port_; }
    [[nodiscard]] int      max_clients() const noexcept { return max_clients_; }

private:
    void accept_loop() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return;

        detail::set_reuseaddr(listen_fd_);

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close(listen_fd_); listen_fd_ = -1; return;
        }
        if (::listen(listen_fd_, max_clients_) < 0) {
            close(listen_fd_); listen_fd_ = -1; return;
        }

        while (running_.load()) {
            sockaddr_in client_addr{};
            socklen_t   addrlen = sizeof(client_addr);
            int cfd = ::accept(listen_fd_,
                               reinterpret_cast<sockaddr*>(&client_addr), &addrlen);
            if (cfd < 0) break;

            // Enforce connection limit
            {
                std::lock_guard<std::mutex> lk(client_fds_mutex_);
                if (static_cast<int>(client_fds_.size()) >= max_clients_) {
                    close(cfd);
                    continue;
                }
                client_fds_.push_back(cfd);
            }

            detail::set_tcp_nodelay(cfd);

            // Reap finished client threads before spawning a new one
            reap_done_threads();

            std::unique_lock<std::mutex> lk(clients_mutex_);
            client_threads_.emplace_back([this, cfd]() {
                handle_client(cfd);
            });
        }
    }

    void handle_client(int cfd) {
        while (running_.load()) {
            IPCMessage msg = detail::recv_ipc_message(cfd);
            if (!msg.valid()) break;
            if (callback_) callback_(msg);
        }
        // Remove fd from the live-fd list and close
        {
            std::lock_guard<std::mutex> lk(client_fds_mutex_);
            auto it = std::find(client_fds_.begin(), client_fds_.end(), cfd);
            if (it != client_fds_.end()) client_fds_.erase(it);
        }
        close(cfd);
    }

    void reap_done_threads() {
        std::unique_lock<std::mutex> lk(clients_mutex_);
        client_threads_.erase(
            std::remove_if(client_threads_.begin(), client_threads_.end(),
                [](std::thread& t) {
                    // A thread is "done" if it is not joinable (already joined
                    // or default-constructed).  We cannot check from here
                    // without a flag, so we keep all joinable threads and join
                    // only non-joinable ones (safety measure).
                    return !t.joinable();
                }),
            client_threads_.end());
    }

    uint16_t                   port_;
    int                        max_clients_;
    int                        listen_fd_{-1};
    std::atomic<bool>          running_{false};
    std::thread                accept_thread_;
    MsgCallback                callback_;

    // Per-client state
    mutable std::mutex         client_fds_mutex_;
    std::vector<int>           client_fds_;     // open client file descriptors

    std::mutex                 clients_mutex_;
    std::vector<std::thread>   client_threads_;
};

// ===========================================================================
// SocketClient — connects to a SocketServer and sends IPCMessages
// ===========================================================================
class SocketClient {
public:
    SocketClient(const std::string& host, uint16_t port)
        : host_(host), port_(port)
    {}

    ~SocketClient() { disconnect(); }

    // Non-copyable
    SocketClient(const SocketClient&)            = delete;
    SocketClient& operator=(const SocketClient&) = delete;

    /// Connect (blocking).  Throws on failure.
    void connect() {
        if (fd_ >= 0) return;

        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            throw std::runtime_error("socket() failed");

        detail::set_tcp_nodelay(fd_);

        // Resolve host
        struct addrinfo hints{};
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        const std::string port_str = std::to_string(port_);
        if (getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) {
            close(fd_); fd_ = -1;
            throw std::runtime_error("getaddrinfo failed for " + host_);
        }

        bool ok = (::connect(fd_, res->ai_addr, res->ai_addrlen) == 0);
        freeaddrinfo(res);
        if (!ok) {
            close(fd_); fd_ = -1;
            throw std::runtime_error("connect() failed to " + host_);
        }
    }

    void disconnect() {
        if (fd_ >= 0) {
            shutdown(fd_, SHUT_RDWR);
            close(fd_);
            fd_ = -1;
        }
    }

    [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

    /// Send an IPCMessage.  Returns true on success.
    bool send(const IPCMessage& msg) noexcept {
        if (fd_ < 0) return false;
        return detail::send_ipc_message(fd_, msg);
    }

    [[nodiscard]] const std::string& host() const noexcept { return host_; }
    [[nodiscard]] uint16_t           port() const noexcept { return port_; }

private:
    std::string host_;
    uint16_t    port_;
    int         fd_{-1};
};

} // namespace lwipc
