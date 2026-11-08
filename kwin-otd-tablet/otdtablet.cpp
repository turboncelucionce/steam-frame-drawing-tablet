/*
    otdtablet: a KWin plugin that feeds OpenTabletDriver's virtual devices into KWin:
    its pen as a real tablet (pressure, tilt), and its keyboard, which sends the shortcuts
    bound to the tablet's express keys and pen buttons.

    Why: on the Steam Frame the desktop's KWin runs nested inside gamescope, so it never reads
    /dev/input itself, and gamescope doesn't pass tablet events on. This plugin opens
    OpenTabletDriver's uinput devices from inside KWin and emits KWin's own input events,
    exactly as KWin's libinput backend would. KWin then speaks the Wayland tablet protocol to
    apps (and Xwayland turns it into X11 tablet devices for X11 apps).

    SPDX-License-Identifier: GPL-2.0-or-later (it links against KWin)
*/

#include "core/inputdevice.h"
#include "input.h"
#include "plugin.h"
#include "workspace.h"

#include <QDir>
#include <QSet>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace KWin
{

static const QStringList s_tabletNames = {
    QStringLiteral("OpenTabletDriver Virtual Artist Tablet"),
    QStringLiteral("OpenTabletDriver Virtual Tablet"),
};
static const QString s_keyboardName = QStringLiteral("OpenTabletDriver Virtual Keyboard");

static std::chrono::microseconds eventTime(const input_event &ev)
{
    return std::chrono::microseconds(qint64(ev.input_event_sec) * 1000000 + ev.input_event_usec);
}

static std::chrono::microseconds now()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
}

/**
 * One of OpenTabletDriver's uinput devices, read from inside KWin.
 * Subclasses say what kind of device it is and turn its events into KWin's.
 */
class OtdDevice : public InputDevice
{
    Q_OBJECT
public:
    OtdDevice(int fd, const QString &path, const QString &name)
        : m_fd(fd)
        , m_path(path)
        , m_name(name)
        , m_notifier(fd, QSocketNotifier::Read)
    {
        connect(&m_notifier, &QSocketNotifier::activated, this, &OtdDevice::readEvents);
    }

    ~OtdDevice() override { ::close(m_fd); }

    QString sysPath() const override { return m_path; }
    QString name() const override { return m_name; }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override { }
    bool isKeyboard() const override { return false; }
    bool isPointer() const override { return false; }
    bool isTouchpad() const override { return false; }
    bool isTouch() const override { return false; }
    bool isTabletTool() const override { return false; }
    bool isTabletPad() const override { return false; }
    bool isTabletModeSwitch() const override { return false; }
    bool isLidSwitch() const override { return false; }

    // Release whatever is held, before the device goes away.
    virtual void leave() { }

Q_SIGNALS:
    void gone();

protected:
    virtual void handle(const input_event &ev) = 0;

    const int m_fd;

private:
    void readEvents()
    {
        input_event evs[64];
        while (true) {
            const ssize_t n = ::read(m_fd, evs, sizeof(evs));
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    return;
                }
                m_notifier.setEnabled(false);
                Q_EMIT gone();
                return;
            }
            if (n == 0) {
                return;
            }
            for (ssize_t i = 0; i < n / ssize_t(sizeof(input_event)); ++i) {
                handle(evs[i]);
            }
        }
    }

    const QString m_path;
    const QString m_name;
    QSocketNotifier m_notifier;
};

// ---------------------------------------------------------------- pen

class OtdTool : public InputDeviceTabletTool
{
    Q_OBJECT
public:
    OtdTool(Type type, quint64 id, QObject *parent)
        : InputDeviceTabletTool(parent)
        , m_type(type)
        , m_id(id)
    {
    }
    quint64 serialId() const override { return m_id; }
    quint64 uniqueId() const override { return m_id; }
    Type type() const override { return m_type; }
    QList<Capability> capabilities() const override { return {Pressure, Tilt}; }

private:
    Type m_type;
    quint64 m_id;
};

struct AbsRange
{
    int min = 0;
    int max = 1;
    bool present = false;
    double normalized(int v) const { return max > min ? double(v - min) / double(max - min) : 0.0; }
};

class OtdTabletDevice : public OtdDevice
{
    Q_OBJECT
public:
    OtdTabletDevice(int fd, const QString &path, const QString &name)
        : OtdDevice(fd, path, name)
        , m_pen(new OtdTool(InputDeviceTabletTool::Pen, 0x07D0001, this))
        , m_eraser(new OtdTool(InputDeviceTabletTool::Eraser, 0x07D0002, this))
    {
        for (int code : {ABS_X, ABS_Y, ABS_PRESSURE, ABS_TILT_X, ABS_TILT_Y}) {
            input_absinfo info{};
            if (ioctl(fd, EVIOCGABS(code), &info) == 0) {
                m_ranges[code] = {info.minimum, info.maximum, true};
                m_values[code] = info.value;
            }
        }
    }

    bool isTabletTool() const override { return true; }

    // Leave proximity cleanly before going away, so nothing stays pressed.
    void leave() override
    {
        if (m_near) {
            if (m_tipDown) {
                emitTip(false, now());
            }
            Q_EMIT tabletToolProximityEvent(position(), tiltX(), tiltY(), 0, 0, false, 0, m_activeTool, now(), this);
            m_near = false;
        }
    }

protected:
    void handle(const input_event &ev) override
    {
        switch (ev.type) {
        case EV_ABS:
            if (ev.code < ABS_CNT) {
                m_values[ev.code] = ev.value;
                m_axesChanged = true;
            }
            break;
        case EV_KEY:
            switch (ev.code) {
            case BTN_TOOL_PEN:
                m_sawToolKeys = true;
                m_toolPen = ev.value;
                break;
            case BTN_TOOL_RUBBER:
                m_sawToolKeys = true;
                m_toolRubber = ev.value;
                break;
            case BTN_TOUCH:
                m_touch = ev.value;
                break;
            case BTN_STYLUS:
            case BTN_STYLUS2:
            case BTN_STYLUS3:
                m_buttons.append({ev.code, ev.value != 0});
                break;
            }
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT) {
                report(eventTime(ev));
            }
            break;
        }
    }

private:
    QPointF position() const
    {
        const Rect area = workspace()->geometry();
        const double nx = std::clamp(m_ranges[ABS_X].normalized(m_values[ABS_X]), 0.0, 1.0);
        const double ny = std::clamp(m_ranges[ABS_Y].normalized(m_values[ABS_Y]), 0.0, 1.0);
        return QPointF(area.x() + nx * (area.width() - 1), area.y() + ny * (area.height() - 1));
    }
    qreal pressure() const
    {
        return m_ranges[ABS_PRESSURE].present ? std::clamp(m_ranges[ABS_PRESSURE].normalized(m_values[ABS_PRESSURE]), 0.0, 1.0) : (m_tipDown ? 1.0 : 0.0);
    }
    // KWin wants degrees. OpenTabletDriver reports tilt in degrees already when its range fits
    // in +-90; otherwise scale the range onto +-60, a typical pen's limit.
    qreal tilt(int code) const
    {
        const AbsRange &r = m_ranges[code];
        if (!r.present) {
            return 0;
        }
        if (r.min >= -90 && r.max <= 90) {
            return m_values[code];
        }
        return (r.normalized(m_values[code]) * 2.0 - 1.0) * 60.0;
    }
    qreal tiltX() const { return tilt(ABS_TILT_X); }
    qreal tiltY() const { return tilt(ABS_TILT_Y); }

    void emitTip(bool down, std::chrono::microseconds time)
    {
        m_tipDown = down;
        Q_EMIT tabletToolTipEvent(position(), pressure(), tiltX(), tiltY(), 0, 0, down, 0, m_activeTool, time, this);
    }

    void report(std::chrono::microseconds time)
    {
        const bool near = m_sawToolKeys ? (m_toolPen || m_toolRubber) : true;
        OtdTool *tool = m_toolRubber ? m_eraser : m_pen;

        if (m_near && (!near || tool != m_activeTool)) {
            if (m_tipDown) {
                emitTip(false, time);
            }
            Q_EMIT tabletToolProximityEvent(position(), tiltX(), tiltY(), 0, 0, false, 0, m_activeTool, time, this);
            m_near = false;
        }
        if (near && !m_near) {
            m_activeTool = tool;
            m_near = true;
            Q_EMIT tabletToolProximityEvent(position(), tiltX(), tiltY(), 0, 0, true, 0, m_activeTool, time, this);
        }
        if (m_near) {
            if (m_touch != m_tipDown) {
                emitTip(m_touch, time);
            } else if (m_axesChanged) {
                Q_EMIT tabletToolAxisEvent(position(), pressure(), tiltX(), tiltY(), 0, 0, m_tipDown, 0, m_activeTool, time, this);
            }
            for (const auto &[code, pressed] : std::as_const(m_buttons)) {
                Q_EMIT tabletToolButtonEvent(code, pressed, m_activeTool, time, this);
            }
        }
        m_buttons.clear();
        m_axesChanged = false;
    }

    OtdTool *const m_pen;
    OtdTool *const m_eraser;
    OtdTool *m_activeTool = nullptr;
    AbsRange m_ranges[ABS_CNT];
    int m_values[ABS_CNT] = {};
    bool m_sawToolKeys = false;
    bool m_toolPen = false;
    bool m_toolRubber = false;
    bool m_touch = false;
    bool m_near = false;
    bool m_tipDown = false;
    bool m_axesChanged = false;
    QList<std::pair<uint, bool>> m_buttons;
};

// ---------------------------------------------------------------- keyboard

/**
 * OpenTabletDriver's virtual keyboard: it types the key bindings set on the tablet's express
 * keys and pen buttons (Ctrl+Z and so on). Keys go to KWin like a real keyboard's, so they
 * combine with other keyboards' modifiers and KWin does key repeat itself.
 */
class OtdKeyboardDevice : public OtdDevice
{
    Q_OBJECT
public:
    using OtdDevice::OtdDevice;

    bool isKeyboard() const override { return true; }

    void leave() override
    {
        const auto held = m_pressed;
        for (quint32 key : held) {
            Q_EMIT keyChanged(key, KeyboardKeyState::Released, now(), this);
        }
        m_pressed.clear();
    }

protected:
    void handle(const input_event &ev) override
    {
        // value 2 is the kernel's autorepeat: skip it, KWin repeats held keys itself
        if (ev.type != EV_KEY || ev.value > 1) {
            return;
        }
        const bool pressed = ev.value == 1;
        if (pressed == m_pressed.contains(ev.code)) {
            return;
        }
        if (pressed) {
            m_pressed.insert(ev.code);
        } else {
            m_pressed.remove(ev.code);
        }
        Q_EMIT keyChanged(ev.code, pressed ? KeyboardKeyState::Pressed : KeyboardKeyState::Released, eventTime(ev), this);
    }

private:
    QSet<quint32> m_pressed;
};

// ---------------------------------------------------------------- plugin

class OtdTabletPlugin : public Plugin
{
    Q_OBJECT
public:
    OtdTabletPlugin()
    {
        connect(&m_scanTimer, &QTimer::timeout, this, &OtdTabletPlugin::scan);
        m_scanTimer.start(2000);
        scan();
    }

    ~OtdTabletPlugin() override
    {
        // At shutdown KWin may already have destroyed input and the workspace.
        const bool alive = input() && workspace();
        for (OtdDevice **slot : {&m_tablet, &m_keyboard}) {
            OtdDevice *device = std::exchange(*slot, nullptr);
            if (device && alive) {
                device->leave();
                input()->removeInputDevice(device);
            }
            delete device;
        }
    }

private:
    void scan()
    {
        if (m_tablet && m_keyboard) {
            return;
        }
        const QStringList nodes = QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("event*")}, QDir::System);
        for (const QString &node : nodes) {
            const QString path = QStringLiteral("/dev/input/") + node;
            const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }
            char buf[256] = {};
            const QString name = ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf) >= 0 ? QString::fromUtf8(buf) : QString();
            if (!m_tablet && s_tabletNames.contains(name)) {
                add(&m_tablet, new OtdTabletDevice(fd, path, name));
            } else if (!m_keyboard && name == s_keyboardName) {
                add(&m_keyboard, new OtdKeyboardDevice(fd, path, name));
            } else {
                ::close(fd);
            }
        }
    }

    void add(OtdDevice **slot, OtdDevice *device)
    {
        *slot = device;
        connect(device, &OtdDevice::gone, this, [this, slot]() {
            drop(slot);
        }, Qt::QueuedConnection);
        input()->addInputDevice(device);
        qWarning("otdtablet: using %s (%s)", qPrintable(device->sysPath()), qPrintable(device->name()));
    }

    void drop(OtdDevice **slot)
    {
        OtdDevice *device = std::exchange(*slot, nullptr);
        if (!device) {
            return;
        }
        qWarning("otdtablet: %s went away", qPrintable(device->sysPath()));
        device->leave();
        input()->removeInputDevice(device);
        device->deleteLater();
    }

    QTimer m_scanTimer;
    OtdDevice *m_tablet = nullptr;
    OtdDevice *m_keyboard = nullptr;
};

class OtdTabletPluginFactory : public PluginFactory
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID PluginFactory_iid FILE "metadata.json")
    Q_INTERFACES(KWin::PluginFactory)
public:
    std::unique_ptr<Plugin> create() const override
    {
        return std::make_unique<OtdTabletPlugin>();
    }
};

} // namespace KWin

#include "otdtablet.moc"
