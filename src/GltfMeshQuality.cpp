#include "GltfMeshQuality.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>

namespace GltfMeshQuality {

namespace {

constexpr int kByte = 5120;
constexpr int kUnsignedByte = 5121;
constexpr int kShort = 5122;
constexpr int kUnsignedShort = 5123;
constexpr int kUnsignedInt = 5125;
constexpr int kFloat = 5126;

constexpr quint32 kGlbMagic = 0x46546C67;       // 'glTF'
constexpr quint32 kChunkJson = 0x4E4F534A;      // 'JSON'
constexpr quint32 kChunkBin = 0x004E4942;       // 'BIN\0'

// Primitive extensions that do not reference vertex data, so the primitive
// can still be measured and rewritten.
const QSet<QString>& harmlessPrimitiveExtensions()
{
    static const QSet<QString> s{QStringLiteral("KHR_materials_variants")};
    return s;
}

int componentSize(int type)
{
    switch (type) {
    case kByte:
    case kUnsignedByte: return 1;
    case kShort:
    case kUnsignedShort: return 2;
    case kUnsignedInt:
    case kFloat: return 4;
    default: return 0;
    }
}

int componentCount(const QString& type)
{
    if (type == QLatin1String("SCALAR")) return 1;
    if (type == QLatin1String("VEC2")) return 2;
    if (type == QLatin1String("VEC3")) return 3;
    if (type == QLatin1String("VEC4")) return 4;
    if (type == QLatin1String("MAT2")) return 4;
    if (type == QLatin1String("MAT3")) return 9;
    if (type == QLatin1String("MAT4")) return 16;
    return 0;
}

struct Doc {
    QString path;
    bool glb = false;
    QJsonObject json;
    QList<QByteArray> buffers;
    // Per buffer: absolute path of an external .bin, or empty (GLB chunk / data URI).
    QStringList externalPaths;
    qint64 bytesOnDisk = 0;
};

bool parseGlb(const QByteArray& bytes, QJsonObject& json, QByteArray& bin, QString& err)
{
    if (bytes.size() < 12) {
        err = QStringLiteral("GLB is too small");
        return false;
    }
    const auto* p = reinterpret_cast<const uchar*>(bytes.constData());
    if (qFromLittleEndian<quint32>(p) != kGlbMagic) {
        err = QStringLiteral("not a GLB (bad magic)");
        return false;
    }
    const qint64 total = qFromLittleEndian<quint32>(p + 8);
    if (total > bytes.size()) {
        err = QStringLiteral("GLB length exceeds the file");
        return false;
    }
    qint64 offset = 12;
    QByteArray jsonChunk;
    while (offset + 8 <= total) {
        const qint64 len = qFromLittleEndian<quint32>(p + offset);
        const quint32 type = qFromLittleEndian<quint32>(p + offset + 4);
        offset += 8;
        if (offset + len > total) {
            err = QStringLiteral("GLB chunk overruns the file");
            return false;
        }
        if (type == kChunkJson)
            jsonChunk = bytes.mid(offset, len);
        else if (type == kChunkBin && bin.isEmpty())
            bin = bytes.mid(offset, len);
        offset += len;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(jsonChunk, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        err = QStringLiteral("GLB JSON chunk does not parse: %1").arg(perr.errorString());
        return false;
    }
    json = doc.object();
    return true;
}

bool loadDoc(const QString& path, Doc& d, QString& err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        err = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    const QByteArray bytes = f.readAll();
    d.path = path;
    d.bytesOnDisk = bytes.size();
    QByteArray glbBin;
    if (bytes.startsWith("glTF")) {
        d.glb = true;
        if (!parseGlb(bytes, d.json, glbBin, err))
            return false;
    } else {
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            err = QStringLiteral("glTF JSON does not parse: %1").arg(perr.errorString());
            return false;
        }
        d.json = doc.object();
    }

    const QString dir = QFileInfo(path).absolutePath();
    const QJsonArray buffers = d.json.value(QStringLiteral("buffers")).toArray();
    for (int i = 0; i < buffers.size(); ++i) {
        const QString uri = buffers.at(i).toObject().value(QStringLiteral("uri")).toString();
        if (uri.isEmpty()) {
            if (!(d.glb && i == 0)) {
                err = QStringLiteral("buffer %1 has no data").arg(i);
                return false;
            }
            d.buffers.append(glbBin);
            d.externalPaths.append(QString());
        } else if (uri.startsWith(QLatin1String("data:"))) {
            const int comma = uri.indexOf(QLatin1Char(','));
            if (comma < 0) {
                err = QStringLiteral("buffer %1 has a malformed data URI").arg(i);
                return false;
            }
            d.buffers.append(QByteArray::fromBase64(uri.mid(comma + 1).toLatin1()));
            d.externalPaths.append(QString());
        } else {
            const QString binPath = QDir::cleanPath(
                QDir(dir).filePath(QUrl::fromPercentEncoding(uri.toUtf8())));
            QFile bf(binPath);
            if (!bf.open(QIODevice::ReadOnly)) {
                err = QStringLiteral("cannot open external buffer %1").arg(binPath);
                return false;
            }
            d.buffers.append(bf.readAll());
            d.externalPaths.append(binPath);
            d.bytesOnDisk += d.buffers.last().size();
        }
    }
    return true;
}

// Where one accessor's elements live.
struct Acc {
    int index = -1;
    int count = 0;
    int type = 0;        // componentType
    int comps = 0;
    int elem = 0;        // bytes per element
    int view = -1;
    int buffer = -1;
    qint64 base = 0;     // byte offset of element 0 in the buffer
    qint64 stride = 0;
    bool sparse = false;
};

bool accessor(const Doc& d, int index, Acc& a, QString& err)
{
    const QJsonArray accessors = d.json.value(QStringLiteral("accessors")).toArray();
    const QJsonArray views = d.json.value(QStringLiteral("bufferViews")).toArray();
    if (index < 0 || index >= accessors.size()) {
        err = QStringLiteral("accessor %1 does not exist").arg(index);
        return false;
    }
    const QJsonObject o = accessors.at(index).toObject();
    a.index = index;
    a.count = o.value(QStringLiteral("count")).toInt();
    a.type = o.value(QStringLiteral("componentType")).toInt();
    a.comps = componentCount(o.value(QStringLiteral("type")).toString());
    a.elem = componentSize(a.type) * a.comps;
    a.sparse = o.contains(QStringLiteral("sparse"));
    if (a.count <= 0 || a.elem <= 0) {
        err = QStringLiteral("accessor %1 is empty or has an unknown type").arg(index);
        return false;
    }
    if (!o.contains(QStringLiteral("bufferView"))) {
        err = QStringLiteral("accessor %1 has no bufferView").arg(index);
        return false;
    }
    a.view = o.value(QStringLiteral("bufferView")).toInt(-1);
    if (a.view < 0 || a.view >= views.size()) {
        err = QStringLiteral("accessor %1 points at a missing bufferView").arg(index);
        return false;
    }
    const QJsonObject v = views.at(a.view).toObject();
    if (v.contains(QStringLiteral("extensions"))) {
        err = QStringLiteral("bufferView %1 is compressed (extension)").arg(a.view);
        return false;
    }
    a.buffer = v.value(QStringLiteral("buffer")).toInt(-1);
    if (a.buffer < 0 || a.buffer >= d.buffers.size()) {
        err = QStringLiteral("bufferView %1 points at a missing buffer").arg(a.view);
        return false;
    }
    a.base = qint64(v.value(QStringLiteral("byteOffset")).toInt(0))
           + o.value(QStringLiteral("byteOffset")).toInt(0);
    a.stride = v.value(QStringLiteral("byteStride")).toInt(0);
    if (a.stride == 0)
        a.stride = a.elem;
    const qint64 end = a.base + qint64(a.count - 1) * a.stride + a.elem;
    if (a.base < 0 || a.stride < a.elem || end > d.buffers.at(a.buffer).size()) {
        err = QStringLiteral("accessor %1 reads past the end of its buffer").arg(index);
        return false;
    }
    return true;
}

const char* element(const Doc& d, const Acc& a, int i)
{
    return d.buffers.at(a.buffer).constData() + a.base + qint64(i) * a.stride;
}

char* element(Doc& d, const Acc& a, int i)
{
    return d.buffers[a.buffer].data() + a.base + qint64(i) * a.stride;
}

double componentValue(const char* p, int type)
{
    switch (type) {
    case kByte: { qint8 v; std::memcpy(&v, p, 1); return v; }
    case kUnsignedByte: { quint8 v; std::memcpy(&v, p, 1); return v; }
    case kShort: { qint16 v; std::memcpy(&v, p, 2); return v; }
    case kUnsignedShort: { quint16 v; std::memcpy(&v, p, 2); return v; }
    case kUnsignedInt: { quint32 v; std::memcpy(&v, p, 4); return v; }
    case kFloat: { float v; std::memcpy(&v, p, 4); return v; }
    default: return 0.0;
    }
}

bool readIndices(const Doc& d, const Acc& a, std::vector<uint32_t>& out)
{
    if (a.comps != 1 || (a.type != kUnsignedByte && a.type != kUnsignedShort && a.type != kUnsignedInt))
        return false;
    out.resize(size_t(a.count));
    for (int i = 0; i < a.count; ++i)
        out[size_t(i)] = uint32_t(componentValue(element(d, a, i), a.type));
    return true;
}

void writeIndices(Doc& d, const Acc& a, const std::vector<uint32_t>& idx)
{
    for (size_t i = 0; i < idx.size(); ++i) {
        char* p = element(d, a, int(i));
        if (a.type == kUnsignedByte) {
            const quint8 v = quint8(idx[i]);
            std::memcpy(p, &v, 1);
        } else if (a.type == kUnsignedShort) {
            const quint16 v = quint16(idx[i]);
            std::memcpy(p, &v, 2);
        } else {
            const quint32 v = idx[i];
            std::memcpy(p, &v, 4);
        }
    }
}

// Tightly packed copy of an accessor's elements.
std::vector<unsigned char> packed(const Doc& d, const Acc& a)
{
    std::vector<unsigned char> out(size_t(a.count) * size_t(a.elem));
    for (int i = 0; i < a.count; ++i)
        std::memcpy(out.data() + size_t(i) * a.elem, element(d, a, i), size_t(a.elem));
    return out;
}

// Set an accessor's count and, when it carried min/max, recompute them from
// the (new) data — a stale POSITION min/max is a validation error.
void updateAccessorJson(Doc& d, const Acc& a, int newCount)
{
    QJsonArray accessors = d.json.value(QStringLiteral("accessors")).toArray();
    QJsonObject o = accessors.at(a.index).toObject();
    o.insert(QStringLiteral("count"), newCount);
    if (o.contains(QStringLiteral("min")) || o.contains(QStringLiteral("max"))) {
        std::vector<double> lo(size_t(a.comps), std::numeric_limits<double>::infinity());
        std::vector<double> hi(size_t(a.comps), -std::numeric_limits<double>::infinity());
        const int cs = componentSize(a.type);
        for (int i = 0; i < newCount; ++i) {
            const char* p = element(d, a, i);
            for (int c = 0; c < a.comps; ++c) {
                const double v = componentValue(p + c * cs, a.type);
                lo[size_t(c)] = std::min(lo[size_t(c)], v);
                hi[size_t(c)] = std::max(hi[size_t(c)], v);
            }
        }
        QJsonArray mn, mx;
        for (int c = 0; c < a.comps; ++c) {
            if (a.type == kFloat) {
                mn.append(double(float(lo[size_t(c)])));
                mx.append(double(float(hi[size_t(c)])));
            } else {
                mn.append(qint64(lo[size_t(c)]));
                mx.append(qint64(hi[size_t(c)]));
            }
        }
        o.insert(QStringLiteral("min"), mn);
        o.insert(QStringLiteral("max"), mx);
    }
    accessors.replace(a.index, o);
    d.json.insert(QStringLiteral("accessors"), accessors);
}

// How many places reference each accessor. An accessor used more than once
// cannot be rewritten for one primitive without breaking the other user.
QHash<int, int> accessorReferences(const QJsonObject& json)
{
    QHash<int, int> refs;
    auto add = [&refs](const QJsonValue& v) {
        if (v.isDouble())
            refs[v.toInt()] += 1;
    };
    auto addAll = [&add](const QJsonObject& o) {
        for (auto it = o.begin(); it != o.end(); ++it)
            add(it.value());
    };
    for (const QJsonValue& mv : json.value(QStringLiteral("meshes")).toArray()) {
        for (const QJsonValue& pv : mv.toObject().value(QStringLiteral("primitives")).toArray()) {
            const QJsonObject prim = pv.toObject();
            add(prim.value(QStringLiteral("indices")));
            addAll(prim.value(QStringLiteral("attributes")).toObject());
            for (const QJsonValue& t : prim.value(QStringLiteral("targets")).toArray())
                addAll(t.toObject());
        }
    }
    for (const QJsonValue& s : json.value(QStringLiteral("skins")).toArray())
        add(s.toObject().value(QStringLiteral("inverseBindMatrices")));
    for (const QJsonValue& an : json.value(QStringLiteral("animations")).toArray()) {
        for (const QJsonValue& s : an.toObject().value(QStringLiteral("samplers")).toArray()) {
            add(s.toObject().value(QStringLiteral("input")));
            add(s.toObject().value(QStringLiteral("output")));
        }
    }
    for (const QJsonValue& n : json.value(QStringLiteral("nodes")).toArray()) {
        const QJsonObject inst = n.toObject().value(QStringLiteral("extensions")).toObject()
                                     .value(QStringLiteral("EXT_mesh_gpu_instancing")).toObject();
        addAll(inst.value(QStringLiteral("attributes")).toObject());
    }
    return refs;
}

// Everything the scan needs about one primitive.
struct Prim {
    PrimitiveReport report;
    Acc indices;
    QList<Acc> vertexAccessors;   // attributes, then morph-target attributes
    MeshQuality::Geometry geometry;
};

// Read one primitive. Returns false (with report.note set) when it cannot
// be measured.
bool readPrimitive(const Doc& d, const QHash<int, int>& refs, int m, int p, Prim& out)
{
    const QJsonObject mesh = d.json.value(QStringLiteral("meshes")).toArray().at(m).toObject();
    const QJsonObject prim = mesh.value(QStringLiteral("primitives")).toArray().at(p).toObject();
    PrimitiveReport& r = out.report;
    r.mesh = m;
    r.primitive = p;
    r.meshName = mesh.value(QStringLiteral("name")).toString();

    if (prim.value(QStringLiteral("mode")).toInt(4) != 4) {
        r.note = QStringLiteral("not a triangle list");
        return false;
    }
    // FB_ngon_encoding (Assimp writes it, so every glTF this editor exports
    // has it) stores n-gons as consecutive fans sharing their first index:
    // the buffers can be measured, but reordering, welding or dropping
    // triangles would merge or split polygons.
    bool ngonEncoded = false;
    for (const QString& ext : prim.value(QStringLiteral("extensions")).toObject().keys()) {
        if (ext == QLatin1String("FB_ngon_encoding")) {
            ngonEncoded = true;
        } else if (!harmlessPrimitiveExtensions().contains(ext)) {
            r.note = QStringLiteral("uses %1").arg(ext);
            return false;
        }
    }
    if (!prim.contains(QStringLiteral("indices"))) {
        r.note = QStringLiteral("not indexed");
        return false;
    }
    QString err;
    if (!accessor(d, prim.value(QStringLiteral("indices")).toInt(-1), out.indices, err)) {
        r.note = err;
        return false;
    }
    if (out.indices.sparse || !readIndices(d, out.indices, out.geometry.indices)
        || out.geometry.indices.size() % 3 != 0) {
        r.note = QStringLiteral("unsupported index accessor");
        return false;
    }

    const QJsonObject attrs = prim.value(QStringLiteral("attributes")).toObject();
    Acc pos;
    if (!attrs.contains(QStringLiteral("POSITION"))
        || !accessor(d, attrs.value(QStringLiteral("POSITION")).toInt(-1), pos, err)
        || pos.type != kFloat || pos.comps != 3 || pos.sparse) {
        r.note = err.isEmpty() ? QStringLiteral("no plain float POSITION") : err;
        return false;
    }
    const int vertexCount = pos.count;

    bool vertexFixable = true;
    QStringList blockers;
    auto addVertexAccessor = [&](int index, const QString& label) -> bool {
        Acc a;
        if (!accessor(d, index, a, err)) {
            r.note = QStringLiteral("%1: %2").arg(label, err);
            return false;
        }
        if (a.count != vertexCount) {
            r.note = QStringLiteral("%1 has %2 elements, POSITION has %3").arg(label).arg(a.count).arg(vertexCount);
            return false;
        }
        if (a.sparse) {
            vertexFixable = false;
            blockers << QStringLiteral("%1 is sparse").arg(label);
        } else if (refs.value(index) > 1) {
            vertexFixable = false;
            blockers << QStringLiteral("%1 is shared with another primitive").arg(label);
        }
        out.vertexAccessors.append(a);
        return true;
    };
    for (auto it = attrs.begin(); it != attrs.end(); ++it)
        if (!addVertexAccessor(it.value().toInt(-1), it.key()))
            return false;
    const QJsonArray targets = prim.value(QStringLiteral("targets")).toArray();
    for (int t = 0; t < targets.size(); ++t) {
        const QJsonObject to = targets.at(t).toObject();
        for (auto it = to.begin(); it != to.end(); ++it)
            if (!addVertexAccessor(it.value().toInt(-1),
                                   QStringLiteral("morph target %1 %2").arg(t).arg(it.key())))
                return false;
    }

    MeshQuality::Geometry& g = out.geometry;
    g.vertexCount = size_t(vertexCount);
    g.positions.resize(size_t(vertexCount) * 3);
    for (int i = 0; i < vertexCount; ++i)
        std::memcpy(&g.positions[size_t(i) * 3], element(d, pos, i), sizeof(float) * 3);

    // Two vertices may merge only when EVERY stream agrees, morph targets
    // and skin weights included.
    size_t keyStride = 0;
    for (const Acc& a : out.vertexAccessors)
        keyStride += size_t(a.elem);
    // The vertex-fetch metric counts what a draw reads: the base attributes,
    // not the morph targets (they come first in vertexAccessors).
    size_t vertexBytes = 0;
    for (int i = 0; i < attrs.size(); ++i)
        vertexBytes += size_t(out.vertexAccessors.at(i).elem);
    g.vertexBytes = vertexBytes;
    g.keyStride = keyStride;
    g.keys.resize(size_t(vertexCount) * keyStride);
    size_t col = 0;
    for (const Acc& a : out.vertexAccessors) {
        for (int i = 0; i < vertexCount; ++i)
            std::memcpy(g.keys.data() + size_t(i) * keyStride + col, element(d, a, i), size_t(a.elem));
        col += size_t(a.elem);
    }

    r.measured = true;
    r.indexFixable = refs.value(out.indices.index) <= 1 && !ngonEncoded;
    r.vertexFixable = vertexFixable && r.indexFixable;
    if (ngonEncoded)
        blockers.prepend(QStringLiteral("its triangle order encodes n-gons (FB_ngon_encoding)"));
    else if (!r.indexFixable)
        blockers.prepend(QStringLiteral("its index accessor is shared"));
    if (!blockers.isEmpty())
        r.note = blockers.join(QStringLiteral("; "));
    return true;
}

// Shrink bufferViews to the bytes their accessors still use and repack each
// buffer. Views referenced from anywhere but an accessor's own `bufferView`
// (images, sparse data, extensions) keep their full length.
void compactBuffers(Doc& d)
{
    QJsonArray views = d.json.value(QStringLiteral("bufferViews")).toArray();
    const int nViews = views.size();
    std::vector<qint64> used(size_t(nViews), 0);
    std::vector<char> keepFull(size_t(nViews), 0);

    std::function<void(const QJsonValue&)> markRefs = [&](const QJsonValue& v) {
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                if (it.key() == QLatin1String("bufferView") && it.value().isDouble()) {
                    const int bv = it.value().toInt(-1);
                    if (bv >= 0 && bv < nViews)
                        keepFull[size_t(bv)] = 1;
                }
                markRefs(it.value());
            }
        } else if (v.isArray()) {
            for (const QJsonValue& e : v.toArray())
                markRefs(e);
        }
    };
    for (auto it = d.json.begin(); it != d.json.end(); ++it)
        if (it.key() != QLatin1String("accessors"))
            markRefs(it.value());

    const QJsonArray accessors = d.json.value(QStringLiteral("accessors")).toArray();
    for (int i = 0; i < accessors.size(); ++i) {
        const QJsonObject o = accessors.at(i).toObject();
        if (o.contains(QStringLiteral("sparse")))
            markRefs(o.value(QStringLiteral("sparse")));
        Acc a;
        QString err;
        if (!o.contains(QStringLiteral("bufferView")) || !accessor(d, i, a, err))
            continue;
        const qint64 viewStart = views.at(a.view).toObject().value(QStringLiteral("byteOffset")).toInt(0);
        const qint64 end = a.base - viewStart + qint64(a.count - 1) * a.stride + a.elem;
        used[size_t(a.view)] = std::max(used[size_t(a.view)], end);
    }

    bool shrank = false;
    std::vector<qint64> newLength(static_cast<size_t>(nViews));
    for (int v = 0; v < nViews; ++v) {
        const qint64 len = views.at(v).toObject().value(QStringLiteral("byteLength")).toInt(0);
        newLength[size_t(v)] = len;
        if (!keepFull[size_t(v)] && used[size_t(v)] > 0 && used[size_t(v)] < len) {
            newLength[size_t(v)] = used[size_t(v)];
            shrank = true;
        }
    }
    if (!shrank)
        return;

    QJsonArray buffers = d.json.value(QStringLiteral("buffers")).toArray();
    for (int b = 0; b < d.buffers.size(); ++b) {
        std::vector<int> mine;
        for (int v = 0; v < nViews; ++v)
            if (views.at(v).toObject().value(QStringLiteral("buffer")).toInt(-1) == b)
                mine.push_back(v);
        std::sort(mine.begin(), mine.end(), [&views](int x, int y) {
            return views.at(x).toObject().value(QStringLiteral("byteOffset")).toInt(0)
                 < views.at(y).toObject().value(QStringLiteral("byteOffset")).toInt(0);
        });
        // Overlapping views (legal, rare) cannot be repacked independently.
        bool overlap = false;
        for (size_t i = 1; i < mine.size() && !overlap; ++i) {
            const QJsonObject prev = views.at(mine[i - 1]).toObject();
            const qint64 prevEnd = prev.value(QStringLiteral("byteOffset")).toInt(0)
                                 + prev.value(QStringLiteral("byteLength")).toInt(0);
            overlap = views.at(mine[i]).toObject().value(QStringLiteral("byteOffset")).toInt(0) < prevEnd;
        }
        if (overlap || mine.empty())
            continue;
        QByteArray repacked;
        for (int v : mine) {
            QJsonObject vo = views.at(v).toObject();
            const qint64 off = vo.value(QStringLiteral("byteOffset")).toInt(0);
            while (repacked.size() % 4 != 0)
                repacked.append('\0');
            const qint64 newOff = repacked.size();
            repacked.append(d.buffers.at(b).constData() + off, newLength[size_t(v)]);
            vo.insert(QStringLiteral("byteOffset"), newOff);
            vo.insert(QStringLiteral("byteLength"), newLength[size_t(v)]);
            views.replace(v, vo);
        }
        d.buffers[b] = repacked;
        QJsonObject bo = buffers.at(b).toObject();
        bo.insert(QStringLiteral("byteLength"), repacked.size());
        buffers.replace(b, bo);
    }
    d.json.insert(QStringLiteral("bufferViews"), views);
    d.json.insert(QStringLiteral("buffers"), buffers);
}

void appendU32(QByteArray& b, quint32 v)
{
    char tmp[4];
    qToLittleEndian<quint32>(v, tmp);
    b.append(tmp, 4);
}

QByteArray buildGlb(const QJsonObject& json, const QByteArray& bin)
{
    QByteArray jsonChunk = QJsonDocument(json).toJson(QJsonDocument::Compact);
    while (jsonChunk.size() % 4 != 0)
        jsonChunk.append(' ');
    QByteArray binChunk = bin;
    while (binChunk.size() % 4 != 0)
        binChunk.append('\0');
    QByteArray out;
    const bool hasBin = !bin.isEmpty();
    const quint32 total = 12 + 8 + quint32(jsonChunk.size()) + (hasBin ? 8 + quint32(binChunk.size()) : 0);
    appendU32(out, kGlbMagic);
    appendU32(out, 2);
    appendU32(out, total);
    appendU32(out, quint32(jsonChunk.size()));
    appendU32(out, kChunkJson);
    out.append(jsonChunk);
    if (hasBin) {
        appendU32(out, quint32(binChunk.size()));
        appendU32(out, kChunkBin);
        out.append(binChunk);
    }
    return out;
}

// Serialise the document: the main file plus every external buffer.
QList<QPair<QString, QByteArray>> serialise(Doc& d)
{
    QList<QPair<QString, QByteArray>> files;
    QJsonArray buffers = d.json.value(QStringLiteral("buffers")).toArray();
    for (int b = 0; b < d.buffers.size(); ++b) {
        QJsonObject bo = buffers.at(b).toObject();
        if (!d.externalPaths.at(b).isEmpty()) {
            files.append({d.externalPaths.at(b), d.buffers.at(b)});
        } else if (!(d.glb && b == 0)) {
            bo.insert(QStringLiteral("uri"), QStringLiteral("data:application/octet-stream;base64,")
                                                 + QString::fromLatin1(d.buffers.at(b).toBase64()));
        }
        bo.insert(QStringLiteral("byteLength"), d.buffers.at(b).size());
        buffers.replace(b, bo);
    }
    if (!buffers.isEmpty())
        d.json.insert(QStringLiteral("buffers"), buffers);
    const QByteArray main = d.glb
        ? buildGlb(d.json, d.buffers.isEmpty() ? QByteArray() : d.buffers.at(0))
        : QJsonDocument(d.json).toJson(QJsonDocument::Indented);
    files.prepend({d.path, main});
    return files;
}

bool writeAll(const QList<QPair<QString, QByteArray>>& files, QString& err)
{
    // Commit only after every file was written: QSaveFile renames on commit.
    for (const auto& [path, bytes] : files) {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly)) {
            err = QStringLiteral("cannot write %1").arg(path);
            return false;
        }
        if (f.write(bytes) != bytes.size() || !f.commit()) {
            err = QStringLiteral("short write to %1").arg(path);
            return false;
        }
    }
    return true;
}

QList<Prim> readPrimitives(const Doc& d)
{
    QList<Prim> prims;
    const QHash<int, int> refs = accessorReferences(d.json);
    const QJsonArray meshes = d.json.value(QStringLiteral("meshes")).toArray();
    for (int m = 0; m < meshes.size(); ++m) {
        const int n = meshes.at(m).toObject().value(QStringLiteral("primitives")).toArray().size();
        for (int p = 0; p < n; ++p) {
            Prim prim;
            readPrimitive(d, refs, m, p, prim);
            prims.append(prim);
        }
    }
    return prims;
}

} // namespace

bool handles(const QString& path)
{
    const QString s = QFileInfo(path).suffix().toLower();
    return s == QLatin1String("gltf") || s == QLatin1String("glb");
}

AnalyzeResult analyzeFile(const QString& path, const MeshQuality::Limits& limits)
{
    AnalyzeResult res;
    Doc d;
    if (!loadDoc(path, d, res.error))
        return res;
    for (Prim& prim : readPrimitives(d)) {
        if (prim.report.measured)
            prim.report.metrics = MeshQuality::analyze(prim.geometry, limits);
        res.primitives.append(prim.report);
    }
    res.ok = true;
    return res;
}

FixReport fixFile(const QString& path, const MeshQuality::FixOptions& opt, bool dryRun,
                  const MeshQuality::Limits& limits)
{
    FixReport rep;
    Doc d;
    if (!loadDoc(path, d, rep.error))
        return rep;
    rep.bytesBefore = d.bytesOnDisk;
    rep.bytesAfter = d.bytesOnDisk;

    for (Prim& prim : readPrimitives(d)) {
        const PrimitiveReport& pr = prim.report;
        const QString label = QStringLiteral("mesh %1%2 primitive %3")
                                  .arg(pr.mesh)
                                  .arg(pr.meshName.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(pr.meshName))
                                  .arg(pr.primitive);
        if (!pr.measured || !pr.indexFixable) {
            rep.notes << QStringLiteral("%1 left as is: %2").arg(label, pr.note);
            continue;
        }
        MeshQuality::FixOptions o = opt;
        if (!pr.vertexFixable && o.remapsVertices()) {
            o.weld = false;
            o.vertexFetch = false;
            rep.notes << QStringLiteral("%1: vertex order and welding skipped (%2)").arg(label, pr.note);
        }
        const MeshQuality::FixResult r = MeshQuality::fix(prim.geometry, o, limits);
        if (!r.changed)
            continue;
        if (r.indices.empty()) {
            rep.notes << QStringLiteral("%1 left as is: every triangle is degenerate").arg(label);
            continue;
        }
        writeIndices(d, prim.indices, r.indices);
        updateAccessorJson(d, prim.indices, int(r.indices.size()));
        if (!r.remap.empty()) {
            for (const Acc& a : prim.vertexAccessors) {
                const std::vector<unsigned char> old = packed(d, a);
                const std::vector<unsigned char> moved = MeshQuality::remapStream(
                    old.data(), size_t(a.count), size_t(a.elem), r.remap, r.newVertexCount);
                for (size_t i = 0; i < r.newVertexCount; ++i)
                    std::memcpy(element(d, a, int(i)), moved.data() + i * size_t(a.elem), size_t(a.elem));
                updateAccessorJson(d, a, int(r.newVertexCount));
            }
        }
        ++rep.primitivesChanged;
        rep.degeneratesRemoved += r.degeneratesRemoved;
        rep.verticesWelded += r.verticesWelded;
        rep.verticesDropped += r.verticesDropped;
        rep.trianglesBefore += r.trianglesBefore;
        rep.trianglesAfter += r.trianglesAfter;
        if (r.simplified)
            ++rep.primitivesSimplified;
    }

    if (rep.primitivesChanged == 0) {
        rep.ok = true;
        return rep;
    }
    compactBuffers(d);
    const auto files = serialise(d);
    rep.bytesAfter = 0;
    for (const auto& f : files)
        rep.bytesAfter += f.second.size();
    if (!dryRun && !writeAll(files, rep.error))
        return rep;
    rep.ok = true;
    return rep;
}

} // namespace GltfMeshQuality
