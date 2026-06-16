#include "UpdateVersion.h"

#include <QRegularExpression>
#include <QString>
#include <QVersionNumber>
#include <optional>

namespace UpdateVersion {

namespace {

struct ParsedVersion {
    QVersionNumber core;
    QString prereleaseSuffix; // text after `-`, before `+`; empty for stable tags
};

QString stripLeadingVAndBuildMetadata(QString s)
{
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V'))) {
        s = s.mid(1);
    }

    const qsizetype plus = s.indexOf(QLatin1Char('+'));
    if (plus >= 0) {
        s = s.left(plus);
    }
    return s;
}

std::optional<ParsedVersion> parseReleaseOnly(const QString& raw)
{
    QString s = stripLeadingVAndBuildMetadata(raw.trimmed());
    if (s.isEmpty()) {
        return std::nullopt;
    }

    const qsizetype dash = s.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        s = s.left(dash);
    }

    static const QRegularExpression rx(QStringLiteral("^\\d+(?:\\.\\d+)*$"));
    if (!rx.match(s).hasMatch()) {
        return std::nullopt;
    }

    const QVersionNumber core = QVersionNumber::fromString(s);
    if (core.isNull()) {
        return std::nullopt;
    }
    return ParsedVersion{core, {}};
}

std::optional<ParsedVersion> parseWithPrerelease(const QString& raw)
{
    QString s = stripLeadingVAndBuildMetadata(raw.trimmed());
    if (s.isEmpty()) {
        return std::nullopt;
    }

    QString corePart = s;
    QString suffix;
    const qsizetype dash = s.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        corePart = s.left(dash);
        suffix = s.mid(dash + 1);
    }

    static const QRegularExpression rx(QStringLiteral("^\\d+(?:\\.\\d+)*$"));
    if (!rx.match(corePart).hasMatch()) {
        return std::nullopt;
    }

    const QVersionNumber core = QVersionNumber::fromString(corePart);
    if (core.isNull()) {
        return std::nullopt;
    }
    return ParsedVersion{core, suffix};
}

Comparison compareParsed(const ParsedVersion& local, const ParsedVersion& remote)
{
    const int coreCmp = QVersionNumber::compare(local.core, remote.core);
    if (coreCmp < 0) {
        return Comparison::Older;
    }
    if (coreCmp > 0) {
        return Comparison::Newer;
    }

    if (local.prereleaseSuffix.isEmpty() && remote.prereleaseSuffix.isEmpty()) {
        return Comparison::Same;
    }
    if (local.prereleaseSuffix.isEmpty()) {
        return Comparison::Newer;
    }
    if (remote.prereleaseSuffix.isEmpty()) {
        return Comparison::Older;
    }

    const int suffixCmp =
        QString::compare(local.prereleaseSuffix, remote.prereleaseSuffix, Qt::CaseInsensitive);
    if (suffixCmp < 0) {
        return Comparison::Older;
    }
    if (suffixCmp > 0) {
        return Comparison::Newer;
    }
    return Comparison::Same;
}

} // namespace

QString normalize(const QString& raw)
{
    const auto parsed = parseReleaseOnly(raw);
    if (!parsed) {
        return {};
    }

    QString s = stripLeadingVAndBuildMetadata(raw.trimmed());
    const qsizetype dash = s.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        s = s.left(dash);
    }
    return s;
}

Comparison compare(const QString& localVersion,
                   const QString& remoteTag,
                   const CompareMode mode)
{
    if (mode == CompareMode::ReleaseOnly) {
        const auto local = parseReleaseOnly(localVersion);
        const auto remote = parseReleaseOnly(remoteTag);
        if (!local || !remote) {
            return Comparison::Invalid;
        }
        return compareParsed(*local, *remote);
    }

    const auto local = parseWithPrerelease(localVersion);
    const auto remote = parseWithPrerelease(remoteTag);
    if (!local || !remote) {
        return Comparison::Invalid;
    }
    return compareParsed(*local, *remote);
}

} // namespace UpdateVersion
