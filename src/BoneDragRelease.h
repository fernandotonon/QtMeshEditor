#ifndef BONE_DRAG_RELEASE_H
#define BONE_DRAG_RELEASE_H

#include <OgreVector3.h>
#include <OgreQuaternion.h>
#include <string>

namespace Ogre {
    class Bone;
    class Entity;
    class SkeletonInstance;
}
class QUndoCommand;

/// Pure-data helper that decides what to do with a bone at the end of a
/// gizmo drag, based on (auto-key on/off, animation selected/not, bone
/// changed/not, edit-rest mode). Extracted from TransformOperator::mouseReleaseEvent so
/// the rules are testable in isolation — without an OgreWidget, mouse
/// event, or QApplication.
///
/// Cases:
/// 1. Auto-key ON + animation active + not edit-rest + changed
///      → keep dragged TRS, release manualControlled (curve drives playback
///        through the new key written by callback). Returns Result::Commit.
/// 2. Otherwise + changed (edit-rest, no anim, or auto-key off)
///      → rest-pose authoring. Returns Result::CommitBind. Caller commits
///        bind via SkeletonEditor; does NOT call setInitialState here.
///
/// Auto-key off no longer reverts: the Inspector documents the bone gizmo
/// as a rest-pose editor, and reverting made Mixamo-enabled meshes snap
/// back on every release.
///
/// "No change" returns Result::NoOp.
class BoneDragRelease {
public:
    enum class Result {
        NoOp,          ///< Bone TRS is unchanged from before-state
        Commit,        ///< Animation + auto-key on: caller writes keyframe
        Revert,        ///< Deprecated — kept for ABI; apply() never returns this
        CommitBind,    ///< Rest-pose edit: caller commits bind
    };

    /// Apply the release-path rules to `bone`. The bone's current local TRS
    /// is treated as the after-state; `before*` describe what the bone was
    /// before the drag started.
    /// @param hasActiveAnim true when an animation is enabled on the entity
    /// @param autoKeyOn     true when auto-key toggle is on
    /// @param editRestMode  true when Inspector edit-rest mode is active
    static Result apply(Ogre::Bone* bone,
                        const Ogre::Vector3& beforePos,
                        const Ogre::Quaternion& beforeOrient,
                        const Ogre::Vector3& beforeScale,
                        bool hasActiveAnim,
                        bool autoKeyOn,
                        Ogre::Entity* entityForUpdate = nullptr,
                        bool editRestMode = false);
};

#endif // BONE_DRAG_RELEASE_H
