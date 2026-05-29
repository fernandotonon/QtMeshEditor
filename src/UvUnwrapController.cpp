#include "UvUnwrapController.h"
#include "UvUnwrap.h"
#include "SelectionSet.h"
#include "SentryReporter.h"

#include <QFileDialog>

#include <Ogre.h>
#include <OgreEntity.h>

UvUnwrapController* UvUnwrapController::m_pSingleton = nullptr;

UvUnwrapController* UvUnwrapController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new UvUnwrapController();
    return m_pSingleton;
}

UvUnwrapController* UvUnwrapController::qmlInstance(QQmlEngine* engine, QJSEngine*)
{
    Q_UNUSED(engine);
    auto* inst = instance();
    QQmlEngine::setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void UvUnwrapController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

UvUnwrapController::UvUnwrapController() : QObject(nullptr)
{
    connect(SelectionSet::getSingleton(), &SelectionSet::selectionChanged,
            this, &UvUnwrapController::selectionChanged);
}

bool UvUnwrapController::hasSelection() const
{
    auto* sel = SelectionSet::getSingleton();
    if (!sel) return false;
    return !sel->getResolvedEntities().isEmpty();
}

QVariantMap UvUnwrapController::unwrapSelectedToFile(const QString& outputPath,
                                                     int resolution,
                                                     int padding,
                                                     int channel,
                                                     bool preserveOriginalAsBackup)
{
    QVariantMap result;

    if (outputPath.isEmpty()) {
        emit error("Output path required.");
        result["applied"] = false;
        return result;
    }

    auto* sel = SelectionSet::getSingleton();
    if (!sel) {
        emit error("No selection set.");
        result["applied"] = false;
        return result;
    }
    const auto entities = sel->getResolvedEntities();
    if (entities.isEmpty()) {
        emit error("No mesh selected.");
        result["applied"] = false;
        return result;
    }
    Ogre::Entity* entity = entities.first();

    UvUnwrapOptions opts;
    opts.resolution                = std::max(64, resolution);
    opts.padding                   = std::max(0, padding);
    opts.channel                   = std::max(0, channel);
    opts.preserveOriginalAsBackup  = preserveOriginalAsBackup;

    SentryReporter::addBreadcrumb(QStringLiteral("ai.assist.uv_unwrap"),
        QString("UI unwrap entity=%1 → %2 res=%3 pad=%4 ch=%5")
            .arg(QString::fromStdString(entity->getName()))
            .arg(outputPath)
            .arg(opts.resolution).arg(opts.padding).arg(opts.channel));

    m_busy = true;
    emit busyChanged();

    UvUnwrapReport report;
    try {
        report = UvUnwrap::unwrapEntityToFile(entity, outputPath, opts);
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

    result["applied"]            = report.applied;
    result["outputPath"]         = outputPath;
    result["meshName"]           = report.meshName;
    result["submeshCount"]       = report.submeshCount;
    result["verticesBefore"]     = report.verticesBefore;
    result["verticesAfter"]      = report.verticesAfter;
    result["trianglesProcessed"] = report.trianglesProcessed;
    result["atlasWidth"]         = report.atlasWidth;
    result["atlasHeight"]        = report.atlasHeight;
    result["chartCount"]         = report.chartCount;
    result["utilization"]        = report.utilization;
    if (!report.error.isEmpty()) result["error"] = report.error;

    if (report.applied) emit unwrapApplied(result);
    else                emit error(report.error.isEmpty() ? QStringLiteral("UV unwrap failed") : report.error);

    return result;
}

QString UvUnwrapController::chooseOutputPath()
{
    auto* sel = SelectionSet::getSingleton();
    QString suggestedName = QStringLiteral("unwrapped.glb");
    if (sel) {
        const auto entities = sel->getResolvedEntities();
        if (!entities.isEmpty() && entities.first() && entities.first()->getMesh()) {
            QString base = QString::fromStdString(entities.first()->getMesh()->getName());
            // Strip extension if the mesh name has one.
            const int dot = base.lastIndexOf('.');
            if (dot > 0) base = base.left(dot);
            suggestedName = base + QStringLiteral("_unwrapped.glb");
        }
    }

    const QString filter = QStringLiteral(
        "glTF 2.0 Binary (*.glb);;"
        "glTF 2.0 (*.gltf);;"
        "Ogre Mesh (*.mesh);;"
        "FBX Binary (*.fbx);;"
        "OBJ (*.obj);;"
        "Collada (*.dae);;"
        "STL (*.stl);;"
        "PLY (*.ply)");

    return QFileDialog::getSaveFileName(nullptr,
        QStringLiteral("Save Unwrapped Mesh"), suggestedName, filter,
        nullptr, QFileDialog::DontUseNativeDialog);
}
