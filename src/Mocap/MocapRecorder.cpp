#ifdef ENABLE_MOCAP

#include "MocapRecorder.h"

#include "FaceCapCanonicalData.h"
#include "../Manager.h"
#include "../MorphAnimationManager.h"
#include "../MotionInbetween.h"
#include "../NodeAnimationManager.h"
#include "../SentryReporter.h"

#include <OgreAnimation.h>
#include <OgreBone.h>
#include <OgreEntity.h>
#include <OgreKeyFrame.h>
#include <OgreMesh.h>
#include <OgreSceneNode.h>
#include <OgreSkeletonInstance.h>

#include <algorithm>
#include <cmath>

namespace MocapRecorder {

namespace {

constexpr int kHeadCanonicalRole = 5;  // MotionInbetween's canonical Head

Ogre::Quaternion toOgre(const std::array<float, 4>& q)  // (x,y,z,w)
{
    return Ogre::Quaternion(q[3], q[0], q[1], q[2]);
}

double quatAngle(const Ogre::Quaternion& a, const Ogre::Quaternion& b)
{
    const double dot = std::abs(a.Dot(b));
    return 2.0 * std::acos(std::min(1.0, dot));
}

// Key-time selection with epsilon run-length suppression + gap holds, shared
// by the weight channels and the head track. `values` is any channel the
// caller can measure a distance on (via `distance(i, j)` between samples).
template <typename DistanceFn>
std::vector<int> selectKeyIndices(const std::vector<int>& confident,
                                  const std::vector<double>& times,
                                  double gapHoldSeconds, double epsilon,
                                  DistanceFn distance)
{
    std::vector<int> keys;
    if (confident.empty())
        return keys;
    int lastWritten = confident.front();
    keys.push_back(lastWritten);
    for (size_t k = 1; k < confident.size(); ++k) {
        const int i = confident[k];
        const int prev = confident[k - 1];
        const bool isLast = (k == confident.size() - 1);
        // gap edges: hold the old value up to the gap, land the new value at
        // its end, regardless of epsilon
        const bool gapEdge =
            (times[i] - times[prev]) > gapHoldSeconds;
        if (gapEdge && keys.back() != prev)
            keys.push_back(prev);
        const bool moved = distance(i, lastWritten) >= epsilon;
        // anchor the frame BEFORE a jump so interpolation doesn't start early
        const bool nextJumps =
            !isLast && distance(confident[k + 1], i) >= epsilon;
        if (moved || nextJumps || isLast || gapEdge) {
            keys.push_back(i);
            lastWritten = i;
        }
    }
    return keys;
}

}  // namespace

QString resolveHeadBone(Ogre::Entity* entity)
{
    if (!entity || !entity->hasSkeleton())
        return {};
    Ogre::SkeletonInstance* skel = entity->getSkeleton();
    for (unsigned short i = 0; i < skel->getNumBones(); ++i) {
        Ogre::Bone* bone = skel->getBone(i);
        if (MotionInbetween::canonicalIndexForBone(
                QString::fromStdString(bone->getName())) == kHeadCanonicalRole)
            return QString::fromStdString(bone->getName());
    }
    return {};
}

FaceRecordReport recordFace(Ogre::Entity* entity,
                            const std::vector<FaceSample>& samples,
                            const FaceCapMapper::Mapping& mapping,
                            const FaceRecordOptions& options)
{
    FaceRecordReport report;
    report.clipName = options.clipName;
    report.unmatchedCanonical = mapping.unmatchedCanonical;
    report.unmatchedMesh = mapping.unmatchedMesh;
    for (const auto& ch : mapping.channels)
        report.matchedChannels
            << QString::fromLatin1(FaceCap::kBlendshapeNames[ch.canonicalIndex]);

    if (!entity) {
        report.error = QStringLiteral("no entity");
        return report;
    }
    if (options.clipName.isEmpty()) {
        report.error = QStringLiteral("empty clip name");
        return report;
    }
    if (mapping.channels.isEmpty() && !options.head) {
        report.error = QStringLiteral("nothing to record: no mapped morph "
                                      "channels and head recording disabled");
        return report;
    }

    report.framesProcessed = static_cast<int>(samples.size());
    std::vector<int> confident;
    confident.reserve(samples.size());
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].confidence > 0.f)
            confident.push_back(static_cast<int>(i));
    }
    report.framesNoFace = report.framesProcessed
                          - static_cast<int>(confident.size());
    if (confident.empty()) {
        report.error = QStringLiteral("no confident face frame in the take");
        return report;
    }

    const double t0 = samples[confident.front()].timeSec;
    std::vector<double> times(samples.size(), 0.0);
    for (size_t i = 0; i < samples.size(); ++i)
        times[i] = samples[i].timeSec - t0;

    Ogre::MeshPtr mesh = entity->getMesh();

    // --- morph weight channels ------------------------------------------------
    if (!mapping.channels.isEmpty() && mesh) {
        if (options.replaceExisting
            && mesh->hasAnimation(options.clipName.toStdString()))
            mesh->removeAnimation(options.clipName.toStdString());

        const std::string clip = options.clipName.toStdString();
        for (const auto& ch : mapping.channels) {
            const int ci = ch.canonicalIndex;
            auto weightAt = [&](int i) {
                return static_cast<double>(samples[i].weights[ci]);
            };
            const std::vector<int> keys = selectKeyIndices(
                confident, times, options.gapHoldSeconds, options.weightEpsilon,
                [&](int i, int j) { return std::abs(weightAt(i) - weightAt(j)); });
            const std::string target = ch.meshTargetName.toStdString();
            for (int i : keys) {
                if (MorphAnimationManager::writeWeightKeyOn(
                        entity, clip, target, static_cast<float>(times[i]),
                        static_cast<float>(weightAt(i))))
                    ++report.keyframesWritten;
            }
        }
        if (mesh->hasAnimation(clip))
            report.clipLength = mesh->getAnimation(clip)->getLength();
        entity->refreshAvailableAnimationState();
    }

    // --- head pose --------------------------------------------------------------
    report.headTarget = QStringLiteral("none");
    if (options.head) {
        // calibration: the first confident frame is neutral
        const Ogre::Quaternion neutral =
            toOgre(samples[confident.front()].headRotation);
        auto deltaAt = [&](int i) {
            // rotation that takes the neutral pose to this frame's pose
            return toOgre(samples[i].headRotation) * neutral.Inverse();
        };
        const std::vector<int> keys = selectKeyIndices(
            confident, times, options.gapHoldSeconds, options.headEpsilonRad,
            [&](int i, int j) { return quatAngle(deltaAt(i), deltaAt(j)); });
        const double length = times[confident.back()];

        const QString headBone = resolveHeadBone(entity);
        if (!headBone.isEmpty()) {
            Ogre::SkeletonInstance* skel = entity->getSkeleton();
            Ogre::Bone* bone = skel->getBone(headBone.toStdString());
            const std::string clip = (options.clipName
                                      + QStringLiteral("_Head")).toStdString();
            const bool exists = skel->hasAnimation(clip);
            if (exists && options.replaceExisting)
                skel->removeAnimation(clip);
            if (!exists || options.replaceExisting) {
                // Express the camera-frame delta on the bone's local axes:
                // rel = boneWorldBind^-1 . delta . boneWorldBind, keyed as the
                // offset Ogre applies onto the binding orientation.
                const Ogre::Quaternion boneWorld = bone->_getDerivedOrientation();
                Ogre::Animation* anim = skel->createAnimation(
                    clip, static_cast<Ogre::Real>(length));
                Ogre::NodeAnimationTrack* track =
                    anim->createNodeTrack(bone->getHandle(), bone);
                for (int i : keys) {
                    auto* kf = track->createNodeKeyFrame(
                        static_cast<Ogre::Real>(times[i]));
                    kf->setRotation(boneWorld.Inverse() * deltaAt(i) * boneWorld);
                }
                entity->refreshAvailableAnimationState();
                report.headKeyframesWritten = static_cast<int>(keys.size());
                report.headTarget = QStringLiteral("bone:") + headBone;
            }
        } else if (entity->getParentSceneNode()) {
            Ogre::SceneNode* node = entity->getParentSceneNode();
            auto* nam = NodeAnimationManager::instance();
            const QString clip = options.clipName + QStringLiteral("_Head");
            if (nam) {
                if (options.replaceExisting)
                    nam->deleteClip(clip);  // no-op when absent
                if (nam->createClip(clip, std::max(length, 0.001))) {
                    const QString nodeName = QString::fromStdString(node->getName());
                    int written = 0;
                    for (int i : keys) {
                        if (nam->addKeyframe(clip, nodeName, times[i],
                                             Ogre::Vector3::ZERO, deltaAt(i),
                                             Ogre::Vector3::UNIT_SCALE))
                            ++written;
                    }
                    report.headKeyframesWritten = written;
                    report.headTarget = QStringLiteral("node");
                }
            }
        }
    }
    if (report.clipLength <= 0.0)
        report.clipLength = times[confident.back()];

    SentryReporter::addBreadcrumb(
        "ai.assist.mocap_face",
        QStringLiteral("recorded '%1': %2 frames (%3 no-face), %4 weight keys, "
                       "%5 head keys (%6)")
            .arg(options.clipName)
            .arg(report.framesProcessed)
            .arg(report.framesNoFace)
            .arg(report.keyframesWritten)
            .arg(report.headKeyframesWritten)
            .arg(report.headTarget));
    return report;
}

}  // namespace MocapRecorder

#endif  // ENABLE_MOCAP
