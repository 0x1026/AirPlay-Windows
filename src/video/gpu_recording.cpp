#include "video/gpu_recording.h"
#include <algorithm>
#if defined(_WIN32)
#include <d3d11.h>
#include <wrl/client.h>
#endif
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
}
namespace ap::video {
struct GpuRecording::Impl {
    AVBufferRef* pool = nullptr;
#if defined(_WIN32)
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11VideoDevice> video_device;
    Ptr<ID3D11VideoContext> video_context;
    Ptr<ID3D11VideoProcessorEnumerator> enumerator;
    Ptr<ID3D11VideoProcessor> processor;
    int source_w = 0, source_h = 0;
    int width = 0, height = 0;
#endif
    ~Impl() { av_buffer_unref(&pool); }
};
GpuRecording::GpuRecording() : impl_(std::make_unique<Impl>()) {}
GpuRecording::~GpuRecording() = default;
AVBufferRef* GpuRecording::frames_ref() const {
    return impl_->pool ? av_buffer_ref(impl_->pool) : nullptr;
}
bool GpuRecording::init(AVBufferRef* input_frames, int width, int height) {
#if defined(_WIN32)
    if (!input_frames || width < 2 || height < 2) return false;
    auto& p = *impl_;
    auto* source = reinterpret_cast<AVHWFramesContext*>(input_frames->data);
    if (source->format != AV_PIX_FMT_D3D11) return false;
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(source->device_ctx->hwctx);
    p.device = d3d->device;
    p.context = d3d->device_context;
    if (FAILED(p.device.As(&p.video_device)) || FAILED(p.context.As(&p.video_context)))
        return false;
    p.pool = av_hwframe_ctx_alloc(source->device_ref);
    if (!p.pool) return false;
    auto* frames = reinterpret_cast<AVHWFramesContext*>(p.pool->data);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = p.width = width;
    frames->height = p.height = height;
    auto* out = static_cast<AVD3D11VAFramesContext*>(frames->hwctx);
    out->BindFlags = D3D11_BIND_RENDER_TARGET;
    if (av_hwframe_ctx_init(p.pool) < 0) {
        av_buffer_unref(&p.pool);
        return false;
    }
    return true;
#else
    (void)input_frames; (void)width; (void)height;
    return false;
#endif
}
bool GpuRecording::convert(const AVFrame* input, AVFrame* output) {
#if defined(_WIN32)
    auto& p = *impl_;
    if (!p.pool || !input || !output || input->format != AV_PIX_FMT_D3D11)
        return false;
    auto* texture = reinterpret_cast<ID3D11Texture2D*>(input->data[0]);
    Impl::Ptr<ID3D11Device> source_device;
    texture->GetDevice(&source_device);
    if (source_device.Get() != p.device.Get()) return false;
    if (!p.processor || p.source_w != input->width || p.source_h != input->height) {
        p.processor.Reset(); p.enumerator.Reset();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = input->width; desc.InputHeight = input->height;
        desc.OutputWidth = p.width; desc.OutputHeight = p.height;
        desc.InputFrameRate = desc.OutputFrameRate = {60, 1};
        desc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;
        if (FAILED(p.video_device->CreateVideoProcessorEnumerator(&desc, &p.enumerator)) ||
            FAILED(p.video_device->CreateVideoProcessor(p.enumerator.Get(), 0, &p.processor)))
            return false;
        p.source_w = input->width; p.source_h = input->height;
        p.video_context->VideoProcessorSetStreamAutoProcessingMode(p.processor.Get(), 0, FALSE);
    }
    av_frame_unref(output);
    if (av_hwframe_get_buffer(p.pool, output, 0) < 0) return false;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC in_desc{};
    in_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    in_desc.Texture2D.ArraySlice = static_cast<UINT>(reinterpret_cast<uintptr_t>(input->data[1]));
    Impl::Ptr<ID3D11VideoProcessorInputView> in_view;
    if (FAILED(p.video_device->CreateVideoProcessorInputView(texture, p.enumerator.Get(),
            &in_desc, &in_view))) return false;
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC out_desc{};
    out_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Impl::Ptr<ID3D11VideoProcessorOutputView> out_view;
    if (FAILED(p.video_device->CreateVideoProcessorOutputView(
            reinterpret_cast<ID3D11Texture2D*>(output->data[0]),
            p.enumerator.Get(), &out_desc, &out_view))) return false;
    int fit_w = p.width, fit_h = p.height;
    if (static_cast<int64_t>(input->width) * p.height > static_cast<int64_t>(p.width) * input->height)
        fit_h = std::max(2, static_cast<int>(static_cast<int64_t>(p.width) * input->height / input->width) & ~1);
    else fit_w = std::max(2, static_cast<int>(static_cast<int64_t>(p.height) * input->width / input->height) & ~1);
    const int x = ((p.width - fit_w) / 2) & ~1;
    const int y = ((p.height - fit_h) / 2) & ~1;
    RECT source_rect{0, 0, input->width, input->height};
    RECT dest_rect{x, y, x + fit_w, y + fit_h};
    RECT target_rect{0, 0, p.width, p.height};
    p.video_context->VideoProcessorSetStreamFrameFormat(p.processor.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    p.video_context->VideoProcessorSetStreamSourceRect(p.processor.Get(), 0, TRUE, &source_rect);
    p.video_context->VideoProcessorSetStreamDestRect(p.processor.Get(), 0, TRUE, &dest_rect);
    p.video_context->VideoProcessorSetOutputTargetRect(p.processor.Get(), TRUE, &target_rect);
    D3D11_VIDEO_COLOR black{};
    black.YCbCr.Y = 16.0f / 255.0f;
    black.YCbCr.Cb = black.YCbCr.Cr = 128.0f / 255.0f;
    black.YCbCr.A = 1.0f;
    p.video_context->VideoProcessorSetOutputBackgroundColor(p.processor.Get(), TRUE, &black);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_color{};
    in_color.YCbCr_Matrix = input->colorspace != AVCOL_SPC_BT470BG && input->colorspace != AVCOL_SPC_SMPTE170M;
    in_color.Nominal_Range = input->color_range == AVCOL_RANGE_JPEG ? 2 : 1;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_color{};
    out_color.YCbCr_Matrix = 1; out_color.Nominal_Range = 1;
    p.video_context->VideoProcessorSetStreamColorSpace(p.processor.Get(), 0, &in_color);
    p.video_context->VideoProcessorSetOutputColorSpace(p.processor.Get(), &out_color);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE; stream.pInputSurface = in_view.Get();
    if (FAILED(p.video_context->VideoProcessorBlt(p.processor.Get(), out_view.Get(), 0, 1, &stream)))
        return false;
    // Submit the GPU conversion before NVENC/AMF consumes the same texture.
    p.context->Flush();
    output->color_range = AVCOL_RANGE_MPEG;
    output->colorspace = AVCOL_SPC_BT709;
    return true;
#else
    (void)input; (void)output;
    return false;
#endif
}
}
