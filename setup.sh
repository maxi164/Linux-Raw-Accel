#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# RawAccel Linux — One-shot Setup
#
# Tek komutla:
#   1. Bağımlılıkları otomatik kurar (Arch/CachyOS, Debian/Ubuntu, Fedora)
#   2. Eski/bozuk kurulumu tamamen temizler (servis durdur, dosya sil)
#   3. Sıfırdan derler
#   4. Sistem geneline yükler (binary, servis, udev, polkit, desktop)
#   5. KDE Plasma (Wayland/X11) için pointer acceleration'ı Flat'e çeker
#   6. Kullanıcıyı input grubuna ekler, uinput modülünü yükler
#   7. Servisi başlatır ve durumu raporlar
#
# Kullanım:
#   sudo bash setup.sh             # tam kurulum
#   sudo bash setup.sh --uninstall # tamamen kaldır
#   sudo bash setup.sh --reinstall # eski varsa tamamen sil + yeni kur (varsayılan)
#   sudo bash setup.sh --no-deps   # bağımlılık kurulumunu atla
#
# Idempotent: birden fazla çalıştırılabilir, eski sürümün üstüne temiz yazar.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$SCRIPT_DIR"

# ── Renkler ──────────────────────────────────────────────────────────────────
if [[ -t 1 ]]; then
    C_R=$'\e[31m'; C_G=$'\e[32m'; C_Y=$'\e[33m'; C_B=$'\e[34m'; C_M=$'\e[35m'
    C_BOLD=$'\e[1m'; C_RESET=$'\e[0m'
else
    C_R=""; C_G=""; C_Y=""; C_B=""; C_M=""; C_BOLD=""; C_RESET=""
fi
say()  { echo "${C_BOLD}${C_B}::${C_RESET} ${C_BOLD}$*${C_RESET}"; }
ok()   { echo "  ${C_G}✓${C_RESET} $*"; }
warn() { echo "  ${C_Y}!${C_RESET} $*"; }
err()  { echo "  ${C_R}✗${C_RESET} $*" >&2; }
die()  { err "$*"; exit 1; }

# ── Argümanlar ───────────────────────────────────────────────────────────────
ACTION="install"
SKIP_DEPS=0
for arg in "$@"; do
    case "$arg" in
        --uninstall|-u)  ACTION="uninstall" ;;
        --reinstall|-r)  ACTION="install" ;;  # zaten varsayılan: önce sil sonra kur
        --no-deps)       SKIP_DEPS=1 ;;
        --help|-h)
            sed -n '2,20p' "$0"; exit 0 ;;
        *) die "Bilinmeyen argüman: $arg (--help için bakın)" ;;
    esac
done

# ── Root kontrolü ────────────────────────────────────────────────────────────
[[ $EUID -eq 0 ]] || die "Lütfen sudo ile çalıştırın: sudo bash setup.sh"

# ── Gerçek kullanıcı ──────────────────────────────────────────────────────────
REAL_USER="${SUDO_USER:-${USER:-}}"
if [[ -z "$REAL_USER" || "$REAL_USER" == "root" ]]; then
    # /home altındaki ilk normal kullanıcıyı dene
    REAL_USER=$(getent passwd | awk -F: '$3>=1000 && $3<65000 {print $1; exit}')
fi
if [[ -z "$REAL_USER" || "$REAL_USER" == "root" ]]; then
    warn "Normal kullanıcı tespit edilemedi; bazı adımlar atlanacak (KDE fix, input grubu)."
    REAL_USER=""
    REAL_HOME=""
else
    REAL_HOME=$(getent passwd "$REAL_USER" | cut -d: -f6)
fi

# ── Distro tespiti ────────────────────────────────────────────────────────────
DISTRO_ID=""
if [[ -f /etc/os-release ]]; then
    # shellcheck disable=SC1091
    . /etc/os-release
    DISTRO_ID="${ID:-}${ID_LIKE:+ ${ID_LIKE}}"
fi

is_arch_like()   { [[ "$DISTRO_ID" =~ (arch|cachyos|manjaro|endeavouros) ]]; }
is_debian_like() { [[ "$DISTRO_ID" =~ (debian|ubuntu|mint|pop) ]]; }
is_fedora_like() { [[ "$DISTRO_ID" =~ (fedora|rhel|centos|nobara) ]]; }

# CachyOS mirrors can occasionally serve a repo DB signed with a key that the
# local pacman rejects until mirrors are re-ranked and the DB is force-refreshed.
install_arch_deps() {
    local packages=(
        base-devel cmake pkgconf libevdev gtk4 polkit systemd python
        qt6-tools
    )

    if pacman -Syu --needed --noconfirm "${packages[@]}"; then
        return
    fi

    warn "pacman başarısız oldu; imza/mirror senkronizasyonu sorunu olabilir."
    if command -v cachyos-rate-mirrors >/dev/null; then
        warn "CachyOS/Arch mirror listeleri yenileniyor: cachyos-rate-mirrors"
        cachyos-rate-mirrors || warn "cachyos-rate-mirrors başarısız oldu; yine de pacman tekrar denenecek."
    else
        warn "cachyos-rate-mirrors bulunamadı; sadece pacman veritabanı zorla yenilenecek."
    fi

    warn "Paket veritabanları zorla yenilenerek bağımlılıklar tekrar kuruluyor..."
    pacman -Syyu --needed --noconfirm "${packages[@]}"
}

# ── 1) Bağımlılıklar ─────────────────────────────────────────────────────────
install_deps() {
    if [[ $SKIP_DEPS -eq 1 ]]; then warn "--no-deps verildi, bağımlılık kurulumu atlandı."; return; fi
    say "[1/7] Bağımlılıklar kuruluyor..."
    if is_arch_like; then
        ok "Arch/CachyOS tespit edildi → pacman"
        install_arch_deps
    elif is_debian_like; then
        ok "Debian/Ubuntu tespit edildi → apt"
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        # qt6-tools-dev-tools sağlar: qdbus6
        apt-get install -y --no-install-recommends \
            build-essential cmake pkg-config libevdev-dev libgtk-4-dev \
            libpolkit-gobject-1-dev systemd udev policykit-1 python3 \
            qt6-tools-dev-tools
    elif is_fedora_like; then
        ok "Fedora/RHEL tespit edildi → dnf"
        # qt6-qttools sağlar: qdbus-qt6
        dnf install -y \
            gcc-c++ cmake pkgconf-pkg-config libevdev-devel gtk4-devel \
            polkit polkit-devel systemd python3 \
            qt6-qttools
    else
        warn "Bilinmeyen dağıtım ($DISTRO_ID); el ile şu paketleri kurun:"
        warn "  g++/clang++, cmake, pkg-config, libevdev (devel), gtk4 (devel),"
        warn "  systemd, polkit, python3"
        warn "Devam etmek için Enter'a basın..."
        read -r
    fi

    # Çekirdek doğrulamaları
    command -v g++ >/dev/null || command -v clang++ >/dev/null \
        || die "C++ derleyici bulunamadı."
    pkg-config --exists libevdev || die "libevdev devel paketi eksik."
}

# ── 2) Eski kurulumu temizle ──────────────────────────────────────────────────
clean_old_install() {
    say "[2/7] Eski kurulum temizleniyor (varsa)..."

    # Servisi durdur ve devre dışı bırak (varsa)
    if systemctl list-unit-files 2>/dev/null | grep -q '^rawaccel\.service'; then
        systemctl stop rawaccel.service 2>/dev/null || true
        systemctl disable rawaccel.service 2>/dev/null || true
        ok "Eski servis durduruldu ve devre dışı bırakıldı."
    fi

    # Çalışan daemon süreçlerini öldür (servis dışı manuel başlatılmış olabilir)
    pkill -TERM -x rawaccel-daemon 2>/dev/null || true
    sleep 0.3
    pkill -KILL -x rawaccel-daemon 2>/dev/null || true

    # Açık GUI'leri kapat (eski binary çalışır halde olabilir)
    pkill -TERM -x rawaccel-gui 2>/dev/null || true

    # PID/socket dosyalarını temizle
    rm -f /run/rawaccel.pid /run/rawaccel.sock /tmp/rawaccel.pid /tmp/rawaccel.sock 2>/dev/null || true

    # Eski binary, servis, udev, polkit, desktop dosyaları
    local files=(
        /usr/local/bin/rawaccel-daemon
        /usr/local/bin/rawaccel-cli
        /usr/local/bin/rawaccel-gui
        /usr/bin/rawaccel-daemon
        /usr/bin/rawaccel-cli
        /usr/bin/rawaccel-gui
        /etc/systemd/system/rawaccel.service
        /etc/systemd/user/rawaccel.service
        /usr/lib/systemd/system/rawaccel.service
        /etc/udev/rules.d/99-rawaccel.rules
        /etc/modules-load.d/rawaccel.conf
        /usr/share/applications/rawaccel.desktop
        /usr/share/polkit-1/actions/org.rawaccel.policy
        /usr/share/polkit-1/rules.d/49-rawaccel.rules
    )
    for f in "${files[@]}"; do
        [[ -e "$f" ]] && rm -f "$f" && ok "Silindi: $f"
    done
    systemctl daemon-reload 2>/dev/null || true
    udevadm control --reload-rules 2>/dev/null || true
}

# ── 3) Build ──────────────────────────────────────────────────────────────────
build_project() {
    say "[3/7] Derleniyor..."
    [[ -x "$ROOT/scripts/build.sh" ]] || die "scripts/build.sh bulunamadı."
    if [[ -n "$REAL_USER" ]]; then
        sudo -u "$REAL_USER" bash "$ROOT/scripts/build.sh"
    else
        bash "$ROOT/scripts/build.sh"
    fi
    [[ -x "$ROOT/build-manual/rawaccel-daemon" ]] || die "Build başarısız."
}

# ── 4) Yeni kurulum ───────────────────────────────────────────────────────────
do_install() {
    say "[4/7] Sistem dosyaları yükleniyor..."

    install -Dm755 "$ROOT/build-manual/rawaccel-daemon" /usr/local/bin/rawaccel-daemon
    install -Dm755 "$ROOT/build-manual/rawaccel-cli"    /usr/local/bin/rawaccel-cli
    [[ -f "$ROOT/build-manual/rawaccel-gui" ]] && \
        install -Dm755 "$ROOT/build-manual/rawaccel-gui" /usr/local/bin/rawaccel-gui

    # System config
    mkdir -p /etc/rawaccel
    if [[ ! -f /etc/rawaccel/settings.json ]]; then
        cp "$ROOT/config/default.json" /etc/rawaccel/settings.json
        ok "Varsayılan config yazıldı: /etc/rawaccel/settings.json"
    else
        ok "Mevcut config korundu: /etc/rawaccel/settings.json"
    fi

    # Kullanıcı configi varsa, sistem config olarak da kopyala (daemon görsün).
    # Mevcut /etc/rawaccel/settings.json'ı timestamp'li yedeğe alıp üzerine yaz —
    # böylece "kullanıcı configi sistem configini sessizce ezdi" sürprizini önler.
    if [[ -n "$REAL_HOME" && -f "$REAL_HOME/.config/rawaccel/settings.json" ]]; then
        if [[ -f /etc/rawaccel/settings.json ]] && \
           ! cmp -s "$REAL_HOME/.config/rawaccel/settings.json" /etc/rawaccel/settings.json; then
            local backup="/etc/rawaccel/settings.json.bak.$(date +%Y%m%d-%H%M%S)"
            cp /etc/rawaccel/settings.json "$backup"
            warn "Mevcut sistem config farklı; yedeklendi: $backup"
        fi
        cp "$REAL_HOME/.config/rawaccel/settings.json" /etc/rawaccel/settings.json
        ok "Kullanıcı config /etc/rawaccel/'a senkronlandı."
    fi

    # System service /etc/rawaccel/settings.json okur; GUI/CLI de daemon
    # çalışıyorsa aynı yolu IPC'den öğrenip oraya yazar. Atomic save_config()
    # temp dosya oluşturup rename yaptığı için dizinin de group-writable olması
    # gerekir.
    if getent group input >/dev/null; then
        chgrp input /etc/rawaccel /etc/rawaccel/settings.json
        chmod 2775 /etc/rawaccel
        chmod 664 /etc/rawaccel/settings.json
        ok "/etc/rawaccel input grubu tarafından yazılabilir yapıldı."
    fi

    # uinput modülü
    echo "uinput" > /etc/modules-load.d/rawaccel.conf
    modprobe uinput 2>/dev/null || warn "uinput modülü yüklenemedi (yeniden başlatma gerekebilir)."

    # udev kuralı
    install -Dm644 "$ROOT/scripts/99-rawaccel.rules" /etc/udev/rules.d/99-rawaccel.rules
    udevadm control --reload-rules
    udevadm trigger

    # libinput quirk: RawAccel sanal cihazını "trackball" olarak işaretle
    # → libinput kendi accel curve'ünü uygulamaz (ModelTrackball=1 → flat default).
    # Compositor-agnostik: KDE/GNOME/sway/X11 hepsi libinput kullanıyor.
    if [[ -d /etc/libinput ]] || [[ -d /usr/share/libinput ]]; then
        install -Dm644 "$ROOT/scripts/rawaccel.quirks" \
            /etc/libinput/local-overrides.quirks
        ok "libinput quirk yüklendi: /etc/libinput/local-overrides.quirks"
    fi

    # polkit
    install -Dm644 "$ROOT/scripts/polkit/org.rawaccel.policy" \
        /usr/share/polkit-1/actions/org.rawaccel.policy
    install -Dm644 "$ROOT/scripts/polkit/49-rawaccel.rules" \
        /usr/share/polkit-1/rules.d/49-rawaccel.rules

    # desktop entry
    [[ -f "$ROOT/build-manual/rawaccel-gui" ]] && \
        install -Dm644 "$ROOT/scripts/rawaccel.desktop" \
            /usr/share/applications/rawaccel.desktop

    # systemd servis
    install -Dm644 "$ROOT/scripts/rawaccel.service" /etc/systemd/system/rawaccel.service
    systemctl daemon-reload

    # input grubu
    if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]]; then
        if id -nG "$REAL_USER" | grep -qw input; then
            ok "$REAL_USER zaten 'input' grubunda."
        else
            usermod -aG input "$REAL_USER"
            ok "$REAL_USER 'input' grubuna eklendi (oturum açıp kapatmanız gerek)."
        fi
    fi

    ok "Sistem dosyaları yüklendi."
}

# ── 5) Servisi başlat ─────────────────────────────────────────────────────────
# Servis önce başlamalı ki "RawAccel" sanal uinput cihazı oluşsun;
# KDE fix adımı bu sanal cihaz için per-device override yazacak.
start_service() {
    say "[5/7] Servis başlatılıyor..."
    systemctl enable --now rawaccel.service
    sleep 2 # virtual device'ın evdev/proc'a yansıması için kısa bekle
    if systemctl is-active --quiet rawaccel.service; then
        ok "Servis ÇALIŞIYOR."
    else
        warn "Servis başlatılamadı — günlüğü inceleyin: journalctl -u rawaccel -n 50"
    fi
}

# ── 6) KDE Plasma fix (Wayland/X11) ──────────────────────────────────────────
# Hem global [Libinput] hem de RawAccel sanal cihazı için per-device override
# yazar. KDE Plasma 6 per-device sections (ör. [Libinput][3][1133][50498][...])
# global ayarı override eder — ikisini birden yazmazsak çift hızlanma yaşanır.
fix_kde_plasma() {
    say "[6/7] KDE Plasma kontrolü..."
    if [[ -z "$REAL_HOME" ]]; then warn "Kullanıcı yok, KDE fix atlandı."; return; fi

    local kwinrc="$REAL_HOME/.config/kwinrc"
    local is_kde=0
    if [[ -f "$kwinrc" ]] || command -v plasmashell &>/dev/null \
        || command -v kwin_wayland &>/dev/null || command -v kwin_x11 &>/dev/null; then
        is_kde=1
    fi
    if [[ $is_kde -eq 0 ]]; then
        warn "KDE Plasma tespit edilmedi — atlandı."
        return
    fi

    ok "KDE Plasma tespit edildi."
    # kde-fix-accel.sh global + per-device override yazar; gerçek kullanıcı olarak
    # çalıştırılmalı (kwinrc kullanıcının ev dizininde, sahibinin user olması lazım).
    if [[ -x "$ROOT/scripts/kde-fix-accel.sh" ]]; then
        sudo -u "$REAL_USER" bash "$ROOT/scripts/kde-fix-accel.sh" --fix \
            || warn "kde-fix-accel.sh hata verdi"
    else
        warn "scripts/kde-fix-accel.sh bulunamadı; KDE fix atlandı."
    fi
}

# ── 7) Özet ───────────────────────────────────────────────────────────────────
print_summary() {
    say "[7/7] Kurulum tamamlandı."
    echo ""
    # KDE Plasma 6 için tek-seferlik manuel adım (mimari sınır)
    if command -v kwin_wayland &>/dev/null || command -v plasmashell &>/dev/null; then
        echo "${C_BOLD}${C_Y}═══════════════════════════════════════════════════════════════${C_RESET}"
        echo "${C_BOLD}${C_Y}  ÖNEMLİ — KDE Plasma 6 için TEK SEFERLİK manuel adım${C_RESET}"
        echo "${C_BOLD}${C_Y}═══════════════════════════════════════════════════════════════${C_RESET}"
        echo ""
        echo "  KDE, yeni keşfedilen sanal mouse'a kendi acceleration'ını uyguluyor."
        echo "  Bunu kapatmak için ${C_BOLD}bir kere${C_RESET} şu adımı yapın:"
        echo ""
        echo "    1. ${C_BOLD}System Settings → Fare ve Dokunmatik Yüzey${C_RESET}"
        echo "    2. Aygıt seç → ${C_BOLD}\"... (RawAccel)\"${C_RESET}"
        echo "    3. ${C_BOLD}\"İşaretçi ivmelendirmesini etkinleştir\"${C_RESET} → AÇ → KAPAT → Uygula"
        echo ""
        echo "  Bu ayar ${C_BOLD}kalıcıdır${C_RESET} — reboot/restart sonrası kaybolmaz."
        echo "  RawAccel zaten doğru kcminputrc'yi yazdı, KWin'in okumasını"
        echo "  bir kere tetiklemek için bu manuel toggle gerekiyor."
        echo ""
        echo "${C_BOLD}${C_Y}═══════════════════════════════════════════════════════════════${C_RESET}"
        echo ""
    fi
    echo "${C_BOLD}Komutlar:${C_RESET}"
    echo "  rawaccel-gui              — GUI'yi aç"
    echo "  rawaccel-cli list         — profilleri listele"
    echo "  rawaccel-cli --help       — CLI yardım"
    echo ""
    echo "${C_BOLD}Servis kontrolü:${C_RESET}"
    echo "  systemctl status rawaccel"
    echo "  journalctl -u rawaccel -f"
    echo ""
    echo "${C_BOLD}Config:${C_RESET}"
    echo "  /etc/rawaccel/settings.json       (servis/daemon kullanıyor)"
    echo "  GUI/CLI daemon çalışıyorsa aynı dosyayı düzenler;"
    echo "  ~/.config/rawaccel/settings.json  (daemon yokken kullanıcı fallback'i)"
    echo ""
    if [[ -n "$REAL_USER" && "$REAL_USER" != "root" ]] && ! id -nG "$REAL_USER" | grep -qw input; then
        warn "$REAL_USER 'input' grubuna eklendi ama mevcut oturumda etkin değil."
        warn "Bir kez ${C_BOLD}çıkış-giriş${C_RESET} yapın (veya yeniden başlatın)."
        echo ""
    fi
    echo "${C_BOLD}Servis durumu:${C_RESET}"
    systemctl status rawaccel --no-pager 2>&1 | head -8 || true
}

# ── Uninstall yolu ────────────────────────────────────────────────────────────
do_uninstall() {
    say "Kaldırılıyor..."
    clean_old_install
    rm -rf /etc/rawaccel
    ok "Kaldırma tamamlandı. Kullanıcı configi (~/.config/rawaccel) korundu."
    echo "  Tamamen silmek için: rm -rf $REAL_HOME/.config/rawaccel"
}

# ── Akış ──────────────────────────────────────────────────────────────────────
echo ""
echo "${C_BOLD}${C_M}╔═══════════════════════════════════════════════╗${C_RESET}"
echo "${C_BOLD}${C_M}║        RawAccel Linux — Otomatik Kurulum     ║${C_RESET}"
echo "${C_BOLD}${C_M}╚═══════════════════════════════════════════════╝${C_RESET}"
echo "  Distro: ${DISTRO_ID:-unknown}    Kullanıcı: ${REAL_USER:-(yok)}"
echo "  İşlem : $ACTION"
echo ""

if [[ "$ACTION" == "uninstall" ]]; then
    do_uninstall
    exit 0
fi

install_deps
clean_old_install   # her zaman önce temizle (eski/bozuk varsa)
build_project
do_install
start_service       # servis önce başlasın → sanal RawAccel cihazı oluşsun
fix_kde_plasma      # sonra per-device kwinrc override yazılsın
print_summary
