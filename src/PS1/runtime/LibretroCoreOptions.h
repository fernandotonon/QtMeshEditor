#ifndef LIBRETROCOREOPTIONS_H
#define LIBRETROCOREOPTIONS_H

#include <QByteArray>
#include <QFileInfo>
#include <QList>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>

#include <QtGlobal>

#include <cstring>

/** libretro core-option overrides for PS1 rip (#660). */
namespace LibretroCoreOptions {

inline bool isHardwareOnlyCorePath(const QString &corePath)
{
    const QString base = QFileInfo(corePath).fileName();
    return base.contains(QStringLiteral("beetle_psx_hw"), Qt::CaseInsensitive)
           || base.contains(QStringLiteral("_hw_libretro"), Qt::CaseInsensitive);
}

/** software (default), hardware, or auto (do not override renderer keys). */
inline QByteArray rendererPreferenceFromEnv()
{
    const QByteArray env = qgetenv("QTMESH_PS1_LIBRETRO_RENDERER").trimmed().toLower();
    if (env == "hardware" || env == "hardware_gl" || env == "hardware_vk")
        return QByteArray("hardware_gl");
    if (env == "auto")
        return {};
    return QByteArray("software");
}

inline bool isRendererVariableKey(const char *key)
{
    if (!key)
        return false;
    return strcmp(key, "beetle_psx_renderer") == 0
           || strcmp(key, "beetle_psx_hw_renderer") == 0
           || strcmp(key, "mednafen_psx_renderer") == 0;
}

/** Returns nullptr when the frontend should not override this key. */
inline const char *valueForKey(const char *key, const QByteArray &rendererPreference)
{
    if (!key)
        return nullptr;

    if (strcmp(key, "beetle_psx_skip_bios") == 0)
        return "enabled";
    if (strcmp(key, "beetle_psx_override_bios") == 0)
        return "disabled";

    if (isRendererVariableKey(key)) {
        if (rendererPreference.isEmpty())
            return nullptr;
        if (rendererPreference == "software")
            return "software";
        if (rendererPreference == "hardware_gl")
            return "hardware_gl";
        return rendererPreference.constData();
    }

    return nullptr;
}

/**
 * Merge `managed` `key = "value"` pairs into an existing core-config line set,
 * preserving every untouched line (comments, blank lines, unrelated keys) in
 * its original order. Managed keys are removed in-place and re-appended at the
 * end so multiple QtMeshEditor sessions converge to the same canonical layout.
 */
inline QStringList mergeCoreConfigLines(
    const QStringList &existingLines,
    const QList<QPair<QByteArray, QByteArray>> &managed)
{
    QSet<QString> managedKeys;
    for (const auto &kv : managed)
        managedKeys.insert(QString::fromLatin1(kv.first));

    QStringList out;
    out.reserve(existingLines.size() + managed.size());
    for (const QString &line : existingLines) {
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq < 0) {
            out.append(line);
            continue;
        }
        const QString key = line.left(eq).trimmed();
        if (managedKeys.contains(key))
            continue;
        out.append(line);
    }
    for (const auto &kv : managed) {
        out.append(QStringLiteral("%1 = \"%2\"")
                       .arg(QString::fromLatin1(kv.first),
                            QString::fromLatin1(kv.second)));
    }
    return out;
}

} // namespace LibretroCoreOptions

#endif // LIBRETROCOREOPTIONS_H
