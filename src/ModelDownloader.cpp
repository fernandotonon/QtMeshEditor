#include "ModelDownloader.h"
#include <QFileInfo>
#include <QDir>
#include <QDebug>

ModelDownloader* ModelDownloader::s_instance = nullptr;

ModelDownloader* ModelDownloader::instance()
{
    if (!s_instance) {
        s_instance = new ModelDownloader();
    }
    return s_instance;
}

ModelDownloader* ModelDownloader::qmlInstance(QQmlEngine *engine, QJSEngine *scriptEngine)
{
    Q_UNUSED(engine)
    Q_UNUSED(scriptEngine)
    return instance();
}

ModelDownloader::ModelDownloader(QObject *parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
    , m_speedTimer(new QTimer(this))
{
    m_speedTimer->setInterval(1000); // Update speed every second
    connect(m_speedTimer, &QTimer::timeout, this, &ModelDownloader::onSpeedTimerTimeout);
}

ModelDownloader::~ModelDownloader()
{
    cancelDownload();
}

void ModelDownloader::startDownload(const QString &url, const QString &destinationPath, const QString &modelName)
{
    if (m_isDownloading) {
        emit downloadError(modelName, "A download is already in progress");
        return;
    }

    m_currentUrl = url;
    m_currentDestinationPath = destinationPath;
    m_currentModelName = modelName;
    m_tempFilePath = destinationPath + ".part";
    m_resumeOffset = 0;
    m_bytesReceived = 0;
    m_bytesTotal = 0;
    m_progress = 0.0f;
    m_downloadSpeed = 0.0f;
    m_lastBytesReceived = 0;

    // Create destination directory if it doesn't exist
    QFileInfo fileInfo(destinationPath);
    QDir dir = fileInfo.absoluteDir();
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    // Check if partial download exists
    QFileInfo tempFileInfo(m_tempFilePath);
    if (tempFileInfo.exists()) {
        m_resumeOffset = tempFileInfo.size();
        qDebug() << "ModelDownloader: Resuming download from byte" << m_resumeOffset;
    }

    // Open output file
    m_outputFile = new QFile(m_tempFilePath, this);
    QIODevice::OpenMode mode = (m_resumeOffset > 0) ? QIODevice::Append : QIODevice::WriteOnly;
    if (!m_outputFile->open(mode)) {
        emit downloadError(modelName, QString("Cannot open file for writing: %1").arg(m_tempFilePath));
        delete m_outputFile;
        m_outputFile = nullptr;
        return;
    }

    // Create request with range header for resume support
    QUrl requestUrl(url);
    QNetworkRequest request(requestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    if (m_resumeOffset > 0) {
        QString rangeHeader = QString("bytes=%1-").arg(m_resumeOffset);
        request.setRawHeader("Range", rangeHeader.toUtf8());
    }

    m_currentReply = m_networkManager->get(request);

    connect(m_currentReply, &QNetworkReply::readyRead, this, &ModelDownloader::onReadyRead);
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, &ModelDownloader::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::finished, this, &ModelDownloader::onDownloadFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred, this, &ModelDownloader::onDownloadError);

    m_isDownloading = true;
    m_isPaused = false;
    m_speedTimer->start();

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadStarted(modelName);

    qDebug() << "ModelDownloader: Started download of" << modelName << "from" << url;
}

void ModelDownloader::pauseDownload()
{
    if (!m_isDownloading || m_isPaused || !m_currentReply) {
        return;
    }

    m_isPaused = true;
    m_speedTimer->stop();

    // Abort the current reply - data already written will be preserved
    m_currentReply->abort();
    m_currentReply->deleteLater();
    m_currentReply = nullptr;

    if (m_outputFile) {
        m_outputFile->flush();
        m_outputFile->close();
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    emit downloadPaused(m_currentModelName);
    qDebug() << "ModelDownloader: Download paused at byte" << m_bytesReceived;
}

void ModelDownloader::resumeDownload()
{
    if (!m_isPaused || m_currentUrl.isEmpty()) {
        return;
    }

    m_resumeOffset = m_bytesReceived;

    // Reopen output file for appending
    m_outputFile = new QFile(m_tempFilePath, this);
    if (!m_outputFile->open(QIODevice::Append)) {
        emit downloadError(m_currentModelName, QString("Cannot open file for writing: %1").arg(m_tempFilePath));
        delete m_outputFile;
        m_outputFile = nullptr;
        return;
    }

    // Create request with range header
    QUrl requestUrl(m_currentUrl);
    QNetworkRequest request(requestUrl);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QString rangeHeader = QString("bytes=%1-").arg(m_resumeOffset);
    request.setRawHeader("Range", rangeHeader.toUtf8());

    m_currentReply = m_networkManager->get(request);

    connect(m_currentReply, &QNetworkReply::readyRead, this, &ModelDownloader::onReadyRead);
    connect(m_currentReply, &QNetworkReply::downloadProgress, this, &ModelDownloader::onDownloadProgress);
    connect(m_currentReply, &QNetworkReply::finished, this, &ModelDownloader::onDownloadFinished);
    connect(m_currentReply, &QNetworkReply::errorOccurred, this, &ModelDownloader::onDownloadError);

    m_isPaused = false;
    m_speedTimer->start();

    emit downloadResumed(m_currentModelName);
    qDebug() << "ModelDownloader: Download resumed from byte" << m_resumeOffset;
}

void ModelDownloader::cancelDownload()
{
    m_speedTimer->stop();

    if (m_currentReply) {
        m_currentReply->abort();
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    if (m_outputFile) {
        m_outputFile->close();
        m_outputFile->remove(); // Remove partial download
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    // Also try to remove temp file
    QFile::remove(m_tempFilePath);

    QString canceledModel = m_currentModelName;

    m_isDownloading = false;
    m_isPaused = false;
    m_currentUrl.clear();
    m_currentDestinationPath.clear();
    m_currentModelName.clear();
    m_tempFilePath.clear();
    m_bytesReceived = 0;
    m_bytesTotal = 0;
    m_progress = 0.0f;
    m_downloadSpeed = 0.0f;
    m_resumeOffset = 0;

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadProgressChanged();
    emit bytesReceivedChanged();
    emit bytesTotalChanged();
    emit downloadSpeedChanged();

    if (!canceledModel.isEmpty()) {
        emit downloadCanceled(canceledModel);
        qDebug() << "ModelDownloader: Download canceled";
    }
}

void ModelDownloader::onReadyRead()
{
    if (m_outputFile && m_currentReply) {
        QByteArray data = m_currentReply->readAll();
        m_outputFile->write(data);
    }
}

void ModelDownloader::onDownloadProgress(qint64 bytesReceived, qint64 bytesTotal)
{
    // Adjust for resume offset
    m_bytesReceived = m_resumeOffset + bytesReceived;

    // Handle Content-Range header for resumed downloads
    if (m_resumeOffset > 0 && bytesTotal > 0) {
        m_bytesTotal = m_resumeOffset + bytesTotal;
    } else if (bytesTotal > 0) {
        m_bytesTotal = bytesTotal;
    }

    if (m_bytesTotal > 0) {
        m_progress = static_cast<float>(m_bytesReceived) / static_cast<float>(m_bytesTotal);
    }

    emit bytesReceivedChanged();
    emit bytesTotalChanged();
    emit downloadProgressChanged();
    emit downloadProgressUpdated(m_currentModelName, m_bytesReceived, m_bytesTotal);
}

void ModelDownloader::onDownloadFinished()
{
    m_speedTimer->stop();

    if (m_currentReply && m_currentReply->error() == QNetworkReply::NoError) {
        // Flush and close the file
        if (m_outputFile) {
            m_outputFile->flush();
            m_outputFile->close();
            delete m_outputFile;
            m_outputFile = nullptr;
        }

        // Rename temp file to final destination
        if (QFile::exists(m_currentDestinationPath)) {
            QFile::remove(m_currentDestinationPath);
        }

        if (QFile::rename(m_tempFilePath, m_currentDestinationPath)) {
            qDebug() << "ModelDownloader: Download completed:" << m_currentDestinationPath;
            emit downloadCompleted(m_currentModelName, m_currentDestinationPath);
        } else {
            emit downloadError(m_currentModelName, "Failed to rename downloaded file");
        }
    }

    // Cleanup
    if (m_currentReply) {
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    QString completedModel = m_currentModelName;
    m_isDownloading = false;
    m_isPaused = false;
    m_currentUrl.clear();
    m_currentDestinationPath.clear();
    m_currentModelName.clear();
    m_tempFilePath.clear();
    m_resumeOffset = 0;
    m_downloadSpeed = 0.0f;

    emit isDownloadingChanged();
    emit currentModelNameChanged();
    emit downloadSpeedChanged();
}

void ModelDownloader::onDownloadError(QNetworkReply::NetworkError error)
{
    if (error == QNetworkReply::OperationCanceledError && m_isPaused) {
        // This is expected when pausing
        return;
    }

    QString errorString = m_currentReply ? m_currentReply->errorString() : "Unknown error";
    qWarning() << "ModelDownloader: Download error:" << errorString;
    emit downloadError(m_currentModelName, errorString);

    // Don't cancel completely on error - allow resume
    m_speedTimer->stop();

    if (m_outputFile) {
        m_outputFile->flush();
        m_outputFile->close();
        delete m_outputFile;
        m_outputFile = nullptr;
    }

    if (m_currentReply) {
        m_currentReply->deleteLater();
        m_currentReply = nullptr;
    }

    m_isPaused = true;
    m_downloadSpeed = 0.0f;
    emit downloadSpeedChanged();
}

void ModelDownloader::onSpeedTimerTimeout()
{
    qint64 bytesThisSecond = m_bytesReceived - m_lastBytesReceived;
    m_downloadSpeed = static_cast<float>(bytesThisSecond);
    m_lastBytesReceived = m_bytesReceived;
    emit downloadSpeedChanged();
}
