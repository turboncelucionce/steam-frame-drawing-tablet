#!/usr/bin/env bash
# Builds and installs the otdtablet KWin plugin and its setup for the Steam Frame desktop.
# Run it on the Frame from this folder:   bash install.sh
# Run it again after a SteamOS update that changes KWin: the plugin must match KWin exactly.
#
# Everything goes in your home folder; nothing needs sudo.
#   ~/.local/lib/qt6/plugins/kwin/plugins/otdtablet.so   the plugin
#   ~/.config/plasma-workspace/env/otd-tablet.sh          env for KWin (plugin path, cursor)
#   ~/.local/share/otd-tablet/                            gamescope cursor fix
#   ~/.config/autostart/otd-hide-gamescope-cursor.desktop runs that fix when the desktop starts
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
data="$HOME/.local/share/otd-tablet"

echo "==> KWin $(pacman -Q kwin | cut -d' ' -f2)"

echo "==> Building the plugin"
rm -rf "$here/build"
cmake -S "$here" -B "$here/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local/lib/qt6/plugins" >/dev/null
cmake --build "$here/build" -j4
# Unlink the old plugin first instead of overwriting it in place: a running KWin has it
# mapped, and rewriting a mapped library under it can crash KWin.
rm -f "$HOME/.local/lib/qt6/plugins/kwin/plugins/otdtablet.so"
cmake --install "$here/build"

echo "==> Installing the desktop environment script"
mkdir -p "$HOME/.config/plasma-workspace/env"
install -m 644 "$here/otd-tablet-env.sh" "$HOME/.config/plasma-workspace/env/otd-tablet.sh"

echo "==> Installing the gamescope cursor fix"
mkdir -p "$data" "$HOME/.config/autostart"
install -m 644 "$here/blank.xbm" "$data/blank.xbm"
install -m 755 "$here/hide-gamescope-cursor.sh" "$data/hide-gamescope-cursor.sh"
cat > "$HOME/.config/autostart/otd-hide-gamescope-cursor.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Hide gamescope's fallback cursor
Comment=Removes the big X cursor over the desktop (otdtablet setup)
Exec=$data/hide-gamescope-cursor.sh
Terminal=false
NoDisplay=true
X-KDE-autostart-phase=2
EOF

echo
echo "Done. Close the desktop and open it again for KWin to load the plugin."
