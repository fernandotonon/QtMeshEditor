#include "PartUnwrap.h"

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <set>
#include <vector>

// Part-based UV islands: pure data, synthetic meshes, no Ogre/GL.

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Mesh {
    std::vector<float> pos;
    std::vector<uint32_t> idx;
};

// Latitude/longitude sphere with welded poles.
Mesh sphere(int rings, int segs)
{
    Mesh m;
    m.pos = {0, 1, 0};
    for (int r = 1; r < rings; ++r) {
        const double th = kPi * r / rings;
        for (int s = 0; s < segs; ++s) {
            const double ph = 2 * kPi * s / segs;
            m.pos.push_back(float(std::sin(th) * std::cos(ph)));
            m.pos.push_back(float(std::cos(th)));
            m.pos.push_back(float(std::sin(th) * std::sin(ph)));
        }
    }
    m.pos.insert(m.pos.end(), {0, -1, 0});
    const uint32_t south = uint32_t(m.pos.size() / 3 - 1);
    auto ring = [segs](int r, int s) { return uint32_t(1 + (r - 1) * segs + (s % segs)); };
    for (int s = 0; s < segs; ++s) m.idx.insert(m.idx.end(), {0, ring(1, s + 1), ring(1, s)});
    for (int r = 1; r < rings - 1; ++r)
        for (int s = 0; s < segs; ++s) {
            m.idx.insert(m.idx.end(), {ring(r, s), ring(r, s + 1), ring(r + 1, s + 1)});
            m.idx.insert(m.idx.end(), {ring(r, s), ring(r + 1, s + 1), ring(r + 1, s)});
        }
    for (int s = 0; s < segs; ++s)
        m.idx.insert(m.idx.end(), {ring(rings - 1, s), ring(rings - 1, s + 1), south});
    return m;
}

// Open tube (two boundary loops).
Mesh tube(int rings, int segs, double height)
{
    Mesh m;
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s < segs; ++s) {
            const double ph = 2 * kPi * s / segs;
            m.pos.insert(m.pos.end(), {float(std::cos(ph)), float(height * r / rings),
                                       float(std::sin(ph))});
        }
    auto v = [segs](int r, int s) { return uint32_t(r * segs + (s % segs)); };
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < segs; ++s) {
            m.idx.insert(m.idx.end(), {v(r, s), v(r + 1, s), v(r + 1, s + 1)});
            m.idx.insert(m.idx.end(), {v(r, s), v(r + 1, s + 1), v(r, s + 1)});
        }
    return m;
}

// Closed torus (genus 1): has no disc cut without opening its handle.
Mesh torus(int rings, int segs, double R, double r0)
{
    Mesh m;
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segs; ++j) {
            const double u = 2 * kPi * i / rings, w = 2 * kPi * j / segs;
            m.pos.insert(m.pos.end(), {float((R + r0 * std::cos(w)) * std::cos(u)),
                                       float(r0 * std::sin(w)),
                                       float((R + r0 * std::cos(w)) * std::sin(u))});
        }
    auto v = [rings, segs](int i, int j) { return uint32_t((i % rings) * segs + (j % segs)); };
    for (int i = 0; i < rings; ++i)
        for (int j = 0; j < segs; ++j) {
            m.idx.insert(m.idx.end(), {v(i, j), v(i + 1, j), v(i + 1, j + 1)});
            m.idx.insert(m.idx.end(), {v(i, j), v(i + 1, j + 1), v(i, j + 1)});
        }
    return m;
}

double triArea3(const Mesh& m, uint32_t a, uint32_t b, uint32_t c)
{
    double e1[3], e2[3];
    for (int k = 0; k < 3; ++k) {
        e1[k] = m.pos[b * 3 + k] - m.pos[a * 3 + k];
        e2[k] = m.pos[c * 3 + k] - m.pos[a * 3 + k];
    }
    const double x = e1[1] * e2[2] - e1[2] * e2[1], y = e1[2] * e2[0] - e1[0] * e2[2],
                 z = e1[0] * e2[1] - e1[1] * e2[0];
    return 0.5 * std::sqrt(x * x + y * y + z * z);
}

double uvArea(const std::vector<float>& uv, uint32_t a, uint32_t b, uint32_t c)
{
    return 0.5 * ((uv[b * 2] - uv[a * 2]) * (uv[c * 2 + 1] - uv[a * 2 + 1])
                  - (uv[c * 2] - uv[a * 2]) * (uv[b * 2 + 1] - uv[a * 2 + 1]));
}

// Charts = face groups connected through shared OUTPUT vertices.
std::vector<int> chartOfFace(const std::vector<uint32_t>& idx, size_t vertexCount)
{
    std::vector<uint32_t> p(vertexCount);
    for (size_t i = 0; i < p.size(); ++i) p[i] = uint32_t(i);
    auto find = [&p](uint32_t x) {
        while (p[x] != x) { p[x] = p[p[x]]; x = p[x]; }
        return x;
    };
    for (size_t t = 0; t < idx.size(); t += 3) {
        p[find(idx[t])] = find(idx[t + 1]);
        p[find(idx[t + 1])] = find(idx[t + 2]);
    }
    std::vector<int> out(idx.size() / 3);
    for (size_t t = 0; t < out.size(); ++t) out[t] = int(find(idx[t * 3]));
    return out;
}

// Every non-leftover face: positive UV area, corners map back to the input
// triangle's vertices, and each island keeps the 3D area (mesh units).
void checkCommon(const Mesh& m, const PartUnwrap::Result& r)
{
    ASSERT_TRUE(r.ok) << r.error.toStdString();
    ASSERT_EQ(r.indices.size(), m.idx.size());
    ASSERT_EQ(r.uvs.size(), r.xref.size() * 2);
    for (size_t t = 0; t < m.idx.size(); ++t) {
        ASSERT_LT(r.indices[t], r.xref.size());
        EXPECT_EQ(r.xref[r.indices[t]], m.idx[t]) << "corner " << t;
    }
    const std::vector<int> chart = chartOfFace(r.indices, r.xref.size());
    std::map<int, std::pair<double, double>> areas;   // chart -> (uv, 3D)
    std::map<int, double> signedAreas;                 // chart -> signed uv area
    for (size_t f = 0; f < m.idx.size() / 3; ++f) {
        const double a = uvArea(r.uvs, r.indices[f * 3], r.indices[f * 3 + 1], r.indices[f * 3 + 2]);
        const double a3 = triArea3(m, m.idx[f * 3], m.idx[f * 3 + 1], m.idx[f * 3 + 2]);
        if (a == 0.0) continue;   // left to xatlas
        areas[chart[f]].first += std::fabs(a);
        signedAreas[chart[f]] += a;
        areas[chart[f]].second += a3;
    }
    for (const auto& [c, a] : areas) EXPECT_NEAR(a.first / a.second, 1.0, 1e-3) << "chart " << c;
    // every island winds counter-clockwise (folded faces allowed, a mirrored
    // island not: xatlas's flood never crosses a winding change)
    for (const auto& [c, a] : signedAreas) EXPECT_GT(a, 0.0) << "chart " << c;
}

}  // namespace

TEST(PartUnwrapTest, SrgbToLabEndpoints)
{
    float lab[3];
    const float white[3] = {1, 1, 1}, black[3] = {0, 0, 0}, red[3] = {1, 0, 0};
    PartUnwrap::srgbToLab(white, lab);
    EXPECT_NEAR(lab[0], 100.0f, 0.1f);
    EXPECT_NEAR(lab[1], 0.0f, 0.5f);
    EXPECT_NEAR(lab[2], 0.0f, 0.5f);
    PartUnwrap::srgbToLab(black, lab);
    EXPECT_NEAR(lab[0], 0.0f, 0.1f);
    PartUnwrap::srgbToLab(red, lab);
    EXPECT_NEAR(lab[0], 53.2f, 0.5f);
    EXPECT_GT(lab[1], 70.0f);
}

TEST(PartUnwrapTest, ColourBoundaryBecomesASeam)
{
    // Red cap (x > 0.35), blue rest: no island may mix the two. Off-centre on
    // purpose — a purely geometric split would never land on it.
    const Mesh m = sphere(24, 32);
    const size_t nf = m.idx.size() / 3;
    std::vector<float> rgb(nf * 3);
    std::vector<int> colour(nf);
    for (size_t f = 0; f < nf; ++f) {
        double x = 0;
        for (int k = 0; k < 3; ++k) x += m.pos[m.idx[f * 3 + k] * 3] / 3.0;
        colour[f] = x > 0.35 ? 0 : 1;
        rgb[f * 3] = colour[f] ? 0.1f : 0.9f;
        rgb[f * 3 + 1] = 0.1f;
        rgb[f * 3 + 2] = colour[f] ? 0.9f : 0.1f;
    }
    PartUnwrap::Options o;
    o.colourClusters = 2;
    const PartUnwrap::Result r = PartUnwrap::unwrap(m.pos, m.idx, rgb, {}, o);
    checkCommon(m, r);
    EXPECT_EQ(r.colourRegions, 2);
    EXPECT_EQ(r.facesToXatlas, 0);
    EXPECT_GE(r.charts, 2);
    // The cap and the rest flatten with little distortion, so they stay a few
    // islands rather than xatlas's dozens.
    EXPECT_LE(r.charts, 8);
    const std::vector<int> chart = chartOfFace(r.indices, r.xref.size());
    std::map<int, std::set<int>> coloursOf;
    for (size_t f = 0; f < nf; ++f) coloursOf[chart[f]].insert(colour[f]);
    for (const auto& [c, s] : coloursOf) EXPECT_EQ(s.size(), 1u) << "chart " << c;
}

TEST(PartUnwrapTest, OpenTubeIsCutIntoOneIsland)
{
    const Mesh m = tube(12, 24, 2.0);
    const PartUnwrap::Result r = PartUnwrap::unwrap(m.pos, m.idx, {}, {}, PartUnwrap::Options{});
    checkCommon(m, r);
    EXPECT_EQ(r.charts, 1);
    EXPECT_EQ(r.facesToXatlas, 0);
    for (size_t f = 0; f < m.idx.size() / 3; ++f)
        EXPECT_GT(uvArea(r.uvs, r.indices[f * 3], r.indices[f * 3 + 1], r.indices[f * 3 + 2]), 0.0);
}

TEST(PartUnwrapTest, TorusHandleIsOpened)
{
    // Genus 1: needs cuts around both cycles before it is a disc.
    const Mesh m = torus(32, 16, 2.0, 0.6);
    const PartUnwrap::Result r = PartUnwrap::unwrap(m.pos, m.idx, {}, {}, PartUnwrap::Options{});
    checkCommon(m, r);
    EXPECT_GE(r.charts, 1);
    EXPECT_LE(r.charts, 6);
    EXPECT_EQ(r.facesToXatlas, 0);
}

TEST(PartUnwrapTest, BuildAtlasPacksThePartIslands)
{
    const Mesh m = sphere(16, 24);
    PartUnwrap::AtlasOptions ao;
    ao.textureSize = 256;
    const PartUnwrap::Atlas a = PartUnwrap::buildAtlas(m.pos, m.idx, {}, {}, ao);
    ASSERT_TRUE(a.ok) << a.error.toStdString();
    EXPECT_TRUE(a.usedPartIslands);
    EXPECT_GT(a.width, 0);
    EXPECT_GT(a.height, 0);
    ASSERT_EQ(a.indices.size(), m.idx.size());
    ASSERT_EQ(a.uv.size(), a.xref.size() * 2);
    for (size_t t = 0; t < m.idx.size(); ++t) EXPECT_EQ(a.xref[a.indices[t]], m.idx[t]);
    for (size_t v = 0; v < a.xref.size(); ++v) {
        EXPECT_GE(a.uv[v * 2], 0.0f);
        EXPECT_LE(a.uv[v * 2], float(a.width));
        EXPECT_GE(a.uv[v * 2 + 1], 0.0f);
        EXPECT_LE(a.uv[v * 2 + 1], float(a.height));
    }
    // xatlas alone cuts a 768-triangle sphere into many more charts
    PartUnwrap::AtlasOptions plain = ao;
    plain.partIslands = false;
    const PartUnwrap::Atlas b = PartUnwrap::buildAtlas(m.pos, m.idx, {}, {}, plain);
    ASSERT_TRUE(b.ok);
    EXPECT_FALSE(b.usedPartIslands);
    EXPECT_LT(a.chartCount, b.chartCount);
}

TEST(PartUnwrapTest, BuildAtlasFallsBackToXatlasWithAReason)
{
    const Mesh m = sphere(8, 12);
    PartUnwrap::AtlasOptions ao;
    ao.textureSize = 128;
    ao.parts.maxFaces = 10;   // "too large"
    const PartUnwrap::Atlas a = PartUnwrap::buildAtlas(m.pos, m.idx, {}, {}, ao);
    ASSERT_TRUE(a.ok);
    EXPECT_FALSE(a.usedPartIslands);
    EXPECT_FALSE(a.partFallbackReason.isEmpty());
    EXPECT_EQ(a.indices.size(), m.idx.size());
}

TEST(PartUnwrapTest, CancellationStopsTheUnwrap)
{
    const Mesh m = sphere(12, 16);
    PartUnwrap::Options o;
    o.cancelled = [] { return true; };
    const PartUnwrap::Result r = PartUnwrap::unwrap(m.pos, m.idx, {}, {}, o);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.cancelled);
    PartUnwrap::AtlasOptions ao;
    ao.cancelled = [] { return true; };
    EXPECT_TRUE(PartUnwrap::buildAtlas(m.pos, m.idx, {}, {}, ao).cancelled);
}

TEST(PartUnwrapTest, RejectsBadInput)
{
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    EXPECT_FALSE(PartUnwrap::unwrap(pos, {}, {}, {}, {}).ok);
    EXPECT_FALSE(PartUnwrap::unwrap(pos, {0, 1, 7}, {}, {}, {}).ok);
    EXPECT_FALSE(PartUnwrap::buildAtlas({}, {}, {}, {}, {}).ok);
}

TEST(PartUnwrapTest, TinyPiecesAreLeftToXatlas)
{
    // A lone triangle is below minChartFaces: it keeps (0,0) UVs for xatlas.
    const std::vector<float> pos{0, 0, 0, 1, 0, 0, 0, 1, 0};
    const std::vector<uint32_t> idx{0, 1, 2};
    const PartUnwrap::Result r = PartUnwrap::unwrap(pos, idx, {}, {}, {});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.charts, 0);
    EXPECT_EQ(r.facesToXatlas, 1);
    PartUnwrap::AtlasOptions ao;
    ao.textureSize = 64;
    const PartUnwrap::Atlas a = PartUnwrap::buildAtlas(pos, idx, {}, {}, ao);
    ASSERT_TRUE(a.ok);
    EXPECT_FALSE(a.usedPartIslands);   // nothing made here -> plain xatlas
    EXPECT_GE(a.chartCount, 1);
}

TEST(PartUnwrapTest, TightLimitsStillCoverAlmostEverything)
{
    // A tight distortion limit rejects every large piece of a sphere: the
    // unwrap keeps splitting instead of handing rejected regions to xatlas.
    // (The degenerate-split fallback this exercises on real noisy meshes —
    // 2295 -> 586 leftover faces on a TRELLIS.2 character — does not trigger
    // on a smooth sphere, so this pins coverage, not that fallback.)
    const Mesh m = sphere(32, 48);
    PartUnwrap::Options o;
    o.maxAreaDistortion = 0.02f;
    o.minChartFaces = 8;
    const PartUnwrap::Result r = PartUnwrap::unwrap(m.pos, m.idx, {}, {}, o);
    checkCommon(m, r);
    EXPECT_GT(r.rejectedDistortion, 0);
    EXPECT_LT(double(r.facesToXatlas), 0.02 * double(m.idx.size() / 3))
        << r.facesToXatlas << " of " << m.idx.size() / 3;
}
