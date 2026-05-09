/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "PS1/PS1RSD.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>
#include <QTextStream>

namespace {

static bool isCommentLine(const QString& s)
{
    const QString t = s.trimmed();
    return t.startsWith(QStringLiteral("#")) || t.startsWith(QStringLiteral("//")) || t.startsWith(QStringLiteral(";"));
}

static bool parseKeyValue(const QString& line, QString& outKey, QString& outValue)
{
    const int eq = line.indexOf('=');
    if (eq <= 0)
        return false;
    outKey = line.left(eq).trimmed();
    outValue = line.mid(eq + 1).trimmed();
    if (outKey.isEmpty())
        return false;
    return true;
}

static int parseTexIndex(const QString& key)
{
    // TEX[0]
    const int lb = key.indexOf('[');
    const int rb = key.indexOf(']');
    if (lb < 0 || rb < 0 || rb <= lb + 1)
        return -1;
    bool ok = false;
    const int idx = key.mid(lb + 1, rb - lb - 1).toInt(&ok, 10);
    return ok ? idx : -1;
}

} // namespace

namespace PS1RSD {

bool parseRsdFile(const QString& rsdPath, RsdDescriptor& out, QString* outError)
{
    out = {};

    QFile f(rsdPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (outError) *outError = QStringLiteral("Could not open file.");
        return false;
    }

    const QByteArray bytes = f.readAll();
    const QString text = QString::fromLatin1(bytes);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\\r?\\n")), Qt::KeepEmptyParts);

    bool sawAnyKV = false;
    for (QString line : lines) {
        line = line.trimmed();
        if (line.isEmpty() || isCommentLine(line))
            continue;

        if (line.startsWith('@')) {
            // Header line like "@RSD940102"
            if (out.headerId.isEmpty())
                out.headerId = line;
            continue;
        }

        QString key, value;
        if (!parseKeyValue(line, key, value))
            continue;

        sawAnyKV = true;
        const QString kUpper = key.toUpper();

        if (kUpper == QStringLiteral("PLY")) {
            out.plyPath = value;
        } else if (kUpper == QStringLiteral("MAT")) {
            out.matPath = value;
        } else if (kUpper == QStringLiteral("GRP")) {
            out.grpPath = value;
        } else if (kUpper == QStringLiteral("NTEX")) {
            bool ok = false;
            const int n = value.toInt(&ok, 10);
            if (ok) out.ntex = n;
        } else if (kUpper.startsWith(QStringLiteral("TEX["))) {
            const int idx = parseTexIndex(key);
            if (idx < 0) {
                // Keep but append in order encountered.
                out.textures << value;
            } else {
                if (out.textures.size() <= idx)
                    out.textures.resize(idx + 1);
                out.textures[idx] = value;
            }
        } else {
            // Unknown keys are ignored for forward compatibility.
        }
    }

    if (!sawAnyKV) {
        if (outError) *outError = QStringLiteral("No key/value pairs found (is this an ASCII RSD?).");
        return false;
    }

    if (out.headerId.isEmpty())
        out.headerId = QStringLiteral("@RSD940102");

    // Trim empty entries (can happen if TEX[2] exists without TEX[0..1]).
    while (!out.textures.isEmpty() && out.textures.last().trimmed().isEmpty())
        out.textures.removeLast();

    return true;
}

bool writeRsdFile(const QString& rsdPath, const RsdDescriptor& desc, QString* outError)
{
    QFile f(rsdPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        if (outError) *outError = QStringLiteral("Could not open file for writing.");
        return false;
    }

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Latin1);

    const QString header = desc.headerId.trimmed().isEmpty() ? QStringLiteral("@RSD940102") : desc.headerId.trimmed();
    ts << header << "\n";

    if (!desc.plyPath.trimmed().isEmpty())
        ts << "PLY=" << desc.plyPath.trimmed() << "\n";
    if (!desc.matPath.trimmed().isEmpty())
        ts << "MAT=" << desc.matPath.trimmed() << "\n";
    if (!desc.grpPath.trimmed().isEmpty())
        ts << "GRP=" << desc.grpPath.trimmed() << "\n";

    const QStringList tex = desc.textures;
    const int ntex = desc.ntex >= 0 ? desc.ntex : tex.size();
    if (ntex > 0 || !tex.isEmpty()) {
        ts << "NTEX=" << ntex << "\n";
        for (int i = 0; i < tex.size(); ++i) {
            const QString v = tex[i].trimmed();
            if (v.isEmpty())
                continue;
            ts << "TEX[" << i << "]=" << v << "\n";
        }
    }

    if (ts.status() != QTextStream::Ok) {
        if (outError) *outError = QStringLiteral("Write failed.");
        return false;
    }

    return true;
}

} // namespace PS1RSD

