#include "MeshLodController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include "MeshOptimizerLod.h"
#include "SentryReporter.h"
#include <Ogre.h>
#include <OgreSubMesh.h>
#include <OgreMeshLodGenerator.h>
#include <OgreLodConfig.h>
#include <QDir>
#include <QVariantMap>
#include <limits>

namespace {

QList<Ogre::Entity*> lodTargetEntities()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel)
        return {};
    return sel->getResolvedEntities();
}

} // namespace

MeshLodController* MeshLodController::m_pSingleton = nullptr;

MeshLodController* MeshLodController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new MeshLodController();
    return m_pSingleton;
}

MeshLodController* MeshLodController::qmlInstance(QQmlEngine* engine, QJSEngine* scriptEngine)
{
    Q_UNUSED(engine);
    Q_UNUSED(scriptEngine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void MeshLodController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

MeshLodController::MeshLodController() : QObject(nullptr)
{
    m_generator = std::make_unique<Ogre::MeshLodGenerator>();

    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, [this]() {
        emit selectionChanged();
        emit lodChanged();
    });
}

MeshLodController::~MeshLodController() = default;

bool MeshLodController::hasSelection() const
{
    return !lodTargetEntities().isEmpty();
}

int MeshLodController::currentLodLevels() const
{
    const QList<Ogre::Entity*> entities = lodTargetEntities();
    if (entities.empty())
        return 0;
    int count = static_cast<int>(entities.front()->getMesh()->getNumLodLevels());
    return std::max(0, count - 1); // exclude base LOD level 0
}

QVariantList MeshLodController::lodLevelInfo() const
{
    QVariantList result;
    const QList<Ogre::Entity*> entities = lodTargetEntities();
    if (entities.empty()) return result;

    Ogre::MeshPtr mesh = entities.front()->getMesh();
    if (!mesh) return result;

    const unsigned int totalLods = mesh->getNumLodLevels();
    const unsigned int numSubs = mesh->getNumSubMeshes();

    // LOD 0 = base mesh: sum indexData->indexCount over all submeshes
    unsigned int baseTris = 0;
    for (unsigned int s = 0; s < numSubs; ++s) {
        Ogre::SubMesh* sub = mesh->getSubMesh(s);
        if (sub->indexData)
            baseTris += sub->indexData->indexCount / 3;
    }
    QVariantMap base;
    base["level"] = 0;
    base["label"] = "Base";
    base["triangles"] = baseTris;
    result.append(base);

    // LOD 1..N-1 = reduced levels
    for (unsigned int lod = 1; lod < totalLods; ++lod) {
        unsigned int lodTris = 0;
        for (unsigned int s = 0; s < numSubs; ++s) {
            Ogre::SubMesh* sub = mesh->getSubMesh(s);
            if ((lod - 1) < sub->mLodFaceList.size() && sub->mLodFaceList[lod - 1])
                lodTris += sub->mLodFaceList[lod - 1]->indexCount / 3;
        }
        QVariantMap entry;
        entry["level"] = static_cast<int>(lod);
        entry["label"] = QString("LOD %1").arg(lod);
        entry["triangles"] = lodTris;
        result.append(entry);
    }

    return result;
}

void MeshLodController::previewLod(int lodIndex)
{
    for (Ogre::Entity* entity : lodTargetEntities()) {
        if (lodIndex < 0) {
            // restore: full range, normal bias
            entity->setMeshLodBias(1.0f, 0, std::numeric_limits<unsigned short>::max());
        } else {
            auto idx = static_cast<unsigned short>(lodIndex);
            entity->setMeshLodBias(1.0f, idx, idx);
        }
    }
}

void MeshLodController::generateLods(int count, QVariantList reductions)
{
    // QML-facing default: meshoptimizer backend (issue #398).
    generateLods(count, reductions, Algorithm::Meshopt);
}

void MeshLodController::generateLodsWithAlgo(int count, QVariantList reductions,
                                             const QString& algo)
{
    const auto a = (algo.toLower() == QStringLiteral("ogre"))
        ? Algorithm::Ogre
        : Algorithm::Meshopt;
    generateLods(count, reductions, a);
}

void MeshLodController::generateLods(int count, const QVariantList& reductions, Algorithm algo)
{
    const QList<Ogre::Entity*> targets = lodTargetEntities();
    if (targets.isEmpty()) {
        emit error("No mesh found in selection.");
        return;
    }

    const char* algoName = (algo == Algorithm::Meshopt) ? "meshopt" : "ogre";
    SentryReporter::addBreadcrumb(
        algo == Algorithm::Meshopt ? "ai.assist.lod" : "ui.action",
        QString("Generate %1 LOD level(s) via %2")
            .arg(std::max(1, std::min(count, 4))).arg(algoName));

    count = std::max(1, std::min(count, 4));

    // distances at which each LOD kicks in (world units)
    static const float kDistances[] = { 50.0f, 150.0f, 400.0f, 1000.0f };

    for (Ogre::Entity* entity : targets) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        // Remove existing LODs before regenerating
        mesh->removeLodLevels();

        if (algo == Algorithm::Meshopt) {
            // Build the per-level reduction vector with the same
            // fallback policy as the Ogre path (25% / 50% / 75% /
            // 100% if the caller didn't supply enough explicit
            // values) so the user-visible behaviour matches across
            // backends.
            std::vector<float> r;
            r.reserve(count);
            for (int i = 0; i < count; ++i) {
                const float v = (i < reductions.size())
                    ? std::max(0.01f, std::min(1.0f, reductions[i].toFloat()))
                    : 0.25f * (i + 1);
                r.push_back(v);
            }

            auto levels = MeshOptimizerLod::generateLods(mesh.get(), r);
            if (levels.empty()) {
                emit error("Meshopt LOD generation produced no levels.");
                return;
            }

            // Mesh-level LOD count (base + N reduced). _setLodInfo
            // resizes the mesh-wide usage table AND each submesh's
            // `mLodFaceList` to (numLevels - 1) slots filled with
            // nullptr — so we assign by index below rather than
            // push_back. Ownership of the IndexData* is transferred
            // out of the LodLevel struct (nulled afterwards) so
            // destroyLevel won't double-free.
            const unsigned int numSubs = mesh->getNumSubMeshes();
            mesh->_setLodInfo(static_cast<unsigned short>(levels.size() + 1));
            for (size_t lod = 0; lod < levels.size(); ++lod) {
                auto& level = levels[lod];
                for (unsigned int s = 0; s < numSubs && s < level.indices.size(); ++s) {
                    auto& faceList = mesh->getSubMesh(s)->mLodFaceList;
                    if (lod < faceList.size()) {
                        faceList[lod] = level.indices[s];
                        level.indices[s] = nullptr;   // ownership transferred
                    }
                }
                // Wire the distance metadata into the mesh itself —
                // this is what `MeshLodController::lodLevelInfo` and
                // Ogre's runtime LOD picker read.
                Ogre::MeshLodUsage usage;
                usage.userValue = kDistances[lod];
                usage.value     = mesh->getLodStrategy()->transformUserValue(usage.userValue);
                mesh->_setLodUsage(static_cast<unsigned short>(lod + 1), usage);
            }
        } else {
            // ---- Ogre legacy path ----
            Ogre::LodConfig lodConfig(mesh);
            for (int i = 0; i < count; ++i) {
                float reduction = (i < reductions.size())
                    ? std::max(0.01f, std::min(1.0f, reductions[i].toFloat()))
                    : 0.25f * (i + 1); // fallback: 25%, 50%, 75%, 100%
                float dist = kDistances[i];
                lodConfig.createGeneratedLodLevel(dist, reduction, Ogre::LodLevel::VRM_PROPORTIONAL);
            }

            try {
                m_generator->generateLodLevels(lodConfig);
            } catch (const Ogre::Exception& e) {
                emit error(QString("LOD generation failed: %1").arg(e.what()));
                return;
            }
        }
    }

    emit lodChanged();
    emit generationSucceeded(count);
}

void MeshLodController::generateAutoLods()
{
    const QList<Ogre::Entity*> targets = lodTargetEntities();
    if (targets.isEmpty()) {
        emit error("No mesh found in selection.");
        return;
    }

    SentryReporter::addBreadcrumb("ui.action", "Auto-generate LOD levels");

    for (Ogre::Entity* entity : targets) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        mesh->removeLodLevels();
        try {
            m_generator->generateAutoconfiguredLodLevels(mesh);
        } catch (const Ogre::Exception& e) {
            emit error(QString("Auto LOD generation failed: %1").arg(e.what()));
            return;
        }
    }

    emit lodChanged();
    emit generationSucceeded(-1); // -1 = auto
}

void MeshLodController::removeLods()
{
    const QList<Ogre::Entity*> targets = lodTargetEntities();
    if (targets.isEmpty()) return;

    SentryReporter::addBreadcrumb("ui.action", "Remove LOD levels");

    for (Ogre::Entity* entity : targets) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (mesh)
            mesh->removeLodLevels();
    }

    emit lodChanged();
}

void MeshLodController::exportLods(const QString& format)
{
    const QList<Ogre::Entity*> targets = lodTargetEntities();
    if (targets.isEmpty()) {
        emit error("No mesh found in selection.");
        return;
    }

    SentryReporter::addBreadcrumb("ui.action",
        QString("Export LOD levels (%1)").arg(format.isEmpty() ? "gltf" : format));

    Ogre::Entity* entity = targets.front();
    if (!entity) return;

    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;

    const unsigned int totalLods = mesh->getNumLodLevels();
    if (totalLods <= 1) {
        emit error("No LOD levels generated yet. Click 'Generate' or 'Auto' first.");
        return;
    }

    // Don't open a dialog here — emit a signal so MainWindow can open the
    // directory picker with the correct parent widget (reliable on macOS).
    emit exportLodsRequested(format);
}

void MeshLodController::doExportLods(const QString& format, const QString& directory)
{
    const QList<Ogre::Entity*> targets = lodTargetEntities();
    if (targets.isEmpty()) return;

    Ogre::Entity* entity = targets.front();
    if (!entity) return;

    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;

    const unsigned int totalLods = mesh->getNumLodLevels();
    if (totalLods <= 1) return;

    Ogre::SceneNode* sn = entity->getParentSceneNode();
    if (!sn) {
        emit error("Entity has no scene node.");
        return;
    }

    const QString baseName = QString::fromStdString(mesh->getName());
    const QString ext = format.isEmpty() ? "gltf" : format;
    int exported = 0;

    // LOD 0 = original full mesh; LOD 1..N are the reduced levels.
    // Temporarily swap each submesh's indexData with its mLodFaceList[i-1]
    // entry, export, then restore — the in-memory mesh is never permanently altered.
    for (unsigned int lod = 1; lod < totalLods; ++lod) {
        const unsigned int numSubs = mesh->getNumSubMeshes();
        std::vector<Ogre::IndexData*> savedIndex(numSubs, nullptr);

        for (unsigned int s = 0; s < numSubs; ++s) {
            Ogre::SubMesh* sub = mesh->getSubMesh(s);
            savedIndex[s] = sub->indexData;
            if ((lod - 1) < sub->mLodFaceList.size())
                sub->indexData = sub->mLodFaceList[lod - 1];
        }

        const QString outPath = QDir(directory).filePath(
            QString("%1_lod%2.%3").arg(baseName).arg(lod).arg(ext));
        if (MeshImporterExporter::exporter(sn, outPath, ext, /*stripAnimations=*/true) == 0)
            ++exported;

        for (unsigned int s = 0; s < numSubs; ++s)
            mesh->getSubMesh(s)->indexData = savedIndex[s];
    }

    emit exportSucceeded(exported, directory);
}
