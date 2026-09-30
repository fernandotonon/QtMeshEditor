/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VATBakerController.h"

#include "GamificationManager.h"
#include "SelectionSet.h"
#include "SentryReporter.h"
#include "VATBaker.h"
#include "VATShaderEmitter.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QWidget>

#include <OgreAnimation.h>
#include <OgreAnimationState.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSkeletonInstance.h>
#include <OgreSubMesh.h>

namespace {

Ogre::Entity* firstSelectedEntity()
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return nullptr;
    auto entities = sel->getResolvedEntities();
    return entities.isEmpty() ? nullptr : entities.first();
}

// Skeletal clip names on `entity` (entity animation states whose name
// is a skeleton animation).
QStringList skeletalClips(Ogre::Entity* entity)
{
    QStringList out;
    if (!entity || !entity->hasSkeleton()) return out;
    auto* skel = entity->getSkeleton();
    if (auto* states = entity->getAllAnimationStates()) {
        auto it = states->getAnimationStateIterator();
        while (it.hasMoreElements()) {
            auto* state = it.getNext();
            if (state && skel->hasAnimation(state->getAnimationName()))
                out << QString::fromStdString(state->getAnimationName());
        }
    }
    return out;
}

// Mesh animations carrying vertex tracks (vertex-anim streams AND
// morph weight clips — both are VAT_POSE on the mesh).
QStringList vertexClips(Ogre::Entity* entity)
{
    QStringList out;
    if (!entity || !entity->getMesh()) return out;
    Ogre::MeshPtr mesh = entity->getMesh();
    for (unsigned short i = 0; i < mesh->getNumAnimations(); ++i) {
        Ogre::Animation* a = mesh->getAnimation(i);
        if (a && !a->_getVertexTrackList().empty())
            out << QString::fromStdString(a->getName());
    }
    return out;
}

bool everySubmeshOwnsVertices(Ogre::Entity* entity)
{
    if (!entity || !entity->getMesh()) return false;
    Ogre::MeshPtr mesh = entity->getMesh();
    if (mesh->getNumSubMeshes() == 0) return false;
    for (unsigned short si = 0; si < mesh->getNumSubMeshes(); ++si) {
        const Ogre::SubMesh* sub = mesh->getSubMesh(si);
        if (!sub || sub->useSharedVertices) return false;
    }
    return true;
}

} // namespace

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
    // `availableAnimations` keeps its historical meaning — the skeletal
    // clips of the selection (what the pre-#522 panel listed). Per-mode
    // lists come from `animationsForMode`.
    Ogre::Entity* entity = firstSelectedEntity();
    QStringList fresh = skeletalClips(entity);

    QStringList modes;
    if (entity) {
        const QStringList vclips = vertexClips(entity);
        const bool hasPoses = entity->getMesh() && entity->getMesh()->getPoseCount() > 0;
        if (!fresh.isEmpty()) modes << VATBaker::modeId(VATBaker::Mode::Skeletal);
        if ((!fresh.isEmpty() || !vclips.isEmpty()) && everySubmeshOwnsVertices(entity))
            modes << VATBaker::modeId(VATBaker::Mode::Rigid);
        if (!vclips.isEmpty()) modes << VATBaker::modeId(VATBaker::Mode::MeshAnim);
        if (!vclips.isEmpty() && hasPoses) modes << VATBaker::modeId(VATBaker::Mode::Morph);
    }

    const bool changed = (fresh != m_animations) || (modes != m_modes);
    m_animations = std::move(fresh);
    m_modes = std::move(modes);
    if (changed) emit availableAnimationsChanged();
}

QStringList VATBakerController::encodingIds() const { return VATBaker::encodingIds(); }
QStringList VATBakerController::targetIds() const   { return VATBaker::targetIds(); }

QStringList VATBakerController::animationsForMode(const QString& modeId) const
{
    VATBaker::Mode mode;
    if (!VATBaker::modeFromId(modeId, &mode)) return {};
    Ogre::Entity* entity = firstSelectedEntity();
    if (!entity) return {};
    switch (mode) {
    case VATBaker::Mode::Skeletal: return skeletalClips(entity);
    case VATBaker::Mode::Rigid:    return skeletalClips(entity) + vertexClips(entity);
    case VATBaker::Mode::MeshAnim: return vertexClips(entity);
    case VATBaker::Mode::Morph:
        return (entity->getMesh() && entity->getMesh()->getPoseCount() > 0)
            ? vertexClips(entity) : QStringList{};
    }
    return {};
}

QString VATBakerController::modeLabel(const QString& modeId) const
{
    VATBaker::Mode mode;
    if (!VATBaker::modeFromId(modeId, &mode)) return modeId;
    switch (mode) {
    case VATBaker::Mode::Skeletal: return QStringLiteral("Skeletal (per-vertex)");
    case VATBaker::Mode::Rigid:    return QStringLiteral("Rigid body (per-chunk)");
    case VATBaker::Mode::MeshAnim: return QStringLiteral("Mesh anim (vertex cache)");
    case VATBaker::Mode::Morph:    return QStringLiteral("Morph (blend shapes)");
    }
    return modeId;
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
                              const QString& basename,
                              const QStringList& includeShadersFor,
                              const QString& modeId,
                              const QString& encodingId,
                              const QString& target)
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

    VATBaker::Mode mode = VATBaker::Mode::Skeletal;
    if (!VATBaker::modeFromId(modeId.isEmpty() ? QStringLiteral("skeletal") : modeId, &mode)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("VAT bake refused: unknown mode '%1'").arg(modeId));
        emit bakeFinished(false, QString(),
            QStringLiteral("unknown VAT mode '%1' (accepted: %2)")
                .arg(modeId, VATBaker::modeIds().join(QStringLiteral(", "))));
        return false;
    }
    int bitDepth = 16;
    if (!VATBaker::bitDepthFromEncodingId(
            encodingId.isEmpty() ? QStringLiteral("rgba16") : encodingId, &bitDepth)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("VAT bake refused: unknown encoding '%1'").arg(encodingId));
        emit bakeFinished(false, QString(),
            QStringLiteral("unknown VAT encoding '%1' (accepted: %2)")
                .arg(encodingId, VATBaker::encodingIds().join(QStringLiteral(", "))));
        return false;
    }
    if (!VATBaker::isValidTargetId(target)) {
        SentryReporter::addBreadcrumb("ui.action",
            QStringLiteral("VAT bake refused: unknown target '%1'").arg(target));
        emit bakeFinished(false, QString(),
            QStringLiteral("unknown VAT target '%1' (accepted: %2)")
                .arg(target, VATBaker::targetIds().join(QStringLiteral(", "))));
        return false;
    }
    if (mode == VATBaker::Mode::Skeletal && !entity->hasSkeleton()) {
        SentryReporter::addBreadcrumb("ui.action",
            "VAT bake refused: selected entity has no skeleton");
        emit bakeFinished(false, QString(), QStringLiteral("selected entity has no skeleton"));
        return false;
    }

    VATBaker::Options opts;
    opts.mode          = mode;
    opts.animationName = animationName;
    opts.fps           = fps;
    opts.outputDir     = outputDir;
    opts.basename      = basename.isEmpty() ? animationName : basename;
    opts.bitDepth      = bitDepth;
    opts.target        = target.trimmed().toLower();

    // file.export — this is an output-writing operation, not a UI
    // click. Sentry split-by-category keeps file telemetry isolated
    // from generic UI noise. `vat_mode=` is the #522 tag.
    SentryReporter::addBreadcrumb("file.export",
        QStringLiteral("OpenVAT bake start: vat_mode=%1 anim=%2 fps=%3 encoding=%4 target=%5 → %6")
            .arg(VATBaker::modeId(mode), animationName).arg(fps)
            .arg(VATBaker::encodingId(bitDepth), opts.target.isEmpty()
                     ? QStringLiteral("agnostic") : opts.target, outputDir));

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

    // Drop the requested drop-in shader templates next to the bake. The
    // controller takes the engine list from the QML inspector's
    // checkboxes (or the CLI parses `--include-shaders` into the same
    // list before calling the static path in CLIPipeline).
    // A non-agnostic target implies "ship that engine's template" —
    // the target IS the reason the user picked it.
    QStringList engines = includeShadersFor;
    if (!opts.target.isEmpty() && opts.target != QLatin1String("agnostic")
        && !engines.contains(opts.target, Qt::CaseInsensitive))
        engines << opts.target;
    if (result.ok && !engines.isEmpty()) {
        const QStringList shadersWritten =
            VATShaderEmitter::writeShaders(outputDir, engines,
                                           mode == VATBaker::Mode::Rigid);
        if (shadersWritten.isEmpty()) {
            // Requested shaders but nothing landed — bad outputDir,
            // missing resource, or QStringList of unknown engines.
            // Breadcrumb it so a user reporting "I asked for Godot and
            // didn't get a shader file" lands telemetry the same as
            // any other file-export miss.
            SentryReporter::addBreadcrumb("file.export",
                QStringLiteral("VAT shaders requested but none written "
                               "(engines=%1, out=%2)")
                    .arg(engines.join(QStringLiteral(",")),
                         outputDir));
        } else {
            SentryReporter::addBreadcrumb("file.export",
                QStringLiteral("VAT shaders written: %1")
                    .arg(shadersWritten.join(QStringLiteral(", "))));
        }
    }

    SentryReporter::addBreadcrumb("file.export",
        result.ok
            ? QStringLiteral("VAT bake ok: vat_mode=%1 %2 frames × %3 columns → %4")
                  .arg(VATBaker::modeId(mode)).arg(result.frameCount)
                  .arg(result.vertexCount).arg(result.posTexPath)
            : QStringLiteral("VAT bake failed: vat_mode=%1 %2")
                  .arg(VATBaker::modeId(mode), result.error));

    if (result.ok)
        GamificationManager::noteOperation(
            QStringLiteral("vat_bake"),
            {{QStringLiteral("frames_baked"), result.frameCount},
             {QStringLiteral("verts_baked"), result.vertexCount},
             {QStringLiteral("vat_mode"), static_cast<int>(mode)}});

    emit bakeFinished(result.ok, result.posTexPath, result.error);
    return true;
}

QString VATBakerController::chooseOutputDir(const QString& startDir)
{
    SentryReporter::addBreadcrumb("ui.action",
        QStringLiteral("OpenVAT output folder picker opened (seed=%1)").arg(startDir));

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

    SentryReporter::addBreadcrumb("ui.action",
        chosen.isEmpty()
            ? QStringLiteral("OpenVAT output folder picker cancelled")
            : QStringLiteral("OpenVAT output folder picker accepted: %1").arg(chosen));
    return chosen;
}
