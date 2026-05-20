#ifndef PS1RIPMESHBUILDER_H
#define PS1RIPMESHBUILDER_H

#include "CaptureSnapshot.h"
#include "MeshReconstructor.h"

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
    };

    static bool attachToScene(const ReconstructedMesh &mesh, const QString &captureId,
                              const CaptureSnapshot *textureSource, BuildResult *resultOut,
                              QString *errorOut = nullptr);

    /** Places deduplicated meshes with one SceneNode per instance (#423). */
    static bool attachCaptureSetToScene(const ReconstructedCaptureSet &captureSet,
                                        const QString &captureId,
                                        const CaptureSnapshot *textureSource, BuildResult *resultOut,
                                        QString *errorOut = nullptr);
};

#endif // PS1RIPMESHBUILDER_H
