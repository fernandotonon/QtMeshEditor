#include "ScanEngine.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTextStream>

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include "SentryReporter.h"

#include <algorithm>
#include <set>

// ---------------------------------------------------------------------------
// Glob matching
// ---------------------------------------------------------------------------

bool ScanEngine::matchesGlob(const QString& path, const QString& pattern)
{
    // Convert a simplified glob to a QRegularExpression:
    //   **/  → matches zero or more directory components
    //   **   → matches anything (including /)
    //   *    → matches anything except /
    //   ?    → matches single char except /
    QString escaped = QRegularExpression::escape(pattern);
    // Qt 6.9+ escapes '/' to '\/' — handle both escaped and unescaped slashes.
    // Must process **/ before ** before * to avoid partial matches.
    escaped.replace("\\/", "/");            // normalize: \/ → /
    escaped.replace("\\*\\*/", "(.+/)?");  // **/ → zero or more dirs
    escaped.replace("\\*\\*", ".*");       // **  → anything
    escaped.replace("\\*", "[^/]*");       // *   → anything except /
    escaped.replace("\\?", "[^/]");        // ?   → single non-/
    QRegularExpression re("^" + escaped + "$",
                          QRegularExpression::CaseInsensitiveOption);
    return re.match(path).hasMatch();
}

bool ScanEngine::matchesWildcard(const QString& text, const QString& pattern)
{
    // Simple wildcard matching for names (not paths): * = anything, ? = one char
    QString escaped = QRegularExpression::escape(pattern);
    escaped.replace("\\/", "/");
    escaped.replace("\\*", ".*");
    escaped.replace("\\?", ".");
    QRegularExpression re("^" + escaped + "$",
                          QRegularExpression::CaseInsensitiveOption);
    return re.match(text).hasMatch();
}

// ---------------------------------------------------------------------------
// File enumeration
// ---------------------------------------------------------------------------

QStringList ScanEngine::enumerateFiles(const ScanConfig& config, const QString& scanRoot)
{
    QStringList result;
    QDir rootDir(scanRoot);
    if (!rootDir.exists()) return result;

    QDirIterator it(rootDir.absolutePath(), QDir::Files | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        it.next();
        QString absPath = it.filePath();
        QString relPath = rootDir.relativeFilePath(absPath);

        // Check excludes first
        bool excluded = false;
        for (const auto& pattern : config.excludePatterns) {
            if (matchesGlob(relPath, pattern)) { excluded = true; break; }
        }
        if (excluded) continue;

        // Check includes (if empty, accept everything)
        if (!config.includePatterns.isEmpty()) {
            bool included = false;
            for (const auto& pattern : config.includePatterns) {
                if (matchesGlob(relPath, pattern)) { included = true; break; }
            }
            if (!included) continue;
        }

        result.append(absPath);
    }

    result.sort(Qt::CaseInsensitive);
    return result;
}

// ---------------------------------------------------------------------------
// Asset inspection via Assimp (lightweight — no Ogre needed)
// ---------------------------------------------------------------------------

AssetInfo ScanEngine::inspectAsset(const QString& filePath, const QString& scanRoot)
{
    AssetInfo info;
    info.filePath = filePath;
    info.relativePath = QDir(scanRoot).relativeFilePath(filePath);
    info.format = QFileInfo(filePath).suffix().toLower();
    info.fileSize = QFileInfo(filePath).size();

    Assimp::Importer importer;
    // Triangulate for consistent vertex/face counts; otherwise minimal processing.
    const aiScene* scene = importer.ReadFile(
        filePath.toStdString(),
        aiProcess_Triangulate | aiProcess_ValidateDataStructure);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        info.loadError = true;
        info.errorMessage = QString::fromUtf8(importer.GetErrorString());
        return info;
    }

    info.meshCount     = scene->mNumMeshes;
    info.materialCount = scene->mNumMaterials;
    info.animationCount = scene->mNumAnimations;

    // Vertex & face counts + skeleton detection
    std::set<std::string> uniqueBones;
    for (unsigned i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        info.vertexCount += mesh->mNumVertices;
        info.faceCount   += mesh->mNumFaces;
        for (unsigned b = 0; b < mesh->mNumBones; ++b)
            uniqueBones.insert(mesh->mBones[b]->mName.C_Str());
    }
    info.boneCount  = static_cast<unsigned int>(uniqueBones.size());
    info.hasSkeleton = !uniqueBones.empty();
    for (const auto& boneName : uniqueBones)
        info.boneNames.append(QString::fromStdString(boneName));

    // Animation details
    for (unsigned i = 0; i < scene->mNumAnimations; ++i) {
        const aiAnimation* anim = scene->mAnimations[i];
        info.animationNames.append(QString::fromUtf8(anim->mName.C_Str()));

        double ticksPerSec = anim->mTicksPerSecond > 0 ? anim->mTicksPerSecond : 25.0;
        info.animationDurations.append(anim->mDuration / ticksPerSec);

        unsigned maxKeys = 0;
        for (unsigned c = 0; c < anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = anim->mChannels[c];
            maxKeys = std::max(maxKeys, ch->mNumPositionKeys);
            maxKeys = std::max(maxKeys, ch->mNumRotationKeys);
            maxKeys = std::max(maxKeys, ch->mNumScalingKeys);
        }
        info.animationKeyframeCounts.append(static_cast<int>(maxKeys));
    }

    // Material names + texture references
    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* mat = scene->mMaterials[i];

        aiString name;
        mat->Get(AI_MATKEY_NAME, name);
        info.materialNames.append(QString::fromUtf8(name.C_Str()));

        // Iterate all texture types
        for (int type = aiTextureType_DIFFUSE; type <= aiTextureType_UNKNOWN; ++type) {
            unsigned count = mat->GetTextureCount(static_cast<aiTextureType>(type));
            for (unsigned j = 0; j < count; ++j) {
                aiString texPath;
                mat->GetTexture(static_cast<aiTextureType>(type), j, &texPath);
                QString tp = QString::fromUtf8(texPath.C_Str());
                info.textureRefCount++;
                if (tp.startsWith('*')) {
                    // Embedded texture (Assimp convention: "*0", "*1", ...)
                    info.hasEmbeddedTextures = true;
                } else if (!tp.isEmpty()) {
                    info.texturePaths.append(tp);
                }
            }
        }
    }

    // Deduplicate embedded-texture flag from scene
    if (scene->mNumTextures > 0)
        info.hasEmbeddedTextures = true;

    return info;
}

// ---------------------------------------------------------------------------
// Name case helpers
// ---------------------------------------------------------------------------

bool ScanEngine::checkNameCase(const QString& fileName, const QString& convention)
{
    QString stem = QFileInfo(fileName).completeBaseName();
    if (stem.isEmpty()) return true;

    if (convention == "snake_case") {
        static QRegularExpression re("^[a-z0-9]+(_[a-z0-9]+)*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "kebab-case") {
        static QRegularExpression re("^[a-z0-9]+(-[a-z0-9]+)*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "camelCase") {
        static QRegularExpression re("^[a-z][a-zA-Z0-9]*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "PascalCase") {
        static QRegularExpression re("^[A-Z][a-zA-Z0-9]*$");
        return re.match(stem).hasMatch();
    }
    if (convention == "lowercase") {
        return stem == stem.toLower();
    }

    return true; // unknown convention → pass
}

QString ScanEngine::convertNameToCase(const QString& fileName, const QString& convention)
{
    QString stem = QFileInfo(fileName).completeBaseName();
    QString ext  = QFileInfo(fileName).suffix();

    if (convention == "snake_case") {
        QString result;
        for (int i = 0; i < stem.size(); ++i) {
            QChar c = stem[i];
            if (c.isUpper() && i > 0 && stem[i - 1] != '_' && !stem[i - 1].isUpper())
                result += '_';
            result += c.toLower();
        }
        result.replace('-', '_').replace(' ', '_');
        // Collapse runs of underscores
        static QRegularExpression multiUnderscore("_+");
        result.replace(multiUnderscore, "_");
        return result + "." + ext;
    }
    if (convention == "kebab-case") {
        QString result;
        for (int i = 0; i < stem.size(); ++i) {
            QChar c = stem[i];
            if (c.isUpper() && i > 0 && stem[i - 1] != '-' && !stem[i - 1].isUpper())
                result += '-';
            result += c.toLower();
        }
        result.replace('_', '-').replace(' ', '-');
        static QRegularExpression multiDash("-+");
        result.replace(multiDash, "-");
        return result + "." + ext;
    }
    if (convention == "lowercase") {
        return stem.toLower() + "." + ext;
    }

    return fileName; // no conversion for camelCase/PascalCase (ambiguous from arbitrary input)
}

// ---------------------------------------------------------------------------
// Rule evaluation
// ---------------------------------------------------------------------------

QList<Finding> ScanEngine::evaluateRules(const AssetInfo& asset, const ScanConfig& globalConfig)
{
    // Apply scoped rule overrides for this asset's path
    ScanConfig config = globalConfig.withScopeOverrides(asset.relativePath);

    QList<Finding> findings;

    if (asset.loadError) {
        findings.append({asset.relativePath, "load_error", Severity::Error,
                         QString("Failed to load: %1").arg(asset.errorMessage)});
        return findings;
    }

    // ---- allowed_formats ----
    if (!config.allowedFormats.isEmpty()) {
        bool allowed = false;
        for (const auto& fmt : config.allowedFormats) {
            if (asset.format.compare(fmt, Qt::CaseInsensitive) == 0) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            findings.append({asset.relativePath, "allowed_formats", Severity::Error,
                             QString(".%1 is not an allowed format (allowed: %2)")
                                 .arg(asset.format, config.allowedFormats.join(", "))});
        }
    }

    // ---- forbidden_extensions ----
    for (const auto& ext : config.forbiddenExtensions) {
        if (asset.format.compare(ext, Qt::CaseInsensitive) == 0) {
            findings.append({asset.relativePath, "forbidden_extensions", Severity::Error,
                             QString(".%1 is a forbidden format").arg(asset.format)});
            break;
        }
    }

    // ---- max/min_file_size_mb ----
    {
        double sizeMb = asset.fileSize / (1024.0 * 1024.0);
        if (config.maxFileSizeMb > 0 && sizeMb > config.maxFileSizeMb)
            findings.append({asset.relativePath, "max_file_size_mb", Severity::Error,
                             QString("%.2f MB exceeds maximum of %.1f MB")
                                 .arg(sizeMb).arg(config.maxFileSizeMb)});
        if (config.minFileSizeMb > 0 && sizeMb < config.minFileSizeMb)
            findings.append({asset.relativePath, "min_file_size_mb", Severity::Warning,
                             QString("%.2f MB is below minimum of %.2f MB")
                                 .arg(sizeMb).arg(config.minFileSizeMb)});
    }

    // ---- max/min_mesh_count ----
    if (config.maxMeshCount > 0 && static_cast<int>(asset.meshCount) > config.maxMeshCount)
        findings.append({asset.relativePath, "max_mesh_count", Severity::Error,
                         QString("%1 meshes exceeds limit of %2")
                             .arg(asset.meshCount).arg(config.maxMeshCount)});
    if (config.minMeshCount > 0 && static_cast<int>(asset.meshCount) < config.minMeshCount)
        findings.append({asset.relativePath, "min_mesh_count", Severity::Warning,
                         QString("%1 meshes is below minimum of %2")
                             .arg(asset.meshCount).arg(config.minMeshCount)});

    // ---- max/min_material_count ----
    if (config.maxMaterialCount > 0 && static_cast<int>(asset.materialCount) > config.maxMaterialCount)
        findings.append({asset.relativePath, "max_material_count", Severity::Error,
                         QString("%1 materials exceeds limit of %2")
                             .arg(asset.materialCount).arg(config.maxMaterialCount)});
    if (config.minMaterialCount > 0 && static_cast<int>(asset.materialCount) < config.minMaterialCount)
        findings.append({asset.relativePath, "min_material_count", Severity::Warning,
                         QString("%1 materials is below minimum of %2")
                             .arg(asset.materialCount).arg(config.minMaterialCount)});

    // ---- max/min_vertex_count ----
    if (config.maxVertexCount > 0 && static_cast<int>(asset.vertexCount) > config.maxVertexCount)
        findings.append({asset.relativePath, "max_vertex_count", Severity::Error,
                         QString("%1 vertices exceeds limit of %2")
                             .arg(asset.vertexCount).arg(config.maxVertexCount)});
    if (config.minVertexCount > 0 && static_cast<int>(asset.vertexCount) < config.minVertexCount)
        findings.append({asset.relativePath, "min_vertex_count", Severity::Warning,
                         QString("%1 vertices is below minimum of %2")
                             .arg(asset.vertexCount).arg(config.minVertexCount)});

    // ---- require_skeleton ----
    if (config.requireSkeleton && !asset.hasSkeleton) {
        findings.append({asset.relativePath, "require_skeleton", Severity::Error,
                         "No skeleton found (require_skeleton is enabled)"});
    }

    // ---- require_animations ----
    if (config.requireAnimations && asset.animationCount == 0) {
        findings.append({asset.relativePath, "require_animations", Severity::Error,
                         "No animations found (require_animations is enabled)"});
    }

    // ---- allow_embedded_textures ----
    if (!config.allowEmbeddedTextures && asset.hasEmbeddedTextures) {
        findings.append({asset.relativePath, "allow_embedded_textures", Severity::Warning,
                         "Asset contains embedded textures (allow_embedded_textures is false)"});
    }

    // ---- require_textures_exist ----
    if (config.requireTexturesExist) {
        QDir assetDir = QFileInfo(asset.filePath).dir();
        for (const auto& texPath : asset.texturePaths) {
            // Try relative to asset directory
            if (!QFileInfo::exists(assetDir.filePath(texPath))) {
                findings.append({asset.relativePath, "require_textures_exist", Severity::Warning,
                                 QString("Referenced texture not found: %1").arg(texPath)});
            }
        }
    }

    // ---- allow_missing_materials ----
    if (!config.allowMissingMaterials) {
        for (const auto& name : asset.materialNames) {
            if (name.isEmpty() || name == "DefaultMaterial" || name == "(null)" ||
                name.startsWith("AI_DEFAULT") || name == "None") {
                findings.append({asset.relativePath, "allow_missing_materials", Severity::Warning,
                                 QString("Placeholder/missing material detected: '%1'").arg(name)});
            }
        }
    }

    // ---- file_name_case ----
    if (!config.fileNameCase.isEmpty()) {
        QString fileName = QFileInfo(asset.filePath).fileName();
        if (!checkNameCase(fileName, config.fileNameCase)) {
            QString suggestion = convertNameToCase(fileName, config.fileNameCase);
            findings.append({asset.relativePath, "file_name_case", Severity::Warning,
                             QString("Expected %1 (suggestion: %2)")
                                 .arg(config.fileNameCase, suggestion),
                             /*fixable=*/true});
        }
    }

    // ---- max/min_anim_keyframes ----
    for (int i = 0; i < asset.animationKeyframeCounts.size(); ++i) {
        int kf = asset.animationKeyframeCounts[i];
        if (config.maxAnimKeyframes > 0 && kf > config.maxAnimKeyframes)
            findings.append({asset.relativePath, "max_anim_keyframes", Severity::Error,
                             QString("Animation '%1' has %2 keyframes (max: %3)")
                                 .arg(asset.animationNames.value(i)).arg(kf).arg(config.maxAnimKeyframes)});
        if (config.minAnimKeyframes > 0 && kf < config.minAnimKeyframes)
            findings.append({asset.relativePath, "min_anim_keyframes", Severity::Warning,
                             QString("Animation '%1' has %2 keyframes (min: %3)")
                                 .arg(asset.animationNames.value(i)).arg(kf).arg(config.minAnimKeyframes)});
    }

    // ---- max/min_anim_duration ----
    for (int i = 0; i < asset.animationDurations.size(); ++i) {
        double dur = asset.animationDurations[i];
        if (config.maxAnimDuration > 0 && dur > config.maxAnimDuration)
            findings.append({asset.relativePath, "max_anim_duration", Severity::Error,
                             QString("Animation '%1' is %2s long (max: %3s)")
                                 .arg(asset.animationNames.value(i))
                                 .arg(dur, 0, 'f', 1).arg(config.maxAnimDuration, 0, 'f', 1)});
        if (config.minAnimDuration > 0 && dur < config.minAnimDuration)
            findings.append({asset.relativePath, "min_anim_duration", Severity::Warning,
                             QString("Animation '%1' is %2s long (min: %3s)")
                                 .arg(asset.animationNames.value(i))
                                 .arg(dur, 0, 'f', 1).arg(config.minAnimDuration, 0, 'f', 1)});
    }

    // ---- require_animation_names ----
    if (!config.requireAnimationNames.isEmpty()) {
        for (const auto& required : config.requireAnimationNames) {
            bool found = false;
            for (const auto& name : asset.animationNames) {
                if (matchesWildcard(name, required)) { found = true; break; }
            }
            if (!found) {
                findings.append({asset.relativePath, "require_animation_names", Severity::Error,
                                 QString("Required animation '%1' not found (has: %2)")
                                     .arg(required, asset.animationNames.join(", "))});
            }
        }
    }

    // ---- require_bone_names ----
    if (!config.requireBoneNames.isEmpty()) {
        for (const auto& required : config.requireBoneNames) {
            bool found = false;
            for (const auto& name : asset.boneNames) {
                if (matchesWildcard(name, required)) { found = true; break; }
            }
            if (!found) {
                findings.append({asset.relativePath, "require_bone_names", Severity::Error,
                                 QString("Required bone '%1' not found").arg(required)});
            }
        }
    }

    return findings;
}

// ---------------------------------------------------------------------------
// Auto-fixes
// ---------------------------------------------------------------------------

void ScanEngine::applyFixes(const ScanConfig& config, AssetInfo& asset,
                            QList<Finding>& findings)
{
    if (!config.fixEnabled) return;

    for (auto& f : findings) {
        if (!f.fixable) continue;

        if (f.rule == "file_name_case") {
            QString oldPath = asset.filePath;
            QString dir = QFileInfo(oldPath).path();
            QString newName = convertNameToCase(QFileInfo(oldPath).fileName(), config.fileNameCase);
            QString newPath = dir + "/" + newName;

            if (oldPath == newPath) continue;

            if (config.dryRun) {
                f.message += QString(" [dry-run: would rename to %1]").arg(newName);
            } else {
                if (QFile::rename(oldPath, newPath)) {
                    SentryReporter::addBreadcrumb("file.export",
                        QString("Rename: %1 -> %2").arg(QFileInfo(oldPath).fileName(), newName));
                    f.message += QString(" [fixed: renamed to %1]").arg(newName);
                    f.fixed = true;
                    asset.filePath = newPath;
                    QString relDir = QFileInfo(asset.relativePath).path();
                    asset.relativePath = (relDir == ".") ? newName : relDir + "/" + newName;
                } else {
                    f.message += " [fix failed: could not rename file]";
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Main scan pipeline
// ---------------------------------------------------------------------------

ScanResult ScanEngine::run(const ScanConfig& config, const QString& rootOverride,
                           const AssetProcessedCallback& onAssetProcessed)
{
    ScanResult result;
    const QString utcFmt = QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'");
    result.scanStartedUtc = QDateTime::currentDateTimeUtc().toString(utcFmt);
    QElapsedTimer timer;
    timer.start();

    // Determine scan root(s)
    QStringList roots;
    if (!rootOverride.isEmpty()) {
        roots.append(QDir(rootOverride).absolutePath());
    } else if (!config.roots.isEmpty()) {
        for (const auto& r : config.roots)
            roots.append(QDir(r).absolutePath());
    } else {
        roots.append(QDir::currentPath());
    }

    // Enumerate and inspect all files across all roots
    for (const auto& scanRoot : roots) {
        SentryReporter::addBreadcrumb("file.import",
            QString("Scan start: %1").arg(scanRoot));
        QStringList files = enumerateFiles(config, scanRoot);

        for (const auto& filePath : files) {
            AssetInfo asset = inspectAsset(filePath, scanRoot);
            if (asset.loadError)
                SentryReporter::addBreadcrumb("file.import",
                    QString("Load error: %1 — %2").arg(asset.relativePath, asset.errorMessage));
            QList<Finding> findings = evaluateRules(asset, config);

            // Apply fixes where possible
            applyFixes(config, asset, findings);

            if (onAssetProcessed)
                onAssetProcessed(asset, findings);

            // Tally — fixed findings don't count toward error/warning totals
            bool hasError = false, hasWarning = false;
            for (const auto& f : findings) {
                if (f.fixed) { result.fixed++; continue; }
                switch (f.severity) {
                case Severity::Error:   result.errors++;   hasError   = true; break;
                case Severity::Warning: result.warnings++; hasWarning = true; break;
                case Severity::Info:    result.infos++;    break;
                }
            }

            if (asset.loadError)
                result.skipped++;
            else if (!hasError && !hasWarning)
                result.passed++;

            result.scanned++;
            result.findings.append(findings);
            result.assets.append(asset);
        }
    }

    result.elapsedMs = timer.elapsed();
    result.scanCompletedUtc = QDateTime::currentDateTimeUtc().toString(utcFmt);
    return result;
}

// ---------------------------------------------------------------------------
// Text formatter
// ---------------------------------------------------------------------------

static QString severityLabel(Severity s)
{
    switch (s) {
    case Severity::Error:   return "ERROR";
    case Severity::Warning: return "WARN ";
    case Severity::Info:    return "INFO ";
    }
    return "     ";
}

static QString colorizeToken(const QString& text, const char* ansiColor, bool enabled)
{
    if (!enabled)
        return text;
    return QStringLiteral("\x1b[%1m%2\x1b[0m").arg(QString::fromLatin1(ansiColor), text);
}

QString ScanEngine::formatText(const ScanResult& result, const ScanConfig& config, bool colorize)
{
    Q_UNUSED(config);
    QString out;
    QTextStream s(&out);

    // Per-asset output
    for (const auto& asset : result.assets) {
        // Collect this asset's findings
        QList<Finding> assetFindings;
        for (const auto& f : result.findings) {
            if (f.file == asset.relativePath)
                assetFindings.append(f);
        }

        bool hasError = false, hasWarning = false;
        for (const auto& f : assetFindings) {
            if (f.severity == Severity::Error)   hasError   = true;
            if (f.severity == Severity::Warning) hasWarning = true;
        }

        // Status label
        if (hasError)
            s << colorizeToken("ERROR", "31", colorize) << "   " << asset.relativePath << "\n";
        else if (hasWarning)
            s << colorizeToken("WARN", "33", colorize) << "    " << asset.relativePath << "\n";
        else
            s << "  " << colorizeToken("OK", "32", colorize) << "    " << asset.relativePath << "\n";

        // Findings detail
        for (const auto& f : assetFindings) {
            QString label = severityLabel(f.severity);
            s << "         [" << label.trimmed().toLower() << "] "
              << f.rule << ": " << f.message << "\n";
        }
    }

    // Summary
    s << "\n";
    s << "Summary:\n";
    s << "  • Scanned:  " << result.scanned  << "\n";
    s << "  ✓ Passed:   " << result.passed   << "\n";
    s << "  ▲ Warnings: " << result.warnings << "\n";
    s << "  ✗ Errors:   " << result.errors   << "\n";
    if (result.infos > 0)
        s << "  ℹ Info:     " << result.infos << "\n";
    if (result.fixed > 0)
        s << "  🔧 Fixed:    " << result.fixed << "\n";
    if (result.skipped > 0)
        s << "  ⏭ Skipped:  " << result.skipped << "\n";
    s << "  ⏱ Time:     " << QString::number(result.elapsedMs / 1000.0, 'f', 1) << "s\n";
    QString utcStart, utcEnd;
    scanReportUtcTimes(result, &utcStart, &utcEnd);
    s << "  UTC start:  " << utcStart << "\n";
    s << "  UTC end:    " << utcEnd << "\n";

    return out;
}

// ---------------------------------------------------------------------------
// JSON formatter
// ---------------------------------------------------------------------------

static QString severityStr(Severity s)
{
    switch (s) {
    case Severity::Error:   return "error";
    case Severity::Warning: return "warning";
    case Severity::Info:    return "info";
    }
    return "info";
}

/// Exported JSON (and cloud upload) must not embed Assimp paths or other local details.
static QString findingMessageForExport(const Finding& f)
{
    if (f.rule == QLatin1String("load_error"))
        return QStringLiteral("Failed to load asset (see local CLI output for details)");
    return f.message;
}

void ScanEngine::scanReportUtcTimes(const ScanResult& result, QString* scanStartedUtc,
                                    QString* scanCompletedUtc)
{
    const QString fmt = QStringLiteral("yyyy-MM-dd'T'HH:mm:ss.zzz'Z'");
    QString end = result.scanCompletedUtc;
    QString start = result.scanStartedUtc;
    if (end.isEmpty())
        end = QDateTime::currentDateTimeUtc().toString(fmt);
    if (start.isEmpty())
        start = end;
    *scanStartedUtc = start;
    *scanCompletedUtc = end;
}

QJsonObject ScanEngine::scanReportToJsonObject(const ScanResult& result)
{
    QJsonObject root;
    root["version"] = QTMESHEDITOR_VERSION;
    QString utcStart, utcEnd;
    scanReportUtcTimes(result, &utcStart, &utcEnd);
    root["scanStartedUtc"] = utcStart;
    root["scanCompletedUtc"] = utcEnd;

    // Summary
    QJsonObject summary;
    summary["scanned"]  = result.scanned;
    summary["passed"]   = result.passed;
    summary["warnings"] = result.warnings;
    summary["errors"]   = result.errors;
    summary["infos"]    = result.infos;
    summary["fixed"]    = result.fixed;
    summary["skipped"]  = result.skipped;
    summary["elapsedMs"] = result.elapsedMs;
    root["summary"] = summary;

    // Assets
    QJsonArray assetsArr;
    for (const auto& asset : result.assets) {
        QJsonObject ao;
        ao["file"]           = asset.relativePath;
        ao["format"]         = asset.format;
        ao["fileSize"]       = static_cast<qint64>(asset.fileSize);
        ao["meshCount"]      = static_cast<int>(asset.meshCount);
        ao["materialCount"]  = static_cast<int>(asset.materialCount);
        ao["vertexCount"]    = static_cast<int>(asset.vertexCount);
        ao["faceCount"]      = static_cast<int>(asset.faceCount);
        ao["animationCount"] = static_cast<int>(asset.animationCount);
        ao["hasSkeleton"]    = asset.hasSkeleton;
        ao["boneCount"]      = static_cast<int>(asset.boneCount);
        ao["textureRefCount"] = static_cast<int>(asset.textureRefCount);

        if (!asset.animationNames.isEmpty()) {
            QJsonArray anims;
            for (int i = 0; i < asset.animationNames.size(); ++i) {
                QJsonObject anim;
                anim["name"] = asset.animationNames[i];
                if (i < asset.animationDurations.size())
                    anim["duration"] = asset.animationDurations[i];
                if (i < asset.animationKeyframeCounts.size())
                    anim["keyframes"] = asset.animationKeyframeCounts[i];
                anims.append(anim);
            }
            ao["animations"] = anims;
        }
        if (!asset.boneNames.isEmpty()) {
            QJsonArray bones;
            for (const auto& b : asset.boneNames) bones.append(b);
            ao["bones"] = bones;
        }

        if (asset.loadError)
            ao["loadError"] = true;

        // Inline findings for this asset
        QJsonArray findingsArr;
        for (const auto& f : result.findings) {
            if (f.file != asset.relativePath) continue;
            QJsonObject fo;
            fo["rule"]     = f.rule;
            fo["severity"] = severityStr(f.severity);
            fo["message"]  = findingMessageForExport(f);
            if (f.fixable) fo["fixable"] = true;
            if (f.fixed)   fo["fixed"]   = true;
            findingsArr.append(fo);
        }
        ao["findings"] = findingsArr;

        assetsArr.append(ao);
    }
    root["assets"] = assetsArr;

    return root;
}

QString ScanEngine::formatJson(const ScanResult& result)
{
    return QString::fromUtf8(QJsonDocument(scanReportToJsonObject(result)).toJson(QJsonDocument::Indented));
}

// ---------------------------------------------------------------------------
// SARIF formatter (Static Analysis Results Interchange Format 2.1.0)
// ---------------------------------------------------------------------------

QString ScanEngine::formatSarif(const ScanResult& result)
{
    // Build rule definitions from unique rule IDs
    QMap<QString, QString> ruleDescriptions;
    ruleDescriptions["load_error"]              = "Asset file could not be loaded";
    ruleDescriptions["allowed_formats"]         = "Asset format is not in the allowed list";
    ruleDescriptions["forbidden_extensions"]    = "Asset format is explicitly forbidden";
    ruleDescriptions["max_file_size_mb"]        = "Asset file exceeds maximum file size";
    ruleDescriptions["max_mesh_count"]          = "Asset exceeds maximum mesh count";
    ruleDescriptions["max_material_count"]      = "Asset exceeds maximum material count";
    ruleDescriptions["max_vertex_count"]        = "Asset exceeds maximum vertex count";
    ruleDescriptions["require_skeleton"]        = "Asset is missing a required skeleton";
    ruleDescriptions["require_animations"]      = "Asset is missing required animations";
    ruleDescriptions["allow_embedded_textures"] = "Asset contains embedded textures";
    ruleDescriptions["require_textures_exist"]  = "Referenced texture file not found on disk";
    ruleDescriptions["allow_missing_materials"] = "Asset has placeholder or missing materials";
    ruleDescriptions["file_name_case"]          = "File name does not match naming convention";
    ruleDescriptions["max_anim_keyframes"]      = "Animation exceeds maximum keyframe count";
    ruleDescriptions["min_anim_keyframes"]      = "Animation has fewer than minimum keyframes";
    ruleDescriptions["max_anim_duration"]       = "Animation exceeds maximum duration";
    ruleDescriptions["min_anim_duration"]       = "Animation is shorter than minimum duration";
    ruleDescriptions["min_file_size_mb"]        = "Asset file is smaller than minimum size";
    ruleDescriptions["min_mesh_count"]          = "Asset has fewer than minimum meshes";
    ruleDescriptions["min_material_count"]      = "Asset has fewer than minimum materials";
    ruleDescriptions["min_vertex_count"]        = "Asset has fewer than minimum vertices";
    ruleDescriptions["require_animation_names"] = "Required animation not found in asset";
    ruleDescriptions["require_bone_names"]      = "Required bone not found in skeleton";

    // Collect unique rules used in findings
    QSet<QString> usedRules;
    for (const auto& f : result.findings)
        usedRules.insert(f.rule);

    QJsonArray rulesArr;
    for (const auto& ruleId : usedRules) {
        QJsonObject rule;
        rule["id"] = ruleId;
        QJsonObject shortDesc;
        shortDesc["text"] = ruleDescriptions.value(ruleId, ruleId);
        rule["shortDescription"] = shortDesc;
        rulesArr.append(rule);
    }

    // Build results
    QJsonArray resultsArr;
    for (const auto& f : result.findings) {
        QJsonObject r;
        r["ruleId"] = f.rule;

        QJsonObject message;
        message["text"] = f.message;
        r["message"] = message;

        QString level;
        switch (f.severity) {
        case Severity::Error:   level = "error"; break;
        case Severity::Warning: level = "warning"; break;
        case Severity::Info:    level = "note"; break;
        }
        r["level"] = level;

        // Location
        QJsonObject physicalLocation;
        QJsonObject artifactLocation;
        artifactLocation["uri"] = f.file;
        physicalLocation["artifactLocation"] = artifactLocation;

        QJsonObject location;
        location["physicalLocation"] = physicalLocation;
        r["locations"] = QJsonArray{location};

        if (f.fixable) {
            QJsonObject props;
            props["fixable"] = true;
            if (f.fixed) props["fixed"] = true;
            r["properties"] = props;
        }

        resultsArr.append(r);
    }

    // Assemble SARIF document
    QJsonObject driver;
    driver["name"] = "qtmesh scan";
    driver["version"] = QTMESHEDITOR_VERSION;
    driver["rules"] = rulesArr;

    QJsonObject tool;
    tool["driver"] = driver;

    QJsonObject run;
    run["tool"] = tool;
    run["results"] = resultsArr;
    QString sarifStart, sarifEnd;
    scanReportUtcTimes(result, &sarifStart, &sarifEnd);
    QJsonObject invocation;
    invocation["startTimeUtc"] = sarifStart;
    invocation["endTimeUtc"] = sarifEnd;
    invocation["executionSuccessful"] = true;
    run["invocations"] = QJsonArray{invocation};

    QJsonObject sarif;
    sarif["$schema"] = "https://json.schemastore.org/sarif-2.1.0.json";
    sarif["version"] = "2.1.0";
    sarif["runs"] = QJsonArray{run};

    return QString::fromUtf8(QJsonDocument(sarif).toJson(QJsonDocument::Indented));
}
