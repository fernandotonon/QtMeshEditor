#ifndef SCANENGINE_H
#define SCANENGINE_H

#include "ScanConfig.h"
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

struct aiScene;

enum class Severity { Info, Warning, Error };

struct Finding {
    QString file;       // relative path
    QString rule;       // rule id (e.g. "max_vertex_count")
    Severity severity;
    QString message;
    bool fixable = false;
    bool fixed = false;
};

struct AssetInfo {
    QString filePath;       // absolute
    QString relativePath;   // relative to scan root
    QString format;         // extension, lowercase
    qint64 fileSize = 0;    // bytes

    unsigned int meshCount = 0;
    unsigned int materialCount = 0;
    unsigned int animationCount = 0;
    unsigned int vertexCount = 0;
    unsigned int faceCount = 0;
    unsigned int boneCount = 0;
    unsigned int textureRefCount = 0;
    bool hasSkeleton = false;
    bool hasEmbeddedTextures = false;

    QStringList materialNames;
    QStringList texturePaths;   // external texture references

    // Animation/skeleton content details
    QStringList animationNames;
    QList<double> animationDurations;      // seconds per animation
    QList<int> animationKeyframeCounts;    // max keyframes per animation
    QStringList boneNames;                 // unique bone names

    bool loadError = false;
    QString errorMessage;
};

struct ScanResult {
    QList<AssetInfo> assets;
    QList<Finding> findings;

    int scanned  = 0;
    int passed   = 0;
    int warnings = 0;
    int errors   = 0;
    int infos    = 0;
    int fixed    = 0;
    int skipped  = 0;
    double elapsedMs = 0;

    /// Wall-clock bounds for reports, always UTC (`yyyy-MM-dd'T'HH:mm:ss.zzzZ`). Set by `ScanEngine::run`.
    QString scanStartedUtc;
    QString scanCompletedUtc;
};

class ScanEngine {
public:
    ScanEngine() = delete;

    using AssetProcessedCallback = std::function<void(const AssetInfo&, const QList<Finding>&)>;

    /// Run the full scan pipeline: enumerate → inspect → evaluate → (fix) → report.
    static ScanResult run(const ScanConfig& config, const QString& rootOverride = {},
                          const AssetProcessedCallback& onAssetProcessed = {});

    // --- Individual pipeline stages (public for testing) ---

    /// Recursively enumerate asset files under scanRoot filtered by config patterns.
    static QStringList enumerateFiles(const ScanConfig& config, const QString& scanRoot);

    /// Inspect a single asset file using Assimp (lightweight, no Ogre needed).
    static AssetInfo inspectAsset(const QString& filePath, const QString& scanRoot);

    /// After `Assimp::Importer::ReadFile`, whether the result would make `inspectAsset` set
    /// `loadError` (e.g. null scene, or no mesh and no animations). A scene may have
    /// the incomplete flag set and still be acceptable if it has animations. Unit-tested.
    static bool isAssimpResultLoadFailure(const aiScene* scene, const char* assimpErrorString,
                                         QString* outErrorMessage = nullptr);

    /// Evaluate all configured rules against one asset.
    static QList<Finding> evaluateRules(const AssetInfo& asset, const ScanConfig& config);

    /// Apply safe auto-fixes for findings that support it.
    static void applyFixes(const ScanConfig& config, AssetInfo& asset,
                           QList<Finding>& findings);

    // --- Formatters ---

    static QString formatText(const ScanResult& result, const ScanConfig& config, bool colorize = false);
    /// Canonical JSON object for `--json`, report files, and QtMesh Cloud upload (identical schema).
    static QJsonObject scanReportToJsonObject(const ScanResult& result);

    /// Merge `GITHUB_*` environment (when set, e.g. in GitHub Actions) into `meta` for `/v1/ingest/scan` (prefers GITHUB_HEAD_REF over GITHUB_REF_NAME for branch).
    static QJsonObject mergeGithubActionsMetaIntoReport(const QJsonObject& report);

    /// UTC ISO-8601 timestamps for reports (`scanStartedUtc` / `scanCompletedUtc`); missing values use `scanCompletedUtc` or current UTC.
    static void scanReportUtcTimes(const ScanResult& result, QString* scanStartedUtc, QString* scanCompletedUtc);
    static QString formatJson(const ScanResult& result);
    static QString formatSarif(const ScanResult& result);

    // --- Helpers (public for testing) ---

    static bool matchesGlob(const QString& path, const QString& pattern);
    static bool matchesWildcard(const QString& text, const QString& pattern);
    static bool checkNameCase(const QString& fileName, const QString& convention);
    static QString convertNameToCase(const QString& fileName, const QString& convention);
};

#endif // SCANENGINE_H
