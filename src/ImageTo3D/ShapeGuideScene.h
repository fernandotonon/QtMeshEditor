#ifndef SHAPE_GUIDE_SCENE_H
#define SHAPE_GUIDE_SCENE_H

#include "ShapeGuide.h"

#include <QString>

namespace Ogre { class Entity; class SceneNode; }

// Ogre side of the shape guide (ShapeGuide.h): turns a scene entity, a mesh
// file or a preset into a guide mesh, and puts a preset into the scene as an
// ordinary editable mesh. MAIN thread only (Ogre).
namespace ShapeGuideScene {

// Mesh-local geometry of an entity (all submeshes).
bool fromEntity(Ogre::Entity* entity, ShapeGuide::Mesh& out, QString* error = nullptr);

// `spec` is one of:
//   a preset id ("house") or "preset:house"
//   an .obj file (read directly, no Ogre needed)
//   any other mesh file the editor imports (.glb/.fbx/...): imported, read,
//     and removed from the scene again
//   the name of an entity or scene node already in the scene
bool resolve(const QString& spec, ShapeGuide::Mesh& out, QString* error = nullptr);

// Adds the mesh to the scene as a plain grey mesh under a new node named
// after `baseName` (made unique). Returns the node, or null on failure.
Ogre::SceneNode* insert(const ShapeGuide::Mesh& mesh, const QString& baseName);

} // namespace ShapeGuideScene

#endif // SHAPE_GUIDE_SCENE_H
