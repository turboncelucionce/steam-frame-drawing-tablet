#!/usr/bin/env bash
# install-otd.sh
# Builds OpenTabletDriver directly in your home folder and sets it up to start on login.
# Made for SteamOS on the Steam Frame (ARM64). Should also work on x86_64 SteamOS.
# Nothing is written to the read-only system partition: everything lives in your home
# folder, except one udev rule in /etc (which is writable and survives updates).
#
# Run it from Konsole with:   bash install-otd.sh
#
# Safe to run again: it re-downloads the latest release, rebuilds, and rewrites the config files.

set -euo pipefail

SRC="$HOME/.local/src/OpenTabletDriver"
BIN="$HOME/otd-bin"
DOTNET_DIR="$HOME/.dotnet"
LOG="$HOME/otd-install.log"
REPO="OpenTabletDriver/OpenTabletDriver"
RULES="/etc/udev/rules.d/70-opentabletdriver.rules"

: > "$LOG"
exec > >(tee -a "$LOG") 2>&1

step() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
ok()   { printf '\033[1;32m    %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
fail() {
    printf '\n\033[1;31m[ERROR] %s\033[0m\n' "$*"
    # If MSBuild crashed, copy its crash reports into the log
    for f in "$HOME"/otd-msbuild-debug/MSBuild_*.failure.txt /tmp/MSBuildTemp*/MSBuild_*.failure.txt; do
        [ -e "$f" ] || continue
        printf '\n----- %s -----\n' "$f"
        head -n 60 "$f"
    done
    printf 'Full log saved to: %s\n' "$LOG"
    exit 1
}
trap 'fail "Something went wrong on line $LINENO. See the end of the log."' ERR

LDCACHE="$( (ldconfig -p || /sbin/ldconfig -p) 2>/dev/null || true)"
has_lib() {
    grep -q "$1" <<< "$LDCACHE" && return 0
    local f
    for f in /usr/lib/"$1"* /usr/lib64/"$1"* /usr/lib/*-linux-gnu/"$1"*; do
        [ -e "$f" ] && return 0
    done
    return 1
}

# ---------------------------------------------------------------------------
step "Checking the system"
# ---------------------------------------------------------------------------
case "$(uname -m)" in
    aarch64) RID="linux-arm64" ;;
    x86_64)  RID="linux-x64" ;;
    *) fail "Unsupported CPU architecture: $(uname -m)" ;;
esac
ok "Architecture: $(uname -m) (building for $RID)"

for c in curl tar jq; do
    command -v "$c" >/dev/null 2>&1 || fail "$c not found."
done
ok "curl, tar and jq found"

if has_lib libevdev.so; then
    ok "libevdev found"
else
    fail "libevdev not found on the system. OpenTabletDriver needs it to create the virtual pen."
fi

HAS_GTK=1
if has_lib libgtk-3.so; then
    ok "GTK3 found (settings window will work)"
else
    HAS_GTK=0
    warn "GTK3 not found: the driver itself will work, but the settings window won't open."
    warn "You can configure it with OpenTabletDriver.Console instead."
fi

if ! sudo -v; then
    fail "sudo didn't work. If you never set a password, run 'passwd' first, then run this script again."
fi

# ---------------------------------------------------------------------------
step "Downloading the latest OpenTabletDriver release"
# ---------------------------------------------------------------------------
curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" -o /tmp/otd-release.json
TAG="$(sed -nE '/"tag_name"/{s/.*"tag_name"[^"]*"([^"]+)".*/\1/p;q}' /tmp/otd-release.json)"
[ -n "$TAG" ] || fail "Couldn't find the latest release tag on GitHub."
ok "Latest release: $TAG"

rm -rf "$SRC"
mkdir -p "$SRC"
curl -fsSL "https://github.com/$REPO/archive/refs/tags/$TAG.tar.gz" \
    | tar -xz -C "$SRC" --strip-components=1
ok "Source extracted to $SRC"

# ---------------------------------------------------------------------------
step "Installing udev rules (asks for your sudo password)"
# ---------------------------------------------------------------------------
# OpenTabletDriver's own generator: rules for every tablet it supports, so the logged-in
# user can read the tablet and create the virtual pen without root.
bash "$SRC/generate-rules.sh" > /tmp/otd-udev.rules
sudo install -m 644 /tmp/otd-udev.rules "$RULES"
sudo rm -f /etc/udev/rules.d/70-otd-wacom.rules  # from older versions of this script
sudo udevadm control --reload-rules
sudo udevadm trigger
ok "Rules installed in $RULES"

FOUND=""
for d in /sys/bus/usb/devices/*; do
    [ -r "$d/idVendor" ] && [ -r "$d/idProduct" ] || continue
    id="$(cat "$d/idVendor"):$(cat "$d/idProduct")"
    if grep -q "idVendor}==\"${id%%:*}\", ATTRS{idProduct}==\"${id##*:}\"" /tmp/otd-udev.rules; then
        FOUND="$(cat "$d/product" 2>/dev/null || echo "$id")"
    fi
done
if [ -n "$FOUND" ]; then
    ok "Supported tablet connected: $FOUND"
else
    warn "No supported tablet connected over USB right now. That's fine, plug it in later."
fi
ok "From here on, nothing else needs your input. The build takes a while."

# ---------------------------------------------------------------------------
step "Installing the .NET SDK (in your home folder)"
# ---------------------------------------------------------------------------
curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh
cd "$SRC"
if [ -f global.json ]; then
    bash /tmp/dotnet-install.sh --jsonfile global.json --install-dir "$DOTNET_DIR"
else
    bash /tmp/dotnet-install.sh --channel 8.0 --install-dir "$DOTNET_DIR"
fi
export DOTNET_ROOT="$DOTNET_DIR"
export PATH="$DOTNET_DIR:$PATH"
export DOTNET_CLI_TELEMETRY_OPTOUT=1
export DOTNET_NOLOGO=1
# Globalization: always run .NET in "invariant" mode (no dependency on the system's
# ICU library, which made MSBuild's worker processes crash on the Frame), but still
# allow named cultures like "en-US", which OpenTabletDriver's code asks for.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
export DOTNET_SYSTEM_GLOBALIZATION_PREDEFINED_CULTURES_ONLY=false
GLOBAL_FLAGS=(-p:InvariantGlobalization=true -p:PredefinedCulturesOnly=false)
# Build in a single process: slower, but uses less RAM and avoids worker crashes.
export MSBUILDDISABLENODEREUSE=1
export MSBUILDDEBUGPATH="$HOME/otd-msbuild-debug"
rm -rf "$MSBUILDDEBUGPATH" /tmp/MSBuildTemp*
mkdir -p "$MSBUILDDEBUGPATH"
ok ".NET SDK $(dotnet --version) ready"

# ---------------------------------------------------------------------------
step "Compiling (this is the slow part)"
# ---------------------------------------------------------------------------
rm -rf "$BIN"
mkdir -p "$BIN"
PROJECTS="OpenTabletDriver.Daemon OpenTabletDriver.Console"
[ "$HAS_GTK" = 1 ] && PROJECTS="$PROJECTS OpenTabletDriver.UX.Gtk"
for p in $PROJECTS; do
    proj="$(find . -name "$p.csproj" -not -path '*/bin/*' -not -path '*/obj/*' -print -quit)"
    [ -n "$proj" ] || fail "Couldn't find $p.csproj in the source."
    echo "    Building $p"
    dotnet publish "$proj" -c Release -r "$RID" --self-contained true \
        "${GLOBAL_FLAGS[@]}" -m:1 -nodeReuse:false -p:UseSharedCompilation=false \
        -o "$BIN"
done
[ -x "$BIN/OpenTabletDriver.Daemon" ] || fail "Daemon binary missing after build."
ok "Binaries are in $BIN"

# ---------------------------------------------------------------------------
step "Setting up autostart and the app menu entry"
# ---------------------------------------------------------------------------
mkdir -p "$HOME/.config/autostart" "$HOME/.local/share/applications"

cat > "$HOME/.config/autostart/opentabletdriver-daemon.desktop" << EOF
[Desktop Entry]
Type=Application
Name=OpenTabletDriver Daemon
Comment=Tablet driver
Exec=$BIN/OpenTabletDriver.Daemon
Icon=input-tablet
Terminal=false
X-KDE-autostart-phase=2
EOF
ok "Daemon will start automatically when you open the desktop"

if [ "$HAS_GTK" = 1 ]; then
    cat > "$HOME/.local/share/applications/opentabletdriver.desktop" << EOF
[Desktop Entry]
Type=Application
Name=OpenTabletDriver
Comment=Tablet settings (the daemon must be running)
Exec=$BIN/OpenTabletDriver.UX.Gtk
Icon=input-tablet
Terminal=false
Categories=Settings;HardwareSettings;
EOF
    ok "Added 'OpenTabletDriver' to the app launcher (settings window)"
fi

# ---------------------------------------------------------------------------
step "Starting OpenTabletDriver"
# ---------------------------------------------------------------------------
if pgrep -f "OpenTabletDriver.Daemon" >/dev/null 2>&1; then
    pkill -f "otd-bin/OpenTabletDriver.Daemon" || true
    sleep 1
fi
nohup "$BIN/OpenTabletDriver.Daemon" > "$HOME/otd-daemon.log" 2>&1 &
disown
sleep 4
if pgrep -f "OpenTabletDriver.Daemon" >/dev/null 2>&1; then
    ok "Daemon running (its output goes to ~/otd-daemon.log)"
else
    fail "The daemon exited right away. Check ~/otd-daemon.log."
fi

trap - ERR
printf '\n\033[1;32mOpenTabletDriver is installed.\033[0m\n'
echo "Tips:"
echo "  - Set the output mode to 'Artist Mode' for pressure sensitivity."
echo "  - If the tablet was plugged in before running this, unplug and replug it."
echo "Log saved to: $LOG"
