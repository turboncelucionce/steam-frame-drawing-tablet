/*
    otdtablet: a KWin plugin that feeds OpenTabletDriver's virtual pen into KWin as a real
    tablet, with pressure and tilt.

    Why: on the Steam Frame the desktop's KWin runs nested inside gamescope, so it never reads
    /dev/input itself, and gamescope doesn't pass tablet events on. This plugin opens
    OpenTabletDriver's virtual tablet (a uinput device) from inside KWin and emits KWin's own
    tablet tool events, exactly as KWin's libinput backend would. KWin then speaks the Wayland
    tablet protocol to apps (and Xwayland turns it into X11 tablet devices for X11 apps).

    SPDX-License-Identifier: GPL-2.0-or-later (it links against KWin)
*/

#include "core/inputdevice.h"
#include "input.h"
#include "plugin.h"
#include "workspace.h"

#include <QDir>
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

class OtdTabletDevice : public InputDevice
{
    Q_OBJECT
public:
    OtdTabletDevice(int fd, const QString &path, const QString &name)
        : m_fd(fd)
        , m_path(path)
        , m_name(name)
        , m_pen(new OtdTool(InputDeviceTabletTool::Pen, 0x07D0001, this))
        , m_eraser(new OtdTool(InputDeviceTabletTool::Eraser, 0x07D0002, this))
        , m_notifier(fd, QSocketNotifier::Read)
    {
        for (int code : {ABS_X, ABS_Y, ABS_PRESSURE, ABS_TILT_X, ABS_TILT_Y}) {
            input_absinfo info{};
            if (ioctl(fd, EVIOCGABS(code), &info) == 0) {
                m_ranges[code] = {info.minimum, info.maximum, true};
                m_values[code] = info.value;
            }
        }
        connect(&m_notifier, &QSocketNotifier::activated, this, &OtdTabletDevice::readEvents);
    }

    ~OtdTabletDevice() override { ::close(m_fd); }

    QString sysPath() const override { return m_path; }
    QString name() const override { return m_name; }
    bool isEnabled() const override { return true; }
    void setEnabled(bool) override { }
    bool isKeyboard() const override { return false; }
    bool isPointer() const override { return false; }
    bool isTouchpad() const override { return false; }
    bool isTouch() const override { return false; }
    bool isTabletTool() const override { return true; }
    bool isTabletPad() const override { return false; }
    bool isTabletModeSwitch() const override { return false; }
    bool isLidSwitch() const override { return false; }

    // Leave proximity cleanly before going away, so nothing stays pressed.
    void leave()
    {
        if (m_near) {
            const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
            if (m_tipDown) {
                emitTip(false, now);
            }
            Q_EMIT tabletToolProximityEvent(position(), tiltX(), tiltY(), 0, 0, false, 0, m_activeTool, now, this);
            m_near = false;
        }
    }

Q_SIGNALS:
    void gone();

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

    void handle(const input_event &ev)
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
                report(std::chrono::microseconds(qint64(ev.input_event_sec) * 1000000 + ev.input_event_usec));
            }
            break;
        }
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

    const int m_fd;
    const QString m_path;
    const QString m_name;
    OtdTool *const m_pen;
    OtdTool *const m_eraser;
    OtdTool *m_activeTool = nullptr;
    QSocketNotifier m_notifier;
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
        if (m_device && input() && workspace()) {
            drop();
        }
        delete m_device;
    }

private:
    void scan()
    {
        if (m_device) {
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
            if (ioctl(fd, EVIOCGNAME(sizeof(buf) - 1), buf) >= 0 && s_tabletNames.contains(QString::fromUtf8(buf))) {
                m_device = new OtdTabletDevice(fd, path, QString::fromUtf8(buf));
                connect(m_device, &OtdTabletDevice::gone, this, &OtdTabletPlugin::drop, Qt::QueuedConnection);
                input()->addInputDevice(m_device);
                qWarning("otdtablet: using %s (%s)", qPrintable(path), buf);
                return;
            }
            ::close(fd);
        }
    }

    void drop()
    {
        if (!m_device) {
            return;
        }
        qWarning("otdtablet: %s went away", qPrintable(m_device->sysPath()));
        m_device->leave();
        input()->removeInputDevice(m_device);
        m_device->deleteLater();
        m_device = nullptr;
    }

    QTimer m_scanTimer;
    OtdTabletDevice *m_device = nullptr;
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
