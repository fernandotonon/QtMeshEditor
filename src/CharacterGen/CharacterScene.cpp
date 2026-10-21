#include "CharacterScene.h"

#include "../Manager.h"
#include "../SentryReporter.h"
#include "../DNA/DnaCLI.h"
#include "../commands/AddFittedDnaCharacterCommand.h"

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

bool fitDnaToGeneratedBody(const Assets& assets, Params params, const QString& dnaPath, const QString& mode, DnaFit* out, QString* error)
{
    auto fail = [&](const QString& m) { if (error) *error = m; return false; };
    if (!out) return fail(QStringLiteral("no output"));
    params.bodyOnly = true;
    params.nativeHead = false;   // the fit target is the body cut at the neck
    params.bustDepth = 0.0f;
    QString err;
    Ogre::SceneNode* node = createCharacterInScene(assets, params, QStringLiteral("dna_target"), nullptr, &err);
    if (!node) return fail(err);
    Ogre::Entity* entity = nullptr;
    for (auto it = node->getAttachedObjectIterator(); it.hasMoreElements();) {
        Ogre::MovableObject* mo = it.getNext();
        if (mo->getMovableType() == "Entity") { entity = static_cast<Ogre::Entity*>(mo); break; }
    }
    if (!entity) { Manager::getSingleton()->destroySceneNode(node); return fail(QStringLiteral("the generated body has no entity")); }
    const auto snapshot = Dna::DnaCLI::snapshotTarget(entity);
    Manager::getSingleton()->destroySceneNode(node);   // the snapshot owns copies of everything the fit reads

    Dna::DnaCLI::FitOptions o;
    o.mode = mode.isEmpty() ? QStringLiteral("joints+surface") : mode;
    o.poses = true;
    o.poseSpace = true;
    out->rig = std::make_shared<Dna::Rig>();
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.character"), QStringLiteral("fit dna mode=%1").arg(o.mode));
    try {
        if (!Dna::DnaCLI::fitSnapshotReport(dnaPath, snapshot, o, out->rig.get(), &out->report, &err, &out->poseSpace))
            return fail(err.contains(QLatin1String("could be paired"))
                            ? err + QStringLiteral("\nThis looks like a DNA without a body skeleton (a face-only file such as SKM_Face.dna). Use a body DNA, or a combined body+head DNA.")
                            : err);
    } catch (const std::exception& e) {
        return fail(QString::fromUtf8(e.what()));
    }
    return true;
}

Ogre::SceneNode* addFittedCharacter(const DnaFit& fit, const QString& dnaPath, const QString& baseName, QString* error)
{
    AddFittedDnaCharacterCommand cmd(fit.rig, dnaPath, baseName, fit.poseSpace);
    cmd.redo();
    if (!cmd.created()) { if (error) *error = cmd.error(); return nullptr; }
    for (auto* n : Manager::getSingleton()->getSceneNodes())
        if (QString::fromStdString(n->getName()) == cmd.nodeName()) return n;
    if (error) *error = QStringLiteral("the fitted character node was not found");
    return nullptr;
}

}  // namespace CharacterGen
