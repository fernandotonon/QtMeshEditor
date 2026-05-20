#ifndef EMUVIEWPORT_H
#define EMUVIEWPORT_H

#include <QImage>
#include <QRect>
#include <QSize>
#include <QWidget>

/**
 * Software framebuffer view for the PS1 emulator (#416).
 * Independent of OgreWidget / GL. Forwards keyboard/mouse as libretro joypad input.
 */
class EmuViewport : public QWidget
{
    Q_OBJECT

public:
    /** How the widget fits the PS1 picture into the window. */
    enum class AspectMode {
        Native,   ///< Use the core framebuffer aspect only.
        Display43 ///< Letterbox/pillarbox to 4:3 (NTSC/PAL display aspect).
    };

    struct FrameLayout {
        QRect target;
        bool smoothFiltering = false;
    };

    explicit EmuViewport(QWidget *parent = nullptr);

    void setFrame(const QImage &frame);
    void setIntegerScale(bool enabled);
    bool integerScale() const { return m_integerScale; }
    void setSmoothFiltering(bool enabled);
    bool smoothFiltering() const { return m_smoothFiltering; }
    void setAspectMode(AspectMode mode);
    AspectMode aspectMode() const { return m_aspectMode; }
    void setFps(double fps);
    void setShowInputHelp(bool show);

    /** Pure layout math for unit tests (#416). */
    static FrameLayout computeFrameLayout(const QSize &widgetSize, const QSize &frameSize,
                                        bool integerScale, bool smoothFiltering, AspectMode aspect);

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
    bool m_smoothFiltering = false;
    AspectMode m_aspectMode = AspectMode::Display43;
    bool m_showInputHelp = true;
    double m_fps = 0.0;
};

#endif // EMUVIEWPORT_H
