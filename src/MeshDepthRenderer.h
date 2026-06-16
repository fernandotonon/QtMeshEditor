#ifndef MESH_DEPTH_RENDERER_H
#define MESH_DEPTH_RENDERER_H

#include <QImage>
#include <QString>

#include <OgreMatrix4.h>
#include <OgreVector3.h>

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
    // A camera view to capture from. `dir` is the WORLD-space direction
    // pointing FROM the camera TOWARD the mesh centre, normalised by the
    // renderer. front = (0,0,-1) because imported characters (Mixamo et al.)
    // face -Z, so a camera on the -Z side looking toward +Z sees the front.
    // `up` is the camera up-vector hint (defaults to +Y; use ±Z for top/bottom
    // views where the look direction is parallel to +Y).
    struct View {
        Ogre::Vector3 dir = Ogre::Vector3(0, 0, -1);
        Ogre::Vector3 up  = Ogre::Vector3::UNIT_Y;
        const char*   name = "front";
    };

    // Built-in named views. front/back differ only in sign of Z.
    static View front()  { return { Ogre::Vector3(0, 0, -1), Ogre::Vector3::UNIT_Y, "front"  }; }
    static View back()   { return { Ogre::Vector3(0, 0,  1), Ogre::Vector3::UNIT_Y, "back"   }; }
    static View left()   { return { Ogre::Vector3(-1, 0, 0), Ogre::Vector3::UNIT_Y, "left"   }; }
    static View right()  { return { Ogre::Vector3( 1, 0, 0), Ogre::Vector3::UNIT_Y, "right"  }; }
    static View top()    { return { Ogre::Vector3(0,  1, 0), Ogre::Vector3(0,0,1),  "top"    }; }
    static View bottom() { return { Ogre::Vector3(0, -1, 0), Ogre::Vector3(0,0,1),  "bottom" }; }

    // Result of a view render: the grayscale depth image PLUS the exact
    // view/projection matrices and camera position used. The multi-view
    // baker re-projects mesh triangles through THESE matrices so the bake
    // aligns pixel-for-pixel with what ControlNet was conditioned on.
    struct RenderResult {
        QImage        depth;            // null on failure
        Ogre::Matrix4 viewMatrix;
        Ogre::Matrix4 projMatrix;
        Ogre::Vector3 camPosition = Ogre::Vector3::ZERO;
        Ogre::Vector3 camDirection = Ogre::Vector3::ZERO;  // normalised view dir
    };

    // Render `entity`'s depth map at `size` x `size` from `view`. Returns a
    // RenderResult whose `.depth` is null on failure (errorOut populated).
    // Must be called on the main/render thread — it touches the Ogre scene
    // manager.
    static RenderResult renderDepthMapView(Ogre::Entity* entity,
                                           int size,
                                           const View& view,
                                           QString* errorOut = nullptr);

    // Back-compat convenience: front-view depth image only (issue #403).
    static QImage renderDepthMap(Ogre::Entity* entity,
                                 int size,
                                 QString* errorOut = nullptr);

    // Release the cached RTT / camera / scene nodes. Safe to call
    // when Ogre is shutting down.
    static void shutdown();
};

#endif // MESH_DEPTH_RENDERER_H
