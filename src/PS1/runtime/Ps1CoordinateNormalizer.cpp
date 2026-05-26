#include "Ps1CoordinateNormalizer.h"

#include <QSettings>
#include <QStringList>

#include <cmath>

#ifdef ENABLE_PS1_RIP
#include "Manager.h"
#include <Ogre.h>
#endif

namespace {

constexpr float kEpsilon = 1e-6f;

bool nearlyEqual(float a, float b)
{
    return std::fabs(a - b) < kEpsilon;
}

} // namespace

bool Ps1NormalizerSettings::isDefault() const
{
    const Ps1NormalizerSettings d{};
    return nearlyEqual(userScale, d.userScale)
        && flipX == d.flipX && flipY == d.flipY && flipZ == d.flipZ
        && perspectiveCorrectUVs == d.perspectiveCorrectUVs
        && nearlyEqual(perspectiveTolerance, d.perspectiveTolerance)
        && perspectiveMaxDepth == d.perspectiveMaxDepth;
}

void Ps1CoordinateNormalizer::composeNodeTransform(const Ps1NormalizerSettings &settings,
                                                   float placementScale,
                                                   float basePosX, float basePosY, float basePosZ,
                                                   float outScale[3], float outPosition[3])
{
    // Same factor goes into BOTH scale and position so a multi-instance
    // capture set (deduped buildings, props, etc.) scales / mirrors as one
    // coherent layout. Scaling only the per-mesh transform while leaving
    // pivots at attach-time positions left adjacent instances overlapping
    // at < 1.0 scale and crossing each other on a single-axis flip — the
    // Codex P1 / CodeRabbit Major finding on the original #424 PR.
    const float sx = placementScale * settings.userScale * settings.signX();
    const float sy = placementScale * settings.userScale * settings.signY();
    const float sz = placementScale * settings.userScale * settings.signZ();
    outScale[0] = sx;
    outScale[1] = sy;
    outScale[2] = sz;
    outPosition[0] = basePosX * sx;
    outPosition[1] = basePosY * sy;
    outPosition[2] = basePosZ * sz;
}

void Ps1CoordinateNormalizer::applyToSceneNode(Ogre::SceneNode *node,
                                               const Ps1NormalizerSettings &settings)
{
#ifdef ENABLE_PS1_RIP
    if (!node)
        return;
    // PS1RipMeshBuilder stamps the auto-fit-to-target-extent factor + the raw
    // capture-time instance position on the node at attach time. Reading them
    // back lets us recompose both transforms (scale + position) from a fresh
    // (userScale × per-axis sign) without losing the original auto-fit or
    // letting the layout drift when the user toggles a flip live (#424).
    float placementScale = 1.0f;
    const Ogre::Any &psAny = node->getUserObjectBindings().getUserAny("ps1RipPlacementScale");
    if (psAny.has_value()) {
        try {
            placementScale = Ogre::any_cast<float>(psAny);
        } catch (const Ogre::Exception &) {
            placementScale = 1.0f;
        }
    }
    Ogre::Vector3 basePos(0.0f, 0.0f, 0.0f);
    const Ogre::Any &bpAny = node->getUserObjectBindings().getUserAny("ps1RipBasePosition");
    if (bpAny.has_value()) {
        try {
            basePos = Ogre::any_cast<Ogre::Vector3>(bpAny);
        } catch (const Ogre::Exception &) {
            basePos = Ogre::Vector3(0.0f, 0.0f, 0.0f);
        }
    }

    float scaleOut[3];
    float posOut[3];
    composeNodeTransform(settings, placementScale, basePos.x, basePos.y, basePos.z,
                         scaleOut, posOut);
    node->setScale(Ogre::Vector3(scaleOut[0], scaleOut[1], scaleOut[2]));
    node->setPosition(Ogre::Vector3(posOut[0], posOut[1], posOut[2]));
#else
    (void)node;
    (void)settings;
#endif
}

int Ps1CoordinateNormalizer::applyToCaptureNodes(const Ps1NormalizerSettings &settings)
{
#ifdef ENABLE_PS1_RIP
    auto *mgr = Manager::getSingletonPtr();
    if (!mgr)
        return 0;
    int touched = 0;
    // Snapshot the node list first — applyToSceneNode itself never mutates
    // the scene graph, but calling Manager APIs while iterating its underlying
    // container can race with destroy events scheduled by Ogre.
    const auto nodes = mgr->getSceneNodes();
    for (Ogre::SceneNode *node : nodes) {
        if (!node)
            continue;
        const QString name = QString::fromStdString(node->getName());
        if (!name.startsWith(QStringLiteral("PS1Capture_")))
            continue;
        applyToSceneNode(node, settings);
        ++touched;
    }
    return touched;
#else
    (void)settings;
    return 0;
#endif
}

void Ps1CoordinateNormalizer::save(QSettings &settings, const QString &prefix,
                                   const Ps1NormalizerSettings &value)
{
    settings.setValue(prefix + QStringLiteral("/userScale"), value.userScale);
    settings.setValue(prefix + QStringLiteral("/flipX"), value.flipX);
    settings.setValue(prefix + QStringLiteral("/flipY"), value.flipY);
    settings.setValue(prefix + QStringLiteral("/flipZ"), value.flipZ);
    settings.setValue(prefix + QStringLiteral("/perspectiveCorrectUVs"), value.perspectiveCorrectUVs);
    settings.setValue(prefix + QStringLiteral("/perspectiveTolerance"), value.perspectiveTolerance);
    settings.setValue(prefix + QStringLiteral("/perspectiveMaxDepth"), value.perspectiveMaxDepth);
}

Ps1NormalizerSettings Ps1CoordinateNormalizer::load(QSettings &settings, const QString &prefix)
{
    Ps1NormalizerSettings out;
    out.userScale = settings.value(prefix + QStringLiteral("/userScale"), out.userScale).toFloat();
    // Defensive clamp: a corrupted ini file could write 0 or a huge value and
    // bake invisible / explosively-scaled capture nodes. The slider in the UI
    // is bounded the same way; we mirror it here so direct-edit ini files
    // don't bypass the safety net.
    if (!(out.userScale >= 0.001f && out.userScale <= 1000.0f))
        out.userScale = 1.0f;
    out.flipX = settings.value(prefix + QStringLiteral("/flipX"), out.flipX).toBool();
    out.flipY = settings.value(prefix + QStringLiteral("/flipY"), out.flipY).toBool();
    out.flipZ = settings.value(prefix + QStringLiteral("/flipZ"), out.flipZ).toBool();
    out.perspectiveCorrectUVs = settings.value(prefix + QStringLiteral("/perspectiveCorrectUVs"),
                                               out.perspectiveCorrectUVs).toBool();
    out.perspectiveTolerance = settings.value(prefix + QStringLiteral("/perspectiveTolerance"),
                                              out.perspectiveTolerance).toFloat();
    if (!(out.perspectiveTolerance >= 1.0f && out.perspectiveTolerance <= 1000.0f))
        out.perspectiveTolerance = 1.3f;
    out.perspectiveMaxDepth = settings.value(prefix + QStringLiteral("/perspectiveMaxDepth"),
                                             out.perspectiveMaxDepth).toInt();
    if (out.perspectiveMaxDepth < 0 || out.perspectiveMaxDepth > 6)
        out.perspectiveMaxDepth = 3;
    return out;
}

QString Ps1CoordinateNormalizer::describe(const Ps1NormalizerSettings &settings)
{
    if (settings.isDefault())
        return QStringLiteral("default");
    QStringList parts;
    parts.append(QStringLiteral("scale=%1").arg(settings.userScale, 0, 'g', 4));
    if (settings.flipX) parts.append(QStringLiteral("flipX"));
    if (settings.flipY) parts.append(QStringLiteral("flipY"));
    if (settings.flipZ) parts.append(QStringLiteral("flipZ"));
    if (settings.perspectiveCorrectUVs) {
        parts.append(QStringLiteral("perspUV(tol=%1,depth=%2)")
                         .arg(settings.perspectiveTolerance, 0, 'g', 3)
                         .arg(settings.perspectiveMaxDepth));
    }
    return parts.join(QLatin1Char(','));
}
