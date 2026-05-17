#ifndef PS1RIPSESSIONWINDOW_H
#define PS1RIPSESSIONWINDOW_H

#include <QMainWindow>

class EmuViewport;
class QLabel;
class PS1RipManager;

/** Temporary host for emulator viewport + transport (#416 / #417). */
class PS1RipSessionWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit PS1RipSessionWindow(QWidget *parent = nullptr);
    ~PS1RipSessionWindow() override;

    static void showSession(QWidget *parent);

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void pickBios();
    void pickIso();
    void onStart();
    void onStop();
    void onPause();
    void onStep();
    void onReset();
    void onFrame(const QImage &frame, quint64 frameIndex);
    void onError(const QString &message);

private:
    void updateFps(quint64 frameIndex);
    void addRecentIso(const QString &path);

    EmuViewport *m_viewport = nullptr;
    QLabel *m_statusLabel = nullptr;
    PS1RipManager *m_manager = nullptr;
    qint64 m_lastFrameMs = 0;
    quint64 m_lastFrameIndex = 0;
    double m_smoothedFps = 0.0;
};

#endif // PS1RIPSESSIONWINDOW_H
