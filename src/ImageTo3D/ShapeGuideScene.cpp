#include "ShapeGuideScene.h"

#include "AutoRig.h"
#include "Manager.h"
#include "MeshGenBuilder.h"
#include "MeshImporterExporter.h"

#include <QFileInfo>
#include <QSet>

#include <OgreEntity.h>
#include <OgreSceneNode.h>

namespace ShapeGuideScene {

bool fromEntity(Ogre::Entity* entity, ShapeGuide::Mesh& out, QString* error)
{
    out = ShapeGuide::Mesh{};
    if (!entity || !AutoRig::gatherGeometry(entity, out.positions, out.indices)
        || out.empty()) {
        if (error)
            *error = QStringLiteral("the guide mesh has no readable triangles");
        return false;
    }
    return true;
}

namespace {

Ogre::Entity* findEntity(const QString& name)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr)
        return nullptr;
    for (Ogre::Entity* e : mgr->getEntities()) {
        if (!e)
            continue;
        if (QString::fromStdString(e->getName()) == name)
            return e;
        if (Ogre::SceneNode* n = e->getParentSceneNode();
            n && QString::fromStdString(n->getName()) == name)
            return e;
    }
    return nullptr;
}

// Import a mesh file, read every entity it brought in, then remove them.
bool fromFile(const QString& path, ShapeGuide::Mesh& out, QString* error)
{
    Manager* mgr = Manager::getSingletonPtr();
    if (!mgr) {
        if (error) *error = QStringLiteral("no scene to import %1 into").arg(path);
        return false;
    }
    QSet<Ogre::Entity*> before;
    for (Ogre::Entity* e : mgr->getEntities())
        before.insert(e);
    MeshImporterExporter::importer({QFileInfo(path).absoluteFilePath()});
    QList<Ogre::Entity*> added;
    for (Ogre::Entity* e : mgr->getEntities())
        if (e && !before.contains(e))
            added.append(e);
    out = ShapeGuide::Mesh{};
    QList<Ogre::SceneNode*> nodes;
    for (Ogre::Entity* e : added) {
        ShapeGuide::Mesh part;
        if (fromEntity(e, part)) {
            const uint32_t base = static_cast<uint32_t>(out.positions.size() / 3);
            out.positions.insert(out.positions.end(), part.positions.begin(), part.positions.end());
            for (uint32_t i : part.indices)
                out.indices.push_back(base + i);
        }
        if (Ogre::SceneNode* n = e->getParentSceneNode(); n && !nodes.contains(n))
            nodes.append(n);
    }
    for (Ogre::SceneNode* n : nodes)
        mgr->destroySceneNode(n);
    if (out.empty()) {
        if (error) *error = QStringLiteral("could not read a mesh from %1").arg(path);
        return false;
    }
    return true;
}

} // namespace

bool resolve(const QString& specIn, ShapeGuide::Mesh& out, QString* error)
{
    QString spec = specIn.trimmed();
    if (spec.startsWith(QStringLiteral("preset:")))
        spec = spec.mid(7);
    if (ShapeGuide::isPreset(spec)) {
        out = ShapeGuide::preset(spec);
        return true;
    }
    if (QFileInfo::exists(spec)) {
        if (spec.endsWith(QStringLiteral(".obj"), Qt::CaseInsensitive))
            return ShapeGuide::readObj(spec, out, error);
        return fromFile(spec, out, error);
    }
    if (Ogre::Entity* e = findEntity(spec))
        return fromEntity(e, out, error);
    QStringList ids;
    for (const ShapeGuide::PresetInfo& p : ShapeGuide::presets())
        ids << p.id;
    if (error)
        *error = QStringLiteral("unknown shape guide '%1': not a preset (%2), a mesh file, "
                                "or a mesh in the scene")
                     .arg(specIn, ids.join(QStringLiteral(", ")));
    return false;
}

Ogre::SceneNode* insert(const ShapeGuide::Mesh& mesh, const QString& baseName)
{
    if (mesh.empty())
        return nullptr;
    MeshGenPredictor::Result r;
    r.ok = true;
    // MeshGenBuilder turns non-TripoSR results 180° about Y; pre-turn so the
    // guide lands exactly as authored.
    r.positions = mesh.positions;
    for (size_t v = 0; v + 2 < r.positions.size(); v += 3) {
        r.positions[v] = -r.positions[v];
        r.positions[v + 2] = -r.positions[v + 2];
    }
    r.indices = mesh.indices;
    r.vertexCount = static_cast<int>(r.positions.size() / 3);
    r.triangleCount = static_cast<int>(r.indices.size() / 3);
    r.bakeTripoSROrientation = false;
    r.bakeTrellisUprightX = false;
    return MeshGenBuilder::buildSceneNode(r, baseName);
}

} // namespace ShapeGuideScene
