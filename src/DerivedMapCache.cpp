/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — on-disk cache for cavity / curvature / AO maps
(Paint v2 Slice G, issue #550)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/
#include "DerivedMapCache.h"

#include "EditableMesh.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

#pragma pack(push, 1)
struct FileHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    int32_t  width = 0;
    int32_t  height = 0;
    uint32_t valueBytes = 0;      ///< width*height*sizeof(float)
    uint32_t coverageBytes = 0;   ///< width*height*sizeof(uint8_t)
};
#pragma pack(pop)

/// A cache key becomes a path component, so validate it structurally: exactly
/// 40 lowercase-or-upper hex chars (SHA-1 hex length). This makes "../" and
/// absolute paths unrepresentable rather than relying on sanitisation.
bool isValidKey(const QString& key)
{
    if (key.size() != 40) return false;
    for (const QChar c : key) {
        const char ch = c.toLatin1();
        const bool hex = (ch >= '0' && ch <= '9')
                      || (ch >= 'a' && ch <= 'f')
                      || (ch >= 'A' && ch <= 'F');
        if (!hex) return false;
    }
    return true;
}

QString entryFile(const QString& meshHash, DerivedMapKind kind)
{
    const QString dir = DerivedMapCache::entryDirectory(meshHash);
    if (dir.isEmpty()) return {};
    return QDir(dir).filePath(
        QString::fromLatin1(DerivedMapGenerator::kindName(kind)) + QStringLiteral(".bin"));
}

} // namespace

namespace DerivedMapCache {

QString cacheRootDirectory()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) return {};      // guard AppData being unavailable
    return QDir(base).filePath(QStringLiteral("paint/derived_maps"));
}

QString entryDirectory(const QString& meshHash)
{
    if (!isValidKey(meshHash)) return {};
    const QString root = cacheRootDirectory();
    if (root.isEmpty()) return {};
    return QDir(root).filePath(meshHash);
}

QString meshHash(const EditableMesh& mesh)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    // Mix in the format version so an algorithm change cannot collide with a
    // stale entry that happens to share geometry.
    const uint32_t ver = kFormatVersion;
    hash.addData(QByteArrayView(reinterpret_cast<const char*>(&ver), sizeof(ver)));

    for (const EditableSubMesh& sm : mesh.subMeshes()) {
        const uint32_t vCount = static_cast<uint32_t>(sm.vertices.size());
        const uint32_t tCount = static_cast<uint32_t>(sm.triangles.size());
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(&vCount), sizeof(vCount)));
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(&tCount), sizeof(tCount)));
        for (const EditableVertex& v : sm.vertices) {
            // Only geometry that can change cavity/curvature/AO. Colour and
            // bone weights are excluded on purpose (they cannot).
            const float buf[8] = {
                v.position.x, v.position.y, v.position.z,
                v.normal.x, v.normal.y, v.normal.z,
                v.uv.x, v.uv.y,
            };
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(buf), sizeof(buf)));
        }
        for (const EditableTriangle& t : sm.triangles) {
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(t.indices),
                                        sizeof(t.indices)));
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool has(const QString& meshHash, DerivedMapKind kind)
{
    const QString path = entryFile(meshHash, kind);
    return !path.isEmpty() && QFileInfo::exists(path);
}

bool load(const QString& meshHash, DerivedMapKind kind, DerivedMap& out, QString& error)
{
    const QString path = entryFile(meshHash, kind);
    if (path.isEmpty()) { error = QStringLiteral("invalid cache key"); return false; }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) { error = QStringLiteral("cache miss"); return false; }

    FileHeader hdr;
    if (f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr)) != qint64(sizeof(hdr))) {
        error = QStringLiteral("truncated header");
        return false;
    }
    // Any structural mismatch is a miss, not an error to surface: a stale entry
    // from an older format must simply be regenerated.
    if (hdr.magic != kMagic || hdr.version != kFormatVersion) {
        error = QStringLiteral("format mismatch");
        return false;
    }
    if (hdr.width <= 0 || hdr.height <= 0) { error = QStringLiteral("bad dimensions"); return false; }

    const size_t n = static_cast<size_t>(hdr.width) * static_cast<size_t>(hdr.height);
    if (hdr.valueBytes != n * sizeof(float) || hdr.coverageBytes != n) {
        error = QStringLiteral("payload size mismatch");
        return false;
    }

    DerivedMap m;
    m.width = hdr.width;
    m.height = hdr.height;
    m.values.resize(n);
    m.coverage.resize(n);
    if (f.read(reinterpret_cast<char*>(m.values.data()), hdr.valueBytes) != qint64(hdr.valueBytes)
     || f.read(reinterpret_cast<char*>(m.coverage.data()), hdr.coverageBytes) != qint64(hdr.coverageBytes)) {
        error = QStringLiteral("truncated payload");
        return false;
    }
    out = std::move(m);
    error.clear();
    return true;
}

bool save(const QString& meshHash, DerivedMapKind kind, const DerivedMap& in, QString& error)
{
    if (in.empty()) { error = QStringLiteral("refusing to cache an empty map"); return false; }
    const QString path = entryFile(meshHash, kind);
    if (path.isEmpty()) { error = QStringLiteral("invalid cache key"); return false; }

    const size_t n = static_cast<size_t>(in.width) * static_cast<size_t>(in.height);
    if (in.values.size() != n || in.coverage.size() != n) {
        error = QStringLiteral("map arrays do not match its dimensions");
        return false;
    }

    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        error = QStringLiteral("could not create cache directory");
        return false;
    }

    // Write to a temp file and rename, so an interrupted save can never leave a
    // half-written entry that a later load would treat as valid.
    const QString tmpPath = path + QStringLiteral(".tmp");
    {
        QFile f(tmpPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            error = QStringLiteral("could not open cache file for writing");
            return false;
        }
        FileHeader hdr;
        hdr.magic = kMagic;
        hdr.version = kFormatVersion;
        hdr.width = in.width;
        hdr.height = in.height;
        hdr.valueBytes = static_cast<uint32_t>(n * sizeof(float));
        hdr.coverageBytes = static_cast<uint32_t>(n);
        const auto bad = [&](const char* what) {
            error = QStringLiteral("short write (%1)").arg(QLatin1String(what));
            f.close();
            QFile::remove(tmpPath);
            return false;
        };
        if (f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr)) != qint64(sizeof(hdr)))
            return bad("header");
        if (f.write(reinterpret_cast<const char*>(in.values.data()), hdr.valueBytes)
                != qint64(hdr.valueBytes))
            return bad("values");
        if (f.write(reinterpret_cast<const char*>(in.coverage.data()), hdr.coverageBytes)
                != qint64(hdr.coverageBytes))
            return bad("coverage");
        f.close();
    }
    QFile::remove(path);            // rename() will not overwrite on all platforms
    if (!QFile::rename(tmpPath, path)) {
        QFile::remove(tmpPath);
        error = QStringLiteral("could not finalise cache file");
        return false;
    }
    error.clear();
    return true;
}

void invalidate(const QString& meshHash, DerivedMapKind kind)
{
    const QString path = entryFile(meshHash, kind);
    if (!path.isEmpty()) QFile::remove(path);
}

void invalidateAll(const QString& meshHash)
{
    const QString dir = entryDirectory(meshHash);
    if (!dir.isEmpty()) QDir(dir).removeRecursively();
}

} // namespace DerivedMapCache
