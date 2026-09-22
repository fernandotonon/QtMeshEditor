#include "LatticeDeformer.h"

#include <QJsonArray>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Lattice {

namespace {
constexpr int kMinRes = 2;
constexpr int kMaxRes = 16;

int clampRes(int n) { return std::clamp(n, kMinRes, kMaxRes); }

/// Linear extrapolation ghost for Catmull-Rom end handling — keeps linear
/// precision exact at the boundaries (a duplicated end point halves the end
/// tangent and would bend a rest lattice near its shell).
inline int ghostIndex(int i, int n) { return std::clamp(i, 0, n - 1); }
} // namespace

QString interpolationId(Interpolation interp)
{
    switch (interp) {
    case Interpolation::Linear: return QStringLiteral("linear");
    case Interpolation::Smooth: return QStringLiteral("smooth");
    case Interpolation::Bezier: return QStringLiteral("bezier");
    }
    return QStringLiteral("smooth");
}

bool interpolationFromId(const QString& id, Interpolation& out)
{
    const QString s = id.trimmed().toLower();
    if (s == QLatin1String("linear")) { out = Interpolation::Linear; return true; }
    if (s == QLatin1String("smooth") || s == QLatin1String("cardinal") || s == QLatin1String("catmull-rom")) {
        out = Interpolation::Smooth; return true;
    }
    if (s == QLatin1String("bezier") || s == QLatin1String("bernstein")) { out = Interpolation::Bezier; return true; }
    return false;
}

// ---------------------------------------------------------------------------
// 1-D bases
// ---------------------------------------------------------------------------

Basis basisLinear(float s, int n)
{
    const int cells = n - 1;
    float x = s * cells;
    int c = static_cast<int>(std::floor(x));
    c = std::clamp(c, 0, cells - 1);
    const float f = std::clamp(x - c, 0.0f, 1.0f);
    return {{c, 1.0f - f}, {c + 1, f}};
}

Basis basisSmooth(float s, int n)
{
    if (n == 2) return basisLinear(s, n); // a 2-point axis has no curvature to speak of
    const int cells = n - 1;
    float x = s * cells;
    int c = static_cast<int>(std::floor(x));
    c = std::clamp(c, 0, cells - 1);
    const float t = std::clamp(x - c, 0.0f, 1.0f);
    const float t2 = t * t, t3 = t2 * t;
    // Catmull-Rom (cardinal, tension 0.5) weights for P[c-1], P[c], P[c+1], P[c+2].
    const float w0 = 0.5f * (-t3 + 2.0f * t2 - t);
    const float w1 = 0.5f * (3.0f * t3 - 5.0f * t2 + 2.0f);
    const float w2 = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
    const float w3 = 0.5f * (t3 - t2);

    Basis b;
    b.reserve(4);
    auto add = [&](int i, float w) {
        if (w == 0.0f) return;
        for (auto& e : b) { if (e.first == i) { e.second += w; return; } }
        b.emplace_back(i, w);
    };
    // Ghost points: P[-1] = 2P[0] − P[1], P[n] = 2P[n−1] − P[n−2] (linear extrapolation).
    if (c - 1 < 0) { add(0, 2.0f * w0); add(1, -w0); } else add(c - 1, w0);
    add(c, w1);
    add(c + 1, w2);
    if (c + 2 > n - 1) { add(n - 1, 2.0f * w3); add(n - 2, -w3); } else add(c + 2, w3);
    return b;
}

Basis basisBezier(float s, int n)
{
    const int deg = n - 1;
    const float x = std::clamp(s, 0.0f, 1.0f);
    Basis b;
    b.reserve(n);
    // Bernstein B_i^deg(x) via iterative binomial coefficients (double for the
    // C(15, k) range — fits comfortably).
    double binom = 1.0;
    for (int i = 0; i <= deg; ++i) {
        if (i > 0) binom = binom * (deg - i + 1) / i;
        const double w = binom * std::pow(static_cast<double>(x), i) * std::pow(1.0 - x, deg - i);
        if (w != 0.0) b.emplace_back(i, static_cast<float>(w));
    }
    return b;
}

// ---------------------------------------------------------------------------
// Grid
// ---------------------------------------------------------------------------

Grid Grid::fromBounds(const Ogre::AxisAlignedBox& bounds, int nx, int ny, int nz, float padding)
{
    Grid g;
    g.nx = clampRes(nx); g.ny = clampRes(ny); g.nz = clampRes(nz);
    Ogre::Vector3 mn = bounds.isNull() || bounds.isInfinite() ? Ogre::Vector3(-0.5f) : bounds.getMinimum();
    Ogre::Vector3 mx = bounds.isNull() || bounds.isInfinite() ? Ogre::Vector3(0.5f) : bounds.getMaximum();
    Ogre::Vector3 ext = mx - mn;
    const float maxExt = std::max({ext.x, ext.y, ext.z, 1e-4f});
    // A flat axis (plane / decal) gets a sliver of thickness so normalisation
    // is finite and the control points don't all coincide.
    const float minExt = maxExt * 0.02f;
    for (int a = 0; a < 3; ++a) {
        if (ext[a] < minExt) {
            const float c = 0.5f * (mn[a] + mx[a]);
            mn[a] = c - 0.5f * minExt;
            mx[a] = c + 0.5f * minExt;
            ext[a] = minExt;
        }
    }
    const float pad = std::max(0.0f, padding) * maxExt;
    g.origin = mn - Ogre::Vector3(pad);
    g.size = ext + Ogre::Vector3(2.0f * pad);
    g.reset();
    return g;
}

void Grid::coordsOf(int index, int& i, int& j, int& k) const
{
    i = index % nx;
    j = (index / nx) % ny;
    k = index / (nx * ny);
}

bool Grid::isValid() const
{
    return nx >= kMinRes && ny >= kMinRes && nz >= kMinRes
        && nx <= kMaxRes && ny <= kMaxRes && nz <= kMaxRes
        && size.x > 0.0f && size.y > 0.0f && size.z > 0.0f
        && static_cast<int>(points.size()) == pointCount();
}

Ogre::Vector3 Grid::restPoint(int i, int j, int k) const
{
    return origin + Ogre::Vector3(size.x * (static_cast<float>(i) / (nx - 1)),
                                  size.y * (static_cast<float>(j) / (ny - 1)),
                                  size.z * (static_cast<float>(k) / (nz - 1)));
}

void Grid::reset()
{
    points.resize(static_cast<size_t>(pointCount()));
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                points[static_cast<size_t>(index(i, j, k))] = restPoint(i, j, k);
}

bool Grid::isAtRest(float eps) const
{
    if (static_cast<int>(points.size()) != pointCount()) return false;
    for (int k = 0; k < nz; ++k)
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i)
                if (points[static_cast<size_t>(index(i, j, k))].squaredDistance(restPoint(i, j, k)) > eps * eps)
                    return false;
    return true;
}

Ogre::Vector3 Grid::toLocal(const Ogre::Vector3& p) const
{
    return Ogre::Vector3((p.x - origin.x) / size.x, (p.y - origin.y) / size.y, (p.z - origin.z) / size.z);
}

Ogre::Vector3 Grid::deform(const Ogre::Vector3& p) const
{
    if (!isValid()) return p;
    const Ogre::Vector3 l = toLocal(p);
    const Ogre::Vector3 lc(std::clamp(l.x, 0.0f, 1.0f), std::clamp(l.y, 0.0f, 1.0f), std::clamp(l.z, 0.0f, 1.0f));

    auto basis = [this](float s, int n) {
        switch (interpolation) {
        case Interpolation::Linear: return basisLinear(s, n);
        case Interpolation::Smooth: return basisSmooth(s, n);
        case Interpolation::Bezier: return basisBezier(s, n);
        }
        return basisSmooth(s, n);
    };
    const Basis bx = basis(lc.x, nx), by = basis(lc.y, ny), bz = basis(lc.z, nz);

    Ogre::Vector3 out = Ogre::Vector3::ZERO;
    for (const auto& [k, wk] : bz)
        for (const auto& [j, wj] : by)
            for (const auto& [i, wi] : bx)
                out += points[static_cast<size_t>(index(i, j, k))] * (wi * wj * wk);

    if (lc != l) {
        // Outside the shell: carry the displacement of the clamped boundary
        // point rather than extrapolating the basis.
        const Ogre::Vector3 restClamped = origin + lc * size;
        return p + (out - restClamped);
    }
    return out;
}

void Grid::deformAll(const std::vector<Ogre::Vector3>& rest, std::vector<Ogre::Vector3>& out) const
{
    out.resize(rest.size());
    for (size_t v = 0; v < rest.size(); ++v) out[v] = deform(rest[v]);
}

QJsonObject Grid::toJson() const
{
    QJsonObject o;
    o[QStringLiteral("schema")] = QString::fromLatin1(kJsonSchema);
    o[QStringLiteral("resolution")] = QJsonArray{nx, ny, nz};
    o[QStringLiteral("origin")] = QJsonArray{origin.x, origin.y, origin.z};
    o[QStringLiteral("size")] = QJsonArray{size.x, size.y, size.z};
    o[QStringLiteral("interpolation")] = interpolationId(interpolation);
    QJsonArray pts;
    for (const auto& p : points) { pts.append(p.x); pts.append(p.y); pts.append(p.z); }
    o[QStringLiteral("points")] = pts;
    return o;
}

bool Grid::fromJson(const QJsonObject& obj, Grid& out, QString* error)
{
    auto fail = [&](const QString& why) { if (error) *error = why; return false; };
    // A coordinate must be a JSON NUMBER that survives the float cast finite —
    // QJsonValue::toDouble() would turn a string/bool/null into 0 and a huge
    // value into inf, silently collapsing control points.
    auto readFloat = [](const QJsonValue& v, float& out) {
        if (!v.isDouble()) return false;
        const double d = v.toDouble();
        if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) return false;
        out = static_cast<float>(d);
        return true;
    };
    auto readVec3 = [&](const QString& key, Ogre::Vector3& v) {
        const QJsonArray a = obj.value(key).toArray();
        if (a.size() != 3) return false;
        return readFloat(a[0], v.x) && readFloat(a[1], v.y) && readFloat(a[2], v.z);
    };

    Grid g;
    const QJsonArray res = obj.value(QStringLiteral("resolution")).toArray();
    if (res.size() != 3) return fail(QStringLiteral("lattice: 'resolution' must be [nx, ny, nz]"));
    g.nx = res[0].toInt(); g.ny = res[1].toInt(); g.nz = res[2].toInt();
    if (g.nx < kMinRes || g.ny < kMinRes || g.nz < kMinRes || g.nx > kMaxRes || g.ny > kMaxRes || g.nz > kMaxRes)
        return fail(QStringLiteral("lattice: resolution out of range [%1, %2]").arg(kMinRes).arg(kMaxRes));
    if (!readVec3(QStringLiteral("origin"), g.origin)) return fail(QStringLiteral("lattice: bad 'origin'"));
    if (!readVec3(QStringLiteral("size"), g.size)) return fail(QStringLiteral("lattice: bad 'size'"));
    if (g.size.x <= 0.0f || g.size.y <= 0.0f || g.size.z <= 0.0f)
        return fail(QStringLiteral("lattice: 'size' components must be > 0"));
    const QString interp = obj.value(QStringLiteral("interpolation")).toString(QStringLiteral("smooth"));
    if (!interpolationFromId(interp, g.interpolation))
        return fail(QStringLiteral("lattice: unknown interpolation '%1'").arg(interp));
    const QJsonArray pts = obj.value(QStringLiteral("points")).toArray();
    if (pts.size() != g.pointCount() * 3)
        return fail(QStringLiteral("lattice: 'points' must hold %1 floats (got %2)")
                        .arg(g.pointCount() * 3).arg(pts.size()));
    g.points.resize(static_cast<size_t>(g.pointCount()));
    for (int p = 0; p < g.pointCount(); ++p) {
        Ogre::Vector3& v = g.points[static_cast<size_t>(p)];
        if (!readFloat(pts[p * 3], v.x) || !readFloat(pts[p * 3 + 1], v.y) || !readFloat(pts[p * 3 + 2], v.z))
            return fail(QStringLiteral("lattice: 'points'[%1] is not a finite number").arg(p * 3));
    }
    out = g;
    return true;
}

} // namespace Lattice
