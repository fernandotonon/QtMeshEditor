#ifndef SCANENGINE_H
#define SCANENGINE_H

#include "ScanConfig.h"
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#ifdef QTMESH_UNIT_TESTS
#include <OgreMesh.h>
#endif

struct aiScene;

enum class Severity { Info, Warning, Error };

struct Finding {
    QString file;       // relative path
    QString rule;       // rule id (e.g. "max_vertex_count")
    Severity severity;
    QString message;
    bool fixable = false;
    bool fixed = false;
    /// True when a fix was attempted but intentionally skipped (e.g. would increase file size).
    /// This is distinct from asset-level "skipped" due to loadError.
    bool skipped = false;
    /// Positive number of bytes saved by applying the fix (0 when not applicable).
    qint64 bytesSaved = 0;
    /// Positive number of keyframes removed by applying the fix (0 when not applicable).
    qint64 keysRemoved = 0;
};

/// Per-mesh geometry summary (issue #364). Only the first kMaxMeshStatsEntries meshes are stored.
struct MeshStats {
    QString name;
    unsigned int vertexCount = 0;
    unsigned int triangleCount = 0;
    unsigned int submeshCount = 0;
};

/// Filesystem probe of one external texture reference (issue #364).
struct TextureRefStats {
    QString path;              // path as referenced in the asset
    QString resolvedPath;      // absolute path when found on disk
    int width = 0;
    int height = 0;
    qint64 fileSizeBytes = -1;
    QString format;            // lowercase extension without dot
    bool missing = false;
};

/// Options for inspectAsset (issue #364). Heavy texture probing is opt-in via profile metadata.
struct AssetInspectOptions {
    /// When true, stat() referenced texture files and read dimensions via QImageReader headers.
    bool probeTextureFiles = false;
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
    /// Triangulated triangle count (same as faceCount after Assimp triangulation / Ogre indices).
    unsigned int triangleCount = 0;
    /// Total submeshes across all meshes/entities in the asset.
    unsigned int submeshCount = 0;
    /// Largest triangle count among individual meshes/entities.
    unsigned int maxTrianglesPerMesh = 0;
    /// Capped per-mesh breakdown (see kMaxMeshStatsEntries).
    QList<MeshStats> meshStats;
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
    /// Fraction of atomic node keys that are redundant under balanced simplify tolerances (0..1), file-level aggregate.
    double animationRedundantKeyframeRatio = 0.0;

    /// Phase 6 slice C: weighted ACMR across all triangulated meshes (0 = unknown / no indices).
    /// Computed via VertexCacheOptimizer::computeAcmr on Assimp's flattened
    /// per-face indices, weighted by triangle count.
    double weightedAcmr = 0.0;

    // Redundant-keyframe analysis (filled when scan rule is active).
    // Total keyframes summed across all tracks of all animations.
    int totalKeyframes = 0;
    int redundantKeyframes = 0;

    // Phase 6 slice C4 — populated by the Ogre scene walk.
    // Maximum dimension (max(width, height)) of any texture bound through
    // any SubEntity's material. 0 when the asset has no bound textures.
    int maxTextureDimension = 0;
    /// Per-texture filesystem probe (issue #364); filled when AssetInspectOptions::probeTextureFiles.
    QList<TextureRefStats> textureStats;
    /// Max(width,height) from textureStats probe (0 when not probed or no files found).
    int probedTextureMaxDimension = 0;
    /// Rough VRAM estimate from probed textures: sum(width*height*4) per file (RGBA32 heuristic).
    qint64 estimatedTextureVramBytes = 0;
    /// Draw-call proxy: Ogre SubEntity count or Assimp mesh count (one batch per submesh/mesh).
    unsigned int estimatedDrawCalls = 0;
    /// Meshes whose names look like LOD variants (_lod, LOD1, etc.).
    unsigned int lodMeshCount = 0;
    // Per-submesh minimum UV-set count. Number of VES_TEXTURE_COORDINATES
    // elements in the vertex declaration of the *least*-equipped submesh.
    // 0 when no submeshes carry UVs at all (or the asset has no entities).
    int minUvChannelCount = 0;
    // Skeleton bones with no vertex-bone-assignment influence on any
    // submesh. Empty when the asset has no skeleton or every bone is used.
    QStringList zeroWeightBoneNames;
    // Fraction (0..1) of triangles whose UV0 AABB overlaps another
    // triangle's UV0 AABB. -1 when UV0 isn't present or the asset has no
    // entities. Conservative upper bound on true UV overlap.
    double overlappingUvsRatio = -1.0;
    // Fraction (0..1) of edges shared by something other than exactly
    // two triangulated faces. -1 when the asset has no triangulated
    // submeshes. Non-manifold input breaks boolean ops / fluid sims /
    // 3D printing.
    double nonManifoldEdgesRatio = -1.0;

    bool loadError = false;
    QString errorMessage;

    /// Maximum meshStats entries stored per asset (issue #364).
    static constexpr int kMaxMeshStatsEntries = 16;
    /// Maximum textureStats entries stored per asset.
    static constexpr int kMaxTextureStatsEntries = 32;
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
    qint64 bytesSaved = 0;
    qint64 keysRemoved = 0;
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

    /// Inspect a single asset file (Assimp for most formats; PlayStation TMD / Psy-Q PLY / RSD
    /// use the same Ogre importers as the editor, with a headless render target when needed).
    /// Texture filesystem probes run only when @p options.probeTextureFiles is true (e.g. from
    /// platform profile metadata `inspect_textures: true`).
    static AssetInfo inspectAsset(const QString& filePath, const QString& scanRoot,
                                  const AssetInspectOptions& options = {});

    /// After `Assimp::Importer::ReadFile`, whether the result would make `inspectAsset` set
    /// `loadError` (e.g. null scene, or no mesh and no animations). A scene may have
    /// the incomplete flag set and still be acceptable if it has animations. Unit-tested.
    static bool isAssimpResultLoadFailure(const aiScene* scene, const char* assimpErrorString,
                                         QString* outErrorMessage = nullptr);

    /// Evaluate all configured rules against one asset.
    static QList<Finding> evaluateRules(const AssetInfo& asset, const ScanConfig& config);

    /// Apply safe auto-fixes for findings that support it.
    /// \a scanRoot is the directory \c asset.relativePath is relative to (same as \c ScanEngine::run).
    static void applyFixes(const ScanConfig& config, const QString& scanRoot, AssetInfo& asset,
                           QList<Finding>& findings);

    // --- Formatters ---

    static QString formatText(const ScanResult& result, const ScanConfig& config, bool colorize = false,
                              const QString& activeProfileId = QString());
    /// Canonical JSON object for `--json`, report files, and QtMesh Cloud upload (identical schema).
    static QJsonObject scanReportToJsonObject(const ScanResult& result);

    /// Merge `GITHUB_*` environment (when set, e.g. in GitHub Actions) into `meta` for `/v1/ingest/scan` (prefers GITHUB_HEAD_REF over GITHUB_REF_NAME for branch).
    static QJsonObject mergeGithubActionsMetaIntoReport(const QJsonObject& report);

    /// UTC ISO-8601 timestamps for reports (`scanStartedUtc` / `scanCompletedUtc`); missing values use `scanCompletedUtc` or current UTC.
    static void scanReportUtcTimes(const ScanResult& result, QString* scanStartedUtc, QString* scanCompletedUtc);
    static QString formatJson(const ScanResult& result);
    static QString formatSarif(const ScanResult& result, const QString& activeProfileId = QString());

    // --- Helpers (public for testing) ---

    static bool matchesGlob(const QString& path, const QString& pattern);
    static bool matchesWildcard(const QString& text, const QString& pattern);
    static bool checkNameCase(const QString& fileName, const QString& convention);
    static QString convertNameToCase(const QString& fileName, const QString& convention);

#ifdef QTMESH_UNIT_TESTS
    /// Fills \a info geometry fields from an in-memory Ogre mesh (same logic as scan Ogre inspect).
    static void testApplyOgreMeshInspectCounts(AssetInfo& info, const Ogre::MeshPtr& mesh);
#endif
};

#endif // SCANENGINE_H
