#!/usr/bin/env bash
# One-time Linux/WSL setup for FioriVoce development (Ubuntu/Debian). Safe to re-run.
#   bash scripts/setup-wsl.sh
set -euo pipefail

# nasm, the ALSA headers and the kernel headers are what vcpkg needs to build FFmpeg,
# PortAudio and OpenSSL for the linux-core preset.
PACKAGES=(build-essential cmake ninja-build gdb git pkg-config curl zip unzip tar
          clang clang-format clang-tidy
          nasm libasound2-dev linux-libc-dev)
VCPKG_DIR="${VCPKG_ROOT:-$HOME/vcpkg}"

# --- Compiler, CMake, Ninja, debugger, clang tools ---------------------------
missing=()
for pkg in "${PACKAGES[@]}"; do
    dpkg -s "$pkg" >/dev/null 2>&1 || missing+=("$pkg")
done
if ((${#missing[@]})); then
    echo "Installing: ${missing[*]}"
    SUDO=sudo
    [ "$(id -u)" -eq 0 ] && SUDO=
    $SUDO apt-get update
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y "${missing[@]}"
fi
echo "[ok] packages"

if [ "$(id -u)" -eq 0 ]; then
    echo "Running as root: packages only. Run again as your own user to set up vcpkg."
    exit 0
fi

# --- vcpkg (kept on the Linux filesystem, where it is much faster) -----------
if [ ! -d "$VCPKG_DIR/.git" ]; then
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_DIR"
fi
if [ ! -x "$VCPKG_DIR/vcpkg" ]; then
    "$VCPKG_DIR/bootstrap-vcpkg.sh" -disableMetrics
fi
if ! grep -q 'VCPKG_ROOT=' "$HOME/.profile" 2>/dev/null; then
    printf '\n# vcpkg (added by fiorivoce setup-wsl.sh)\nexport VCPKG_ROOT="%s"\n' "$VCPKG_DIR" >> "$HOME/.profile"
fi
echo "[ok] vcpkg: $VCPKG_DIR (VCPKG_ROOT exported from ~/.profile)"

echo
echo "Done. Open a new shell so VCPKG_ROOT is visible, then see BUILDING.md."
