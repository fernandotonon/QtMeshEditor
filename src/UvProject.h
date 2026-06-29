#ifndef UV_PROJECT_H
#define UV_PROJECT_H

#include <OgreMatrix4.h>
#include <OgreVector2.h>
#include <QString>
#include <vector>

class EditableMesh;

/// Quick-start UV layouts via geometric projection (issue #463).
class UvProject
{
public:
    enum class Mode {
        View,
        Box,
        Cylinder,
        Sphere,
        ResetBox
    };

    struct VertChange {
        int subMeshIndex = 0;
        int vertexIndex = 0;
        Ogre::Vector2 oldUv;
        Ogre::Vector2 newUv;
    };

    /// Per-submesh triangle inclusion mask. When empty for a submesh, all
    /// triangles in that submesh are included.
    struct Selection {
        std::vector<std::vector<bool>> includeTriangle;
    };

    struct Options {
        Mode mode = Mode::Box;
        /// 0 = X, 1 = Y, 2 = Z (cylinder / sphere).
        int axis = 1;
        float boxScale = 1.0f;

        /// View projection matrices (world space). Required for Mode::View.
        Ogre::Matrix4 viewMatrix;
        Ogre::Matrix4 projMatrix;
        Ogre::Matrix4 worldMatrix;
        bool hasViewMatrices = false;
    };

    struct Report {
        bool applied = false;
        QString error;
        int vertsChanged = 0;
        std::vector<VertChange> changes;
    };

    static Report project(EditableMesh& mesh, const Selection& selection, const Options& opts);

    static QString modeToString(Mode mode);
};

#endif // UV_PROJECT_H
