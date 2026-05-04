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
/// changed/not). Extracted from TransformOperator::mouseReleaseEvent so
/// the rules are testable in isolation — without an OgreWidget, mouse
/// event, or QApplication.
///
/// Three cases:
/// 1. Animation active + auto-key ON + changed
///      → keep dragged TRS, release manualControlled (curve drives playback
///        through the new key written by callback). Returns Result::Commit.
///        Caller is responsible for writing the keyframe (via
///        AnimationControlController::autoKeyOnTransform) and pushing the
///        BoneTransformCommand.
/// 2. Animation active + auto-key OFF + changed
///      → revert bone to before-state. Returns Result::Revert. Caller does
///        not push undo (no commit).
/// 3. No active animation + changed
///      → setInitialState (commits new bind pose). Returns Result::CommitBind.
///        Caller pushes BoneTransformCommand for undo.
///
/// "No change" returns Result::NoOp.
class BoneDragRelease {
public:
    enum class Result {
        NoOp,          ///< Bone TRS is unchanged from before-state
        Commit,        ///< Animation + auto-key on: caller writes keyframe
        Revert,        ///< Animation + auto-key off: bone reverted in-place
        CommitBind,    ///< No animation: bone's initial state updated
    };

    /// Apply the release-path rules to `bone`. The bone's current local TRS
    /// is treated as the after-state; `before*` describe what the bone was
    /// before the drag started.
    /// @param hasActiveAnim true when an animation is selected in the panel
    /// @param autoKeyOn     true when auto-key toggle is on
    static Result apply(Ogre::Bone* bone,
                        const Ogre::Vector3& beforePos,
                        const Ogre::Quaternion& beforeOrient,
                        const Ogre::Vector3& beforeScale,
                        bool hasActiveAnim,
                        bool autoKeyOn,
                        Ogre::Entity* entityForUpdate = nullptr);
};

#endif // BONE_DRAG_RELEASE_H
