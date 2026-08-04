#ifndef VIDEOFRAMESOURCE_H
#define VIDEOFRAMESOURCE_H

// Performance capture frame sources (epic #869, Slice B #871).
//
// One abstraction delivers timestamped RGB888 frames from (a) a video file,
// (b) a live camera, (c) an image sequence (the headless test double / the
// CLI --frames-dir debug path) — so the predictors and controllers never
// touch Qt Multimedia directly. Everything in this header is compiled only
// under ENABLE_MOCAP (src/CMakeLists.txt adds the .cpp behind the flag).

#ifdef ENABLE_MOCAP

#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QStringList>

#include <memory>
#include <mutex>

class QCamera;
class QElapsedTimer;
class QMediaCaptureSession;
class QMediaPlayer;
class QVideoFrame;
class QVideoSink;

// One decoded frame. image is guaranteed Format_RGB888.
struct MocapFrame {
    QImage image;
    double timeSec = 0.0;   // media timestamp (file/sequence) or wall-clock since start (camera)
    qint64 frameIndex = 0;  // source frame counter (pre-decimation)
};
Q_DECLARE_METATYPE(MocapFrame)

// Drops frames so a source delivering at native fps emits ~targetFps.
// Pure data — unit-tested headless. targetFps <= 0 disables decimation.
class FrameDecimator {
public:
    explicit FrameDecimator(double targetFps = 0.0) : m_targetFps(targetFps) {}

    // Called with each frame's timestamp (monotonically increasing); returns
    // true when the frame should be emitted. The first frame always passes.
    // Tolerates timestamps landing half a source-frame early so 60 -> 30 fps
    // emits exactly every other frame instead of every third.
    bool shouldEmit(double timeSec)
    {
        if (m_targetFps <= 0.0)
            return true;
        const double interval = 1.0 / m_targetFps;
        if (m_hasEmitted && timeSec - m_lastEmitted < interval * 0.75)
            return false;
        m_hasEmitted = true;
        m_lastEmitted = timeSec;
        return true;
    }

    void reset() { m_hasEmitted = false; m_lastEmitted = 0.0; }

private:
    double m_targetFps;
    double m_lastEmitted = 0.0;
    bool m_hasEmitted = false;
};

// Latest-wins single-slot mailbox between the capture thread and a (slower)
// inference consumer: a new frame REPLACES any undelivered pending frame, so
// live inference never falls behind the camera. Thread-safe; unit-tested.
class FrameMailbox {
public:
    void put(const MocapFrame& frame)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_hasPending)
            ++m_dropped;
        m_pending = frame;
        m_hasPending = true;
    }

    // Takes the newest pending frame, if any. Returns false when empty.
    bool take(MocapFrame* out)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hasPending)
            return false;
        *out = m_pending;
        m_pending = MocapFrame{};  // release the QImage
        m_hasPending = false;
        return true;
    }

    // Frames overwritten before a consumer took them (diagnostics/HUD).
    qint64 droppedCount() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_dropped;
    }

private:
    mutable std::mutex m_mutex;
    MocapFrame m_pending;
    bool m_hasPending = false;
    qint64 m_dropped = 0;
};

// Guarantees Format_RGB888 (the input contract of every mocap predictor).
QImage mocapFrameToRgb888(const QImage& image);
// Decode a QVideoFrame to RGB888 (maps the buffer when toImage() alone fails).
QImage mocapFrameFromVideoFrame(const QVideoFrame& frame);

class VideoFrameSource : public QObject {
    Q_OBJECT
public:
    explicit VideoFrameSource(QObject* parent = nullptr) : QObject(parent) {}
    ~VideoFrameSource() override = default;

    // Prepare the source. Returns false and fills *error on failure.
    virtual bool open(QString* error) = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual bool isLive() const = 0;
    virtual double nativeFps() const = 0;  // 0 if unknown

    // Latest-wins mailbox for the (slower) inference consumer running on a
    // worker thread. Live camera AND file playback both feed it — the live
    // GUI preview drops intermediate frames either way (real-time). Sources
    // that emit frames write via emitFrame() below, which both signals
    // frameReady() and posts to this mailbox.
    FrameMailbox& mailbox() { return m_mailbox; }

signals:
    void frameReady(const MocapFrame& frame);
    void finished();                      // file/sequence sources: end of media
    void errorOccurred(const QString& message);

protected:
    // Emit a frame to BOTH the frameReady() signal (synchronous consumers /
    // tests) and the mailbox (worker-thread live preview).
    void emitFrame(const MocapFrame& frame)
    {
        m_mailbox.put(frame);
        emit frameReady(frame);
    }

    FrameMailbox m_mailbox;
};

// (c) Image sequence — synchronous test double. Emits every image on start()
// with timestamps i/fps, honouring targetFps decimation, then finished().
class ImageSequenceFrameSource : public VideoFrameSource {
    Q_OBJECT
public:
    ImageSequenceFrameSource(const QStringList& imagePaths, double fps,
                             double targetFps = 0.0, QObject* parent = nullptr);

    bool open(QString* error) override;
    void start() override;
    void stop() override;
    bool isLive() const override { return false; }
    double nativeFps() const override { return m_fps; }

private:
    QStringList m_paths;
    double m_fps;
    FrameDecimator m_decimator;
    bool m_stopped = false;
};

// (a) Video file — QMediaPlayer + QVideoSink. Playback-driven (real-time;
// faster-than-realtime decode is a known follow-up, QVideoSink is fed by the
// player clock). targetFps decimates delivery for offline capture.
class FileFrameSource : public VideoFrameSource {
    Q_OBJECT
public:
    explicit FileFrameSource(const QString& filePath, double targetFps = 0.0,
                             QObject* parent = nullptr);
    ~FileFrameSource() override;

    bool open(QString* error) override;
    void start() override;
    void stop() override;
    bool isLive() const override { return false; }
    double nativeFps() const override { return m_nativeFps; }

private:
    void handleVideoFrame(const QVideoFrame& frame);

    QString m_path;
    FrameDecimator m_decimator;
    std::unique_ptr<QMediaPlayer> m_player;
    std::unique_ptr<QVideoSink> m_sink;
    double m_nativeFps = 0.0;
    qint64 m_frameIndex = 0;
    bool m_finishedEmitted = false;
};

// (b) Live camera — QCamera + QMediaCaptureSession + QVideoSink. Frames are
// emitted as frameReady AND written into mailbox() (latest-wins) for a
// slower inference consumer. deviceId empty = default camera.
class CameraFrameSource : public VideoFrameSource {
    Q_OBJECT
public:
    struct DeviceInfo {
        QString id;
        QString description;
    };
    // Enumerates video inputs (feeds the GUI picker + MCP list_capture_devices).
    static QList<DeviceInfo> availableDevices();

    explicit CameraFrameSource(const QString& deviceId = {},
                               QObject* parent = nullptr);
    ~CameraFrameSource() override;

    bool open(QString* error) override;
    void start() override;
    void stop() override;
    bool isLive() const override { return true; }
    double nativeFps() const override { return m_nativeFps; }

private:
    void handleVideoFrame(const QVideoFrame& frame);

    QString m_deviceId;
    std::unique_ptr<QCamera> m_camera;
    std::unique_ptr<QMediaCaptureSession> m_session;
    std::unique_ptr<QVideoSink> m_sink;
    std::unique_ptr<QElapsedTimer> m_clock;
    double m_nativeFps = 0.0;
    qint64 m_frameIndex = 0;
};

#endif // ENABLE_MOCAP
#endif // VIDEOFRAMESOURCE_H
