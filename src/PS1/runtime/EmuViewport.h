#ifndef EMUVIEWPORT_H
#define EMUVIEWPORT_H

#include <QImage>
#include <QWidget>

/**
 * Software framebuffer view for the PS1 emulator (#416).
 * Independent of OgreWidget / GL. Forwards keyboard/mouse as libretro joypad input.
 */
class EmuViewport : public QWidget
{
    Q_OBJECT

public:
    explicit EmuViewport(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void setIntegerScale(bool enabled);
    bool integerScale() const { return m_integerScale; }
    void setFps(double fps);
    void setShowInputHelp(bool show);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void applyKey(Qt::Key key, bool pressed);
    void applyMouseButton(Qt::MouseButton button, bool pressed);

    QImage m_frame;
    bool m_integerScale = true;
    bool m_showInputHelp = true;
    double m_fps = 0.0;
};

#endif // EMUVIEWPORT_H
