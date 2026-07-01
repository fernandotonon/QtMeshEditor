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

    void setSkyBoxVisible(OgreWidget* widget, bool visible);
    bool skyBoxVisible(const OgreWidget* widget) const;

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

    static HdrViewportController* s_singleton;
    std::unordered_map<OgreWidget*, std::unique_ptr<HdrViewportPipeline>> m_pipelines;
};
