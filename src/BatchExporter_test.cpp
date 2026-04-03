#include <gtest/gtest.h>
#include "BatchExporter.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <vector>

class TestBatchExporter : public BatchExporter {
public:
    using BatchExporter::BatchExporter;

    struct Invocation {
        std::vector<std::string> args;
    };

    std::vector<Invocation> invocations;
    std::vector<int> plannedResults;

protected:
    int runCliPipeline(int argc, char* argv[]) override {
        Invocation inv;
        inv.args.reserve(static_cast<size_t>(argc));
        for (int i = 0; i < argc; ++i) {
            inv.args.emplace_back(argv[i] ? argv[i] : "");
        }
        invocations.push_back(std::move(inv));

        if (!plannedResults.empty()) {
            const int result = plannedResults.front();
            plannedResults.erase(plannedResults.begin());
            return result;
        }
        return 0;
    }
};

class BatchExporterTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override {
        if (app) app->processEvents();
    }
};

TEST_F(BatchExporterTests, ConstructDestruct) {
    BatchExporter exporter;
    // Should construct and destruct without crash
}

TEST_F(BatchExporterTests, SetInputFiles) {
    BatchExporter exporter;
    QStringList files = {"file1.fbx", "file2.obj", "file3.dae"};
    EXPECT_NO_THROW(exporter.setInputFiles(files));
}

TEST_F(BatchExporterTests, SetOutputFormat) {
    BatchExporter exporter;
    EXPECT_NO_THROW(exporter.setOutputFormat("gltf2"));
}

TEST_F(BatchExporterTests, SetOutputDirectory) {
    BatchExporter exporter;
    EXPECT_NO_THROW(exporter.setOutputDirectory("/tmp/batch_export"));
}

TEST_F(BatchExporterTests, ExecuteWithEmptyFileList) {
    BatchExporter exporter;
    exporter.setInputFiles(QStringList());
    exporter.setOutputFormat("gltf2");
    exporter.setOutputDirectory("/tmp/batch_export_test");

    QSignalSpy finishedSpy(&exporter, &BatchExporter::finished);

    exporter.execute();

    ASSERT_EQ(finishedSpy.count(), 1);
    // With empty file list, success=0, fail=0
    EXPECT_EQ(finishedSpy.at(0).at(0).toInt(), 0); // successCount
    EXPECT_EQ(finishedSpy.at(0).at(1).toInt(), 0); // failCount
}

TEST_F(BatchExporterTests, SetOutputDirectoryEmpty) {
    // Verify that setting empty output dir doesn't crash
    BatchExporter exporter;
    EXPECT_NO_THROW(exporter.setOutputDirectory(""));
}

TEST_F(BatchExporterTests, ParentOwnership) {
    QObject parent;
    auto* exporter = new BatchExporter(&parent);
    EXPECT_EQ(exporter->parent(), &parent);
    // parent will delete exporter on destruction
}

TEST_F(BatchExporterTests, SignalConnections) {
    BatchExporter exporter;

    // Verify all signals can be connected to without issue
    QSignalSpy progressSpy(&exporter, &BatchExporter::progressChanged);
    QSignalSpy finishedSpy(&exporter, &BatchExporter::finished);
    QSignalSpy errorSpy(&exporter, &BatchExporter::error);

    EXPECT_TRUE(progressSpy.isValid());
    EXPECT_TRUE(finishedSpy.isValid());
    EXPECT_TRUE(errorSpy.isValid());
}

TEST_F(BatchExporterTests, SetMultipleFormats) {
    BatchExporter exporter;
    EXPECT_NO_THROW(exporter.setOutputFormat("gltf2"));
    EXPECT_NO_THROW(exporter.setOutputFormat("obj"));
    EXPECT_NO_THROW(exporter.setOutputFormat("fbx"));
    EXPECT_NO_THROW(exporter.setOutputFormat("dae"));
}

TEST_F(BatchExporterTests, ExecuteEmptyListWithEmptyDir) {
    BatchExporter exporter;
    exporter.setInputFiles(QStringList());
    exporter.setOutputFormat("obj");
    exporter.setOutputDirectory(""); // empty dir

    QSignalSpy finishedSpy(&exporter, &BatchExporter::finished);
    exporter.execute();

    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_EQ(finishedSpy.at(0).at(0).toInt(), 0); // successCount
    EXPECT_EQ(finishedSpy.at(0).at(1).toInt(), 0); // failCount
}

TEST_F(BatchExporterTests, ExecuteWithTwoFilesUsesConfiguredOutputDirectory) {
    TestBatchExporter exporter;
    exporter.setInputFiles(QStringList{"/tmp/source/model_a.fbx", "/tmp/source/model_b.obj"});
    exporter.setOutputFormat("gltf2");
    exporter.setOutputDirectory("/tmp/out");

    QSignalSpy progressSpy(&exporter, &BatchExporter::progressChanged);
    QSignalSpy finishedSpy(&exporter, &BatchExporter::finished);
    QSignalSpy errorSpy(&exporter, &BatchExporter::error);

    exporter.execute();

    ASSERT_EQ(progressSpy.count(), 2);
    EXPECT_EQ(progressSpy.at(0).at(0).toInt(), 1);
    EXPECT_EQ(progressSpy.at(0).at(1).toInt(), 2);
    EXPECT_EQ(progressSpy.at(1).at(0).toInt(), 2);
    EXPECT_EQ(progressSpy.at(1).at(1).toInt(), 2);

    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_EQ(finishedSpy.at(0).at(0).toInt(), 2);
    EXPECT_EQ(finishedSpy.at(0).at(1).toInt(), 0);
    EXPECT_EQ(errorSpy.count(), 0);

    ASSERT_EQ(exporter.invocations.size(), 2u);
    ASSERT_EQ(exporter.invocations[0].args.size(), 5u);
    EXPECT_EQ(exporter.invocations[0].args[0], "qtmesh");
    EXPECT_EQ(exporter.invocations[0].args[1], "convert");
    EXPECT_EQ(exporter.invocations[0].args[2], "/tmp/source/model_a.fbx");
    EXPECT_EQ(exporter.invocations[0].args[3], "-o");
    EXPECT_EQ(exporter.invocations[0].args[4], "/tmp/out/model_a.gltf2");

    ASSERT_EQ(exporter.invocations[1].args.size(), 5u);
    EXPECT_EQ(exporter.invocations[1].args[2], "/tmp/source/model_b.obj");
    EXPECT_EQ(exporter.invocations[1].args[4], "/tmp/out/model_b.gltf2");
}

TEST_F(BatchExporterTests, ExecuteWithFailureEmitsErrorAndCountsFailures) {
    TestBatchExporter exporter;
    exporter.setInputFiles(QStringList{"/tmp/source/one.fbx", "/tmp/source/two.fbx"});
    exporter.setOutputFormat("mesh");
    exporter.setOutputDirectory("/tmp/out");
    exporter.plannedResults = {0, 1};

    QSignalSpy finishedSpy(&exporter, &BatchExporter::finished);
    QSignalSpy errorSpy(&exporter, &BatchExporter::error);

    exporter.execute();

    ASSERT_EQ(finishedSpy.count(), 1);
    EXPECT_EQ(finishedSpy.at(0).at(0).toInt(), 1);
    EXPECT_EQ(finishedSpy.at(0).at(1).toInt(), 1);

    ASSERT_EQ(errorSpy.count(), 1);
    EXPECT_EQ(errorSpy.at(0).at(0).toString(), QString("/tmp/source/two.fbx"));
    EXPECT_EQ(errorSpy.at(0).at(1).toString(), QString("Conversion failed"));
}

TEST_F(BatchExporterTests, ExecuteWithoutOutputDirectoryUsesInputFileDirectory) {
    TestBatchExporter exporter;
    QTemporaryDir inputDir;
    ASSERT_TRUE(inputDir.isValid());
    const QString inputPath = QDir(inputDir.path()).filePath("asset.dae");
    exporter.setInputFiles(QStringList{inputPath});
    exporter.setOutputFormat("obj");
    exporter.setOutputDirectory("");

    exporter.execute();

    ASSERT_EQ(exporter.invocations.size(), 1u);
    ASSERT_EQ(exporter.invocations[0].args.size(), 5u);
    const QString expectedOutput = QDir(QFileInfo(inputPath).absolutePath()).filePath("asset.obj");
    EXPECT_EQ(QDir::cleanPath(QString::fromStdString(exporter.invocations[0].args[4])),
              QDir::cleanPath(expectedOutput));
}
