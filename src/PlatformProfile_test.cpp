#include <gtest/gtest.h>

#include "PlatformProfile.h"
#include "ScanConfig.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

QString profilesSourceDir()
{
#ifdef QTMESH_UT_SOURCE_ROOT
    const QString dir = QStringLiteral(QTMESH_UT_SOURCE_ROOT) + QStringLiteral("/profiles");
    if (QDir(dir).exists())
        return dir;
#endif
    const QString fromApp =
        QCoreApplication::applicationDirPath() + QStringLiteral("/profiles");
    if (QDir(fromApp).exists())
        return fromApp;
    return {};
}

void writeJsonFile(const QString& path, const QByteArray& json)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(f.write(json), json.size());
}

} // namespace

TEST(PlatformProfileLoaderTest, ResolvesBuiltinExampleMinimal)
{
    ASSERT_FALSE(profilesSourceDir().isEmpty()) << "profiles/ directory not found";

    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("example-minimal"));
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.id, QStringLiteral("example-minimal"));
    EXPECT_EQ(loaded.profile.displayName, QStringLiteral("Example Minimal"));
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_vertex_count")).toInt(), 10000);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_material_count")).toInt(), 8);
    EXPECT_TRUE(loaded.profile.rules.value(QStringLiteral("require_textures_exist")).toBool());
}

TEST(PlatformProfileLoaderTest, InheritanceMergesParentRules)
{
    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("example-minimal"));
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_vertex_count")).toInt(), 10000);
    EXPECT_TRUE(loaded.profile.rules.value(QStringLiteral("require_textures_exist")).toBool());
}

TEST(PlatformProfileLoaderTest, MissingBuiltinIdIsActionable)
{
    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("does-not-exist-profile"));
    EXPECT_FALSE(loaded.ok);
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("does-not-exist-profile")));
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("Unknown platform profile"))
                || loaded.error.contains(QStringLiteral("not found")));
}

TEST(PlatformProfileLoaderTest, InvalidJsonReturnsError)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("bad.json"));
    writeJsonFile(path, QByteArray("{ not json"));

    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(path);
    EXPECT_FALSE(loaded.ok);
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("Invalid JSON")));
}

TEST(PlatformProfileLoaderTest, CircularExtendsDetected)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeJsonFile(temp.filePath(QStringLiteral("a.json")),
                  QByteArray(R"({"id":"a","extends":"b","rules":{}})"));
    writeJsonFile(temp.filePath(QStringLiteral("b.json")),
                  QByteArray(R"({"id":"b","extends":"a","rules":{}})"));

    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(temp.filePath(QStringLiteral("a.json")));
    EXPECT_FALSE(loaded.ok);
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("Circular")));
}

TEST(PlatformProfileLoaderTest, UnknownTopLevelAndRuleKeysWarn)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeJsonFile(temp.filePath(QStringLiteral("warn.json")),
                  QByteArray(R"({
  "id": "warn",
  "foo": 1,
  "rules": {
    "max_vertex_count": 1,
    "not_a_real_rule": true
  }
})"));

    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(temp.filePath(QStringLiteral("warn.json")));
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    bool sawTop = false;
    bool sawRule = false;
    for (const QString& w : loaded.warnings) {
        if (w.contains(QStringLiteral("foo")))
            sawTop = true;
        if (w.contains(QStringLiteral("not_a_real_rule")))
            sawRule = true;
    }
    EXPECT_TRUE(sawTop);
    EXPECT_TRUE(sawRule);
    EXPECT_FALSE(loaded.profile.rules.contains(QStringLiteral("not_a_real_rule")));
}

TEST(ApplyPlatformProfileTest, MergeOrderDefaultsProfileProjectCli)
{
    ScanConfig config = ScanConfig::defaults();
    EXPECT_EQ(config.maxVertexCount, 0);

    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("example-minimal"));
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    applyPlatformProfile(config, loaded.profile);
    EXPECT_EQ(config.maxVertexCount, 10000);
    EXPECT_EQ(config.maxMaterialCount, 8);
    EXPECT_TRUE(config.requireTexturesExist);

    QVariantMap project;
    QVariantMap rules;
    rules[QStringLiteral("max_vertex_count")] = 8000;
    project[QStringLiteral("rules")] = rules;
    ScanConfig::applyProjectConfig(config, project);
    EXPECT_EQ(config.maxVertexCount, 8000);
    EXPECT_EQ(config.maxMaterialCount, 8);

    config.maxVertexCount = 5000;
    EXPECT_EQ(config.maxVertexCount, 5000);
}

TEST(PlatformProfileLoaderTest, ListBuiltinIncludesExampleProfiles)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    ASSERT_FALSE(ids.isEmpty());
    EXPECT_TRUE(ids.contains(QStringLiteral("example-minimal")));
    EXPECT_TRUE(ids.contains(QStringLiteral("example-base")));
}

TEST(PlatformProfileLoaderTest, BuiltinIdMismatchIsRejected)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    writeJsonFile(temp.filePath(QStringLiteral("wrong-id.json")),
                  QByteArray(R"({"id":"other-id","rules":{"max_vertex_count":1}})"));

    const QByteArray prior = qgetenv("QTMESH_PROFILES_DIR");
    ASSERT_TRUE(qputenv("QTMESH_PROFILES_DIR", QFileInfo(temp.path()).absoluteFilePath().toUtf8()));

    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("wrong-id"));
    if (!prior.isEmpty())
        ASSERT_TRUE(qputenv("QTMESH_PROFILES_DIR", prior));
    else
        qunsetenv("QTMESH_PROFILES_DIR");

    EXPECT_FALSE(loaded.ok);
    EXPECT_TRUE(loaded.error.contains(QStringLiteral("id mismatch")));
}

TEST(PlatformProfileLoaderTest, LoadByExplicitPath)
{
    const QString dir = profilesSourceDir();
    ASSERT_FALSE(dir.isEmpty());
    const QString path = QDir(dir).absoluteFilePath(QStringLiteral("example-base.json"));

    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(path);
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.id, QStringLiteral("example-base"));
}

TEST(ApplyPlatformProfileTest, MetadataInspectTexturesEnablesProbe)
{
    ScanConfig config = ScanConfig::defaults();
    EXPECT_FALSE(config.probeTextureFiles);

    PlatformProfile profile;
    profile.id = QStringLiteral("test-inspect");
    profile.metadata.insert(QStringLiteral("inspect_textures"), true);
    applyPlatformProfile(config, profile);
    EXPECT_TRUE(config.probeTextureFiles);
}

TEST(PlatformProfileLoaderTest, BuiltinExampleBudgetProfileLoadsRules)
{
    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("example-budget"));
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_triangle_count")).toInt(), 50000);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_bones")).toInt(), 64);
    EXPECT_TRUE(loaded.profile.metadata.value(QStringLiteral("inspect_textures")).toBool());

    ScanConfig config = ScanConfig::defaults();
    applyPlatformProfile(config, loaded.profile);
    EXPECT_EQ(config.maxTriangleCount, 50000);
    EXPECT_EQ(config.maxBoneCount, 64);
    EXPECT_TRUE(config.probeTextureFiles);
    EXPECT_TRUE(config.requireTexturePowerOfTwo);
}

TEST(PlatformProfileLoaderTest, BuiltinExampleTextureInspectProfileEnablesProbe)
{
    const PlatformProfileLoadResult loaded =
        PlatformProfileLoader::load(QStringLiteral("example-texture-inspect"));
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    EXPECT_TRUE(loaded.profile.metadata.value(QStringLiteral("inspect_textures")).toBool());

    ScanConfig config = ScanConfig::defaults();
    applyPlatformProfile(config, loaded.profile);
    EXPECT_TRUE(config.probeTextureFiles);
}
