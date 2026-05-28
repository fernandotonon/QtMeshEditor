#ifndef PLATFORMPROFILE_H
#define PLATFORMPROFILE_H

#include "ScanConfig.h"

#include <QString>
#include <QStringList>
#include <QVariantMap>

/**
 * Platform profile — reusable named scan rule bundle (issue #362).
 *
 * JSON schema (profiles/<id>.json or project-local path):
 *
 *   id            (string, required)  Profile identifier; must match filename stem when built-in.
 *   displayName   (string, optional) Human-readable title.
 *   description   (string, optional) Short documentation blurb.
 *   extends       (string, optional) Parent profile id for inheritance (child overrides parent).
 *   rules         (object, optional)  Rule keys aligned with qtmesh.yml `rules:` (snake_case).
 *   scopes        (object, optional) Path glob → rule overrides (same as qtmesh.yml `scopes:`).
 *   metadata      (object, optional) Docs-only hints (engine, platform); not applied to ScanConfig.
 *
 * Unknown top-level keys are ignored with a warning. Unknown keys inside `rules` are warned
 * and skipped. Unknown keys inside `scopes` rule maps are warned per scope.
 *
 * Config precedence when scanning:
 *   1. ScanConfig::defaults()
 *   2. Selected platform profile (--profile or `profile:` in qtmesh.yml)
 *   3. Local project config (scan / rules / scopes / fix / report)
 *   4. CLI flags (highest)
 */
struct PlatformProfile {
    QString id;
    QString displayName;
    QString description;
    QString extends;
    QVariantMap rules;
    QVariantMap scopes;
    QVariantMap metadata;
};

struct PlatformProfileLoadResult {
    bool ok = false;
    QString error;
    PlatformProfile profile;
    QStringList warnings;
};

class PlatformProfileLoader {
public:
    /// Load by built-in id (e.g. `example-minimal`) or explicit `.json` path.
    static PlatformProfileLoadResult load(const QString& pathOrId);

    /// Built-in profile ids discovered under builtinProfilesDirectory().
    static QStringList listBuiltinIds();

    /// Directory searched for built-in profiles (also used by tests).
    static QString builtinProfilesDirectory();

    /// Rule keys understood by ScanConfig::applyRuleOverrides (for unknown-key warnings).
    static QStringList knownRuleKeys();

private:
    static PlatformProfileLoadResult loadFile(const QString& absolutePath,
                                              const QString& displayIdHint = {},
                                              const QString& resolveExtendsInDir = {});
    static PlatformProfileLoadResult loadResolved(const QString& pathOrId,
                                                  QStringList& visitChain,
                                                  const QString& resolveExtendsInDir = {});
    static PlatformProfile parseProfileObject(const QJsonObject& obj,
                                              const QString& sourceLabel,
                                              QStringList* warnings);
    static void mergeProfileOnto(PlatformProfile& base, const PlatformProfile& overlay);
};

/// Merge profile rules and scopes onto @p config (does not reset scan/fix/report).
void applyPlatformProfile(ScanConfig& config, const PlatformProfile& profile);

#endif // PLATFORMPROFILE_H
