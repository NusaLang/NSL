#!/usr/bin/env bash
set -e

bold()  { printf '\033[1m%s\033[0m\n' "$1"; }
info()  { printf '  %s\n' "$1"; }
fail()  { printf '\nGagal: %s\n\n' "$1" >&2; exit 1; }

RELEASE_BASE="https://github.com/NusaLang/NSL/releases/latest/download"

is_android() {
    case "$(uname -o 2>/dev/null)" in Android) return 0 ;; esac
    case "${PREFIX:-}" in */com.termux/*) return 0 ;; esac
    return 1
}

install_prebuilt() {
    ASSET="$1"
    PLUGIN_ASSET="$2"
    command -v curl >/dev/null 2>&1 || fail "curl tidak ditemukan."
    INSTALL_DIR="${NUSA_INSTALL_DIR:-$HOME/.local/bin}"
    mkdir -p "$INSTALL_DIR"

    info "Ambil binary jadi ($ASSET) ..."
    curl -fsSL "$RELEASE_BASE/$ASSET" -o "$INSTALL_DIR/nusa.new" ||
        fail "gagal download -- cek koneksi, atau lihat rilis manual di github.com/NusaLang/NSL/releases"
    chmod +x "$INSTALL_DIR/nusa.new"
    mv "$INSTALL_DIR/nusa.new" "$INSTALL_DIR/nusa"
    info "Terpasang di $INSTALL_DIR/nusa"

    if [ -n "$PLUGIN_ASSET" ]; then
        info "Ambil plugin native ($PLUGIN_ASSET) ..."
        rm -rf "$INSTALL_DIR/nusantara-plugins"
        mkdir -p "$INSTALL_DIR/nusantara-plugins"
        curl -fsSL "$RELEASE_BASE/$PLUGIN_ASSET" | tar -xz -C "$INSTALL_DIR/nusantara-plugins" ||
            info "Gagal ambil plugin -- nusa tetap jalan, cuma fitur muat_plugin() yang kena."
    fi
}

add_path_line() {
    RC="$1"
    MARK="# Nusantara (nusa)"
    touch "$RC"
    if ! grep -qF "$MARK" "$RC"; then
        {
            echo ""
            echo "$MARK"
            echo "export PATH=\"$INSTALL_DIR:\$PATH\""
        } >> "$RC"
        info "PATH ditambahin ke $RC."
    fi
}

UNAME_S="$(uname -s)"
UNAME_M="$(uname -m)"

case "$UNAME_S" in
    MINGW*|MSYS*|CYGWIN*)
        bold "Nusantara -- Windows"
        INSTALL_DIR="${NUSA_INSTALL_DIR:-$HOME/.nusa}"
        mkdir -p "$INSTALL_DIR"
        info "Ambil binary jadi ..."
        curl -fsSL "$RELEASE_BASE/nusa-windows-x86_64.exe" -o "$INSTALL_DIR/nusa.exe" ||
            fail "gagal download -- cek koneksi, atau lihat rilis manual di github.com/NusaLang/NSL/releases"
        chmod +x "$INSTALL_DIR/nusa.exe"
        setx PATH "%PATH%;$(cygpath -w "$INSTALL_DIR" 2>/dev/null || echo "$INSTALL_DIR")" >/dev/null 2>&1 || true
        echo
        bold "Selesai."
        info "Terpasang di $INSTALL_DIR/nusa.exe -- buka terminal baru, terus:"
        info "  nusa run examples/hello.ns"
        info ""
        info "PowerShell native (bukan Git Bash/MSYS)? Pakai install.ps1 langsung."
        exit 0
        ;;
esac

bold "Nusantara -- install"
echo

if [ "$UNAME_S" = "Linux" ] && is_android; then
    install_prebuilt "nusa-android-arm64" ""
elif [ "$UNAME_S" = "Linux" ] && [ "$UNAME_M" = "x86_64" ]; then
    install_prebuilt "nusa-linux-x86_64" "nusantara-plugins-linux-x86_64.tar.gz"
elif [ "$UNAME_S" = "Linux" ] && [ "$UNAME_M" = "aarch64" ]; then
    install_prebuilt "nusa-linux-arm64" "nusantara-plugins-linux-arm64.tar.gz"
else
    command -v make >/dev/null 2>&1 || fail "make tidak ditemukan (belum ada binary jadi buat $UNAME_S/$UNAME_M)."
    command -v g++ >/dev/null 2>&1 || fail "g++ tidak ditemukan (belum ada binary jadi buat $UNAME_S/$UNAME_M)."
    info "Belum ada binary jadi buat $UNAME_S/$UNAME_M -- build dari source ..."
    make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
    make plugins || info "Sebagian/semua plugin gagal dibuild -- nusa tetap jalan, cuma fitur muat_plugin() yang kena."
    INSTALL_DIR="${NUSA_INSTALL_DIR:-$HOME/.local/bin}"
    mkdir -p "$INSTALL_DIR"
    cp nusantara "$INSTALL_DIR/nusa.new"
    mv "$INSTALL_DIR/nusa.new" "$INSTALL_DIR/nusa"
    chmod +x "$INSTALL_DIR/nusa"
    info "Terpasang di $INSTALL_DIR/nusa"
    if [ -d nusantara-plugins ]; then
        rm -rf "$INSTALL_DIR/nusantara-plugins"
        cp -rL nusantara-plugins "$INSTALL_DIR/nusantara-plugins"
    fi
fi

case "$UNAME_S" in
    Linux)
        add_path_line "$HOME/.bashrc"
        [ -f "$HOME/.zshrc" ] && add_path_line "$HOME/.zshrc"
        ;;
    Darwin)
        add_path_line "$HOME/.zshrc"
        [ -f "$HOME/.bash_profile" ] && add_path_line "$HOME/.bash_profile"
        ;;
esac

echo
bold "Selesai."
info "Buka terminal baru (atau source ~/.bashrc / ~/.zshrc), terus:"
info "  nusa run examples/hello.ns"
