#include "KimodoCorrection.h"

#include "AnimProcessing/Utility.h"

#include <algorithm>
#include <cmath>

namespace AIMotion::KimodoCorrection {

namespace {

float finiteOrZero(float x) { return std::isfinite(x) ? x : 0.0f; }

bool fail(std::string* error, const char* msg)
{
    if (error) *error = msg;
    return false;
}

std::vector<float> maskOf(const std::vector<float>& m, size_t T) { return m.empty() ? std::vector<float>(T, 0.0f) : m; }

// The masks with every channel filled to one value per frame.
struct FrameMasks {
    std::vector<float> fullBody;
    std::vector<float> leftHand;
    std::vector<float> rightHand;
    std::vector<float> leftFoot;
    std::vector<float> rightFoot;
    std::vector<float> root;
};

bool frameMasks(const Masks& in, size_t T, FrameMasks* out)
{
    *out = {maskOf(in.fullBody, T), maskOf(in.leftHand, T), maskOf(in.rightHand, T),
            maskOf(in.leftFoot, T), maskOf(in.rightFoot, T), maskOf(in.root, T)};
    for (const auto* m : {&out->fullBody, &out->leftHand, &out->rightHand, &out->leftFoot, &out->rightFoot, &out->root})
        if (m->size() != T) return false;
    return true;
}

bool sizesMatch(const MotionArrays& a, const Rig& rig, std::string* error)
{
    const size_t J = rig.parents.size();
    if (!a.rootTranslations || !a.jointRotationsWxyz || !a.rootTarget || !a.rotationsTargetWxyz || !a.contacts)
        return fail(error, "correctMotion: missing motion array");
    if (J == 0 || rig.translations.size() != J || rig.rotationsXyzw.size() != J)
        return fail(error, "correctMotion: rig arrays do not match");
    for (int idx : {rig.leftHand, rig.rightHand, rig.leftFoot, rig.rightFoot})
        if (idx < 0 || size_t(idx) >= J) return fail(error, "correctMotion: hand/foot index out of range");
    const size_t T = a.rootTranslations->size() / 3;
    if (a.rootTranslations->size() != 3 * T || a.jointRotationsWxyz->size() != 4 * J * T || a.rootTarget->size() != 3 * T
        || a.rotationsTargetWxyz->size() != 4 * J * T || a.contacts->size() != 4 * T)
        return fail(error, "correctMotion: array sizes do not match the frame count");
    return true;
}

std::vector<Math::Transform> restPose(const Rig& rig)
{
    std::vector<Math::Transform> pose(rig.parents.size());
    for (size_t i = 0; i < pose.size(); ++i) {
        const auto& t = rig.translations[i];
        const auto& q = rig.rotationsXyzw[i];
        pose[i].SetTranslation(Math::Vector(t[0], t[1], t[2]));
        pose[i].SetRotation(Math::Quaternion(q[0], q[1], q[2], q[3]));
    }
    return pose;
}

// End-effector pins (hands, feet), active only where the frame is not a full-body keyframe.
std::vector<Animation::ContactInfo> endEffectorPins(const Rig& rig, const FrameMasks& m)
{
    const int joints[4] = {rig.leftHand, rig.rightHand, rig.leftFoot, rig.rightFoot};
    const float hints[4] = {-0.1f, -0.1f, 0.1f, 0.1f};
    const std::vector<float>* masks[4] = {&m.leftHand, &m.rightHand, &m.leftFoot, &m.rightFoot};
    std::vector<Animation::ContactInfo> pins(4);
    for (size_t p = 0; p < 4; ++p) {
        pins[p].jointIndex = joints[p];
        pins[p].hintOffset = Math::Vector(0.0f, 0.0f, hints[p]);
        pins[p].contactMask.reserve(m.fullBody.size());
        for (size_t i = 0; i < m.fullBody.size(); ++i) pins[p].contactMask.push_back((1.0f - m.fullBody[i]) * (*masks[p])[i]);
    }
    return pins;
}

// Ankle contact (heel or toe on the ground) for one foot; channels `heel`/`toe` index Kimodo's 4 contacts.
Animation::ContactInfo ankleContact(const Rig& rig, const std::vector<Math::Transform>& rest, int foot,
                                    const std::vector<float>& footMask, const std::vector<float>& contacts, int heel, int toe)
{
    Animation::ContactInfo c;
    c.jointIndex = foot;
    c.hintOffset = Math::Vector(0.0f, 0.0f, 0.1f);
    c.minHeight = Animation::JointLocalToGlobal(rig.parents, foot, rest).GetTranslation().GetY();
    c.contactMask.resize(footMask.size());
    for (size_t i = 0; i < footMask.size(); ++i) {
        // A masked (constrained) foot is not a contact; the heel counts as one when the toe is.
        const float heelOn = footMask[i] != 0.0f ? 0.0f : contacts[4 * i + size_t(heel)];
        const float toeOn = footMask[i] != 0.0f ? 0.0f : contacts[4 * i + size_t(toe)];
        c.contactMask[i] = std::min(toeOn + heelOn, 1.0f);
    }
    return c;
}

Animation::ContactInfo toeContact(int toeJoint, float minHeight, const std::vector<float>& footMask,
                                  const std::vector<float>& contacts, int toe)
{
    Animation::ContactInfo c;
    c.jointIndex = toeJoint;
    c.contactType = Animation::kOneBone;
    c.minHeight = minHeight;
    c.contactMask.resize(footMask.size());
    for (size_t i = 0; i < footMask.size(); ++i) c.contactMask[i] = footMask[i] != 0.0f ? 0.0f : contacts[4 * i + size_t(toe)];
    return c;
}

int childOf(const std::vector<int>& parents, int joint)
{
    int child = -1;
    for (size_t i = 0; i < parents.size(); ++i)
        if (parents[i] == joint) child = int(i);   // the last child, as the upstream binding picks it
    return child;
}

std::vector<Animation::ContactInfo> footContacts(const Rig& rig, const std::vector<Math::Transform>& rest, const FrameMasks& m,
                                                 const std::vector<float>& contacts)
{
    std::vector<Animation::ContactInfo> out;
    out.push_back(ankleContact(rig, rest, rig.rightFoot, m.rightFoot, contacts, 2, 3));
    out.push_back(ankleContact(rig, rest, rig.leftFoot, m.leftFoot, contacts, 0, 1));
    const int leftToe = childOf(rig.parents, rig.leftFoot);
    const int rightToe = childOf(rig.parents, rig.rightFoot);
    if (leftToe != -1 && rightToe != -1) {
        const float toeY = Animation::JointLocalToGlobal(rig.parents, rightToe, rest).GetTranslation().GetY();
        out.push_back(toeContact(rightToe, toeY, m.rightFoot, contacts, 3));
        out.push_back(toeContact(leftToe, toeY, m.leftFoot, contacts, 1));
    }
    return out;
}

void setTransforms(std::vector<std::vector<Math::Transform>>& poses, const std::vector<float>& root,
                   const std::vector<float>& rotWxyz, size_t J)
{
    for (size_t f = 0; f < poses.size(); ++f) {
        poses[f][0].SetTranslation(Math::Vector(finiteOrZero(root[3 * f]), finiteOrZero(root[3 * f + 1]),
                                                finiteOrZero(root[3 * f + 2])));
        for (size_t j = 0; j < J; ++j) {
            const float* q = &rotWxyz[4 * (J * f + j)];
            Math::Quaternion quat(finiteOrZero(q[1]), finiteOrZero(q[2]), finiteOrZero(q[3]), finiteOrZero(q[0]));
            quat.Normalize();
            poses[f][j].SetRotation(quat);
        }
    }
}

void readBack(const std::vector<std::vector<Math::Transform>>& poses, size_t J, std::vector<float>& root, std::vector<float>& rotWxyz)
{
    for (size_t f = 0; f < poses.size(); ++f) {
        const Math::Vector t = poses[f][0].GetTranslation();
        root[3 * f] = t.GetX();
        root[3 * f + 1] = t.GetY();
        root[3 * f + 2] = t.GetZ();
        for (size_t j = 0; j < J; ++j) {
            const Float4 q = poses[f][j].GetRotation().ToFloat4();
            float* out = &rotWxyz[4 * (J * f + j)];
            out[0] = q.m_w;
            out[1] = q.m_x;
            out[2] = q.m_y;
            out[3] = q.m_z;
        }
    }
}

} // namespace

bool correctMotion(const MotionArrays& motion, const Masks& masks, const Rig& rig, const Params& params, std::string* error)
{
    if (!sizesMatch(motion, rig, error)) return false;
    const size_t J = rig.parents.size();
    const size_t T = motion.rootTranslations->size() / 3;
    FrameMasks m;
    if (!frameMasks(masks, T, &m)) return fail(error, "correctMotion: all masks must have one value per frame");

    const std::vector<Math::Transform> rest = restPose(rig);
    std::vector<Animation::ContactInfo> pins = endEffectorPins(rig, m);
    std::vector<Animation::ContactInfo> contacts = footContacts(rig, rest, m, *motion.contacts);

    std::vector<std::vector<Math::Transform>> poses(T, rest);
    std::vector<std::vector<Math::Transform>> targets(T, rest);
    setTransforms(poses, *motion.rootTranslations, *motion.jointRotationsWxyz, J);
    setTransforms(targets, *motion.rootTarget, *motion.rotationsTargetWxyz, J);

    Animation::CorrectMotion(poses, targets, m.fullBody, m.root, contacts, pins, rig.parents, rest,
                             params.contactThreshold, params.rootMargin, rig.hasDoubleAnkleJoints);
    readBack(poses, J, *motion.rootTranslations, *motion.jointRotationsWxyz);
    return true;
}

} // namespace AIMotion::KimodoCorrection
