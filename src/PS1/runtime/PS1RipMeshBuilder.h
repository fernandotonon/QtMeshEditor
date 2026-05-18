#ifndef PS1RIPMESHBUILDER_H
#define PS1RIPMESHBUILDER_H

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
                              BuildResult *resultOut, QString *errorOut = nullptr);
};

#endif // PS1RIPMESHBUILDER_H
