// AirPlay-Windows — first-skeleton entry point.
//
// Responsibilities of main():
//   1. Initialise Winsock (net::global_init).
//   2. Build a DeviceContext (identifiers echoed in both mDNS TXT and /info).
//   3. Start the AirPlay RTSP-like server on TCP 7000.
//   4. Start the native mDNS advertisement (Windows DnsService API).
//   5. Block until Ctrl-C, then tear everything down cleanly.

#include "airplay/hls_local_server.h"
#include "airplay/live_settings.h"
#include "airplay/server.h"
#include "video/hls_player.h"
#include "crypto/identity.h"
#include "log.h"
#include "mdns/mdns_service.h"
#include "net/socket.h"
#include "settings.h"
#include "usb/usb_supervisor.h"
#include "video/video_renderer.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <csignal>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <shellapi.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

#if defined(_WIN32)
BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE;
    }
    return FALSE;
}

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          value.data(),
                                          static_cast<int>(value.size()),
                                          nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        value.data(), static_cast<int>(value.size()),
                        out.data(), count);
    return out;
}

constexpr wchar_t kSingleInstanceMutexName[] =
    L"Local\\AirPlayWindowsSingleInstance";
constexpr wchar_t kActivationEventName[] =
    L"Local\\AirPlayWindowsActivate";

class SingleInstanceGuard {
public:
    SingleInstanceGuard() = default;

    ~SingleInstanceGuard() {
        if (mutex_) CloseHandle(mutex_);
    }

    SingleInstanceGuard(const SingleInstanceGuard&) = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    // Returns true for the first instance. A later interactive launch
    // signals the already-running tray instance to show its renderer and
    // returns false so the duplicate process can exit immediately.
    // Startup launches use --start-hidden, so they never unexpectedly pop
    // the existing renderer to the foreground.
    bool acquire(bool activate_existing) {
        mutex_ = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
        if (!mutex_) {
            LOG_WARN << "single-instance mutex could not be created; continuing";
            return true; // Fail open rather than preventing AirPlay startup.
        }

        if (GetLastError() != ERROR_ALREADY_EXISTS) return true;

        CloseHandle(mutex_);
        mutex_ = nullptr;

        if (activate_existing) {
            // The first process may still be starting its tray thread. Give
            // it a short window to create the activation event before giving
            // up. This only runs in the duplicate process.
            for (int attempt = 0; attempt < 40; ++attempt) {
                HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                          kActivationEventName);
                if (event) {
                    SetEvent(event);
                    CloseHandle(event);
                    return false;
                }
                Sleep(50);
            }
        }

        return false;
    }

private:
    HANDLE mutex_{nullptr};
};

class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon() { stop(); }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool start(ap::video::VideoRenderer* renderer, const std::string& receiver_name) {
        if (thread_.joinable()) return true;
        renderer_ = renderer;
        receiver_name_ = receiver_name;

        activate_event_ = CreateEventW(nullptr, FALSE, FALSE,
                                       kActivationEventName);
        if (!activate_event_) {
            LOG_ERROR << "system tray activation event could not be created";
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(ready_mtx_);
            ready_ = false;
            ok_ = false;
        }
        thread_ = std::thread(&TrayIcon::thread_fn, this);
        std::unique_lock<std::mutex> lock(ready_mtx_);
        ready_cv_.wait(lock, [this] { return ready_; });
        return ok_;
    }

    void stop() {
        HWND hwnd = hwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        if (thread_.joinable()) thread_.join();
        hwnd_.store(nullptr);
        if (activate_event_) {
            CloseHandle(activate_event_);
            activate_event_ = nullptr;
        }
    }

private:
    static constexpr UINT kTrayMessage = WM_APP + 0x31;
    static constexpr UINT kCmdToggle   = 1001;
    static constexpr UINT kCmdExit     = 1002;
    static constexpr UINT kIconId      = 1;

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg,
                                     WPARAM wparam, LPARAM lparam) {
        TrayIcon* self = reinterpret_cast<TrayIcon*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<TrayIcon*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(self));
        }

        if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);

        if (msg == kTrayMessage) {
            const UINT mouse_msg = LOWORD(static_cast<DWORD>(lparam));
            if (mouse_msg == WM_LBUTTONUP || mouse_msg == WM_LBUTTONDBLCLK) {
                if (self->renderer_) self->renderer_->show_window();
                return 0;
            }
            if (mouse_msg == WM_RBUTTONUP || mouse_msg == WM_CONTEXTMENU) {
                self->show_menu();
                return 0;
            }
        }

        switch (msg) {
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kCmdToggle:
                        if (self->renderer_) {
                            if (self->renderer_->window_visible())
                                self->renderer_->hide_window();
                            else
                                self->renderer_->show_window();
                        }
                        return 0;
                    case kCmdExit:
                        g_stop = true;
                        return 0;
                    default:
                        break;
                }
                break;
            case WM_CLOSE:
                DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                self->remove_icon();
                PostQuitMessage(0);
                return 0;
            default:
                break;
        }

        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    void show_menu() {
        if (!renderer_) return;

        HMENU menu = CreatePopupMenu();
        if (!menu) return;

        AppendMenuW(menu, MF_STRING, kCmdToggle,
                    renderer_->window_visible() ? L"Hide AirPlay-Windows"
                                                : L"Show AirPlay-Windows");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit");

        POINT pt{};
        GetCursorPos(&pt);
        HWND hwnd = hwnd_.load();
        SetForegroundWindow(hwnd);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
        PostMessageW(hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    bool add_icon(HWND hwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = kIconId;
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = kTrayMessage;
        nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

        std::wstring tip = L"AirPlay-Windows";
        const std::wstring receiver = utf8_to_wide(receiver_name_);
        if (!receiver.empty()) {
            tip += L" — ";
            tip += receiver;
        }
        wcsncpy_s(nid.szTip, _countof(nid.szTip), tip.c_str(), _TRUNCATE);

        if (!Shell_NotifyIconW(NIM_ADD, &nid)) return false;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        icon_added_ = true;
        return true;
    }

    void remove_icon() {
        if (!icon_added_) return;
        HWND hwnd = hwnd_.load();
        if (hwnd) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd;
            nid.uID = kIconId;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        icon_added_ = false;
    }

    void thread_fn() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        const wchar_t* class_name = L"AirPlayWindowsTrayWindow";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = &TrayIcon::wnd_proc;
        wc.hInstance = instance;
        wc.lpszClassName = class_name;

        ATOM atom = RegisterClassExW(&wc);
        if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            signal_ready(false);
            return;
        }

        HWND hwnd = CreateWindowExW(0, class_name, L"AirPlay-Windows Tray",
                                    0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    instance, this);
        if (!hwnd) {
            signal_ready(false);
            return;
        }
        hwnd_.store(hwnd);

        if (!add_icon(hwnd)) {
            DestroyWindow(hwnd);
            signal_ready(false);
            return;
        }

        LOG_INFO << "system tray icon ready";
        signal_ready(true);

        bool quit = false;
        while (!quit) {
            const DWORD wait = MsgWaitForMultipleObjects(
                1, &activate_event_, FALSE, INFINITE, QS_ALLINPUT);

            if (wait == WAIT_OBJECT_0) {
                if (renderer_) renderer_->show_window();
                continue;
            }

            if (wait != WAIT_OBJECT_0 + 1) {
                LOG_WARN << "system tray wait failed: " << GetLastError();
                break;
            }

            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) {
                    quit = true;
                    break;
                }
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }

        hwnd_.store(nullptr);
        LOG_INFO << "system tray icon stopped";
    }

    void signal_ready(bool ok) {
        {
            std::lock_guard<std::mutex> lock(ready_mtx_);
            ok_ = ok;
            ready_ = true;
        }
        ready_cv_.notify_all();
    }

    ap::video::VideoRenderer* renderer_{nullptr};
    std::string receiver_name_;
    std::thread thread_;
    std::atomic<HWND> hwnd_{nullptr};
    HANDLE activate_event_{nullptr};
    bool icon_added_{false};
    std::mutex ready_mtx_;
    std::condition_variable ready_cv_;
    bool ready_{false};
    bool ok_{false};
};
#else
void signal_handler(int) { g_stop = true; }
#endif

// Pick sensible defaults. The PI and feature bitmask are copied from RPiPlay's
// defaults — known to pass the initial iOS detection phase.
//
// `hls_playback` gates the AirPlay Streaming HLS proxy path (bits 0+4 of
// features low word). When it's off, iOS routes YouTube / Photos to the
// audio fallback — the mirror + RAOP-audio pipeline that actually behaves
// like AirPlay mirror (low-latency, native). When on, iOS hands us an
// mlhls:// URL and we play back via libavformat — works but acts like a
// VOD player (probing, buffering, occasional stutter), best treated as
// an opt-in experimental path.
ap::airplay::DeviceContext build_device_context(bool hls_playback, const std::string& receiver_name) {
    ap::airplay::DeviceContext ctx;
    ctx.name     = receiver_name;
    ctx.deviceid = ap::net::primary_mac();
    ctx.model    = "AppleTV3,2";
    ctx.pi       = "b08f5a79-db29-4384-b456-a4784d9e6055";
    // Features:
    //   0x5A7FFEE6 - UxPlay default (mirror + audio, no HLS/video push).
    //   + bit 0    - AirPlay video supported  (enables AirPlay Streaming)
    //   + bit 4    - HTTP live streaming (HLS) supported
    //   High word 0x400: bit 42 "Screen Multi Codec" = HEVC mirror.
    ctx.features = hls_playback ? "0x5A7FFEF7,0x400"   // video+HLS+HEVC
                                : "0x5A7FFEE6,0x400";  // HEVC mirror only
    ctx.srcvers  = "220.68";
    return ctx;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    // CRT narrow argv uses the system code page. DNS and settings use UTF-8.
    int wide_argc = 0;
    LPWSTR* wide_argv = CommandLineToArgvW(GetCommandLineW(), &wide_argc);
    if (!wide_argv) return 1;
    std::vector<std::string> utf8_args;
    for (int i = 0; i < wide_argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            wide_argv[i], -1, nullptr, 0, nullptr, nullptr);
        if (!size) { LocalFree(wide_argv); return 1; }
        std::string value(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1,
            value.data(), size, nullptr, nullptr);
        value.pop_back(); // Remove the converted terminator.
        utf8_args.push_back(std::move(value));
    }
    LocalFree(wide_argv);
    std::vector<char*> utf8_argv;
    for (auto& value : utf8_args) utf8_argv.push_back(value.data());
    utf8_argv.push_back(nullptr);
    argc = wide_argc;
    argv = utf8_argv.data();
#endif
    [[maybe_unused]] bool show_help = false;
    bool early_start_hidden = false;
    // Logs are off by default in the published binary. Scan for the
    // toggle BEFORE anything else so even the very first init message
    // honours the flag.
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--name" && i + 1 < argc &&
            std::string(argv[i + 1]).rfind("--", 0) != 0) { ++i; continue; }
        if (arg == "--help" || arg == "-h") show_help = true;
        if (arg == "--start-hidden") early_start_hidden = true;
        if (std::string(argv[i]) == "--log" ||
            std::string(argv[i]) == "--verbose") {
            ap::set_log_enabled(true);
        }
    }

#if defined(_WIN32)
    // We're a /SUBSYSTEM:WINDOWS binary so no console pops up by
    // default. With --log, attach to the parent terminal if we were
    // launched from one (cmd, powershell, terminal); otherwise spawn
    // a fresh console window so the user sees the stderr stream.
    if (ap::is_log_enabled() || show_help) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        FILE* dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
#endif

#if defined(_WIN32)
    // Keep one receiver process per interactive Windows session. Starting
    // AirPlay-Windows from Start/Search while the tray instance is already
    // running activates that existing window instead of binding a second
    // server/tray instance. The --start-hidden Startup shortcut is treated
    // as a background launch and therefore does not force an existing
    // renderer window open.
    SingleInstanceGuard single_instance;
    if (!single_instance.acquire(!early_start_hidden)) {
        return 0;
    }
#endif

    auto saved = ap::load_settings();
    std::string receiver_name = saved.receiver_name;
    if (saved.mirror_width == 0 || saved.mirror_height == 0) {
        saved.mirror_width = 2560;
        saved.mirror_height = 1440;
        if (SDL_Init(SDL_INIT_VIDEO) == 0) {
            SDL_DisplayMode dm{};
            if (SDL_GetCurrentDisplayMode(0, &dm) == 0 &&
                dm.w >= 320 && dm.h >= 240 &&
                dm.w <= 7680 && dm.h <= 4320) {
                saved.mirror_width = dm.w;
                saved.mirror_height = dm.h;
            }
            SDL_Quit();
        }
    }

    if (!ap::net::global_init()) {
        return 1;
    }

#if defined(_WIN32)
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    // Default mode: native AirPlay (mirror for video, RAOP for audio).
    // This matches UxPlay's "no --hls" behaviour — low-latency, reliable,
    // but YouTube/Photos play audio-only with thumbnail. Opt-in HLS
    // proxy for the rare cases where you want the video stream delivered
    // via the signed-CDN path we built.
    bool hls_playback = false;
    bool start_hidden = early_start_hidden;
    bool mirror_hwaccel = saved.mirror_hwaccel;
    int  mirror_w = saved.mirror_width;
    int  mirror_h = saved.mirror_height;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--name" || arg.rfind("--name=", 0) == 0) {
            std::string value;
            if (arg == "--name") {
                if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0)
                    value = argv[++i];
            } else {
                value = arg.substr(7);
            }
            if (!ap::valid_receiver_name(value)) {
                const char* error = "--name requires 1-50 UTF-8 bytes, without "
                    "control characters, dots, backslashes or outer spaces.";
                std::fprintf(stderr, "%s\n", error);
#if defined(_WIN32)
                if (!ap::is_log_enabled() && !show_help)
                    MessageBoxA(nullptr, error, "AirPlay-Windows", MB_OK | MB_ICONERROR);
#endif
                ap::net::global_shutdown();
                return 2;
            }
            receiver_name = value;
        } else if (arg == "--hls-proxy-playback" || arg == "--hls") {
            hls_playback = true;
        } else if (arg == "--mirror-hwaccel") {
            mirror_hwaccel = true;
        } else if (arg == "--start-hidden") {
            start_hidden = true;
        } else if ((arg == "--mirror-res" || arg == "-r") && i + 1 < argc) {
            // Accept "WxH" or "WxHi" with either separator, e.g. 1920x1080,
            // 2560x1440, 3840x2160. Anything malformed falls back silently
            // to the default and logs a warning.
            const std::string val = argv[++i];
            const auto x = val.find_first_of("xX*");
            if (x != std::string::npos) {
                try {
                    const int w = std::stoi(val.substr(0, x));
                    const int h = std::stoi(val.substr(x + 1));
                    if (w >= 320 && h >= 240 &&
                        w <= 7680 && h <= 4320) {
                        mirror_w = w;
                        mirror_h = h;
                    } else {
                        LOG_WARN << "--mirror-res out of range (" << val
                                 << "), keeping default";
                    }
                } catch (...) {
                    LOG_WARN << "--mirror-res not parseable (" << val
                             << "), keeping default";
                }
            }
        } else if (arg == "--log" || arg == "--verbose") {
            // Already handled in the pre-init scan above; consume so
            // future unknown-arg paths don't trip on it.
        } else if (arg == "--help" || arg == "-h") {
            std::puts(
                "AirPlay-Windows [--name NAME] [--log] [--hls-proxy-playback]\n"
                "                [--mirror-res WxH] [--mirror-hwaccel] [--start-hidden]\n"
                "  --name NAME          AirPlay/RAOP receiver name (also --name=NAME)\n"
                "                        default: saved name or AirPlay-Windows;\n"
                "                        saved on normal exit; 1-50 UTF-8 bytes,\n"
                "                        no controls, dots, backslashes or outer spaces\n"
                "  --log / --verbose     print init/runtime logs to stderr\n"
                "                        (off by default in the released\n"
                "                        binary)\n"
                "  --start-hidden        start with the renderer window hidden;\n"
                "                        intended for Windows logon/startup use\n"
                "                        with the system tray icon\n"
                "  --hls-proxy-playback  advertise HLS capability so iOS\n"
                "                        hands video URLs to our proxy\n"
                "                        (experimental, VOD-style latency)\n"
                "  --mirror-res WxH      advertise the given display\n"
                "                        resolution to iOS (default: saved or\n"
                "                        detected display, else 2560x1440;\n"
                "                        higher = sharper portrait at the cost\n"
                "                        of bandwidth, common: 1920x1080,\n"
                "                        2560x1440, 3840x2160)\n"
                "  --mirror-hwaccel      decode mirror H.264/HEVC on the GPU\n"
                "                        (D3D11VA, GPU-resident presentation).\n"
                "                        No CPU pixel readback for display.\n"
                "                        Software fallback if unavailable.");
            ap::net::global_shutdown();
            return 0;
        }
    }

    auto ctx = build_device_context(hls_playback, receiver_name);
    ctx.mirror_width   = mirror_w;
    ctx.mirror_height  = mirror_h;
    ctx.mirror_hwaccel = mirror_hwaccel;
    LOG_INFO << "Mirror display advertised: " << mirror_w << 'x' << mirror_h;
    LOG_INFO << "Mirror decoder default: "
             << (mirror_hwaccel
                   ? "GPU hwaccel (--mirror-hwaccel; D3D11VA resident surfaces)"
                   : "software libavcodec (toggle GPU in OPTIONS panel)");

    // Live-mutable settings exposed to the overlay UI. Seed from the
    // CLI defaults; the UI thread can change values at runtime, and
    // /info reads them on every iPhone handshake (so changes take
    // effect on the next reconnect).
    ap::airplay::LiveSettings live_settings;
    live_settings.mirror_width.store(mirror_w);
    live_settings.mirror_height.store(mirror_h);
    live_settings.hevc_enabled.store(saved.hevc_enabled);
    live_settings.max_fps.store(saved.max_fps);
    live_settings.refresh_rate.store(saved.refresh_rate);
    live_settings.mirror_hwaccel.store(mirror_hwaccel);
    live_settings.vsync_enabled.store(saved.vsync_enabled);
    live_settings.fullscreen.store(saved.fullscreen);
    live_settings.chromeless.store(saved.chromeless);
    live_settings.video_rotation.store(saved.video_rotation);
    ctx.live = &live_settings;
    LOG_INFO << "AirPlay Streaming HLS path: "
             << (hls_playback ? "ENABLED (--hls-proxy-playback)"
                              : "disabled (mirror + RAOP audio only)");

    // Load (or create) the persistent Ed25519 identity. iOS caches receivers
    // by pk — keeping a stable keypair avoids re-pairing on every launch.
    auto identity = ap::crypto::Identity::load_or_create("identity.key");
    if (!identity) {
        LOG_ERROR << "failed to initialise identity — aborting";
        ap::net::global_shutdown();
        return 1;
    }
    ctx.public_key = identity->public_key();
    ctx.identity   = identity.get();

    LOG_INFO << "=== AirPlay-Windows skeleton ===";
    LOG_INFO << "name="     << ctx.name;
    LOG_INFO << "deviceid=" << ctx.deviceid;
    const std::string local_ip   = ap::net::primary_ipv4();
    const std::string local_ipv6 = ap::net::primary_ipv6();
    LOG_INFO << "ip="       << local_ip;
    if (!local_ipv6.empty()) {
        LOG_INFO << "ipv6="  << local_ipv6;
    }
    LOG_INFO << "pk(Ed25519)=" << ctx.public_key.size() << " bytes";

    // Start the SDL2 renderer. On Windows the native notification-area
    // icon owns the close-to-tray / explicit Exit lifecycle.
    ap::video::VideoRenderer renderer;
    renderer.set_live_settings(&live_settings);
    if (!renderer.start("AirPlay-Windows", start_hidden)) {
        LOG_WARN << "VideoRenderer could not start — running headless";
    }

#if defined(_WIN32)
    TrayIcon tray;
    const bool tray_available = tray.start(&renderer, ctx.name);
    renderer.set_close_hides(tray_available);
    if (!tray_available) {
        LOG_WARN << "system tray unavailable — window close will exit";
        if (start_hidden) renderer.show_window();
    }
#else
    const bool tray_available = false;
    if (start_hidden) {
        LOG_WARN << "--start-hidden requires the Windows tray backend; showing window";
        renderer.show_window();
    }
#endif
    // Prefer the v4 address on the idle screen (shorter, more familiar),
    // but fall back to v6 if we're on a v6-only network. Without this,
    // an v6-only host would display "0.0.0.0" to the user.
    const std::string display_ip =
        (local_ip.empty() || local_ip == "0.0.0.0") && !local_ipv6.empty()
            ? local_ipv6 : local_ip;
    renderer.set_idle_info(ctx.name, display_ip);
    ctx.renderer = &renderer;

    ap::video::HlsPlayer hls_player;
    if (hls_playback) ctx.hls_player = &hls_player;

    ap::airplay::Server server;
    if (!server.start(ctx, 7000)) {
        LOG_ERROR << "Failed to start AirPlay server";
        renderer.stop();
        ap::net::global_shutdown();
        return 1;
    }
    // Wire the toolbar "Disconnect" button. Closing the RTSP TCP
    // sockets cascades into the per-session cleanup that tears down
    // mirror + audio without a process restart. We also wipe the
    // renderer's session state directly because a forced TCP close
    // doesn't go through the normal TEARDOWN path that would do it.
    renderer.set_disconnect_handler([&server, &renderer]() {
        server.disconnect_clients();
        renderer.clear_session();
        renderer.clear_active_device();
    });

    // Local HTTP proxy + libavformat player only when the HLS path is
    // opted into. In the default (mirror + RAOP) mode nothing connects
    // to them anyway, but skipping the bind avoids the port claim and
    // keeps a clean log.
    ap::airplay::HlsLocalServer hls_server;
    if (hls_playback) {
        if (!hls_server.start(7100)) {
            LOG_WARN << "HLS local server could not bind 7100 — "
                        "--hls-proxy-playback playback will not work";
        }
        ctx.hls_local_port = hls_server.port();
    }

    ap::mdns::MdnsService mdns;
    if (!mdns.start(ctx, server.port())) {
        LOG_WARN << "mDNS not available — receiver won't auto-appear on iOS";
    }

    // Watch for USB-connected iPhones in parallel with the Wi-Fi mDNS
    // path. Phase 1: just logs arrivals + paired/not-paired. The full
    // QuickTime mirror session is layered on top in later phases —
    // when ready, the supervisor will hand a connected device off to
    // the same VideoRenderer / AudioOutput that mirror_listener uses.
    ap::usb::UsbSupervisor usb_supervisor;
    usb_supervisor.start();

    if (tray_available) {
        LOG_INFO << "Ready. Window close hides to tray; use tray Exit or Ctrl-C to exit.";
        while (!g_stop) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    } else {
        LOG_INFO << "Ready. Close the window or press Ctrl-C to exit.";
        while (!g_stop && !renderer.user_closed()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    LOG_INFO << "Shutting down... (g_stop=" << g_stop.load()
             << " user_closed=" << renderer.user_closed() << ")";
#if defined(_WIN32)
    tray.stop();
#endif
    LOG_INFO << "shutdown: stopping USB supervisor";
    usb_supervisor.stop();
    LOG_INFO << "shutdown: USB supervisor stopped";

    LOG_INFO << "shutdown: stopping mDNS";
    mdns.stop();
    LOG_INFO << "shutdown: mDNS stopped";

    LOG_INFO << "shutdown: stopping HLS player";
    hls_player.stop();
    LOG_INFO << "shutdown: HLS player stopped";

    LOG_INFO << "shutdown: stopping HLS server";
    hls_server.stop();
    LOG_INFO << "shutdown: HLS server stopped";

    LOG_INFO << "shutdown: stopping AirPlay server";
    server.stop();
    LOG_INFO << "shutdown: AirPlay server stopped";

    auto final_settings = ap::snapshot(live_settings);
    final_settings.receiver_name = ctx.name;
    ap::save_settings(final_settings);

    renderer.stop();

    // Final global SDL cleanup: by this point both video and AirPlay audio
    // users are fully stopped, so no live device can be invalidated.
    SDL_Quit();

    ap::net::global_shutdown();
    return 0;
}
