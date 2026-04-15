#include "ScanConfig.h"
#include "ScanEngine.h"

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

ScanConfig ScanConfig::loadFromFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream(stderr) << "Warning: Cannot open config file: " << path << Qt::endl;
        return defaults();
    }

    QString content = QString::fromUtf8(file.readAll());

    if (path.endsWith(".json", Qt::CaseInsensitive)) {
        QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8());
        if (doc.isNull()) {
            QTextStream(stderr) << "Warning: Invalid JSON in config file: " << path << Qt::endl;
            return defaults();
        }
        return fromJson(doc.object());
    }

    QVariantMap map = parseSimpleYaml(content);
    return fromVariantMap(map);
}

ScanConfig ScanConfig::fromVariantMap(const QVariantMap& root)
{
    ScanConfig config;

    config.version = root.value("version", 1).toInt();

    // scan section
    QVariantMap scan = root.value("scan").toMap();
    if (!scan.isEmpty()) {
        if (scan.contains("roots"))
            config.roots = scan.value("roots").toStringList();
        if (scan.contains("include")) {
            config.includePatterns = scan.value("include").toStringList();
        }
        if (scan.contains("exclude"))
            config.excludePatterns = scan.value("exclude").toStringList();
    }

    // rules section
    QVariantMap rules = root.value("rules").toMap();
    if (!rules.isEmpty()) {
        if (rules.contains("allowed_formats"))
            config.allowedFormats = rules.value("allowed_formats").toStringList();
        if (rules.contains("forbidden_extensions"))
            config.forbiddenExtensions = rules.value("forbidden_extensions").toStringList();
        config.maxFileSizeMb         = rules.value("max_file_size_mb",         config.maxFileSizeMb).toDouble();
        config.minFileSizeMb         = rules.value("min_file_size_mb",         config.minFileSizeMb).toDouble();
        config.maxMeshCount          = rules.value("max_mesh_count",           config.maxMeshCount).toInt();
        config.minMeshCount          = rules.value("min_mesh_count",           config.minMeshCount).toInt();
        config.maxMaterialCount      = rules.value("max_material_count",       config.maxMaterialCount).toInt();
        config.minMaterialCount      = rules.value("min_material_count",       config.minMaterialCount).toInt();
        config.maxVertexCount        = rules.value("max_vertex_count",         config.maxVertexCount).toInt();
        config.minVertexCount        = rules.value("min_vertex_count",         config.minVertexCount).toInt();
        config.requireSkeleton       = rules.value("require_skeleton",         config.requireSkeleton).toBool();
        config.requireAnimations     = rules.value("require_animations",       config.requireAnimations).toBool();
        config.allowEmbeddedTextures = rules.value("allow_embedded_textures",  config.allowEmbeddedTextures).toBool();
        config.requireTexturesExist  = rules.value("require_textures_exist",   config.requireTexturesExist).toBool();
        config.allowMissingMaterials = rules.value("allow_missing_materials",  config.allowMissingMaterials).toBool();
        config.fileNameCase          = rules.value("file_name_case",           config.fileNameCase).toString();
        config.maxAnimKeyframes      = rules.value("max_anim_keyframes",       config.maxAnimKeyframes).toInt();
        config.minAnimKeyframes      = rules.value("min_anim_keyframes",       config.minAnimKeyframes).toInt();
        config.maxAnimDuration       = rules.value("max_anim_duration",        config.maxAnimDuration).toDouble();
        config.minAnimDuration       = rules.value("min_anim_duration",        config.minAnimDuration).toDouble();
        if (rules.contains("require_animation_names"))
            config.requireAnimationNames = rules.value("require_animation_names").toStringList();
        if (rules.contains("require_bone_names"))
            config.requireBoneNames = rules.value("require_bone_names").toStringList();
    }

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

    return config;
}

ScanConfig ScanConfig::fromJson(const QJsonObject& obj)
{
    return fromVariantMap(obj.toVariantMap());
}
