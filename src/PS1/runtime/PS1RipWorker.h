#ifndef PS1RIPWORKER_H
#define PS1RIPWORKER_H

#include <QObject>
#include <QString>
#include <QtGlobal>

/**
 * Worker-thread stub for the PS1 emulator core (Phase 1+).
 * Lives on a dedicated QThread owned by PS1RipManager.
 */
class PS1RipWorker : public QObject
{
    Q_OBJECT

public:
    explicit PS1RipWorker(QObject *parent = nullptr);

public slots:
    void startEmulation();
    void stopEmulation();
    void pauseEmulation();
    void stepFrame();

signals:
    void emulationStarted();
    void emulationStopped();
    void frameAdvanced(quint64 frameIndex);

private:
    bool m_running = false;
    bool m_paused = false;
    quint64 m_frameIndex = 0;
};

#endif // PS1RIPWORKER_H
