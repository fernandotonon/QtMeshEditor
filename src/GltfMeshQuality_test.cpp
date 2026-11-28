// File-level tests for glTF / GLB mesh-buffer quality (#678): synthetic
// documents written to a temp dir, measured, fixed, and read back.

#include "GltfMeshQuality.h"

#include <gtest/gtest.h>

#include <type_traits>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <random>
#include <set>
#include <tuple>
#include <vector>

namespace {

constexpr int kUByte = 5121;
constexpr int kUShort = 5123;
constexpr int kUInt = 5125;
constexpr int kFloat = 5126;

// Minimal glTF writer: one binary buffer, one bufferView per call.
struct Doc {
    QByteArray bin;
    QJsonArray views;
    QJsonArray accessors;
    QJsonArray meshes;

    int view(const QByteArray& data, int stride = 0)
    {
        while (bin.size() % 4)
            bin.append('\0');
        QJsonObject v{{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", data.size()}};
        if (stride)
            v.insert("byteStride", stride);
        bin.append(data);
        views.append(v);
        return views.size() - 1;
    }
    int accessor(int view, int compType, const QString& type, int count, int byteOffset = 0)
    {
        QJsonObject a{{"bufferView", view}, {"componentType", compType}, {"type", type}, {"count", count}};
        if (byteOffset)
            a.insert("byteOffset", byteOffset);
        accessors.append(a);
        return accessors.size() - 1;
    }
    int floats(const std::vector<float>& v, const QString& type, int comps, bool minMax = false)
    {
        const int idx = accessor(view(QByteArray(reinterpret_cast<const char*>(v.data()), int(v.size() * 4))),
                                 kFloat, type, int(v.size()) / comps);
        if (minMax) {
            QJsonArray mn, mx;
            for (int c = 0; c < comps; ++c) {
                float lo = v[size_t(c)], hi = v[size_t(c)];
                for (size_t i = size_t(c); i < v.size(); i += size_t(comps)) {
                    lo = std::min(lo, v[i]);
                    hi = std::max(hi, v[i]);
                }
                mn.append(double(lo));
                mx.append(double(hi));
            }
            QJsonObject a = accessors.last().toObject();
            a.insert("min", mn);
            a.insert("max", mx);
            accessors.replace(idx, a);
        }
        return idx;
    }
    int indices(const std::vector<uint32_t>& idx, int compType)
    {
        QByteArray data;
        for (uint32_t i : idx) {
            if (compType == kUByte) data.append(char(i));
            else if (compType == kUShort) { quint16 v = quint16(i); data.append(reinterpret_cast<const char*>(&v), 2); }
            else data.append(reinterpret_cast<const char*>(&i), 4);
        }
        return accessor(view(data), compType, "SCALAR", int(idx.size()));
    }
    void primitive(QJsonObject prim, const QString& name = QString())
    {
        meshes.append(QJsonObject{{"name", name}, {"primitives", QJsonArray{prim}}});
    }
    QJsonObject json() const
    {
        QJsonArray nodes;
        for (int i = 0; i < meshes.size(); ++i)
            nodes.append(QJsonObject{{"mesh", i}});
        return QJsonObject{{"asset", QJsonObject{{"version", "2.0"}}},
                           {"buffers", QJsonArray{QJsonObject{{"byteLength", bin.size()}}}},
                           {"bufferViews", views}, {"accessors", accessors}, {"meshes", meshes},
                           {"nodes", nodes},
                           {"scenes", QJsonArray{QJsonObject{{"nodes", QJsonArray{0}}}}}};
    }
};

void appendU32(QByteArray& b, quint32 v)
{
    char t[4];
    qToLittleEndian<quint32>(v, t);
    b.append(t, 4);
}

QString writeGlb(const QString& path, const QJsonObject& json, QByteArray bin)
{
    QByteArray js = QJsonDocument(json).toJson(QJsonDocument::Compact);
    while (js.size() % 4) js.append(' ');
    while (bin.size() % 4) bin.append('\0');
    QByteArray out;
    appendU32(out, 0x46546C67);
    appendU32(out, 2);
    appendU32(out, quint32(12 + 8 + js.size() + 8 + bin.size()));
    appendU32(out, quint32(js.size()));
    appendU32(out, 0x4E4F534A);
    out.append(js);
    appendU32(out, quint32(bin.size()));
    appendU32(out, 0x004E4942);
    out.append(bin);
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(out);
    return path;
}

QString writeGltfExternal(const QString& dir, const QJsonObject& jsonIn, const QByteArray& bin)
{
    QJsonObject json = jsonIn;
    QJsonArray buffers = json.value("buffers").toArray();
    QJsonObject b0 = buffers.at(0).toObject();
    b0.insert("uri", "mesh data.bin");
    buffers.replace(0, b0);
    json.insert("buffers", buffers);
    QFile bf(dir + "/mesh data.bin");
    EXPECT_TRUE(bf.open(QIODevice::WriteOnly));
    bf.write(bin);
    QFile f(dir + "/mesh.gltf");
    EXPECT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(QJsonDocument(json).toJson());
    return dir + "/mesh.gltf";
}

// Read back a GLB: JSON + BIN.
std::pair<QJsonObject, QByteArray> readGlb(const QString& path)
{
    QFile f(path);
    EXPECT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray b = f.readAll();
    const auto* p = reinterpret_cast<const uchar*>(b.constData());
    const int jl = int(qFromLittleEndian<quint32>(p + 12));
    const QJsonObject json = QJsonDocument::fromJson(b.mid(20, jl)).object();
    QByteArray bin;
    if (b.size() > 20 + jl + 8) {
        const int bl = int(qFromLittleEndian<quint32>(p + 20 + jl));
        bin = b.mid(28 + jl, bl);
    }
    return {json, bin};
}

// Elements of an accessor as raw byte strings.
std::vector<QByteArray> elements(const QJsonObject& json, const QByteArray& bin, int accessor)
{
    const QJsonObject a = json["accessors"].toArray()[accessor].toObject();
    const QJsonObject v = json["bufferViews"].toArray()[a["bufferView"].toInt()].toObject();
    const int comps = a["type"].toString() == "SCALAR" ? 1 : a["type"].toString().back().digitValue();
    const int cs = a["componentType"].toInt() == kUByte ? 1 : a["componentType"].toInt() == kUShort ? 2 : 4;
    const int elem = comps * cs;
    const int stride = v["byteStride"].toInt(elem);
    const int base = v["byteOffset"].toInt() + a["byteOffset"].toInt();
    std::vector<QByteArray> out;
    for (int i = 0; i < a["count"].toInt(); ++i)
        out.push_back(bin.mid(base + i * stride, elem));
    return out;
}

std::vector<uint32_t> indexValues(const QJsonObject& json, const QByteArray& bin, int accessor)
{
    const int type = json["accessors"].toArray()[accessor].toObject()["componentType"].toInt();
    std::vector<uint32_t> out;
    for (const QByteArray& e : elements(json, bin, accessor)) {
        uint32_t v = 0;
        std::memcpy(&v, e.constData(), size_t(e.size()));
        out.push_back(type == kUByte ? (v & 0xFF) : type == kUShort ? (v & 0xFFFF) : v);
    }
    return out;
}

// Triangles of primitive 0 of mesh m, every corner = all its attribute and
// morph-target bytes, rotation-canonical.
std::multiset<std::array<QByteArray, 3>> triangles(const QJsonObject& json, const QByteArray& bin, int m)
{
    const QJsonObject prim = json["meshes"].toArray()[m].toObject()["primitives"].toArray()[0].toObject();
    std::vector<std::vector<QByteArray>> streams;
    const QJsonObject attrs = prim["attributes"].toObject();
    for (const QString& k : attrs.keys())
        streams.push_back(elements(json, bin, attrs[k].toInt()));
    for (const QJsonValue& t : prim["targets"].toArray())
        for (const QString& k : t.toObject().keys())
            streams.push_back(elements(json, bin, t.toObject()[k].toInt()));
    const auto idx = indexValues(json, bin, prim["indices"].toInt());
    std::multiset<std::array<QByteArray, 3>> out;
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        std::array<QByteArray, 3> c;
        for (int k = 0; k < 3; ++k)
            for (const auto& s : streams)
                c[size_t(k)] += s[idx[t + size_t(k)]];
        const auto r = std::min_element(c.begin(), c.end()) - c.begin();
        std::rotate(c.begin(), c.begin() + r, c.end());
        out.insert(c);
    }
    return out;
}

// `tris` without its repeated-corner triangles (byte-identical corners):
// what a lossless degenerate removal must leave, exactly.
std::multiset<std::array<QByteArray, 3>> withoutRepeatedCorners(std::multiset<std::array<QByteArray, 3>> tris)
{
    for (auto it = tris.begin(); it != tris.end();) {
        const auto& c = *it;
        if (c[0] == c[1] || c[1] == c[2] || c[0] == c[2])
            it = tris.erase(it);
        else
            ++it;
    }
    return tris;
}

// A shuffled n x n grid with POSITION (min/max), TEXCOORD_0, optional JOINTS/WEIGHTS
// and a morph target, plus `dupes` duplicated vertices and one degenerate triangle.
struct GridSpec {
    int n = 20;
    int indexType = kUShort;
    bool skin = false;
    bool morph = false;
    bool degenerate = true;
    bool duplicate = true;
};

QJsonObject gridPrimitive(Doc& d, const GridSpec& s)
{
    std::vector<float> pos, uv, delta;
    std::vector<uint8_t> joints;
    std::vector<float> weights;
    const int side = s.n + 1;
    for (int y = 0; y <= s.n; ++y) {
        for (int x = 0; x <= s.n; ++x) {
            pos.insert(pos.end(), {float(x), float(y), float((x * 7 + y * 3) % 5) * 0.1f});
            uv.insert(uv.end(), {float(x) / float(s.n), float(y) / float(s.n)});
            delta.insert(delta.end(), {0.0f, 0.0f, float(x) * 0.01f});
            joints.insert(joints.end(), {uint8_t(x % 3), uint8_t(y % 2), 0, 0});
            weights.insert(weights.end(), {0.75f, 0.25f, 0.0f, 0.0f});
        }
    }
    std::vector<std::array<uint32_t, 3>> tris;
    for (int y = 0; y < s.n; ++y)
        for (int x = 0; x < s.n; ++x) {
            const uint32_t a = uint32_t(y * side + x), b = a + 1, c = a + uint32_t(side), e = c + 1;
            tris.push_back({a, b, e});
            tris.push_back({a, e, c});
        }
    std::mt19937 rng(3);
    std::shuffle(tris.begin(), tris.end(), rng);
    if (s.duplicate) {
        // Duplicate vertex 0 and point the first triangle using it at the copy.
        const uint32_t copy = uint32_t(pos.size() / 3);
        // Copy first: a range insert from the same vector is undefined.
        auto appendPrefix = [](auto& v, size_t n) {
            const std::decay_t<decltype(v)> head(v.begin(), v.begin() + std::ptrdiff_t(n));
            v.insert(v.end(), head.begin(), head.end());
        };
        appendPrefix(pos, 3);
        appendPrefix(uv, 2);
        appendPrefix(delta, 3);
        appendPrefix(joints, 4);
        appendPrefix(weights, 4);
        bool redirected = false;
        for (auto& t : tris)
            for (auto& v : t)
                if (v == 0 && !redirected) {
                    v = copy;
                    redirected = true;
                }
    }
    if (s.degenerate)
        tris.push_back({1, 1, 2});
    std::vector<uint32_t> idx;
    for (const auto& t : tris)
        idx.insert(idx.end(), t.begin(), t.end());

    QJsonObject attrs{{"POSITION", d.floats(pos, "VEC3", 3, true)}, {"TEXCOORD_0", d.floats(uv, "VEC2", 2)}};
    if (s.skin) {
        const QByteArray jointBytes(reinterpret_cast<const char*>(joints.data()), int(joints.size()));
        attrs.insert("JOINTS_0", d.accessor(d.view(jointBytes), kUByte, "VEC4", int(joints.size() / 4)));
        attrs.insert("WEIGHTS_0", d.floats(weights, "VEC4", 4));
    }
    QJsonObject prim{{"attributes", attrs}, {"indices", d.indices(idx, s.indexType)}, {"mode", 4}};
    if (s.morph)
        prim.insert("targets", QJsonArray{QJsonObject{{"POSITION", d.floats(delta, "VEC3", 3, true)}}});
    return prim;
}

MeshQuality::FixOptions allLossless()
{
    MeshQuality::FixOptions o;
    o.removeDegenerates = o.weld = o.vertexCache = o.overdraw = o.vertexFetch = true;
    return o;
}

} // namespace

TEST(GltfMeshQuality, HandlesGltfAndGlbOnly)
{
    EXPECT_TRUE(GltfMeshQuality::handles("a/b.GLB"));
    EXPECT_TRUE(GltfMeshQuality::handles("x.gltf"));
    EXPECT_FALSE(GltfMeshQuality::handles("x.fbx"));
}

TEST(GltfMeshQuality, AnalyzeReadsTheFileBuffers)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}), "grid");
    const QString path = writeGlb(dir.filePath("grid.glb"), d.json(), d.bin);
    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    ASSERT_EQ(res.primitives.size(), 1);
    const auto& p = res.primitives[0];
    EXPECT_TRUE(p.measured);
    EXPECT_TRUE(p.indexFixable);
    EXPECT_TRUE(p.vertexFixable);
    EXPECT_EQ(p.meshName, "grid");
    EXPECT_EQ(p.metrics.triangles, 20u * 20u * 2u + 1u);
    EXPECT_EQ(p.metrics.degenerateTriangles, 1u);
    EXPECT_EQ(p.metrics.redundantVertices, 1u);
    EXPECT_GT(p.metrics.acmrGainPct(), 20.0);
}

TEST(GltfMeshQuality, FixKeepsEveryNonDegenerateTriangle)
{
    for (int indexType : {kUByte, kUShort, kUInt}) {
        QTemporaryDir dir;
        Doc d;
        GridSpec s;
        s.n = indexType == kUByte ? 9 : 20;   // fits 8-bit indices
        s.indexType = indexType;
        s.skin = true;
        s.morph = true;
        d.primitive(gridPrimitive(d, s));
        const QString path = writeGlb(dir.filePath("grid.glb"), d.json(), d.bin);
        const auto [json0, bin0] = readGlb(path);
        auto before = triangles(json0, bin0, 0);

        const auto rep = GltfMeshQuality::fixFile(path, allLossless());
        ASSERT_TRUE(rep.ok) << rep.error.toStdString();
        EXPECT_EQ(rep.primitivesChanged, 1);
        EXPECT_EQ(rep.degeneratesRemoved, 1u);
        EXPECT_EQ(rep.verticesWelded, 1u);
        EXPECT_LT(rep.bytesAfter, rep.bytesBefore);

        const auto [json1, bin1] = readGlb(path);
        auto after = triangles(json1, bin1, 0);
        // The only triangle gone is the degenerate one; welding the duplicate
        // vertex leaves byte-identical corners.
        ASSERT_EQ(withoutRepeatedCorners(before).size(), before.size() - 1);
        EXPECT_EQ(after, withoutRepeatedCorners(before));

        // The fix leaves nothing for a second pass.
        const auto again = GltfMeshQuality::analyzeFile(path);
        ASSERT_TRUE(again.ok);
        const auto& m = again.primitives[0].metrics;
        EXPECT_EQ(m.degenerateTriangles, 0u);
        EXPECT_EQ(m.redundantVertices, 0u);
        EXPECT_EQ(m.unreferencedVertices, 0u);
        // The overdraw pass deliberately gives back up to ~5% of the cache
        // win (meshoptimizer threshold 1.05), so "optimal" is within that.
        EXPECT_LT(m.acmrGainPct(), 6.0);

        // Index type kept, counts and POSITION bounds consistent.
        const QJsonObject prim = json1["meshes"].toArray()[0].toObject()["primitives"].toArray()[0].toObject();
        const QJsonObject idxAcc = json1["accessors"].toArray()[prim["indices"].toInt()].toObject();
        EXPECT_EQ(idxAcc["componentType"].toInt(), indexType);
        const QJsonObject posAcc = json1["accessors"].toArray()[prim["attributes"].toObject()["POSITION"].toInt()].toObject();
        EXPECT_EQ(posAcc["count"].toInt(), (s.n + 1) * (s.n + 1));
        EXPECT_DOUBLE_EQ(posAcc["max"].toArray()[0].toDouble(), double(s.n));
        const int target = prim["targets"].toArray()[0].toObject()["POSITION"].toInt();
        EXPECT_EQ(json1["accessors"].toArray()[target].toObject()["count"].toInt(), posAcc["count"].toInt());
    }
}

TEST(GltfMeshQuality, SharedVertexBuffersAreOnlyReordered)
{
    QTemporaryDir dir;
    Doc d;
    QJsonObject a = gridPrimitive(d, {});
    QJsonObject b = a;   // same attributes, its own index accessor
    b.insert("indices", d.indices(std::vector<uint32_t>{0, 1, 22, 0, 22, 21}, kUShort));
    d.primitive(a, "a");
    d.primitive(b, "b");
    const QString path = writeGlb(dir.filePath("shared.glb"), d.json(), d.bin);
    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok);
    ASSERT_EQ(res.primitives.size(), 2);
    EXPECT_TRUE(res.primitives[0].indexFixable);
    EXPECT_FALSE(res.primitives[0].vertexFixable);
    EXPECT_TRUE(res.primitives[0].note.contains("shared"));

    const auto [json0, bin0] = readGlb(path);
    const auto beforeB = triangles(json0, bin0, 1);
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.verticesWelded, 0u);   // weld needs a private vertex buffer
    EXPECT_FALSE(rep.notes.isEmpty());
    const auto [json1, bin1] = readGlb(path);
    EXPECT_EQ(triangles(json1, bin1, 1), beforeB);   // the other user is intact
}

TEST(GltfMeshQuality, NgonEncodedPrimitivesAreMeasuredButNotTouched)
{
    QTemporaryDir dir;
    Doc d;
    QJsonObject prim = gridPrimitive(d, {});
    prim.insert("extensions", QJsonObject{{"FB_ngon_encoding", QJsonObject{}}});
    d.primitive(prim);
    const QString path = writeGlb(dir.filePath("ngon.glb"), d.json(), d.bin);
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray original = f.readAll();
    f.close();

    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(res.primitives[0].measured);
    EXPECT_FALSE(res.primitives[0].indexFixable);
    EXPECT_TRUE(res.primitives[0].note.contains("n-gons"));

    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok);
    EXPECT_EQ(rep.primitivesChanged, 0);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), original);
}

TEST(GltfMeshQuality, CompressedOrNonTrianglePrimitivesAreSkipped)
{
    QTemporaryDir dir;
    Doc d;
    QJsonObject draco = gridPrimitive(d, {});
    draco.insert("extensions", QJsonObject{{"KHR_draco_mesh_compression", QJsonObject{}}});
    QJsonObject lines = gridPrimitive(d, {});
    lines.insert("mode", 1);
    d.primitive(draco);
    d.primitive(lines);
    const auto res = GltfMeshQuality::analyzeFile(writeGlb(dir.filePath("skip.glb"), d.json(), d.bin));
    ASSERT_TRUE(res.ok);
    EXPECT_FALSE(res.primitives[0].measured);
    EXPECT_TRUE(res.primitives[0].note.contains("KHR_draco_mesh_compression"));
    EXPECT_FALSE(res.primitives[1].measured);
}

TEST(GltfMeshQuality, DryRunWritesNothing)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    const QString path = writeGlb(dir.filePath("dry.glb"), d.json(), d.bin);
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QByteArray original = f.readAll();
    f.close();
    const auto rep = GltfMeshQuality::fixFile(path, allLossless(), /*dryRun=*/true);
    ASSERT_TRUE(rep.ok);
    EXPECT_EQ(rep.primitivesChanged, 1);
    EXPECT_LT(rep.bytesAfter, rep.bytesBefore);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    EXPECT_EQ(f.readAll(), original);
}

TEST(GltfMeshQuality, GltfWithExternalBufferIsRewritten)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    const QString path = writeGltfExternal(dir.path(), d.json(), d.bin);
    const qint64 binBefore = QFileInfo(dir.filePath("mesh data.bin")).size();
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.primitivesChanged, 1);
    EXPECT_LT(QFileInfo(dir.filePath("mesh data.bin")).size(), binBefore);
    // Still an external buffer (not inlined), and it measures clean.
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::ReadOnly));
    const QJsonObject json = QJsonDocument::fromJson(f.readAll()).object();
    EXPECT_EQ(json["buffers"].toArray()[0].toObject()["uri"].toString(), "mesh data.bin");
    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok);
    EXPECT_EQ(res.primitives[0].metrics.redundantVertices, 0u);
}

TEST(GltfMeshQuality, BrokenFilesReportAnError)
{
    QTemporaryDir dir;
    QFile f(dir.filePath("bad.glb"));
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("glTF\x02\0\0\0garbage");
    f.close();
    EXPECT_FALSE(GltfMeshQuality::analyzeFile(dir.filePath("bad.glb")).ok);
    EXPECT_FALSE(GltfMeshQuality::fixFile(dir.filePath("bad.glb"), allLossless()).ok);
    EXPECT_FALSE(GltfMeshQuality::analyzeFile(dir.filePath("missing.gltf")).ok);
}

namespace {

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

MeshQuality::FixOptions orderOnly()
{
    MeshQuality::FixOptions o;
    o.vertexCache = o.overdraw = true;
    return o;
}

} // namespace

TEST(GltfMeshQuality, BlendedMaterialsKeepTheirTriangleOrder)
{
    for (bool viaVariant : {false, true}) {
        QTemporaryDir dir;
        Doc d;
        GridSpec s;
        s.degenerate = s.duplicate = false;
        QJsonObject prim = gridPrimitive(d, s);
        if (viaVariant) {
            prim.insert("material", 0);
            prim.insert("extensions", QJsonObject{{"KHR_materials_variants", QJsonObject{
                {"mappings", QJsonArray{QJsonObject{{"material", 1}, {"variants", QJsonArray{0}}}}}}}});
        } else {
            prim.insert("material", 1);
        }
        d.primitive(prim);
        QJsonObject json = d.json();
        json.insert("materials", QJsonArray{QJsonObject{{"alphaMode", "OPAQUE"}},
                                            QJsonObject{{"alphaMode", "BLEND"}}});
        const QString path = writeGlb(dir.filePath("glass.glb"), json, d.bin);
        const QByteArray original = fileBytes(path);

        const auto res = GltfMeshQuality::analyzeFile(path);
        ASSERT_TRUE(res.ok);
        EXPECT_TRUE(res.primitives[0].indexFixable);
        EXPECT_FALSE(res.primitives[0].orderFixable);
        EXPECT_TRUE(res.primitives[0].note.contains("alpha-blended"));

        const auto rep = GltfMeshQuality::fixFile(path, orderOnly());
        ASSERT_TRUE(rep.ok);
        EXPECT_EQ(rep.primitivesChanged, 0);
        EXPECT_EQ(fileBytes(path), original);
    }
}

TEST(GltfMeshQuality, SkinnedAndMorphedPrimitivesAreDeformable)
{
    QTemporaryDir dir;
    Doc d;
    GridSpec skinned;
    skinned.skin = true;
    d.primitive(gridPrimitive(d, skinned));
    d.primitive(gridPrimitive(d, {}));
    const auto res = GltfMeshQuality::analyzeFile(writeGlb(dir.filePath("d.glb"), d.json(), d.bin));
    ASSERT_TRUE(res.ok);
    EXPECT_TRUE(res.primitives[0].deformable);
    EXPECT_FALSE(res.primitives[1].deformable);
}

TEST(GltfMeshQuality, AccessorsAliasingTheSameBytesAreNotRewritten)
{
    QTemporaryDir dir;
    Doc d;
    GridSpec s;
    s.degenerate = false;
    const QJsonObject a = gridPrimitive(d, s);
    // b reads the SAME bytes through its own accessor objects: every
    // accessor has one reference, yet a rewrite of a would scramble b.
    QJsonObject battrs;
    const QJsonObject aattrs = a["attributes"].toObject();
    for (auto it = aattrs.begin(); it != aattrs.end(); ++it) {
        d.accessors.append(d.accessors.at(it.value().toInt()));
        battrs.insert(it.key(), d.accessors.size() - 1);
    }
    QJsonObject b{{"attributes", battrs}, {"mode", 4},
                  {"indices", d.indices(std::vector<uint32_t>{0, 1, 22, 0, 22, 21}, kUShort)}};
    d.primitive(a, "a");
    d.primitive(b, "b");
    const QString path = writeGlb(dir.filePath("alias.glb"), d.json(), d.bin);
    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok);
    EXPECT_FALSE(res.primitives[0].vertexFixable);
    EXPECT_TRUE(res.primitives[0].note.contains("overlap"));

    const auto [json0, bin0] = readGlb(path);
    const auto beforeB = triangles(json0, bin0, 1);
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.verticesWelded, 0u);
    const auto [json1, bin1] = readGlb(path);
    EXPECT_EQ(triangles(json1, bin1, 1), beforeB);
}

TEST(GltfMeshQuality, GlbWithExtraChunksIsNotRewritten)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    const QString path = writeGlb(dir.filePath("extra.glb"), d.json(), d.bin);
    QByteArray bytes = fileBytes(path);
    appendU32(bytes, 4);
    appendU32(bytes, 0x54584558);   // an unknown extension chunk
    bytes.append("abcd");
    qToLittleEndian<quint32>(quint32(bytes.size()), bytes.data() + 8);
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
    }
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    EXPECT_FALSE(rep.ok);
    EXPECT_TRUE(rep.error.contains("chunks"));
    EXPECT_EQ(fileBytes(path), bytes);
}

TEST(GltfMeshQuality, ExternalBuffersOutsideTheRootAreNotWritten)
{
    QTemporaryDir dir;
    ASSERT_TRUE(QDir(dir.path()).mkpath("assets"));
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    QJsonObject json = d.json();
    json.insert("buffers", QJsonArray{QJsonObject{{"byteLength", d.bin.size()}, {"uri", "../outside.bin"}}});
    {
        QFile bf(dir.filePath("outside.bin"));
        ASSERT_TRUE(bf.open(QIODevice::WriteOnly));
        bf.write(d.bin);
        QFile f(dir.filePath("assets/mesh.gltf"));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(json).toJson());
    }
    const QString path = dir.filePath("assets/mesh.gltf");
    const QByteArray binBefore = fileBytes(dir.filePath("outside.bin"));
    for (const QString& root : {QString(), dir.filePath("assets")}) {
        const auto rep = GltfMeshQuality::fixFile(path, allLossless(), false, {}, root);
        EXPECT_FALSE(rep.ok);
        EXPECT_TRUE(rep.error.contains("outside"));
        EXPECT_EQ(fileBytes(dir.filePath("outside.bin")), binBefore);
    }
    // Inside a wider root it is allowed.
    EXPECT_TRUE(GltfMeshQuality::fixFile(path, allLossless(), false, {}, dir.path()).ok);
    EXPECT_NE(fileBytes(dir.filePath("outside.bin")), binBefore);
}

TEST(GltfMeshQuality, BuffersSharedByTwoDocumentsAreNotRewritten)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    const QString a = writeGltfExternal(dir.path(), d.json(), d.bin);
    // A second document reading the same .bin, plus a GLB that reads nothing external.
    const QString b = dir.filePath("other.gltf");
    ASSERT_TRUE(QFile::copy(a, b));
    const QString glb = writeGlb(dir.filePath("own.glb"), d.json(), d.bin);

    const QString bin = QFileInfo(dir.filePath("mesh data.bin")).canonicalFilePath();
    EXPECT_EQ(GltfMeshQuality::externalBufferPaths(a), QStringList{bin});
    EXPECT_TRUE(GltfMeshQuality::externalBufferPaths(glb).isEmpty());
    const QSet<QString> shared = GltfMeshQuality::sharedExternalBuffers({a, b, glb, dir.filePath("x.fbx")});
    EXPECT_EQ(shared, QSet<QString>{bin});
    EXPECT_TRUE(GltfMeshQuality::sharedExternalBuffers({a, glb}).isEmpty());
    // The same document listed twice (overlapping scan roots, another spelling
    // of its path) is still one user.
    EXPECT_TRUE(GltfMeshQuality::sharedExternalBuffers(
        {a, a, dir.path() + "/./" + QFileInfo(a).fileName()}).isEmpty());

    const QByteArray before = fileBytes(dir.filePath("mesh data.bin"));
    const auto rep = GltfMeshQuality::fixFile(a, allLossless(), false, {}, QString(), shared);
    EXPECT_FALSE(rep.ok);
    EXPECT_TRUE(rep.error.contains("another glTF")) << rep.error.toStdString();
    EXPECT_EQ(fileBytes(dir.filePath("mesh data.bin")), before);
    // A document not using the shared buffer is still fixed.
    EXPECT_TRUE(GltfMeshQuality::fixFile(glb, allLossless(), false, {}, QString(), shared).ok);
}

TEST(GltfMeshQuality, GlbBufferZeroWithADataUriStaysADataUri)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    QJsonObject json = d.json();
    json.insert("buffers", QJsonArray{QJsonObject{
        {"byteLength", d.bin.size()},
        {"uri", "data:application/octet-stream;base64," + QString::fromLatin1(d.bin.toBase64())}}});
    const QString path = writeGlb(dir.filePath("uri.glb"), json, QByteArray());
    const auto before = triangles(json, d.bin, 0);

    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.primitivesChanged, 1);
    const auto [json1, bin1] = readGlb(path);
    const QString uri = json1["buffers"].toArray()[0].toObject()["uri"].toString();
    ASSERT_TRUE(uri.startsWith("data:"));
    const QByteArray data = QByteArray::fromBase64(uri.mid(uri.indexOf(',') + 1).toLatin1());
    EXPECT_EQ(json1["buffers"].toArray()[0].toObject()["byteLength"].toInt(), data.size());
    // Exactly the degenerate triangle is gone, nothing else lost or duplicated.
    ASSERT_EQ(withoutRepeatedCorners(before).size(), before.size() - 1);
    EXPECT_EQ(triangles(json1, data, 0), withoutRepeatedCorners(before));
}

namespace {

// Four vertices, a real triangle and a triangle that is exactly flat in
// POSITION (0,0,0)-(1,0,0)-(2,0,0); with `morph`, a target lifts vertex 2
// so the flat triangle opens up when the morph is applied.
QJsonObject flatTrianglePrimitive(Doc& d, bool morph)
{
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 2, 0, 0, 0, 1, 0};
    QJsonObject prim{{"attributes", QJsonObject{{"POSITION", d.floats(pos, "VEC3", 3, true)}}},
                     {"indices", d.indices(std::vector<uint32_t>{0, 1, 3, 0, 1, 2}, kUShort)},
                     {"mode", 4}};
    if (morph) {
        const std::vector<float> delta{0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};
        prim.insert("targets", QJsonArray{QJsonObject{{"POSITION", d.floats(delta, "VEC3", 3, true)}}});
    }
    return prim;
}

} // namespace

TEST(GltfMeshQuality, MorphedFlatTrianglesAreKept)
{
    QTemporaryDir dir;
    for (bool morph : {true, false}) {
        Doc d;
        d.primitive(flatTrianglePrimitive(d, morph));
        const QString path = writeGlb(dir.filePath(morph ? "morph.glb" : "static.glb"), d.json(), d.bin);
        const auto res = GltfMeshQuality::analyzeFile(path);
        ASSERT_TRUE(res.ok);
        const auto& pr = res.primitives[0];
        EXPECT_EQ(pr.deformable, morph);   // morph alone, no skin
        EXPECT_EQ(pr.metrics.degenerateTriangles, 1u);
        EXPECT_EQ(pr.metrics.zeroAreaTriangles, 1u);
        EXPECT_EQ(pr.metrics.repeatedCornerTriangles, 0u);

        MeshQuality::FixOptions o;
        o.removeDegenerates = true;
        const auto rep = GltfMeshQuality::fixFile(path, o);
        ASSERT_TRUE(rep.ok) << rep.error.toStdString();
        EXPECT_EQ(rep.degeneratesRemoved, morph ? 0u : 1u);
        const auto [json1, bin1] = readGlb(path);
        EXPECT_EQ(indexValues(json1, bin1, json1["meshes"].toArray()[0].toObject()["primitives"]
                                               .toArray()[0].toObject()["indices"].toInt()).size(),
                  morph ? 6u : 3u);
    }
}

TEST(GltfMeshQuality, MeshoptFallbackBufferDoesNotBlockPlainPrimitives)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    QJsonObject json = d.json();
    const QJsonObject fallback{{"byteLength", 4096},
                               {"extensions", QJsonObject{{"EXT_meshopt_compression",
                                                           QJsonObject{{"fallback", true}}}}}};
    QJsonArray buffers = json["buffers"].toArray();
    buffers.append(fallback);
    json.insert("buffers", buffers);
    const QString path = writeGlb(dir.filePath("fallback.glb"), json, d.bin);

    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok) << res.error.toStdString();
    EXPECT_TRUE(res.primitives[0].indexFixable);
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.primitivesChanged, 1);
    const auto [json1, bin1] = readGlb(path);
    EXPECT_EQ(json1["buffers"].toArray()[1].toObject(), fallback);   // untouched, still no uri
}

TEST(GltfMeshQuality, OutlineExtensionCountsAsAnIndexConsumer)
{
    QTemporaryDir dir;
    Doc d;
    const QJsonObject a = gridPrimitive(d, {});
    QJsonObject b{{"attributes", a["attributes"]}, {"mode", 1}, {"indices", d.indices({0, 1}, kUShort)},
                  {"extensions", QJsonObject{{"CESIUM_primitive_outline",
                                              QJsonObject{{"indices", a["indices"]}}}}}};
    d.primitive(a, "a");
    d.primitive(b, "b");
    const QString path = writeGlb(dir.filePath("outline.glb"), d.json(), d.bin);
    const auto res = GltfMeshQuality::analyzeFile(path);
    ASSERT_TRUE(res.ok);
    EXPECT_FALSE(res.primitives[0].indexFixable);
    const QByteArray original = fileBytes(path);
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok);
    EXPECT_EQ(rep.primitivesChanged, 0);
    EXPECT_EQ(fileBytes(path), original);
}

TEST(GltfMeshQuality, ExistingStagingAndBackupNamesAreLeftAlone)
{
    QTemporaryDir dir;
    Doc d;
    d.primitive(gridPrimitive(d, {}));
    const QString path = writeGlb(dir.filePath("mesh.glb"), d.json(), d.bin);
    for (const QString& stray : {path + ".qtmesh_tmp", path + ".qtmesh_bak"}) {
        QFile f(stray);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("not ours");
    }
    const auto rep = GltfMeshQuality::fixFile(path, allLossless());
    ASSERT_TRUE(rep.ok) << rep.error.toStdString();
    EXPECT_EQ(rep.primitivesChanged, 1);
    EXPECT_EQ(fileBytes(path + ".qtmesh_tmp"), QByteArray("not ours"));
    EXPECT_EQ(fileBytes(path + ".qtmesh_bak"), QByteArray("not ours"));
    // Nothing of our own is left behind either.
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files).size(), 3);
}
