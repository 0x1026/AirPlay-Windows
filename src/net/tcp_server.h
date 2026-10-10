#pragma once

#include "net/socket.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace ap::net {

struct ClientSocket {
    socket_t fd = INVALID_SOCK;
    std::string peer;   // "ip:port"
};

// Minimal blocking TCP server. Each accepted client runs on its own thread
// calling `handler(client)`. The handler owns the socket and must close it.
//
// stop() prevents new connections, interrupts all currently-connected client
// sockets so their blocking recv() calls wake up, then joins every worker.
class TcpServer {
public:
    using Handler = std::function<void(ClientSocket)>;

    TcpServer() = default;
    ~TcpServer();

    TcpServer(const TcpServer&)            = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    bool start(uint16_t port, Handler handler);
    void stop();

    // Forcibly shutdown every currently-connected client socket. The
    // handler thread sees recv() return 0 / error and tears down its
    // session naturally. Listener stays up — new connections can still
    // be accepted. Used by the "Disconnect" button to drop an active
    // AirPlay session on demand.
    void close_all_clients();

    uint16_t port() const { return port_; }

private:
    void accept_loop();

    // Calls shutdown() on every currently tracked client socket.
    // Does NOT close the descriptor: ownership remains with the handler,
    // which closes it during normal session teardown.
    std::size_t shutdown_active_clients();

    socket_t listener_{INVALID_SOCK};
    uint16_t port_{0};
    std::thread accept_thread_;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;
    Handler handler_;

    // Tracked here so stop()/Disconnect can interrupt blocking client
    // handlers from another thread. Entries are erased by the worker
    // when its handler returns.
    std::mutex         clients_mtx_;
    std::set<socket_t> active_clients_;
};

} // namespace ap::net