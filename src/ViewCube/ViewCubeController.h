#ifndef VIEWCUBECONTROLLER_H
#define VIEWCUBECONTROLLER_H

#include <QObject>
#include <QQmlEngine>
#include <QPointer>

class OgreWidget;
class SpaceCamera;
class QQuickWidget;
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
    Q_PROPERTY(bool visible READ isVisible NOTIFY visibilityChanged)

public:
    explicit ViewCubeController(QWidget* mainWindow, QObject* parent = nullptr);
    ~ViewCubeController() override;

    static ViewCubeController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static ViewCubeController* instance();

    qreal qw() const { return m_qw; }
    qreal qx() const { return m_qx; }
    qreal qy() const { return m_qy; }
    qreal qz() const { return m_qz; }
    bool isVisible() const;

    Q_INVOKABLE void snapToView(const QString& face);
    Q_INVOKABLE void snapToDirection(qreal dx, qreal dy, qreal dz);
    Q_INVOKABLE void rotateByDelta(qreal dx, qreal dy);

    void setActiveWidget(OgreWidget* widget);
    void updateOrientation();
    void setVisible(bool visible);
    void initWidget();

signals:
    void orientationChanged();
    void visibilityChanged(bool visible);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void reposition();
    void updateWidgetVisibility();
    SpaceCamera* activeCamera() const;

    static ViewCubeController* s_instance;
    QWidget* m_mainWindow = nullptr;

    qreal m_qw = 1.0;
    qreal m_qx = 0.0;
    qreal m_qy = 0.0;
    qreal m_qz = 0.0;
    bool m_visible = false;
    QPointer<OgreWidget> m_activeWidget;
    QQuickWidget* m_cubeWidget = nullptr;
};

#endif // VIEWCUBECONTROLLER_H
