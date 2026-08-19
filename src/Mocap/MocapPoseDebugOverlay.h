#ifndef MOCAPPOSEDEBUGOVERLAY_H
#define MOCAPPOSEDEBUGOVERLAY_H

#ifdef ENABLE_MOCAP

#include "MocapLiveTypes.h"

namespace Ogre {
class ManualObject;
class SceneManager;
class SceneNode;
}

// Live debug draw: what PoseIK / MediaPipe think the body looks like (canonical
// +Y up, +Z forward frame), scaled beside the driven entity. Cyan = raw
// landmark stick figure; yellow = 22-joint FK from PoseIK quats (coarse
// pose tips only when 21-pt Hands is missing); magenta = 21-point Hands
// skeleton, with pose-ray fallback per missing hand.
class MocapPoseDebugOverlay {
public:
    void attach(Ogre::SceneManager* sceneMgr, Ogre::SceneNode* entityNode);
    void detach();
    void update(const BodyLiveFrame& body, float entityHeightLocal);

private:
    void ensureMaterial();
    void rebuildDrawables();

    Ogre::SceneManager* m_sceneMgr = nullptr;
    Ogre::SceneNode* m_anchor = nullptr;
    Ogre::SceneNode* m_root = nullptr;
    Ogre::ManualObject* m_landmarks = nullptr;
    Ogre::ManualObject* m_poseIk = nullptr;
    Ogre::ManualObject* m_fingers = nullptr;
    float m_lastScale = 1.f;
};

#endif  // ENABLE_MOCAP
#endif  // MOCAPPOSEDEBUGOVERLAY_H
