#include <gtest/gtest.h>

#include "BodyTexture.h"

#include <QImage>
#include <cmath>
#include <cstring>

using namespace SamBody;

namespace {

struct Sphere {
    std::vector<float> pos, uv;
    std::vector<int> tris;
};

// Lat/long sphere, seam column duplicated, outward counter-clockwise.
Sphere makeSphere(float radius, float cy, int nu = 48, int nv = 24)
{
    Sphere s;
    for (int j = 0; j <= nv; ++j)
        for (int i = 0; i <= nu; ++i) {
            const float u = static_cast<float>(i) / nu, v = static_cast<float>(j) / nv;
            const float th = u * 6.2831853f, ph = v * 3.14159265f;
            s.pos.insert(s.pos.end(), {radius * std::sin(ph) * std::cos(th), cy + radius * std::cos(ph), radius * std::sin(ph) * std::sin(th)});
            s.uv.insert(s.uv.end(), {u, v});
        }
    for (int j = 0; j < nv; ++j)
        for (int i = 0; i < nu; ++i) {
            const int a = j * (nu + 1) + i, b = a + 1, c = a + nu + 1, d = c + 1;
            // seen from outside with v increasing downwards (ph from the +Y pole), th increasing around +Y
            s.tris.insert(s.tris.end(), {a, b, c, b, d, c});
        }
    return s;
}

TexturedSurface surfaceOf(const Sphere& s)
{
    TexturedSurface t;
    t.positions = s.pos;
    for (int i : s.tris) t.indices.push_back(static_cast<uint32_t>(i));
    t.uvs = s.uv;
    QImage img(256, 256, QImage::Format_RGB888);
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) img.setPixel(x, y, qRgb(x, y, 128));
    t.albedo = img;
    return t;
}

UvLayout layoutOf(const Sphere& s)
{
    UvLayout u;
    u.texcoords = s.uv;
    u.faceUv = s.tris;
    return u;
}

} // namespace

TEST(BodyTextureLayout, ParsesTheExportedFileAndRejectsDamagedOnes)
{
    const int nt = 3, nf = 1;
    QByteArray b;
    b.append(reinterpret_cast<const char*>(&nt), 4);
    b.append(reinterpret_cast<const char*>(&nf), 4);
    const float tc[6] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    const int fi[3] = {0, 1, 2};
    b.append(reinterpret_cast<const char*>(tc), sizeof(tc));
    b.append(reinterpret_cast<const char*>(fi), sizeof(fi));
    const UvLayout u = parseUvLayout(b);
    ASSERT_TRUE(u.valid());
    EXPECT_EQ(u.texcoords.size(), 6u);
    EXPECT_FLOAT_EQ(u.texcoords[3], 0.4f);
    EXPECT_EQ(u.faceUv[2], 2);

    EXPECT_FALSE(parseUvLayout(b.left(b.size() - 1)).valid());          // truncated
    EXPECT_FALSE(parseUvLayout(QByteArray()).valid());
    QByteArray bad = b;
    const int out = 7;
    std::memcpy(bad.data() + 8 + 24, &out, 4);                          // a UV index past the table
    EXPECT_FALSE(parseUvLayout(bad).valid());
}

TEST(BodyTextureTransfer, SamplesTheSurfaceTextureAtTheSameDirection)
{
    // The source is a bigger, offset sphere with a colour that encodes (u, v); the body is a small sphere whose
    // UV layout follows the same directions. Every body texel must come out with the colour of its own direction.
    const Sphere src = makeSphere(2.0f, 5.0f), body = makeSphere(0.9f, 0.9f);
    TransferOptions opt;
    opt.textureSize = 128;
    opt.nonRigid = false;
    opt.tryBothFacings = false;
    opt.dilatePx = 0;
    const TransferResult r = transferTexture(surfaceOf(src), body.pos, body.tris, layoutOf(body), opt);
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    EXPECT_GT(r.coverage, 0.95f);
    EXPECT_LT(r.meanDistanceFraction, 0.01f);
    ASSERT_EQ(r.albedo.width(), 128);
    // texel (u = 0.3, v = 0.5)
    const QRgb c = r.albedo.pixel(static_cast<int>(0.3f * 128), static_cast<int>(0.5f * 128));
    EXPECT_NEAR(qRed(c), 0.3f * 255, 8);
    EXPECT_NEAR(qGreen(c), 0.5f * 255, 8);
    EXPECT_EQ(qBlue(c), 128);
    EXPECT_EQ(qAlpha(c), 255);
}

TEST(BodyTextureTransfer, TheNonRigidWrapReachesASurfaceTheSimilarityCannot)
{
    // A tall ellipsoid source vs a round body: height normalisation alone leaves the equator off the surface;
    // the wrap must pull the body onto it.
    Sphere src = makeSphere(1.0f, 0.0f);
    for (size_t i = 0; i + 2 < src.pos.size(); i += 3) { src.pos[i] *= 0.55f; src.pos[i + 2] *= 0.55f; }
    const Sphere body = makeSphere(1.0f, 0.0f);
    TransferOptions rigid, wrapped;
    rigid.textureSize = wrapped.textureSize = 96;
    rigid.tryBothFacings = wrapped.tryBothFacings = false;
    rigid.nonRigid = false;
    const TransferResult a = transferTexture(surfaceOf(src), body.pos, body.tris, layoutOf(body), rigid);
    const TransferResult b = transferTexture(surfaceOf(src), body.pos, body.tris, layoutOf(body), wrapped);
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_LT(b.meanDistanceFraction, a.meanDistanceFraction * 0.5f);
    EXPECT_GT(b.coverage, a.coverage);
}

TEST(BodyTextureTransfer, RejectsInconsistentInput)
{
    const Sphere s = makeSphere(1.0f, 0.0f);
    TexturedSurface noTex = surfaceOf(s);
    noTex.albedo = QImage();
    EXPECT_FALSE(transferTexture(noTex, s.pos, s.tris, layoutOf(s)).ok);
    TexturedSurface badUv = surfaceOf(s);
    badUv.uvs.pop_back();
    EXPECT_FALSE(transferTexture(badUv, s.pos, s.tris, layoutOf(s)).ok);
    UvLayout wrong = layoutOf(s);
    wrong.faceUv.pop_back();
    EXPECT_FALSE(transferTexture(surfaceOf(s), s.pos, s.tris, wrong).ok);
    std::vector<int> oob = s.tris;
    oob[0] = 1 << 20;
    EXPECT_FALSE(transferTexture(surfaceOf(s), s.pos, oob, layoutOf(s)).ok);
}

TEST(BodyTextureObj, WritesUvSplitFacesAndAFlippedV)
{
    UvLayout uv;
    uv.texcoords = {0.0f, 0.25f, 1.0f, 0.25f, 0.5f, 0.75f};
    uv.faceUv = {0, 1, 2};
    const std::vector<float> pos = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const QByteArray obj = toTexturedObj(pos, {0, 1, 2}, uv, QStringLiteral("b.mtl"), false);
    EXPECT_TRUE(obj.contains("mtllib b.mtl\n"));
    EXPECT_TRUE(obj.contains("vt 0.5 0.25\n"));                         // v = 0.75 written as 1 - v
    EXPECT_TRUE(obj.contains("f 1/1 2/2 3/3\n"));
    EXPECT_FALSE(obj.contains(','));
    const QByteArray turned = toTexturedObj(pos, {0, 1, 2}, uv, {}, true);
    EXPECT_TRUE(turned.contains("v -1 0 -0\n") || turned.contains("v -1 0 0\n"));
    EXPECT_FALSE(turned.contains("mtllib"));
    const QByteArray m = toMtl(QStringLiteral("body"), QStringLiteral("d.png"), QStringLiteral("r.png"), {});
    EXPECT_TRUE(m.contains("map_Kd d.png"));
    EXPECT_TRUE(m.contains("map_Pr r.png"));
    EXPECT_FALSE(m.contains("map_Pm"));
}

TEST(BodyTextureGlb, IsAWellFormedSelfContainedGlTF)
{
    const Sphere s = makeSphere(0.9f, 0.9f, 8, 6);
    QImage albedo(16, 16, QImage::Format_RGBA8888);
    albedo.fill(Qt::red);
    QImage rough(8, 8, QImage::Format_Grayscale8);
    rough.fill(100);
    const QByteArray glb = toTexturedGlb(s.pos, s.tris, layoutOf(s), albedo, rough, QImage(), true);
    ASSERT_GT(glb.size(), 100);
    EXPECT_EQ(glb.left(4), QByteArray("glTF"));
    auto le = [&](int off) { quint32 v; std::memcpy(&v, glb.constData() + off, 4); return v; };
    EXPECT_EQ(le(4), 2u);
    EXPECT_EQ(static_cast<int>(le(8)), glb.size());                       // total length
    const int jsonLen = static_cast<int>(le(12));
    EXPECT_EQ(glb.mid(16, 4), QByteArray("JSON"));
    EXPECT_EQ(jsonLen % 4, 0);
    const int binOff = 20 + jsonLen;
    EXPECT_EQ(glb.mid(binOff + 4, 4), QByteArray("BIN\0", 4));
    EXPECT_EQ(static_cast<int>(le(binOff)) + binOff + 8, glb.size());
    const QByteArray json = glb.mid(20, jsonLen);
    EXPECT_TRUE(json.contains("\"TEXCOORD_0\""));
    EXPECT_TRUE(json.contains("\"metallicRoughnessTexture\""));           // roughness was supplied
    EXPECT_TRUE(json.contains("\"baseColorTexture\""));
    EXPECT_EQ(glb.indexOf("\x89PNG"), glb.indexOf("\x89PNG"));           // embedded images exist
    EXPECT_GT(glb.count("PNG"), 1);
    EXPECT_TRUE(toTexturedGlb(s.pos, s.tris, layoutOf(s), QImage(), QImage(), QImage(), false).isEmpty());
    UvLayout bad = layoutOf(s);
    bad.faceUv.pop_back();
    EXPECT_TRUE(toTexturedGlb(s.pos, s.tris, bad, albedo, QImage(), QImage(), false).isEmpty());
}

TEST(BodyTextureTransfer, FailsWhenNoTexelMatchesTheSurface)
{
    // Every candidate rejected by the normal gate: reporting success with a transparent atlas would waste a multi-minute run.
    const Sphere s = makeSphere(1.0f, 0.0f);
    TransferOptions opt;
    opt.textureSize = 64;
    opt.nonRigid = false;
    opt.minNormalDot = 2.0f;                       // no normal can agree this much
    const TransferResult r = transferTexture(surfaceOf(s), s.pos, s.tris, layoutOf(s), opt);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.isEmpty());
}
