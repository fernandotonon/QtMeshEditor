#ifndef DNA_DNATEXTURES_H
#define DNA_DNATEXTURES_H

// Bind DCC-exported textures to an imported MetaHuman character (epic #1095, follow-ups).
//
// A DNA file carries geometry and rig data, never textures: they come from the DCC export (Maya/Unreal) and sit next
// to it. The importer used to leave every character on the shared white material. Companion files are found BY NAME
// in the DNA's folder (and a "textures" subfolder): a file belongs to a mesh when its name contains the mesh's token
// (the DNA mesh name before "_lod", e.g. "head_lod0_mesh" -> "head"), and its map type comes from a keyword
// (basecolor/albedo/diffuse/color, normal, roughness). A character with a single mesh also accepts files that name
// no mesh at all. Metallic / ambient occlusion are NOT bound: the editor packs them into one ORM texture (the
// `metallic` slot's channel layout), and a plain grayscale map there would read the wrong channel — they are reported.
//
// The matching is pure (file names only) and unit-tested; the Ogre part clones the white material per submesh so the
// shared BaseWhite is never modified.

#include <QString>
#include <QStringList>

#include <vector>

namespace Ogre { class Entity; }

namespace Dna {
struct Rig;

struct MeshTextures {
    QString meshName;     ///< the DNA mesh name
    QString albedo, normal, roughness;   ///< absolute paths, empty = none found
};

struct TextureScan {
    std::vector<MeshTextures> perMesh;
    QStringList notes;    ///< e.g. metallic/AO files that were found but are not bound
};

/// Pure: look for companion textures of the meshes in `meshNames` next to `dnaPath`.
TextureScan findCompanionTextures(const QString& dnaPath, const QStringList& meshNames);

/// Bind what findCompanionTextures found to `entity`'s submeshes (submesh i = meshNames[i]). Returns how many
/// submeshes received at least one texture; `notes` gets a line per skipped/unbindable item.
int bindCompanionTextures(Ogre::Entity* entity, const QString& dnaPath, const QStringList& meshNames, QStringList* notes);

/// "head_lod0_mesh" -> "head": the part of a DNA mesh name that textures are named after.
QString meshToken(const QString& meshName);

}  // namespace Dna

#endif  // DNA_DNATEXTURES_H
