#pragma once
#include "rawaccel.hpp"
#include <string>
#include <vector>
#include <stdexcept>

namespace rawaccel {

static constexpr const char* DEFAULT_CONFIG_PATH = "/etc/rawaccel/settings.json";

struct device_config {
    bool   disable         = false;
    int    dpi             = 800;
    int    polling_rate    = 1000;
};

struct device_profile {
    std::string  device_id;     // empty = apply to all mice
    std::string  name;
    device_config dev_cfg;
    profile      prof;
};

struct app_config {
    std::vector<device_profile> profiles;
    std::string                 active_profile = "default";
    bool                        use_raw_input  = true;
};

/// Load config from a JSON file. Throws on parse error.
app_config load_config(const std::string& path);

/// Save config to a JSON file.
void save_config(const app_config& cfg, const std::string& path);

/// Convert a profile to/from JSON string (for IPC).
std::string profile_to_json(const device_profile& p);
device_profile profile_from_json(const std::string& json_str);

/// Sanitize a device_profile in-place (clamp DPI, polling_rate, rotation, etc.).
void sanitize_device_profile(device_profile& dp);

/// Find or create config path (user home or /etc).
std::string find_config_path();

} // namespace rawaccel
