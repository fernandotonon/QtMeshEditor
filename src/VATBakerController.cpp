/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VATBakerController.h"

#include "SelectionSet.h"
#include "SentryReporter.h"
#include "VATBaker.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QWidget>

#include <OgreAnimationState.h>
#include <OgreEntity.h>

VATBakerController* VATBakerController::s_instance = nullptr;

VATBakerController* VATBakerController::instance()
{
    if (!s_instance)
        s_instance = new VATBakerController();
    return s_instance;
}

VATBakerController* VATBakerController::qmlInstance(QQmlEngine*, QJSEngine*)
{
    return instance();
}

void VATBakerController::kill()
{
    if (!s_instance) return;
    delete s_instance;
    s_instance = nullptr;
}

VATBakerController::VATBakerController(QObject* parent)
    : QObject(parent)
{
    // Sync the animation list with the current selection.
    if (auto* sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged,
                this, &VATBakerController::refreshAnimations);
    }
    refreshAnimations();
}

VATBakerController::~VATBakerController() = default;

void VATBakerController::refreshAnimations()
{
    QStringList fresh;
    auto* sel = SelectionSet::getSingleton();
    if (sel) {
        auto entities = sel->getResolvedEntities();
        if (!entities.isEmpty()) {
            if (auto* entity = entities.first()) {
                if (entity->hasSkeleton()) {
                    if (auto* states = entity->getAllAnimationStates()) {
                        auto it = states->getAnimationStateIterator();
                        while (it.hasMoreElements()) {
                            auto* state = it.getNext();
                            if (state) fresh << QString::fromStdString(state->getAnimationName());
                        }
                    }
                }
            }
        }
    }
    if (fresh != m_animations) {
        m_animations = std::move(fresh);
        emit availableAnimationsChanged();
    }
}

void VATBakerController::setIsBaking(bool b)
{
    if (m_isBaking == b) return;
    m_isBaking = b;
    emit isBakingChanged();
}

bool VATBakerController::bake(const QString& animationName,
                              double fps,
                              const QString& outputDir,
                              const QString& basename)
{
    if (m_isBaking) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: already baking");
        return false;
    }
    if (animationName.isEmpty()) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: animationName is required");
        emit bakeFinished(false, QString(), QStringLiteral("animationName is required"));
        return false;
    }
    if (outputDir.isEmpty()) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: outputDir is required");
        emit bakeFinished(false, QString(), QStringLiteral("outputDir is required"));
        return false;
    }

    auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: no SelectionSet");
        emit bakeFinished(false, QString(), QStringLiteral("no SelectionSet"));
        return false;
    }
    auto entities = sel->getResolvedEntities();
    if (entities.isEmpty() || !entities.first()) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: no entity selected");
        emit bakeFinished(false, QString(), QStringLiteral("no entity selected"));
        return false;
    }
    Ogre::Entity* entity = entities.first();
    if (!entity->hasSkeleton()) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: selected entity has no skeleton");
        emit bakeFinished(false, QString(), QStringLiteral("selected entity has no skeleton"));
        return false;
    }

    VATBaker::Options opts;
    opts.animationName = animationName;
    opts.fps           = fps;
    opts.outputDir     = outputDir;
    opts.basename      = basename.isEmpty() ? animationName : basename;

    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("OpenVAT bake start: anim=%1 fps=%2")
            .arg(animationName).arg(fps));

    // The Ogre animation state lives on the main thread; sampling
    // from a worker would race with Manager's per-frame updates.
    // VATBaker::bake is fast enough on a single skeleton that we
    // run it inline rather than building a true off-thread pipeline
    // — slice 4's progress signals still fire so QML can show the
    // "baking..." state while bake() runs. A future slice can split
    // the per-frame sampling out if needed.
    setIsBaking(true);
    // Reset progress before signalling so property-bound QML listeners
    // never read a stale value through the bound members.
    m_progressDone = 0;
    m_progressTotal = 0;
    emit bakeProgress(m_progressDone, m_progressTotal);

    VATBaker::BakeResult result = VATBaker::bake(entity, opts);

    m_progressDone = result.frameCount;
    m_progressTotal = result.frameCount;
    emit bakeProgress(m_progressDone, m_progressTotal);
    setIsBaking(false);

    SentryReporter::addBreadcrumb("ui.action",
        result.ok
            ? QStringLiteral("VAT bake ok: %1 frames × %2 vertices → %3")
                  .arg(result.frameCount).arg(result.vertexCount).arg(result.posTexPath)
            : QStringLiteral("VAT bake failed: %1").arg(result.error));

    emit bakeFinished(result.ok, result.posTexPath, result.error);
    return true;
}

QString VATBakerController::chooseOutputDir(const QString& startDir)
{
    QString seed = startDir;
    if (seed.isEmpty() || !QFileInfo(seed).isDir())
        seed = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (seed.isEmpty())
        seed = QDir::homePath();

    // Mirror the MaterialEditorQML::openFileDialog dance: process pending
    // events + raise the active window before opening the dialog, and
    // force the Qt-rendered dialog rather than the native one. Native
    // file dialogs hosted from inside a QQuickWidget have been observed
    // to silently no-op on macOS — DontUseNativeDialog reliably opens.
    QApplication::processEvents();
    QWidget* parent = QApplication::activeWindow();
    if (parent) {
        parent->raise();
        parent->activateWindow();
    }
    QApplication::processEvents();

    const QString chosen = QFileDialog::getExistingDirectory(
        parent,
        QStringLiteral("Choose OpenVAT output folder"),
        seed,
        QFileDialog::ShowDirsOnly
            | QFileDialog::DontUseNativeDialog
            | QFileDialog::DontUseCustomDirectoryIcons);
    return chosen;
}
