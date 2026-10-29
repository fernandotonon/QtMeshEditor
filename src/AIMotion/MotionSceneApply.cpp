#include "MotionSceneApply.h"

#include "../AnimationMerger.h"
#include "../AnimationRetargeter.h"
#include "../SentryReporter.h"
#include "../UndoManager.h"
#include "../commands/RetargetAnimationCommand.h"

#include <QJsonArray>

#include <OgreAnimation.h>
#include <OgreEntity.h>
#include <OgreSkeletonInstance.h>

namespace AIMotion {

QString variantClipName(const QString& base, int index)
{
    return index == 0 ? base : QStringLiteral("%1_v%2").arg(base).arg(index + 1);
}

QJsonObject outcomeJson(const ApplyOutcome& out)
{
    return QJsonObject{
        {QStringLiteral("name"), QString::fromStdString(out.clip)},
        {QStringLiteral("frames"), out.frames},
        {QStringLiteral("length"), out.length},
        {QStringLiteral("mappedBones"), out.mappedBones},
        {QStringLiteral("fingerBones"), out.fingerBones},
        {QStringLiteral("unmappedTarget"), QJsonArray::fromStringList(out.unmappedTarget)},
        {QStringLiteral("unmappedSource"), QJsonArray::fromStringList(out.unmappedSource)},
        {QStringLiteral("excludedSource"), QJsonArray::fromStringList(out.excludedSource)},
        {QStringLiteral("inferredFingers"), QJsonArray::fromStringList(out.inferredFingers)},
        {QStringLiteral("heightScale"), out.report.heightScale},
        {QStringLiteral("rootBone"), QString::fromStdString(out.report.rootTarget)},
        {QStringLiteral("mapVersion"), out.mapVersion},
    };
}

bool loadMotionFiles(const QStringList& files, std::vector<Motion>* out, QString* error)
{
    out->clear();
    for (const QString& f : files) {
        Motion m;
        QString e;
        if (!loadInterchange(f, &m, &e)) {
            if (error) *error = QStringLiteral("%1: %2").arg(f, e);
            return false;
        }
        out->push_back(std::move(m));
    }
    return true;
}

SceneApplyResult applyMotionsToEntity(Ogre::Entity* entity, const std::vector<Motion>& motions,
                                      const SceneApplyOptions& options)
{
    SceneApplyResult r;
    if (!entity || !entity->hasSkeleton()) { r.error = QStringLiteral("no rigged entity: Kimodo motion needs a skeleton"); return r; }
    if (motions.empty()) { r.error = QStringLiteral("no motion to apply"); return r; }
    Ogre::Skeleton* skel = entity->getSkeleton();
    const QString base = options.baseName.trimmed().isEmpty() ? QStringLiteral("generated_kimodo") : options.baseName.trimmed();

    // Retarget every variation first; a failure removes what this call wrote so nothing half-applied is left behind.
    for (size_t v = 0; v < motions.size(); ++v) {
        const std::string clip = Retarget::uniqueAnimationName(skel, variantClipName(base, int(v)).toStdString());
        ApplyOptions ao;
        ao.rootMotion = options.rootMotion;
        ApplyOutcome out = applyMotionToSkeleton(skel, motions[v], clip, ao);
        if (!out.ok) {
            for (const QString& c : r.clips) skel->removeAnimation(c.toStdString());
            r.clips.clear();
            r.outcomes.clear();
            r.error = motions.size() > 1 ? QStringLiteral("variation %1: %2").arg(v + 1).arg(out.error) : out.error;
            return r;
        }
        if (options.footPin) AnimationMerger::pinFeet(skel, clip);
        r.clips << QString::fromStdString(clip);
        r.outcomes.push_back(std::move(out));
    }
    entity->refreshAvailableAnimationState();

    if (options.undoable)
        if (auto* um = UndoManager::getSingleton()) {
            um->stack()->beginMacro(QStringLiteral("Kimodo motion (%1 clip%2)").arg(r.clips.size()).arg(r.clips.size() == 1 ? "" : "s"));
            for (const QString& c : r.clips) um->push(new RetargetAnimationCommand(entity->getName(), c.toStdString(), true));
            um->stack()->endMacro();
            r.undoIndexAfter = um->stack()->index();
        }
    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.kimodo"),
                                  QStringLiteral("applied %1 clip(s) to %2").arg(r.clips.size()).arg(QString::fromStdString(entity->getName())));
    r.ok = true;
    return r;
}

void rollbackAppliedMotions(Ogre::Entity* entity, const SceneApplyResult& applied)
{
    if (!entity || !entity->hasSkeleton() || applied.clips.isEmpty()) return;
    auto* um = UndoManager::getSingleton();
    if (um && applied.undoIndexAfter >= 0 && um->stack()->index() == applied.undoIndexAfter && um->stack()->canUndo()) {
        um->stack()->undo();   // the batch is still the newest step: undoing it removes every clip
        return;
    }
    Ogre::Skeleton* skel = entity->getSkeleton();
    Ogre::AnimationStateSet* states = entity->getAllAnimationStates();
    for (const QString& c : applied.clips) {
        const std::string name = c.toStdString();
        // refreshAvailableAnimationState only ADDS states: drop the clip's own, as RetargetAnimationCommand::undo does.
        if (states && states->hasAnimationState(name)) states->removeAnimationState(name);
        if (skel->hasAnimation(name)) skel->removeAnimation(name);
    }
}

} // namespace AIMotion
