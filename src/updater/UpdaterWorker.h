#ifndef UPDATERWORKER_H
#define UPDATERWORKER_H

#include "GitHubReleaseParser.h"

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief Worker-thread HTTP for the auto-updater (#441).
 *
 * Owns its QNetworkAccessManager on the worker thread so the main thread
 * never blocks on GitHub API calls.
 */
class UpdaterWorker : public QObject
{
    Q_OBJECT

public:
    explicit UpdaterWorker(QObject* parent = nullptr);
    ~UpdaterWorker() override;

public slots:
    void checkForUpdates(const QString& apiUrl,
                         const QString& localVersion,
                         GitHubReleaseParser::Channel channel);
    void cancelActiveRequest();

signals:
    void checkFinished(const GitHubReleaseParser::CheckResult& result,
                       const QString& networkError);

private slots:
    void onReplyFinished();

private:
    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_activeReply = nullptr;
};

#endif // UPDATERWORKER_H
