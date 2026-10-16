#include "video/gpu_video.h"
#include "log.h"
#include <SDL.h>
#include <cstdlib>
#include <vector>


#if defined(_WIN32)
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>

#include <wrl/client.h>
#include <SDL_system.h>
#endif

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#if defined(_WIN32)
#include <libavutil/hwcontext_d3d11va.h>
#endif
}

namespace ap::video {
struct GpuVideo::Impl {
    AVBufferRef* hw = nullptr;
#if defined(_WIN32)
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device> device;
    Ptr<ID3D11DeviceContext> context;
    Ptr<ID3D11Device> render_device;
    Ptr<ID3D11DeviceContext> render_context;
    Ptr<ID3D11Texture2D> shared_output;
    Ptr<ID3D11Texture2D> render_input;
    Ptr<ID3D11DeviceContext4> decode_timeline;
    Ptr<ID3D11DeviceContext4> render_timeline;
    Ptr<ID3D11Fence> decode_fence;
    Ptr<ID3D11Fence> render_fence;
    UINT64 fence_value = 0;
    Ptr<ID3D11VideoDevice> video_device;
    Ptr<ID3D11VideoContext> video_context;
    Ptr<ID3D11VideoProcessorEnumerator> enumerator;
    Ptr<ID3D11VideoProcessor> processor;
    Ptr<ID3D11VideoProcessorOutputView> output_view;
    Ptr<ID3D11Texture2D> source_texture;
    std::vector<Ptr<ID3D11VideoProcessorInputView>> input_views;
    SDL_Texture* target = nullptr;
    Ptr<ID3D11Resource> target_resource;
    int width = 0, height = 0;
#endif
    ~Impl() { av_buffer_unref(&hw); }
};

GpuVideo::GpuVideo() : impl_(std::make_unique<Impl>()) {}
GpuVideo::~GpuVideo() = default;

bool GpuVideo::init(SDL_Renderer* renderer) {
#if defined(_WIN32)
    auto& p = *impl_;
    SDL_RendererInfo renderer_info{};
    SDL_GetRendererInfo(renderer, &renderer_info);
    LOG_INFO << "GPU video: SDL backend " << (renderer_info.name ? renderer_info.name : "unknown");
    p.render_device.Attach(SDL_RenderGetD3D11Device(renderer)); // SDL returns an owned ref.
    if (!p.render_device) {
        LOG_WARN << "GPU video: no SDL D3D11 device: " << SDL_GetError();
        return false;
    }
    p.render_device->GetImmediateContext(&p.render_context);
    // Keep interactive mirroring from building a driver-side present backlog.
    Impl::Ptr<IDXGIDevice1> latency_device;
    if (SUCCEEDED(p.render_device.As(&latency_device))) {
        const HRESULT latency_hr = latency_device->SetMaximumFrameLatency(1);
        if (FAILED(latency_hr))
            LOG_WARN << "GPU video: could not limit present queue: " << static_cast<long>(latency_hr);
    }
    // SDL creates a single-threaded device. Keep decoding on a separate
    // multithread-protected device on the SAME adapter and share GPU memory.
    Impl::Ptr<IDXGIDevice> dxgi_device;
    Impl::Ptr<IDXGIAdapter> adapter;
    HRESULT hr = p.render_device.As(&dxgi_device);
    if (SUCCEEDED(hr)) hr = dxgi_device->GetAdapter(&adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        char name[128]{};
        std::wcstombs(name, desc.Description, sizeof(name) - 1);
        LOG_INFO << "GPU video: adapter " << name << ", vendor=" << desc.VendorId;
    }
    UINT device_flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (SUCCEEDED(hr)) hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN,
        nullptr, device_flags,
        nullptr, 0, D3D11_SDK_VERSION, &p.device, nullptr, &p.context);
    if (FAILED(hr)) {
        LOG_WARN << "GPU video: decode device creation failed: " << static_cast<long>(hr);
        return false;
    }
    Impl::Ptr<ID3D11Multithread> multithread;
    hr = p.context.As(&multithread);
    if (FAILED(hr)) {
        LOG_WARN << "GPU video: multithread protection unavailable: " << static_cast<long>(hr);
        return false;
    }
    multithread->SetMultithreadProtected(TRUE);
    Impl::Ptr<ID3D11Device5> decode_device5, render_device5;
    hr = p.device.As(&decode_device5);
    if (SUCCEEDED(hr)) hr = p.render_device.As(&render_device5);
    if (SUCCEEDED(hr)) hr = p.context.As(&p.decode_timeline);
    if (SUCCEEDED(hr)) hr = p.render_context.As(&p.render_timeline);
    if (SUCCEEDED(hr)) hr = decode_device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED,
        IID_PPV_ARGS(&p.decode_fence));
    HANDLE fence_handle = nullptr;
    if (SUCCEEDED(hr)) hr = p.decode_fence->CreateSharedHandle(nullptr, GENERIC_ALL,
        nullptr, &fence_handle);
    if (SUCCEEDED(hr)) hr = render_device5->OpenSharedFence(fence_handle,
        IID_PPV_ARGS(&p.render_fence));
    if (fence_handle) CloseHandle(fence_handle);
    if (FAILED(hr)) {
        LOG_WARN << "GPU video: shared GPU fence unavailable: " << static_cast<long>(hr);
        return false;
    }
    hr = p.device.As(&p.video_device);
    if (SUCCEEDED(hr)) hr = p.context.As(&p.video_context);
    if (FAILED(hr)) {
        LOG_WARN << "GPU video: video interfaces unavailable: " << static_cast<long>(hr);
        return false;
    }

    p.hw = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!p.hw) return false;
    auto* hw = reinterpret_cast<AVHWDeviceContext*>(p.hw->data);
    auto* d3d = static_cast<AVD3D11VADeviceContext*>(hw->hwctx);
    d3d->device = p.device.Get();
    d3d->device->AddRef();
    if (av_hwdevice_ctx_init(p.hw) < 0) {
        av_buffer_unref(&p.hw);
        return false;
    }
    LOG_INFO << "GPU video: D3D11 devices ready on same adapter, GPU-only shared textures";
    return true;
#else
    (void)renderer;
    return false;
#endif
}

AVBufferRef* GpuVideo::device_ref() const {
    return impl_->hw ? av_buffer_ref(impl_->hw) : nullptr;
}

void GpuVideo::reset_target() {
#if defined(_WIN32)
    impl_->target = nullptr;
    impl_->target_resource.Reset();
#endif
}

bool GpuVideo::convert(SDL_Renderer* renderer, SDL_Texture* target,
                       const AVFrame* frame) {
#if defined(_WIN32)
    auto& p = *impl_;
    if (!p.hw || !frame || frame->format != AV_PIX_FMT_D3D11) return false;
    auto* source = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
    if (!source) return false;
    Impl::Ptr<ID3D11Device> source_device;
    source->GetDevice(&source_device);
    if (source_device.Get() != p.device.Get()) return false;

    if (!p.processor || p.width != frame->width || p.height != frame->height) {
        p.processor.Reset();
        p.enumerator.Reset();
        p.output_view.Reset();
        p.source_texture.Reset();
        p.input_views.clear();
        reset_target();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = desc.OutputWidth = frame->width;
        desc.InputHeight = desc.OutputHeight = frame->height;
        desc.InputFrameRate = desc.OutputFrameRate = {60, 1};
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        HRESULT setup_hr = p.video_device->CreateVideoProcessorEnumerator(&desc, &p.enumerator);
        if (SUCCEEDED(setup_hr))
            setup_hr = p.video_device->CreateVideoProcessor(p.enumerator.Get(), 0, &p.processor);
        if (FAILED(setup_hr)) {
            LOG_WARN << "GPU video: processor creation failed " << static_cast<long>(setup_hr);
            return false;
        }
        p.width = frame->width;
        p.height = frame->height;
        p.video_context->VideoProcessorSetStreamAutoProcessingMode(p.processor.Get(), 0, FALSE);
        p.shared_output.Reset();
        p.render_input.Reset();
        D3D11_TEXTURE2D_DESC shared_desc{};
        shared_desc.Width = frame->width;
        shared_desc.Height = frame->height;
        shared_desc.MipLevels = shared_desc.ArraySize = 1;
        shared_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        shared_desc.SampleDesc.Count = 1;
        shared_desc.Usage = D3D11_USAGE_DEFAULT;
        shared_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        shared_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        setup_hr = p.device->CreateTexture2D(&shared_desc, nullptr, &p.shared_output);
        if (FAILED(setup_hr)) {
            LOG_WARN << "GPU video: shared output creation failed " << static_cast<long>(setup_hr);
            p.processor.Reset();
            return false;
        }
        Impl::Ptr<IDXGIResource> resource;
        HANDLE shared_handle = nullptr;
        HRESULT import_hr = p.shared_output.As(&resource);
        if (SUCCEEDED(import_hr)) import_hr = resource->GetSharedHandle(&shared_handle);
        if (SUCCEEDED(import_hr)) import_hr = p.render_device->OpenSharedResource(shared_handle,
            IID_PPV_ARGS(&p.render_input));
        if (FAILED(import_hr)) {
            LOG_WARN << "GPU video: shared output import failed";
            p.processor.Reset();
            return false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output_desc{};
        output_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        setup_hr = p.video_device->CreateVideoProcessorOutputView(
            p.shared_output.Get(), p.enumerator.Get(), &output_desc, &p.output_view);
        if (FAILED(setup_hr)) {
            p.processor.Reset();
            return false;
        }
    }

    // FFmpeg normally reuses one decoder texture array. Create a view once
    // per slice, and drop the whole cache when its pool changes.
    if (p.source_texture.Get() != source) {
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        p.source_texture = source;
        p.input_views.clear();
        p.input_views.resize(desc.ArraySize);
    }
    const UINT slice = static_cast<UINT>(reinterpret_cast<uintptr_t>(frame->data[1]));
    if (slice >= p.input_views.size()) return false;
    if (!p.input_views[slice]) {
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input_desc{};
        input_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input_desc.Texture2D.ArraySlice = slice;
        const HRESULT input_hr = p.video_device->CreateVideoProcessorInputView(
            source, p.enumerator.Get(), &input_desc, &p.input_views[slice]);
        if (FAILED(input_hr)) {
            LOG_WARN << "GPU video: input view creation failed " << static_cast<long>(input_hr);
            return false;
        }
    }

    // Only discover/bind the SDL render target once per texture lifetime.
    // Subsequent frames copy straight into the retained native resource.
    if (p.target != target || !p.target_resource) {
        // Bind SDL's target through public rendering APIs, then obtain its native
        // RTV. No SDL private structs or pixel readback are involved.
        if (SDL_RenderFlush(renderer) < 0 || SDL_SetRenderTarget(renderer, target) < 0)
            return false;
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        // ClearRenderTargetView alone does not bind the target in D3D11.
        // A tiny draw makes SDL install its target before querying the RTV.
        const SDL_Rect bind_pixel{0, 0, 1, 1};
        SDL_RenderFillRect(renderer, &bind_pixel);
        SDL_RenderFlush(renderer);
        Impl::Ptr<ID3D11RenderTargetView> rtv;
        p.render_context->OMGetRenderTargets(1, &rtv, nullptr);
        Impl::Ptr<ID3D11Resource> output_resource;
        if (rtv) rtv->GetResource(&output_resource);
        Impl::Ptr<ID3D11Texture2D> output_texture;
        bool target_matches = output_resource && SUCCEEDED(output_resource.As(&output_texture));
        if (target_matches) {
            D3D11_TEXTURE2D_DESC desc{};
            output_texture->GetDesc(&desc);
            target_matches = desc.Width == static_cast<UINT>(frame->width) &&
                             desc.Height == static_cast<UINT>(frame->height) &&
                             desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
        }
        if (!target_matches) {
            SDL_SetRenderTarget(renderer, nullptr);
            LOG_WARN << "GPU video: unexpected SDL render-target dimensions or format";
            return false;
        }
        p.target = target;
        p.target_resource = output_resource;
        if (SDL_SetRenderTarget(renderer, nullptr) < 0) {
            reset_target();
            return false;
        }
    }
    // Flush queued SDL draws before overwriting a texture they may sample.
    if (SDL_RenderFlush(renderer) < 0) return false;
    HRESULT hr = S_OK;
    // GPU timelines order access across devices without blocking the CPU.
    if (SUCCEEDED(hr)) hr = p.decode_timeline->Wait(p.decode_fence.Get(), p.fence_value);
    if (SUCCEEDED(hr)) {
        RECT rect{0, 0, frame->width, frame->height};
        p.video_context->VideoProcessorSetStreamFrameFormat(p.processor.Get(), 0,
            D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        p.video_context->VideoProcessorSetStreamSourceRect(p.processor.Get(), 0, TRUE, &rect);
        p.video_context->VideoProcessorSetStreamDestRect(p.processor.Get(), 0, TRUE, &rect);
        p.video_context->VideoProcessorSetOutputTargetRect(p.processor.Get(), TRUE, &rect);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in_color{};
        in_color.YCbCr_Matrix = frame->colorspace != AVCOL_SPC_BT470BG &&
                               frame->colorspace != AVCOL_SPC_SMPTE170M;
        in_color.Nominal_Range = frame->color_range == AVCOL_RANGE_JPEG ? 2 : 1;
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out_color{};
        out_color.RGB_Range = 0;
        p.video_context->VideoProcessorSetStreamColorSpace(p.processor.Get(), 0, &in_color);
        p.video_context->VideoProcessorSetOutputColorSpace(p.processor.Get(), &out_color);
        p.video_context->VideoProcessorSetOutputAlphaFillMode(p.processor.Get(),
            D3D11_VIDEO_PROCESSOR_ALPHA_FILL_MODE_OPAQUE, 0);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = p.input_views[slice].Get();
        hr = p.video_context->VideoProcessorBlt(p.processor.Get(), p.output_view.Get(), 0, 1, &stream);
    }
    if (SUCCEEDED(hr)) {
        const UINT64 converted = p.fence_value + 1;
        const UINT64 consumed = p.fence_value + 2;
        hr = p.decode_timeline->Signal(p.decode_fence.Get(), converted);
        p.context->Flush();
        if (SUCCEEDED(hr)) hr = p.render_timeline->Wait(p.render_fence.Get(), converted);
        if (SUCCEEDED(hr)) {
            p.render_context->CopyResource(p.target_resource.Get(), p.render_input.Get());
            hr = p.render_timeline->Signal(p.render_fence.Get(), consumed);
            p.render_context->Flush();
            p.fence_value = consumed;
        } else hr = E_FAIL;
    }
    if (FAILED(hr)) LOG_WARN << "GPU video conversion failed, HRESULT=" << static_cast<long>(hr);
    return SUCCEEDED(hr);
#else
    (void)renderer; (void)target; (void)frame;
    return false;
#endif
}
} // namespace ap::video
