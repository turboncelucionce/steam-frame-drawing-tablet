#!/usr/bin/env python3
"""otd-pen-bridge: feed OpenTabletDriver's virtual pen into the Steam Frame's desktop.

On the Frame, the desktop's KWin runs nested inside gamescope and only sees input that
SteamVR hands it, and SteamVR only opens /dev/input devices when it starts. So the
virtual tablet OpenTabletDriver creates is never read by anyone.

This reads that virtual tablet directly and injects it into the desktop's KWin through
its EIS remote-desktop interface (org.kde.KWin.EIS.RemoteDesktop + libei):
  pen position -> absolute pointer position on the desktop (whole desktop area)
  pen tip      -> left button
  pen button 1 -> right button, pen button 2 -> middle button
No pressure: the libei protocol has no tablet support, so apps see a mouse.

Run it inside the desktop session (KDE autostart), so it talks to that session's DBus.
Pass -v to log every event.
"""
import ctypes
import ctypes.util
import fcntl
import os
import select
import struct
import sys
import time

from gi.repository import Gio, GLib

VERBOSE = "-v" in sys.argv
TABLET_NAMES = ("OpenTabletDriver Virtual Artist Tablet", "OpenTabletDriver Virtual Tablet")

EV_SYN, EV_KEY, EV_ABS = 0, 1, 3
SYN_REPORT = 0
ABS_X, ABS_Y = 0, 1
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE = 0x110, 0x111, 0x112
BTN_TOOL_PEN, BTN_TOOL_RUBBER, BTN_TOUCH, BTN_STYLUS, BTN_STYLUS2 = 0x140, 0x141, 0x14A, 0x14B, 0x14C
PEN_BUTTONS = {BTN_TOUCH: BTN_LEFT, BTN_STYLUS: BTN_RIGHT, BTN_STYLUS2: BTN_MIDDLE}
EVENT = struct.Struct("qqHHi")  # struct input_event on 64-bit


def log(*a):
    print("otd-pen-bridge:", *a, flush=True)


def eviocgabs(code):
    return (2 << 30) | (24 << 16) | (ord("E") << 8) | (0x40 + code)


def eviocgname(n):
    return (2 << 30) | (n << 16) | (ord("E") << 8) | 0x06


# ---------------------------------------------------------------- libei (ctypes)
CAP_POINTER_ABSOLUTE, CAP_BUTTON = 1 << 1, 1 << 5
(EV_CONNECT, EV_DISCONNECT, EV_SEAT_ADDED, EV_SEAT_REMOVED, EV_DEVICE_ADDED,
 EV_DEVICE_REMOVED, EV_DEVICE_PAUSED, EV_DEVICE_RESUMED) = range(1, 9)

ei = ctypes.CDLL(ctypes.util.find_library("ei") or "libei.so.1")
P = ctypes.c_void_p
for name, res, args in [
    ("ei_new_sender", P, [P]),
    ("ei_unref", P, [P]),
    ("ei_configure_name", None, [P, ctypes.c_char_p]),
    ("ei_setup_backend_fd", ctypes.c_int, [P, ctypes.c_int]),
    ("ei_get_fd", ctypes.c_int, [P]),
    ("ei_dispatch", None, [P]),
    ("ei_get_event", P, [P]),
    ("ei_now", ctypes.c_uint64, [P]),
    ("ei_event_get_type", ctypes.c_int, [P]),
    ("ei_event_type_to_string", ctypes.c_char_p, [ctypes.c_int]),
    ("ei_event_get_seat", P, [P]),
    ("ei_event_get_device", P, [P]),
    ("ei_event_unref", P, [P]),
    ("ei_device_ref", P, [P]),
    ("ei_device_unref", P, [P]),
    ("ei_device_get_name", ctypes.c_char_p, [P]),
    ("ei_device_has_capability", ctypes.c_bool, [P, ctypes.c_int]),
    ("ei_device_start_emulating", None, [P, ctypes.c_uint32]),
    ("ei_device_pointer_motion_absolute", None, [P, ctypes.c_double, ctypes.c_double]),
    ("ei_device_button_button", None, [P, ctypes.c_uint32, ctypes.c_bool]),
    ("ei_device_frame", None, [P, ctypes.c_uint64]),
    ("ei_device_get_region", P, [P, ctypes.c_size_t]),
    ("ei_region_get_x", ctypes.c_uint32, [P]),
    ("ei_region_get_y", ctypes.c_uint32, [P]),
    ("ei_region_get_width", ctypes.c_uint32, [P]),
    ("ei_region_get_height", ctypes.c_uint32, [P]),
]:
    f = getattr(ei, name)
    f.restype, f.argtypes = res, args
ei.ei_seat_bind_capabilities.restype = ctypes.c_int  # variadic, NULL-terminated


class KWinEIS:
    """One EIS connection to the desktop's KWin, with its absolute pointer device."""

    def __init__(self):
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION)
        res, fds = self.bus.call_with_unix_fd_list_sync(
            "org.kde.KWin", "/org/kde/KWin/EIS/RemoteDesktop", "org.kde.KWin.EIS.RemoteDesktop",
            "connectToEIS", GLib.Variant("(i)", (2,)),  # 2 = pointer
            GLib.VariantType("(hi)"), Gio.DBusCallFlags.NONE, -1, None, None)
        self.cookie = res[1]
        fd = fds.get(res[0])
        self.ctx = ei.ei_new_sender(None)
        ei.ei_configure_name(self.ctx, b"otd-pen-bridge")
        if ei.ei_setup_backend_fd(self.ctx, fd) != 0:
            raise OSError("ei_setup_backend_fd failed")
        self.device = None
        self.ready = False
        self.sequence = 1
        self.area = (0.0, 0.0, 1280.0, 800.0)

    def fileno(self):
        return ei.ei_get_fd(self.ctx)

    def dispatch(self):
        """Handle EIS events. Returns False once KWin disconnects."""
        ei.ei_dispatch(self.ctx)
        alive = True
        while True:
            ev = ei.ei_get_event(self.ctx)
            if not ev:
                break
            t = ei.ei_event_get_type(ev)
            if VERBOSE:
                log("ei event", ei.ei_event_type_to_string(t).decode())
            if t == EV_SEAT_ADDED:
                ei.ei_seat_bind_capabilities(ctypes.c_void_p(ei.ei_event_get_seat(ev)),
                                             ctypes.c_int(CAP_POINTER_ABSOLUTE),
                                             ctypes.c_int(CAP_BUTTON), ctypes.c_void_p(None))
            elif t == EV_DEVICE_ADDED:
                dev = ei.ei_event_get_device(ev)
                if ei.ei_device_has_capability(dev, CAP_POINTER_ABSOLUTE) and self.device is None:
                    self.device = ei.ei_device_ref(dev)
                    self.area = self._area(dev)
                    log(f"desktop pointer: {ei.ei_device_get_name(dev).decode()}, area {self.area}")
            elif t == EV_DEVICE_RESUMED:
                if ei.ei_event_get_device(ev) == self.device:
                    ei.ei_device_start_emulating(self.device, self.sequence)
                    self.sequence += 1
                    self.ready = True
            elif t == EV_DEVICE_PAUSED:
                if ei.ei_event_get_device(ev) == self.device:
                    self.ready = False
            elif t == EV_DEVICE_REMOVED:
                if ei.ei_event_get_device(ev) == self.device:
                    ei.ei_device_unref(self.device)
                    self.device, self.ready = None, False
            elif t == EV_DISCONNECT:
                alive = False
            ei.ei_event_unref(ev)
        return alive

    @staticmethod
    def _area(dev):
        """Bounding box of all the device's regions (one per desktop screen)."""
        boxes, i = [], 0
        while True:
            r = ei.ei_device_get_region(dev, i)
            if not r:
                break
            x, y = ei.ei_region_get_x(r), ei.ei_region_get_y(r)
            boxes.append((x, y, x + ei.ei_region_get_width(r), y + ei.ei_region_get_height(r)))
            i += 1
        if not boxes:
            return (0.0, 0.0, 1280.0, 800.0)
        x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
        x1, y1 = max(b[2] for b in boxes), max(b[3] for b in boxes)
        return (float(x0), float(y0), float(x1 - x0), float(y1 - y0))

    def send(self, nx, ny, buttons):
        """nx, ny in 0..1 (or None to not move); buttons: [(evdev button, pressed)]."""
        if not self.ready:
            return
        x0, y0, w, h = self.area
        if nx is not None:
            # stay just inside the area: KWin drops positions on the far edge
            x = x0 + min(max(nx, 0.0), 1.0) * (w - 1)
            y = y0 + min(max(ny, 0.0), 1.0) * (h - 1)
            ei.ei_device_pointer_motion_absolute(self.device, x, y)
        for code, pressed in buttons:
            ei.ei_device_button_button(self.device, code, pressed)
        ei.ei_device_frame(self.device, ei.ei_now(self.ctx))
        ei.ei_dispatch(self.ctx)  # flush

    def close(self):
        try:
            self.bus.call_sync("org.kde.KWin", "/org/kde/KWin/EIS/RemoteDesktop",
                               "org.kde.KWin.EIS.RemoteDesktop", "disconnect",
                               GLib.Variant("(i)", (self.cookie,)), None, Gio.DBusCallFlags.NONE, -1, None)
        except GLib.Error:
            pass


# ---------------------------------------------------------------- OTD virtual tablet
class Tablet:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        self.path = path
        self.ranges = {}
        for code in (ABS_X, ABS_Y):
            buf = bytearray(24)
            fcntl.ioctl(self.fd, eviocgabs(code), buf)
            _, lo, hi, *_ = struct.unpack("6i", buf)
            self.ranges[code] = (lo, max(hi, lo + 1))
        self.pos = {ABS_X: None, ABS_Y: None}
        self.moved = False
        self.buttons = []
        self.in_range = False

    def fileno(self):
        return self.fd

    def read(self):
        """Yield (nx, ny, buttons) per report."""
        try:
            data = os.read(self.fd, EVENT.size * 64)
        except BlockingIOError:
            return
        for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
            _, _, typ, code, value = EVENT.unpack_from(data, off)
            if typ == EV_ABS and code in self.pos:
                self.pos[code] = value
                self.moved = True
            elif typ == EV_KEY:
                if code in (BTN_TOOL_PEN, BTN_TOOL_RUBBER):
                    self.in_range = bool(value)
                elif code in PEN_BUTTONS:
                    self.buttons.append((PEN_BUTTONS[code], bool(value)))
            elif typ == EV_SYN and code == SYN_REPORT:
                nx = ny = None
                if self.moved and None not in self.pos.values():
                    (xl, xh), (yl, yh) = self.ranges[ABS_X], self.ranges[ABS_Y]
                    nx = (self.pos[ABS_X] - xl) / (xh - xl)
                    ny = (self.pos[ABS_Y] - yl) / (yh - yl)
                if nx is not None or self.buttons:
                    yield nx, ny, self.buttons
                self.moved, self.buttons = False, []

    def close(self):
        os.close(self.fd)


def find_tablet():
    for n in sorted(os.listdir("/dev/input")):
        if not n.startswith("event"):
            continue
        path = f"/dev/input/{n}"
        try:
            fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
        except OSError:
            continue
        try:
            buf = bytearray(256)
            fcntl.ioctl(fd, eviocgname(len(buf)), buf)
            if buf.split(b"\0", 1)[0].decode(errors="replace") in TABLET_NAMES:
                return path
        except OSError:
            pass
        finally:
            os.close(fd)
    return None


def main():
    kwin = tablet = None
    while True:
        try:
            if kwin is None:
                kwin = KWinEIS()
                log("connected to the desktop's KWin")
            if tablet is None:
                path = find_tablet()
                if path:
                    tablet = Tablet(path)
                    log(f"reading {path}, ranges {tablet.ranges}")
            fds = [kwin] + ([tablet] if tablet else [])
            readable, _, _ = select.select(fds, [], [], 2.0)
            if kwin in readable and not kwin.dispatch():
                log("KWin disconnected")
                kwin.close()
                kwin = None
                continue
            if tablet and tablet in readable:
                for nx, ny, buttons in tablet.read():
                    if VERBOSE:
                        log(f"pen {nx} {ny} {buttons}")
                    kwin.send(nx, ny, buttons)
        except OSError as e:  # tablet gone (OTD restarted) or similar
            log("error:", e)
            if tablet:
                tablet.close()
                tablet = None
            time.sleep(1)
        except GLib.Error as e:  # KWin not there (yet)
            log("can't reach KWin:", e.message)
            kwin = None
            time.sleep(3)


if __name__ == "__main__":
    main()
