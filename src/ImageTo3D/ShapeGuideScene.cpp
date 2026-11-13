#include "ShapeGuideScene.h"

#include "AutoRig.h"
#include "Manager.h"
#include "MeshGenBuilder.h"
#include "MeshImporterExporter.h"

#include <QFileInfo>
#include <QSet>

#include <utility>
#include <vector>

#include <OgreEntity.h>
#include <OgreCamera.h>
#include <OgreSceneManager.h>
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
    // Use the guide as it is SHOWN: a blockout stretched or turned with the
    // gizmo must steer the generation that way. Translation does not matter
    // (the guide is centred in the grid).
    if (Ogre::SceneNode* n = entity->getParentSceneNode()) {
        const Ogre::Quaternion q = n->_getDerivedOrientation();
        const Ogre::Vector3 s = n->_getDerivedScale();
        if (q != Ogre::Quaternion::IDENTITY || s != Ogre::Vector3::UNIT_SCALE) {
            for (size_t v = 0; v + 2 < out.positions.size(); v += 3) {
                const Ogre::Vector3 p = q * (s * Ogre::Vector3(out.positions[v], out.positions[v + 1],
                                                               out.positions[v + 2]));
                out.positions[v] = p.x;
                out.positions[v + 1] = p.y;
                out.positions[v + 2] = p.z;
            }
            // A mirroring scale turns the surface inside out: keep it outward.
            if (s.x * s.y * s.z < 0.0f)
                for (size_t t = 0; t + 2 < out.indices.size(); t += 3)
                    std::swap(out.indices[t + 1], out.indices[t + 2]);
        }
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
    // The importer frames the cameras on what it loaded (it moves their
    // parent nodes); this import is temporary, so put the view back after.
    std::vector<std::pair<Ogre::SceneNode*, Ogre::Vector3>> cameraNodes;
    if (Ogre::SceneManager* sm = mgr->getSceneMgr())
        for (const auto& [name, cam] : sm->getCameras())
            if (Ogre::SceneNode* cn = cam ? cam->getParentSceneNode() : nullptr)
                cameraNodes.emplace_back(cn, cn->getPosition());
    MeshImporterExporter::importer({QFileInfo(path).absoluteFilePath()});
    for (const auto& [cn, pos] : cameraNodes)
        cn->setPosition(pos);
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
    r.nodeScale = 1.0f;   // a blockout is authored in metres
    return MeshGenBuilder::buildSceneNode(r, baseName);
}

} // namespace ShapeGuideScene
