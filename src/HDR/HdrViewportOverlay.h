#pragma once

#include <QObject>
#include <QPointer>

class OgreWidget;
class QQuickWidget;
class QWidget;

/// Top-right viewport overlay for compact HDR tonemap controls (Slice E, #471).
class HdrViewportOverlay : public QObject
{
    Q_OBJECT

public:
    explicit HdrViewportOverlay(QWidget* mainWindow, QObject* parent = nullptr);
    ~HdrViewportOverlay() override;

    bool isVisible() const;

public slots:
    void setVisible(bool visible);
    void setActiveWidget(OgreWidget* widget);
    void reposition();
    void onEnvironmentStateChanged();

signals:
    void visibilityChanged(bool visible);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

private:
    void initWidget();
    void updateWidgetVisibility();

    QWidget* m_mainWindow = nullptr;
    QPointer<QQuickWidget> m_overlayWidget;
    QPointer<OgreWidget> m_activeWidget;
    bool m_visible = true;
};
