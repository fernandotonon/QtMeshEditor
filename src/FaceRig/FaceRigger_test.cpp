#include <gtest/gtest.h>

#include "FaceRig/ArkitTemplate.h"
#include "FaceRig/FaceRigger.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct Grid {
    std::vector<float> V;
    std::vector<int> F;
};

// A bumpy plane — a stand-in "face" surface with real triangles.
Grid makeGrid(int n, float extent, float bump)
{
    Grid g;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float fx = (float(x)/(n-1) - 0.5f) * extent;
            const float fy = (float(y)/(n-1) - 0.5f) * extent;
            const float fz = bump * std::sin(1.5f*float(x)) * std::cos(1.5f*float(y));
            g.V.insert(g.V.end(), {fx, fy, fz});
        }
    for (int y = 0; y < n-1; ++y)
        for (int x = 0; x < n-1; ++x) {
            const int a = y*n+x, b = y*n+x+1, c = (y+1)*n+x, d = (y+1)*n+x+1;
            g.F.insert(g.F.end(), {a, b, c});
            g.F.insert(g.F.end(), {b, d, c});
        }
    return g;
}

void putI32(QByteArray& b, int32_t v) {
    v = qToLittleEndian(v);
    b.append(reinterpret_cast<const char*>(&v), 4);
}
void putF32(QByteArray& b, float f) {
    quint32 raw; std::memcpy(&raw, &f, 4); raw = qToLittleEndian(raw);
    b.append(reinterpret_cast<const char*>(&raw), 4);
}

// Pack a synthetic arkit_template.bin (magic + V/F/S + neutral + faces +
// S×(name[32] + delta[V*3])) and write it to `path`.
bool writeSyntheticTemplate(const QString& path, const Grid& g,
                            const std::vector<std::pair<QString, std::vector<float>>>& shapes)
{
    QByteArray b;
    b.append("QMFRT1\0\0", 8);
    putI32(b, int(g.V.size()/3));
    putI32(b, int(g.F.size()/3));
    putI32(b, int(shapes.size()));
    for (float v : g.V) putF32(b, v);
    for (int i : g.F) putI32(b, i);
    for (const auto& [name, delta] : shapes) {
        char nm[32] = {0};
        const QByteArray n = name.toLatin1();
        std::memcpy(nm, n.constData(), std::min<size_t>(31, size_t(n.size())));
        b.append(nm, 32);
        for (float d : delta) putF32(b, d);
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(b);
    f.close();
    return true;
}

QString tempTemplatePath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return QDir(dir).filePath("qtmesh_facerig_test_template.bin");
}

}  // namespace

TEST(FaceRigger, RejectsBadInput)
{
    FaceRig::ArkitTemplate empty;
    const auto r = FaceRig::buildFaceRig({}, {}, empty);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(FaceRigger, ProducesPerUserVertexShapes)
{
    const Grid tmpl = makeGrid(12, 2.0f, 0.12f);
    const int nt = int(tmpl.V.size()/3);

    // shape A: push a centre bump in +Z (a "smile"-ish local deform)
    std::vector<float> smile(tmpl.V.size(), 0.0f);
    for (int i = 0; i < nt; ++i) {
        const float x = tmpl.V[size_t(i)*3], y = tmpl.V[size_t(i)*3+1];
        smile[size_t(i)*3+2] = 0.15f * std::exp(-(x*x + y*y) * 5.0f);
    }
    // shape B: drop the lower half in -Z (a "jawOpen"-ish region deform)
    std::vector<float> jaw(tmpl.V.size(), 0.0f);
    for (int i = 0; i < nt; ++i) {
        const float y = tmpl.V[size_t(i)*3+1];
        if (y < 0.0f) jaw[size_t(i)*3+2] = -0.2f * (-y);
    }

    const QString path = tempTemplatePath();
    ASSERT_TRUE(writeSyntheticTemplate(path, tmpl,
        {{"mouthSmileLeft", smile}, {"jawOpen", jaw}}));

    FaceRig::ArkitTemplate at;
    QString err;
    ASSERT_TRUE(at.load(path, &err)) << err.toStdString();
    ASSERT_EQ(at.shapeCount(), 2);

    // user = the SAME surface at a different tessellation (different topology)
    const Grid user = makeGrid(16, 2.0f, 0.12f);
    const int nu = int(user.V.size()/3);

    const auto r = FaceRig::buildFaceRig(user.V, user.F, at);
    ASSERT_TRUE(r.ok) << r.error;
    EXPECT_EQ(r.userVertexCount, nu);
    EXPECT_EQ(int(r.shapes.size()), 2);
    // same-surface fit should be tight
    EXPECT_LT(r.fitMeanResidualPct, 5.0);

    for (const auto& sh : r.shapes) {
        EXPECT_EQ(int(sh.userDeltas.size()), nu * 3);
        for (float v : sh.userDeltas) EXPECT_TRUE(std::isfinite(v));
        EXPECT_GT(sh.nonZeroVerts, 0);        // the shape actually moves verts
        EXPECT_GT(sh.maxDisp, 0.0f);
    }

    // names preserved, order preserved
    EXPECT_EQ(r.shapes[0].name, QStringLiteral("mouthSmileLeft"));
    EXPECT_EQ(r.shapes[1].name, QStringLiteral("jawOpen"));

    // semantics: the smile shape moves CENTRE verts most; the jaw shape moves
    // LOWER-half verts most. Check the centroid of moved mass.
    auto movedCentroidY = [&](const FaceRig::FaceRigShape& sh) {
        double sy = 0, w = 0;
        for (int i = 0; i < nu; ++i) {
            const float* d = &sh.userDeltas[size_t(i)*3];
            const double m = std::sqrt(double(d[0])*d[0]+double(d[1])*d[1]+double(d[2])*d[2]);
            sy += m * double(user.V[size_t(i)*3+1]);
            w += m;
        }
        return w > 0 ? sy / w : 0.0;
    };
    // jaw deformation lives in the lower half (y<0) → its moved-mass centroid Y
    // is clearly below the smile's (which is centred at y≈0).
    EXPECT_LT(movedCentroidY(r.shapes[1]), movedCentroidY(r.shapes[0]));
}

// RBF pre-warp: anchors land exactly on their targets and the space between
// interpolates smoothly (a pure translation of all anchors translates the
// whole mesh).
TEST(FaceRigger, RbfWarpInterpolatesAnchors)
{
    const Grid g = makeGrid(8, 2.0f, 0.1f);
    const int n = int(g.V.size()/3);

    // pure translation: 5 anchors all displaced by (0.3, -0.2, 0.1)
    std::vector<FaceRig::NricpLandmark> anchors;
    const int picks[5] = {0, 7, n/2, n-8, n-1};
    for (int p : picks) {
        FaceRig::NricpLandmark a;
        a.tmplVertex = p;
        a.target = {g.V[size_t(p)*3] + 0.3f, g.V[size_t(p)*3+1] - 0.2f,
                    g.V[size_t(p)*3+2] + 0.1f};
        anchors.push_back(a);
    }
    const auto warped = FaceRig::rbfWarpByAnchors(g.V, anchors);
    ASSERT_EQ(warped.size(), g.V.size());
    // anchors land exactly (affine part reproduces the translation)
    for (int p : picks) {
        EXPECT_NEAR(warped[size_t(p)*3],   g.V[size_t(p)*3] + 0.3f, 1e-3f);
        EXPECT_NEAR(warped[size_t(p)*3+1], g.V[size_t(p)*3+1] - 0.2f, 1e-3f);
        EXPECT_NEAR(warped[size_t(p)*3+2], g.V[size_t(p)*3+2] + 0.1f, 1e-3f);
    }
    // a pure-translation anchor set translates EVERY vertex (thin-plate exact
    // for affine displacement fields)
    for (int v = 0; v < n; ++v) {
        EXPECT_NEAR(warped[size_t(v)*3],   g.V[size_t(v)*3] + 0.3f, 1e-2f);
        EXPECT_NEAR(warped[size_t(v)*3+1], g.V[size_t(v)*3+1] - 0.2f, 1e-2f);
    }
    // too few anchors → empty (caller falls back to the unwarped template)
    anchors.resize(3);
    EXPECT_TRUE(FaceRig::rbfWarpByAnchors(g.V, anchors).empty());
}

TEST(FaceRigger, RejectsNonFaceMesh)
{
    // template = bumpy plane with a shape
    const Grid tmpl = makeGrid(10, 2.0f, 0.12f);
    std::vector<float> shape(tmpl.V.size(), 0.0f);
    for (size_t i = 2; i < shape.size(); i += 3) shape[i] = 0.1f;
    const QString path = tempTemplatePath();
    ASSERT_TRUE(writeSyntheticTemplate(path, tmpl, {{"jawOpen", shape}}));
    FaceRig::ArkitTemplate at;
    ASSERT_TRUE(at.load(path));

    // user = a closed sphere: a fundamentally different topology/shape than the
    // open plane template. NRICP cannot wrap a plane around a sphere cleanly, so
    // the fit residual stays high and the humanoid-only guard rejects it.
    Grid user;
    {
        const int nlat = 12, nlon = 16;
        for (int i = 0; i <= nlat; ++i) {
            const float th = 3.14159265358979f * float(i) / nlat;
            for (int j = 0; j < nlon; ++j) {
                const float ph = 2.0f * 3.14159265358979f * float(j) / nlon;
                user.V.insert(user.V.end(),
                    {std::sin(th)*std::cos(ph), std::cos(th), std::sin(th)*std::sin(ph)});
            }
        }
        for (int i = 0; i < nlat; ++i)
            for (int j = 0; j < nlon; ++j) {
                const int a = i*nlon + j, b = i*nlon + (j+1)%nlon;
                const int c = (i+1)*nlon + j, d = (i+1)*nlon + (j+1)%nlon;
                user.F.insert(user.F.end(), {a, b, c});
                user.F.insert(user.F.end(), {b, d, c});
            }
    }
    FaceRig::FaceRigOptions opts;
    opts.maxFitResidualPct = 3.0;
    const auto r = FaceRig::buildFaceRig(user.V, user.F, at, opts);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.error.find("face"), std::string::npos);
}

// #1059 — a DISCONNECTED satellite island must move with the main surface.
//
// The template is split into a fitted main component plus satellites (mouth
// interior, teeth, eyeballs, lashes), which have no correspondence of their
// own and are placed by transporting the main surface's displacement. The
// original rule fitted an affine to the 60 main verts nearest the island
// CENTROID, which extrapolated badly for an island sitting off the surface:
// on the real ICT template every satellite lagged the main surface on
// jawOpen (0.898-0.915 vs 1.008) and some verts received NO motion at all.
//
// Here a small island floats well OFF the plane (so its nearest main verts are
// a distant, nearly co-planar patch — the ill-conditioned case) directly above
// a region the shape translates uniformly. Any correct placement rule carries
// the island by that same translation.
//
// The offset is load-bearing and was chosen by measurement, not taste. Both
// rules were built into a harness and swept, reading the island's ratio to the
// target translation:
//
//   island offset   affine (old)   blend (new)
//        0.5            0.967         0.985
//        0.9            0.930         0.981
//        1.5            0.858         0.979
//        2.5            0.726         0.980
//        6.0            0.412         0.986
//
// The blend is flat; the affine decays with distance (non-monotonically —
// that wobble IS the ill-conditioning). An earlier draft of this test used
// 0.9 and PASSED against the old broken code, pinning nothing. At 2.5 the old
// rule misses the band on every vertex and the new one clears it on every
// vertex, so this test actually fails if the affine is restored.
TEST(FaceRigger, DisconnectedSatelliteIslandTracksTheMainSurface)
{
    Grid tmpl = makeGrid(12, 2.0f, 0.12f);
    const int mainVerts = int(tmpl.V.size()/3);

    // A satellite: a small quad floating above the +y half of the plane,
    // sharing NO vertex or edge with it.
    const int islandBase = mainVerts;
    const float zOff = 2.5f;   // see the sweep above — separates the two rules
    for (float dy : {0.30f, 0.55f})
        for (float dx : {-0.12f, 0.12f})
            tmpl.V.insert(tmpl.V.end(), {dx, dy, zOff});
    tmpl.F.insert(tmpl.F.end(), {islandBase, islandBase+1, islandBase+2});
    tmpl.F.insert(tmpl.F.end(), {islandBase+1, islandBase+3, islandBase+2});
    const int totalVerts = int(tmpl.V.size()/3);

    // Shape: translate the whole +y half (island included) by a constant.
    // A uniform translation is the cleanest possible target — every correct
    // rule must reproduce it exactly, so a shortfall is unambiguous.
    const float kShift = 0.20f;
    std::vector<float> shape(tmpl.V.size(), 0.0f);
    for (int i = 0; i < totalVerts; ++i)
        if (tmpl.V[size_t(i)*3+1] > 0.0f) shape[size_t(i)*3+2] = kShift;

    const QString path = tempTemplatePath();
    ASSERT_TRUE(writeSyntheticTemplate(path, tmpl, {{"jawOpen", shape}}));

    FaceRig::ArkitTemplate at;
    QString err;
    ASSERT_TRUE(at.load(path, &err)) << err.toStdString();

    // The user mesh is the same surface PLUS the same island, retessellated
    // on the main surface only (so the fit is tight and the island is again a
    // separate component).
    Grid user = makeGrid(16, 2.0f, 0.12f);
    const int userMain = int(user.V.size()/3);
    for (float dy : {0.30f, 0.55f})
        for (float dx : {-0.12f, 0.12f})
            user.V.insert(user.V.end(), {dx, dy, zOff});
    user.F.insert(user.F.end(), {userMain, userMain+1, userMain+2});
    user.F.insert(user.F.end(), {userMain+1, userMain+3, userMain+2});

    const auto r = FaceRig::buildFaceRig(user.V, user.F, at);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(int(r.shapes.size()), 1);
    const auto& sh = r.shapes[0];

    // Every island vertex must pick up essentially the full translation.
    // Measured: the blend gives 0.980 of it, the old affine 0.726. A
    // displacement blend cannot fall short by much, because it can only ever
    // return a convex combination of displacements that actually occurred
    // (all of which are exactly +kShift here).
    int checked = 0;
    for (int i = userMain; i < int(user.V.size()/3); ++i) {
        ASSERT_LT(size_t(i)*3+2, sh.userDeltas.size());
        const float dz = sh.userDeltas[size_t(i)*3+2];
        EXPECT_TRUE(std::isfinite(dz));
        // Band 0.90..1.10. Measured 0.980 here, so there is ~8% of headroom
        // for solver noise, while the old affine's 0.726 misses by a wide
        // margin — the gap is big enough that the bound need not be loose.
        EXPECT_GT(dz, 0.90f * kShift)
            << "island vertex " << i << " lags the surface it sits on (dz=" << dz << ")";
        EXPECT_LT(dz, 1.10f * kShift)
            << "island vertex " << i << " overshoots (dz=" << dz << ")";
        ++checked;
    }
    EXPECT_EQ(checked, 4) << "the island must survive into the user shape";
}

// A satellite island must scale WITH the head, not stay behind it.
//
// Found in review of #1060. The satellite placement blends the displacements
// the main surface underwent. Under a global SCALE that displacement is
// (s-1)*p — POSITION-DEPENDENT — so averaging it and adding the result to a
// satellite at q gives q + (s-1)*mean(mainPositions) instead of s*q: an eye
// or tooth sitting away from the main surface's centroid keeps roughly its
// original place and size while the head grows around it. NRICP's bbox
// prealign produces exactly this whenever the user mesh is in different units.
//
// Measured on the real template before the fix, placing 12,657 satellites
// under a pure scale (error against the exact s*p): 0.199 mean at s=1.05,
// 3.98 mean / 84% relative at s=2.0. The fix extracts the global similarity,
// applies it exactly, and blends only the residual.
//
// Here the user mesh is the template scaled up, so the whole rig — island
// included — should simply scale. The island is placed by the blend, so if
// the similarity is not preserved it lands short.
TEST(FaceRigger, SatelliteIslandFollowsAGlobalScale)
{
    Grid tmpl = makeGrid(12, 2.0f, 0.12f);
    const int islandBase = int(tmpl.V.size()/3);
    // The island must sit FAR from the main surface's centroid. The dropped
    // term is (s-1)*(mean(mainPositions) - q), so an island near the centroid
    // shows no error at all — an earlier draft put it at z=1.6, close to the
    // plane's centre, and PASSED against a build with the similarity disabled.
    for (float dy : {0.30f, 0.55f})
        for (float dx : {-0.12f, 0.12f})
            tmpl.V.insert(tmpl.V.end(), {dx + 6.0f, dy + 6.0f, 6.0f});
    tmpl.F.insert(tmpl.F.end(), {islandBase, islandBase+1, islandBase+2});
    tmpl.F.insert(tmpl.F.end(), {islandBase+1, islandBase+3, islandBase+2});

    // A shape that moves the island along with the +y half.
    const float kShift = 0.20f;
    std::vector<float> shape(tmpl.V.size(), 0.0f);
    for (int i = 0; i < int(tmpl.V.size()/3); ++i)
        if (tmpl.V[size_t(i)*3+1] > 0.0f) shape[size_t(i)*3+2] = kShift;

    const QString path = tempTemplatePath();
    ASSERT_TRUE(writeSyntheticTemplate(path, tmpl, {{"jawOpen", shape}}));
    FaceRig::ArkitTemplate at;
    QString err;
    ASSERT_TRUE(at.load(path, &err)) << err.toStdString();

    // The user mesh is the SAME geometry scaled about the origin — the case
    // that produces a pure global scale in the fit.
    const float kScale = 2.0f;
    Grid user = tmpl;
    for (float& v : user.V) v *= kScale;

    const auto r = FaceRig::buildFaceRig(user.V, user.F, at);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_EQ(int(r.shapes.size()), 1);
    const auto& sh = r.shapes[0];

    // The island's motion must scale with the mesh: kShift * kScale. Getting
    // the UNSCALED kShift back is the symptom of a dropped similarity.
    const float want = kShift * kScale;
    int checked = 0;
    for (int i = islandBase; i < int(user.V.size()/3); ++i) {
        ASSERT_LT(size_t(i)*3+2, sh.userDeltas.size());
        const float dz = sh.userDeltas[size_t(i)*3+2];
        EXPECT_TRUE(std::isfinite(dz));
        EXPECT_GT(dz, 0.80f * want)
            << "island vertex " << i << " did not scale with the head (dz="
            << dz << ", want ~" << want << ")";
        EXPECT_LT(dz, 1.20f * want)
            << "island vertex " << i << " overshot (dz=" << dz << ")";
        ++checked;
    }
    EXPECT_EQ(checked, 4);
}

// #1061 — a vertex must not match a triangle on the WRONG surface where two
// surfaces nearly touch.
//
// This is env-gated on the REAL ICT template rather than synthetic, because
// synthetic geometry did not reproduce the bug. A flat plane with a parallel
// floating patch was tried at four separation heights and a proximity-only
// build produced IDENTICAL output to the fixed one every time — the test
// would have pinned nothing. The real template has whatever the synthetic
// case lacks (curvature, a genuinely interleaved seam), and the failure is
// sharp and reproducible there.
//
// What the fix does: the resample seeds its triangle search from the nearest
// correspondence VERTEX and considers only triangles incident to it, so the
// seed decides which SURFACE is searched. On the real template the
// mouth-interior island passes ~0.14 from the outer lip, and every triangle
// incident to an island vertex is an island triangle (measured 7 of 7, 5 of
// 5, 7 of 7) — so three lip vertices seeded on the island, the correct lip
// triangle was never a candidate, and they inherited its ZERO motion while
// every neighbour moved ~4.0. Choosing the seed by NORMAL AGREEMENT fixes it:
// the island scores -0.974 against the query normal, the correct
// main-surface vertex +0.993, and the latter is also nearer (0.118 v 0.144).
//
// Measured on the control mesh (template rigged against itself), jawOpen:
//
//                        tears>1.0   worst tear   frozen lip verts
//   before (master)        1182        4.018            -
//   satellite fix (#1060)   372        4.076            3
//   + this seeding fix       60        1.885            0
//
// Set QTMESH_FACERIG_TEMPLATE to the packed arkit_template.bin to run it.
TEST(FaceRigger, RealTemplateHasNoFrozenLipVertices)
{
    const QByteArray tp = qgetenv("QTMESH_FACERIG_TEMPLATE");
    if (tp.isEmpty()) {
        // A skipped test counts as a suite failure in this CI harness.
        SUCCEED() << "QTMESH_FACERIG_TEMPLATE not set — real template not exercised";
        return;
    }
    FaceRig::ArkitTemplate at;
    QString err;
    ASSERT_TRUE(at.load(QString::fromUtf8(tp), &err)) << err.toStdString();
    ASSERT_GT(at.vertexCount(), 10000);

    // The template rigged against ITSELF: every shape should reproduce
    // exactly, so any vertex that fails to move is unambiguously a defect
    // rather than fit error.
    const std::vector<float>& V = at.neutral();
    const std::vector<int>&   F = at.faces();
    const auto r = FaceRig::buildFaceRig(V, F, at);
    ASSERT_TRUE(r.ok) << r.error;
    ASSERT_FALSE(r.shapes.empty());

    const FaceRig::FaceRigShape* jaw = nullptr;
    for (const auto& sh : r.shapes)
        if (sh.name == QStringLiteral("jawOpen")) { jaw = &sh; break; }
    ASSERT_NE(jaw, nullptr) << "the template must carry jawOpen";

    // Per-vertex 1-ring from the template topology.
    const int nv = int(V.size() / 3);
    // NB braces + resize, not parens: `vector<vector<int>> ring(size_t(nv))`
    // is the most vexing parse — the compiler reads it as a declaration of a
    // FUNCTION taking size_t. This bites repeatedly in this file.
    std::vector<std::vector<int>> ring{};
    ring.resize(size_t(nv));
    for (size_t f = 0; f + 2 < F.size(); f += 3) {
        const int a = F[f], b = F[f+1], c = F[f+2];
        if (a < 0 || b < 0 || c < 0 || a >= nv || b >= nv || c >= nv) continue;
        ring[size_t(a)].push_back(b); ring[size_t(b)].push_back(a);
        ring[size_t(b)].push_back(c); ring[size_t(c)].push_back(b);
        ring[size_t(c)].push_back(a); ring[size_t(a)].push_back(c);
    }
    auto mag = [&](int i) {
        if (size_t(i)*3+2 >= jaw->userDeltas.size()) return 0.0;
        const float* d = &jaw->userDeltas[size_t(i)*3];
        return std::sqrt(double(d[0])*d[0] + double(d[1])*d[1] + double(d[2])*d[2]);
    };

    // A FROZEN vertex: still, while at least 5 of its neighbours move a lot.
    // That is the exact signature of matching a wrong-surface triangle, and
    // it is what produces the visible spike.
    int frozen = 0;
    for (int i = 0; i < nv; ++i) {
        if (ring[size_t(i)].size() < 5) continue;
        if (mag(i) > 0.05) continue;
        int movingNb = 0;
        for (const int n : ring[size_t(i)]) if (mag(n) > 2.0) ++movingNb;
        if (movingNb >= 5) ++frozen;
    }
    EXPECT_EQ(frozen, 0)
        << frozen << " vertices are frozen while 5+ of their neighbours move "
           ">2.0 — they matched a triangle on the wrong surface (#1061)";
}
