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

TEST(PlatformProfileLoaderTest, ListBuiltinIncludesRetroProfiles)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    ASSERT_FALSE(ids.isEmpty());
    EXPECT_TRUE(ids.contains(QStringLiteral("ps1")));
    EXPECT_TRUE(ids.contains(QStringLiteral("n64")));
    EXPECT_TRUE(ids.contains(QStringLiteral("nds")));
    EXPECT_TRUE(ids.contains(QStringLiteral("dreamcast")));
}

TEST(PlatformProfileLoaderTest, ListBuiltinIncludesModernProfiles)
{
    const QStringList ids = PlatformProfileLoader::listBuiltinIds();
    ASSERT_FALSE(ids.isEmpty());
    EXPECT_TRUE(ids.contains(QStringLiteral("modern-console")));
    EXPECT_TRUE(ids.contains(QStringLiteral("switch-like")));
    EXPECT_TRUE(ids.contains(QStringLiteral("steamdeck")));
    EXPECT_TRUE(ids.contains(QStringLiteral("mobile-low")));
    EXPECT_TRUE(ids.contains(QStringLiteral("webgl")));
    EXPECT_TRUE(ids.contains(QStringLiteral("vr")));
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
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_triangles_per_mesh")).toInt(), 20000);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_bones")).toInt(), 64);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_submesh_count")).toInt(), 8);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_draw_calls")).toInt(), 16);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_texture_dimension")).toInt(), 512);
    EXPECT_TRUE(loaded.profile.rules.value(QStringLiteral("texture_not_power_of_two")).toBool());
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("allowed_texture_formats")).toStringList(),
              (QStringList{QStringLiteral("png"), QStringLiteral("jpg")}));
    EXPECT_TRUE(loaded.profile.metadata.value(QStringLiteral("inspect_textures")).toBool());

    ScanConfig config = ScanConfig::defaults();
    applyPlatformProfile(config, loaded.profile);
    EXPECT_EQ(config.maxTriangleCount, 50000);
    EXPECT_EQ(config.maxTrianglesPerMesh, 20000);
    EXPECT_EQ(config.maxBoneCount, 64);
    EXPECT_EQ(config.maxSubmeshCount, 8);
    EXPECT_EQ(config.maxDrawCalls, 16);
    EXPECT_EQ(config.maxTextureResolution, 512);
    EXPECT_TRUE(config.probeTextureFiles);
    EXPECT_TRUE(config.requireTexturePowerOfTwo);
    EXPECT_EQ(config.allowedTextureFormats,
              (QStringList{QStringLiteral("png"), QStringLiteral("jpg")}));
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

namespace {

struct RetroProfileSnapshot {
    const char* id;
    int maxTriangleCount;
    int maxTrianglesPerMesh;
    int maxVertexCount;
    int maxBones;
    int maxSubmeshCount;
    int maxMaterialCount;
    int maxDrawCalls;
    int maxTextureResolution;
    const char* const* allowedTextureFormats;
    int allowedTextureFormatCount;
};

void expectRetroProfileLoadsAndApplies(const RetroProfileSnapshot& snap)
{
    const QString id = QString::fromLatin1(snap.id);
    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(id);
    ASSERT_TRUE(loaded.ok) << snap.id << ": " << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.id, id);
    EXPECT_FALSE(loaded.profile.description.isEmpty());
    EXPECT_TRUE(loaded.profile.description.contains(QStringLiteral("Validation-only"),
                                                    Qt::CaseInsensitive)
                || loaded.profile.description.contains(QStringLiteral("validation-only")));
    EXPECT_TRUE(loaded.profile.metadata.value(QStringLiteral("inspect_textures")).toBool());
    EXPECT_EQ(loaded.profile.metadata.value(QStringLiteral("scope")).toString(),
              QStringLiteral("validation"));

    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_triangle_count")).toInt(),
              snap.maxTriangleCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_triangles_per_mesh")).toInt(),
              snap.maxTrianglesPerMesh);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_vertex_count")).toInt(),
              snap.maxVertexCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_bones")).toInt(), snap.maxBones);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_submesh_count")).toInt(),
              snap.maxSubmeshCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_material_count")).toInt(),
              snap.maxMaterialCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_draw_calls")).toInt(),
              snap.maxDrawCalls);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_texture_dimension")).toInt(),
              snap.maxTextureResolution);
    EXPECT_TRUE(loaded.profile.rules.value(QStringLiteral("texture_not_power_of_two")).toBool());

    QStringList expectedFormats;
    for (int i = 0; i < snap.allowedTextureFormatCount; ++i)
        expectedFormats.append(QString::fromLatin1(snap.allowedTextureFormats[i]));
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("allowed_texture_formats")).toStringList(),
              expectedFormats);

    ScanConfig config = ScanConfig::defaults();
    applyPlatformProfile(config, loaded.profile);
    EXPECT_EQ(config.maxTriangleCount, snap.maxTriangleCount);
    EXPECT_EQ(config.maxTrianglesPerMesh, snap.maxTrianglesPerMesh);
    EXPECT_EQ(config.maxVertexCount, snap.maxVertexCount);
    EXPECT_EQ(config.maxBoneCount, snap.maxBones);
    EXPECT_EQ(config.maxSubmeshCount, snap.maxSubmeshCount);
    EXPECT_EQ(config.maxMaterialCount, snap.maxMaterialCount);
    EXPECT_EQ(config.maxDrawCalls, snap.maxDrawCalls);
    EXPECT_EQ(config.maxTextureResolution, snap.maxTextureResolution);
    EXPECT_TRUE(config.probeTextureFiles);
    EXPECT_TRUE(config.requireTexturePowerOfTwo);
    EXPECT_EQ(config.allowedTextureFormats, expectedFormats);
    EXPECT_EQ(config.requireUvChannels, 1);
}

} // namespace

TEST(PlatformProfileLoaderTest, BuiltinPs1ProfileLoadsRules)
{
    static const char* kFormats[] = {"png", "jpg"};
    expectRetroProfileLoadsAndApplies(
        { "ps1", 5000, 2000, 5000, 16, 4, 4, 8, 256, kFormats, 2 });
}

TEST(PlatformProfileLoaderTest, BuiltinN64ProfileLoadsRules)
{
    static const char* kFormats[] = {"png", "jpg"};
    expectRetroProfileLoadsAndApplies(
        { "n64", 12000, 4000, 12000, 32, 6, 8, 12, 256, kFormats, 2 });
}

TEST(PlatformProfileLoaderTest, BuiltinNdsProfileLoadsRules)
{
    static const char* kFormats[] = {"png", "jpg"};
    expectRetroProfileLoadsAndApplies(
        { "nds", 4000, 2000, 4000, 16, 4, 4, 8, 256, kFormats, 2 });
}

TEST(PlatformProfileLoaderTest, BuiltinDreamcastProfileLoadsRules)
{
    static const char* kFormats[] = {"png", "jpg", "pvr"};
    expectRetroProfileLoadsAndApplies(
        { "dreamcast", 15000, 8000, 15000, 64, 8, 12, 16, 512, kFormats, 3 });
}

namespace {

struct ModernProfileSnapshot {
    const char* id;
    int maxTriangleCount;
    int maxTrianglesPerMesh;
    int maxVertexCount;
    int maxBones;
    int maxSubmeshCount;
    int maxMaterialCount;
    int maxDrawCalls;
    int maxTextureResolution;
    int maxAnimKeyframes;
    double maxAnimDuration;
    double redundantKeyframesPct;
    double maxAcmr;
    const char* const* engineHints;
    int engineHintCount;
};

void expectModernProfileLoadsAndApplies(const ModernProfileSnapshot& snap)
{
    const QString id = QString::fromLatin1(snap.id);
    const PlatformProfileLoadResult loaded = PlatformProfileLoader::load(id);
    ASSERT_TRUE(loaded.ok) << snap.id << ": " << loaded.error.toStdString();
    EXPECT_EQ(loaded.profile.id, id);
    EXPECT_FALSE(loaded.profile.description.isEmpty());
    EXPECT_TRUE(loaded.profile.description.contains(QStringLiteral("Validation-only"),
                                                    Qt::CaseInsensitive)
                || loaded.profile.description.contains(QStringLiteral("validation-only")));
    EXPECT_TRUE(loaded.profile.description.contains(QStringLiteral("source"),
                                                    Qt::CaseInsensitive));
    EXPECT_TRUE(loaded.profile.metadata.value(QStringLiteral("inspect_textures")).toBool());
    EXPECT_EQ(loaded.profile.metadata.value(QStringLiteral("scope")).toString(),
              QStringLiteral("validation"));
    EXPECT_EQ(loaded.profile.metadata.value(QStringLiteral("category")).toString(),
              QStringLiteral("modern"));

    QStringList expectedHints;
    for (int i = 0; i < snap.engineHintCount; ++i)
        expectedHints.append(QString::fromLatin1(snap.engineHints[i]));
    EXPECT_EQ(loaded.profile.metadata.value(QStringLiteral("engineHints")).toStringList(),
              expectedHints);
    EXPECT_TRUE(loaded.profile.metadata.value(QStringLiteral("todoRules")).toStringList()
                    .contains(QStringLiteral("require_lod")));

    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_triangle_count")).toInt(),
              snap.maxTriangleCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_triangles_per_mesh")).toInt(),
              snap.maxTrianglesPerMesh);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_vertex_count")).toInt(),
              snap.maxVertexCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_bones")).toInt(), snap.maxBones);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_submesh_count")).toInt(),
              snap.maxSubmeshCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_material_count")).toInt(),
              snap.maxMaterialCount);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_draw_calls")).toInt(),
              snap.maxDrawCalls);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_texture_dimension")).toInt(),
              snap.maxTextureResolution);
    EXPECT_EQ(loaded.profile.rules.value(QStringLiteral("max_anim_keyframes")).toInt(),
              snap.maxAnimKeyframes);
    EXPECT_DOUBLE_EQ(loaded.profile.rules.value(QStringLiteral("max_anim_duration")).toDouble(),
                     snap.maxAnimDuration);
    EXPECT_DOUBLE_EQ(loaded.profile.rules.value(QStringLiteral("redundant_keyframes_pct")).toDouble(),
                     snap.redundantKeyframesPct);
    EXPECT_DOUBLE_EQ(loaded.profile.rules.value(QStringLiteral("max_acmr")).toDouble(),
                     snap.maxAcmr);
    EXPECT_TRUE(loaded.profile.rules.value(QStringLiteral("detect_zero_weight_bones")).toBool());

    ScanConfig config = ScanConfig::defaults();
    applyPlatformProfile(config, loaded.profile);
    EXPECT_EQ(config.maxTriangleCount, snap.maxTriangleCount);
    EXPECT_EQ(config.maxTrianglesPerMesh, snap.maxTrianglesPerMesh);
    EXPECT_EQ(config.maxVertexCount, snap.maxVertexCount);
    EXPECT_EQ(config.maxBoneCount, snap.maxBones);
    EXPECT_EQ(config.maxSubmeshCount, snap.maxSubmeshCount);
    EXPECT_EQ(config.maxMaterialCount, snap.maxMaterialCount);
    EXPECT_EQ(config.maxDrawCalls, snap.maxDrawCalls);
    EXPECT_EQ(config.maxTextureResolution, snap.maxTextureResolution);
    EXPECT_EQ(config.maxAnimKeyframes, snap.maxAnimKeyframes);
    EXPECT_DOUBLE_EQ(config.maxAnimDuration, snap.maxAnimDuration);
    EXPECT_DOUBLE_EQ(config.redundantKeyframesPctThreshold, snap.redundantKeyframesPct);
    EXPECT_DOUBLE_EQ(config.maxAcmr, snap.maxAcmr);
    EXPECT_TRUE(config.probeTextureFiles);
    EXPECT_TRUE(config.detectZeroWeightBones);
    EXPECT_EQ(config.allowedFormats,
              (QStringList{QStringLiteral("fbx"), QStringLiteral("glb"),
                           QStringLiteral("gltf"), QStringLiteral("vrm"),
                           QStringLiteral("obj")}));
}

} // namespace

TEST(PlatformProfileLoaderTest, BuiltinModernConsoleProfileLoadsRules)
{
    static const char* kHints[] = {"unreal", "unity", "godot", "custom"};
    expectModernProfileLoadsAndApplies(
        { "modern-console", 250000, 120000, 250000, 256, 32, 32, 128, 4096, 80000, 600.0, 25.0,
          1.2, kHints, 4 });
}

TEST(PlatformProfileLoaderTest, BuiltinSwitchLikeProfileLoadsRules)
{
    static const char* kHints[] = {"unreal", "unity", "godot"};
    expectModernProfileLoadsAndApplies(
        { "switch-like", 80000, 40000, 80000, 128, 16, 16, 32, 2048, 30000, 300.0, 30.0, 1.0,
          kHints, 3 });
}

TEST(PlatformProfileLoaderTest, BuiltinSteamdeckProfileLoadsRules)
{
    static const char* kHints[] = {"unreal", "unity", "godot", "custom"};
    expectModernProfileLoadsAndApplies(
        { "steamdeck", 150000, 75000, 150000, 200, 24, 24, 64, 4096, 50000, 450.0, 28.0, 1.1,
          kHints, 4 });
}

TEST(PlatformProfileLoaderTest, BuiltinMobileLowProfileLoadsRules)
{
    static const char* kHints[] = {"unity", "godot", "unreal"};
    expectModernProfileLoadsAndApplies(
        { "mobile-low", 25000, 12000, 25000, 64, 8, 8, 16, 1024, 12000, 180.0, 35.0, 0.9, kHints,
          3 });
}

TEST(PlatformProfileLoaderTest, BuiltinWebglProfileLoadsRules)
{
    static const char* kHints[] = {"unity", "godot", "custom"};
    expectModernProfileLoadsAndApplies(
        { "webgl", 40000, 20000, 40000, 96, 12, 12, 24, 2048, 20000, 240.0, 35.0, 0.95, kHints,
          3 });
}

TEST(PlatformProfileLoaderTest, BuiltinVrProfileLoadsRules)
{
    static const char* kHints[] = {"unreal", "unity", "godot"};
    expectModernProfileLoadsAndApplies(
        { "vr", 70000, 35000, 70000, 96, 12, 12, 20, 2048, 25000, 300.0, 25.0, 0.85, kHints, 3 });
}
