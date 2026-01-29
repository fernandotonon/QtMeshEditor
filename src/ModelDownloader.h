#ifndef MODELDOWNLOADER_H
#define MODELDOWNLOADER_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QFile>
#include <QUrl>
#include <QTimer>
#include <QQmlEngine>
#include <QJSEngine>

class ModelDownloader : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool isDownloading READ isDownloading NOTIFY isDownloadingChanged)
    Q_PROPERTY(QString currentModelName READ currentModelName NOTIFY currentModelNameChanged)
    Q_PROPERTY(float downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)
    Q_PROPERTY(qint64 bytesReceived READ bytesReceived NOTIFY bytesReceivedChanged)
    Q_PROPERTY(qint64 bytesTotal READ bytesTotal NOTIFY bytesTotalChanged)
    Q_PROPERTY(float downloadSpeed READ downloadSpeed NOTIFY downloadSpeedChanged)

public:
    static ModelDownloader* instance();
    static ModelDownloader* qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine);

    bool isDownloading() const { return m_isDownloading; }
    QString currentModelName() const { return m_currentModelName; }
    float downloadProgress() const { return m_progress; }
    qint64 bytesReceived() const { return m_bytesReceived; }
    qint64 bytesTotal() const { return m_bytesTotal; }
    float downloadSpeed() const { return m_downloadSpeed; }

public slots:
    Q_INVOKABLE void startDownload(const QString &url, const QString &destinationPath, const QString &modelName);
    Q_INVOKABLE void pauseDownload();
    Q_INVOKABLE void resumeDownload();
    Q_INVOKABLE void cancelDownload();

signals:
    void isDownloadingChanged();
    void currentModelNameChanged();
    void downloadProgressChanged();
    void bytesReceivedChanged();
    void bytesTotalChanged();
    void downloadSpeedChanged();

    void downloadStarted(const QString &modelName);
    void downloadProgressUpdated(const QString &modelName, qint64 bytesReceived, qint64 bytesTotal);
    void downloadCompleted(const QString &modelName, const QString &filePath);
    void downloadError(const QString &modelName, const QString &error);
    void downloadPaused(const QString &modelName);
    void downloadResumed(const QString &modelName);
    void downloadCanceled(const QString &modelName);

private:
    explicit ModelDownloader(QObject *parent = nullptr);
    ~ModelDownloader();

    void updateDownloadSpeed();

private slots:
    void onReadyRead();
    void onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void onDownloadFinished();
    void onDownloadError(QNetworkReply::NetworkError error);
    void onSpeedTimerTimeout();

private:
    static ModelDownloader* s_instance;

    QNetworkAccessManager *m_networkManager;
    QNetworkReply *m_currentReply = nullptr;
    QFile *m_outputFile = nullptr;

    QString m_currentUrl;
    QString m_currentDestinationPath;
    QString m_currentModelName;
    QString m_tempFilePath;

    bool m_isDownloading = false;
    bool m_isPaused = false;
    float m_progress = 0.0f;
    qint64 m_bytesReceived = 0;
    qint64 m_bytesTotal = 0;
    qint64 m_resumeOffset = 0;

    // Speed calculation
    QTimer *m_speedTimer;
    qint64 m_lastBytesReceived = 0;
    float m_downloadSpeed = 0.0f;
};

#endif // MODELDOWNLOADER_H
