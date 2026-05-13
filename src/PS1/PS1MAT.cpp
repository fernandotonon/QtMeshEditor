/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/
#include "PS1/PS1MAT.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>

#include <algorithm>

#include "SentryReporter.h"

namespace PS1MAT {

namespace {

constexpr int kMaxPolyEntries = 1 << 20; // sanity guard against hostile counts.

bool isSkippable(const QString& line)
{
    const QString t = line.trimmed();
    return t.isEmpty() || t.startsWith('#') || t.startsWith(QStringLiteral("//")) || t.startsWith(';');
}

/** Heuristic: a single token of length 1 made up of A-Z is a shading or type char. */
bool isSingleLetter(const QString& s)
{
    return s.size() == 1 && s[0].isLetter();
}

QColor clampedRgb(int r, int g, int b)
{
    return QColor(qBound(0, r, 255), qBound(0, g, 255), qBound(0, b, 255));
}

/** Read a single trailing color triple from `ints` starting at offset `off`. */
bool readRgb(const QVector<int>& ints, int off, QColor& outColor)
{
    if (off < 0 || off + 2 >= ints.size())
        return false;
    outColor = clampedRgb(ints[off], ints[off + 1], ints[off + 2]);
    return true;
}

/** Decode the rest-of-line payload for a parsed MAT entry. Returns false on malformed input. */
bool decodePayload(MatEntry& e, const QVector<int>& ints)
{
    // Layout summary (number of trailing ints after the type char):
    //   C  : 3                 -> 1 colour
    //   G  : 9 or 12           -> 3 or 4 colours
    //   T  : 9                 -> texIdx + 8 UVs
    //   D  : 12                -> texIdx + 8 UVs + 1 colour
    //   H  : 21                -> texIdx + 8 UVs + 12 colours (4 corners; tris pad with the last colour)
    //
    // We accept slightly degenerate counts (e.g. 6-int H tris with no padding) by
    // greedily reading what's present.
    e.uvs.clear();
    e.vertColors.clear();
    e.textured = false;
    e.textureIndex = -1;

    const int n = ints.size();

    auto readUvBlock = [&](int off, int uvCount) {
        for (int i = 0; i < uvCount && off + 1 < n; ++i, off += 2)
            e.uvs.push_back({ints[off], ints[off + 1]});
    };

    auto readColorBlock = [&](int off, int colCount) {
        for (int i = 0; i < colCount && off + 2 < n; ++i, off += 3)
            e.vertColors.push_back(clampedRgb(ints[off], ints[off + 1], ints[off + 2]));
    };

    switch (e.typeChar) {
        case 'C': {
            QColor c;
            if (!readRgb(ints, n - 3, c))
                return false;
            e.vertColors.push_back(c);
            e.rgb = c;
            return true;
        }
        case 'G': {
            // 9 ints = tri (3 RGB), 12 ints = quad (4 RGB).
            const int rgbCount = (n >= 12) ? 4 : (n >= 9 ? 3 : 0);
            if (rgbCount == 0)
                return false;
            readColorBlock(0, rgbCount);
            if (e.vertColors.isEmpty())
                return false;
            e.rgb = e.vertColors.first();
            return true;
        }
        case 'T': {
            // texIdx + 8 UVs = 9 ints.
            if (n < 9)
                return false;
            e.textured = true;
            e.textureIndex = ints[0];
            readUvBlock(1, 4);
            // T entries have no colour data; pick white so back-compat callers don't render black.
            e.rgb = QColor(255, 255, 255);
            return true;
        }
        case 'D': {
            // texIdx + 8 UVs + 3 RGB = 12 ints.
            if (n < 12)
                return false;
            e.textured = true;
            e.textureIndex = ints[0];
            readUvBlock(1, 4);
            QColor c;
            if (!readRgb(ints, 9, c))
                return false;
            e.vertColors.push_back(c);
            e.rgb = c;
            return true;
        }
        case 'H': {
            // texIdx + 8 UVs + 12 RGB = 21 ints (tris pad with last RGB + dummy zeroes).
            if (n < 9)
                return false;
            e.textured = true;
            e.textureIndex = ints[0];
            readUvBlock(1, 4);
            // Remaining ints are colours. Floor-divide by 3 to get colour count.
            const int colsStart = 9;
            const int colorInts = std::max(0, n - colsStart);
            const int colCount = std::min(4, colorInts / 3);
            readColorBlock(colsStart, colCount);
            if (e.vertColors.isEmpty())
                e.rgb = QColor(255, 255, 255);
            else
                e.rgb = e.vertColors.first();
            return true;
        }
        default:
            return false;
    }
}

bool tryParseEntryLine(const QString& line, MatEntry& outEntry)
{
    const QStringList parts = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() < 5) // at least: idx flag shading type one-color-component
        return false;

    bool okIdx = false;
    parts[0].toInt(&okIdx, 10);
    if (!okIdx)
        return false;

    // Optional packed flag integer (Blender exporter) before the two letter tokens.
    int scanStart = 1;
    int matFlags = 0;
    bool haveMatFlags = false;
    if (parts.size() > 2) {
        bool okF = false;
        const int maybe = parts[1].toInt(&okF, 10);
        if (okF && maybe >= 0 && maybe <= 0xFFFFFF) {
            matFlags = maybe;
            haveMatFlags = true;
            scanStart = 2;
        }
    }

    // Find the type char: scan from `scanStart` and pick the SECOND single-letter token
    // (first is shadingChar, second is typeChar).
    int letterCount = 0;
    int typeCharIdx = -1;
    char shadingChar = 'F';
    char typeChar = 'C';
    for (int i = scanStart; i < parts.size(); ++i) {
        const QString& p = parts[i];
        if (isSingleLetter(p)) {
            const char c = p.at(0).toUpper().toLatin1();
            if (letterCount == 0)
                shadingChar = c;
            else if (letterCount == 1) {
                typeChar = c;
                typeCharIdx = i;
                break;
            }
            ++letterCount;
        }
    }
    if (typeCharIdx < 0)
        return false;

    QVector<int> trailing;
    trailing.reserve(parts.size() - (typeCharIdx + 1));
    for (int i = typeCharIdx + 1; i < parts.size(); ++i) {
        bool ok = false;
        const int v = parts[i].toInt(&ok, 10);
        if (!ok)
            return false;
        trailing.push_back(v);
    }

    outEntry = MatEntry{};
    outEntry.shadingChar = shadingChar;
    outEntry.typeChar = typeChar;
    if (haveMatFlags)
        outEntry.unlit = (matFlags & 1) != 0;
    if (!decodePayload(outEntry, trailing))
        return false;
    return true;
}

bool readExpectedCount(const QStringList& lines, int& outCount, bool& outSawHeader)
{
    outSawHeader = false;
    outCount = -1;
    for (const QString& line : lines) {
        const QString t = line.trimmed();
        if (t.startsWith('@')) {
            if (t.startsWith(QStringLiteral("@MAT"), Qt::CaseInsensitive))
                outSawHeader = true;
            continue;
        }
        if (isSkippable(t))
            continue;
        // Lone integer => entry count.
        const QStringList parts = t.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() == 1) {
            bool ok = false;
            const int n = parts[0].toInt(&ok, 10);
            if (ok && n > 0 && n <= kMaxPolyEntries) {
                outCount = n;
                return true;
            }
        }
        // First non-count, non-comment, non-header line — we've gone too far.
        if (outCount < 0)
            return false;
    }
    return outCount > 0;
}

} // namespace

/** Blender `Playstation RSD Exporter.py` materialFlag("000",0,0,unlit) → decimal int. */
static int encodeBlenderMaterialFlagBits(bool unlit)
{
    const QString bits = QStringLiteral("00") + QStringLiteral("000") + QLatin1Char('0')
                        + QLatin1Char('0') + QLatin1Char(unlit ? '1' : '0');
    bool ok = false;
    const int v = bits.toInt(&ok, 2);
    return ok ? v : (unlit ? 1 : 0);
}

bool parseMatFile(const QString& matPath, QVector<MatEntry>& outEntries, QString* outError)
{
    outEntries.clear();

    const QString matName = QFileInfo(matPath).fileName();
    QFile f(matPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (outError) *outError = QStringLiteral("Could not open MAT file.");
        SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
            QStringLiteral("PS1 MAT open failed: %1").arg(matName));
        return false;
    }

    const QString text = QString::fromLatin1(f.readAll());
    const QStringList lines = text.split(QRegularExpression(QStringLiteral(R"(\r\n|\n|\r)")));

    int expected = -1;
    bool sawHeader = false;
    if (!readExpectedCount(lines, expected, sawHeader)) {
        if (outError) *outError = QStringLiteral("Missing item count.");
        return false;
    }

    if (!sawHeader) {
        if (outError) *outError = QStringLiteral("Missing @MAT header.");
        return false;
    }

    for (const QString& line : lines) {
        const QString t = line.trimmed();
        if (isSkippable(t) || t.startsWith('@'))
            continue;
        const QStringList parts = t.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (parts.size() == 1)
            continue; // lone count
        MatEntry e;
        if (!tryParseEntryLine(t, e))
            continue;
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

    SentryReporter::addBreadcrumb(QStringLiteral("file.import"),
        QStringLiteral("PS1 MAT parsed: %1 (%2 entries)").arg(matName).arg(outEntries.size()));
    return true;
}

bool writeMatFile(const QString& matPath, const QVector<MatEntry>& entries, QString* outError)
{
    const QString matName = QFileInfo(matPath).fileName();
    QFile f(matPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (outError) *outError = QStringLiteral("Could not open MAT file for writing.");
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("PS1 MAT write open failed: %1").arg(matName));
        return false;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Latin1);

    ts << "@MAT940801\n";
    ts << entries.size() << "\n";

    for (int i = 0; i < entries.size(); ++i) {
        const MatEntry& e = entries[i];
        char typeChar = e.typeChar;
        if (typeChar != 'C' && typeChar != 'G' && typeChar != 'T'
            && typeChar != 'D' && typeChar != 'H')
            typeChar = 'C';
        const char shading =
            (e.shadingChar == 'F' || e.shadingChar == 'G' || e.shadingChar == 'S')
                ? e.shadingChar
                : 'F';
        ts << i << " " << encodeBlenderMaterialFlagBits(e.unlit) << " " << shading << " " << typeChar;

        auto writeUvs = [&]() {
            if (e.uvs.size() >= 4) {
                for (int k = 0; k < 4; ++k)
                    ts << " " << e.uvs[k].u << " " << e.uvs[k].v;
            } else if (e.uvs.size() == 3) {
                for (int k = 0; k < 3; ++k)
                    ts << " " << e.uvs[k].u << " " << e.uvs[k].v;
                // Pad the 4th UV by repeating the last (PS1 quad-shaped slot).
                ts << " " << e.uvs[2].u << " " << e.uvs[2].v;
            } else {
                ts << " 0 0 0 0 0 0 0 0";
            }
        };

        auto writeColor = [&](const QColor& c) {
            ts << " " << qBound(0, c.red(), 255)
               << " " << qBound(0, c.green(), 255)
               << " " << qBound(0, c.blue(), 255);
        };

        switch (typeChar) {
            case 'C': {
                const QColor c = !e.vertColors.isEmpty() ? e.vertColors.first() : e.rgb;
                writeColor(c.isValid() ? c : QColor(255, 255, 255));
                break;
            }
            case 'G': {
                const int nC = (e.vertColors.size() >= 4) ? 4
                              : (e.vertColors.size() >= 3 ? 3 : 0);
                if (nC == 0) {
                    // Fall back to repeating the back-compat colour 4 times so we still produce a valid quad row.
                    const QColor c = e.rgb.isValid() ? e.rgb : QColor(255, 255, 255);
                    for (int k = 0; k < 4; ++k)
                        writeColor(c);
                } else {
                    for (int k = 0; k < nC; ++k)
                        writeColor(e.vertColors[k]);
                    // Match the Blender RSD exporter convention: pad tri G entries to a
                    // 4-corner layout with a trailing `0 0 0` so downstream tooling that
                    // assumes a fixed 4-colour stride (PS1 emulators, third-party loaders)
                    // can read every G line with the same parsing path.
                    if (nC == 3)
                        ts << " 0 0 0";
                }
                break;
            }
            case 'T': {
                ts << " " << std::max(0, e.textureIndex);
                writeUvs();
                break;
            }
            case 'D': {
                ts << " " << std::max(0, e.textureIndex);
                writeUvs();
                const QColor c = !e.vertColors.isEmpty() ? e.vertColors.first() : e.rgb;
                writeColor(c.isValid() ? c : QColor(255, 255, 255));
                break;
            }
            case 'H': {
                ts << " " << std::max(0, e.textureIndex);
                writeUvs();
                const int nC = (e.vertColors.size() >= 4) ? 4
                              : (e.vertColors.size() >= 3 ? 3 : 0);
                if (nC == 0) {
                    const QColor c = e.rgb.isValid() ? e.rgb : QColor(255, 255, 255);
                    for (int k = 0; k < 4; ++k)
                        writeColor(c);
                } else {
                    for (int k = 0; k < nC; ++k)
                        writeColor(e.vertColors[k]);
                    // Pad tris (3 colours) to the 4-corner shape for layout stability.
                    if (nC == 3)
                        ts << " 0 0 0";
                }
                break;
            }
        }

        ts << "\n";
    }

    if (ts.status() != QTextStream::Ok) {
        if (outError) *outError = QStringLiteral("Write failed.");
        SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
            QStringLiteral("PS1 MAT write failed: %1").arg(matName));
        return false;
    }
    SentryReporter::addBreadcrumb(QStringLiteral("file.export"),
        QStringLiteral("PS1 MAT written: %1 (%2 entries)").arg(matName).arg(entries.size()));
    return true;
}

} // namespace PS1MAT
