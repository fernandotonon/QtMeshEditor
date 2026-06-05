#include "PlatformProfile.h"
#include "SentryReporter.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>

namespace {

bool looksLikeFilesystemPath(const QString& pathOrId)
{
    if (pathOrId.contains(QLatin1Char('/')) || pathOrId.contains(QLatin1Char('\\')))
        return true;
    return pathOrId.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive);
}

QString normalizeProfileId(const QString& id)
{
    return id.trimmed();
}

void warnUnknownRuleKeys(const QVariantMap& rules, const QString& context, QStringList* warnings)
{
    if (!warnings)
        return;
    const QStringList known = PlatformProfileLoader::knownRuleKeys();
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it) {
        if (!known.contains(it.key()))
            warnings->append(QStringLiteral("Ignoring unknown rule key '%1' in %2")
                                 .arg(it.key(), context));
    }
}

QVariantMap filterKnownRuleKeys(const QVariantMap& rules, const QString& context, QStringList* warnings)
{
    warnUnknownRuleKeys(rules, context, warnings);
    QVariantMap filtered;
    const QStringList known = PlatformProfileLoader::knownRuleKeys();
    for (auto it = rules.constBegin(); it != rules.constEnd(); ++it) {
        if (known.contains(it.key()))
            filtered.insert(it.key(), it.value());
    }
    return filtered;
}

void appendProfileScopes(ScanConfig& config, const QVariantMap& scopesMap, QStringList* warnings)
{
    QStringList scopeOrder = scopesMap.value(QStringLiteral("_order")).toStringList();
    if (scopeOrder.isEmpty()) {
        for (auto it = scopesMap.constBegin(); it != scopesMap.constEnd(); ++it) {
            if (it.key() == QStringLiteral("_order"))
                continue;
            ScanScope scope;
            scope.pathPattern = it.key();
            const QVariantMap rawRules = it.value().toMap();
            scope.rules = filterKnownRuleKeys(
                rawRules,
                QStringLiteral("profile scope '%1'").arg(scope.pathPattern),
                warnings);
            config.scopes.append(scope);
        }
        return;
    }

    for (const QString& key : scopeOrder) {
        if (!scopesMap.contains(key))
            continue;
        ScanScope scope;
        scope.pathPattern = key;
        const QVariantMap rawRules = scopesMap.value(key).toMap();
        scope.rules = filterKnownRuleKeys(
            rawRules,
            QStringLiteral("profile scope '%1'").arg(scope.pathPattern),
            warnings);
        config.scopes.append(scope);
    }
}

QStringList candidateBuiltinProfileDirectories()
{
    QStringList dirs;

    const QByteArray envDir = qgetenv("QTMESH_PROFILES_DIR");
    if (!envDir.isEmpty())
        dirs.append(QString::fromUtf8(envDir));

#ifdef QTMESH_UT_SOURCE_ROOT
    dirs.append(QStringLiteral(QTMESH_UT_SOURCE_ROOT) + QStringLiteral("/profiles"));
#endif

    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty()) {
        dirs.append(appDir + QStringLiteral("/profiles"));
        dirs.append(QDir(appDir).filePath(QStringLiteral("../profiles")));
        dirs.append(QDir(appDir).filePath(QStringLiteral("../share/qtmesh/profiles")));
    }

#ifdef Q_OS_LINUX
    // Linux .deb/Docker layout: /usr/share/qtmesheditor/{qtmesheditor,profiles}
    dirs.append(QStringLiteral("/usr/share/qtmesheditor/profiles"));
#endif

    return dirs;
}

} // namespace

QStringList PlatformProfileLoader::knownRuleKeys()
{
    return {
        QStringLiteral("allowed_formats"),
        QStringLiteral("forbidden_extensions"),
        QStringLiteral("max_file_size_mb"),
        QStringLiteral("min_file_size_mb"),
        QStringLiteral("max_mesh_count"),
        QStringLiteral("min_mesh_count"),
        QStringLiteral("max_material_count"),
        QStringLiteral("min_material_count"),
        QStringLiteral("max_vertex_count"),
        QStringLiteral("min_vertex_count"),
        QStringLiteral("max_acmr"),
        QStringLiteral("require_skeleton"),
        QStringLiteral("require_animations"),
        QStringLiteral("allow_embedded_textures"),
        QStringLiteral("require_textures_exist"),
        QStringLiteral("allow_missing_materials"),
        QStringLiteral("file_name_case"),
        QStringLiteral("max_anim_keyframes"),
        QStringLiteral("min_anim_keyframes"),
        QStringLiteral("max_anim_duration"),
        QStringLiteral("min_anim_duration"),
        QStringLiteral("require_animation_names"),
        QStringLiteral("require_bone_names"),
        QStringLiteral("redundant_keyframes_pct"),
        QStringLiteral("redundant_keyframes_translation_tol"),
        QStringLiteral("redundant_keyframes_rotation_deg_tol"),
        QStringLiteral("redundant_keyframes_scale_tol"),
        QStringLiteral("max_texture_resolution"),
        QStringLiteral("require_uv_channels"),
        QStringLiteral("detect_zero_weight_bones"),
        QStringLiteral("detect_overlapping_uvs_pct"),
        QStringLiteral("detect_non_manifold_edges_pct"),
        QStringLiteral("max_triangle_count"),
        QStringLiteral("max_triangles_per_mesh"),
        QStringLiteral("max_bones"),
        QStringLiteral("max_submesh_count"),
        QStringLiteral("max_draw_calls"),
        QStringLiteral("max_texture_dimension"),
        QStringLiteral("texture_not_power_of_two"),
        QStringLiteral("allowed_texture_formats"),
        QStringLiteral("disallowed_texture_formats"),
    };
}

QString PlatformProfileLoader::builtinProfilesDirectory()
{
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("platform profiles: builtin dir probe"));
    for (const QString& dir : candidateBuiltinProfileDirectories()) {
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                      QStringLiteral("platform profiles: try dir=%1").arg(dir));
        if (QDir(dir).exists())
            return QDir(dir).absolutePath();
    }
    return {};
}

QStringList PlatformProfileLoader::listBuiltinIds()
{
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("platform profiles: listBuiltinIds"));
    QStringList ids;
    const QString dirPath = builtinProfilesDirectory();
    if (dirPath.isEmpty()) {
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                      QStringLiteral("platform profiles: listBuiltinIds no dir"),
                                      QStringLiteral("warning"));
        return ids;
    }

    const QFileInfoList entries =
        QDir(dirPath).entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : entries)
        ids.append(fi.completeBaseName());
    ids.sort(Qt::CaseInsensitive);
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("platform profiles: found %1 ids in %2")
                                      .arg(ids.size()).arg(dirPath));
    return ids;
}

void PlatformProfileLoader::mergeProfileOnto(PlatformProfile& base, const PlatformProfile& overlay)
{
    if (!overlay.id.isEmpty())
        base.id = overlay.id;
    if (!overlay.displayName.isEmpty())
        base.displayName = overlay.displayName;
    if (!overlay.description.isEmpty())
        base.description = overlay.description;

    for (auto it = overlay.rules.constBegin(); it != overlay.rules.constEnd(); ++it)
        base.rules.insert(it.key(), it.value());

    for (auto it = overlay.scopes.constBegin(); it != overlay.scopes.constEnd(); ++it)
        base.scopes.insert(it.key(), it.value());

    for (auto it = overlay.metadata.constBegin(); it != overlay.metadata.constEnd(); ++it)
        base.metadata.insert(it.key(), it.value());
}

PlatformProfile PlatformProfileLoader::parseProfileObject(const QJsonObject& obj,
                                                        const QString& sourceLabel,
                                                        QStringList* warnings)
{
    static const QSet<QString> kKnownTopLevel = {
        QStringLiteral("id"),
        QStringLiteral("displayName"),
        QStringLiteral("description"),
        QStringLiteral("extends"),
        QStringLiteral("rules"),
        QStringLiteral("scopes"),
        QStringLiteral("metadata"),
    };

    PlatformProfile profile;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        if (!kKnownTopLevel.contains(it.key()) && warnings) {
            warnings->append(QStringLiteral("Ignoring unknown profile field '%1' in %2")
                                 .arg(it.key(), sourceLabel));
        }
    }

    profile.id = obj.value(QStringLiteral("id")).toString().trimmed();
    profile.displayName = obj.value(QStringLiteral("displayName")).toString().trimmed();
    profile.description = obj.value(QStringLiteral("description")).toString().trimmed();
    profile.extends = obj.value(QStringLiteral("extends")).toString().trimmed();

    const QJsonObject rulesObj = obj.value(QStringLiteral("rules")).toObject();
    if (!rulesObj.isEmpty()) {
        const QVariantMap rawRules = rulesObj.toVariantMap();
        profile.rules = filterKnownRuleKeys(rawRules, sourceLabel + QStringLiteral(" rules"), warnings);
    }

    const QJsonObject scopesObj = obj.value(QStringLiteral("scopes")).toObject();
    if (!scopesObj.isEmpty())
        profile.scopes = scopesObj.toVariantMap();

    const QJsonObject metaObj = obj.value(QStringLiteral("metadata")).toObject();
    if (!metaObj.isEmpty())
        profile.metadata = metaObj.toVariantMap();

    return profile;
}

PlatformProfileLoadResult PlatformProfileLoader::loadFile(const QString& absolutePath,
                                                          const QString& displayIdHint,
                                                          const QString& resolveExtendsInDir)
{
    PlatformProfileLoadResult result;
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot open platform profile file: %1").arg(absolutePath);
        return result;
    }

    const QByteArray bytes = file.readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        result.error = QStringLiteral("Invalid JSON in platform profile '%1': %2")
                           .arg(absolutePath, parseError.errorString());
        return result;
    }

    const QString sourceLabel = QFileInfo(absolutePath).fileName();
    result.profile =
        parseProfileObject(doc.object(), sourceLabel, &result.warnings);

    if (result.profile.id.isEmpty())
        result.profile.id = displayIdHint.isEmpty() ? QFileInfo(absolutePath).completeBaseName()
                                                    : displayIdHint;
    else if (!displayIdHint.isEmpty()
             && result.profile.id.compare(displayIdHint, Qt::CaseSensitive) != 0) {
        result.error = QStringLiteral("Platform profile id mismatch in '%1': expected '%2', got '%3'")
                           .arg(absolutePath, displayIdHint, result.profile.id);
        return result;
    }

    if (result.profile.id.isEmpty()) {
        result.error =
            QStringLiteral("Platform profile '%1' is missing required field 'id'").arg(absolutePath);
        return result;
    }

    result.ok = true;
    return result;
}

PlatformProfileLoadResult PlatformProfileLoader::loadResolved(const QString& pathOrId,
                                                              QStringList& visitChain,
                                                              const QString& resolveExtendsInDir)
{
    const QString normalized = normalizeProfileId(pathOrId);
    if (normalized.isEmpty()) {
        PlatformProfileLoadResult r;
        r.error = QStringLiteral("Platform profile id is empty");
        return r;
    }

    if (visitChain.contains(normalized, Qt::CaseInsensitive)) {
        PlatformProfileLoadResult r;
        r.error = QStringLiteral("Circular profile inheritance: %1 -> %2")
                      .arg(visitChain.join(QStringLiteral(" -> ")), normalized);
        return r;
    }
    visitChain.append(normalized);

    PlatformProfileLoadResult loaded;
    QString extendsDir = resolveExtendsInDir;
    if (looksLikeFilesystemPath(normalized)) {
        QFileInfo fi(normalized);
        if (!fi.exists()) {
            loaded.error = QStringLiteral("Platform profile file not found: %1").arg(normalized);
            return loaded;
        }
        extendsDir = fi.absolutePath();
        loaded = loadFile(fi.absoluteFilePath(), {}, extendsDir);
    } else {
        bool found = false;
        for (const QString& dir : candidateBuiltinProfileDirectories()) {
            const QString candidate =
                QDir(dir).absoluteFilePath(normalized + QStringLiteral(".json"));
            if (QFileInfo::exists(candidate)) {
                loaded = loadFile(candidate, normalized, QDir(dir).absolutePath());
                found = true;
                break;
            }
        }
        if (!found) {
            const QString searched = candidateBuiltinProfileDirectories().join(QStringLiteral(", "));
            loaded.error = QStringLiteral("Unknown platform profile '%1'. Built-in profiles are loaded from: %2")
                               .arg(normalized, searched.isEmpty() ? QStringLiteral("(none)") : searched);
            return loaded;
        }
    }

    if (!loaded.ok)
        return loaded;

    if (!loaded.profile.extends.isEmpty()) {
        QString parentRef = loaded.profile.extends;
        if (!extendsDir.isEmpty() && !looksLikeFilesystemPath(parentRef)) {
            const QString sibling =
                QDir(extendsDir).absoluteFilePath(parentRef + QStringLiteral(".json"));
            if (QFileInfo::exists(sibling))
                parentRef = sibling;
        }

        PlatformProfileLoadResult parent = loadResolved(parentRef, visitChain, extendsDir);
        if (!parent.ok) {
            parent.error = QStringLiteral("Profile '%1' extends '%2': %3")
                               .arg(loaded.profile.id, loaded.profile.extends, parent.error);
            return parent;
        }
        for (const QString& w : parent.warnings)
            loaded.warnings.append(w);

        PlatformProfile merged = parent.profile;
        mergeProfileOnto(merged, loaded.profile);
        merged.extends.clear();
        loaded.profile = merged;
    }

    loaded.ok = true;
    return loaded;
}

PlatformProfileLoadResult PlatformProfileLoader::load(const QString& pathOrId)
{
    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                  QStringLiteral("platform profile load requested: %1").arg(pathOrId));
    QStringList visitChain;
    PlatformProfileLoadResult res = loadResolved(pathOrId, visitChain, {});
    if (res.ok) {
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                      QStringLiteral("platform profile loaded: %1").arg(res.profile.id));
    } else {
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
                                      QStringLiteral("platform profile load failed: %1").arg(res.error),
                                      QStringLiteral("warning"));
    }
    return res;
}

void applyPlatformProfile(ScanConfig& config, const PlatformProfile& profile)
{
    SentryReporter::addBreadcrumb(QStringLiteral("ui.action"),
                                  QStringLiteral("platform profile applied: %1").arg(profile.id));
    QStringList warnings;
    config.applyRuleOverrides(filterKnownRuleKeys(
        profile.rules,
        QStringLiteral("profile '%1'").arg(profile.id),
        &warnings));
    if (!profile.scopes.isEmpty())
        appendProfileScopes(config, profile.scopes, &warnings);

    const QVariantMap meta = profile.metadata;
    if (meta.value(QStringLiteral("inspect_textures")).toBool()
        || meta.value(QStringLiteral("inspectTextures")).toBool()) {
        config.probeTextureFiles = true;
    }

    for (const QString& w : warnings) {
        QTextStream(stderr) << "Warning: " << w << Qt::endl;
    }
}
