#include <gtest/gtest.h>
#include "AssetBrowserController.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QSettings>

class AssetBrowserControllerTests : public ::testing::Test {
protected:
    QApplication* app = nullptr;

    void SetUp() override {
        app = qobject_cast<QApplication*>(QCoreApplication::instance());
        ASSERT_NE(app, nullptr);
    }

    void TearDown() override {
        AssetBrowserController::kill();
    }
};

TEST_F(AssetBrowserControllerTests, Singleton) {
    auto* abc = AssetBrowserController::instance();
    ASSERT_NE(abc, nullptr);
    EXPECT_EQ(abc, AssetBrowserController::instance());
}

TEST_F(AssetBrowserControllerTests, QmlInstanceReturnsSameAsInstance) {
    auto* abc1 = AssetBrowserController::instance();
    auto* abc2 = AssetBrowserController::qmlInstance(nullptr, nullptr);
    EXPECT_EQ(abc1, abc2);
}

TEST_F(AssetBrowserControllerTests, KillAndRecreate) {
    auto* abc1 = AssetBrowserController::instance();
    ASSERT_NE(abc1, nullptr);

    AssetBrowserController::kill();

    auto* abc2 = AssetBrowserController::instance();
    ASSERT_NE(abc2, nullptr);
    EXPECT_FALSE(abc2->rootPath().isEmpty());
}

TEST_F(AssetBrowserControllerTests, DefaultRootPathIsValid) {
    auto* abc = AssetBrowserController::instance();
    ASSERT_NE(abc, nullptr);
    EXPECT_FALSE(abc->rootPath().isEmpty());
    EXPECT_TRUE(QDir(abc->rootPath()).exists());
}

TEST_F(AssetBrowserControllerTests, SetRootPathEmitsSignal) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::rootPathChanged);

    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    abc->setRootPath(tmpDir.path());
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(abc->rootPath(), tmpDir.path());
}

TEST_F(AssetBrowserControllerTests, SetRootPathIgnoresNonexistent) {
    auto* abc = AssetBrowserController::instance();
    QString original = abc->rootPath();
    QSignalSpy spy(abc, &AssetBrowserController::rootPathChanged);

    abc->setRootPath("/this/path/does/not/exist/at/all");
    EXPECT_EQ(spy.count(), 0);
    EXPECT_EQ(abc->rootPath(), original);
}

TEST_F(AssetBrowserControllerTests, SetRootPathSameValueNoSignal) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::rootPathChanged);

    abc->setRootPath(abc->rootPath());
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(AssetBrowserControllerTests, FilterDefaultIsAll) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->filter(), "all");
}

TEST_F(AssetBrowserControllerTests, SetFilterEmitsSignal) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::filterChanged);

    abc->setFilter("meshes");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(abc->filter(), "meshes");
}

TEST_F(AssetBrowserControllerTests, SetFilterSameValueNoSignal) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::filterChanged);

    abc->setFilter("all");  // already the default
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(AssetBrowserControllerTests, SearchQueryEmitsSignal) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::searchQueryChanged);

    abc->setSearchQuery("test");
    EXPECT_EQ(spy.count(), 1);
    EXPECT_EQ(abc->searchQuery(), "test");
}

TEST_F(AssetBrowserControllerTests, FilesListFromTempDir) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Create some test files
    QFile(tmpDir.path() + "/model.fbx").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/texture.png").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/mat.material").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/readme.txt").open(QIODevice::WriteOnly);
    QDir(tmpDir.path()).mkdir("subdir");

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());

    QVariantList files = abc->files();
    EXPECT_EQ(files.size(), 5);  // 1 dir + 4 files
}

TEST_F(AssetBrowserControllerTests, FilterMeshesOnly) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QFile(tmpDir.path() + "/model.fbx").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/texture.png").open(QIODevice::WriteOnly);
    QDir(tmpDir.path()).mkdir("subdir");

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());
    abc->setFilter("meshes");

    QVariantList files = abc->files();
    // Should include the directory + the mesh file, not the texture
    int meshCount = 0;
    int dirCount = 0;
    for (const QVariant& v : files) {
        QVariantMap m = v.toMap();
        if (m["isDir"].toBool()) dirCount++;
        else if (m["type"].toString() == "mesh") meshCount++;
    }
    EXPECT_EQ(meshCount, 1);
    EXPECT_EQ(dirCount, 1);  // directories always shown
}

TEST_F(AssetBrowserControllerTests, FilterTexturesOnly) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QFile(tmpDir.path() + "/model.fbx").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/texture.png").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/another.jpg").open(QIODevice::WriteOnly);

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());
    abc->setFilter("textures");

    int textureCount = 0;
    for (const QVariant& v : abc->files()) {
        QVariantMap m = v.toMap();
        if (!m["isDir"].toBool() && m["type"].toString() == "texture")
            textureCount++;
    }
    EXPECT_EQ(textureCount, 2);
}

TEST_F(AssetBrowserControllerTests, SearchQueryFiltersFiles) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QFile(tmpDir.path() + "/player.fbx").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/enemy.fbx").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/player_diffuse.png").open(QIODevice::WriteOnly);

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());
    abc->setSearchQuery("player");

    QVariantList files = abc->files();
    EXPECT_EQ(files.size(), 2);  // player.fbx and player_diffuse.png
}

TEST_F(AssetBrowserControllerTests, NavigateUp) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir(tmpDir.path()).mkdir("child");
    QString childPath = tmpDir.path() + "/child";

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(childPath);
    EXPECT_EQ(abc->rootPath(), childPath);

    abc->navigateUp();
    EXPECT_EQ(abc->rootPath(), tmpDir.path());
}

TEST_F(AssetBrowserControllerTests, NavigateToDirectory) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir(tmpDir.path()).mkdir("subdir");
    QString subdirPath = tmpDir.path() + "/subdir";

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());

    abc->navigateToDirectory(subdirPath);
    EXPECT_EQ(abc->rootPath(), subdirPath);
}

TEST_F(AssetBrowserControllerTests, FileTypeClassification) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.fbx"), "mesh");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.gltf"), "mesh");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.vrm"), "mesh");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.obj"), "mesh");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.png"), "texture");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.jpg"), "texture");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.tga"), "texture");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.material"), "material");
    EXPECT_EQ(abc->fileTypeForPath("/foo/bar.txt"), "other");
}

TEST_F(AssetBrowserControllerTests, OpenFileMeshEmitsImportSignal) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QString meshPath = tmpDir.path() + "/model.fbx";
    QFile(meshPath).open(QIODevice::WriteOnly);

    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::importMeshRequested);

    abc->openFile(meshPath);
    EXPECT_EQ(spy.count(), 1);
    QStringList paths = spy.at(0).at(0).toStringList();
    EXPECT_EQ(paths.size(), 1);
    EXPECT_EQ(paths.at(0), meshPath);
}

TEST_F(AssetBrowserControllerTests, OpenFileNonexistentDoesNothing) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::importMeshRequested);

    abc->openFile("/nonexistent/path/file.fbx");
    EXPECT_EQ(spy.count(), 0);
}

TEST_F(AssetBrowserControllerTests, OpenFileDirectoryNavigates) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir(tmpDir.path()).mkdir("subdir");
    QString subdirPath = tmpDir.path() + "/subdir";

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());
    QSignalSpy spy(abc, &AssetBrowserController::importMeshRequested);

    abc->openFile(subdirPath);
    EXPECT_EQ(spy.count(), 0);  // should not emit import signal
    EXPECT_EQ(abc->rootPath(), subdirPath);  // should navigate into the directory
}

TEST_F(AssetBrowserControllerTests, BrowseRequestedSignal) {
    auto* abc = AssetBrowserController::instance();
    QSignalSpy spy(abc, &AssetBrowserController::browseRequested);

    abc->browseForDirectory();
    EXPECT_EQ(spy.count(), 1);
}

TEST_F(AssetBrowserControllerTests, RootPathPersistedInSettings) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    {
        auto* abc = AssetBrowserController::instance();
        abc->setRootPath(tmpDir.path());
    }

    QSettings settings;
    EXPECT_EQ(settings.value("AssetBrowser/rootPath").toString(), tmpDir.path());
}

// --- Additional file type classification tests ---

TEST_F(AssetBrowserControllerTests, ClassifyExtensionForDotX) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/model.x"), "mesh");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionForMeshXml) {
    auto* abc = AssetBrowserController::instance();
    // .mesh.xml files: fileTypeForPath uses suffix which gives "xml"
    // Verify the behavior for the .mesh extension itself
    EXPECT_EQ(abc->fileTypeForPath("/foo/model.mesh"), "mesh");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionForStl) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/part.stl"), "mesh");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionFor3ds) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/scene.3ds"), "mesh");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionForDds) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/texture.dds"), "texture");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionForHdr) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/env.hdr"), "texture");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionForExr) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/light.exr"), "texture");
}

TEST_F(AssetBrowserControllerTests, ClassifyExtensionCaseInsensitive) {
    auto* abc = AssetBrowserController::instance();
    EXPECT_EQ(abc->fileTypeForPath("/foo/Model.FBX"), "mesh");
    EXPECT_EQ(abc->fileTypeForPath("/foo/Texture.PNG"), "texture");
    EXPECT_EQ(abc->fileTypeForPath("/foo/Mat.MATERIAL"), "material");
}

TEST_F(AssetBrowserControllerTests, MaterialPreviewReturnsEmptyForNonMaterialFile) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Create a non-material file
    QString path = tmpDir.path() + "/model.fbx";
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write("not a material");
    f.close();

    auto* abc = AssetBrowserController::instance();
    // materialPreview on a non-material file (or one with no valid material name)
    // should return empty
    QString preview = abc->materialPreview(path);
    EXPECT_TRUE(preview.isEmpty());
}

TEST_F(AssetBrowserControllerTests, MaterialPreviewReturnsEmptyForNonexistentFile) {
    auto* abc = AssetBrowserController::instance();
    QString preview = abc->materialPreview("/nonexistent/path/foo.material");
    EXPECT_TRUE(preview.isEmpty());
}

TEST_F(AssetBrowserControllerTests, FilterMaterialsOnly) {
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QFile(tmpDir.path() + "/model.fbx").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/texture.png").open(QIODevice::WriteOnly);
    QFile(tmpDir.path() + "/shader.material").open(QIODevice::WriteOnly);

    auto* abc = AssetBrowserController::instance();
    abc->setRootPath(tmpDir.path());
    abc->setFilter("materials");

    int materialCount = 0;
    for (const QVariant& v : abc->files()) {
        QVariantMap m = v.toMap();
        if (!m["isDir"].toBool() && m["type"].toString() == "material")
            materialCount++;
    }
    EXPECT_EQ(materialCount, 1);
}
