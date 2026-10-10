#include "net/tcp_server.h"
#include "log.h"

#include <cstring>

namespace ap::net {

TcpServer::~TcpServer() {
    stop();
}

bool TcpServer::start(uint16_t port, Handler handler) {
    if (running_) {
        LOG_WARN << "TcpServer::start called while already running";
        return false;
    }

    handler_ = std::move(handler);

    // Prefer a dual-stack IPv6 listener. IPv4 peers then arrive as
    // IPv4-mapped IPv6 addresses (::ffff:A.B.C.D).
    bool listener_is_v6 = true;

    listener_ = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listener_ == INVALID_SOCK) {
        LOG_WARN << "socket(AF_INET6) failed — falling back to IPv4-only: "
                 << last_error_string();

        listener_is_v6 = false;
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (listener_ == INVALID_SOCK) {
            LOG_ERROR << "socket(AF_INET) failed: " << last_error_string();
            return false;
        }
    }

    int on = 1;
    ::setsockopt(
        listener_,
        SOL_SOCKET,
        SO_REUSEADDR,
        reinterpret_cast<const char*>(&on),
        sizeof(on)
    );

    if (listener_is_v6) {
        // Windows defaults IPV6_V6ONLY to enabled. Explicitly disable it
        // so this listener accepts both IPv6 and IPv4-mapped clients.
        int v6only = 0;

        if (::setsockopt(
                listener_,
                IPPROTO_IPV6,
                IPV6_V6ONLY,
                reinterpret_cast<const char*>(&v6only),
                sizeof(v6only)) != 0) {

            LOG_WARN << "setsockopt(IPV6_V6ONLY=0) failed: "
                     << last_error_string();
        }

        sockaddr_in6 addr6{};
        addr6.sin6_family = AF_INET6;
        addr6.sin6_addr   = in6addr_any;
        addr6.sin6_port   = htons(port);

        if (::bind(
                listener_,
                reinterpret_cast<sockaddr*>(&addr6),
                sizeof(addr6)) != 0) {

            LOG_ERROR << "bind([::]:" << port
                      << ") failed: " << last_error_string();

            close_socket(listener_);
            listener_ = INVALID_SOCK;
            return false;
        }
    } else {
        sockaddr_in addr4{};
        addr4.sin_family      = AF_INET;
        addr4.sin_addr.s_addr = htonl(INADDR_ANY);
        addr4.sin_port        = htons(port);

        if (::bind(
                listener_,
                reinterpret_cast<sockaddr*>(&addr4),
                sizeof(addr4)) != 0) {

            LOG_ERROR << "bind(0.0.0.0:" << port
                      << ") failed: " << last_error_string();

            close_socket(listener_);
            listener_ = INVALID_SOCK;
            return false;
        }
    }

    if (::listen(listener_, 8) != 0) {
        LOG_ERROR << "listen() failed: " << last_error_string();

        close_socket(listener_);
        listener_ = INVALID_SOCK;
        return false;
    }

    port_    = port;
    running_ = true;

    accept_thread_ = std::thread(&TcpServer::accept_loop, this);

    if (listener_is_v6) {
        LOG_INFO << "TCP server listening on [::]:" << port
                 << " (IPv4+IPv6)";
    } else {
        LOG_INFO << "TCP server listening on 0.0.0.0:" << port
                 << " (IPv4 only)";
    }

    return true;
}

void TcpServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    LOG_INFO << "TCP server: stopping";

    // Stop accepting new clients first.
    //
    // Closing the listener wakes the blocking accept() call. Once the
    // accept thread has joined, no new entries can be added to
    // active_clients_ or workers_.
    if (listener_ != INVALID_SOCK) {
        close_socket(listener_);
        listener_ = INVALID_SOCK;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Existing AirPlay RTSP connections may still be blocked in recv().
    // Interrupt them before joining the worker threads. The handler still
    // owns each descriptor and will close it during normal teardown.
    const std::size_t active = shutdown_active_clients();

    if (active > 0) {
        LOG_INFO << "TCP server: shutdown " << active
                 << " active client(s) during server stop";
    }

    // The client shutdown above wakes each handler's recv(), allowing the
    // AirPlay session to run its normal teardown:
    //
    // StreamSession::teardown()
    //   -> NtpClient::stop()
    //   -> AudioReceiver::stop()
    //   -> socket cleanup
    //
    // Only after that is it safe for the worker thread to terminate.
    for (auto& t : workers_) {
        if (t.joinable()) {
            t.join();
        }
    }

    workers_.clear();

    {
        std::lock_guard<std::mutex> lock(clients_mtx_);

        if (!active_clients_.empty()) {
            LOG_WARN << "TCP server: "
                     << active_clients_.size()
                     << " client socket(s) remained tracked after workers joined";
        }

        active_clients_.clear();
    }

    handler_ = {};
    port_    = 0;

    LOG_INFO << "TCP server: stopped";
}

void TcpServer::accept_loop() {
    while (running_) {
        sockaddr_storage peer{};

#if defined(_WIN32)
        int len = sizeof(peer);
#else
        socklen_t len = sizeof(peer);
#endif

        socket_t fd = ::accept(
            listener_,
            reinterpret_cast<sockaddr*>(&peer),
            &len
        );

        if (fd == INVALID_SOCK) {
            if (running_) {
                LOG_WARN << "accept() failed: " << last_error_string();
            }

            return;
        }

        // stop() may have raced with accept(): the listener can return one
        // final connection just as running_ becomes false. Do not create a
        // new worker during shutdown.
        if (!running_) {
#if defined(_WIN32)
            ::shutdown(fd, SD_BOTH);
#else
            ::shutdown(fd, SHUT_RDWR);
#endif
            close_socket(fd);
            return;
        }

        // Format the peer as a human-readable "ip:port".
        //
        // IPv4-mapped IPv6 addresses (::ffff:A.B.C.D) are rendered as
        // ordinary dotted quads. Native IPv6 peers use "[address]:port".
        char ip[INET6_ADDRSTRLEN] = {0};
        uint16_t peer_port = 0;
        bool peer_is_v6 = false;

        if (peer.ss_family == AF_INET6) {
            auto* in6 = reinterpret_cast<sockaddr_in6*>(&peer);

            if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
                in_addr a{};

                const auto* bytes = in6->sin6_addr.s6_addr;
                std::memcpy(
                    &a,
                    bytes + 12,
                    sizeof(a)
                );

                inet_ntop(AF_INET, &a, ip, sizeof(ip));
            } else {
                inet_ntop(
                    AF_INET6,
                    &in6->sin6_addr,
                    ip,
                    sizeof(ip)
                );

                peer_is_v6 = true;
            }

            peer_port = ntohs(in6->sin6_port);
        } else if (peer.ss_family == AF_INET) {
            auto* in4 = reinterpret_cast<sockaddr_in*>(&peer);

            inet_ntop(
                AF_INET,
                &in4->sin_addr,
                ip,
                sizeof(ip)
            );

            peer_port = ntohs(in4->sin_port);
        }

        ClientSocket client;
        client.fd = fd;

        client.peer = peer_is_v6
            ? "[" + std::string(ip) + "]:" + std::to_string(peer_port)
            : std::string(ip) + ":" + std::to_string(peer_port);

        LOG_INFO << "accepted " << client.peer;

        const socket_t track_fd = client.fd;

        {
            std::lock_guard<std::mutex> lock(clients_mtx_);
            active_clients_.insert(track_fd);
        }

        workers_.emplace_back(
            [this,
             h = handler_,
             c = std::move(client),
             track_fd]() mutable {

                h(std::move(c));

                std::lock_guard<std::mutex> lock(clients_mtx_);
                active_clients_.erase(track_fd);
            }
        );
    }
}

std::size_t TcpServer::shutdown_active_clients() {
    std::lock_guard<std::mutex> lock(clients_mtx_);

    const std::size_t count = active_clients_.size();

    for (socket_t fd : active_clients_) {
        if (fd == INVALID_SOCK) {
            continue;
        }

#if defined(_WIN32)
        ::shutdown(fd, SD_BOTH);
#else
        ::shutdown(fd, SHUT_RDWR);
#endif
    }

    return count;
}

void TcpServer::close_all_clients() {
    const std::size_t count = shutdown_active_clients();

    LOG_INFO << "TCP server: shutdown "
             << count
             << " active client(s) on user request";

    // Do not erase entries here. Their worker threads own the socket and
    // erase their own tracking entry after their handler has completed
    // normal session teardown.
}

} // namespace ap::net