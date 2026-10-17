#include "video/video_recorder.h"
#include "video/gpu_recording.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

namespace ap::video {
namespace {

int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string ff_error(int rc) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(rc, buf, sizeof(buf));
    return buf;
}

std::string timestamped_filename() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream os;
    os << "AirPlay-" << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S") << ".mp4";
    return os.str();
}

AVPixelFormat encoder_pixel_format(const AVCodec* codec, bool prefer_gpu_frames = false) {
    if (!codec) return AV_PIX_FMT_NONE;
    const AVPixelFormat* formats = nullptr;
#if LIBAVCODEC_VERSION_MAJOR >= 61
    const void* configs = nullptr;
    if (avcodec_get_supported_config(nullptr, codec,
            AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, nullptr) < 0) {
        return AV_PIX_FMT_NONE;
    }
    formats = static_cast<const AVPixelFormat*>(configs);
#else
    formats = codec->pix_fmts;
#endif
    // A null list means the encoder accepts every pixel format.
    if (!formats) return AV_PIX_FMT_YUV420P;
    if (prefer_gpu_frames) {
        for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p)
            if (*p == AV_PIX_FMT_D3D11) return *p;
    }
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_YUV420P) return AV_PIX_FMT_YUV420P;
    }
    for (const AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_NV12) return AV_PIX_FMT_NV12;
    }
    return AV_PIX_FMT_NONE;
}

void copy_plane(uint8_t* dst, int dst_stride, const uint8_t* src,
                int src_stride, int width, int height) {
    for (int row = 0; row < height; ++row) {
        std::memcpy(dst + static_cast<std::size_t>(row) * dst_stride,
                    src + static_cast<std::size_t>(row) * src_stride,
                    static_cast<std::size_t>(width));
    }
}

} // namespace

VideoRecorder::~VideoRecorder() { stop(); }

std::string VideoRecorder::default_directory() {
#if defined(_WIN32)
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length > 0 && length < path.size()) {
        return std::filesystem::path(
            std::wstring(path.data(), length)).parent_path().u8string();
    }
#else
    // On non-Windows builds the executable location is not available through
    // a single portable C++17 API; the launch directory is the closest match.
#endif
    return std::filesystem::current_path().u8string();
}

bool VideoRecorder::start(const std::string& directory, Options options) {
    stop();
    try {
        std::filesystem::path dir = std::filesystem::u8path(
            directory.empty() ? default_directory() : directory);
        std::filesystem::create_directories(dir);
        const auto path = dir / timestamped_filename();
        std::lock_guard<std::mutex> lock(mtx_);
        output_path_ = path.u8string();
        codec_name_.clear();
        error_.clear();
        options_ = options;
        options_.bitrate_mbps = std::clamp(options_.bitrate_mbps, 1, 100);
        stop_requested_ = false;
        frames_written_.store(0);
        frames_dropped_.store(0);
        audio_samples_written_.store(0);
        readback_frames_.store(0);
        gpu_resident_.store(false);
        started_ns_.store(steady_ns());
        accepting_.store(true);
        thread_ = std::thread(&VideoRecorder::thread_fn, this);
        LOG_INFO << "Recording armed: " << output_path_;
        return true;
    } catch (const std::exception& e) {
        set_error(std::string("Cannot create recording folder: ") + e.what());
        return false;
    }
}

void VideoRecorder::stop() {
    accepting_.store(false);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mtx_);
    while (!queue_.empty()) {
        av_frame_free(&queue_.front().frame);
        queue_.pop_front();
    }
    audio_queue_.clear();
}

void VideoRecorder::set_error(const std::string& message) {
    accepting_.store(false);
    std::lock_guard<std::mutex> lock(mtx_);
    error_ = message;
    LOG_ERROR << "Recorder: " << message;
}

void VideoRecorder::submit(const AVFrame* frame) {
    if (!accepting_.load(std::memory_order_relaxed) || !frame ||
        frame->width <= 0 || frame->height <= 0) return;
    AVFrame* clone = av_frame_clone(frame);
    if (!clone) return;
    const int64_t pts = std::max<int64_t>(
        0, (steady_ns() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!accepting_.load(std::memory_order_relaxed)) {
            av_frame_free(&clone);
            return;
        }
        // Hardware surfaces come from the decoder's bounded pool. Keeping
        // a long recording backlog would exhaust it and stall live playback.
        const std::size_t queue_limit = frame->hw_frames_ctx ? 2 : kMaxQueuedFrames;
        if (queue_.size() >= queue_limit) {
            av_frame_free(&queue_.front().frame);
            queue_.pop_front();
            frames_dropped_.fetch_add(1);
        }
        queue_.push_back({clone, pts});
    }
    cv_.notify_one();
}

void VideoRecorder::submit_i420(const uint8_t* y, int y_stride,
                                const uint8_t* u, int u_stride,
                                const uint8_t* v, int v_stride,
                                int width, int height) {
    if (!accepting_.load(std::memory_order_relaxed) || !y || !u || !v) return;
    AVFrame* f = av_frame_alloc();
    if (!f) return;
    f->format = AV_PIX_FMT_YUV420P; f->width = width; f->height = height;
    if (av_frame_get_buffer(f, 32) < 0) { av_frame_free(&f); return; }
    copy_plane(f->data[0], f->linesize[0], y, y_stride, width, height);
    copy_plane(f->data[1], f->linesize[1], u, u_stride, (width + 1) / 2,
               (height + 1) / 2);
    copy_plane(f->data[2], f->linesize[2], v, v_stride, (width + 1) / 2,
               (height + 1) / 2);
    submit(f);
    av_frame_free(&f);
}

void VideoRecorder::submit_nv12(const uint8_t* y, int y_stride,
                                const uint8_t* uv, int uv_stride,
                                int width, int height) {
    if (!accepting_.load(std::memory_order_relaxed) || !y || !uv) return;
    AVFrame* f = av_frame_alloc();
    if (!f) return;
    f->format = AV_PIX_FMT_NV12; f->width = width; f->height = height;
    if (av_frame_get_buffer(f, 32) < 0) { av_frame_free(&f); return; }
    copy_plane(f->data[0], f->linesize[0], y, y_stride, width, height);
    copy_plane(f->data[1], f->linesize[1], uv, uv_stride, width,
               (height + 1) / 2);
    submit(f);
    av_frame_free(&f);
}

void VideoRecorder::submit_audio(const int16_t* samples, int sample_count,
                                 int sample_rate, int channels) {
    if (!accepting_.load(std::memory_order_relaxed) || !samples ||
        sample_count <= 0 || sample_rate <= 0 || channels != 2) return;
    QueuedAudio audio;
    audio.samples.assign(samples, samples + sample_count);
    audio.sample_rate = sample_rate;
    audio.channels = channels;
    const int64_t now_ms = std::max<int64_t>(0,
        (steady_ns() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000);
    const int frames = sample_count / channels;
    audio.pts_ms = std::max<int64_t>(
        0, now_ms - static_cast<int64_t>(frames) * 1000 / sample_rate);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!accepting_.load(std::memory_order_relaxed)) return;
        // About ten seconds of stereo PCM is ample to absorb encoder stalls.
        std::size_t queued_values = 0;
        for (const auto& q : audio_queue_) queued_values += q.samples.size();
        while (!audio_queue_.empty() &&
               queued_values + audio.samples.size() >
                   static_cast<std::size_t>(sample_rate * channels * 10)) {
            queued_values -= audio_queue_.front().samples.size();
            audio_queue_.pop_front();
        }
        audio_queue_.push_back(std::move(audio));
    }
    cv_.notify_one();
}

VideoRecorder::Status VideoRecorder::status() const {
    Status s;
    s.recording = accepting_.load(std::memory_order_relaxed);
    s.frames = frames_written_.load(std::memory_order_relaxed);
    s.dropped = frames_dropped_.load(std::memory_order_relaxed);
    s.audio_samples = audio_samples_written_.load(std::memory_order_relaxed);
    s.readback_frames = readback_frames_.load(std::memory_order_relaxed);
    s.gpu_resident = gpu_resident_.load(std::memory_order_relaxed);
    if (s.recording) {
        s.elapsed_ms = std::max<int64_t>(0,
            (steady_ns() - started_ns_.load(std::memory_order_relaxed)) / 1'000'000);
    }
    std::lock_guard<std::mutex> lock(mtx_);
    s.output_path = output_path_;
    s.codec = codec_name_;
    s.error = error_;
    return s;
}

void VideoRecorder::thread_fn() {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* enc = nullptr;
    AVStream* stream = nullptr;
    AVCodecContext* audio_enc = nullptr;
    AVStream* audio_stream = nullptr;
    AVPacket* packet = av_packet_alloc();
    AVFrame* output = nullptr;
    AVFrame* audio_frame = nullptr;
    AVFrame* last_output = nullptr;
    SwsContext* sws = nullptr;
    GpuRecording gpu_converter;
    bool resident_video = false;
    int64_t first_pts = -1;
    int64_t last_pts = -1;
    int64_t audio_next_pts = 0;
    int audio_rate = 44100;
    bool audio_timeline_started = false;
    std::deque<int16_t> audio_pcm;
    bool header_written = false;
    bool failed = false;
    Options options;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        options = options_;
    }

    auto fail = [&](const std::string& msg) {
        failed = true;
        set_error(msg);
    };
    auto drain = [&](AVCodecContext* codec, AVStream* target_stream,
                     AVFrame* frame) {
        if (avcodec_send_frame(codec, frame) < 0) return false;
        while (true) {
            const int rc = avcodec_receive_packet(codec, packet);
            if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
            if (rc < 0) return false;
            av_packet_rescale_ts(packet, codec->time_base,
                                 target_stream->time_base);
            packet->stream_index = target_stream->index;
            const int wr = av_interleaved_write_frame(fmt, packet);
            av_packet_unref(packet);
            if (wr < 0) return false;
        }
        return true;
    };
    auto write_video = [&](int64_t pts_ms) {
        if (first_pts < 0) first_pts = pts_ms;
        output->pts = std::max<int64_t>(last_pts + 1, pts_ms - first_pts);
        last_pts = output->pts;
        if (!drain(enc, stream, output)) fail("Video encoding/muxing failed");
        else {
            frames_written_.fetch_add(1);
            if (last_output) av_frame_free(&last_output);
            last_output = av_frame_clone(output);
        }
    };

    while (true) {
        QueuedFrame item;
        bool have_video = false;
        bool finish_after_batch = false;
        std::deque<QueuedAudio> audio_batch;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [&] {
                return stop_requested_ || !queue_.empty() ||
                       (enc && !audio_queue_.empty());
            });
            if (!queue_.empty()) {
                item = queue_.front();
                queue_.pop_front();
                have_video = true;
            }
            if (enc || have_video) audio_batch.swap(audio_queue_);
            if (stop_requested_ && !enc && !have_video) {
                // Recording was stopped before the first video frame. No MP4
                // timeline can be created, so discard any early PCM cleanly.
                audio_queue_.clear();
            }
            finish_after_batch = stop_requested_ && queue_.empty() &&
                                 audio_queue_.empty();
        }

        if (!have_video && !enc) {
            if (finish_after_batch) break;
            continue;
        }

        AVFrame* input = have_video ? item.frame : nullptr;
        if (!enc) {
            const int out_w = input->width & ~1;
            const int out_h = input->height & ~1;
            std::string path;
            { std::lock_guard<std::mutex> lock(mtx_); path = output_path_; }
            int rc = avformat_alloc_output_context2(&fmt, nullptr, "mp4", path.c_str());
            if (rc < 0 || !fmt) { fail("MP4 initialization failed: " + ff_error(rc)); }

            static const char* h264_gpu_candidates[] = {
                "h264_nvenc", "h264_amf", "h264_qsv", "h264_mf"
            };
            static const char* h265_gpu_candidates[] = {
                "hevc_nvenc", "hevc_amf", "hevc_qsv", "hevc_mf"
            };
            static const char* h264_cpu_candidates[] = { "libx264", "mpeg4" };
            static const char* h265_cpu_candidates[] = { "libx265" };
            std::vector<const char*> candidates;
            std::vector<const char*> gpu_candidates;
            std::vector<const char*> cpu_candidates;
            if (options.codec == VideoCodec::H265) {
                gpu_candidates.assign(std::begin(h265_gpu_candidates),
                                      std::end(h265_gpu_candidates));
                cpu_candidates.assign(std::begin(h265_cpu_candidates),
                                      std::end(h265_cpu_candidates));
            } else {
                gpu_candidates.assign(std::begin(h264_gpu_candidates),
                                      std::end(h264_gpu_candidates));
                cpu_candidates.assign(std::begin(h264_cpu_candidates),
                                      std::end(h264_cpu_candidates));
            }
            if (options.encoder != EncoderMode::Cpu) {
                candidates.insert(candidates.end(), gpu_candidates.begin(),
                                  gpu_candidates.end());
            }
            if (options.encoder != EncoderMode::Gpu) {
                candidates.insert(candidates.end(), cpu_candidates.begin(),
                                  cpu_candidates.end());
            }
            const AVCodec* chosen = nullptr;
            bool chosen_is_gpu = false;
            const bool gpu_input = input->format == AV_PIX_FMT_D3D11 && input->hw_frames_ctx;
            const bool gpu_conversion_ready = gpu_input && options.encoder != EncoderMode::Cpu &&
                gpu_converter.init(input->hw_frames_ctx, out_w, out_h);
            if (!failed) {
                for (const char* name : candidates) {
                    const AVCodec* codec = avcodec_find_encoder_by_name(name);
                    const AVPixelFormat pixel_format = encoder_pixel_format(codec, gpu_conversion_ready);
                    if (pixel_format == AV_PIX_FMT_NONE) continue;
                    // Explicit GPU mode must never hide a readback of GPU input.
                    if (gpu_input && options.encoder == EncoderMode::Gpu &&
                        pixel_format != AV_PIX_FMT_D3D11) continue;
                    AVCodecContext* trial = avcodec_alloc_context3(codec);
                    if (!trial) continue;
                    trial->codec_id = codec->id;
                    trial->codec_type = AVMEDIA_TYPE_VIDEO;
                    trial->width = out_w; trial->height = out_h;
                    trial->pix_fmt = pixel_format;
                    if (pixel_format == AV_PIX_FMT_D3D11)
                        trial->hw_frames_ctx = gpu_converter.frames_ref();
                    trial->time_base = AVRational{1, 1000};
                    trial->framerate = AVRational{60, 1};
                    trial->bit_rate = static_cast<int64_t>(options.bitrate_mbps)
                                      * 1'000'000;
                    trial->rc_max_rate = trial->bit_rate;
                    trial->rc_buffer_size =
                        static_cast<int>(trial->bit_rate * 2);
                    trial->gop_size = 120;
                    trial->max_b_frames = 0;
                    if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
                        trial->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                    AVDictionary* opts = nullptr;
                    if (std::strcmp(name, "libx264") == 0 ||
                        std::strcmp(name, "libx265") == 0) {
                        av_dict_set(&opts, "preset", "veryfast", 0);
                        av_dict_set(&opts, "tune", "zerolatency", 0);
                    }
                    if (std::strcmp(name, "h264_nvenc") == 0 ||
                        std::strcmp(name, "hevc_nvenc") == 0) {
                        av_dict_set(&opts, "preset", "p4", 0);
                        av_dict_set(&opts, "tune", "ll", 0);
                        av_dict_set(&opts, "zerolatency", "1", 0);
                        av_dict_set(&opts, "delay", "0", 0);
                    }
                    rc = avcodec_open2(trial, codec, &opts);
                    av_dict_free(&opts);
                    if (rc == 0) {
                        enc = trial;
                        resident_video = pixel_format == AV_PIX_FMT_D3D11;
                        gpu_resident_.store(resident_video);
                        chosen = codec;
                        chosen_is_gpu =
                            std::find(gpu_candidates.begin(),
                                      gpu_candidates.end(), name)
                                != gpu_candidates.end();
                        break;
                    }
                    avcodec_free_context(&trial);
                }
                if (!enc) {
                    const char* codec_label = options.codec == VideoCodec::H265
                        ? "H.265/HEVC" : "H.264";
                    fail(std::string("No compatible ") + codec_label +
                         (options.encoder == EncoderMode::Gpu
                              ? " GPU encoder is available"
                              : " encoder is available"));
                }
            }
            if (!failed) {
                stream = avformat_new_stream(fmt, nullptr);
                if (!stream) fail("Cannot create the MP4 video stream");
            }
            if (!failed) {
                stream->time_base = enc->time_base;
                rc = avcodec_parameters_from_context(stream->codecpar, enc);
                if (rc < 0) fail("Cannot configure MP4 stream: " + ff_error(rc));
                if (options.codec == VideoCodec::H265) {
                    // hvc1 advertises that parameter sets are stored in the
                    // sample description, improving Windows/Apple playback.
                    stream->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
                }
            }
            if (!failed) {
                const AVCodec* audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
                if (!audio_codec) {
                    fail("AAC encoder is unavailable; audio cannot be recorded");
                } else {
                    audio_enc = avcodec_alloc_context3(audio_codec);
                }
                if (!audio_enc) fail("Cannot allocate AAC encoder");
            }
            if (!failed) {
                // AirPlay RAOP audio is stereo 44.1 kHz. The native FFmpeg
                // AAC encoder consumes planar float samples.
                audio_enc->sample_rate = audio_rate;
                audio_enc->sample_fmt = AV_SAMPLE_FMT_FLTP;
                audio_enc->time_base = AVRational{1, audio_rate};
                audio_enc->bit_rate = 192'000;
                av_channel_layout_default(&audio_enc->ch_layout, 2);
                if (fmt->oformat->flags & AVFMT_GLOBALHEADER)
                    audio_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                rc = avcodec_open2(audio_enc, nullptr, nullptr);
                if (rc < 0) fail("Cannot open AAC encoder: " + ff_error(rc));
            }
            if (!failed) {
                audio_stream = avformat_new_stream(fmt, nullptr);
                if (!audio_stream) {
                    fail("Cannot create the MP4 audio stream");
                } else {
                    audio_stream->time_base = audio_enc->time_base;
                    rc = avcodec_parameters_from_context(
                        audio_stream->codecpar, audio_enc);
                    if (rc < 0)
                        fail("Cannot configure MP4 audio: " + ff_error(rc));
                }
            }
            if (!failed && !(fmt->oformat->flags & AVFMT_NOFILE)) {
                rc = avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
                if (rc < 0) fail("Cannot open output file: " + ff_error(rc));
            }
            if (!failed) {
                AVDictionary* mux_opts = nullptr;
                av_dict_set(&mux_opts, "movflags", "+faststart", 0);
                rc = avformat_write_header(fmt, &mux_opts);
                av_dict_free(&mux_opts);
                if (rc < 0) fail("Cannot write MP4 header: " + ff_error(rc));
                else header_written = true;
            }
            if (!failed) {
                output = av_frame_alloc();
                if (output) {
                    output->format = enc->pix_fmt;
                    output->width = enc->width; output->height = enc->height;
                }
                if (!output || (!resident_video && av_frame_get_buffer(output, 32) < 0))
                    fail("Cannot allocate encoder frame");
            }
            if (!failed) {
                audio_frame = av_frame_alloc();
                if (audio_frame) {
                    audio_frame->format = audio_enc->sample_fmt;
                    audio_frame->sample_rate = audio_enc->sample_rate;
                    audio_frame->nb_samples = audio_enc->frame_size > 0
                        ? audio_enc->frame_size : 1024;
                    av_channel_layout_copy(&audio_frame->ch_layout,
                                           &audio_enc->ch_layout);
                }
                if (!audio_frame || av_frame_get_buffer(audio_frame, 0) < 0)
                    fail("Cannot allocate AAC frame");
            }
            if (!failed) {
                std::lock_guard<std::mutex> lock(mtx_);
                codec_name_ = chosen && chosen->long_name
                    ? chosen->long_name : (chosen ? chosen->name : "");
                codec_name_ += resident_video ? " [GPU resident]"
                    : chosen_is_gpu ? " [GPU encoder, CPU pixels]" : " [CPU]";
                LOG_INFO << "Recording started: " << path << " ("
                         << codec_name_ << ", " << out_w << 'x' << out_h
                         << ", " << options.bitrate_mbps << " Mbps)";
            }
        }

        // Download only for an explicitly selected CPU path or Auto fallback.
        // The resident encode path never calls transfer_data or swscale.
        if (input && input->hw_frames_ctx && !resident_video && !failed) {
            AVFrame* downloaded = av_frame_alloc();
            if (!downloaded || av_hwframe_transfer_data(downloaded, input, 0) < 0) {
                av_frame_free(&downloaded);
                fail("GPU recording frame download failed");
            } else {
                av_frame_copy_props(downloaded, input);
                av_frame_free(&input);
                input = downloaded;
                readback_frames_.fetch_add(1);
            }
        }
        if (have_video && !failed && resident_video) {
            if (!gpu_converter.convert(input, output))
                fail("GPU recording conversion failed; GPU-decoded frames are required");
            else write_video(item.pts_ms);
        } else if (have_video && !failed && av_frame_make_writable(output) >= 0) {
            for (int y = 0; y < output->height; ++y)
                std::memset(output->data[0] + y * output->linesize[0], 16,
                            static_cast<std::size_t>(output->width));
            for (int y = 0; y < output->height / 2; ++y) {
                const int chroma_width = output->format == AV_PIX_FMT_NV12
                    ? output->width : output->width / 2;
                std::memset(output->data[1] + y * output->linesize[1], 128,
                            static_cast<std::size_t>(chroma_width));
                if (output->format == AV_PIX_FMT_YUV420P) {
                    std::memset(output->data[2] + y * output->linesize[2], 128,
                                static_cast<std::size_t>(output->width / 2));
                }
            }

            const double src_ar = static_cast<double>(input->width) / input->height;
            const double dst_ar = static_cast<double>(output->width) / output->height;
            int fit_w = output->width, fit_h = output->height;
            if (src_ar > dst_ar) fit_h = static_cast<int>(fit_w / src_ar) & ~1;
            else fit_w = static_cast<int>(fit_h * src_ar) & ~1;
            fit_w = std::max(2, fit_w); fit_h = std::max(2, fit_h);
            const int x = ((output->width - fit_w) / 2) & ~1;
            const int y = ((output->height - fit_h) / 2) & ~1;
            sws = sws_getCachedContext(sws, input->width, input->height,
                static_cast<AVPixelFormat>(input->format), fit_w, fit_h,
                static_cast<AVPixelFormat>(output->format), SWS_BILINEAR,
                nullptr, nullptr, nullptr);
            uint8_t* dst[4]{};
            dst[0] = output->data[0] + y * output->linesize[0] + x;
            if (output->format == AV_PIX_FMT_NV12) {
                dst[1] = output->data[1] + (y / 2) * output->linesize[1] + x;
            } else {
                dst[1] = output->data[1] + (y / 2) * output->linesize[1] + x / 2;
                dst[2] = output->data[2] + (y / 2) * output->linesize[2] + x / 2;
            }
            if (!sws || sws_scale(sws, input->data, input->linesize, 0,
                                  input->height, dst, output->linesize) <= 0) {
                fail("Pixel conversion failed while recording");
            } else {
                write_video(item.pts_ms);
            }
        }
        av_frame_free(&input);

        // Queue PCM against the same zero point as the first video frame.
        // Only the first block needs timestamp correction; subsequent RAOP
        // PCM is gapless and follows its decoded sample cadence.
        if (!failed && audio_enc && first_pts >= 0) {
            for (auto& audio : audio_batch) {
                std::size_t skip_values = 0;
                if (!audio_timeline_started) {
                    const int64_t delta_ms = audio.pts_ms - first_pts;
                    if (delta_ms > 0) {
                        const int64_t silence_frames =
                            delta_ms * audio_rate / 1000;
                        audio_pcm.insert(audio_pcm.end(),
                            static_cast<std::size_t>(silence_frames * 2), 0);
                    } else if (delta_ms < 0) {
                        const int64_t skip_frames = std::min<int64_t>(
                            static_cast<int64_t>(audio.samples.size() / 2),
                            (-delta_ms) * audio_rate / 1000);
                        skip_values = static_cast<std::size_t>(skip_frames * 2);
                    }
                    audio_timeline_started = true;
                }
                audio_pcm.insert(audio_pcm.end(),
                    audio.samples.begin() +
                        static_cast<std::ptrdiff_t>(skip_values),
                    audio.samples.end());
            }

            const int frame_samples = audio_frame->nb_samples;
            while (audio_pcm.size() >=
                   static_cast<std::size_t>(frame_samples * 2)) {
                if (av_frame_make_writable(audio_frame) < 0) {
                    fail("Cannot prepare AAC audio frame");
                    break;
                }
                auto* left = reinterpret_cast<float*>(audio_frame->data[0]);
                auto* right = reinterpret_cast<float*>(audio_frame->data[1]);
                for (int i = 0; i < frame_samples; ++i) {
                    left[i] = audio_pcm.front() / 32768.0f;
                    audio_pcm.pop_front();
                    right[i] = audio_pcm.front() / 32768.0f;
                    audio_pcm.pop_front();
                }
                audio_frame->pts = audio_next_pts;
                audio_next_pts += frame_samples;
                if (!drain(audio_enc, audio_stream, audio_frame)) {
                    fail("AAC encoding/muxing failed");
                    break;
                }
                audio_samples_written_.fetch_add(
                    static_cast<uint64_t>(frame_samples));
            }
        }
        if (failed) {
            std::lock_guard<std::mutex> lock(mtx_);
            stop_requested_ = true;
            while (!queue_.empty()) {
                av_frame_free(&queue_.front().frame);
                queue_.pop_front();
            }
            audio_queue_.clear();
            break;
        }
        if (finish_after_batch) break;
    }

    if (enc && header_written && !failed) {
        if (audio_enc && audio_frame && !audio_pcm.empty()) {
            const int frame_samples = audio_frame->nb_samples;
            if (av_frame_make_writable(audio_frame) >= 0) {
                auto* left = reinterpret_cast<float*>(audio_frame->data[0]);
                auto* right = reinterpret_cast<float*>(audio_frame->data[1]);
                for (int i = 0; i < frame_samples; ++i) {
                    if (audio_pcm.size() >= 2) {
                        left[i] = audio_pcm.front() / 32768.0f;
                        audio_pcm.pop_front();
                        right[i] = audio_pcm.front() / 32768.0f;
                        audio_pcm.pop_front();
                    } else {
                        left[i] = right[i] = 0.0f;
                    }
                }
                audio_frame->pts = audio_next_pts;
                audio_next_pts += frame_samples;
                drain(audio_enc, audio_stream, audio_frame);
                audio_samples_written_.fetch_add(
                    static_cast<uint64_t>(frame_samples));
            }
        }
        const int64_t final_pts = std::max<int64_t>(last_pts + 1,
            (steady_ns() - started_ns_.load()) / 1'000'000 -
            std::max<int64_t>(0, first_pts));
        if (last_output && final_pts > last_pts + 1) {
            last_output->pts = final_pts;
            drain(enc, stream, last_output);
        }
        drain(enc, stream, nullptr);
        if (audio_enc) drain(audio_enc, audio_stream, nullptr);
        av_write_trailer(fmt);
        LOG_INFO << "Recording saved: " << output_path_ << " ("
                 << frames_written_.load() << " video frames, "
                 << audio_samples_written_.load() << " audio samples)";
    }

    if (sws) sws_freeContext(sws);
    if (last_output) av_frame_free(&last_output);
    if (output) av_frame_free(&output);
    if (audio_frame) av_frame_free(&audio_frame);
    if (packet) av_packet_free(&packet);
    if (enc) avcodec_free_context(&enc);
    if (audio_enc) avcodec_free_context(&audio_enc);
    if (fmt) {
        if (fmt->pb) avio_closep(&fmt->pb);
        avformat_free_context(fmt);
    }
    accepting_.store(false);
}

} // namespace ap::video
