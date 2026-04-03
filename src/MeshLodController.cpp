#include "MeshLodController.h"
#include "Manager.h"
#include "SelectionSet.h"
#include <Ogre.h>
#include <OgreMeshLodGenerator.h>
#include <OgreLodConfig.h>

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
