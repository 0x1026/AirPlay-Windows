#pragma once

#include "net/socket.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace ap::airplay {

// AirPlay NTP client — ported from UxPlay's raop_ntp.c (GPL-3.0).
//
// Contrary to what the name suggests, the SERVER is the NTP client in
// AirPlay: iOS runs a small NTP responder on its announced `timingPort`
// (advertised in the SETUP body), and we poll it every 3 seconds.
//
// Besides keeping the AirPlay session alive, the responses are used to
// estimate the offset between the iOS media clock and the local wall clock.
// The audio receiver combines this clock mapping with RAOP 0x54 RTP-sync
// packets so PCM is queued for the time at which iOS intended it to play.
class NtpClient {
public:
    NtpClient();
    ~NtpClient();

    NtpClient(const NtpClient&)            = delete;
    NtpClient& operator=(const NtpClient&) = delete;

    // Start polling. `sock` is our already-bound UDP socket (our
    // `timing_lport`), `remote_ip` is the iPhone's IP, `remote_port` is the
    // `timingPort` value iOS told us in the SETUP body. NtpClient does NOT
    // take ownership of the socket; StreamSession owns it.
    bool start(socket_t sock, const std::string& remote_ip, uint16_t remote_port);

    // Stops and joins the thread. Safe to call multiple times.
    void stop();

    // True once at least one valid NTP exchange has produced a clock offset.
    bool synchronized() const {
        return synchronized_.load(std::memory_order_acquire);
    }

    // Convert an AirPlay/NTP raw 64-bit timestamp (seconds since 1900 in the
    // high 32 bits, fractional seconds in the low 32 bits) to nanoseconds in
    // the iOS remote clock domain. AirPlay's NTP clock can use an arbitrary
    // epoch; the NTP offset below maps that domain to the local wall clock.
    uint64_t remote_ntp_to_ns(uint64_t ntp_timestamp) const;

    // Convert nanoseconds in the iOS remote clock domain to local Unix wall
    // clock nanoseconds. Returns 0 until NTP synchronization is available.
    uint64_t remote_to_local_ns(uint64_t remote_ns) const;

    // Remote minus local clock offset, in nanoseconds. Mostly useful for
    // diagnostics; only meaningful once synchronized() is true.
    int64_t offset_ns() const {
        return offset_ns_.load(std::memory_order_acquire);
    }

private:
    void thread_fn();

    socket_t          sock_{INVALID_SOCK};
    std::string       remote_ip_;
    uint16_t          remote_port_{0};
    std::atomic<bool> running_{false};
    std::thread       thread_;

    // NTP convention used here: offset = remote_clock - local_clock.
    std::atomic<bool>    synchronized_{false};
    std::atomic<int64_t> offset_ns_{0};
};

} // namespace ap::airplay
