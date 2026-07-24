#ifdef ENABLE_MOCAP

#include "VideoFrameSource.h"
#include "MocapCameraHints.h"

#include <QCamera>
#include <QCameraDevice>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QMediaMetaData>
#include <QMediaPlayer>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoSink>

namespace {
// Queued frameReady connections (capture thread -> worker) need the metatype.
struct MocapMetaTypeRegistrar {
    MocapMetaTypeRegistrar() { qRegisterMetaType<MocapFrame>("MocapFrame"); }
};
const MocapMetaTypeRegistrar registrar;
}  // namespace

QImage mocapFrameToRgb888(const QImage& image)
{
    if (image.format() == QImage::Format_RGB888)
        return image;
    return image.convertToFormat(QImage::Format_RGB888);
}

// ---------------------------------------------------------------------------
// ImageSequenceFrameSource
// ---------------------------------------------------------------------------

ImageSequenceFrameSource::ImageSequenceFrameSource(const QStringList& imagePaths,
                                                   double fps, double targetFps,
                                                   QObject* parent)
    : VideoFrameSource(parent),
      m_paths(imagePaths),
      m_fps(fps > 0.0 ? fps : 30.0),
      m_decimator(targetFps)
{
}

bool ImageSequenceFrameSource::open(QString* error)
{
    if (m_paths.isEmpty()) {
        if (error) *error = tr("image sequence is empty");
        return false;
    }
    for (const QString& p : m_paths) {
        if (!QFileInfo::exists(p)) {
            if (error) *error = tr("image not found: %1").arg(p);
            return false;
        }
    }
    return true;
}

void ImageSequenceFrameSource::start()
{
    m_stopped = false;
    m_decimator.reset();
    for (qint64 i = 0; i < m_paths.size() && !m_stopped; ++i) {
        const double t = static_cast<double>(i) / m_fps;
        if (!m_decimator.shouldEmit(t))
            continue;
        QImage img(m_paths.at(i));
        if (img.isNull()) {
            emit errorOccurred(tr("failed to load image: %1").arg(m_paths.at(i)));
            return;
        }
        MocapFrame frame;
        frame.image = mocapFrameToRgb888(img);
        frame.timeSec = t;
        frame.frameIndex = i;
        emit frameReady(frame);
    }
    if (!m_stopped)
        emit finished();
}

void ImageSequenceFrameSource::stop()
{
    m_stopped = true;
}

// ---------------------------------------------------------------------------
// FileFrameSource
// ---------------------------------------------------------------------------

FileFrameSource::FileFrameSource(const QString& filePath, double targetFps,
                                 QObject* parent)
    : VideoFrameSource(parent), m_path(filePath), m_decimator(targetFps)
{
}

FileFrameSource::~FileFrameSource()
{
    stop();
    // Tear down in dependency order: the player holds the sink pointer via
    // setVideoSink(), so it must die (or be unbound) BEFORE the sink. Member
    // destruction runs in reverse declaration order (m_sink before m_player),
    // which would leave the player briefly pointing at a freed sink — reset
    // the player first here.
    if (m_player) {
        m_player->setVideoSink(nullptr);
        m_player.reset();
    }
    m_sink.reset();
}

bool FileFrameSource::open(QString* error)
{
    if (!QFileInfo::exists(m_path)) {
        if (error) *error = tr("video file not found: %1").arg(m_path);
        return false;
    }
    m_player = std::make_unique<QMediaPlayer>();
    m_sink = std::make_unique<QVideoSink>();
    m_player->setVideoSink(m_sink.get());

    connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
            &FileFrameSource::handleVideoFrame);
    connect(m_player.get(), &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
                if (status == QMediaPlayer::EndOfMedia && !m_finishedEmitted) {
                    m_finishedEmitted = true;
                    emit finished();
                }
                if (status == QMediaPlayer::LoadedMedia) {
                    const auto rate = m_player->metaData()
                                          .value(QMediaMetaData::VideoFrameRate);
                    if (rate.isValid())
                        m_nativeFps = rate.toDouble();
                }
            });
    connect(m_player.get(), &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error, const QString& message) {
                emit errorOccurred(tr("video decode error: %1").arg(message));
            });

    m_player->setSource(QUrl::fromLocalFile(m_path));
    return true;
}

void FileFrameSource::start()
{
    if (!m_player) {
        emit errorOccurred(tr("start() before open()"));
        return;
    }
    m_decimator.reset();
    m_frameIndex = 0;
    m_finishedEmitted = false;
    m_player->play();
}

void FileFrameSource::stop()
{
    if (m_player)
        m_player->stop();
}

void FileFrameSource::handleVideoFrame()
{
    const QVideoFrame vf = m_sink->videoFrame();
    if (!vf.isValid())
        return;
    const qint64 index = m_frameIndex++;
    // Prefer the frame's own timestamp; fall back to the player clock.
    double t = vf.startTime() >= 0
                   ? vf.startTime() / 1e6
                   : (m_player ? m_player->position() / 1e3 : 0.0);
    if (!m_decimator.shouldEmit(t))
        return;
    MocapFrame frame;
    frame.image = mocapFrameToRgb888(vf.toImage());
    frame.timeSec = t;
    frame.frameIndex = index;
    if (!frame.image.isNull())
        emitFrame(frame);   // -> frameReady() + mailbox (worker-thread preview)
}

// ---------------------------------------------------------------------------
// CameraFrameSource
// ---------------------------------------------------------------------------

QList<CameraFrameSource::DeviceInfo> CameraFrameSource::availableDevices()
{
    QList<DeviceInfo> out;
    const auto devices = QMediaDevices::videoInputs();
    for (const QCameraDevice& d : devices)
        out.append({QString::fromUtf8(d.id()), d.description()});
    return out;
}

CameraFrameSource::CameraFrameSource(const QString& deviceId, QObject* parent)
    : VideoFrameSource(parent), m_deviceId(deviceId)
{
}

CameraFrameSource::~CameraFrameSource()
{
    stop();
    // The capture session references the sink via setVideoSink(); detach and
    // destroy the session (and camera) BEFORE the sink. Member destruction
    // runs in reverse declaration order (m_sink before m_session), so do it
    // explicitly here.
    if (m_session)
        m_session->setVideoSink(nullptr);
    m_camera.reset();
    m_session.reset();
    m_sink.reset();
}

bool CameraFrameSource::open(QString* error)
{
    QCameraDevice device;
    const auto devices = QMediaDevices::videoInputs();
    if (m_deviceId.isEmpty()) {
        device = QMediaDevices::defaultVideoInput();
    } else {
        for (const QCameraDevice& d : devices) {
            if (QString::fromUtf8(d.id()) == m_deviceId) {
                device = d;
                break;
            }
        }
    }
    if (device.isNull()) {
        if (error) {
            if (devices.isEmpty()) {
                *error = tr("no camera available") + MocapCameraHints::snapConnectHint();
            } else {
                *error = tr("camera not found: %1").arg(m_deviceId);
            }
        }
        return false;
    }

    m_camera = std::make_unique<QCamera>(device);
    m_session = std::make_unique<QMediaCaptureSession>();
    m_sink = std::make_unique<QVideoSink>();
    m_session->setCamera(m_camera.get());
    m_session->setVideoSink(m_sink.get());
    m_clock = std::make_unique<QElapsedTimer>();

    const auto formats = device.videoFormats();
    if (!formats.isEmpty())
        m_nativeFps = formats.first().maxFrameRate();

    connect(m_sink.get(), &QVideoSink::videoFrameChanged, this,
            &CameraFrameSource::handleVideoFrame);
    connect(m_camera.get(), &QCamera::errorOccurred, this,
            [this](QCamera::Error err, const QString& message) {
                if (err == QCamera::CameraError && message.contains(
                        QStringLiteral("permission"), Qt::CaseInsensitive)) {
                    emit errorOccurred(
                        tr("camera permission denied — %1")
                            .arg(MocapCameraHints::permissionDeniedMessage()));
                } else {
                    emit errorOccurred(tr("camera error: %1").arg(message));
                }
            });
    return true;
}

void CameraFrameSource::start()
{
    if (!m_camera) {
        emit errorOccurred(tr("start() before open()"));
        return;
    }
    m_frameIndex = 0;
    m_clock->start();
    m_camera->start();
}

void CameraFrameSource::stop()
{
    if (m_camera)
        m_camera->stop();
}

void CameraFrameSource::handleVideoFrame()
{
    const QVideoFrame vf = m_sink->videoFrame();
    if (!vf.isValid())
        return;
    MocapFrame frame;
    frame.image = mocapFrameToRgb888(vf.toImage());
    if (frame.image.isNull())
        return;
    frame.timeSec = m_clock->isValid() ? m_clock->elapsed() / 1e3 : 0.0;
    frame.frameIndex = m_frameIndex++;
    // Latest-wins for the inference consumer; the signal serves lightweight
    // observers (preview HUD) that keep up with the camera.
    emitFrame(frame);
}

#endif  // ENABLE_MOCAP
