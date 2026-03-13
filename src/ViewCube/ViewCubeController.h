#ifndef VIEWCUBECONTROLLER_H
#define VIEWCUBECONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QPointer>

class OgreWidget;
class SpaceCamera;
class QWidget;

class ViewCubeController : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(qreal qw READ qw NOTIFY orientationChanged)
    Q_PROPERTY(qreal qx READ qx NOTIFY orientationChanged)
    Q_PROPERTY(qreal qy READ qy NOTIFY orientationChanged)
    Q_PROPERTY(qreal qz READ qz NOTIFY orientationChanged)
    Q_PROPERTY(int windowX READ windowX NOTIFY positionChanged)
    Q_PROPERTY(int windowY READ windowY NOTIFY positionChanged)
    Q_PROPERTY(bool visible READ isVisible NOTIFY visibilityChanged)

public:
    explicit ViewCubeController(QWidget* mainWindow, QObject* parent = nullptr);

    static ViewCubeController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static ViewCubeController* instance();

    qreal qw() const { return m_qw; }
    qreal qx() const { return m_qx; }
    qreal qy() const { return m_qy; }
    qreal qz() const { return m_qz; }
    int windowX() const { return m_windowX; }
    int windowY() const { return m_windowY; }
    bool isVisible() const { return m_visible && m_activeWidget; }

    Q_INVOKABLE void snapToView(const QString& face);
    Q_INVOKABLE void snapToDirection(qreal dx, qreal dy, qreal dz);
    Q_INVOKABLE void rotateByDelta(qreal dx, qreal dy);

    void setActiveWidget(OgreWidget* widget);
    void updateOrientation();
    void setVisible(bool visible);

signals:
    void orientationChanged();
    void positionChanged();
    void visibilityChanged(bool visible);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void reposition();
    SpaceCamera* activeCamera() const;

    static ViewCubeController* s_instance;
    QWidget* m_mainWindow = nullptr;

    qreal m_qw = 1.0;
    qreal m_qx = 0.0;
    qreal m_qy = 0.0;
    qreal m_qz = 0.0;
    int m_windowX = 0;
    int m_windowY = 0;
    bool m_visible = false;
    QPointer<OgreWidget> m_activeWidget;
};

#endif // VIEWCUBECONTROLLER_H
