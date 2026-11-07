#!/bin/sh
# Make gamescope's fallback cursor invisible.
#
# With KWIN_FORCE_SW_CURSOR=1 the desktop's KWin draws its own cursor and gives gamescope an
# empty one. gamescope then shows its X servers' default root cursor instead: a big X11 cross
# wherever the controller laser points. This sets that root cursor to a blank image on
# gamescope's own Xwayland servers (the ones started with -core; the desktop's own is not).
# It doesn't survive a SteamVR restart, so it runs every time the desktop starts.

blank="$HOME/.local/share/otd-tablet/blank.xbm"
for d in $(pgrep -a Xwayland | grep -- ' -core' | grep -oE ' :[0-9]+'); do
    DISPLAY=$d xsetroot -cursor "$blank" "$blank"
done
