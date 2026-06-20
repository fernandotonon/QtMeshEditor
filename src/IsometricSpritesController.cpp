#include "IsometricSpritesController.h"

#include "Manager.h"
#include "ModelIsometricRenderer.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>
#include <OgreEntity.h>

IsometricSpritesController *IsometricSpritesController::s_instance = nullptr;

IsometricSpritesController *IsometricSpritesController::instance()
{
    if (!s_instance)
        s_instance = new IsometricSpritesController();
    return s_instance;
}

IsometricSpritesController *IsometricSpritesController::qmlInstance(QQmlEngine *, QJSEngine *)
{
    return instance();
}

void IsometricSpritesController::kill()
{
    delete s_instance;
    s_instance = nullptr;
}

IsometricSpritesController::IsometricSpritesController(QObject *parent) : QObject(parent)
{
    if (auto *sel = SelectionSet::getSingleton()) {
        connect(sel, &SelectionSet::selectionChanged, this, [this]() {
            refreshAnimations();
            emit selectionChanged();
        });
    }
    refreshAnimations();
}

bool IsometricSpritesController::hasExportableSelection() const
{
    auto *sel = SelectionSet::getSingleton();
    if (!sel)
        return false;
    const auto entities = sel->getResolvedEntities();
    for (Ogre::Entity *entity : entities) {
        if (entity && entity->getMesh())
            return true;
    }
    return false;
}

bool IsometricSpritesController::hasSkinnedSelection() const
{
    auto *sel = SelectionSet::getSingleton();
    if (!sel)
        return false;
    const auto entities = sel->getResolvedEntities();
    for (Ogre::Entity *entity : entities) {
        if (entity && entity->hasSkeleton())
            return true;
    }
    return false;
}

void IsometricSpritesController::refreshAnimations()
{
    QStringList fresh;
    auto *sel = SelectionSet::getSingleton();
    if (sel) {
        const auto entities = sel->getResolvedEntities();
        for (Ogre::Entity *entity : entities) {
            if (!entity || !entity->hasSkeleton())
                continue;
            if (auto *states = entity->getAllAnimationStates()) {
                auto it = states->getAnimationStateIterator();
                while (it.hasMoreElements()) {
                    if (auto *state = it.getNext()) {
                        const QString name = QString::fromStdString(state->getAnimationName());
                        if (!name.isEmpty() && !fresh.contains(name))
                            fresh << name;
                    }
                }
            }
        }
    }
    fresh.sort(Qt::CaseInsensitive);
    if (fresh != m_animations) {
        m_animations = std::move(fresh);
        emit availableAnimationsChanged();
    }
}

void IsometricSpritesController::setExporting(bool exporting)
{
    if (m_isExporting == exporting)
        return;
    m_isExporting = exporting;
    emit isExportingChanged();
}

void IsometricSpritesController::requestOutputPathPick(const QString &startPath)
{
    SentryReporter::addBreadcrumb("ui.action",
                                  QStringLiteral("Isometric sprite save dialog requested (seed=%1)").arg(startPath));
    emit outputPathPickRequested(startPath);
}

QString IsometricSpritesController::chooseOutputPath(const QString &startPath)
{
    SentryReporter::addBreadcrumb("ui.action",
                                  QStringLiteral("Isometric sprite save dialog opened (seed=%1)").arg(startPath));

    QString seed = startPath;
    if (seed.isEmpty() || !QFileInfo(seed).isDir())
        seed = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (seed.isEmpty())
        seed = QDir::homePath();
    seed = QDir(seed).filePath(QStringLiteral("isometric_sprites.png"));

    QApplication::processEvents();
    QWidget *parent = QApplication::activeWindow();
    if (parent) {
        parent->raise();
        parent->activateWindow();
    }
    QApplication::processEvents();

    const QString chosen = QFileDialog::getSaveFileName(
        nullptr, QStringLiteral("Save isometric sprite sheet"), seed,
        QStringLiteral("PNG image (*.png)"), nullptr,
        QFileDialog::DontUseNativeDialog | QFileDialog::DontUseCustomDirectoryIcons);

    SentryReporter::addBreadcrumb(
        "ui.action",
        chosen.isEmpty() ? QStringLiteral("Isometric sprite save dialog cancelled")
                         : QStringLiteral("Isometric sprite save dialog accepted: %1").arg(chosen));
    return chosen;
}

QVariantMap IsometricSpritesController::exportSelected(const QString &outputPath,
                                                     const QString &animationName,
                                                     int directions,
                                                     int frames,
                                                     int resolution,
                                                     double elevation,
                                                     double padding,
                                                     double cameraDistance,
                                                     double startAzimuth)
{
    QVariantMap result;
    auto fail = [&](const QString &msg) {
        result["ok"] = false;
        result["error"] = msg;
        emit exportFinished(false, outputPath, msg);
        return result;
    };

    SentryReporter::addBreadcrumb("ui.action", QStringLiteral("Isometric sprite export requested"));

    if (m_isExporting)
        return fail(QStringLiteral("Export already in progress"));
    if (outputPath.isEmpty())
        return fail(QStringLiteral("Output path is required"));
    if (!hasExportableSelection())
        return fail(QStringLiteral("Select a mesh to export"));
    if (directions <= 0)
        return fail(QStringLiteral("directions must be positive"));
    if (frames <= 0)
        return fail(QStringLiteral("frames must be positive"));
    if (resolution < 16 || resolution > 8192)
        return fail(QStringLiteral("resolution must be in [16..8192]"));

    auto *sel = SelectionSet::getSingleton();
    if (!sel)
        return fail(QStringLiteral("Selection is unavailable"));

    const QList<Ogre::Entity *> entityList = sel->getResolvedEntities();
    if (entityList.isEmpty())
        return fail(QStringLiteral("No mesh selected"));

    const QString anim = animationName.trimmed();
    const int frameCount = anim.isEmpty() ? 1 : frames;

    Ogre::Entity *animatedEntity = nullptr;
    if (!anim.isEmpty()) {
        animatedEntity = ModelIsometricRenderer::findEntityWithAnimation(entityList, anim);
        if (!animatedEntity)
            return fail(QStringLiteral("Animation '%1' not found on selection").arg(anim));
    }

    IsometricOptions options;
    options.width = resolution;
    options.height = resolution;
    options.elevationDegrees = static_cast<float>(elevation);
    options.directionCount = directions;
    options.startAzimuthDegrees = static_cast<float>(startAzimuth);
    options.cameraDistance = static_cast<float>(cameraDistance);
    options.cameraPadding = static_cast<float>(padding <= 0.0 ? 1.25 : padding);

    SentryReporter::addBreadcrumb(
        "file.export",
        QStringLiteral("isometric gui export start dirs=%1 frames=%2 anim=%3")
            .arg(directions)
            .arg(frameCount)
            .arg(anim.isEmpty() ? QStringLiteral("static") : anim));

    setExporting(true);

    QList<QList<QImage>> grid;
    QString renderError;
    const bool rendered =
        ModelIsometricRenderer::renderToGrid(entityList, animatedEntity, anim, frameCount, options, &grid,
                                             &renderError);

    ModelIsometricRenderer::shutdown();

    // renderToGrid clears the selection for a clean capture — restore it.
    sel->clear();
    for (Ogre::Entity *entity : entityList) {
        if (entity)
            sel->append(entity);
    }

    setExporting(false);

    if (!rendered)
        return fail(renderError.isEmpty() ? QStringLiteral("Isometric render failed") : renderError);

    const QImage sheet = ModelIsometricRenderer::composeDirectionGrid(grid);
    if (sheet.isNull() || !sheet.save(outputPath))
        return fail(QStringLiteral("Failed to write %1").arg(outputPath));

    SentryReporter::addBreadcrumb("file.export", QFileInfo(outputPath).absoluteFilePath());

    result["ok"] = true;
    result["outputPath"] = outputPath;
    result["directions"] = directions;
    result["frames"] = frameCount;
    result["sheetWidth"] = sheet.width();
    result["sheetHeight"] = sheet.height();
    result["directionOrder"] = ModelIsometricRenderer::directionOrderConvention();
    result["error"] = QString();
    emit exportFinished(true, outputPath, QString());
    return result;
}
