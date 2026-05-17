#ifndef EMUVIEWPORT_H
#define EMUVIEWPORT_H

#include <QImage>
#include <QWidget>

/**
 * Software framebuffer view for the PS1 emulator (#416).
 * Independent of OgreWidget / GL.
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

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    QImage m_frame;
    bool m_integerScale = true;
    double m_fps = 0.0;
};

#endif // EMUVIEWPORT_H
