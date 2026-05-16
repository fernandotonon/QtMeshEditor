#include "UpdateVersion.h"

#include <QRegularExpression>
#include <QString>
#include <QVersionNumber>

namespace UpdateVersion {

QString normalize(const QString& raw)
{
    QString s = raw.trimmed();
    if (s.isEmpty()) return {};

    // Strip a single leading `v` / `V`. Common GitHub habit.
    if (s.startsWith(QLatin1Char('v')) || s.startsWith(QLatin1Char('V')))
        s = s.mid(1);

    // Cut off pre-release / build metadata. Semver lets us treat
    // anything after `-` or `+` as a non-numeric suffix; for the
    // purposes of "is there a newer stable release" we just drop it.
    const int dash  = s.indexOf(QLatin1Char('-'));
    const int plus  = s.indexOf(QLatin1Char('+'));
    int cut = -1;
    if (dash >= 0) cut = dash;
    if (plus >= 0 && (cut < 0 || plus < cut)) cut = plus;
    if (cut >= 0) s = s.left(cut);

    // Require the result to look like one or more dot-separated
    // numeric components, nothing else. This blocks "latest",
    // "main", and the empty string from being treated as a version.
    static const QRegularExpression rx(QStringLiteral("^\\d+(?:\\.\\d+)*$"));
    if (!rx.match(s).hasMatch()) return {};

    return s;
}

Comparison compare(const QString& localVersion, const QString& remoteTag)
{
    const QString local = normalize(localVersion);
    const QString remote = normalize(remoteTag);
    if (local.isEmpty() || remote.isEmpty())
        return Comparison::Invalid;

    bool okL = false, okR = false;
    const QVersionNumber lv = QVersionNumber::fromString(local, /*suffixIndex=*/nullptr);
    const QVersionNumber rv = QVersionNumber::fromString(remote, /*suffixIndex=*/nullptr);
    okL = !lv.isNull();
    okR = !rv.isNull();
    if (!okL || !okR) return Comparison::Invalid;

    const int cmp = QVersionNumber::compare(lv, rv);
    if (cmp <  0) return Comparison::Older;
    if (cmp >  0) return Comparison::Newer;
    return Comparison::Same;
}

} // namespace UpdateVersion
