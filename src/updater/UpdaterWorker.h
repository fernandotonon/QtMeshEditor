#ifndef UPDATERWORKER_H
#define UPDATERWORKER_H

#include "GitHubReleaseParser.h"

#include <QMetaType>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

struct DownloadRequest {
    QString artifactUrl;
    QString artifactPartPath;
    QString artifactFinalPath;
    QString signatureUrl;
    QString signaturePath;
    QString sha256SumsUrl;
    QString sha256SumsPath;
    QString fileName;
};

struct DownloadOutcome {
    bool ok = false;
    bool cancelled = false;
    QString errorMessage;
    QString artifactPath;
    QString signaturePath;
    QString sha256SumsPath;
    QString fileName;
};

struct VerifyRequest {
    QString artifactPath;
    QString signaturePath;
    QString sha256SumsPath;
    QString fileName;
};

struct VerifyOutcome {
    bool ok = false;
    QString errorMessage;
    QString failedStage;
};

Q_DECLARE_METATYPE(DownloadRequest)
Q_DECLARE_METATYPE(DownloadOutcome)
Q_DECLARE_METATYPE(VerifyRequest)
Q_DECLARE_METATYPE(VerifyOutcome)

/**
 * @brief Worker-thread HTTP for the auto-updater (#441, #444, #445).
 *
 * Owns its QNetworkAccessManager on the worker thread so the main thread
 * never blocks on GitHub API or artifact download calls.
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
    void downloadUpdate(const DownloadRequest& request);
    void verifyDownload(const VerifyRequest& request);
    void cancelActiveRequest();

signals:
    void checkFinished(const GitHubReleaseParser::CheckResult& result,
                       const QString& networkError);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished(const DownloadOutcome& outcome);
    void verifyFinished(const VerifyOutcome& outcome);

private slots:
    void onCheckReplyFinished();
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadReplyFinished();

private:
    enum class ActiveJob {
        None,
        Check,
        Download,
    };

    bool downloadUrlBlocking(const QString& url,
                             const QString& destPath,
                             bool resume,
                             QString* errorMessage);
    void finishDownloadWithError(const QString& message, bool cancelled = false);
    void startArtifactDownloadAttempt(int attemptIndex);

    QNetworkAccessManager* m_network = nullptr;
    QNetworkReply* m_activeReply = nullptr;
    ActiveJob m_activeJob = ActiveJob::None;
    DownloadRequest m_downloadRequest;
    int m_downloadAttempt = 0;
    static constexpr int kMaxDownloadAttempts = 3;
    static constexpr int kRetryDelaysMs[kMaxDownloadAttempts] = {0, 5000, 15000};
};

#endif // UPDATERWORKER_H
