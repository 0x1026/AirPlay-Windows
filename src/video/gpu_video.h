#pragma once

#include <memory>

struct SDL_Renderer;
struct SDL_Texture;
extern "C" {
struct AVBufferRef;
struct AVFrame;
}

namespace ap::video {

// D3D11 decode and SDL rendering use the same GPU adapter. Shared textures
// and GPU fences keep conversion and presentation free of CPU pixel access.
class GpuVideo {
public:
    GpuVideo();
    ~GpuVideo();
    bool init(SDL_Renderer* renderer);
    AVBufferRef* device_ref() const;
    // Call before destroying/replacing the SDL target texture.
    void reset_target();
    bool convert(SDL_Renderer* renderer, SDL_Texture* target, const AVFrame* frame);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::video
