#include "UpdaterWorker.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

UpdaterWorker::UpdaterWorker(QObject* parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
}

UpdaterWorker::~UpdaterWorker()
{
    cancelActiveRequest();
}

void UpdaterWorker::cancelActiveRequest()
{
    if (!m_activeReply) {
        return;
    }
    m_activeReply->abort();
    m_activeReply->deleteLater();
    m_activeReply = nullptr;
}

void UpdaterWorker::checkForUpdates(const QString& apiUrl,
                                    const QString& localVersion,
                                    GitHubReleaseParser::Channel channel)
{
    cancelActiveRequest();

    const QUrl url(apiUrl);
    QNetworkRequest httpRequest(url);
    httpRequest.setHeader(QNetworkRequest::UserAgentHeader,
                          QStringLiteral("QtMeshEditor-Updater/1.0"));
    httpRequest.setRawHeader("Accept", "application/vnd.github+json");

    m_activeReply = m_network->get(httpRequest);
    m_activeReply->setProperty("localVersion", localVersion);
    m_activeReply->setProperty("channel", static_cast<int>(channel));
    connect(m_activeReply, &QNetworkReply::finished, this, &UpdaterWorker::onReplyFinished);
}

void UpdaterWorker::onReplyFinished()
{
    QNetworkReply* reply = m_activeReply;
    m_activeReply = nullptr;

    if (!reply) {
        return;
    }

    const QString localVersion = reply->property("localVersion").toString();
    const auto channel =
        static_cast<GitHubReleaseParser::Channel>(reply->property("channel").toInt());

    GitHubReleaseParser::CheckResult result;
    QString networkError;

    if (reply->error() != QNetworkReply::NoError) {
        networkError = reply->errorString();
    } else {
        result = GitHubReleaseParser::pickLatestForChannel(reply->readAll(), channel, localVersion);
        if (!result.parseOk && result.errorMessage.isEmpty()) {
            result.errorMessage = QStringLiteral("Failed to parse GitHub releases response");
        }
    }

    reply->deleteLater();
    emit checkFinished(result, networkError);
}
