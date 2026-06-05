#include <gtest/gtest.h>

#include "ScanConfig.h"

#include <QDir>
#include <QFile>
#include <QTemporaryFile>
#include <QTemporaryDir>

namespace {

void writeFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(f.write(content), content.size());
    f.close();
}

} // namespace

// -------------------- parseSimpleYaml --------------------

TEST(ParseSimpleYamlTest, EmptyContent)
{
    const QVariantMap m = parseSimpleYaml(QString());
    EXPECT_TRUE(m.isEmpty());
}

TEST(ParseSimpleYamlTest, TopLevelScalars)
{
    const QString yaml = QStringLiteral(
        "version: 1\n"
        "name: \"my-scan\"\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    EXPECT_EQ(m.value("version").toInt(), 1);
    EXPECT_EQ(m.value("name").toString(), QStringLiteral("my-scan"));
}

TEST(ParseSimpleYamlTest, ScalarTypes)
{
    const QString yaml = QStringLiteral(
        "anInt: 42\n"
        "aDouble: 3.14\n"
        "aTrue: true\n"
        "aYes: yes\n"
        "aOn: on\n"
        "aFalse: false\n"
        "aNo: no\n"
        "aOff: off\n"
        "aString: hello\n"
        "quotedString: \"with spaces\"\n"
        "singleQuoted: 'sq'\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    EXPECT_EQ(m.value("anInt").toInt(), 42);
    EXPECT_DOUBLE_EQ(m.value("aDouble").toDouble(), 3.14);
    EXPECT_TRUE(m.value("aTrue").toBool());
    EXPECT_TRUE(m.value("aYes").toBool());
    EXPECT_TRUE(m.value("aOn").toBool());
    EXPECT_FALSE(m.value("aFalse").toBool());
    EXPECT_FALSE(m.value("aNo").toBool());
    EXPECT_FALSE(m.value("aOff").toBool());
    EXPECT_EQ(m.value("aString").toString(), QStringLiteral("hello"));
    EXPECT_EQ(m.value("quotedString").toString(), QStringLiteral("with spaces"));
    EXPECT_EQ(m.value("singleQuoted").toString(), QStringLiteral("sq"));
}

TEST(ParseSimpleYamlTest, SectionWithInlineList)
{
    const QString yaml = QStringLiteral(
        "scan:\n"
        "  roots: [./a, ./b]\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    const QVariantMap scan = m.value("scan").toMap();
    const QStringList roots = scan.value("roots").toStringList();
    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots[0], QStringLiteral("./a"));
    EXPECT_EQ(roots[1], QStringLiteral("./b"));
}

TEST(ParseSimpleYamlTest, SectionWithBlockList)
{
    const QString yaml = QStringLiteral(
        "scan:\n"
        "  roots:\n"
        "    - ./first\n"
        "    - ./second\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    const QVariantMap scan = m.value("scan").toMap();
    const QStringList roots = scan.value("roots").toStringList();
    ASSERT_EQ(roots.size(), 2);
    EXPECT_EQ(roots[0], QStringLiteral("./first"));
    EXPECT_EQ(roots[1], QStringLiteral("./second"));
}

TEST(ParseSimpleYamlTest, CommentsAreStripped)
{
    const QString yaml = QStringLiteral(
        "version: 1  # trailing comment\n"
        "# full line\n"
        "name: bob\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    EXPECT_EQ(m.value("version").toInt(), 1);
    EXPECT_EQ(m.value("name").toString(), QStringLiteral("bob"));
}

TEST(ParseSimpleYamlTest, HashInsideQuotesIsKept)
{
    // # inside a quoted string should not start a comment
    const QString yaml = QStringLiteral(
        "name: \"a # b\"\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    EXPECT_EQ(m.value("name").toString(), QStringLiteral("a # b"));
}

TEST(ParseSimpleYamlTest, NestedScopesWithOrderPreserved)
{
    // Two scopes with different rule overrides — _order must reflect declaration order.
    const QString yaml = QStringLiteral(
        "scopes:\n"
        "  \"characters/**\":\n"
        "    max_vertex_count: 5000\n"
        "  \"props/**\":\n"
        "    max_vertex_count: 1000\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    const QVariantMap scopes = m.value("scopes").toMap();
    const QStringList order = scopes.value("_order").toStringList();
    ASSERT_EQ(order.size(), 2);
    EXPECT_EQ(order[0], QStringLiteral("characters/**"));
    EXPECT_EQ(order[1], QStringLiteral("props/**"));

    const QVariantMap chars = scopes.value("characters/**").toMap();
    EXPECT_EQ(chars.value("max_vertex_count").toInt(), 5000);

    const QVariantMap props = scopes.value("props/**").toMap();
    EXPECT_EQ(props.value("max_vertex_count").toInt(), 1000);
}

TEST(ParseSimpleYamlTest, EmptyKeyFollowedByNothingIsTreatedAsScalar)
{
    // A key with no value that isn't followed by list items or a subsection
    // should not break parsing of the next top-level key.
    const QString yaml = QStringLiteral(
        "section:\n"
        "  emptyKey:\n"
        "version: 7\n");
    const QVariantMap m = parseSimpleYaml(yaml);
    EXPECT_EQ(m.value("version").toInt(), 7);
}

// -------------------- ScanConfig defaults --------------------

TEST(ScanConfigDefaultsTest, DefaultsHaveExpectedValues)
{
    const ScanConfig cfg = ScanConfig::defaults();
    EXPECT_EQ(cfg.version, 1);
    EXPECT_FALSE(cfg.includePatterns.isEmpty());
    EXPECT_FALSE(cfg.excludePatterns.isEmpty());
    EXPECT_EQ(cfg.maxFileSizeMb, 0);
    EXPECT_EQ(cfg.maxMeshCount, 0);
    EXPECT_FALSE(cfg.requireSkeleton);
    EXPECT_FALSE(cfg.requireAnimations);
    EXPECT_TRUE(cfg.allowEmbeddedTextures);
    EXPECT_FALSE(cfg.requireTexturesExist);
    EXPECT_TRUE(cfg.allowMissingMaterials);
    EXPECT_FALSE(cfg.fixEnabled);
    EXPECT_FALSE(cfg.dryRun);
    EXPECT_EQ(cfg.reportFormat, QStringLiteral("text"));
    EXPECT_EQ(cfg.failOn, QStringLiteral("error"));
    EXPECT_EQ(cfg.maxAcmr, 0.0);
    EXPECT_EQ(cfg.redundantKeyframesPctThreshold, 0.0);
    EXPECT_EQ(cfg.maxTextureResolution, 0);
    EXPECT_EQ(cfg.requireUvChannels, 0);
    EXPECT_FALSE(cfg.detectZeroWeightBones);
}

TEST(ScanConfigDefaultsTest, IncludePatternsContainCommonExtensions)
{
    const QStringList globs = ScanConfig::defaultIncludePatternsForAssimpImports();
    // Should contain a few well-known ones: mesh, tmd, rsd, fbx (from Assimp)
    auto contains = [&](const QString& tail) {
        for (const QString& g : globs)
            if (g.endsWith(tail, Qt::CaseInsensitive))
                return true;
        return false;
    };
    EXPECT_TRUE(contains(QStringLiteral(".mesh")));
    EXPECT_TRUE(contains(QStringLiteral(".tmd")));
    EXPECT_TRUE(contains(QStringLiteral(".rsd")));
    // FBX is registered through Assimp on standard builds.
    EXPECT_TRUE(contains(QStringLiteral(".fbx")));
}

TEST(ScanConfigDefaultsTest, IncludePatternsAreSorted)
{
    QStringList globs = ScanConfig::defaultIncludePatternsForAssimpImports();
    QStringList sorted = globs;
    sorted.sort(Qt::CaseInsensitive);
    EXPECT_EQ(globs, sorted);
}

// -------------------- fromVariantMap --------------------

TEST(ScanConfigFromVariantMapTest, EmptyMapReturnsDefaults)
{
    const ScanConfig cfg = ScanConfig::fromVariantMap({});
    const ScanConfig def = ScanConfig::defaults();
    EXPECT_EQ(cfg.version, def.version);
    EXPECT_EQ(cfg.maxFileSizeMb, def.maxFileSizeMb);
    EXPECT_EQ(cfg.failOn, def.failOn);
}

TEST(ScanConfigFromVariantMapTest, PopulatesAllRuleFields)
{
    QVariantMap root;
    root["version"] = 2;

    QVariantMap scan;
    scan["roots"] = QStringList{QStringLiteral("./assets")};
    scan["include"] = QStringList{QStringLiteral("**/*.fbx")};
    scan["exclude"] = QStringList{QStringLiteral("**/legacy/**")};
    root["scan"] = scan;

    QVariantMap rules;
    rules["allowed_formats"]         = QStringList{QStringLiteral("fbx"), QStringLiteral("glb")};
    rules["forbidden_extensions"]    = QStringList{QStringLiteral("dae")};
    rules["max_file_size_mb"]        = 25.5;
    rules["min_file_size_mb"]        = 0.05;
    rules["max_mesh_count"]          = 16;
    rules["min_mesh_count"]          = 1;
    rules["max_material_count"]      = 4;
    rules["min_material_count"]      = 1;
    rules["max_vertex_count"]        = 50000;
    rules["min_vertex_count"]        = 8;
    rules["max_acmr"]                = 1.5;
    rules["require_skeleton"]        = true;
    rules["require_animations"]      = true;
    rules["allow_embedded_textures"] = false;
    rules["require_textures_exist"]  = true;
    rules["allow_missing_materials"] = false;
    rules["file_name_case"]          = QStringLiteral("snake_case");
    rules["max_anim_keyframes"]      = 200;
    rules["min_anim_keyframes"]      = 2;
    rules["max_anim_duration"]       = 30.0;
    rules["min_anim_duration"]       = 0.2;
    rules["require_animation_names"] = QStringList{QStringLiteral("walk"), QStringLiteral("attack*")};
    rules["require_bone_names"]      = QStringList{QStringLiteral("root_*")};
    rules["redundant_keyframes_pct"]                  = 40.0;
    rules["redundant_keyframes_translation_tol"]      = 1e-3;
    rules["redundant_keyframes_rotation_deg_tol"]     = 1.0;
    rules["redundant_keyframes_scale_tol"]            = 1e-3;
    rules["max_texture_resolution"]   = 4096;
    rules["require_uv_channels"]      = 2;
    rules["detect_zero_weight_bones"] = true;
    rules["detect_overlapping_uvs_pct"] = 5.0;
    rules["detect_non_manifold_edges_pct"] = 1.0;
    root["rules"] = rules;

    QVariantMap fix;
    fix["enabled"]           = true;
    fix["dry_run"]           = true;
    fix["optimize_meshes"]   = true;
    fix["rename_animations"] = true;
    fix["convert_to_format"] = QStringLiteral("glb");
    fix["output_dir"]        = QStringLiteral("./out");
    root["fix"] = fix;

    QVariantMap report;
    report["format"]       = QStringLiteral("json");
    report["output"]       = QStringLiteral("report.json");
    report["sarif_output"] = QStringLiteral("report.sarif");
    report["fail_on"]      = QStringLiteral("warning");
    root["report"] = report;

    const ScanConfig cfg = ScanConfig::fromVariantMap(root);

    EXPECT_EQ(cfg.version, 2);
    EXPECT_EQ(cfg.roots, (QStringList{QStringLiteral("./assets")}));
    // fromVariantMap injects editor-only mesh globs (tmd/rsd/ply) when the
    // explicit include list omits them — assert the user's pattern survives
    // and the auto-injected extras are present, without coupling to the
    // injection list's exact contents/order.
    EXPECT_TRUE(cfg.includePatterns.contains(QStringLiteral("**/*.fbx")));
    EXPECT_TRUE(cfg.includePatterns.contains(QStringLiteral("**/*.tmd")));
    EXPECT_TRUE(cfg.includePatterns.contains(QStringLiteral("**/*.rsd")));
    EXPECT_TRUE(cfg.includePatterns.contains(QStringLiteral("**/*.ply")));
    EXPECT_EQ(cfg.excludePatterns, (QStringList{QStringLiteral("**/legacy/**")}));

    EXPECT_EQ(cfg.allowedFormats, (QStringList{QStringLiteral("fbx"), QStringLiteral("glb")}));
    EXPECT_EQ(cfg.forbiddenExtensions, (QStringList{QStringLiteral("dae")}));
    EXPECT_DOUBLE_EQ(cfg.maxFileSizeMb, 25.5);
    EXPECT_DOUBLE_EQ(cfg.minFileSizeMb, 0.05);
    EXPECT_EQ(cfg.maxMeshCount, 16);
    EXPECT_EQ(cfg.minMeshCount, 1);
    EXPECT_EQ(cfg.maxMaterialCount, 4);
    EXPECT_EQ(cfg.minMaterialCount, 1);
    EXPECT_EQ(cfg.maxVertexCount, 50000);
    EXPECT_EQ(cfg.minVertexCount, 8);
    EXPECT_DOUBLE_EQ(cfg.maxAcmr, 1.5);
    EXPECT_TRUE(cfg.requireSkeleton);
    EXPECT_TRUE(cfg.requireAnimations);
    EXPECT_FALSE(cfg.allowEmbeddedTextures);
    EXPECT_TRUE(cfg.requireTexturesExist);
    EXPECT_FALSE(cfg.allowMissingMaterials);
    EXPECT_EQ(cfg.fileNameCase, QStringLiteral("snake_case"));
    EXPECT_EQ(cfg.maxAnimKeyframes, 200);
    EXPECT_EQ(cfg.minAnimKeyframes, 2);
    EXPECT_DOUBLE_EQ(cfg.maxAnimDuration, 30.0);
    EXPECT_DOUBLE_EQ(cfg.minAnimDuration, 0.2);
    EXPECT_EQ(cfg.requireAnimationNames, (QStringList{QStringLiteral("walk"), QStringLiteral("attack*")}));
    EXPECT_EQ(cfg.requireBoneNames, (QStringList{QStringLiteral("root_*")}));
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesPctThreshold, 40.0);
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesTranslationTol, 1e-3);
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesRotationDegTol, 1.0);
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesScaleTol, 1e-3);
    EXPECT_EQ(cfg.maxTextureResolution, 4096);
    EXPECT_EQ(cfg.requireUvChannels, 2);
    EXPECT_TRUE(cfg.detectZeroWeightBones);
    EXPECT_DOUBLE_EQ(cfg.detectOverlappingUvsPct, 5.0);
    EXPECT_DOUBLE_EQ(cfg.detectNonManifoldEdgesPct, 1.0);

    EXPECT_TRUE(cfg.fixEnabled);
    EXPECT_TRUE(cfg.dryRun);
    EXPECT_TRUE(cfg.optimizeMeshes);
    EXPECT_TRUE(cfg.renameAnimations);
    EXPECT_EQ(cfg.convertToFormat, QStringLiteral("glb"));
    EXPECT_EQ(cfg.outputDir, QStringLiteral("./out"));

    EXPECT_EQ(cfg.reportFormat, QStringLiteral("json"));
    EXPECT_EQ(cfg.reportOutput, QStringLiteral("report.json"));
    EXPECT_EQ(cfg.sarifOutput, QStringLiteral("report.sarif"));
    EXPECT_EQ(cfg.failOn, QStringLiteral("warning"));
}

TEST(ScanConfigFromVariantMapTest, ScopesFromMapWithoutOrderUseAlphabetical)
{
    // JSON path: no _order key. Iteration order is alphabetical via QMap.
    QVariantMap root;
    QVariantMap scopes;
    QVariantMap chars;  chars["max_vertex_count"] = 5000;
    QVariantMap props; props["max_vertex_count"] = 1000;
    scopes["characters/**"] = chars;
    scopes["props/**"] = props;
    root["scopes"] = scopes;

    const ScanConfig cfg = ScanConfig::fromVariantMap(root);
    ASSERT_EQ(cfg.scopes.size(), 2);
    // QVariantMap iterates alphabetically: characters/** before props/**
    EXPECT_EQ(cfg.scopes[0].pathPattern, QStringLiteral("characters/**"));
    EXPECT_EQ(cfg.scopes[1].pathPattern, QStringLiteral("props/**"));
    EXPECT_EQ(cfg.scopes[0].rules.value("max_vertex_count").toInt(), 5000);
    EXPECT_EQ(cfg.scopes[1].rules.value("max_vertex_count").toInt(), 1000);
}

TEST(ScanConfigFromVariantMapTest, ScopesWithOrderHonorDeclarationOrder)
{
    QVariantMap root;
    QVariantMap scopes;
    QVariantMap chars;  chars["max_vertex_count"] = 5000;
    QVariantMap props; props["max_vertex_count"] = 1000;
    scopes["characters/**"] = chars;
    scopes["props/**"] = props;
    scopes["_order"] = QStringList{QStringLiteral("props/**"), QStringLiteral("characters/**")};
    root["scopes"] = scopes;

    const ScanConfig cfg = ScanConfig::fromVariantMap(root);
    ASSERT_EQ(cfg.scopes.size(), 2);
    EXPECT_EQ(cfg.scopes[0].pathPattern, QStringLiteral("props/**"));
    EXPECT_EQ(cfg.scopes[1].pathPattern, QStringLiteral("characters/**"));
}

// -------------------- fromJson --------------------

TEST(ScanConfigFromJsonTest, ParsesJsonObject)
{
    const QByteArray json = R"({
        "version": 3,
        "rules": { "max_mesh_count": 9 },
        "report": { "format": "json" }
    })";
    QJsonDocument doc = QJsonDocument::fromJson(json);
    ASSERT_FALSE(doc.isNull());
    const ScanConfig cfg = ScanConfig::fromJson(doc.object());
    EXPECT_EQ(cfg.version, 3);
    EXPECT_EQ(cfg.maxMeshCount, 9);
    EXPECT_EQ(cfg.reportFormat, QStringLiteral("json"));
}

// -------------------- loadFromFile --------------------

TEST(ScanConfigLoadFromFileTest, MissingFileReturnsDefaults)
{
    const ScanConfig cfg = ScanConfig::loadFromFile(QStringLiteral("/nonexistent/path/qtmesh.yml"));
    const ScanConfig def = ScanConfig::defaults();
    EXPECT_EQ(cfg.version, def.version);
    EXPECT_EQ(cfg.maxMeshCount, def.maxMeshCount);
    EXPECT_EQ(cfg.failOn, def.failOn);
}

TEST(ScanConfigLoadFromFileTest, LoadYamlFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("qtmesh.yml");
    writeFile(path, QByteArrayLiteral(
        "version: 2\n"
        "rules:\n"
        "  max_mesh_count: 5\n"
        "  require_skeleton: true\n"
        "report:\n"
        "  format: json\n"));
    const ScanConfig cfg = ScanConfig::loadFromFile(path);
    EXPECT_EQ(cfg.version, 2);
    EXPECT_EQ(cfg.maxMeshCount, 5);
    EXPECT_TRUE(cfg.requireSkeleton);
    EXPECT_EQ(cfg.reportFormat, QStringLiteral("json"));
}

TEST(ScanConfigLoadFromFileTest, LoadJsonFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("qtmesh.json");
    writeFile(path, R"({
        "version": 1,
        "rules": { "max_vertex_count": 100 },
        "fix":   { "dry_run": true }
    })");
    const ScanConfig cfg = ScanConfig::loadFromFile(path);
    EXPECT_EQ(cfg.maxVertexCount, 100);
    EXPECT_TRUE(cfg.dryRun);
}

TEST(ScanConfigLoadFromFileTest, InvalidJsonReturnsDefaults)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("bad.json");
    writeFile(path, QByteArrayLiteral("{ this is not json"));
    const ScanConfig cfg = ScanConfig::loadFromFile(path);
    // Falls back to defaults
    EXPECT_EQ(cfg.maxVertexCount, 0);
}

TEST(ScanConfigLoadFromFileTest, YamlMergesEditorOnlyMeshGlobsWhenIncludeMissingThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("qtmesh.yml");
    // User declares ONLY fbx — engine should still inject tmd / rsd / ply
    // so those non-Assimp formats keep getting scanned.
    writeFile(path, QByteArrayLiteral(
        "scan:\n"
        "  include: [\"**/*.fbx\"]\n"));
    const ScanConfig cfg = ScanConfig::loadFromFile(path);
    auto contains = [&](const QString& tail) {
        for (const QString& g : cfg.includePatterns)
            if (g.endsWith(tail, Qt::CaseInsensitive))
                return true;
        return false;
    };
    EXPECT_TRUE(contains(QStringLiteral(".fbx")));
    EXPECT_TRUE(contains(QStringLiteral(".tmd")));
    EXPECT_TRUE(contains(QStringLiteral(".rsd")));
    EXPECT_TRUE(contains(QStringLiteral(".ply")));
}

// -------------------- applyRuleOverrides + withScopeOverrides --------------------

TEST(ScanConfigApplyOverridesTest, ApplyRuleOverridesAffectsAllFields)
{
    ScanConfig cfg = ScanConfig::defaults();
    QVariantMap r;
    r["max_file_size_mb"]            = 99.0;
    r["min_file_size_mb"]            = 0.1;
    r["max_mesh_count"]              = 99;
    r["min_mesh_count"]              = 1;
    r["max_material_count"]          = 99;
    r["min_material_count"]          = 1;
    r["max_vertex_count"]            = 99;
    r["min_vertex_count"]            = 1;
    r["max_acmr"]                    = 2.5;
    r["require_skeleton"]            = true;
    r["require_animations"]          = true;
    r["allow_embedded_textures"]     = false;
    r["require_textures_exist"]      = true;
    r["allow_missing_materials"]     = false;
    r["file_name_case"]              = QStringLiteral("PascalCase");
    r["max_anim_keyframes"]          = 50;
    r["min_anim_keyframes"]          = 1;
    r["max_anim_duration"]           = 5.0;
    r["min_anim_duration"]           = 0.1;
    r["allowed_formats"]             = QStringList{QStringLiteral("fbx")};
    r["forbidden_extensions"]        = QStringList{QStringLiteral("3ds")};
    r["require_animation_names"]     = QStringList{QStringLiteral("idle")};
    r["require_bone_names"]          = QStringList{QStringLiteral("hip")};
    r["redundant_keyframes_pct"]              = 30.0;
    r["redundant_keyframes_translation_tol"]  = 1e-2;
    r["redundant_keyframes_rotation_deg_tol"] = 0.5;
    r["redundant_keyframes_scale_tol"]        = 1e-2;
    r["max_texture_resolution"]      = 2048;
    r["require_uv_channels"]         = 1;
    r["detect_zero_weight_bones"]    = true;
    r["detect_overlapping_uvs_pct"]  = 2.0;
    r["detect_non_manifold_edges_pct"] = 0.5;
    r["max_triangle_count"]            = 12000;
    r["max_triangles_per_mesh"]        = 8000;
    r["max_bones"]                     = 64;
    r["max_submesh_count"]             = 8;
    r["max_draw_calls"]                = 16;
    r["texture_not_power_of_two"]      = true;
    r["allowed_texture_formats"]       = QStringList{QStringLiteral("png")};
    r["disallowed_texture_formats"]    = QStringList{QStringLiteral("tga")};
    r["max_texture_dimension"]         = 1024;

    cfg.applyRuleOverrides(r);
    EXPECT_DOUBLE_EQ(cfg.maxFileSizeMb, 99.0);
    EXPECT_DOUBLE_EQ(cfg.minFileSizeMb, 0.1);
    EXPECT_EQ(cfg.maxMeshCount, 99);
    EXPECT_EQ(cfg.minMeshCount, 1);
    EXPECT_EQ(cfg.maxMaterialCount, 99);
    EXPECT_EQ(cfg.minMaterialCount, 1);
    EXPECT_EQ(cfg.maxVertexCount, 99);
    EXPECT_EQ(cfg.minVertexCount, 1);
    EXPECT_DOUBLE_EQ(cfg.maxAcmr, 2.5);
    EXPECT_TRUE(cfg.requireSkeleton);
    EXPECT_TRUE(cfg.requireAnimations);
    EXPECT_FALSE(cfg.allowEmbeddedTextures);
    EXPECT_TRUE(cfg.requireTexturesExist);
    EXPECT_FALSE(cfg.allowMissingMaterials);
    EXPECT_EQ(cfg.fileNameCase, QStringLiteral("PascalCase"));
    EXPECT_EQ(cfg.maxAnimKeyframes, 50);
    EXPECT_EQ(cfg.minAnimKeyframes, 1);
    EXPECT_DOUBLE_EQ(cfg.maxAnimDuration, 5.0);
    EXPECT_DOUBLE_EQ(cfg.minAnimDuration, 0.1);
    EXPECT_EQ(cfg.allowedFormats, (QStringList{QStringLiteral("fbx")}));
    EXPECT_EQ(cfg.forbiddenExtensions, (QStringList{QStringLiteral("3ds")}));
    EXPECT_EQ(cfg.requireAnimationNames, (QStringList{QStringLiteral("idle")}));
    EXPECT_EQ(cfg.requireBoneNames, (QStringList{QStringLiteral("hip")}));
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesPctThreshold, 30.0);
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesTranslationTol, 1e-2);
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesRotationDegTol, 0.5);
    EXPECT_DOUBLE_EQ(cfg.redundantKeyframesScaleTol, 1e-2);
    EXPECT_EQ(cfg.requireUvChannels, 1);
    EXPECT_TRUE(cfg.detectZeroWeightBones);
    EXPECT_DOUBLE_EQ(cfg.detectOverlappingUvsPct, 2.0);
    EXPECT_DOUBLE_EQ(cfg.detectNonManifoldEdgesPct, 0.5);
    EXPECT_EQ(cfg.maxTriangleCount, 12000);
    EXPECT_EQ(cfg.maxTrianglesPerMesh, 8000);
    EXPECT_EQ(cfg.maxBoneCount, 64);
    EXPECT_EQ(cfg.maxSubmeshCount, 8);
    EXPECT_EQ(cfg.maxDrawCalls, 16);
    EXPECT_TRUE(cfg.requireTexturePowerOfTwo);
    EXPECT_EQ(cfg.allowedTextureFormats, (QStringList{QStringLiteral("png")}));
    EXPECT_EQ(cfg.disallowedTextureFormats, (QStringList{QStringLiteral("tga")}));
    // max_texture_dimension alias overrides max_texture_resolution when both are set.
    EXPECT_EQ(cfg.maxTextureResolution, 1024);
}

TEST(ScanConfigApplyOverridesTest, ApplyRuleOverridesIgnoresUnknownKeys)
{
    ScanConfig cfg = ScanConfig::defaults();
    const int origMax = cfg.maxMeshCount;
    QVariantMap r;
    r["totally_unknown_key"] = 999;
    cfg.applyRuleOverrides(r);
    EXPECT_EQ(cfg.maxMeshCount, origMax);
}

TEST(ScanConfigApplyOverridesTest, WithScopeOverridesAppliesMatchingScope)
{
    ScanConfig cfg = ScanConfig::defaults();
    cfg.maxVertexCount = 1000;

    ScanScope s1;
    s1.pathPattern = QStringLiteral("characters/**");
    s1.rules.insert(QStringLiteral("max_vertex_count"), 5000);
    cfg.scopes.append(s1);

    ScanScope s2;
    s2.pathPattern = QStringLiteral("props/**");
    s2.rules.insert(QStringLiteral("max_vertex_count"), 200);
    cfg.scopes.append(s2);

    const ScanConfig chars = cfg.withScopeOverrides(QStringLiteral("characters/hero.fbx"));
    EXPECT_EQ(chars.maxVertexCount, 5000);

    const ScanConfig props = cfg.withScopeOverrides(QStringLiteral("props/barrel.fbx"));
    EXPECT_EQ(props.maxVertexCount, 200);

    const ScanConfig other = cfg.withScopeOverrides(QStringLiteral("other/door.fbx"));
    EXPECT_EQ(other.maxVertexCount, 1000);
}

TEST(ScanConfigApplyOverridesTest, LaterScopesOverrideEarlierWhenBothMatch)
{
    ScanConfig cfg = ScanConfig::defaults();
    cfg.maxVertexCount = 1000;

    ScanScope general;
    general.pathPattern = QStringLiteral("**");
    general.rules.insert(QStringLiteral("max_vertex_count"), 2000);
    cfg.scopes.append(general);

    ScanScope specific;
    specific.pathPattern = QStringLiteral("hero/**");
    specific.rules.insert(QStringLiteral("max_vertex_count"), 9000);
    cfg.scopes.append(specific);

    const ScanConfig result = cfg.withScopeOverrides(QStringLiteral("hero/x.fbx"));
    // Both match; later wins.
    EXPECT_EQ(result.maxVertexCount, 9000);
}

TEST(ScanConfigApplyOverridesTest, NoScopesLeavesConfigUntouched)
{
    ScanConfig cfg = ScanConfig::defaults();
    cfg.maxVertexCount = 1234;
    const ScanConfig out = cfg.withScopeOverrides(QStringLiteral("anything.fbx"));
    EXPECT_EQ(out.maxVertexCount, 1234);
}
