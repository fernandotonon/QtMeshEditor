#include "MeshLodController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "MeshImporterExporter.h"
#include <Ogre.h>
#include <OgreSubMesh.h>
#include <OgreMeshLodGenerator.h>
#include <OgreLodConfig.h>
#include <QFileDialog>
#include <QDir>

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
            this, &MeshLodController::selectionChanged);
}

MeshLodController::~MeshLodController() = default;

bool MeshLodController::hasSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    return sel && sel->hasEntities();
}

int MeshLodController::currentLodLevels() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities())
        return 0;
    auto entities = sel->getEntitiesSelectionList();
    if (entities.empty())
        return 0;
    int count = static_cast<int>(entities.front()->getMesh()->getNumLodLevels());
    return std::max(0, count - 1); // exclude base LOD level 0
}

void MeshLodController::generateLods(int count, QVariantList reductions)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities()) {
        emit error("No mesh selected.");
        return;
    }

    count = std::max(1, std::min(count, 4));

    // distances at which each LOD kicks in (world units)
    static const float kDistances[] = { 50.0f, 150.0f, 400.0f, 1000.0f };

    for (Ogre::Entity* entity : sel->getEntitiesSelectionList()) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (!mesh) continue;

        // Remove existing LODs before regenerating
        mesh->removeLodLevels();

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

    emit lodChanged();
    emit generationSucceeded(count);
}

void MeshLodController::generateAutoLods()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities()) {
        emit error("No mesh selected.");
        return;
    }

    for (Ogre::Entity* entity : sel->getEntitiesSelectionList()) {
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
    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities()) return;

    for (Ogre::Entity* entity : sel->getEntitiesSelectionList()) {
        Ogre::MeshPtr mesh = entity->getMesh();
        if (mesh)
            mesh->removeLodLevels();
    }

    emit lodChanged();
}

void MeshLodController::exportLods(const QString& format)
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel || !sel->hasEntities()) {
        emit error("No mesh selected.");
        return;
    }

    auto entities = sel->getEntitiesSelectionList();
    Ogre::Entity* entity = entities.empty() ? nullptr : entities.front();
    if (!entity) return;

    Ogre::MeshPtr mesh = entity->getMesh();
    if (!mesh) return;

    const unsigned int totalLods = mesh->getNumLodLevels();
    if (totalLods <= 1) {
        emit error("No LOD levels generated yet. Use Generate or Auto first.");
        return;
    }

    QString dir = QFileDialog::getExistingDirectory(
        nullptr, "Choose export directory for LOD levels", QDir::homePath());
    if (dir.isEmpty()) return;

    const QString baseName = QString::fromStdString(mesh->getName());
    const QString ext = format.isEmpty() ? "gltf2" : format;

    // Determine the scene node for exporting
    Ogre::SceneNode* sn = entity->getParentSceneNode();
    if (!sn) {
        emit error("Entity has no scene node.");
        return;
    }

    int exported = 0;

    // LOD 0 = original full mesh (always present); LOD 1..N are the reduced levels.
    // Export LOD level i by temporarily swapping each submesh's indexData with its
    // mLodFaceList[i-1] entry, exporting, then restoring.
    for (unsigned int lod = 1; lod < totalLods; ++lod) {
        // Collect and swap index data for every submesh
        const unsigned int numSubs = mesh->getNumSubMeshes();
        std::vector<Ogre::IndexData*> savedIndex(numSubs, nullptr);

        for (unsigned int s = 0; s < numSubs; ++s) {
            Ogre::SubMesh* sub = mesh->getSubMesh(s);
            savedIndex[s] = sub->indexData;
            if ((lod - 1) < sub->mLodFaceList.size())
                sub->indexData = sub->mLodFaceList[lod - 1];
        }

        const QString outPath = QDir(dir).filePath(
            QString("%1_lod%2.%3").arg(baseName).arg(lod).arg(ext));

        MeshImporterExporter::exporter(sn, outPath, ext);
        ++exported;

        // Restore original index data
        for (unsigned int s = 0; s < numSubs; ++s)
            mesh->getSubMesh(s)->indexData = savedIndex[s];
    }

    emit exportSucceeded(exported, dir);
}
