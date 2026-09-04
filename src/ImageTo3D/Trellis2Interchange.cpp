#include "Trellis2Interchange.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <cstring>
#include <limits>
#include <cmath>

namespace Trellis2Interchange {

namespace {

constexpr char kMagic[8] = {'Q', 'T', 'M', 'E', 'S', 'H', '3', 'D'};
constexpr uint32_t kVersion = 1;

inline qint64 align16(qint64 n) { return (n + 15) & ~qint64(15); }

struct ArrayRef {
    QString dtype;
    QList<qint64> shape;
    qint64 offset = 0;
    qint64 byteLength = 0;
    bool valid = false;
};

ArrayRef arrayRef(const QJsonObject& arrays, const QString& name)
{
    ArrayRef ref;
    if (!arrays.contains(name))
        return ref;
    const QJsonObject o = arrays.value(name).toObject();
    ref.dtype = o.value(QStringLiteral("dtype")).toString();
    for (const QJsonValue& v : o.value(QStringLiteral("shape")).toArray())
        ref.shape.append(static_cast<qint64>(v.toDouble(-1)));
    ref.offset = static_cast<qint64>(o.value(QStringLiteral("offset")).toDouble(-1));
    ref.byteLength =
        static_cast<qint64>(o.value(QStringLiteral("byteLength")).toDouble(-1));
    ref.valid = !ref.dtype.isEmpty() && !ref.shape.isEmpty()
                && ref.offset >= 0 && ref.byteLength >= 0;
    return ref;
}

qint64 elementSize(const QString& dtype)
{
    if (dtype == QLatin1String("f32") || dtype == QLatin1String("u32")
        || dtype == QLatin1String("i32"))
        return 4;
    if (dtype == QLatin1String("f16") || dtype == QLatin1String("u16"))
        return 2;
    if (dtype == QLatin1String("u8"))
        return 1;
    return 0;
}

qint64 elementCount(const ArrayRef& ref)
{
    // shape values are FILE-CONTROLLED: multiply with overflow checks so a
    // crafted manifest can't wrap the count and slip past checkedBlob().
    qint64 n = 1;
    for (qint64 d : ref.shape) {
        if (d < 0)
            return -1;
        if (d != 0 && n > std::numeric_limits<qint64>::max() / d)
            return -1;
        n *= d;
    }
    return n;
}

// Bounds-check an array against the blob section, verify byteLength matches
// dtype*shape, and return a pointer into `blob`.
const uint8_t* checkedBlob(const QByteArray& blob, qint64 blobBase,
                           const ArrayRef& ref, QString* error)
{
    const qint64 esize = elementSize(ref.dtype);
    if (esize == 0) {
        *error = QStringLiteral("unsupported dtype '%1'").arg(ref.dtype);
        return nullptr;
    }
    const qint64 count = elementCount(ref);
    if (count < 0 || count > std::numeric_limits<qint64>::max() / esize
        || count * esize != ref.byteLength) {
        *error = QStringLiteral("array size mismatch (dtype %1)").arg(ref.dtype);
        return nullptr;
    }
    // offset/byteLength are file-controlled too — reject any range whose
    // arithmetic would overflow before it can be bounds-checked.
    if (ref.offset > std::numeric_limits<qint64>::max() - blobBase) {
        *error = QStringLiteral("array exceeds file bounds");
        return nullptr;
    }
    const qint64 start = blobBase + ref.offset;
    if (start < 0 || ref.byteLength > blob.size()
        || start > blob.size() - ref.byteLength) {
        *error = QStringLiteral("array exceeds file bounds");
        return nullptr;
    }
    return reinterpret_cast<const uint8_t*>(blob.constData()) + start;
}

} // namespace

// IEC 61966-2-1 linear -> sRGB transfer.
static float linearToSrgb(float v)
{
    return v <= 0.0031308f ? v * 12.92f
                           : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

ReadResult read(const QString& path)
{
    ReadResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("cannot open %1").arg(path);
        return r;
    }
    const QByteArray blob = f.readAll();
    if (blob.size() < 16 || std::memcmp(blob.constData(), kMagic, 8) != 0) {
        r.error = QStringLiteral("not a QTM3D file");
        return r;
    }
    uint32_t version = 0, jsonLen = 0;
    std::memcpy(&version, blob.constData() + 8, 4);
    std::memcpy(&jsonLen, blob.constData() + 12, 4);
    if (version != kVersion) {
        r.error = QStringLiteral("unsupported QTM3D version %1").arg(version);
        return r;
    }
    if (qint64(16) + jsonLen > blob.size()) {
        r.error = QStringLiteral("truncated manifest");
        return r;
    }
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArray(blob.constData() + 16, static_cast<int>(jsonLen)), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error = QStringLiteral("bad manifest JSON: %1").arg(perr.errorString());
        return r;
    }
    const QJsonObject manifest = doc.object();
    const QJsonObject arrays = manifest.value(QStringLiteral("arrays")).toObject();
    const qint64 blobBase = align16(16 + jsonLen);

    Data& d = r.data;
    d.meta = manifest.value(QStringLiteral("meta")).toObject();
    d.resolution = d.meta.value(QStringLiteral("resolution")).toInt(0);
    d.voxelSize =
        static_cast<float>(d.meta.value(QStringLiteral("voxelSize")).toDouble(0.0));
    const QJsonArray originArr = d.meta.value(QStringLiteral("origin")).toArray();
    for (int i = 0; i < 3 && i < originArr.size(); ++i)
        d.origin[i] = static_cast<float>(originArr.at(i).toDouble(d.origin[i]));

    QString err;

    // ---- positions (required, f32 [N,3]) -----------------------------------
    {
        const ArrayRef ref = arrayRef(arrays, QStringLiteral("positions"));
        if (!ref.valid || ref.shape.size() != 2 || ref.shape[1] != 3
            || ref.dtype != QLatin1String("f32")) {
            r.error = QStringLiteral("missing/invalid 'positions' array");
            return r;
        }
        const uint8_t* p = checkedBlob(blob, blobBase, ref, &err);
        if (!p) { r.error = QStringLiteral("positions: %1").arg(err); return r; }
        d.vertexCount = static_cast<int>(ref.shape[0]);
        d.positions.resize(static_cast<size_t>(d.vertexCount) * 3);
        std::memcpy(d.positions.data(), p, static_cast<size_t>(ref.byteLength));
    }

    // ---- indices (required, u32 [M,3]) --------------------------------------
    {
        const ArrayRef ref = arrayRef(arrays, QStringLiteral("indices"));
        if (!ref.valid || ref.shape.size() != 2 || ref.shape[1] != 3
            || ref.dtype != QLatin1String("u32")) {
            r.error = QStringLiteral("missing/invalid 'indices' array");
            return r;
        }
        const uint8_t* p = checkedBlob(blob, blobBase, ref, &err);
        if (!p) { r.error = QStringLiteral("indices: %1").arg(err); return r; }
        d.triangleCount = static_cast<int>(ref.shape[0]);
        d.indices.resize(static_cast<size_t>(d.triangleCount) * 3);
        std::memcpy(d.indices.data(), p, static_cast<size_t>(ref.byteLength));
        for (uint32_t idx : d.indices) {
            if (idx >= static_cast<uint32_t>(d.vertexCount)) {
                r.error = QStringLiteral("index out of range (%1 >= %2)")
                              .arg(idx).arg(d.vertexCount);
                return r;
            }
        }
    }

    // ---- voxel_coords (optional, u16/u32 [L,3]) ------------------------------
    {
        const ArrayRef ref = arrayRef(arrays, QStringLiteral("voxel_coords"));
        if (ref.valid) {
            if (ref.shape.size() != 2 || ref.shape[1] != 3
                || (ref.dtype != QLatin1String("u16")
                    && ref.dtype != QLatin1String("u32"))) {
                r.error = QStringLiteral("invalid 'voxel_coords' array");
                return r;
            }
            const uint8_t* p = checkedBlob(blob, blobBase, ref, &err);
            if (!p) { r.error = QStringLiteral("voxel_coords: %1").arg(err); return r; }
            d.voxelCount = static_cast<int>(ref.shape[0]);
            d.voxelCoords.resize(static_cast<size_t>(d.voxelCount) * 3);
            if (ref.dtype == QLatin1String("u32")) {
                std::memcpy(d.voxelCoords.data(), p,
                            static_cast<size_t>(ref.byteLength));
            } else {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(p);
                for (size_t i = 0; i < d.voxelCoords.size(); ++i)
                    d.voxelCoords[i] = s[i];
            }
            // Coordinates index the res³ grid — a coordinate outside
            // [0, resolution) would make every volume sampler mis-key.
            if (d.resolution <= 0) {
                r.error = QStringLiteral(
                    "'voxel_coords' present without a positive 'resolution'");
                return r;
            }
            for (uint32_t c : d.voxelCoords) {
                if (c >= static_cast<uint32_t>(d.resolution)) {
                    r.error = QStringLiteral(
                        "voxel coordinate %1 outside grid (resolution %2)")
                                  .arg(c).arg(d.resolution);
                    return r;
                }
            }
        }
    }

    // ---- voxel_attrs (required alongside voxel_coords, u8 [L,6]) -------------
    {
        const ArrayRef ref = arrayRef(arrays, QStringLiteral("voxel_attrs"));
        if (ref.valid) {
            if (ref.shape.size() != 2 || ref.shape[1] != 6
                || ref.dtype != QLatin1String("u8")
                || ref.shape[0] != d.voxelCount) {
                r.error = QStringLiteral("invalid 'voxel_attrs' array");
                return r;
            }
            const uint8_t* p = checkedBlob(blob, blobBase, ref, &err);
            if (!p) { r.error = QStringLiteral("voxel_attrs: %1").arg(err); return r; }
            d.voxelAttrs.assign(p, p + ref.byteLength);
        } else if (d.voxelCount > 0) {
            r.error = QStringLiteral("'voxel_coords' present without 'voxel_attrs'");
            return r;
        }
    }

    // ---- vertex_colors (optional, u8 [N,4]) ----------------------------------
    {
        const ArrayRef ref = arrayRef(arrays, QStringLiteral("vertex_colors"));
        if (ref.valid) {
            if (ref.shape.size() != 2 || ref.shape[1] != 4
                || ref.dtype != QLatin1String("u8")
                || ref.shape[0] != d.vertexCount) {
                r.error = QStringLiteral("invalid 'vertex_colors' array");
                return r;
            }
            const uint8_t* p = checkedBlob(blob, blobBase, ref, &err);
            if (!p) { r.error = QStringLiteral("vertex_colors: %1").arg(err); return r; }
            d.vertexColors.assign(p, p + ref.byteLength);
        }
    }

    if (d.vertexCount <= 0 || d.triangleCount <= 0) {
        r.error = QStringLiteral("empty mesh");
        return r;
    }
    // LEGACY color space: qtm3d files written before the sRGB stamp hold
    // LINEAR base-color lanes (both the trellis.cpp dump path and the
    // Python sidecar wrote the model's raw linear output). Convert them on
    // load so old preserved sources re-bake with correct brightness; files
    // stamped "srgb" are already display-ready.
    if (d.meta.value(QStringLiteral("colorSpace")).toString()
            != QLatin1String("srgb")
        && !d.voxelAttrs.empty()) {
        for (size_t i = 0; i < d.voxelAttrs.size(); ++i) {
            if (i % 6 >= 3)
                continue;
            const float lin = d.voxelAttrs[i] / 255.0f;
            d.voxelAttrs[i] = static_cast<uint8_t>(
                linearToSrgb(lin) * 255.0f + 0.5f);
        }
        d.meta.insert(QStringLiteral("colorSpace"), QStringLiteral("srgb"));
    }
    r.ok = true;
    return r;
}

bool write(const QString& path, const Data& data, QString* error)
{
    auto failWith = [error](const QString& msg) {
        if (error) *error = msg;
        return false;
    };
    if (data.positions.size() != static_cast<size_t>(data.vertexCount) * 3
        || data.indices.size() != static_cast<size_t>(data.triangleCount) * 3)
        return failWith(QStringLiteral("inconsistent counts"));

    struct Entry {
        const char* name;
        const char* dtype;
        qint64 rows;
        qint64 cols;
        const void* ptr;
        qint64 bytes;
    };
    std::vector<Entry> entries;
    entries.push_back({"positions", "f32", data.vertexCount, 3,
                       data.positions.data(),
                       qint64(data.positions.size() * sizeof(float))});
    entries.push_back({"indices", "u32", data.triangleCount, 3,
                       data.indices.data(),
                       qint64(data.indices.size() * sizeof(uint32_t))});
    if (data.voxelCount > 0) {
        entries.push_back({"voxel_coords", "u32", data.voxelCount, 3,
                           data.voxelCoords.data(),
                           qint64(data.voxelCoords.size() * sizeof(uint32_t))});
        entries.push_back({"voxel_attrs", "u8", data.voxelCount, 6,
                           data.voxelAttrs.data(),
                           qint64(data.voxelAttrs.size())});
    }
    if (!data.vertexColors.empty())
        entries.push_back({"vertex_colors", "u8", data.vertexCount, 4,
                           data.vertexColors.data(),
                           qint64(data.vertexColors.size())});

    QJsonObject arrays;
    qint64 offset = 0;
    for (const Entry& e : entries) {
        QJsonObject o;
        o.insert(QStringLiteral("dtype"), QLatin1String(e.dtype));
        o.insert(QStringLiteral("shape"),
                 QJsonArray{static_cast<double>(e.rows),
                            static_cast<double>(e.cols)});
        o.insert(QStringLiteral("offset"), static_cast<double>(offset));
        o.insert(QStringLiteral("byteLength"), static_cast<double>(e.bytes));
        arrays.insert(QLatin1String(e.name), o);
        offset = align16(offset + e.bytes);
    }

    QJsonObject meta = data.meta;
    meta.insert(QStringLiteral("resolution"), data.resolution);
    meta.insert(QStringLiteral("voxelSize"), static_cast<double>(data.voxelSize));
    meta.insert(QStringLiteral("origin"),
                QJsonArray{static_cast<double>(data.origin[0]),
                           static_cast<double>(data.origin[1]),
                           static_cast<double>(data.origin[2])});

    QJsonObject manifest;
    manifest.insert(QStringLiteral("generator"), QStringLiteral("qtmesh-trellis2"));
    manifest.insert(QStringLiteral("formatVersion"),
                    static_cast<int>(kVersion));
    manifest.insert(QStringLiteral("meta"), meta);
    manifest.insert(QStringLiteral("arrays"), arrays);
    const QByteArray json =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return failWith(QStringLiteral("cannot write %1").arg(path));
    // A short write (disk full, quota, unplugged volume) must not leave a
    // truncated file that reads as "valid until it isn't" — check every
    // write and fail loudly.
    auto put = [&f](const char* p, qint64 n) {
        return f.write(p, n) == n;
    };
    bool ok = put(kMagic, 8);
    const uint32_t version = kVersion;
    const uint32_t jsonLen = static_cast<uint32_t>(json.size());
    ok = ok && put(reinterpret_cast<const char*>(&version), 4);
    ok = ok && put(reinterpret_cast<const char*>(&jsonLen), 4);
    ok = ok && f.write(json) == json.size();
    const qint64 headerLen = 16 + json.size();
    const qint64 blobBase = align16(headerLen);
    static const char zeros[16] = {};
    ok = ok && put(zeros, blobBase - headerLen);
    qint64 pos = 0;
    for (const Entry& e : entries) {
        if (!ok)
            break;
        const qint64 wanted =
            static_cast<qint64>(arrays.value(QLatin1String(e.name))
                                    .toObject()
                                    .value(QStringLiteral("offset"))
                                    .toDouble());
        if (wanted > pos) {
            ok = put(zeros, wanted - pos);
            pos = wanted;
        }
        ok = ok && put(reinterpret_cast<const char*>(e.ptr), e.bytes);
        pos += e.bytes;
    }
    ok = ok && f.flush();
    if (!ok) {
        f.close();
        QFile::remove(path);
        return failWith(QStringLiteral("short write to %1 (%2)")
                            .arg(path, f.errorString()));
    }
    return true;
}

ReadResult readTrellisCppDump(const QString& path)
{
    ReadResult r;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("cannot open %1").arg(path);
        return r;
    }
    const QByteArray blob = f.readAll();
    if (blob.size() < 16) {
        r.error = QStringLiteral("truncated trellis.cpp dump");
        return r;
    }
    int32_t V = 0, F = 0, Mv = 0, res = 0;
    std::memcpy(&V, blob.constData() + 0, 4);
    std::memcpy(&F, blob.constData() + 4, 4);
    std::memcpy(&Mv, blob.constData() + 8, 4);
    std::memcpy(&res, blob.constData() + 12, 4);
    if (V <= 0 || F <= 0 || Mv < 0 || res <= 0 || res > 4096
        || V > 100000000 || F > 200000000 || Mv > 200000000) {
        r.error = QStringLiteral("implausible trellis.cpp dump header "
                                 "(V=%1 F=%2 Mv=%3 res=%4)")
                      .arg(V).arg(F).arg(Mv).arg(res);
        return r;
    }
    const qint64 expected = 16
        + qint64(V) * 3 * 4 + qint64(F) * 3 * 4
        + qint64(Mv) * 3 * 4 + qint64(Mv) * 6 * 4;
    if (blob.size() < expected) {
        r.error = QStringLiteral("trellis.cpp dump shorter than header claims "
                                 "(%1 < %2 bytes)").arg(blob.size()).arg(expected);
        return r;
    }

    Data& d = r.data;
    const char* p = blob.constData() + 16;
    d.vertexCount = V;
    d.triangleCount = F;
    d.voxelCount = Mv;
    d.resolution = res;
    d.voxelSize = 1.0f / static_cast<float>(res);
    d.origin[0] = d.origin[1] = d.origin[2] = -0.5f;
    d.positions.resize(static_cast<size_t>(V) * 3);
    std::memcpy(d.positions.data(), p, d.positions.size() * 4);
    p += static_cast<size_t>(V) * 12;
    d.indices.resize(static_cast<size_t>(F) * 3);
    // faces are i32 in the dump; reinterpret via copy with range check.
    {
        const int32_t* fp = reinterpret_cast<const int32_t*>(p);
        for (size_t i = 0; i < d.indices.size(); ++i) {
            const int32_t idx = fp[i];
            if (idx < 0 || idx >= V) {
                r = ReadResult{};
                r.error = QStringLiteral("dump face index out of range (%1)").arg(idx);
                return r;
            }
            d.indices[i] = static_cast<uint32_t>(idx);
        }
        p += static_cast<size_t>(F) * 12;
    }
    if (Mv > 0) {
        d.voxelCoords.resize(static_cast<size_t>(Mv) * 3);
        const int32_t* cp = reinterpret_cast<const int32_t*>(p);
        for (size_t i = 0; i < d.voxelCoords.size(); ++i) {
            const int32_t c = cp[i];
            if (c < 0 || c >= res) {
                r = ReadResult{};
                r.error = QStringLiteral("dump voxel coord out of range (%1)").arg(c);
                return r;
            }
            d.voxelCoords[i] = static_cast<uint32_t>(c);
        }
        p += static_cast<size_t>(Mv) * 12;
        d.voxelAttrs.resize(static_cast<size_t>(Mv) * 6);
        const float* ap = reinterpret_cast<const float*>(p);
        for (size_t i = 0; i < d.voxelAttrs.size(); ++i) {
            const float v = ap[i];
            // NaN fails both comparisons and float->u8 of NaN is UB —
            // treat non-finite lanes as 0.
            float cl = !std::isfinite(v) ? 0.0f
                : (v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v));
            // trellis.cpp emits LINEAR-light colors (no gamma anywhere in
            // its pipeline — verified against the source). Encode the BASE
            // COLOR channels (lanes 0..2 of the 6-lane layout) to sRGB
            // before quantizing, or every texture bakes murky-dark (a
            // bright-green goblin measured mean luminance 0.096 raw vs a
            // plausible 0.32 in sRGB). Metallic/roughness/alpha stay linear
            // by definition.
            if (i % 6 < 3)
                cl = linearToSrgb(cl);
            d.voxelAttrs[i] = static_cast<uint8_t>(cl * 255.0f + 0.5f);
        }
    }
    d.meta.insert(QStringLiteral("generator"), QStringLiteral("trellis.cpp"));
    d.meta.insert(QStringLiteral("resolution"), res);
    // Stamp the color space so a re-bake of this preserved source doesn't
    // convert twice; qtm3d files WITHOUT the stamp predate the conversion
    // and hold linear values (read() converts those on load).
    d.meta.insert(QStringLiteral("colorSpace"), QStringLiteral("srgb"));
    r.ok = true;
    return r;
}

} // namespace Trellis2Interchange
