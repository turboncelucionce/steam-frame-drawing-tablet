# Pen tablets on the Steam Frame

Use a drawing tablet in the Steam Frame's desktop mode, with **pressure and tilt**, in Krita and other drawing apps.

The Frame's SteamOS doesn't recognize drawing tablets, and its desktop never sees them even with a userspace driver. This repo has two parts: a script that builds [OpenTabletDriver](https://github.com/OpenTabletDriver/OpenTabletDriver) for the Frame, and a small KWin plugin that sends its pen into the desktop as a real tablet.

Tested with a Wacom CTH-670 (Intuos Pen & Touch Medium) and Krita 5.3 (Flatpak), on SteamOS 0.5.8 with KWin 6.6.2. Other tablets that OpenTabletDriver supports should work the same way.

> This is an independent project, not made by or affiliated with Valve, KDE, or the OpenTabletDriver project.

## Install

You need a Steam Frame, an internet connection, a USB tablet, and a `sudo` password (run `passwd` in Konsole first if you never set one).

1. Open the desktop, then **Konsole** from the app menu.
2. Run:

   ```bash
   git clone https://github.com/OWNER/REPO.git ~/steamframe-pentablet
   bash ~/steamframe-pentablet/install.sh
   ```

   It asks for your `sudo` password once, for the udev rules. Then it builds OpenTabletDriver, which takes a while.
3. Close the desktop and open it again from the SteamVR dashboard.
4. Open **OpenTabletDriver** from the app menu and set the output mode to **Artist Mode** (needed for pressure). Turning on **Lock aspect ratio** on the Output tab avoids squashed strokes.
5. Open your drawing app. If it was already open, restart it, since apps look for tablets when they start.

Everything goes in your home folder, apart from the udev rules in `/etc/udev/rules.d/`, which SteamOS updates keep.

### After a SteamOS update

The KWin plugin is built for one exact KWin version. If an update changes KWin, the pen stops working in the desktop until you rebuild it:

```bash
bash ~/steamframe-pentablet/kwin-otd-tablet/install.sh
```

Then restart the desktop. `pacman -Q kwin` shows which KWin you have. OpenTabletDriver itself survives updates.

## How it works

- **No kernel driver.** The Frame's kernel has no `wacom` module, and its root filesystem is read-only. OpenTabletDriver works around that: it reads the tablet through `/dev/hidraw*` and creates a virtual pen through `/dev/uinput`. It has no ARM64 Linux builds, so `install-otd.sh` builds it from source, with workarounds for .NET build crashes on the Frame.
- **The desktop never sees the virtual pen.** The desktop's KWin runs nested inside gamescope, which runs inside SteamVR. It doesn't read `/dev/input` at all and only gets what gamescope passes on: mouse and laser pointer, no tablets. SteamVR only opens input devices when it starts.
- **The KWin plugin** (`kwin-otd-tablet/`) opens OpenTabletDriver's virtual pen from inside KWin and adds it as a tablet device. To KWin, it looks like a tablet from its own libinput backend. KWin then speaks the Wayland tablet protocol to apps, and Xwayland passes it to X11 apps such as Krita. A script in `~/.config/plasma-workspace/env/` adds the plugin's folder to `QT_PLUGIN_PATH`, so KWin finds it.
- **Cursor fixes.** Normally gamescope draws the desktop's cursor where its own pointer is, so the cursor wouldn't follow the pen. `KWIN_FORCE_SW_CURSOR=1` makes KWin draw it instead. `XCURSOR_SIZE=32` fixes its size, because the Steam session sets 256 and only gamescope scaled that down. With KWin drawing the cursor, gamescope shows its X server's default cross under the controller laser. `hide-gamescope-cursor.sh` blanks that cross every time the desktop starts.

## Files

| Path | What |
| --- | --- |
| `install.sh` | Runs both installers below |
| `install-otd.sh` | Builds and installs OpenTabletDriver, its udev rules and autostart |
| `kwin-otd-tablet/otdtablet.cpp` | The KWin plugin |
| `kwin-otd-tablet/install.sh` | Builds and installs the plugin, the env script and the cursor fix |
| `kwin-otd-tablet/otd-tablet-env.sh` | Installed as `~/.config/plasma-workspace/env/otd-tablet.sh` |
| `kwin-otd-tablet/hide-gamescope-cursor.sh`, `blank.xbm` | The gamescope cursor fix, started from `~/.config/autostart/` |
| `extras/otd-pen-bridge.py` | An earlier approach: injects the pen as a mouse through KWin's EIS interface. No pressure, but no compiling either |

## Uninstall

```bash
rm -f ~/.local/lib/qt6/plugins/kwin/plugins/otdtablet.so \
      ~/.config/plasma-workspace/env/otd-tablet.sh \
      ~/.config/autostart/otd-hide-gamescope-cursor.desktop \
      ~/.config/autostart/opentabletdriver-daemon.desktop \
      ~/.local/share/applications/opentabletdriver.desktop
rm -rf ~/.local/share/otd-tablet ~/otd-bin ~/.local/src/OpenTabletDriver ~/.dotnet
sudo rm -f /etc/udev/rules.d/70-opentabletdriver.rules
```

Then restart the desktop.

## Known limits

- Only tested in SteamOS's own desktop. [Frametop](https://github.com/Frametop/frametop)'s desktop also runs KWin, so the plugin probably works there, but that's untested.
- Tablet buttons (express keys) and touch aren't passed on. The pen, its two buttons and the eraser are.
- The pen maps onto the whole desktop. To map it to part of the screen, change the display area in OpenTabletDriver.

## License

GPL-2.0-or-later, since the plugin links against KWin. See [LICENSE](LICENSE).
