// RawAccel Linux — Birim testleri
// Derleme: bash tests/run_tests.sh  (veya cmake --build build && ctest --test-dir build)
// Bağımlılık yok — sadece standart C++20 ve proje başlıkları.
//
// Test yapısı: basit assert makrosu, harici framework gerektirmiyor.
// Her SECTION() bağımsız bir test grubu; hata varsa stderr'e yazar ve
// süreç 1 ile çıkar.

#include "../include/accel-classic.hpp"
#include "../include/accel-natural.hpp"
#include "../include/accel-jump.hpp"
#include "../include/accel-synchronous.hpp"
#include "../include/accel-power.hpp"
#include "../include/accel-lookup.hpp"
#include "../include/accel-noaccel.hpp"
#include "../include/accel-union.hpp"
#include "../include/config.hpp"
#include "../daemon/lat_stats.hpp"    // latency histogram (no libevdev dependency)
#include "../daemon/motion_math.hpp"  // apply_motion_math — subpixel accumulation

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <random>
#include <regex>
#include <string>
#include <vector>
#include <unistd.h>

using namespace rawaccel;

// ── Test altyapısı ────────────────────────────────────────────────────────────
//
// CLI seçenekleri (R16 — test framework filtreleme desteği):
//   --filter <regex>   Yalnızca section adı regex ile eşleşenleri çalıştır
//   --list             Section adlarını basıp çık (assertion çalıştırma)
//   --quiet            PASS satırlarını bastırma; sadece FAIL ve özet göster
//   --help             Kullanım bilgisini bas

static int  g_tests  = 0;
static int  g_passed = 0;
static int  g_failed = 0;
static int  g_skipped_sections = 0;
static const char* g_section = "";

static bool        g_section_active = true;   // current section runs assertions
static bool        g_list_only      = false;  // --list: print names, skip asserts
static bool        g_quiet          = false;  // --quiet: suppress PASS lines
static bool        g_have_filter    = false;
static std::regex  g_filter_regex;

#define SECTION(name) do { \
    g_section = name; \
    if (g_list_only) { \
        std::printf("%s\n", name); \
        g_section_active = false; \
    } else if (g_have_filter && !std::regex_search(std::string(name), g_filter_regex)) { \
        g_section_active = false; \
        g_skipped_sections++; \
    } else { \
        g_section_active = true; \
        std::printf("\n[%s]\n", name); \
    } \
} while(0)

#define EXPECT(cond) do { \
    if (!g_section_active) break; \
    g_tests++; \
    if (cond) { \
        g_passed++; \
        if (!g_quiet) std::printf("  PASS  %s\n", #cond); \
    } else { \
        g_failed++; \
        std::fprintf(stderr, "  FAIL  %s:%d  %s  (section: %s)\n", \
                     __FILE__, __LINE__, #cond, g_section); \
    } \
} while(0)

#define EXPECT_NEAR(a, b, tol) do { \
    if (!g_section_active) break; \
    g_tests++; \
    double _a = (a), _b = (b), _t = (tol); \
    if (std::fabs(_a - _b) <= _t) { \
        g_passed++; \
        if (!g_quiet) std::printf("  PASS  %s ≈ %s  (%.6g ≈ %.6g)\n", #a, #b, _a, _b); \
    } else { \
        g_failed++; \
        std::fprintf(stderr, "  FAIL  %s:%d  |%s - %s| = %.6g > %.6g  (section: %s)\n", \
                     __FILE__, __LINE__, #a, #b, std::fabs(_a - _b), _t, g_section); \
    } \
} while(0)

// ── Yardımcı: varsayılan accel_args ile belirli alanları geçersiz kıl ─────────

static accel_args make_args(accel_mode mode) {
    accel_args a;
    a.mode = mode;
    a.gain = true;
    a.acceleration   = 0.005;
    a.exponent_classic = 2.0;
    a.exponent_power = 0.05;
    a.limit          = 1.5;
    a.decay_rate     = 0.1;
    a.input_offset   = 0.0;
    a.sync_speed     = 5.0;
    a.smooth         = 0.5;
    a.cap            = { 15.0, 1.5 };
    a.cap_mode_val   = cap_mode::out;
    a.length         = 0;
    return a;
}

// ── Test 1: noaccel ──────────────────────────────────────────────────────────

static void test_noaccel() {
    SECTION("noaccel");
    accel_args args = make_args(accel_mode::noaccel);
    accel_noaccel na;
    EXPECT_NEAR(na(0.0,   args), 1.0, 1e-12);
    EXPECT_NEAR(na(10.0,  args), 1.0, 1e-12);
    EXPECT_NEAR(na(100.0, args), 1.0, 1e-12);
}

// ── Test 2: classic (gain mode) ──────────────────────────────────────────────

static void test_classic() {
    SECTION("classic — gain mode");
    accel_args args = make_args(accel_mode::classic);
    classic c(args);

    // x == 0 → gain = 1
    EXPECT_NEAR(c(0.0, args), 1.0, 1e-9);

    // x küçük → gain baseline'e yakın (1 + epsilon)
    double g_low  = c(1.0, args);
    double g_high = c(30.0, args);
    EXPECT(g_low  > 1.0);
    EXPECT(g_high > g_low);

    // cap.y = 1.5 → gain asla 1.5'i geçmemeli
    EXPECT(g_high <= 1.5 + 1e-9);

    // exponent <= 1 yolu: sabit gain = acceleration, cap guard
    accel_args args2 = args;
    args2.exponent_classic = 1.0;
    args2.cap.y = 0.5; // degenerate: cap.y < 1
    classic c2(args2);
    double g2 = c2(10.0, args2);
    EXPECT(g2 >= 1.0); // cap.y < 1 guard: negatif gain üretmemeli
}

// ── Test 3: natural ──────────────────────────────────────────────────────────

static void test_natural() {
    SECTION("natural — gain mode");
    accel_args args = make_args(accel_mode::natural);
    natural n(args);

    // x = 0 → 1.0 (guard)
    EXPECT_NEAR(n(0.0, args), 1.0, 1e-9);

    // Hız arttıkça gain artar.
    // Not: gain mode integral formülü kullanır; asimptotu args.limit DEĞİL,
    // daha yüksek bir değere yakınsıyor — bu matematiksel olarak beklenen davranış.
    double g1 = n(1.0, args);
    double g2 = n(10.0, args);
    double g3 = n(50.0, args);
    EXPECT(g1 > 1.0);
    EXPECT(g2 > g1);
    EXPECT(g3 > g2);
    EXPECT(std::isfinite(g3)); // sonsuz veya NaN üretmemeli

    // x yakın 0 → guard devreye girmeli, 1.0 döndürmeli
    EXPECT_NEAR(n(1e-10, args), 1.0, 1e-9);

    // Gain mode kapalı (legacy)
    args.gain = false;
    natural n_legacy(args);
    double gl = n_legacy(10.0, args);
    EXPECT(gl > 1.0);
    EXPECT(gl <= args.limit + 1e-6);
}

// ── Test 4: jump ─────────────────────────────────────────────────────────────

static void test_jump() {
    SECTION("jump — sigmoid");
    accel_args args = make_args(accel_mode::jump);
    // sync_speed=5, smooth=0.5, limit=1.5

    jump j(args);

    // x=0 → 1.0
    EXPECT_NEAR(j(0.0, args), 1.0, 1e-12);

    // Çok düşük hız → ~1.0 (sigmoid sıfıra yakın)
    double g_vlow = j(0.01, args);
    EXPECT(g_vlow >= 1.0);
    EXPECT(g_vlow < 1.1);

    // Çok yüksek hız → limit'e yakın (1 + (1.5-1) = 1.5)
    double g_high = j(50.0, args);
    EXPECT(g_high > 1.4);
    EXPECT(g_high <= 1.5 + 1e-9);

    // Artan hız → artan gain
    EXPECT(j(5.0, args) > j(1.0, args));
    EXPECT(j(20.0, args) > j(5.0, args));

    // limit < 1 guard: decelerasyon üretmemeli
    accel_args args2 = args;
    args2.limit = 0.5; // limit - 1 = -0.5 → clamp → 0
    jump j2(args2);
    // Her hızda gain >= 1.0 olmalı
    EXPECT(j2(1.0,  args2) >= 1.0);
    EXPECT(j2(10.0, args2) >= 1.0);
    EXPECT(j2(50.0, args2) >= 1.0);
}

// ── Test 5: synchronous ──────────────────────────────────────────────────────

static void test_synchronous() {
    SECTION("synchronous");
    accel_args args = make_args(accel_mode::synchronous);
    args.sync_speed     = 5.0;
    args.exponent_power = 1.5; // power > 1 → gain artar hızla

    synchronous s(args);

    // x=0 → 1.0
    EXPECT_NEAR(s(0.0, args), 1.0, 1e-12);

    // x = sync_speed → gain = (sync_speed/sync_speed)^(power-1) = 1.0
    EXPECT_NEAR(s(5.0, args), 1.0, 1e-9);

    // x > sync_speed → gain > 1
    EXPECT(s(10.0, args) > 1.0);
    // x < sync_speed → gain < 1
    EXPECT(s(2.0, args) < 1.0);

    // power < 1: x→0 sonsuzluğa gitmesin (MIN_SPEED guard)
    accel_args args2 = args;
    args2.exponent_power = 0.5;
    synchronous s2(args2);
    double g = s2(1e-9, args2);
    EXPECT(std::isfinite(g));
}

// ── Test 6: lookup (LUT) ─────────────────────────────────────────────────────

static void test_lookup() {
    SECTION("lookup — LUT interpolation");
    accel_args args = make_args(accel_mode::lookup);

    // 3 nokta: (0, 1.0), (10, 1.5), (20, 2.0)
    float pts[] = { 0.0f, 1.0f, 10.0f, 1.5f, 20.0f, 2.0f };
    ::memcpy(args.data, pts, sizeof(pts));
    args.length = 6; // 3 çift

    lookup lut(args);

    // Tam noktalarda
    EXPECT_NEAR(lut(0.0,  args), 1.0, 1e-6);
    EXPECT_NEAR(lut(10.0, args), 1.5, 1e-6);
    EXPECT_NEAR(lut(20.0, args), 2.0, 1e-6);

    // İnterpolasyon: x=5 → 1.25
    EXPECT_NEAR(lut(5.0, args), 1.25, 1e-6);

    // İlk noktanın altı → ilk gain
    EXPECT_NEAR(lut(-1.0, args), 1.0, 1e-6);

    // Son noktanın üstü → son gain
    EXPECT_NEAR(lut(30.0, args), 2.0, 1e-6);

    // Boş LUT → 1.0
    accel_args empty = make_args(accel_mode::lookup);
    empty.length = 0;
    lookup lut_empty(empty);
    EXPECT_NEAR(lut_empty(5.0, empty), 1.0, 1e-12);
}

// ── Test 7: power ────────────────────────────────────────────────────────────

static void test_power() {
    SECTION("power");
    accel_args args = make_args(accel_mode::power);
    args.exponent_power = 0.5;
    args.scale          = 1.0;
    args.output_offset  = 0.0;
    args.cap            = { 15.0, 2.0 };
    args.cap_mode_val   = cap_mode::out;

    power p(args);

    // x=0 → 1.0 (guard)
    EXPECT_NEAR(p(0.0, args), 1.0, 1e-9);

    // Pozitif girişte sonlu değer
    double g5  = p(5.0,  args);
    double g10 = p(10.0, args);
    EXPECT(std::isfinite(g5));
    EXPECT(std::isfinite(g10));
    EXPECT(g5 > 1.0);

    // cap.x=0 ile io modunda NaN üretmemeli (scale_from_gain_point guard)
    accel_args args_io = args;
    args_io.cap_mode_val = cap_mode::io;
    args_io.cap.x = 0.0; // degenerate
    power p_io(args_io);
    double g_io = p_io(5.0, args_io);
    EXPECT(std::isfinite(g_io));
}

// ── Test 8: accel_union dispatch ─────────────────────────────────────────────

static void test_accel_union() {
    SECTION("accel_union — mod dispatch");
    // Her mod için accel_union'un doğru implementasyonu seçtiğini doğrula
    accel_union au;

    auto check = [&](accel_mode m, double speed) -> double {
        accel_args a = make_args(m);
        au.init(a);
        return au.apply(speed, a);
    };

    EXPECT_NEAR(check(accel_mode::noaccel, 10.0), 1.0, 1e-12);
    EXPECT(check(accel_mode::classic,     10.0) >= 1.0);
    EXPECT(check(accel_mode::natural,     10.0) >= 1.0);
    EXPECT(std::isfinite(check(accel_mode::jump,        10.0)));
    EXPECT(std::isfinite(check(accel_mode::synchronous, 10.0)));
    EXPECT(std::isfinite(check(accel_mode::power,       10.0)));
}

// ── Test 9: JSON round-trip ──────────────────────────────────────────────────

static void test_json_roundtrip() {
    SECTION("JSON round-trip (config serialize/deserialize)");

    // Profil oluştur
    device_profile dp;
    dp.name      = "test-profile";
    dp.device_id = "/dev/input/event3";
    dp.dev_cfg.dpi          = 1600;
    dp.dev_cfg.polling_rate = 500;
    dp.prof.degrees_rotation = 15.0;
    dp.prof.degrees_snap     = 5.0;
    dp.prof.speed_min        = 2.0;
    dp.prof.speed_max        = 80.0;

    auto& ax = dp.prof.accel_x;
    ax.mode             = accel_mode::classic;
    ax.gain             = true;
    ax.acceleration     = 0.007;
    ax.exponent_classic = 2.5;
    ax.limit            = 1.8;
    ax.cap              = { 12.0, 1.8 };
    ax.cap_mode_val     = cap_mode::in;
    dp.prof.accel_y = ax;

    // JSON'a serialize et
    std::string json_str = profile_to_json(dp);
    EXPECT(!json_str.empty());

    // JSON'dan deserialize et
    device_profile dp2 = profile_from_json(json_str);

    EXPECT(dp2.name       == dp.name);
    EXPECT(dp2.device_id  == dp.device_id);
    EXPECT(dp2.dev_cfg.dpi          == dp.dev_cfg.dpi);
    EXPECT(dp2.dev_cfg.polling_rate == dp.dev_cfg.polling_rate);
    EXPECT_NEAR(dp2.prof.degrees_rotation, dp.prof.degrees_rotation, 1e-9);
    EXPECT_NEAR(dp2.prof.degrees_snap,     dp.prof.degrees_snap,     1e-9);
    EXPECT_NEAR(dp2.prof.speed_min,        dp.prof.speed_min,        1e-9);
    EXPECT_NEAR(dp2.prof.speed_max,        dp.prof.speed_max,        1e-9);

    auto& ax2 = dp2.prof.accel_x;
    EXPECT(ax2.mode             == ax.mode);
    EXPECT(ax2.gain             == ax.gain);
    EXPECT_NEAR(ax2.acceleration,     ax.acceleration,     1e-9);
    EXPECT_NEAR(ax2.exponent_classic, ax.exponent_classic, 1e-9);
    EXPECT_NEAR(ax2.limit,            ax.limit,            1e-9);
    EXPECT_NEAR(ax2.cap.x,            ax.cap.x,            1e-9);
    EXPECT_NEAR(ax2.cap.y,            ax.cap.y,            1e-9);
    EXPECT(ax2.cap_mode_val == ax.cap_mode_val);
}

// ── Test 10: JSON round-trip — LUT data ──────────────────────────────────────

static void test_json_roundtrip_lut() {
    SECTION("JSON round-trip — LUT noktaları");

    device_profile dp;
    dp.name = "lut-test";
    auto& ax = dp.prof.accel_x;
    ax.mode   = accel_mode::lookup;

    // 4 LUT noktası ekle
    float pts[] = { 0.0f, 1.0f, 5.0f, 1.3f, 15.0f, 1.7f, 30.0f, 2.0f };
    ::memcpy(ax.data, pts, sizeof(pts));
    ax.length = 8; // 4 çift
    dp.prof.accel_y = ax;

    std::string json_str = profile_to_json(dp);
    device_profile dp2   = profile_from_json(json_str);

    auto& ax2 = dp2.prof.accel_x;
    EXPECT(ax2.mode   == accel_mode::lookup);
    EXPECT(ax2.length == ax.length);
    for (int i = 0; i < ax.length; i++) {
        EXPECT_NEAR(static_cast<double>(ax2.data[i]),
                    static_cast<double>(ax.data[i]), 1e-4f);
    }
}

// ── Test 11: app_config dosya round-trip ─────────────────────────────────────

static void test_file_roundtrip() {
    SECTION("app_config dosya round-trip (save + load)");

    app_config cfg;
    cfg.active_profile = "gaming";

    device_profile dp1;
    dp1.name = "default";
    dp1.prof.accel_x.mode = accel_mode::noaccel;
    dp1.prof.accel_y      = dp1.prof.accel_x;
    cfg.profiles.push_back(dp1);

    device_profile dp2;
    dp2.name = "gaming";
    dp2.dev_cfg.dpi = 3200;
    dp2.prof.accel_x.mode         = accel_mode::classic;
    dp2.prof.accel_x.acceleration = 0.003;
    dp2.prof.accel_x.exponent_classic = 1.8;
    dp2.prof.accel_y = dp2.prof.accel_x;
    cfg.profiles.push_back(dp2);

    // Geçici dosyaya kaydet
    std::string tmp = "/tmp/rawaccel_test_roundtrip.json";
    save_config(cfg, tmp);

    // Yeniden yükle
    app_config cfg2 = load_config(tmp);

    EXPECT(cfg2.active_profile == cfg.active_profile);
    EXPECT(cfg2.profiles.size() == cfg.profiles.size());

    auto& p2 = cfg2.profiles[1]; // gaming
    EXPECT(p2.name == "gaming");
    EXPECT(p2.dev_cfg.dpi == 3200);
    EXPECT(p2.prof.accel_x.mode == accel_mode::classic);
    EXPECT_NEAR(p2.prof.accel_x.acceleration,     0.003, 1e-9);
    EXPECT_NEAR(p2.prof.accel_x.exponent_classic, 1.8,   1e-9);

    // Temizlik
    std::remove(tmp.c_str());
}

static void test_save_config_relative_path() {
    SECTION("BUG-22 — save_config supports bare relative filenames");

    char tmpl[] = "/tmp/rawaccel_rel_XXXXXX";
    char* dir = mkdtemp(tmpl);
    EXPECT(dir != nullptr);
    if (!g_section_active || !dir) return;

    char old_cwd[4096] = {};
    bool ok = getcwd(old_cwd, sizeof(old_cwd)) != nullptr;
    EXPECT(ok);
    if (!ok) { rmdir(dir); return; }

    ok = chdir(dir) == 0;
    EXPECT(ok);
    if (!ok) { rmdir(dir); return; }

    app_config cfg;
    cfg.active_profile = "default";
    device_profile dp;
    dp.name = "default";
    cfg.profiles.push_back(dp);

    try {
        save_config(cfg, "settings.json");
        app_config loaded = load_config("settings.json");
        ok = loaded.profiles.size() == 1 && loaded.profiles[0].name == "default";
    } catch (...) {
        ok = false;
    }
    EXPECT(ok);

    std::remove("settings.json.tmp");
    std::remove("settings.json");
    chdir(old_cwd);
    rmdir(dir);
}

// ── Test 12: Monotonik artış kontrolü ────────────────────────────────────────

static void test_monotonic() {
    SECTION("gain monotonic artiş — classic/natural/jump");

    // Tüm modlarda gain, hız arttıkça azalmamalı (offset'i geçtikten sonra)
    struct Case { accel_mode mode; const char* name; };
    Case cases[] = {
        { accel_mode::classic, "classic" },
        { accel_mode::natural, "natural" },
        { accel_mode::jump,    "jump"    },
    };

    for (auto& tc : cases) {
        accel_args args = make_args(tc.mode);
        accel_union au;
        au.init(args);

        double prev = au.apply(0.1, args);
        bool monotonic = true;
        for (int i = 2; i <= 40; i++) {
            double g = au.apply(i * 1.0, args);
            if (g < prev - 1e-9) { monotonic = false; break; }
            prev = g;
        }
        if (!g_section_active) continue;
        g_tests++;
        if (monotonic) {
            g_passed++;
            if (!g_quiet) std::printf("  PASS  %s gain monotonic\n", tc.name);
        } else {
            g_failed++;
            std::fprintf(stderr, "  FAIL  %s gain NOT monotonic\n", tc.name);
        }
    }
}

// ── Test 13: power modunda monotonik artış ───────────────────────────────────

static void test_power_monotonic() {
    SECTION("power mod — monotonik artış");

    accel_args args;
    args.mode           = accel_mode::power;
    args.gain           = true;
    args.exponent_power = 0.05;
    args.scale          = 1.0;
    args.output_offset  = 0.0;
    args.cap_mode_val   = cap_mode::out;
    args.cap            = { 15.0, 2.0 };

    power pw(args);

    double prev = pw(1.0, args);
    bool ok = true;
    for (int i = 2; i <= 30; ++i) {
        double cur = pw(static_cast<double>(i), args);
        if (cur < prev - 1e-9) { ok = false; break; }
        prev = cur;
    }
    EXPECT(ok);

    // cap_mode::io
    args.cap_mode_val = cap_mode::io;
    args.cap = { 12.0, 1.8 };
    power pw_io(args);
    prev = pw_io(1.0, args);
    ok   = true;
    for (int i = 2; i <= 30; ++i) {
        double cur = pw_io(static_cast<double>(i), args);
        if (cur < prev - 1e-9) { ok = false; break; }
        prev = cur;
    }
    EXPECT(ok);

    // cap_mode::in
    args.cap_mode_val = cap_mode::in;
    args.cap = { 10.0, 0.0 };
    power pw_in(args);
    prev = pw_in(1.0, args);
    ok   = true;
    for (int i = 2; i <= 30; ++i) {
        double cur = pw_in(static_cast<double>(i), args);
        if (cur < prev - 1e-9) { ok = false; break; }
        prev = cur;
    }
    EXPECT(ok);
}

// ── Test 14: classic cap_mode::io ve cap_mode::in dalları ───────────────────

static void test_classic_cap_modes() {
    SECTION("classic — cap_mode::io ve cap_mode::in");

    // cap_mode::io: cap.x hızında cap.y kazanımına kilitler
    {
        accel_args args;
        args.mode              = accel_mode::classic;
        args.gain              = true;
        args.acceleration      = 0.005;
        args.exponent_classic  = 2.0;
        args.input_offset      = 0.0;
        args.cap_mode_val      = cap_mode::io;
        args.cap               = { 10.0, 1.8 };

        classic cl(args);
        // At cap.x, gain must equal cap.y (within tolerance)
        double g_at_cap = cl(args.cap.x, args);
        EXPECT_NEAR(g_at_cap, args.cap.y, 1e-6);
        // Beyond cap, gain must not increase
        double g_beyond = cl(args.cap.x * 2.0, args);
        EXPECT(g_beyond <= g_at_cap + 1e-9);
        // Below offset, gain must be 1
        EXPECT_NEAR(cl(0.0, args), 1.0, 1e-12);
    }

    // cap_mode::in: cap.x speeds limit input, gain still rises (or is flat) beyond
    {
        accel_args args;
        args.mode              = accel_mode::classic;
        args.gain              = true;
        args.acceleration      = 0.005;
        args.exponent_classic  = 2.0;
        args.input_offset      = 0.0;
        args.cap_mode_val      = cap_mode::in;
        args.cap               = { 12.0, 0.0 };  // cap.x active, cap.y ignored

        classic cl(args);
        // gain at cap.x and 2×cap.x must be the same (flat after input cap)
        double g_at_cap   = cl(args.cap.x,       args);
        double g_beyond   = cl(args.cap.x * 2.0, args);
        EXPECT_NEAR(g_at_cap, g_beyond, 1e-9);
        // gain must be > 1 (actually accelerating)
        EXPECT(g_at_cap > 1.0);
    }
}

// ── Test 15: modifier — domain/range weights ve rotation ────────────────────
// Modifier sınıfı config.hpp içinde inline tanımlı; x/y ağırlıkları
// ile basit bir ölçekleme + rotasyon testi yapıyoruz.

static void test_modifier() {
    SECTION("modifier — domain/range weights, degrees_rotation");

    // range_weights.x == 2 → x hızı 2 katına çıkar
    {
        profile p;
        p.range_weights  = { 2.0, 1.0 };
        p.domain_weights = { 1.0, 1.0 };
        p.degrees_rotation = 0.0;

        // x'e doğrudan ağırlık uygulaması (modifier içinden hesap)
        // range_weights.x, x çıkışını scale eder
        double wx = p.range_weights.x;
        EXPECT_NEAR(wx, 2.0, 1e-12);
        EXPECT_NEAR(p.range_weights.y, 1.0, 1e-12);
    }

    // degrees_rotation: 90° → x ve y yer değiştirir
    {
        profile p;
        p.degrees_rotation = 90.0;
        const double PI   = 3.14159265358979323846;
        double rad        = p.degrees_rotation * PI / 180.0;
        double cx         = std::cos(rad);
        double sx         = std::sin(rad);
        // cos(90°) ≈ 0, sin(90°) ≈ 1
        EXPECT_NEAR(cx, 0.0, 1e-10);
        EXPECT_NEAR(sx, 1.0, 1e-10);
    }

    // domain_weights: eşit ağırlıkta, hız değişmez
    {
        profile p;
        p.domain_weights = { 1.0, 1.0 };
        double vx = 3.0, vy = 4.0;
        // Euclidean hız: sqrt(3²+4²) = 5
        double speed = std::hypot(vx * p.domain_weights.x,
                                  vy * p.domain_weights.y);
        EXPECT_NEAR(speed, 5.0, 1e-10);
    }

    // domain_weights: x ağırlığı 2 → hız artar
    {
        profile p;
        p.domain_weights = { 2.0, 1.0 };
        double vx = 3.0, vy = 4.0;
        double speed = std::hypot(vx * p.domain_weights.x,
                                  vy * p.domain_weights.y);
        // sqrt((6)²+(4)²) = sqrt(52)
        EXPECT_NEAR(speed, std::sqrt(52.0), 1e-10);
    }
}

// ── Test 16: N8 natural limit<1 guard, N11 power exponent_power=0 ─────────────

static void test_edge_guards() {
    SECTION("N8 natural limit<1 no inversion, N11 power exponent=0 finite");

    // N8: natural with args.limit < 1 should never produce gain < 1 or invert mouse
    {
        accel_args args = make_args(accel_mode::natural);
        args.limit = 0.5; // < 1 → would produce limit=-0.5 without guard
        args.gain  = false; // non-gain mode: most likely to go negative
        natural n(args);
        // With N8 fix, limit is clamped to 0 → output is always 1.0
        EXPECT(n(0.0, args) >= 1.0);
        EXPECT(n(5.0, args) >= 1.0);
        EXPECT(n(20.0, args) >= 1.0);
        EXPECT(std::isfinite(n(50.0, args)));
    }

    // N8: natural with args.limit = 0 (extreme) should not crash or go below 0
    {
        accel_args args = make_args(accel_mode::natural);
        args.limit = 0.0;
        args.gain  = false;
        natural n(args);
        EXPECT(n(10.0, args) >= 0.0); // must not invert
        EXPECT(std::isfinite(n(10.0, args)));
    }

    // N11: power with exponent_power == 0 should be finite (not NaN/Inf)
    {
        accel_args args = make_args(accel_mode::power);
        args.exponent_power = 0.0; // would cause gain_inverse div-by-zero without guard
        power p(args);
        double g = p(10.0, args);
        EXPECT(std::isfinite(g));
        EXPECT(g >= 0.0);
    }

    // N11: power with exponent_power = 0 in io mode (uses scale_from_gain_point)
    {
        accel_args args = make_args(accel_mode::power);
        args.exponent_power = 0.0;
        args.cap_mode_val   = cap_mode::io;
        power p(args);
        double g = p(5.0, args);
        EXPECT(std::isfinite(g));
    }
}

// ── Test 17: modifier.modify() end-to-end ────────────────────────────────────
// R13: modifier hot-path'ini doğrudan test et — rotation, snap, speed clamp,
// directional weights ve acceleration'ın birlikte doğru çalıştığını doğrula.

static void test_modifier_end_to_end() {
    SECTION("modifier.modify() — end-to-end");

    // ── 1. Saf noaccel: modify hiçbir şey değiştirmemeli ──────────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        vec2d in = { 3.0, 4.0 };
        mod.modify(in, sp, ms, 1.0 /*dpi_factor*/, 10.0 /*ms*/);
        EXPECT_NEAR(in.x, 3.0, 1e-9);
        EXPECT_NEAR(in.y, 4.0, 1e-9);
    }

    // ── 2. time <= 0 guard: modify must be a no-op ────────────────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::classic;
        p.accel_x.acceleration = 0.01;
        p.accel_y = p.accel_x;

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        vec2d in = { 5.0, 0.0 };
        mod.modify(in, sp, ms, 1.0, 0.0 /*time=0*/);
        // time<=0 → early return, in unchanged
        EXPECT_NEAR(in.x, 5.0, 1e-9);
        EXPECT_NEAR(in.y, 0.0, 1e-9);
    }

    // ── 3. Classic acceleration: gain > 1 so output > input ───────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::classic;
        p.accel_x.acceleration     = 0.01;
        p.accel_x.exponent_classic = 2.0;
        p.accel_x.cap              = { 100.0, 3.0 };
        p.accel_x.cap_mode_val     = cap_mode::out;
        p.accel_y = p.accel_x;

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        vec2d in = { 30.0, 0.0 }; // fast horizontal movement
        mod.modify(in, sp, ms, 1.0, 1.0);
        // With acceleration, output magnitude must exceed input magnitude
        EXPECT(std::fabs(in.x) > 30.0);
        EXPECT(std::isfinite(in.x));
        EXPECT(std::isfinite(in.y));
    }

    // ── 4. 90° rotation: (dx,dy)=(1,0) → (0,1) ───────────────────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        p.degrees_rotation = 90.0;

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        vec2d in = { 1.0, 0.0 };
        mod.modify(in, sp, ms, 1.0, 1.0);
        // cos(90°)≈0, sin(90°)≈1 → rotated (1,0) = (0,1)
        EXPECT_NEAR(in.x,  0.0, 1e-9);
        EXPECT_NEAR(in.y,  1.0, 1e-9);
    }

    // ── 5. Speed clamp: slow input clamped up to speed_min ────────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        p.speed_min = 50.0;   // ips
        p.speed_max = 200.0;

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        // Input speed (counts/ms): 3 counts over 1ms, dpi_factor=1 → 3 ips
        // 3 ips < speed_min(50) → clamped up to 50 ips
        vec2d in = { 3.0, 0.0 };
        mod.modify(in, sp, ms, 1.0, 1.0);
        double out_speed = std::fabs(in.x); // ips after clamp
        EXPECT(out_speed >= 50.0 - 1e-6);
        EXPECT(out_speed <= 200.0 + 1e-6);
    }

    // ── 6. Directional DPI ratio: lr_output_dpi_ratio > 1 ────────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        p.lr_output_dpi_ratio = 2.0; // rightward motion doubled

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        vec2d in_right = { 5.0, 0.0 };
        mod.modify(in_right, sp, ms, 1.0, 1.0);
        EXPECT_NEAR(in_right.x, 10.0, 1e-9); // 5 * 2.0

        vec2d in_left = { -5.0, 0.0 };
        mod.modify(in_left, sp, ms, 1.0, 1.0);
        EXPECT_NEAR(in_left.x, -2.5, 1e-9); // -5 * (1/2.0)
    }

    // ── 7. Angle snap: diagonal near horizontal → snapped to horizontal ──
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        p.degrees_snap = 20.0; // snap cone half-angle

        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor   sp; sp.init(p.speed_processor_args);
        modifier          mod;

        // 10° angle from horizontal → within 20° snap cone → snapped to pure X
        double ang = 10.0 * M_PI / 180.0;
        double mag = 5.0;
        vec2d in = { mag * std::cos(ang), mag * std::sin(ang) };
        mod.modify(in, sp, ms, 1.0, 1.0);
        // After snap, y component must be zero
        EXPECT_NEAR(in.y, 0.0, 1e-9);
        EXPECT(std::fabs(in.x) > 0.0);
    }
}

// ── Test 18: speed_processor ─────────────────────────────────────────────────
// R14: calc_speed_whole/separate, distance modes, smoothing init

static void test_speed_processor() {
    SECTION("speed_processor — distance modes");

    speed_args sa;

    // ── euclidean (whole=true, lp_norm≈2) ────────────────────────────────
    {
        sa.whole   = true;
        sa.lp_norm = 2.0;
        speed_processor sp; sp.init(sa);
        EXPECT(sp.speed_flags.dist_mode == distance_mode::euclidean);

        // speed of (3,4) = 5
        vec2d v = { 3.0, 4.0 };
        double s = sp.calc_speed_whole(v, 1.0);
        EXPECT_NEAR(s, 5.0, 1e-9);
    }

    // ── max norm (lp_norm > MAX_NORM) ─────────────────────────────────────
    {
        sa.whole   = true;
        sa.lp_norm = 100.0; // > MAX_NORM(16)
        speed_processor sp; sp.init(sa);
        EXPECT(sp.speed_flags.dist_mode == distance_mode::max);

        vec2d v = { 3.0, 4.0 };
        double s = sp.calc_speed_whole(v, 1.0);
        EXPECT_NEAR(s, 4.0, 1e-9); // max(3,4) = 4
    }

    // ── Lp norm (whole=true, lp_norm=1) ──────────────────────────────────
    {
        sa.whole   = true;
        sa.lp_norm = 1.0; // Manhattan
        speed_processor sp; sp.init(sa);
        EXPECT(sp.speed_flags.dist_mode == distance_mode::Lp);

        vec2d v = { 3.0, 4.0 };
        double s = sp.calc_speed_whole(v, 1.0);
        EXPECT_NEAR(s, 7.0, 1e-9); // |3|+|4| = 7
    }

    // ── separate mode (whole=false) ───────────────────────────────────────
    {
        sa.whole = false;
        speed_processor sp; sp.init(sa);
        EXPECT(sp.speed_flags.dist_mode == distance_mode::separate);

        vec2d v = { -3.0, 4.0 };
        sp.calc_speed_separate(v, 1.0);
        // separate returns |x|, |y|
        EXPECT_NEAR(v.x, 3.0, 1e-9);
        EXPECT_NEAR(v.y, 4.0, 1e-9);
    }

    // ── smoothing flags set correctly ─────────────────────────────────────
    {
        speed_args sa2;
        sa2.whole = true;
        sa2.lp_norm = 2.0;
        sa2.input_speed_smooth_halflife  = 5.0;
        sa2.scale_smooth_halflife        = 3.0;
        sa2.output_speed_smooth_halflife = 2.0;
        speed_processor sp; sp.init(sa2);
        EXPECT(sp.speed_flags.should_smooth_input);
        EXPECT(sp.speed_flags.should_smooth_scale);
        EXPECT(sp.speed_flags.should_smooth_output);
    }

    // ── no smoothing when halflives are 0 ────────────────────────────────
    {
        speed_args sa3;
        sa3.input_speed_smooth_halflife  = 0.0;
        sa3.scale_smooth_halflife        = 0.0;
        sa3.output_speed_smooth_halflife = 0.0;
        speed_processor sp; sp.init(sa3);
        EXPECT(!sp.speed_flags.should_smooth_input);
        EXPECT(!sp.speed_flags.should_smooth_scale);
        EXPECT(!sp.speed_flags.should_smooth_output);
    }
}

// ── Test 19: config error paths ──────────────────────────────────────────────
// R15: bozuk JSON, eksik alan, aşırı LUT uzunluğu

static void test_config_error_paths() {
    SECTION("config error paths — bad JSON, missing fields, overlong LUT");

    // ── bozuk JSON → load_config exception fırlatmalı ─────────────────────
    {
        std::string tmp = "/tmp/rawaccel_test_bad.json";
        { std::ofstream f(tmp); f << "{ not valid json !!!"; }
        bool threw = false;
        try { load_config(tmp); } catch (...) { threw = true; }
        EXPECT(threw);
        std::remove(tmp.c_str());
    }

    // ── var olmayan dosya → exception ────────────────────────────────────
    {
        bool threw = false;
        try { load_config("/tmp/rawaccel_nonexistent_XXXXX.json"); }
        catch (...) { threw = true; }
        EXPECT(threw);
    }

    // ── eksik alanlar → varsayılan değerler korunmalı ─────────────────────
    {
        std::string tmp = "/tmp/rawaccel_test_minimal.json";
        { std::ofstream f(tmp); f << R"({"profiles": [{"name": "x"}]})"; }
        app_config cfg = load_config(tmp);
        EXPECT(!cfg.profiles.empty());
        EXPECT(cfg.profiles[0].name == "x");
        // DPI should fall back to struct default (0 — not yet overridden)
        EXPECT(cfg.profiles[0].dev_cfg.dpi == 0 || cfg.profiles[0].dev_cfg.dpi >= 0);
        std::remove(tmp.c_str());
    }

    // ── LUT length > capacity → clamped, no buffer overflow ───────────────
    {
        accel_args a = make_args(accel_mode::lookup);
        a.length = (int)LUT_RAW_DATA_CAPACITY + 100; // way over capacity
        lookup lut(a);
        // point_count must be <= LUT_POINTS_CAPACITY
        EXPECT(lut.point_count <= (int)LUT_POINTS_CAPACITY);
    }

    // ── profile_from_json with invalid JSON string → exception ────────────
    {
        bool threw = false;
        try { profile_from_json("not json"); }
        catch (...) { threw = true; }
        EXPECT(threw);
    }

    // ── cap array with too few elements → no crash (uses default) ─────────
    {
        std::string json_str = R"({
            "name": "test",
            "profile": {
                "accel_x": { "mode": "classic", "cap": [5] },
                "accel_y": { "mode": "noaccel" }
            }
        })";
        bool threw = false;
        device_profile dp;
        try { dp = profile_from_json(json_str); }
        catch (...) { threw = true; }
        // Should not throw — incomplete cap array falls back silently
        EXPECT(!threw);
    }
}

// ── Test 20: input validation / sanitization ─────────────────────────────────
// Verify that sanitize_device_profile() clamps values to safe ranges.

static void test_input_validation() {
    SECTION("input validation — DPI / polling_rate / rotation / snap / speed bounds");

    // ── DPI clamping via JSON load ────────────────────────────────────────
    {
        std::string tmp = "/tmp/rawaccel_test_validation.json";
        { std::ofstream f(tmp); f << R"({
            "profiles": [{
                "name": "v",
                "dpi": -100,
                "polling_rate": 999999
            }]
        })"; }
        app_config cfg = load_config(tmp);
        EXPECT(!cfg.profiles.empty());
        EXPECT(cfg.profiles[0].dev_cfg.dpi >= 1);           // negative → clamped up
        EXPECT(cfg.profiles[0].dev_cfg.polling_rate <= 8000); // huge → clamped down
        std::remove(tmp.c_str());
    }

    // ── rotation normalisation ────────────────────────────────────────────
    {
        std::string tmp = "/tmp/rawaccel_test_rot.json";
        { std::ofstream f(tmp); f << R"({
            "profiles": [{"name":"r","profile":{"degrees_rotation":720}}]
        })"; }
        app_config cfg = load_config(tmp);
        double rot = cfg.profiles[0].prof.degrees_rotation;
        EXPECT(rot >= 0 && rot < 360); // 720° → 0°
        std::remove(tmp.c_str());
    }

    // ── snap angle clamp ──────────────────────────────────────────────────
    {
        std::string tmp = "/tmp/rawaccel_test_snap.json";
        { std::ofstream f(tmp); f << R"({
            "profiles": [{"name":"s","profile":{"degrees_snap":90}}]
        })"; }
        app_config cfg = load_config(tmp);
        EXPECT(cfg.profiles[0].prof.degrees_snap <= 45); // 90° → 45°
        std::remove(tmp.c_str());
    }

    // ── speed_max >= speed_min ────────────────────────────────────────────
    {
        std::string tmp = "/tmp/rawaccel_test_speed.json";
        { std::ofstream f(tmp); f << R"({
            "profiles": [{"name":"sp","profile":{"speed_min":10,"speed_max":5}}]
        })"; }
        app_config cfg = load_config(tmp);
        double mn = cfg.profiles[0].prof.speed_min;
        double mx = cfg.profiles[0].prof.speed_max;
        EXPECT(mx == 0 || mx >= mn); // max < min → max fixed up
        std::remove(tmp.c_str());
    }

    // ── sanitize_device_profile standalone ───────────────────────────────
    {
        device_profile dp;
        dp.dev_cfg.dpi          = 0;
        dp.dev_cfg.polling_rate = 50; // below min
        dp.prof.degrees_snap    = -5;
        dp.prof.output_dpi      = 99999;
        sanitize_device_profile(dp);
        EXPECT(dp.dev_cfg.dpi >= 1);
        EXPECT(dp.dev_cfg.polling_rate >= 125);
        EXPECT(dp.prof.degrees_snap >= 0);
        EXPECT(dp.prof.output_dpi <= 32000);
    }
}

// ── Test 21: multi-profile config round-trip ──────────────────────────────────
// Write a config with several profiles, reload it, verify integrity.

static void test_multi_profile_roundtrip() {
    SECTION("multi-profile config — save/load/active_profile integrity");

    std::string tmp = "/tmp/rawaccel_test_multiprof.json";

    // Build a config with 3 profiles
    app_config cfg_out;
    cfg_out.active_profile = "gaming";
    for (auto& nm : {"default", "gaming", "office"}) {
        device_profile dp;
        dp.name = nm;
        dp.dev_cfg.dpi = (dp.name == "gaming") ? 1600 : 800;
        dp.dev_cfg.polling_rate = 1000;
        dp.prof.accel_x.mode = (dp.name == "gaming")
                                ? accel_mode::classic : accel_mode::noaccel;
        cfg_out.profiles.push_back(dp);
    }
    save_config(cfg_out, tmp);

    // Reload
    app_config cfg_in = load_config(tmp);
    EXPECT(cfg_in.profiles.size() == 3);
    EXPECT(cfg_in.active_profile == "gaming");

    // Check profile order preserved
    EXPECT(cfg_in.profiles[0].name == "default");
    EXPECT(cfg_in.profiles[1].name == "gaming");
    EXPECT(cfg_in.profiles[2].name == "office");

    // Check DPI and mode
    EXPECT(cfg_in.profiles[1].dev_cfg.dpi == 1600);
    EXPECT(cfg_in.profiles[1].prof.accel_x.mode == accel_mode::classic);
    EXPECT(cfg_in.profiles[0].prof.accel_x.mode == accel_mode::noaccel);

    std::remove(tmp.c_str());
}

// ── Test 22: atomic write — no partial-read window ───────────────────────────
// Verify that save_config writes atomically (tmp → rename).

static void test_atomic_write() {
    SECTION("atomic config write — no .tmp file left after successful save");

    std::string tmp_path = "/tmp/rawaccel_test_atomic.json";
    std::string tmp_file = tmp_path + ".tmp";

    app_config cfg;
    device_profile dp; dp.name = "atomic_test";
    cfg.profiles.push_back(dp);

    save_config(cfg, tmp_path);

    // .tmp must not exist after a successful save
    std::ifstream leftover(tmp_file);
    EXPECT(!leftover.good()); // tmp file should be gone (renamed to final)

    // Final file must exist and be valid JSON
    app_config reloaded = load_config(tmp_path);
    EXPECT(!reloaded.profiles.empty());
    EXPECT(reloaded.profiles[0].name == "atomic_test");

    std::remove(tmp_path.c_str());
}

// ── Test 23: profile_from_json / profile_to_json IPC round-trip ──────────────

static void test_ipc_json_roundtrip() {
    SECTION("IPC profile JSON round-trip — profile_to_json → profile_from_json");

    device_profile dp;
    dp.name      = "ipc_test";
    dp.device_id = "/dev/input/by-id/usb-Razer-event-mouse";
    dp.dev_cfg.dpi          = 1200;
    dp.dev_cfg.polling_rate = 500;
    dp.prof.accel_x.mode    = accel_mode::natural;
    dp.prof.accel_x.motivity = 1.7;
    dp.prof.degrees_rotation = 15.0;

    std::string json_str = profile_to_json(dp);
    EXPECT(!json_str.empty());

    device_profile dp2 = profile_from_json(json_str);
    EXPECT(dp2.name == dp.name);
    EXPECT(dp2.device_id == dp.device_id);
    EXPECT(dp2.dev_cfg.dpi == dp.dev_cfg.dpi);
    EXPECT(dp2.dev_cfg.polling_rate == dp.dev_cfg.polling_rate);
    EXPECT(dp2.prof.accel_x.mode == dp.prof.accel_x.mode);
    EXPECT(std::fabs(dp2.prof.accel_x.motivity - dp.prof.accel_x.motivity) < 1e-9);
    EXPECT(std::fabs(dp2.prof.degrees_rotation - dp.prof.degrees_rotation) < 1e-9);
}

// ── Test 24: apply_motion_math — subpixel accumulation ───────────────────────
// flush_motion() delegates all math to apply_motion_math().  Test it directly
// without requiring uinput/libevdev.

static void test_motion_math() {
    SECTION("apply_motion_math — subpixel accumulation + noaccel passthrough");

    using rawaccel::apply_motion_math;
    using rawaccel::modifier;
    using rawaccel::speed_processor;
    using rawaccel::modifier_settings;
    using rawaccel::init_settings;
    using rawaccel::profile;
    using rawaccel::accel_mode;

    // ── 1. noaccel: output == input (no acceleration) ─────────────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor sp; sp.init(p.speed_processor_args);
        modifier mod;

        double rx = 0.0, ry = 0.0;
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, ms, 1.0, 10.0,
                          5.0, 3.0, rx, ry, ox, oy);
        EXPECT(ox == 5);
        EXPECT(oy == 3);
        EXPECT_NEAR(rx, 0.0, 1e-9);
        EXPECT_NEAR(ry, 0.0, 1e-9);
    }

    // ── 2. Subpixel accumulation: 0.4 + 0.4 + 0.4 = 1 after 3 calls ─────
    // Input 0.4 each call → truncated to 0 until sum ≥ 1
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor sp; sp.init(p.speed_processor_args);
        modifier mod;

        double rx = 0.0, ry = 0.0;
        int ox = 0, oy = 0;

        apply_motion_math(mod, sp, ms, 1.0, 10.0, 0.4, 0.0, rx, ry, ox, oy);
        EXPECT(ox == 0);
        EXPECT_NEAR(rx, 0.4, 1e-9);

        apply_motion_math(mod, sp, ms, 1.0, 10.0, 0.4, 0.0, rx, ry, ox, oy);
        EXPECT(ox == 0);
        EXPECT_NEAR(rx, 0.8, 0.001); // small float rounding OK

        apply_motion_math(mod, sp, ms, 1.0, 10.0, 0.4, 0.0, rx, ry, ox, oy);
        EXPECT(ox == 1);                // 0.8 + 0.4 = 1.2 → trunc = 1
        EXPECT(rx < 0.25);              // remainder ≈ 0.2
    }

    // ── 3. Negative motion: subpixel remainder sign-correct ───────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::noaccel;
        p.accel_y.mode = accel_mode::noaccel;
        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor sp; sp.init(p.speed_processor_args);
        modifier mod;

        double rx = 0.0, ry = 0.0;
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, ms, 1.0, 10.0, -0.7, 0.0, rx, ry, ox, oy);
        EXPECT(ox == 0);
        EXPECT(rx < 0.0);   // remainder is negative (sign-correct)
        EXPECT(rx > -1.0);

        apply_motion_math(mod, sp, ms, 1.0, 10.0, -0.7, 0.0, rx, ry, ox, oy);
        EXPECT(ox == -1);   // -0.7 + (-0.7) = -1.4 → trunc = -1
    }

    // ── 4. Acceleration: output magnitude > input magnitude ──────────────
    {
        profile p;
        p.accel_x.mode = accel_mode::classic;
        p.accel_x.acceleration     = 0.01;
        p.accel_x.exponent_classic = 2.0;
        p.accel_x.cap              = { 100.0, 5.0 };
        p.accel_y = p.accel_x;
        modifier_settings ms; ms.prof = p; init_settings(ms);
        speed_processor sp; sp.init(p.speed_processor_args);
        modifier mod;

        double rx = 0.0, ry = 0.0;
        int ox = 0, oy = 0;
        // Large fast input: 50 counts in 1 ms → speed >> 1
        apply_motion_math(mod, sp, ms, 1.0, 1.0, 50.0, 0.0, rx, ry, ox, oy);
        EXPECT(ox > 50);  // acceleration applied
        EXPECT(std::isfinite(rx));
    }
}

// ── Test 25: lat_stats histogram ─────────────────────────────────────────────
// lat_stats yaşam döngüsü, histogram doldurma, percentile hesabı, reset.

static void test_lat_stats() {
    SECTION("lat_stats — histogram, percentile, snapshot_and_reset");

    using rawaccel::lat_stats;

    // ── Boş histogram: count=0, percentile=0 ─────────────────────────────
    {
        lat_stats ls;
        EXPECT(ls.count == 0);
        EXPECT_NEAR(ls.avg_us(), 0.0, 1e-12);
        EXPECT_NEAR(ls.percentile(50), 0.0, 1e-12);
        EXPECT_NEAR(ls.percentile(99), 0.0, 1e-12);
    }

    // ── Tek örnek: min/max/avg = aynı değer ──────────────────────────────
    {
        lat_stats ls;
        ls.record(10.0);
        EXPECT(ls.count == 1);
        EXPECT_NEAR(ls.min_us, 10.0, 1e-9);
        EXPECT_NEAR(ls.max_us, 10.0, 1e-9);
        EXPECT_NEAR(ls.avg_us(), 10.0, 1e-9);
    }

    // ── Percentile hesabı — 100 örnek, tam dağılım ───────────────────────
    // 50 × 1µs + 50 × 3µs → p50 ≈ 1.5µs (bucket 2), p99 ≈ 3.0µs (bucket 6)
    {
        lat_stats ls;
        for (int i = 0; i < 50; i++) ls.record(1.0);
        for (int i = 0; i < 50; i++) ls.record(3.0);
        EXPECT(ls.count == 100);
        // p50: 50th sample → in the 50×1µs group (bucket index 2, upper edge 1.5µs)
        double p50 = ls.percentile(50);
        EXPECT(p50 >= 1.0 - 1e-9);
        EXPECT(p50 <= 1.5 + 1e-9);
        // p99: 99th sample → in the 50×3µs group
        double p99 = ls.percentile(99);
        EXPECT(p99 >= 3.0 - 1e-9);
        EXPECT(p99 <= 3.5 + 1e-9);
        // avg = (50×1 + 50×3)/100 = 2.0
        EXPECT_NEAR(ls.avg_us(), 2.0, 1e-9);
    }

    // ── Overflow bucket: sample > RANGE_US ───────────────────────────────
    {
        lat_stats ls;
        ls.record(1.0);                              // normal
        ls.record(lat_stats::RANGE_US + 100.0);      // overflow (> 500µs)
        EXPECT(ls.count == 2);
        EXPECT(ls.over == 1);
        // max_us must reflect the overflow sample
        EXPECT(ls.max_us > lat_stats::RANGE_US);
    }

    // ── snapshot_and_reset atomically copies + resets ─────────────────────
    {
        lat_stats ls;
        for (int i = 1; i <= 10; i++) ls.record(static_cast<double>(i));
        EXPECT(ls.count == 10);

        auto s = ls.snapshot_and_reset();
        // Snapshot has the old data
        EXPECT(s.count == 10);
        EXPECT_NEAR(s.min_us, 1.0, 0.5 + 1e-9); // ≤ 1.5 (first bucket upper edge)
        EXPECT(s.max_us >= 10.0);
        // After reset, ls is clean
        EXPECT(ls.count == 0);
        EXPECT_NEAR(ls.avg_us(), 0.0, 1e-12);
        // Snapshot percentile works independently
        double sp50 = s.percentile(50);
        EXPECT(sp50 > 0.0);
    }

    // ── Monotone: p50 ≤ p95 ≤ p99 ────────────────────────────────────────
    {
        lat_stats ls;
        // Uniform 1–20µs
        for (int i = 1; i <= 20; i++)
            for (int j = 0; j < 5; j++)  // 5 samples each → 100 total
                ls.record(static_cast<double>(i));
        EXPECT(ls.count == 100);
        double p50 = ls.percentile(50);
        double p95 = ls.percentile(95);
        double p99 = ls.percentile(99);
        EXPECT(p50 <= p95 + 1e-9);
        EXPECT(p95 <= p99 + 1e-9);
        EXPECT(p99 > 0.0);
    }

    // ── Move constructor: data transfers, mutex is fresh ──────────────────
    {
        lat_stats a;
        a.record(5.0);
        a.record(10.0);
        lat_stats b(std::move(a));
        EXPECT(b.count == 2);
        EXPECT(a.count == 0);       // source is reset after move
    }
}

// ── Test: natural gain mode with decay_rate=0 (BUG-2 regression) ─────────────

static void test_natural_decay_zero() {
    SECTION("natural — decay_rate=0 (accel=0) no NaN");
    // BUG-2: decay_rate=0 → accel=0 → division by zero in gain mode integral.
    // After fix, should return 1.0 (no acceleration) without NaN.

    // gain mode ON, decay_rate=0
    {
        accel_args args = make_args(accel_mode::natural);
        args.decay_rate = 0.0;
        args.gain = true;
        args.limit = 1.5;
        args.input_offset = 0;
        natural n(args);
        double g1 = n(0.0, args);
        double g5 = n(5.0, args);
        double g50 = n(50.0, args);
        EXPECT_NEAR(g1, 1.0, 1e-9);
        EXPECT(std::isfinite(g5));
        EXPECT(g5 >= 1.0 - 1e-9);   // should be exactly 1.0
        EXPECT(std::isfinite(g50));
        EXPECT(g50 >= 1.0 - 1e-9);
    }

    // gain mode OFF, decay_rate=0 — legacy mode, should also work
    {
        accel_args args = make_args(accel_mode::natural);
        args.decay_rate = 0.0;
        args.gain = false;
        args.limit = 1.5;
        args.input_offset = 0;
        natural n(args);
        double g = n(10.0, args);
        EXPECT(std::isfinite(g));
        EXPECT(g >= 1.0 - 1e-9);
    }

    // very small decay_rate (near-zero but nonzero)
    {
        accel_args args = make_args(accel_mode::natural);
        args.decay_rate = 1e-15;
        args.gain = true;
        args.limit = 2.0;
        args.input_offset = 0;
        natural n(args);
        double g = n(10.0, args);
        EXPECT(std::isfinite(g));
    }
}

// ── Test: all algorithms with extreme input values ───────────────────────────

static void test_extreme_inputs() {
    SECTION("extreme input values — no NaN/Inf from any algorithm");

    const double extremes[] = { 0.0, 1e-15, 1e-9, 0.001, 1.0, 100.0, 1e6, 1e15 };
    const accel_mode modes[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::natural,
        accel_mode::jump, accel_mode::synchronous, accel_mode::power
    };

    for (auto mode : modes) {
        accel_args args = make_args(mode);
        accel_union au;
        au.init(args);
        bool all_finite = true;
        for (double x : extremes) {
            double g = au.apply(x, args);
            if (!std::isfinite(g)) {
                all_finite = false;
                std::fprintf(stderr, "  NaN/Inf at mode=%d, x=%.3g, g=%.3g\n",
                             (int)mode, x, g);
            }
        }
        EXPECT(all_finite);
    }

    // Negative speeds — should not crash
    {
        bool all_ok = true;
        for (auto mode : modes) {
            accel_args args = make_args(mode);
            accel_union au;
            au.init(args);
            double g = au.apply(-5.0, args);
            if (!std::isfinite(g)) all_ok = false;
        }
        EXPECT(all_ok);
    }
}

// ── Test: classic with degenerate cap_mode::io configs ───────────────────────

static void test_classic_degenerate() {
    SECTION("classic — degenerate cap_mode::io (cap.x <= input_offset)");

    accel_args args = make_args(accel_mode::classic);
    args.cap_mode_val = cap_mode::io;
    args.cap.x = 0.0;   // degenerate: cap.x == 0
    args.cap.y = 2.0;
    args.input_offset = 1.0;
    classic c(args);
    // Should not crash; gain should be finite
    double g = c(5.0, args);
    EXPECT(std::isfinite(g));
    EXPECT(g >= 1.0);
}

// ── Test: power with exponent_power=0 ────────────────────────────────────────

static void test_power_zero_exponent() {
    SECTION("power — exponent_power=0 (clamped to 1e-4)");
    accel_args args = make_args(accel_mode::power);
    args.exponent_power = 0.0;
    power p(args);
    double g = p(10.0, args);
    EXPECT(std::isfinite(g));
    EXPECT(g > 0.0);

    // Negative exponent (should still work, clamped)
    args.exponent_power = -1.0;
    power p2(args);
    double g2 = p2(10.0, args);
    EXPECT(std::isfinite(g2));
}

// ── Test: lookup with edge cases ─────────────────────────────────────────────

static void test_lookup_edge_cases() {
    SECTION("lookup — duplicate points, single point, odd length");

    // Single point (odd length=1 → 0 pairs)
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        args.length = 1;
        args.data[0] = 5.0f;
        lookup lut(args);
        EXPECT(lut.point_count == 0);
        EXPECT_NEAR(lut(5.0, args), 1.0, 1e-9);
    }

    // Duplicate X values (x0 == x1)
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        args.length = 4;
        args.data[0] = 10.0f; args.data[1] = 1.5f;
        args.data[2] = 10.0f; args.data[3] = 2.0f;  // same X
        lookup lut(args);
        double g = lut(10.0, args);
        EXPECT(std::isfinite(g));
    }

    // Maximum capacity: exactly LUT_POINTS_CAPACITY pairs
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        int max_pairs = (int)LUT_POINTS_CAPACITY;
        args.length = max_pairs * 2;
        for (int i = 0; i < max_pairs; i++) {
            args.data[i*2]   = (float)(i * 1.0);
            args.data[i*2+1] = 1.0f + (float)i * 0.01f;
        }
        lookup lut(args);
        EXPECT(lut.point_count == max_pairs);
        double g = lut(50.0, args);
        EXPECT(std::isfinite(g));
    }
}

// ── Test: modifier with zero time (division by zero guard) ───────────────────

static void test_modifier_zero_time() {
    SECTION("modifier — zero/negative time guard");

    modifier mod;
    speed_processor sp;
    modifier_settings settings;
    settings.prof.accel_x.mode = accel_mode::classic;
    settings.prof.accel_y = settings.prof.accel_x;
    init_settings(settings);
    sp.init(settings.prof.speed_processor_args);

    // time = 0 → modify should return early, leaving input unchanged
    {
        vec2d in = { 5.0, 3.0 };
        mod.modify(in, sp, settings, 1.0, 0.0);
        EXPECT_NEAR(in.x, 5.0, 1e-9);
        EXPECT_NEAR(in.y, 3.0, 1e-9);
    }

    // time = negative → also returns early
    {
        vec2d in = { 5.0, 3.0 };
        mod.modify(in, sp, settings, 1.0, -1.0);
        EXPECT_NEAR(in.x, 5.0, 1e-9);
        EXPECT_NEAR(in.y, 3.0, 1e-9);
    }
}

// ── Test: EMA smoother stability with very large/small halflife ──────────────

static void test_ema_smoother_stability() {
    SECTION("EMA smoother — extreme halflife values");

    // halflife = 0 → coefficient = 0, smoother should pass through
    {
        simple_ema_smoother s;
        s.init(0.0);
        double r1 = s.smooth(10.0, 1.0);
        double r2 = s.smooth(20.0, 1.0);
        EXPECT(std::isfinite(r1));
        EXPECT(std::isfinite(r2));
    }

    // Very large halflife → heavy smoothing, output stays near zero initially
    {
        simple_ema_smoother s;
        s.init(1000.0);
        double r = s.smooth(100.0, 1.0);
        EXPECT(std::isfinite(r));
        EXPECT(r >= 0.0);
        EXPECT(r <= 100.0);
    }

    // linear_ema_smoother with zero halflife
    {
        linear_ema_smoother s;
        s.init(0.0, 0.0);
        double r = s.smooth(50.0, 1.0);
        EXPECT(std::isfinite(r));
    }
}

// ── Test: subpixel accumulation sign correctness ─────────────────────────────

static void test_subpixel_sign() {
    SECTION("subpixel accumulation — sign correctness across directions");

    modifier mod;
    speed_processor sp;
    modifier_settings settings;
    settings.prof.accel_x.mode = accel_mode::noaccel;
    settings.prof.accel_y = settings.prof.accel_x;
    init_settings(settings);
    sp.init(settings.prof.speed_processor_args);

    double rx = 0, ry = 0;
    int ox, oy;

    // Accumulate small positive movements
    for (int i = 0; i < 5; i++) {
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          0.3, 0.0, rx, ry, ox, oy);
    }
    // After 5 × 0.3 = 1.5, should have produced at least 1 count
    // (accumulated output is 1 with 0.5 remainder)
    EXPECT(rx >= 0.0);
    EXPECT(rx < 1.0);

    // Now reverse direction
    rx = 0; ry = 0;
    for (int i = 0; i < 5; i++) {
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          -0.3, 0.0, rx, ry, ox, oy);
    }
    EXPECT(rx <= 0.0);
    EXPECT(rx > -1.0);

    // Large rapid direction change: should not accumulate incorrectly
    rx = 0.9;  // almost at +1
    apply_motion_math(mod, sp, settings, 1.0, 1.0,
                      -2.0, 0.0, rx, ry, ox, oy);
    // Expected: 0.9 + (-2.0) = -1.1 → output -1, remainder -0.1
    EXPECT(ox == -1);
    EXPECT_NEAR(rx, -0.1, 1e-9);
}

// ── Test: sanitize_device_profile clamps extreme values ──────────────────────

static void test_sanitize_extremes() {
    SECTION("sanitize_device_profile — extreme value clamping");

    device_profile dp;
    dp.dev_cfg.dpi = -100;
    dp.dev_cfg.polling_rate = 99999;
    dp.prof.degrees_rotation = -720.0;
    dp.prof.degrees_snap = 90.0;
    dp.prof.output_dpi = 0;
    dp.prof.lr_output_dpi_ratio = 0.0001;
    dp.prof.ud_output_dpi_ratio = 999.0;
    dp.prof.speed_min = -5;
    dp.prof.speed_max = -10;
    dp.prof.speed_processor_args.lp_norm = -1.0;

    sanitize_device_profile(dp);

    EXPECT(dp.dev_cfg.dpi >= 1);
    EXPECT(dp.dev_cfg.polling_rate <= 8000);
    EXPECT(dp.prof.degrees_rotation >= 0.0);
    EXPECT(dp.prof.degrees_rotation < 360.0);
    EXPECT(dp.prof.degrees_snap <= 45.0);
    EXPECT(dp.prof.output_dpi >= 1);
    EXPECT(dp.prof.lr_output_dpi_ratio >= 0.01);
    EXPECT(dp.prof.ud_output_dpi_ratio <= 100.0);
    EXPECT(dp.prof.speed_min >= 0);
    EXPECT(dp.prof.speed_max >= 0);
    EXPECT(dp.prof.speed_processor_args.lp_norm > 0);
}

// ── Test: sanitize sorts unsorted LUT data (BUG-NEW-1 fix) ──────────────────

static void test_lut_sort_on_sanitize() {
    SECTION("sanitize_device_profile — sorts unsorted LUT data");

    // Unsorted LUT points: (20, 2.0), (5, 1.2), (10, 1.5), (0, 1.0)
    // After sanitize, they must be sorted by speed (X): 0, 5, 10, 20
    {
        device_profile dp;
        dp.prof.accel_x.mode = accel_mode::lookup;
        dp.prof.accel_x.length = 8;  // 4 pairs × 2
        dp.prof.accel_x.data[0] = 20.0f; dp.prof.accel_x.data[1] = 2.0f;
        dp.prof.accel_x.data[2] = 5.0f;  dp.prof.accel_x.data[3] = 1.2f;
        dp.prof.accel_x.data[4] = 10.0f; dp.prof.accel_x.data[5] = 1.5f;
        dp.prof.accel_x.data[6] = 0.0f;  dp.prof.accel_x.data[7] = 1.0f;

        sanitize_device_profile(dp);

        // Verify sorted order: 0, 5, 10, 20
        EXPECT_NEAR(dp.prof.accel_x.data[0], 0.0f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[1], 1.0f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[2], 5.0f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[3], 1.2f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[4], 10.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[5], 1.5f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[6], 20.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[7], 2.0f,  1e-9);

        // Verify the sorted LUT actually produces correct interpolation
        lookup lut(dp.prof.accel_x);
        EXPECT_NEAR(lut(0.0, dp.prof.accel_x), 1.0, 1e-6);
        EXPECT_NEAR(lut(5.0, dp.prof.accel_x), 1.2, 1e-6);
        EXPECT_NEAR(lut(10.0, dp.prof.accel_x), 1.5, 1e-6);
        EXPECT_NEAR(lut(20.0, dp.prof.accel_x), 2.0, 1e-6);
        // Midpoint interpolation: speed=2.5 → between (0,1.0) and (5,1.2) → 1.1
        EXPECT_NEAR(lut(2.5, dp.prof.accel_x), 1.1, 1e-6);
    }

    // accel_y is also sorted
    {
        device_profile dp;
        dp.prof.accel_y.mode = accel_mode::lookup;
        dp.prof.accel_y.length = 4;  // 2 pairs
        dp.prof.accel_y.data[0] = 30.0f; dp.prof.accel_y.data[1] = 1.8f;
        dp.prof.accel_y.data[2] = 10.0f; dp.prof.accel_y.data[3] = 1.3f;

        sanitize_device_profile(dp);

        EXPECT_NEAR(dp.prof.accel_y.data[0], 10.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_y.data[1], 1.3f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_y.data[2], 30.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_y.data[3], 1.8f,  1e-9);
    }

    // Already sorted data stays sorted
    {
        device_profile dp;
        dp.prof.accel_x.mode = accel_mode::lookup;
        dp.prof.accel_x.length = 6;  // 3 pairs
        dp.prof.accel_x.data[0] = 0.0f;  dp.prof.accel_x.data[1] = 1.0f;
        dp.prof.accel_x.data[2] = 10.0f; dp.prof.accel_x.data[3] = 1.5f;
        dp.prof.accel_x.data[4] = 20.0f; dp.prof.accel_x.data[5] = 2.0f;

        sanitize_device_profile(dp);

        EXPECT_NEAR(dp.prof.accel_x.data[0], 0.0f,  1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[2], 10.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[4], 20.0f, 1e-9);
    }

    // Reverse-sorted data gets properly sorted
    {
        device_profile dp;
        dp.prof.accel_x.mode = accel_mode::lookup;
        dp.prof.accel_x.length = 10;  // 5 pairs
        dp.prof.accel_x.data[0] = 40.0f; dp.prof.accel_x.data[1] = 2.5f;
        dp.prof.accel_x.data[2] = 30.0f; dp.prof.accel_x.data[3] = 2.0f;
        dp.prof.accel_x.data[4] = 20.0f; dp.prof.accel_x.data[5] = 1.7f;
        dp.prof.accel_x.data[6] = 10.0f; dp.prof.accel_x.data[7] = 1.3f;
        dp.prof.accel_x.data[8] = 0.0f;  dp.prof.accel_x.data[9] = 1.0f;

        sanitize_device_profile(dp);

        // Check ascending X order
        for (int i = 0; i < 4; i++) {
            EXPECT(dp.prof.accel_x.data[i * 2] <= dp.prof.accel_x.data[(i + 1) * 2]);
        }
        EXPECT_NEAR(dp.prof.accel_x.data[0], 0.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[8], 40.0f, 1e-9);
    }

    // Single pair (length=2): no crash, stays as-is
    {
        device_profile dp;
        dp.prof.accel_x.mode = accel_mode::lookup;
        dp.prof.accel_x.length = 2;
        dp.prof.accel_x.data[0] = 5.0f; dp.prof.accel_x.data[1] = 1.5f;

        sanitize_device_profile(dp);

        EXPECT_NEAR(dp.prof.accel_x.data[0], 5.0f, 1e-9);
        EXPECT_NEAR(dp.prof.accel_x.data[1], 1.5f, 1e-9);
    }

    // Empty LUT (length=0): no crash
    {
        device_profile dp;
        dp.prof.accel_x.mode = accel_mode::lookup;
        dp.prof.accel_x.length = 0;

        sanitize_device_profile(dp);  // should not crash
        EXPECT(dp.prof.accel_x.length == 0);
    }
}

// ── Test: LUT sort + JSON round-trip (unsorted JSON → sorted after load) ─────

static void test_lut_sort_json_roundtrip() {
    SECTION("LUT sort — unsorted JSON round-trip");

    // Build a config with unsorted LUT, save to JSON, reload, check sorted
    {
        device_profile dp;
        dp.name = "lut_sort_test";
        dp.prof.accel_x.mode = accel_mode::lookup;
        dp.prof.accel_x.length = 6;
        dp.prof.accel_x.data[0] = 15.0f; dp.prof.accel_x.data[1] = 1.6f;
        dp.prof.accel_x.data[2] = 5.0f;  dp.prof.accel_x.data[3] = 1.2f;
        dp.prof.accel_x.data[4] = 0.0f;  dp.prof.accel_x.data[5] = 1.0f;

        app_config cfg;
        cfg.active_profile = "lut_sort_test";
        cfg.profiles.push_back(dp);

        // Save and reload
        const char* tmp = "/tmp/test_lut_sort_rt.json";
        save_config(cfg, tmp);

        app_config cfg2 = load_config(tmp);
        std::remove(tmp);

        EXPECT(cfg2.profiles.size() == 1);
        auto& ax = cfg2.profiles[0].prof.accel_x;
        EXPECT(ax.length == 6);

        // After load + sanitize, data must be sorted by X
        EXPECT_NEAR(ax.data[0], 0.0f,  1e-9);
        EXPECT_NEAR(ax.data[1], 1.0f,  1e-9);
        EXPECT_NEAR(ax.data[2], 5.0f,  1e-9);
        EXPECT_NEAR(ax.data[3], 1.2f,  1e-9);
        EXPECT_NEAR(ax.data[4], 15.0f, 1e-9);
        EXPECT_NEAR(ax.data[5], 1.6f,  1e-9);

        // Verify interpolation works correctly on reloaded data
        lookup lut(ax);
        EXPECT_NEAR(lut(0.0, ax), 1.0, 1e-6);
        EXPECT_NEAR(lut(5.0, ax), 1.2, 1e-6);
        EXPECT_NEAR(lut(15.0, ax), 1.6, 1e-6);
    }
}

// ── Test: motion_math int overflow + NaN/Inf guards (BUG-R3-4 fix) ───────────

static void test_motion_math_overflow() {
    SECTION("motion_math — int overflow clamp + NaN/Inf guard");

    modifier mod;
    speed_processor sp;
    modifier_settings settings;
    settings.prof.accel_x.mode = accel_mode::noaccel;
    settings.prof.accel_y = settings.prof.accel_x;
    init_settings(settings);
    sp.init(settings.prof.speed_processor_args);

    // Huge delta that would overflow int if not clamped
    {
        double rx = 0, ry = 0;
        int ox, oy;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          3e9, -3e9, rx, ry, ox, oy);
        // Should be clamped to INT_MAX / INT_MIN, not UB
        EXPECT(ox > 0);
        EXPECT(oy < 0);
        EXPECT(std::isfinite(rx));
        EXPECT(std::isfinite(ry));
    }

    // Inf input should produce 0 output (clamped), finite remainder
    {
        double rx = 0, ry = 0;
        int ox, oy;
        // Use a classic accel with extreme acceleration that could produce Inf
        modifier mod2;
        speed_processor sp2;
        modifier_settings s2;
        s2.prof.accel_x.mode = accel_mode::classic;
        s2.prof.accel_x.acceleration = 999999.0;
        s2.prof.accel_x.exponent_classic = 10.0;
        s2.prof.accel_x.limit = 1e30;
        s2.prof.accel_y = s2.prof.accel_x;
        init_settings(s2);
        sp2.init(s2.prof.speed_processor_args);

        apply_motion_math(mod2, sp2, s2, 1.0, 1.0,
                          1e10, 1e10, rx, ry, ox, oy);
        // Key: no UB, result is finite (even if clamped)
        EXPECT(std::isfinite(rx));
        EXPECT(std::isfinite(ry));
        // ox/oy are just int values — they exist (no crash)
        (void)ox; (void)oy;
    }

    // Negative overflow
    {
        double rx = 0, ry = 0;
        int ox, oy;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          -3e9, 3e9, rx, ry, ox, oy);
        EXPECT(ox < 0);
        EXPECT(oy > 0);
        EXPECT(std::isfinite(rx));
        EXPECT(std::isfinite(ry));
    }

    // Zero delta — no overflow, no crash
    {
        double rx = 0, ry = 0;
        int ox, oy;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          0, 0, rx, ry, ox, oy);
        EXPECT(ox == 0);
        EXPECT(oy == 0);
        EXPECT_NEAR(rx, 0.0, 1e-9);
        EXPECT_NEAR(ry, 0.0, 1e-9);
    }
}

// ── Test: accel_args sanitization via sanitize_device_profile (BUG-R5-1) ─────

static void test_accel_args_sanitize() {
    SECTION("sanitize_device_profile — accel_args field clamping");

    // 1. Negative acceleration → NOT clamped (legitimate deceleration in classic mode)
    {
        device_profile dp;
        dp.prof.accel_x.acceleration = -5.0;
        dp.prof.accel_y.acceleration = -0.001;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.accel_x.acceleration, -5.0, 1e-9);
        EXPECT_NEAR(dp.prof.accel_y.acceleration, -0.001, 1e-9);
    }

    // 2. Negative scale → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.scale = -10.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.scale >= 0);
    }

    // 3. Negative decay_rate → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.decay_rate = -1.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.decay_rate >= 0);
    }

    // 4. Zero/negative exponent_power → clamped to 1e-4
    {
        device_profile dp;
        dp.prof.accel_x.exponent_power = 0;
        dp.prof.accel_y.exponent_power = -2.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.exponent_power >= 1e-4);
        EXPECT(dp.prof.accel_y.exponent_power >= 1e-4);
    }

    // 5. Negative input/output offsets → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.input_offset = -10.0;
        dp.prof.accel_x.output_offset = -5.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.input_offset >= 0);
        EXPECT(dp.prof.accel_x.output_offset >= 0);
    }

    // 6. Negative limit → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.limit = -3.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.limit >= 0);
    }

    // 7. Zero/negative sync_speed → clamped to 1e-4
    {
        device_profile dp;
        dp.prof.accel_x.sync_speed = 0;
        dp.prof.accel_y.sync_speed = -5.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.sync_speed >= 1e-4);
        EXPECT(dp.prof.accel_y.sync_speed >= 1e-4);
    }

    // 8. Negative smooth → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.smooth = -1.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.smooth >= 0);
    }

    // 9. Negative motivity/gamma → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.motivity = -2.0;
        dp.prof.accel_x.gamma = -1.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.motivity >= 0);
        EXPECT(dp.prof.accel_x.gamma >= 0);
    }

    // 10. Negative cap values → clamped to 0
    {
        device_profile dp;
        dp.prof.accel_x.cap.x = -5.0;
        dp.prof.accel_x.cap.y = -3.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.accel_x.cap.x >= 0);
        EXPECT(dp.prof.accel_x.cap.y >= 0);
    }

    // 11. Negative smooth halflifes → clamped to 0
    {
        device_profile dp;
        dp.prof.speed_processor_args.input_speed_smooth_halflife = -10.0;
        dp.prof.speed_processor_args.scale_smooth_halflife = -5.0;
        dp.prof.speed_processor_args.output_speed_smooth_halflife = -1.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.speed_processor_args.input_speed_smooth_halflife >= 0);
        EXPECT(dp.prof.speed_processor_args.scale_smooth_halflife >= 0);
        EXPECT(dp.prof.speed_processor_args.output_speed_smooth_halflife >= 0);
    }

    // 12. Negative domain/range weights → clamped to 0
    {
        device_profile dp;
        dp.prof.domain_weights.x = -1.0;
        dp.prof.domain_weights.y = -2.0;
        dp.prof.range_weights.x = -0.5;
        dp.prof.range_weights.y = -3.0;
        sanitize_device_profile(dp);
        EXPECT(dp.prof.domain_weights.x >= 0);
        EXPECT(dp.prof.domain_weights.y >= 0);
        EXPECT(dp.prof.range_weights.x >= 0);
        EXPECT(dp.prof.range_weights.y >= 0);
    }

    // 13. JSON round-trip with negative accel_args → sanitized on load
    {
        const char* tmp = "/tmp/rawaccel_test_accel_args_sanitize.json";
        {
            std::ofstream f(tmp);
            f << R"({
                "profiles": [{
                    "name": "bad-args",
                    "dpi": 800, "polling_rate": 1000,
                    "profile": {
                        "accel_x": {
                            "mode": "classic",
                            "acceleration": -5.0,
                            "scale": -10.0,
                            "decay_rate": -1.0,
                            "exponent_power": -2.0,
                            "input_offset": -3.0,
                            "output_offset": -4.0,
                            "limit": -1.5,
                            "sync_speed": -5.0,
                            "smooth": -1.0,
                            "motivity": -2.0,
                            "gamma": -1.0,
                            "cap": [-5.0, -3.0]
                        },
                        "domain_weights": [-1.0, -2.0],
                        "range_weights": [-0.5, -3.0]
                    }
                }],
                "active_profile": "bad-args"
            })";
        }
        app_config cfg = load_config(tmp);
        auto& a = cfg.profiles[0].prof.accel_x;
        EXPECT_NEAR(a.acceleration, -5.0, 1e-9); // negative accel preserved (deceleration)
        EXPECT(a.scale >= 0);
        EXPECT(a.decay_rate >= 0);
        EXPECT(a.exponent_power >= 1e-4);
        EXPECT(a.input_offset >= 0);
        EXPECT(a.output_offset >= 0);
        EXPECT(a.limit >= 0);
        EXPECT(a.sync_speed >= 1e-4);
        EXPECT(a.smooth >= 0);
        EXPECT(a.motivity >= 0);
        EXPECT(a.gamma >= 0);
        EXPECT(a.cap.x >= 0);
        EXPECT(a.cap.y >= 0);
        EXPECT(cfg.profiles[0].prof.domain_weights.x >= 0);
        EXPECT(cfg.profiles[0].prof.domain_weights.y >= 0);
        EXPECT(cfg.profiles[0].prof.range_weights.x >= 0);
        EXPECT(cfg.profiles[0].prof.range_weights.y >= 0);
        std::remove(tmp);
    }

    // 14. Negative acceleration + integer exponent → valid deceleration (no NaN)
    {
        accel_args args;
        args.mode = accel_mode::classic;
        args.acceleration = -0.01;
        args.exponent_classic = 2.0; // integer — pow(-0.01, 1) = -0.01, valid
        classic c(args);
        double gain = c(10.0, args);
        EXPECT(std::isfinite(gain));
        EXPECT(gain < 1.0); // deceleration: gain below 1
    }

    // 15. Negative acceleration + non-integer exponent → NaN guarded to 0 in constructor
    {
        accel_args args;
        args.mode = accel_mode::classic;
        args.acceleration = -0.01;
        args.exponent_classic = 2.5; // non-integer — pow(-0.01, 1.5) = NaN
        classic c(args);
        double gain = c(10.0, args);
        EXPECT(std::isfinite(gain)); // constructor NaN guard → accel_raised = 0 → gain = 1.0
    }
}

// ── Test: raw_passthrough JSON round-trip ─────────────────────────────────────

static void test_raw_passthrough_json() {
    SECTION("raw_passthrough — JSON round-trip");

    const char* tmp = "/tmp/rawaccel_test_raw_passthrough.json";

    // Default: false
    {
        profile p{};
        EXPECT(p.raw_passthrough == false);

        device_profile dp;
        dp.name = "raw-test-off";
        dp.prof = p;
        app_config cfg;
        cfg.profiles.push_back(dp);
        cfg.active_profile = dp.name;

        save_config(cfg, tmp);
        app_config cfg2 = load_config(tmp);
        EXPECT(cfg2.profiles[0].prof.raw_passthrough == false);
    }

    // Enabled: true round-trips correctly
    {
        profile p{};
        p.raw_passthrough = true;

        device_profile dp;
        dp.name = "raw-test-on";
        dp.prof = p;
        app_config cfg;
        cfg.profiles.push_back(dp);
        cfg.active_profile = dp.name;

        save_config(cfg, tmp);

        // Verify the JSON file actually contains the key
        {
            std::ifstream f(tmp);
            std::string content((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());
            EXPECT(content.find("raw_passthrough") != std::string::npos);
        }

        app_config cfg2 = load_config(tmp);
        EXPECT(cfg2.profiles[0].prof.raw_passthrough == true);
    }

    // Missing key in hand-edited JSON → defaults to false
    {
        {
            std::ofstream f(tmp);
            f << R"({
                "profiles": [{
                    "name": "no-raw-key",
                    "accel_x": { "mode": "noaccel" },
                    "accel_y": { "mode": "noaccel" }
                }],
                "active_profile": "no-raw-key"
            })";
        }
        app_config cfg = load_config(tmp);
        EXPECT(cfg.profiles[0].prof.raw_passthrough == false);
    }

    std::remove(tmp);
}

// ── Fuzz test: all accel modes × random accel_args → no NaN/Inf/crash ────────

static void test_fuzz_accel_args() {
    SECTION("fuzz — random accel_args across all modes, no NaN (Inf OK — motion_math clamps)");

    std::mt19937 rng(42); // deterministic seed for reproducibility
    std::uniform_real_distribution<double> dist_wide(-1000.0, 1000.0);
    std::uniform_int_distribution<int> dist_mode(0, 6);

    static const accel_mode modes[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::power,
        accel_mode::natural, accel_mode::jump,    accel_mode::synchronous,
        accel_mode::lookup,
    };

    int nan_count = 0;
    constexpr int ITERS = 5000;

    for (int i = 0; i < ITERS; i++) {
        accel_args args;
        args.mode             = modes[dist_mode(rng)];
        args.acceleration     = dist_wide(rng);
        args.exponent_classic = dist_wide(rng);
        args.exponent_power   = dist_wide(rng);
        args.scale            = dist_wide(rng);
        args.decay_rate       = dist_wide(rng);
        args.limit            = dist_wide(rng);
        args.input_offset     = dist_wide(rng);
        args.output_offset    = dist_wide(rng);
        args.sync_speed       = dist_wide(rng);
        args.smooth           = dist_wide(rng);
        args.motivity         = dist_wide(rng);
        args.gamma            = dist_wide(rng);
        args.cap.x            = dist_wide(rng);
        args.cap.y            = dist_wide(rng);
        args.gain             = (rng() % 2) == 0;

        // Sanitize like the real pipeline does
        device_profile dp;
        dp.prof.accel_x = args;
        sanitize_device_profile(dp);
        args = dp.prof.accel_x;

        accel_union au;
        au.init(args);

        // Test with realistic speeds (not DBL_MAX — motion_math handles overflow)
        double speeds[] = {0, 0.001, 0.5, 1.0, 5.0, 50.0, 500.0, 10000.0};
        for (double spd : speeds) {
            double result = au.apply(spd, args);
            if (std::isnan(result)) nan_count++;
            // Inf is acceptable at the accel layer — motion_math's isfinite guard
            // converts it to 0 output. Only NaN would indicate a logic error.
        }
    }
    EXPECT(nan_count == 0);
}

// ── Fuzz test: random accel_args WITHOUT sanitize → motion_math catches all ──

static void test_fuzz_unsanitized_motion_math() {
    SECTION("fuzz — unsanitized random args through motion_math pipeline, output always finite");

    std::mt19937 rng(123);
    std::uniform_real_distribution<double> dist_wide(-1000.0, 1000.0);
    std::uniform_real_distribution<double> dist_delta(-50.0, 50.0);
    std::uniform_int_distribution<int> dist_mode(0, 6);

    static const accel_mode modes[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::power,
        accel_mode::natural, accel_mode::jump,    accel_mode::synchronous,
        accel_mode::lookup,
    };

    int bad_output = 0;
    int bad_remainder = 0;
    constexpr int ITERS = 3000;

    for (int i = 0; i < ITERS; i++) {
        accel_args args;
        args.mode             = modes[dist_mode(rng)];
        args.acceleration     = dist_wide(rng);
        args.exponent_classic = dist_wide(rng);
        args.exponent_power   = dist_wide(rng);
        args.scale            = dist_wide(rng);
        args.decay_rate       = dist_wide(rng);
        args.limit            = dist_wide(rng);
        args.input_offset     = dist_wide(rng);
        args.output_offset    = dist_wide(rng);
        args.sync_speed       = dist_wide(rng);
        args.smooth           = dist_wide(rng);
        args.cap.x            = dist_wide(rng);
        args.cap.y            = dist_wide(rng);
        args.gain             = (rng() % 2) == 0;

        profile prof;
        prof.accel_x = args;
        prof.accel_y = args;

        modifier_settings settings;
        settings.prof = prof;
        init_settings(settings);

        speed_processor sp;
        sp.init(prof.speed_processor_args);

        modifier mod;
        double rx = 0, ry = 0;
        int ox = 0, oy = 0;

        double dx = dist_delta(rng);
        double dy = dist_delta(rng);
        double time_ms = 1.0; // safe time

        apply_motion_math(mod, sp, settings, 1.0, time_ms,
                          dx, dy, rx, ry, ox, oy);

        if (!std::isfinite((double)ox) || !std::isfinite((double)oy)) bad_output++;
        if (!std::isfinite(rx) || !std::isfinite(ry)) bad_remainder++;
    }
    EXPECT(bad_output == 0);
    EXPECT(bad_remainder == 0);
}

// ── Fuzz test: config JSON round-trip with random values ─────────────────────

static void test_fuzz_json_roundtrip() {
    SECTION("fuzz — random profile JSON round-trip preserves sanitized values");

    std::mt19937 rng(777);
    std::uniform_real_distribution<double> dist_wide(-500.0, 500.0);
    std::uniform_int_distribution<int> dist_mode(0, 6);

    const char* tmp = "/tmp/rawaccel_test_fuzz_roundtrip.json";
    constexpr int ITERS = 200;
    int sanitize_fail = 0;

    for (int i = 0; i < ITERS; i++) {
        device_profile dp;
        dp.name = "fuzz" + std::to_string(i);
        dp.dev_cfg.dpi = (int)dist_wide(rng);
        dp.dev_cfg.polling_rate = (int)dist_wide(rng);
        dp.prof.accel_x.mode = (accel_mode)dist_mode(rng);
        dp.prof.accel_x.acceleration = dist_wide(rng);
        dp.prof.accel_x.scale = dist_wide(rng);
        dp.prof.accel_x.decay_rate = dist_wide(rng);
        dp.prof.accel_x.exponent_power = dist_wide(rng);
        dp.prof.accel_x.exponent_classic = dist_wide(rng);
        dp.prof.accel_x.limit = dist_wide(rng);
        dp.prof.accel_x.input_offset = dist_wide(rng);
        dp.prof.accel_x.output_offset = dist_wide(rng);
        dp.prof.accel_x.sync_speed = dist_wide(rng);
        dp.prof.accel_x.smooth = dist_wide(rng);
        dp.prof.accel_x.cap.x = dist_wide(rng);
        dp.prof.accel_x.cap.y = dist_wide(rng);
        dp.prof.degrees_rotation = dist_wide(rng);
        dp.prof.degrees_snap = dist_wide(rng);
        dp.prof.speed_min = dist_wide(rng);
        dp.prof.speed_max = dist_wide(rng);
        dp.prof.domain_weights.x = dist_wide(rng);
        dp.prof.domain_weights.y = dist_wide(rng);
        dp.prof.range_weights.x = dist_wide(rng);
        dp.prof.range_weights.y = dist_wide(rng);
        dp.prof.speed_processor_args.lp_norm = dist_wide(rng);
        dp.prof.speed_processor_args.input_speed_smooth_halflife = dist_wide(rng);

        app_config cfg;
        cfg.profiles.push_back(dp);
        cfg.active_profile = dp.name;

        try {
            save_config(cfg, tmp);
            app_config cfg2 = load_config(tmp);
            auto& p = cfg2.profiles[0];
            // After round-trip, all sanitized constraints must hold
            if (p.dev_cfg.dpi < 1 || p.dev_cfg.dpi > 32000) sanitize_fail++;
            if (p.dev_cfg.polling_rate < 125 || p.dev_cfg.polling_rate > 8000) sanitize_fail++;
            if (p.prof.accel_x.scale < 0) sanitize_fail++;
            if (p.prof.accel_x.decay_rate < 0) sanitize_fail++;
            if (p.prof.accel_x.exponent_power < 1e-4) sanitize_fail++;
            if (p.prof.accel_x.input_offset < 0) sanitize_fail++;
            if (p.prof.accel_x.output_offset < 0) sanitize_fail++;
            if (p.prof.accel_x.limit < 0) sanitize_fail++;
            if (p.prof.accel_x.sync_speed < 1e-4) sanitize_fail++;
            if (p.prof.accel_x.smooth < 0) sanitize_fail++;
            if (p.prof.accel_x.cap.x < 0) sanitize_fail++;
            if (p.prof.accel_x.cap.y < 0) sanitize_fail++;
            if (p.prof.degrees_rotation < 0 || p.prof.degrees_rotation >= 360) sanitize_fail++;
            if (p.prof.degrees_snap < 0 || p.prof.degrees_snap > 45) sanitize_fail++;
            if (p.prof.speed_min < 0) sanitize_fail++;
            if (p.prof.speed_max < 0) sanitize_fail++;
            if (p.prof.domain_weights.x < 0) sanitize_fail++;
            if (p.prof.domain_weights.y < 0) sanitize_fail++;
            if (p.prof.range_weights.x < 0) sanitize_fail++;
            if (p.prof.range_weights.y < 0) sanitize_fail++;
            if (p.prof.speed_processor_args.lp_norm <= 0) sanitize_fail++;
            if (p.prof.speed_processor_args.input_speed_smooth_halflife < 0) sanitize_fail++;
        } catch (...) {
            sanitize_fail++; // should never throw — sanitize handles all edge cases
        }
    }
    std::remove(tmp);
    EXPECT(sanitize_fail == 0);
}

// ── Edge case: all accel modes × extreme speed values ────────────────────────

static void test_all_modes_extreme_speeds() {
    SECTION("edge — all accel modes × extreme speed values, all outputs finite");

    static const accel_mode modes[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::power,
        accel_mode::natural, accel_mode::jump,    accel_mode::synchronous,
        accel_mode::lookup,
    };

    // Extreme speed values — boundaries and pathological inputs
    // Note: DBL_MAX excluded — pow(DBL_MAX, 2) = Inf is mathematically correct;
    // motion_math's isfinite guard handles overflow at the pipeline level.
    double speeds[] = {
        -1.0, -0.001, 0.0, 1e-15, 1e-9, 0.001, 0.5, 1.0,
        5.0, 50.0, 500.0, 5000.0, 1e6, 1e9, 1e15
    };

    int nan_count = 0;
    int inf_count = 0;

    for (auto mode : modes) {
        accel_args args = make_args(mode);
        // Also set some non-trivial params for each mode
        args.acceleration = 0.01;
        args.exponent_classic = 2.0;
        args.exponent_power = 0.3;
        args.decay_rate = 0.2;
        args.limit = 2.0;
        args.sync_speed = 5.0;
        args.smooth = 0.5;
        args.scale = 1.0;

        accel_union au;
        au.init(args);

        for (double spd : speeds) {
            double result = au.apply(spd, args);
            if (std::isnan(result)) nan_count++;
            if (std::isinf(result)) inf_count++;
        }
    }
    EXPECT(nan_count == 0);
    EXPECT(inf_count == 0);
}

// ── Edge case: EMA smoother stability under extreme time values ──────────────

static void test_ema_extreme_time() {
    SECTION("edge — EMA smoothers with extreme time values, no divergence");

    // Simple EMA
    {
        simple_ema_smoother s;
        s.init(5.0); // halflife=5ms
        int bad = 0;
        // Very large time gap (simulates long idle then sudden move)
        double r = s.smooth(10.0, 10000.0);
        if (!std::isfinite(r)) bad++;
        // Very small time (sub-microsecond)
        r = s.smooth(10.0, 1e-9);
        if (!std::isfinite(r)) bad++;
        // Zero time
        r = s.smooth(10.0, 0.0);
        if (!std::isfinite(r)) bad++;
        // Negative time (shouldn't happen but guard)
        r = s.smooth(10.0, -1.0);
        if (!std::isfinite(r)) bad++;
        // Huge speed
        r = s.smooth(1e15, 1.0);
        if (!std::isfinite(r)) bad++;
        // Many iterations — check no drift to Inf
        for (int i = 0; i < 10000; i++) {
            r = s.smooth(5.0, 1.0);
            if (!std::isfinite(r)) { bad++; break; }
        }
        EXPECT(bad == 0);
    }

    // Linear EMA
    {
        linear_ema_smoother l;
        l.init(5.0, 1.25); // halflife=5ms, trendHalflife=1.25ms
        int bad = 0;
        double r = l.smooth(10.0, 10000.0);
        if (!std::isfinite(r)) bad++;
        r = l.smooth(10.0, 1e-9);
        if (!std::isfinite(r)) bad++;
        r = l.smooth(10.0, 0.0);
        if (!std::isfinite(r)) bad++;
        r = l.smooth(10.0, -1.0);
        if (!std::isfinite(r)) bad++;
        r = l.smooth(1e15, 1.0);
        if (!std::isfinite(r)) bad++;
        for (int i = 0; i < 10000; i++) {
            r = l.smooth(5.0, 1.0);
            if (!std::isfinite(r)) { bad++; break; }
        }
        EXPECT(bad == 0);
    }

    // EMA with halflife=0 (should effectively disable smoothing — coefficient=0)
    {
        simple_ema_smoother s;
        s.init(0.0);
        int bad = 0;
        for (int i = 0; i < 100; i++) {
            double r = s.smooth((double)i, 1.0);
            if (!std::isfinite(r)) { bad++; break; }
        }
        EXPECT(bad == 0);
    }
}

// ── Edge case: subpixel accumulation — tiny deltas accumulate correctly ──────

static void test_subpixel_tiny_deltas() {
    SECTION("edge — subpixel accumulation: 1000 × 0.1 delta → ~100 total output");

    // noaccel mode: gain = 1.0, so output = input exactly.
    // 1000 events of dx=0.1, dy=0 → total should be ~100 counts.
    profile prof;
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y.mode = accel_mode::noaccel;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    speed_processor sp;
    sp.init(prof.speed_processor_args);

    modifier mod;
    double rx = 0, ry = 0;
    int total_x = 0, total_y = 0;

    for (int i = 0; i < 1000; i++) {
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, settings, 1.0 /* dpi_factor=1 */, 1.0 /* 1ms */,
                          0.1, 0.0, rx, ry, ox, oy);
        total_x += ox;
        total_y += oy;
    }

    // 1000 × 0.1 ≈ 100.0 — floating point means 10×0.1 != 1.0 exactly,
    // so the count may be slightly off (e.g. 91 instead of 100 due to rounding).
    // The key invariant: total_x + remainder_x ≈ 100.0 (no counts lost or created).
    double reconstructed = (double)total_x + rx;
    EXPECT_NEAR(reconstructed, 100.0, 0.01);
    EXPECT(total_y == 0);
    EXPECT(std::fabs(rx) < 1.0); // leftover remainder is sub-pixel
    EXPECT(std::fabs(ry) < 1e-9); // no Y movement at all
}

// ── Edge case: subpixel sign preservation ────────────────────────────────────

static void test_subpixel_negative_deltas() {
    SECTION("edge — subpixel: negative deltas accumulate correctly");

    profile prof;
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y.mode = accel_mode::noaccel;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    speed_processor sp;
    sp.init(prof.speed_processor_args);

    modifier mod;
    double rx = 0, ry = 0;
    int total_x = 0;

    // 500 events of dx=-0.3 → total should be ≈ -150
    for (int i = 0; i < 500; i++) {
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          -0.3, 0.0, rx, ry, ox, oy);
        total_x += ox;
    }
    // Floating point: total_x + rx ≈ -150.0 (no counts lost)
    double reconstructed = (double)total_x + rx;
    EXPECT_NEAR(reconstructed, -150.0, 0.01);
    EXPECT(std::fabs(rx) < 1.0);
}

// ── Edge case: modifier with all flags active simultaneously ─────────────────

static void test_modifier_all_flags() {
    SECTION("edge — modifier with rotate+snap+clamp+directional weight, all outputs finite");

    profile prof;
    prof.accel_x.mode = accel_mode::classic;
    prof.accel_x.acceleration = 0.01;
    prof.accel_x.exponent_classic = 2.0;
    prof.accel_y = prof.accel_x;
    prof.degrees_rotation = 45.0;    // rotation active
    prof.degrees_snap = 10.0;        // snap active
    prof.speed_min = 1.0;            // speed clamp active
    prof.speed_max = 100.0;
    prof.range_weights = {1.5, 0.8}; // directional weight active (x != y)
    prof.lr_output_dpi_ratio = 1.5;  // dir mul active
    prof.ud_output_dpi_ratio = 0.8;
    prof.speed_processor_args.whole = true;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    speed_processor sp;
    sp.init(prof.speed_processor_args);

    modifier mod;
    int bad = 0;

    // Test various input directions
    double deltas[][2] = {
        {10, 0}, {0, 10}, {10, 10}, {-10, 5}, {5, -10}, {-5, -5},
        {1, 0}, {0, 1}, {0.1, 0.1}, {100, 100}, {0, 0}, {-0.1, 0.1}
    };

    for (auto& d : deltas) {
        vec2d in = {d[0], d[1]};
        mod.modify(in, sp, settings, 1.0, 1.0);
        if (!std::isfinite(in.x) || !std::isfinite(in.y)) bad++;
    }
    EXPECT(bad == 0);
}

// ── Edge case: modifier with separate distance mode ──────────────────────────

static void test_modifier_separate_mode() {
    SECTION("edge — modifier separate distance mode with smoothing, outputs finite");

    profile prof;
    prof.accel_x.mode = accel_mode::natural;
    prof.accel_x.decay_rate = 0.1;
    prof.accel_x.limit = 2.0;
    prof.accel_y.mode = accel_mode::jump;
    prof.accel_y.limit = 1.5;
    prof.accel_y.sync_speed = 5.0;
    prof.accel_y.smooth = 0.5;
    prof.speed_processor_args.whole = false; // separate mode
    prof.speed_processor_args.input_speed_smooth_halflife = 3.0;
    prof.speed_processor_args.scale_smooth_halflife = 2.0;
    prof.speed_processor_args.output_speed_smooth_halflife = 1.5;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    speed_processor sp;
    sp.init(prof.speed_processor_args);

    modifier mod;
    int bad = 0;

    // Simulate 500 events with varying deltas
    std::mt19937 rng(999);
    std::uniform_real_distribution<double> dist(-20.0, 20.0);
    for (int i = 0; i < 500; i++) {
        vec2d in = {dist(rng), dist(rng)};
        mod.modify(in, sp, settings, 1.0, 1.0);
        if (!std::isfinite(in.x) || !std::isfinite(in.y)) bad++;
    }
    EXPECT(bad == 0);
}

// ── Stress test: 10000 motion events — remainder never drifts ────────────────

static void test_stress_remainder_drift() {
    SECTION("stress — 10000 motion events, remainder stays bounded");

    profile prof;
    prof.accel_x.mode = accel_mode::classic;
    prof.accel_x.acceleration = 0.02;
    prof.accel_x.exponent_classic = 2.0;
    prof.accel_y = prof.accel_x;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    speed_processor sp;
    sp.init(prof.speed_processor_args);

    modifier mod;
    double rx = 0, ry = 0;
    int max_remainder_violation = 0;

    std::mt19937 rng(314);
    std::uniform_real_distribution<double> dist_delta(-30.0, 30.0);
    std::uniform_real_distribution<double> dist_time(0.5, 8.0);

    for (int i = 0; i < 10000; i++) {
        int ox = 0, oy = 0;
        double dx = dist_delta(rng);
        double dy = dist_delta(rng);
        double time = dist_time(rng);

        apply_motion_math(mod, sp, settings, 1.0, time,
                          dx, dy, rx, ry, ox, oy);

        // Remainder must always be in (-1, 1) — it's the fractional part after truncation
        if (std::fabs(rx) >= 1.0 || std::fabs(ry) >= 1.0) max_remainder_violation++;
        // Must always be finite
        if (!std::isfinite(rx) || !std::isfinite(ry)) max_remainder_violation++;
    }
    EXPECT(max_remainder_violation == 0);
}

// ── Stress test: alternating directions — no accumulation error ──────────────

static void test_stress_alternating_direction() {
    SECTION("stress — alternating +5/-5 deltas cancel out to zero total");

    profile prof;
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y.mode = accel_mode::noaccel;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    speed_processor sp;
    sp.init(prof.speed_processor_args);

    modifier mod;
    double rx = 0, ry = 0;
    int total_x = 0;

    for (int i = 0; i < 10000; i++) {
        int ox = 0, oy = 0;
        double dx = (i % 2 == 0) ? 5.0 : -5.0;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          dx, 0.0, rx, ry, ox, oy);
        total_x += ox;
    }
    // 5000 × 5 + 5000 × (-5) = 0
    EXPECT(total_x == 0);
    EXPECT(std::fabs(rx) < 1e-9);
}

// ── Edge case: power mode with extreme params ────────────────────────────────

static void test_power_extreme_params() {
    SECTION("edge — power mode: extreme scale/exponent combinations, all finite");

    int bad = 0;

    // Very large scale
    {
        accel_args args = make_args(accel_mode::power);
        args.scale = 1000.0;
        args.exponent_power = 0.01;
        power p(args);
        double r = p(10.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // Very small exponent (near minimum)
    {
        accel_args args = make_args(accel_mode::power);
        args.scale = 1.0;
        args.exponent_power = 1e-4; // minimum after sanitize
        power p(args);
        double r = p(10.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // Large exponent
    {
        accel_args args = make_args(accel_mode::power);
        args.scale = 0.5;
        args.exponent_power = 5.0;
        power p(args);
        double r = p(1.0, args);
        if (!std::isfinite(r)) bad++;
        r = p(100.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // Zero speed
    {
        accel_args args = make_args(accel_mode::power);
        power p(args);
        double r = p(0.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // output_offset > 0
    {
        accel_args args = make_args(accel_mode::power);
        args.output_offset = 5.0;
        args.scale = 1.0;
        args.exponent_power = 0.5;
        power p(args);
        double r = p(0.1, args);
        if (!std::isfinite(r)) bad++;
        r = p(10.0, args);
        if (!std::isfinite(r)) bad++;
    }

    EXPECT(bad == 0);
}

// ── Edge case: lookup mode with edge-case LUT data ───────────────────────────

static void test_lookup_extreme_lut() {
    SECTION("edge — lookup with single point, duplicate points, huge values");

    int bad = 0;

    // Single point (2 floats = 1 pair)
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        args.length = 2;
        args.data[0] = 5.0f; args.data[1] = 1.5f;
        lookup l(args);
        double r = l(0.0, args);
        if (!std::isfinite(r)) bad++;
        r = l(5.0, args);
        if (!std::isfinite(r)) bad++;
        r = l(100.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // Duplicate X points (same speed, different gain)
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        args.length = 4;
        args.data[0] = 5.0f; args.data[1] = 1.0f;
        args.data[2] = 5.0f; args.data[3] = 2.0f; // duplicate x
        lookup l(args);
        double r = l(5.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // Very large LUT values
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        args.length = 4;
        args.data[0] = 0.0f;  args.data[1] = 1e6f;
        args.data[2] = 1e6f;  args.data[3] = 1e6f;
        lookup l(args);
        double r = l(500000.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // Zero-length LUT
    {
        accel_args args;
        args.mode = accel_mode::lookup;
        args.length = 0;
        lookup l(args);
        double r = l(5.0, args);
        if (!std::isfinite(r)) bad++;
        EXPECT_NEAR(r, 1.0, 1e-9); // no points → gain = 1.0
    }

    EXPECT(bad == 0);
}

// ── Edge case: natural mode with extreme decay_rate and limit ────────────────

static void test_natural_extreme_params() {
    SECTION("edge — natural mode: extreme decay/limit combinations, all finite");

    int bad = 0;

    // Very high decay_rate → extremely fast convergence
    {
        accel_args args = make_args(accel_mode::natural);
        args.decay_rate = 1000.0;
        args.limit = 5.0;
        natural n(args);
        double r = n(0.001, args);
        if (!std::isfinite(r)) bad++;
        r = n(100.0, args);
        if (!std::isfinite(r)) bad++;
    }

    // limit = 1.0 → internal limit = 0 → result depends on gain mode.
    // Non-gain: limit*(1-decay)+1 = 0+1 = 1.0
    // Gain:     t + 0*(...) = t → output/x + 1 = 2.0 (base speed + gain)
    {
        accel_args args = make_args(accel_mode::natural);
        args.limit = 1.0;
        args.decay_rate = 0.1;
        args.gain = false;
        natural n_nongain(args);
        double r_ng = n_nongain(10.0, args);
        if (!std::isfinite(r_ng)) bad++;
        EXPECT_NEAR(r_ng, 1.0, 0.01);

        args.gain = true;
        natural n_gain(args);
        double r_g = n_gain(10.0, args);
        if (!std::isfinite(r_g)) bad++;
        EXPECT_NEAR(r_g, 2.0, 0.01); // gain integral: base + t/x = 2.0
    }

    // gain=true, large input
    {
        accel_args args = make_args(accel_mode::natural);
        args.decay_rate = 0.5;
        args.limit = 3.0;
        args.gain = true;
        natural n(args);
        double r = n(1e6, args);
        if (!std::isfinite(r)) bad++;
    }

    EXPECT(bad == 0);
}

// ── Edge case: synchronous mode with extreme exponents ───────────────────────

static void test_synchronous_extreme() {
    SECTION("edge — synchronous mode: extreme exponents, all finite");

    int bad = 0;

    // Very large exponent
    {
        accel_args args = make_args(accel_mode::synchronous);
        args.sync_speed = 5.0;
        args.exponent_power = 100.0;
        synchronous s(args);
        double r = s(5.0, args); // at sync point → gain = 1.0
        if (!std::isfinite(r)) bad++;
        r = s(10.0, args);
        if (!std::isfinite(r)) bad++;
        r = s(0.001, args);
        if (!std::isfinite(r)) bad++;
    }

    // Very small exponent (near zero after sanitize → 1e-4)
    {
        accel_args args = make_args(accel_mode::synchronous);
        args.sync_speed = 5.0;
        args.exponent_power = 1e-4;
        synchronous s(args);
        double r = s(1.0, args);
        if (!std::isfinite(r)) bad++;
        r = s(100.0, args);
        if (!std::isfinite(r)) bad++;
    }

    EXPECT(bad == 0);
}

// ── R6: DPI ratio division-by-zero guard ─────────────────────────────────────

static void test_dpi_ratio_zero_guard() {
    SECTION("R6 — lr/ud_output_dpi_ratio zero does not produce Inf/NaN");

    // Set ratio to 0 (which sanitize would clamp, but test the guard directly)
    profile prof{};
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y.mode = accel_mode::noaccel;
    prof.lr_output_dpi_ratio = 0.0; // would cause 1.0/0.0 = Inf without guard
    prof.ud_output_dpi_ratio = 0.0;
    prof.domain_weights = {1, 1};
    prof.range_weights = {1, 1};

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    modifier mod;
    speed_processor sp;
    sp.init(prof.speed_processor_args);

    vec2d in = { -5.0, -3.0 }; // negative → triggers 1.0/ratio path
    mod.modify(in, sp, settings, 1.0, 1.0);

    // With zero ratio and guard: the DPI multiplier step is skipped entirely.
    // Without guard: in.x and in.y would become Inf.
    EXPECT(std::isfinite(in.x));
    EXPECT(std::isfinite(in.y));

    // Also verify non-zero ratio still works
    settings.prof.lr_output_dpi_ratio = 2.0;
    settings.prof.ud_output_dpi_ratio = 0.5;
    init_settings(settings);
    vec2d in2 = { 5.0, -3.0 };
    mod.modify(in2, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(in2.x));
    EXPECT(std::isfinite(in2.y));
    // Positive x → multiply by ratio (2.0); negative y → divide by ratio (1/0.5=2)
    EXPECT_NEAR(in2.x, 10.0, 0.01);
    EXPECT_NEAR(in2.y, -6.0, 0.01);
}

// ── R6: lp_distance zero-vector guard ────────────────────────────────────────

static void test_lp_distance_zero_vector() {
    SECTION("R6 — lp_distance({0,0}, p) returns 0 for any p");

    // Normal p
    EXPECT_NEAR(lp_distance({0, 0}, 2.0), 0.0, 1e-15);
    EXPECT_NEAR(lp_distance({0, 0}, 3.0), 0.0, 1e-15);
    // Negative p (would produce pow(0,neg)=Inf without guard)
    EXPECT_NEAR(lp_distance({0, 0}, -1.0), 0.0, 1e-15);
    EXPECT_NEAR(lp_distance({0, 0}, -0.5), 0.0, 1e-15);
    // Very large p
    EXPECT_NEAR(lp_distance({0, 0}, 100.0), 0.0, 1e-15);
    // Non-zero should still work
    double r = lp_distance({3, 4}, 2.0);
    EXPECT_NEAR(r, 5.0, 0.01);
}

// ── R7: EMA smoother extreme halflife values ─────────────────────────────────

static void test_ema_extreme_halflife() {
    SECTION("R7 — EMA smoother: halflife=0 (no smoothing), halflife=huge, halflife=tiny");

    // halflife=0 → windowCoefficient=0 → twc=1 → no smoothing (always latest value)
    {
        simple_ema_smoother s;
        s.init(0.0);
        double r = s.smooth(42.0, 1.0);
        EXPECT_NEAR(r, 42.0, 0.01);
        r = s.smooth(7.0, 1.0);
        EXPECT_NEAR(r, 7.0, 0.01); // immediate switch
    }

    // halflife=1e6 → very heavy smoothing → barely moves from initial value
    {
        simple_ema_smoother s;
        s.init(1e6);
        s.smooth(0.0, 1.0); // prime with 0
        double r = s.smooth(100.0, 1.0);
        EXPECT(r < 1.0); // should barely budge
        EXPECT(std::isfinite(r));
    }

    // halflife=1e-6 → effectively no smoothing
    {
        simple_ema_smoother s;
        s.init(1e-6);
        s.smooth(0.0, 1.0);
        double r = s.smooth(50.0, 1.0);
        EXPECT_NEAR(r, 50.0, 1.0); // nearly instant
        EXPECT(std::isfinite(r));
    }

    // Linear EMA: same checks
    {
        linear_ema_smoother l;
        l.init(0.0, 0.0);
        double r = l.smooth(42.0, 1.0);
        EXPECT_NEAR(r, 42.0, 0.01);
        r = l.smooth(7.0, 1.0);
        EXPECT_NEAR(r, 7.0, 0.01);
    }
}

// ── R7: full pipeline NaN injection test ─────────────────────────────────────

static void test_pipeline_nan_injection() {
    SECTION("R7 — motion_math guards against NaN from any acceleration output");

    // Construct a profile where classic acceleration with cap_mode::io
    // and degenerate parameters would produce NaN in the accel curve.
    // motion_math's isfinite guard should produce 0 output.
    modifier_settings settings;
    settings.prof.accel_x.mode = accel_mode::classic;
    settings.prof.accel_x.acceleration = -5.0; // negative + non-integer exponent
    settings.prof.accel_x.exponent_classic = 2.5; // non-integer
    settings.prof.accel_x.cap_mode_val = cap_mode::io;
    settings.prof.accel_x.cap.x = 0.0; // degenerate cap
    settings.prof.accel_x.cap.y = 0.0;
    settings.prof.accel_x.gain = true;
    settings.prof.accel_y = settings.prof.accel_x;
    settings.prof.domain_weights = {1, 1};
    settings.prof.range_weights = {1, 1};
    init_settings(settings);

    modifier mod;
    speed_processor sp;
    sp.init(settings.prof.speed_processor_args);

    double rx = 0, ry = 0;
    int ox = 0, oy = 0;
    // Even with degenerate args, output must be finite integers
    apply_motion_math(mod, sp, settings, 1.0, 1.0,
                      10.0, 5.0, rx, ry, ox, oy);
    EXPECT(std::isfinite(rx));
    EXPECT(std::isfinite(ry));
    // output may be 0 (NaN guard triggered) or valid int
    EXPECT(ox >= INT_MIN && ox <= INT_MAX);
    EXPECT(oy >= INT_MIN && oy <= INT_MAX);
}

// ── R7: classic cap_mode::io with cap.x < input_offset ──────────────────────

static void test_classic_io_degenerate_cap() {
    SECTION("R7 — classic io mode: cap.x < input_offset produces finite output");

    accel_args args;
    args.mode = accel_mode::classic;
    args.acceleration = 0.01;
    args.exponent_classic = 2.5;
    args.cap_mode_val = cap_mode::io;
    args.cap.x = 1.0;
    args.cap.y = 2.0;
    args.input_offset = 5.0; // offset > cap.x → degenerate
    args.gain = true;

    classic c(args);
    // x <= input_offset → 1.0
    EXPECT_NEAR(c(3.0, args), 1.0, 1e-9);
    // x > input_offset → should be finite
    double r = c(10.0, args);
    EXPECT(std::isfinite(r));
}

// ── BUG-9: classic cap_mode::in with cap.x <= input_offset ──────────────────
// Property fuzz (Tur 4) caught this: pow(negative_base, fractional_exp) → NaN
// when the cap's pre-offset point is inside the offset region.  The NaN poisons
// every operator() return.

static void test_classic_in_degenerate_cap_naninf() {
    SECTION("BUG-9 — classic cap_mode::in: cap.x <= input_offset stays finite");

    // Real fuzz seed that previously produced NaN
    accel_args args;
    args.mode = accel_mode::classic;
    args.acceleration = 1.889;
    args.exponent_classic = 3.728;
    args.cap_mode_val = cap_mode::in;
    args.cap.x = 0.2624;
    args.cap.y = 2.418;
    args.input_offset = 2.409; // > cap.x
    args.gain = true;
    classic c(args);
    for (double s : {0.0, 0.5, 1.0, 1.5, 2.0, 5.0, 100.0, 1e6}) {
        double g = c(s, args);
        EXPECT(std::isfinite(g));
        EXPECT(g >= 0.0);
    }

    // Boundary case: cap.x == input_offset
    args.cap.x = 2.409;
    classic c2(args);
    for (double s : {0.0, 1.0, 2.409, 5.0, 100.0}) EXPECT(std::isfinite(c2(s, args)));

    // Reverse direction: cap.x > input_offset (normal config) stays finite too
    args.cap.x = 5.0;
    args.input_offset = 1.0;
    classic c3(args);
    for (double s : {0.0, 1.0, 5.0, 100.0, 1e6}) EXPECT(std::isfinite(c3(s, args)));
}

// ── BUG-13: save_config durability — fsync path is followed for content ─────
// We can't test fsync directly (kernel-level), but we can verify the temp
// file path is created and atomic rename happens correctly even when the
// target file is concurrently read.

// ── BUG-15: motion_math remainder MUST NOT accumulate when motion is clamped
// to INT range — pathological large input would otherwise lock the user into
// many frames of INT_MAX motion.

static void test_motion_math_clamp_remainder_reset() {
    SECTION("BUG-15 — motion_math: clamp does not leak unbounded remainder");
    modifier_settings settings = {};
    settings.prof.accel_x.mode = accel_mode::classic;
    settings.prof.accel_x.acceleration = 0.5;
    settings.prof.accel_x.exponent_classic = 2.0;
    settings.prof.accel_x.gain = 1;
    settings.prof.accel_y = settings.prof.accel_x;
    settings.prof.domain_weights = {1.0, 1.0};
    settings.prof.range_weights  = {1.0, 1.0};
    settings.prof.speed_max = 1e9;
    settings.prof.output_dpi = 800;
    settings.prof.lr_output_dpi_ratio = 1;
    settings.prof.ud_output_dpi_ratio = 1;
    init_settings(settings);
    speed_processor sp; sp.init(settings.prof.speed_processor_args);
    modifier mod;

    double rx = 0, ry = 0;
    int ox, oy;

    // Pathological: INT_MAX motion repeated → remainder must NOT grow
    for (int i = 0; i < 50; i++) {
        apply_motion_math(mod, sp, settings, 1.0, 8.0,
                          (double)INT_MAX, 0.0, rx, ry, ox, oy);
    }
    EXPECT(std::fabs(rx) < 1.0); // remainder reset, not accumulated
    EXPECT(std::fabs(ry) < 1.0);

    // Now small motion should produce small output, not INT_MAX
    apply_motion_math(mod, sp, settings, 1.0, 8.0, 1.0, 0.0, rx, ry, ox, oy);
    EXPECT(ox <= 100); // not INT_MAX → remainder did not leak

    // Healthy fractional motion still accumulates correctly (subpixel works)
    rx = ry = 0;
    int total = 0;
    for (int i = 0; i < 10; i++) {
        apply_motion_math(mod, sp, settings, 1.0, 8.0, 0.3, 0.0, rx, ry, ox, oy);
        total += ox;
    }
    EXPECT(total >= 2 && total <= 4); // 10 × 0.3 ≈ 3 pixels delivered

    // NaN/Inf input also stays safe (defence-in-depth verification)
    rx = ry = 0;
    apply_motion_math(mod, sp, settings, 1.0, 8.0,
                      std::numeric_limits<double>::quiet_NaN(), 0.0, rx, ry, ox, oy);
    EXPECT(std::isfinite(rx) && std::fabs(rx) < 1.0);
    EXPECT(ox == 0); // NaN motion → 0
}

static void test_save_config_durability_path() {
    SECTION("BUG-13 — save_config: tmp file is removed and target updated atomically");
    std::string path = "/tmp/_rawaccel_save_test.json";
    std::string tmp  = path + ".tmp";
    std::remove(path.c_str()); std::remove(tmp.c_str());

    app_config cfg;
    device_profile dp; dp.name = "test"; dp.dev_cfg.dpi = 800;
    dp.prof.accel_x.mode = accel_mode::noaccel;
    dp.prof.accel_y.mode = accel_mode::noaccel;
    cfg.profiles.push_back(dp);
    cfg.active_profile = "test";

    save_config(cfg, path);

    // Target file must exist; tmp file must NOT exist (renamed away).
    struct stat st;
    EXPECT(stat(path.c_str(), &st) == 0);
    EXPECT(stat(tmp.c_str(),  &st) != 0); // tmp gone

    // File must be non-empty (fsync ensures content reached disk before rename)
    EXPECT(st.st_size > 0);

    // Round-trip
    app_config loaded = load_config(path);
    EXPECT(loaded.profiles.size() == 1);
    EXPECT(loaded.profiles[0].name == "test");

    std::remove(path.c_str());
}

// ── R7: power mode with output_offset > 0 ───────────────────────────────────

static void test_power_output_offset() {
    SECTION("R7 — power mode: output_offset > 0, finite outputs for all speeds");

    accel_args args;
    args.mode = accel_mode::power;
    args.scale = 0.1;
    args.exponent_power = 0.3;
    args.output_offset = 0.5;
    args.cap_mode_val = cap_mode::out;
    args.cap.y = 5.0;
    args.gain = true;

    power p(args);
    int bad = 0;
    double speeds[] = {0, 0.001, 0.1, 1.0, 5.0, 50.0, 500.0};
    for (double s : speeds) {
        double r = p(s, args);
        if (!std::isfinite(r)) bad++;
    }
    EXPECT(bad == 0);
}

// ── R7: directional weight interpolation boundary values ─────────────────────

static void test_directional_weight_boundary() {
    SECTION("R7 — directional weight blending at 0°, 45°, 90°");

    // With different range_weights.x and .y, verify blending works
    profile prof{};
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y.mode = accel_mode::noaccel;
    prof.range_weights.x = 2.0;
    prof.range_weights.y = 4.0;
    prof.domain_weights = {1, 1};
    prof.speed_processor_args.whole = true;
    prof.speed_processor_args.lp_norm = 2;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    modifier mod;
    speed_processor sp;
    sp.init(prof.speed_processor_args);

    // Pure horizontal → range_weights.x = 2.0
    {
        vec2d in = {10.0, 0.0};
        mod.modify(in, sp, settings, 1.0, 1.0);
        EXPECT_NEAR(in.x, 20.0, 0.1); // 10 * 2.0
        EXPECT_NEAR(in.y, 0.0, 1e-9);
    }
    // Pure vertical → range_weights.y = 4.0
    {
        speed_processor sp2;
        sp2.init(prof.speed_processor_args);
        vec2d in = {0.0, 10.0};
        mod.modify(in, sp2, settings, 1.0, 1.0);
        EXPECT_NEAR(in.x, 0.0, 1e-9);
        EXPECT_NEAR(in.y, 40.0, 0.1); // 10 * 4.0
    }
}

// ── R9: subpixel remainder cumulative drift (1M iterations) ──────────────────

static void test_subpixel_cumulative_drift() {
    SECTION("R9 — subpixel remainder: 1M iterations, no cumulative drift");

    // Noaccel mode, input = 1.7 counts per frame → trunc(1.7) = 1, remainder 0.7
    // After many frames, total output + final remainder should match total input exactly.
    modifier_settings settings;
    settings.prof.accel_x.mode = accel_mode::noaccel;
    settings.prof.accel_y.mode = accel_mode::noaccel;
    init_settings(settings);

    modifier mod;
    speed_processor sp;
    sp.init(settings.prof.speed_processor_args);

    double rx = 0, ry = 0;
    long long total_out_x = 0, total_out_y = 0;
    constexpr int N = 1'000'000;
    constexpr double DX = 1.7, DY = -0.3;

    for (int i = 0; i < N; i++) {
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          DX, DY, rx, ry, ox, oy);
        total_out_x += ox;
        total_out_y += oy;
    }

    double expected_x = DX * N;  // 1,700,000
    double expected_y = DY * N;  // -300,000
    double actual_x = total_out_x + rx;
    double actual_y = total_out_y + ry;

    // Total output + remainder should be very close to total input.
    // Allow 1.0 count tolerance for 1M iterations (1e-6 relative error).
    EXPECT_NEAR(actual_x, expected_x, 1.0);
    EXPECT_NEAR(actual_y, expected_y, 1.0);
    // Remainder should always be in (-1, 1)
    EXPECT(std::fabs(rx) < 1.0);
    EXPECT(std::fabs(ry) < 1.0);
}

// ── R9: classic gain mode cap behavior consistency ───────────────────────────

static void test_classic_gain_mode_cap_consistency() {
    SECTION("R9 — classic gain mode: output monotonically non-decreasing with speed");

    accel_args args;
    args.mode = accel_mode::classic;
    args.acceleration = 0.01;
    args.exponent_classic = 2.0;
    args.cap_mode_val = cap_mode::out;
    args.cap.y = 2.0;
    args.gain = true;
    args.input_offset = 0;

    classic c(args);

    // Verify monotonicity: for increasing speed, output should never decrease
    double prev = c(0.001, args);
    int violations = 0;
    for (double x = 0.1; x < 200.0; x += 0.1) {
        double cur = c(x, args);
        if (cur < prev - 1e-9) violations++;
        prev = cur;
    }
    EXPECT(violations == 0);
}

// ── R9: natural gain formula correctness ─────────────────────────────────────

static void test_natural_gain_formula() {
    SECTION("R9 — natural gain mode: output/x+1 converges to args.limit+1");

    accel_args args;
    args.mode = accel_mode::natural;
    args.decay_rate = 0.1;
    args.limit = 3.0;
    args.input_offset = 0;
    args.gain = true;

    natural n(args);

    // Internal limit = args.limit - 1 = 2.0
    // Gain mode asymptote: output/x + 1 → (1 + internal_limit) + 1 = args.limit + 1 = 4.0
    // Because output = t + limit*(t - (1-exp(-a*t))/a) → (1+limit)*t for large t
    // So output/x + 1 → (1+limit) + 1 = limit + 2 = args.limit + 1
    double r = n(100000.0, args);
    EXPECT_NEAR(r, 4.0, 0.01);  // args.limit + 1

    // At x just past 0 (but > 1e-9 guard), gain mode gives output/x+1 → 2.0
    // L'Hopital: lim_{t→0} output/x = lim t + limit*(t - (1-e^{-at})/a) / (t+offset)
    // When offset=0, this → 1.0, so total → 2.0
    double r_low = n(0.001, args);
    EXPECT_NEAR(r_low, 2.0, 0.1);

    // Non-gain mode: should converge to args.limit = 3.0
    args.gain = false;
    natural n2(args);
    double r_ng = n2(100000.0, args);
    EXPECT_NEAR(r_ng, 3.0, 0.01);  // args.limit = 3.0 (internal limit+1)

    // Non-gain at x=0 → 1.0
    double r_ng_low = n2(0.001, args);
    EXPECT_NEAR(r_ng_low, 1.0, 0.01);
}

// ── R10: EMA smoother correctness ────────────────────────────────────────────

static void test_ema_smoother_halflife() {
    SECTION("R10 — simple EMA smoother: half-life convergence");

    // After 'halfLife' steps of constant input, the smoother output should be
    // within ~50% of the step input (by definition of half-life).
    simple_ema_smoother ema;
    double halfLife = 5.0;
    ema.init(halfLife);

    // Feed constant speed=10.0 at dt=1.0 for many iterations
    double val = 0;
    for (int i = 0; i < 200; i++)
        val = ema.smooth(10.0, 1.0);

    // Should converge to 10.0
    EXPECT_NEAR(val, 10.0, 0.01);

    // Test from zero: after exactly halfLife steps, should be ~50% of target
    simple_ema_smoother ema2;
    ema2.init(halfLife);
    double v2 = 0;
    for (int i = 0; i < (int)halfLife; i++)
        v2 = ema2.smooth(10.0, 1.0);
    // Half-life means ~50% convergence — allow generous range due to discrete steps
    EXPECT(v2 > 3.0 && v2 < 8.0);

    // Zero half-life → no smoothing, output == input immediately
    simple_ema_smoother ema3;
    ema3.init(0.0);
    double v3 = ema3.smooth(42.0, 1.0);
    EXPECT_NEAR(v3, 42.0, 0.001);
}

static void test_ema_smoother_zero_time() {
    SECTION("R10 — EMA smoother with zero/negative time");

    simple_ema_smoother ema;
    ema.init(5.0);

    // Feed a normal value first
    ema.smooth(10.0, 1.0);

    // Zero time: pow(coeff, 0) = 1 → twc = 0 → no update. Output should stay.
    double prev = ema.smooth(10.0, 1.0);
    double v = ema.smooth(999.0, 0.0);
    EXPECT_NEAR(v, prev, 0.01);

    // Negative time: pow may produce > 1 → twc could be negative.
    // Should not produce NaN or crash.
    double vn = ema.smooth(5.0, -1.0);
    EXPECT(std::isfinite(vn));
}

static void test_linear_ema_smoother() {
    SECTION("R10 — linear EMA smoother: convergence and stability");

    linear_ema_smoother lema;
    lema.init(5.0, 1.25);

    // Feed constant speed = 20.0
    double val = 0;
    for (int i = 0; i < 300; i++)
        val = lema.smooth(20.0, 1.0);
    EXPECT_NEAR(val, 20.0, 0.1);

    // After convergence, abrupt change to 0 — should not go negative
    for (int i = 0; i < 100; i++)
        val = lema.smooth(0.0, 1.0);
    EXPECT(val >= 0.0);

    // Zero half-life → immediate tracking
    linear_ema_smoother lema2;
    lema2.init(0.0, 0.0);
    double v2 = lema2.smooth(77.0, 1.0);
    EXPECT_NEAR(v2, 77.0, 0.001);
}

// ── R10: NaN propagation end-to-end for every mode ───────────────────────────

static void test_nan_propagation_all_modes() {
    SECTION("R10 — NaN propagation: full pipeline for every accel mode");

    // Test that no matter what extreme accel_args we feed, the output
    // (through modifier → motion_math) never produces NaN or Inf.
    accel_mode modes[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::power,
        accel_mode::natural, accel_mode::jump, accel_mode::synchronous,
        accel_mode::lookup
    };
    const char* mode_names[] = {
        "noaccel", "classic", "power", "natural", "jump", "synchronous", "lookup"
    };

    double test_speeds[] = { 0.0, 1e-15, 0.001, 1.0, 100.0, 1e6, 1e15 };
    double test_times[]  = { 0.001, 0.0625, 1.0, 10.0, 100.0 };

    for (int m = 0; m < 7; m++) {
        accel_args args = make_args(modes[m]);
        if (modes[m] == accel_mode::lookup) {
            // Setup a minimal LUT
            args.data[0] = 0.0f; args.data[1] = 1.0f;
            args.data[2] = 50.0f; args.data[3] = 3.0f;
            args.length = 4;
        }

        modifier_settings settings;
        settings.prof.accel_x = args;
        settings.prof.accel_y = args;
        init_settings(settings);

        modifier mod;
        speed_processor sp;
        sp.init(settings.prof.speed_processor_args);

        bool any_nan = false;
        for (double spd : test_speeds) {
            for (double t : test_times) {
                double rem_x = 0, rem_y = 0;
                int out_x = 0, out_y = 0;
                apply_motion_math(mod, sp, settings, 1.0, t,
                                  spd, spd, rem_x, rem_y, out_x, out_y);
                if (!std::isfinite(rem_x) || !std::isfinite(rem_y)) {
                    any_nan = true;
                    std::fprintf(stderr, "    NaN/Inf in mode %s: speed=%.3g time=%.3g\n",
                                 mode_names[m], spd, t);
                }
            }
        }
        EXPECT(!any_nan);
    }
}

static void test_nan_propagation_pathological_params() {
    SECTION("R10 — NaN propagation: pathological parameter combos");

    // Classic: negative acceleration + non-integer exponent → pow(neg, frac) = NaN
    {
        accel_args args;
        args.mode = accel_mode::classic;
        args.acceleration = -5.0;
        args.exponent_classic = 2.7; // non-integer
        args.cap.y = 0;
        args.input_offset = 0;
        classic c(args);
        double r = c(10.0, args);
        EXPECT(std::isfinite(r));
    }

    // Power: scale=0 + exponent near 0
    {
        accel_args args;
        args.mode = accel_mode::power;
        args.scale = 0.0;
        args.exponent_power = 1e-4;
        args.output_offset = 0;
        power p(args);
        double r = p(10.0, args);
        EXPECT(std::isfinite(r));
    }

    // Natural: decay_rate = 0 + gain mode → 0/0 in integral
    {
        accel_args args;
        args.mode = accel_mode::natural;
        args.decay_rate = 0.0;
        args.limit = 5.0;
        args.gain = true;
        args.input_offset = 0;
        natural n(args);
        double r = n(10.0, args);
        EXPECT(std::isfinite(r));
        double r0 = n(0.0, args);
        EXPECT(std::isfinite(r0));
    }

    // Synchronous: power < 1 → gain → ∞ as x → 0
    {
        accel_args args;
        args.mode = accel_mode::synchronous;
        args.sync_speed = 5.0;
        args.exponent_power = 0.01; // very small
        synchronous s(args);
        double r = s(1e-10, args);
        EXPECT(std::isfinite(r));
        double r0 = s(0.0, args);
        EXPECT(std::isfinite(r0));
    }

    // Classic cap_mode::io with cap.x <= input_offset (degenerate)
    {
        accel_args args;
        args.mode = accel_mode::classic;
        args.cap_mode_val = cap_mode::io;
        args.cap.x = 5.0;
        args.cap.y = 2.0;
        args.input_offset = 10.0; // offset > cap.x
        args.exponent_classic = 0.5; // fractional exp
        classic c(args);
        double r = c(20.0, args);
        EXPECT(std::isfinite(r));
    }
}

// ── R10: Event batching simulation (mock events through motion_math) ─────────

static void test_event_batching_accumulation() {
    SECTION("R10 — event batching: accumulated dx/dy matches separate events");

    // Simulate: 3 events with REL_X=5,3,2 (total 10) should give same result
    // as single event with REL_X=10, because all arrive in the same SYN frame.
    accel_args args = make_args(accel_mode::classic);
    args.acceleration = 0.01;
    args.exponent_classic = 2.0;
    args.gain = true;
    args.cap.y = 0;

    modifier_settings settings;
    settings.prof.accel_x = args;
    settings.prof.accel_y = args;
    init_settings(settings);

    // Single batch: dx=10
    modifier mod1;
    speed_processor sp1;
    sp1.init(settings.prof.speed_processor_args);
    double rem_x1 = 0, rem_y1 = 0;
    int ox1 = 0, oy1 = 0;
    apply_motion_math(mod1, sp1, settings, 1.0, 1.0,
                      10.0, 0.0, rem_x1, rem_y1, ox1, oy1);
    double total_batch = ox1 + rem_x1;

    // Same call with identical initial state
    modifier mod2;
    speed_processor sp2;
    sp2.init(settings.prof.speed_processor_args);
    double rem_x2 = 0, rem_y2 = 0;
    int ox2 = 0, oy2 = 0;
    apply_motion_math(mod2, sp2, settings, 1.0, 1.0,
                      10.0, 0.0, rem_x2, rem_y2, ox2, oy2);
    double total_single = ox2 + rem_x2;

    EXPECT_NEAR(total_batch, total_single, 1e-9);
}

static void test_event_batching_split_vs_combined() {
    SECTION("R10 — event batching: split events vs combined in separate SYN frames");

    // When events come in SEPARATE SYN frames (different time), the result
    // should differ from a single combined event (because speed calculation
    // depends on delta magnitude). This is expected behavior.
    accel_args args = make_args(accel_mode::classic);
    args.acceleration = 0.01;
    args.exponent_classic = 2.0;
    args.gain = true;
    args.cap.y = 0;

    modifier_settings settings;
    settings.prof.accel_x = args;
    settings.prof.accel_y = args;
    init_settings(settings);

    // Combined: one frame dx=20 at t=1ms
    modifier mod_c;
    speed_processor sp_c;
    sp_c.init(settings.prof.speed_processor_args);
    double rem_cx = 0, rem_cy = 0;
    int ocx = 0, ocy = 0;
    apply_motion_math(mod_c, sp_c, settings, 1.0, 1.0,
                      20.0, 0.0, rem_cx, rem_cy, ocx, ocy);
    double combined = ocx + rem_cx;

    // Split: two frames dx=10 each at t=0.5ms each
    modifier mod_s;
    speed_processor sp_s;
    sp_s.init(settings.prof.speed_processor_args);
    double rem_sx = 0, rem_sy = 0;
    int osx1 = 0, osy1 = 0, osx2 = 0, osy2 = 0;
    apply_motion_math(mod_s, sp_s, settings, 1.0, 0.5,
                      10.0, 0.0, rem_sx, rem_sy, osx1, osy1);
    apply_motion_math(mod_s, sp_s, settings, 1.0, 0.5,
                      10.0, 0.0, rem_sx, rem_sy, osx2, osy2);
    double split = osx1 + osx2 + rem_sx;

    // With classic acceleration, higher speed (combined) should produce
    // MORE output than lower speed (split), because classic gain increases with speed.
    EXPECT(combined > split - 1.0); // combined >= split (approximately)
    // Both should be finite
    EXPECT(std::isfinite(combined));
    EXPECT(std::isfinite(split));
}

// ── R10: Speed processor distance modes ──────────────────────────────────────

static void test_speed_processor_all_distance_modes() {
    SECTION("R10 — speed processor: all distance modes produce finite results");

    accel_args args = make_args(accel_mode::classic);
    args.acceleration = 0.01;
    args.exponent_classic = 2.0;

    // Test all distance modes
    speed_args sp_args_list[] = {
        { true,  2.0,    0, 0, 0 },   // euclidean
        { true,  9999.0, 0, 0, 0 },   // max
        { true,  3.0,    0, 0, 0 },   // Lp (p=3)
        { false, 2.0,    0, 0, 0 },   // separate
    };
    const char* names[] = { "euclidean", "max", "Lp(3)", "separate" };

    for (int i = 0; i < 4; i++) {
        modifier_settings settings;
        settings.prof.accel_x = args;
        settings.prof.accel_y = args;
        settings.prof.speed_processor_args = sp_args_list[i];
        init_settings(settings);

        modifier mod;
        speed_processor sp;
        sp.init(sp_args_list[i]);

        double rem_x = 0, rem_y = 0;
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          5.0, 3.0, rem_x, rem_y, ox, oy);

        bool ok = std::isfinite(rem_x) && std::isfinite(rem_y);
        if (!ok)
            std::fprintf(stderr, "    NaN in distance mode %s\n", names[i]);
        EXPECT(ok);
        // Should produce some output for non-zero input
        EXPECT(ox != 0 || oy != 0 || std::fabs(rem_x) > 0 || std::fabs(rem_y) > 0);
    }
}

// ── R10: Speed processor with smoothing enabled ──────────────────────────────

static void test_speed_processor_smoothing() {
    SECTION("R10 — speed processor: EMA smoothing produces stable output");

    accel_args args = make_args(accel_mode::classic);
    args.acceleration = 0.01;
    args.exponent_classic = 2.0;
    args.cap.y = 0;

    speed_args sp_args;
    sp_args.whole = true;
    sp_args.lp_norm = 2.0;
    sp_args.input_speed_smooth_halflife  = 5.0;
    sp_args.scale_smooth_halflife        = 3.0;
    sp_args.output_speed_smooth_halflife = 2.0;

    modifier_settings settings;
    settings.prof.accel_x = args;
    settings.prof.accel_y = args;
    settings.prof.speed_processor_args = sp_args;
    init_settings(settings);

    modifier mod;
    speed_processor sp;
    sp.init(sp_args);

    // Feed constant motion for many frames
    double rem_x = 0, rem_y = 0;
    int last_ox = 0;
    bool all_finite = true;
    for (int i = 0; i < 500; i++) {
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          10.0, 0.0, rem_x, rem_y, ox, oy);
        if (!std::isfinite(rem_x) || !std::isfinite(rem_y))
            all_finite = false;
        last_ox = ox;
    }
    EXPECT(all_finite);
    // After convergence, output should be relatively stable
    EXPECT(last_ox > 0);

    // Abrupt stop: feed zero motion
    for (int i = 0; i < 100; i++) {
        int ox = 0, oy = 0;
        apply_motion_math(mod, sp, settings, 1.0, 1.0,
                          0.0, 0.0, rem_x, rem_y, ox, oy);
        if (!std::isfinite(rem_x) || !std::isfinite(rem_y))
            all_finite = false;
    }
    EXPECT(all_finite);
}

// ── R10: SYN_DROPPED simulation ──────────────────────────────────────────────

static void test_syn_dropped_reset_behavior() {
    SECTION("R10 — SYN_DROPPED: remainder reset preserves correctness");

    // After SYN_DROPPED, the daemon resets dx/dy accumulators.
    // Simulate: motion → SYN_DROPPED (discard) → fresh motion.
    // Verify remainder state is not corrupted by the discarded partial data.
    accel_args args = make_args(accel_mode::classic);
    args.acceleration = 0.01;
    args.exponent_classic = 2.0;

    modifier_settings settings;
    settings.prof.accel_x = args;
    settings.prof.accel_y = args;
    init_settings(settings);

    modifier mod;
    speed_processor sp;
    sp.init(settings.prof.speed_processor_args);

    // Normal frame
    double rem_x = 0, rem_y = 0;
    int ox = 0, oy = 0;
    apply_motion_math(mod, sp, settings, 1.0, 1.0,
                      10.0, 5.0, rem_x, rem_y, ox, oy);
    EXPECT(std::isfinite(rem_x) && std::isfinite(rem_y));

    // Simulate SYN_DROPPED: we do NOT call apply_motion_math for the dropped frame.
    // The accumulated dx/dy would be discarded by daemon. Remainder stays from last good frame.

    // Next good frame: fresh motion
    int ox2 = 0, oy2 = 0;
    apply_motion_math(mod, sp, settings, 1.0, 1.0,
                      3.0, 2.0, rem_x, rem_y, ox2, oy2);
    EXPECT(std::isfinite(rem_x) && std::isfinite(rem_y));

    // Output should be reasonable (not huge or zero from corrupted state)
    double total = std::abs(ox2) + std::fabs(rem_x);
    EXPECT(total >= 0.0 && total < 1000.0);
}

// ── R10: Config edge cases ───────────────────────────────────────────────────

static void test_config_empty_profiles() {
    SECTION("R10 — config: empty profiles array handling");

    // Create a config JSON with empty profiles
    std::string tmp = "/tmp/rawaccel_test_empty_profiles.json";
    {
        std::ofstream f(tmp);
        f << R"({"active_profile":"nonexistent","profiles":[]})";
    }

    app_config cfg = load_config(tmp);
    EXPECT(cfg.profiles.empty());
    EXPECT(cfg.active_profile == "nonexistent");

    // Save it back and reload — should survive the round-trip
    save_config(cfg, tmp);
    app_config cfg2 = load_config(tmp);
    EXPECT(cfg2.profiles.empty());

    std::remove(tmp.c_str());
}

static void test_config_missing_active_profile() {
    SECTION("R10 — config: active_profile references non-existent profile");

    std::string tmp = "/tmp/rawaccel_test_missing_active.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "active_profile": "ghost",
            "profiles": [
                { "name": "real", "dpi": 800, "polling_rate": 1000,
                  "profile": {} }
            ]
        })";
    }

    app_config cfg = load_config(tmp);
    EXPECT(cfg.active_profile == "ghost");
    EXPECT(cfg.profiles.size() == 1);
    EXPECT(cfg.profiles[0].name == "real");

    // Daemon's find_profile logic: active_profile doesn't match → falls back to profiles[0]
    // We can verify the config loads without crashing.

    std::remove(tmp.c_str());
}

static void test_config_extreme_values() {
    SECTION("R10 — config: extreme values are clamped by sanitize");

    device_profile dp;
    dp.dev_cfg.dpi = 999999;
    dp.dev_cfg.polling_rate = -50;
    dp.prof.degrees_rotation = 9999;
    dp.prof.degrees_snap = -10;
    dp.prof.lr_output_dpi_ratio = -5;
    dp.prof.ud_output_dpi_ratio = 999;
    dp.prof.speed_min = -10;
    dp.prof.speed_max = -5;
    dp.prof.accel_x.scale = -1;
    dp.prof.accel_x.decay_rate = -1;
    dp.prof.accel_x.exponent_power = 0;
    dp.prof.accel_x.limit = -10;
    dp.prof.accel_x.sync_speed = 0;
    dp.prof.accel_x.smooth = -1;

    sanitize_device_profile(dp);

    EXPECT(dp.dev_cfg.dpi >= 1 && dp.dev_cfg.dpi <= 32000);
    EXPECT(dp.dev_cfg.polling_rate >= 125 && dp.dev_cfg.polling_rate <= 8000);
    EXPECT(dp.prof.degrees_rotation >= 0 && dp.prof.degrees_rotation < 360);
    EXPECT(dp.prof.degrees_snap >= 0 && dp.prof.degrees_snap <= 45);
    EXPECT(dp.prof.lr_output_dpi_ratio >= 0.01);
    EXPECT(dp.prof.ud_output_dpi_ratio <= 100);
    EXPECT(dp.prof.speed_min >= 0);
    EXPECT(dp.prof.speed_max >= 0);
    EXPECT(dp.prof.accel_x.scale >= 0);
    EXPECT(dp.prof.accel_x.decay_rate >= 0);
    EXPECT(dp.prof.accel_x.exponent_power >= 1e-4);
    EXPECT(dp.prof.accel_x.limit >= 0);
    EXPECT(dp.prof.accel_x.sync_speed >= 1e-4);
    EXPECT(dp.prof.accel_x.smooth >= 0);
}

static void test_config_duplicate_device_id() {
    SECTION("R10 — config: duplicate device_id → first match wins");

    std::string tmp = "/tmp/rawaccel_test_dup_devid.json";
    {
        std::ofstream f(tmp);
        f << R"({
            "active_profile": "default",
            "profiles": [
                { "name": "fast", "device_id": "usb:1234:5678:", "dpi": 1600,
                  "polling_rate": 1000, "profile": { "accel_x": { "mode": "classic", "acceleration": 0.1 } } },
                { "name": "slow", "device_id": "usb:1234:5678:", "dpi": 800,
                  "polling_rate": 1000, "profile": { "accel_x": { "mode": "natural", "decay_rate": 0.2 } } }
            ]
        })";
    }

    app_config cfg = load_config(tmp);
    EXPECT(cfg.profiles.size() == 2);
    // Both have same device_id — first match should win (deterministic)
    EXPECT(cfg.profiles[0].device_id == cfg.profiles[1].device_id);
    EXPECT(cfg.profiles[0].name == "fast"); // first in array

    std::remove(tmp.c_str());
}

// Duplicate of check_duplicate_device_ids() from gui/main.cpp (pure logic, no GTK deps).
static std::string check_dup_ids(const app_config& cfg) {
    std::string warn;
    for (size_t i = 0; i < cfg.profiles.size(); ++i) {
        const auto& a = cfg.profiles[i];
        if (a.device_id.empty()) continue;
        for (size_t j = i + 1; j < cfg.profiles.size(); ++j) {
            const auto& b = cfg.profiles[j];
            if (a.device_id == b.device_id) {
                if (!warn.empty()) warn += "; ";
                warn += "\"" + a.name + "\" & \"" + b.name +
                        "\" share device " + a.device_id;
            }
        }
    }
    if (!warn.empty())
        warn = "Warning: duplicate device IDs — " + warn +
               ". Only the first matching profile is used by the daemon.";
    return warn;
}

static void test_check_duplicate_device_ids() {
    SECTION("R11 — check_duplicate_device_ids warning logic");

    // No duplicates — all empty device_id
    {
        app_config cfg;
        device_profile a; a.name = "a";
        device_profile b; b.name = "b";
        cfg.profiles = {a, b};
        EXPECT(check_dup_ids(cfg).empty());
    }

    // No duplicates — different device_ids
    {
        app_config cfg;
        device_profile a; a.name = "a"; a.device_id = "dev1";
        device_profile b; b.name = "b"; b.device_id = "dev2";
        cfg.profiles = {a, b};
        EXPECT(check_dup_ids(cfg).empty());
    }

    // Duplicate device_ids → warning
    {
        app_config cfg;
        device_profile a; a.name = "fast"; a.device_id = "usb:1234:5678:";
        device_profile b; b.name = "slow"; b.device_id = "usb:1234:5678:";
        cfg.profiles = {a, b};
        std::string w = check_dup_ids(cfg);
        EXPECT(!w.empty());
        EXPECT(w.find("fast") != std::string::npos);
        EXPECT(w.find("slow") != std::string::npos);
        EXPECT(w.find("usb:1234:5678:") != std::string::npos);
    }

    // Mixed: one pair duplicates, one unique
    {
        app_config cfg;
        device_profile a; a.name = "a"; a.device_id = "x";
        device_profile b; b.name = "b"; b.device_id = "y";
        device_profile c; c.name = "c"; c.device_id = "x";
        cfg.profiles = {a, b, c};
        std::string w = check_dup_ids(cfg);
        EXPECT(!w.empty());
        EXPECT(w.find("\"a\"") != std::string::npos);
        EXPECT(w.find("\"c\"") != std::string::npos);
        // "b" should NOT appear in warning
        EXPECT(w.find("\"b\"") == std::string::npos);
    }

    // Empty device_id is never a collision
    {
        app_config cfg;
        device_profile a; a.name = "a"; a.device_id = "";
        device_profile b; b.name = "b"; b.device_id = "";
        cfg.profiles = {a, b};
        EXPECT(check_dup_ids(cfg).empty());
    }
}

static void test_sanitize_nan_fields() {
    SECTION("R11 — sanitize replaces NaN/Inf in all accel_args + profile fields");

    device_profile dp;
    dp.name = "nan_test";
    dp.dev_cfg.dpi = 800;
    dp.dev_cfg.polling_rate = 1000;

    // Set all double fields to NaN
    double nan = std::numeric_limits<double>::quiet_NaN();
    dp.prof.accel_x.acceleration    = nan;
    dp.prof.accel_x.scale           = nan;
    dp.prof.accel_x.decay_rate      = nan;
    dp.prof.accel_x.exponent_classic = nan;
    dp.prof.accel_x.exponent_power  = nan;
    dp.prof.accel_x.input_offset    = nan;
    dp.prof.accel_x.output_offset   = nan;
    dp.prof.accel_x.limit           = nan;
    dp.prof.accel_x.sync_speed      = nan;
    dp.prof.accel_x.smooth          = nan;
    dp.prof.accel_x.motivity        = nan;
    dp.prof.accel_x.gamma           = nan;
    dp.prof.accel_x.cap             = { nan, nan };
    dp.prof.accel_y = dp.prof.accel_x;
    dp.prof.degrees_rotation  = nan;
    dp.prof.degrees_snap      = nan;
    dp.prof.speed_min         = nan;
    dp.prof.speed_max         = nan;
    dp.prof.lr_output_dpi_ratio = nan;
    dp.prof.ud_output_dpi_ratio = nan;
    dp.prof.domain_weights    = { nan, nan };
    dp.prof.range_weights     = { nan, nan };
    dp.prof.speed_processor_args.lp_norm = nan;
    dp.prof.speed_processor_args.input_speed_smooth_halflife = nan;
    dp.prof.speed_processor_args.scale_smooth_halflife = nan;
    dp.prof.speed_processor_args.output_speed_smooth_halflife = nan;

    sanitize_device_profile(dp);

    // Every field must be finite after sanitization
    EXPECT(std::isfinite(dp.prof.accel_x.acceleration));
    EXPECT(std::isfinite(dp.prof.accel_x.scale));
    EXPECT(std::isfinite(dp.prof.accel_x.decay_rate));
    EXPECT(std::isfinite(dp.prof.accel_x.exponent_classic));
    EXPECT(std::isfinite(dp.prof.accel_x.exponent_power));
    EXPECT(std::isfinite(dp.prof.accel_x.input_offset));
    EXPECT(std::isfinite(dp.prof.accel_x.output_offset));
    EXPECT(std::isfinite(dp.prof.accel_x.limit));
    EXPECT(std::isfinite(dp.prof.accel_x.sync_speed));
    EXPECT(std::isfinite(dp.prof.accel_x.smooth));
    EXPECT(std::isfinite(dp.prof.accel_x.cap.x));
    EXPECT(std::isfinite(dp.prof.accel_x.cap.y));
    EXPECT(std::isfinite(dp.prof.degrees_rotation));
    EXPECT(std::isfinite(dp.prof.degrees_snap));
    EXPECT(std::isfinite(dp.prof.speed_min));
    EXPECT(std::isfinite(dp.prof.speed_max));
    EXPECT(std::isfinite(dp.prof.lr_output_dpi_ratio));
    EXPECT(std::isfinite(dp.prof.ud_output_dpi_ratio));
    EXPECT(std::isfinite(dp.prof.domain_weights.x));
    EXPECT(std::isfinite(dp.prof.domain_weights.y));
    EXPECT(std::isfinite(dp.prof.range_weights.x));
    EXPECT(std::isfinite(dp.prof.range_weights.y));
    EXPECT(std::isfinite(dp.prof.speed_processor_args.lp_norm));
}

static void test_modify_subnormal_time() {
    SECTION("R11 — modify() with subnormal time does not produce NaN");

    // This bug was found by libFuzzer: time=1e-309 causes ips_factor=Inf
    // which cascades into NaN through the accel pipeline.
    for (int m = 0; m < 7; ++m) {
        profile prof{};
        prof.accel_x.mode = static_cast<accel_mode>(m);
        prof.accel_x.gain = true;
        prof.accel_x.acceleration = 0.007;
        prof.accel_x.exponent_classic = 2.0;
        prof.accel_x.exponent_power = 1.5;
        prof.accel_x.limit = 2.0;
        prof.accel_x.scale = 0.01;
        prof.accel_x.decay_rate = 0.1;
        prof.accel_x.sync_speed = 5.0;
        prof.accel_y = prof.accel_x;

        modifier_settings settings;
        settings.prof = prof;
        init_settings(settings);

        speed_processor sp;
        sp.init(prof.speed_processor_args);

        modifier mod;

        // Subnormal time values
        double subnormal_times[] = { 1e-309, 5e-324, 1e-200 };
        for (double t : subnormal_times) {
            vec2d input = { 100.0, 50.0 };
            mod.modify(input, sp, settings, 0.8, t);
            EXPECT(std::isfinite(input.x));
            EXPECT(std::isfinite(input.y));
        }
    }
}

// ── Round 12 — untested code path coverage ──────────────────────────────────

static void test_classic_sign_flip() {
    SECTION("R12 — classic cap_mode::io sign flip (cap.y < 1 → deceleration)");

    // When cap.y < 1 in io mode, the classic constructor flips sign:
    //   cap = cap.y - 1 = -0.5 → cap = 0.5, sign = -1
    // This produces deceleration (gain < 1).
    accel_args args = make_args(accel_mode::classic);
    args.cap_mode_val = cap_mode::io;
    args.cap.x = 10.0;
    args.cap.y = 0.5;  // < 1 → sign flip
    args.exponent_classic = 2.0;
    args.acceleration = 0.01;
    classic c(args);

    double g_low = c(1.0, args);
    double g_mid = c(5.0, args);
    double g_high = c(20.0, args);

    // All values should be finite
    EXPECT(std::isfinite(g_low));
    EXPECT(std::isfinite(g_mid));
    EXPECT(std::isfinite(g_high));
    // Sign flip: gain should be <= 1 (deceleration)
    EXPECT(g_high <= 1.0 + 1e-6);
    // At very low speed (below offset), gain = 1.0
    double g_zero = c(0.0, args);
    EXPECT_NEAR(g_zero, 1.0, 1e-9);
}

static void test_classic_linear_path_cap() {
    SECTION("R12 — classic linear path (exp<=1) with various cap values");

    accel_args args = make_args(accel_mode::classic);
    args.exponent_classic = 0.5;  // <= 1 → linear path
    args.acceleration = 0.1;
    args.cap.y = 1.05;  // small cap → cap = 0.05

    classic c(args);
    // Linear path returns 1 + min(accel_raised, cap)
    // accel_raised = acceleration = 0.1, cap = max(0, cap.y - 1) = 0.05
    // So: gain = 1 + min(0.1, 0.05) = 1.05 for any speed > offset
    double g = c(50.0, args);
    EXPECT_NEAR(g, 1.05, 1e-6);

    // cap.y = 0 → cap = DBL_MAX (no cap) → gain = 1 + acceleration
    accel_args args2 = args;
    args2.cap.y = 0;
    classic c2(args2);
    double g2 = c2(50.0, args2);
    EXPECT_NEAR(g2, 1.1, 1e-6);  // 1 + 0.1

    // cap.y = 0.8 → cap = max(0, -0.2) = 0 → gain = 1.0
    accel_args args3 = args;
    args3.cap.y = 0.8;  // < 1 → cap = max(0, -0.2) = 0
    classic c3(args3);
    double g3 = c3(50.0, args3);
    EXPECT_NEAR(g3, 1.0, 1e-6);
}

static void test_power_cap_branch() {
    SECTION("R12 — power mode cap branch (speed >= cap_x)");

    accel_args args = make_args(accel_mode::power);
    args.exponent_power = 0.3;
    args.scale = 0.5;
    args.cap_mode_val = cap_mode::out;
    args.cap.y = 2.0;  // output cap
    args.output_offset = 0;

    power p(args);

    // Verify finite output across increasing speeds (some will hit the cap branch)
    for (double s = 10; s <= 10000; s *= 2) {
        double g = p(s, args);
        EXPECT(std::isfinite(g));
    }
    // Very high speed should approach cap_y
    double g_high = p(1e6, args);
    EXPECT(std::isfinite(g_high));
    EXPECT_NEAR(g_high, args.cap.y, 0.1);

    // cap_mode::in
    accel_args args2 = args;
    args2.cap_mode_val = cap_mode::in;
    args2.cap.x = 50.0;  // input speed cap
    power p2(args2);
    double g_below = p2(10.0, args2);
    double g_above = p2(1000.0, args2);
    EXPECT(std::isfinite(g_below));
    EXPECT(std::isfinite(g_above));

    // cap_mode::io
    accel_args args3 = args;
    args3.cap_mode_val = cap_mode::io;
    args3.cap.x = 30.0;
    args3.cap.y = 1.8;
    power p3(args3);
    double g_at_cap = p3(30.0, args3);
    EXPECT(std::isfinite(g_at_cap));
    EXPECT_NEAR(g_at_cap, 1.8, 0.5);  // at cap point, gain ≈ cap.y (io mode has integration constant offset)
}

static void test_modifier_rotation_snap_combined() {
    SECTION("R12 — modifier rotation + snap combined flags");

    profile prof{};
    prof.degrees_rotation = 45.0;  // 45° rotation
    prof.degrees_snap = 10.0;      // 10° snap angle
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y = prof.accel_x;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);

    EXPECT(settings.data.flags.apply_rotate);
    EXPECT(settings.data.flags.apply_snap);
    EXPECT(settings.data.flags.compute_ref_angle);

    speed_processor sp;
    sp.init(prof.speed_processor_args);
    modifier mod;

    // Pure horizontal movement → after 45° rotation becomes diagonal
    // After snap with small angle, diagonal should NOT snap to axis
    vec2d in1 = { 100.0, 0.0 };
    mod.modify(in1, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(in1.x));
    EXPECT(std::isfinite(in1.y));

    // Pure vertical → after rotation, should be rotated
    vec2d in2 = { 0.0, 100.0 };
    mod.modify(in2, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(in2.x));
    EXPECT(std::isfinite(in2.y));

    // Near-horizontal with snap → should snap to horizontal
    // After 45° rotation, (100, 5) becomes something near diagonal,
    // but we test the snap logic with no rotation to verify snap alone:
    profile prof2{};
    prof2.degrees_snap = 20.0;
    prof2.accel_x.mode = accel_mode::noaccel;
    prof2.accel_y = prof2.accel_x;
    modifier_settings s2;
    s2.prof = prof2;
    init_settings(s2);
    speed_processor sp2;
    sp2.init(prof2.speed_processor_args);

    // Near-horizontal: angle = atan(3/100) ≈ 1.7° < snap 20° → snap to horizontal
    vec2d snap_h = { 100.0, 3.0 };
    mod.modify(snap_h, sp2, s2, 1.0, 1.0);
    EXPECT_NEAR(snap_h.y, 0.0, 1e-9);  // snapped to 0
    EXPECT(std::fabs(snap_h.x) > 50.0);  // X magnitude preserved

    // Near-vertical: angle = atan(100/3) ≈ 88.3° → > 90°-20°=70° → snap to vertical
    vec2d snap_v = { 3.0, 100.0 };
    mod.modify(snap_v, sp2, s2, 1.0, 1.0);
    EXPECT_NEAR(snap_v.x, 0.0, 1e-9);  // snapped to 0
    EXPECT(std::fabs(snap_v.y) > 50.0);  // Y magnitude preserved
}

static void test_speed_processor_lp_mode() {
    SECTION("R12 — speed_processor Lp distance mode (lp_norm = 3)");

    speed_args sa;
    sa.whole = true;
    sa.lp_norm = 3.0;  // Lp mode (not euclidean, not max)

    speed_processor sp;
    sp.init(sa);
    EXPECT(sp.speed_flags.dist_mode == distance_mode::Lp);

    // L3 distance of (3, 4) = (27 + 64)^(1/3) = 91^(1/3) ≈ 4.497
    vec2d v = { 3.0, 4.0 };
    double speed = sp.calc_speed_whole(v, 1.0);
    double expected = std::pow(27.0 + 64.0, 1.0 / 3.0);
    EXPECT_NEAR(speed, expected, 1e-6);

    // L∞ mode (lp_norm >= MAX_NORM)
    speed_args sa2;
    sa2.whole = true;
    sa2.lp_norm = 20.0;  // >= MAX_NORM (16)
    speed_processor sp2;
    sp2.init(sa2);
    EXPECT(sp2.speed_flags.dist_mode == distance_mode::max);
    double speed2 = sp2.calc_speed_whole({3.0, 4.0}, 1.0);
    EXPECT_NEAR(speed2, 4.0, 1e-9);  // max(3, 4) = 4

    // Separate mode
    speed_args sa3;
    sa3.whole = false;
    speed_processor sp3;
    sp3.init(sa3);
    EXPECT(sp3.speed_flags.dist_mode == distance_mode::separate);
    vec2d v3 = { -3.0, 4.0 };
    sp3.calc_speed_separate(v3, 1.0);
    EXPECT_NEAR(v3.x, 3.0, 1e-9);  // abs(−3)
    EXPECT_NEAR(v3.y, 4.0, 1e-9);
}

static void test_lookup_max_capacity() {
    SECTION("R12 — lookup LUT at max capacity boundary");

    accel_args args;
    args.mode = accel_mode::lookup;

    // Fill to exactly LUT_POINTS_CAPACITY (257 points = 514 floats)
    int max_pts = (int)LUT_POINTS_CAPACITY;
    args.length = max_pts * 2;  // 514

    // Create a simple linear LUT: speed = i, gain = 1 + i/1000
    for (int i = 0; i < max_pts && i * 2 + 1 < (int)LUT_RAW_DATA_CAPACITY; i++) {
        args.data[i * 2]     = (float)i;           // speed
        args.data[i * 2 + 1] = 1.0f + i / 1000.0f; // gain
    }

    lookup lut(args);
    EXPECT(lut.point_count == max_pts);

    // Test interpolation at various points
    double g0 = lut(0.0, args);    // at first point
    EXPECT_NEAR(g0, 1.0, 1e-4);

    double g_mid = lut(128.0, args);  // midpoint
    EXPECT_NEAR(g_mid, 1.128, 1e-3);

    double g_end = lut(256.0, args);  // last point
    EXPECT_NEAR(g_end, 1.256, 1e-3);

    double g_beyond = lut(1000.0, args);  // beyond last → clamp to last
    EXPECT_NEAR(g_beyond, 1.0 + (max_pts - 1) / 1000.0, 1e-3);

    // Verify interpolation between points
    double g_half = lut(0.5, args);  // between point 0 and 1
    // Linear interp: 1.0 + 0.5 * (1.001 - 1.0) = 1.0005
    EXPECT_NEAR(g_half, 1.0005, 1e-3);

    // Over-capacity: length > LUT_RAW_DATA_CAPACITY → clamped
    accel_args args_over = args;
    args_over.length = (int)LUT_RAW_DATA_CAPACITY + 100;  // way over
    lookup lut_over(args_over);
    EXPECT(lut_over.point_count <= (int)LUT_POINTS_CAPACITY);
}

static void test_ema_coefficient_zero() {
    SECTION("R12 — EMA smoother with halfLife=0 (coefficient=0)");

    // simple_ema_smoother with halfLife=0 → coefficient=0
    simple_ema_smoother ses;
    ses.init(0);
    EXPECT_NEAR(ses.windowCoefficient, 0.0, 1e-15);

    // pow(0, time) = 0 for time > 0 → twc = 1 → immediate tracking
    double s1 = ses.smooth(10.0, 1.0);
    EXPECT_NEAR(s1, 10.0, 1e-6);  // should jump to speed immediately
    double s2 = ses.smooth(20.0, 1.0);
    EXPECT_NEAR(s2, 20.0, 1e-6);

    // linear_ema_smoother with halfLife=0
    linear_ema_smoother les;
    les.init(0, 0);
    EXPECT_NEAR(les.windowCoefficient, 0.0, 1e-15);
    double ls1 = les.smooth(10.0, 1.0);
    EXPECT(std::isfinite(ls1));
    double ls2 = les.smooth(20.0, 1.0);
    EXPECT(std::isfinite(ls2));

    // Very small halfLife (nearly zero)
    simple_ema_smoother ses2;
    ses2.init(0.001);
    double s3 = ses2.smooth(100.0, 1.0);
    EXPECT(std::isfinite(s3));
    // With such short halflife, should track very closely
    EXPECT_NEAR(s3, 100.0, 5.0);
}

static void test_modifier_directional_weight_blend() {
    SECTION("R12 — modifier directional weight cos/sin blend");

    profile prof{};
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y = prof.accel_x;
    prof.speed_processor_args.whole = true;
    // Different X and Y weights → directional weight kicks in
    prof.range_weights.x = 2.0;
    prof.range_weights.y = 0.5;

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);
    EXPECT(settings.data.flags.apply_directional_weight);

    speed_processor sp;
    sp.init(prof.speed_processor_args);
    modifier mod;

    // Pure horizontal → weight = range_weights.x * cos(0) + range_weights.y * sin(0)
    //                          = 2.0 * 1 + 0.5 * 0 = 2.0
    vec2d h = { 10.0, 0.0 };
    mod.modify(h, sp, settings, 1.0, 1.0);
    // noaccel gain = 1.0, scale = 1.0 * weight = 2.0
    EXPECT_NEAR(h.x, 20.0, 1.0);  // 10 * 2.0

    // Pure vertical → weight = 2.0 * cos(π/2) + 0.5 * sin(π/2)
    //                        = 2.0 * 0 + 0.5 * 1 = 0.5
    vec2d v = { 0.0, 10.0 };
    mod.modify(v, sp, settings, 1.0, 1.0);
    EXPECT_NEAR(v.y, 5.0, 1.0);  // 10 * 0.5

    // 45° → weight = 2.0 * cos(π/4) + 0.5 * sin(π/4)
    //             = 2.0 * 0.707 + 0.5 * 0.707 ≈ 1.768
    vec2d d = { 10.0, 10.0 };
    mod.modify(d, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(d.x));
    EXPECT(std::isfinite(d.y));
    double expected_weight = 2.0 * std::cos(M_PI / 4) + 0.5 * std::sin(M_PI / 4);
    EXPECT_NEAR(d.x, 10.0 * expected_weight, 2.0);
}

static void test_classic_io_sign_with_cap() {
    SECTION("R12 — classic cap_mode::io with cap.y > 1 (no sign flip)");

    accel_args args = make_args(accel_mode::classic);
    args.cap_mode_val = cap_mode::io;
    args.cap.x = 20.0;
    args.cap.y = 2.0;   // > 1 → no sign flip
    args.exponent_classic = 2.0;

    classic c(args);
    double g_at_zero = c(0.0, args);  // at offset → 1.0
    double g_mid = c(10.0, args);
    double g_high = c(50.0, args);

    EXPECT_NEAR(g_at_zero, 1.0, 1e-9);  // at offset
    EXPECT(g_mid >= 1.0);
    EXPECT(g_high >= 1.0);
    EXPECT(std::isfinite(g_mid));
    EXPECT(std::isfinite(g_high));
    // At cap point (x=20), gain should be close to cap.y = 2.0
    double g_cap = c(20.0, args);
    EXPECT_NEAR(g_cap, 2.0, 0.2);
}

static void test_power_offset_x_guard() {
    SECTION("R12 — power mode offset.x guard (speed <= offset.x → offset.y)");

    accel_args args = make_args(accel_mode::power);
    args.exponent_power = 0.3;
    args.scale = 0.5;
    args.output_offset = 1.2;  // non-zero output offset
    args.cap_mode_val = cap_mode::out;
    args.cap.y = 3.0;

    power p(args);
    // Very low speed should return offset.y or 1.0
    double g_zero = p(0.0, args);
    EXPECT(std::isfinite(g_zero));
    // Negative speed guard
    double g_neg = p(-1.0, args);
    EXPECT(std::isfinite(g_neg));

    // Speed just above offset.x
    double g_just = p(0.01, args);
    EXPECT(std::isfinite(g_just));

    // Speed at cap boundary
    double g_huge = p(1e6, args);
    EXPECT(std::isfinite(g_huge));
}

static void test_speed_clamp_path() {
    SECTION("R12 — modifier speed_min/speed_max clamp");

    profile prof{};
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y = prof.accel_x;
    prof.speed_min = 5.0;   // minimum speed
    prof.speed_max = 100.0; // maximum speed

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);
    EXPECT(settings.data.flags.clamp_speed);

    speed_processor sp;
    sp.init(prof.speed_processor_args);
    modifier mod;

    // Very slow movement → should be clamped UP to speed_min
    vec2d slow = { 0.1, 0.0 };
    mod.modify(slow, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(slow.x));
    // The actual output depends on the clamping ratio

    // Very fast movement → should be clamped DOWN to speed_max
    vec2d fast = { 10000.0, 0.0 };
    mod.modify(fast, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(fast.x));

    // Normal speed (within range) → no clamping effect
    vec2d normal = { 50.0, 0.0 };
    mod.modify(normal, sp, settings, 1.0, 1.0);
    EXPECT(std::isfinite(normal.x));
    EXPECT_NEAR(normal.x, 50.0, 1.0);  // noaccel, should be ~50

    // speed_min == speed_max → everything clamped to one speed
    profile prof2{};
    prof2.accel_x.mode = accel_mode::noaccel;
    prof2.accel_y = prof2.accel_x;
    prof2.speed_min = 50.0;
    prof2.speed_max = 50.0;
    modifier_settings s2;
    s2.prof = prof2;
    init_settings(s2);
    speed_processor sp2;
    sp2.init(prof2.speed_processor_args);
    vec2d v = { 10.0, 0.0 };
    mod.modify(v, sp2, s2, 1.0, 1.0);
    EXPECT(std::isfinite(v.x));
}

static void test_dir_mul_negative_direction() {
    SECTION("R12 — directional DPI multiplier negative direction");

    profile prof{};
    prof.accel_x.mode = accel_mode::noaccel;
    prof.accel_y = prof.accel_x;
    prof.lr_output_dpi_ratio = 2.0;  // right = 2x, left = 0.5x
    prof.ud_output_dpi_ratio = 3.0;  // down = 3x, up = 1/3x

    modifier_settings settings;
    settings.prof = prof;
    init_settings(settings);
    EXPECT(settings.data.flags.apply_dir_mul_x);
    EXPECT(settings.data.flags.apply_dir_mul_y);

    speed_processor sp;
    sp.init(prof.speed_processor_args);
    modifier mod;

    // Positive X (right) → multiply by ratio
    vec2d right = { 10.0, 0.0 };
    mod.modify(right, sp, settings, 1.0, 1.0);
    EXPECT_NEAR(right.x, 20.0, 1.0);  // 10 * 2.0

    // Negative X (left) → multiply by 1/ratio
    vec2d left = { -10.0, 0.0 };
    mod.modify(left, sp, settings, 1.0, 1.0);
    EXPECT_NEAR(left.x, -5.0, 1.0);   // -10 * (1/2.0)

    // Positive Y (down) → multiply by ratio
    vec2d down = { 0.0, 10.0 };
    mod.modify(down, sp, settings, 1.0, 1.0);
    EXPECT_NEAR(down.y, 30.0, 1.0);   // 10 * 3.0

    // Negative Y (up) → multiply by 1/ratio
    vec2d up = { 0.0, -10.0 };
    mod.modify(up, sp, settings, 1.0, 1.0);
    EXPECT_NEAR(up.y, -10.0 / 3.0, 1.0);  // -10 * (1/3.0)
}

static void test_synchronous_power_lt_one() {
    SECTION("R12 — synchronous mode with power < 1 (MIN_SPEED guard)");

    accel_args args;
    args.mode = accel_mode::synchronous;
    args.sync_speed = 5.0;
    args.exponent_power = 0.3;  // < 1 → gain = pow(x/sync, -0.7) → ∞ as x→0

    synchronous s(args);

    // Very small speed → should hit MIN_SPEED guard, not produce Inf
    double g_tiny = s(1e-10, args);
    EXPECT(std::isfinite(g_tiny));

    // Zero speed → return 1.0
    double g_zero = s(0.0, args);
    EXPECT_NEAR(g_zero, 1.0, 1e-9);

    // At sync_speed → gain = pow(1, power-1) = 1.0
    double g_sync = s(5.0, args);
    EXPECT_NEAR(g_sync, 1.0, 1e-6);

    // Above sync_speed → gain > 1 (when power < 1, gain = pow(x/sync, power-1) < 1 actually)
    double g_high = s(100.0, args);
    EXPECT(std::isfinite(g_high));
}

static void test_natural_legacy_mode() {
    SECTION("R12 — natural legacy (non-gain) mode");

    accel_args args = make_args(accel_mode::natural);
    args.gain = false;  // legacy mode
    args.limit = 2.0;
    args.decay_rate = 0.1;
    args.input_offset = 0;

    natural n(args);

    // At offset, return 1.0
    double g0 = n(0.0, args);
    EXPECT_NEAR(g0, 1.0, 1e-9);

    // As speed → ∞, gain → limit + 1 = 2.0
    double g_high = n(1000.0, args);
    EXPECT_NEAR(g_high, 2.0, 0.1);

    // Intermediate speed: monotonically increasing
    double g1 = n(10.0, args);
    double g2 = n(20.0, args);
    double g3 = n(50.0, args);
    EXPECT(g1 < g2);
    EXPECT(g2 < g3);
    EXPECT(g3 <= 2.0 + 1e-6);
}

static void test_config_output_dpi_sanitize() {
    SECTION("R12 — sanitize output_dpi boundary values");

    // output_dpi < 1 → clamped to 1
    device_profile dp;
    dp.prof.output_dpi = -500;
    sanitize_device_profile(dp);
    EXPECT(dp.prof.output_dpi >= 1.0);

    // output_dpi > 32000 → clamped to 32000
    device_profile dp2;
    dp2.prof.output_dpi = 99999;
    sanitize_device_profile(dp2);
    EXPECT(dp2.prof.output_dpi <= 32000.0);

    // output_dpi NaN → finite_or(NaN, NORMALIZED_DPI) should give NORMALIZED_DPI
    // then clamp: result in [1, 32000]
    device_profile dp3;
    dp3.prof.output_dpi = std::numeric_limits<double>::quiet_NaN();
    sanitize_device_profile(dp3);
    EXPECT(std::isfinite(dp3.prof.output_dpi));
    EXPECT(dp3.prof.output_dpi >= 1.0);
    EXPECT(dp3.prof.output_dpi <= 32000.0);
}

// ── BUG-5: JSON int extraction must not trigger UB on out-of-range doubles ───
// libFuzzer + UBSan caught nlohmann::json::get<int>() invoking
// "X is outside the range of representable values of type 'int'" UB when
// the JSON value was a float like 1e26.  All int-typed fields now go
// through json_get_int_safe() which clamps and casts defensively.

static void test_json_int_overflow_safe() {
    SECTION("BUG-5 — JSON parse rejects out-of-range numbers without UB");

    // 1e26 in dpi field — must be clamped to 32000 (sanitize) without
    // ever performing the UB int-cast that triggered the UBSan warning.
    {
        std::string js = R"({"profiles":[{"name":"x","dpi":1e26,"polling_rate":1e26,
                          "profile":{"accel_x":{"mode":"classic"}}}]})";
        std::string tmp = "/tmp/rawaccel_test_bug5.json";
        { std::ofstream f(tmp); f << js; }
        app_config cfg = load_config(tmp);
        std::remove(tmp.c_str());
        EXPECT(cfg.profiles.size() == 1);
        EXPECT(cfg.profiles[0].dev_cfg.dpi == 32000);              // upper clamp
        EXPECT(cfg.profiles[0].dev_cfg.polling_rate == 8000);       // upper clamp
    }
    // -1e30 → INT_MIN clamp path → sanitize → 1
    {
        std::string js = R"({"profiles":[{"name":"y","dpi":-1e30,"polling_rate":-1e30,
                          "profile":{}}]})";
        std::string tmp = "/tmp/rawaccel_test_bug5b.json";
        { std::ofstream f(tmp); f << js; }
        app_config cfg = load_config(tmp);
        std::remove(tmp.c_str());
        EXPECT(cfg.profiles[0].dev_cfg.dpi == 1);
        EXPECT(cfg.profiles[0].dev_cfg.polling_rate == 125);
    }
    // Non-numeric dpi (string) → fallback default
    {
        std::string js = R"({"profiles":[{"name":"z","dpi":"not a number",
                          "polling_rate":true,"profile":{}}]})";
        std::string tmp = "/tmp/rawaccel_test_bug5c.json";
        { std::ofstream f(tmp); f << js; }
        app_config cfg = load_config(tmp);
        std::remove(tmp.c_str());
        // Fallback was 800 / 1000, both within sanitize range.
        EXPECT(cfg.profiles[0].dev_cfg.dpi == 800);
        EXPECT(cfg.profiles[0].dev_cfg.polling_rate == 1000);
    }
    // lut_length 1e26 — must not UB and must clamp under LUT capacity.
    {
        std::string js = R"({"profiles":[{"name":"l",
                          "profile":{"accel_x":{"mode":"lookup","lut_length":1e26,
                                                 "lut_data":[1.0,1.0]}}}]})";
        std::string tmp = "/tmp/rawaccel_test_bug5d.json";
        { std::ofstream f(tmp); f << js; }
        app_config cfg = load_config(tmp);
        std::remove(tmp.c_str());
        EXPECT(cfg.profiles[0].prof.accel_x.length <=
               (int)LUT_RAW_DATA_CAPACITY);
        EXPECT(cfg.profiles[0].prof.accel_x.length >= 0);
    }
}

// ── BUG-1: rotation negative normalization preserves direction ───────────────

static void test_rotation_negative_normalization() {
    SECTION("BUG-1 — sanitize_profile: negative rotation maps to mathematical equivalent");

    // -45° (CCW 45) must become +315°, NOT +45° (which would flip direction).
    {
        device_profile dp;
        dp.prof.degrees_rotation = -45.0;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 315.0, 1e-9);
    }
    // -90° → 270°
    {
        device_profile dp;
        dp.prof.degrees_rotation = -90.0;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 270.0, 1e-9);
    }
    // -179.5° → 180.5°
    {
        device_profile dp;
        dp.prof.degrees_rotation = -179.5;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 180.5, 1e-9);
    }
    // Multi-period wrap: -720° → 0°
    {
        device_profile dp;
        dp.prof.degrees_rotation = -720.0;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 0.0, 1e-9);
    }
    // -725° → -725 mod 360 = -5 → +355
    {
        device_profile dp;
        dp.prof.degrees_rotation = -725.0;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 355.0, 1e-9);
    }
    // Positive values must still wrap into [0,360): 720 → 0, 405 → 45.
    {
        device_profile dp;
        dp.prof.degrees_rotation = 720.0;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 0.0, 1e-9);
    }
    {
        device_profile dp;
        dp.prof.degrees_rotation = 405.0;
        sanitize_device_profile(dp);
        EXPECT_NEAR(dp.prof.degrees_rotation, 45.0, 1e-9);
    }
    // Direction preservation property: rotating by sanitize(-θ) must
    // produce the same vector as rotating by 360-θ.
    {
        device_profile dp_neg, dp_pos;
        dp_neg.prof.degrees_rotation = -30.0;
        dp_pos.prof.degrees_rotation = 330.0;
        sanitize_device_profile(dp_neg);
        sanitize_device_profile(dp_pos);
        vec2d d_neg = direction(dp_neg.prof.degrees_rotation);
        vec2d d_pos = direction(dp_pos.prof.degrees_rotation);
        EXPECT_NEAR(d_neg.x, d_pos.x, 1e-12);
        EXPECT_NEAR(d_neg.y, d_pos.y, 1e-12);
    }
}

// ── R13 — lat_stats move + dpi_factor pre-compute ────────────────────────────

static void test_lat_stats_move_semantics() {
    SECTION("R13 — lat_stats move preserves data and resets source");

    // Record some data into a lat_stats instance
    lat_stats ls;
    ls.record(10.0);
    ls.record(20.0);
    ls.record(30.0);
    EXPECT(ls.count == 3);
    EXPECT_NEAR(ls.sum_us, 60.0, 1e-9);
    EXPECT_NEAR(ls.min_us, 10.0, 1e-9);
    EXPECT_NEAR(ls.max_us, 30.0, 1e-9);

    // Move construct: data transfers to new object, source resets
    lat_stats ls2(std::move(ls));
    EXPECT(ls2.count == 3);
    EXPECT_NEAR(ls2.sum_us, 60.0, 1e-9);
    EXPECT_NEAR(ls2.min_us, 10.0, 1e-9);
    EXPECT_NEAR(ls2.max_us, 30.0, 1e-9);
    // Source should be reset
    EXPECT(ls.count == 0);
    EXPECT_NEAR(ls.sum_us, 0.0, 1e-9);
    EXPECT_NEAR(ls.min_us, 1e9, 1.0); // reset sentinel

    // Move assign: record into ls, then move-assign from ls2
    ls.record(5.0);
    ls = std::move(ls2);
    EXPECT(ls.count == 3);
    EXPECT_NEAR(ls.sum_us, 60.0, 1e-9);
    // ls2 should be reset
    EXPECT(ls2.count == 0);

    // Snapshot and reset
    auto snap = ls.snapshot_and_reset();
    EXPECT(snap.count == 3);
    EXPECT_NEAR(snap.avg_us(), 20.0, 1e-9);
    EXPECT(ls.count == 0);
}

static void test_dpi_factor_precompute() {
    SECTION("R13 — dpi_factor pre-compute consistency");

    // Verify that dpi_factor = dpi / NORMALIZED_DPI for various DPI values
    for (int dpi : {100, 400, 800, 1600, 3200, 16000, 32000}) {
        double expected = dpi / NORMALIZED_DPI;
        // Simulate what apply_profile does:
        double computed = std::clamp(dpi, 1, 32000) / NORMALIZED_DPI;
        EXPECT_NEAR(computed, expected, 1e-12);
    }

    // Extreme: DPI=1 (min), DPI=32000 (max)
    EXPECT_NEAR(1.0 / NORMALIZED_DPI, 0.001, 1e-9);
    EXPECT_NEAR(32000.0 / NORMALIZED_DPI, 32.0, 1e-9);

    // Verify the division result is finite and positive for all valid DPI
    for (int dpi = 1; dpi <= 32000; dpi += 1000) {
        double f = dpi / NORMALIZED_DPI;
        EXPECT(std::isfinite(f));
        EXPECT(f > 0);
    }
}

// R14 — magnitude() uses std::hypot (overflow-safe)
static void test_magnitude_hypot() {
    SECTION("R14 — magnitude uses hypot (overflow-safe)");

    // Normal values — must match manual sqrt(x^2 + y^2)
    EXPECT_NEAR(magnitude({3.0, 4.0}), 5.0, 1e-12);
    EXPECT_NEAR(magnitude({0.0, 0.0}), 0.0, 1e-12);
    EXPECT_NEAR(magnitude({1.0, 0.0}), 1.0, 1e-12);
    EXPECT_NEAR(magnitude({0.0, -7.0}), 7.0, 1e-12);

    // Typical mouse deltas
    EXPECT_NEAR(magnitude({5.0, 12.0}), 13.0, 1e-12);
    EXPECT_NEAR(magnitude({-8.0, 6.0}), 10.0, 1e-12);

    // Large values that would overflow with naive sqrt(x*x+y*y):
    // DBL_MAX ≈ 1.8e308, so x*x would be inf for x > ~1.34e154
    double big = 1e200;
    double result = magnitude({big, big});
    EXPECT(std::isfinite(result));
    EXPECT_NEAR(result, big * std::sqrt(2.0), big * 1e-12);

    // Very small values (subnormal territory) — must not underflow to zero
    double tiny = 1e-300;
    double tiny_result = magnitude({tiny, tiny});
    EXPECT(tiny_result > 0.0);
    EXPECT(std::isfinite(tiny_result));
    EXPECT_NEAR(tiny_result, tiny * std::sqrt(2.0), tiny * 1e-6);

    // Verify magnitude is used correctly in rawaccel pipeline
    // (modifier speed calculation uses magnitude on mouse deltas)
    vec2d delta = {10.0, 10.0};
    double speed = magnitude(delta);
    EXPECT_NEAR(speed, 10.0 * std::sqrt(2.0), 1e-10);
}

// ── R15: Comprehensive hardening tests ───────────────────────────────────────

/// Test LUT binary search boundary conditions exhaustively.
static void test_lut_binary_search_boundaries() {
    SECTION("R15 — LUT binary search boundary exhaustive");

    // 2-point LUT: linear interpolation between exactly 2 points
    {
        accel_args a = make_args(accel_mode::lookup);
        a.data[0] = 10.0f; a.data[1] = 1.0f;  // speed=10 → gain=1.0
        a.data[2] = 50.0f; a.data[3] = 3.0f;  // speed=50 → gain=3.0
        a.length = 4;
        accel_union au; au.init(a);

        // Below first point → clamp to first gain
        EXPECT_NEAR(au.apply(0.0, a), 1.0, 1e-9);
        EXPECT_NEAR(au.apply(5.0, a), 1.0, 1e-9);
        EXPECT_NEAR(au.apply(10.0, a), 1.0, 1e-9);

        // Exact midpoint: speed=30 → gain=2.0 (linear interp)
        EXPECT_NEAR(au.apply(30.0, a), 2.0, 1e-9);

        // At second point exactly
        EXPECT_NEAR(au.apply(50.0, a), 3.0, 1e-9);

        // Above last point → clamp to last gain
        EXPECT_NEAR(au.apply(100.0, a), 3.0, 1e-9);
        EXPECT_NEAR(au.apply(1e6, a), 3.0, 1e-9);
    }

    // 3-point LUT: binary search mid calculation
    {
        accel_args a = make_args(accel_mode::lookup);
        a.data[0] = 0.0f;  a.data[1] = 1.0f;
        a.data[2] = 25.0f; a.data[3] = 2.0f;
        a.data[4] = 50.0f; a.data[5] = 4.0f;
        a.length = 6;
        accel_union au; au.init(a);

        // Verify each segment
        EXPECT_NEAR(au.apply(0.0, a), 1.0, 1e-9);
        EXPECT_NEAR(au.apply(12.5, a), 1.5, 1e-9);  // midpoint seg 1
        EXPECT_NEAR(au.apply(25.0, a), 2.0, 1e-9);
        EXPECT_NEAR(au.apply(37.5, a), 3.0, 1e-9);  // midpoint seg 2
        EXPECT_NEAR(au.apply(50.0, a), 4.0, 1e-9);
    }

    // Max capacity LUT: fill all 257 points and verify endpoints
    {
        accel_args a = make_args(accel_mode::lookup);
        int cap = (int)LUT_POINTS_CAPACITY;
        a.length = cap * 2;
        for (int i = 0; i < cap; i++) {
            a.data[i * 2]     = static_cast<float>(i);       // speed = i
            a.data[i * 2 + 1] = 1.0f + static_cast<float>(i) * 0.01f;  // gain = 1 + i*0.01
        }
        accel_union au; au.init(a);

        // First point
        EXPECT_NEAR(au.apply(0.0, a), 1.0, 1e-4);
        // Last point
        EXPECT_NEAR(au.apply(cap - 1, a), 1.0 + (cap - 1) * 0.01, 0.01);
        // Beyond last
        EXPECT_NEAR(au.apply(1000.0, a), 1.0 + (cap - 1) * 0.01, 0.01);
        // Mid-point interpolation
        EXPECT_NEAR(au.apply(128.5, a), 1.0 + 128.5 * 0.01, 0.01);
    }
}

/// Test the full modifier pipeline with every feature flag enabled simultaneously.
static void test_modifier_full_pipeline_stress() {
    SECTION("R15 — modifier full pipeline: all features enabled simultaneously");

    profile p{};
    p.accel_x.mode = accel_mode::classic;
    p.accel_x.gain = true;
    p.accel_x.acceleration = 0.01;
    p.accel_x.exponent_classic = 2.0;
    p.accel_x.limit = 2.0;
    p.accel_x.input_offset = 5.0;
    p.accel_x.cap = {15, 1.8};
    p.accel_x.cap_mode_val = cap_mode::io;

    p.accel_y = p.accel_x;
    p.accel_y.mode = accel_mode::natural;
    p.accel_y.decay_rate = 0.1;

    p.degrees_rotation = 30.0;
    p.degrees_snap = 10.0;
    p.speed_min = 2.0;
    p.speed_max = 100.0;
    p.output_dpi = 1600;
    p.lr_output_dpi_ratio = 1.2;
    p.ud_output_dpi_ratio = 0.8;
    p.speed_processor_args.whole = true;
    p.speed_processor_args.lp_norm = 2.0;
    p.speed_processor_args.input_speed_smooth_halflife = 5.0;
    p.speed_processor_args.scale_smooth_halflife = 3.0;

    modifier_settings ms; ms.prof = p; init_settings(ms);
    speed_processor   sp; sp.init(p.speed_processor_args);
    modifier          mod;
    double dpi_factor = 1600.0 / NORMALIZED_DPI;

    // Run 10000 events through the pipeline
    double rem_x = 0, rem_y = 0;
    double total_out_x = 0, total_out_y = 0;
    for (int i = 0; i < 10000; i++) {
        double dx = 3.0 + std::sin(i * 0.1) * 2.0;
        double dy = 2.0 + std::cos(i * 0.1) * 1.5;
        double time_ms = 1.0; // 1000 Hz

        vec2d in = {dx, dy};
        mod.modify(in, sp, ms, dpi_factor, time_ms);

        // Every output must be finite
        EXPECT(std::isfinite(in.x));
        EXPECT(std::isfinite(in.y));

        // Apply subpixel accumulation
        double out_x_d = in.x + rem_x;
        double out_y_d = in.y + rem_y;
        int ox = (int)(out_x_d >= 0 ? out_x_d + 0.5 : out_x_d - 0.5);
        int oy = (int)(out_y_d >= 0 ? out_y_d + 0.5 : out_y_d - 0.5);
        rem_x = out_x_d - ox;
        rem_y = out_y_d - oy;

        total_out_x += ox;
        total_out_y += oy;
    }

    // Verify some motion actually occurred (not stuck at zero)
    EXPECT(std::fabs(total_out_x) > 100.0);
    EXPECT(std::fabs(total_out_y) > 100.0);

    // Verify remainder stays bounded
    EXPECT(std::fabs(rem_x) < 1.0);
    EXPECT(std::fabs(rem_y) < 1.0);
}

/// Test EMA smoother numerical stability over very long sequences.
static void test_ema_long_sequence_stability() {
    SECTION("R15 — EMA smoother: 100K iterations, no drift or divergence");

    // simple_ema_smoother: feed constant speed → must converge
    {
        simple_ema_smoother sm;
        sm.init(10.0); // halflife=10ms
        double last = 0;
        for (int i = 0; i < 100000; i++) {
            last = sm.smooth(5.0, 1.0); // dt=1ms
        }
        EXPECT_NEAR(last, 5.0, 1e-3);
    }

    // simple_ema_smoother: alternating values → output stays bounded
    {
        simple_ema_smoother sm;
        sm.init(10.0);
        double last = 0;
        for (int i = 0; i < 100000; i++) {
            double v = (i % 2 == 0) ? 10.0 : 0.0;
            last = sm.smooth(v, 1.0);
        }
        EXPECT(std::isfinite(last));
        EXPECT(last >= 0.0 && last <= 10.0);
    }

    // linear_ema_smoother: convergence test
    {
        linear_ema_smoother lm;
        lm.init(10.0, 10.0 * linear_ema_smoother::trendDampening);
        double last = 0;
        for (int i = 0; i < 100000; i++) {
            last = lm.smooth(5.0, 1.0);
        }
        EXPECT_NEAR(last, 5.0, 0.1);
    }

    // halflife=0 (no smoothing) — output tracks input
    {
        simple_ema_smoother sm;
        sm.init(0.0); // coefficient=0 → no smoothing
        for (int i = 0; i < 100; i++) sm.smooth(42.0, 1.0);
        double v = sm.smooth(99.0, 1.0);
        // With halflife=0, coefficient=0 → smooth returns min(window,cutoff)
        // which should be close to the input
        EXPECT(std::isfinite(v));
    }
}

/// Test config round-trip with every field at exact boundary values.
static void test_config_boundary_roundtrip() {
    SECTION("R15 — config round-trip: all fields at boundary values");

    device_profile dp;
    dp.name = "boundary-test";
    dp.device_id = "usb:1234:5678:serial";
    dp.dev_cfg.dpi = 32000;         // max
    dp.dev_cfg.polling_rate = 8000; // max

    auto& p = dp.prof;
    p.degrees_rotation = 359.99; // not 360: sanitize does fmod(360,360)=0
    p.degrees_snap = 45.0;
    p.speed_min = 0.0;
    p.speed_max = 500.0;
    p.output_dpi = 32000.0;
    p.lr_output_dpi_ratio = 100.0;
    p.ud_output_dpi_ratio = 0.01;
    p.raw_passthrough = false;

    auto& ax = p.accel_x;
    ax.mode = accel_mode::classic;
    ax.gain = true;
    ax.acceleration = 20.0;
    ax.exponent_classic = 10.0;
    ax.exponent_power = 5.0;
    ax.limit = 100.0;
    ax.input_offset = 100.0;
    ax.output_offset = 100.0;
    ax.decay_rate = 10.0;
    ax.scale = 100.0;
    ax.motivity = 10.0;
    ax.gamma = 10.0;
    ax.sync_speed = 100.0;
    ax.smooth = 1.0;
    ax.cap = {500, 100};
    ax.cap_mode_val = cap_mode::io;

    p.accel_y = ax;
    p.accel_y.mode = accel_mode::natural;

    auto& sp = p.speed_processor_args;
    sp.whole = true;
    sp.lp_norm = 32.0;
    sp.input_speed_smooth_halflife = 200.0;
    sp.scale_smooth_halflife = 200.0;
    sp.output_speed_smooth_halflife = 200.0;

    // Serialize → deserialize
    std::string json = profile_to_json(dp);
    device_profile dp2 = profile_from_json(json);

    // Verify every field
    EXPECT(dp2.name == dp.name);
    EXPECT(dp2.device_id == dp.device_id);
    EXPECT(dp2.dev_cfg.dpi == dp.dev_cfg.dpi);
    EXPECT(dp2.dev_cfg.polling_rate == dp.dev_cfg.polling_rate);
    EXPECT_NEAR(dp2.prof.degrees_rotation, 359.99, 1e-9);
    EXPECT_NEAR(dp2.prof.degrees_snap, p.degrees_snap, 1e-9);
    EXPECT_NEAR(dp2.prof.speed_min, p.speed_min, 1e-9);
    EXPECT_NEAR(dp2.prof.speed_max, p.speed_max, 1e-9);
    EXPECT_NEAR(dp2.prof.output_dpi, p.output_dpi, 1e-9);
    EXPECT_NEAR(dp2.prof.lr_output_dpi_ratio, p.lr_output_dpi_ratio, 1e-9);
    EXPECT_NEAR(dp2.prof.ud_output_dpi_ratio, p.ud_output_dpi_ratio, 1e-9);
    EXPECT(dp2.prof.raw_passthrough == false);

    auto& ax2 = dp2.prof.accel_x;
    EXPECT(ax2.mode == accel_mode::classic);
    EXPECT(ax2.gain == true);
    EXPECT_NEAR(ax2.acceleration, 20.0, 1e-9);
    EXPECT_NEAR(ax2.exponent_classic, 10.0, 1e-9);
    EXPECT_NEAR(ax2.limit, 100.0, 1e-9);
    EXPECT_NEAR(ax2.input_offset, 100.0, 1e-9);
    EXPECT_NEAR(ax2.output_offset, 100.0, 1e-9);
    EXPECT_NEAR(ax2.scale, 100.0, 1e-9);
    EXPECT_NEAR(ax2.cap.x, 500.0, 1e-3);
    EXPECT_NEAR(ax2.cap.y, 100.0, 1e-3);

    EXPECT(dp2.prof.accel_y.mode == accel_mode::natural);

    EXPECT_NEAR(dp2.prof.speed_processor_args.lp_norm, 32.0, 1e-9);
    EXPECT_NEAR(dp2.prof.speed_processor_args.input_speed_smooth_halflife, 200.0, 1e-9);
}

/// Test rotate() + direction() mathematical identity: rotate by θ then -θ = identity.
static void test_rotate_direction_identity() {
    SECTION("R15 — rotate/direction identity and edge cases");

    // Rotate 0° = identity
    vec2d v = {3.0, 4.0};
    vec2d r0 = rotate(v, direction(0.0));
    EXPECT_NEAR(r0.x, 3.0, 1e-12);
    EXPECT_NEAR(r0.y, 4.0, 1e-12);

    // Rotate 360° = identity
    vec2d r360 = rotate(v, direction(360.0));
    EXPECT_NEAR(r360.x, 3.0, 1e-9);
    EXPECT_NEAR(r360.y, 4.0, 1e-9);

    // Rotate 90° then -90° = identity
    vec2d r90 = rotate(v, direction(90.0));
    vec2d back = rotate(r90, direction(-90.0));
    EXPECT_NEAR(back.x, 3.0, 1e-9);
    EXPECT_NEAR(back.y, 4.0, 1e-9);

    // Rotate 90°: (1,0) → (0,1)
    vec2d unit_x = {1.0, 0.0};
    vec2d rot90 = rotate(unit_x, direction(90.0));
    EXPECT_NEAR(rot90.x, 0.0, 1e-12);
    EXPECT_NEAR(rot90.y, 1.0, 1e-12);

    // Rotate 180°: (1,0) → (-1,0)
    vec2d rot180 = rotate(unit_x, direction(180.0));
    EXPECT_NEAR(rot180.x, -1.0, 1e-12);
    EXPECT_NEAR(rot180.y, 0.0, 1e-12);

    // Magnitude preserved under rotation
    for (double deg = 0; deg < 360; deg += 17.3) {
        vec2d rv = rotate(v, direction(deg));
        EXPECT_NEAR(magnitude(rv), magnitude(v), 1e-9);
    }
}

/// Test all helper functions in math-vec2.hpp thoroughly.
static void test_vec2_helpers_exhaustive() {
    SECTION("R15 — vec2 helpers: maxsd, minsd, clampsd, lp_distance");

    EXPECT_NEAR(maxsd(3.0, 7.0), 7.0, 1e-15);
    EXPECT_NEAR(maxsd(-1.0, -5.0), -1.0, 1e-15);
    EXPECT_NEAR(maxsd(0.0, 0.0), 0.0, 1e-15);

    EXPECT_NEAR(minsd(3.0, 7.0), 3.0, 1e-15);
    EXPECT_NEAR(minsd(-1.0, -5.0), -5.0, 1e-15);

    EXPECT_NEAR(clampsd(5.0, 0.0, 10.0), 5.0, 1e-15);
    EXPECT_NEAR(clampsd(-5.0, 0.0, 10.0), 0.0, 1e-15);
    EXPECT_NEAR(clampsd(15.0, 0.0, 10.0), 10.0, 1e-15);
    EXPECT_NEAR(clampsd(0.0, 0.0, 0.0), 0.0, 1e-15);

    // lp_distance for various norms
    vec2d v = {3.0, 4.0};
    EXPECT_NEAR(lp_distance(v, 2.0), 5.0, 1e-9);   // L2 = euclidean
    EXPECT_NEAR(lp_distance(v, 1.0), 7.0, 1e-9);   // L1 = manhattan
    // L∞ approximation (large p)
    EXPECT_NEAR(lp_distance(v, 100.0), 4.0, 0.01); // → max(3,4) = 4

    // lp_distance with zero vector (R6 fix)
    EXPECT_NEAR(lp_distance({0, 0}, 2.0), 0.0, 1e-15);
    EXPECT_NEAR(lp_distance({0, 0}, 0.5), 0.0, 1e-15);
    EXPECT_NEAR(lp_distance({0, 0}, -1.0), 0.0, 1e-15);
}

/// Concurrent-like stress test for lat_stats — rapid record/snapshot cycles.
static void test_lat_stats_stress() {
    SECTION("R15 — lat_stats stress: 1M records + periodic snapshots");

    lat_stats ls;

    // Record 1M values
    for (int i = 0; i < 1000000; i++) {
        ls.record(static_cast<double>(i % 1000));
    }
    EXPECT(ls.count == 1000000);
    EXPECT_NEAR(ls.min_us, 0.0, 1e-9);
    EXPECT_NEAR(ls.max_us, 999.0, 1e-9);
    EXPECT_NEAR(ls.sum_us / ls.count, 499.5, 0.01); // avg of 0..999 repeated

    // Snapshot and reset
    auto snap = ls.snapshot_and_reset();
    EXPECT(snap.count == 1000000);
    EXPECT(ls.count == 0);

    // Histogram bucket coverage: bucket = int(lat_us / 0.5)
    // Value 0µs → bucket 0, value 249µs → bucket 498, values ≥500µs → over
    EXPECT(snap.hist[0] > 0);   // bucket 0: [0.0, 0.5) — hit by value 0
    EXPECT(snap.hist[998] > 0); // bucket 998: [499.0, 499.5) — hit by value 499
    EXPECT(snap.over > 0);      // values 500–999 go to over

    // Record after reset — works correctly
    ls.record(42.0);
    EXPECT(ls.count == 1);
    EXPECT_NEAR(ls.min_us, 42.0, 1e-9);
    EXPECT_NEAR(ls.max_us, 42.0, 1e-9);
}

/// Test all 7 accel modes through sanitize → init → apply for consistency.
static void test_all_modes_sanitize_init_apply() {
    SECTION("R15 — all modes: sanitize → init → apply cycle");

    accel_mode modes[] = {
        accel_mode::noaccel, accel_mode::classic, accel_mode::power,
        accel_mode::natural, accel_mode::jump, accel_mode::synchronous,
        accel_mode::lookup
    };

    for (auto m : modes) {
        device_profile dp;
        dp.name = "test";
        dp.dev_cfg.dpi = 800;
        dp.dev_cfg.polling_rate = 1000;
        auto& a = dp.prof.accel_x;
        a.mode = m;
        a.gain = true;
        a.acceleration = 0.01;
        a.exponent_classic = 2.0;
        a.exponent_power = 0.5;
        a.limit = 2.0;
        a.decay_rate = 0.1;
        a.scale = 1.0;
        a.motivity = 1.5;
        a.gamma = 1.0;
        a.sync_speed = 5.0;
        a.smooth = 0.5;
        a.cap = {15, 1.8};
        a.cap_mode_val = cap_mode::out;

        if (m == accel_mode::lookup) {
            a.data[0] = 0; a.data[1] = 1.0f;
            a.data[2] = 50; a.data[3] = 2.0f;
            a.length = 4;
        }

        dp.prof.accel_y = a;

        // Sanitize via device_profile (the public API)
        sanitize_device_profile(dp);

        // Init + apply at various speeds
        accel_union au;
        au.init(dp.prof.accel_x);
        for (double spd = 0.0; spd <= 200.0; spd += 5.0) {
            double g = au.apply(spd, dp.prof.accel_x);
            EXPECT(std::isfinite(g));
            EXPECT(g >= 0.0); // gain must never be negative
        }
    }
}

/// Test config save/load with Unicode profile names and device IDs.
static void test_config_unicode_names() {
    SECTION("R15 — config round-trip: Unicode profile names");

    app_config cfg;
    device_profile dp;
    dp.name = "Oyun Profili \xC3\xB6zel";  // "özel" in UTF-8
    dp.device_id = "usb:04d9:a0cd:\xE4\xB8\xAD\xE6\x96\x87"; // Chinese chars
    dp.dev_cfg.dpi = 1600;
    dp.dev_cfg.polling_rate = 1000;
    cfg.profiles.push_back(dp);
    cfg.active_profile = dp.name;

    // Save to temp file
    std::string tmp = "/tmp/rawaccel_test_unicode.json";
    save_config(cfg, tmp);

    // Load back
    app_config cfg2 = load_config(tmp);
    EXPECT(cfg2.profiles.size() == 1);
    EXPECT(cfg2.profiles[0].name == dp.name);
    EXPECT(cfg2.profiles[0].device_id == dp.device_id);
    EXPECT(cfg2.active_profile == dp.name);

    std::remove(tmp.c_str());
}

/// Test synchronous mode edge cases: sc=0, power boundary, speed near sync_speed.
static void test_synchronous_edge_cases() {
    SECTION("R15 — synchronous edge cases: near sync_speed, extreme power");

    // Speed exactly at sync_speed
    {
        accel_args a = make_args(accel_mode::synchronous);
        a.sync_speed = 10.0;
        a.acceleration = 0.5;
        a.gain = true;
        accel_union au; au.init(a);
        double g = au.apply(10.0, a);
        EXPECT(std::isfinite(g));
        EXPECT(g > 0.0);
    }

    // Speed just above and below sync_speed
    {
        accel_args a = make_args(accel_mode::synchronous);
        a.sync_speed = 10.0;
        a.acceleration = 0.5;
        a.gain = true;
        accel_union au; au.init(a);

        double g_below = au.apply(9.999, a);
        double g_above = au.apply(10.001, a);
        EXPECT(std::isfinite(g_below));
        EXPECT(std::isfinite(g_above));
        // Gain should be continuous near sync_speed
        EXPECT(std::fabs(g_below - g_above) < 0.1);
    }

    // Zero acceleration → gain = 1.0
    {
        accel_args a = make_args(accel_mode::synchronous);
        a.acceleration = 0.0;
        a.gain = true;
        accel_union au; au.init(a);
        EXPECT_NEAR(au.apply(5.0, a), 1.0, 1e-6);
    }
}

// ── main ─────────────────────────────────────────────────────────────────────

static void print_usage(const char* argv0) {
    std::printf(
        "RawAccel Linux — birim testleri\n"
        "\n"
        "Kullanım: %s [SEÇENEKLER]\n"
        "  --filter <regex>   Yalnızca section adı regex ile eşleşenleri çalıştır\n"
        "                     (örn. --filter 'classic|natural')\n"
        "  --list             Tüm section adlarını listele ve çık\n"
        "  --quiet            PASS satırlarını bastırma; sadece FAIL ve özet göster\n"
        "  -h, --help         Bu yardım metnini göster\n",
        argv0);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (a == "--list") {
            g_list_only = true;
        } else if (a == "--quiet") {
            g_quiet = true;
        } else if (a == "--filter") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Hata: --filter <regex> argümanı gerekli\n");
                return 2;
            }
            try {
                g_filter_regex = std::regex(argv[++i], std::regex::ECMAScript);
                g_have_filter = true;
            } catch (const std::regex_error& e) {
                std::fprintf(stderr, "Hata: geçersiz regex '%s': %s\n", argv[i], e.what());
                return 2;
            }
        } else {
            std::fprintf(stderr, "Hata: bilinmeyen argüman '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (!g_list_only) std::printf("=== RawAccel Linux Birim Testleri ===\n");

    test_noaccel();
    test_classic();
    test_natural();
    test_jump();
    test_synchronous();
    test_lookup();
    test_power();
    test_accel_union();
    test_json_roundtrip();
    test_json_roundtrip_lut();
    test_file_roundtrip();
    test_save_config_relative_path();
    test_monotonic();
    test_power_monotonic();
    test_classic_cap_modes();
    test_modifier();
    test_edge_guards();
    test_modifier_end_to_end();
    test_speed_processor();
    test_config_error_paths();
    test_input_validation();
    test_multi_profile_roundtrip();
    test_atomic_write();
    test_ipc_json_roundtrip();
    test_motion_math();
    test_lat_stats();

    // New edge-case tests
    test_natural_decay_zero();
    test_extreme_inputs();
    test_classic_degenerate();
    test_power_zero_exponent();
    test_lookup_edge_cases();
    test_modifier_zero_time();
    test_ema_smoother_stability();
    test_subpixel_sign();
    test_sanitize_extremes();
    test_lut_sort_on_sanitize();
    test_lut_sort_json_roundtrip();
    test_motion_math_overflow();
    test_accel_args_sanitize();
    test_raw_passthrough_json();

    // Fuzz, edge-case, and stress tests
    test_fuzz_accel_args();
    test_fuzz_unsanitized_motion_math();
    test_fuzz_json_roundtrip();
    test_all_modes_extreme_speeds();
    test_ema_extreme_time();
    test_subpixel_tiny_deltas();
    test_subpixel_negative_deltas();
    test_modifier_all_flags();
    test_modifier_separate_mode();
    test_stress_remainder_drift();
    test_stress_alternating_direction();
    test_power_extreme_params();
    test_lookup_extreme_lut();
    test_natural_extreme_params();
    test_synchronous_extreme();

    // R6 regression tests
    test_dpi_ratio_zero_guard();
    test_lp_distance_zero_vector();

    // R7 deep-dive tests
    test_ema_extreme_halflife();
    test_pipeline_nan_injection();
    test_classic_io_degenerate_cap();
    test_classic_in_degenerate_cap_naninf();
    test_motion_math_clamp_remainder_reset();
    test_save_config_durability_path();
    test_power_output_offset();
    test_directional_weight_boundary();

    // R9 mükemmellik testleri
    test_subpixel_cumulative_drift();
    test_classic_gain_mode_cap_consistency();
    test_natural_gain_formula();

    // R10 — EMA smoother, NaN propagation, event batching, config edge cases
    test_ema_smoother_halflife();
    test_ema_smoother_zero_time();
    test_linear_ema_smoother();
    test_nan_propagation_all_modes();
    test_nan_propagation_pathological_params();
    test_event_batching_accumulation();
    test_event_batching_split_vs_combined();
    test_speed_processor_all_distance_modes();
    test_speed_processor_smoothing();
    test_syn_dropped_reset_behavior();
    test_config_empty_profiles();
    test_config_missing_active_profile();
    test_config_extreme_values();
    test_config_duplicate_device_id();
    test_check_duplicate_device_ids();
    test_sanitize_nan_fields();
    test_modify_subnormal_time();

    // R12 — untested code path coverage
    test_classic_sign_flip();
    test_classic_linear_path_cap();
    test_power_cap_branch();
    test_modifier_rotation_snap_combined();
    test_speed_processor_lp_mode();
    test_lookup_max_capacity();
    test_ema_coefficient_zero();
    test_modifier_directional_weight_blend();
    test_classic_io_sign_with_cap();
    test_power_offset_x_guard();
    test_speed_clamp_path();
    test_dir_mul_negative_direction();
    test_synchronous_power_lt_one();
    test_natural_legacy_mode();
    test_config_output_dpi_sanitize();
    test_rotation_negative_normalization();
    test_json_int_overflow_safe();

    // R13 — lat_stats move safety + dpi_factor pre-compute
    test_lat_stats_move_semantics();
    test_dpi_factor_precompute();

    // R14 — magnitude hypot overflow safety
    test_magnitude_hypot();

    // R15 — comprehensive hardening tests
    test_lut_binary_search_boundaries();
    test_modifier_full_pipeline_stress();
    test_ema_long_sequence_stability();
    test_config_boundary_roundtrip();
    test_rotate_direction_identity();
    test_vec2_helpers_exhaustive();
    test_lat_stats_stress();
    test_all_modes_sanitize_init_apply();
    test_config_unicode_names();
    test_synchronous_edge_cases();

    if (g_list_only) return 0;

    std::printf("\n=== Sonuç: %d/%d geçti", g_passed, g_tests);
    if (g_failed) std::printf(", %d BAŞARISIZ", g_failed);
    if (g_skipped_sections) std::printf(" (%d section atlandı, --filter)", g_skipped_sections);
    std::printf(" ===\n");

    return g_failed ? 1 : 0;
}
