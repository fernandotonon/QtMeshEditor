#ifdef ENABLE_MOCAP

#include "MocapPoseDebugOverlay.h"

#include "MocapLiveTypes.h"
#include "MocapPoseIkFk.h"
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
    m_landmarks = m_sceneMgr->createManualObject("MocapPoseDebug/Landmarks");
    m_poseIk = m_sceneMgr->createManualObject("MocapPoseDebug/PoseIK");
    m_landmarks->setCastShadows(false);
    m_poseIk->setCastShadows(false);
    m_root->attachObject(m_landmarks);
    m_root->attachObject(m_poseIk);
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
    if (!m_landmarks || !m_poseIk || !body.valid)
        return;

    std::array<std::array<float, 3>, PoseIK::kLandmarkCount> canon{};
    PoseIK::Solver::canonicalizeMediaPipeWorld(body.world.data(), canon);

    const float skelH = skeletonHeight(canon);
    const float scale =
        (entityHeightLocal > 1e-3f ? entityHeightLocal : 1.8f) / skelH;

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
    m_lastScale = scale;
}

#endif  // ENABLE_MOCAP
