#include "QtMeshCloudClient.h"
#include "FeedbackDiagnostics.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSet>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

namespace {

QString trimSnippet(const QByteArray& body, int maxLen = 512)
{
    QString s = QString::fromUtf8(body);
    s.replace(QLatin1Char('\n'), QLatin1Char(' '));
    s.replace(QLatin1Char('\r'), QLatin1Char(' '));
    if (s.size() > maxLen)
        s = s.left(maxLen) + QStringLiteral("…");
    return s;
}

bool httpStatusRetryable(int code)
{
    return code == 408 || code == 429 || code == 500 || code == 502 || code == 503 || code == 504;
}

QNetworkRequest authorizedJsonRequest(const QUrl& url, const QString& bearerToken, int timeoutMs)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);
    return req;
}

QString ownerProjectPath(const QString& ownerSlug, const QString& projectSlug, const QString& suffix)
{
    return QStringLiteral("/v1/u/%1/p/%2/%3")
        .arg(QString::fromUtf8(QUrl::toPercentEncoding(ownerSlug)),
             QString::fromUtf8(QUrl::toPercentEncoding(projectSlug)),
             suffix);
}

QString jsonStringField(const QJsonObject& object, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const QJsonValue value = object.value(QString::fromLatin1(key));
        if (value.isString()) {
            const QString text = value.toString().trimmed();
            if (!text.isEmpty())
                return text;
        }
    }
    return {};
}

qint64 jsonInt64Field(const QJsonObject& object, std::initializer_list<const char*> keys)
{
    for (const char* key : keys) {
        const QJsonValue value = object.value(QString::fromLatin1(key));
        if (value.isDouble())
            return static_cast<qint64>(value.toDouble());
        if (value.isString()) {
            bool ok = false;
            const qint64 parsed = value.toString().trimmed().toLongLong(&ok);
            if (ok)
                return parsed;
        }
    }
    return 0;
}

QString inferSourceFormat(const QString& explicitFormat, const QString& mainFile)
{
    const QString fmt = explicitFormat.trimmed().toLower();
    if (!fmt.isEmpty())
        return fmt;
    const QString suffix = QFileInfo(mainFile).suffix().toLower();
    return suffix.isEmpty() ? QString() : suffix;
}

} // namespace

QString QtMeshCloudClient::projectDashboardUrl(const QString& ownerSlug, const QString& projectSlug)
{
    const QString owner = ownerSlug.trimmed();
    const QString slug = projectSlug.trimmed();
    if (owner.isEmpty() || slug.isEmpty())
        return {};
    return QStringLiteral("https://qtmesh.dev/#/projects/%1/%2/dashboard").arg(owner, slug);
}

namespace {
bool parseJsonObjectBody(const QByteArray& body, QJsonObject& out, QString& error)
{
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QStringLiteral("invalid JSON: %1").arg(perr.errorString());
        return false;
    }
    out = doc.object();
    return true;
}

QString jsonErrorCode(const QJsonObject& root)
{
    return root.value(QStringLiteral("error")).toString();
}

QString pathLeaf(const QString& path)
{
    const QString name = QFileInfo(path).fileName();
    return name.isEmpty() ? path : name;
}

} // namespace

QString QtMeshCloudClient::apiBaseUrl()
{
    const QByteArray env = qgetenv("QTMESH_API_BASE");
    if (!env.isEmpty()) {
        QString u = QString::fromUtf8(env).trimmed();
        while (u.endsWith(QLatin1Char('/')))
            u.chop(1);
        return u;
    }
    return QStringLiteral("https://api.qtmesh.dev");
}

bool QtMeshCloudClient::validateCloudConfigJson(const QJsonObject& root)
{
    const QJsonValue ver = root.value(QStringLiteral("version"));
    if (ver.isUndefined() || ver.isNull())
        return false;
    // JSON numbers only (reject strings and bools — server must send numeric version)
    if (ver.isString() || ver.isBool() || !ver.isDouble())
        return false;

    const QJsonValue scan = root.value(QStringLiteral("scan"));
    if (!scan.isObject())
        return false;

    const QJsonValue rules = root.value(QStringLiteral("rules"));
    if (!rules.isObject())
        return false;

    return true;
}

QtMeshCloudClient::RulesResult QtMeshCloudClient::fetchRules(const QString& bearerToken, int timeoutMs)
{
    RulesResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/ingest/rules"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;

    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QStringLiteral("QtMesh Cloud fetchRules: start %1").arg(url.toString()));

    for (int attempt = 0; attempt < 3; ++attempt) {
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesh-cli"));
        req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
        req.setTransferTimeout(timeoutMs);

        QNetworkReply* reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto err = reply->error();
        const QString transportErr = reply->errorString();
        reply->deleteLater();

        if (err == QNetworkReply::NoError && httpStatus == 200) {
            QJsonParseError perr{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
            if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
                out.errorString = QStringLiteral("invalid JSON: %1").arg(perr.errorString());
                return out;
            }
            const QJsonObject obj = doc.object();
            const QJsonValue cfgVal = obj.value(QStringLiteral("config"));
            if (!cfgVal.isObject()) {
                out.errorString = QStringLiteral("response missing \"config\" object");
                return out;
            }
            out.config = cfgVal.toObject();
            out.source = obj.value(QStringLiteral("source")).toString();
            if (out.source.isEmpty())
                out.source = QStringLiteral("default");
            if (!validateCloudConfigJson(out.config)) {
                out.errorString = QStringLiteral("remote config failed validation (need version, scan, rules)");
                SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                    QStringLiteral("QtMesh Cloud fetchRules: validation failed"));
                out.config = {};
                return out;
            }
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                QStringLiteral("QtMesh Cloud fetchRules: ok source=%1").arg(out.source));
            out.ok = true;
            return out;
        }

        QString errMsg;
        if (err != QNetworkReply::NoError)
            errMsg = transportErr;
        else
            errMsg = QStringLiteral("HTTP %1").arg(httpStatus);

        const bool retry = attempt < 2
            && (err == QNetworkReply::TimeoutError
                || err == QNetworkReply::TemporaryNetworkFailureError
                || err == QNetworkReply::NetworkSessionFailedError
                || err == QNetworkReply::ConnectionRefusedError
                || httpStatusRetryable(httpStatus));

        if (retry) {
            QThread::msleep(150 * (attempt + 1));
            continue;
        }

        out.errorString = errMsg;
        if (!body.isEmpty())
            out.errorString += QStringLiteral(": ") + trimSnippet(body);
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
            QStringLiteral("QtMesh Cloud fetchRules: failure %1").arg(out.errorString),
            QStringLiteral("warning"));
        return out;
    }

    out.errorString = QStringLiteral("exhausted retries");
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QStringLiteral("QtMesh Cloud fetchRules: exhausted retries"), QStringLiteral("warning"));
    return out;
}

QtMeshCloudClient::UploadResult QtMeshCloudClient::uploadScanReport(const QString& bearerToken,
                                                                    const QJsonObject& reportJson,
                                                                    int timeoutMs)
{
    UploadResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/ingest/scan"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesh-cli"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    const QByteArray payload = QJsonDocument(reportJson).toJson(QJsonDocument::Compact);

    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("QtMesh Cloud uploadScan: start %1").arg(url.toString()));

    for (int attempt = 0; attempt < 3; ++attempt) {
        QNetworkReply* reply = nam.post(req, payload);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();

        out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto nerr = reply->error();
        const QString transportErr = reply->errorString();
        reply->deleteLater();

        if (nerr == QNetworkReply::NoError && out.httpStatus >= 200 && out.httpStatus < 300) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
                QStringLiteral("QtMesh Cloud uploadScan: ok HTTP %1").arg(out.httpStatus));
            out.ok = true;
            return out;
        }

        QString errMsg;
        if (nerr != QNetworkReply::NoError)
            errMsg = transportErr;
        else
            errMsg = QStringLiteral("HTTP %1").arg(out.httpStatus);

        out.responseBodySnippet = trimSnippet(body);

        const bool retry = attempt < 2
            && (nerr == QNetworkReply::TimeoutError
                || nerr == QNetworkReply::TemporaryNetworkFailureError
                || httpStatusRetryable(out.httpStatus));

        if (retry) {
            QThread::msleep(200 * (attempt + 1));
            continue;
        }

        out.errorString = errMsg;
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("QtMesh Cloud uploadScan: failure HTTP %1 %2")
                .arg(out.httpStatus)
                .arg(out.responseBodySnippet.isEmpty() ? errMsg : out.responseBodySnippet),
            QStringLiteral("warning"));
        return out;
    }

    out.errorString = QStringLiteral("exhausted retries");
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("QtMesh Cloud uploadScan: exhausted retries"), QStringLiteral("warning"));
    return out;
}

QtMeshCloudClient::DeviceCodeResult QtMeshCloudClient::requestDeviceCode(const QString& clientName,
                                                                         int timeoutMs)
{
    DeviceCodeResult out;
    const QString trimmedClientName = clientName.trimmed();
    if (trimmedClientName.isEmpty()) {
        out.errorString = QStringLiteral("client name is required");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/oauth/device/code"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QJsonObject body;
    body.insert(QStringLiteral("clientName"), trimmedClientName);

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setTransferTimeout(timeoutMs);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
        QStringLiteral("QtMesh Cloud device code: start"));

    QNetworkReply* reply = nam.post(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud device code: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString))
        return out;

    out.deviceCode = root.value(QStringLiteral("device_code")).toString();
    out.userCode = root.value(QStringLiteral("user_code")).toString();
    out.verificationUri = root.value(QStringLiteral("verification_uri")).toString();
    out.verificationUriComplete = root.value(QStringLiteral("verification_uri_complete")).toString();
    out.expiresInSeconds = root.value(QStringLiteral("expires_in")).toInt(0);
    out.intervalSeconds = root.value(QStringLiteral("interval")).toInt(5);
    out.ok = !out.deviceCode.isEmpty() && !out.userCode.isEmpty() && !out.verificationUriComplete.isEmpty();
    if (!out.ok)
        out.errorString = QStringLiteral("response missing device_code, user_code, or verification_uri_complete");
    else
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud device code: ok"));
    return out;
}

QtMeshCloudClient::DeviceTokenResult QtMeshCloudClient::pollDeviceToken(const QString& deviceCode,
                                                                        int timeoutMs)
{
    DeviceTokenResult out;
    if (deviceCode.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("device code is required");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/oauth/device/token"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QJsonObject body;
    body.insert(QStringLiteral("device_code"), deviceCode.trimmed());

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setTransferTimeout(timeoutMs);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = nam.post(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    QJsonObject root;
    QString parseError;
    const bool parsed = parseJsonObjectBody(responseBody, root, parseError);
    if (parsed) {
        out.errorCode = jsonErrorCode(root);
        out.intervalSeconds = root.value(QStringLiteral("interval")).toInt(5);
    }

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        if (!out.errorCode.isEmpty())
            out.errorString = out.errorCode;
        else if (nerr != QNetworkReply::NoError)
            out.errorString = transportErr;
        else
            out.errorString = QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (out.errorCode.isEmpty() && !out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        return out;
    }

    if (!parsed) {
        out.errorString = parseError;
        return out;
    }

    out.token = root.value(QStringLiteral("token")).toString();
    out.expiresAt = static_cast<qint64>(root.value(QStringLiteral("expiresAt")).toDouble(0));
    out.user = root.value(QStringLiteral("user")).toObject();
    out.ok = !out.token.isEmpty() && !out.user.isEmpty();
    if (!out.ok)
        out.errorString = QStringLiteral("response missing token or user");
    else
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud device token: ok"));
    return out;
}

QtMeshCloudClient::CurrentUserResult QtMeshCloudClient::fetchCurrentUser(const QString& bearerToken,
                                                                         int timeoutMs)
{
    CurrentUserResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/auth/me"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
        QStringLiteral("QtMesh Cloud fetchCurrentUser: start"));

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud fetchCurrentUser: failure %1").arg(out.errorString),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString)) {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud fetchCurrentUser: failure %1").arg(out.errorString),
            QStringLiteral("warning"));
        return out;
    }
    out.user = root.value(QStringLiteral("user")).toObject();
    out.ok = !out.user.isEmpty();
    if (!out.ok) {
        out.errorString = QStringLiteral("response missing user");
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud fetchCurrentUser: failure %1").arg(out.errorString),
            QStringLiteral("warning"));
        return out;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
        QStringLiteral("QtMesh Cloud fetchCurrentUser: ok"));
    return out;
}

QtMeshCloudClient::UploadResult QtMeshCloudClient::logout(const QString& bearerToken, int timeoutMs)
{
    UploadResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/auth/logout"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req = authorizedJsonRequest(url, bearerToken, timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
        QStringLiteral("QtMesh Cloud logout: start"));

    QNetworkReply* reply = nam.post(req, QByteArrayLiteral("{}"));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    out.responseBodySnippet = trimSnippet(responseBody);
    out.ok = nerr == QNetworkReply::NoError && out.httpStatus >= 200 && out.httpStatus < 300;
    if (!out.ok) {
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud logout: failure %1").arg(out.errorString),
            QStringLiteral("warning"));
    } else {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.auth"),
            QStringLiteral("QtMesh Cloud logout: ok"));
    }
    return out;
}

QtMeshCloudClient::ProjectsListResult QtMeshCloudClient::fetchProjects(const QString& bearerToken,
                                                                       const QString& cursor,
                                                                       int limit,
                                                                       int timeoutMs)
{
    ProjectsListResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }

    QUrl url(apiBaseUrl() + QStringLiteral("/v1/projects"));
    {
        QUrlQuery query;
        if (!cursor.isEmpty())
            query.addQueryItem(QStringLiteral("cursor"), cursor);
        if (limit > 0)
            query.addQueryItem(QStringLiteral("limit"), QString::number(limit));
        if (!query.isEmpty())
            url.setQuery(query);
    }
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.list"),
                                  QStringLiteral("QtMesh Cloud fetchProjects: start"));

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.list"),
            QStringLiteral("QtMesh Cloud fetchProjects: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString))
        return out;

    const QJsonValue projectsValue = root.value(QStringLiteral("projects"));
    if (!projectsValue.isArray()) {
        out.errorString = QStringLiteral("response missing \"projects\" array");
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.list"),
            QStringLiteral("QtMesh Cloud fetchProjects: malformed response"),
            QStringLiteral("warning"));
        return out;
    }

    const QJsonArray projects = projectsValue.toArray();
    for (const QJsonValue& value : projects) {
        const QJsonObject project = value.toObject();
        ProjectSummary summary;
        summary.id = project.value(QStringLiteral("id")).toString();
        summary.ownerSlug = project.value(QStringLiteral("ownerSlug")).toString();
        summary.projectSlug = project.value(QStringLiteral("slug")).toString();
        if (summary.projectSlug.isEmpty())
            summary.projectSlug = project.value(QStringLiteral("projectSlug")).toString();
        summary.name = jsonStringField(project, {"name"});
        const QString mainFile = jsonStringField(project, {"mainFile", "main_file"});
        summary.mainFile = mainFile;
        summary.sourceFormat = inferSourceFormat(
            jsonStringField(project, {"sourceFormat", "source_format"}), mainFile);
        summary.sizeBytes = jsonInt64Field(
            project, {"sizeBytes", "size_bytes", "totalSize", "total_size", "size"});
        summary.updatedAt = jsonStringField(
            project, {"updatedAt", "updated_at", "modifiedAt", "modified_at", "lastModified",
                      "createdAt", "created_at"});
        if (!summary.ownerSlug.isEmpty() && !summary.projectSlug.isEmpty()) {
            summary.browserUrl =
                projectDashboardUrl(summary.ownerSlug, summary.projectSlug);
            summary.projectUrl = summary.browserUrl;
        }
        if (!summary.id.isEmpty()
            && !summary.ownerSlug.isEmpty()
            && !summary.projectSlug.isEmpty()) {
            out.projects.append(summary);
        }
    }

    out.nextCursor = root.value(QStringLiteral("nextCursor")).toString();
    out.hasMore = !out.nextCursor.isEmpty();
    out.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.list"),
        QStringLiteral("QtMesh Cloud fetchProjects: ok count=%1").arg(out.projects.size()));
    return out;
}

QtMeshCloudClient::UploadResult QtMeshCloudClient::deleteProject(const QString& bearerToken,
                                                                 const QString& projectId,
                                                                 int timeoutMs)
{
    UploadResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (projectId.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("missing project id");
        return out;
    }

    const QUrl url(apiBaseUrl()
        + QStringLiteral("/v1/projects/")
        + QString::fromUtf8(QUrl::toPercentEncoding(projectId.trimmed())));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.delete"),
                                  QStringLiteral("QtMesh Cloud deleteProject: start"));

    QNetworkReply* reply = nam.deleteResource(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    out.responseBodySnippet = trimSnippet(responseBody);
    out.ok = nerr == QNetworkReply::NoError && out.httpStatus >= 200 && out.httpStatus < 300;
    if (!out.ok) {
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.delete"),
                                      QStringLiteral("projectId=%1 failure HTTP %2")
                                          .arg(projectId.trimmed())
                                          .arg(out.httpStatus),
                                      QStringLiteral("warning"));
    } else {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.delete"),
                                      QStringLiteral("projectId=%1").arg(projectId.trimmed()));
    }
    return out;
}

QtMeshCloudClient::ProjectResult QtMeshCloudClient::createProject(const QString& bearerToken,
                                                                  const QString& name,
                                                                  const QString& slug,
                                                                  const QString& description,
                                                                  int timeoutMs)
{
    ProjectResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (name.trimmed().isEmpty() || slug.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("project name and slug are required");
        return out;
    }

    const QUrl url(apiBaseUrl() + QStringLiteral("/v1/projects"));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QJsonObject body;
    body.insert(QStringLiteral("name"), name.trimmed());
    body.insert(QStringLiteral("slug"), slug.trimmed().toLower());
    if (!description.trimmed().isEmpty())
        body.insert(QStringLiteral("description"), description.trimmed());

    QNetworkAccessManager nam;
    QNetworkRequest req = authorizedJsonRequest(url, bearerToken, timeoutMs);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.project"),
        QStringLiteral("QtMesh Cloud createProject: start slug=%1").arg(slug.trimmed().toLower()));

    QNetworkReply* reply = nam.post(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.project"),
            QStringLiteral("QtMesh Cloud createProject: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString))
        return out;
    const QJsonObject project = root.value(QStringLiteral("project")).toObject();
    out.projectId = project.value(QStringLiteral("id")).toString();
    out.ownerSlug = project.value(QStringLiteral("ownerSlug")).toString();
    out.projectSlug = project.value(QStringLiteral("slug")).toString(slug.trimmed().toLower());
    if (!out.ownerSlug.isEmpty() && !out.projectSlug.isEmpty())
        out.projectUrl = projectDashboardUrl(out.ownerSlug, out.projectSlug);
    out.ok = !out.projectId.isEmpty() && !out.ownerSlug.isEmpty() && !out.projectSlug.isEmpty();
    if (!out.ok)
        out.errorString = QStringLiteral("response missing project id, ownerSlug, or slug");
    else
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.project"),
            QStringLiteral("QtMesh Cloud createProject: ok"));
    return out;
}

QtMeshCloudClient::UploadUrlsResult QtMeshCloudClient::requestUploadUrls(
    const QString& bearerToken,
    const QString& ownerSlug,
    const QString& projectSlug,
    const QList<AssetFileDescriptor>& files,
    int timeoutMs)
{
    UploadUrlsResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (ownerSlug.isEmpty() || projectSlug.isEmpty()) {
        out.errorString = QStringLiteral("owner and project slugs are required");
        return out;
    }
    if (files.isEmpty()) {
        out.errorString = QStringLiteral("at least one file is required");
        return out;
    }

    QJsonArray fileArray;
    for (const AssetFileDescriptor& file : files) {
        const QFileInfo info(file.path);
        const qint64 size = file.sizeBytes >= 0 ? file.sizeBytes : info.size();
        if (size <= 0) {
            out.errorString = QStringLiteral("file size must be greater than zero: %1").arg(pathLeaf(file.path));
            return out;
        }

        QJsonObject f;
        f.insert(QStringLiteral("name"), file.uploadName.isEmpty() ? pathLeaf(file.path) : file.uploadName);
        f.insert(QStringLiteral("sizeBytes"), size);
        if (!file.role.isEmpty())
            f.insert(QStringLiteral("role"), file.role);
        if (!file.mimeType.isEmpty())
            f.insert(QStringLiteral("mimeType"), file.mimeType);
        fileArray.append(f);
    }

    QJsonObject body;
    body.insert(QStringLiteral("files"), fileArray);

    const QString path = ownerProjectPath(ownerSlug, projectSlug, QStringLiteral("files/upload-urls"));
    const QUrl url(apiBaseUrl() + path);
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req = authorizedJsonRequest(url, bearerToken, timeoutMs);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud requestUploadUrls: start files=%1").arg(files.size()));

    QNetworkReply* reply = nam.post(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
            QStringLiteral("QtMesh Cloud requestUploadUrls: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString))
        return out;
    out.uploadMethod = root.value(QStringLiteral("uploadMethod")).toString(QStringLiteral("PUT"));
    out.expiresAt = static_cast<qint64>(root.value(QStringLiteral("expiresAt")).toDouble(0));
    const QJsonArray uploads = root.value(QStringLiteral("uploads")).toArray();
    for (const QJsonValue& value : uploads) {
        const QJsonObject u = value.toObject();
        UploadTarget target;
        target.fileId = u.value(QStringLiteral("id")).toString();
        target.uploadUrl = u.value(QStringLiteral("uploadUrl")).toString();
        target.sanitizedName = u.value(QStringLiteral("sanitizedName")).toString();
        target.role = u.value(QStringLiteral("role")).toString();
        target.extension = u.value(QStringLiteral("extension")).toString();
        target.mimeType = u.value(QStringLiteral("mimeType")).toString();
        target.sizeBytes = static_cast<qint64>(u.value(QStringLiteral("sizeBytes")).toDouble(0));
        target.expiresAt = static_cast<qint64>(u.value(QStringLiteral("expiresAt")).toDouble(out.expiresAt));
        if (!target.fileId.isEmpty() && !target.uploadUrl.isEmpty())
            out.uploads.append(target);
    }

    out.ok = out.uploads.size() == files.size();
    if (!out.ok)
        out.errorString = QStringLiteral("response upload count did not match request");
    else
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
            QStringLiteral("QtMesh Cloud requestUploadUrls: ok files=%1").arg(out.uploads.size()));
    return out;
}

QtMeshCloudClient::FileUploadResult QtMeshCloudClient::uploadFileContent(
    const QString& bearerToken,
    const UploadTarget& target,
    const QString& localPath,
    const std::atomic_bool* canceled,
    int timeoutMs)
{
    FileUploadResult out;
    out.fileId = target.fileId;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (target.uploadUrl.isEmpty() || target.fileId.isEmpty()) {
        out.errorString = QStringLiteral("upload target is incomplete");
        return out;
    }
    if (QCoreApplication::instance()
        && QThread::currentThread() == QCoreApplication::instance()->thread()) {
        out.errorString = QStringLiteral("uploadFileContent must run on a worker thread");
        return out;
    }
    if (canceled && canceled->load()) {
        out.canceled = true;
        out.errorString = QStringLiteral("upload canceled");
        return out;
    }

    QFile file(localPath);
    if (!file.open(QIODevice::ReadOnly)) {
        out.errorString = QStringLiteral("could not open file: %1").arg(pathLeaf(localPath));
        return out;
    }
    const qint64 fileSize = file.size();
    if (target.sizeBytes > 0 && fileSize != target.sizeBytes) {
        out.errorString = QStringLiteral("file size changed before upload: %1").arg(pathLeaf(localPath));
        return out;
    }

    const QUrl url(target.uploadUrl);
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid upload URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setHeader(QNetworkRequest::ContentLengthHeader, fileSize);
    if (!target.mimeType.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, target.mimeType);
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud uploadFileContent: start fileId=%1 bytes=%2")
            .arg(target.fileId, QString::number(fileSize)));

    QNetworkReply* reply = nam.put(req, &file);
    QEventLoop loop;
    QTimer cancelPoll;
    if (canceled) {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, &loop, [&]() {
            if (canceled->load())
                reply->abort();
        });
        cancelPoll.start();
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    if (canceled)
        cancelPoll.stop();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (canceled && canceled->load()) {
        out.canceled = true;
        out.errorString = QStringLiteral("upload canceled");
        return out;
    }

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
            QStringLiteral("QtMesh Cloud uploadFileContent: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    QString parseError;
    if (parseJsonObjectBody(responseBody, root, parseError))
        out.sizeBytes = static_cast<qint64>(root.value(QStringLiteral("sizeBytes")).toDouble(fileSize));
    else
        out.sizeBytes = fileSize;
    out.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud uploadFileContent: ok fileId=%1").arg(target.fileId));
    return out;
}

QtMeshCloudClient::CompleteUploadResult QtMeshCloudClient::completeUpload(
    const QString& bearerToken,
    const QString& ownerSlug,
    const QString& projectSlug,
    const QStringList& fileIds,
    const QString& mainFileId,
    int timeoutMs)
{
    CompleteUploadResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (ownerSlug.isEmpty() || projectSlug.isEmpty() || fileIds.isEmpty()) {
        out.errorString = QStringLiteral("owner slug, project slug, and fileIds are required");
        return out;
    }

    QJsonArray ids;
    for (const QString& id : fileIds)
        ids.append(id);
    QJsonObject body;
    body.insert(QStringLiteral("fileIds"), ids);
    if (!mainFileId.isEmpty())
        body.insert(QStringLiteral("mainFileId"), mainFileId);

    const QString path = ownerProjectPath(ownerSlug, projectSlug, QStringLiteral("files/complete"));
    const QUrl url(apiBaseUrl() + path);
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req = authorizedJsonRequest(url, bearerToken, timeoutMs);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud completeUpload: start files=%1").arg(fileIds.size()));

    QNetworkReply* reply = nam.post(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
            QStringLiteral("QtMesh Cloud completeUpload: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString))
        return out;
    out.scanStatus = root.value(QStringLiteral("scanStatus")).toString();
    const QJsonArray files = root.value(QStringLiteral("files")).toArray();
    for (const QJsonValue& value : files) {
        const QString id = value.toObject().value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            out.fileIds.append(id);
    }
    out.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud completeUpload: ok files=%1").arg(out.fileIds.size()));
    return out;
}

QtMeshCloudClient::UploadResult QtMeshCloudClient::uploadFileReport(
    const QString& bearerToken,
    const QString& ownerSlug,
    const QString& projectSlug,
    const QString& fileId,
    const QJsonObject& report,
    int timeoutMs)
{
    UploadResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (ownerSlug.isEmpty() || projectSlug.isEmpty() || fileId.isEmpty()) {
        out.errorString = QStringLiteral("owner slug, project slug, and fileId are required");
        return out;
    }
    if (report.isEmpty()) {
        out.errorString = QStringLiteral("report JSON is empty");
        return out;
    }

    const QByteArray payload = QJsonDocument(report).toJson(QJsonDocument::Compact);
    static constexpr int kMaxReportBytes = 5 * 1024 * 1024;
    if (payload.size() > kMaxReportBytes) {
        out.errorString = QStringLiteral("report exceeds 5 MB limit");
        return out;
    }

    const QString path = ownerProjectPath(ownerSlug, projectSlug,
                                          QStringLiteral("files/%1/report")
                                              .arg(QString::fromUtf8(QUrl::toPercentEncoding(fileId))));
    const QUrl url(apiBaseUrl() + path);
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req = authorizedJsonRequest(url, bearerToken, timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud uploadFileReport: start fileId=%1 bytes=%2")
            .arg(fileId, QString::number(payload.size())));

    QNetworkReply* reply = nam.put(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
            QStringLiteral("QtMesh Cloud uploadFileReport: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    QJsonObject root;
    if (!parseJsonObjectBody(responseBody, root, out.errorString))
        return out;
    if (root.contains(QStringLiteral("ok")) && !root.value(QStringLiteral("ok")).toBool()) {
        out.errorString = jsonErrorCode(root);
        if (out.errorString.isEmpty())
            out.errorString = QStringLiteral("report upload rejected");
        out.responseBodySnippet = trimSnippet(responseBody);
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
            QStringLiteral("QtMesh Cloud uploadFileReport: rejected"),
            QStringLiteral("warning"));
        return out;
    }

    out.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.upload"),
        QStringLiteral("QtMesh Cloud uploadFileReport: ok fileId=%1").arg(fileId));
    return out;
}

QtMeshCloudClient::ManifestResult QtMeshCloudClient::fetchProjectManifest(const QString& bearerToken,
                                                                          const QString& ownerSlug,
                                                                          const QString& projectSlug,
                                                                          int timeoutMs)
{
    ManifestResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        return out;
    }
    if (ownerSlug.isEmpty() || projectSlug.isEmpty()) {
        out.errorString = QStringLiteral("owner and project slugs are required");
        return out;
    }

    const QString path = ownerProjectPath(ownerSlug, projectSlug, QStringLiteral("manifest"));
    const QUrl url(apiBaseUrl() + path);
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        return out;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.project"),
        QStringLiteral("QtMesh Cloud fetchProjectManifest: start"));

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || out.httpStatus < 200 || out.httpStatus >= 300) {
        out.responseBodySnippet = trimSnippet(responseBody);
        out.errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(out.httpStatus);
        if (!out.responseBodySnippet.isEmpty())
            out.errorString += QStringLiteral(" — ") + out.responseBodySnippet;
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.project"),
            QStringLiteral("QtMesh Cloud fetchProjectManifest: failure HTTP %1").arg(out.httpStatus),
            QStringLiteral("warning"));
        return out;
    }

    if (!parseJsonObjectBody(responseBody, out.manifest, out.errorString))
        return out;
    out.ok = true;
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.project"),
        QStringLiteral("QtMesh Cloud fetchProjectManifest: ok"));
    return out;
}

namespace {

QString manifestFileRelativePath(const QJsonObject& file)
{
    QString relative = file.value(QStringLiteral("originalName")).toString();
    if (relative.isEmpty())
        relative = file.value(QStringLiteral("name")).toString();
    relative.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (relative.startsWith(QLatin1Char('/')))
        relative = relative.mid(1);
    if (relative.contains(QStringLiteral("..")))
        relative = QFileInfo(relative).fileName();
    if (relative.isEmpty())
        relative = QStringLiteral("asset.bin");
    return relative;
}

bool isImportableMeshRelativePath(const QString& relative)
{
    return QtMeshCloudClient::isImportableCloudAssetPath(relative);
}

QString pickManifestMainRelativePath(const QJsonArray& files)
{
    QString modelRoleRelative;
    QString meshFallbackRelative;

    for (const QJsonValue& value : files) {
        const QJsonObject file = value.toObject();
        const QString relative = manifestFileRelativePath(file);
        const QString role = file.value(QStringLiteral("role")).toString().toLower();
        if ((role == QLatin1String("main") || role == QLatin1String("model"))
            && modelRoleRelative.isEmpty()) {
            modelRoleRelative = relative;
        }
        if (meshFallbackRelative.isEmpty() && isImportableMeshRelativePath(relative))
            meshFallbackRelative = relative;
    }

    if (!modelRoleRelative.isEmpty())
        return modelRoleRelative;
    if (!meshFallbackRelative.isEmpty())
        return meshFallbackRelative;
    if (files.isEmpty())
        return QString();
    return manifestFileRelativePath(files.first().toObject());
}

bool downloadAuthenticatedUrl(const QString& bearerToken,
                              const QUrl& url,
                              const QString& localPath,
                              int timeoutMs,
                              QString& errorString)
{
    if (bearerToken.isEmpty()) {
        errorString = QStringLiteral("missing bearer token");
        return false;
    }
    if (!url.isValid()) {
        errorString = QStringLiteral("invalid download URL");
        return false;
    }

    QFileInfo targetInfo(localPath);
    QDir parent = targetInfo.dir();
    if (!parent.exists() && !parent.mkpath(QStringLiteral("."))) {
        errorString = QStringLiteral("could not create directory: %1").arg(parent.absolutePath());
        return false;
    }

    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));
    req.setRawHeader("Authorization", QByteArrayLiteral("Bearer ") + bearerToken.toUtf8());
    req.setTransferTimeout(timeoutMs);

    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    if (nerr != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
        errorString = nerr != QNetworkReply::NoError ? transportErr : QStringLiteral("HTTP %1").arg(httpStatus);
        return false;
    }

    QFile outFile(localPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        errorString = QStringLiteral("could not write file: %1").arg(localPath);
        return false;
    }
    if (outFile.write(body) != body.size()) {
        errorString = QStringLiteral("incomplete write: %1").arg(localPath);
        outFile.remove();
        return false;
    }
    outFile.close();
    return true;
}

QString relativePathForProjectFileEntry(const QtMeshCloudClient::ProjectFileEntry& entry)
{
    QJsonObject file;
    file.insert(QStringLiteral("originalName"), entry.originalName);
    file.insert(QStringLiteral("name"), entry.name);
    return manifestFileRelativePath(file);
}

} // namespace

bool QtMeshCloudClient::isImportableCloudAssetPath(const QString& relativePath)
{
    const QString lower = relativePath.toLower();
    if (lower.endsWith(QStringLiteral(".scene.glb"))
        || lower.endsWith(QStringLiteral(".scene.gltf"))) {
        return true;
    }

    static const QStringList meshExtensions = {
        QStringLiteral(".fbx"),
        QStringLiteral(".glb"),
        QStringLiteral(".gltf"),
        QStringLiteral(".gltf2"),
        QStringLiteral(".obj"),
        QStringLiteral(".dae"),
        QStringLiteral(".stl"),
        QStringLiteral(".ply"),
        QStringLiteral(".mesh"),
        QStringLiteral(".xml"),
        QStringLiteral(".tmd"),
        QStringLiteral(".rsd"),
        QStringLiteral(".x"),
        QStringLiteral(".vrm"),
    };
    for (const QString& ext : meshExtensions) {
        if (lower.endsWith(ext))
            return true;
    }
    return false;
}

QList<QtMeshCloudClient::ProjectFileEntry> QtMeshCloudClient::projectFilesFromManifest(
    const QJsonObject& manifest)
{
    QList<ProjectFileEntry> entries;
    const QJsonArray files = manifest.value(QStringLiteral("files")).toArray();
    entries.reserve(files.size());
    for (const QJsonValue& value : files) {
        const QJsonObject file = value.toObject();
        ProjectFileEntry entry;
        entry.id = file.value(QStringLiteral("id")).toString();
        entry.originalName = file.value(QStringLiteral("originalName")).toString();
        if (entry.originalName.isEmpty())
            entry.originalName = file.value(QStringLiteral("name")).toString();
        entry.name = file.value(QStringLiteral("name")).toString();
        entry.role = file.value(QStringLiteral("role")).toString();
        entry.extension = file.value(QStringLiteral("extension")).toString();
        entry.sizeBytes = file.value(QStringLiteral("sizeBytes")).toVariant().toLongLong();
        entry.downloadUrl = file.value(QStringLiteral("downloadUrl")).toString();
        if (!entry.id.isEmpty())
            entries.append(entry);
    }
    return entries;
}

QStringList QtMeshCloudClient::companionFileIdsForOpen(const QList<ProjectFileEntry>& files,
                                                       const QString& selectedFileId)
{
    QStringList ids;
    if (selectedFileId.isEmpty())
        return ids;

    ids.append(selectedFileId);
    for (const ProjectFileEntry& file : files) {
        if (file.id == selectedFileId)
            continue;
        const QString role = file.role.toLower();
        if (role == QLatin1String("texture")
            || role == QLatin1String("material")
            || role == QLatin1String("sidecar")
            || role == QLatin1String("metadata")) {
            if (!ids.contains(file.id))
                ids.append(file.id);
        }
    }
    return ids;
}

QtMeshCloudClient::ProjectDownloadResult QtMeshCloudClient::downloadManifestFiles(
    const QString& bearerToken,
    const QJsonArray& files,
    const QString& destDir,
    const QStringList& fileIds,
    const QString& localOpenRelative,
    const std::function<void(int, int, const QString&)>& progress,
    const std::atomic_bool* canceled,
    int timeoutMs)
{
    ProjectDownloadResult out;
    out.destDirectory = destDir;
    if (destDir.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("destination directory is required");
        return out;
    }
    if (localOpenRelative.isEmpty()) {
        out.errorString = QStringLiteral("open file path is required");
        return out;
    }
    if (files.isEmpty()) {
        out.errorString = QStringLiteral("project has no downloadable files yet");
        return out;
    }

    const QDir root(destDir);
    if (!root.exists() && !root.mkpath(QStringLiteral("."))) {
        out.errorString = QStringLiteral("could not create destination directory");
        return out;
    }

    QSet<QString> allowedIds;
    if (!fileIds.isEmpty()) {
        for (const QString& id : fileIds) {
            if (!id.isEmpty())
                allowedIds.insert(id);
        }
    }

    QVector<QJsonObject> toDownload;
    toDownload.reserve(files.size());
    for (const QJsonValue& value : files) {
        const QJsonObject file = value.toObject();
        const QString id = file.value(QStringLiteral("id")).toString();
        if (!allowedIds.isEmpty() && !allowedIds.contains(id))
            continue;
        toDownload.append(file);
    }

    if (toDownload.isEmpty()) {
        out.errorString = QStringLiteral("no matching files to download");
        return out;
    }

    const int total = toDownload.size();
    for (int i = 0; i < total; ++i) {
        if (canceled && canceled->load()) {
            out.errorString = QStringLiteral("download canceled");
            return out;
        }

        const QJsonObject file = toDownload.at(i);
        const QString relative = manifestFileRelativePath(file);
        const QString downloadUrl = file.value(QStringLiteral("downloadUrl")).toString();
        if (downloadUrl.isEmpty()) {
            out.errorString = QStringLiteral("manifest file is missing downloadUrl");
            return out;
        }

        if (progress)
            progress(i + 1, total, relative);

        const QString localPath = root.filePath(relative);
        QString downloadError;
        if (!downloadAuthenticatedUrl(bearerToken, QUrl(downloadUrl), localPath, timeoutMs, downloadError)) {
            out.errorString = downloadError.isEmpty()
                ? QStringLiteral("download failed for %1").arg(relative)
                : downloadError;
            return out;
        }
    }

    out.localMainFile = root.filePath(localOpenRelative);
    if (!QFileInfo::exists(out.localMainFile)) {
        out.errorString = QStringLiteral("downloaded file is missing");
        out.localMainFile.clear();
        return out;
    }

    out.ok = true;
    return out;
}

QtMeshCloudClient::ProjectDownloadResult QtMeshCloudClient::downloadProjectFileBySlug(
    const QString& bearerToken,
    const QString& ownerSlug,
    const QString& projectSlug,
    const QString& fileId,
    const QString& destDir,
    const std::function<void(int, int, const QString& fileName)>& progress,
    const std::atomic_bool* canceled,
    int timeoutMs)
{
    ProjectDownloadResult out;
    out.destDirectory = destDir;
    if (ownerSlug.trimmed().isEmpty() || projectSlug.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("owner and project slugs are required");
        return out;
    }
    if (fileId.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("file id is required");
        return out;
    }

    auto manifest = fetchProjectManifest(bearerToken, ownerSlug, projectSlug, timeoutMs);
    if (!manifest.ok) {
        out.errorString = manifest.errorString.isEmpty()
            ? QStringLiteral("could not load project manifest")
            : manifest.errorString;
        return out;
    }

    const QList<ProjectFileEntry> entries = projectFilesFromManifest(manifest.manifest);
    QString openRelative;
    for (const ProjectFileEntry& entry : entries) {
        if (entry.id == fileId) {
            openRelative = relativePathForProjectFileEntry(entry);
            break;
        }
    }
    if (openRelative.isEmpty()) {
        out.errorString = QStringLiteral("file not found in project manifest");
        return out;
    }
    if (!isImportableCloudAssetPath(openRelative)) {
        out.errorString = QStringLiteral("this file type cannot be opened in the editor");
        return out;
    }

    const QStringList ids = companionFileIdsForOpen(entries, fileId);
    const QJsonArray files = manifest.manifest.value(QStringLiteral("files")).toArray();
    out = downloadManifestFiles(bearerToken,
                                  files,
                                  destDir,
                                  ids,
                                  openRelative,
                                  progress,
                                  canceled,
                                  timeoutMs);
    if (out.ok) {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.download"),
            QStringLiteral("QtMesh Cloud downloadProjectFileBySlug: ok %1/%2 file=%3")
                .arg(ownerSlug, projectSlug, fileId));
    }
    return out;
}

QtMeshCloudClient::ProjectDownloadResult QtMeshCloudClient::downloadProjectBySlug(
    const QString& bearerToken,
    const QString& ownerSlug,
    const QString& projectSlug,
    const QString& destDir,
    const std::function<void(int, int, const QString&)>& progress,
    const std::atomic_bool* canceled,
    int timeoutMs)
{
    ProjectDownloadResult out;
    out.destDirectory = destDir;
    if (ownerSlug.trimmed().isEmpty() || projectSlug.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("owner and project slugs are required");
        return out;
    }
    if (destDir.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("destination directory is required");
        return out;
    }

    auto manifest = fetchProjectManifest(bearerToken, ownerSlug, projectSlug, timeoutMs);
    if (!manifest.ok) {
        out.errorString = manifest.errorString.isEmpty()
            ? QStringLiteral("could not load project manifest")
            : manifest.errorString;
        return out;
    }

    const QJsonArray files = manifest.manifest.value(QStringLiteral("files")).toArray();
    if (files.isEmpty()) {
        out.errorString = QStringLiteral("project has no downloadable files yet");
        return out;
    }

    const QString mainRelative = pickManifestMainRelativePath(files);
    if (mainRelative.isEmpty()) {
        out.errorString = QStringLiteral("project has no downloadable files");
        return out;
    }
    if (!isImportableCloudAssetPath(mainRelative)) {
        out.errorString = QStringLiteral("project has no importable mesh file");
        return out;
    }

    out = downloadManifestFiles(bearerToken,
                                files,
                                destDir,
                                {},
                                mainRelative,
                                progress,
                                canceled,
                                timeoutMs);
    if (out.ok) {
        SentryReporter::addBreadcrumb(QStringLiteral("cloud.projects.download"),
            QStringLiteral("QtMesh Cloud downloadProjectBySlug: ok %1/%2")
                .arg(ownerSlug, projectSlug));
    }
    return out;
}

QString QtMeshCloudClient::normalizeFeedbackType(const QString& type)
{
    const QString trimmed = type.trimmed();
    if (trimmed == QStringLiteral("feature"))
        return QStringLiteral("feature_request");
    return trimmed;
}

QJsonObject QtMeshCloudClient::buildFeedbackPayload(const FeedbackSubmission& submission)
{
    QJsonObject body;
    body.insert(QStringLiteral("type"), normalizeFeedbackType(submission.type));
    if (!submission.rating.trimmed().isEmpty())
        body.insert(QStringLiteral("rating"), submission.rating.trimmed());
    body.insert(QStringLiteral("message"), submission.message.trimmed());

    const QString appVersion = QCoreApplication::applicationVersion();
    body.insert(QStringLiteral("appVersion"), appVersion.isEmpty() ? QStringLiteral("unknown") : appVersion);
    body.insert(QStringLiteral("osName"), QSysInfo::productType());
    body.insert(QStringLiteral("osVersion"), QSysInfo::productVersion());
    body.insert(QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture());
    body.insert(QStringLiteral("locale"), QLocale::system().name());
    body.insert(QStringLiteral("editorSessionId"), FeedbackDiagnostics::editorSessionId());

    if (!submission.relatedOperation.trimmed().isEmpty())
        body.insert(QStringLiteral("relatedOperation"), submission.relatedOperation.trimmed());
    if (!submission.relatedFormat.trimmed().isEmpty())
        body.insert(QStringLiteral("relatedFormat"), submission.relatedFormat.trimmed());

    body.insert(QStringLiteral("includeDiagnostics"), submission.includeDiagnostics);
    body.insert(QStringLiteral("contactAllowed"), submission.contactAllowed);

    if (submission.includeDiagnostics) {
        QJsonObject diagnostics = submission.diagnosticsJson;
        if (diagnostics.isEmpty())
            diagnostics = FeedbackDiagnostics::collectDiagnostics(true);
        const QString diagnosticsString = FeedbackDiagnostics::diagnosticsJsonString(diagnostics);
        if (!diagnosticsString.isEmpty()) {
            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(diagnosticsString.toUtf8(), &parseError);
            if (parseError.error == QJsonParseError::NoError && doc.isObject())
                body.insert(QStringLiteral("diagnosticsJson"), doc.object());
            else
                body.insert(QStringLiteral("diagnosticsJson"), diagnosticsString);
        } else {
            body.insert(QStringLiteral("diagnosticsJson"), QJsonObject());
        }
    }

    return body;
}

QString QtMeshCloudClient::friendlyFeedbackError(int httpStatus,
                                                 const QString& errorCode,
                                                 const QString& fallback)
{
    if (httpStatus == 401 || errorCode == QStringLiteral("unauthorized")
        || fallback.contains(QStringLiteral("Unauthorized"), Qt::CaseInsensitive))
        return QStringLiteral("Your QtMesh Cloud session expired. Sign in again and retry.");
    if (httpStatus == 413 || errorCode == QStringLiteral("payload_too_large")
        || fallback.contains(QStringLiteral("Payload too large"), Qt::CaseInsensitive)
        || fallback.contains(QStringLiteral("too large"), Qt::CaseInsensitive))
        return QStringLiteral("Your feedback is too large. Shorten the message or turn off diagnostics.");
    if (httpStatus == 429 || errorCode == QStringLiteral("rate_limited")
        || fallback.contains(QStringLiteral("Too many feedback"), Qt::CaseInsensitive))
        return QStringLiteral("Too many feedback submissions. Please wait a few minutes and try again.");
    if (httpStatus == 400 || errorCode == QStringLiteral("validation_error")) {
        if (fallback.contains(QStringLiteral("Invalid feedback type"), Qt::CaseInsensitive))
            return QStringLiteral("This feedback category is not supported. Choose another type and try again.");
        if (fallback.contains(QStringLiteral("Invalid rating"), Qt::CaseInsensitive))
            return QStringLiteral("The selected rating is not supported. Choose another rating and try again.");
        if (fallback.contains(QStringLiteral("Invalid related operation"), Qt::CaseInsensitive))
            return QStringLiteral("The related workflow value was not accepted. Clear it and try again.");
        if (fallback.contains(QStringLiteral("Message is required"), Qt::CaseInsensitive))
            return QStringLiteral("Enter a message before sending feedback.");
        if (fallback.contains(QStringLiteral("Invalid JSON"), Qt::CaseInsensitive))
            return QStringLiteral("The feedback payload could not be sent. Try again or send it on the website.");
        if (!fallback.isEmpty())
            return QStringLiteral("The server could not accept this feedback: %1").arg(fallback);
        return QStringLiteral("The feedback could not be accepted. Check the message and try again.");
    }
    if (!fallback.isEmpty())
        return fallback;
    if (httpStatus > 0)
        return QStringLiteral("Could not send feedback (HTTP %1).").arg(httpStatus);
    return QStringLiteral("Could not send feedback. Check your connection and try again.");
}

QtMeshCloudClient::FeedbackResult QtMeshCloudClient::submitFeedback(const QString& bearerToken,
                                                                    const FeedbackSubmission& submission,
                                                                    int timeoutMs)
{
    FeedbackResult out;
    if (bearerToken.isEmpty()) {
        out.errorString = QStringLiteral("missing bearer token");
        out.userMessage = friendlyFeedbackError(401, QStringLiteral("unauthorized"), out.errorString);
        return out;
    }
    if (submission.type.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("feedback type is required");
        out.userMessage = friendlyFeedbackError(400, QStringLiteral("validation_error"), out.errorString);
        return out;
    }
    const QString normalizedType = normalizeFeedbackType(submission.type);
    if (!normalizedType.isEmpty()
        && normalizedType != QStringLiteral("bug")
        && normalizedType != QStringLiteral("feature_request")
        && normalizedType != QStringLiteral("general")
        && normalizedType != QStringLiteral("import_problem")
        && normalizedType != QStringLiteral("export_problem")) {
        out.errorString = QStringLiteral("unsupported feedback type: %1").arg(submission.type);
        out.userMessage = friendlyFeedbackError(400, QStringLiteral("validation_error"),
                                                QStringLiteral("Invalid feedback type"));
        return out;
    }
    if (submission.message.trimmed().isEmpty()) {
        out.errorString = QStringLiteral("message is required");
        out.userMessage = friendlyFeedbackError(400, QStringLiteral("validation_error"), out.errorString);
        return out;
    }
    if (submission.message.size() > kFeedbackMaxMessageLength) {
        out.errorString = QStringLiteral("message exceeds maximum length");
        out.userMessage = friendlyFeedbackError(413, QStringLiteral("payload_too_large"), out.errorString);
        return out;
    }

    const QUrl url(apiBaseUrl() + QString(kFeedbackApiPath));
    if (!url.isValid()) {
        out.errorString = QStringLiteral("invalid API base URL");
        out.userMessage = out.errorString;
        return out;
    }

    const QJsonObject body = buildFeedbackPayload(submission);
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkAccessManager nam;
    QNetworkRequest req = authorizedJsonRequest(url, bearerToken, timeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("qtmesheditor"));

    SentryReporter::addBreadcrumb(QStringLiteral("cloud.feedback"),
                                  QStringLiteral("QtMesh Cloud submitFeedback: start type=%1")
                                      .arg(submission.type));

    QNetworkReply* reply = nam.post(req, payload);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    out.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray responseBody = reply->readAll();
    const auto nerr = reply->error();
    const QString transportErr = reply->errorString();
    reply->deleteLater();

    out.responseBodySnippet = trimSnippet(responseBody);

    QJsonObject root;
    QString parseError;
    const bool parsed = parseJsonObjectBody(responseBody, root, parseError);
    const QString errorCode = parsed ? jsonErrorCode(root) : QString();
    const QString apiErrorText = parsed ? root.value(QStringLiteral("error")).toString() : QString();

    // POST /v1/feedback — success is HTTP 201 with { ok, id, status, createdAt }.
    if (nerr == QNetworkReply::NoError && out.httpStatus == 201 && parsed) {
        out.id = root.value(QStringLiteral("id")).toString();
        out.status = root.value(QStringLiteral("status")).toString();
        const QJsonValue createdAtVal = root.value(QStringLiteral("createdAt"));
        if (createdAtVal.isDouble())
            out.createdAt = QString::number(static_cast<qint64>(createdAtVal.toDouble()));
        else
            out.createdAt = createdAtVal.toString();
        const bool okFlag = root.contains(QStringLiteral("ok")) ? root.value(QStringLiteral("ok")).toBool()
                                                                : true;
        if (okFlag && !out.id.isEmpty()) {
            out.ok = true;
            SentryReporter::addBreadcrumb(QStringLiteral("cloud.feedback"),
                                          QStringLiteral("QtMesh Cloud submitFeedback: ok id=%1 HTTP %2")
                                              .arg(out.id)
                                              .arg(out.httpStatus));
            return out;
        }
    }

    QString errMsg;
    if (nerr != QNetworkReply::NoError)
        errMsg = transportErr;
    else if (!apiErrorText.isEmpty())
        errMsg = apiErrorText;
    else
        errMsg = QStringLiteral("HTTP %1").arg(out.httpStatus);
    if (!errorCode.isEmpty() && errorCode != apiErrorText)
        errMsg = errorCode;
    if (!out.responseBodySnippet.isEmpty() && !errMsg.contains(out.responseBodySnippet))
        errMsg += QStringLiteral(" — ") + out.responseBodySnippet;

    out.errorString = errMsg;
    out.userMessage = friendlyFeedbackError(out.httpStatus, errorCode, errMsg);
    SentryReporter::addBreadcrumb(QStringLiteral("cloud.feedback"),
                                  QStringLiteral("QtMesh Cloud submitFeedback: failure HTTP %1 %2")
                                      .arg(out.httpStatus)
                                      .arg(errMsg),
                                  QStringLiteral("warning"));
    return out;
}
