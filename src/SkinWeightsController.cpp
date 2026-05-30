#include "SkinWeightsController.h"
#include "SkinWeights.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

SkinWeightsController* SkinWeightsController::m_pSingleton = nullptr;

SkinWeightsController* SkinWeightsController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new SkinWeightsController();
    return m_pSingleton;
}

SkinWeightsController* SkinWeightsController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void SkinWeightsController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

SkinWeightsController::SkinWeightsController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &SkinWeightsController::selectionChanged);
}

bool SkinWeightsController::hasSkinnedSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) return false;
    Ogre::Entity* first = entities.first();
    if (!first || !first->getMesh()) return false;
    return first->getMesh()->getSkeleton() != nullptr;
}

QVariantMap SkinWeightsController::computeWeightsForSelected(int maxInfluencesPerVertex,
                                                              double falloff,
                                                              double maxInfluenceDistance,
                                                              bool skipUnweightedBones,
                                                              bool replaceExisting)
{
    QVariantMap result;

    auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        const auto msg = QStringLiteral("No selection set.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) {
        const auto msg = QStringLiteral("No mesh selected.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    Ogre::Entity* entity = entities.first();

    SkinWeightsOptions opts;
    opts.maxInfluencesPerVertex = maxInfluencesPerVertex;
    opts.falloff                = falloff;
    opts.maxInfluenceDistance   = maxInfluenceDistance;
    opts.skipUnweightedBones    = skipUnweightedBones;
    opts.replaceExisting        = replaceExisting;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.skin_weights"),
        QString("UI skin entity=%1 maxInf=%2 falloff=%3 maxDist=%4")
            .arg(QString::fromStdString(entity->getName()))
            .arg(maxInfluencesPerVertex)
            .arg(falloff).arg(maxInfluenceDistance));

    m_busy = true;
    emit busyChanged();

    SkinWeightsReport report;
    try {
        report = SkinWeights::computeAndApply(entity, opts);
    } catch (const Ogre::Exception& e) {
        m_busy = false;
        emit busyChanged();
        const auto msg = QString::fromStdString(e.getFullDescription());
        emit error(QStringLiteral("Ogre error: %1").arg(msg));
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }

    m_busy = false;
    emit busyChanged();

    result["applied"]               = report.applied;
    result["meshName"]              = report.meshName;
    result["skeletonName"]          = report.skeletonName;
    result["totalBones"]            = report.totalBones;
    result["totalVerticesProcessed"] = report.totalVerticesProcessed;
    result["totalAssignmentsBefore"] = report.totalAssignmentsBefore;
    result["totalAssignmentsAfter"]  = report.totalAssignmentsAfter;
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit weightsApplied(result);
    else                emit error(report.error.isEmpty()
                                    ? QStringLiteral("Skin weights failed")
                                    : report.error);

    return result;
}
