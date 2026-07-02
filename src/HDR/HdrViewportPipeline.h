#pragma once

#include "HDR/HdrTonemap.h"

#include <Ogre.h>

#include <QString>

class OgreWidget;

/// Per-viewport skybox sync + optional HDR tonemap compositor (Slice D, #470).
class HdrViewportPipeline
{
public:
    static constexpr const char* kCompositorBaseName = "QtMeshHdrPipeline";
    static constexpr const char* kTonemapMaterialBaseName = "QtMesh/HdrTonemapPass";
    /// Set true when #470 compositor render_scene works with RTSS; until then skybox/IBL
    /// use the normal viewport path and tonemap uniforms are unused.
    static constexpr bool kUseTonemapCompositor = false;

    explicit HdrViewportPipeline(OgreWidget* widget);
    ~HdrViewportPipeline();

    void refresh();
    /// Pushes global tonemap settings to the compositor material (no-op while compositor off).
    void updateTonemapUniforms();

    void setSkyBoxVisible(bool visible);
    bool skyBoxVisible() const { return m_skyBoxVisible; }

private:
    void syncViewportEnvironment();
    void disablePipeline();
    void removeCompositor();
    void ensureTonemapMaterial();
    void ensureCompositorScript();
    bool attachTonemapCompositor(Ogre::Viewport* vp);

    OgreWidget* m_widget = nullptr;
    Ogre::Viewport* m_viewport = nullptr;
    Ogre::CompositorInstance* m_compositor = nullptr;
    QString m_compositorName;
    QString m_tonemapMaterialName;
    bool m_skyBoxVisible = false;
    bool m_enabled = false;
};
