#ifdef ENABLE_MOCAP

#include "MocapPoseDebugOverlay.h"

#include "MocapLiveTypes.h"
#include "MocapPoseIkFk.h"
#include "../AnimationMerger.h"
#include "../MotionInbetween.h"

#include <Ogre.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

constexpr const char* kMatName = "MocapPoseDebug/Unlit";

using Vec3 = std::array<float, 3>;

Vec3 canonLm(const std::array<std::array<float, 3>, PoseIK::kLandmarkCount>& p,
             int i)
{
    return p[static_cast<size_t>(i)];
}

Vec3 add(const Vec3& a, const Vec3& b)
{
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
Vec3 mul(const Vec3& a, float s)
{
    return {a[0] * s, a[1] * s, a[2] * s};
}
float dot(const Vec3& a, const Vec3& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
float len(const Vec3& a)
{
    return std::sqrt(dot(a, a));
}
Vec3 mid(const Vec3& a, const Vec3& b)
{
    return mul(add(a, b), 0.5f);
}

Vec3 mulVec3(const Ogre::Quaternion& q, const Vec3& v)
{
    const Ogre::Vector3 r = q * Ogre::Vector3(v[0], v[1], v[2]);
    return {r.x, r.y, r.z};
}

Ogre::Vector3 toOgre(const Vec3& v, float scale)
{
    return {v[0] * scale, v[1] * scale, v[2] * scale};
}

Ogre::Quaternion quatFromArray(const std::array<float, 4>& q)
{
    return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
}

bool handResolved(uint32_t mask, int handRole)
{
    return (mask & (1u << static_cast<unsigned>(handRole))) != 0u;
}

const int kLmEdges[][2] = {
    {0, 1},   {1, 2},   {2, 3},   {3, 7},   {0, 4},   {4, 5},   {5, 6},
    {6, 8},   {9, 10},  {11, 12}, {11, 13}, {13, 15}, {15, 17}, {15, 19},
    {15, 21}, {17, 19}, {12, 14}, {14, 16}, {16, 18}, {16, 20}, {16, 22},
    {18, 20}, {11, 23}, {12, 24}, {23, 24}, {23, 25}, {24, 26}, {25, 27},
    {26, 28}, {27, 29}, {28, 30}, {29, 31}, {30, 32}, {27, 31}, {28, 32},
};

float skeletonHeight(const std::array<std::array<float, 3>, PoseIK::kLandmarkCount>& p)
{
    float mn = 1e9f, mx = -1e9f;
    for (const auto& v : p) {
        mn = std::min(mn, v[1]);
        mx = std::max(mx, v[1]);
    }
    const float h = mx - mn;
    return h > 1e-4f ? h : 1.65f;
}

void appendLines(Ogre::ManualObject* mo,
                 const std::vector<std::pair<Vec3, Vec3>>& segs, float scale,
                 const Ogre::ColourValue& colour)
{
    for (const auto& [a, b] : segs) {
        mo->position(toOgre(a, scale));
        mo->colour(colour);
        mo->position(toOgre(b, scale));
        mo->colour(colour);
    }
}

}  // namespace

void MocapPoseDebugOverlay::ensureMaterial()
{
    if (Ogre::MaterialManager::getSingleton().resourceExists(kMatName))
        return;
    auto mat = Ogre::MaterialManager::getSingleton().create(
        kMatName, Ogre::ResourceGroupManager::INTERNAL_RESOURCE_GROUP_NAME);
    if (!mat->getNumTechniques())
        mat->createTechnique();
    Ogre::Technique* tech = mat->getTechnique(0);
    if (!tech->getNumPasses())
        tech->createPass();
    Ogre::Pass* pass = tech->getPass(0);
    pass->setLightingEnabled(false);
    pass->setVertexColourTracking(Ogre::TVC_DIFFUSE);
    pass->setCullingMode(Ogre::CULL_NONE);
    pass->setDepthCheckEnabled(true);
    pass->setDepthWriteEnabled(false);
    pass->setSceneBlending(Ogre::SBT_TRANSPARENT_ALPHA);
}

void MocapPoseDebugOverlay::rebuildDrawables()
{
    if (!m_sceneMgr || !m_root)
        return;
    ensureMaterial();
    if (m_landmarks)
        m_sceneMgr->destroyManualObject(m_landmarks);
    if (m_poseIk)
        m_sceneMgr->destroyManualObject(m_poseIk);
    if (m_fingers)
        m_sceneMgr->destroyManualObject(m_fingers);
    m_landmarks = m_sceneMgr->createManualObject("MocapPoseDebug/Landmarks");
    m_poseIk = m_sceneMgr->createManualObject("MocapPoseDebug/PoseIK");
    m_fingers = m_sceneMgr->createManualObject("MocapPoseDebug/Fingers");
    m_landmarks->setDynamic(true);
    m_poseIk->setDynamic(true);
    m_fingers->setDynamic(true);
    m_landmarks->setCastShadows(false);
    m_poseIk->setCastShadows(false);
    m_fingers->setCastShadows(false);
    m_root->attachObject(m_landmarks);
    m_root->attachObject(m_poseIk);
    m_root->attachObject(m_fingers);
}

void MocapPoseDebugOverlay::attach(Ogre::SceneManager* sceneMgr,
                                   Ogre::SceneNode* entityNode)
{
    detach();
    if (!sceneMgr || !entityNode)
        return;
    m_sceneMgr = sceneMgr;
    m_anchor = entityNode;
    m_root = entityNode->createChildSceneNode("MocapPoseDebugRoot");
    m_root->setPosition(0.8f, 0.f, 0.f);
    rebuildDrawables();
}

void MocapPoseDebugOverlay::detach()
{
    if (!m_sceneMgr)
        return;
    if (m_landmarks) {
        m_sceneMgr->destroyManualObject(m_landmarks);
        m_landmarks = nullptr;
    }
    if (m_poseIk) {
        m_sceneMgr->destroyManualObject(m_poseIk);
        m_poseIk = nullptr;
    }
    if (m_fingers) {
        m_sceneMgr->destroyManualObject(m_fingers);
        m_fingers = nullptr;
    }
    if (m_root && m_anchor) {
        m_anchor->removeChild(m_root);
        m_sceneMgr->destroySceneNode(m_root);
    }
    m_root = nullptr;
    m_anchor = nullptr;
    m_sceneMgr = nullptr;
}

void MocapPoseDebugOverlay::update(const BodyLiveFrame& body, float entityHeightLocal)
{
    if (!m_landmarks || !m_poseIk || !m_fingers || !body.valid)
        return;

    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(body.world.data(), canon);

    const float skelH = skeletonHeight(canon);
    const float scale =
        (entityHeightLocal > 1e-3f ? entityHeightLocal : 1.8f) / skelH;

    auto visible = [&](int lm) {
        return body.visibility[static_cast<size_t>(lm)] >= 0.2f;
    };

    std::vector<std::pair<Vec3, Vec3>> lmLines;
    lmLines.reserve(std::size(kLmEdges));
    for (const auto& e : kLmEdges)
        lmLines.emplace_back(canonLm(canon, e[0]), canonLm(canon, e[1]));

    std::array<Vec3, PoseIK::kCanonicalRoles> joints{};
    MocapPoseIkFk::fkPoseIkJoints(body.quats, body.resolvedMask, canon, joints);

    std::vector<std::pair<Vec3, Vec3>> ikLines;
    for (int role = 0; role < PoseIK::kCanonicalRoles; ++role) {
        const int parent = MotionInbetween::canonicalParentOf(role);
        if (parent < 0)
            continue;
        if (!(body.resolvedMask & (1u << static_cast<unsigned>(role)))
            || !(body.resolvedMask & (1u << static_cast<unsigned>(parent))))
            continue;
        ikLines.emplace_back(joints[static_cast<size_t>(parent)],
                             joints[static_cast<size_t>(role)]);
    }

    auto appendHand21 = [&](std::vector<std::pair<Vec3, Vec3>>& dest,
                            const HandLandmarks& h, int handRole, int wristLm) {
        if (!h.valid)
            return;
        Vec3 wrist = canonLm(canon, wristLm);
        Ogre::Quaternion wristQ = Ogre::Quaternion::IDENTITY;
        if (handResolved(body.resolvedMask, handRole)) {
            wrist = joints[static_cast<size_t>(handRole)];
            const auto& hq = body.quats[static_cast<size_t>(handRole)];
            wristQ = Ogre::Quaternion(hq[3], hq[0], hq[1], hq[2]);
        }
        const float wx = h.cropXyz[0], wy = h.cropXyz[1];
        auto toCanon = [&](int i) -> Vec3 {
            const float dx = h.cropXyz[static_cast<size_t>(i * 3 + 0)] - wx;
            const float dy = h.cropXyz[static_cast<size_t>(i * 3 + 1)] - wy;
            const float dz = h.cropXyz[static_cast<size_t>(i * 3 + 2)]
                             - h.cropXyz[2];
            const Ogre::Vector3 local(dx, -dy, -dz);
            const Ogre::Vector3 c = wristQ * local * 0.28f;
            return add(wrist, {c.x, c.y, c.z});
        };
        static const int kHandEdges[][2] = {
            {0, 1},  {1, 2},  {2, 3},  {3, 4},  {0, 5},  {5, 6},  {6, 7},
            {7, 8},  {0, 9},  {9, 10}, {10, 11}, {11, 12}, {0, 13}, {13, 14},
            {14, 15}, {15, 16}, {0, 17}, {17, 18}, {18, 19}, {19, 20},
            {5, 9},  {9, 13}, {13, 17},
        };
        for (const auto& e : kHandEdges)
            dest.emplace_back(toCanon(e[0]), toCanon(e[1]));
    };

    // Coarse BlazePose tips on yellow only when 21-pt Hands is missing.
    struct HandTips {
        int handRole;
        int wristLm;
        int thumbLm;
        int indexLm;
        int pinkyLm;
        const HandLandmarks* hands;
    };
    const HandTips kHands[] = {
        {PoseIK::RHand, 16, 22, 20, 18, &body.hands.right},
        {PoseIK::LHand, 15, 21, 19, 17, &body.hands.left},
    };
    for (const HandTips& h : kHands) {
        if (h.hands->valid)
            continue;
        if (!handResolved(body.resolvedMask, h.handRole))
            continue;
        const Vec3 handJ = joints[static_cast<size_t>(h.handRole)];
        const auto& handQuat =
            body.quats[static_cast<size_t>(h.handRole)];
        const Ogre::Quaternion wristQ(handQuat[3], handQuat[0], handQuat[1],
                                      handQuat[2]);
        for (int tipLm : {h.thumbLm, h.indexLm, h.pinkyLm}) {
            if (!visible(tipLm))
                continue;
            const Vec3 wrist = canonLm(canon, h.wristLm);
            const Vec3 tip = MocapPoseIkFk::fingerTipFromScreenCrop(
                wrist, body.screenCrop.data(), h.wristLm, tipLm, wristQ);
            ikLines.emplace_back(handJ, tip);
        }
    }

    std::vector<std::pair<Vec3, Vec3>> fingerLines;
    appendHand21(fingerLines, body.hands.right, PoseIK::RHand, 16);
    appendHand21(fingerLines, body.hands.left, PoseIK::LHand, 15);
    std::array<std::array<float, 3>, AnimationMerger::kFingerSlots> fingerDirs{};
    if (!body.hands.right.valid || !body.hands.left.valid) {
        AnimationMerger::collectFingerDirsFromPoseLandmarks(
            body.world.data(), body.visibility.data(), fingerDirs,
            body.screenCrop.data(), nullptr, &body.quats, body.resolvedMask);
    }
    auto appendPoseRays = [&](int side, int handRole, int wristLm,
                              int thumbLm, int indexLm, int pinkyLm) {
        struct FingerRay {
            int finger;
            int tipLm;
        };
        const FingerRay rays[] = {
            {0, thumbLm}, {1, indexLm}, {4, pinkyLm},
        };
        constexpr float kRayLen = 0.085f;
        for (const FingerRay& fr : rays) {
            if (!visible(wristLm) || !visible(fr.tipLm))
                continue;
            const Vec3 w = canonLm(canon, wristLm);
            Vec3 tip = canonLm(canon, fr.tipLm);
            if (handResolved(body.resolvedMask, handRole)) {
                const auto& hq = body.quats[static_cast<size_t>(handRole)];
                const Ogre::Quaternion wristQ(hq[3], hq[0], hq[1], hq[2]);
                tip = MocapPoseIkFk::fingerTipFromScreenCrop(
                    w, body.screenCrop.data(), wristLm, fr.tipLm, wristQ);
            }
            fingerLines.emplace_back(w, tip);
            const int slot = AnimationMerger::fingerSlot(side, fr.finger, 0);
            if (slot < 0)
                continue;
            const auto& d = fingerDirs[static_cast<size_t>(slot)];
            if (d[0] == 0.f && d[1] == 0.f && d[2] == 0.f)
                continue;
            fingerLines.emplace_back(w, add(w, mul(d, kRayLen)));
        }
    };
    if (!body.hands.right.valid)
        appendPoseRays(0, PoseIK::RHand, 16, 22, 20, 18);
    if (!body.hands.left.valid)
        appendPoseRays(1, PoseIK::LHand, 15, 21, 19, 17);

    m_landmarks->clear();
    m_landmarks->begin("MocapPoseDebug/Unlit", Ogre::RenderOperation::OT_LINE_LIST);
    appendLines(m_landmarks, lmLines, scale,
                Ogre::ColourValue(0.1f, 0.95f, 1.f, 0.95f));
    m_landmarks->end();

    m_poseIk->clear();
    m_poseIk->begin("MocapPoseDebug/Unlit", Ogre::RenderOperation::OT_LINE_LIST);
    appendLines(m_poseIk, ikLines, scale,
                Ogre::ColourValue(1.f, 0.85f, 0.1f, 0.95f));
    m_poseIk->end();

    m_fingers->clear();
    m_fingers->begin("MocapPoseDebug/Unlit", Ogre::RenderOperation::OT_LINE_LIST);
    appendLines(m_fingers, fingerLines, scale,
                Ogre::ColourValue(1.f, 0.2f, 0.95f, 0.95f));
    m_fingers->end();
    m_lastScale = scale;
}

#endif  // ENABLE_MOCAP
