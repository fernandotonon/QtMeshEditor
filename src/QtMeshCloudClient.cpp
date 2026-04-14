#include "QtMeshCloudClient.h"
#include "SentryReporter.h"

#include <QEventLoop>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>

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
    // JSON numbers only (reject strings such as "foo" — server must send numeric version)
    if (!ver.isDouble())
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
