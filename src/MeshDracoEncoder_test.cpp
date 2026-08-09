/*
-----------------------------------------------------------------------------------
A QtMeshEditor file — MeshDracoEncoder unit tests (issue #506)

Copyright (c) Fernando Tonon (https://github.com/fernandotonon)
The MIT License — see other project sources for the full header.
-----------------------------------------------------------------------------------
*/

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>

#include <cstring>
#include <vector>

#include "MeshDracoEncoder.h"

#ifdef ENABLE_DRACO
#include "draco/compression/decode.h"
#include "draco/core/decoder_buffer.h"
#include "draco/mesh/mesh.h"
#endif

namespace {

// Build a flat W×H triangulated grid (in the XY plane) as raw POSITION floats
// + a uint32 triangle index list. Enough triangles that Draco actually beats
// the raw layout, so the "binary chunk shrinks" assertion is meaningful.
struct Grid {
    std::vector<float> positions;   // 3 per vertex
    std::vector<uint32_t> indices;  // 3 per triangle
    int vertexCount = 0;
    int triCount = 0;
};

Grid makeGrid(int w, int h)
{
    Grid g;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            g.positions.push_back(static_cast<float>(x));
            g.positions.push_back(static_cast<float>(y));
            g.positions.push_back(static_cast<float>((x * 7 + y * 13) % 5)); // some Z variation
        }
    auto idx = [w](int x, int y) { return static_cast<uint32_t>(y * w + x); };
    for (int y = 0; y < h - 1; ++y)
        for (int x = 0; x < w - 1; ++x) {
            g.indices.push_back(idx(x, y));
            g.indices.push_back(idx(x + 1, y));
            g.indices.push_back(idx(x + 1, y + 1));
            g.indices.push_back(idx(x, y));
            g.indices.push_back(idx(x + 1, y + 1));
            g.indices.push_back(idx(x, y + 1));
        }
    g.vertexCount = w * h;
    g.triCount = static_cast<int>(g.indices.size() / 3);
    return g;
}

void appendU32(QByteArray& b, quint32 v)
{
    char t[4];
    qToLittleEndian<quint32>(v, t);
    b.append(t, 4);
}

// Hand-assemble a valid single-mesh, single-primitive .glb from a Grid.
QByteArray buildGlb(const Grid& g)
{
    // --- binary buffer: [indices uint32][positions float] ---
    QByteArray bin;
    const int indicesOffset = 0;
    for (uint32_t i : g.indices) appendU32(bin, i);
    while (bin.size() % 4 != 0) bin.append('\0');
    const int posOffset = bin.size();
    for (float f : g.positions) {
        char t[4];
        std::memcpy(t, &f, 4);
        bin.append(t, 4);
    }
    while (bin.size() % 4 != 0) bin.append('\0');

    // min/max for POSITION (required by spec).
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (int v = 0; v < g.vertexCount; ++v)
        for (int c = 0; c < 3; ++c) {
            float val = g.positions[v * 3 + c];
            mn[c] = std::min(mn[c], val);
            mx[c] = std::max(mx[c], val);
        }

    QJsonObject root;
    root.insert("asset", QJsonObject{{"version", "2.0"}});

    QJsonArray buffers;
    buffers.append(QJsonObject{{"byteLength", bin.size()}});
    root.insert("buffers", buffers);

    QJsonArray bufferViews;
    bufferViews.append(QJsonObject{
        {"buffer", 0}, {"byteOffset", indicesOffset},
        {"byteLength", int(g.indices.size() * 4)}, {"target", 34963}}); // ELEMENT_ARRAY
    bufferViews.append(QJsonObject{
        {"buffer", 0}, {"byteOffset", posOffset},
        {"byteLength", int(g.positions.size() * 4)}, {"target", 34962}}); // ARRAY
    root.insert("bufferViews", bufferViews);

    QJsonArray accessors;
    accessors.append(QJsonObject{
        {"bufferView", 0}, {"componentType", 5125}, // UNSIGNED_INT
        {"count", int(g.indices.size())}, {"type", "SCALAR"}});
    accessors.append(QJsonObject{
        {"bufferView", 1}, {"componentType", 5126}, // FLOAT
        {"count", g.vertexCount}, {"type", "VEC3"},
        {"min", QJsonArray{mn[0], mn[1], mn[2]}},
        {"max", QJsonArray{mx[0], mx[1], mx[2]}}});
    root.insert("accessors", accessors);

    QJsonObject prim;
    prim.insert("attributes", QJsonObject{{"POSITION", 1}});
    prim.insert("indices", 0);
    prim.insert("mode", 4);
    QJsonObject mesh;
    mesh.insert("primitives", QJsonArray{prim});
    root.insert("meshes", QJsonArray{mesh});

    root.insert("nodes", QJsonArray{QJsonObject{{"mesh", 0}}});
    root.insert("scenes", QJsonArray{QJsonObject{{"nodes", QJsonArray{0}}}});
    root.insert("scene", 0);

    // --- pack glb ---
    QByteArray jsonChunk = QJsonDocument(root).toJson(QJsonDocument::Compact);
    while (jsonChunk.size() % 4 != 0) jsonChunk.append(' ');
    QByteArray binChunk = bin;
    while (binChunk.size() % 4 != 0) binChunk.append('\0');

    QByteArray glb;
    appendU32(glb, 0x46546C67); // 'glTF'
    appendU32(glb, 2);
    appendU32(glb, 12 + 8 + jsonChunk.size() + 8 + binChunk.size());
    appendU32(glb, jsonChunk.size());
    appendU32(glb, 0x4E4F534A); // JSON
    glb.append(jsonChunk);
    appendU32(glb, binChunk.size());
    appendU32(glb, 0x004E4942); // BIN
    glb.append(binChunk);
    return glb;
}

// Re-parse a glb into (json, bin) for assertions.
bool splitGlb(const QByteArray& bytes, QJsonObject& json, QByteArray& bin)
{
    if (bytes.size() < 12) return false;
    const uchar* p = reinterpret_cast<const uchar*>(bytes.constData());
    quint32 total = qFromLittleEndian<quint32>(p + 8);
    int off = 12;
    QByteArray jsonChunk;
    while (off + 8 <= int(total)) {
        quint32 clen = qFromLittleEndian<quint32>(p + off);
        quint32 ctype = qFromLittleEndian<quint32>(p + off + 4);
        off += 8;
        if (ctype == 0x4E4F534A) jsonChunk = bytes.mid(off, clen);
        else if (ctype == 0x004E4942) bin = bytes.mid(off, clen);
        off += clen;
    }
    json = QJsonDocument::fromJson(jsonChunk).object();
    return !jsonChunk.isEmpty();
}

bool arrayHas(const QJsonArray& arr, const QString& s)
{
    for (const auto& v : arr) if (v.toString() == s) return true;
    return false;
}

} // namespace

// --------------------------------------------------------------------------
// Build-flag sanity: the feature reports its own availability.
// --------------------------------------------------------------------------
TEST(MeshDracoEncoderTest, SupportedMatchesBuildFlag) {
#ifdef ENABLE_DRACO
    EXPECT_TRUE(MeshDracoEncoder::isSupported());
#else
    EXPECT_FALSE(MeshDracoEncoder::isSupported());
#endif
}

#ifndef ENABLE_DRACO
// Without Draco the encoder must fail cleanly with a helpful message.
TEST(MeshDracoEncoderTest, FailsClearlyWhenUnsupported) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("x.glb");
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("glTF", 4); }
    auto r = MeshDracoEncoder::compressFile(path);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.error.contains("ENABLE_DRACO"));
}
#endif

#ifdef ENABLE_DRACO

TEST(MeshDracoEncoderTest, CompressesGlbAndWiresExtension) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("grid.glb");

    Grid g = makeGrid(32, 32); // 1024 verts, 1922 tris
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write(buildGlb(g)); }

    auto r = MeshDracoEncoder::compressFile(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_EQ(r.primitivesTotal, 1);
    EXPECT_EQ(r.primitivesCompressed, 1);

    // Re-read and validate the glTF-level wiring.
    QFile out(path);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    QByteArray outBytes = out.readAll();
    QJsonObject json;
    QByteArray bin;
    ASSERT_TRUE(splitGlb(outBytes, json, bin));

    EXPECT_TRUE(arrayHas(json.value("extensionsUsed").toArray(), "KHR_draco_mesh_compression"));
    EXPECT_TRUE(arrayHas(json.value("extensionsRequired").toArray(), "KHR_draco_mesh_compression"));

    QJsonObject prim = json.value("meshes").toArray().at(0).toObject()
                           .value("primitives").toArray().at(0).toObject();
    QJsonObject ext = prim.value("extensions").toObject()
                          .value("KHR_draco_mesh_compression").toObject();
    ASSERT_FALSE(ext.isEmpty());
    EXPECT_TRUE(ext.contains("bufferView"));
    EXPECT_TRUE(ext.value("attributes").toObject().contains("POSITION"));

    // Per spec, a Draco-compressed accessor keeps count/type but drops bufferView.
    QJsonArray accessors = json.value("accessors").toArray();
    int posAcc = prim.value("attributes").toObject().value("POSITION").toInt();
    EXPECT_FALSE(accessors.at(posAcc).toObject().contains("bufferView"));
    EXPECT_EQ(accessors.at(posAcc).toObject().value("count").toInt(), g.vertexCount);
    int idxAcc = prim.value("indices").toInt();
    EXPECT_FALSE(accessors.at(idxAcc).toObject().contains("bufferView"));
}

TEST(MeshDracoEncoderTest, CompressedGeometryIsSmaller) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("grid.glb");
    Grid g = makeGrid(48, 48);
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write(buildGlb(g)); }

    auto r = MeshDracoEncoder::compressFile(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    // Original geometry = indices + positions bytes; compressed = draco blob.
    EXPECT_GT(r.originalBinBytes, 0);
    EXPECT_GT(r.compressedBinBytes, 0);
    EXPECT_LT(r.compressedBinBytes, r.originalBinBytes)
        << "Draco should shrink the geometry buffer";
}

TEST(MeshDracoEncoderTest, RoundTripPreservesFaceCount) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath("grid.glb");
    Grid g = makeGrid(24, 24);
    const int expectedFaces = g.triCount;
    const int expectedVerts = g.vertexCount;
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write(buildGlb(g)); }

    auto r = MeshDracoEncoder::compressFile(path);
    ASSERT_TRUE(r.ok) << r.error.toStdString();

    // Pull the draco blob straight back out and decode it — the decoded mesh
    // must have the same triangle count (the acceptance criterion), and the
    // point count must match the original vertex count (grid has no shared
    // positions, so Draco dedup keeps them all).
    QFile out(path);
    ASSERT_TRUE(out.open(QIODevice::ReadOnly));
    QByteArray outBytes = out.readAll();
    QJsonObject json;
    QByteArray bin;
    ASSERT_TRUE(splitGlb(outBytes, json, bin));

    QJsonObject prim = json.value("meshes").toArray().at(0).toObject()
                           .value("primitives").toArray().at(0).toObject();
    int bvIdx = prim.value("extensions").toObject()
                    .value("KHR_draco_mesh_compression").toObject()
                    .value("bufferView").toInt();
    QJsonObject bv = json.value("bufferViews").toArray().at(bvIdx).toObject();
    int bo = bv.value("byteOffset").toInt(0);
    int bl = bv.value("byteLength").toInt();
    QByteArray blob = bin.mid(bo, bl);
    ASSERT_EQ(blob.size(), bl);

    draco::DecoderBuffer db;
    db.Init(blob.constData(), blob.size());
    draco::Decoder decoder;
    auto meshOr = decoder.DecodeMeshFromBuffer(&db);
    ASSERT_TRUE(meshOr.ok()) << meshOr.status().error_msg();
    std::unique_ptr<draco::Mesh> mesh = std::move(meshOr).value();
    EXPECT_EQ(static_cast<int>(mesh->num_faces()), expectedFaces);
    EXPECT_EQ(static_cast<int>(mesh->num_points()), expectedVerts);
}

TEST(MeshDracoEncoderTest, FailsOnMissingFile) {
    auto r = MeshDracoEncoder::compressFile("/nonexistent/path/to/file.glb");
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}

#endif // ENABLE_DRACO
