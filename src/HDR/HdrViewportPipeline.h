#pragma once

#include "HDR/HdrTonemap.h"

#include <Ogre.h>

class OgreWidget;

/// Per-viewport HDR render target + tonemap compositor (Slice D, #470).
class HdrViewportPipeline
{
public:
    static constexpr const char* kCompositorName = "QtMeshHdrPipeline";

    explicit HdrViewportPipeline(OgreWidget* widget);
    ~HdrViewportPipeline();

    void refresh();
    void updateTonemapUniforms();

    void setSkyBoxVisible(bool visible);
    bool skyBoxVisible() const { return m_skyBoxVisible; }

private:
    void enablePipeline();
    void disablePipeline();

    OgreWidget* m_widget = nullptr;
    Ogre::Viewport* m_viewport = nullptr;
    Ogre::CompositorInstance* m_compositor = nullptr;
    bool m_skyBoxVisible = true;
    bool m_enabled = false;
};
