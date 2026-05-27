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

TEST(PlatformProfileLoaderTest, LoadByExplicitPath)
{
    const QString dir = profilesSourceDir();
    ASSERT_FALSE(dir.isEmpty());
    const QString path = QDir(dir).absoluteFilePath(QStringLiteral("example-base.json"));

    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(path);
    ASSERT_TRUE(loaded.ok) << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.id, QStringLiteral("example-base"));
}
