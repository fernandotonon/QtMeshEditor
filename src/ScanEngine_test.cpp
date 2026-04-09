#include <gtest/gtest.h>
#include "ScanConfig.h"
#include "ScanEngine.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

// ---------------------------------------------------------------------------
// YAML parser tests
// ---------------------------------------------------------------------------

TEST(ScanConfigTest, ParseSimpleYaml_ScalarValues)
{
    QString yaml =
        "version: 1\n"
        "\n"
        "rules:\n"
        "  max_file_size_mb: 50\n"
        "  require_skeleton: true\n"
        "  file_name_case: snake_case\n";

    QVariantMap map = parseSimpleYaml(yaml);
    EXPECT_EQ(map.value("version").toInt(), 1);

    QVariantMap rules = map.value("rules").toMap();
    EXPECT_EQ(rules.value("max_file_size_mb").toDouble(), 50.0);
    EXPECT_TRUE(rules.value("require_skeleton").toBool());
    EXPECT_EQ(rules.value("file_name_case").toString(), "snake_case");
}

TEST(ScanConfigTest, ParseSimpleYaml_InlineList)
{
    QString yaml =
        "rules:\n"
        "  allowed_formats: [fbx, glb, gltf, obj]\n"
        "  forbidden_extensions: [dae, 3ds]\n";

    QVariantMap map = parseSimpleYaml(yaml);
    QVariantMap rules = map.value("rules").toMap();
    QStringList formats = rules.value("allowed_formats").toStringList();
    EXPECT_EQ(formats.size(), 4);
    EXPECT_EQ(formats[0], "fbx");
    EXPECT_EQ(formats[3], "obj");
}

TEST(ScanConfigTest, ParseSimpleYaml_BlockList)
{
    QString yaml =
        "scan:\n"
        "  roots:\n"
        "    - assets/\n"
        "    - models/\n"
        "  include:\n"
        "    - \"**/*.fbx\"\n"
        "    - \"**/*.glb\"\n";

    QVariantMap map = parseSimpleYaml(yaml);
    QVariantMap scan = map.value("scan").toMap();
    QStringList roots = scan.value("roots").toStringList();
    EXPECT_EQ(roots.size(), 2);
    EXPECT_EQ(roots[0], "assets/");
    EXPECT_EQ(roots[1], "models/");

    QStringList include = scan.value("include").toStringList();
    EXPECT_EQ(include.size(), 2);
    EXPECT_EQ(include[0], "**/*.fbx");
}

TEST(ScanConfigTest, ParseSimpleYaml_Comments)
{
    QString yaml =
        "# top-level comment\n"
        "version: 1  # inline comment\n"
        "rules:\n"
        "  # section comment\n"
        "  max_file_size_mb: 25\n";

    QVariantMap map = parseSimpleYaml(yaml);
    EXPECT_EQ(map.value("version").toInt(), 1);
    EXPECT_EQ(map.value("rules").toMap().value("max_file_size_mb").toDouble(), 25.0);
}

TEST(ScanConfigTest, ParseSimpleYaml_Booleans)
{
    QString yaml =
        "rules:\n"
        "  require_skeleton: false\n"
        "  require_animations: no\n"
        "  allow_embedded_textures: yes\n";

    QVariantMap map = parseSimpleYaml(yaml);
    QVariantMap rules = map.value("rules").toMap();
    EXPECT_FALSE(rules.value("require_skeleton").toBool());
    EXPECT_FALSE(rules.value("require_animations").toBool());
    EXPECT_TRUE(rules.value("allow_embedded_textures").toBool());
}

TEST(ScanConfigTest, LoadFromVariantMap)
{
    QString yaml =
        "version: 1\n"
        "scan:\n"
        "  roots:\n"
        "    - assets/\n"
        "rules:\n"
        "  max_vertex_count: 50000\n"
        "  file_name_case: kebab-case\n"
        "fix:\n"
        "  enabled: true\n"
        "  dry_run: true\n"
        "report:\n"
        "  fail_on: warning\n";

    ScanConfig config = ScanConfig::fromVariantMap(parseSimpleYaml(yaml));
    EXPECT_EQ(config.roots.size(), 1);
    EXPECT_EQ(config.roots[0], "assets/");
    EXPECT_EQ(config.maxVertexCount, 50000);
    EXPECT_EQ(config.fileNameCase, "kebab-case");
    EXPECT_TRUE(config.fixEnabled);
    EXPECT_TRUE(config.dryRun);
    EXPECT_EQ(config.failOn, "warning");
}

// ---------------------------------------------------------------------------
// Glob matching tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, MatchesGlob_DoubleStarSlash)
{
    EXPECT_TRUE(ScanEngine::matchesGlob("models/player.fbx", "**/*.fbx"));
    EXPECT_TRUE(ScanEngine::matchesGlob("deep/nested/dir/model.fbx", "**/*.fbx"));
    EXPECT_TRUE(ScanEngine::matchesGlob("model.fbx", "**/*.fbx"));
    EXPECT_FALSE(ScanEngine::matchesGlob("model.obj", "**/*.fbx"));
}

TEST(ScanEngineTest, MatchesGlob_ExcludePatterns)
{
    EXPECT_TRUE(ScanEngine::matchesGlob("vendor/models/a.fbx", "**/vendor/**"));
    EXPECT_TRUE(ScanEngine::matchesGlob("assets/vendor/a.fbx", "**/vendor/**"));
    EXPECT_FALSE(ScanEngine::matchesGlob("assets/models/a.fbx", "**/vendor/**"));
}

TEST(ScanEngineTest, MatchesGlob_SingleStar)
{
    EXPECT_TRUE(ScanEngine::matchesGlob("model.fbx", "*.fbx"));
    EXPECT_FALSE(ScanEngine::matchesGlob("dir/model.fbx", "*.fbx"));
}

TEST(ScanEngineTest, MatchesGlob_CaseInsensitive)
{
    EXPECT_TRUE(ScanEngine::matchesGlob("Model.FBX", "**/*.fbx"));
}

// ---------------------------------------------------------------------------
// Name case validation tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, CheckNameCase_SnakeCase)
{
    EXPECT_TRUE(ScanEngine::checkNameCase("player_model.fbx", "snake_case"));
    EXPECT_TRUE(ScanEngine::checkNameCase("a.fbx", "snake_case"));
    EXPECT_TRUE(ScanEngine::checkNameCase("player_model_v2.fbx", "snake_case"));
    EXPECT_FALSE(ScanEngine::checkNameCase("PlayerModel.fbx", "snake_case"));
    EXPECT_FALSE(ScanEngine::checkNameCase("player-model.fbx", "snake_case"));
    EXPECT_FALSE(ScanEngine::checkNameCase("Player_Model.fbx", "snake_case"));
}

TEST(ScanEngineTest, CheckNameCase_KebabCase)
{
    EXPECT_TRUE(ScanEngine::checkNameCase("player-model.fbx", "kebab-case"));
    EXPECT_FALSE(ScanEngine::checkNameCase("player_model.fbx", "kebab-case"));
    EXPECT_FALSE(ScanEngine::checkNameCase("PlayerModel.fbx", "kebab-case"));
}

TEST(ScanEngineTest, CheckNameCase_PascalCase)
{
    EXPECT_TRUE(ScanEngine::checkNameCase("PlayerModel.fbx", "PascalCase"));
    EXPECT_FALSE(ScanEngine::checkNameCase("playerModel.fbx", "PascalCase"));
    EXPECT_FALSE(ScanEngine::checkNameCase("player_model.fbx", "PascalCase"));
}

TEST(ScanEngineTest, CheckNameCase_CamelCase)
{
    EXPECT_TRUE(ScanEngine::checkNameCase("playerModel.fbx", "camelCase"));
    EXPECT_FALSE(ScanEngine::checkNameCase("PlayerModel.fbx", "camelCase"));
    EXPECT_FALSE(ScanEngine::checkNameCase("player_model.fbx", "camelCase"));
}

TEST(ScanEngineTest, CheckNameCase_Lowercase)
{
    EXPECT_TRUE(ScanEngine::checkNameCase("playermodel.fbx", "lowercase"));
    EXPECT_TRUE(ScanEngine::checkNameCase("player_model.fbx", "lowercase"));
    EXPECT_FALSE(ScanEngine::checkNameCase("PlayerModel.fbx", "lowercase"));
}

// ---------------------------------------------------------------------------
// Name conversion tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, ConvertNameToCase_SnakeCase)
{
    EXPECT_EQ(ScanEngine::convertNameToCase("PlayerModel.fbx", "snake_case"), "player_model.fbx");
    EXPECT_EQ(ScanEngine::convertNameToCase("player-model.fbx", "snake_case"), "player_model.fbx");
    EXPECT_EQ(ScanEngine::convertNameToCase("NPC Guard.fbx", "snake_case"), "npc_guard.fbx");
}

TEST(ScanEngineTest, ConvertNameToCase_KebabCase)
{
    EXPECT_EQ(ScanEngine::convertNameToCase("PlayerModel.fbx", "kebab-case"), "player-model.fbx");
    // Underscores are replaced with hyphens in kebab conversion
    EXPECT_EQ(ScanEngine::convertNameToCase("player_model.fbx", "kebab-case"), "player-model.fbx");
}

// ---------------------------------------------------------------------------
// File enumeration tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, EnumerateFiles_Basic)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    // Create test files
    QFile(tmpDir.filePath("model.fbx")).open(QIODevice::WriteOnly);
    QFile(tmpDir.filePath("texture.png")).open(QIODevice::WriteOnly);

    QDir(tmpDir.path()).mkpath("subdir");
    QFile(tmpDir.filePath("subdir/nested.obj")).open(QIODevice::WriteOnly);

    ScanConfig config;
    config.includePatterns = {"**/*.fbx", "**/*.obj"};
    config.excludePatterns = {};

    QStringList files = ScanEngine::enumerateFiles(config, tmpDir.path());
    EXPECT_EQ(files.size(), 2);

    // Check that .png was filtered out
    bool hasPng = false;
    for (const auto& f : files)
        if (f.endsWith(".png")) hasPng = true;
    EXPECT_FALSE(hasPng);
}

TEST(ScanEngineTest, EnumerateFiles_ExcludePattern)
{
    QTemporaryDir tmpDir;
    ASSERT_TRUE(tmpDir.isValid());

    QDir(tmpDir.path()).mkpath("vendor");
    QFile(tmpDir.filePath("model.fbx")).open(QIODevice::WriteOnly);
    QFile(tmpDir.filePath("vendor/lib.fbx")).open(QIODevice::WriteOnly);

    ScanConfig config;
    config.includePatterns = {"**/*.fbx"};
    config.excludePatterns = {"**/vendor/**"};

    QStringList files = ScanEngine::enumerateFiles(config, tmpDir.path());
    EXPECT_EQ(files.size(), 1);
    EXPECT_TRUE(files[0].endsWith("model.fbx"));
}

// ---------------------------------------------------------------------------
// Rule evaluation tests (using synthetic AssetInfo, no Assimp needed)
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, EvaluateRules_MaxVertexCount)
{
    AssetInfo asset;
    asset.relativePath = "model.fbx";
    asset.format = "fbx";
    asset.vertexCount = 150000;

    ScanConfig config = ScanConfig::defaults();
    config.maxVertexCount = 100000;

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_EQ(findings.size(), 1);
    EXPECT_EQ(findings[0].rule, "max_vertex_count");
    EXPECT_EQ(findings[0].severity, Severity::Error);
}

TEST(ScanEngineTest, EvaluateRules_ForbiddenExtension)
{
    AssetInfo asset;
    asset.relativePath = "model.dae";
    asset.format = "dae";

    ScanConfig config = ScanConfig::defaults();
    config.forbiddenExtensions = {"dae", "3ds"};

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_GE(findings.size(), 1);
    bool found = false;
    for (const auto& f : findings)
        if (f.rule == "forbidden_extensions") found = true;
    EXPECT_TRUE(found);
}

TEST(ScanEngineTest, EvaluateRules_AllowedFormats)
{
    AssetInfo asset;
    asset.relativePath = "model.stl";
    asset.format = "stl";

    ScanConfig config = ScanConfig::defaults();
    config.allowedFormats = {"fbx", "glb", "gltf"};

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_GE(findings.size(), 1);
    EXPECT_EQ(findings[0].rule, "allowed_formats");
}

TEST(ScanEngineTest, EvaluateRules_MaxFileSize)
{
    AssetInfo asset;
    asset.relativePath = "huge.fbx";
    asset.format = "fbx";
    asset.fileSize = 60 * 1024 * 1024; // 60 MB

    ScanConfig config = ScanConfig::defaults();
    config.maxFileSizeMb = 50;

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_GE(findings.size(), 1);
    EXPECT_EQ(findings[0].rule, "max_file_size_mb");
}

TEST(ScanEngineTest, EvaluateRules_RequireSkeleton)
{
    AssetInfo asset;
    asset.relativePath = "static.fbx";
    asset.format = "fbx";
    asset.hasSkeleton = false;

    ScanConfig config = ScanConfig::defaults();
    config.requireSkeleton = true;

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_GE(findings.size(), 1);
    bool found = false;
    for (const auto& f : findings)
        if (f.rule == "require_skeleton") found = true;
    EXPECT_TRUE(found);
}

TEST(ScanEngineTest, EvaluateRules_FileNameCase)
{
    AssetInfo asset;
    asset.filePath = "/path/to/PlayerModel.fbx";
    asset.relativePath = "PlayerModel.fbx";
    asset.format = "fbx";

    ScanConfig config = ScanConfig::defaults();
    config.fileNameCase = "snake_case";

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_GE(findings.size(), 1);
    bool found = false;
    for (const auto& f : findings)
        if (f.rule == "file_name_case") { found = true; EXPECT_TRUE(f.fixable); }
    EXPECT_TRUE(found);
}

TEST(ScanEngineTest, EvaluateRules_PassesClean)
{
    AssetInfo asset;
    asset.filePath = "/path/to/player_model.fbx";
    asset.relativePath = "player_model.fbx";
    asset.format = "fbx";
    asset.fileSize = 1024 * 1024; // 1 MB
    asset.vertexCount = 5000;
    asset.meshCount = 1;
    asset.materialCount = 2;

    ScanConfig config = ScanConfig::defaults();
    config.maxVertexCount = 100000;
    config.maxFileSizeMb = 50;
    config.fileNameCase = "snake_case";

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_EQ(findings.size(), 0);
}

TEST(ScanEngineTest, EvaluateRules_MissingMaterials)
{
    AssetInfo asset;
    asset.relativePath = "model.fbx";
    asset.format = "fbx";
    asset.materialNames = {"GoodMaterial", "DefaultMaterial", "Metal"};

    ScanConfig config = ScanConfig::defaults();
    config.allowMissingMaterials = false;

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_GE(findings.size(), 1);
    bool found = false;
    for (const auto& f : findings)
        if (f.rule == "allow_missing_materials") found = true;
    EXPECT_TRUE(found);
}

// ---------------------------------------------------------------------------
// Formatter tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, FormatJson_Structure)
{
    ScanResult result;
    result.scanned = 2;
    result.passed = 1;
    result.errors = 1;
    result.elapsedMs = 1500;

    AssetInfo goodAsset;
    goodAsset.relativePath = "good.fbx";
    goodAsset.format = "fbx";
    result.assets.append(goodAsset);

    AssetInfo badAsset;
    badAsset.relativePath = "bad.fbx";
    badAsset.format = "fbx";
    badAsset.vertexCount = 200000;
    result.assets.append(badAsset);

    Finding f;
    f.file = "bad.fbx";
    f.rule = "max_vertex_count";
    f.severity = Severity::Error;
    f.message = "200000 vertices exceeds limit of 100000";
    result.findings.append(f);

    QString json = ScanEngine::formatJson(result);
    EXPECT_TRUE(json.contains("\"scanned\": 2"));
    EXPECT_TRUE(json.contains("\"max_vertex_count\""));
    EXPECT_TRUE(json.contains("bad.fbx"));
}

TEST(ScanEngineTest, FormatText_ContainsSummary)
{
    ScanResult result;
    result.scanned = 3;
    result.passed = 2;
    result.warnings = 1;
    result.elapsedMs = 500;

    AssetInfo asset;
    asset.relativePath = "ok.fbx";
    result.assets.append(asset);

    ScanConfig config;
    QString text = ScanEngine::formatText(result, config);
    EXPECT_TRUE(text.contains("Scanned:"));
    EXPECT_TRUE(text.contains("Passed:"));
}

TEST(ScanEngineTest, FormatSarif_ValidStructure)
{
    ScanResult result;
    Finding f;
    f.file = "model.fbx";
    f.rule = "max_vertex_count";
    f.severity = Severity::Error;
    f.message = "too many vertices";
    result.findings.append(f);

    QString sarif = ScanEngine::formatSarif(result);
    EXPECT_TRUE(sarif.contains("\"version\": \"2.1.0\""));
    EXPECT_TRUE(sarif.contains("qtmesh scan"));
    EXPECT_TRUE(sarif.contains("max_vertex_count"));
}

// ---------------------------------------------------------------------------
// Wildcard matching tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, MatchesWildcard_Basic)
{
    EXPECT_TRUE(ScanEngine::matchesWildcard("Walk", "Walk"));
    EXPECT_TRUE(ScanEngine::matchesWildcard("Attack1", "Attack*"));
    EXPECT_TRUE(ScanEngine::matchesWildcard("Attack_Heavy", "Attack*"));
    EXPECT_TRUE(ScanEngine::matchesWildcard("dance_01", "dance_*"));
    EXPECT_FALSE(ScanEngine::matchesWildcard("Walk", "Run"));
    EXPECT_FALSE(ScanEngine::matchesWildcard("Idle", "Attack*"));
}

TEST(ScanEngineTest, MatchesWildcard_QuestionMark)
{
    EXPECT_TRUE(ScanEngine::matchesWildcard("Attack1", "Attack?"));
    EXPECT_FALSE(ScanEngine::matchesWildcard("Attack12", "Attack?"));
}

TEST(ScanEngineTest, MatchesWildcard_CaseInsensitive)
{
    EXPECT_TRUE(ScanEngine::matchesWildcard("walk", "Walk"));
    EXPECT_TRUE(ScanEngine::matchesWildcard("ATTACK1", "attack*"));
}

// ---------------------------------------------------------------------------
// Animation/skeleton content rule tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, EvaluateRules_MaxAnimKeyframes)
{
    AssetInfo asset;
    asset.relativePath = "model.fbx";
    asset.format = "fbx";
    asset.animationCount = 2;
    asset.animationNames = {"Walk", "Run"};
    asset.animationDurations = {1.0, 0.8};
    asset.animationKeyframeCounts = {30, 200};

    ScanConfig config = ScanConfig::defaults();
    config.maxAnimKeyframes = 100;

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_EQ(findings.size(), 1);
    EXPECT_EQ(findings[0].rule, "max_anim_keyframes");
    EXPECT_TRUE(findings[0].message.contains("Run"));
}

TEST(ScanEngineTest, EvaluateRules_MaxAnimDuration)
{
    AssetInfo asset;
    asset.relativePath = "model.fbx";
    asset.format = "fbx";
    asset.animationCount = 2;
    asset.animationNames = {"Idle", "LongCutscene"};
    asset.animationDurations = {2.0, 60.0};
    asset.animationKeyframeCounts = {24, 720};

    ScanConfig config = ScanConfig::defaults();
    config.maxAnimDuration = 30.0;

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_EQ(findings.size(), 1);
    EXPECT_EQ(findings[0].rule, "max_anim_duration");
    EXPECT_TRUE(findings[0].message.contains("LongCutscene"));
}

TEST(ScanEngineTest, EvaluateRules_RequireAnimationNames)
{
    AssetInfo asset;
    asset.relativePath = "character.fbx";
    asset.format = "fbx";
    asset.animationCount = 3;
    asset.animationNames = {"Walk", "Run", "Attack1"};
    asset.hasSkeleton = true;
    asset.boneCount = 10;

    ScanConfig config = ScanConfig::defaults();
    config.requireAnimationNames = {"Walk", "Run", "Jump", "Attack*"};

    auto findings = ScanEngine::evaluateRules(asset, config);
    // Walk found, Run found, Attack* matches Attack1, but Jump is missing
    EXPECT_EQ(findings.size(), 1);
    EXPECT_EQ(findings[0].rule, "require_animation_names");
    EXPECT_TRUE(findings[0].message.contains("Jump"));
}

TEST(ScanEngineTest, EvaluateRules_RequireBoneNames)
{
    AssetInfo asset;
    asset.relativePath = "character.fbx";
    asset.format = "fbx";
    asset.hasSkeleton = true;
    asset.boneCount = 5;
    asset.boneNames = {"Hips", "Spine", "Head", "r_hand_attach", "l_hand_attach"};

    ScanConfig config = ScanConfig::defaults();
    config.requireBoneNames = {"r_hand_attach", "l_hand_attach", "backpack", "top_head"};

    auto findings = ScanEngine::evaluateRules(asset, config);
    // r_hand_attach and l_hand_attach found; backpack and top_head missing
    EXPECT_EQ(findings.size(), 2);
    bool foundBackpack = false, foundTopHead = false;
    for (const auto& f : findings) {
        EXPECT_EQ(f.rule, "require_bone_names");
        if (f.message.contains("backpack")) foundBackpack = true;
        if (f.message.contains("top_head")) foundTopHead = true;
    }
    EXPECT_TRUE(foundBackpack);
    EXPECT_TRUE(foundTopHead);
}

TEST(ScanEngineTest, EvaluateRules_RequireBoneNames_Wildcard)
{
    AssetInfo asset;
    asset.relativePath = "character.fbx";
    asset.format = "fbx";
    asset.hasSkeleton = true;
    asset.boneCount = 3;
    asset.boneNames = {"r_hand_attach", "l_hand_attach", "weapon_slot_01"};

    ScanConfig config = ScanConfig::defaults();
    config.requireBoneNames = {"*_hand_attach", "weapon_slot_*"};

    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_EQ(findings.size(), 0); // all patterns matched
}

// ---------------------------------------------------------------------------
// Scoped rule tests
// ---------------------------------------------------------------------------

TEST(ScanEngineTest, ScopedRules_OverridesGlobal)
{
    ScanConfig config = ScanConfig::defaults();
    config.maxVertexCount = 100000;
    config.requireSkeleton = false;

    ScanScope scope;
    scope.pathPattern = "characters/**";
    scope.rules["require_skeleton"] = true;
    scope.rules["max_vertex_count"] = 50000;
    config.scopes.append(scope);

    // Asset in characters/ should get overridden rules
    ScanConfig effective = config.withScopeOverrides("characters/hero.fbx");
    EXPECT_TRUE(effective.requireSkeleton);
    EXPECT_EQ(effective.maxVertexCount, 50000);

    // Asset outside characters/ should keep global rules
    ScanConfig global = config.withScopeOverrides("props/barrel.fbx");
    EXPECT_FALSE(global.requireSkeleton);
    EXPECT_EQ(global.maxVertexCount, 100000);
}

TEST(ScanEngineTest, ScopedRules_AnimationNames)
{
    AssetInfo asset;
    asset.relativePath = "characters/hero.fbx";
    asset.format = "fbx";
    asset.animationCount = 1;
    asset.animationNames = {"Walk"};
    asset.hasSkeleton = true;
    asset.boneCount = 1;
    asset.boneNames = {"Hips"};

    ScanConfig config = ScanConfig::defaults();
    // No global animation requirements

    ScanScope scope;
    scope.pathPattern = "characters/**";
    scope.rules["require_animation_names"] = QStringList{"Walk", "Run", "Jump"};
    scope.rules["require_bone_names"] = QStringList{"Hips", "weapon_slot"};
    config.scopes.append(scope);

    auto findings = ScanEngine::evaluateRules(asset, config);
    // Walk found, Run missing, Jump missing, Hips found, weapon_slot missing
    int animFindings = 0, boneFindings = 0;
    for (const auto& f : findings) {
        if (f.rule == "require_animation_names") animFindings++;
        if (f.rule == "require_bone_names") boneFindings++;
    }
    EXPECT_EQ(animFindings, 2); // Run and Jump missing
    EXPECT_EQ(boneFindings, 1); // weapon_slot missing
}

TEST(ScanEngineTest, ScopedRules_NoMatchDoesNotOverride)
{
    AssetInfo asset;
    asset.relativePath = "props/barrel.fbx";
    asset.format = "fbx";
    asset.vertexCount = 80000;

    ScanConfig config = ScanConfig::defaults();
    config.maxVertexCount = 100000;

    ScanScope scope;
    scope.pathPattern = "characters/**";
    scope.rules["max_vertex_count"] = 50000;
    config.scopes.append(scope);

    // props/barrel.fbx doesn't match characters/** so max stays at 100000
    auto findings = ScanEngine::evaluateRules(asset, config);
    EXPECT_EQ(findings.size(), 0); // 80000 < 100000
}

// ---------------------------------------------------------------------------
// YAML parser 3-level nesting tests (for scopes)
// ---------------------------------------------------------------------------

TEST(ScanConfigTest, ParseSimpleYaml_Scopes)
{
    QString yaml =
        "scopes:\n"
        "  \"characters/**\":\n"
        "    require_skeleton: true\n"
        "    max_vertex_count: 50000\n"
        "    require_animation_names:\n"
        "      - walk\n"
        "      - run\n"
        "  \"props/**\":\n"
        "    max_vertex_count: 5000\n";

    QVariantMap map = parseSimpleYaml(yaml);
    QVariantMap scopes = map.value("scopes").toMap();

    // Check characters scope
    QVariantMap chars = scopes.value("characters/**").toMap();
    EXPECT_TRUE(chars.value("require_skeleton").toBool());
    EXPECT_EQ(chars.value("max_vertex_count").toInt(), 50000);
    QStringList animNames = chars.value("require_animation_names").toStringList();
    EXPECT_EQ(animNames.size(), 2);
    EXPECT_EQ(animNames[0], "walk");
    EXPECT_EQ(animNames[1], "run");

    // Check props scope
    QVariantMap props = scopes.value("props/**").toMap();
    EXPECT_EQ(props.value("max_vertex_count").toInt(), 5000);
}

TEST(ScanConfigTest, ParseSimpleYaml_ScopesWithInlineList)
{
    QString yaml =
        "scopes:\n"
        "  \"characters/**\":\n"
        "    require_bone_names: [r_hand, l_hand, backpack]\n";

    QVariantMap map = parseSimpleYaml(yaml);
    QVariantMap chars = map.value("scopes").toMap().value("characters/**").toMap();
    QStringList bones = chars.value("require_bone_names").toStringList();
    EXPECT_EQ(bones.size(), 3);
    EXPECT_EQ(bones[0], "r_hand");
}

TEST(ScanConfigTest, LoadConfig_WithScopes)
{
    QString yaml =
        "version: 1\n"
        "rules:\n"
        "  max_vertex_count: 100000\n"
        "scopes:\n"
        "  \"characters/**\":\n"
        "    require_skeleton: true\n"
        "    require_animation_names: [walk, run]\n"
        "  \"props/**\":\n"
        "    max_vertex_count: 5000\n";

    ScanConfig config = ScanConfig::fromVariantMap(parseSimpleYaml(yaml));
    EXPECT_EQ(config.maxVertexCount, 100000);
    EXPECT_EQ(config.scopes.size(), 2);
    EXPECT_EQ(config.scopes[0].pathPattern, "characters/**");
    EXPECT_TRUE(config.scopes[0].rules.contains("require_skeleton"));
    EXPECT_EQ(config.scopes[1].pathPattern, "props/**");
}
