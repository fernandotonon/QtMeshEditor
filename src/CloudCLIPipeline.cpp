#include "CloudCLIPipeline.h"

#include "CloudCredentialStore.h"
#include "CloudUploadPlanner.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"
#include "QtMeshCloudSession.h"
#include "ScanConfig.h"
#include "ScanEngine.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#ifndef Q_OS_WIN
#include <unistd.h>
#else
#include <io.h>
#endif

namespace {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

bool stdinIsInteractive()
{
#ifndef Q_OS_WIN
    return isatty(STDIN_FILENO);
#else
    return _isatty(_fileno(stdin));
#endif
}

QString sessionToken(const QString& apiKeyFlag)
{
    if (!apiKeyFlag.trimmed().isEmpty())
        return apiKeyFlag.trimmed();
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    return CloudCredentialStore::loadSession().token;
}

int cloudSubcommandIndex(int argc, char* argv[])
{
    for (int i = 1; i < argc; ++i) {
        if (QString::fromUtf8(argv[i]) == QLatin1String("cloud"))
            return i + 1;
    }
    return 2;
}

int cloudPayloadIndex(int argc, char* argv[])
{
    return cloudSubcommandIndex(argc, argv) + 1;
}

QString cloudSubcommand(int argc, char* argv[])
{
    const int index = cloudSubcommandIndex(argc, argv);
    return index < argc ? QString::fromUtf8(argv[index]) : QString();
}

bool payloadHasJsonFlag(int argc, char* argv[])
{
    for (int i = cloudPayloadIndex(argc, argv); i < argc; ++i) {
        if (QString::fromUtf8(argv[i]) == QLatin1String("--json"))
            return true;
    }
    return false;
}

QStringList splitCommaList(const QString& value)
{
    QStringList parts;
    for (const QString& part : value.split(QLatin1Char(','), Qt::SkipEmptyParts))
        parts.append(part.trimmed());
    return parts;
}

void persistUserProfile(const QJsonObject& user)
{
    if (user.isEmpty())
        return;
    CloudSession session = CloudCredentialStore::loadSession();
    if (!session.hasToken())
        return;
    const QString email = user.value(QStringLiteral("email")).toString();
    if (!email.isEmpty())
        session.email = email;
    CloudCredentialStore::saveSession(session);
}

QJsonObject limitsToJson(const QtMeshCloudClient::UploadLimitsResult& limits)
{
    QJsonObject obj;
    obj.insert(QStringLiteral("maxFileSizeBytes"), limits.maxFileSizeBytes);
    obj.insert(QStringLiteral("maxProjectSizeBytes"), limits.maxProjectSizeBytes);
    obj.insert(QStringLiteral("maxReportSizeBytes"), limits.maxReportSizeBytes);
    return obj;
}

bool scanSummaryHasErrors(const QJsonObject& scanSummary)
{
    const QJsonObject summary = scanSummary.value(QStringLiteral("summary")).toObject();
    return summary.value(QStringLiteral("errors")).toInt() > 0;
}

bool confirmProceedAfterScanErrors()
{
    if (!stdinIsInteractive())
        return false;
    err() << "Scan reported errors. Proceed with upload? [y/N] " << Qt::flush;
    QTextStream stdinStream(stdin);
    const QString answer = stdinStream.readLine().trimmed().toLower();
    return answer == QLatin1String("y") || answer == QLatin1String("yes");
}

void emitUploadProgressEvent(bool jsonOutput, int current, int total, const QString& fileName)
{
    if (jsonOutput) {
        QJsonObject event;
        event.insert(QStringLiteral("event"), QStringLiteral("progress"));
        event.insert(QStringLiteral("current"), current);
        event.insert(QStringLiteral("total"), total);
        event.insert(QStringLiteral("file"), fileName);
        err() << QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)) << Qt::endl;
        return;
    }
    err() << "Uploading " << current << '/' << total << ": " << fileName << Qt::endl;
}

int cmdCloudLogin(int argc, char* argv[])
{
    QString apiKey;
    for (int i = cloudPayloadIndex(argc, argv); i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == QLatin1String("--api-key") && i + 1 < argc)
            apiKey = QString::fromUtf8(argv[++i]);
    }

    if (!apiKey.isEmpty()) {
        CloudSession session;
        session.token = apiKey;
        const auto me = QtMeshCloudClient::fetchCurrentUser(apiKey);
        if (me.ok)
            persistUserProfile(me.user);
        if (!CloudCredentialStore::saveSession(session)) {
            err() << "Error: could not persist API key securely." << Qt::endl;
            return 1;
        }
        out() << "Saved API key to secure storage." << Qt::endl;
        return 0;
    }

    const auto code = QtMeshCloudClient::requestDeviceCode();
    if (!code.ok) {
        err() << "Error: " << code.errorString << Qt::endl;
        return 1;
    }

    out() << "Visit: " << code.verificationUriComplete << Qt::endl;
    out() << "Code: " << code.userCode << Qt::endl;

    int intervalMs = qMax(1, code.intervalSeconds) * 1000;
    const int maxAttempts = qMax(1, code.expiresInSeconds / qMax(1, code.intervalSeconds) + 2);
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        const auto token = QtMeshCloudClient::pollDeviceToken(code.deviceCode);
        if (token.ok) {
            CloudSession session;
            session.token = token.token;
            session.expiresAt = token.expiresAt;
            session.email = token.user.value(QStringLiteral("email")).toString();
            if (!CloudCredentialStore::saveSession(session)) {
                err() << "Error: signed in but session could not be saved securely." << Qt::endl;
                return 1;
            }
            out() << "Signed in as "
                  << (session.email.isEmpty() ? QStringLiteral("(unknown)") : session.email)
                  << Qt::endl;
            return 0;
        }
        if (token.errorCode != QStringLiteral("authorization_pending")
            && token.errorCode != QStringLiteral("slow_down")) {
            err() << "Error: " << token.errorString << Qt::endl;
            return 1;
        }
        if (token.errorCode == QStringLiteral("slow_down"))
            intervalMs += 2000;
        QThread::msleep(static_cast<unsigned long>(intervalMs));
    }

    err() << "Error: sign-in timed out." << Qt::endl;
    return 1;
}

int cmdCloudLogout()
{
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const QString token = CloudCredentialStore::loadSession().token;
    if (!token.isEmpty())
        QtMeshCloudClient::logout(token);
    CloudCredentialStore::clearSession();
    out() << "Signed out." << Qt::endl;
    return 0;
}

int cmdCloudStatus(bool jsonOutput)
{
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    const bool signedIn = CloudCredentialStore::hasSession();
    CloudSession session = CloudCredentialStore::loadSession();
    QtMeshCloudClient::UploadLimitsResult limits;
    if (signedIn) {
        const auto me = QtMeshCloudClient::fetchCurrentUser(session.token);
        if (me.ok)
            persistUserProfile(me.user);
        session = CloudCredentialStore::loadSession();
        limits = QtMeshCloudClient::fetchUploadLimits(session.token);
    }
    const qint64 lastUploadAt = CloudCredentialStore::lastUploadAt();

    if (jsonOutput) {
        QJsonObject obj;
        obj.insert(QStringLiteral("connected"), signedIn);
        if (signedIn) {
            if (!session.email.isEmpty())
                obj.insert(QStringLiteral("email"), session.email);
            if (lastUploadAt > 0)
                obj.insert(QStringLiteral("lastUploadAt"), lastUploadAt);
            if (limits.ok)
                obj.insert(QStringLiteral("limits"), limitsToJson(limits));
        }
        out() << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)) << Qt::endl;
        return 0;
    }

    if (!signedIn) {
        out() << "Not connected to QtMesh Cloud." << Qt::endl;
        return 0;
    }
    out() << "Connected to QtMesh Cloud";
    if (!session.email.isEmpty())
        out() << " as " << session.email;
    out() << Qt::endl;
    if (lastUploadAt > 0) {
        out() << "Last upload: "
              << QDateTime::fromMSecsSinceEpoch(lastUploadAt).toString(Qt::ISODate) << Qt::endl;
    }
    if (limits.ok) {
        if (limits.maxFileSizeBytes > 0)
            out() << "Max file size: " << limits.maxFileSizeBytes << " bytes" << Qt::endl;
        if (limits.maxProjectSizeBytes > 0)
            out() << "Max project size: " << limits.maxProjectSizeBytes << " bytes" << Qt::endl;
    }
    return 0;
}

int cmdCloudLimits(bool jsonOutput)
{
    const QString token = sessionToken({});
    if (token.isEmpty()) {
        err() << "Error: not signed in. Run `qtmesh cloud login` first." << Qt::endl;
        return 1;
    }
    const auto limits = QtMeshCloudClient::fetchUploadLimits(token);
    if (!limits.ok) {
        err() << "Error: " << limits.errorString << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        out() << QString::fromUtf8(QJsonDocument(limitsToJson(limits)).toJson(QJsonDocument::Compact))
              << Qt::endl;
        return 0;
    }

    out() << "Max file size: "
          << (limits.maxFileSizeBytes > 0 ? QString::number(limits.maxFileSizeBytes) + QStringLiteral(" bytes")
                                          : QStringLiteral("(not reported)"))
          << Qt::endl;
    out() << "Max project size: "
          << (limits.maxProjectSizeBytes > 0
                  ? QString::number(limits.maxProjectSizeBytes) + QStringLiteral(" bytes")
                  : QStringLiteral("(not reported)"))
          << Qt::endl;
    out() << "Max scan report size: " << limits.maxReportSizeBytes << " bytes" << Qt::endl;
    return 0;
}

int cmdCloudList(bool jsonOutput)
{
    const QString token = sessionToken({});
    if (token.isEmpty()) {
        err() << "Error: not signed in. Run `qtmesh cloud login` first." << Qt::endl;
        return 1;
    }
    const auto result = QtMeshCloudClient::fetchAllProjects(token);
    if (!result.ok) {
        err() << "Error: " << result.errorString << Qt::endl;
        return 1;
    }

    if (jsonOutput) {
        QJsonArray projects;
        for (const auto& project : result.projects) {
            QJsonObject obj;
            obj.insert(QStringLiteral("id"), project.id);
            obj.insert(QStringLiteral("name"), project.name);
            obj.insert(QStringLiteral("ownerSlug"), project.ownerSlug);
            obj.insert(QStringLiteral("projectSlug"), project.projectSlug);
            obj.insert(QStringLiteral("projectUrl"), project.projectUrl);
            projects.append(obj);
        }
        QJsonObject root;
        root.insert(QStringLiteral("projects"), projects);
        out() << QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)) << Qt::endl;
        return 0;
    }

    for (const auto& project : result.projects) {
        out() << project.id << '\t'
              << project.ownerSlug << '/' << project.projectSlug << '\t'
              << (project.name.isEmpty() ? project.projectSlug : project.name) << Qt::endl;
    }
    return 0;
}

int cmdCloudDelete(int argc, char* argv[])
{
    QString projectId;
    for (int i = cloudPayloadIndex(argc, argv); i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (!arg.startsWith(QLatin1Char('-')) && projectId.isEmpty())
            projectId = arg;
    }
    if (projectId.isEmpty()) {
        err() << "Usage: qtmesh cloud delete <project-id>" << Qt::endl;
        return 2;
    }

    const QString token = sessionToken({});
    if (token.isEmpty()) {
        err() << "Error: not signed in." << Qt::endl;
        return 1;
    }
    const auto result = QtMeshCloudClient::deleteProject(token, projectId);
    if (!result.ok) {
        err() << "Error: " << result.errorString << Qt::endl;
        return 1;
    }
    out() << "Deleted project " << projectId << Qt::endl;
    return 0;
}

int cmdCloudUpload(int argc, char* argv[])
{
    QString mainFile;
    QString projectName;
    QStringList includeGlobs;
    QStringList excludeGlobs;
    bool jsonOutput = false;
    bool runScan = true;
    bool noConfirm = false;
    for (int i = cloudPayloadIndex(argc, argv); i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == QLatin1String("--json")) {
            jsonOutput = true;
        } else if (arg == QLatin1String("--no-scan")) {
            runScan = false;
        } else if (arg == QLatin1String("--no-confirm")) {
            noConfirm = true;
        } else if (arg == QLatin1String("--name") && i + 1 < argc) {
            projectName = QString::fromUtf8(argv[++i]);
        } else if (arg == QLatin1String("--include") && i + 1 < argc) {
            includeGlobs = splitCommaList(QString::fromUtf8(argv[++i]));
        } else if (arg == QLatin1String("--exclude") && i + 1 < argc) {
            excludeGlobs = splitCommaList(QString::fromUtf8(argv[++i]));
        } else if (!arg.startsWith(QLatin1Char('-')) && mainFile.isEmpty()) {
            mainFile = arg;
        }
    }

    if (mainFile.isEmpty()) {
        err() << "Usage: qtmesh cloud upload <main-file> [--name <name>] [--include \"<glob>,...\"]"
              << " [--exclude \"<glob>,...\"] [--no-scan] [--no-confirm] [--json]" << Qt::endl;
        return 2;
    }
    if (!QFileInfo::exists(mainFile)) {
        err() << "Error: file not found: " << mainFile << Qt::endl;
        return 1;
    }

    const QString token = sessionToken({});
    if (token.isEmpty()) {
        err() << "Error: not signed in." << Qt::endl;
        return 1;
    }

    if (projectName.isEmpty())
        projectName = QFileInfo(mainFile).completeBaseName();

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                  QStringLiteral("QtMesh Cloud CLI upload start"));

    if (runScan) {
        ScanConfig config;
        config.roots = {QFileInfo(mainFile).absolutePath()};
        config.includePatterns = {QFileInfo(mainFile).fileName()};
        const QJsonObject scanSummary =
            ScanEngine::scanReportToJsonObject(ScanEngine::run(config, config.roots.first()));
        if (scanSummaryHasErrors(scanSummary) && !noConfirm && !confirmProceedAfterScanErrors()) {
            err() << "Upload canceled." << Qt::endl;
            return 1;
        }
    }

    const QStringList selectedPaths =
        CloudUploadPlanner::selectedPathsForUpload(mainFile, includeGlobs, excludeGlobs);

    CloudPackageUploadRequest request;
    request.mainAssetPath = mainFile;
    request.selectedAbsolutePaths = selectedPaths;
    request.projectName = projectName;
    request.createNewProject = true;
    request.runLocalScan = runScan;

    QtMeshCloudSession session(token);
    QEventLoop loop;
    QString projectUrl;
    QString error;
    QString reportWarning;
    bool uploadOk = false;
    const int uploadedFileCount = selectedPaths.size();

    QObject::connect(&session, &QtMeshCloudSession::uploadProgress, &loop,
                     [&](int current, int total, const QString& fileName) {
                         emitUploadProgressEvent(jsonOutput, current, total, fileName);
                     });
    QObject::connect(&session, &QtMeshCloudSession::uploadFinished, &loop,
                     [&](bool ok, const QString& err, const QString& url, const QString&) {
                         uploadOk = ok;
                         projectUrl = url;
                         if (ok && !err.isEmpty())
                             reportWarning = err;
                         else if (!ok)
                             error = err.isEmpty() ? QStringLiteral("Upload failed") : err;
                         loop.quit();
                     });
    QObject::connect(&session, &QtMeshCloudSession::uploadCanceled, &loop, [&]() {
        uploadOk = false;
        error = QStringLiteral("Upload canceled");
        loop.quit();
    });
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(10 * 60 * 1000);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        session.cancel();
        uploadOk = false;
        error = QStringLiteral("Upload timed out");
        loop.quit();
    });
    timeout.start();
    session.uploadPackageFromAssets(request);
    loop.exec();

    if (!uploadOk) {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                      QStringLiteral("QtMesh Cloud CLI upload failed"),
                                      QStringLiteral("error"));
        err() << "Error: " << error << Qt::endl;
        return 1;
    }

    CloudCredentialStore::setLastUploadAt(QDateTime::currentMSecsSinceEpoch());
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
                                  QStringLiteral("QtMesh Cloud CLI upload completed"));

    if (jsonOutput) {
        QJsonObject obj;
        obj.insert(QStringLiteral("event"), QStringLiteral("complete"));
        obj.insert(QStringLiteral("ok"), true);
        obj.insert(QStringLiteral("projectUrl"), projectUrl);
        obj.insert(QStringLiteral("fileCount"), uploadedFileCount);
        if (!reportWarning.isEmpty())
            obj.insert(QStringLiteral("reportWarning"), reportWarning);
        out() << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)) << Qt::endl;
    } else {
        out() << "Uploaded " << uploadedFileCount << " file(s).";
        if (!projectUrl.isEmpty())
            out() << " " << projectUrl;
        out() << Qt::endl;
        if (!reportWarning.isEmpty())
            err() << "Warning: " << reportWarning << Qt::endl;
    }
    return 0;
}

} // namespace

int CloudCLIPipeline::run(int argc, char* argv[])
{
    const QString sub = cloudSubcommand(argc, argv);
    if (sub.isEmpty()) {
        err() << "Usage: qtmesh cloud <login|logout|status|limits|list|upload|delete> ..." << Qt::endl;
        return 2;
    }

    if (sub == QLatin1String("login"))
        return cmdCloudLogin(argc, argv);
    if (sub == QLatin1String("logout"))
        return cmdCloudLogout();
    if (sub == QLatin1String("status"))
        return cmdCloudStatus(payloadHasJsonFlag(argc, argv));
    if (sub == QLatin1String("limits"))
        return cmdCloudLimits(payloadHasJsonFlag(argc, argv));
    if (sub == QLatin1String("list"))
        return cmdCloudList(payloadHasJsonFlag(argc, argv));
    if (sub == QLatin1String("delete"))
        return cmdCloudDelete(argc, argv);
    if (sub == QLatin1String("upload"))
        return cmdCloudUpload(argc, argv);

    err() << "Error: unknown cloud subcommand '" << sub << "'" << Qt::endl;
    return 2;
}
