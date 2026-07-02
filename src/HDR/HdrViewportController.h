#pragma once

#include <QObject>

#include <memory>
#include <unordered_map>

class OgreWidget;
class HdrViewportPipeline;

/// Registers per-viewport HDR pipelines and syncs them with HDREnvironmentManager.
class HdrViewportController : public QObject
{
    Q_OBJECT

public:
    static HdrViewportController* getSingleton();
    static HdrViewportController* getSingletonPtr();
    static void kill();

    void registerWidget(OgreWidget* widget);
    void unregisterWidget(OgreWidget* widget);

    void setActiveWidget(OgreWidget* widget);
    OgreWidget* activeWidget() const { return m_activeWidget; }

    void tickViewport(OgreWidget* widget);
    void tickActiveViewports();

private slots:
    void onEnvironmentChanged();
    void onTonemapChanged();
    void onSkyboxDefaultChanged();

private:
    explicit HdrViewportController(QObject* parent = nullptr);
    ~HdrViewportController() override;

    HdrViewportPipeline* pipelineFor(OgreWidget* widget) const;
    void refreshAll();
    void syncAllSkyBoxesFromDefault();

    static HdrViewportController* s_singleton;
    OgreWidget* m_activeWidget = nullptr;
    std::unordered_map<OgreWidget*, std::unique_ptr<HdrViewportPipeline>> m_pipelines;
};
