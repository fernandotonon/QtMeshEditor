#include "PsxDiscResolver.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCryptographicHash>

namespace {

QString suffixLower(const QFileInfo &info)
{
    const QString name = info.fileName().toLower();
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return {};
    return name.mid(dot);
}

bool isDirectLibretroDiscPath(const QString &suffix)
{
    return suffix == QStringLiteral(".cue") || suffix == QStringLiteral(".ccd")
           || suffix == QStringLiteral(".toc") || suffix == QStringLiteral(".m3u")
           || suffix == QStringLiteral(".pbp") || suffix == QStringLiteral(".chd")
           || suffix == QStringLiteral(".exe");
}

bool isIsoLikeSuffix(const QString &suffix)
{
    return suffix == QStringLiteral(".iso") || suffix == QStringLiteral(".img")
           || suffix.endsWith(QStringLiteral(".iso"));
}

QString escapeCuePath(const QString &path)
{
    QString escaped = QDir::toNativeSeparators(path);
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

QString wrapperDirectoryFor(const QString &absolutePath)
{
    const QByteArray hash =
        QCryptographicHash::hash(absolutePath.toUtf8(), QCryptographicHash::Sha1).toHex();
    QDir dir(QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    dir.mkpath(QStringLiteral("qtmesh_ps1_disc"));
    return dir.filePath(QStringLiteral("qtmesh_ps1_disc/%1").arg(QString::fromLatin1(hash)));
}

bool writeCueSheetForDataFile(const QString &cuePath, const QString &dataFilePath, const char *trackMode)
{
    QFile cue(cuePath);
    if (!cue.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;

    const QByteArray line1 =
        QByteArray("FILE \"") + escapeCuePath(dataFilePath).toUtf8() + "\" BINARY\n";
    if (cue.write(line1) != line1.size())
        return false;
    const QByteArray line2 =
        QByteArray("  TRACK 01 ") + trackMode + "\n    INDEX 01 00:00:00\n";
    return cue.write(line2) == line2.size();
}

QString detectBinTrackMode(qint64 fileSize)
{
    if (fileSize > 0 && (fileSize % 2352) == 0)
        return QStringLiteral("MODE2/2352");
    if (fileSize > 0 && (fileSize % 2048) == 0)
        return QStringLiteral("MODE1/2048");
    return {};
}

PsxDiscResolveResult fail(const QString &message)
{
    PsxDiscResolveResult result;
    result.errorMessage = message;
    return result;
}

PsxDiscResolveResult resolveWithCueWrapper(const QString &absolutePath, const char *trackMode)
{
    const QString wrapperDir = wrapperDirectoryFor(absolutePath);
    QDir().mkpath(wrapperDir);

    const QString cuePath = QDir(wrapperDir).filePath(QStringLiteral("wrapper.cue"));
    if (!writeCueSheetForDataFile(cuePath, absolutePath, trackMode)) {
        return fail(QStringLiteral("Could not write temporary CUE sheet for: %1").arg(absolutePath));
    }

    PsxDiscResolveResult result;
    result.ok = true;
    result.loadPath = cuePath;
    return result;
}

} // namespace

bool PsxDiscResolver::needsCueWrapper(const QString &path)
{
    const QFileInfo info(path);
    const QString suffix = suffixLower(info);
    if (isIsoLikeSuffix(suffix))
        return true;
    if (suffix != QStringLiteral(".bin"))
        return false;
    if (!info.exists())
        return true;
    return !info.dir().exists(info.completeBaseName() + QStringLiteral(".cue"));
}

PsxDiscResolveResult PsxDiscResolver::resolve(const QString &userPath)
{
    const QFileInfo info(userPath);
    if (!info.exists() || !info.isFile())
        return fail(QStringLiteral("Disc image not found: %1").arg(userPath));

    const QString absolute = info.absoluteFilePath();
    const QString suffix = suffixLower(info);

    if (isDirectLibretroDiscPath(suffix)) {
        if (suffix == QStringLiteral(".cue")) {
            QFile cueFile(absolute);
            if (!cueFile.open(QIODevice::ReadOnly)) {
                return fail(QStringLiteral("Could not read CUE sheet: %1").arg(absolute));
            }
            const QByteArray firstLine = cueFile.readLine().trimmed();
            if (!firstLine.startsWith("FILE \"")) {
                return fail(QStringLiteral("Invalid CUE sheet (missing FILE line): %1").arg(absolute));
            }
            const int closeQuote = firstLine.indexOf('"', 6);
            if (closeQuote <= 6) {
                return fail(QStringLiteral("Invalid CUE sheet (malformed FILE line): %1").arg(absolute));
            }
            QString binName = QString::fromUtf8(firstLine.mid(6, closeQuote - 6));
            binName.replace(QStringLiteral("\\\""), QStringLiteral("\""));
            const QString binPath = QFileInfo(absolute).dir().filePath(binName);
            if (!QFileInfo::exists(binPath)) {
                return fail(QStringLiteral("CUE references missing file \"%1\" (expected at %2)")
                                .arg(binName, binPath));
            }
        }
        PsxDiscResolveResult result;
        result.ok = true;
        result.loadPath = absolute;
        return result;
    }

    if (isIsoLikeSuffix(suffix))
        return resolveWithCueWrapper(absolute, "MODE1/2048");

    if (suffix == QStringLiteral(".bin")) {
        const QString cueSibling =
            info.dir().filePath(info.completeBaseName() + QStringLiteral(".cue"));
        if (QFileInfo::exists(cueSibling)) {
            PsxDiscResolveResult result;
            result.ok = true;
            result.loadPath = QFileInfo(cueSibling).absoluteFilePath();
            return result;
        }
        const QString mode = detectBinTrackMode(info.size());
        if (mode.isEmpty()) {
            return fail(QStringLiteral(
                "Unsupported .bin size (%1 bytes). Use a matching .cue or a Redump BIN+CUE rip.")
                            .arg(info.size()));
        }
        const QByteArray trackMode =
            (mode == QStringLiteral("MODE2/2352")) ? "MODE2/2352" : "MODE1/2048";
        return resolveWithCueWrapper(absolute, trackMode.constData());
    }

    return fail(QStringLiteral(
        "Unsupported disc image type (%1). Use .cue, .chd, .pbp, or a standard .iso/.bin rip.")
                    .arg(suffix.isEmpty() ? info.fileName() : suffix));
}
