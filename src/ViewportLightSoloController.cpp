#include "ViewportLightSoloController.h"

#include "LightManager.h"
#include "OgreWidget.h"
#include "SentryReporter.h"

#include <OgreLight.h>

#include <QList>

ViewportLightSoloController* ViewportLightSoloController::s_singleton = nullptr;

ViewportLightSoloController* ViewportLightSoloController::instance()
{
    if (!s_singleton)
        s_singleton = new ViewportLightSoloController(); // NOSONAR — singleton
    return s_singleton;
}

ViewportLightSoloController* ViewportLightSoloController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void ViewportLightSoloController::kill()
{
    delete s_singleton; // NOSONAR — singleton
    s_singleton = nullptr;
}

ViewportLightSoloController::ViewportLightSoloController(QObject* parent)
    : QObject(parent)
{
    if (auto* lights = LightManager::getSingletonPtr())
    {
        connect(lights, &LightManager::lightCreated, this, &ViewportLightSoloController::lightsChanged);
        connect(lights, &LightManager::lightDeleted, this, &ViewportLightSoloController::lightsChanged);
        connect(lights, &LightManager::lightChanged, this, &ViewportLightSoloController::lightsChanged);
    }
}

void ViewportLightSoloController::registerWidget(OgreWidget* widget)
{
    if (!widget)
        return;
    if (m_soloByWidget.find(widget) == m_soloByWidget.end())
        m_soloByWidget.emplace(widget, QString());
}

void ViewportLightSoloController::unregisterWidget(OgreWidget* widget)
{
    if (!widget)
        return;
    m_soloByWidget.erase(widget);
    if (m_activeWidget == widget)
    {
        m_activeWidget = nullptr;
        emit activeViewportChanged();
        emit activeViewportSoloChanged();
    }
}

void ViewportLightSoloController::setActiveWidget(OgreWidget* widget)
{
    if (m_activeWidget == widget)
        return;
    m_activeWidget = widget;
    emit activeViewportChanged();
    emit activeViewportSoloChanged();
}

QStringList ViewportLightSoloController::lightNames() const
{
    QStringList names;
    if (auto* lights = LightManager::getSingletonPtr())
    {
        for (const LightHandle& handle : lights->lights())
            names.append(handle.name);
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

QString ViewportLightSoloController::activeViewportSoloLight() const
{
    return soloLightFor(m_activeWidget);
}

void ViewportLightSoloController::setActiveViewportSoloLight(const QString& lightName)
{
    if (!m_activeWidget)
        return;

    const QString trimmed = lightName.trimmed();
    m_soloByWidget[m_activeWidget] = trimmed;
    emit activeViewportSoloChanged();
    SentryReporter::addBreadcrumb(QStringLiteral("scene.light.solo"),
                                  trimmed.isEmpty()
                                      ? QStringLiteral("Viewport solo cleared")
                                      : QStringLiteral("Viewport solo: %1").arg(trimmed));
}

bool ViewportLightSoloController::hasActiveViewport() const
{
    return m_activeWidget != nullptr;
}

QString ViewportLightSoloController::soloLightFor(OgreWidget* widget) const
{
    const auto it = m_soloByWidget.find(widget);
    if (it == m_soloByWidget.end())
        return {};
    return it->second;
}

void ViewportLightSoloController::beginRenderPass(OgreWidget* widget)
{
    if (m_inRenderPass)
        return;

    const QString soloName = soloLightFor(widget);
    if (soloName.isEmpty())
        return;

    auto* lights = LightManager::getSingletonPtr();
    if (!lights)
        return;

    m_inRenderPass = true;
    m_savedVisibility.clear();
    for (const LightHandle& handle : lights->lights())
    {
        if (!handle.light)
            continue;
        m_savedVisibility.insert(handle.name, handle.light->getVisible());
        handle.light->setVisible(handle.name == soloName);
    }
}

void ViewportLightSoloController::endRenderPass(OgreWidget* widget)
{
    Q_UNUSED(widget);
    if (!m_inRenderPass)
        return;

    auto* lights = LightManager::getSingletonPtr();
    if (!lights)
    {
        m_inRenderPass = false;
        return;
    }

    for (const LightHandle& handle : lights->lights())
    {
        if (!handle.light)
            continue;
        const auto it = m_savedVisibility.constFind(handle.name);
        handle.light->setVisible(it != m_savedVisibility.constEnd() ? it.value()
                                                                    : handle.light->getVisible());
    }
    m_savedVisibility.clear();
    m_inRenderPass = false;
}
