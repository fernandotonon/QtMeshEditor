#include <gtest/gtest.h>

#include "HairFitting.h"
#include "ProceduralHair.h"

using namespace CharacterGen::Hair;

namespace {

// A head ellipsoid centred at (0, 1.6, 0) — same construction as ProceduralHair_test.
struct Head {
    std::vector<float> positions;
    std::vector<std::array<uint32_t, 3>> tris;
    std::vector<uint8_t> mask;
    BodyCollider collider;
    HeadFrame frame;
    Head(float rx, float ry, float rz)
    {
        const int rings = 28, segs = 40;
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
                tris.push_back({a, c, b});
                tris.push_back({b, c, d});
            }
        mask.assign(positions.size() / 3, 1);
        collider.build(positions, tris);
        frame = estimateHeadFrame(positions, mask);
    }
};

// A hairstyle authored elsewhere: centimetres, origin at its own floor, a dome over the crown plus a skirt hanging 30 cm lower.
// `cap` is the dome's relative radius (1.0 = tight on a head of the authored size).
ImportedHair makeHair(float cap = 1.0f)
{
    ImportedHair h;
    h.name = "synthetic";
    const int rings = 12, segs = 24;
    const float R = 9.0f * cap;                    // cm, authored for a head ~15 cm wide
    for (int i = 0; i <= rings; ++i) {
        const float th = 0.5f * 3.14159265f * float(i) / float(rings);       // dome: top to equator
        for (int j = 0; j < segs; ++j) {
            const float ph = 2.0f * 3.14159265f * float(j) / float(segs);
            h.pos.push_back({R * std::sin(th) * std::sin(ph), 150.0f + 11.0f * std::cos(th) * cap, R * std::sin(th) * std::cos(ph)});
            h.uv.push_back(float(j) / segs);
            h.uv.push_back(1.0f - float(i) / rings);
        }
    }
    const int base = rings * segs;
    for (int k = 1; k <= 6; ++k)                    // skirt
        for (int j = 0; j < segs; ++j) {
            const float ph = 2.0f * 3.14159265f * float(j) / float(segs);
            h.pos.push_back({R * std::sin(ph), 150.0f - 5.0f * float(k), R * std::cos(ph)});
            h.uv.push_back(float(j) / segs);
            h.uv.push_back(0.0f);
        }
    auto ring = [&](int r) { return (r <= rings ? r * segs : base + (r - rings) * segs); };
    for (int r = 0; r < rings + 6; ++r)
        for (int j = 0; j < segs; ++j) {
            const uint32_t a = uint32_t(ring(r) + j), b = uint32_t(ring(r) + (j + 1) % segs), c = uint32_t(ring(r + 1) + j), d = uint32_t(ring(r + 1) + (j + 1) % segs);
            h.idx.insert(h.idx.end(), {a, c, b, b, c, d});
        }
    return h;
}

float width(const std::vector<V3>& p)
{
    float lo = 1e9f, hi = -1e9f;
    for (const V3& v : p) { lo = std::min(lo, v.x); hi = std::max(hi, v.x); }
    return hi - lo;
}

}  // namespace

TEST(HairFitting, ScalesAndPlacesOnTheHeadInMetres)
{
    Head head(0.080f, 0.115f, 0.095f);
    std::vector<V3> pos, nrm;
    FitReport rep;
    fitToHead(makeHair(), head.frame, head.collider, FitAdjust(), &pos, &nrm, &rep);
    ASSERT_EQ(pos.size(), makeHair().pos.size());
    EXPECT_NEAR(rep.scale, 0.01f * 0.16f / 0.15f * 1.12f / 1.12f, 0.004f);    // ~cm -> m, matched to the head's width
    float top = -1e9f;
    for (const V3& v : pos) top = std::max(top, v.y);
    EXPECT_NEAR(top, head.frame.top + 0.012f, 0.004f);
    float cx = 0;
    for (const V3& v : pos) cx += v.x;
    EXPECT_NEAR(cx / float(pos.size()), 0.0f, 0.01f);
}

TEST(HairFitting, NoPenetrationOnNarrowAndWideHeads)
{
    for (const auto& dims : {std::array<float, 3>{0.068f, 0.105f, 0.085f}, std::array<float, 3>{0.095f, 0.125f, 0.110f}, std::array<float, 3>{0.075f, 0.135f, 0.080f}}) {
        Head head(dims[0], dims[1], dims[2]);
        std::vector<V3> pos, nrm;
        FitReport rep;
        // a tight cap (0.85) would sink into any head: the fit must lift it out
        fitToHead(makeHair(0.85f), head.frame, head.collider, FitAdjust(), &pos, &nrm, &rep);
        EXPECT_EQ(countPenetrations(pos, head.collider, 0.001f), 0) << dims[0];
    }
}

TEST(HairFitting, WiderHeadGivesWiderHair)
{
    Head narrow(0.066f, 0.110f, 0.085f), wide(0.096f, 0.110f, 0.085f);
    std::vector<V3> a, b, n;
    fitToHead(makeHair(), narrow.frame, narrow.collider, FitAdjust(), &a, &n);
    fitToHead(makeHair(), wide.frame, wide.collider, FitAdjust(), &b, &n);
    EXPECT_GT(width(b), width(a) * 1.25f);
}

TEST(HairFitting, KeepsTheOuterSilhouetteAndTheLength)
{
    Head head(0.080f, 0.115f, 0.095f);
    std::vector<V3> pos, n;
    FitReport rep;
    fitToHead(makeHair(), head.frame, head.collider, FitAdjust(), &pos, &n, &rep);
    // the skirt (the last 6 rings) hangs far from the head surface and is only scaled, not pulled onto the scalp
    const size_t skirtStart = pos.size() - 6 * 24;
    int farFromScalp = 0;
    for (size_t i = skirtStart; i < pos.size(); ++i) {
        float d = 0;
        head.collider.nearest(pos[i], &d);
        if (d > 0.04f) ++farFromScalp;
    }
    EXPECT_GT(farFromScalp, 40);
    float lo = 1e9f, hi = -1e9f;
    for (const V3& v : pos) { lo = std::min(lo, v.y); hi = std::max(hi, v.y); }
    EXPECT_NEAR((hi - lo) / (rep.scale * 41.0f), 1.0f, 0.1f);      // 41 cm authored extent (11 + 30)
}

TEST(HairFitting, ManualAdjustmentsMoveRotateAndScale)
{
    Head head(0.080f, 0.115f, 0.095f);
    std::vector<V3> base, moved, scaled, n;
    fitToHead(makeHair(), head.frame, head.collider, FitAdjust(), &base, &n);
    FitAdjust a;
    a.translate = {0.0f, 0.0f, 0.02f};
    a.snug = 0.0f;
    fitToHead(makeHair(), head.frame, head.collider, a, &moved, &n);
    float zb = 0, zm = 0;
    for (size_t i = 0; i < base.size(); ++i) { zb += base[i].z; zm += moved[i].z; }
    EXPECT_NEAR((zm - zb) / float(base.size()), 0.02f, 0.006f);
    FitAdjust s;
    s.scale = 1.3f;
    fitToHead(makeHair(), head.frame, head.collider, s, &scaled, &n);
    EXPECT_NEAR(width(scaled) / width(base), 1.3f, 0.08f);
}

TEST(HairFitting, FlipFrontTurnsTheStyleHalfAround)
{
    Head head(0.080f, 0.115f, 0.095f);
    ImportedHair h = makeHair();
    for (V3& p : h.pos) if (p.y > 150.0f) p.z += 4.0f * (p.x * p.x < 4.0f ? 1.0f : 0.0f);   // a forward lump
    std::vector<V3> a, b, n;
    fitToHead(h, head.frame, head.collider, FitAdjust(), &a, &n);
    FitAdjust f;
    f.flipFront = true;
    fitToHead(h, head.frame, head.collider, f, &b, &n);
    float za = 0, zb = 0;
    for (size_t i = 0; i < a.size(); ++i) if (h.pos[i].y > 150.0f && std::fabs(h.pos[i].x) < 2.0f) { za += a[i].z; zb += b[i].z; }
    EXPECT_GT(za, zb);
}

TEST(HairFitting, AdjustJsonRoundTripClamps)
{
    FitAdjust a;
    a.translate = {0.01f, 0.02f, -0.01f};
    a.rotateDeg = {5, 10, 15};
    a.scale = 1.2f;
    a.flipFront = true;
    const FitAdjust b = fitAdjustFromJson(toJson(a));
    EXPECT_FLOAT_EQ(b.scale, 1.2f);
    EXPECT_TRUE(b.flipFront);
    EXPECT_FLOAT_EQ(b.rotateDeg.z, 15.0f);
    QJsonObject o = toJson(a);
    o["scale"] = 50.0;
    o["tx"] = 9.0;
    const FitAdjust c = fitAdjustFromJson(o);
    EXPECT_LE(c.scale, 3.0f);
    EXPECT_LE(c.translate.x, 0.3f);
}

TEST(HairFitting, EmptyOrUnmeasurableHeadLeavesTheStyleUnfitted)
{
    ImportedHair h = makeHair();
    BodyCollider empty;
    std::vector<V3> pos, n;
    FitReport rep;
    fitToHead(h, HeadFrame(), empty, FitAdjust(), &pos, &n, &rep);
    EXPECT_EQ(pos.size(), h.pos.size());
    EXPECT_FALSE(rep.warnings.isEmpty());
}
