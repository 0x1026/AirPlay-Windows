#pragma once

#include "audio/aac_decoder.h"
#include "audio/audio_output.h"
#include "net/socket.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare OpenSSL type to keep this header self-contained.
struct evp_cipher_ctx_st;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace ap::airplay { class NtpClient; }
namespace ap::video   { class VideoRenderer; }

namespace ap::audio {

// UDP audio receiver for the AirPlay RAOP stream (type 96).
//
// The network thread receives both RTP audio and RAOP control/sync packets.
// Decoded PCM is held in a timestamped queue until the playout thread maps
// RTP time -> iOS NTP time -> local wall-clock time and queues the samples to
// SDL near their intended presentation time.
class AudioReceiver {
public:
    struct Config {
        socket_t                    data_sock    = INVALID_SOCK; // ownership transferred
        socket_t                    control_sock = INVALID_SOCK; // ownership transferred
        std::vector<unsigned char>  aes_key;                    // 16 B
        std::vector<unsigned char>  aes_iv;                     // 16 B
        int                         ct          = 0;             // compression type
        int                         sample_rate = 44100;

        // Non-owning. StreamSession keeps NtpClient alive until after the
        // AudioReceiver has stopped.
        ap::airplay::NtpClient*     ntp         = nullptr;

        // Non-owning renderer hook for play/pause state and audio visualizer.
        ap::video::VideoRenderer*   renderer    = nullptr;
    };

    AudioReceiver();
    ~AudioReceiver();

    AudioReceiver(const AudioReceiver&)            = delete;
    AudioReceiver& operator=(const AudioReceiver&) = delete;

    bool start(Config cfg);
    void stop();

    // Thread-safe: forwards to SdlAudioOutput when it exists.
    void set_volume_db(float db);

private:
    struct SyncAnchor {
        bool     valid         = false;
        uint32_t rtp_sync      = 0;
        uint64_t remote_ntp_ns = 0;
        uint32_t next_rtp      = 0;
        uint64_t generation    = 0;
    };

    struct PcmFrame {
        uint32_t             rtp_timestamp = 0;
        std::vector<int16_t> samples;
    };

    void network_thread_fn();
    void playout_thread_fn();
    void process_control_packet(const unsigned char* packet, int len);
    void process_audio_packet(const unsigned char* packet, int len);
    bool current_sync_anchor(SyncAnchor& out) const;

    Config                    cfg_;
    std::atomic<bool>         running_{false};
    std::thread               network_thread_;
    std::thread               playout_thread_;

    EVP_CIPHER_CTX*               aes_ctx_{nullptr};
    std::unique_ptr<AacDecoder>   decoder_;
    std::unique_ptr<SdlAudioOutput> output_;

    mutable std::mutex sync_mtx_;
    SyncAnchor         sync_anchor_;

    std::mutex              pcm_mtx_;
    std::condition_variable pcm_cv_;
    std::deque<PcmFrame>    pcm_queue_;

    // Telemetry shared by the network thread only; stored here so helper
    // methods can update it without an oversized parameter list.
    uint64_t pkts_{0};
    uint64_t dedup_dropped_{0};
    uint64_t total_bytes_{0};
    uint64_t no_data_markers_{0};
    uint64_t alac_preroll_packets_{0};
    uint64_t pcm_overflow_dropped_{0};
    uint64_t hist_0_16_{0};
    uint64_t hist_17_64_{0};
    uint64_t hist_65_256_{0};
    uint64_t hist_257_512_{0};
    uint64_t hist_513_plus_{0};
    int      big_pkts_logged_{0};

    std::vector<bool> seen_seq_ = std::vector<bool>(65536, false);
    uint16_t highest_seq_{0};
    bool have_highest_seq_{false};
};

// Human-readable label for a RAOP "ct" (compression type) value.
const char* ct_name(int ct);

} // namespace ap::audio
