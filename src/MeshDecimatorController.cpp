#include "MeshDecimatorController.h"
#include "MeshDecimator.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <Ogre.h>
#include <OgreMeshLodGenerator.h>
#include <OgreLodConfig.h>
#include <OgreSubMesh.h>
#include <OgreEntity.h>

#include <limits>

namespace {

QList<Ogre::Entity*> decimateTargets()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return {};
    return sel->getResolvedEntities();
}

} // namespace

MeshDecimatorController* MeshDecimatorController::m_pSingleton = nullptr;

MeshDecimatorController* MeshDecimatorController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new MeshDecimatorController();
    return m_pSingleton;
}

MeshDecimatorController* MeshDecimatorController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MeshDecimatorController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MeshDecimatorController::~MeshDecimatorController() = default;

MeshDecimatorController::MeshDecimatorController() : QObject(nullptr)
{
    m_generator = std::make_unique<Ogre::MeshLodGenerator>();

    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, [this]() {
        clearPreview();
        refreshBaseline();
        emit selectionChanged();
        emit baseChanged();
    });
}

bool MeshDecimatorController::hasSelection() const
{
    return !decimateTargets().isEmpty();
}

int MeshDecimatorController::baseTriangleCount() const
{
    // Lazy refresh: if we don't have a cached count but a selection exists,
    // populate now. Cast away const — m_baseTriangleCount is metadata, not
    // observable state from outside the controller's invariants.
    if (m_baseTriangleCount == 0 && hasSelection()) {
        const_cast<MeshDecimatorController*>(this)->refreshBaseline();
    }
    return m_baseTriangleCount;
}

void MeshDecimatorController::refreshBaseline()
{
    const QList<Ogre::Entity*> targets = decimateTargets();
    if (targets.isEmpty()) {
        m_baseTriangleCount = 0;
        return;
    }
    int tris = 0;
    int verts = 0;
    MeshDecimator::countBaseline(targets.front(), tris, verts);
    m_baseTriangleCount = tris;
}

// LCOV_EXCL_START — Ogre-only path
void MeshDecimatorController::previewReduction(double reduction)
{
    const QList<Ogre::Entity*> targets = decimateTargets();
    if (targets.isEmpty()) {
        emit error(QStringLiteral("No mesh selected."));
        return;
    }
    const double r = MeshDecimator::clampReduction(reduction);
    if (r <= 0.0) {
        clearPreview();
        return;
    }

    Ogre::Entity* entity = targets.front();
    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;

    // Tear down any previous preview LOD before regenerating — the slider
    // moves often, so we don't want stale LOD chains piling up.
    mesh->removeLodLevels();

    Ogre::LodConfig lodConfig(mesh);
    lodConfig.createGeneratedLodLevel(0.0f, static_cast<float>(r),
                                      Ogre::LodLevel::VRM_PROPORTIONAL);
    try {
        m_generator->generateLodLevels(lodConfig);
    } catch (const Ogre::Exception& e) {
        emit error(QString("Preview failed: %1").arg(e.what()));
        return;
    }

    // Force display of the LOD we just generated.
    for (Ogre::Entity* e : targets)
        e->setMeshLodBias(1.0f, 1, 1);

    // Count the preview tris (LOD 1's index count).
    int previewTris = 0;
    for (unsigned int s = 0; s < mesh->getNumSubMeshes(); ++s) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (!sub || sub->mLodFaceList.empty() || !sub->mLodFaceList.front()) continue;
        previewTris += static_cast<int>(sub->mLodFaceList.front()->indexCount / 3);
    }
    m_previewTriangleCount = previewTris;
    m_hasPreview = true;
    emit previewChanged();
}

void MeshDecimatorController::clearPreview()
{
    if (!m_hasPreview) return;

    for (Ogre::Entity* entity : decimateTargets()) {
        entity->setMeshLodBias(1.0f, 0,
                               std::numeric_limits<unsigned short>::max());
        Ogre::MeshPtr mesh = entity->getMesh();
        if (mesh) mesh->removeLodLevels();
    }
    m_hasPreview = false;
    m_previewTriangleCount = 0;
    emit previewChanged();
}

void MeshDecimatorController::applyReduction(double reduction)
{
    const QList<Ogre::Entity*> targets = decimateTargets();
    if (targets.isEmpty()) {
        emit error(QStringLiteral("No mesh selected."));
        return;
    }
    const double r = MeshDecimator::clampReduction(reduction);
    if (r <= 0.0) return;

    // Drop any preview-only LOD swap before committing — applyEntity does
    // its own removeLodLevels() too, but resetting the bias first keeps the
    // viewport honest if the apply fails halfway.
    for (Ogre::Entity* entity : targets)
        entity->setMeshLodBias(1.0f, 0,
                               std::numeric_limits<unsigned short>::max());

    SentryReporter::addBreadcrumb("ui.action",
        QString("Decimate (in-place, r=%1)").arg(r, 0, 'f', 2));

    Ogre::Entity* entity = targets.front();
    const DecimationReport report = MeshDecimator::decimateEntity(entity, r);
    if (!report.applied) {
        emit error(QStringLiteral("Decimation failed. The mesh may not be "
                                  "suitable for in-place reduction."));
        return;
    }

    m_hasPreview = false;
    m_previewTriangleCount = 0;
    refreshBaseline();
    emit previewChanged();
    emit baseChanged();
    emit applied(report.totalTrianglesBefore, report.totalTrianglesAfter);
}
// LCOV_EXCL_STOP
