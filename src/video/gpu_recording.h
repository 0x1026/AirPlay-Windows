#pragma once
#include <memory>
extern "C" {
struct AVBufferRef;
struct AVFrame;
}
namespace ap::video {
// Recorder-owned NV12 surfaces, on the decoder device. Resizes/letterboxes
// on the GPU so encoder buffering cannot retain the decoder's surface pool.
class GpuRecording {
public:
    GpuRecording();
    ~GpuRecording();
    bool init(AVBufferRef* input_frames, int width, int height);
    AVBufferRef* frames_ref() const;
    bool convert(const AVFrame* input, AVFrame* output);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
