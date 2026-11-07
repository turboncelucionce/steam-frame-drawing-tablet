# Goes in ~/.config/plasma-workspace/env/otd-tablet.sh on the Frame (sourced by startplasma-wayland).
# OpenTabletDriver pen support for the Steam Frame desktop (nested KWin).
# Lets KWin load the otdtablet plugin from ~/.local/lib/qt6/plugins/kwin/plugins,
# which feeds OpenTabletDriver's virtual pen into KWin as a real tablet (pressure, tilt).
export QT_PLUGIN_PATH="$HOME/.local/lib/qt6/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
# Let KWin draw the cursor itself, so it shows where the pen is
# (otherwise gamescope draws it where its own pointer is).
export KWIN_FORCE_SW_CURSOR=1
# The desktop inherits XCURSOR_SIZE=256 from the Steam session, which gamescope scaled down
# when it drew the cursor. Now that KWin draws it, use a normal size (KWin and apps read this).
export XCURSOR_SIZE=32
