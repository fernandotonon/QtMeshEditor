#include <gtest/gtest.h>

#include "ProceduralHair.h"
#include "HairFitting.h"

#include <QImage>
#include <QJsonObject>
#include <set>
#include <tuple>

using namespace CharacterGen::Hair;

namespace HairTestSupport {

// An ellipsoid "head" (UV sphere) centred at (0,1.6,0), with the face looking +Z, as the body mesh the hair is built on.
struct SyntheticHead {
    std::vector<float> positions;
    std::vector<std::array<uint32_t, 3>> tris;
    std::vector<uint8_t> mask;
    BodyCollider collider;
    Scalp scalp;
    explicit SyntheticHead(float rx = 0.080f, float ry = 0.115f, float rz = 0.095f, int rings = 28, int segs = 40)
    {
        for (int i = 0; i <= rings; ++i) {
            const float th = 3.14159265f * float(i) / float(rings);
            for (int j = 0; j < segs; ++j) {
                const float ph = 2.0f * 3.14159265f * float(j) / float(segs);
                positions.insert(positions.end(), {rx * std::sin(th) * std::sin(ph), 1.6f + ry * std::cos(th), rz * std::sin(th) * std::cos(ph)});
            }
        }
        for (int i = 0; i < rings; ++i)
            for (int j = 0; j < segs; ++j) {
                const uint32_t a = uint32_t(i * segs + j), b = uint32_t(i * segs + (j + 1) % segs), c = uint32_t((i + 1) * segs + j), d = uint32_t((i + 1) * segs + (j + 1) % segs);
                tris.push_back({a, c, b});   // outward winding
                tris.push_back({b, c, d});
            }
        mask.assign(positions.size() / 3, 1);
        collider.build(positions, tris);
        scalp = extractScalp(positions, tris, mask);
    }
};

}  // namespace HairTestSupport

using HairTestSupport::SyntheticHead;

TEST(HairRng, IsDeterministicAndInRange)
{
    Rng a(42), b(42), c(43);
    for (int i = 0; i < 100; ++i) {
        const float x = a.uniform();
        EXPECT_FLOAT_EQ(x, b.uniform());
        EXPECT_GE(x, 0.0f);
        EXPECT_LT(x, 1.0f);
    }
    EXPECT_NE(Rng(42).next(), c.next());
}

TEST(HairCurve, CatmullRomKeepsEndpointsAndIsContinuous)
{
    const std::vector<V3> cp = {{0, 0, 0}, {0, -1, 0.2f}, {0.1f, -2, 0.1f}, {0.3f, -3, 0}};
    const V3 s = evalCurve(cp, 0.0f), e = evalCurve(cp, 1.0f);
    EXPECT_NEAR(length(s - cp.front()), 0.0f, 1e-6f);
    EXPECT_NEAR(length(e - cp.back()), 0.0f, 1e-6f);
    V3 prev = s;
    for (int i = 1; i <= 50; ++i) {
        const V3 p = evalCurve(cp, float(i) / 50.0f);
        EXPECT_LT(length(p - prev), 0.25f);
        prev = p;
    }
    EXPECT_GT(curveLength(cp), 3.0f);
}

TEST(HairScalp, FindsTheHeadAndAHairlineBelowTheCrown)
{
    SyntheticHead h;
    ASSERT_TRUE(h.scalp.valid());
    EXPECT_GT(h.scalp.totalArea, 0.005f);
    float minY = 1e9f;
    for (const V3& p : h.scalp.pos) minY = std::min(minY, p.y);
    EXPECT_GT(minY, h.scalp.head.chin);
    EXPECT_LT(minY, h.scalp.head.top - 0.03f);        // not just a cap: it reaches down towards the temples and the nape
    EXPECT_LT(hairlineFraction(0, -1, 0), hairlineFraction(0, 1, 0));   // the nape hairline is lower than the forehead's
}

TEST(HairGeneration, IsDeterministicForTheSameSeed)
{
    SyntheticHead h;
    Params p = presetParams("medium_layered");
    const Result a = generate(p, h.scalp, h.collider), b = generate(p, h.scalp, h.collider);
    ASSERT_GT(a.mesh.triangles(), 100);
    ASSERT_EQ(a.mesh.pos.size(), b.mesh.pos.size());
    for (size_t i = 0; i < a.mesh.pos.size(); ++i) EXPECT_EQ(length(a.mesh.pos[i] - b.mesh.pos[i]), 0.0f);
    p.seed = 99;
    const Result c = generate(p, h.scalp, h.collider);
    bool differs = c.mesh.pos.size() != a.mesh.pos.size();
    for (size_t i = 0; !differs && i < a.mesh.pos.size(); ++i) differs = length(a.mesh.pos[i] - c.mesh.pos[i]) > 1e-6f;
    EXPECT_TRUE(differs);
}

TEST(HairGeneration, EveryPresetProducesAValidMesh)
{
    SyntheticHead h;
    for (const QString& id : presetIds()) {
        const Result r = generate(presetParams(id), h.scalp, h.collider);
        SCOPED_TRACE(id.toStdString());
        EXPECT_GT(r.mesh.cards, 20);
        EXPECT_TRUE(validate(r.mesh).ok());
        EXPECT_EQ(r.mesh.pos.size(), r.mesh.nrm.size());
        EXPECT_EQ(r.mesh.uv.size(), r.mesh.pos.size() * 2);
        EXPECT_LE(r.mesh.triangles(), presetParams(id).maxTriangles + 64);
    }
}

TEST(HairGeneration, PresetsHaveDistinctSilhouettes)
{
    SyntheticHead h;
    std::set<int> lengthBins;
    for (const QString& id : presetIds()) {
        const Result r = generate(presetParams(id), h.scalp, h.collider);
        float minY = 1e9f;
        for (const V3& p : r.mesh.pos) minY = std::min(minY, p.y);
        lengthBins.insert(int((h.scalp.head.top - minY) * 50.0f));    // 2 cm bins
    }
    EXPECT_GE(lengthBins.size(), 5u);
}

TEST(HairGeneration, LengthParameterControlsHowFarTheHairHangs)
{
    SyntheticHead h;
    auto lowest = [&](float len) {
        Params p = presetParams("long_straight");
        p.length = len;
        float minY = 1e9f;
        for (const V3& v : generate(p, h.scalp, h.collider).mesh.pos) minY = std::min(minY, v.y);
        return minY;
    };
    EXPECT_LT(lowest(0.4f), lowest(0.15f) - 0.05f);
}

TEST(HairGeneration, RespectsTheTriangleBudget)
{
    SyntheticHead h;
    Params p = presetParams("curly");
    p.density = 1.0f;
    p.quality = 2;
    p.maxTriangles = 1500;
    const Result r = generate(p, h.scalp, h.collider);
    EXPECT_LE(r.mesh.triangles(), 1500 + 2 * 12 * 3);
    EXPECT_GT(r.mesh.triangles(), 200);
}

TEST(HairGeneration, UvsStayInsideTheTextureAndRunAlongTheCard)
{
    SyntheticHead h;
    const Result r = generate(presetParams("wavy"), h.scalp, h.collider);
    ASSERT_GT(r.mesh.cards, 1);
    for (float u : r.mesh.uv) {
        EXPECT_GE(u, 0.0f);
        EXPECT_LE(u, 1.0f);
    }
    EXPECT_FLOAT_EQ(r.mesh.uv[1], 1.0f);                                    // root: v = 1 (top row)
    const size_t tipVertex = size_t(r.mesh.rootVertex[1]) - 1;              // last vertex of the first card
    EXPECT_FLOAT_EQ(r.mesh.uv[tipVertex * 2 + 1], 0.0f);                    // tip: v = 0
}

TEST(HairGeneration, CardsFaceOutwardWithConsistentWinding)
{
    SyntheticHead h;
    const Result r = generate(presetParams("long_straight"), h.scalp, h.collider);
    int outward = 0, total = 0;
    for (size_t t = 0; t + 2 < r.mesh.idx.size(); t += 3) {
        const V3 &a = r.mesh.pos[r.mesh.idx[t]], &b = r.mesh.pos[r.mesh.idx[t + 1]], &c = r.mesh.pos[r.mesh.idx[t + 2]];
        const V3 n = cross(b - a, c - a);
        const V3 mid = (a + b + c) * (1.0f / 3.0f);
        const V3 radial(mid.x - h.scalp.head.centre.x, 0, mid.z - h.scalp.head.centre.z);
        ++total;
        if (dot(n, radial) > 0) ++outward;
    }
    // the winding is the same for every strip, so a clear majority (not a coin flip) agrees on the side
    const float f = float(outward) / float(total);
    EXPECT_TRUE(f > 0.75f || f < 0.25f) << f;
}

TEST(HairGeneration, HairStaysOutsideTheHead)
{
    SyntheticHead h;
    for (const QString& id : {QStringLiteral("bob"), QStringLiteral("curly"), QStringLiteral("short_side_part")}) {
        const Result r = generate(presetParams(id), h.scalp, h.collider);
        int inside = 0;
        for (const V3& p : r.mesh.pos) if (h.collider.signedDistance(p) < -0.004f) ++inside;
        SCOPED_TRACE(id.toStdString());
        EXPECT_LT(float(inside) / float(r.mesh.pos.size()), 0.02f);
    }
}

TEST(HairGeneration, PartingChangesTheFlow)
{
    SyntheticHead h;
    auto meanX = [&](float parting) {
        Params p = presetParams("short_side_part");
        p.parting = parting;
        p.symmetric = false;
        p.fringe = 0.8f;
        double sum = 0; int n = 0;
        for (const Guide& g : generateGuides(p, h.scalp, h.collider))
            if (g.root.z > h.scalp.head.centre.z + 0.04f) { sum += g.cp.back().x - g.root.x; ++n; }
        return n ? sum / n : 0.0;
    };
    EXPECT_NE(meanX(-0.8f), meanX(0.8f));
}

TEST(HairGeneration, SymmetricGuidesMirrorAcrossTheHead)
{
    SyntheticHead h;
    Params p = presetParams("long_straight");
    p.symmetric = true;
    const auto guides = generateGuides(p, h.scalp, h.collider);
    ASSERT_GT(guides.size(), 20u);
    int matched = 0;
    for (const Guide& g : guides)
        for (const Guide& o : guides)
            if (length(V3(2 * h.scalp.head.centre.x - g.root.x, g.root.y, g.root.z) - o.root) < 1e-4f) { ++matched; break; }
    EXPECT_GT(float(matched) / float(guides.size()), 0.9f);
}

TEST(HairGeneration, HasAtLeastSixteenPresetsAndEachOneIsDistinct)
{
    EXPECT_GE(presetIds().size(), 16);
    SyntheticHead h;
    std::set<QString> fingerprints;
    for (const QString& id : presetIds()) {
        const Result r = generate(presetParams(id), h.scalp, h.collider);
        double sx = 0, sy = 0, sz = 0;
        for (const V3& v : r.mesh.pos) { sx += v.x; sy += v.y; sz += v.z; }
        fingerprints.insert(QString::number(int(sy / double(r.mesh.pos.size()) * 400)) + "/" + QString::number(int(sz / double(r.mesh.pos.size()) * 400)) +
                            "/" + QString::number(r.mesh.triangles() / 400));
    }
    EXPECT_GE(fingerprints.size(), 10u);
}

TEST(HairGeneration, TheScalpLinerAndTheCrownFanAreInTheMesh)
{
    SyntheticHead h;
    const Result r = generate(presetParams("pixie"), h.scalp, h.collider);
    // sheets + crown fan + liner: more strips than a single ring of locks
    EXPECT_GT(r.mesh.cards, 30);
    // the liner follows the scalp: some vertices sit within a few mm of a scalp vertex
    int onScalp = 0;
    for (const V3& v : r.mesh.pos)
        for (const V3& s : h.scalp.pos)
            if (length(v - s) < 0.004f) { ++onScalp; break; }
    EXPECT_GT(onScalp, int(h.scalp.pos.size()) / 2);
}

TEST(HairGeneration, NoHairInFrontOfTheFaceBelowTheBrows)
{
    SyntheticHead h;
    const HeadFrame& hd = h.scalp.head;
    for (const QString& id : presetIds()) {
        if (id.contains("ponytail")) continue;
        const Result r = generate(presetParams(id), h.scalp, h.collider);
        int bad = 0;
        for (size_t vi = 0; vi < r.mesh.pos.size(); ++vi) {
            const V3& v = r.mesh.pos[vi];
            if (std::fabs(v.x - hd.centre.x) < 0.7f * hd.radii.x && v.z > hd.centre.z + 0.55f * hd.radii.z && v.y < hd.chin + 0.62f * (hd.top - hd.chin) && v.y > hd.chin - 0.1f) {
                ++bad;
            }
        }
        SCOPED_TRACE(id.toStdString());
        EXPECT_LE(bad, 8) << id.toStdString();   // a lock at the very edge of the region may graze it
    }
}

TEST(HairScalp, ScalpWeightsAreSoftAtTheHairlineAndZeroBelowIt)
{
    SyntheticHead h;
    const std::vector<float> w = scalpWeights(h.positions, h.mask);
    ASSERT_EQ(w.size(), h.mask.size());
    const HeadFrame& hd = h.scalp.head;
    int top = 0, topFull = 0, chin = 0, chinZero = 0, soft = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        const float hf = (h.positions[i * 3 + 1] - hd.chin) / (hd.top - hd.chin);
        EXPECT_GE(w[i], 0.0f);
        EXPECT_LE(w[i], 1.0f);
        if (hf > 0.95f) { ++top; if (w[i] > 0.95f) ++topFull; }
        if (hf < 0.25f) { ++chin; if (w[i] == 0.0f) ++chinZero; }
        if (w[i] > 0.05f && w[i] < 0.95f) ++soft;
    }
    EXPECT_GT(top, 0);
    EXPECT_EQ(topFull, top);        // the crown is all scalp
    EXPECT_EQ(chinZero, chin);      // the jaw is none
    EXPECT_GT(soft, 0);             // and the hairline is a gradient, not a step
}

TEST(HairCollider, EarSphereKeepsAStrandOutAndPushesItOverTheEar)
{
    SyntheticHead h;
    BodyCollider col = h.collider;
    const V3 ear(h.scalp.head.centre.x + h.scalp.head.radii.x * 1.05f, h.scalp.head.centre.y, h.scalp.head.centre.z);
    col.addSphere(ear, 0.03f, 1.0f);
    const V3 inside = ear + V3(-0.01f, 0.0f, 0.0f);                    // between the ear and the head
    const V3 pushed = col.pushOut(inside, 0.002f);
    EXPECT_GE(length(pushed - ear), 0.03f);                            // out of the ball
    EXPECT_GT(pushed.x, inside.x);                                     // towards the side, over the ear, never into the head
    EXPECT_EQ(length(col.pushOut(inside, 0.002f, false) - h.collider.pushOut(inside, 0.002f, false)), 0.0f);   // roots ignore it
}

TEST(HairGeneration, EveryParameterIsContinuousAroundZero)
{
    // A rule that switches on at exactly "> 0" makes 0 and 0.01 look completely different (the ponytail did: strands past the tie stopped
    // following the head for ANY ponytail > 0). Nudging a value off 0 must move the hair only a little.
    SyntheticHead h;
    for (const QString& base : {QStringLiteral("long_straight"), QStringLiteral("bob"), QStringLiteral("long_wavy")}) {
        for (const QString& key : {QStringLiteral("ponytail"), QStringLiteral("curliness"), QStringLiteral("frizz"), QStringLiteral("fringe"), QStringLiteral("volume"),
                                    QStringLiteral("layers"), QStringLiteral("taper"), QStringLiteral("parting")}) {
            Params a = presetParams(base), b = a;
            setParam(a, key, 0.0);
            setParam(b, key, 0.01);
            const Result ra = generate(a, h.scalp, h.collider), rb = generate(b, h.scalp, h.collider);
            ASSERT_EQ(ra.mesh.pos.size(), rb.mesh.pos.size()) << key.toStdString();
            double sum = 0;
            for (size_t i = 0; i < ra.mesh.pos.size(); ++i) sum += length(ra.mesh.pos[i] - rb.mesh.pos[i]);
            const double mean = sum / double(ra.mesh.pos.size());
            SCOPED_TRACE((base + "/" + key).toStdString());
            EXPECT_LT(mean, 0.012) << "mean displacement " << mean << " m for a 0.01 change";   // linear sensitivity is a few mm on long hair; a rule that switches at 0 moved it by cm
        }
    }
}

TEST(HairGeneration, HangingHairNeverClimbsBackUp)
{
    // The brow limit used to lerp a strand towards the brow line by its "front-ness" on every step, which dragged hair hanging low along the
    // cheek UP (long hair pointing up at the front). Below the middle of the head a strand may only go down (or a hair's breadth up).
    SyntheticHead h;
    for (const QString& id : presetIds()) {
        if (id.contains("ponytail") || id == "afro" || id == "curly" || id == "curly_bob") continue;     // curls and ties legitimately wander
        Params p = presetParams(id);
        int bad = 0;
        for (const Guide& g : generateGuides(p, h.scalp, h.collider))
            for (size_t k = 3; k < g.cp.size(); ++k)
                if (g.cp[k - 1].y < h.scalp.head.centre.y && g.cp[k].y - g.cp[k - 1].y > 0.006f) ++bad;
        EXPECT_EQ(bad, 0) << id.toStdString();
    }
}

TEST(HairGeneration, SheetsAreContinuousSoNeighbouringColumnsShareTheirEdge)
{
    SyntheticHead h;
    Params p = presetParams("long_straight");
    p.clumping = 0.0f;                                   // no lock separation: the strips of a ring must meet exactly
    const Result r = generate(p, h.scalp, h.collider);
    // every vertex of a strip's last line has a twin (same position) in the next strip's first line, except at lock gaps
    std::set<std::tuple<int, int, int>> seen;
    int dup = 0;
    for (const V3& v : r.mesh.pos) {
        const auto key = std::make_tuple(int(std::lround(v.x * 1e4f)), int(std::lround(v.y * 1e4f)), int(std::lround(v.z * 1e4f)));
        if (!seen.insert(key).second) ++dup;
    }
    EXPECT_GT(dup, int(r.mesh.pos.size()) / 8);
}

TEST(HairGuides, EditsMoveControlPointsAndKeepTheRoot)
{
    SyntheticHead h;
    Params p = presetParams("long_straight");
    auto guides = generateGuides(p, h.scalp, h.collider);
    ASSERT_GT(guides.size(), 3u);
    const V3 root = guides[0].root, tip = guides[0].cp.back();
    applyGuideEdit(guides, {0, 3, V3(0.05f, 0, 0)});
    EXPECT_NEAR(guides[0].cp.back().x - tip.x, 0.05f * 0.6f, 1e-5f);
    applyGuideEdit(guides, {0, 0, V3(1, 1, 1)});          // the root cannot be moved
    EXPECT_EQ(length(guides[0].root - root), 0.0f);
    const float lenBefore = curveLength(guides[0].cp);
    applyGuideEdit(guides, {0, -1, V3(0.5f, 0, 0)});      // +50 % length
    EXPECT_NEAR(curveLength(guides[0].cp) / lenBefore, 1.5f, 0.01f);
    p.edits.push_back({2, 4, V3(0, 0.02f, 0)});           // edits stored in the params replay identically
    const auto a = generateGuides(p, h.scalp, h.collider), b = generateGuides(p, h.scalp, h.collider);
    EXPECT_EQ(length(a[2].cp[4] - b[2].cp[4]), 0.0f);
}

TEST(HairValidation, DetectsAndRepairsBrokenGeometry)
{
    CardMesh m;
    m.pos = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {std::nanf(""), 0, 0}, {0, 0, 0}};
    m.nrm.assign(5, V3(0, 0, 1));
    m.uv = {0, 0, 1, 0, 0, 1, 0.5f, 0.5f, 2.0f, 0};
    m.idx = {0, 1, 2, 0, 1, 3, 0, 0, 4, 0, 1, 99};
    const ValidationReport r = validate(m);
    EXPECT_FALSE(r.ok());
    EXPECT_EQ(r.nanVertices, 1);
    EXPECT_EQ(r.badIndices, 1);
    EXPECT_GE(r.zeroAreaTriangles, 1);
    EXPECT_EQ(r.badUvs, 1);
    repair(m);
    EXPECT_EQ(m.idx.size(), 3u);
    EXPECT_TRUE(validate(m).ok());
}

TEST(HairParams, JsonRoundTripAndClamping)
{
    Params p = presetParams("curly");
    p.edits.push_back({3, 2, V3(0.01f, 0.02f, 0.03f)});
    p.seed = 1234;
    Params q;
    ASSERT_TRUE(fromJsonString(toJsonString(p), &q));
    EXPECT_FLOAT_EQ(q.curliness, p.curliness);
    EXPECT_EQ(q.seed, 1234u);
    ASSERT_EQ(q.edits.size(), 1u);
    EXPECT_EQ(q.edits[0].guide, 3);
    EXPECT_FLOAT_EQ(q.edits[0].delta.z, 0.03f);
    QJsonObject o = toJson(p);
    o["length"] = 99.0;
    o["density"] = -4.0;
    ASSERT_TRUE(fromJson(o, &q));
    EXPECT_LE(q.length, 1.2f);
    EXPECT_GE(q.density, 0.0f);
    EXPECT_FALSE(fromJsonString(QStringLiteral("[1,2]"), &q));
}

TEST(HairTexture, CardTextureHasAlphaCutoutAndVariants)
{
    const QByteArray png = bakeCardTexturePng(256);
    QImage img;
    ASSERT_TRUE(img.loadFromData(png, "PNG"));
    EXPECT_EQ(img.width(), 256);
    int opaque = 0, clear = 0;
    for (int y = 0; y < img.height(); y += 4)
        for (int x = 0; x < img.width(); x += 4) (qAlpha(img.pixel(x, y)) > 128 ? opaque : clear)++;
    EXPECT_GT(opaque, 100);
    EXPECT_GT(clear, 100);
}

TEST(HairDna, DnaMeshCarriesSkinFromTheNearestBodyVertex)
{
    SyntheticHead h;
    std::vector<std::vector<Dna::SkinInfluence>> skin(h.positions.size() / 3, std::vector<Dna::SkinInfluence>{{7, 1.0f}});
    const Result r = generate(presetParams("short_straight"), h.scalp, h.collider);
    const auto m = toDnaMesh(r.mesh, h.collider, skin);
    ASSERT_EQ(m.positions.size(), r.mesh.pos.size() * 3);
    ASSERT_EQ(m.skin.size(), r.mesh.pos.size());
    EXPECT_EQ(m.skin[0][0].joint, 7);
    EXPECT_EQ(m.faces.size(), size_t(r.mesh.triangles()));
}
