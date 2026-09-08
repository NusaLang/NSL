#!/usr/bin/env bash
set -e

bold()  { printf '\033[1m%s\033[0m\n' "$1"; }
info()  { printf '  %s\n' "$1"; }
fail()  { printf '\nGagal: %s\n\n' "$1" >&2; exit 1; }

command -v make >/dev/null 2>&1 || fail "make tidak ditemukan."
command -v g++ >/dev/null 2>&1 || fail "g++ (atau compiler C++17 lain) tidak ditemukan."

bold "Nusantara -- build & install dari source"
echo

info "Build ..."
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

INSTALL_DIR="${NUSA_INSTALL_DIR:-$HOME/.local/bin}"
mkdir -p "$INSTALL_DIR"

if [ -f "$INSTALL_DIR/nusa" ]; then
    # rename atomik -- aman walau binary lama lagi jalan (lihat README)
    cp nusantara "$INSTALL_DIR/nusa.new"
    mv "$INSTALL_DIR/nusa.new" "$INSTALL_DIR/nusa"
else
    cp nusantara "$INSTALL_DIR/nusa"
fi
chmod +x "$INSTALL_DIR/nusa"
info "Terpasang di $INSTALL_DIR/nusa"

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

case "$(uname -s)" in
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
info ""
info "Plugin native (SQLite/HTTP+TLS/dst) opsional, jalanin manual kalau butuh:"
info "  make plugins"
