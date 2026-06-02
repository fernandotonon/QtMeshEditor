#ifndef MESH_DEPTH_RENDERER_H
#define MESH_DEPTH_RENDERER_H

#include <QImage>
#include <QString>

namespace Ogre {
    class Entity;
}

// Renders a grayscale depth map of a single entity for use as a
// ControlNet conditioning image (issue #403, epic #397).
//
// ControlNet "depth" expects a grayscale image where near surfaces
// are bright and far surfaces are dark (the MiDaS convention). We
// produce that WITHOUT custom shaders — which would otherwise need
// per-backend GLSL/HLSL/Metal authoring — by:
//
//   1. Creating an offscreen RTT (reusing the same Ogre RTT pattern
//      as ModelTurntableRenderer).
//   2. Cloning the entity into a private scene-manager-free pass is
//      overkill; instead we override the viewport's material scheme
//      to a flat white emissive material and enable fixed-function
//      EXP/LINEAR fog from near→far over the entity's depth range.
//      White-emissive geometry fades to the (black) fog colour with
//      distance, yielding a near=white / far=black gradient.
//   3. Reading the RTT back to a QImage (RGB8, grayscale).
//
// The camera is auto-framed on the entity's bounding sphere so the
// whole mesh fills the frame — matching how the generated texture
// will be planar-projected back. (A future revision can render from
// the live editor viewport camera instead; framing is the safe v1.)
class MeshDepthRenderer {
public:
    // Render `entity`'s depth map at `size` x `size`. Returns a
    // grayscale QImage on success, or a null QImage on failure
    // (errorOut populated). Must be called on the main/render thread
    // — it touches the Ogre scene manager.
    static QImage renderDepthMap(Ogre::Entity* entity,
                                 int size,
                                 QString* errorOut = nullptr);

    // Release the cached RTT / camera / scene nodes. Safe to call
    // when Ogre is shutting down.
    static void shutdown();
};

#endif // MESH_DEPTH_RENDERER_H
