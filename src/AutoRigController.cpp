#include "AutoRigController.h"
#include "AutoRig.h"
#include "SkinWeights.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>

AutoRigController* AutoRigController::m_pSingleton = nullptr;

AutoRigController* AutoRigController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new AutoRigController();
    return m_pSingleton;
}

AutoRigController* AutoRigController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AutoRigController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AutoRigController::AutoRigController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &AutoRigController::selectionChanged);
}

bool AutoRigController::hasRiggableSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) return false;
    Ogre::Entity* first = entities.first();
    if (!first || !first->getMesh()) return false;
    // Riggable == static (no skeleton yet). An already-skinned mesh is
    // intentionally excluded (re-rigging would wipe its existing rig).
    return first->getMesh()->getSkeleton() == nullptr;
}

QVariantMap AutoRigController::autoRigSelected(const QString& templateName,
                                               const QString& upAxis,
                                               bool alsoSkin)
{
    QVariantMap result;

    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("Auto-rig requested (%1, up=%2%3)")
            .arg(templateName, upAxis,
                 alsoSkin ? QStringLiteral(", +skin") : QString()));

    auto* sel = SelectionSet::getSingleton();
    const auto entities = sel ? sel->getResolvedEntities() : QList<Ogre::Entity*>{};
    if (entities.isEmpty()) {
        const auto msg = QStringLiteral("No mesh selected.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }
    Ogre::Entity* entity = entities.first();
    if (!entity || !entity->getMesh()) {
        const auto msg = QStringLiteral("Selected entity is no longer valid.");
        emit error(msg);
        result["applied"] = false;
        result["error"]   = msg;
        return result;
    }

    AutoRig::Options opts;
    opts.tmpl = AutoRig::templateFromString(templateName);
    const QString ax = upAxis.trimmed().toLower();
    if (ax == QStringLiteral("x")) opts.upAxis = 0;
    else if (ax == QStringLiteral("z")) opts.upAxis = 2;
    else opts.upAxis = 1;   // y (default)

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.auto_rig"),
        QStringLiteral("UI auto-rig entity=%1 template=%2")
            .arg(QString::fromStdString(entity->getName()),
                 AutoRig::templateToString(opts.tmpl)));

    m_busy = true;
    emit busyChanged();

    AutoRig::Report report;
    bool skinned = false;
    try {
        report = AutoRig::rigEntity(entity, opts);
        if (report.applied && alsoSkin) {
            const auto sw = SkinWeights::computeAndApply(entity, {});
            skinned = sw.applied;
            if (!sw.applied)
                report.error = QStringLiteral("rigged, but skinning failed: %1")
                    .arg(sw.error);
        }
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
    emit selectionChanged();   // skeleton state changed → refresh button bindings

    result["applied"]          = report.applied;
    result["meshName"]         = report.meshName;
    result["skeletonName"]     = report.skeletonName;
    result["template"]         = report.templateName;
    result["boneCount"]        = report.boneCount;
    result["verticesSampled"]  = report.verticesSampled;
    result["jointsRecentered"] = report.jointsRecentered;
    result["skinned"]          = skinned;
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit rigged(result);
    else emit error(report.error.isEmpty()
                        ? QStringLiteral("Auto-rig failed") : report.error);

    return result;
}
