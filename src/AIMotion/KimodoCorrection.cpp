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

void setTransforms(std::vector<std::vector<Math::Transform>>& poses, const std::vector<float>& root,
                   const std::vector<float>& rotWxyz, size_t T, size_t J)
{
    for (size_t f = 0; f < T; ++f)
        poses[f][0].SetTranslation(Math::Vector(finiteOrZero(root[3 * f]), finiteOrZero(root[3 * f + 1]),
                                                finiteOrZero(root[3 * f + 2])));
    for (size_t f = 0; f < T; ++f)
        for (size_t j = 0; j < J; ++j) {
            const float* q = &rotWxyz[4 * (J * f + j)];
            Math::Quaternion quat(finiteOrZero(q[1]), finiteOrZero(q[2]), finiteOrZero(q[3]), finiteOrZero(q[0]));
            quat.Normalize();
            poses[f][j].SetRotation(quat);
        }
}

} // namespace

bool correctMotion(std::vector<float>& rootTranslations, std::vector<float>& jointRotationsWxyz,
                   const std::vector<float>& rootTarget, const std::vector<float>& rotationsTargetWxyz,
                   const Masks& masksIn, const std::vector<float>& contacts, const Rig& rig,
                   float contactThreshold, float rootMargin, std::string* error)
{
    const size_t J = rig.parents.size();
    const size_t T = rootTranslations.size() / 3;
    if (J == 0 || rig.translations.size() != J || rig.rotationsXyzw.size() != J)
        return fail(error, "correctMotion: rig arrays do not match");
    for (int idx : {rig.leftHand, rig.rightHand, rig.leftFoot, rig.rightFoot})
        if (idx < 0 || size_t(idx) >= J) return fail(error, "correctMotion: hand/foot index out of range");
    if (rootTranslations.size() != 3 * T || jointRotationsWxyz.size() != 4 * J * T || rootTarget.size() != 3 * T
        || rotationsTargetWxyz.size() != 4 * J * T || contacts.size() != 4 * T)
        return fail(error, "correctMotion: array sizes do not match the frame count");
    const auto maskOf = [T](const std::vector<float>& m) { return m.empty() ? std::vector<float>(T, 0.0f) : m; };
    const std::vector<float> fullBody = maskOf(masksIn.fullBody), lh = maskOf(masksIn.leftHand), rh = maskOf(masksIn.rightHand),
                             lf = maskOf(masksIn.leftFoot), rf = maskOf(masksIn.rightFoot), rootMask = maskOf(masksIn.root);
    for (const auto* m : {&fullBody, &lh, &rh, &lf, &rf, &rootMask})
        if (m->size() != T) return fail(error, "correctMotion: all masks must have one value per frame");

    std::vector<Math::Transform> defaultPose(J);
    for (size_t i = 0; i < J; ++i) {
        defaultPose[i].SetTranslation(Math::Vector(rig.translations[i][0], rig.translations[i][1], rig.translations[i][2]));
        defaultPose[i].SetRotation(Math::Quaternion(rig.rotationsXyzw[i][0], rig.rotationsXyzw[i][1],
                                                    rig.rotationsXyzw[i][2], rig.rotationsXyzw[i][3]));
    }
    const std::vector<int>& parents = rig.parents;

    // End-effector pins (hands, feet), active only where the frame is not a full-body keyframe.
    std::vector<Animation::ContactInfo> pins(4);
    pins[0].jointIndex = rig.leftHand;
    pins[0].hintOffset = Math::Vector(0.0f, 0.0f, -0.1f);
    pins[1].jointIndex = rig.rightHand;
    pins[1].hintOffset = Math::Vector(0.0f, 0.0f, -0.1f);
    pins[2].jointIndex = rig.leftFoot;
    pins[2].hintOffset = Math::Vector(0.0f, 0.0f, 0.1f);
    pins[3].jointIndex = rig.rightFoot;
    pins[3].hintOffset = Math::Vector(0.0f, 0.0f, 0.1f);
    for (size_t i = 0; i < T; ++i) {
        pins[0].contactMask.push_back((1.0f - fullBody[i]) * lh[i]);
        pins[1].contactMask.push_back((1.0f - fullBody[i]) * rh[i]);
        pins[2].contactMask.push_back((1.0f - fullBody[i]) * lf[i]);
        pins[3].contactMask.push_back((1.0f - fullBody[i]) * rf[i]);
    }

    // Foot contacts: ankle (heel or toe in contact), then the toes as one-bone contacts.
    std::vector<Animation::ContactInfo> contactInfo(2);
    contactInfo[0].jointIndex = rig.rightFoot;
    contactInfo[0].hintOffset = Math::Vector(0.0f, 0.0f, 0.1f);
    contactInfo[0].minHeight = Animation::JointLocalToGlobal(parents, rig.rightFoot, defaultPose).GetTranslation().GetY();
    contactInfo[1].jointIndex = rig.leftFoot;
    contactInfo[1].hintOffset = Math::Vector(0.0f, 0.0f, 0.1f);
    contactInfo[1].minHeight = Animation::JointLocalToGlobal(parents, rig.leftFoot, defaultPose).GetTranslation().GetY();
    auto& rC = contactInfo[0].contactMask;
    auto& lC = contactInfo[1].contactMask;
    rC.resize(T);
    lC.resize(T);
    for (size_t i = 0; i < T; ++i) {
        rC[i] = rf[i] != 0.0f ? 0.0f : contacts[4 * i + 2];
        lC[i] = lf[i] != 0.0f ? 0.0f : contacts[4 * i + 0];
        rC[i] = std::min((rf[i] != 0.0f ? 0.0f : contacts[4 * i + 3]) + rC[i], 1.0f);
        lC[i] = std::min((lf[i] != 0.0f ? 0.0f : contacts[4 * i + 1]) + lC[i], 1.0f);
    }
    int leftToe = -1, rightToe = -1;
    for (size_t i = 0; i < J; ++i) {
        if (parents[i] == rig.leftFoot) leftToe = int(i);
        if (parents[i] == rig.rightFoot) rightToe = int(i);
    }
    if (leftToe != -1 && rightToe != -1) {
        const float toeY = Animation::JointLocalToGlobal(parents, rightToe, defaultPose).GetTranslation().GetY();
        contactInfo.resize(4);
        contactInfo[2].jointIndex = rightToe;
        contactInfo[2].contactType = Animation::kOneBone;
        contactInfo[2].minHeight = toeY;
        contactInfo[3].jointIndex = leftToe;
        contactInfo[3].contactType = Animation::kOneBone;
        contactInfo[3].minHeight = toeY;
        auto& rT = contactInfo[2].contactMask;
        auto& lT = contactInfo[3].contactMask;
        rT.resize(T);
        lT.resize(T);
        for (size_t i = 0; i < T; ++i) {
            rT[i] = rf[i] != 0.0f ? 0.0f : contacts[4 * i + 3];
            lT[i] = lf[i] != 0.0f ? 0.0f : contacts[4 * i + 1];
        }
    }

    std::vector<std::vector<Math::Transform>> poses(T, defaultPose), targets(T, defaultPose);
    setTransforms(poses, rootTranslations, jointRotationsWxyz, T, J);
    setTransforms(targets, rootTarget, rotationsTargetWxyz, T, J);

    Animation::CorrectMotion(poses, targets, fullBody, rootMask, contactInfo, pins, parents, defaultPose,
                             contactThreshold, rootMargin, rig.hasDoubleAnkleJoints);

    for (size_t f = 0; f < T; ++f) {
        const Math::Vector t = poses[f][0].GetTranslation();
        rootTranslations[3 * f] = t.GetX();
        rootTranslations[3 * f + 1] = t.GetY();
        rootTranslations[3 * f + 2] = t.GetZ();
        for (size_t j = 0; j < J; ++j) {
            const Math::Quaternion q = poses[f][j].GetRotation();
            const float* xyzw = reinterpret_cast<const float*>(&q);
            float* out = &jointRotationsWxyz[4 * (J * f + j)];
            out[0] = xyzw[3];
            out[1] = xyzw[0];
            out[2] = xyzw[1];
            out[3] = xyzw[2];
        }
    }
    return true;
}

} // namespace AIMotion::KimodoCorrection
