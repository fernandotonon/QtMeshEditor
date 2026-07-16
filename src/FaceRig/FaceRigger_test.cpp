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
