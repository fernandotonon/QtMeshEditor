#include "LipsyncApply.h"

#include "A2FPredictor.h"

#include "../MorphAnimationManager.h"

#include <OgreEntity.h>
#include <OgreMesh.h>

#include <cmath>

namespace AudioToFace {

namespace {

/// Mesh targets and solver poses are compared with a leading underscore
/// stripped and case folded, so `JawOpen`, `jawOpen` and `_jawOpen` agree.
QString canonicalName(QString n)
{
    while (n.startsWith(QLatin1Char('_'))) n.remove(0, 1);
    return n.toLower();
}

}  // namespace

NameBinding bindPoseNames(const QStringList& poseNames,
                          const QStringList& meshTargets,
                          const QHash<QString, QString>& overrides,
                          const QSet<QString>& ignore)
{
    NameBinding b;
    b.poseTarget.assign(size_t(poseNames.size()), QString());

    QHash<QString, QString> byNormalised;   // normalised -> the mesh's spelling
    for (const QString& t : meshTargets) byNormalised.insert(canonicalName(t), t);

    for (int i = 0; i < poseNames.size(); ++i) {
        const QString& pose = poseNames[i];
        if (ignore.contains(pose)) { b.ignored << pose; continue; }

        QString hit;
        const auto ov = overrides.constFind(pose);
        if (ov != overrides.constEnd()) {
            hit = ov.value();
            if (!meshTargets.contains(hit)) {
                // Refuse rather than fall back: the caller asked for a
                // specific binding, and quietly name-matching instead would
                // look like the mapping was honoured.
                b.error = QStringLiteral(
                    "mapping binds %1 to '%2', which is not a morph target on "
                    "this mesh").arg(pose, hit);
                return b;
            }
        } else {
            hit = byNormalised.value(canonicalName(pose));
        }

        b.poseTarget[size_t(i)] = hit;
        if (hit.isEmpty()) b.unmatched << pose; else b.matched << pose;
    }

    if (b.matched.isEmpty()) {
        b.error = QStringLiteral(
            "none of the mesh's %1 morph targets match an ARKit pose name "
            "(expected jawOpen, mouthPucker, mouthSmileLeft, …); "
            "`qtmesh facerig` produces targets with the right names")
            .arg(meshTargets.size());
    }
    return b;
}

std::vector<size_t> selectKeyFrames(const PredictResult& pred,
                                    const NameBinding& binding,
                                    float epsilon)
{
    const size_t n = binding.poseTarget.size();
    std::vector<float> lastWritten(n, -1.0f);
    std::vector<size_t> keys;

    for (size_t fi = 0; fi < pred.frames.size(); ++fi) {
        const auto& f = pred.frames[fi];
        // First and last are always keyed, so the clip starts and ends at a
        // defined pose instead of holding whatever preceded it.
        bool key = (fi == 0) || (fi + 1 == pred.frames.size());
        for (size_t i = 0; !key && i < n && i < f.weights.size(); ++i) {
            if (binding.poseTarget[i].isEmpty()) continue;
            if (std::abs(f.weights[i] - lastWritten[i]) >= epsilon) key = true;
        }
        if (!key) continue;

        keys.push_back(fi);
        for (size_t i = 0; i < n && i < f.weights.size(); ++i)
            if (!binding.poseTarget[i].isEmpty())
                lastWritten[i] = f.weights[i];
    }
    return keys;
}

ApplyResult applyToEntity(Ogre::Entity* entity,
                          const QString& clipName,
                          const PredictResult& pred,
                          const NameBinding& binding,
                          float epsilon)
{
    ApplyResult r;
    if (!entity) { r.error = QStringLiteral("no entity"); return r; }
    if (!binding.ok()) {
        r.error = binding.error.isEmpty() ? QStringLiteral("no channels matched")
                                          : binding.error;
        return r;
    }

    const std::string clip = clipName.toStdString();

    // Replace, don't merge — see the header note.
    if (Ogre::MeshPtr mesh = entity->getMesh(); mesh && mesh->hasAnimation(clip))
        mesh->removeAnimation(clip);

    const std::vector<size_t> keys = selectKeyFrames(pred, binding, epsilon);
    const size_t n = binding.poseTarget.size();
    for (size_t fi : keys) {
        const auto& f = pred.frames[fi];
        for (size_t i = 0; i < n && i < f.weights.size(); ++i) {
            if (binding.poseTarget[i].isEmpty()) continue;
            if (MorphAnimationManager::writeWeightKeyOn(
                    entity, clip, binding.poseTarget[i].toStdString(),
                    float(f.timeSec), f.weights[i]))
                ++r.keyframesWritten;
        }
    }
    entity->refreshAvailableAnimationState();

    if (r.keyframesWritten == 0)
        r.error = QStringLiteral(
            "the solve produced no motion above the %1 threshold — the audio "
            "may be silent").arg(double(epsilon));
    return r;
}

}  // namespace AudioToFace
