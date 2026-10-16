// Hardware integration test. Upload/readback here are test instrumentation;
// the application display path never performs either transfer for GPU frames.
#include "video/gpu_video.h"
#include "video/h264_decoder.h"
#include "video/video_recorder.h"
#include "log.h"
#include <SDL.h>
#include <SDL_system.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <chrono>
#include <filesystem>
#include <thread>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::vector<uint8_t> encode_fixture(AVCodecID id, int w, int h) {
    const AVCodec* codec = avcodec_find_encoder_by_name(
        id == AV_CODEC_ID_HEVC ? "hevc_nvenc" : "h264_nvenc");
    require(codec != nullptr, "NVENC test encoder unavailable");
    AVCodecContext* enc = avcodec_alloc_context3(codec);
    enc->width = w; enc->height = h;
    enc->pix_fmt = AV_PIX_FMT_NV12;
    enc->time_base = {1, 60}; enc->framerate = {60, 1};
    enc->max_b_frames = 0; enc->gop_size = 1;
    enc->bit_rate = 2'000'000;
    av_opt_set(enc->priv_data, "preset", "p1", 0);
    av_opt_set(enc->priv_data, "tune", "ull", 0);
    av_opt_set(enc->priv_data, "zerolatency", "1", 0);
    require(avcodec_open2(enc, codec, nullptr) >= 0, "Test encoder open failed");
    AVFrame* f = av_frame_alloc();
    f->format = enc->pix_fmt; f->width = w; f->height = h;
    require(av_frame_get_buffer(f, 32) >= 0, "Encoder frame allocation failed");
    for (int y = 0; y < h; ++y) {
        std::memset(f->data[0] + y * f->linesize[0], 16, w / 2);
        std::memset(f->data[0] + y * f->linesize[0] + w / 2, 235, w / 2);
    }
    for (int y = 0; y < h / 2; ++y) std::memset(f->data[1] + y * f->linesize[1], 128, w);
    require(avcodec_send_frame(enc, f) >= 0, "Test encode failed");
    avcodec_send_frame(enc, nullptr);
    AVPacket* packet = av_packet_alloc();
    require(avcodec_receive_packet(enc, packet) >= 0, "Test encoded packet missing");
    std::vector<uint8_t> result(packet->data, packet->data + packet->size);
    av_packet_free(&packet); av_frame_free(&f); avcodec_free_context(&enc);
    return result;
}

std::vector<uint8_t> fixture_config(const std::vector<uint8_t>& bytes, AVCodecID id) {
    std::vector<std::vector<uint8_t>> nals;
    std::size_t start = 0;
    for (std::size_t i = 0; i + 3 <= bytes.size();) {
        std::size_t prefix = 0;
        if (i + 4 <= bytes.size() && bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 0 && bytes[i+3] == 1)
            prefix = 4;
        else if (bytes[i] == 0 && bytes[i+1] == 0 && bytes[i+2] == 1) prefix = 3;
        if (prefix) {
            if (start) nals.emplace_back(bytes.begin() + start, bytes.begin() + i);
            start = i + prefix; i = start;
        } else ++i;
    }
    if (start) nals.emplace_back(bytes.begin() + start, bytes.end());
    auto find = [&](int type) {
        for (const auto& nal : nals)
            if ((id == AV_CODEC_ID_HEVC ? (nal[0] >> 1) & 63 : nal[0] & 31) == type) return nal;
        throw std::runtime_error("Encoded fixture parameter set missing");
    };
    auto append = [](std::vector<uint8_t>& dst, const std::vector<uint8_t>& nal) {
        dst.push_back(static_cast<uint8_t>(nal.size() >> 8));
        dst.push_back(static_cast<uint8_t>(nal.size()));
        dst.insert(dst.end(), nal.begin(), nal.end());
    };
    if (id == AV_CODEC_ID_H264) {
        const auto sps = find(7);
        std::vector<uint8_t> config{1, sps[1], sps[2], sps[3], 255, 225};
        append(config, sps); config.push_back(1); append(config, find(8));
        return config;
    }
    std::vector<uint8_t> config(23, 0);
    config[0] = 1; config[22] = 3;
    for (int type : {32, 33, 34}) {
        config.push_back(static_cast<uint8_t>(128 | type));
        config.push_back(0); config.push_back(1);
        append(config, find(type));
    }
    return config;
}

void check_recording(const std::string& path, AVCodecID expected_codec) {
    AVFormatContext* file = nullptr;
    require(avformat_open_input(&file, path.c_str(), nullptr, nullptr) >= 0, "Recorded MP4 cannot be opened");
    require(avformat_find_stream_info(file, nullptr) >= 0, "Recorded MP4 streams missing");
    const int video = av_find_best_stream(file, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio = av_find_best_stream(file, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    require(video >= 0 && audio >= 0, "Recorded audio/video stream missing");
    const auto* info = file->streams[video]->codecpar;
    require(expected_codec == AV_CODEC_ID_NONE || info->codec_id == expected_codec, "Wrong recorded video codec");
    require(info->width == 256 && info->height == 512, "Recording canvas changed on device rotation");
    require(file->streams[audio]->codecpar->codec_id == AV_CODEC_ID_AAC, "Recorded audio is not AAC");
    AVCodecContext* decoder = avcodec_alloc_context3(avcodec_find_decoder(info->codec_id));
    avcodec_parameters_to_context(decoder, info);
    require(avcodec_open2(decoder, nullptr, nullptr) >= 0, "Recorded video decoder failed");
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    int frames = 0, audio_packets = 0;
    int64_t previous_dts = AV_NOPTS_VALUE;
    bool last_bars = false;
    auto receive = [&] {
        while (avcodec_receive_frame(decoder, frame) == 0) {
            ++frames;
            const auto* y = frame->data[0];
            const int stride = frame->linesize[0];
            require(y[256 * stride + 192] > 210 && y[256 * stride + 64] < 40,
                "Recorded GPU image colors incorrect");
            last_bars = y[16 * stride + 192] < 40;
        }
    };
    while (av_read_frame(file, packet) >= 0) {
        if (packet->stream_index == video) {
            require(previous_dts == AV_NOPTS_VALUE || packet->dts > previous_dts, "Non-monotonic recording timestamps");
            previous_dts = packet->dts;
            require(avcodec_send_packet(decoder, packet) >= 0, "Recorded video packet is corrupt");
            receive();
        } else if (packet->stream_index == audio) ++audio_packets;
        av_packet_unref(packet);
    }
    avcodec_send_packet(decoder, nullptr); receive();
    require(frames >= 12 && audio_packets > 0 && last_bars,
        "Recording frames, audio or GPU letterboxing failed");
    av_packet_free(&packet); av_frame_free(&frame);
    avcodec_free_context(&decoder); avformat_close_input(&file);
}

void test_recording(AVBufferRef* device, ap::video::GpuVideo& gpu, SDL_Renderer* renderer) {
    using Recorder = ap::video::VideoRecorder;
    for (int mode = 0; mode < 5; ++mode) {
        Recorder recorder;
        Recorder::Options options;
        options.encoder = mode == 3 ? Recorder::EncoderMode::Cpu
            : mode == 2 ? Recorder::EncoderMode::Auto : Recorder::EncoderMode::Gpu;
        options.codec = mode == 1 ? Recorder::VideoCodec::H265 : Recorder::VideoCodec::H264;
        const auto directory = std::filesystem::current_path() / "build" / "gpu-recording-tests" /
            ("mode-" + std::to_string(mode));
        require(recorder.start(directory.u8string(), options), "Recorder start failed");
        ap::video::H264Decoder decoder;
        require(decoder.init(mode != 4, device), "Recording fixture decoder failed");
        int sent = 0;
        for (const auto size : {SDL_Point{256, 512}, SDL_Point{512, 256}}) {
            const auto packet = encode_fixture(AV_CODEC_ID_H264, size.x, size.y);
            const auto config = fixture_config(packet, AV_CODEC_ID_H264);
            require(decoder.set_parameter_sets_from_avcc(config.data(), config.size()), "Recording fixture config failed");
            SDL_Texture* display = mode != 4 ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                SDL_TEXTUREACCESS_TARGET, size.x, size.y) : nullptr;
            require(mode == 4 || display != nullptr, "Concurrent display texture allocation failed");
            for (int i = 0; i < 8; ++i) {
                bool got = false; int w = 0, h = 0;
                require(decoder.decode(packet.data(), packet.size(), true, got, w, h) && got,
                    "Recording fixture decode failed");
                recorder.submit(decoder.current_avframe());
                if (display) {
                    require(gpu.convert(renderer, display, decoder.current_avframe()), "Concurrent recording/display failed");
                    SDL_RenderCopy(renderer, display, nullptr, nullptr);
                    SDL_RenderPresent(renderer);
                }
                std::vector<int16_t> pcm(2048, 0);
                recorder.submit_audio(pcm.data(), static_cast<int>(pcm.size()), 44100, 2);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                ++sent;
                while (recorder.status().frames < static_cast<uint64_t>(sent) &&
                       recorder.status().error.empty() && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                require(recorder.status().error.empty(), recorder.status().error.c_str());
                require(recorder.status().frames >= static_cast<uint64_t>(sent), "Recording encoder stalled");
            }
            gpu.reset_target();
            if (display) SDL_DestroyTexture(display);
        }
        recorder.stop();
        const auto status = recorder.status();
        require(status.error.empty(), status.error.c_str());
        require(mode == 3 ? !status.gpu_resident && status.readback_frames > 0
              : mode == 4 ? !status.gpu_resident && status.readback_frames == 0
                          : status.gpu_resident && status.readback_frames == 0,
            "Recording video memory path is incorrect");
        check_recording(status.output_path, mode == 3 ? AV_CODEC_ID_NONE
            : mode == 1 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
        std::printf("PASS recording mode %d: %s, readbacks=%llu, MP4 pixels/audio/timestamps/rotation verified\n",
            mode, status.codec.c_str(), static_cast<unsigned long long>(status.readback_frames));
    }
}

int main(int argc, char**) {
    ap::set_log_enabled(true);
    try {
        require(SDL_Init(SDL_INIT_VIDEO) == 0, SDL_GetError());
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
        SDL_Window* window = SDL_CreateWindow("GPU test", 0, 0, 320, 320, SDL_WINDOW_HIDDEN);
        require(window != nullptr, SDL_GetError());
        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
        require(renderer != nullptr, SDL_GetError());
        {
            ap::video::GpuVideo gpu;
            require(gpu.init(renderer), "Shared D3D11 device initialization failed");
            Microsoft::WRL::ComPtr<ID3D11Device> render_device;
            render_device.Attach(SDL_RenderGetD3D11Device(renderer));
            Microsoft::WRL::ComPtr<IDXGIDevice1> latency_device;
            require(SUCCEEDED(render_device.As(&latency_device)), "DXGI latency interface missing");
            UINT max_latency = 0;
            require(SUCCEEDED(latency_device->GetMaximumFrameLatency(&max_latency)) && max_latency == 1,
                "Display queue was not limited to one frame");
            AVBufferRef* device = gpu.device_ref();
            test_recording(device, gpu, renderer);
            {
                ap::video::H264Decoder hardware_decoder, software_decoder;
                require(hardware_decoder.init(true, device), "GPU decoder init failed");
                require(software_decoder.init(), "CPU decoder init failed");
                for (auto codec : {AV_CODEC_ID_H264, AV_CODEC_ID_HEVC, AV_CODEC_ID_H264}) {
                    for (const auto size : {SDL_Point{512, 256}, SDL_Point{256, 512}}) {
                        const auto packet = encode_fixture(codec, size.x, size.y);
                        const auto config = fixture_config(packet, codec);
                        for (auto* decoder : {&hardware_decoder, &software_decoder}) {
                            require(decoder->set_parameter_sets_from_avcc(config.data(), config.size()), "Fixture config failed");
                            SDL_Texture* target = nullptr;
                            if (decoder == &hardware_decoder) {
                                target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                    SDL_TEXTUREACCESS_TARGET, size.x, size.y);
                                require(target != nullptr, SDL_GetError());
                            }
                            for (int i = 0; i < 12; ++i) {
                                bool got = false; int w = 0, h = 0;
                                require(decoder->decode(packet.data(), packet.size(), true, got, w, h) && got,
                                    "Decoder failed to emit frame without delay");
                                require(w == size.x && h == size.y, "Decoder resolution change failed");
                                const AVFrame* f = decoder->current_avframe();
                                const bool hw = decoder == &hardware_decoder;
                                require(hw ? f->format == AV_PIX_FMT_D3D11 : !f->hw_frames_ctx,
                                    "Unexpected decoder output memory type");
                                if (hw) {
                                    require(target && gpu.convert(renderer, target, f), "Decoded GPU surface conversion failed");
                                    SDL_RenderClear(renderer);
                                    SDL_RenderCopy(renderer, target, nullptr, nullptr);
                                    std::vector<uint32_t> pixels(320 * 320);
                                    require(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                        pixels.data(), 320 * 4) == 0, "Decoded GPU readback for test failed");
                                    require((pixels[160 * 320 + 80] & 255) < 10 &&
                                            (pixels[160 * 320 + 240] & 255) > 245,
                                        "Decoded GPU image pixels incorrect");
                                    SDL_RenderPresent(renderer);
                                }
                            }
                            if (target) {
                                gpu.reset_target();
                                SDL_DestroyTexture(target);
                            }
                        }
                        std::printf("PASS %s %dx%d: CPU/GPU decode, resident surfaces, live resolution/codec switches\n",
                            avcodec_get_name(codec), size.x, size.y);
                    }
                }
            }
            std::vector<SDL_Point> sizes{{256, 128}, {128, 256}, {256, 128}};
            if (argc > 1) sizes.push_back({1920, 1080});
            for (const auto size : sizes) {
                AVBufferRef* pool = av_hwframe_ctx_alloc(device);
                require(pool != nullptr, "Frame pool allocation failed");
                auto* frames = reinterpret_cast<AVHWFramesContext*>(pool->data);
                frames->format = AV_PIX_FMT_D3D11;
                frames->sw_format = AV_PIX_FMT_NV12;
                frames->width = size.x;
                frames->height = size.y;
                frames->initial_pool_size = 4;
                auto* d3d = static_cast<AVD3D11VAFramesContext*>(frames->hwctx);
                d3d->BindFlags = D3D11_BIND_DECODER;
                require(av_hwframe_ctx_init(pool) >= 0, "Frame pool init failed");
                // Hold several frames so array slices other than zero are tested.
                AVFrame* hardware[3]{};
                for (auto& f : hardware) {
                    f = av_frame_alloc();
                    require(f && av_hwframe_get_buffer(pool, f, 0) >= 0, "GPU frame allocation failed");
                }
                AVFrame* sw = av_frame_alloc();
                sw->format = AV_PIX_FMT_NV12;
                sw->width = size.x;
                sw->height = size.y;
                require(av_frame_get_buffer(sw, 32) >= 0, "CPU test frame allocation failed");
                SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_TARGET, size.x, size.y);
                require(target != nullptr, SDL_GetError());
                for (int range = 0; range < 2; ++range) {
                    const int black = range ? 0 : 16;
                    const int white = range ? 255 : 235;
                    for (int y = 0; y < size.y; ++y) {
                        std::memset(sw->data[0] + y * sw->linesize[0], black, size.x / 2);
                        std::memset(sw->data[0] + y * sw->linesize[0] + size.x / 2, white, size.x / 2);
                    }
                    for (int y = 0; y < size.y / 2; ++y)
                        std::memset(sw->data[1] + y * sw->linesize[1], 128, size.x);
                    for (auto* f : hardware) {
                        require(av_hwframe_transfer_data(f, sw, 0) >= 0, "Test upload failed");
                        f->color_range = range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
                        f->colorspace = AVCOL_SPC_BT709;
                        require(gpu.convert(renderer, target, f), "GPU conversion failed");
                        for (int angle : {0, 90, 180, 270}) {
                            SDL_SetRenderDrawColor(renderer, 100, 100, 100, 255);
                            SDL_RenderClear(renderer);
                            SDL_Rect dst{32, 32, 256, 256};
                            require(SDL_RenderCopyEx(renderer, target, nullptr, &dst,
                                angle, nullptr, SDL_FLIP_NONE) == 0, SDL_GetError());
                            std::vector<uint32_t> pixels(320 * 320);
                            require(SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888,
                                pixels.data(), 320 * 4) == 0, SDL_GetError());
                            auto level = [&](int x, int y) { return static_cast<int>(pixels[y * 320 + x] & 255); };
                            const int a = angle % 180 ? level(160, 80) : level(80, 160);
                            const int b = angle % 180 ? level(160, 240) : level(240, 160);
                            require(angle < 180 ? a < 8 && b > 247 : b < 8 && a > 247,
                                "Unexpected GPU pixels or rotation/range mismatch");
                            SDL_RenderPresent(renderer);
                        }
                    }
                }
                std::printf("PASS %dx%d: array slices, limited/full range, four rotations\n", size.x, size.y);
                if (size.x == 1920) {
                    using Clock = std::chrono::steady_clock;
                    double convert_us = 0;
                    const auto begin = Clock::now();
                    for (int i = 0; i < 260; ++i) {
                        const auto t0 = Clock::now();
                        require(gpu.convert(renderer, target, hardware[i % 3]), "Benchmark conversion failed");
                        const auto t1 = Clock::now();
                        SDL_RenderCopy(renderer, target, nullptr, nullptr);
                        SDL_RenderPresent(renderer);
                        if (i >= 20) convert_us += std::chrono::duration<double, std::micro>(t1 - t0).count();
                    }
                    std::printf("BENCH 1080p: CPU conversion submission %.1f us/frame; whole loop %.2f ms/frame (not AirPlay latency)\n",
                        convert_us / 240, std::chrono::duration<double, std::milli>(Clock::now() - begin).count() / 260);
                }
                gpu.reset_target();
                SDL_DestroyTexture(target);
                av_frame_free(&sw);
                for (auto& f : hardware) av_frame_free(&f);
                av_buffer_unref(&pool);
            }
            av_buffer_unref(&device);
        }
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s (%s)\n", error.what(), SDL_GetError());
        return 1;
    }
}
