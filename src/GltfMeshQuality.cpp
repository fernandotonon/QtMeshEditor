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
#include <QUuid>
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
    // GLB only: the buffer stored in the BIN chunk (buffer 0 when it has no
    // uri, else none), the BIN chunk bytes as read, and whether the file
    // carries chunks this writer cannot reproduce.
    int binBuffer = -1;
    QByteArray glbBin;
    bool glbExtraChunks = false;
    // EXT_meshopt_compression fallback buffers: no uri and no bytes (a
    // loader that decodes the extension never reads them). Loaded empty and
    // written back untouched.
    QSet<int> placeholderBuffers;
};

bool isMeshoptFallback(const QJsonObject& buffer)
{
    return buffer.value(QStringLiteral("extensions")).toObject()
               .value(QStringLiteral("EXT_meshopt_compression")).toObject()
               .value(QStringLiteral("fallback")).toBool();
}

bool parseGlb(const QByteArray& bytes, QJsonObject& json, QByteArray& bin, bool& extraChunks,
              QString& err)
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
    bool haveJson = false;
    bool haveBin = false;
    while (offset + 8 <= total) {
        const qint64 len = qFromLittleEndian<quint32>(p + offset);
        const quint32 type = qFromLittleEndian<quint32>(p + offset + 4);
        offset += 8;
        if (offset + len > total) {
            err = QStringLiteral("GLB chunk overruns the file");
            return false;
        }
        if (type == kChunkJson && !haveJson) {
            jsonChunk = bytes.mid(offset, len);
            haveJson = true;
        } else if (type == kChunkBin && !haveBin) {
            bin = bytes.mid(offset, len);
            haveBin = true;
        } else {
            extraChunks = true;   // a rewrite would drop it
        }
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
        if (!parseGlb(bytes, d.json, glbBin, d.glbExtraChunks, err))
            return false;
        d.glbBin = glbBin;
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
        if (uri.isEmpty() && isMeshoptFallback(buffers.at(i).toObject())) {
            // Views into it read past the (empty) bytes, so the primitives
            // using them are reported as unmeasurable, never rewritten.
            d.buffers.append(QByteArray());
            d.externalPaths.append(QString());
            d.placeholderBuffers.insert(i);
        } else if (uri.isEmpty()) {
            if (!(d.glb && i == 0)) {
                err = QStringLiteral("buffer %1 has no data").arg(i);
                return false;
            }
            d.buffers.append(glbBin);
            d.externalPaths.append(QString());
            d.binBuffer = 0;
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
    // Accessors a primitive extension names (CESIUM_primitive_outline's
    // `indices`, an `attributes` map, ...). Unknown extensions are read
    // conservatively: an extra count only blocks a rewrite. Draco's
    // `attributes` are Draco ids, not accessors, and the variants
    // extension names materials.
    std::function<void(const QJsonValue&)> addExtensionRefs = [&](const QJsonValue& v) {
        if (v.isArray()) {
            for (const QJsonValue& e : v.toArray())
                addExtensionRefs(e);
            return;
        }
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            const QString& key = it.key();
            if (key == QLatin1String("attributes") && it.value().isObject())
                addAll(it.value().toObject());
            else if (key == QLatin1String("indices") || key == QLatin1String("accessor"))
                add(it.value());
            else if (it.value().isObject() || it.value().isArray())
                addExtensionRefs(it.value());
        }
    };
    for (const QJsonValue& mv : json.value(QStringLiteral("meshes")).toArray()) {
        for (const QJsonValue& pv : mv.toObject().value(QStringLiteral("primitives")).toArray()) {
            const QJsonObject prim = pv.toObject();
            add(prim.value(QStringLiteral("indices")));
            addAll(prim.value(QStringLiteral("attributes")).toObject());
            for (const QJsonValue& t : prim.value(QStringLiteral("targets")).toArray())
                addAll(t.toObject());
            const QJsonObject exts = prim.value(QStringLiteral("extensions")).toObject();
            for (auto it = exts.begin(); it != exts.end(); ++it)
                if (it.key() != QLatin1String("KHR_draco_mesh_compression")
                    && it.key() != QLatin1String("KHR_materials_variants"))
                    addExtensionRefs(it.value());
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

// Bytes one accessor or bufferView occupies in its buffer.
struct Range {
    int buffer = -1;
    int view = -1;
    qint64 start = 0;
    qint64 end = 0;
};

bool overlaps(const Range& a, const Range& b)
{
    return a.buffer >= 0 && a.buffer == b.buffer && a.start < b.end && b.start < a.end;
}

// Views referenced from anywhere but an accessor's own `bufferView`:
// images, sparse indices/values, extensions. Their bytes have readers the
// accessor bookkeeping does not see.
std::vector<char> viewsReferencedOutsideAccessors(const QJsonObject& json, int nViews)
{
    std::vector<char> marked(size_t(std::max(0, nViews)), 0);
    std::function<void(const QJsonValue&)> mark = [&](const QJsonValue& v) {
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                if (it.key() == QLatin1String("bufferView") && it.value().isDouble()) {
                    const int bv = it.value().toInt(-1);
                    if (bv >= 0 && bv < nViews)
                        marked[size_t(bv)] = 1;
                }
                mark(it.value());
            }
        } else if (v.isArray()) {
            for (const QJsonValue& e : v.toArray())
                mark(e);
        }
    };
    for (auto it = json.begin(); it != json.end(); ++it)
        if (it.key() != QLatin1String("accessors"))
            mark(it.value());
    for (const QJsonValue& a : json.value(QStringLiteral("accessors")).toArray())
        mark(a.toObject().value(QStringLiteral("sparse")));
    return marked;
}

// Where every accessor and bufferView lives, so a rewrite can prove that
// the bytes it changes belong to nobody else. Accessor-ID reference counts
// are not enough: two accessors (or two views) may address the same bytes.
struct Storage {
    struct Entry {
        Range range;
        int accessor = -1;   // -1: the entry is a bufferView's range
    };
    struct BufferRanges {
        std::vector<Entry> entries;        // sorted by start
        std::vector<qint64> maxEndSoFar;   // max end over entries[0..i]
    };
    QHash<int, Range> accessors;
    QHash<int, BufferRanges> byBuffer;
    std::vector<char> outsideRefs;
};

Storage storageMap(const Doc& d)
{
    Storage st;
    const QJsonArray views = d.json.value(QStringLiteral("bufferViews")).toArray();
    for (int v = 0; v < views.size(); ++v) {
        const QJsonObject o = views.at(v).toObject();
        Range r;
        r.buffer = o.value(QStringLiteral("buffer")).toInt(-1);
        r.view = v;
        r.start = o.value(QStringLiteral("byteOffset")).toInt(0);
        r.end = r.start + o.value(QStringLiteral("byteLength")).toInt(0);
        st.byBuffer[r.buffer].entries.push_back({r, -1});
    }
    const int nAcc = d.json.value(QStringLiteral("accessors")).toArray().size();
    for (int i = 0; i < nAcc; ++i) {
        Acc a;
        QString err;
        if (!accessor(d, i, a, err))
            continue;
        const Range r{a.buffer, a.view, a.base, a.base + qint64(a.count - 1) * a.stride + a.elem};
        st.accessors.insert(i, r);
        st.byBuffer[r.buffer].entries.push_back({r, i});
    }
    for (auto it = st.byBuffer.begin(); it != st.byBuffer.end(); ++it) {
        auto& e = it.value().entries;
        std::sort(e.begin(), e.end(),
                  [](const Storage::Entry& x, const Storage::Entry& y) { return x.range.start < y.range.start; });
        auto& maxEnd = it.value().maxEndSoFar;
        maxEnd.resize(e.size());
        qint64 m = std::numeric_limits<qint64>::min();
        for (size_t i = 0; i < e.size(); ++i)
            maxEnd[i] = m = std::max(m, e[i].range.end);
    }
    st.outsideRefs = viewsReferencedOutsideAccessors(d.json, int(views.size()));
    return st;
}

// May `a` be rewritten when the accessors in `own` are rewritten with it?
bool privateStorage(const Storage& st, const QSet<int>& own, const Acc& a, QString& why)
{
    if (a.view >= 0 && size_t(a.view) < st.outsideRefs.size() && st.outsideRefs[size_t(a.view)]) {
        why = QStringLiteral("bufferView %1 is also read by an image, sparse data or an extension").arg(a.view);
        return false;
    }
    const Range mine = st.accessors.value(a.index);
    QSet<int> ownViews;
    for (int i : own)
        ownViews.insert(st.accessors.value(i).view);
    const auto bit = st.byBuffer.constFind(mine.buffer);
    if (bit == st.byBuffer.constEnd())
        return true;
    const auto& entries = bit.value().entries;
    const auto& maxEnd = bit.value().maxEndSoFar;
    // Only entries starting before `mine` ends can overlap it; walk them
    // back while some earlier entry still reaches past `mine`'s start.
    const auto first = std::lower_bound(entries.begin(), entries.end(), mine.end,
                                        [](const Storage::Entry& e, qint64 v) { return e.range.start < v; });
    for (auto i = std::distance(entries.begin(), first) - 1; i >= 0 && maxEnd[size_t(i)] > mine.start; --i) {
        const Storage::Entry& e = entries[size_t(i)];
        if (!overlaps(mine, e.range))
            continue;
        if (e.accessor >= 0 && !own.contains(e.accessor)) {
            why = QStringLiteral("its bytes overlap accessor %1").arg(e.accessor);
            return false;
        }
        if (e.accessor < 0 && !ownViews.contains(e.range.view)) {
            why = QStringLiteral("its bytes overlap bufferView %1").arg(e.range.view);
            return false;
        }
    }
    return true;
}

// Does any material the primitive can be drawn with blend (alphaMode
// BLEND)? Variants count: any of them may be selected at runtime.
bool usesBlendedMaterial(const QJsonObject& json, const QJsonObject& prim)
{
    const QJsonArray materials = json.value(QStringLiteral("materials")).toArray();
    auto blended = [&materials](const QJsonValue& idx) {
        if (!idx.isDouble())
            return false;
        const int m = idx.toInt(-1);
        return m >= 0 && m < materials.size()
            && materials.at(m).toObject().value(QStringLiteral("alphaMode")).toString()
                   == QLatin1String("BLEND");
    };
    if (blended(prim.value(QStringLiteral("material"))))
        return true;
    const QJsonArray mappings = prim.value(QStringLiteral("extensions")).toObject()
                                    .value(QStringLiteral("KHR_materials_variants")).toObject()
                                    .value(QStringLiteral("mappings")).toArray();
    for (const QJsonValue& m : mappings)
        if (blended(m.toObject().value(QStringLiteral("material"))))
            return true;
    return false;
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
bool readPrimitive(const Doc& d, const QHash<int, int>& refs, const Storage& st, int m, int p,
                   Prim& out)
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

    // The bytes a rewrite would change must belong to this primitive alone.
    QString why;
    const bool indexPrivate = privateStorage(st, {out.indices.index}, out.indices, why);
    const QString indexBlocker = indexPrivate ? QString() : QStringLiteral("index accessor: %1").arg(why);
    if (vertexFixable) {
        QSet<int> own;
        for (const Acc& a : out.vertexAccessors)
            own.insert(a.index);
        for (const Acc& a : out.vertexAccessors) {
            if (!privateStorage(st, own, a, why)) {
                vertexFixable = false;
                blockers << QStringLiteral("vertex accessor %1: %2").arg(a.index).arg(why);
                break;
            }
        }
    }

    r.measured = true;
    r.indexFixable = refs.value(out.indices.index) <= 1 && !ngonEncoded && indexPrivate;
    r.vertexFixable = vertexFixable && r.indexFixable;
    const bool blended = usesBlendedMaterial(d.json, prim);
    r.orderFixable = r.indexFixable && !blended;
    r.deformable = !targets.isEmpty();
    for (auto it = attrs.begin(); it != attrs.end() && !r.deformable; ++it)
        r.deformable = it.key().startsWith(QLatin1String("JOINTS_"));
    if (ngonEncoded)
        blockers.prepend(QStringLiteral("its triangle order encodes n-gons (FB_ngon_encoding)"));
    else if (!indexBlocker.isEmpty())
        blockers.prepend(indexBlocker);
    else if (!r.indexFixable)
        blockers.prepend(QStringLiteral("its index accessor is shared"));
    if (blended && r.indexFixable)
        blockers << QStringLiteral("its material is alpha-blended, so triangle order is visible");
    if (!blockers.isEmpty())
        r.note = blockers.join(QStringLiteral("; "));
    return true;
}

// Buffers whose bytes something addresses directly, not through a plain
// bufferView (EXT_meshopt_compression's `buffer`/`byteOffset`, any view
// extension): moving their views would leave those references stale.
std::vector<char> buffersAddressedByExtensions(const QJsonArray& views, int nBuffers)
{
    std::vector<char> fixed(size_t(std::max(0, nBuffers)), 0);
    auto markBuffer = [&fixed, nBuffers](int b) {
        if (b >= 0 && b < nBuffers)
            fixed[size_t(b)] = 1;
    };
    std::function<void(const QJsonValue&)> markRefs = [&](const QJsonValue& v) {
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            for (auto it = o.begin(); it != o.end(); ++it) {
                if (it.key() == QLatin1String("buffer") && it.value().isDouble())
                    markBuffer(it.value().toInt(-1));
                markRefs(it.value());
            }
        } else if (v.isArray()) {
            for (const QJsonValue& e : v.toArray())
                markRefs(e);
        }
    };
    for (const QJsonValue& vv : views) {
        const QJsonObject vo = vv.toObject();
        if (!vo.contains(QStringLiteral("extensions")))
            continue;
        markBuffer(vo.value(QStringLiteral("buffer")).toInt(-1));
        markRefs(vo.value(QStringLiteral("extensions")));
    }
    return fixed;
}

// Repack one buffer's views back to back. Returns false (buffer untouched)
// when the views overlap or any of them lies outside the loaded bytes.
bool repackBuffer(Doc& d, int b, QJsonArray& views, const std::vector<qint64>& newLength)
{
    const int nViews = views.size();
    std::vector<int> mine;
    for (int v = 0; v < nViews; ++v)
        if (views.at(v).toObject().value(QStringLiteral("buffer")).toInt(-1) == b)
            mine.push_back(v);
    if (mine.empty())
        return false;
    auto offsetOf = [&views](int v) {
        return qint64(views.at(v).toObject().value(QStringLiteral("byteOffset")).toInt(0));
    };
    auto lengthOf = [&views](int v) {
        return qint64(views.at(v).toObject().value(QStringLiteral("byteLength")).toInt(0));
    };
    std::sort(mine.begin(), mine.end(), [&offsetOf](int x, int y) { return offsetOf(x) < offsetOf(y); });
    const qint64 size = d.buffers.at(b).size();
    qint64 prevEnd = 0;
    for (int v : mine) {
        const qint64 off = offsetOf(v);
        const qint64 len = lengthOf(v);
        // Overlapping views (legal, rare) cannot be repacked independently,
        // and a view past the end (an image nobody validated) must not be read.
        if (off < prevEnd || off < 0 || len < 0 || off + len > size || newLength[size_t(v)] > len)
            return false;
        prevEnd = off + len;
    }
    QByteArray repacked;
    for (int v : mine) {
        QJsonObject vo = views.at(v).toObject();
        while (repacked.size() % 4 != 0)
            repacked.append('\0');
        const qint64 newOff = repacked.size();
        repacked.append(d.buffers.at(b).constData() + offsetOf(v), newLength[size_t(v)]);
        vo.insert(QStringLiteral("byteOffset"), newOff);
        vo.insert(QStringLiteral("byteLength"), newLength[size_t(v)]);
        views.replace(v, vo);
    }
    d.buffers[b] = repacked;
    return true;
}

// Shrink bufferViews to the bytes their accessors still use and repack each
// buffer. Views referenced from anywhere but an accessor's own `bufferView`
// (images, sparse data, extensions) keep their full length, and buffers an
// extension addresses directly are not repacked at all.
void compactBuffers(Doc& d)
{
    QJsonArray views = d.json.value(QStringLiteral("bufferViews")).toArray();
    const int nViews = views.size();
    std::vector<qint64> used(size_t(nViews), 0);
    const std::vector<char> keepFull = viewsReferencedOutsideAccessors(d.json, nViews);

    const QJsonArray accessors = d.json.value(QStringLiteral("accessors")).toArray();
    for (int i = 0; i < accessors.size(); ++i) {
        Acc a;
        QString err;
        if (!accessors.at(i).toObject().contains(QStringLiteral("bufferView")) || !accessor(d, i, a, err))
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

    const std::vector<char> pinned = buffersAddressedByExtensions(views, d.buffers.size());
    QJsonArray buffers = d.json.value(QStringLiteral("buffers")).toArray();
    for (int b = 0; b < d.buffers.size(); ++b) {
        if (pinned[size_t(b)] || d.placeholderBuffers.contains(b) || !repackBuffer(d, b, views, newLength))
            continue;
        QJsonObject bo = buffers.at(b).toObject();
        bo.insert(QStringLiteral("byteLength"), d.buffers.at(b).size());
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

// Serialise the document: every external buffer, then the main file LAST
// (writeAll publishes in this order, so the manifest that points at the new
// buffer layout is the final file to change).
QList<QPair<QString, QByteArray>> serialise(Doc& d)
{
    QList<QPair<QString, QByteArray>> files;
    QJsonArray buffers = d.json.value(QStringLiteral("buffers")).toArray();
    for (int b = 0; b < d.buffers.size(); ++b) {
        if (d.placeholderBuffers.contains(b))
            continue;   // its byteLength describes the decoded data
        QJsonObject bo = buffers.at(b).toObject();
        if (!d.externalPaths.at(b).isEmpty()) {
            files.append({d.externalPaths.at(b), d.buffers.at(b)});
        } else if (b != d.binBuffer) {
            // A data-URI buffer (also possible inside a GLB) stays one.
            bo.insert(QStringLiteral("uri"), QStringLiteral("data:application/octet-stream;base64,")
                                                 + QString::fromLatin1(d.buffers.at(b).toBase64()));
        }
        bo.insert(QStringLiteral("byteLength"), d.buffers.at(b).size());
        buffers.replace(b, bo);
    }
    if (!buffers.isEmpty())
        d.json.insert(QStringLiteral("buffers"), buffers);
    const QByteArray main = d.glb
        ? buildGlb(d.json, d.binBuffer >= 0 ? d.buffers.at(d.binBuffer) : d.glbBin)
        : QJsonDocument(d.json).toJson(QJsonDocument::Indented);
    files.append({d.path, main});
    return files;
}

// A sibling of `path` that does not exist yet: staging and backup files are
// never pre-existing ones (a stray `mesh.glb.qtmesh_tmp` is not ours to
// truncate or delete).
QString freshSibling(const QString& path, const QString& tag)
{
    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString candidate = QStringLiteral("%1.%2.%3").arg(
            path, QUuid::createUuid().toString(QUuid::Id128).left(12), tag);
        const QFileInfo fi(candidate);
        if (!fi.exists() && !fi.isSymLink())
            return candidate;
    }
    return QString();
}

// Publish `files` as one unit: stage every file next to its destination,
// then swap them in (in order: buffers, then the main document). A failed
// swap restores every file already replaced, so the asset is never left
// with new offsets pointing into old buffer bytes. If a restore itself
// fails, the error names the file and keeps its backup.
bool writeAll(const QList<QPair<QString, QByteArray>>& files, QString& err)
{
    QStringList staged;   // parallel to `files`; only paths this call created
    auto removeStaged = [&staged]() {
        for (const QString& s : staged)
            QFile::remove(s);
    };
    for (const auto& [path, bytes] : files) {
        const QString stage = freshSibling(path, QStringLiteral("qtmesh_tmp"));
        QFile f(stage);
        if (stage.isEmpty() || !f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            err = QStringLiteral("cannot create a staging file next to %1").arg(path);
            removeStaged();
            return false;
        }
        staged << stage;
        if (f.write(bytes) != bytes.size() || !f.flush()) {
            err = QStringLiteral("cannot write %1").arg(path);
            f.close();
            removeStaged();
            return false;
        }
    }

    struct Swapped {
        QString path;
        QString backup;   // empty: the destination did not exist before
    };
    QList<Swapped> swapped;
    auto rollback = [&]() {
        QStringList unrestored;
        for (auto it = swapped.rbegin(); it != swapped.rend(); ++it) {
            QFile::remove(it->path);
            if (!it->backup.isEmpty() && !QFile::rename(it->backup, it->path))
                unrestored << QStringLiteral("%1 (original kept at %2)").arg(it->path, it->backup);
        }
        removeStaged();
        if (!unrestored.isEmpty())
            err += QStringLiteral("; could not restore %1").arg(unrestored.join(QStringLiteral(", ")));
    };
    for (int i = 0; i < files.size(); ++i) {
        const QString& path = files.at(i).first;
        Swapped sw{path, QString()};
        if (QFile::exists(path)) {
            sw.backup = freshSibling(path, QStringLiteral("qtmesh_bak"));
            if (sw.backup.isEmpty() || !QFile::rename(path, sw.backup)) {
                err = QStringLiteral("cannot replace %1").arg(path);
                rollback();
                return false;
            }
        }
        if (!QFile::rename(staged.at(i), path)) {
            err = QStringLiteral("cannot replace %1").arg(path);
            if (!sw.backup.isEmpty() && !QFile::rename(sw.backup, path))
                err += QStringLiteral(" (original kept at %1)").arg(sw.backup);
            rollback();
            return false;
        }
        swapped.append(sw);
    }
    for (const Swapped& sw : swapped)
        if (!sw.backup.isEmpty())
            QFile::remove(sw.backup);
    return true;
}

// Refuse rewrites this writer cannot do faithfully or safely.
bool checkWritable(const Doc& d, const QString& writeRoot, const QSet<QString>& sharedBuffers,
                   QString& err)
{
    if (d.glbExtraChunks) {
        err = QStringLiteral("the GLB has chunks besides JSON and BIN, which a rewrite would drop");
        return false;
    }
    const QString root = QFileInfo(writeRoot.isEmpty() ? QFileInfo(d.path).absolutePath() : writeRoot)
                             .canonicalFilePath();
    if (root.isEmpty()) {
        err = QStringLiteral("cannot resolve the write root %1").arg(writeRoot);
        return false;
    }
    const QString rootPrefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    QSet<QString> seen{QFileInfo(d.path).canonicalFilePath()};
    for (const QString& ext : d.externalPaths) {
        if (ext.isEmpty())
            continue;
        const QString canon = QFileInfo(ext).canonicalFilePath();
        if (canon.isEmpty() || !canon.startsWith(rootPrefix)) {
            err = QStringLiteral("external buffer %1 is outside %2; not rewriting it").arg(ext, root);
            return false;
        }
        if (seen.contains(canon)) {
            err = QStringLiteral("two buffers (or the document) resolve to %1; not rewriting").arg(canon);
            return false;
        }
        if (sharedBuffers.contains(canon)) {
            err = QStringLiteral("external buffer %1 is also used by another glTF file; rewriting it "
                                 "would break that file").arg(ext);
            return false;
        }
        seen.insert(canon);
    }
    return true;
}

QList<Prim> readPrimitives(const Doc& d)
{
    QList<Prim> prims;
    const QHash<int, int> refs = accessorReferences(d.json);
    const Storage st = storageMap(d);
    const QJsonArray meshes = d.json.value(QStringLiteral("meshes")).toArray();
    for (int m = 0; m < meshes.size(); ++m) {
        const int n = meshes.at(m).toObject().value(QStringLiteral("primitives")).toArray().size();
        for (int p = 0; p < n; ++p) {
            Prim prim;
            readPrimitive(d, refs, st, m, p, prim);
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
                  const MeshQuality::Limits& limits, const QString& writeRoot,
                  const QSet<QString>& sharedBuffers)
{
    FixReport rep;
    Doc d;
    if (!loadDoc(path, d, rep.error) || !checkWritable(d, writeRoot, sharedBuffers, rep.error))
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
        if (!pr.orderFixable && (o.vertexCache || o.overdraw)) {
            o.vertexCache = false;
            o.overdraw = false;
            rep.notes << QStringLiteral("%1: triangle reordering skipped (%2)").arg(label, pr.note);
        }
        if (pr.deformable && o.removeDegenerates) {
            o.degeneratesByIndexOnly = true;
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

QStringList externalBufferPaths(const QString& path)
{
    // JSON only: the buffers themselves are not read.
    QStringList out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QByteArray bytes = f.readAll();
    QJsonObject json;
    if (bytes.startsWith("glTF")) {
        QByteArray bin;
        bool extra = false;
        QString err;
        if (!parseGlb(bytes, json, bin, extra, err))
            return out;
    } else {
        const QJsonDocument doc = QJsonDocument::fromJson(bytes);
        if (!doc.isObject())
            return out;
        json = doc.object();
    }
    const QDir dir = QFileInfo(path).absoluteDir();
    for (const QJsonValue& b : json.value(QStringLiteral("buffers")).toArray()) {
        const QString uri = b.toObject().value(QStringLiteral("uri")).toString();
        if (uri.isEmpty() || uri.startsWith(QLatin1String("data:")))
            continue;
        const QString canon = QFileInfo(QDir::cleanPath(
            dir.filePath(QUrl::fromPercentEncoding(uri.toUtf8())))).canonicalFilePath();
        if (!canon.isEmpty() && !out.contains(canon))
            out << canon;
    }
    return out;
}

QSet<QString> sharedExternalBuffers(const QStringList& documents)
{
    // Each DOCUMENT counts once per buffer: overlapping scan roots list the
    // same file twice, and a document may name one .bin in several buffers;
    // neither is a second user.
    QHash<QString, int> users;
    QSet<QString> seenDocs;
    for (const QString& doc : documents) {
        const QFileInfo info(doc);
        const QString suffix = info.suffix().toLower();
        if (suffix != QLatin1String("gltf") && suffix != QLatin1String("glb"))
            continue;
        QString key = info.canonicalFilePath();
        if (key.isEmpty())
            key = info.absoluteFilePath();
        if (seenDocs.contains(key))
            continue;
        seenDocs.insert(key);
        const QStringList bufs = externalBufferPaths(doc);
        for (const QString& buf : QSet<QString>(bufs.cbegin(), bufs.cend()))
            ++users[buf];
    }
    QSet<QString> shared;
    for (auto it = users.cbegin(); it != users.cend(); ++it)
        if (it.value() > 1)
            shared.insert(it.key());
    return shared;
}

} // namespace GltfMeshQuality
