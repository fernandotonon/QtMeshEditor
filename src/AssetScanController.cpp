#include "AssetScanController.h"

#include "AppSettingsKeys.h"
#include "PlatformProfile.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <memory>

namespace {

constexpr int kProfilePickerSettingsVersion = 2;

QString defaultValidationProfileId()
{
    return QStringLiteral("modern-console");
}

bool isUiExcludedProfileId(const QString& id)
{
    // Example/CI profiles stay on disk for tests and CLI, but are hidden from the GUI picker.
    return id.startsWith(QStringLiteral("example-"));
}

QString sanitizeProfileLabel(const QString& displayName, const QString& id)
{
    QString label = displayName.trimmed();
    if (label.isEmpty())
        label = id;

    static const QRegularExpression suffixRe(
        QStringLiteral(R"(\s*\((?:source\s+)?validation\)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    label.remove(suffixRe);
    return label.trimmed();
}

void sortUiProfiles(QStringList& ids, QStringList& labels)
{
    const int defaultIdx = ids.indexOf(defaultValidationProfileId());
    if (defaultIdx > 0) {
        const QString defaultId = ids.takeAt(defaultIdx);
        const QString defaultLabel = labels.takeAt(defaultIdx);
        ids.prepend(defaultId);
        labels.prepend(defaultLabel);
    }
}

} // namespace

AssetScanController* AssetScanController::m_pSingleton = nullptr;

AssetScanController* AssetScanController::instance()
{
    if (!m_pSingleton)
        m_pSingleton = new AssetScanController();
    return m_pSingleton;
}

AssetScanController* AssetScanController::qmlInstance(QQmlEngine* engine, QJSEngine* /*scriptEngine*/)
{
    auto* inst = instance();
    engine->setObjectOwnership(inst, QQmlEngine::CppOwnership);
    return inst;
}

void AssetScanController::kill()
{
    delete m_pSingleton;
    m_pSingleton = nullptr;
}

AssetScanController::AssetScanController(QObject* parent)
    : QObject(parent)
{
    reloadProfiles();

    QSettings settings;
    const int pickerVersion =
        settings.value(AppSettingsKeys::validationPlatformProfilePickerVersion(), 0).toInt();
    QString saved = settings.value(AppSettingsKeys::validationPlatformProfileId()).toString();

    // One-time migration: older builds could persist an unintended first-launch profile.
    if (pickerVersion < kProfilePickerSettingsVersion) {
        saved.clear();
        settings.setValue(AppSettingsKeys::validationPlatformProfilePickerVersion(),
                          kProfilePickerSettingsVersion);
    }

    if (!saved.isEmpty() && !isUiExcludedProfileId(saved) && m_profileIds.contains(saved))
        setSelectedProfileId(saved);
    else if (m_profileIds.contains(defaultValidationProfileId()))
        setSelectedProfileId(defaultValidationProfileId());
    else if (!m_profileIds.isEmpty())
        setSelectedProfileId(m_profileIds.first());
}

void AssetScanController::reloadProfiles()
{
    m_profileIds.clear();
    m_profileLabels.clear();

    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    for (const QString& id : ids) {
        if (isUiExcludedProfileId(id))
            continue;
        const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(id);
        if (!loaded.ok)
            continue;
        m_profileIds.append(loaded.profile.id);
        m_profileLabels.append(sanitizeProfileLabel(loaded.profile.displayName, loaded.profile.id));
    }

    sortUiProfiles(m_profileIds, m_profileLabels);
    emit profilesChanged();
}

void AssetScanController::setSelectedProfileId(const QString& id)
{
    const QString trimmed = id.trimmed();
    if (m_selectedProfileId == trimmed)
        return;
    if (!trimmed.isEmpty() && !m_profileIds.contains(trimmed))
        return;

    m_selectedProfileId = trimmed;
    updateProfileDescription();

    QSettings settings;
    settings.setValue(AppSettingsKeys::validationPlatformProfileId(), m_selectedProfileId);

    if (!m_selectedProfileId.isEmpty()) {
        SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
            QStringLiteral("validation profile=%1").arg(m_selectedProfileId));
    }

    emit selectedProfileIdChanged();
}

void AssetScanController::updateProfileDescription()
{
    m_profileDescription.clear();
    if (m_selectedProfileId.isEmpty())
        return;

    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(m_selectedProfileId);
    if (loaded.ok)
        m_profileDescription = loaded.profile.description.trimmed();
}

QString AssetScanController::resolveCliBinaryForTest()
{
    const QDir appDir(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString launcher = appDir.filePath(QStringLiteral("qtmesh.exe"));
    if (QFileInfo::exists(launcher))
        return launcher;
#endif
    const QString symlink = appDir.filePath(QStringLiteral("qtmesh"));
    if (QFileInfo::exists(symlink))
        return QFileInfo(symlink).canonicalFilePath();
    return QCoreApplication::applicationFilePath();
}

bool AssetScanController::parseScanJsonReport(const QByteArray& jsonBytes,
                                              int* scanned, int* passed, int* warnings, int* errors,
                                              QVariantList* findings, QString* errorOut)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("Invalid scan JSON: %1").arg(parseError.errorString());
        return false;
    }

    const QJsonObject root = doc.object();
    const QJsonObject summary = root.value(QStringLiteral("summary")).toObject();
    if (scanned)
        *scanned = summary.value(QStringLiteral("scanned")).toInt(0);
    if (passed)
        *passed = summary.value(QStringLiteral("passed")).toInt(0);
    if (warnings)
        *warnings = summary.value(QStringLiteral("warnings")).toInt(0);
    if (errors)
        *errors = summary.value(QStringLiteral("errors")).toInt(0);

    if (findings) {
        findings->clear();
        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue& assetVal : assets) {
            const QJsonObject asset = assetVal.toObject();
            const QString file = asset.value(QStringLiteral("file")).toString();
            const QJsonArray assetFindings = asset.value(QStringLiteral("findings")).toArray();
            for (const QJsonValue& findingVal : assetFindings) {
                const QJsonObject fo = findingVal.toObject();
                const QString severity = fo.value(QStringLiteral("severity")).toString();
                if (severity == QLatin1String("info"))
                    continue;
                QVariantMap row;
                row.insert(QStringLiteral("file"), file);
                row.insert(QStringLiteral("rule"), fo.value(QStringLiteral("rule")).toString());
                row.insert(QStringLiteral("severity"), severity);
                row.insert(QStringLiteral("message"), fo.value(QStringLiteral("message")).toString());
                findings->append(row);
            }
        }
    }

    return true;
}

void AssetScanController::setScanning(bool scanning)
{
    if (m_scanning == scanning)
        return;
    m_scanning = scanning;
    emit scanningChanged();
}

void AssetScanController::applyScanReport(const QByteArray& jsonBytes)
{
    int scanned = 0;
    int passed = 0;
    int warnings = 0;
    int errors = 0;
    QVariantList parsedFindings;
    QString parseError;
    if (!parseScanJsonReport(jsonBytes, &scanned, &passed, &warnings, &errors,
                             &parsedFindings, &parseError)) {
        emit error(parseError);
        emit scanFinished(false, parseError);
        return;
    }

    m_summaryScanned = scanned;
    m_summaryPassed = passed;
    m_summaryWarnings = warnings;
    m_summaryErrors = errors;
    m_findings = parsedFindings;
    m_hasResults = true;
    emit resultsChanged();

    const QString message = QStringLiteral("Scanned %1 file(s): %2 passed, %3 warning(s), %4 error(s)")
                                .arg(scanned)
                                .arg(passed)
                                .arg(warnings)
                                .arg(errors);
    emit scanFinished(true, message);
}

struct ScanSubprocessOutcome {
    bool ok = false;
    QString message;
    QByteArray jsonBytes;
};

static ScanSubprocessOutcome runScanSubprocessSync(const QString& rootPath,
                                                   const QString& profileId,
                                                   const QString& includePattern)
{
    ScanSubprocessOutcome outcome;
    const QString binary = AssetScanController::resolveCliBinaryForTest();
    QStringList args;
    const QString baseName = QFileInfo(binary).fileName().toLower();
    if (baseName.contains(QStringLiteral("editor")))
        args << QStringLiteral("--cli");
    args << QStringLiteral("scan")
         << QDir(rootPath).absolutePath();
    if (!profileId.isEmpty())
        args << QStringLiteral("--target") << profileId;
    args << QStringLiteral("--json")
         << QStringLiteral("--no-telemetry");
    if (!includePattern.isEmpty())
        args << QStringLiteral("--include") << includePattern;

    QProcess process;
    process.setProgram(binary);
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start();
    if (!process.waitForStarted(15000)) {
        outcome.message = QStringLiteral("Could not start scan process: %1").arg(process.errorString());
        return outcome;
    }
    constexpr int kScanTimeoutMs = 10 * 60 * 1000;
    if (!process.waitForFinished(kScanTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        outcome.message = QStringLiteral("Scan process timed out after %1s")
                              .arg(kScanTimeoutMs / 1000);
        return outcome;
    }

    outcome.jsonBytes = process.readAllStandardOutput();
    const QByteArray stderrBytes = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit) {
        outcome.message = QStringLiteral("Scan process crashed");
        return outcome;
    }

    if (outcome.jsonBytes.trimmed().isEmpty()) {
        outcome.message = stderrBytes.trimmed().isEmpty()
                              ? QStringLiteral("Scan produced no output (exit %1)").arg(process.exitCode())
                              : QString::fromUtf8(stderrBytes).trimmed();
        return outcome;
    }

    outcome.ok = true;
    return outcome;
}

QByteArray AssetScanController::runIsolatedScanJsonSync(const QString& rootPath,
                                                          const QString& includePattern,
                                                          QString* errorOut)
{
    const ScanSubprocessOutcome outcome =
        runScanSubprocessSync(rootPath, QString(), includePattern);
    if (!outcome.ok) {
        if (errorOut)
            *errorOut = outcome.message;
        return {};
    }
    return outcome.jsonBytes;
}

void AssetScanController::scanFolder(const QString& rootPath)
{
    if (m_scanning) {
        emit error(QStringLiteral("A scan is already running"));
        return;
    }

    const QString absRoot = QDir(rootPath).absolutePath();
    if (absRoot.isEmpty() || !QFileInfo(absRoot).isDir()) {
        const QString msg = QStringLiteral("Choose a valid asset folder first");
        emit error(msg);
        emit scanFinished(false, msg);
        return;
    }

    if (m_selectedProfileId.isEmpty()) {
        const QString msg = QStringLiteral("Select a platform profile first");
        emit error(msg);
        emit scanFinished(false, msg);
        return;
    }

    const PlatformProfileScanSetup setup = buildScanConfigWithPlatformProfile(m_selectedProfileId);
    if (!setup.ok) {
        emit error(setup.error);
        emit scanFinished(false, setup.error);
        return;
    }

    const QString rootName = QFileInfo(absRoot).fileName();
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
        QStringLiteral("asset scan start profile=%1 rootName=%2")
            .arg(m_selectedProfileId, rootName.isEmpty() ? QStringLiteral("<root>") : rootName));

    setScanning(true);
    m_hasResults = false;
    emit resultsChanged();

    const QString profileId = m_selectedProfileId;
    auto outcome = std::make_shared<ScanSubprocessOutcome>();
    QThread* worker = QThread::create([absRoot, profileId, outcome]() {
        *outcome = runScanSubprocessSync(absRoot, profileId, QString());
    });

    connect(worker, &QThread::finished, this, [this, worker, outcome]() {
        setScanning(false);
        if (!outcome->ok) {
            emit error(outcome->message);
            emit scanFinished(false, outcome->message);
        } else {
            applyScanReport(outcome->jsonBytes);
        }
        worker->deleteLater();
    }, Qt::QueuedConnection);

    worker->start();
}

#ifdef QTMESH_UNIT_TESTS
void AssetScanController::ingestScanReportJsonForTest(const QByteArray& jsonBytes)
{
    applyScanReport(jsonBytes);
}

QString AssetScanController::sanitizeProfileLabelForTest(const QString& displayName, const QString& id)
{
    return sanitizeProfileLabel(displayName, id);
}
#endif
