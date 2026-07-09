#ifndef PS1RIPMESHBUILDER_H
#define PS1RIPMESHBUILDER_H

#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"
#include "Ps1CoordinateNormalizer.h"

#include <QHash>
#include <QImage>
#include <QString>

namespace Ogre {
class Entity;
class SceneNode;
}

/** Attaches reconstructed capture meshes to the live Ogre scene (#422). */
class PS1RipMeshBuilder
{
public:
    struct BuildResult {
        Ogre::SceneNode *sceneNode = nullptr;
        Ogre::Entity *entity = nullptr;
        int vertexCount = 0;
        int triangleCount = 0;
        /** Decoded texture page (256×256 RGBA) keyed by logical material
         *  name, populated for the inspector's Texture / Material thumbs
         *  (#426). Only textured materials are present; mono / shaded
         *  materials have no entry. */
        QHash<QString, QImage> textureImages;
    };

    static bool attachToScene(const ReconstructedMesh &mesh, const QString &captureId,
                              const CaptureSnapshot *textureSource, BuildResult *resultOut,
                              QString *errorOut = nullptr);

    /** Places deduplicated meshes with one SceneNode per instance (#423).
     *  Applies `normalize` as SceneNode scale on each newly created capture
     *  node so the user's flip/scale settings take effect immediately (#424).
     *  Instances carrying a tracked GTE matrix (#816) additionally get their
     *  group rotation applied as node orientation. */
    static bool attachCaptureSetToScene(const ReconstructedCaptureSet &captureSet,
                                        const QString &captureId,
                                        const CaptureSnapshot *textureSource, BuildResult *resultOut,
                                        QString *errorOut = nullptr,
                                        const Ps1NormalizerSettings &normalize = {});

    /** Converts a unit-normalised GTE rotation (`ReconstructedInstance::rot`,
     *  row-major, camera = R·model) into the editor axis convention used by
     *  `GteInverse::modelToEditor` (Y/Z negated): out = S·R·S with
     *  S = diag(1,-1,-1). Pure-data so the conversion is testable without
     *  Ogre (#816). */
    static void editorRotationFromGte(const float rot[9], float out[9]);
};

#endif // PS1RIPMESHBUILDER_H
