#include <gtest/gtest.h>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

namespace {

QString findAppBinary()
{
    // The test binary is in build_local/bin/; the app binary is nearby.
    QString testBinDir = QCoreApplication::applicationDirPath();

#ifdef Q_OS_MACOS
    // On macOS the app is a .app bundle:
    //   build_local/bin/QtMeshEditor.app/Contents/MacOS/QtMeshEditor
    QString macPath = testBinDir + "/QtMeshEditor.app/Contents/MacOS/QtMeshEditor";
    if (QFile::exists(macPath))
        return macPath;
#endif

    // Linux / Windows: binary is directly in the bin directory
    QString directPath = testBinDir + "/QtMeshEditor";
#ifdef Q_OS_WIN
    directPath += ".exe";
#endif
    if (QFile::exists(directPath))
        return directPath;

    return {};
}

QString testDataDir()
{
    // Test binary is in build_local/bin/. Project root is two levels up.
    QString binDir = QCoreApplication::applicationDirPath();
    QDir dir(binDir);
    dir.cdUp(); // bin -> build_local
    dir.cdUp(); // build_local -> project root
    return dir.absoluteFilePath("media/models");
}

QString tempPath(const QString& filename)
{
    return QDir::tempPath() + "/" + filename;
}

} // anonymous namespace

// --- Argument validation tests (no test data needed) ---

TEST(MergeAnimationsCLI, MissingArgs)
{
    QString binary = findAppBinary();
    if (binary.isEmpty())
        GTEST_SKIP() << "QtMeshEditor binary not found";

    // anim with no file or flags should fail with usage error
    QProcess proc;
    proc.start(binary, {"anim"});
    ASSERT_TRUE(proc.waitForFinished(30000));

    EXPECT_NE(proc.exitCode(), 0);
    QString stderrOutput = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(stderrOutput.contains("Error:")) << "stderr: " << stderrOutput.toStdString();
}

TEST(MergeAnimationsCLI, MissingMergeFiles)
{
    QString binary = findAppBinary();
    if (binary.isEmpty())
        GTEST_SKIP() << "QtMeshEditor binary not found";

    // anim with file but --merge with no files should still proceed to load
    // but fail because no merge files means only 1 entity loaded
    QProcess proc;
    proc.start(binary, {"anim", "somefile.fbx", "--merge"});
    ASSERT_TRUE(proc.waitForFinished(30000));

    EXPECT_NE(proc.exitCode(), 0);
}

TEST(MergeAnimationsCLI, MissingAction)
{
    QString binary = findAppBinary();
    if (binary.isEmpty())
        GTEST_SKIP() << "QtMeshEditor binary not found";

    // anim with file but no --list/--rename/--merge should fail with usage
    QProcess proc;
    proc.start(binary, {"anim", "somefile.fbx"});
    ASSERT_TRUE(proc.waitForFinished(30000));

    EXPECT_NE(proc.exitCode(), 0);
    QString stderrOutput = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(stderrOutput.contains("--merge")) << "stderr: " << stderrOutput.toStdString();
}

TEST(MergeAnimationsCLI, NonExistentBaseFile)
{
    QString binary = findAppBinary();
    if (binary.isEmpty())
        GTEST_SKIP() << "QtMeshEditor binary not found";

    QProcess proc;
    proc.start(binary, {"anim", tempPath("nonexistent_file_12345.fbx"),
                         "--merge", "other.fbx",
                         "-o", tempPath("merge_test_out.mesh")});
    ASSERT_TRUE(proc.waitForFinished(30000));

    EXPECT_NE(proc.exitCode(), 0);
}

// --- Tests that require test data files ---

TEST(MergeAnimationsCLI, SingleFileNoAnimations)
{
    QString binary = findAppBinary();
    if (binary.isEmpty())
        GTEST_SKIP() << "QtMeshEditor binary not found";

    QString dataDir = testDataDir();
    QString baseFile = dataDir + "/Twist Dance.fbx";
    if (!QFile::exists(baseFile))
        GTEST_SKIP() << "Test data not found: " << baseFile.toStdString();

    QString outputFile = tempPath("merge_test_single.mesh");

    QProcess proc;
    proc.start(binary, {"anim", baseFile, "--merge",
                         "-o", outputFile});
    ASSERT_TRUE(proc.waitForFinished(60000));

    EXPECT_EQ(proc.exitCode(), 1);
    QString stderrOutput = QString::fromUtf8(proc.readAllStandardError());
    EXPECT_TRUE(stderrOutput.contains("Need at least 2"))
        << "stderr: " << stderrOutput.toStdString();

    // Clean up in case a file was created
    QFile::remove(outputFile);
}

TEST(MergeAnimationsCLI, SuccessfulMerge)
{
    QString binary = findAppBinary();
    if (binary.isEmpty())
        GTEST_SKIP() << "QtMeshEditor binary not found";

    QString dataDir = testDataDir();
    QString baseFile = dataDir + "/Twist Dance.fbx";
    QString animFile = dataDir + "/Hip Hop Dancing.fbx";
    QString outputFile = tempPath("merge_test_cli_output.mesh");
    QString materialFile = tempPath("merge_test_cli_output.material");

    if (!QFile::exists(baseFile))
        GTEST_SKIP() << "Test data not found: " << baseFile.toStdString();
    if (!QFile::exists(animFile))
        GTEST_SKIP() << "Test data not found: " << animFile.toStdString();

    // Remove any leftover output from previous runs
    QFile::remove(outputFile);
    QFile::remove(materialFile);

    QProcess proc;
    proc.start(binary, {"anim", baseFile, "--merge", animFile,
                         "-o", outputFile});
    ASSERT_TRUE(proc.waitForFinished(120000)) << "Process timed out";

    QString stderrOutput = QString::fromUtf8(proc.readAllStandardError());
    QString stdoutOutput = QString::fromUtf8(proc.readAllStandardOutput());

    EXPECT_EQ(proc.exitCode(), 0)
        << "stderr: " << stderrOutput.toStdString()
        << "\nstdout: " << stdoutOutput.toStdString();

    // Verify output file was created and is non-empty
    QFileInfo outputInfo(outputFile);
    EXPECT_TRUE(outputInfo.exists()) << "Output file was not created";
    EXPECT_GT(outputInfo.size(), 0) << "Output file is empty";

    // Clean up
    QFile::remove(outputFile);
    QFile::remove(materialFile);
}
