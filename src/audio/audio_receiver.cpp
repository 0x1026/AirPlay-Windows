#include "audio/audio_receiver.h"

#include "airplay/ntp_client.h"
#include "log.h"
#include "video/video_renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include <openssl/evp.h>

#if defined(_WIN32)
    #include <winsock2.h>
#else
    #include <sys/socket.h>
    #include <sys/time.h>
    #include <errno.h>
#endif

namespace ap::audio {
namespace {

constexpr int      kSelectTimeoutMs       = 200;
constexpr int      kSilencePauseMs        = 500;
constexpr int      kActiveAudioMinBytes   = 100;
constexpr uint64_t kNanosPerSecond        = 1'000'000'000ULL;
constexpr uint64_t kSinkLeadNs            = 25'000'000ULL;   // ~one SDL buffer @44.1k
constexpr uint64_t kLateDropNs            = 250'000'000ULL;  // don't replay stale PCM
constexpr std::size_t kMaxBufferedFrames  = 4096;

uint64_t now_ns_since_1970() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

uint32_t get_u32_be(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
         | (static_cast<uint32_t>(p[1]) << 16)
         | (static_cast<uint32_t>(p[2]) << 8)
         |  static_cast<uint32_t>(p[3]);
}

uint64_t get_u64_be(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | p[i];
    }
    return v;
}

std::string hex_dump(const unsigned char* b,
                     std::size_t n,
                     std::size_t max = 32) {
    std::ostringstream os;
    for (std::size_t i = 0; i < n && i < max; ++i) {
        char tmp[4];
        std::snprintf(tmp, sizeof(tmp), "%02x ", b[i]);
        os << tmp;
    }
    if (n > max) {
        os << "...(" << n << "B)";
    }
    return os.str();
}

bool is_aac_eld_no_data_marker(const unsigned char* payload,
                               int payload_len,
                               int ct) {
    if (ct != 4 && ct != 8) {
        return false;
    }

    return payload_len == 4
        && payload[0] == 0x00
        && payload[1] == 0x68
        && payload[2] == 0x34
        && payload[3] == 0x00;
}

uint64_t queued_audio_ns(const SdlAudioOutput* output,
                         int sample_rate,
                         int channels) {
    if (!output || sample_rate <= 0 || channels <= 0) {
        return 0;
    }

    const uint64_t bytes = output->queued_bytes();
    const uint64_t bytes_per_second =
        static_cast<uint64_t>(sample_rate)
        * static_cast<uint64_t>(channels)
        * sizeof(int16_t);

    if (bytes_per_second == 0) {
        return 0;
    }

    return bytes * kNanosPerSecond / bytes_per_second;
}

} // namespace

const char* ct_name(int ct) {
    switch (ct) {
        case 0: return "unspecified";
        case 1: return "PCM";
        case 2: return "ALAC";
        case 3: return "AAC-LC";
        case 4: return "AAC-ELD";
        case 8: return "AAC-ELD 44.1k";
        default: return "unknown";
    }
}

AudioReceiver::AudioReceiver() = default;

AudioReceiver::~AudioReceiver() {
    stop();
}

bool AudioReceiver::start(Config cfg) {
    if (cfg.data_sock == INVALID_SOCK) {
        LOG_ERROR << "AudioReceiver: invalid data socket";
        return false;
    }

    if (cfg.control_sock == INVALID_SOCK) {
        LOG_ERROR << "AudioReceiver: invalid control socket";
        return false;
    }

    if (!cfg.ntp) {
        LOG_ERROR << "AudioReceiver: NTP clock is required for synchronized playout";
        return false;
    }

    if (cfg.aes_key.size() != 16 || cfg.aes_iv.size() != 16) {
        LOG_ERROR << "AudioReceiver: aes_key/aes_iv must be 16 B "
                  << "(got " << cfg.aes_key.size() << '/'
                  << cfg.aes_iv.size() << ')';
        return false;
    }

    cfg_ = std::move(cfg);

    aes_ctx_ = EVP_CIPHER_CTX_new();
    if (!aes_ctx_ ||
        EVP_DecryptInit_ex(aes_ctx_, EVP_aes_128_cbc(), nullptr,
                           cfg_.aes_key.data(), cfg_.aes_iv.data()) != 1) {
        LOG_ERROR << "AudioReceiver: EVP_DecryptInit_ex(aes-128-cbc) failed";
        if (aes_ctx_) {
            EVP_CIPHER_CTX_free(aes_ctx_);
            aes_ctx_ = nullptr;
        }
        return false;
    }
    EVP_CIPHER_CTX_set_padding(aes_ctx_, 0);

    decoder_ = std::make_unique<AacDecoder>();
    AacDecoder::Config dc;
    dc.ct          = cfg_.ct;
    dc.sample_rate = cfg_.sample_rate;
    dc.channels    = 2;
    dc.spf         = 480;

    if (!decoder_->init(dc)) {
        LOG_WARN << "AudioReceiver: decoder init failed — decrypt-only mode";
        decoder_.reset();
    }

    if (decoder_) {
        output_ = std::make_unique<SdlAudioOutput>();
        if (!output_->start(cfg_.sample_rate, 2)) {
            LOG_WARN << "AudioReceiver: SDL audio output unavailable — "
                        "PCM will be decoded but not played";
            output_.reset();
        }
    }

    {
        std::lock_guard<std::mutex> lock(sync_mtx_);
        sync_anchor_ = {};
    }
    {
        std::lock_guard<std::mutex> lock(pcm_mtx_);
        pcm_queue_.clear();
    }

    pkts_                  = 0;
    dedup_dropped_         = 0;
    total_bytes_           = 0;
    no_data_markers_       = 0;
    alac_preroll_packets_  = 0;
    pcm_overflow_dropped_  = 0;
    hist_0_16_             = 0;
    hist_17_64_            = 0;
    hist_65_256_           = 0;
    hist_257_512_          = 0;
    hist_513_plus_         = 0;
    big_pkts_logged_       = 0;
    std::fill(seen_seq_.begin(), seen_seq_.end(), false);
    highest_seq_ = 0;
    have_highest_seq_ = false;

    running_.store(true, std::memory_order_release);

    playout_thread_ = std::thread(&AudioReceiver::playout_thread_fn, this);
    network_thread_ = std::thread(&AudioReceiver::network_thread_fn, this);

    LOG_INFO << "AudioReceiver listening (ct=" << cfg_.ct
             << ' ' << ct_name(cfg_.ct)
             << ", sample_rate=" << cfg_.sample_rate
             << ", timestamped playout=on)";
    return true;
}

void AudioReceiver::set_volume_db(float db) {
    if (output_) {
        output_->set_volume_db(db);
    }
}

void AudioReceiver::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }

    // Do not overwrite cfg_.data_sock/control_sock until the network thread
    // has exited: it reads those descriptor values while building select().
    const socket_t data_sock    = cfg_.data_sock;
    const socket_t control_sock = cfg_.control_sock;

    if (data_sock != INVALID_SOCK) {
        ap::net::close_socket(data_sock);
    }
    if (control_sock != INVALID_SOCK) {
        ap::net::close_socket(control_sock);
    }

    pcm_cv_.notify_all();

    if (network_thread_.joinable()) {
        network_thread_.join();
    }
    if (playout_thread_.joinable()) {
        playout_thread_.join();
    }

    cfg_.data_sock    = INVALID_SOCK;
    cfg_.control_sock = INVALID_SOCK;

    {
        std::lock_guard<std::mutex> lock(pcm_mtx_);
        pcm_queue_.clear();
    }

    if (output_) {
        output_->stop();
        output_.reset();
    }

    if (decoder_) {
        decoder_.reset();
    }

    if (aes_ctx_) {
        EVP_CIPHER_CTX_free(aes_ctx_);
        aes_ctx_ = nullptr;
    }
}

bool AudioReceiver::current_sync_anchor(SyncAnchor& out) const {
    std::lock_guard<std::mutex> lock(sync_mtx_);
    if (!sync_anchor_.valid) {
        return false;
    }
    out = sync_anchor_;
    return true;
}

void AudioReceiver::process_control_packet(const unsigned char* packet, int len) {
    if (!packet || len < 2) {
        return;
    }

    const uint8_t type = static_cast<uint8_t>(packet[1] & 0x7f);

    // RAOP RTP sync packet, matching UxPlay's raop_rtp.c:
    //   [4..7]   sync RTP timestamp
    //   [8..15]  remote NTP timestamp
    //   [16..19] next RTP timestamp
    if (type == 0x54 && len >= 20) {
        const uint32_t rtp_sync = get_u32_be(packet + 4);
        const uint64_t ntp_raw  = get_u64_be(packet + 8);
        const uint32_t next_rtp = get_u32_be(packet + 16);

        const uint64_t remote_ntp_ns =
            cfg_.ntp ? cfg_.ntp->remote_ntp_to_ns(ntp_raw) : 0;

        if (!remote_ntp_ns) {
            LOG_WARN << "RAOP sync: invalid remote NTP timestamp";
            return;
        }

        bool first = false;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(sync_mtx_);
            first = !sync_anchor_.valid;
            sync_anchor_.valid         = true;
            sync_anchor_.rtp_sync      = rtp_sync;
            sync_anchor_.remote_ntp_ns = remote_ntp_ns;
            sync_anchor_.next_rtp      = next_rtp;
            generation = ++sync_anchor_.generation;
        }

        const int32_t lead_ticks =
            static_cast<int32_t>(next_rtp - rtp_sync);
        const double lead_ms =
            static_cast<double>(lead_ticks) * 1000.0 /
            static_cast<double>(cfg_.sample_rate);

        LOG_INFO << "RAOP audio sync" << (first ? " (first)" : "")
                 << ": rtp=" << rtp_sync
                 << " next=" << next_rtp
                 << " lead=" << lead_ms << " ms"
                 << " generation=" << generation;

        pcm_cv_.notify_all();
        return;
    }

    // 0x56 is a resent RTP packet. We do not request resends yet, so seeing
    // one is informational only; reliability/reorder is a separate change.
    if (type == 0x56) {
        return;
    }
}

void AudioReceiver::process_audio_packet(const unsigned char* packet, int len) {
    if (!packet || len < 12) {
        return;
    }

    const uint8_t pt = static_cast<uint8_t>(packet[1] & 0x7f);
    if (pt != 96) {
        return;
    }

    const uint16_t seq =
        static_cast<uint16_t>((static_cast<uint16_t>(packet[2]) << 8)
                            |  static_cast<uint16_t>(packet[3]));
    const uint32_t ts = get_u32_be(packet + 4);

    const int payload_len = len - 12;
    if (payload_len <= 0) {
        return;
    }

    ++pkts_;
    total_bytes_ += static_cast<uint64_t>(payload_len);

    if (payload_len <= 16) {
        ++hist_0_16_;
    } else if (payload_len <= 64) {
        ++hist_17_64_;
    } else if (payload_len <= 256) {
        ++hist_65_256_;
    } else if (payload_len <= 512) {
        ++hist_257_512_;
    } else {
        ++hist_513_plus_;
    }

    // UxPlay ignores 44-byte ALAC RTP packets (12-byte RTP + 32-byte payload):
    // they are startup/preroll format information, not audio to render.
    if (cfg_.ct == 2 && len == 44) {
        ++alac_preroll_packets_;
        return;
    }

    // AAC-ELD can use a plaintext 4-byte no-data marker.
    if (is_aac_eld_no_data_marker(packet + 12, payload_len, cfg_.ct)) {
        ++no_data_markers_;
        return;
    }

    // Advance the deduplication window by RTP sequence, not packet count.
    // Duplicates/preroll can skip a count-based cleanup and leave stale bits
    // that reject new audio after the 16-bit sequence number wraps.
    constexpr uint16_t kSequenceWindow = 8192;
    if (!have_highest_seq_) {
        highest_seq_ = seq;
        have_highest_seq_ = true;
    } else {
        const uint16_t advance = static_cast<uint16_t>(seq - highest_seq_);
        if (advance != 0 && advance < 0x8000) {
            if (advance >= kSequenceWindow) {
                std::fill(seen_seq_.begin(), seen_seq_.end(), false);
            } else {
                for (uint16_t step = 1; step <= advance; ++step) {
                    seen_seq_[static_cast<uint16_t>(
                        highest_seq_ + step - kSequenceWindow)] = false;
                }
            }
            highest_seq_ = seq;
        } else if (static_cast<uint16_t>(highest_seq_ - seq) >= kSequenceWindow) {
            ++dedup_dropped_;
            return; // Too old to belong to the retained reorder window.
        }
    }
    if (seen_seq_[seq]) {
        ++dedup_dropped_;
        return;
    }
    seen_seq_[seq] = true;

    const int encrypted_len = (payload_len / 16) * 16;
    const int tail_len      = payload_len - encrypted_len;

    unsigned char cleartext[4096];
    int outlen = 0;

    if (EVP_DecryptInit_ex(aes_ctx_, nullptr, nullptr, nullptr,
                           cfg_.aes_iv.data()) != 1) {
        LOG_WARN << "AudioReceiver: EVP_DecryptInit_ex reset failed";
        return;
    }

    if (encrypted_len > 0) {
        if (EVP_DecryptUpdate(aes_ctx_, cleartext, &outlen,
                              packet + 12, encrypted_len) != 1) {
            LOG_WARN << "AudioReceiver: EVP_DecryptUpdate failed";
            return;
        }
    }

    if (tail_len > 0) {
        std::memcpy(cleartext + outlen,
                    packet + 12 + encrypted_len,
                    static_cast<std::size_t>(tail_len));
        outlen += tail_len;
    }

    if (len >= kActiveAudioMinBytes) {
        if (cfg_.renderer && !cfg_.renderer->in_flush_grace()) {
            cfg_.renderer->push_playback_rate(1.0f);
        }
    }

    if (big_pkts_logged_ < 5 && payload_len >= 100) {
        LOG_INFO << "audio pkt#" << pkts_
                 << ": pt=" << static_cast<int>(pt)
                 << " seq=" << seq
                 << " ts=" << ts
                 << " paylen=" << payload_len
                 << " (enc=" << encrypted_len
                 << " tail=" << tail_len << ')';
        LOG_INFO << "  clear: "
                 << hex_dump(cleartext, static_cast<std::size_t>(outlen));
        ++big_pkts_logged_;
    }

    if (!decoder_) {
        return;
    }

    const int got = decoder_->decode(cleartext, outlen);
    if (got <= 0) {
        return;
    }

    if (decoder_->frames_decoded() <= 3) {
        LOG_INFO << "  decoded " << got << " samples (" << got / 2
                 << " per channel) — total PCM frames out: "
                 << decoder_->frames_decoded();
    }

    int16_t pcm[8192];
    int have = 0;
    uint32_t sample_offset = 0;

    while ((have = decoder_->pull_pcm_s16(
                pcm,
                static_cast<int>(sizeof(pcm) / sizeof(pcm[0])))) > 0) {
        PcmFrame frame;
        frame.rtp_timestamp = ts + sample_offset;
        frame.samples.assign(pcm, pcm + have);

        const uint32_t frames = static_cast<uint32_t>(have / 2);
        sample_offset += frames;

        {
            std::lock_guard<std::mutex> lock(pcm_mtx_);
            if (pcm_queue_.size() >= kMaxBufferedFrames) {
                pcm_queue_.pop_front();
                ++pcm_overflow_dropped_;
                if (pcm_overflow_dropped_ == 1 ||
                    (pcm_overflow_dropped_ % 128) == 0) {
                    LOG_WARN << "audio sync queue overflow: dropped "
                             << pcm_overflow_dropped_ << " old PCM frame(s)";
                }
            }
            pcm_queue_.push_back(std::move(frame));
        }
        pcm_cv_.notify_one();
    }
}

void AudioReceiver::network_thread_fn() {
    auto last_active_audio = std::chrono::steady_clock::now();
    bool audio_ever_seen = false;

    unsigned char data_packet[4096];
    unsigned char control_packet[2048];

    while (running_.load(std::memory_order_acquire)) {
        fd_set rfds;
        FD_ZERO(&rfds);

        if (cfg_.data_sock != INVALID_SOCK) {
            FD_SET(cfg_.data_sock, &rfds);
        }
        if (cfg_.control_sock != INVALID_SOCK) {
            FD_SET(cfg_.control_sock, &rfds);
        }

        timeval tv{};
        tv.tv_sec  = kSelectTimeoutMs / 1000;
        tv.tv_usec = (kSelectTimeoutMs % 1000) * 1000;

#if defined(_WIN32)
        const int selected = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
        const int nfds =
            std::max(cfg_.data_sock, cfg_.control_sock) + 1;
        const int selected = ::select(nfds, &rfds, nullptr, nullptr, &tv);
#endif

        if (selected < 0) {
            if (running_.load(std::memory_order_acquire)) {
                LOG_WARN << "AudioReceiver: select() failed";
            }
            break;
        }

        if (selected == 0) {
            if (cfg_.renderer && audio_ever_seen) {
                const auto idle =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - last_active_audio
                    ).count();
                if (idle >= kSilencePauseMs) {
                    cfg_.renderer->push_playback_rate(0.0f);
                }
            }
            continue;
        }

        if (cfg_.control_sock != INVALID_SOCK &&
            FD_ISSET(cfg_.control_sock, &rfds)) {
            const int n = ::recvfrom(
                cfg_.control_sock,
                reinterpret_cast<char*>(control_packet),
                sizeof(control_packet),
                0,
                nullptr,
                nullptr
            );
            if (n > 0) {
                process_control_packet(control_packet, n);
            }
        }

        if (cfg_.data_sock != INVALID_SOCK &&
            FD_ISSET(cfg_.data_sock, &rfds)) {
            const int n = ::recvfrom(
                cfg_.data_sock,
                reinterpret_cast<char*>(data_packet),
                sizeof(data_packet),
                0,
                nullptr,
                nullptr
            );

            if (n > 0) {
                if (n >= kActiveAudioMinBytes) {
                    last_active_audio = std::chrono::steady_clock::now();
                    audio_ever_seen = true;
                }
                process_audio_packet(data_packet, n);
            }
        }

        if ((pkts_ % 500) == 0 && pkts_ != 0) {
            static thread_local uint64_t last_logged_pkts = 0;
            if (pkts_ != last_logged_pkts) {
                last_logged_pkts = pkts_;
                std::size_t queued_frames = 0;
                {
                    std::lock_guard<std::mutex> lock(pcm_mtx_);
                    queued_frames = pcm_queue_.size();
                }
                LOG_INFO << "audio: " << pkts_ << " pkts, "
                         << (total_bytes_ / 1024) << " KB received, "
                         << queued_frames << " PCM frame(s) waiting for playout";
            }
        }
    }

    LOG_INFO << "AudioReceiver network stopped (" << pkts_ << " pkts, "
             << total_bytes_ << " payload bytes, "
             << dedup_dropped_ << " duplicates dropped, "
             << alac_preroll_packets_ << " ALAC preroll packets ignored, "
             << no_data_markers_ << " AAC-ELD no-data markers, "
             << (decoder_ ? decoder_->frames_decoded() : 0)
             << " PCM frames decoded)";
    LOG_INFO << "  payload size histogram: "
             << "[0..16]=" << hist_0_16_
             << "  [17..64]=" << hist_17_64_
             << "  [65..256]=" << hist_65_256_
             << "  [257..512]=" << hist_257_512_
             << "  [513+]=" << hist_513_plus_;
}

void AudioReceiver::playout_thread_fn() {
    uint64_t played_frames = 0;
    uint64_t late_dropped  = 0;
    bool waiting_logged    = false;

    while (running_.load(std::memory_order_acquire)) {
        uint32_t front_rtp = 0;

        {
            std::unique_lock<std::mutex> lock(pcm_mtx_);
            pcm_cv_.wait_for(
                lock,
                std::chrono::milliseconds(20),
                [this]() {
                    return !running_.load(std::memory_order_acquire)
                        || !pcm_queue_.empty();
                }
            );

            if (!running_.load(std::memory_order_acquire)) {
                break;
            }
            if (pcm_queue_.empty()) {
                continue;
            }

            front_rtp = pcm_queue_.front().rtp_timestamp;
        }

        SyncAnchor anchor;
        const bool have_anchor = current_sync_anchor(anchor);
        const bool have_ntp = cfg_.ntp && cfg_.ntp->synchronized();

        if (!have_anchor || !have_ntp) {
            if (!waiting_logged) {
                LOG_INFO << "audio playout: waiting for RTP/NTP sync anchor";
                waiting_logged = true;
            }
            std::unique_lock<std::mutex> lock(pcm_mtx_);
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }

        if (waiting_logged) {
            LOG_INFO << "audio playout: clock synchronized, starting timestamped output";
            waiting_logged = false;
        }

        const int32_t rtp_delta =
            static_cast<int32_t>(front_rtp - anchor.rtp_sync);
        const int64_t delta_ns =
            static_cast<int64_t>(rtp_delta)
            * static_cast<int64_t>(kNanosPerSecond)
            / static_cast<int64_t>(cfg_.sample_rate);

        const int64_t remote_target_signed =
            static_cast<int64_t>(anchor.remote_ntp_ns) + delta_ns;
        if (remote_target_signed <= 0) {
            std::lock_guard<std::mutex> lock(pcm_mtx_);
            if (!pcm_queue_.empty()) {
                pcm_queue_.pop_front();
            }
            ++late_dropped;
            continue;
        }

        const uint64_t local_target = cfg_.ntp->remote_to_local_ns(
            static_cast<uint64_t>(remote_target_signed));
        if (!local_target) {
            std::unique_lock<std::mutex> lock(pcm_mtx_);
            pcm_cv_.wait_for(lock, std::chrono::milliseconds(20));
            continue;
        }

        const uint64_t now = now_ns_since_1970();
        const uint64_t queued_ns =
            queued_audio_ns(output_.get(), cfg_.sample_rate, 2);

        // If queued now, this frame starts after already-queued PCM. Account
        // for roughly one SDL/WASAPI device buffer so the audible start lands
        // close to the AirPlay timestamp instead of one hardware buffer late.
        const uint64_t estimated_start = now + queued_ns + kSinkLeadNs;

        if (estimated_start + 1'000'000ULL < local_target) {
            const uint64_t wait_ns = local_target - estimated_start;
            const auto wait_for = std::chrono::nanoseconds(
                static_cast<int64_t>(std::min<uint64_t>(
                    wait_ns,
                    20'000'000ULL
                ))
            );

            std::unique_lock<std::mutex> lock(pcm_mtx_);
            pcm_cv_.wait_for(lock, wait_for);
            continue;
        }

        if (estimated_start > local_target + kLateDropNs) {
            {
                std::lock_guard<std::mutex> lock(pcm_mtx_);
                if (!pcm_queue_.empty()) {
                    pcm_queue_.pop_front();
                }
            }
            ++late_dropped;
            if (late_dropped == 1 || (late_dropped % 64) == 0) {
                const double late_ms =
                    static_cast<double>(estimated_start - local_target)
                    / 1'000'000.0;
                LOG_WARN << "audio playout: dropped " << late_dropped
                         << " late frame(s), latest=" << late_ms << " ms";
            }
            continue;
        }

        PcmFrame frame;
        {
            std::lock_guard<std::mutex> lock(pcm_mtx_);
            if (pcm_queue_.empty()) {
                continue;
            }
            frame = std::move(pcm_queue_.front());
            pcm_queue_.pop_front();
        }

        if (cfg_.renderer && !frame.samples.empty()) {
            cfg_.renderer->push_audio_pcm(
                frame.samples.data(),
                static_cast<int>(frame.samples.size()),
                cfg_.sample_rate,
                2
            );
        }

        if (output_ && !frame.samples.empty()) {
            output_->push(
                frame.samples.data(),
                static_cast<int>(frame.samples.size())
            );
        }

        ++played_frames;
        if ((played_frames % 500) == 0) {
            const double queue_ms =
                static_cast<double>(queued_audio_ns(
                    output_.get(), cfg_.sample_rate, 2)) / 1'000'000.0;
            LOG_INFO << "audio playout: " << played_frames
                     << " PCM frames queued to SDL, sink_queue="
                     << queue_ms << " ms, late_dropped=" << late_dropped;
        }
    }

    LOG_INFO << "AudioReceiver playout stopped (played=" << played_frames
             << ", late_dropped=" << late_dropped << ')';
}

} // namespace ap::audio
