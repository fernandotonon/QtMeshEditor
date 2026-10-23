#ifndef CHARACTERGEN_CHARACTERSCENE_H
#define CHARACTERGEN_CHARACTERSCENE_H

// Character generator, Ogre side: turns a built Dna::Rig into a skinned entity (the DNA scene importer does the skeleton, the
// skinning and the ARKit morph targets) and gives every part its tint material.

#include "CharacterModel.h"
#include "../DNA/DnaScene.h"

#include <QString>

namespace Ogre { class SceneNode; class Entity; }

namespace CharacterGen {

struct SceneBuild {
    BuildReport build;
    Dna::ImportReport import;
};

/// Build and add a character to the active scene under a new node named after `baseName`. Returns nullptr and sets `error` on
/// failure. Requires an active Ogre scene.
Ogre::SceneNode* createCharacterInScene(const Assets& assets, const Params& params, const QString& baseName,
                                        SceneBuild* report = nullptr, QString* error = nullptr);

/// (Re)assign the tint material of every submesh of `entity` from `params` (skin / mouth / teeth / sclera / iris / lashes).
/// Materials are named "<prefix>_<surface>" and shared by all parts of that surface (body + head skin stay the same tone).
/// `skinTextureFile` (optional): a PNG baked for the body's UVs, bound as the body's diffuse texture; `hairTextureFile`: the
/// hairstyle's PNG (alpha-tested cards).
void applyTints(Ogre::Entity* entity, const Params& params, const QString& prefix, const QString& skinTextureFile = QString(),
                const QString& hairTextureFile = QString());

}  // namespace CharacterGen

#endif  // CHARACTERGEN_CHARACTERSCENE_H
