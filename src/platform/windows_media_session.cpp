#include "platform/windows_media_session.h"
#include "airplay/event_channel.h"
#include "log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
    #include <windows.h>
    #include <systemmediatransportcontrolsinterop.h>
    #include <winrt/base.h>
    #include <winrt/Windows.Foundation.h>
    #include <winrt/Windows.Media.h>
    #include <winrt/Windows.Storage.Streams.h>
#endif

namespace ap::platform {

struct WindowsMediaSession::Impl {
    std::mutex              mtx;
    std::condition_variable cv;
    std::condition_variable ready_cv;
    std::thread             thread;

    bool stop_requested{false};
    bool ready{false};
    bool ok{false};
    void* native_window{nullptr};

    bool active{false};
    bool playing{true};
    std::string title;
    std::string artist;
    std::string album;
    std::vector<uint8_t> cover;
    uint32_t elapsed_ms{0};
    uint32_t total_ms{0};

    bool playback_dirty{true};
    bool metadata_dirty{true};
    bool cover_dirty{true};
    bool timeline_dirty{true};

#if defined(_WIN32)
    void worker();
#endif
};

WindowsMediaSession::WindowsMediaSession()
    : impl_(std::make_unique<Impl>()) {}

WindowsMediaSession::~WindowsMediaSession() {
    stop();
}

bool WindowsMediaSession::start(void* native_window) {
#if !defined(_WIN32)
    (void)native_window;
    return true;
#else
    if (!native_window) {
        LOG_WARN << "SMTC: cannot start without a native HWND";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->thread.joinable()) return impl_->ok;
        impl_->native_window = native_window;
        impl_->stop_requested = false;
        impl_->ready = false;
        impl_->ok = false;
    }

    impl_->thread = std::thread(&Impl::worker, impl_.get());

    std::unique_lock<std::mutex> lock(impl_->mtx);
    impl_->ready_cv.wait(lock, [this] { return impl_->ready; });
    return impl_->ok;
#endif
}

void WindowsMediaSession::stop() {
#if defined(_WIN32)
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->stop_requested = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
#endif
}

void WindowsMediaSession::set_active(bool active) {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->active == active) return;
        impl_->active = active;
        impl_->playback_dirty = true;
    }
    impl_->cv.notify_one();
}

void WindowsMediaSession::set_playing(bool playing) {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->playing == playing) return;
        impl_->playing = playing;
        impl_->playback_dirty = true;
    }
    impl_->cv.notify_one();
}

void WindowsMediaSession::set_metadata(const std::string& title,
                                       const std::string& artist,
                                       const std::string& album) {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        if (impl_->title == title &&
            impl_->artist == artist &&
            impl_->album == album) {
            return;
        }
        impl_->title = title;
        impl_->artist = artist;
        impl_->album = album;
        impl_->metadata_dirty = true;
    }
    impl_->cv.notify_one();
}

void WindowsMediaSession::set_cover_art(const uint8_t* data, std::size_t size) {
    if (!data || size == 0) return;
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->cover.assign(data, data + size);
        impl_->cover_dirty = true;
    }
    impl_->cv.notify_one();
}

void WindowsMediaSession::set_timeline(uint32_t elapsed_ms, uint32_t total_ms) {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        elapsed_ms = std::min(elapsed_ms, total_ms == 0 ? elapsed_ms : total_ms);
        if (impl_->elapsed_ms == elapsed_ms && impl_->total_ms == total_ms) return;
        impl_->elapsed_ms = elapsed_ms;
        impl_->total_ms = total_ms;
        impl_->timeline_dirty = true;
    }
    impl_->cv.notify_one();
}

void WindowsMediaSession::clear() {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->active = false;
        impl_->playing = true;
        impl_->title.clear();
        impl_->artist.clear();
        impl_->album.clear();
        impl_->cover.clear();
        impl_->elapsed_ms = 0;
        impl_->total_ms = 0;
        impl_->playback_dirty = true;
        impl_->metadata_dirty = true;
        impl_->cover_dirty = true;
        impl_->timeline_dirty = true;
    }
    impl_->cv.notify_one();
}

#if defined(_WIN32)
namespace {

winrt::hstring utf8_hstring(const std::string& value) {
    if (value.empty()) return {};
    const int count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                             value.data(),
                                             static_cast<int>(value.size()),
                                             nullptr, 0);
    if (count <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                          value.data(), static_cast<int>(value.size()),
                          wide.data(), count);
    return winrt::hstring(wide);
}

winrt::Windows::Foundation::TimeSpan ms_to_timespan(uint32_t ms) {
    using namespace std::chrono;
    return duration_cast<winrt::Windows::Foundation::TimeSpan>(milliseconds(ms));
}

} // namespace

void WindowsMediaSession::Impl::worker() {
    using winrt::Windows::Media::MediaPlaybackStatus;
    using winrt::Windows::Media::MediaPlaybackType;
    using winrt::Windows::Media::SystemMediaTransportControls;
    using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;
    using winrt::Windows::Storage::Streams::DataWriter;
    using winrt::Windows::Storage::Streams::InMemoryRandomAccessStream;
    using winrt::Windows::Storage::Streams::RandomAccessStreamReference;

    bool apartment_initialized = false;

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment_initialized = true;

        auto factory = winrt::get_activation_factory<SystemMediaTransportControls>();
        auto interop = factory.as<ISystemMediaTransportControlsInterop>();

        SystemMediaTransportControls controls{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            reinterpret_cast<HWND>(native_window),
            winrt::guid_of<SystemMediaTransportControls>(),
            winrt::put_abi(controls)));

        auto updater = controls.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);

        // Manual SMTC integration must expose at least Play/Pause and handle
        // ButtonPressed for Windows to treat the source as a real media session.
        // Keep those two capabilities registered for the lifetime of the SMTC
        // object, while IsEnabled controls whether the AirPlay session is visible.
        //
        // Play/Pause/Next/Previous are relayed to the pair-verified AirPlay 2
        // session event channel. The iPhone remains the authoritative player;
        // local SMTC button presses never mutate the receiver timeline directly.
        controls.IsPlayEnabled(true);
        controls.IsPauseEnabled(true);
        controls.IsStopEnabled(false);
        controls.IsNextEnabled(true);
        controls.IsPreviousEnabled(true);
        controls.IsFastForwardEnabled(false);
        controls.IsRewindEnabled(false);
        controls.IsEnabled(false);
        controls.PlaybackStatus(MediaPlaybackStatus::Closed);
        controls.PlaybackRate(0.0);

        auto button_token = controls.ButtonPressed(
            [](SystemMediaTransportControls const&,
               winrt::Windows::Media::SystemMediaTransportControlsButtonPressedEventArgs const& args) {
                using winrt::Windows::Media::SystemMediaTransportControlsButton;

                bool recognized = true;
                ap::airplay::MediaRemoteCommand command =
                    ap::airplay::MediaRemoteCommand::TogglePlay;

                switch (args.Button()) {
                    case SystemMediaTransportControlsButton::Play:
                        command = ap::airplay::MediaRemoteCommand::Play;
                        break;
                    case SystemMediaTransportControlsButton::Pause:
                        command = ap::airplay::MediaRemoteCommand::Pause;
                        break;
                    case SystemMediaTransportControlsButton::Next:
                        command = ap::airplay::MediaRemoteCommand::Next;
                        break;
                    case SystemMediaTransportControlsButton::Previous:
                        command = ap::airplay::MediaRemoteCommand::Previous;
                        break;
                    default:
                        recognized = false;
                        break;
                }

                if (!recognized) return;

                const char* name = ap::airplay::media_remote_command_name(command);
                LOG_INFO << "SMTC button pressed: " << name;

                if (!ap::airplay::send_active_media_remote_command(command)) {
                    LOG_WARN << "SMTC -> AirPlay command failed: " << name;
                }
            });

        {
            std::lock_guard<std::mutex> lock(mtx);
            ok = true;
            ready = true;
        }
        ready_cv.notify_all();
        LOG_INFO << "SMTC media session ready";

        InMemoryRandomAccessStream thumbnail_stream{nullptr};

        while (true) {
            bool local_active = false;
            bool local_playing = true;
            std::string local_title;
            std::string local_artist;
            std::string local_album;
            std::vector<uint8_t> local_cover;
            uint32_t local_elapsed = 0;
            uint32_t local_total = 0;
            bool do_playback = false;
            bool do_metadata = false;
            bool do_cover = false;
            bool do_timeline = false;

            {
                std::unique_lock<std::mutex> lock(mtx);
                cv.wait(lock, [this] {
                    return stop_requested || playback_dirty || metadata_dirty ||
                           cover_dirty || timeline_dirty;
                });

                if (stop_requested) break;

                local_active = active;
                local_playing = playing;
                local_title = title;
                local_artist = artist;
                local_album = album;
                local_cover = cover;
                local_elapsed = elapsed_ms;
                local_total = total_ms;

                do_playback = std::exchange(playback_dirty, false);
                do_metadata = std::exchange(metadata_dirty, false);
                do_cover = std::exchange(cover_dirty, false);
                do_timeline = std::exchange(timeline_dirty, false);
            }

            try {
                bool display_dirty = false;

                if (do_metadata) {
                    if (local_title.empty() &&
                        local_artist.empty() &&
                        local_album.empty()) {
                        updater.ClearAll();
                        updater.Type(MediaPlaybackType::Music);
                    } else {
                        updater.Type(MediaPlaybackType::Music);
                        auto music = updater.MusicProperties();
                        music.Title(utf8_hstring(local_title));
                        music.Artist(utf8_hstring(local_artist));
                        music.AlbumArtist(utf8_hstring(local_artist));
                        music.AlbumTitle(utf8_hstring(local_album));
                    }
                    display_dirty = true;
                }

                if (do_cover) {
                    if (local_cover.empty()) {
                        updater.Thumbnail(nullptr);
                        thumbnail_stream = nullptr;
                    } else {
                        InMemoryRandomAccessStream stream;
                        DataWriter writer(stream);
                        writer.WriteBytes(winrt::array_view<uint8_t const>(
                            local_cover.data(), local_cover.data() + local_cover.size()));
                        writer.StoreAsync().get();
                        writer.DetachStream();
                        stream.Seek(0);
                        updater.Thumbnail(RandomAccessStreamReference::CreateFromStream(stream));
                        thumbnail_stream = stream; // keep backing bytes alive
                    }
                    display_dirty = true;
                }

                if (display_dirty) updater.Update();

                if (do_timeline) {
                    SystemMediaTransportControlsTimelineProperties timeline;
                    const auto zero = ms_to_timespan(0);
                    const auto end = ms_to_timespan(local_total);
                    const auto pos = ms_to_timespan(
                        local_total == 0 ? 0 : std::min(local_elapsed, local_total));
                    timeline.StartTime(zero);
                    timeline.MinSeekTime(zero);
                    timeline.Position(pos);
                    timeline.EndTime(end);
                    timeline.MaxSeekTime(end);
                    controls.UpdateTimelineProperties(timeline);
                }

                if (do_playback) {
                    if (!local_active) {
                        controls.PlaybackRate(0.0);
                        controls.PlaybackStatus(MediaPlaybackStatus::Closed);
                        controls.IsEnabled(false);
                        LOG_INFO << "SMTC state: inactive";
                    } else {
                        // Keep Play/Pause capabilities enabled while the source
                        // is active. Microsoft documents these as the minimum
                        // manual-SMTC transport capabilities.
                        controls.IsPlayEnabled(true);
                        controls.IsPauseEnabled(true);
                        controls.IsNextEnabled(true);
                        controls.IsPreviousEnabled(true);
                        controls.IsEnabled(true);
                        controls.PlaybackRate(local_playing ? 1.0 : 0.0);
                        controls.PlaybackStatus(local_playing
                            ? MediaPlaybackStatus::Playing
                            : MediaPlaybackStatus::Paused);
                        LOG_INFO << "SMTC state: active, "
                                 << (local_playing ? "playing" : "paused");
                    }
                }
            } catch (const winrt::hresult_error& e) {
                LOG_WARN << "SMTC update failed: hr=0x"
                         << std::hex << static_cast<uint32_t>(e.code().value)
                         << std::dec << " " << winrt::to_string(e.message());
            }
        }

        // Remove our session cleanly from the global Windows media list.
        controls.PlaybackRate(0.0);
        controls.PlaybackStatus(MediaPlaybackStatus::Closed);
        controls.IsEnabled(false);
        controls.ButtonPressed(button_token);

        LOG_INFO << "SMTC media session stopped";
    } catch (const winrt::hresult_error& e) {
        LOG_WARN << "SMTC unavailable: hr=0x"
                 << std::hex << static_cast<uint32_t>(e.code().value)
                 << std::dec << " " << winrt::to_string(e.message());
        {
            std::lock_guard<std::mutex> lock(mtx);
            ok = false;
            ready = true;
        }
        ready_cv.notify_all();
    } catch (const std::exception& e) {
        LOG_WARN << "SMTC unavailable: " << e.what();
        {
            std::lock_guard<std::mutex> lock(mtx);
            ok = false;
            ready = true;
        }
        ready_cv.notify_all();
    }

    if (apartment_initialized) winrt::uninit_apartment();
}
#endif

} // namespace ap::platform
