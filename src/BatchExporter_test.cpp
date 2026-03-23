#include <gtest/gtest.h>
#include "BatchExporter.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>

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

// Note: Tests that call execute() with actual files are NOT included here
// because BatchExporter::execute() calls CLIPipeline::run(), which creates
// a new QApplication and calls _exit(), terminating the test process.
// Integration tests with real files should be done separately.
