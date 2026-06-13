#include "CloudCLIPipeline.h"

#include "CloudCredentialStore.h"
#include "CloudUploadPlanner.h"
#include "ProjectPackager.h"
#include "QtMeshCloudClient.h"
#include "ScanConfig.h"
#include "ScanEngine.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThread>

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

QString sessionToken(const QString& apiKeyFlag)
{
    if (!apiKeyFlag.trimmed().isEmpty())
        return apiKeyFlag.trimmed();
    CloudCredentialStore::migrateLegacySettingsIfNeeded();
    return CloudCredentialStore::loadSession().token;
}

int cmdCloudLogin(int argc, char* argv[])
{
    QString apiKey;
    for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == QLatin1String("--api-key") && i + 1 < argc)
            apiKey = QString::fromUtf8(argv[++i]);
    }

    if (!apiKey.isEmpty()) {
        CloudSession session;
        session.token = apiKey;
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
    const CloudSession session = CloudCredentialStore::loadSession();

    if (jsonOutput) {
        QJsonObject obj;
        obj.insert(QStringLiteral("connected"), signedIn);
        if (signedIn)
            obj.insert(QStringLiteral("email"), session.email);
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
    return 0;
}

int cmdCloudList(bool jsonOutput)
{
    const QString token = sessionToken({});
    if (token.isEmpty()) {
        err() << "Error: not signed in. Run `qtmesh cloud login` first." << Qt::endl;
        return 1;
    }
    const auto result = QtMeshCloudClient::fetchProjects(token);
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
    for (int i = 2; i < argc; ++i) {
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
    bool jsonOutput = false;
    bool runScan = true;
    for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromUtf8(argv[i]);
        if (arg == QLatin1String("--json")) {
            jsonOutput = true;
        } else if (arg == QLatin1String("--no-scan")) {
            runScan = false;
        } else if (arg == QLatin1String("--name") && i + 1 < argc) {
            projectName = QString::fromUtf8(argv[++i]);
        } else if (!arg.startsWith(QLatin1Char('-')) && mainFile.isEmpty()) {
            mainFile = arg;
        }
    }

    if (mainFile.isEmpty()) {
        err() << "Usage: qtmesh cloud upload <main-file> [--name <name>] [--no-scan] [--json]" << Qt::endl;
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

    PackageMetadata manifest = ProjectPackager::buildManifest(mainFile, {}, projectName);
    if (runScan) {
        ScanConfig config;
        config.roots = {QFileInfo(mainFile).absolutePath()};
        config.includePatterns = {QFileInfo(mainFile).fileName()};
        manifest.scanSummary = ScanEngine::scanReportToJsonObject(
            ScanEngine::run(config, config.roots.first()));
    }

    QString slug = CloudUploadPlanner::makeProjectSlug(manifest.projectName);
    auto project = QtMeshCloudClient::createProject(token, manifest.projectName, slug);
    if (!project.ok && project.httpStatus == 409) {
        slug = CloudUploadPlanner::makeProjectSlug(
            QStringLiteral("%1-%2").arg(manifest.projectName, slug));
        project = QtMeshCloudClient::createProject(token, manifest.projectName, slug);
    }
    if (!project.ok) {
        err() << "Error: " << project.errorString << Qt::endl;
        return 1;
    }

    QList<QtMeshCloudClient::AssetFileDescriptor> descriptors;
    for (const PackageEntry& entry : manifest.files) {
        QtMeshCloudClient::AssetFileDescriptor descriptor;
        descriptor.path = entry.absolutePath.isEmpty() ? entry.relativePath : entry.absolutePath;
        descriptor.uploadName = entry.relativePath;
        descriptor.role = entry.role;
        descriptor.sizeBytes = entry.size;
        descriptors.append(descriptor);
    }

    const auto uploadUrls = QtMeshCloudClient::requestUploadUrls(
        token, project.ownerSlug, project.projectSlug, descriptors);
    if (!uploadUrls.ok) {
        err() << "Error: " << uploadUrls.errorString << Qt::endl;
        return 1;
    }

    QStringList uploadedFileIds;
    QString mainFileId;
    QString fallbackMainFileId;
    for (int i = 0; i < uploadUrls.uploads.size(); ++i) {
        const auto result = QtMeshCloudClient::uploadFileContent(
            token, uploadUrls.uploads.at(i), descriptors.at(i).path, nullptr);
        if (!result.ok) {
            err() << "Error: " << result.errorString << Qt::endl;
            return 1;
        }
        uploadedFileIds.append(uploadUrls.uploads.at(i).fileId);
        if (fallbackMainFileId.isEmpty())
            fallbackMainFileId = uploadUrls.uploads.at(i).fileId;
        if (mainFileId.isEmpty() && descriptors.at(i).role == QLatin1String("main"))
            mainFileId = uploadUrls.uploads.at(i).fileId;
    }
    if (mainFileId.isEmpty())
        mainFileId = fallbackMainFileId;

    const auto completed = QtMeshCloudClient::completeUpload(
        token, project.ownerSlug, project.projectSlug, uploadedFileIds, mainFileId);
    if (!completed.ok) {
        err() << "Error: " << completed.errorString << Qt::endl;
        return 1;
    }

    const QString projectUrl = project.projectUrl;

    if (jsonOutput) {
        QJsonObject obj;
        obj.insert(QStringLiteral("ok"), true);
        obj.insert(QStringLiteral("projectUrl"), projectUrl);
        obj.insert(QStringLiteral("fileCount"), manifest.files.size());
        out() << QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)) << Qt::endl;
    } else {
        out() << "Uploaded " << manifest.files.size() << " file(s).";
        if (!projectUrl.isEmpty())
            out() << " " << projectUrl;
        out() << Qt::endl;
    }
    return 0;
}

} // namespace

int CloudCLIPipeline::run(int argc, char* argv[])
{
    if (argc < 3) {
        err() << "Usage: qtmesh cloud <login|logout|status|list|upload|delete> ..." << Qt::endl;
        return 2;
    }

    const QString sub = QString::fromUtf8(argv[2]);
    if (sub == QLatin1String("login"))
        return cmdCloudLogin(argc, argv);
    if (sub == QLatin1String("logout"))
        return cmdCloudLogout();
    if (sub == QLatin1String("status"))
        return cmdCloudStatus(argc > 3 && QString::fromUtf8(argv[3]) == QLatin1String("--json"));
    if (sub == QLatin1String("list"))
        return cmdCloudList(argc > 3 && QString::fromUtf8(argv[3]) == QLatin1String("--json"));
    if (sub == QLatin1String("delete"))
        return cmdCloudDelete(argc, argv);
    if (sub == QLatin1String("upload"))
        return cmdCloudUpload(argc, argv);

    err() << "Error: unknown cloud subcommand '" << sub << "'" << Qt::endl;
    return 2;
}
