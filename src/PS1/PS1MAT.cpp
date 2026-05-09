/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/
#include "PS1/PS1MAT.h"

#include <QFile>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>

namespace PS1MAT {

static bool isSkippable(const QString& line)
{
    const QString t = line.trimmed();
    return t.isEmpty() || t.startsWith('#') || t.startsWith("//") || t.startsWith(';');
}

bool parseMatFile(const QString& matPath, QVector<MatEntry>& outEntries, QString* outError)
{
    outEntries.clear();

    QFile f(matPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (outError) *outError = QStringLiteral("Could not open MAT file.");
        return false;
    }

    const QString text = QString::fromLatin1(f.readAll());
    const QStringList lines = text.split(QRegularExpression(QStringLiteral(R"(\r\n|\n|\r)")));

    int expected = -1;
    bool sawHeader = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString t = lines[i].trimmed();
        if (t.startsWith('@')) {
            if (t.startsWith(QStringLiteral("@MAT"), Qt::CaseInsensitive))
                sawHeader = true;
            continue;
        }
        if (isSkippable(t))
            continue;
        bool ok = false;
        const int n = t.toInt(&ok, 10);
        if (ok) {
            expected = n;
            break;
        }
    }

    if (!sawHeader) {
        if (outError) *outError = QStringLiteral("Missing @MAT header.");
        return false;
    }

    if (expected <= 0) {
        if (outError) *outError = QStringLiteral("Missing item count.");
        return false;
    }

    // Parse material lines: we only require at least 3 trailing ints as RGB.
    for (const QString& line : lines) {
        const QString t = line.trimmed();
        if (isSkippable(t) || t.startsWith('@'))
            continue;
        const QStringList parts = t.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() < 3)
            continue;
        bool okR=false, okG=false, okB=false;
        const int r = parts[parts.size()-3].toInt(&okR, 10);
        const int g = parts[parts.size()-2].toInt(&okG, 10);
        const int b = parts[parts.size()-1].toInt(&okB, 10);
        if (!okR || !okG || !okB)
            continue;
        MatEntry e;
        e.rgb = QColor(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
        outEntries.push_back(e);
        if (outEntries.size() >= expected)
            break;
    }

    if (outEntries.isEmpty()) {
        if (outError) *outError = QStringLiteral("No material entries parsed.");
        return false;
    }

    if (outEntries.size() < expected) {
        if (outError)
            *outError = QStringLiteral("Parsed %1 of %2 material entries.")
                            .arg(outEntries.size())
                            .arg(expected);
        return false;
    }

    return true;
}

bool writeMatFile(const QString& matPath, const QVector<MatEntry>& entries, QString* outError)
{
    QFile f(matPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (outError) *outError = QStringLiteral("Could not open MAT file for writing.");
        return false;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Latin1);

    ts << "@MAT940801\n";
    ts << entries.size() << "\n";
    for (int i = 0; i < entries.size(); ++i) {
        const QColor c = entries[i].rgb;
        ts << i << " 0 F C "
           << qBound(0, c.red(), 255) << " "
           << qBound(0, c.green(), 255) << " "
           << qBound(0, c.blue(), 255) << "\n";
    }

    if (ts.status() != QTextStream::Ok) {
        if (outError) *outError = QStringLiteral("Write failed.");
        return false;
    }
    return true;
}

} // namespace PS1MAT

