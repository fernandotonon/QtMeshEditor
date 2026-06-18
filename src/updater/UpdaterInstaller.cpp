#include "UpdaterInstaller.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

namespace UpdaterInstaller {

namespace {

QString platformSlug()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unknown");
#endif
}

QString artifactKindSlug(ArtifactKind kind)
{
    switch (kind) {
    case ArtifactKind::Zip: return QStringLiteral("zip");
    case ArtifactKind::TarGz: return QStringLiteral("tar_gz");
    case ArtifactKind::TarXz: return QStringLiteral("tar_xz");
    case ArtifactKind::Dmg: return QStringLiteral("dmg");
    case ArtifactKind::AppImage: return QStringLiteral("app_image");
    case ArtifactKind::Unknown: break;
    }
    return QStringLiteral("unknown");
}

bool runCommand(QProcess* process,
                const QString& program,
                const QStringList& arguments,
                int timeoutMs,
                QString* errorMessage)
{
    process->start(program, arguments);
    if (!process->waitForStarted()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Failed to start %1: %2")
                                .arg(program, process->errorString());
        }
        return false;
    }
    if (!process->waitForFinished(timeoutMs)) {
        process->kill();
        process->waitForFinished(5000);
        if (errorMessage) {
            *errorMessage = QStringLiteral("%1 timed out").arg(program);
        }
        return false;
    }
    if (process->exitStatus() != QProcess::NormalExit || process->exitCode() != 0) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("%1 failed (%2): %3")
                                .arg(program)
                                .arg(process->exitCode())
                                .arg(QString::fromUtf8(process->readAllStandardError()));
        }
        return false;
    }
    return true;
}

bool extractArchive(const QString& artifactPath,
                    const QString& destinationDir,
                    ArtifactKind kind,
                    QString* errorMessage)
{
    QProcess process;
    QStringList args;

    switch (kind) {
    case ArtifactKind::Zip:
        QDir().mkpath(destinationDir);
        args << QStringLiteral("-xf") << artifactPath << QStringLiteral("-C") << destinationDir;
        return runCommand(&process, QStringLiteral("tar"), args, 600000, errorMessage);
    case ArtifactKind::TarGz:
        QDir().mkpath(destinationDir);
        args << QStringLiteral("-xzf") << artifactPath << QStringLiteral("-C") << destinationDir;
        return runCommand(&process, QStringLiteral("tar"), args, 600000, errorMessage);
    case ArtifactKind::TarXz:
        QDir().mkpath(destinationDir);
        args << QStringLiteral("-xJf") << artifactPath << QStringLiteral("-C") << destinationDir;
        return runCommand(&process, QStringLiteral("tar"), args, 600000, errorMessage);
    case ArtifactKind::AppImage:
        QDir().mkpath(QFileInfo(destinationDir).absolutePath());
        if (QFileInfo::exists(destinationDir) && QFileInfo(destinationDir).isDir()) {
            QDir(destinationDir).removeRecursively();
        } else if (QFile::exists(destinationDir)) {
            QFile::remove(destinationDir);
        }
        if (!QFile::copy(artifactPath, destinationDir)) {
            if (errorMessage) {
                *errorMessage = QStringLiteral("Could not stage AppImage payload");
            }
            return false;
        }
        QFile::setPermissions(destinationDir,
                              QFile::permissions(destinationDir) | QFile::ExeUser | QFile::ExeGroup
                                  | QFile::ExeOther);
        return true;
    default:
        if (errorMessage) {
            *errorMessage = QStringLiteral("Unsupported artifact type for extraction");
        }
        return false;
    }
}

#if defined(Q_OS_MACOS)

bool extractDmg(const QString& dmgPath, const QString& payloadDir, QString* errorMessage)
{
    QDir().mkpath(payloadDir);
    QProcess attach;
    QStringList attachArgs;
    attachArgs << QStringLiteral("attach") << dmgPath << QStringLiteral("-nobrowse")
               << QStringLiteral("-readonly") << QStringLiteral("-plist");
    if (!runCommand(&attach, QStringLiteral("hdiutil"), attachArgs, 120000, errorMessage)) {
        return false;
    }

    const QString plistOutput = QString::fromUtf8(attach.readAllStandardOutput());
    QRegularExpression mountRe(QStringLiteral("<key>mount-point</key>\\s*<string>([^<]+)</string>"));
    const QRegularExpressionMatch match = mountRe.match(plistOutput);
    if (!match.hasMatch()) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Could not determine DMG mount point");
        }
        return false;
    }

    const QString mountPoint = match.captured(1).trimmed();
    const QString sourceApp = mountPoint + QStringLiteral("/QtMeshEditor.app");
    if (!QFileInfo::exists(sourceApp)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("DMG does not contain QtMeshEditor.app");
        }
        QProcess detach;
        runCommand(&detach, QStringLiteral("hdiutil"), {QStringLiteral("detach"), mountPoint}, 60000,
                   nullptr);
        return false;
    }

    QProcess copy;
    const QString targetApp = QDir(payloadDir).filePath(QStringLiteral("QtMeshEditor.app"));
    const bool copied =
        runCommand(&copy,
                   QStringLiteral("rsync"),
                   {QStringLiteral("-a"), sourceApp + QStringLiteral("/"), targetApp + QStringLiteral("/")},
                   600000,
                   errorMessage);

    QProcess detach;
    runCommand(&detach, QStringLiteral("hdiutil"), {QStringLiteral("detach"), mountPoint}, 60000,
               nullptr);
    return copied;
}

#endif

QString installRootFromExecutable(const QString& executablePath)
{
    const QFileInfo exeInfo(executablePath);
#if defined(Q_OS_MACOS)
    QString path = exeInfo.absoluteFilePath();
    const int appIndex = path.indexOf(QStringLiteral(".app"), 0, Qt::CaseInsensitive);
    if (appIndex >= 0) {
        return path.left(appIndex + 4);
    }
    return exeInfo.absolutePath();
#elif defined(Q_OS_WIN)
    if (exeInfo.dir().dirName().compare(QStringLiteral("bin"), Qt::CaseInsensitive) == 0) {
        return QFileInfo(exeInfo.absolutePath()).dir().absolutePath();
    }
    return exeInfo.absolutePath();
#else
    QString path = exeInfo.absoluteFilePath();
    if (path.contains(QStringLiteral("/opt/QtMeshEditor"), Qt::CaseInsensitive)) {
        return QStringLiteral("/opt/QtMeshEditor");
    }
    if (exeInfo.dir().dirName() == QStringLiteral("bin")) {
        return QFileInfo(exeInfo.absolutePath()).dir().absolutePath();
    }
    return exeInfo.absolutePath();
#endif
}

bool writeManifest(const InstallContext& context,
                   const InstallPlan& plan,
                   const QString& payloadDir,
                   const QString& newDir,
                   const QString& oldDir,
                   QString* outManifestPath,
                   QString* errorMessage)
{
    const QString manifestDir =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/updater");
    QDir().mkpath(manifestDir);
    const QString manifestPath = QDir(manifestDir).filePath(QStringLiteral("install-manifest.txt"));

    QFile manifestFile(manifestPath);
    if (!manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = QStringLiteral("Cannot write install manifest");
        }
        return false;
    }

    QTextStream out(&manifestFile);
    out << "parent_pid=" << context.parentPid << '\n';
    out << "platform=" << platformSlug() << '\n';
    out << "artifact_kind=" << artifactKindSlug(plan.artifactKind) << '\n';
    out << "install_root=" << context.installRoot << '\n';
    out << "executable_path=" << context.executablePath << '\n';
    out << "payload_dir=" << payloadDir << '\n';
    if (!newDir.isEmpty()) {
        out << "new_dir=" << newDir << '\n';
    }
    if (!oldDir.isEmpty()) {
        out << "old_dir=" << oldDir << '\n';
    }
    out << "release_tag=" << context.releaseTag << '\n';
    manifestFile.close();
    if (outManifestPath) {
        *outManifestPath = manifestPath;
    }
    return true;
}

} // namespace

ArtifactKind detectArtifactKind(const QString& fileName)
{
    const QString lower = fileName.toLower();
    if (lower.endsWith(QStringLiteral(".zip"))) {
        return ArtifactKind::Zip;
    }
    if (lower.endsWith(QStringLiteral(".tar.gz")) || lower.endsWith(QStringLiteral(".tgz"))) {
        return ArtifactKind::TarGz;
    }
    if (lower.endsWith(QStringLiteral(".tar.xz"))) {
        return ArtifactKind::TarXz;
    }
    if (lower.endsWith(QStringLiteral(".dmg"))) {
        return ArtifactKind::Dmg;
    }
    if (lower.endsWith(QStringLiteral(".appimage"))) {
        return ArtifactKind::AppImage;
    }
    return ArtifactKind::Unknown;
}

QString relauncherExecutablePath()
{
    const QFileInfo exe(QCoreApplication::applicationFilePath());
#if defined(Q_OS_WIN)
    return exe.dir().absoluteFilePath(QStringLiteral("qtmesh-relauncher.exe"));
#else
    return exe.dir().absoluteFilePath(QStringLiteral("qtmesh-relauncher"));
#endif
}

bool isInstallLocationWritable(const InstallContext& context)
{
    if (context.installRoot.isEmpty()) {
        return false;
    }
    const QFileInfo rootInfo(context.installRoot);
    if (rootInfo.exists()) {
        return rootInfo.isWritable();
    }
    return QFileInfo(rootInfo.absolutePath()).isWritable();
}

QString resolveInstallRoot(const QString& executablePath)
{
    const QString exe =
        executablePath.isEmpty() ? QCoreApplication::applicationFilePath() : executablePath;
    return installRootFromExecutable(exe);
}

InstallPlan prepareInstall(const InstallContext& context)
{
    InstallPlan plan;

    InstallContext resolved = context;
    if (resolved.executablePath.isEmpty()) {
        resolved.executablePath = QCoreApplication::applicationFilePath();
    }
    if (resolved.installRoot.isEmpty()) {
        resolved.installRoot = resolveInstallRoot(resolved.executablePath);
    }

    if (resolved.stagedArtifactPath.isEmpty()
        || !QFileInfo::exists(resolved.stagedArtifactPath)) {
        plan.errorMessage = QStringLiteral("Staged update artifact is missing");
        return plan;
    }

    plan.artifactKind = detectArtifactKind(QFileInfo(resolved.stagedArtifactPath).fileName());
    if (plan.artifactKind == ArtifactKind::Unknown) {
        plan.errorMessage = QStringLiteral("Unsupported update artifact type");
        return plan;
    }

    if (!isInstallLocationWritable(resolved)) {
        plan.errorMessage =
            QStringLiteral("Install location is not writable — use your package manager instead");
        return plan;
    }

    QString extractError;
    const QFileInfo stagedInfo(resolved.stagedArtifactPath);
    const QString workDir = stagedInfo.absolutePath();
    QString payloadDir;
    QString newDir;
    QString oldDir;

#if defined(Q_OS_WIN)
    {
        QDir installParent(resolved.installRoot);
        const QString installFolderName = QFileInfo(resolved.installRoot).fileName();
        installParent.cdUp();
        newDir = installParent.filePath(installFolderName + QStringLiteral("_new"));
        oldDir = installParent.filePath(installFolderName + QStringLiteral("_old"));
    }
    QDir(newDir).removeRecursively();
    QDir().mkpath(newDir);
    payloadDir = newDir;
    if (!extractArchive(resolved.stagedArtifactPath, payloadDir, plan.artifactKind, &extractError)) {
        plan.errorMessage = extractError;
        return plan;
    }
#elif defined(Q_OS_MACOS)
    payloadDir = QDir(workDir).filePath(QStringLiteral("payload"));
    QDir(payloadDir).removeRecursively();
    if (plan.artifactKind == ArtifactKind::Dmg) {
        if (!extractDmg(resolved.stagedArtifactPath, payloadDir, &extractError)) {
            plan.errorMessage = extractError;
            return plan;
        }
    } else if (!extractArchive(resolved.stagedArtifactPath, payloadDir, plan.artifactKind,
                               &extractError)) {
        plan.errorMessage = extractError;
        return plan;
    }
#elif defined(Q_OS_LINUX)
    if (plan.artifactKind == ArtifactKind::AppImage) {
        payloadDir = QDir(workDir).filePath(QStringLiteral("payload/") + stagedInfo.fileName());
        if (!extractArchive(resolved.stagedArtifactPath, payloadDir, plan.artifactKind,
                            &extractError)) {
            plan.errorMessage = extractError;
            return plan;
        }
    } else {
        payloadDir = QDir(workDir).filePath(QStringLiteral("payload"));
        QDir(payloadDir).removeRecursively();
        if (!extractArchive(resolved.stagedArtifactPath, payloadDir, plan.artifactKind,
                            &extractError)) {
            plan.errorMessage = extractError;
            return plan;
        }
    }
#else
    plan.errorMessage = QStringLiteral("In-app install is not supported on this platform");
    return plan;
#endif

    QString manifestError;
    if (!writeManifest(resolved, plan, payloadDir, newDir, oldDir, &plan.manifestPath,
                       &manifestError)) {
        plan.errorMessage = manifestError;
        return plan;
    }

    plan.ok = true;
    return plan;
}

bool launchRelauncher(const InstallPlan& plan)
{
    if (!plan.ok || plan.manifestPath.isEmpty()) {
        return false;
    }

    const QString relauncher = relauncherExecutablePath();
    if (!QFileInfo::exists(relauncher)) {
        return false;
    }

    return QProcess::startDetached(relauncher, {plan.manifestPath});
}

#ifdef QTMESH_UNIT_TESTS
QString resolveInstallRootForTest(const QString& applicationFilePath)
{
    return resolveInstallRoot(applicationFilePath);
}
#endif

} // namespace UpdaterInstaller
