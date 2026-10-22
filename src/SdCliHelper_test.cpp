#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "SdCliHelper.h"

namespace {
SdCliHelper::Request baseRequest()
{
    SdCliHelper::Request r;
    r.diffusion  = "/m/flux3-image-klein-Q4_0.gguf";
    r.llm        = "/m/Qwen3VL-4B-Instruct-Q4_K_M.gguf";
    r.vae        = "/m/flux2-vae.safetensors";
    r.prompt     = "a wooden chair";
    r.outputPath = "/tmp/out.png";
    return r;
}
int indexOf(const QStringList& l, const QString& v) { return l.indexOf(v); }
} // namespace

TEST(SdCliHelperTest, ArgumentsCarryTheFluxComponentSetAndDistilledSampling)
{
    const QStringList a = SdCliHelper::buildArguments(baseRequest());
    const int d = indexOf(a, "--diffusion-model");
    ASSERT_GE(d, 0);
    EXPECT_EQ(a[d + 1], "/m/flux3-image-klein-Q4_0.gguf");
    EXPECT_EQ(a[indexOf(a, "--llm") + 1], "/m/Qwen3VL-4B-Instruct-Q4_K_M.gguf");
    EXPECT_EQ(a[indexOf(a, "--vae") + 1], "/m/flux2-vae.safetensors");
    EXPECT_EQ(a[indexOf(a, "-p") + 1], "a wooden chair");
    // A distilled flow model: cfg 1.0 and euler, never the SD-style cfg 7.
    EXPECT_EQ(a[indexOf(a, "--cfg-scale") + 1], "1.00");
    EXPECT_EQ(a[indexOf(a, "--sampling-method") + 1], "euler");
    EXPECT_EQ(a[indexOf(a, "--steps") + 1], "16");
    EXPECT_EQ(a[indexOf(a, "-W") + 1], "1024");
    EXPECT_EQ(a[indexOf(a, "-H") + 1], "1024");
    EXPECT_EQ(a[indexOf(a, "-o") + 1], "/tmp/out.png");
}

TEST(SdCliHelperTest, OptionalArgumentsAreOmittedWhenUnset)
{
    const QStringList a = SdCliHelper::buildArguments(baseRequest());
    EXPECT_EQ(indexOf(a, "-s"), -1);   // random seed
    EXPECT_EQ(indexOf(a, "-t"), -1);   // helper's default threads
    EXPECT_EQ(indexOf(a, "-r"), -1);   // no reference image
}

TEST(SdCliHelperTest, SeedThreadsAndReferenceAreForwarded)
{
    SdCliHelper::Request r = baseRequest();
    r.seed = 42;
    r.threads = 6;
    r.refImagePath = "/tmp/ref.png";
    r.steps = 20;
    const QStringList a = SdCliHelper::buildArguments(r);
    EXPECT_EQ(a[indexOf(a, "-s") + 1], "42");
    EXPECT_EQ(a[indexOf(a, "-t") + 1], "6");
    EXPECT_EQ(a[indexOf(a, "-r") + 1], "/tmp/ref.png");
    EXPECT_EQ(a[indexOf(a, "--steps") + 1], "20");
}

TEST(SdCliHelperTest, PromptWithSpacesAndQuotesStaysOneArgument)
{
    SdCliHelper::Request r = baseRequest();
    r.prompt = "a \"red\" chair; rm -rf /";
    const QStringList a = SdCliHelper::buildArguments(r);
    EXPECT_EQ(a[indexOf(a, "-p") + 1], r.prompt);   // argv, never a shell string
}

TEST(SdCliHelperTest, ParsesSamplingProgress)
{
    int step = 0, total = 0;
    EXPECT_TRUE(SdCliHelper::parseProgress("  |=========>      | 12/16 - 1.23s/it", &step, &total));
    EXPECT_EQ(step, 12);
    EXPECT_EQ(total, 16);
}

TEST(SdCliHelperTest, IgnoresNonProgressLines)
{
    int step = -1, total = -1;
    EXPECT_FALSE(SdCliHelper::parseProgress("[I] loading tensors completed, taking 0.41s", &step, &total));
    // A date or a fraction in prose must not look like a progress bar.
    EXPECT_FALSE(SdCliHelper::parseProgress("size 3/4 of the image", &step, &total));
    EXPECT_EQ(step, -1);
    EXPECT_EQ(total, -1);
}

TEST(SdCliHelperTest, RunReportsAMissingHelperClearly)
{
    std::atomic<bool> stop{false};
    const SdCliHelper::Result r = SdCliHelper::run(baseRequest(), "/no/such/helper", nullptr, stop);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.cancelled);
    EXPECT_TRUE(r.error.contains("qtmesh_sdcli"));
}

#ifndef Q_OS_WIN
// Drives the REAL process path with a stand-in "sd-cli": a shell script that
// draws a progress bar and writes the file named after -o.
namespace {
QString writeFakeHelper(const QTemporaryDir& dir, const QString& body)
{
    const QString path = dir.filePath("fake_sdcli.sh");
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(("#!/bin/sh\n" + body).toUtf8());
    f.close();
    QFile::setPermissions(path, QFile::permissions(path) | QFile::ExeOwner | QFile::ExeUser);
    return path;
}
} // namespace

TEST(SdCliHelperTest, RunSucceedsAndForwardsOnlySamplingProgress)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // tensor-loading bar (140) first, then the 16-step sampling bar.
    const QString helper = writeFakeHelper(dir,
        "while [ \"$1\" != \"-o\" ]; do shift; done; out=\"$2\"\n"
        "printf '  |###| 132/140 - 400MB/s\\r  |###| 140/140 - 400MB/s\\n'\n"
        "printf '  |==>| 1/16 - 1.0s/it\\r  |==>| 8/16 - 1.0s/it\\r  |==>| 16/16 - 1.0s/it\\n'\n"
        "echo png > \"$out\"\n");

    SdCliHelper::Request r = baseRequest();
    r.outputPath = dir.filePath("out.png");
    std::atomic<bool> stop{false};
    QList<QPair<int, int>> seen;
    const SdCliHelper::Result res = SdCliHelper::run(r, helper,
        [&](int s, int t) { seen.append({s, t}); }, stop);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    ASSERT_FALSE(seen.isEmpty());
    for (const auto& p : seen)
        EXPECT_EQ(p.second, 16) << "the 140-tensor loading bar leaked into sampling progress";
    EXPECT_EQ(seen.last().first, 16);
}

TEST(SdCliHelperTest, RunSurfacesTheHelpersOwnErrorOnFailure)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString helper = writeFakeHelper(dir,
        "echo 'GGML_ASSERT something broke' >&2\nexit 134\n");
    SdCliHelper::Request r = baseRequest();
    r.outputPath = dir.filePath("out.png");
    std::atomic<bool> stop{false};
    const SdCliHelper::Result res = SdCliHelper::run(r, helper, nullptr, stop);
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.error.contains("134"));
    EXPECT_TRUE(res.error.contains("GGML_ASSERT something broke"));
}

TEST(SdCliHelperTest, ProgressBarsNeverLeakIntoTheErrorText)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    // An EARLY bar (no |==== / |### marker would have filtered it before) and
    // then a crash: the user-facing error must carry only the helper's words.
    const QString helper = writeFakeHelper(dir,
        "printf '  |==>| 1/16 - 1.0s/it\\r'\n"
        "echo 'GGML_ASSERT boom' >&2\n"
        "exit 3\n");
    SdCliHelper::Request r = baseRequest();
    r.outputPath = dir.filePath("out.png");
    std::atomic<bool> stop{false};
    // No progress callback at all: parsing must not depend on one being set.
    const SdCliHelper::Result res = SdCliHelper::run(r, helper, nullptr, stop);
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.error.contains("GGML_ASSERT boom"));
    EXPECT_FALSE(res.error.contains("1/16")) << res.error.toStdString();
}

TEST(SdCliHelperTest, ExitZeroWithoutAnImageIsAnError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString helper = writeFakeHelper(dir, "exit 0\n");
    SdCliHelper::Request r = baseRequest();
    r.outputPath = dir.filePath("never_written.png");
    std::atomic<bool> stop{false};
    const SdCliHelper::Result res = SdCliHelper::run(r, helper, nullptr, stop);
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.error.contains("no image"));
}

TEST(SdCliHelperTest, StaleOutputFromAnEarlierRunIsNotAccepted)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    SdCliHelper::Request r = baseRequest();
    r.outputPath = dir.filePath("out.png");
    QFile stale(r.outputPath);
    ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
    stale.write("old");
    stale.close();
    const QString helper = writeFakeHelper(dir, "exit 0\n");   // writes nothing
    std::atomic<bool> stop{false};
    EXPECT_FALSE(SdCliHelper::run(r, helper, nullptr, stop).ok);
}

TEST(SdCliHelperTest, StopKillsTheProcess)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString helper = writeFakeHelper(dir, "sleep 30\n");
    SdCliHelper::Request r = baseRequest();
    r.outputPath = dir.filePath("out.png");
    std::atomic<bool> stop{true};   // already requested
    const SdCliHelper::Result res = SdCliHelper::run(r, helper, nullptr, stop);
    EXPECT_FALSE(res.ok);
    EXPECT_TRUE(res.cancelled);
}
#endif
