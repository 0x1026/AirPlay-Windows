#pragma once

#include <filesystem>
#include <string>

namespace ap::airplay { struct LiveSettings; }

namespace ap {

inline constexpr const char* default_receiver_name = "AirPlay-Windows";
bool valid_receiver_name(const std::string& name);

struct PersistentSettings {
    std::string receiver_name = default_receiver_name;
    int  mirror_width   = 0;
    int  mirror_height  = 0;
    bool hevc_enabled   = true;
    int  max_fps        = 60;
    int  refresh_rate   = 60;
    bool mirror_hwaccel = false;
    bool vsync_enabled  = true;
    bool fullscreen     = false;
    bool chromeless     = false;
};

std::filesystem::path settings_path();
PersistentSettings  load_settings();
void                save_settings(const PersistentSettings& s);
PersistentSettings  snapshot(const ap::airplay::LiveSettings& live);

} // namespace ap
