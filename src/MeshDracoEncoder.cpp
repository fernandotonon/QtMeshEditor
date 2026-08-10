/*
-----------------------------------------------------------------------------------
A QtMeshEditor file

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)

The MIT License

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------------
*/

#include "MeshDracoEncoder.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QByteArray>
#include <QMap>
#include <QSet>
#include <QUrl>
#include <QtEndian>

#include <cstring>
#include <functional>
#include <vector>

#ifdef ENABLE_DRACO
#include "draco/compression/encode.h"
#include "draco/mesh/triangle_soup_mesh_builder.h"
#include "draco/core/encoder_buffer.h"
#include "draco/attributes/geometry_attribute.h"
#include "draco/core/draco_types.h"
#endif

bool MeshDracoEncoder::isSupported()
{
#ifdef ENABLE_DRACO
    return true;
#else
    return false;
#endif
}

MeshDracoEncoder::Result MeshDracoEncoder::compressFile(const QString& path)
{
    return compressFile(path, Options());
}

#ifndef ENABLE_DRACO

MeshDracoEncoder::Result MeshDracoEncoder::compressFile(const QString&, const Options&)
{
    Result r;
    r.ok = false;
    r.error = QStringLiteral(
        "Draco compression is not available in this build. "
        "Rebuild with -DENABLE_DRACO=ON (needs the Draco library — build the "
        "vendored contrib/draco standalone, or pass -DDRACO_ROOT=<dir>).");
    return r;
}

#else // ENABLE_DRACO

namespace {

// ---------------------------------------------------------------------------
// glTF accessor / component-type constants (from the glTF 2.0 spec)
// ---------------------------------------------------------------------------
constexpr int GLTF_BYTE           = 5120;
constexpr int GLTF_UNSIGNED_BYTE  = 5121;
constexpr int GLTF_SHORT          = 5122;
constexpr int GLTF_UNSIGNED_SHORT = 5123;
constexpr int GLTF_UNSIGNED_INT   = 5125;
constexpr int GLTF_FLOAT          = 5126;

int componentByteSize(int componentType)
{
    switch (componentType) {
    case GLTF_BYTE:
    case GLTF_UNSIGNED_BYTE:  return 1;
    case GLTF_SHORT:
    case GLTF_UNSIGNED_SHORT: return 2;
    case GLTF_UNSIGNED_INT:
    case GLTF_FLOAT:          return 4;
    default:                  return 0;
    }
}

int typeComponentCount(const QString& type)
{
    if (type == "SCALAR") return 1;
    if (type == "VEC2")   return 2;
    if (type == "VEC3")   return 3;
    if (type == "VEC4")   return 4;
    if (type == "MAT2")   return 4;
    if (type == "MAT3")   return 9;
    if (type == "MAT4")   return 16;
    return 0;
}

// A single logical glTF binary buffer (glb keeps one; .gltf may have one via
// data: URI or an external .bin — we resolve both into memory).
struct GltfContainer {
    QJsonObject json;
    QByteArray  bin;       // the single binary buffer (buffer index 0)
    bool        isGlb = false;
};

// glb: 12-byte header (magic 'glTF', version, total length) then chunks:
//   [uint32 chunkLength][uint32 chunkType][data]
// chunkType 0x4E4F534A = 'JSON', 0x004E4942 = 'BIN\0'.
constexpr quint32 GLB_MAGIC     = 0x46546C67; // 'glTF' little-endian
constexpr quint32 GLB_CHUNK_JSON = 0x4E4F534A;
constexpr quint32 GLB_CHUNK_BIN  = 0x004E4942;

bool parseGlb(const QByteArray& bytes, GltfContainer& out, QString& err)
{
    if (bytes.size() < 12) { err = "glb too small"; return false; }
    const uchar* p = reinterpret_cast<const uchar*>(bytes.constData());
    quint32 magic = qFromLittleEndian<quint32>(p);
    if (magic != GLB_MAGIC) { err = "not a glb (bad magic)"; return false; }
    quint32 total = qFromLittleEndian<quint32>(p + 8);
    if (static_cast<int>(total) > bytes.size()) { err = "glb length exceeds file"; return false; }

    int offset = 12;
    QByteArray jsonChunk;
    QByteArray binChunk;
    while (offset + 8 <= static_cast<int>(total)) {
        quint32 clen = qFromLittleEndian<quint32>(p + offset);
        quint32 ctype = qFromLittleEndian<quint32>(p + offset + 4);
        offset += 8;
        if (offset + static_cast<int>(clen) > bytes.size()) { err = "glb chunk overruns file"; return false; }
        if (ctype == GLB_CHUNK_JSON)
            jsonChunk = bytes.mid(offset, clen);
        else if (ctype == GLB_CHUNK_BIN)
            binChunk = bytes.mid(offset, clen);
        offset += clen;
    }
    if (jsonChunk.isEmpty()) { err = "glb has no JSON chunk"; return false; }

    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(jsonChunk, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        err = "glb JSON parse failed: " + perr.errorString();
        return false;
    }
    out.json = doc.object();
    out.bin = binChunk;
    out.isGlb = true;
    return true;
}

// Resolve buffer 0 for a .gltf: either a data: URI or an external .bin next to
// the file. Only single-buffer glTF is handled (Assimp writes exactly one).
bool resolveGltfBuffer(const QJsonObject& json, const QString& gltfDir,
                       QByteArray& binOut, QString& err)
{
    QJsonArray buffers = json.value("buffers").toArray();
    if (buffers.isEmpty()) { binOut = QByteArray(); return true; }
    QJsonObject buf0 = buffers.at(0).toObject();
    QString uri = buf0.value("uri").toString();
    if (uri.isEmpty()) { err = ".gltf buffer 0 has no uri"; return false; }
    if (uri.startsWith("data:")) {
        int comma = uri.indexOf(',');
        if (comma < 0) { err = "malformed data URI"; return false; }
        binOut = QByteArray::fromBase64(uri.mid(comma + 1).toUtf8());
        return true;
    }
    QString binPath = gltfDir + "/" + QUrl::fromPercentEncoding(uri.toUtf8());
    QFile f(binPath);
    if (!f.open(QIODevice::ReadOnly)) { err = "cannot open external buffer: " + binPath; return false; }
    binOut = f.readAll();
    return true;
}

// Read one attribute/index accessor into a tightly-packed float or int array.
// Returns raw bytes packed at the accessor's natural stride (no interleaving).
struct AccessorData {
    int count = 0;
    int numComponents = 0;
    int componentType = 0;
    QByteArray packed;  // count * numComponents * componentByteSize, tight
};

bool readAccessor(const GltfContainer& c, int accessorIndex, AccessorData& out, QString& err)
{
    QJsonArray accessors = c.json.value("accessors").toArray();
    QJsonArray bufferViews = c.json.value("bufferViews").toArray();
    if (accessorIndex < 0 || accessorIndex >= accessors.size()) { err = "accessor index out of range"; return false; }
    QJsonObject acc = accessors.at(accessorIndex).toObject();

    out.count = acc.value("count").toInt();
    out.componentType = acc.value("componentType").toInt();
    out.numComponents = typeComponentCount(acc.value("type").toString());
    if (out.count <= 0 || out.numComponents <= 0) { err = "accessor has no elements"; return false; }
    int compSize = componentByteSize(out.componentType);
    if (compSize == 0) { err = "unknown componentType"; return false; }
    int elemSize = compSize * out.numComponents;

    if (!acc.contains("bufferView")) {
        // Sparse-only or all-zero accessor — treat as zero-filled.
        out.packed = QByteArray(out.count * elemSize, '\0');
        return true;
    }
    int bvIndex = acc.value("bufferView").toInt();
    if (bvIndex < 0 || bvIndex >= bufferViews.size()) { err = "bufferView index out of range"; return false; }
    QJsonObject bv = bufferViews.at(bvIndex).toObject();
    int bvOffset = bv.value("byteOffset").toInt(0);
    int accOffset = acc.value("byteOffset").toInt(0);
    int stride = bv.value("byteStride").toInt(0);
    if (stride == 0) stride = elemSize; // tightly packed

    int base = bvOffset + accOffset;
    out.packed.resize(out.count * elemSize);
    for (int i = 0; i < out.count; ++i) {
        int src = base + i * stride;
        if (src + elemSize > c.bin.size()) { err = "accessor reads past end of buffer"; return false; }
        std::memcpy(out.packed.data() + i * elemSize, c.bin.constData() + src, elemSize);
    }
    return true;
}

// Read the index accessor into a flat uint32 face-index array (triangles).
bool readIndices(const GltfContainer& c, int accessorIndex,
                 std::vector<uint32_t>& indices, QString& err)
{
    AccessorData a;
    if (!readAccessor(c, accessorIndex, a, err)) return false;
    if (a.numComponents != 1) { err = "index accessor is not SCALAR"; return false; }
    indices.resize(a.count);
    const char* d = a.packed.constData();
    for (int i = 0; i < a.count; ++i) {
        switch (a.componentType) {
        case GLTF_UNSIGNED_BYTE:
            indices[i] = static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(d)[i]); break;
        case GLTF_UNSIGNED_SHORT:
            indices[i] = static_cast<uint32_t>(reinterpret_cast<const uint16_t*>(d)[i]); break;
        case GLTF_UNSIGNED_INT:
            indices[i] = reinterpret_cast<const uint32_t*>(d)[i]; break;
        default:
            err = "unsupported index componentType"; return false;
        }
    }
    return true;
}

// Read an attribute accessor into a flat float array (Draco quantizes floats).
// Integer component types are converted to float (COLOR_0 can be normalized
// ushort/ubyte; JOINTS/WEIGHTS are excluded from compression, see caller).
bool readAttributeAsFloat(const GltfContainer& c, int accessorIndex,
                          std::vector<float>& values, int& numComponents, QString& err)
{
    AccessorData a;
    if (!readAccessor(c, accessorIndex, a, err)) return false;
    numComponents = a.numComponents;
    values.resize(static_cast<size_t>(a.count) * a.numComponents);
    const char* d = a.packed.constData();
    const int n = a.count * a.numComponents;
    for (int i = 0; i < n; ++i) {
        switch (a.componentType) {
        case GLTF_FLOAT:
            values[i] = reinterpret_cast<const float*>(d)[i]; break;
        case GLTF_UNSIGNED_BYTE:
            values[i] = static_cast<float>(reinterpret_cast<const uint8_t*>(d)[i]); break;
        case GLTF_BYTE:
            values[i] = static_cast<float>(reinterpret_cast<const int8_t*>(d)[i]); break;
        case GLTF_UNSIGNED_SHORT:
            values[i] = static_cast<float>(reinterpret_cast<const uint16_t*>(d)[i]); break;
        case GLTF_SHORT:
            values[i] = static_cast<float>(reinterpret_cast<const int16_t*>(d)[i]); break;
        case GLTF_UNSIGNED_INT:
            values[i] = static_cast<float>(reinterpret_cast<const uint32_t*>(d)[i]); break;
        default:
            err = "unsupported attribute componentType"; return false;
        }
    }
    return true;
}

// Map a glTF attribute semantic to a Draco attribute type + per-attribute
// quantization bits. Returns false for semantics we deliberately do NOT
// compress (JOINTS_n / WEIGHTS_n must stay lossless for skinning).
bool dracoAttrForSemantic(const QString& semantic,
                          const MeshDracoEncoder::Options& opts,
                          draco::GeometryAttribute::Type& type, int& bits)
{
    if (semantic == "POSITION")      { type = draco::GeometryAttribute::POSITION;  bits = opts.positionBits; return true; }
    if (semantic == "NORMAL")        { type = draco::GeometryAttribute::NORMAL;    bits = opts.normalBits;   return true; }
    if (semantic.startsWith("TEXCOORD")) { type = draco::GeometryAttribute::TEX_COORD; bits = opts.texCoordBits; return true; }
    if (semantic.startsWith("COLOR"))    { type = draco::GeometryAttribute::COLOR;     bits = opts.colorBits;    return true; }
    if (semantic == "TANGENT")       { type = draco::GeometryAttribute::GENERIC;   bits = opts.genericBits;  return true; }
    // JOINTS_n / WEIGHTS_n / anything else — leave uncompressed.
    return false;
}

struct CompressedPrimitive {
    QByteArray dracoBlob;
    // semantic -> draco attribute unique id, plus the semantic's accessor idx
    QMap<QString, int> attrUniqueIds;
    QStringList semanticsInOrder;
    QMap<QString, int> semanticAccessor;
    int indicesAccessor = -1;
    int decodedPointCount = 0; // draco point count after dedup
};

// Build a Draco mesh for one primitive and encode it. `compressible` lists the
// attribute semantics (in the primitive) we will fold into Draco.
bool encodePrimitive(const GltfContainer& c, const QJsonObject& prim,
                     const MeshDracoEncoder::Options& opts,
                     CompressedPrimitive& outCp, QString& err)
{
    // Require indexed triangles (mode 4 or unspecified default 4).
    int mode = prim.value("mode").toInt(4);
    if (mode != 4) { err = "primitive is not triangles (mode != 4)"; return false; }
    if (!prim.contains("indices")) { err = "primitive has no indices"; return false; }
    int indicesAcc = prim.value("indices").toInt();

    std::vector<uint32_t> indices;
    if (!readIndices(c, indicesAcc, indices, err)) return false;
    if (indices.size() % 3 != 0) { err = "index count not a multiple of 3"; return false; }
    const int numFaces = static_cast<int>(indices.size() / 3);
    if (numFaces == 0) { err = "primitive has zero faces"; return false; }

    QJsonObject attrs = prim.value("attributes").toObject();
    if (!attrs.contains("POSITION")) { err = "primitive has no POSITION"; return false; }

    // Compression must be ALL-OR-NOTHING per primitive. Draco's mesh builder
    // reorders + deduplicates points, so ANY per-vertex stream that is NOT
    // folded into the Draco buffer would be left in the original vertex order
    // and its accessor would then be addressed by the reordered Draco indices
    // — silently corrupting skin weights (JOINTS_n/WEIGHTS_n), normalized
    // integer colours, and morph-target deltas on rigged/animated meshes.
    // So if the primitive carries ANY attribute we cannot represent losslessly
    // in Draco, we refuse to compress the whole primitive (it is left intact).
    //
    //  - JOINTS_n / WEIGHTS_n: skinning must stay bit-exact; not compressed.
    //  - Non-FLOAT attributes (e.g. normalized-integer COLOR_0): Draco stores
    //    floats, which would require rewriting the accessor's componentType/
    //    normalized flags to stay correct — out of scope, so refuse.
    //  - Morph targets (prim.targets): each target is a parallel per-vertex
    //    accessor array keyed by the same vertex order; Draco reordering breaks
    //    the correspondence, so refuse.
    if (prim.contains("targets") && !prim.value("targets").toArray().isEmpty()) {
        err = "primitive has morph targets (skipped to avoid corrupting them)";
        return false;
    }

    QJsonArray allAccessors = c.json.value("accessors").toArray();
    QStringList semantics = attrs.keys();
    for (const QString& sem : semantics) {
        draco::GeometryAttribute::Type dtype;
        int bits;
        if (!dracoAttrForSemantic(sem, opts, dtype, bits)) {
            err = QString("primitive has an attribute Draco can't fold in "
                          "losslessly (%1); skipped").arg(sem);
            return false;
        }
        int accIdx = attrs.value(sem).toInt();
        if (accIdx < 0 || accIdx >= allAccessors.size()) {
            err = "attribute accessor index out of range"; return false;
        }
        int ct = allAccessors.at(accIdx).toObject().value("componentType").toInt();
        if (ct != GLTF_FLOAT) {
            err = QString("primitive has a non-FLOAT attribute (%1); skipped "
                          "to avoid corrupting its normalization").arg(sem);
            return false;
        }
    }

    draco::TriangleSoupMeshBuilder builder;
    builder.Start(numFaces);

    // Deterministic attribute order (POSITION first — keeps ids stable).
    semantics.removeAll("POSITION");
    semantics.sort();
    semantics.prepend("POSITION");

    struct AttrPlan {
        QString semantic;
        int accessorIndex;
        int dracoAttId;
        int numComponents;
        int elementCount;   // accessor.count — guards index-vs-attr bounds
        std::vector<float> values;
        uint32_t uniqueId;
        draco::GeometryAttribute::Type dracoType; // captured once, reused for quantization
        int quantBits;
    };
    std::vector<AttrPlan> plans;
    uint32_t nextUniqueId = 0;

    for (const QString& sem : semantics) {
        // Initialize defensively: dracoAttrForSemantic only writes dtype/bits on
        // success. Every semantic here already passed the eligibility check
        // above, but guard the return anyway so dtype is never used uninit'd.
        draco::GeometryAttribute::Type dtype = draco::GeometryAttribute::GENERIC;
        int bits = opts.genericBits;
        if (!dracoAttrForSemantic(sem, opts, dtype, bits)) {
            err = "internal: attribute became ineligible during encode"; // unreachable
            return false;
        }
        int accIdx = attrs.value(sem).toInt();
        AttrPlan plan;
        plan.semantic = sem;
        plan.accessorIndex = accIdx;
        plan.dracoType = dtype;
        plan.quantBits = bits;
        int nc = 0;
        if (!readAttributeAsFloat(c, accIdx, plan.values, nc, err)) return false;
        plan.numComponents = nc;
        plan.elementCount = (nc > 0) ? static_cast<int>(plan.values.size() / nc) : 0;
        plan.uniqueId = nextUniqueId++;
        plan.dracoAttId = builder.AddAttribute(dtype,
                              static_cast<int8_t>(nc), draco::DT_FLOAT32);
        builder.SetAttributeUniqueId(plan.dracoAttId, plan.uniqueId);
        plans.push_back(std::move(plan));
    }

    // Fill per-face corner values from the index buffer. Every index must be
    // within the attribute's element count — a glTF from a third-party tool
    // can carry indices that disagree with the accessor count; dereferencing
    // past plan.values would be a heap over-read.
    for (int f = 0; f < numFaces; ++f) {
        uint32_t i0 = indices[f * 3 + 0];
        uint32_t i1 = indices[f * 3 + 1];
        uint32_t i2 = indices[f * 3 + 2];
        for (const AttrPlan& plan : plans) {
            const int nc = plan.numComponents;
            if (i0 >= static_cast<uint32_t>(plan.elementCount) ||
                i1 >= static_cast<uint32_t>(plan.elementCount) ||
                i2 >= static_cast<uint32_t>(plan.elementCount)) {
                err = QString("index out of range for attribute %1 "
                              "(index >= count %2)").arg(plan.semantic).arg(plan.elementCount);
                return false;
            }
            const float* base = plan.values.data();
            const float* v0 = base + static_cast<size_t>(i0) * nc;
            const float* v1 = base + static_cast<size_t>(i1) * nc;
            const float* v2 = base + static_cast<size_t>(i2) * nc;
            builder.SetAttributeValuesForFace(plan.dracoAttId,
                                              draco::FaceIndex(f), v0, v1, v2);
        }
    }

    std::unique_ptr<draco::Mesh> mesh = builder.Finalize();
    if (!mesh) { err = "Draco mesh build failed"; return false; }

    draco::Encoder encoder;
    for (const AttrPlan& plan : plans) {
        // Reuse the type/bits captured when the plan was built (no second
        // lookup, no uninitialized-value path).
        if (plan.quantBits > 0)
            encoder.SetAttributeQuantization(plan.dracoType, plan.quantBits);
    }
    encoder.SetSpeedOptions(opts.encodeSpeed, opts.decodeSpeed);

    draco::EncoderBuffer buffer;
    const draco::Status status = encoder.EncodeMeshToBuffer(*mesh, &buffer);
    if (!status.ok()) { err = QString("Draco encode failed: %1").arg(status.error_msg()); return false; }

    outCp.dracoBlob = QByteArray(buffer.data(), static_cast<int>(buffer.size()));
    outCp.indicesAccessor = indicesAcc;
    outCp.decodedPointCount = static_cast<int>(mesh->num_points());
    for (const AttrPlan& plan : plans) {
        outCp.attrUniqueIds.insert(plan.semantic, static_cast<int>(plan.uniqueId));
        outCp.semanticAccessor.insert(plan.semantic, plan.accessorIndex);
        outCp.semanticsInOrder.append(plan.semantic);
    }
    return true;
}

// 4-byte align helper for glb chunk padding.
QByteArray padTo4(const QByteArray& in, char pad)
{
    QByteArray out = in;
    while (out.size() % 4 != 0) out.append(pad);
    return out;
}

void writeU32(QByteArray& b, quint32 v)
{
    char tmp[4];
    qToLittleEndian<quint32>(v, tmp);
    b.append(tmp, 4);
}

QByteArray buildGlb(const QJsonObject& json, const QByteArray& bin)
{
    QByteArray jsonChunk = QJsonDocument(json).toJson(QJsonDocument::Compact);
    jsonChunk = padTo4(jsonChunk, ' ');
    QByteArray binChunk = padTo4(bin, '\0');

    quint32 total = 12 + 8 + jsonChunk.size() + 8 + binChunk.size();
    QByteArray glb;
    writeU32(glb, GLB_MAGIC);
    writeU32(glb, 2); // version
    writeU32(glb, total);
    writeU32(glb, jsonChunk.size());
    writeU32(glb, GLB_CHUNK_JSON);
    glb.append(jsonChunk);
    writeU32(glb, binChunk.size());
    writeU32(glb, GLB_CHUNK_BIN);
    glb.append(binChunk);
    return glb;
}

} // namespace

MeshDracoEncoder::Result MeshDracoEncoder::compressFile(const QString& path, const Options& opts)
{
    Result r;
    QFileInfo fi(path);
    r.originalFileBytes = fi.size();

    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) { r.error = "cannot open input: " + path; return r; }
    QByteArray bytes = in.readAll();
    in.close();

    const QString suffix = fi.suffix().toLower();
    const bool wantGlb = (suffix == "glb" || suffix == "glb2" || suffix == "vrm");

    GltfContainer c;
    QString err;
    if (bytes.startsWith("glTF")) {
        if (!parseGlb(bytes, c, err)) { r.error = err; return r; }
    } else {
        QJsonParseError perr;
        QJsonDocument doc = QJsonDocument::fromJson(bytes, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
            r.error = "glTF JSON parse failed: " + perr.errorString();
            return r;
        }
        c.json = doc.object();
        c.isGlb = false;
        if (!resolveGltfBuffer(c.json, fi.absolutePath(), c.bin, err)) { r.error = err; return r; }
    }

    QJsonArray meshes = c.json.value("meshes").toArray();
    if (meshes.isEmpty()) { r.error = "glTF has no meshes"; return r; }

    // Encode every eligible primitive, collecting new draco bufferViews.
    QByteArray newBin = c.bin;                 // preserve existing data verbatim
    QJsonArray bufferViews = c.json.value("bufferViews").toArray();
    QJsonArray accessors = c.json.value("accessors").toArray();

    // Track accessors that become "compressed" (must drop their bufferView).
    QSet<int> compressedAccessors;
    // Attribute accessors whose element count must be rewritten to the Draco
    // decoded point count (Draco may merge duplicate points, so count can
    // shrink; a stale count makes validators/consumers reject the file).
    QMap<int, int> attrAccessorNewCount;

    for (int m = 0; m < meshes.size(); ++m) {
        QJsonObject mesh = meshes.at(m).toObject();
        QJsonArray prims = mesh.value("primitives").toArray();
        for (int p = 0; p < prims.size(); ++p) {
            r.primitivesTotal++;
            QJsonObject prim = prims.at(p).toObject();

            CompressedPrimitive cp;
            QString perr;
            if (!encodePrimitive(c, prim, opts, cp, perr)) {
                // Skip this primitive (leave it uncompressed) but keep going.
                continue;
            }

            // Append the draco blob 4-byte aligned; record its bufferView.
            while (newBin.size() % 4 != 0) newBin.append('\0');
            int blobOffset = newBin.size();
            newBin.append(cp.dracoBlob);
            r.compressedBinBytes += cp.dracoBlob.size();

            QJsonObject dracoBv;
            dracoBv.insert("buffer", 0);
            dracoBv.insert("byteOffset", blobOffset);
            dracoBv.insert("byteLength", cp.dracoBlob.size());
            int dracoBvIndex = bufferViews.size();
            bufferViews.append(dracoBv);

            // Build the extension object on the primitive.
            QJsonObject dracoExt;
            dracoExt.insert("bufferView", dracoBvIndex);
            QJsonObject extAttrs;
            for (const QString& sem : cp.semanticsInOrder) {
                extAttrs.insert(sem, cp.attrUniqueIds.value(sem));
                int accIdx = cp.semanticAccessor.value(sem);
                compressedAccessors.insert(accIdx);
                // Attribute accessors adopt the Draco point count.
                attrAccessorNewCount.insert(accIdx, cp.decodedPointCount);
            }
            dracoExt.insert("attributes", extAttrs);

            QJsonObject primExt = prim.value("extensions").toObject();
            primExt.insert("KHR_draco_mesh_compression", dracoExt);
            prim.insert("extensions", primExt);
            compressedAccessors.insert(cp.indicesAccessor);

            prims.replace(p, prim);
            r.primitivesCompressed++;
        }
        mesh.insert("primitives", prims);
        meshes.replace(m, mesh);
    }

    if (r.primitivesCompressed == 0) {
        r.nothingEligible = true;
        r.error = "no primitive was eligible for Draco compression — every "
                  "primitive is skinned (JOINTS/WEIGHTS), has morph targets, "
                  "uses a non-FLOAT attribute, or is not an indexed triangle "
                  "mesh; these are left uncompressed to avoid corrupting them";
        return r;
    }

    // Per glTF spec, a Draco-compressed accessor keeps its metadata (count,
    // type, componentType, min/max) but MUST NOT reference a bufferView — the
    // decoder fills it from the Draco stream. Strip the bufferView + byteOffset.
    // Also tally the original geometry byte size for the report.
    for (int a = 0; a < accessors.size(); ++a) {
        if (!compressedAccessors.contains(a)) continue;
        QJsonObject acc = accessors.at(a).toObject();
        int count = acc.value("count").toInt();
        int nc = typeComponentCount(acc.value("type").toString());
        int cs = componentByteSize(acc.value("componentType").toInt());
        r.originalBinBytes += static_cast<qint64>(count) * nc * cs;
        acc.remove("bufferView");
        acc.remove("byteOffset");
        // Attribute accessors adopt the Draco decoded point count (indices
        // accessors are not in this map and keep their triangle-index count).
        if (attrAccessorNewCount.contains(a))
            acc.insert("count", attrAccessorNewCount.value(a));
        accessors.replace(a, acc);
    }

    // ---- Garbage-collect orphaned bufferViews -----------------------------
    // The original geometry bufferViews the compressed accessors used to point
    // at are now dead weight — without removing them the file would GROW
    // instead of shrink (the draco blob is added on top). Rebuild the binary
    // buffer keeping only LIVE bufferViews, then remap every bufferView index.
    //
    // To be conservative against consumers we don't model (EXT_meshopt_
    // compression, EXT_structural_metadata, vendor extensions on any node), the
    // live set is built by a GENERIC recursive walk of the whole JSON that
    // collects EVERY integer under a "bufferView" key — the accessors + Draco
    // extensions we just rewrote already carry their references, and anything
    // else that names a bufferView is preserved automatically. Only bufferViews
    // no reference points at (the stripped geometry) are dropped.
    {
        // CRITICAL: sync the just-rewritten accessors/meshes back into c.json
        // BEFORE walking it. c.json still holds the PRE-strip copies (they're
        // otherwise only re-inserted at the end); walking those stale copies
        // would see the original geometry bufferView references and keep every
        // orphan alive — the file would then grow instead of shrink.
        c.json.insert("accessors", accessors);
        c.json.insert("meshes", meshes);

        QSet<int> live;
        std::function<void(const QJsonValue&)> walk = [&](const QJsonValue& v) {
            if (v.isObject()) {
                const QJsonObject o = v.toObject();
                for (auto it = o.begin(); it != o.end(); ++it) {
                    if (it.key() == QLatin1String("bufferView") && it.value().isDouble()) {
                        int bv = it.value().toInt(-1);
                        if (bv >= 0 && bv < bufferViews.size()) live.insert(bv);
                    }
                    walk(it.value());
                }
            } else if (v.isArray()) {
                for (const QJsonValue& e : v.toArray()) walk(e);
            }
        };
        walk(c.json);

        // Build the compacted buffer + old->new bufferView index map.
        QByteArray compact;
        QJsonArray newBufferViews;
        QMap<int, int> remap;
        for (int i = 0; i < bufferViews.size(); ++i) {
            if (!live.contains(i)) continue;
            QJsonObject bv = bufferViews.at(i).toObject();
            int off = bv.value("byteOffset").toInt(0);
            int len = bv.value("byteLength").toInt(0);
            // Bound the copy against the source buffer — byteOffset/byteLength
            // come straight from JSON and a malformed file could point past the
            // end (QByteArray::append does not clamp → heap over-read).
            if (off < 0 || len < 0 || static_cast<qint64>(off) + len > newBin.size()) {
                r.error = QString("bufferView %1 is out of bounds "
                                  "(offset %2 + length %3 > buffer %4)")
                              .arg(i).arg(off).arg(len).arg(newBin.size());
                return r;
            }
            // 4-byte align each view's start (glb + accessor alignment rules).
            while (compact.size() % 4 != 0) compact.append('\0');
            int newOff = compact.size();
            compact.append(newBin.constData() + off, len);
            bv.insert("byteOffset", newOff);
            remap.insert(i, newBufferViews.size());
            newBufferViews.append(bv);
        }

        // Rewrite every bufferView reference through the remap.
        for (int a = 0; a < accessors.size(); ++a) {
            QJsonObject acc = accessors.at(a).toObject();
            bool changed = false;
            if (acc.contains("bufferView")) {
                acc.insert("bufferView", remap.value(acc.value("bufferView").toInt()));
                changed = true;
            }
            if (acc.contains("sparse")) {
                QJsonObject sp = acc.value("sparse").toObject();
                QJsonObject si = sp.value("indices").toObject();
                si.insert("bufferView", remap.value(si.value("bufferView").toInt()));
                sp.insert("indices", si);
                QJsonObject sv = sp.value("values").toObject();
                sv.insert("bufferView", remap.value(sv.value("bufferView").toInt()));
                sp.insert("values", sv);
                acc.insert("sparse", sp);
                changed = true;
            }
            if (changed) accessors.replace(a, acc);
        }
        {
            QJsonArray imgs = c.json.value("images").toArray();
            bool anyImg = false;
            for (int i = 0; i < imgs.size(); ++i) {
                QJsonObject im = imgs.at(i).toObject();
                if (im.contains("bufferView")) {
                    im.insert("bufferView", remap.value(im.value("bufferView").toInt()));
                    imgs.replace(i, im);
                    anyImg = true;
                }
            }
            if (anyImg) c.json.insert("images", imgs);
        }
        for (int m = 0; m < meshes.size(); ++m) {
            QJsonObject mesh = meshes.at(m).toObject();
            QJsonArray prims = mesh.value("primitives").toArray();
            bool anyPrim = false;
            for (int p = 0; p < prims.size(); ++p) {
                QJsonObject prim = prims.at(p).toObject();
                QJsonObject exts = prim.value("extensions").toObject();
                if (exts.contains("KHR_draco_mesh_compression")) {
                    QJsonObject de = exts.value("KHR_draco_mesh_compression").toObject();
                    de.insert("bufferView", remap.value(de.value("bufferView").toInt()));
                    exts.insert("KHR_draco_mesh_compression", de);
                    prim.insert("extensions", exts);
                    prims.replace(p, prim);
                    anyPrim = true;
                }
            }
            if (anyPrim) { mesh.insert("primitives", prims); meshes.replace(m, mesh); }
        }

        bufferViews = newBufferViews;
        newBin = compact;
    }

    // Add KHR_draco_mesh_compression to extensionsUsed + extensionsRequired.
    auto addExtension = [](QJsonObject& root, const char* key) {
        QJsonArray arr = root.value(key).toArray();
        for (const auto& v : arr) if (v.toString() == "KHR_draco_mesh_compression") return;
        arr.append("KHR_draco_mesh_compression");
        root.insert(key, arr);
    };
    addExtension(c.json, "extensionsUsed");
    addExtension(c.json, "extensionsRequired");

    // Update buffer 0 length to the (possibly grown) binary.
    QJsonArray buffers = c.json.value("buffers").toArray();
    QJsonObject buf0 = buffers.isEmpty() ? QJsonObject() : buffers.at(0).toObject();
    buf0.insert("byteLength", newBin.size());
    if (!wantGlb) {
        // Embed the binary as a base64 data URI so .gltf stays self-contained.
        buf0.insert("uri", QString("data:application/octet-stream;base64,")
                    + QString::fromLatin1(newBin.toBase64()));
    } else {
        buf0.remove("uri"); // glb stores the buffer in the BIN chunk
    }
    if (buffers.isEmpty()) buffers.append(buf0); else buffers.replace(0, buf0);
    c.json.insert("buffers", buffers);
    c.json.insert("bufferViews", bufferViews);
    c.json.insert("accessors", accessors);
    c.json.insert("meshes", meshes);

    // Serialize + write back.
    QByteArray outBytes;
    if (wantGlb) {
        outBytes = buildGlb(c.json, newBin);
    } else {
        outBytes = QJsonDocument(c.json).toJson(QJsonDocument::Indented);
    }

    // Atomic write: write to a sibling temp file, verify the full byte count
    // and flush, then replace the target. `compressFile` is called on a file
    // the exporter has already written, so a short write or a crash must NOT
    // leave a truncated file where the valid uncompressed export was.
    const QString tmpPath = path + ".draco.tmp";
    {
        QFile tmp(tmpPath);
        if (!tmp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            r.error = "cannot open temp file for writing: " + tmpPath;
            return r;
        }
        const qint64 written = tmp.write(outBytes);
        const bool flushed = tmp.flush();
        tmp.close();
        if (written != outBytes.size() || !flushed) {
            QFile::remove(tmpPath);
            r.error = QString("short write to %1 (%2 of %3 bytes)")
                          .arg(tmpPath).arg(written).arg(outBytes.size());
            return r;
        }
    }
    // QFile::rename won't overwrite an existing target — remove then rename.
    if (!QFile::remove(path)) {
        QFile::remove(tmpPath);
        r.error = "cannot replace output file: " + path;
        return r;
    }
    if (!QFile::rename(tmpPath, path)) {
        r.error = "cannot rename temp file into place: " + tmpPath;
        return r;
    }

    r.outputFileBytes = outBytes.size();
    r.ok = true;
    return r;
}

#endif // ENABLE_DRACO
