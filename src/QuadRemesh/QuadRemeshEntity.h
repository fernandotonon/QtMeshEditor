#ifndef QUAD_REMESH_ENTITY_H
#define QUAD_REMESH_ENTITY_H

#include "QuadRemeshCore.h"

#include <OgreMesh.h>

#include <QJsonObject>
#include <QString>

#include <string>

namespace Ogre { class Entity; }

// Ogre adapter for the QuadriFlow remesh (QuadRemeshCore is the pure half).
//
// Split in three so the GUI can keep Ogre on the main thread and the heavy
// remesh on a worker:
//   readSource (main thread)  →  QuadRemesh::remesh (any thread)  →
//   buildMesh  (main thread).
// remeshEntity() chains them for the blocking CLI/MCP callers.
//
// The result is ONE submesh with one material: a clone of the source's first
// material whose 2D texture slots are rebound to the re-baked atlas (all
// source materials are baked into the same atlas, per source triangle). A
// skinned source keeps its skeleton; the weights come from the closest point
// on the original surface. Quads ride the `qtme.faces.0` n-gon binding, so
// FBX/OBJ export and Edit Mode see them.
namespace QuadRemeshEntity {

struct SourceInfo {
    std::string templateMaterial;   // material cloned for the result
    QString skeletonName;           // empty for static meshes
    QString meshName;
};

bool readSource(Ogre::Entity* entity, QuadRemesh::SourceMesh& out, SourceInfo& info,
                QString* error = nullptr);

struct Built {
    bool ok = false;
    QString error;
    Ogre::MeshPtr mesh;
    std::string materialName;
    QStringList texturePaths;       // PNGs written for the baked slots
};

// `textureDir` empty = <AppData>/generated_textures/remesh. The directory is
// registered as a resource location so the viewport and exporters find the
// PNGs.
Built buildMesh(const QuadRemesh::Result& result, const SourceInfo& info,
                const std::string& baseName, const QString& textureDir = QString());

struct Outcome {
    bool ok = false;
    bool cancelled = false;
    QString error;
    Ogre::MeshPtr mesh;
    QuadRemesh::Result result;      // stats (+ buffers)
    QStringList texturePaths;
};

Outcome remeshEntity(Ogre::Entity* entity, const QuadRemesh::Options& opts,
                     const QString& textureDir = QString());

QJsonObject reportToJson(const QuadRemesh::Result& r);
QString reportToText(const QuadRemesh::Result& r);

QuadRemesh::VoxelMode voxelModeFromString(const QString& s, bool* ok = nullptr);

} // namespace QuadRemeshEntity

#endif // QUAD_REMESH_ENTITY_H
