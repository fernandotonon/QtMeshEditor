#ifndef SKINNING_DISPLAY_H
#define SKINNING_DISPLAY_H

#include <QString>

namespace Ogre {
    class Entity;
}

// Dual-quaternion skinning display option (issue #819, Slice D).
//
// No weight map fixes linear-blend volume collapse on twists (the
// "candy-wrapper" artifact) — that requires a different blend at
// render time. Ogre ships dual-quaternion hardware skinning via the
// RTSS HardwareSkinning sub-render-state; this helper toggles it
// per entity:
//
//   Linear          — Ogre's default path (software LBS, or hardware
//                     LBS where a material requests it). This is the
//                     app's out-of-the-box state.
//   DualQuaternion  — the entity's materials are imprinted for the
//                     RTSS hardware-skinning SRS in dual-quaternion
//                     mode and their generated techniques rebuilt.
//
// This is a RUNTIME SHADING choice only: exporters consume the
// vertex weights, which are unchanged — glTF/engines re-skin with
// their own blend. Entities whose bone count exceeds the RTSS
// hardware-skinning cap keep rendering through the default path
// (the SRS deactivates itself), so toggling is always safe.
//
// The active mode is tracked on the entity's UserObjectBindings so
// the Inspector can reflect it across selection changes.

class SkinningDisplay {
public:
    enum class Mode {
        Linear,          // default LBS path
        DualQuaternion,  // RTSS hardware DQS
    };

    // Applies `mode` to every material of `entity` and invalidates
    // their RTSS techniques so the next frame regenerates shaders.
    // Returns false (with `error` filled when given) when the
    // entity is null/skeleton-less or RTSS is unavailable.
    static bool apply(Ogre::Entity* entity, Mode mode,
                      QString* error = nullptr);

    // The mode last applied to `entity` (Linear when never toggled).
    static Mode current(const Ogre::Entity* entity);

    static QString modeToString(Mode mode);
    static Mode    modeFromString(const QString& s);
};

#endif // SKINNING_DISPLAY_H
