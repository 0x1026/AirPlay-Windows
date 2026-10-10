#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ap::platform {

// Publishes the current AirPlay playback into Windows System Media
// Transport Controls (SMTC). Public methods are thread-safe: the actual
// WinRT calls run on a dedicated MTA worker so RTSP/audio/render threads
// never need to initialize COM/WinRT themselves.
class WindowsMediaSession {
public:
    WindowsMediaSession();
    ~WindowsMediaSession();

    WindowsMediaSession(const WindowsMediaSession&) = delete;
    WindowsMediaSession& operator=(const WindowsMediaSession&) = delete;

    // native_window is an HWND on Windows. On other platforms this is a no-op.
    bool start(void* native_window);
    void stop();

    // A session is only exposed to the Windows global media session manager
    // while an AirPlay device is actively connected.
    void set_active(bool active);
    void set_playing(bool playing);

    void set_metadata(const std::string& title,
                      const std::string& artist,
                      const std::string& album);

    // JPEG/PNG bytes as received from AirPlay metadata.
    void set_cover_art(const uint8_t* data, std::size_t size);

    // milliseconds; total_ms == 0 clears the timeline.
    void set_timeline(uint32_t elapsed_ms, uint32_t total_ms);

    // Clear metadata/art/timeline and mark playback inactive.
    void clear();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ap::platform
