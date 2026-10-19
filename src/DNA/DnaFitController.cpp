#include "DnaFitController.h"

#include "DnaCLI.h"
#include "DnaScene.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "UndoManager.h"
#include "commands/AddFittedDnaCharacterCommand.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QQmlEngine>

#include <OgreAny.h>
#include <OgreEntity.h>
#include <OgreSceneNode.h>

DnaFitController* DnaFitController::m_pSingleton = nullptr;

namespace {
// The DNA entity among the selection (it carries the importer's source-path binding), and the other one.
struct Pair {
    Ogre::Entity* dna = nullptr;
    Ogre::Entity* target = nullptr;
    int entities = 0;
    bool dnaInScene = false;
};

QString sourcePathOf(Ogre::Entity* e)
{
    if (!e) return QString();
    const Ogre::Any a = e->getUserObjectBindings().getUserAny(Dna::kSourcePathKey);
    if (a.isEmpty()) return QString();
    try { return QString::fromStdString(Ogre::any_cast<std::string>(a)); } catch (...) { return QString(); }
}

Pair selectedPair()
{
    Pair p;
    for (auto* o : Manager::getSingleton()->getEntities())
        if (o && o->getMovableType() == "Entity" && !sourcePathOf(static_cast<Ogre::Entity*>(o)).isEmpty()) p.dnaInScene = true;
    // The editor keeps selected NODES and selected ENTITIES in separate lists, and clicking in the
    // viewport / scene tree selects nodes: gather the entities from both (deduplicated, in order).
    QList<Ogre::Entity*> sel = SelectionSet::getSingleton()->getEntitiesSelectionList();
    for (Ogre::SceneNode* n : SelectionSet::getSingleton()->getNodesSelectionList()) {
        if (!n) continue;
        for (auto it = n->getAttachedObjectIterator(); it.hasMoreElements();) {
            Ogre::MovableObject* mo = it.getNext();
            if (mo && mo->getMovableType() == "Entity") {
                Ogre::Entity* e = static_cast<Ogre::Entity*>(mo);
                if (!sel.contains(e)) sel.append(e);
            }
        }
    }
    for (Ogre::Entity* e : sel) {
        ++p.entities;
        if (!sourcePathOf(e).isEmpty() && !p.dna) p.dna = e;
        else if (!p.target) p.target = e;
    }
    return p;
}
}  // namespace

DnaFitController::DnaFitController(QObject* parent) : QObject(parent)
{
    if (auto* sel = SelectionSet::getSingleton())
        connect(sel, &SelectionSet::selectionChanged, this, &DnaFitController::refresh);
    if (auto* m = Manager::getSingleton()) {
        connect(m, &Manager::entityCreated, this, &DnaFitController::refresh);
        connect(m, &Manager::sceneNodeDestroyed, this, &DnaFitController::refresh);
    }
    refresh();
}

DnaFitController* DnaFitController::instance()
{
    if (!m_pSingleton) m_pSingleton = new DnaFitController();
    return m_pSingleton;
}

DnaFitController* DnaFitController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    DnaFitController* c = instance();
    QQmlEngine::setObjectOwnership(c, QQmlEngine::CppOwnership);
    return c;
}

void DnaFitController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

void DnaFitController::refresh()
{
    const Pair p = selectedPair();
    const bool wasCan = m_canFit, wasHas = m_hasDna;
    const QString wasHint = m_hint;
    m_hasDna = p.dnaInScene;
    m_canFit = p.dna && p.target && p.entities == 2;
    if (!p.dnaInScene) m_hint = QStringLiteral("Import a MetaHuman body .dna to fit it to another character.");
    else if (!p.dna) m_hint = QStringLiteral("Select the MetaHuman body (the imported .dna) and the character to fit it to.");
    else if (!p.target || p.entities != 2) m_hint = QStringLiteral("Also select the character to fit it to (Ctrl+click) — exactly two entities.");
    else m_hint = p.target->hasSkeleton()
        ? QStringLiteral("Ready: the body takes the target's pose and proportions from its skeleton, then its shape from its surface (skeleton + surface).")
        : QStringLiteral("Ready: the target has no skeleton. Same topology/UVs as the body (an edit of it) fits exactly; any other mesh is wrapped onto the body (surface).");
    if (wasCan != m_canFit || wasHas != m_hasDna || wasHint != m_hint) emit changed();
}

bool DnaFitController::fitSelected(const QString& modeIn, bool alignLimbs)
{
    const Pair p = selectedPair();
    if (!p.dna || !p.target || p.entities != 2) {
        m_status = QStringLiteral("Select the MetaHuman body and the character to fit it to.");
        emit changed();
        return false;
    }
    QString mode = modeIn;
    if (mode.isEmpty() || mode == QLatin1String("auto")) mode = p.target->hasSkeleton() ? QStringLiteral("joints+surface") : QStringLiteral("topology");
    const QString src = sourcePathOf(p.dna);
    SentryReporter::addBreadcrumb("ui.action", QStringLiteral("dna fit mode=%1").arg(mode));

    m_status = QStringLiteral("Fitting (%1)… the editor is busy for a few seconds.").arg(mode);
    emit changed();
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);   // let the status paint before the solver blocks

    Dna::DnaCLI::FitOptions o;
    o.mode = mode;
    o.alignLimbs = alignLimbs;
    o.poses = true;
    auto rig = std::make_shared<Dna::Rig>();
    QVariantMap report;
    QString err;
    bool ok = false;
    try {
        ok = Dna::DnaCLI::fitEntityReport(src, p.target, o, rig.get(), &report, &err);
    } catch (const std::exception& e) {
        err = QString::fromUtf8(e.what());
    }
    if (!ok) {
        m_status = QStringLiteral("Fit failed: %1").arg(err);
        emit changed();
        return false;
    }
    auto* cmd = new AddFittedDnaCharacterCommand(rig, src, QFileInfo(src).completeBaseName() + QStringLiteral("_fitted"));
    // push() runs redo() and deletes a command that marked itself obsolete (a failed build): read the shared
    // outcome, never the command, afterwards.
    const auto outcome = cmd->outcome();
    UndoManager::getSingleton()->push(cmd);
    cmd = nullptr;
    if (!outcome->created) {
        m_status = QStringLiteral("The fit worked but the fitted character could not be built: %1").arg(outcome->error);
        emit changed();
        return false;
    }
    double worstStretchP99 = 0;
    for (const QVariant& v : report.value("poses").toList()) worstStretchP99 = std::max(worstStretchP99, v.toMap().value("stretchDeltaP99").toDouble());
    m_status = QStringLiteral("Fitted (%1): body moved by mean %2 mm, joints by mean %3 mm; reference-pose stretch p99 <= %4. "
                              "Added as '%5' — Ctrl+Z removes it. Corrective data is not recalibrated.")
                   .arg(mode).arg(report.value("meanVertexShiftMm").toDouble(), 0, 'f', 0).arg(report.value("meanJointShiftMm").toDouble(), 0, 'f', 0)
                   .arg(worstStretchP99, 0, 'f', 2).arg(outcome->nodeName);
    {
        const QVariantMap reg = report.value("registration").toMap();
        if (reg.contains("shouldersYawDeg"))
            m_status += QStringLiteral("\nAlignment vs the target's skeleton (degrees): shoulders yaw %1 / roll %2, hips yaw %3 / roll %4, spine tilt %5; "
                                       "joints %6 mm from its bones on average.")
                            .arg(reg.value("shouldersYawDeg").toDouble(), 0, 'f', 1).arg(reg.value("shouldersRollDeg").toDouble(), 0, 'f', 1)
                            .arg(reg.value("hipsYawDeg").toDouble(), 0, 'f', 1).arg(reg.value("hipsRollDeg").toDouble(), 0, 'f', 1)
                            .arg(reg.value("spineTiltDeg").toDouble(), 0, 'f', 1).arg(reg.value("finalJointVsTargetBonesMeanMm").toDouble(), 0, 'f', 0);
    }
    for (const QString& w : report.value("warnings").toStringList()) m_status += QStringLiteral("\n• ") + w;
    emit changed();
    return true;
}
