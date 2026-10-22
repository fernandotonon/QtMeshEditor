#include "CharacterScene.h"

#include "../Manager.h"
#include "../SentryReporter.h"

#include <OgreEntity.h>
#include <OgreMaterialManager.h>
#include <OgreMesh.h>
#include <OgrePass.h>
#include <OgreTechnique.h>
#include <OgreSceneNode.h>
#include <OgreSubEntity.h>
#include <OgreSubMesh.h>

namespace CharacterGen {

namespace {
Rgb colourFor(Surface s, const Params& p)
{
    switch (s) {
        case Surface::Skin: return p.skin;
        case Surface::Mouth: return p.lip;
        case Surface::Teeth: return {0.90f, 0.88f, 0.82f};
        case Surface::Sclera: return {0.93f, 0.92f, 0.90f};
        case Surface::Iris: return p.eye;
        case Surface::Lashes: return p.lash;
    }
    return p.skin;
}
const char* surfaceName(Surface s)
{
    switch (s) {
        case Surface::Skin: return "skin";
        case Surface::Mouth: return "mouth";
        case Surface::Teeth: return "teeth";
        case Surface::Sclera: return "sclera";
        case Surface::Iris: return "iris";
        case Surface::Lashes: return "lashes";
    }
    return "skin";
}
}  // namespace

void applyTints(Ogre::Entity* entity, const Params& params, const QString& prefix)
{
    if (!entity) return;
    auto& matMgr = Ogre::MaterialManager::getSingleton();
    Ogre::MeshPtr mesh = entity->getMesh();
    for (const auto& kv : mesh->getSubMeshNameMap()) {
        Surface surface;
        if (!surfaceForMesh(QString::fromStdString(kv.first), &surface)) continue;
        if (kv.second >= entity->getNumSubEntities()) continue;
        const std::string matName = (prefix + QLatin1Char('_') + QLatin1String(surfaceName(surface))).toStdString();
        const Rgb c = colourFor(surface, params);
        Ogre::MaterialPtr mat = matMgr.getByName(matName);
        if (!mat) mat = matMgr.create(matName, Ogre::ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME);
        Ogre::Pass* pass = mat->getTechnique(0)->getPass(0);
        pass->setLightingEnabled(true);
        pass->setDiffuse(Ogre::ColourValue(c[0], c[1], c[2]));
        pass->setAmbient(Ogre::ColourValue(c[0] * 0.5f, c[1] * 0.5f, c[2] * 0.5f));
        const bool glossy = surface == Surface::Sclera || surface == Surface::Iris || surface == Surface::Teeth;
        pass->setSpecular(glossy ? Ogre::ColourValue(0.4f, 0.4f, 0.4f) : Ogre::ColourValue(0.06f, 0.06f, 0.06f));
        pass->setShininess(glossy ? 60.0f : 12.0f);
        mat->compile();
        entity->getSubEntity(kv.second)->setMaterialName(matName);
    }
}

Ogre::SceneNode* createCharacterInScene(const Assets& assets, const Params& params, const QString& baseName,
                                        SceneBuild* report, QString* error)
{
    SceneBuild local;
    SceneBuild& rep = report ? *report : local;
    rep = SceneBuild();
    Dna::Rig rig = buildRig(assets, params, &rep.build, error);
    if (rig.meshes.empty()) return nullptr;
    Ogre::SceneNode* node = Dna::importRigToScene(rig, baseName, &rep.import, error);
    if (!node) return nullptr;
    for (auto it = node->getAttachedObjectIterator(); it.hasMoreElements();) {
        Ogre::MovableObject* mo = it.getNext();
        if (mo->getMovableType() == "Entity") applyTints(static_cast<Ogre::Entity*>(mo), params, QString::fromStdString(node->getName()));   // node names are unique: a reused material would keep the old tint
    }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.character"),
                                  QStringLiteral("create vertices=%1 morphs=%2").arg(rep.import.vertices).arg(rep.import.morphTargets));
    return node;
}

}  // namespace CharacterGen
