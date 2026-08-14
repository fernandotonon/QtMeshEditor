/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — Paint v2 Slice D channel-aware presets (#547)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see the header for the full notice.
-----------------------------------------------------------------------------------
*/

#include "PaintChannelPresets.h"
#include "TexturePaintController.h"
#include "PaintLayerBlend.h"
#include "SentryReporter.h"

#include <QColor>

PaintChannelPresets* PaintChannelPresets::m_pSingleton = nullptr;

PaintChannelPresets* PaintChannelPresets::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new PaintChannelPresets();
    return m_pSingleton;
}

PaintChannelPresets* PaintChannelPresets::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void PaintChannelPresets::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

PaintChannelPresets::PaintChannelPresets() : QObject(nullptr) {}

const QVector<PaintChannelPresets::Preset>& PaintChannelPresets::presets()
{
    using C = PaintChannelNS::Channel;
    using M = PaintLayerBlend::Mode;
    // The five channel-aware presets from issue #547. Each bundles a target
    // channel + brush parameters tuned for that channel's semantic.
    static const QVector<Preset> kPresets = {
        // Scratches into roughness — thin, hard, high-strength strokes that
        // raise roughness (bright = rough) so scratches catch the light.
        { QStringLiteral("Scratches into roughness"), C::Roughness,
          /*radius*/0.010, /*strength*/0.95, /*falloff*/0.5,
          /*rgb*/230, 230, 230, static_cast<int>(M::Normal),
          /*tool*/TexturePaintController::ToolPaint, /*stamp*/QString(),
          QStringLiteral("Hard thin strokes that roughen the surface where scratched.") },

        // Emissive sparks — small bright dabs added onto the emissive channel
        // (Add blend so overlapping sparks intensify).
        { QStringLiteral("Emissive sparks"), C::Emissive,
          0.015, 1.0, 1.0,
          255, 210, 120, static_cast<int>(M::Add),
          TexturePaintController::ToolPaint, QStringLiteral("Soft Circle"),
          QStringLiteral("Bright warm dabs that glow; overlapping sparks add up.") },

        // Edge wear — light metallic exposed at edges. Broad soft brush,
        // moderate strength, screen-ish; targets Metallic.
        { QStringLiteral("Edge wear"), C::Metallic,
          0.030, 0.6, 2.5,
          255, 255, 255, static_cast<int>(M::Screen),
          TexturePaintController::ToolPaint, QString(),
          QStringLiteral("Exposes bare metal along worn edges.") },

        // Dirt build-up — darkens AO in crevices. Large soft brush, low
        // strength, multiply so repeated passes deepen the grime.
        { QStringLiteral("Dirt build-up"), C::AO,
          0.045, 0.35, 3.0,
          60, 55, 45, static_cast<int>(M::Multiply),
          TexturePaintController::ToolPaint, QStringLiteral("Soft Circle"),
          QStringLiteral("Accumulating grime that darkens ambient occlusion.") },

        // Sticker — a crisp opaque decal on the base colour, using a stamp
        // footprint at full strength with no falloff.
        { QStringLiteral("Sticker"), C::BaseColor,
          0.060, 1.0, 0.0,
          255, 255, 255, static_cast<int>(M::Normal),
          TexturePaintController::ToolPaint, QStringLiteral("Soft Circle"),
          QStringLiteral("Crisp opaque decal stamped onto the base colour.") },
    };
    return kPresets;
}

QStringList PaintChannelPresets::presetNames() const
{
    QStringList names;
    for (const auto& p : presets()) names << p.name;
    return names;
}

bool PaintChannelPresets::findPreset(const QString& name, Preset& out)
{
    for (const auto& p : presets()) {
        if (p.name == name) { out = p; return true; }
    }
    return false;
}

bool PaintChannelPresets::applyPreset(const QString& name)
{
    Preset p;
    if (!findPreset(name, p)) return false;

    auto* pc = TexturePaintController::instance();
    if (!pc) return false;

    SentryReporter::addBreadcrumb(
        "paint.channel", QStringLiteral("preset %1 → %2")
            .arg(name, PaintChannelNS::id(p.channel)));

    pc->setActiveChannel(static_cast<int>(p.channel));
    pc->setBrushTool(p.brushTool);
    pc->setBrushRadius(p.radius);
    pc->setBrushStrength(p.strength);
    pc->setBrushFalloff(p.falloff);
    pc->setBrushColor(QColor(p.colorR, p.colorG, p.colorB));
    // Stamp vs round brush.
    if (!p.stamp.isEmpty()) {
        pc->setFootprintType(static_cast<int>(BrushFootprint::FootprintType::StampImage));
        pc->setActiveStampName(p.stamp);
    } else {
        pc->setFootprintType(static_cast<int>(BrushFootprint::FootprintType::Round));
    }

    emit presetApplied(name);
    return true;
}
