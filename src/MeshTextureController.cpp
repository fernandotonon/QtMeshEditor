#include "MeshTextureController.h"
#include "MeshDepthRenderer.h"
#include "SDManager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QFileInfo>
#include <QImage>

#include <Ogre.h>
#include <OgreEntity.h>

#include <algorithm>

MeshTextureController* MeshTextureController::m_pSingleton = nullptr;

MeshTextureController* MeshTextureController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new MeshTextureController();
    return m_pSingleton;
}

MeshTextureController* MeshTextureController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MeshTextureController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MeshTextureController::MeshTextureController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &MeshTextureController::selectionChanged);
}

bool MeshTextureController::hasSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    return !sel->getResolvedEntities().isEmpty();
}

bool MeshTextureController::sdAvailable() const
{
#ifdef ENABLE_STABLE_DIFFUSION
    return true;
#else
    return false;
#endif
}

QString MeshTextureController::generateForSelected(const QString& prompt,
                                                   const QString& controlNetPath,
                                                   double controlStrength,
                                                   int depthSize)
{
#ifndef ENABLE_STABLE_DIFFUSION
    Q_UNUSED(prompt); Q_UNUSED(controlNetPath);
    Q_UNUSED(controlStrength); Q_UNUSED(depthSize);
    return QStringLiteral(
        "This build was compiled without AI texture generation. "
        "Rebuild with -DENABLE_STABLE_DIFFUSION=ON.");
#else
    if (prompt.trimmed().isEmpty())
        return QStringLiteral("Prompt is required.");

    auto* sel = SelectionSet::getSingleton();
    if (!sel) return QStringLiteral("No selection set.");
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) return QStringLiteral("No mesh selected.");
    Ogre::Entity* entity = entities.first();
    if (!entity || !entity->getMesh())
        return QStringLiteral("Selected entity is no longer valid.");

    // A ControlNet path is optional — without it we degrade to plain
    // txt2img — but warn if the file was named yet doesn't exist, so
    // the user isn't surprised by an unconditioned result.
    if (!controlNetPath.isEmpty() && !QFileInfo::exists(controlNetPath))
        return QStringLiteral("ControlNet model not found: %1").arg(controlNetPath);

    // Render the depth map (main thread — touches the scene).
    QString depthErr;
    const QImage depth = MeshDepthRenderer::renderDepthMap(entity, depthSize, &depthErr);
    if (depth.isNull())
        return QStringLiteral("Depth render failed: %1").arg(depthErr);

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.mesh_texture"),
        QStringLiteral("entity=%1 controlNet=%2 strength=%3 depth=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(controlNetPath.isEmpty() ? QStringLiteral("(none)")
                                          : QFileInfo(controlNetPath).fileName())
            .arg(controlStrength).arg(depthSize));

    SDManager* sd = SDManager::instance();
    if (!sd) return QStringLiteral("SD manager unavailable.");
    if (!sd->isModelLoaded())
        return QStringLiteral("No SD model loaded. Load a base model first.");

    const float strength =
        static_cast<float>(std::clamp(controlStrength, 0.0, 1.0));
    // Fire-and-forget — progress + completion (and the apply-to-
    // material step) flow through SDManager's existing signals,
    // which MaterialEditorQML is already wired to.
    sd->generateMeshTexture(prompt, depth, controlNetPath, strength);
    return QString();   // success — generation proceeds async
#endif
}
