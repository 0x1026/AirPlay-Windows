#include "airplay/event_channel.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <plist/plist.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
#endif

namespace ap::airplay {
namespace {

constexpr std::size_t kRxLimit = 1024 * 1024;

std::mutex g_active_mtx;
std::weak_ptr<AirPlayEventChannel> g_active_channel;

std::string peer_to_string(const sockaddr_storage& peer) {
    char ip[INET6_ADDRSTRLEN] = {0};
    uint16_t port = 0;

    if (peer.ss_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(&peer);
        port = ntohs(in6->sin6_port);

        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) {
            in_addr v4{};
            std::memcpy(&v4, in6->sin6_addr.s6_addr + 12, sizeof(v4));
            inet_ntop(AF_INET, &v4, ip, sizeof(ip));
            return std::string(ip) + ':' + std::to_string(port);
        }

        inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof(ip));
        return '[' + std::string(ip) + "]:" + std::to_string(port);
    }

    if (peer.ss_family == AF_INET) {
        const auto* in4 = reinterpret_cast<const sockaddr_in*>(&peer);
        port = ntohs(in4->sin_port);
        inet_ntop(AF_INET, &in4->sin_addr, ip, sizeof(ip));
        return std::string(ip) + ':' + std::to_string(port);
    }

    return "unknown";
}

bool make_listener(socket_t& listener, uint16_t& port) {
    bool listener_is_v6 = true;

    listener = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCK) {
        listener_is_v6 = false;
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCK) return false;
    }

    int on = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&on), sizeof(on));

    if (listener_is_v6) {
        int v6only = 0;
        ::setsockopt(listener, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&v6only), sizeof(v6only));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = 0;

        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ap::net::close_socket(listener);
            listener = INVALID_SOCK;
            return false;
        }

#if defined(_WIN32)
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ap::net::close_socket(listener);
            listener = INVALID_SOCK;
            return false;
        }
        port = ntohs(addr.sin6_port);
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;

        if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ap::net::close_socket(listener);
            listener = INVALID_SOCK;
            return false;
        }

#if defined(_WIN32)
        int len = sizeof(addr);
#else
        socklen_t len = sizeof(addr);
#endif
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            ap::net::close_socket(listener);
            listener = INVALID_SOCK;
            return false;
        }
        port = ntohs(addr.sin_port);
    }

    if (::listen(listener, 4) != 0) {
        ap::net::close_socket(listener);
        listener = INVALID_SOCK;
        return false;
    }

    return true;
}

std::vector<unsigned char> build_media_remote_plist(MediaRemoteCommand command) {
    std::vector<unsigned char> out;

    plist_t root = plist_new_dict();
    if (!root) return out;

    plist_dict_set_item(root, "type", plist_new_string("sendMediaRemoteCommand"));
    plist_dict_set_item(root, "modernMediaRemoteCommand",
                        plist_new_uint(static_cast<uint64_t>(command)));

    char* raw = nullptr;
    uint32_t len = 0;
    plist_to_bin(root, &raw, &len);
    plist_free(root);

    if (raw && len > 0) {
        out.assign(reinterpret_cast<unsigned char*>(raw),
                   reinterpret_cast<unsigned char*>(raw) + len);
    }
    std::free(raw);
    return out;
}

std::vector<unsigned char> build_update_info_plist(
    const std::vector<unsigned char>& info_plist) {
    std::vector<unsigned char> out;
    if (info_plist.empty()) return out;

    plist_t info = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(info_plist.data()),
                   static_cast<uint32_t>(info_plist.size()), &info);
    if (!info || plist_get_node_type(info) != PLIST_DICT) {
        if (info) plist_free(info);
        return out;
    }

    plist_t root = plist_new_dict();
    if (!root) {
        plist_free(info);
        return out;
    }

    plist_dict_set_item(root, "type", plist_new_string("updateInfo"));
    // Ownership of `info` transfers to `root` here.
    plist_dict_set_item(root, "value", info);

    char* raw = nullptr;
    uint32_t len = 0;
    plist_to_bin(root, &raw, &len);
    plist_free(root);

    if (raw && len > 0) {
        out.assign(reinterpret_cast<unsigned char*>(raw),
                   reinterpret_cast<unsigned char*>(raw) + len);
    }
    std::free(raw);
    return out;
}

bool starts_with(const std::string& s, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool header_uint(const std::string& headers,
                 const char* name,
                 uint32_t& value) {
    std::string lower = headers;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    std::string key(name);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    key += ':';

    const auto at = lower.find(key);
    if (at == std::string::npos) return false;

    std::size_t pos = at + key.size();
    while (pos < headers.size() &&
           (headers[pos] == ' ' || headers[pos] == '\t')) {
        ++pos;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(headers.c_str() + pos, &end, 10);
    if (end == headers.c_str() + pos) return false;
    value = static_cast<uint32_t>(parsed);
    return true;
}

int rtsp_status_code(const std::string& first_line) {
    if (!starts_with(first_line, "RTSP/")) return 0;
    const auto first_space = first_line.find(' ');
    if (first_space == std::string::npos) return 0;
    return std::atoi(first_line.c_str() + first_space + 1);
}

std::string plist_type_from_body(const std::string& body) {
    if (body.size() < 8 || std::memcmp(body.data(), "bplist00", 8) != 0) {
        return {};
    }

    plist_t root = nullptr;
    plist_from_bin(body.data(), static_cast<uint32_t>(body.size()), &root);
    if (!root || plist_get_node_type(root) != PLIST_DICT) {
        if (root) plist_free(root);
        return {};
    }

    std::string result;
    plist_t type = plist_dict_get_item(root, "type");
    if (type && plist_get_node_type(type) == PLIST_STRING) {
        char* raw = nullptr;
        plist_get_string_val(type, &raw);
        if (raw) {
            result.assign(raw);
            std::free(raw);
        }
    }
    plist_free(root);
    return result;
}

} // namespace

const char* media_remote_command_name(MediaRemoteCommand command) {
    switch (command) {
        case MediaRemoteCommand::Play:       return "Play";
        case MediaRemoteCommand::Pause:      return "Pause";
        case MediaRemoteCommand::TogglePlay: return "TogglePlay";
        case MediaRemoteCommand::Stop:       return "Stop";
        case MediaRemoteCommand::Next:       return "Next";
        case MediaRemoteCommand::Previous:   return "Previous";
        default:                             return "Unknown";
    }
}

AirPlayEventChannel::AirPlayEventChannel() = default;
AirPlayEventChannel::~AirPlayEventChannel() { stop(); }

bool AirPlayEventChannel::start(
    uint16_t& local_port,
    const std::vector<unsigned char>& pair_verify_secret) {
    if (running_.load()) {
        local_port = local_port_;
        return true;
    }

    // This receiver currently advertises UxPlay-style Legacy Pairing rather
    // than native AirPlay-2 CoreUtils/HomeKit pairing. In that mode event RTSP
    // is plaintext; the ECDH secret is still used elsewhere for the media key
    // post-hash, but not for this socket.
    (void)pair_verify_secret;

    socket_t listener = INVALID_SOCK;
    uint16_t port = 0;
    if (!make_listener(listener, port)) {
        LOG_ERROR << "AirPlay event channel: failed to bind TCP listener: "
                  << ap::net::last_error_string();
        return false;
    }

    listener_ = listener;
    local_port_ = port;
    cseq_ = 0;
    update_info_sent_ = false;
    update_info_acked_ = false;
    update_info_cseq_ = 0xffffffffu;
    {
        std::lock_guard<std::mutex> lock(response_mtx_);
        responses_.clear();
        uncorrelated_responses_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(update_info_mtx_);
        update_info_plist_.clear();
    }

    running_ = true;
    connected_ = false;
    thread_ = std::thread(&AirPlayEventChannel::accept_loop, this);

    local_port = port;
    LOG_INFO << "AirPlay event listener ready on TCP port " << port
             << " (Legacy Pairing plaintext RTSP)";
    return true;
}

void AirPlayEventChannel::stop() {
    if (!running_.exchange(false)) return;

    if (listener_ != INVALID_SOCK) {
#if defined(_WIN32)
        ::shutdown(listener_, SD_BOTH);
#else
        ::shutdown(listener_, SHUT_RDWR);
#endif
        ap::net::close_socket(listener_);
        listener_ = INVALID_SOCK;
    }

    {
        std::lock_guard<std::mutex> lock(channel_mtx_);
        if (channel_ != INVALID_SOCK) {
#if defined(_WIN32)
            ::shutdown(channel_, SD_BOTH);
#else
            ::shutdown(channel_, SHUT_RDWR);
#endif
        }
    }

    response_cv_.notify_all();
    if (thread_.joinable()) thread_.join();

    {
        std::lock_guard<std::mutex> lock(channel_mtx_);
        if (channel_ != INVALID_SOCK) {
            ap::net::close_socket(channel_);
            channel_ = INVALID_SOCK;
        }
    }

    connected_ = false;
    local_port_ = 0;
    update_info_sent_ = false;
    update_info_acked_ = false;
    update_info_cseq_ = 0xffffffffu;
    LOG_INFO << "AirPlay event channel stopped";
}

bool AirPlayEventChannel::send_plain_locked(const unsigned char* data,
                                             std::size_t len) {
    if (channel_ == INVALID_SOCK) return false;
    return ap::net::send_all(channel_, data, static_cast<int>(len)) >= 0;
}

bool AirPlayEventChannel::wait_for_response(uint32_t cseq,
                                             int timeout_ms,
                                             int& status_code) {
    std::unique_lock<std::mutex> lock(response_mtx_);
    const bool ready = response_cv_.wait_for(
        lock,
        std::chrono::milliseconds(timeout_ms),
        [&] {
            return !running_.load() ||
                   responses_.find(cseq) != responses_.end() ||
                   !uncorrelated_responses_.empty();
        });

    if (!ready) return false;

    const auto it = responses_.find(cseq);
    if (it != responses_.end()) {
        status_code = it->second;
        responses_.erase(it);
        return true;
    }

    if (!uncorrelated_responses_.empty()) {
        status_code = uncorrelated_responses_.front();
        uncorrelated_responses_.pop_front();
        return true;
    }

    return false;
}

void AirPlayEventChannel::publish_response(bool has_cseq,
                                            uint32_t cseq,
                                            int status_code) {
    {
        std::lock_guard<std::mutex> lock(response_mtx_);
        if (has_cseq) responses_[cseq] = status_code;
        else uncorrelated_responses_.push_back(status_code);
    }
    response_cv_.notify_all();
}

bool AirPlayEventChannel::send_rtsp_ok(bool has_cseq, uint32_t cseq) {
    std::ostringstream os;
    os << "RTSP/1.0 200 OK\r\n"
       << "Server: AirTunes/220.68\r\n";
    if (has_cseq) os << "CSeq: " << cseq << "\r\n";
    os << "\r\n";
    const std::string response = os.str();

    std::lock_guard<std::mutex> lock(channel_mtx_);
    if (!running_.load() || channel_ == INVALID_SOCK) return false;
    if (!send_plain_locked(
            reinterpret_cast<const unsigned char*>(response.data()),
            response.size())) {
        LOG_WARN << "AirPlay event channel: failed to acknowledge sender"
                 << (has_cseq ? " CSeq=" + std::to_string(cseq) : std::string{})
                 << ": " << ap::net::last_error_string();
        return false;
    }

    LOG_INFO << "AirPlay event channel: replied 200 OK to sender"
             << (has_cseq ? " CSeq=" + std::to_string(cseq) : std::string{});
    return true;
}

bool AirPlayEventChannel::send_plist_request(
    const std::vector<unsigned char>& body,
    const char* description,
    uint32_t& cseq_out,
    int ack_timeout_ms) {
    if (body.empty()) return false;

    uint32_t cseq = 0;
    {
        std::lock_guard<std::mutex> lock(channel_mtx_);
        if (!running_.load() || channel_ == INVALID_SOCK) {
            LOG_WARN << "AirPlay event command " << description
                     << " ignored: event channel is not connected";
            return false;
        }

        cseq = cseq_++;
        std::ostringstream os;
        os << "POST /command RTSP/1.0\r\n"
           << "CSeq: " << cseq << "\r\n"
           << "Content-Length: " << body.size() << "\r\n"
           << "Content-Type: application/x-apple-binary-plist\r\n"
           << "\r\n";
        const std::string header = os.str();

        std::vector<unsigned char> request;
        request.reserve(header.size() + body.size());
        request.insert(request.end(), header.begin(), header.end());
        request.insert(request.end(), body.begin(), body.end());

        if (!send_plain_locked(request.data(), request.size())) {
            LOG_WARN << "AirPlay event command " << description
                     << " send failed: " << ap::net::last_error_string();
            return false;
        }
    }

    cseq_out = cseq;
    LOG_INFO << "AirPlay event command sent: " << description
             << " (CSeq=" << cseq << ", plaintext)";

    if (ack_timeout_ms <= 0) return true;

    int status = 0;
    if (!wait_for_response(cseq, ack_timeout_ms, status)) {
        LOG_WARN << "AirPlay event command " << description
                 << " timed out waiting for RTSP response (CSeq=" << cseq << ')';
        return false;
    }

    if (status != 200) {
        LOG_WARN << "AirPlay event command " << description
                 << " rejected with RTSP " << status
                 << " (CSeq=" << cseq << ')';
        return false;
    }

    LOG_INFO << "AirPlay event command acknowledged: " << description
             << " (CSeq=" << cseq << ", 200 OK)";
    return true;
}

bool AirPlayEventChannel::send_update_info(
    const std::vector<unsigned char>& info_plist) {
    {
        std::lock_guard<std::mutex> lock(update_info_mtx_);
        update_info_plist_ = info_plist;
    }

    const auto body = build_update_info_plist(info_plist);
    if (body.empty()) {
        LOG_WARN << "AirPlay event updateInfo: could not build plist";
        return false;
    }

    uint32_t cseq = 0;
    update_info_acked_ = false;
    const bool ok = send_plist_request(body, "updateInfo", cseq, 0);
    if (!ok) {
        update_info_sent_ = false;
        update_info_cseq_ = 0xffffffffu;
        return false;
    }

    update_info_cseq_ = cseq;
    update_info_sent_ = true;
    LOG_INFO << "AirPlay event protocol initialisation sent "
             << "(updateInfo CSeq=" << cseq
             << ", plaintext); awaiting sender ACK asynchronously";
    return true;
}

bool AirPlayEventChannel::send_media_remote_command(MediaRemoteCommand command) {
    std::unique_lock<std::mutex> command_lock(command_mtx_, std::try_to_lock);
    if (!command_lock.owns_lock()) {
        LOG_INFO << "AirPlay remote command " << media_remote_command_name(command)
                 << " ignored: another MediaRemote command is in flight";
        return false;
    }

    if (!update_info_acked_.load()) {
        std::vector<unsigned char> info;
        {
            std::lock_guard<std::mutex> lock(update_info_mtx_);
            info = update_info_plist_;
        }

        if (!info.empty() && !update_info_sent_.load()) {
            if (!send_update_info(info)) {
                LOG_WARN << "AirPlay remote command " << media_remote_command_name(command)
                         << ": could not send event-channel updateInfo; "
                            "trying MediaRemote command directly";
            }
        }

        if (update_info_sent_.load()) {
            const uint32_t init_cseq = update_info_cseq_.load();
            int init_status = 0;
            if (init_cseq != 0xffffffffu &&
                wait_for_response(init_cseq, 350, init_status)) {
                if (init_status == 200) {
                    update_info_acked_ = true;
                    LOG_INFO << "AirPlay event protocol ready "
                             << "(updateInfo CSeq=" << init_cseq << ", 200 OK)";
                } else {
                    LOG_WARN << "AirPlay event updateInfo returned RTSP "
                             << init_status
                             << "; trying MediaRemote command anyway";
                }
            } else {
                LOG_WARN << "AirPlay event updateInfo was not acknowledged; "
                            "trying MediaRemote command anyway";
            }
        }
    }

    const auto body = build_media_remote_plist(command);
    if (body.empty()) {
        LOG_WARN << "AirPlay remote command: could not serialize plist";
        return false;
    }

    uint32_t cseq = 0;
    const std::string description =
        std::string("sendMediaRemoteCommand/") + media_remote_command_name(command);
    const bool ok = send_plist_request(body, description.c_str(), cseq, 1000);
    if (ok) {
        LOG_INFO << "AirPlay remote command acknowledged: "
                 << media_remote_command_name(command)
                 << " (" << static_cast<uint32_t>(command)
                 << ", CSeq=" << cseq << ')';
    } else {
        LOG_WARN << "AirPlay remote command failed: "
                 << media_remote_command_name(command)
                 << " (" << static_cast<uint32_t>(command)
                 << ", CSeq=" << cseq << ')';
    }
    return ok;
}

void AirPlayEventChannel::accept_loop() {
    while (running_.load()) {
        sockaddr_storage peer{};
#if defined(_WIN32)
        int peer_len = sizeof(peer);
#else
        socklen_t peer_len = sizeof(peer);
#endif

        socket_t fd = ::accept(listener_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
        if (fd == INVALID_SOCK) {
            if (running_.load()) {
                LOG_WARN << "AirPlay event channel accept failed: "
                         << ap::net::last_error_string();
            }
            break;
        }

        if (!running_.load()) {
            ap::net::close_socket(fd);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(channel_mtx_);
            if (channel_ != INVALID_SOCK) {
#if defined(_WIN32)
                ::shutdown(channel_, SD_BOTH);
#else
                ::shutdown(channel_, SHUT_RDWR);
#endif
                ap::net::close_socket(channel_);
            }
            channel_ = fd;
            cseq_ = 0;
            update_info_sent_ = false;
            update_info_acked_ = false;
            update_info_cseq_ = 0xffffffffu;
            {
                std::lock_guard<std::mutex> response_lock(response_mtx_);
                responses_.clear();
                uncorrelated_responses_.clear();
            }
        }

        connected_ = true;
        LOG_INFO << "AirPlay event channel connected from "
                 << peer_to_string(peer)
                 << " (Legacy Pairing plaintext RTSP)";

        drain_connection(fd);

        {
            std::lock_guard<std::mutex> lock(channel_mtx_);
            if (channel_ == fd) channel_ = INVALID_SOCK;
        }
        connected_ = false;
        response_cv_.notify_all();
        ap::net::close_socket(fd);

        if (running_.load()) {
            LOG_INFO << "AirPlay event channel disconnected; waiting for reconnect";
        }
    }
}

void AirPlayEventChannel::drain_connection(socket_t fd) {
    unsigned char buf[4096];
    std::string pending;

    while (running_.load()) {
        const int n = ::recv(fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (n <= 0) break;

        LOG_INFO << "AirPlay event channel: received " << n
                 << " plaintext byte(s)";
        pending.append(reinterpret_cast<const char*>(buf),
                       static_cast<std::size_t>(n));

        while (true) {
            const auto headers_end = pending.find("\r\n\r\n");
            if (headers_end == std::string::npos) break;

            const auto eol = pending.find("\r\n");
            const std::string first_line =
                eol == std::string::npos ? std::string{} : pending.substr(0, eol);

            std::size_t content_length = 0;
            const std::string headers = pending.substr(0, headers_end + 4);
            uint32_t content_length_u32 = 0;
            if (header_uint(headers, "Content-Length", content_length_u32)) {
                content_length = static_cast<std::size_t>(content_length_u32);
            }

            const std::size_t message_len = headers_end + 4 + content_length;
            if (pending.size() < message_len) break;

            const std::string body = pending.substr(headers_end + 4, content_length);
            if (!first_line.empty()) {
                LOG_INFO << "AirPlay event channel: " << first_line;
            }

            uint32_t message_cseq = 0;
            const bool has_cseq = header_uint(headers, "CSeq", message_cseq);

            if (starts_with(first_line, "RTSP/")) {
                const int status = rtsp_status_code(first_line);
                if (status != 0) {
                    publish_response(has_cseq, message_cseq, status);
                }
            } else if (starts_with(first_line, "POST ") ||
                       starts_with(first_line, "GET ") ||
                       starts_with(first_line, "SET_PARAMETER ")) {
                const std::string type = plist_type_from_body(body);
                if (!type.empty()) {
                    LOG_INFO << "AirPlay event channel request type=" << type;
                }
                if (!send_rtsp_ok(has_cseq, message_cseq)) {
                    return;
                }
            }

            pending.erase(0, message_len);
        }

        if (pending.size() > kRxLimit) {
            LOG_WARN << "AirPlay event channel: oversized pending input";
            return;
        }
    }
}

void set_active_event_channel(const std::shared_ptr<AirPlayEventChannel>& channel) {
    std::lock_guard<std::mutex> lock(g_active_mtx);
    g_active_channel = channel;
}

void clear_active_event_channel(const std::shared_ptr<AirPlayEventChannel>& channel) {
    std::lock_guard<std::mutex> lock(g_active_mtx);
    const auto current = g_active_channel.lock();
    if (!current || current == channel) g_active_channel.reset();
}

bool send_active_media_remote_command(MediaRemoteCommand command) {
    std::shared_ptr<AirPlayEventChannel> channel;
    {
        std::lock_guard<std::mutex> lock(g_active_mtx);
        channel = g_active_channel.lock();
    }

    if (!channel) {
        LOG_WARN << "AirPlay remote command " << media_remote_command_name(command)
                 << " ignored: no active AirPlay event channel";
        return false;
    }

    return channel->send_media_remote_command(command);
}

} // namespace ap::airplay
