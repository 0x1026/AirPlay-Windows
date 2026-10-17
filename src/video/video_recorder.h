#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" { struct AVFrame; }

namespace ap::video {

// Asynchronous FFmpeg-backed MP4 recorder. Decoded frames are cloned into a
// bounded queue so disk/encoder latency can never stall the AirPlay decoder.
class VideoRecorder {
public:
    enum class EncoderMode { Auto, Gpu, Cpu };
    enum class VideoCodec { H264, H265 };

    struct Options {
        EncoderMode encoder = EncoderMode::Auto;
        VideoCodec codec = VideoCodec::H264;
        int bitrate_mbps = 8;
    };

    struct Status {
        bool        recording = false;
        uint64_t    frames = 0;
        uint64_t    dropped = 0;
        uint64_t    audio_samples = 0;
        uint64_t    readback_frames = 0;
        bool        gpu_resident = false;
        int64_t     elapsed_ms = 0;
        std::string output_path;
        std::string codec;
        std::string error;
    };

    VideoRecorder() = default;
    ~VideoRecorder();

    VideoRecorder(const VideoRecorder&) = delete;
    VideoRecorder& operator=(const VideoRecorder&) = delete;

    bool start(const std::string& directory, Options options = {});
    void stop();

    void submit(const AVFrame* frame);
    void submit_i420(const uint8_t* y, int y_stride,
                     const uint8_t* u, int u_stride,
                     const uint8_t* v, int v_stride,
                     int width, int height);
    void submit_nv12(const uint8_t* y, int y_stride,
                     const uint8_t* uv, int uv_stride,
                     int width, int height);
    void submit_audio(const int16_t* interleaved_samples, int sample_count,
                      int sample_rate, int channels);

    Status status() const;
    static std::string default_directory();

private:
    struct QueuedFrame { AVFrame* frame = nullptr; int64_t pts_ms = 0; };
    struct QueuedAudio {
        std::vector<int16_t> samples;
        int sample_rate = 44100;
        int channels = 2;
        int64_t pts_ms = 0; // estimated beginning of this PCM block
    };
    void thread_fn();
    void set_error(const std::string& message);

    static constexpr std::size_t kMaxQueuedFrames = 90;

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::deque<QueuedFrame> queue_;
    std::deque<QueuedAudio> audio_queue_;
    std::thread             thread_;
    std::atomic<bool>       accepting_{false};
    bool                    stop_requested_{false};
    std::string             output_path_;
    std::string             codec_name_;
    std::string             error_;
    Options                 options_;
    std::atomic<uint64_t>   frames_written_{0};
    std::atomic<uint64_t>   frames_dropped_{0};
    std::atomic<uint64_t>   audio_samples_written_{0};
    std::atomic<uint64_t>   readback_frames_{0};
    std::atomic<bool>       gpu_resident_{false};
    std::atomic<int64_t>    started_ns_{0};
};

} // namespace ap::video
