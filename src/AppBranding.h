#ifndef APP_BRANDING_H
#define APP_BRANDING_H

#include <QString>

/**
 * @brief Product naming: the display name and the technical name are SEPARATE.
 *
 * "QtMesh Editor" is the product name users see (part of the QtMesh family:
 * QtMesh Cloud, QtMesh ID, QtMesh Games). "QtMeshEditor" stays the technical
 * identifier everywhere it is persisted or matched: the Qt organization and
 * application names (QSettings, AppData paths, credentials, recent files),
 * the executable and bundle names, the repository, CMake targets, the MCP
 * server name and installer/package ids. Changing the technical name would
 * orphan every existing install's settings and session, so only the display
 * name follows the brand.
 */
namespace AppBranding {

/// What the user reads: titles, headings, dialogs, store listings.
inline QString displayName() { return QStringLiteral("QtMesh Editor"); }

/// Persisted / matched identifier. Do not change (see above).
inline QString technicalName() { return QStringLiteral("QtMeshEditor"); }

} // namespace AppBranding

#endif // APP_BRANDING_H
