#include "HDR/HdrMaterialScript.h"

#include <QRegularExpression>
#include <QtGlobal>

namespace HdrMaterialScript {

namespace {

constexpr QLatin1String kIntensityPrefix("pbr_environment_intensity");
constexpr QLatin1String kTintPrefix("pbr_environment_tint");

bool isEnvironmentLine(const QString& trimmed)
{
    return trimmed.startsWith(kIntensityPrefix) || trimmed.startsWith(kTintPrefix);
}

int findPassBlockInsertLine(const QStringList& lines)
{
    int braceDepth = 0;
    bool inPass = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines[i].trimmed();
        if (trimmed.startsWith(QLatin1String("pass"))
            && (trimmed == QLatin1String("pass")
                || trimmed.startsWith(QLatin1String("pass ")))) {
            inPass = true;
            braceDepth = 0;
        }
        if (!inPass)
            continue;
        if (trimmed.contains(QLatin1Char('{')))
            ++braceDepth;
        if (trimmed.contains(QLatin1Char('}'))) {
            --braceDepth;
            if (braceDepth <= 0)
                return i;
        }
    }
    return -1;
}

} // namespace

QString stripEnvironmentLines(const QString& materialScript)
{
    QStringList out;
    for (const QString& line : materialScript.split(QLatin1Char('\n'))) {
        if (!isEnvironmentLine(line.trimmed()))
            out.append(line);
    }
    while (!out.isEmpty() && out.last().trimmed().isEmpty())
        out.removeLast();
    return out.join(QLatin1Char('\n'));
}

QString injectEnvironmentLines(const QString& materialScript,
                               float intensity,
                               const QColor& tint)
{
    QString base = stripEnvironmentLines(materialScript);
    QStringList lines = base.split(QLatin1Char('\n'));

    const QString intensityLine =
        QStringLiteral("            pbr_environment_intensity %1")
            .arg(intensity, 0, 'f', 3);
    const QString tintLine =
        QStringLiteral("            pbr_environment_tint %1 %2 %3")
            .arg(tint.redF(), 0, 'f', 3)
            .arg(tint.greenF(), 0, 'f', 3)
            .arg(tint.blueF(), 0, 'f', 3);

    const int insertAt = findPassBlockInsertLine(lines);
    if (insertAt >= 0) {
        lines.insert(insertAt, intensityLine);
        lines.insert(insertAt + 1, tintLine);
    } else {
        if (!lines.isEmpty() && !lines.last().trimmed().isEmpty())
            lines.append(QString());
        lines.append(intensityLine.trimmed());
        lines.append(tintLine.trimmed());
    }
    return lines.join(QLatin1Char('\n'));
}

bool parseEnvironmentLines(const QString& materialScript,
                           float& intensityOut,
                           QColor& tintOut)
{
    intensityOut = kDefaultEnvIntensity;
    tintOut = QColor::fromRgbF(1., 1., 1.);
    bool found = false;

    for (const QString& line : materialScript.split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        if (trimmed.startsWith(kIntensityPrefix)) {
            const QStringList parts = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                                    Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                intensityOut = qBound(kMinEnvIntensity, parts[1].toFloat(), kMaxEnvIntensity);
                found = true;
            }
        } else if (trimmed.startsWith(kTintPrefix)) {
            const QStringList parts = trimmed.split(QRegularExpression(QStringLiteral("\\s+")),
                                                    Qt::SkipEmptyParts);
            if (parts.size() >= 4) {
                tintOut = QColor::fromRgbF(parts[1].toFloat(),
                                           parts[2].toFloat(),
                                           parts[3].toFloat());
                found = true;
            }
        }
    }
    return found;
}

} // namespace HdrMaterialScript
