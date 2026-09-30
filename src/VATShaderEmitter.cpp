/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon/)

The MIT License
-----------------------------------------------------------------------------------
*/

#include "VATShaderEmitter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

namespace {

// Source path under `:/vat-shaders/` (where the Qt resource lands) and
// the on-disk filename written to the bake's output directory. Kept in
// the order Godot → Unity → Unreal so `writeShaders` emits a stable
// ordering for tests and for the CLI's printed file list.
struct EngineSpec {
    const char* engine;       // canonical lowercase id
    const char* resourcePath; // qrc path (with the `:` prefix)
    const char* outputName;   // filename written to outputDir
};

const EngineSpec kSpecs[] = {
    { "godot",  ":/vat-shaders/openvat.gdshader", "openvat.gdshader" },
    { "unity",  ":/vat-shaders/openvat.shader",   "openvat.shader"   },
    { "unreal", ":/vat-shaders/openvat.usf",      "openvat.usf"      },
};

// Rigid-body (#522) variants. An engine absent here has no rigid
// template yet: `writeShaders(.., rigidMode=true)` skips it rather
// than shipping the per-vertex shader, which would decode a chunk
// texture as if every column were a vertex and render garbage.
const EngineSpec kRigidSpecs[] = {
    { "godot",  ":/vat-shaders/openvat_rigid.gdshader", "openvat_rigid.gdshader" },
};

const EngineSpec* findSpec(const QString& engineLower)
{
    for (const auto& s : kSpecs) {
        if (engineLower == QLatin1String(s.engine))
            return &s;
    }
    return nullptr;
}

// Copy a Qt resource verbatim to `dstPath`, overwriting any existing
// file. Returns true if the destination is on disk after the call.
bool copyResource(const QString& resourcePath, const QString& dstPath)
{
    QFile src(resourcePath);
    if (!src.open(QIODevice::ReadOnly))
        return false;
    QByteArray bytes = src.readAll();
    src.close();

    QFile dst(dstPath);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const qint64 written = dst.write(bytes);
    dst.close();
    return written == bytes.size();
}

} // namespace

QStringList VATShaderEmitter::parseEngineList(const QString& csv,
                                               QStringList* rejectedOut)
{
    QStringList out;
    if (rejectedOut) rejectedOut->clear();
    if (csv.trimmed().isEmpty())
        return out;
    // Collect valid tokens into a set, then walk the canonical engine
    // spec order to fill `out`. This keeps the output order stable
    // (godot, unity, unreal) regardless of input order — the API
    // contract callers rely on for displaying / reporting the engine
    // list, and what the docstring promises.
    QSet<QString> requested;
    const auto tokens = csv.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& raw : tokens) {
        const QString trimmed = raw.trimmed();
        if (trimmed.isEmpty()) continue;
        const QString t = trimmed.toLower();
        if (t == QLatin1String("all")) {
            for (const auto& s : kSpecs)
                requested.insert(QString::fromLatin1(s.engine));
            continue;
        }
        if (findSpec(t)) {
            requested.insert(t);
        } else if (rejectedOut) {
            // Preserve original casing so the warning shows the user
            // exactly what they typed (helps diagnose typos).
            rejectedOut->append(trimmed);
        }
    }
    for (const auto& s : kSpecs) {
        const QString name = QString::fromLatin1(s.engine);
        if (requested.contains(name))
            out.append(name);
    }
    return out;
}

QStringList VATShaderEmitter::rigidEngines()
{
    QStringList out;
    for (const auto& s : kRigidSpecs) out.append(QString::fromLatin1(s.engine));
    return out;
}

QStringList VATShaderEmitter::writeShaders(const QString& outputDir,
                                           const QStringList& engines,
                                           bool rigidMode)
{
    QStringList written;
    if (outputDir.isEmpty() || engines.isEmpty())
        return written;

    QDir dir(outputDir);
    if (!dir.exists() && !QDir().mkpath(outputDir))
        return written;

    // Stable lowercased subset against the canonical engine list. We
    // also dedupe here in case the caller passed `engines` directly
    // (e.g. from a QML checkbox set) without going through
    // parseEngineList.
    QSet<QString> requested;
    for (const QString& e : engines)
        requested.insert(e.trimmed().toLower());

    auto writeSpec = [&](const EngineSpec& spec) {
        if (!requested.contains(QString::fromLatin1(spec.engine)))
            return;
        const QString dst = QFileInfo(dir.filePath(QString::fromLatin1(
            spec.outputName))).absoluteFilePath();
        if (copyResource(QString::fromLatin1(spec.resourcePath), dst))
            written.append(dst);
    };
    if (rigidMode) {
        for (const auto& spec : kRigidSpecs) writeSpec(spec);
    } else {
        for (const auto& spec : kSpecs) writeSpec(spec);
    }

    // README — only when at least one engine was actually written, so
    // bakes the user explicitly didn't want any shader for don't get
    // an unsolicited file. The README is a small markdown doc with
    // pointers to the per-engine integration notes.
    if (!written.isEmpty()) {
        const QString readmePath = QFileInfo(dir.filePath(
            QStringLiteral("OpenVAT_README.md"))).absoluteFilePath();
        if (copyResource(QStringLiteral(":/vat-shaders/README.md"), readmePath))
            written.append(readmePath);
    }

    return written;
}
