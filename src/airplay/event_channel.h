#pragma once

#include "net/socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ap::airplay {

// AirPlay MediaRemote command numbers used by receiver -> sender
// `sendMediaRemoteCommand` requests.
enum class MediaRemoteCommand : uint32_t {
    Play       = 0,
    Pause      = 1,
    TogglePlay = 2,
    Stop       = 3,
    Next       = 4,
    Previous   = 5,
};

const char* media_remote_command_name(MediaRemoteCommand command);

// Event channel used by this receiver's current AirPlay "Legacy Pairing"
// path (features bit 27, CoreUtils/HomeKit pairing bits disabled).
//
// UxPlay/RPiPlay's legacy SETUP advertises an RTSP eventPort without enabling
// the native AirPlay-2 HAP/CoreUtils channel encryption. The sender opens a
// second TCP connection to the advertised eventPort and RTSP messages on that
// connection remain plaintext. This is deliberately different from native
// AirPlay 2 / HomeKit-paired sessions, whose event channel uses
// Events-Salt + ChaCha20-Poly1305 framing.
//
// We keep a dedicated ephemeral listener instead of reusing the main RAOP
// listening port so the socket is owned explicitly and can be used by SMTC to
// send receiver -> sender POST /command requests.
class AirPlayEventChannel {
public:
    AirPlayEventChannel();
    ~AirPlayEventChannel();

    AirPlayEventChannel(const AirPlayEventChannel&) = delete;
    AirPlayEventChannel& operator=(const AirPlayEventChannel&) = delete;

    // `pair_verify_secret` is retained for API compatibility with
    // StreamSession. It is intentionally unused in Legacy Pairing mode.
    bool start(uint16_t& local_port,
               const std::vector<unsigned char>& pair_verify_secret);
    void stop();

    bool connected() const { return connected_.load(); }

    // Queue the receiver's system-information update. The caller passes the
    // binary plist value and this class wraps it as:
    //   { "type": "updateInfo", "value": <dict> }
    bool send_update_info(const std::vector<unsigned char>& info_plist);

    // Thread-safe. Sends a binary-plist `sendMediaRemoteCommand` request to
    // the currently connected AirPlay sender and waits for its RTSP response.
    bool send_media_remote_command(MediaRemoteCommand command);

private:
    void accept_loop();
    void drain_connection(socket_t fd);

    bool send_plain_locked(const unsigned char* data, std::size_t len);
    bool send_plist_request(const std::vector<unsigned char>& body,
                            const char* description,
                            uint32_t& cseq_out,
                            int ack_timeout_ms);
    bool wait_for_response(uint32_t cseq, int timeout_ms, int& status_code);
    void publish_response(bool has_cseq, uint32_t cseq, int status_code);
    bool send_rtsp_ok(bool has_cseq, uint32_t cseq);

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};

    socket_t listener_{INVALID_SOCK};
    socket_t channel_{INVALID_SOCK};
    uint16_t local_port_{0};
    uint32_t cseq_{0};

    std::thread thread_;
    mutable std::mutex channel_mtx_;

    std::mutex response_mtx_;
    std::condition_variable response_cv_;
    std::unordered_map<uint32_t, int> responses_;
    std::deque<int> uncorrelated_responses_;

    // Windows may deliver repeated SMTC callbacks while a request is waiting
    // for its network ACK. Do not build an unbounded command backlog.
    std::mutex command_mtx_;

    std::mutex update_info_mtx_;
    std::vector<unsigned char> update_info_plist_;
    std::atomic<bool> update_info_sent_{false};
    std::atomic<bool> update_info_acked_{false};
    std::atomic<uint32_t> update_info_cseq_{0xffffffffu};
};

// The app currently supports one active AirPlay playback session at a time.
void set_active_event_channel(const std::shared_ptr<AirPlayEventChannel>& channel);
void clear_active_event_channel(const std::shared_ptr<AirPlayEventChannel>& channel);
bool send_active_media_remote_command(MediaRemoteCommand command);

} // namespace ap::airplay
