#ifndef CHARACTERGEN_CHARACTERSCENE_H
#define CHARACTERGEN_CHARACTERSCENE_H

// Character generator, Ogre side: turns a built Dna::Rig into a skinned entity (the DNA scene importer does the skeleton, the
// skinning and the ARKit morph targets) and gives every part its tint material.

#include <QVariantMap>
#include <memory>
#include <vector>
#include "../PoseSpaceRbf.h"
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
void applyTints(Ogre::Entity* entity, const Params& params, const QString& prefix);

/// A MetaHuman body DNA fitted to the generated body: the character is built body-only as the shape target, the DNA body
/// (skeleton, skin weights, correctives) is wrapped onto it, and the temporary target is removed again. Needs an active scene.
struct DnaFit {
    std::shared_ptr<Dna::Rig> rig;
    std::shared_ptr<std::vector<PoseSpace::Set>> poseSpace;
    QVariantMap report;
};
bool fitDnaToGeneratedBody(const Assets& assets, Params params, const QString& dnaPath, const QString& mode, DnaFit* out, QString* error);

/// Add the fitted MetaHuman character to the scene (the same build the Fit DNA Body panel does, correctives included).
Ogre::SceneNode* addFittedCharacter(const DnaFit& fit, const QString& dnaPath, const QString& baseName, QString* error);

}  // namespace CharacterGen

#endif  // CHARACTERGEN_CHARACTERSCENE_H
