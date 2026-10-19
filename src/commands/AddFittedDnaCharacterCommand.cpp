#include "AddFittedDnaCharacterCommand.h"

#include "DNA/DnaReader.h"
#include "DNA/DnaRigLogic.h"
#include "DNA/DnaScene.h"
#include "Manager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <OgreSceneNode.h>

AddFittedDnaCharacterCommand::AddFittedDnaCharacterCommand(std::shared_ptr<Dna::Rig> rig, QString sourcePath, QString baseName)
    : m_rig(std::move(rig)), m_sourcePath(std::move(sourcePath)), m_baseName(std::move(baseName))
{
    setText(QObject::tr("Fit MetaHuman body"));
}

void AddFittedDnaCharacterCommand::redo()
{
    if (!m_rig || m_created) return;
    // The body correctives are driven by the SOURCE file's rig logic: same joints, same driver
    // semantics, only the proportions differ, so the evaluator is created from it again.
    std::shared_ptr<Dna::RigEvaluator> ev;
    if (!m_sourcePath.isEmpty()) {
        std::unique_ptr<Dna::RigEvaluator> made = Dna::RigEvaluator::create(m_sourcePath);
        if (made && !made->driverJoints().isEmpty() && !made->drivenJoints().empty()) {
            ev = std::shared_ptr<Dna::RigEvaluator>(made.release());
            m_rig->protectedJoint.assign(m_rig->joints.size(), 0);
            for (const QString& d : ev->driverJoints()) {
                const int j = ev->jointIndex(d);
                if (j >= 0 && size_t(j) < m_rig->protectedJoint.size()) m_rig->protectedJoint[size_t(j)] = 1;
            }
            for (const int j : ev->drivenJoints())
                if (j >= 0 && size_t(j) < m_rig->protectedJoint.size()) m_rig->protectedJoint[size_t(j)] = 1;
        }
    }
    Dna::ImportReport report;
    QString err;
    const QString name = QStringLiteral("%1_%2").arg(m_baseName).arg(++m_revision);
    Ogre::SceneNode* node = Dna::importRigToScene(*m_rig, name, &report, &err, ev);
    if (!node) {
        m_error = err;
        m_outcome->error = err;
        setObsolete(true);
        SentryReporter::addBreadcrumb("ai.assist.dna_fit", QStringLiteral("commit failed: %1").arg(err));
        return;
    }
    m_nodeName = QString::fromStdString(node->getName());
    m_created = true;
    m_outcome->created = true;
    m_outcome->nodeName = m_nodeName;
    SelectionSet::getSingleton()->selectOne(node);
    SentryReporter::addBreadcrumb("ai.assist.dna_fit", QStringLiteral("fitted character added: %1").arg(m_nodeName));
}

void AddFittedDnaCharacterCommand::undo()
{
    if (!m_created) return;
    for (auto* n : Manager::getSingleton()->getSceneNodes())
        if (QString::fromStdString(n->getName()) == m_nodeName) {
            SelectionSet::getSingleton()->clearList();   // no dangling selection of the node about to go
            Manager::getSingleton()->destroySceneNode(n);
            break;
        }
    m_created = false;
    m_outcome->created = false;
}
