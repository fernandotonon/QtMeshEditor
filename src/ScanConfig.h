#ifndef SCANCONFIG_H
#define SCANCONFIG_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QVariantMap>
#include <QList>

/// A scoped rule override — applies rule overrides to assets matching a path pattern.
struct ScanScope {
    QString pathPattern;    // glob pattern (e.g. "characters/**")
    QVariantMap rules;      // rule key-value overrides from YAML/JSON
};

struct ScanConfig {
    int version = 1;

    // scan section
    QStringList roots;
    /// Glob patterns; default ctor fills with all Assimp import extensions (plus Ogre .mesh / .mesh.xml).
    QStringList includePatterns;
    QStringList excludePatterns = {
        "**/node_modules/**", "**/.git/**", "**/build/**", "**/Build/**"
    };

    // rules section — existence checks
    QStringList allowedFormats;       // empty = all supported formats allowed
    QStringList forbiddenExtensions;
    double maxFileSizeMb = 0;         // 0 = no limit
    double minFileSizeMb = 0;         // 0 = no limit; catch stub/empty files
    int maxMeshCount = 0;             // 0 = no limit
    int minMeshCount = 0;
    int maxMaterialCount = 0;
    int minMaterialCount = 0;
    int maxVertexCount = 0;
    int minVertexCount = 0;
    bool requireSkeleton = false;     // true = error if no skeleton
    bool requireAnimations = false;   // true = error if no animations
    bool allowEmbeddedTextures = true;
    bool requireTexturesExist = false;
    bool allowMissingMaterials = true;
    QString fileNameCase;             // snake_case, kebab-case, camelCase, PascalCase, lowercase

    // rules section — animation/skeleton content validation
    int maxAnimKeyframes = 0;         // 0 = no limit; max keyframes per animation
    int minAnimKeyframes = 0;         // 0 = no limit; catch degenerate 1-keyframe anims
    double maxAnimDuration = 0;       // 0 = no limit; max duration in seconds
    double minAnimDuration = 0;       // 0 = no limit; catch too-short anims
    QStringList requireAnimationNames; // wildcard patterns: walk, run, "attack*", "dance_*"
    QStringList requireBoneNames;      // wildcard patterns: r_hand_attach, top_head

    // scoped rules — path-specific overrides
    QList<ScanScope> scopes;

    // fix section
    bool fixEnabled = false;
    bool dryRun = false;
    bool optimizeMeshes = false;
    bool renameAnimations = false;
    QString convertToFormat;
    QString outputDir;

    // report section
    QString reportFormat = "text";    // text, json, both
    QString reportOutput;
    QString sarifOutput;
    QString failOn = "error";         // info, warning, error, never

    ScanConfig();

    /// `**/*.<ext>` for every file extension registered by Assimp importers, plus `mesh` / `mesh.xml`.
    static QStringList defaultIncludePatternsForAssimpImports();

    static ScanConfig defaults();
    static ScanConfig loadFromFile(const QString& path);
    static ScanConfig fromVariantMap(const QVariantMap& map);
    static ScanConfig fromJson(const QJsonObject& obj);

    /// Return a copy with scope overrides applied for the given relative path.
    /// Scopes are matched in order; later scopes override earlier ones.
    ScanConfig withScopeOverrides(const QString& relativePath) const;

    /// Apply rule overrides from a QVariantMap (used by scope merging).
    void applyRuleOverrides(const QVariantMap& rules);
};

/// Minimal YAML parser for the qtmesh.yml config schema.
/// Handles scalars, inline lists [a, b], block lists (- item),
/// and two levels of section nesting (for scopes).
/// Not a general-purpose YAML parser.
QVariantMap parseSimpleYaml(const QString& content);

#endif // SCANCONFIG_H
