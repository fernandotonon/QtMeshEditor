#pragma once

#include "HDR/HdrTonemap.h"

#include <Ogre.h>

#include <QString>

class OgreWidget;

/// Per-viewport HDR render target + tonemap compositor (Slice D, #470).
class HdrViewportPipeline
{
public:
    static constexpr const char* kCompositorBaseName = "QtMeshHdrPipeline";
    static constexpr const char* kTonemapMaterialBaseName = "QtMesh/HdrTonemapPass";

    explicit HdrViewportPipeline(OgreWidget* widget);
    ~HdrViewportPipeline();

    void refresh();
    void updateTonemapUniforms();

    void setSkyBoxVisible(bool visible);
    bool skyBoxVisible() const { return m_skyBoxVisible; }

    bool tonemapOverride() const { return m_tonemapOverride; }
    void setTonemapOverride(bool enabled);
    HdrTonemap::Operator tonemapOperator() const;
    void setTonemapOperator(HdrTonemap::Operator op);
    float exposureEv() const;
    void setExposureEv(float exposureEv);

    const QString& compositorName() const { return m_compositorName; }
    const QString& tonemapMaterialName() const { return m_tonemapMaterialName; }

private:
    void enablePipeline();
    void disablePipeline();
    void ensureTonemapMaterial();
    void ensureCompositorScript();

    HdrTonemap::Operator effectiveTonemapOperator() const;
    float effectiveExposureEv() const;

    OgreWidget* m_widget = nullptr;
    Ogre::Viewport* m_viewport = nullptr;
    Ogre::CompositorInstance* m_compositor = nullptr;
    QString m_compositorName;
    QString m_tonemapMaterialName;
    bool m_skyBoxVisible = true;
    bool m_enabled = false;

    bool m_tonemapOverride = false;
    HdrTonemap::Operator m_localTonemapOperator = HdrTonemap::Operator::ACES;
    float m_localExposureEv = 0.f;
};
