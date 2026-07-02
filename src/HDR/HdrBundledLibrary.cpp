#include "HDR/HdrBundledLibrary.h"

#include "HDR/HDREnvironmentManager.h"
#include "HDR/HdrTonemap.h"

#include <OgreRoot.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace HdrBundledLibrary {

namespace {

constexpr QLatin1String kFirstRunKey("HdrEnvironment/defaultsAppliedV1");
constexpr QLatin1String kPolyHavenApiBase("https://api.polyhaven.com/files/");

const Entry kCatalog[] = {
    {QStringLiteral("studio_neutral.hdr"), QStringLiteral("studio_small_09"),
     QStringLiteral("studio_small_09 (Poly Haven CC0)")},
    {QStringLiteral("sunset_outdoor.hdr"), QStringLiteral("sunset_forest"),
     QStringLiteral("sunset_forest (Poly Haven CC0)")},
    {QStringLiteral("overcast_outdoor.hdr"), QStringLiteral("overcast_soil_puresky"),
     QStringLiteral("overcast_soil_puresky (Poly Haven CC0)")},
    {QStringLiteral("indoor_window.hdr"), QStringLiteral("anniversary_lounge"),
     QStringLiteral("anniversary_lounge (Poly Haven CC0)")},
    {QStringLiteral("flat_grey.hdr"), {}, QStringLiteral("synthetic neutral grey")},
};

QString bundledHdriRoot()
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates{
        appDir + QStringLiteral("/media/hdri"),
        appDir + QStringLiteral("/../media/hdri"),
    };
#ifdef Q_OS_MACOS
    candidates << appDir + QStringLiteral("/../../media/hdri")
               << appDir + QStringLiteral("/../../../media/hdri");
#endif
#ifdef QTMESH_UT_SOURCE_ROOT
    candidates << QDir(QString::fromUtf8(QTMESH_UT_SOURCE_ROOT))
                      .filePath(QStringLiteral("media/hdri"));
#endif
    for (const QString& path : candidates) {
        if (QDir(path).exists())
            return QDir(path).canonicalPath();
    }
    return candidates.first();
}

QString fetchPolyHavenHdr2kUrl(const QString& assetId, QString* errorOut)
{
    QNetworkAccessManager nam;
    QEventLoop loop;
    QString result;
    QString error;

    QNetworkRequest request(QUrl(kPolyHavenApiBase + assetId));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("QtMeshEditor/3.0 (https://github.com/fernandotonon/QtMeshEditor)"));
    QNetworkReply* reply = nam.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(120000);
    loop.exec();

    if (!timer.isActive()) {
        reply->abort();
        error = QStringLiteral("Timed out fetching Poly Haven metadata for %1").arg(assetId);
    } else if (reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
    } else {
        const auto doc = QJsonDocument::fromJson(reply->readAll());
        const QJsonObject hdri2k =
            doc.object().value(QStringLiteral("hdri")).toObject()
                .value(QStringLiteral("2k")).toObject()
                .value(QStringLiteral("hdr")).toObject();
        result = hdri2k.value(QStringLiteral("url")).toString();
        if (result.isEmpty())
            error = QStringLiteral("No 2k HDR URL in Poly Haven response for %1").arg(assetId);
    }
    reply->deleteLater();
    if (errorOut)
        *errorOut = error;
    return result;
}

bool downloadUrlToFile(const QUrl& url, const QString& destPath, QString* errorOut)
{
    QNetworkAccessManager nam;
    QEventLoop loop;
    QString error;
    bool ok = false;

    QNetworkReply* reply = nam.get(QNetworkRequest(url));
    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly)) {
        if (errorOut)
            *errorOut = QStringLiteral("Cannot write %1").arg(destPath);
        reply->abort();
        reply->deleteLater();
        return false;
    }

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
        out.write(reply->readAll());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(600000);
    loop.exec();

    out.close();
    if (!timer.isActive()) {
        reply->abort();
        error = QStringLiteral("Timed out downloading %1").arg(url.toString());
        QFile::remove(destPath);
    } else if (reply->error() != QNetworkReply::NoError) {
        error = reply->errorString();
        QFile::remove(destPath);
    } else {
        out.write(reply->readAll());
        ok = QFileInfo(destPath).size() > 0;
        if (!ok)
            error = QStringLiteral("Downloaded file is empty: %1").arg(destPath);
    }
    reply->deleteLater();
    if (errorOut)
        *errorOut = error;
    return ok;
}

} // namespace

QStringList catalogFileNames()
{
    QStringList names;
    for (const Entry& e : kCatalog)
        names.append(e.fileName);
    return names;
}

const Entry* findByFileName(const QString& fileName)
{
    const QString base = QFileInfo(fileName).fileName();
    for (const Entry& e : kCatalog) {
        if (e.fileName.compare(base, Qt::CaseInsensitive) == 0)
            return &e;
    }
    return nullptr;
}

const Entry* findByBaseName(const QString& baseName)
{
    QString probe = baseName;
    if (!probe.endsWith(QStringLiteral(".hdr"), Qt::CaseInsensitive)
        && !probe.endsWith(QStringLiteral(".exr"), Qt::CaseInsensitive))
        probe += QStringLiteral(".hdr");
    return findByFileName(probe);
}

QStringList hdriSearchRoots()
{
    QStringList roots;
    const QString bundled = bundledHdriRoot();
    if (QDir(bundled).exists())
        roots.append(bundled);
    const QString user = userHdriDirectory();
    if (QDir(user).exists())
        roots.append(user);
    return roots;
}

QString userHdriDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString dir = base + QStringLiteral("/hdri");
    QDir().mkpath(dir);
    return dir;
}

QString resolveHdriPath(const QString& pathOrFileName)
{
    QFileInfo info(pathOrFileName);
    if (info.isAbsolute() && info.isFile())
        return info.canonicalFilePath();

    const QString fileName = info.fileName();
    for (const QString& root : hdriSearchRoots()) {
        const QFileInfo candidate(root + QLatin1Char('/') + fileName);
        if (candidate.exists() && candidate.isFile())
            return candidate.canonicalFilePath();
    }
    if (info.exists() && info.isFile())
        return info.canonicalFilePath();
    return {};
}

bool downloadHdri(const QString& nameOrFileName, QString* errorOut)
{
    const Entry* entry = findByBaseName(nameOrFileName);
    if (!entry) {
        if (errorOut)
            *errorOut = QStringLiteral("Unknown HDRI '%1'. Use --list for names.").arg(nameOrFileName);
        return false;
    }
    if (entry->polyhavenId.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 is bundled/synthetic and cannot be downloaded.")
                             .arg(entry->fileName);
        return false;
    }

    QString metaError;
    const QString url = fetchPolyHavenHdr2kUrl(entry->polyhavenId, &metaError);
    if (url.isEmpty()) {
        if (errorOut)
            *errorOut = metaError;
        return false;
    }

    const QString dest = userHdriDirectory() + QLatin1Char('/') + entry->fileName;
    QString dlError;
    if (!downloadUrlToFile(QUrl(url), dest, &dlError)) {
        if (errorOut)
            *errorOut = dlError;
        return false;
    }
    return true;
}

void applyFirstRunDefaultsIfNeeded()
{
    QSettings settings;
    if (settings.value(kFirstRunKey, false).toBool())
        return;

    if (!Ogre::Root::getSingletonPtr() || !Ogre::Root::getSingleton().getRenderSystem())
        return;

    auto* hdrMgr = HDREnvironmentManager::getSingletonPtr();
    if (!hdrMgr)
        return;

    if (resolveHdriPath(QStringLiteral("studio_neutral.hdr")).isEmpty())
        return;

    if (hdrMgr->loadEnvironment(QStringLiteral("studio_neutral.hdr"))) {
        hdrMgr->setTonemapOperator(HdrTonemap::Operator::ACES);
        hdrMgr->setExposureEv(0.f);
        settings.setValue(kFirstRunKey, true);
    }
}

} // namespace HdrBundledLibrary
