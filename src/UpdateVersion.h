#ifndef UPDATEVERSION_H
#define UPDATEVERSION_H

#include <QString>

/**
 * @brief Pure-data helpers for the "check for update" feature.
 *
 * Parses semver-ish version strings (`X.Y.Z`) and compares them so the
 * update prompt only fires when the remote version is *strictly newer*
 * than the local one. The previous implementation did a raw string
 * equality test against the GitHub release tag, which silently broke
 * whenever someone tagged a release with a `v` prefix, used a
 * pre-release suffix, or just added a fourth component — the user
 * would get a permanent "update available" prompt that pointed back
 * at the same version they already had.
 *
 * The helper is pure (no Qt UI, no network) so it can be exhaustively
 * unit-tested without a GL context.
 */
namespace UpdateVersion {

enum class Comparison {
    Older   = -1,  ///< local < remote → update available
    Same    =  0,  ///< local == remote → already on latest
    Newer   =  1,  ///< local > remote → user is ahead (dev builds, rollbacks)
    Invalid =  2,  ///< at least one input couldn't be parsed at all
};

/**
 * @brief Normalise a version-ish string into a canonical `X.Y.Z` form.
 *
 * Strips a single leading `v` / `V`, leading/trailing whitespace, and
 * any pre-release / build-metadata suffix (`-rc1`, `+build42`, …).
 * Returns an empty string when the input doesn't match the basic
 * shape `\\d+(\\.\\d+)*`.
 */
QString normalize(const QString& raw);

/**
 * @brief Compare a local version string against a remote release tag.
 *
 * Both inputs go through `normalize` first, so `"v3.2.0"` and
 * `"3.2.0"` compare equal. Comparison uses `QVersionNumber`, which
 * is component-wise numeric — so `"3.10.0"` is correctly newer than
 * `"3.2.0"` (string equality would have failed even before they
 * diverged).
 *
 * If either input fails to parse, returns Invalid. The caller should
 * treat Invalid as "do nothing" rather than "update available" — a
 * malformed release tag from the API shouldn't prompt the user.
 */
Comparison compare(const QString& localVersion, const QString& remoteTag);

/// True iff `remoteTag` represents a version strictly newer than
/// `localVersion`. Convenience wrapper used by the UI to decide
/// whether to show the update prompt.
inline bool isUpdateAvailable(const QString& localVersion, const QString& remoteTag)
{
    return compare(localVersion, remoteTag) == Comparison::Older;
}

} // namespace UpdateVersion

#endif // UPDATEVERSION_H
