#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

#define private public
#include "ModelDownloader.h"
#undef private
#include "ModelFetch.h"

// ModelFetch drives the REAL ModelDownloader singleton through QNAM's file://
// backend, so these are end-to-end for everything except a remote host. The
// two properties that matter most are (1) the downloader's error TEXT survives
// to the caller and (2) a synchronous rejection returns at once instead of
// hanging until the timeout — the race only one of twenty consumers guarded.

class ModelFetchTest : public ::testing::Test {
protected:
    QTemporaryDir dir;
    ModelDownloader* dl = nullptr;

    void SetUp() override {
        dl = ModelDownloader::instance();
        ASSERT_NE(dl, nullptr);
        dl->cancelDownload();
        QCoreApplication::processEvents();
        dl->m_isDownloading = false;
        dl->m_expectedSha256.clear();
    }
    void TearDown() override {
        dl->cancelDownload();
        QCoreApplication::processEvents();
        dl->m_isDownloading = false;
    }
    QString p(const QString& n) const { return dir.path() + "/" + n; }
    QString writeFile(const QString& n, const QByteArray& bytes) {
        QFile f(p(n)); EXPECT_TRUE(f.open(QIODevice::WriteOnly)); f.write(bytes); f.close();
        return p(n);
    }
};

TEST_F(ModelFetchTest, ExistingFileIsOkWithoutTouchingTheDownloader)
{
    const QString dst = writeFile("present.onnx", "already here");
    QSignalSpy started(dl, &ModelDownloader::downloadStarted);

    ModelFetch::Request r; r.destination = dst; r.url = "https://example.invalid/x"; r.label = "P";
    const auto out = ModelFetch::ensureBlocking(r);

    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.path, dst);
    EXPECT_TRUE(out.error.isEmpty());
    EXPECT_EQ(started.count(), 0) << "must not start a download for a file that exists";
}

TEST_F(ModelFetchTest, DownloadsFromFileUrlEndToEnd)
{
    const QString src = writeFile("mirror.onnx", "MODEL-BYTES");
    const QString dst = p("dl/fetched.onnx");

    ModelFetch::Request r;
    r.url = QUrl::fromLocalFile(src).toString();
    r.destination = dst; r.label = "E2E"; r.timeoutMs = 10000;
    const auto out = ModelFetch::ensureBlocking(r);

    ASSERT_TRUE(out.ok) << out.error.toStdString();
    QFile f(dst); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("MODEL-BYTES"));
}

TEST_F(ModelFetchTest, DownloaderErrorTextReachesTheCaller)
{
    // #1029's https gate emits a precise reason; eighteen consumers threw it
    // away. The helper must hand it back verbatim.
    ModelFetch::Request r;
    r.url = "http://example.invalid/m.onnx";      // refused: plain http
    r.destination = p("refused.onnx"); r.label = "Refused"; r.timeoutMs = 10000;
    QElapsedTimer t; t.start();
    const auto out = ModelFetch::ensureBlocking(r);

    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.timedOut) << "a refusal must not be reported as a timeout";
    EXPECT_TRUE(out.error.contains("https://")) << out.error.toStdString();
    EXPECT_LT(t.elapsed(), 5000) << "queued refusal must arrive well inside the timeout";
    EXPECT_FALSE(QFileInfo::exists(p("refused.onnx")));
}

TEST_F(ModelFetchTest, MissingFileUrlYieldsTheNetworkErrorNotATimeout)
{
    ModelFetch::Request r;
    r.url = QUrl::fromLocalFile(p("does-not-exist.onnx")).toString();
    r.destination = p("missing.onnx"); r.label = "Missing"; r.timeoutMs = 10000;
    const auto out = ModelFetch::ensureBlocking(r);

    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.timedOut);
    EXPECT_FALSE(out.error.isEmpty()) << "a failed download must say why";
}

TEST_F(ModelFetchTest, SynchronousRejectionReturnsImmediatelyInsteadOfHanging)
{
    // THE race: startDownload emits downloadError synchronously when another
    // download is active. A handler's loop.quit() then fires BEFORE exec() and
    // is lost; without the `settled` guard the caller sits in exec() until the
    // timeout. With a 2 s timeout that is the difference between ~0 ms and
    // 2000 ms — and the error text must be the downloader's, not "timed out".
    dl->m_isDownloading = true;                    // someone else owns it
    ModelFetch::Request r;
    r.url = "https://example.invalid/m.onnx";
    r.destination = p("busy.onnx"); r.label = "Busy"; r.timeoutMs = 2000;
    QElapsedTimer t; t.start();
    const auto out = ModelFetch::ensureBlocking(r);
    const qint64 ms = t.elapsed();
    dl->m_isDownloading = false;

    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.timedOut) << "must not wait out the timeout on a synchronous rejection";
    EXPECT_LT(ms, 1000) << "hung for " << ms << " ms — the settled guard is not working";
    EXPECT_TRUE(out.error.contains("already in progress")) << out.error.toStdString();
}

// --- #1025: an existing file must match its published digest to count as present ---

namespace {
QString shaHex(const QByteArray& bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace

TEST_F(ModelFetchTest, ExistingFileWithMatchingDigestIsOkWithoutDownload)
{
    const QString dst = writeFile("good.onnx", "MODEL-BYTES");
    QSignalSpy started(dl, &ModelDownloader::downloadStarted);
    ModelFetch::Request r; r.destination = dst; r.url = "https://example.invalid/x"; r.label = "G";
    r.expectedSha256 = shaHex("MODEL-BYTES").toUpper();   // case-insensitive
    const auto out = ModelFetch::ensureBlocking(r);
    EXPECT_TRUE(out.ok) << out.error.toStdString();
    EXPECT_FALSE(out.replacedCorrupt);
    EXPECT_EQ(started.count(), 0);
}

TEST_F(ModelFetchTest, ExistingCorruptFileIsDeletedAndRefetched)
{
    // The #1025 shape: a same-size file whose bytes are not the published ones.
    // "Exists" must not mean "usable" — delete it and fetch the real one.
    const QString src = writeFile("mirror.onnx", "MODEL-BYTES");
    const QString dst = writeFile("corrupt.onnx", "GARBAGE-BYT");   // same length, wrong content
    ModelFetch::Request r;
    r.url = QUrl::fromLocalFile(src).toString(); r.destination = dst; r.label = "C"; r.timeoutMs = 10000;
    r.expectedSha256 = shaHex("MODEL-BYTES");
    const auto out = ModelFetch::ensureBlocking(r);
    ASSERT_TRUE(out.ok) << out.error.toStdString();
    EXPECT_TRUE(out.replacedCorrupt);
    QFile f(dst); ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), QByteArray("MODEL-BYTES")) << "the corrupt copy must be replaced by the published bytes";
}

TEST_F(ModelFetchTest, ExistingCorruptFileWithoutUrlIsDeletedAndReported)
{
    const QString dst = writeFile("orphan.onnx", "GARBAGE");
    ModelFetch::Request r; r.destination = dst; r.label = "O";
    r.expectedSha256 = shaHex("MODEL-BYTES");
    const auto out = ModelFetch::ensureBlocking(r);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(out.replacedCorrupt);
    EXPECT_TRUE(out.error.contains("SHA-256 mismatch")) << out.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(dst)) << "a known-corrupt model must never be left to be loaded";
}

TEST_F(ModelFetchTest, VerifyOnlyRequestNeverTouchesTheDownloader)
{
    // The offline (*_NO_DOWNLOAD) shape: URL empty, digest known. A matching
    // cached file is ok — and the downloader singleton is never involved, so a
    // worker thread can run this without giving the singleton thread affinity
    // (review on #1025). A mismatch deletes the file and fails without network.
    const QString dst = writeFile("offline.onnx", "MODEL-BYTES");
    QSignalSpy started(dl, &ModelDownloader::downloadStarted);
    ModelFetch::Request r; r.destination = dst; r.label = "V";
    r.expectedSha256 = shaHex("MODEL-BYTES");
    const auto ok = ModelFetch::ensureBlocking(r);
    EXPECT_TRUE(ok.ok) << ok.error.toStdString();
    EXPECT_EQ(started.count(), 0);

    const QString bad = writeFile("offline_bad.onnx", "GARBAGE-BYT");
    r.destination = bad;
    const auto out = ModelFetch::ensureBlocking(r);
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(out.replacedCorrupt);
    EXPECT_FALSE(QFileInfo::exists(bad));
    EXPECT_EQ(started.count(), 0) << "no URL: never a download attempt";
}

TEST_F(ModelFetchTest, EmptyInputsFailFast)
{
    ModelFetch::Request r; r.label = "X";
    EXPECT_FALSE(ModelFetch::ensureBlocking(r).ok);          // no destination
    r.destination = p("nodest.onnx");
    const auto out = ModelFetch::ensureBlocking(r);          // no url
    EXPECT_FALSE(out.ok);
    EXPECT_TRUE(out.error.contains("URL")) << out.error.toStdString();
}
