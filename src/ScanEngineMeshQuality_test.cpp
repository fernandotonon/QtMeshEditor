// Scan rules, config and fix gating for mesh-buffer quality (#678).

#include "ScanConfig.h"
#include "ScanEngine.h"

#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

namespace {

AssetInfo gltfAsset()
{
    AssetInfo a;
    a.filePath = QStringLiteral("/nowhere/a.glb");
    a.relativePath = QStringLiteral("a.glb");
    a.format = QStringLiteral("glb");
    a.meshQualityMeasured = true;
    return a;
}

MeshQualityEntry entry(size_t tris, double acmr, double acmrOpt)
{
    MeshQualityEntry e;
    e.meshName = QStringLiteral("body");
    e.measured = true;
    e.indexFixable = true;
    e.vertexFixable = true;
    e.metrics.triangles = tris;
    e.metrics.vertices = tris;
    e.metrics.acmr = acmr;
    e.metrics.acmrOptimized = acmrOpt;
    e.metrics.overdraw = 1.2;
    e.metrics.overdrawOptimized = 1.2;
    e.metrics.overfetch = 1.0;
    e.metrics.overfetchOptimized = 1.0;
    return e;
}

const Finding* findRule(const QList<Finding>& fs, const QString& rule)
{
    for (const Finding& f : fs)
        if (f.rule == rule)
            return &f;
    return nullptr;
}

} // namespace

TEST(ScanMeshQuality, DefaultsWarnOnLargeOrderingGains)
{
    AssetInfo a = gltfAsset();
    a.meshQuality = {entry(5000, 1.2, 0.7)};   // 42% gain
    const auto fs = ScanEngine::evaluateRules(a, ScanConfig::defaults());
    const Finding* f = findRule(fs, "detect_acmr_gain_pct");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->severity, Severity::Warning);
    EXPECT_TRUE(f->fixable);
    EXPECT_TRUE(f->message.contains("--fix-mesh vertex-cache"));
}

TEST(ScanMeshQuality, SmallGainsAndSmallMeshesStayQuiet)
{
    AssetInfo a = gltfAsset();
    a.meshQuality = {entry(5000, 0.8, 0.7)};   // 12% gain
    EXPECT_EQ(findRule(ScanEngine::evaluateRules(a, ScanConfig::defaults()), "detect_acmr_gain_pct"), nullptr);

    a.meshQuality = {entry(12, 2.0, 0.5)};     // big gain on a 12-triangle crate
    EXPECT_EQ(findRule(ScanEngine::evaluateRules(a, ScanConfig::defaults()), "detect_acmr_gain_pct"), nullptr);
}

TEST(ScanMeshQuality, ZeroThresholdDisablesTheRule)
{
    AssetInfo a = gltfAsset();
    a.meshQuality = {entry(5000, 1.2, 0.7)};
    ScanConfig c = ScanConfig::defaults();
    c.detectAcmrGainPct = 0.0;
    EXPECT_EQ(findRule(ScanEngine::evaluateRules(a, c), "detect_acmr_gain_pct"), nullptr);
}

TEST(ScanMeshQuality, UnfixablePrimitivesSayWhy)
{
    AssetInfo a = gltfAsset();
    MeshQualityEntry e = entry(5000, 1.2, 0.7);
    e.indexFixable = false;
    e.vertexFixable = false;
    e.note = QStringLiteral("its triangle order encodes n-gons (FB_ngon_encoding)");
    a.meshQuality = {e};
    const auto fs = ScanEngine::evaluateRules(a, ScanConfig::defaults());
    const Finding* f = findRule(fs, "detect_acmr_gain_pct");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->fixable);
    EXPECT_TRUE(f->message.contains("FB_ngon_encoding")) << f->message.toStdString();
}

TEST(ScanMeshQuality, RedundantAndDegenerate)
{
    AssetInfo a = gltfAsset();
    MeshQualityEntry e = entry(1000, 0.7, 0.7);
    e.metrics.vertices = 1000;
    e.metrics.redundantVertices = 80;
    e.metrics.degenerateTriangles = 3;
    a.meshQuality = {e};
    a.degenerateTriangles = 3;
    a.degenerateTrianglesOf = 1000;
    const auto fs = ScanEngine::evaluateRules(a, ScanConfig::defaults());
    ASSERT_NE(findRule(fs, "detect_redundant_verts_pct"), nullptr);
    ASSERT_NE(findRule(fs, "detect_degenerate_tris_pct"), nullptr);
    EXPECT_TRUE(findRule(fs, "detect_degenerate_tris_pct")->message.contains("3 of 1000"));
}

TEST(ScanMeshQuality, OtherFormatsOnlyGetTheDegenerateAndBudgetRules)
{
    AssetInfo a;
    a.relativePath = QStringLiteral("a.fbx");
    a.format = QStringLiteral("fbx");
    a.degenerateTriangles = 50;
    a.degenerateTrianglesOf = 1000;
    a.largestSubmeshTriangles = 30000;
    a.largestSubmeshName = QStringLiteral("Body submesh 0");
    ScanConfig c = ScanConfig::defaults();
    c.maxTrianglesPerSubmesh = 20000;
    const auto fs = ScanEngine::evaluateRules(a, c);
    const Finding* deg = findRule(fs, "detect_degenerate_tris_pct");
    ASSERT_NE(deg, nullptr);
    EXPECT_FALSE(deg->fixable);
    EXPECT_TRUE(deg->message.contains("glTF/GLB only"));
    const Finding* budget = findRule(fs, "max_triangles_per_submesh");
    ASSERT_NE(budget, nullptr);
    EXPECT_TRUE(budget->message.contains("Body submesh 0"));
    EXPECT_FALSE(budget->fixable);
}

TEST(ScanMeshQuality, MeshFixIdsNormalise)
{
    QStringList unknown;
    const QStringList all = ScanConfig::normalizeMeshFixes({QStringLiteral("all")}, &unknown);
    EXPECT_EQ(all, (QStringList{"degenerates", "weld", "vertex-cache", "overdraw", "vertex-fetch"}));
    EXPECT_FALSE(all.contains("simplify"));   // destructive: never part of all
    EXPECT_EQ(ScanConfig::normalizeMeshFixes({QStringLiteral("Vertex_Cache, weld,bogus")}, &unknown),
              (QStringList{"vertex-cache", "weld"}));
    EXPECT_EQ(unknown, QStringList{"bogus"});

    ScanConfig c;
    EXPECT_TRUE(c.effectiveMeshFixes().isEmpty());   // nothing unless asked
    c.optimizeMeshes = true;
    EXPECT_EQ(c.effectiveMeshFixes().size(), 5);
}

TEST(ScanMeshQuality, ConfigKeysParse)
{
    const QVariantMap root{
        {"rules", QVariantMap{{"detect_acmr_gain_pct", 40}, {"detect_redundant_verts_pct", 0},
                              {"max_triangles_per_submesh", 20000}, {"mesh_quality_min_triangles", 100}}},
        {"fix", QVariantMap{{"mesh_fixes", QStringList{"weld", "simplify"}}, {"simplify_target_tris", 15000}}},
    };
    const ScanConfig c = ScanConfig::fromVariantMap(root);
    EXPECT_DOUBLE_EQ(c.detectAcmrGainPct, 40.0);
    EXPECT_DOUBLE_EQ(c.detectRedundantVertsPct, 0.0);
    EXPECT_EQ(c.maxTrianglesPerSubmesh, 20000);
    EXPECT_EQ(c.meshQualityMinTriangles, 100);
    EXPECT_EQ(c.meshFixes, (QStringList{"weld", "simplify"}));
    EXPECT_EQ(c.simplifyTargetTris, 15000);
    EXPECT_FALSE(c.optimizeMeshes);

    const QVariantMap yaml = parseSimpleYaml(QStringLiteral("fix:\n  optimize_meshes: true\n"));
    EXPECT_TRUE(ScanConfig::fromVariantMap(yaml).optimizeMeshes);
}

TEST(ScanMeshQuality, PlainFixDoesNotRewriteMeshes)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("a.glb");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("not touched");
    }
    AssetInfo a = gltfAsset();
    a.filePath = path;
    a.meshQuality = {entry(5000, 1.2, 0.7)};
    ScanConfig c = ScanConfig::defaults();
    c.fixEnabled = true;   // --fix without --fix-mesh
    QList<Finding> fs = ScanEngine::evaluateRules(a, c);
    ScanEngine::applyFixes(c, dir.path(), a, fs);
    const Finding* f = findRule(fs, "detect_acmr_gain_pct");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->fixed);
    EXPECT_TRUE(f->message.contains("not fixed"));
    QFile check(path);
    ASSERT_TRUE(check.open(QIODevice::ReadOnly));
    EXPECT_EQ(check.readAll(), QByteArray("not touched"));
}

TEST(ScanMeshQuality, SelectedFixOnBrokenFileReportsTheFailure)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("a.glb");
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("not a glb");
    }
    AssetInfo a = gltfAsset();
    a.filePath = path;
    a.meshQuality = {entry(5000, 1.2, 0.7)};
    ScanConfig c = ScanConfig::defaults();
    c.fixEnabled = true;
    c.meshFixes = {QStringLiteral("vertex-cache")};
    QList<Finding> fs = ScanEngine::evaluateRules(a, c);
    ScanEngine::applyFixes(c, dir.path(), a, fs);
    const Finding* f = findRule(fs, "detect_acmr_gain_pct");
    ASSERT_NE(f, nullptr);
    EXPECT_FALSE(f->fixed);
    EXPECT_TRUE(f->message.contains("fix failed"));
}
