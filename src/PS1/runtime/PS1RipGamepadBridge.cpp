#include "PS1RipGamepadBridge.h"
#include "PsxJoypadState.h"

#include <QTimer>

#ifdef HAVE_QT_GAMEPAD
#include <QGamepad>
#include <QGamepadManager>
#endif

namespace {

constexpr unsigned kPort = 0;
constexpr double kAxisDeadzone = 0.45;

void setBtn(unsigned id, bool pressed)
{
    PsxJoypadState::setPressed(kPort, id, pressed);
}

} // namespace

PS1RipGamepadBridge::PS1RipGamepadBridge(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
#ifdef HAVE_QT_GAMEPAD
    connect(m_timer, &QTimer::timeout, this, &PS1RipGamepadBridge::poll);
    m_timer->setInterval(16);

    auto *manager = QGamepadManager::instance();
    connect(manager, &QGamepadManager::gamepadConnected, this, [this](int deviceId) {
        if (m_deviceId < 0) {
            m_deviceId = deviceId;
            m_pad = new QGamepad(deviceId, this);
            m_timer->start();
        }
    });
    connect(manager, &QGamepadManager::gamepadDisconnected, this, [this](int deviceId) {
        if (deviceId != m_deviceId)
            return;
        m_timer->stop();
        delete m_pad;
        m_pad = nullptr;
        m_deviceId = -1;
        PsxJoypadState::reset(kPort);
    });

    const QList<int> connected = manager->connectedGamepads();
    if (!connected.isEmpty()) {
        m_deviceId = connected.first();
        m_pad = new QGamepad(m_deviceId, this);
        m_timer->start();
    }
#else
    Q_UNUSED(m_timer);
#endif
}

PS1RipGamepadBridge::~PS1RipGamepadBridge()
{
    m_timer->stop();
    PsxJoypadState::reset(kPort);
}

bool PS1RipGamepadBridge::isActive() const
{
    return m_pad != nullptr;
}

void PS1RipGamepadBridge::poll()
{
    applyGamepad();
}

void PS1RipGamepadBridge::applyGamepad()
{
#ifdef HAVE_QT_GAMEPAD
    if (!m_pad)
        return;

    setBtn(PsxJoypadButton::A, m_pad->buttonA());
    setBtn(PsxJoypadButton::B, m_pad->buttonB());
    setBtn(PsxJoypadButton::X, m_pad->buttonX());
    setBtn(PsxJoypadButton::Y, m_pad->buttonY());
    setBtn(PsxJoypadButton::L, m_pad->buttonL1());
    setBtn(PsxJoypadButton::R, m_pad->buttonR1());
    setBtn(PsxJoypadButton::L2, m_pad->buttonL2());
    setBtn(PsxJoypadButton::R2, m_pad->buttonR2());
    setBtn(PsxJoypadButton::L3, m_pad->buttonL3());
    setBtn(PsxJoypadButton::R3, m_pad->buttonR3());
    setBtn(PsxJoypadButton::Select, m_pad->buttonSelect());
    setBtn(PsxJoypadButton::Start, m_pad->buttonStart());

    const bool dpadUp = m_pad->buttonUp();
    const bool dpadDown = m_pad->buttonDown();
    const bool dpadLeft = m_pad->buttonLeft();
    const bool dpadRight = m_pad->buttonRight();
    const bool axisUp = m_pad->axisLeftY() < -kAxisDeadzone;
    const bool axisDown = m_pad->axisLeftY() > kAxisDeadzone;
    const bool axisLeft = m_pad->axisLeftX() < -kAxisDeadzone;
    const bool axisRight = m_pad->axisLeftX() > kAxisDeadzone;

    setBtn(PsxJoypadButton::Up, dpadUp || axisUp);
    setBtn(PsxJoypadButton::Down, dpadDown || axisDown);
    setBtn(PsxJoypadButton::Left, dpadLeft || axisLeft);
    setBtn(PsxJoypadButton::Right, dpadRight || axisRight);
#endif
}
