#include "QuadRetopoController.h"
#include "QuadRetopo.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <Ogre.h>
#include <OgreEntity.h>

QuadRetopoController* QuadRetopoController::m_pSingleton = nullptr;

QuadRetopoController* QuadRetopoController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new QuadRetopoController();
    return m_pSingleton;
}

QuadRetopoController* QuadRetopoController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void QuadRetopoController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

QuadRetopoController::QuadRetopoController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &QuadRetopoController::selectionChanged);
}

bool QuadRetopoController::hasSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    return !sel->getResolvedEntities().isEmpty();
}

QVariantMap QuadRetopoController::retopologizeSelected(int targetFaces,
                                                       double maxAngleDeg,
                                                       double shapeToleranceDeg,
                                                       double maxAspectRatio)
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

    QuadRetopoOptions opts;
    opts.targetFaces        = targetFaces;
    opts.maxAngleDeg        = maxAngleDeg;
    opts.shapeToleranceDeg  = shapeToleranceDeg;
    opts.maxAspectRatio     = maxAspectRatio;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.retopo"),
        QString("UI retopo entity=%1 target=%2 maxAngle=%3 shape=%4 aspect=%5")
            .arg(QString::fromStdString(entity->getName()))
            .arg(targetFaces)
            .arg(maxAngleDeg).arg(shapeToleranceDeg).arg(maxAspectRatio));

    m_busy = true;
    emit busyChanged();

    QuadRetopoReport report;
    try {
        report = QuadRetopo::retopologize(entity, opts);
    } catch (const Ogre::Exception& e) {
        m_busy = false;
        emit busyChanged();
        emit error(QString("Ogre error: %1").arg(QString::fromStdString(e.getFullDescription())));
        result["applied"] = false;
        result["error"]   = QString::fromStdString(e.getFullDescription());
        return result;
    }

    m_busy = false;
    emit busyChanged();

    result["applied"]              = report.applied;
    result["meshName"]             = report.meshName;
    result["totalTrianglesBefore"] = report.totalTrianglesBefore;
    result["totalFacesAfter"]      = report.totalFacesAfter;
    result["totalQuadsAfter"]      = report.totalQuadsAfter;
    result["totalTrianglesAfter"]  = report.totalTrianglesAfterRetopo;
    result["quadDominance"]        = report.quadDominance();
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit retopoApplied(result);
    else                emit error(report.error.isEmpty()
                                    ? QStringLiteral("Quad retopology failed")
                                    : report.error);

    return result;
}
