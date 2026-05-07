#include "config.hpp"
#include "nlohmann/json.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <limits>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <pwd.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace rawaccel {

// ── Helpers ───────────────────────────────────────────────────────────────────

static accel_mode str_to_mode(const std::string& s) {
    if (s == "classic")     return accel_mode::classic;
    if (s == "power")       return accel_mode::power;
    if (s == "natural")     return accel_mode::natural;
    if (s == "jump")        return accel_mode::jump;
    if (s == "synchronous") return accel_mode::synchronous;
    if (s == "lookup")      return accel_mode::lookup;
    return accel_mode::noaccel;
}

static std::string mode_to_str(accel_mode m) {
    switch (m) {
    case accel_mode::classic:     return "classic";
    case accel_mode::power:       return "power";
    case accel_mode::natural:     return "natural";
    case accel_mode::jump:        return "jump";
    case accel_mode::synchronous: return "synchronous";
    case accel_mode::lookup:      return "lookup";
    default:                      return "noaccel";
    }
}

static cap_mode str_to_cap(const std::string& s) {
    if (s == "io") return cap_mode::io;
    if (s == "in") return cap_mode::in;
    return cap_mode::out;
}

static std::string cap_to_str(cap_mode c) {
    switch (c) {
    case cap_mode::io: return "io";
    case cap_mode::in: return "in";
    default:           return "out";
    }
}

// ── accel_args serialization ──────────────────────────────────────────────────

static json accel_args_to_json(const accel_args& a) {
    json j;
    j["mode"]             = mode_to_str(a.mode);
    j["gain"]             = a.gain;
    j["input_offset"]     = a.input_offset;
    j["output_offset"]    = a.output_offset;
    j["acceleration"]     = a.acceleration;
    j["decay_rate"]       = a.decay_rate;
    j["gamma"]            = a.gamma;
    j["motivity"]         = a.motivity;
    j["exponent_classic"] = a.exponent_classic;
    j["scale"]            = a.scale;
    j["exponent_power"]   = a.exponent_power;
    j["limit"]            = a.limit;
    j["sync_speed"]       = a.sync_speed;
    j["smooth"]           = a.smooth;
    j["cap"]              = { a.cap.x, a.cap.y };
    j["cap_mode"]         = cap_to_str(a.cap_mode_val);
    // LUT data
    if (a.length > 0) {
        json pts = json::array();
        for (int i = 0; i < a.length; i++)
            pts.push_back(a.data[i]);
        j["lut_data"] = pts;
        j["lut_length"] = a.length;
    }
    return j;
}

static accel_args accel_args_from_json(const json& j) {
    accel_args a;
    if (j.contains("mode"))             a.mode             = str_to_mode(j["mode"].get<std::string>());
    if (j.contains("gain"))             a.gain             = j["gain"].get<bool>();
    if (j.contains("input_offset"))     a.input_offset     = j["input_offset"].get<double>();
    if (j.contains("output_offset"))    a.output_offset    = j["output_offset"].get<double>();
    if (j.contains("acceleration"))     a.acceleration     = j["acceleration"].get<double>();
    if (j.contains("decay_rate"))       a.decay_rate       = j["decay_rate"].get<double>();
    if (j.contains("gamma"))            a.gamma            = j["gamma"].get<double>();
    if (j.contains("motivity"))         a.motivity         = j["motivity"].get<double>();
    if (j.contains("exponent_classic")) a.exponent_classic = j["exponent_classic"].get<double>();
    if (j.contains("scale"))            a.scale            = j["scale"].get<double>();
    if (j.contains("exponent_power"))   a.exponent_power   = j["exponent_power"].get<double>();
    if (j.contains("limit"))            a.limit            = j["limit"].get<double>();
    if (j.contains("sync_speed"))       a.sync_speed       = j["sync_speed"].get<double>();
    if (j.contains("smooth"))           a.smooth           = j["smooth"].get<double>();
    if (j.contains("cap")) {
        auto& cap = j["cap"];
        // O3: silently use default on missing element or wrong type instead of throwing
        if (cap.is_array() && cap.size() >= 2) {
            a.cap.x = cap[0].get<double>();
            a.cap.y = cap[1].get<double>();
        }
    }
    if (j.contains("cap_mode"))  a.cap_mode_val = str_to_cap(j["cap_mode"].get<std::string>());
    if (j.contains("lut_data") && j.contains("lut_length")) {
        // BUG-5: nlohmann::json::get<int>() invokes UB when the JSON value
        // doesn't fit in `int` (libFuzzer + UBSan caught this with payloads
        // like `"lut_length": 1e26`).  Read as a double first, range-clamp,
        // then cast — the cast is now defined.
        double raw = j["lut_length"].is_number()
                     ? j["lut_length"].get<double>() : 0.0;
        if (!std::isfinite(raw) || raw < 0) raw = 0;
        if (raw > (double)LUT_RAW_DATA_CAPACITY) raw = LUT_RAW_DATA_CAPACITY;
        a.length = static_cast<int>(raw);
        auto& pts = j["lut_data"];
        int n = std::min((int)pts.size(), a.length);
        for (int i = 0; i < n; i++) {
            // Same defence on the LUT entries themselves.
            double v = pts[i].is_number() ? pts[i].get<double>() : 0.0;
            if (!std::isfinite(v)) v = 0;
            // double → float overflow yields ±Inf which corrupts the LUT
            // binary search.  Clamp to the float-representable range first.
            constexpr double FLT_HI = static_cast<double>(std::numeric_limits<float>::max());
            if (v > FLT_HI)  v = FLT_HI;
            if (v < -FLT_HI) v = -FLT_HI;
            a.data[i] = static_cast<float>(v);
        }
    }
    return a;
}

// ── profile serialization ─────────────────────────────────────────────────────

static json profile_to_json_obj(const profile& p) {
    json j;
    j["name"]              = std::string(p.name);
    j["raw_passthrough"]   = p.raw_passthrough;
    j["domain_weights"]    = { p.domain_weights.x, p.domain_weights.y };
    j["range_weights"]     = { p.range_weights.x,  p.range_weights.y  };
    j["accel_x"]           = accel_args_to_json(p.accel_x);
    j["accel_y"]           = accel_args_to_json(p.accel_y);
    j["output_dpi"]        = p.output_dpi;
    j["degrees_rotation"]  = p.degrees_rotation;
    j["degrees_snap"]      = p.degrees_snap;
    j["speed_min"]         = p.speed_min;
    j["speed_max"]         = p.speed_max;
    j["lr_output_dpi_ratio"] = p.lr_output_dpi_ratio;
    j["ud_output_dpi_ratio"] = p.ud_output_dpi_ratio;

    // speed_processor_args
    auto& sp = p.speed_processor_args;
    j["speed_processor"] = {
        {"whole",                       sp.whole},
        {"lp_norm",                     sp.lp_norm},
        {"input_speed_smooth_halflife",  sp.input_speed_smooth_halflife},
        {"scale_smooth_halflife",        sp.scale_smooth_halflife},
        {"output_speed_smooth_halflife", sp.output_speed_smooth_halflife},
    };
    return j;
}

static profile profile_from_json_obj(const json& j) {
    profile p;

    if (j.contains("name")) {
        auto s = j["name"].get<std::string>();
        std::strncpy(p.name, s.c_str(), MAX_NAME_LEN - 1);
        // strncpy doesn't write a null terminator when src ≥ N.  Default
        // construction zero-fills the array, but defend in depth in case
        // the caller passes a previously-populated profile.
        p.name[MAX_NAME_LEN - 1] = '\0';
    }
    if (j.contains("raw_passthrough")) p.raw_passthrough = j["raw_passthrough"].get<bool>();
    if (j.contains("domain_weights")) {
        auto& dw = j["domain_weights"];
        if (dw.is_array() && dw.size() >= 2) {
            p.domain_weights.x = dw[0].get<double>();
            p.domain_weights.y = dw[1].get<double>();
        }
    }
    if (j.contains("range_weights")) {
        auto& rw = j["range_weights"];
        if (rw.is_array() && rw.size() >= 2) {
            p.range_weights.x = rw[0].get<double>();
            p.range_weights.y = rw[1].get<double>();
        }
    }
    if (j.contains("accel_x")) p.accel_x = accel_args_from_json(j["accel_x"]);
    if (j.contains("accel_y")) p.accel_y = accel_args_from_json(j["accel_y"]);
    if (j.contains("output_dpi"))         p.output_dpi         = j["output_dpi"].get<double>();
    if (j.contains("degrees_rotation"))   p.degrees_rotation   = j["degrees_rotation"].get<double>();
    if (j.contains("degrees_snap"))       p.degrees_snap       = j["degrees_snap"].get<double>();
    if (j.contains("speed_min"))          p.speed_min          = j["speed_min"].get<double>();
    if (j.contains("speed_max"))          p.speed_max          = j["speed_max"].get<double>();
    if (j.contains("lr_output_dpi_ratio")) p.lr_output_dpi_ratio = j["lr_output_dpi_ratio"].get<double>();
    if (j.contains("ud_output_dpi_ratio")) p.ud_output_dpi_ratio = j["ud_output_dpi_ratio"].get<double>();

    if (j.contains("speed_processor")) {
        auto& sp_j = j["speed_processor"];
        auto& sp   = p.speed_processor_args;
        if (sp_j.contains("whole"))                       sp.whole                        = sp_j["whole"].get<bool>();
        if (sp_j.contains("lp_norm"))                     sp.lp_norm                      = sp_j["lp_norm"].get<double>();
        if (sp_j.contains("input_speed_smooth_halflife")) sp.input_speed_smooth_halflife  = sp_j["input_speed_smooth_halflife"].get<double>();
        if (sp_j.contains("scale_smooth_halflife"))       sp.scale_smooth_halflife        = sp_j["scale_smooth_halflife"].get<double>();
        if (sp_j.contains("output_speed_smooth_halflife")) sp.output_speed_smooth_halflife = sp_j["output_speed_smooth_halflife"].get<double>();
    }

    return p;
}

// ── device_profile serialization ─────────────────────────────────────────────

static json device_profile_to_json(const device_profile& dp) {
    json j;
    j["name"]       = dp.name;
    j["device_id"]  = dp.device_id;
    j["dpi"]        = dp.dev_cfg.dpi;
    j["polling_rate"] = dp.dev_cfg.polling_rate;
    j["disable"]    = dp.dev_cfg.disable;
    j["profile"]    = profile_to_json_obj(dp.prof);
    return j;
}

/// Clamp device_config fields to safe ranges.
/// Called after JSON deserialization to prevent bad values reaching the daemon.
static void sanitize_device_config(device_config& dc) {
    // DPI: 1–32000 (modern sensors go up to 32 000)
    if (dc.dpi < 1)     dc.dpi = 1;
    if (dc.dpi > 32000) dc.dpi = 32000;
    // Polling rate: 125–8000 Hz
    if (dc.polling_rate < POLL_RATE_MIN) dc.polling_rate = POLL_RATE_MIN;
    if (dc.polling_rate > POLL_RATE_MAX) dc.polling_rate = POLL_RATE_MAX;
}

/// Sort LUT data in-place by speed (ascending) so binary search in lookup::operator()
/// produces correct results.  GUI already sorts via lut_set_points(), but JSON files
/// edited by hand or generated by external tools may have unsorted data.
static void sort_lut_data(accel_args& a) {
    const int n = a.length / 2;
    if (n <= 1) return;
    // Simple insertion sort — n is small (max 257) and avoids heap allocation.
    for (int i = 1; i < n; i++) {
        const size_t si = static_cast<size_t>(i);
        const float kx = a.data[si * 2];
        const float ky = a.data[si * 2 + 1];
        int j = i - 1;
        while (j >= 0 && a.data[static_cast<size_t>(j) * 2] > kx) {
            const size_t sj = static_cast<size_t>(j);
            a.data[(sj + 1) * 2]     = a.data[sj * 2];
            a.data[(sj + 1) * 2 + 1] = a.data[sj * 2 + 1];
            j--;
        }
        const int insert = j + 1;
        a.data[static_cast<size_t>(insert) * 2]     = kx;
        a.data[static_cast<size_t>(insert) * 2 + 1] = ky;
    }
}

/// Clamp accel_args fields to safe ranges.
/// Prevents NaN/Inf from pow(negative, non-integer) and similar edge cases
/// when values come from hand-edited JSON or external tools.
/// Replace non-finite (NaN / Inf) with a safe default.
static inline double finite_or(double v, double def) {
    return std::isfinite(v) ? v : def;
}

static void sanitize_accel_args(accel_args& a) {
    // NaN / Inf guard: NaN silently passes comparison guards (NaN < 0 → false),
    // so we must replace non-finite values with safe defaults first.
    a.acceleration    = finite_or(a.acceleration, 0);
    a.scale           = finite_or(a.scale, 0);
    a.decay_rate      = finite_or(a.decay_rate, 0);
    a.exponent_classic = finite_or(a.exponent_classic, 2);
    a.exponent_power  = finite_or(a.exponent_power, 1);
    a.input_offset    = finite_or(a.input_offset, 0);
    a.output_offset   = finite_or(a.output_offset, 0);
    a.limit           = finite_or(a.limit, 0);
    a.sync_speed      = finite_or(a.sync_speed, 1);
    a.smooth          = finite_or(a.smooth, 0);
    a.motivity        = finite_or(a.motivity, 0);
    a.gamma           = finite_or(a.gamma, 0);
    a.cap.x           = finite_or(a.cap.x, 0);
    a.cap.y           = finite_or(a.cap.y, 0);

    // acceleration: used as pow(acceleration, exp-1) in classic mode.
    //   Negative acceleration with integer exponent is a legitimate deceleration feature.
    //   Negative + non-integer exponent produces NaN, but the classic constructor and
    //   motion_math NaN guard handle this downstream — don't clamp here.
    // scale: used as pow(scale * x, exp) in power mode.
    //   Negative scale * positive x → negative base → NaN with non-integer exp.
    if (a.scale < 0) a.scale = 0;
    // decay_rate: natural mode divides by limit to get internal accel coefficient.
    //   Negative → exp(+large) → diverging gain.  Clamp to >= 0.
    if (a.decay_rate < 0) a.decay_rate = 0;
    // exponent_power: power mode constructor clamps to 1e-4, but sanitize early.
    //   Zero causes division-by-zero in gain_inverse (1/n).
    if (a.exponent_power < 1e-4) a.exponent_power = 1e-4;
    // input_offset, output_offset: negative offsets are meaningless (speed is always >= 0).
    if (a.input_offset < 0) a.input_offset = 0;
    if (a.output_offset < 0) a.output_offset = 0;
    // limit: natural/jump subtract 1 → limit < 1 means deceleration.
    //   Constructors already clamp to max(0, limit-1), but values < 0 are nonsensical.
    if (a.limit < 0) a.limit = 0;
    // sync_speed: synchronous/jump mode midpoint.  Zero → division-by-zero.
    if (a.sync_speed < 1e-4) a.sync_speed = 1e-4;
    // smooth: jump mode steepness factor.  Negative just inverts the sigmoid — harmless
    //   but confusing; clamp to >= 0 for UI consistency.
    if (a.smooth < 0) a.smooth = 0;
    // motivity, gamma: not currently used by any algorithm but stored in config.
    //   Prevent negative values for forward-compatibility.
    if (a.motivity < 0) a.motivity = 0;
    if (a.gamma < 0) a.gamma = 0;
    // cap values: negative caps are meaningless.
    if (a.cap.x < 0) a.cap.x = 0;
    if (a.cap.y < 0) a.cap.y = 0;
}

/// Clamp profile fields to safe ranges.
static void sanitize_profile(profile& p) {
    // NaN / Inf guard on all double fields — NaN silently passes < / > comparisons.
    p.degrees_rotation     = finite_or(p.degrees_rotation, 0);
    p.degrees_snap         = finite_or(p.degrees_snap, 0);
    p.speed_min            = finite_or(p.speed_min, 0);
    p.speed_max            = finite_or(p.speed_max, 0);
    p.lr_output_dpi_ratio  = finite_or(p.lr_output_dpi_ratio, 1);
    p.ud_output_dpi_ratio  = finite_or(p.ud_output_dpi_ratio, 1);
    p.domain_weights.x     = finite_or(p.domain_weights.x, 1);
    p.domain_weights.y     = finite_or(p.domain_weights.y, 1);
    p.range_weights.x      = finite_or(p.range_weights.x, 1);
    p.range_weights.y      = finite_or(p.range_weights.y, 1);
    p.output_dpi           = finite_or(p.output_dpi, NORMALIZED_DPI);
    p.speed_processor_args.lp_norm = finite_or(p.speed_processor_args.lp_norm, 2);
    p.speed_processor_args.input_speed_smooth_halflife =
        finite_or(p.speed_processor_args.input_speed_smooth_halflife, 0);
    p.speed_processor_args.scale_smooth_halflife =
        finite_or(p.speed_processor_args.scale_smooth_halflife, 0);
    p.speed_processor_args.output_speed_smooth_halflife =
        finite_or(p.speed_processor_args.output_speed_smooth_halflife, 0);

    // Rotation: normalize into [0, 360) preserving direction.
    // Using fabs() would map -45° → 45° (wrong direction); the correct
    // mathematical equivalent of -45° is +315°.  fmod() preserves sign,
    // so we add 360 to push negative residues into the positive half.
    p.degrees_rotation = std::fmod(p.degrees_rotation, 360.0);
    if (p.degrees_rotation < 0)         p.degrees_rotation += 360.0;
    if (p.degrees_rotation == 0.0)      p.degrees_rotation  = 0.0;     // clear -0.0 sign
    if (p.degrees_rotation >= 360.0)    p.degrees_rotation  = 0.0;     // FP rounding edge
    // Snap: 0–45 degrees (meaningful range)
    if (p.degrees_snap < 0)  p.degrees_snap = 0;
    if (p.degrees_snap > 45) p.degrees_snap = 45;
    // Output DPI: 1–32000
    if (p.output_dpi < 1)     p.output_dpi = 1;
    if (p.output_dpi > 32000) p.output_dpi = 32000;
    // DPI ratios: 0.01–100
    if (p.lr_output_dpi_ratio < 0.01) p.lr_output_dpi_ratio = 0.01;
    if (p.lr_output_dpi_ratio > 100)  p.lr_output_dpi_ratio = 100;
    if (p.ud_output_dpi_ratio < 0.01) p.ud_output_dpi_ratio = 0.01;
    if (p.ud_output_dpi_ratio > 100)  p.ud_output_dpi_ratio = 100;
    // speed_min / speed_max: non-negative; max >= min if both nonzero
    if (p.speed_min < 0) p.speed_min = 0;
    if (p.speed_max < 0) p.speed_max = 0;
    if (p.speed_max > 0 && p.speed_max < p.speed_min)
        p.speed_max = p.speed_min;
    // lp_norm: must be > 0
    if (p.speed_processor_args.lp_norm <= 0) p.speed_processor_args.lp_norm = 2;
    // Smooth halflifes: negative has no meaning (> 0 check enables smoothing)
    if (p.speed_processor_args.input_speed_smooth_halflife < 0)
        p.speed_processor_args.input_speed_smooth_halflife = 0;
    if (p.speed_processor_args.scale_smooth_halflife < 0)
        p.speed_processor_args.scale_smooth_halflife = 0;
    if (p.speed_processor_args.output_speed_smooth_halflife < 0)
        p.speed_processor_args.output_speed_smooth_halflife = 0;
    // Domain/range weights: negative values invert axes — confusing and unintended.
    // Clamp to a small positive minimum so acceleration math stays well-defined.
    if (p.domain_weights.x < 0) p.domain_weights.x = 0;
    if (p.domain_weights.y < 0) p.domain_weights.y = 0;
    if (p.range_weights.x < 0) p.range_weights.x = 0;
    if (p.range_weights.y < 0) p.range_weights.y = 0;
    // Acceleration arguments: clamp per-algorithm fields to safe ranges
    sanitize_accel_args(p.accel_x);
    sanitize_accel_args(p.accel_y);
    // LUT data: sort by speed so binary search in lookup::operator() works correctly
    sort_lut_data(p.accel_x);
    sort_lut_data(p.accel_y);
}

/// Safely extract an integer from a JSON node.  nlohmann's `get<int>()`
/// triggers UndefinedBehaviorSanitizer when the underlying number doesn't
/// fit in `int` (libFuzzer caught a 1e26 input).  Reading via double, then
/// clamping, then casting avoids that UB regardless of the input.
static int json_get_int_safe(const json& v, int fallback) {
    if (!v.is_number()) return fallback;
    double d = v.get<double>();
    if (!std::isfinite(d)) return fallback;
    if (d < (double)INT_MIN) return INT_MIN;
    if (d > (double)INT_MAX) return INT_MAX;
    return static_cast<int>(d);
}

static device_profile device_profile_from_json(const json& j) {
    device_profile dp;
    if (j.contains("name"))         dp.name          = j["name"].get<std::string>();
    if (j.contains("device_id"))    dp.device_id     = j["device_id"].get<std::string>();
    if (j.contains("dpi"))          dp.dev_cfg.dpi   = json_get_int_safe(j["dpi"], 800);
    if (j.contains("polling_rate")) dp.dev_cfg.polling_rate = json_get_int_safe(j["polling_rate"], 1000);
    if (j.contains("disable"))      dp.dev_cfg.disable = j["disable"].is_boolean()
                                                         ? j["disable"].get<bool>() : false;
    if (j.contains("profile"))      dp.prof          = profile_from_json_obj(j["profile"]);
    // Clamp to safe ranges after loading
    sanitize_device_config(dp.dev_cfg);
    sanitize_profile(dp.prof);
    return dp;
}

// ── Public API ────────────────────────────────────────────────────────────────

app_config load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open config file: " + path);

    json j = json::parse(f);
    app_config cfg;

    if (j.contains("active_profile"))
        cfg.active_profile = j["active_profile"].get<std::string>();
    if (j.contains("use_raw_input"))
        cfg.use_raw_input = j["use_raw_input"].get<bool>();

    if (j.contains("profiles")) {
        for (auto& pj : j["profiles"]) {
            cfg.profiles.push_back(device_profile_from_json(pj));
        }
    }
    return cfg;
}

void save_config(const app_config& cfg, const std::string& path) {
    fs::path parent_path = fs::path(path).parent_path();
    if (!parent_path.empty())
        fs::create_directories(parent_path);

    json j;
    j["active_profile"] = cfg.active_profile;
    j["use_raw_input"]  = cfg.use_raw_input;
    j["profiles"]       = json::array();
    for (auto& dp : cfg.profiles)
        j["profiles"].push_back(device_profile_to_json(dp));

    // Write to a temp file first, then rename — atomic on Linux (same filesystem).
    // This prevents the daemon from reading a half-written/truncated JSON.
    //
    // BUG-13: ofstream::flush() only pushes the userspace buffer to the kernel
    // page cache.  rename() is atomic with respect to other readers on the same
    // FS, but on a power loss the new directory entry may point at a file
    // whose pages were never committed → user finds an empty file after reboot.
    // Cure: fsync() the file before rename, then fsync() the parent directory
    // so the rename itself is durable.
    std::string tmp_path = path + ".tmp";
    {
        std::string content = j.dump(4) + "\n";
        int fd = ::open(tmp_path.c_str(),
                        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0)
            throw std::runtime_error("Cannot write temp config: " + tmp_path +
                                     " (" + std::strerror(errno) + ")");
        const char* p = content.c_str();
        size_t left = content.size();
        while (left > 0) {
            ssize_t w = ::write(fd, p, left);
            if (w < 0) {
                if (errno == EINTR) continue;
                int err = errno;
                ::close(fd); ::unlink(tmp_path.c_str());
                throw std::runtime_error("Write error on temp config: " +
                                         tmp_path + " (" + std::strerror(err) + ")");
            }
            p += w; left -= (size_t)w;
        }
        if (::fsync(fd) != 0) {
            int err = errno;
            ::close(fd); ::unlink(tmp_path.c_str());
            throw std::runtime_error("fsync(temp config) failed: " +
                                     tmp_path + " (" + std::strerror(err) + ")");
        }
        ::close(fd);
    }

    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        // rename failed (e.g. cross-device) — fall back to copy+delete
        // D2: use separate error_code variables for copy_file and remove;
        // otherwise a successful remove would overwrite a copy_file error, silencing it.
        std::error_code ec_copy, ec_rm;
        fs::copy_file(tmp_path, path, fs::copy_options::overwrite_existing, ec_copy);
        fs::remove(tmp_path, ec_rm);
        if (ec_copy)
            throw std::runtime_error("Cannot replace config file: " + path +
                                     " (" + ec_copy.message() + ")");
    }

    // BUG-13 (cont.): fsync the parent directory so the rename is durable.
    // Without this, a power loss between rename() and a kernel writeback
    // could leave the directory entry pointing nowhere.  Best-effort —
    // some filesystems (e.g. NFS) may not honour directory fsync.
    {
        std::string parent = fs::path(path).parent_path().string();
        if (parent.empty()) parent = ".";
        int dfd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)::fsync(dfd);
            ::close(dfd);
        }
    }
}

std::string profile_to_json(const device_profile& p) {
    return device_profile_to_json(p).dump(4);
}

device_profile profile_from_json(const std::string& json_str) {
    // device_profile_from_json already calls sanitize_* on load
    return device_profile_from_json(json::parse(json_str));
}

/// Sanitize a device_profile in-place (useful for values set programmatically).
void sanitize_device_profile(device_profile& dp) {
    sanitize_device_config(dp.dev_cfg);
    sanitize_profile(dp.prof);
}

std::string find_config_path() {
    // D7: when running under sudo, HOME may be /root — prefer SUDO_USER's home directory.
    const char* sudo_user = std::getenv("SUDO_USER");
    if (sudo_user && sudo_user[0] != '\0') {
        struct passwd  pwd_buf;
        struct passwd* result = nullptr;
        std::vector<char> buf(16384);
        int ret = getpwnam_r(sudo_user, &pwd_buf, buf.data(), buf.size(), &result);
        if (ret == 0 && result && result->pw_dir && result->pw_dir[0] != '\0') {
            return std::string(result->pw_dir) + "/.config/rawaccel/settings.json";
        }
    }
    // Normal user or root (e.g. systemd service)
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::string(home) + "/.config/rawaccel/settings.json";
    }
    return DEFAULT_CONFIG_PATH;
}

} // namespace rawaccel
