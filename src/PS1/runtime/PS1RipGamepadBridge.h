#ifndef PS1RIPGAMEPADBRIDGE_H
#define PS1RIPGAMEPADBRIDGE_H

#include <QObject>

class QTimer;

/** Forwards Qt Gamepad (controller 1) input to PsxJoypadState (#417). */
class PS1RipGamepadBridge : public QObject
{
    Q_OBJECT

public:
    explicit PS1RipGamepadBridge(QObject *parent = nullptr);
    ~PS1RipGamepadBridge() override;

    bool isActive() const;

private slots:
    void poll();

private:
    void applyGamepad();

    QTimer *m_timer = nullptr;
    int m_deviceId = -1;
    class QGamepad *m_pad = nullptr;
};

#endif // PS1RIPGAMEPADBRIDGE_H
