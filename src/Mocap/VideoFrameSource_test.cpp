#ifdef ENABLE_MOCAP

#include <gtest/gtest.h>

#include <QDir>
#include <QImage>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <thread>
#include <vector>

#include "Mocap/VideoFrameSource.h"

namespace {

QStringList writeTestImages(const QString& dir, int count, int size = 8)
{
    QStringList paths;
    for (int i = 0; i < count; ++i) {
        QImage img(size, size, QImage::Format_ARGB32);
        img.fill(QColor(i * 10 % 255, 0, 0));
        const QString p = dir + QStringLiteral("/frame_%1.png").arg(i, 3, 10, QChar('0'));
        img.save(p);
        paths << p;
    }
    return paths;
}

}  // namespace

TEST(FrameDecimator, PassesEverythingWhenDisabled)
{
    FrameDecimator d(0.0);
    for (int i = 0; i < 10; ++i)
        EXPECT_TRUE(d.shouldEmit(i / 60.0));
}

TEST(FrameDecimator, HalvesSixtyToThirty)
{
    FrameDecimator d(30.0);
    int emitted = 0;
    for (int i = 0; i < 60; ++i)
        if (d.shouldEmit(i / 60.0))
            ++emitted;
    EXPECT_GE(emitted, 28);
    EXPECT_LE(emitted, 32);
}

TEST(FrameDecimator, FirstFrameAlwaysPasses)
{
    FrameDecimator d(1.0);
    EXPECT_TRUE(d.shouldEmit(0.0));
    EXPECT_FALSE(d.shouldEmit(0.1));
    d.reset();
    EXPECT_TRUE(d.shouldEmit(0.1));
}

TEST(FrameMailbox, LatestWinsDropsIntermediates)
{
    FrameMailbox box;
    for (int i = 0; i < 3; ++i) {
        MocapFrame f;
        f.frameIndex = i;
        box.put(f);
    }
    MocapFrame out;
    ASSERT_TRUE(box.take(&out));
    EXPECT_EQ(out.frameIndex, 2);       // only the newest survives
    EXPECT_FALSE(box.take(&out));       // and only once
    EXPECT_EQ(box.droppedCount(), 2);   // the two overwritten frames
}

TEST(FrameMailbox, ThreadSafePutTake)
{
    FrameMailbox box;
    std::thread producer([&box] {
        for (int i = 0; i < 1000; ++i) {
            MocapFrame f;
            f.frameIndex = i;
            box.put(f);
        }
    });
    qint64 last = -1;
    for (int i = 0; i < 2000; ++i) {
        MocapFrame out;
        if (box.take(&out)) {
            EXPECT_GT(out.frameIndex, last);  // monotone: never re-deliver older
            last = out.frameIndex;
        }
    }
    producer.join();
    MocapFrame out;
    while (box.take(&out))
        last = out.frameIndex;
    EXPECT_EQ(last, 999);  // the final frame is never lost
}

TEST(ImageSequenceFrameSource, EmitsAllFramesInOrderWithTimestamps)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QStringList paths = writeTestImages(tmp.path(), 5);

    ImageSequenceFrameSource src(paths, 10.0);
    QString error;
    ASSERT_TRUE(src.open(&error)) << error.toStdString();

    std::vector<MocapFrame> frames;
    bool done = false;
    QObject::connect(&src, &VideoFrameSource::frameReady,
                     [&frames](const MocapFrame& f) { frames.push_back(f); });
    QObject::connect(&src, &VideoFrameSource::finished, [&done] { done = true; });
    src.start();

    ASSERT_TRUE(done);
    ASSERT_EQ(frames.size(), 5u);
    for (size_t i = 0; i < frames.size(); ++i) {
        EXPECT_EQ(frames[i].frameIndex, static_cast<qint64>(i));
        EXPECT_DOUBLE_EQ(frames[i].timeSec, i / 10.0);
        EXPECT_EQ(frames[i].image.format(), QImage::Format_RGB888);
        EXPECT_FALSE(frames[i].image.isNull());
    }
}

TEST(ImageSequenceFrameSource, DecimatesToTargetFps)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QStringList paths = writeTestImages(tmp.path(), 60);

    ImageSequenceFrameSource src(paths, 60.0, /*targetFps=*/30.0);
    QString error;
    ASSERT_TRUE(src.open(&error)) << error.toStdString();

    int emitted = 0;
    QObject::connect(&src, &VideoFrameSource::frameReady,
                     [&emitted](const MocapFrame&) { ++emitted; });
    src.start();
    EXPECT_GE(emitted, 28);
    EXPECT_LE(emitted, 32);
}

TEST(ImageSequenceFrameSource, OpenFailsOnMissingFile)
{
    ImageSequenceFrameSource src({QStringLiteral("/nonexistent/frame.png")}, 30.0);
    QString error;
    EXPECT_FALSE(src.open(&error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(ImageSequenceFrameSource, OpenFailsOnEmptyList)
{
    ImageSequenceFrameSource src({}, 30.0);
    QString error;
    EXPECT_FALSE(src.open(&error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(FileFrameSource, OpenFailsOnMissingFile)
{
    FileFrameSource src(QStringLiteral("/nonexistent/video.mp4"));
    QString error;
    EXPECT_FALSE(src.open(&error));
    EXPECT_FALSE(error.isEmpty());
}

// Real camera tests are impossible in CI; enumeration must not crash headless.
TEST(CameraFrameSource, AvailableDevicesDoesNotCrash)
{
    const auto devices = CameraFrameSource::availableDevices();
    for (const auto& d : devices) {
        EXPECT_FALSE(d.id.isEmpty());
    }
}

TEST(CameraFrameSource, OpenFailsOnBogusDeviceId)
{
    if (!qEnvironmentVariableIsSet("QTMESH_MOCAP_CAMERA_TESTS")
        && CameraFrameSource::availableDevices().isEmpty()) {
        // headless CI: also exercises the no-camera error path
    }
    CameraFrameSource src(QStringLiteral("definitely-not-a-camera-id"));
    QString error;
    EXPECT_FALSE(src.open(&error));
    EXPECT_FALSE(error.isEmpty());
}

TEST(MocapFrameToRgb888, ConvertsAndPassesThrough)
{
    QImage argb(4, 4, QImage::Format_ARGB32);
    argb.fill(Qt::green);
    const QImage converted = mocapFrameToRgb888(argb);
    EXPECT_EQ(converted.format(), QImage::Format_RGB888);

    const QImage same = mocapFrameToRgb888(converted);
    EXPECT_EQ(same.format(), QImage::Format_RGB888);
}

#endif  // ENABLE_MOCAP
