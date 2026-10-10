#include "airplay/ntp_client.h"
#include "log.h"

#include <array>
#include <chrono>
#include <cstring>
#include <limits>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netdb.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/time.h>
#endif

namespace ap::airplay {
namespace {

constexpr uint64_t kSecondsFrom1900To1970 = 2208988800ULL;
constexpr uint64_t kNanosPerSecond        = 1'000'000'000ULL;
constexpr auto     kPollInterval          = std::chrono::seconds(3);
constexpr auto     kRecvTimeout           = std::chrono::seconds(1);
constexpr std::size_t kClockSamples       = 8;

uint64_t now_ns_since_1970() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

void put_u32_be(unsigned char* buf, int off, uint32_t v) {
    buf[off + 0] = static_cast<unsigned char>((v >> 24) & 0xff);
    buf[off + 1] = static_cast<unsigned char>((v >> 16) & 0xff);
    buf[off + 2] = static_cast<unsigned char>((v >>  8) & 0xff);
    buf[off + 3] = static_cast<unsigned char>((v >>  0) & 0xff);
}

void put_u64_be(unsigned char* buf, int off, uint64_t v) {
    put_u32_be(buf, off + 0, static_cast<uint32_t>(v >> 32));
    put_u32_be(buf, off + 4, static_cast<uint32_t>(v));
}

uint64_t get_u64_be(const unsigned char* buf, int off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | buf[off + i];
    }
    return v;
}

uint64_t ntp_raw_to_ns(uint64_t ntp_timestamp) {
    const uint64_t seconds_1900 = (ntp_timestamp >> 32) & 0xffffffffULL;
    const uint64_t fraction     = ntp_timestamp & 0xffffffffULL;

    if (seconds_1900 < kSecondsFrom1900To1970) {
        return 0;
    }

    const uint64_t seconds = seconds_1900 - kSecondsFrom1900To1970;
    const uint64_t nanos = (fraction * kNanosPerSecond) >> 32;
    return seconds * kNanosPerSecond + nanos;
}

// UxPlay's byteutils_put_ntp_timestamp: 32-bit big-endian seconds since 1900,
// then 32-bit big-endian fractional seconds.
void put_ntp_timestamp(unsigned char* buf, int off, uint64_t ns_since_1970) {
    const uint64_t seconds =
        ns_since_1970 / kNanosPerSecond + kSecondsFrom1900To1970;
    const uint64_t nanos    = ns_since_1970 % kNanosPerSecond;
    const uint64_t fraction = (nanos << 32) / kNanosPerSecond;

    put_u32_be(buf, off + 0, static_cast<uint32_t>(seconds));
    put_u32_be(buf, off + 4, static_cast<uint32_t>(fraction));
}

void set_recv_timeout(socket_t s, std::chrono::milliseconds t) {
#if defined(_WIN32)
    DWORD ms = static_cast<DWORD>(t.count());
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec  = static_cast<time_t>(t.count() / 1000);
    tv.tv_usec = static_cast<suseconds_t>((t.count() % 1000) * 1000);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

struct ClockSample {
    int64_t offset_ns = 0; // remote - local
    int64_t delay_ns  = std::numeric_limits<int64_t>::max();
    bool    valid     = false;
};

} // namespace

NtpClient::NtpClient() = default;

NtpClient::~NtpClient() {
    stop();
}

bool NtpClient::start(socket_t sock,
                      const std::string& remote_ip,
                      uint16_t remote_port) {
    if (sock == INVALID_SOCK) {
        LOG_ERROR << "NtpClient.start: invalid socket";
        return false;
    }

    sock_        = sock;
    remote_ip_   = remote_ip;
    remote_port_ = remote_port;

    offset_ns_.store(0, std::memory_order_release);
    synchronized_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);

    thread_ = std::thread(&NtpClient::thread_fn, this);

    LOG_INFO << "NTP client polling " << remote_ip_ << ':' << remote_port_
             << " every 3 sec";
    return true;
}

void NtpClient::stop() {
    // Always join a joinable thread even if it exited by itself. Otherwise
    // std::thread's destructor would terminate the process.
    const bool was_running = running_.exchange(false);

    if (was_running && sock_ != INVALID_SOCK) {
        // Wake the blocking recvfrom instead of waiting for SO_RCVTIMEO.
        // The StreamSession still owns and closes this socket later.
#if defined(_WIN32)
        ::shutdown(sock_, SD_BOTH);
#else
        ::shutdown(sock_, SHUT_RDWR);
#endif
    }

    if (thread_.joinable()) {
        thread_.join();
    }
}

uint64_t NtpClient::remote_ntp_to_ns(uint64_t ntp_timestamp) const {
    return ntp_raw_to_ns(ntp_timestamp);
}

uint64_t NtpClient::remote_to_local_ns(uint64_t remote_ns) const {
    if (!synchronized()) {
        return 0;
    }

    const int64_t offset = offset_ns_.load(std::memory_order_acquire);
    const int64_t remote = static_cast<int64_t>(remote_ns);
    const int64_t local  = remote - offset;

    return local > 0 ? static_cast<uint64_t>(local) : 0;
}

void NtpClient::thread_fn() {
    // Fixed 32-byte request used by AirPlay's NTP timing protocol.
    unsigned char request[32] = {
        0x80, 0xd2, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    unsigned char response[128] = {0};

    // Resolve independently from the local socket family. bind_udp() normally
    // gives us an IPv6 dual-stack socket, while the iPhone may be an IPv4
    // literal. Build ::ffff:x.y.z.w explicitly when needed on Windows.
    sockaddr_storage remote{};
    socklen_t remote_len = 0;

    sockaddr_storage local_ss{};
#if defined(_WIN32)
    int local_len = sizeof(local_ss);
#else
    socklen_t local_len = sizeof(local_ss);
#endif

    if (::getsockname(sock_, reinterpret_cast<sockaddr*>(&local_ss),
                      &local_len) != 0) {
        LOG_ERROR << "NtpClient: getsockname failed";
        running_ = false;
        return;
    }

    const int sock_family = local_ss.ss_family;

    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_NUMERICHOST;

    const std::string port_str = std::to_string(remote_port_);
    addrinfo* info = nullptr;
    const int gai = ::getaddrinfo(remote_ip_.c_str(),
                                  port_str.c_str(),
                                  &hints, &info);
    if (gai != 0 || !info) {
        LOG_ERROR << "NtpClient: getaddrinfo(" << remote_ip_
                  << ") failed gai=" << gai;
        running_ = false;
        return;
    }

    bool resolved = false;
    for (addrinfo* ai = info; ai && !resolved; ai = ai->ai_next) {
        if (ai->ai_family == sock_family) {
            if (ai->ai_addrlen <= sizeof(remote)) {
                std::memcpy(&remote, ai->ai_addr, ai->ai_addrlen);
                remote_len = static_cast<socklen_t>(ai->ai_addrlen);
                resolved = true;
            }
            continue;
        }

        if (sock_family == AF_INET6 && ai->ai_family == AF_INET) {
            const auto* src =
                reinterpret_cast<const sockaddr_in*>(ai->ai_addr);

            sockaddr_in6 mapped{};
            mapped.sin6_family = AF_INET6;
            mapped.sin6_port   = src->sin_port;

            auto* bytes = mapped.sin6_addr.s6_addr;
            std::memset(bytes, 0, 10);
            bytes[10] = 0xff;
            bytes[11] = 0xff;
            std::memcpy(bytes + 12, &src->sin_addr, sizeof(src->sin_addr));

            std::memcpy(&remote, &mapped, sizeof(mapped));
            remote_len = static_cast<socklen_t>(sizeof(mapped));
            resolved = true;
        }
    }

    ::freeaddrinfo(info);

    if (!resolved) {
        LOG_ERROR << "NtpClient: remote address family is incompatible "
                  << "with timing socket (remote=" << remote_ip_
                  << ", socket_family=" << sock_family << ')';
        running_ = false;
        return;
    }

    set_recv_timeout(sock_, kRecvTimeout);

    uint64_t recv_time       = 0;
    uint64_t client_ref_time = 0;
    uint64_t probes_sent     = 0;
    uint64_t probes_received = 0;

    std::array<ClockSample, kClockSamples> samples{};
    std::size_t sample_index = 0;
    std::size_t sample_count = 0;

    while (running_) {
        const uint64_t send_time = now_ns_since_1970();
        put_ntp_timestamp(request, 24, send_time);

        if (recv_time) {
            put_u64_be(request, 8, client_ref_time);
            put_ntp_timestamp(request, 16, recv_time);
        }

        const int sent = ::sendto(
            sock_,
            reinterpret_cast<const char*>(request),
            sizeof(request),
            0,
            reinterpret_cast<sockaddr*>(&remote),
            remote_len
        );

        if (sent < 0) {
            if (running_) {
                LOG_WARN << "NtpClient: sendto failed";
            }
        } else {
            ++probes_sent;
        }

        const int n = ::recvfrom(
            sock_,
            reinterpret_cast<char*>(response),
            sizeof(response),
            0,
            nullptr,
            nullptr
        );

        if (n >= 32) {
            // AirPlay NTP response:
            //   t0 response[8..15]  local request departure time (echoed)
            //   t1 response[16..23] remote request arrival time
            //   t2 response[24..31] remote response departure time
            //   t3                  local response arrival time
            // offset is remote_clock - local_clock.
            const uint64_t t3_u = now_ns_since_1970();
            const uint64_t t0_u = ntp_raw_to_ns(get_u64_be(response, 8));
            const uint64_t t1_u = remote_ntp_to_ns(get_u64_be(response, 16));
            const uint64_t t2_u = remote_ntp_to_ns(get_u64_be(response, 24));

            recv_time       = t3_u;
            client_ref_time = get_u64_be(response, 24);
            ++probes_received;

            if (t0_u && t1_u && t2_u && t3_u) {
                const int64_t t0 = static_cast<int64_t>(t0_u);
                const int64_t t1 = static_cast<int64_t>(t1_u);
                const int64_t t2 = static_cast<int64_t>(t2_u);
                const int64_t t3 = static_cast<int64_t>(t3_u);

                const int64_t offset = ((t1 - t0) + (t2 - t3)) / 2;
                const int64_t delay  = (t3 - t0) - (t2 - t1);

                samples[sample_index] = ClockSample{offset, delay, true};
                sample_index = (sample_index + 1) % samples.size();
                if (sample_count < samples.size()) {
                    ++sample_count;
                }

                // As UxPlay does, prefer the sample with the lowest RTT.
                // This rejects asymmetric queueing spikes without smearing
                // the media clock with a large moving average.
                const ClockSample* best = nullptr;
                for (const auto& sample : samples) {
                    if (!sample.valid) {
                        continue;
                    }
                    if (!best || sample.delay_ns < best->delay_ns) {
                        best = &sample;
                    }
                }

                if (best) {
                    const bool first_sync = !synchronized();
                    const int64_t previous =
                        offset_ns_.exchange(best->offset_ns,
                                            std::memory_order_acq_rel);
                    synchronized_.store(true, std::memory_order_release);

                    if (first_sync) {
                        LOG_INFO << "NTP clock synchronized: rtt="
                                 << (static_cast<double>(best->delay_ns) / 1'000'000.0)
                                 << " ms";
                    } else if ((probes_received % 10) == 0) {
                        const int64_t correction = best->offset_ns - previous;
                        LOG_INFO << "NTP clock: rtt="
                                 << (static_cast<double>(best->delay_ns) / 1'000'000.0)
                                 << " ms correction="
                                 << (static_cast<double>(correction) / 1'000'000.0)
                                 << " ms";
                    }
                }
            }
        }

        if ((probes_sent % 10) == 0 && probes_sent != 0) {
            LOG_INFO << "NTP poll: sent=" << probes_sent
                     << " received=" << probes_received;
        }

        // Sleep up to 3 sec, but stop promptly when teardown begins.
        constexpr auto kSlice = std::chrono::milliseconds(100);
        const int slices = static_cast<int>(kPollInterval / kSlice);
        for (int i = 0; i < slices && running_; ++i) {
            std::this_thread::sleep_for(kSlice);
        }
    }

    LOG_INFO << "NtpClient thread exiting";
    LOG_INFO << "NTP client stopped (sent=" << probes_sent
             << ", received=" << probes_received << ')';
}

} // namespace ap::airplay
