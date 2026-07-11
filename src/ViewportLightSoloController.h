#pragma once

#include <QHash>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>

#include <unordered_map>

class OgreWidget;

/// Per-viewport runtime light solo — only one light contributes in a viewport
/// while others are temporarily hidden during that viewport's render pass.
class ViewportLightSoloController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList lightNames READ lightNames NOTIFY lightsChanged)
    Q_PROPERTY(QString activeViewportSoloLight READ activeViewportSoloLight WRITE setActiveViewportSoloLight
                   NOTIFY activeViewportSoloChanged)
    Q_PROPERTY(bool hasActiveViewport READ hasActiveViewport NOTIFY activeViewportChanged)

public:
    static ViewportLightSoloController* instance();
    static ViewportLightSoloController* qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine);
    static void kill();

    void registerWidget(OgreWidget* widget);
    void unregisterWidget(OgreWidget* widget);
    void setActiveWidget(OgreWidget* widget);

    QStringList lightNames() const;
    QString activeViewportSoloLight() const;
    void setActiveViewportSoloLight(const QString& lightName);
    bool hasActiveViewport() const;

    /// Called from each widget's render-target listener.
    void beginRenderPass(OgreWidget* widget);
    void endRenderPass(OgreWidget* widget);

signals:
    void lightsChanged();
    void activeViewportSoloChanged();
    void activeViewportChanged();

private:
    explicit ViewportLightSoloController(QObject* parent = nullptr);

    QString soloLightFor(OgreWidget* widget) const;

    static ViewportLightSoloController* s_singleton;
    OgreWidget* m_activeWidget = nullptr;
    std::unordered_map<OgreWidget*, QString> m_soloByWidget;
    QHash<QString, bool> m_savedVisibility;
    bool m_inRenderPass = false;
};
