#include "ScanConfig.h"
#include "ScanEngine.h"
#include "SentryReporter.h"

#include <assimp/Importer.hpp>
#include <assimp/importerdesc.h>

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

// ---------------------------------------------------------------------------
// Minimal YAML parser helpers
// ---------------------------------------------------------------------------

static QString stripQuotes(const QString& s)
{
    if (s.size() >= 2) {
        if ((s.startsWith('"') && s.endsWith('"')) ||
            (s.startsWith('\'') && s.endsWith('\'')))
            return s.mid(1, s.size() - 2);
    }
    return s;
}

static QVariant parseYamlScalar(const QString& raw)
{
    QString v = raw.trimmed();
    if (v.isEmpty()) return QString();

    if ((v.startsWith('"') && v.endsWith('"')) ||
        (v.startsWith('\'') && v.endsWith('\'')))
        return v.mid(1, v.size() - 2);

    if (v == "true" || v == "yes" || v == "on")   return true;
    if (v == "false" || v == "no" || v == "off")   return false;

    bool ok = false;
    int intVal = v.toInt(&ok);
    if (ok) return intVal;
    double dblVal = v.toDouble(&ok);
    if (ok) return dblVal;

    return v;
}

static QStringList parseInlineList(const QString& value)
{
    QStringList items;
    QString inner = value.mid(1, value.size() - 2);
    for (const auto& item : inner.split(','))
        items.append(stripQuotes(item.trimmed()));
    return items;
}

static int countIndent(const QString& line)
{
    int n = 0;
    while (n < line.size() && line[n] == ' ') ++n;
    return n;
}

/// Parse a simple YAML config with up to 3 levels of nesting.
///
/// Level 0 (indent 0): top-level keys
/// Level 1 (indent 2): section content, or scope path patterns
/// Level 2 (indent 4): subsection content (rules inside a scope)
///
/// Block lists ("- item") can appear at any level.
/// Supports inline lists [a, b, c] and quoted keys "path/**".
QVariantMap parseSimpleYaml(const QString& content)
{
    QVariantMap root;

    // Level 1
    QString section;
    QVariantMap sectionMap;

    // Level 2 (subsection, for scopes)
    QString subsection;
    QVariantMap subsectionMap;

    // Track subsection insertion order (QVariantMap sorts alphabetically)
    QStringList subsectionOrder;

    // Block list accumulation
    QString listKey;
    QStringList listItems;
    int listNestLevel = 0; // 0=root, 1=section, 2=subsection

    // True when we saw a key with no value and haven't decided if it's a list or subsection
    bool pendingDecision = false;

    auto flushList = [&]() {
        if (listKey.isEmpty()) return;
        QStringList items = listItems;
        if (listNestLevel == 2 && !subsection.isEmpty())
            subsectionMap[listKey] = items;
        else if (listNestLevel == 1 && !section.isEmpty())
            sectionMap[listKey] = items;
        else
            root[listKey] = items;
        listKey.clear();
        listItems.clear();
        pendingDecision = false;
    };

    auto flushSubsection = [&]() {
        flushList();
        if (!subsection.isEmpty() && !section.isEmpty()) {
            sectionMap[subsection] = QVariant::fromValue(subsectionMap);
            subsectionOrder.append(subsection);
            subsectionMap.clear();
            subsection.clear();
        }
    };

    auto flushSection = [&]() {
        flushSubsection();
        if (!section.isEmpty()) {
            if (!subsectionOrder.isEmpty())
                sectionMap["_order"] = subsectionOrder;
            root[section] = QVariant::fromValue(sectionMap);
            sectionMap.clear();
            subsectionOrder.clear();
            section.clear();
        }
    };

    const QStringList lines = content.split('\n');
    for (const QString& rawLine : lines) {
        // Strip trailing comment (not inside quotes)
        QString line = rawLine;
        {
            bool inQuote = false;
            for (int i = 0; i < line.size(); ++i) {
                QChar c = line[i];
                if (c == '"' || c == '\'') inQuote = !inQuote;
                if (c == '#' && !inQuote) { line = line.left(i); break; }
            }
        }

        if (line.trimmed().isEmpty()) continue;

        int indent = countIndent(line);
        QString trimmed = line.trimmed();

        // Block list item: "- value"
        if (trimmed.startsWith("- ")) {
            QString val = stripQuotes(trimmed.mid(2).trimmed());
            listItems.append(val);
            pendingDecision = false;
            continue;
        }

        // Not a list item — resolve any pending decision
        if (pendingDecision && !listKey.isEmpty()) {
            if (!listItems.isEmpty()) {
                // Was a block list — flush it
                flushList();
            } else {
                // No "- " items followed → it was a subsection header
                if (listNestLevel == 1) {
                    subsection = listKey;
                    subsectionMap.clear();
                }
                listKey.clear();
                listItems.clear();
                pendingDecision = false;
            }
        } else if (!listKey.isEmpty() && !listItems.isEmpty()) {
            flushList();
        }

        // Handle indent-level transitions
        if (indent == 0 && !section.isEmpty()) flushSection();
        if (indent <= 2 && !subsection.isEmpty()) flushSubsection();

        // Parse key: value
        int colon = trimmed.indexOf(':');
        if (colon < 0) continue;

        QString key   = stripQuotes(trimmed.left(colon).trimmed());
        QString value = trimmed.mid(colon + 1).trimmed();

        if (indent == 0) {
            // Top-level
            if (value.isEmpty()) {
                section = key;
                sectionMap.clear();
            } else {
                root[key] = parseYamlScalar(value);
            }
        } else if (indent < 4) {
            // Level 1: section content or scope key
            if (value.isEmpty()) {
                // Potential block list or subsection — defer decision
                listKey = key;
                listNestLevel = 1;
                listItems.clear();
                pendingDecision = true;
            } else if (value.startsWith('[') && value.endsWith(']')) {
                sectionMap[key] = parseInlineList(value);
            } else {
                sectionMap[key] = parseYamlScalar(value);
            }
        } else {
            // Level 2: subsection content (indent >= 4)
            if (value.isEmpty()) {
                // Block list inside subsection
                listKey = key;
                listNestLevel = 2;
                listItems.clear();
                pendingDecision = true;
            } else if (value.startsWith('[') && value.endsWith(']')) {
                subsectionMap[key] = parseInlineList(value);
            } else {
                subsectionMap[key] = parseYamlScalar(value);
            }
        }
    }

    // Flush remaining state
    if (pendingDecision && !listKey.isEmpty()) {
        if (!listItems.isEmpty())
            flushList();
        else if (listNestLevel == 1 && !listKey.isEmpty()) {
            subsection = listKey;
            subsectionMap.clear();
            listKey.clear();
            pendingDecision = false;
        }
    } else if (!listKey.isEmpty()) {
        flushList();
    }
    flushSection();
    return root;
}

// ---------------------------------------------------------------------------
// Rule override application
// ---------------------------------------------------------------------------

void ScanConfig::applyRuleOverrides(const QVariantMap& r)
{
    if (r.contains("allowed_formats"))       allowedFormats       = r["allowed_formats"].toStringList();
    if (r.contains("forbidden_extensions"))  forbiddenExtensions  = r["forbidden_extensions"].toStringList();
    if (r.contains("max_file_size_mb"))      maxFileSizeMb        = r["max_file_size_mb"].toDouble();
    if (r.contains("min_file_size_mb"))      minFileSizeMb        = r["min_file_size_mb"].toDouble();
    if (r.contains("max_mesh_count"))        maxMeshCount         = r["max_mesh_count"].toInt();
    if (r.contains("min_mesh_count"))        minMeshCount         = r["min_mesh_count"].toInt();
    if (r.contains("max_material_count"))    maxMaterialCount     = r["max_material_count"].toInt();
    if (r.contains("min_material_count"))    minMaterialCount     = r["min_material_count"].toInt();
    if (r.contains("max_vertex_count"))      maxVertexCount       = r["max_vertex_count"].toInt();
    if (r.contains("min_vertex_count"))      minVertexCount       = r["min_vertex_count"].toInt();
    if (r.contains("max_acmr"))              maxAcmr              = r["max_acmr"].toDouble();
    if (r.contains("require_skeleton"))      requireSkeleton      = r["require_skeleton"].toBool();
    if (r.contains("require_animations"))    requireAnimations    = r["require_animations"].toBool();
    if (r.contains("allow_embedded_textures")) allowEmbeddedTextures = r["allow_embedded_textures"].toBool();
    if (r.contains("require_textures_exist"))  requireTexturesExist  = r["require_textures_exist"].toBool();
    if (r.contains("allow_missing_materials")) allowMissingMaterials = r["allow_missing_materials"].toBool();
    if (r.contains("file_name_case"))        fileNameCase         = r["file_name_case"].toString();
    if (r.contains("max_anim_keyframes"))    maxAnimKeyframes     = r["max_anim_keyframes"].toInt();
    if (r.contains("min_anim_keyframes"))    minAnimKeyframes     = r["min_anim_keyframes"].toInt();
    if (r.contains("max_anim_duration"))     maxAnimDuration      = r["max_anim_duration"].toDouble();
    if (r.contains("min_anim_duration"))     minAnimDuration      = r["min_anim_duration"].toDouble();
    if (r.contains("require_animation_names")) requireAnimationNames = r["require_animation_names"].toStringList();
    if (r.contains("require_bone_names"))    requireBoneNames     = r["require_bone_names"].toStringList();
    if (r.contains("redundant_keyframes_pct"))
        redundantKeyframesPctThreshold = r["redundant_keyframes_pct"].toDouble();
    if (r.contains("redundant_keyframes_translation_tol"))
        redundantKeyframesTranslationTol = r["redundant_keyframes_translation_tol"].toDouble();
    if (r.contains("redundant_keyframes_rotation_deg_tol"))
        redundantKeyframesRotationDegTol = r["redundant_keyframes_rotation_deg_tol"].toDouble();
    if (r.contains("redundant_keyframes_scale_tol"))
        redundantKeyframesScaleTol = r["redundant_keyframes_scale_tol"].toDouble();
    // C4 quality rules
    if (r.contains("max_texture_resolution"))
        maxTextureResolution = r["max_texture_resolution"].toInt();
    if (r.contains("require_uv_channels"))
        requireUvChannels = r["require_uv_channels"].toInt();
    if (r.contains("detect_zero_weight_bones"))
        detectZeroWeightBones = r["detect_zero_weight_bones"].toBool();
    if (r.contains("detect_overlapping_uvs_pct"))
        detectOverlappingUvsPct = r["detect_overlapping_uvs_pct"].toDouble();
    if (r.contains("detect_non_manifold_edges_pct"))
        detectNonManifoldEdgesPct = r["detect_non_manifold_edges_pct"].toDouble();
    // Budget rules (#365)
    if (r.contains("max_triangle_count"))
        maxTriangleCount = r["max_triangle_count"].toInt();
    if (r.contains("max_triangles_per_mesh"))
        maxTrianglesPerMesh = r["max_triangles_per_mesh"].toInt();
    if (r.contains("max_bones"))
        maxBoneCount = r["max_bones"].toInt();
    if (r.contains("max_submesh_count"))
        maxSubmeshCount = r["max_submesh_count"].toInt();
    if (r.contains("max_draw_calls"))
        maxDrawCalls = r["max_draw_calls"].toInt();
    if (r.contains("texture_not_power_of_two"))
        requireTexturePowerOfTwo = r["texture_not_power_of_two"].toBool();
    if (r.contains("allowed_texture_formats"))
        allowedTextureFormats = r["allowed_texture_formats"].toStringList();
    if (r.contains("disallowed_texture_formats"))
        disallowedTextureFormats = r["disallowed_texture_formats"].toStringList();
    // Alias for max_texture_resolution (issue #365 naming); canonical key wins when both present.
    if (r.contains("max_texture_dimension") && !r.contains("max_texture_resolution"))
        maxTextureResolution = r["max_texture_dimension"].toInt();
}

ScanConfig ScanConfig::withScopeOverrides(const QString& relativePath) const
{
    ScanConfig result = *this;
    for (const auto& scope : scopes) {
        if (ScanEngine::matchesGlob(relativePath, scope.pathPattern))
            result.applyRuleOverrides(scope.rules);
    }
    return result;
}

// ---------------------------------------------------------------------------
// ScanConfig loading
// ---------------------------------------------------------------------------

namespace {

/**
 * When a project sets `scan.include`, it replaces the default Assimp-based globs.
 * PlayStation `.tmd` / `.rsd` and Psy-Q `.ply` are not Assimp extensions, so they
 * would never be scanned unless we merge these patterns in when missing.
 */
void appendEditorOnlyMeshScanGlobsIfMissing(QStringList& patterns)
{
    if (patterns.isEmpty())
        return;

    const QLatin1String kExtras[] = {
        QLatin1String("tmd"),
        QLatin1String("rsd"),
        QLatin1String("ply"),
    };

    auto hasExtensionGlob = [&](QLatin1String extNoDot) -> bool {
        const QString token = QStringLiteral("*.") + QString(extNoDot);
        for (const QString& p : patterns) {
            if (p.contains(token, Qt::CaseInsensitive))
                return true;
        }
        return false;
    };

    for (QLatin1String ext : kExtras) {
        if (!hasExtensionGlob(ext))
            patterns.append(QStringLiteral("**/*.") + QString(ext));
    }
}

} // namespace

ScanConfig::ScanConfig()
    : includePatterns(ScanConfig::defaultIncludePatternsForAssimpImports())
{
}

QStringList ScanConfig::defaultIncludePatternsForAssimpImports()
{
    static const QStringList cached = []() {
        QSet<QString> extSet;
        Assimp::Importer importer;
        for (unsigned i = 0; i < importer.GetImporterCount(); ++i) {
            const aiImporterDesc* desc = importer.GetImporterInfo(i);
            if (!desc || !desc->mFileExtensions)
                continue;
            const QString extList = QString::fromLatin1(desc->mFileExtensions);
            static const QRegularExpression sep(QStringLiteral("[;\\s,]+"));
            const QStringList parts = extList.split(sep, Qt::SkipEmptyParts);
            for (const QString& raw : parts) {
                QString ext = raw.trimmed().toLower();
                if (ext.startsWith(QLatin1Char('.')))
                    ext.remove(0, 1);
                if (ext.isEmpty())
                    continue;
                extSet.insert(ext);
            }
        }
        // Ogre mesh formats used by the editor (may or may not appear as separate Assimp importers)
        extSet.insert(QStringLiteral("mesh"));
        extSet.insert(QStringLiteral("mesh.xml"));

        // PlayStation / Psy-Q sidecars — imported by QtMeshEditor, not Assimp extensions
        extSet.insert(QStringLiteral("tmd"));
        extSet.insert(QStringLiteral("rsd"));

        QStringList globs;
        globs.reserve(extSet.size());
        for (const QString& ext : extSet) {
            globs.append(QStringLiteral("**/*.") + ext);
        }
        globs.sort(Qt::CaseInsensitive);
        return globs;
    }();
    return cached;
}

ScanConfig ScanConfig::defaults()
{
    return ScanConfig();
}

QVariantMap ScanConfig::loadProjectMapFromFile(const QString& path)
{
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("scan config open_attempt=%1").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                      QStringLiteral("scan config open_failed=%1").arg(path),
                                      QStringLiteral("warning"));
        QTextStream(stderr) << "Warning: Cannot open config file: " << path << Qt::endl;
        return {};
    }

    const QString content = QString::fromUtf8(file.readAll());
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("scan config parse_attempt=%1").arg(path));

    if (path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
        if (doc.isNull() || !doc.isObject()) {
            SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                          QStringLiteral("scan config parse_failed=%1").arg(path),
                                          QStringLiteral("warning"));
            QTextStream(stderr) << "Warning: Invalid JSON in config file: " << path << Qt::endl;
            return {};
        }
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                      QStringLiteral("scan config parse_success=%1 (json)").arg(path));
        return doc.object().toVariantMap();
    }

    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("scan config parse_success=%1 (yaml)").arg(path));
    return parseSimpleYaml(content);
}

ScanConfig ScanConfig::loadFromFile(const QString& path)
{
    const QVariantMap map = loadProjectMapFromFile(path);
    if (map.isEmpty())
        return defaults();
    return fromVariantMap(map);
}

void ScanConfig::applyProjectConfig(ScanConfig& config, const QVariantMap& root)
{
    config.version = root.value(QStringLiteral("version"), config.version).toInt();

    // scan section
    QVariantMap scan = root.value("scan").toMap();
    if (!scan.isEmpty()) {
        if (scan.contains("roots"))
            config.roots = scan.value("roots").toStringList();
        if (scan.contains("include")) {
            config.includePatterns = scan.value("include").toStringList();
            appendEditorOnlyMeshScanGlobsIfMissing(config.includePatterns);
        }
        if (scan.contains("exclude"))
            config.excludePatterns = scan.value("exclude").toStringList();
    }

    // rules section
    const QVariantMap rules = root.value("rules").toMap();
    if (!rules.isEmpty())
        config.applyRuleOverrides(rules);

    // scopes section — map of path patterns to rule override maps
    // Use _order key (if present) to preserve YAML declaration order,
    // since QVariantMap/QMap sorts keys alphabetically.
    QVariantMap scopesMap = root.value("scopes").toMap();
    QStringList scopeOrder = scopesMap.value("_order").toStringList();
    if (scopeOrder.isEmpty()) {
        // Fallback: iterate map keys (alphabetical — JSON or missing _order)
        for (auto it = scopesMap.constBegin(); it != scopesMap.constEnd(); ++it) {
            if (it.key() == "_order") continue;
            ScanScope scope;
            scope.pathPattern = it.key();
            scope.rules = it.value().toMap();
            config.scopes.append(scope);
        }
    } else {
        for (const auto& key : scopeOrder) {
            ScanScope scope;
            scope.pathPattern = key;
            scope.rules = scopesMap.value(key).toMap();
            config.scopes.append(scope);
        }
    }

    // fix section
    QVariantMap fix = root.value("fix").toMap();
    if (!fix.isEmpty()) {
        config.fixEnabled       = fix.value("enabled",           config.fixEnabled).toBool();
        config.dryRun           = fix.value("dry_run",           config.dryRun).toBool();
        config.optimizeMeshes   = fix.value("optimize_meshes",   config.optimizeMeshes).toBool();
        config.renameAnimations = fix.value("rename_animations", config.renameAnimations).toBool();
        config.convertToFormat  = fix.value("convert_to_format", config.convertToFormat).toString();
        config.outputDir        = fix.value("output_dir",        config.outputDir).toString();
    }

    // report section
    QVariantMap report = root.value("report").toMap();
    if (!report.isEmpty()) {
        config.reportFormat = report.value("format",       config.reportFormat).toString();
        config.reportOutput = report.value("output",       config.reportOutput).toString();
        config.sarifOutput  = report.value("sarif_output", config.sarifOutput).toString();
        config.failOn       = report.value("fail_on",      config.failOn).toString();
    }

}

ScanConfig ScanConfig::fromVariantMap(const QVariantMap& root)
{
    ScanConfig config = defaults();
    applyProjectConfig(config, root);
    return config;
}

ScanConfig ScanConfig::fromJson(const QJsonObject& obj)
{
    return fromVariantMap(obj.toVariantMap());
}
