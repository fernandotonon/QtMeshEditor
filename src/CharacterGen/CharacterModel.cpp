#include "CharacterModel.h"

#include "../BodyFromPhoto/SamBodyRetarget.h"

#include <QColor>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QHash>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <limits>
#include <random>
#include <unordered_map>

namespace CharacterGen {

namespace {

constexpr int kIctChinVertex = 966;   // ICT-FaceKit Multi-PIE landmark 8
constexpr float kCmToM = 0.01f;

bool fail(QString* e, const QString& msg)
{
    if (e) *e = msg;
    return false;
}

struct Quat {
    float x, y, z, w;
};
Quat mul(const Quat& a, const Quat& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
std::array<float, 3> rotate(const Quat& q, const std::array<float, 3>& v)
{
    const float tx = 2 * (q.y * v[2] - q.z * v[1]), ty = 2 * (q.z * v[0] - q.x * v[2]), tz = 2 * (q.x * v[1] - q.y * v[0]);
    return {v[0] + q.w * tx + (q.y * tz - q.z * ty), v[1] + q.w * ty + (q.z * tx - q.x * tz), v[2] + q.w * tz + (q.x * ty - q.y * tx)};
}

float smoothstep(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

/// Per-position normals (area weighted) from the faces of a mesh whose layouts have normal == position.
std::vector<float> positionNormals(const std::vector<float>& pos, const std::vector<std::vector<uint32_t>>& faces,
                                   const std::vector<Dna::Mesh::Layout>& layouts)
{
    std::vector<float> n(pos.size(), 0.0f);
    for (const auto& f : faces) {
        for (size_t k = 1; k + 1 < f.size(); ++k) {
            const uint32_t a = layouts[f[0]].position, b = layouts[f[k]].position, c = layouts[f[k + 1]].position;
            const float e1[3] = {pos[b * 3] - pos[a * 3], pos[b * 3 + 1] - pos[a * 3 + 1], pos[b * 3 + 2] - pos[a * 3 + 2]};
            const float e2[3] = {pos[c * 3] - pos[a * 3], pos[c * 3 + 1] - pos[a * 3 + 1], pos[c * 3 + 2] - pos[a * 3 + 2]};
            const float cr[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            for (uint32_t v : {a, b, c})
                for (int i = 0; i < 3; ++i) n[v * 3 + i] += cr[i];
        }
    }
    for (size_t v = 0; v + 2 < n.size(); v += 3) {
        const float l = std::sqrt(n[v] * n[v] + n[v + 1] * n[v + 1] + n[v + 2] * n[v + 2]);
        if (l > 1e-20f) { n[v] /= l; n[v + 1] /= l; n[v + 2] /= l; }
        else { n[v] = 0; n[v + 1] = 1; n[v + 2] = 0; }
    }
    return n;
}


using EdgeKey = uint64_t;
EdgeKey edgeKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

/// Chain undirected edges (each vertex normally has two) into closed loops of vertex ids. Open chains are dropped.
std::vector<std::vector<uint32_t>> chainLoops(const std::vector<std::pair<uint32_t, uint32_t>>& edges)
{
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    for (const auto& e : edges) { adj[e.first].push_back(e.second); adj[e.second].push_back(e.first); }
    std::unordered_map<EdgeKey, bool> used;
    std::vector<std::vector<uint32_t>> loops;
    for (const auto& e : edges) {
        if (used[edgeKey(e.first, e.second)]) continue;
        std::vector<uint32_t> loop{e.first};
        uint32_t prev = e.first, cur = e.second;
        used[edgeKey(e.first, e.second)] = true;
        bool closed = false;
        for (size_t guard = 0; guard <= edges.size(); ++guard) {
            if (cur == loop.front()) { closed = true; break; }
            loop.push_back(cur);
            uint32_t next = UINT32_MAX;
            for (uint32_t n : adj[cur])
                if (n != prev && !used[edgeKey(cur, n)]) { next = n; break; }
            if (next == UINT32_MAX) {   // maybe the way back to the start
                for (uint32_t n : adj[cur]) if (n == loop.front() && !used[edgeKey(cur, n)]) { next = n; break; }
            }
            if (next == UINT32_MAX) break;
            used[edgeKey(cur, next)] = true;
            prev = cur;
            cur = next;
        }
        if (closed && loop.size() >= 3) loops.push_back(std::move(loop));
    }
    return loops;
}


/// A closed ring seen from an axis: radius as a function of angle (piecewise linear, wraps around).
struct PolarRing {
    std::vector<std::pair<float, float>> pts;   // (angle, radius) sorted by angle
    bool valid() const { return pts.size() >= 3; }
    float at(float th) const
    {
        const size_t n = pts.size();
        size_t hi = 0;
        while (hi < n && pts[hi].first < th) ++hi;
        const size_t lo = (hi + n - 1) % n;
        hi %= n;
        float a0 = pts[lo].first, a1 = pts[hi].first, t = th;
        const float tp = 6.2831853f;
        if (a1 < a0) a1 += tp;
        if (t < a0) t += tp;
        const float u = a1 > a0 ? std::min(1.0f, std::max(0.0f, (t - a0) / (a1 - a0))) : 0.0f;
        return pts[lo].second + u * (pts[hi].second - pts[lo].second);
    }
    /// Build from scattered (x,z) points around the axis (cx,cz): the outermost point per angular bin; empty bins borrow a neighbour.
    static PolarRing fromScatter(const std::vector<std::array<float, 2>>& xz, float cx, float cz, int bins = 48)
    {
        std::vector<float> r(size_t(bins), -1.0f);
        for (const auto& q : xz) {
            const float dx = q[0] - cx, dz = q[1] - cz;
            int b = int((std::atan2(dz, dx) + 3.14159265f) / 6.2831853f * float(bins));
            b = std::min(bins - 1, std::max(0, b));
            r[size_t(b)] = std::max(r[size_t(b)], std::sqrt(dx * dx + dz * dz));
        }
        PolarRing out;
        for (int pass = 0; pass < bins; ++pass)
            for (int b = 0; b < bins; ++b)
                if (r[size_t(b)] < 0) {
                    const float l = r[size_t((b + bins - 1) % bins)], h = r[size_t((b + 1) % bins)];
                    if (l >= 0 && h >= 0) r[size_t(b)] = 0.5f * (l + h); else if (l >= 0) r[size_t(b)] = l; else if (h >= 0) r[size_t(b)] = h;
                }
        for (int b = 0; b < bins; ++b)
            if (r[size_t(b)] >= 0) out.pts.push_back({(float(b) + 0.5f) / float(bins) * 6.2831853f - 3.14159265f, r[size_t(b)]});
        return out;
    }
};

bool pointInPolygon(float x, float z, const std::vector<std::array<float, 2>>& poly)
{
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if (((poly[i][1] > z) != (poly[j][1] > z)) && (x < (poly[j][0] - poly[i][0]) * (z - poly[i][1]) / (poly[j][1] - poly[i][1]) + poly[i][0]))
            in = !in;
    return in;
}

/// The closed outline where the mesh crosses the horizontal plane y = planeY, as (x,z); the loop enclosing (cx,cz) with the smallest
/// area (the neck, not the shoulders or an arm). Empty when none encloses it.
std::vector<std::array<float, 2>> sliceLoopAround(const std::vector<float>& posM, const std::vector<int>& faces, float planeY, float cx, float cz)
{
    std::unordered_map<EdgeKey, uint32_t> id;
    std::vector<std::array<float, 2>> pt;
    std::vector<std::pair<uint32_t, uint32_t>> segs;
    auto crossing = [&](int a, int b) -> uint32_t {
        auto it = id.find(edgeKey(uint32_t(a), uint32_t(b)));
        if (it != id.end()) return it->second;
        const float ya = posM[size_t(a) * 3 + 1], yb = posM[size_t(b) * 3 + 1];
        const float t = (planeY - ya) / (yb - ya);
        pt.push_back({posM[size_t(a) * 3] + t * (posM[size_t(b) * 3] - posM[size_t(a) * 3]),
                      posM[size_t(a) * 3 + 2] + t * (posM[size_t(b) * 3 + 2] - posM[size_t(a) * 3 + 2])});
        const uint32_t n = uint32_t(pt.size() - 1);
        id.emplace(edgeKey(uint32_t(a), uint32_t(b)), n);
        return n;
    };
    for (size_t f = 0; f + 2 < faces.size(); f += 3) {
        uint32_t c[2] = {0, 0};
        int nc = 0;
        for (int k = 0; k < 3; ++k) {
            const int a = faces[f + size_t(k)], b = faces[f + size_t((k + 1) % 3)];
            const float da = posM[size_t(a) * 3 + 1] - planeY, db = posM[size_t(b) * 3 + 1] - planeY;
            if ((da < 0) != (db < 0) && da != db) { if (nc < 2) c[nc] = crossing(a, b); ++nc; }
        }
        if (nc == 2 && c[0] != c[1]) segs.push_back({c[0], c[1]});
    }
    std::vector<std::array<float, 2>> best;
    double bestArea = 1e30;
    for (const auto& loop : chainLoops(segs)) {
        std::vector<std::array<float, 2>> poly;
        double a = 0;
        for (size_t i = 0; i < loop.size(); ++i) {
            poly.push_back(pt[loop[i]]);
            const auto& u = pt[loop[i]]; const auto& v = pt[loop[(i + 1) % loop.size()]];
            a += double(u[0]) * v[1] - double(v[0]) * u[1];
        }
        a = std::fabs(0.5 * a);
        if (pointInPolygon(cx, cz, poly) && a < bestArea) { bestArea = a; best = std::move(poly); }
    }
    return best;
}

/// Signed area of a loop in the x/z plane (positive = counter-clockwise seen from +Y... sign only matters for comparing two loops).
float loopArea(const std::vector<uint32_t>& loop, const std::vector<float>& pos)
{
    double a = 0;
    for (size_t i = 0; i < loop.size(); ++i) {
        const size_t j = (i + 1) % loop.size();
        a += double(pos[size_t(loop[i]) * 3]) * pos[size_t(loop[j]) * 3 + 2] - double(pos[size_t(loop[j]) * 3]) * pos[size_t(loop[i]) * 3 + 2];
    }
    return float(0.5 * a);
}

/// Bridge two closed loops with a triangle strip (greedy shortest diagonal). Returns triangles as (index into A, index into B) pairs:
/// each triangle is {a0,a1,b0}+flag, expressed through the callback so the caller maps ids. Both loops must wind the same way.
struct StripTri { int ia0, ia1, ib0, ib1; bool aEdge; };   // aEdge: triangle (A[ia0], A[ia1], B[ib0]); else (A[ia0], B[ib1], B[ib0])
std::vector<StripTri> zipLoops(const std::vector<std::array<float, 3>>& A, const std::vector<std::array<float, 3>>& B)
{
    auto d2 = [](const std::array<float, 3>& x, const std::array<float, 3>& y) {
        return (x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) + (x[2] - y[2]) * (x[2] - y[2]);
    };
    const int nA = int(A.size()), nB = int(B.size());
    // start B at the vertex nearest A[0]
    int b0 = 0;
    float best = 1e30f;
    for (int i = 0; i < nB; ++i) { const float d = d2(A[0], B[size_t(i)]); if (d < best) { best = d; b0 = i; } }
    std::vector<StripTri> out;
    int ia = 0, ib = 0;   // steps taken along A and B
    while (ia < nA || ib < nB) {
        const int a = ia % nA, a1 = (ia + 1) % nA, b = (b0 + ib) % nB, b1 = (b0 + ib + 1) % nB;
        bool advanceA;
        if (ia >= nA) advanceA = false;
        else if (ib >= nB) advanceA = true;
        else advanceA = d2(A[size_t(a1)], B[size_t(b)]) <= d2(A[size_t(a)], B[size_t(b1)]);
        if (advanceA) { out.push_back({a, a1, b, b, true}); ++ia; }
        else { out.push_back({a, a, b, b1, false}); ++ib; }
    }
    return out;
}

QStringList jsonStrings(const QJsonValue& v)
{
    QStringList out;
    for (const auto& s : v.toArray()) out << s.toString();
    return out;
}

}  // namespace

const IctPart* Assets::part(const QString& name) const
{
    for (const auto& p : parts)
        if (p.name == name) return &p;
    return nullptr;
}

bool loadAssets(const QmcBundle& mhr, const QmcBundle& ict, Assets& a, QString* error)
{
    a = Assets();
    QString e;
    auto needF = [&](const QmcBundle& b, const char* n, std::vector<float>& out) {
        out = b.readFloat(QString::fromLatin1(n), &e);
        return !out.empty();
    };
    auto needI = [&](const QmcBundle& b, const char* n, std::vector<int32_t>& out) {
        out = b.readInt(QString::fromLatin1(n), &e);
        return !out.empty();
    };
    if (!needF(mhr, "base", a.mhrBase) || !needF(mhr, "shape_vectors", a.mhrShapeVectors) || !needI(mhr, "faces", a.mhrFaces)
        || !needI(mhr, "texcoord_faces", a.mhrTexFaces) || !needF(mhr, "texcoords", a.mhrTexcoords)
        || !needI(mhr, "parents", a.jointParents) || !needF(mhr, "joint_offsets", a.jointOffsets)
        || !needF(mhr, "joint_prerot", a.jointPrerot) || !needI(mhr, "skin_count", a.skinCount)
        || !needI(mhr, "skin_joint", a.skinJoint) || !needF(mhr, "skin_weight", a.skinWeight))
        return fail(error, QStringLiteral("MHR bundle: %1").arg(e));
    a.mhrVertices = int(a.mhrBase.size() / 3);
    const auto sv = mhr.shapeOf(QStringLiteral("shape_vectors"));
    a.mhrShapes = sv.size() == 3 ? int(sv[0]) : 0;
    const int J = int(a.jointParents.size());
    if (a.mhrVertices <= 0 || a.mhrShapes <= 0 || a.mhrShapeVectors.size() != size_t(a.mhrShapes) * a.mhrBase.size()
        || a.mhrFaces.size() != a.mhrTexFaces.size() || a.mhrFaces.size() % 3 != 0
        || a.jointOffsets.size() != size_t(J) * 3 || a.jointPrerot.size() != size_t(J) * 4
        || a.skinCount.size() != size_t(a.mhrVertices) || a.skinJoint.size() != a.skinWeight.size())
        return fail(error, QStringLiteral("MHR bundle: inconsistent array sizes"));
    const QJsonObject mmeta = QJsonDocument::fromJson(mhr.readBytes(QStringLiteral("meta"))).object();
    a.jointNames = jsonStrings(mmeta.value(QStringLiteral("joint_names")));
    if (a.jointNames.size() != J) return fail(error, QStringLiteral("MHR bundle: %1 joint names for %2 joints").arg(a.jointNames.size()).arg(J));
    for (int j = 0; j < J; ++j)
        if (a.jointParents[size_t(j)] >= j) return fail(error, QStringLiteral("MHR bundle: joint %1 is not after its parent").arg(j));
    a.skinStart.assign(size_t(a.mhrVertices) + 1, 0);
    for (int v = 0; v < a.mhrVertices; ++v) {
        if (a.skinCount[size_t(v)] < 0) return fail(error, QStringLiteral("MHR bundle: negative skin count"));
        a.skinStart[size_t(v) + 1] = a.skinStart[size_t(v)] + uint32_t(a.skinCount[size_t(v)]);
    }
    if (a.skinStart.back() != a.skinJoint.size()) return fail(error, QStringLiteral("MHR bundle: skin counts do not add up"));
    for (int32_t j : a.skinJoint)
        if (j < 0 || j >= J) return fail(error, QStringLiteral("MHR bundle: skin joint out of range"));
    for (int32_t f : a.mhrFaces)
        if (f < 0 || f >= a.mhrVertices) return fail(error, QStringLiteral("MHR bundle: face index out of range"));
    for (int32_t f : a.mhrTexFaces)
        if (f < 0 || size_t(f) * 2 + 1 >= a.mhrTexcoords.size()) return fail(error, QStringLiteral("MHR bundle: texcoord index out of range"));

    if (!mhr.shapeOf(QStringLiteral("arkit_delta")).empty()) {
        QString e2;
        a.mhrArkit = mhr.readFloat(QStringLiteral("arkit_delta"), &e2);
        if (a.mhrArkit.empty()) return fail(error, QStringLiteral("MHR bundle: arkit_delta: %1").arg(e2));
    }
    if (!mhr.shapeOf(QStringLiteral("eye_drop")).empty()) {
        const QByteArray eb = mhr.readBytes(QStringLiteral("eye_drop"), nullptr);
        a.mhrEyeDrop.assign(eb.begin(), eb.end());
    }
    if (!mhr.shapeOf(QStringLiteral("eye_open")).empty()) a.mhrEyeOpen = mhr.readFloat(QStringLiteral("eye_open"), nullptr);
    if (!needF(ict, "neutral", a.ictNeutral) || !needF(ict, "texcoords", a.ictTexcoords) || !needI(ict, "poly_size", a.polySize)
        || !needI(ict, "poly_vertex", a.polyVertex) || !needI(ict, "poly_texcoord", a.polyTexcoord)
        || !needF(ict, "identity", a.ictIdentity) || !needF(ict, "expression", a.ictExpression))
        return fail(error, QStringLiteral("ICT bundle: %1").arg(e));
    a.ictVertices = int(a.ictNeutral.size() / 3);
    const auto iv = ict.shapeOf(QStringLiteral("identity")), ev = ict.shapeOf(QStringLiteral("expression"));
    a.ictIdentities = iv.size() == 3 ? int(iv[0]) : 0;
    a.ictExpressions = ev.size() == 3 ? int(ev[0]) : 0;
    if (a.ictVertices <= 0 || a.ictIdentity.size() != size_t(a.ictIdentities) * a.ictNeutral.size()
        || a.ictExpression.size() != size_t(a.ictExpressions) * a.ictNeutral.size() || a.polyVertex.size() != a.polyTexcoord.size())
        return fail(error, QStringLiteral("ICT bundle: inconsistent array sizes"));
    a.polyStart.assign(a.polySize.size() + 1, 0);
    for (size_t p = 0; p < a.polySize.size(); ++p) {
        if (a.polySize[p] < 3) return fail(error, QStringLiteral("ICT bundle: degenerate polygon"));
        a.polyStart[p + 1] = a.polyStart[p] + uint32_t(a.polySize[p]);
    }
    if (a.polyStart.back() != a.polyVertex.size()) return fail(error, QStringLiteral("ICT bundle: polygon sizes do not add up"));
    for (int32_t v : a.polyVertex)
        if (v < 0 || v >= a.ictVertices) return fail(error, QStringLiteral("ICT bundle: polygon vertex out of range"));
    for (int32_t t : a.polyTexcoord)
        if (t < 0 || size_t(t) * 2 + 1 >= a.ictTexcoords.size()) return fail(error, QStringLiteral("ICT bundle: polygon texcoord out of range"));
    const QJsonObject imeta = QJsonDocument::fromJson(ict.readBytes(QStringLiteral("meta"))).object();
    a.expressionNames = jsonStrings(imeta.value(QStringLiteral("expression_names")));
    if (a.expressionNames.size() != a.ictExpressions)
        return fail(error, QStringLiteral("ICT bundle: %1 expression names for %2 shapes").arg(a.expressionNames.size()).arg(a.ictExpressions));
    for (const auto& pv : imeta.value(QStringLiteral("parts")).toArray()) {
        const QJsonObject po = pv.toObject();
        IctPart p;
        p.name = po.value(QStringLiteral("name")).toString();
        p.v0 = po.value(QStringLiteral("v0")).toInt();
        p.v1 = po.value(QStringLiteral("v1")).toInt();
        p.p0 = po.value(QStringLiteral("p0")).toInt();
        p.p1 = po.value(QStringLiteral("p1")).toInt();
        if (p.v0 < 0 || p.v1 >= a.ictVertices || p.p0 < 0 || p.p1 >= int(a.polySize.size()))
            return fail(error, QStringLiteral("ICT bundle: part '%1' is out of range").arg(p.name));
        a.parts.push_back(p);
    }
    if (a.parts.empty() || kIctChinVertex >= a.ictVertices) return fail(error, QStringLiteral("ICT bundle: no parts"));
    return true;
}

bool loadAssetsFromDir(const QString& dir, Assets& out, QString* error)
{
    QmcBundle mhr, ict;
    QString e;
    if (!mhr.open(dir + QStringLiteral("/mhr_character.qmcb"), &e)) return fail(error, e);
    if (!ict.open(dir + QStringLiteral("/ict_character.qmcb"), &e)) return fail(error, e);
    return loadAssets(mhr, ict, out, error);
}

std::vector<float> randomCoefficients(uint32_t seed, int count, float sigma, int modes)
{
    std::vector<float> c(size_t(std::max(0, count)), 0.0f);
    std::mt19937 rng(seed);
    std::normal_distribution<float> n(0.0f, 1.0f);
    for (int i = 0; i < count; ++i) {
        const float g = std::clamp(n(rng), -3.0f, 3.0f);
        if (i < modes) c[size_t(i)] = g * sigma;
    }
    return c;
}

bool parseCoefficients(const QString& text, std::vector<float>& out, int limit, QString* error)
{
    for (const QString& item : text.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QStringList kv = item.split(QLatin1Char('='));
        bool ok1 = false, ok2 = false;
        const int idx = kv.size() == 2 ? kv[0].trimmed().toInt(&ok1) : -1;
        const float v = kv.size() == 2 ? kv[1].trimmed().toFloat(&ok2) : 0.0f;
        if (!ok1 || !ok2 || idx < 0 || idx >= limit || !std::isfinite(v)) {
            if (error) *error = QStringLiteral("bad coefficient '%1' (want index=value, index 0..%2)").arg(item).arg(limit - 1);
            return false;
        }
        if (out.size() < size_t(limit)) out.resize(size_t(limit), 0.0f);
        out[size_t(idx)] = v;
    }
    return true;
}

namespace {
const QHash<QString, QString>& colourTable(ColourKind kind)
{
    static const QHash<QString, QString> skin = {{"pale", "#f1cdb3"}, {"light", "#e0ac8c"}, {"tan", "#c68c63"}, {"brown", "#8d5a3b"}, {"dark", "#4f3224"}};
    static const QHash<QString, QString> eye = {{"blue", "#3f6fa8"}, {"green", "#4c8a5a"}, {"brown", "#5a3a22"}, {"hazel", "#8a6a2e"}, {"grey", "#7d8a94"}};
    return kind == ColourKind::Skin ? skin : eye;
}
}  // namespace

bool parseColour(const QString& text, ColourKind kind, Rgb* out)
{
    const QString named = colourTable(kind).value(text.trimmed().toLower());
    QColor c(named.isEmpty() ? text.trimmed() : named);
    if (!c.isValid()) return false;
    if (out) *out = {float(c.redF()), float(c.greenF()), float(c.blueF())};
    return true;
}

QStringList colourNames(ColourKind kind)
{
    QStringList n = colourTable(kind).keys();
    n.sort();
    return n;
}

bool surfaceForMesh(const QString& meshName, Surface* out)
{
    static const std::pair<const char*, Surface> table[] = {
        {"body", Surface::Skin}, {"head_skin", Surface::Skin}, {"head_mouth", Surface::Mouth}, {"head_teeth", Surface::Teeth},
        {"head_sclera", Surface::Sclera}, {"head_iris", Surface::Iris}, {"head_lashes", Surface::Lashes}};
    for (const auto& t : table)
        if (meshName == QLatin1String(t.first)) {
            if (out) *out = t.second;
            return true;
        }
    return false;
}

Dna::Rig buildRig(const Assets& a, const Params& p, BuildReport* report, QString* error)
{
    BuildReport local;
    BuildReport& rep = report ? *report : local;
    rep = BuildReport();
    Dna::Rig rig;
    const int J = int(a.jointParents.size());
    const int cNeck = a.joint(QStringLiteral("c_neck")), cHead = a.joint(QStringLiteral("c_head"));
    if (J == 0 || cNeck < 0 || cHead < 0) {
        fail(error, QStringLiteral("The MHR skeleton has no neck/head joint."));
        return rig;
    }

    // ---- skeleton (the zero pose: parent-relative offsets + prerotations) -------------------------------------------
    rig.descriptor.name = QStringLiteral("Character");
    rig.descriptor.translationUnit = QStringLiteral("m");
    rig.descriptor.coordinateSystem = QStringLiteral("x=left y=up z=front");
    rig.flipV = true;
    std::vector<std::array<float, 3>> gt(static_cast<size_t>(J));
    std::vector<Quat> gq(static_cast<size_t>(J));
    for (int j = 0; j < J; ++j) {
        const std::array<float, 3> lt = {a.jointOffsets[size_t(j) * 3], a.jointOffsets[size_t(j) * 3 + 1], a.jointOffsets[size_t(j) * 3 + 2]};
        const Quat lq = {a.jointPrerot[size_t(j) * 4], a.jointPrerot[size_t(j) * 4 + 1], a.jointPrerot[size_t(j) * 4 + 2], a.jointPrerot[size_t(j) * 4 + 3]};
        const int par = a.jointParents[size_t(j)];
        if (par < 0) { gt[size_t(j)] = lt; gq[size_t(j)] = lq; }
        else {
            const auto r = rotate(gq[size_t(par)], lt);
            const auto& pt = gt[size_t(par)];
            gt[size_t(j)] = {pt[0] + r[0], pt[1] + r[1], pt[2] + r[2]};
            gq[size_t(j)] = mul(gq[size_t(par)], lq);
        }
    }
    // Which joints are kept. MHR carries ~127 joints: the standard body (spine, limbs, fingers: the ones with a standard name),
    // plus procedural helpers (twist joints driven by its runtime), sub-joints (wrist twist, ankle sub-joints, metacarpals) and the
    // face joints. Animation tools drive plain rotations of a Mixamo-style skeleton and know nothing about the extras: left in, they
    // sit undriven between driven bones and the skin around them collapses into blades on forearms, hands and feet. So the default
    // keeps only the standard-named joints; everything else is FOLDED: its skin weights go to the nearest kept ancestor (exactly what
    // it does when undriven) and its children are re-parented to that ancestor, keeping their world positions.
    std::vector<char> keep(static_cast<size_t>(J), 1);
    for (int j = 0; j < J; ++j) {
        const QString& n = a.jointNames[j];
        if (p.standardSkeleton)
            keep[size_t(j)] = QString::fromStdString(SamBody::mhrAlias(n.toStdString())) != n;
        else if (p.foldHelperJoints)
            keep[size_t(j)] = !(n.contains(QLatin1String("_proc")) || n.endsWith(QLatin1String("_null")));
    }
    std::vector<int> foldTo(static_cast<size_t>(J), -1), newIndex(static_cast<size_t>(J), -1);   // kept ancestor-or-self; index in the rig
    int kept = 0;
    for (int j = 0; j < J; ++j) {
        const int par = a.jointParents[size_t(j)];
        foldTo[size_t(j)] = keep[size_t(j)] ? j : (par >= 0 ? foldTo[size_t(par)] : -1);
        if (keep[size_t(j)]) newIndex[size_t(j)] = kept++;
    }
    for (int j = 0; j < J; ++j) {
        if (!keep[size_t(j)]) continue;
        Dna::Joint dj;
        // standard (Mixamo-style) names for the main bones, so text-to-motion, retargeting and capture recognise the rig
        dj.name = (p.standardNames || p.standardSkeleton) ? QString::fromStdString(SamBody::mhrAlias(a.jointNames[j].toStdString())) : a.jointNames[j];
        const int par = a.jointParents[size_t(j)];
        const int keptParent = par >= 0 ? foldTo[size_t(par)] : -1;
        dj.parent = keptParent >= 0 ? newIndex[size_t(keptParent)] : -1;
        if (p.identityBones || p.standardSkeleton) {
            // World-aligned bones: every rest rotation is the identity and the offset is the world-space difference of the joint
            // positions (to the KEPT parent). Skinning at rest only depends on the positions, so the mesh is unchanged, but the rig
            // follows the convention the animation tools assume (a Mixamo rig's bones are identity at rest). MHR's own bones carry
            // per-bone prerotations that those tools do not account for.
            const std::array<float, 3> pp = keptParent < 0 ? std::array<float, 3>{0, 0, 0} : gt[size_t(keptParent)];
            dj.translation = {double(gt[size_t(j)][0] - pp[0]) * kCmToM, double(gt[size_t(j)][1] - pp[1]) * kCmToM,
                              double(gt[size_t(j)][2] - pp[2]) * kCmToM};
            dj.orientation = {1.0, 0.0, 0.0, 0.0};
        } else {
            const std::array<float, 3> lt = {a.jointOffsets[size_t(j) * 3], a.jointOffsets[size_t(j) * 3 + 1], a.jointOffsets[size_t(j) * 3 + 2]};
            dj.translation = {double(lt[0]) * kCmToM, double(lt[1]) * kCmToM, double(lt[2]) * kCmToM};
            dj.orientation = {a.jointPrerot[size_t(j) * 4 + 3], a.jointPrerot[size_t(j) * 4], a.jointPrerot[size_t(j) * 4 + 1], a.jointPrerot[size_t(j) * 4 + 2]};
        }
        rig.lodJointIndices.push_back(int(rig.joints.size()));
        rig.joints.push_back(dj);
    }
    rep.joints = int(rig.joints.size());
    if (newIndex[size_t(cHead)] < 0) {
        fail(error, QStringLiteral("The head joint is not part of the kept skeleton."));
        return rig;
    }
    // ---- body: shaped positions ------------------------------------------------------------------------------------
    const int V = a.mhrVertices;
    std::vector<float> shaped(a.mhrBase);
    for (int s = 0; s < a.mhrShapes && s < int(p.bodyShape.size()); ++s) {
        const float c = p.bodyShape[size_t(s)];
        if (c == 0.0f) continue;
        const float* vec = a.mhrShapeVectors.data() + size_t(s) * V * 3;
        for (size_t i = 0; i < shaped.size(); ++i) shaped[i] += c * vec[i];
    }

    // ---- head alignment: chin + top of the head, uniform scale -----------------------------------------------------
    // MHR chin = the lowest vertex of the front of the head (dominant joint is a head-ish one); ICT chin = Multi-PIE landmark 8.
    std::vector<char> headish(size_t(J), 0), neckish(size_t(J), 0);
    for (int j = 0; j < J; ++j) {
        const QString n = a.jointNames[j];
        headish[size_t(j)] = n.contains(QLatin1String("head")) || n.contains(QLatin1String("jaw")) || n.contains(QLatin1String("eye"))
                             || n.contains(QLatin1String("tongue")) || n.contains(QLatin1String("teeth"));
        neckish[size_t(j)] = n.contains(QLatin1String("neck"));
    }
    std::vector<int> dominant(size_t(V), -1);
    int chinVertex = -1;
    float topY = -1e9f;
    const float frontZ = gt[size_t(cHead)][2] + 4.0f;   // cm
    for (int v = 0; v < V; ++v) {
        topY = std::max(topY, shaped[size_t(v) * 3 + 1]);
        float best = 0;
        for (uint32_t k = a.skinStart[size_t(v)]; k < a.skinStart[size_t(v) + 1]; ++k)
            if (a.skinWeight[k] > best) { best = a.skinWeight[k]; dominant[size_t(v)] = a.skinJoint[k]; }
        const int dom = dominant[size_t(v)];
        if (dom < 0 || !headish[size_t(dom)] || a.mhrBase[size_t(v) * 3 + 2] < frontZ) continue;
        if (chinVertex < 0 || a.mhrBase[size_t(v) * 3 + 1] < a.mhrBase[size_t(chinVertex) * 3 + 1]) chinVertex = v;
    }
    if (chinVertex < 0) {
        fail(error, QStringLiteral("Could not find the chin of the MHR head."));
        return rig;
    }
    const float chinMy = shaped[size_t(chinVertex) * 3 + 1], chinMz = shaped[size_t(chinVertex) * 3 + 2];
    const float* nI = a.ictNeutral.data();
    const float chinIx = nI[size_t(kIctChinVertex) * 3], chinIy = nI[size_t(kIctChinVertex) * 3 + 1], chinIz = nI[size_t(kIctChinVertex) * 3 + 2];
    float topIy = -1e9f;
    for (int v = 0; v < a.ictVertices; ++v) topIy = std::max(topIy, nI[size_t(v) * 3 + 1]);
    const float s = (topY - chinMy) / (topIy - chinIy);
    rep.headScale = s;
    if (!(s > 0.3f && s < 3.0f)) {
        fail(error, QStringLiteral("The head alignment scale %1 is implausible.").arg(s));
        return rig;
    }
    (void)chinIx;
    // world (cm, MHR frame) of an ICT-space point
    auto toWorldCm = [&](float x, float y, float z) {
        return std::array<float, 3>{x * s, (y - chinIy) * s + chinMy, (z - chinIz) * s + chinMz};   // x: both models are symmetric about x = 0
    };
    const float neckCutY = (chinMy + (p.neckRimIct - chinIy) * s) * kCmToM;   // world y (m) of the neck cut (may move up, see below)
    rep.neckCutY = neckCutY;

    // ---- head: identity-shaped positions for every ICT vertex (world metres) ----------------------------------------
    std::vector<float> headPos(size_t(a.ictVertices) * 3);
    {
        std::vector<float> id(a.ictNeutral);
        for (int k = 0; k < a.ictIdentities && k < int(p.faceIdentity.size()) && !p.nativeHead; ++k) {   // (the MHR head is the face in native mode)
            const float c = p.faceIdentity[size_t(k)];
            if (c == 0.0f) continue;
            const float* vec = a.ictIdentity.data() + size_t(k) * a.ictNeutral.size();
            for (size_t i = 0; i < id.size(); ++i) id[i] += c * vec[i];
        }
        for (int v = 0; v < a.ictVertices; ++v) {
            const auto w = toWorldCm(id[size_t(v) * 3], id[size_t(v) * 3 + 1], id[size_t(v) * 3 + 2]);
            for (int i = 0; i < 3; ++i) headPos[size_t(v) * 3 + i] = w[i] * kCmToM;
        }
    }
    const float trimY = neckCutY - p.neckOverlap;
    // the polygons of the head's skin group that survive the neck trim (also the source of the rim loop for the seam)
    std::vector<int> skinPolys;
    for (const char* pn : {"face", "head_neck", "mouth_socket", "eye_socket_l", "eye_socket_r"}) {
        const IctPart* part = a.part(QString::fromLatin1(pn));
        if (!part) continue;
        for (int pi = part->p0; pi <= part->p1; ++pi) {
            const uint32_t b0 = a.polyStart[size_t(pi)], n = uint32_t(a.polySize[size_t(pi)]);
            bool below = false;
            for (uint32_t k = 0; k < n; ++k)
                if (headPos[size_t(a.polyVertex[b0 + k]) * 3 + 1] < trimY) { below = true; break; }
            if (!below) skinPolys.push_back(pi);
        }
    }

    // ---- neck seam + body ---------------------------------------------------------------------------------------------
    // The head's neck is a short funnel (ICT) and the body's neck is longer and set differently (MHR), so there is no clean ring
    // to weld. Instead: the body loses everything skinned to the head, plus the neck column — the vertices above the rim that
    // fall inside the head's neck footprint (an ellipse measured from the head's own rim band). The head's neck rows below the
    // chin are then snapped onto the nearest vertices of the body's cut edge, fading out towards the chin, so the two surfaces
    // meet on the same ring and nothing is left to see through. (MHR skins its neck mostly to the SPINE joint, which is why the
    // column is found geometrically and not by joint.)
    const float chinWy = chinMy * kCmToM;
    struct Ring { float cx = 0, cz = 0, rx = 0, rz = 0; int n = 0; };
    auto measure = [](const std::vector<std::array<float, 2>>& pts) {
        Ring r;
        if (pts.empty()) return r;
        float mnx = 1e9f, mxx = -1e9f, mnz = 1e9f, mxz = -1e9f;
        for (const auto& q : pts) { mnx = std::min(mnx, q[0]); mxx = std::max(mxx, q[0]); mnz = std::min(mnz, q[1]); mxz = std::max(mxz, q[1]); }
        r.cx = 0.5f * (mnx + mxx); r.cz = 0.5f * (mnz + mxz); r.rx = 0.5f * (mxx - mnx); r.rz = 0.5f * (mxz - mnz); r.n = int(pts.size());
        return r;
    };
    std::vector<std::array<float, 2>> headRing;
    for (const char* pn : {"face", "head_neck"})
        if (const IctPart* part = a.part(QString::fromLatin1(pn)))
            for (int v = part->v0; v <= part->v1; ++v) {
                const float y = headPos[size_t(v) * 3 + 1];
                if (y >= trimY && y <= trimY + 0.015f) headRing.push_back({headPos[size_t(v) * 3], headPos[size_t(v) * 3 + 2]});
            }
    const Ring rh = measure(headRing);
    const bool haveRing = !p.nativeHead && rh.n >= 6 && rh.rx > 1e-4f && rh.rz > 1e-4f;
    if (!haveRing && !p.nativeHead)
        rep.warnings << QStringLiteral("The head's neck ring could not be measured (%1 vertices); the seam is not matched.").arg(rh.n);

    // ---- the straight horizontal cut ------------------------------------------------------------------------------------
    // Cut the body on the plane of the head's neck rim (y = neckCutY). The outline where the body crosses that plane is its real
    // neck/shoulder cross-section; everything above the plane inside that outline goes (the MHR head and neck). The head's neck
    // rows below the chin are warped radially so the head's rim lands on that same outline - the neck flares along the body's own
    // slope into the shoulders - and the two rings are then joined vertex to vertex.
    std::vector<std::array<float, 2>> cutPoly;
    PolarRing bodyRing;
    if (haveRing) {
        std::vector<float> bodyM(shaped.size());
        for (size_t i = 0; i < shaped.size(); ++i) bodyM[i] = shaped[i] * kCmToM;
        cutPoly = sliceLoopAround(bodyM, a.mhrFaces, neckCutY, rh.cx, rh.cz);
        const PolarRing headRingPolar = PolarRing::fromScatter(headRing, rh.cx, rh.cz, 72);
        if (cutPoly.size() >= 8 && headRingPolar.valid()) {
            bodyRing = PolarRing::fromScatter(cutPoly, rh.cx, rh.cz, 72);
            if (bodyRing.valid()) {
                const float top = chinWy - 0.005f, bottom = neckCutY;
                for (int v = 0; v < a.ictVertices; ++v) {
                    float* hp = &headPos[size_t(v) * 3];
                    if (hp[1] >= top) continue;
                    float w = std::min(1.0f, std::max(0.0f, (top - hp[1]) / std::max(0.01f, top - bottom)));
                    w = w * w;   // flat at the chin end, steepest at the rim so the flare carries on into the body's own slope
                    const float dx = hp[0] - rh.cx, dz = hp[2] - rh.cz;
                    const float th = std::atan2(dz, dx);
                    const float ratio = std::min(6.0f, std::max(0.5f, bodyRing.at(th) / std::max(1e-4f, headRingPolar.at(th))));
                    const float k = 1.0f + w * (ratio - 1.0f);
                    hp[0] = rh.cx + dx * k;
                    hp[2] = rh.cz + dz * k;
                }
            }
        }
        if (!bodyRing.valid()) cutPoly.clear();
    }
    Dna::Mesh body;
    body.name = QStringLiteral("body");
    body.positions.resize(shaped.size());
    for (size_t i = 0; i < shaped.size(); ++i) body.positions[i] = shaped[i] * kCmToM;
    body.uvs = a.mhrTexcoords;
    if (p.nativeHead && p.openEyes && a.mhrEyeOpen.size() == body.positions.size())
        for (size_t i = 0; i < body.positions.size(); ++i) body.positions[i] += a.mhrEyeOpen[i] * kCmToM;

    std::vector<char> gone(size_t(V), 0);
    for (int v = 0; v < V; ++v) {
        const int dom = dominant[size_t(v)];
        if (p.nativeHead) continue;
        if (body.positions[size_t(v) * 3 + 1] <= neckCutY) continue;   // below the plane nothing is removed (no stray pockets)
        if (!cutPoly.empty()) {
            // inside the outline, or within a margin of it: the collar bones / trapezius rise above the plane just outside the neck's
            // own outline and would otherwise stand around the head's neck like a wall
            const float rx = body.positions[size_t(v) * 3] - rh.cx, rz = body.positions[size_t(v) * 3 + 2] - rh.cz;
            if (pointInPolygon(body.positions[size_t(v) * 3], body.positions[size_t(v) * 3 + 2], cutPoly)
                || std::sqrt(rx * rx + rz * rz) < p.neckRadiusRatio * 1.3f * bodyRing.at(std::atan2(rz, rx)) + 0.015f)
                gone[size_t(v)] = 1;
            else if (dom >= 0 && headish[size_t(dom)] && body.positions[size_t(v) * 3 + 1] > chinWy) gone[size_t(v)] = 1;
            continue;
        }
        if (dom >= 0 && headish[size_t(dom)]) { gone[size_t(v)] = 1; continue; }
        if (!haveRing) continue;
        const float dx = (body.positions[size_t(v) * 3] - rh.cx) / (p.neckRadiusRatio * rh.rx),
                    dz = (body.positions[size_t(v) * 3 + 2] - rh.cz) / (p.neckRadiusRatio * rh.rz);
        if (dx * dx + dz * dz < 1.0f) gone[size_t(v)] = 1;
    }
    if (!cutPoly.empty()) {
        // the surface just outside the hole is higher than the cut plane (the neck/shoulder slope): left alone it stands as a
        // ledge around the joint. Let it fall smoothly to the plane over a short band so the junction sits low and flush.
        const float band = 0.05f;
        for (int v = 0; v < V; ++v) {
            float& y = body.positions[size_t(v) * 3 + 1];
            if (gone[size_t(v)] || y <= neckCutY) continue;
            const float rx = body.positions[size_t(v) * 3] - rh.cx, rz = body.positions[size_t(v) * 3 + 2] - rh.cz;
            const float d = std::sqrt(rx * rx + rz * rz) - (p.neckRadiusRatio * 1.3f * bodyRing.at(std::atan2(rz, rx)) + 0.015f);
            if (d >= band) continue;
            float t = std::max(0.0f, d) / band;
            t = t * t * (3.0f - 2.0f * t);
            y = neckCutY + (y - neckCutY) * t;
        }
    }
    body.skin.resize(size_t(V));
    for (int v = 0; v < V; ++v) {
        std::vector<Dna::SkinInfluence> inf;
        for (uint32_t k = a.skinStart[size_t(v)]; k < a.skinStart[size_t(v) + 1]; ++k) {
            const int keptJ = foldTo[size_t(a.skinJoint[k])];
            if (keptJ < 0) continue;
            const uint16_t j = uint16_t(newIndex[size_t(keptJ)]);
            bool merged = false;
            for (auto& e : inf) if (e.joint == j) { e.weight += a.skinWeight[k]; merged = true; break; }
            if (!merged) inf.push_back({j, a.skinWeight[k]});
        }
        std::sort(inf.begin(), inf.end(), [](const auto& x, const auto& y) { return x.weight > y.weight; });
        if (inf.size() > 4) inf.resize(4);
        float sum = 0;
        for (auto& i : inf) sum += i.weight;
        if (sum > 0) for (auto& i : inf) i.weight /= sum;
        body.skin[size_t(v)] = std::move(inf);
    }
    std::vector<uint32_t> uvOfPosition(size_t(V), 0);
    {
        std::vector<char> have(size_t(V), 0);
        for (size_t i = 0; i < a.mhrFaces.size(); ++i)
            if (!have[size_t(a.mhrFaces[i])]) { have[size_t(a.mhrFaces[i])] = 1; uvOfPosition[size_t(a.mhrFaces[i])] = uint32_t(a.mhrTexFaces[i]); }
    }
    std::unordered_map<uint64_t, uint32_t> seenLayouts;
    std::vector<uint32_t> bridgeUv;
    std::vector<size_t> seamFaces;
    {
        auto& seen = seenLayouts;
        for (size_t f = 0; f + 2 < a.mhrFaces.size(); f += 3) {
            int removed = 0;
            for (int k = 0; k < 3; ++k) removed += gone[size_t(a.mhrFaces[f + k])] ? 1 : 0;
            if (removed >= 2) continue;
            if (p.nativeHead && p.openEyes && f / 3 < a.mhrEyeDrop.size() && a.mhrEyeDrop[f / 3]) continue;
            if (p.bustDepth > 0.0f) {
                float cy = 0;
                for (int k = 0; k < 3; ++k) cy += body.positions[size_t(a.mhrFaces[f + k]) * 3 + 1];
                if (cy / 3.0f < neckCutY - p.bustDepth) continue;
            }
            std::vector<uint32_t> face;
            for (int k = 0; k < 3; ++k) {
                const uint32_t pi = uint32_t(a.mhrFaces[f + k]), ti = uint32_t(a.mhrTexFaces[f + k]);
                const uint64_t key = (uint64_t(pi) << 32) | ti;
                auto it = seen.find(key);
                if (it == seen.end()) {
                    it = seen.emplace(key, uint32_t(body.layouts.size())).first;
                    body.layouts.push_back({pi, ti, pi});
                }
                face.push_back(it->second);
            }
            body.faces.push_back(std::move(face));
        }
    }
    // Soften the crease below the joint: relax the body rows just under the hole (the hole's own edge stays put, the strip is
    // stitched to it) so the body's slope eases into the head's flare instead of meeting it at an angle.
    if (!cutPoly.empty() && !p.bodyOnly) {
        std::vector<std::vector<int>> adj(static_cast<size_t>(V));
        std::vector<char> nearHole(static_cast<size_t>(V), 0);
        for (size_t f = 0; f + 2 < a.mhrFaces.size(); f += 3) {
            const int t[3] = {a.mhrFaces[f], a.mhrFaces[f + 1], a.mhrFaces[f + 2]};
            bool anyGone = false;
            for (int k = 0; k < 3; ++k) anyGone = anyGone || gone[size_t(t[k])];
            for (int k = 0; k < 3; ++k) {
                if (gone[size_t(t[k])]) continue;
                if (anyGone) nearHole[size_t(t[k])] = 1;
                for (int j = 0; j < 3; ++j)
                    if (j != k && !gone[size_t(t[j])]) adj[size_t(t[k])].push_back(t[j]);
            }
        }
        std::vector<int> band;
        for (int v = 0; v < V; ++v) {
            const float y = body.positions[size_t(v) * 3 + 1];
            if (!gone[size_t(v)] && !nearHole[size_t(v)] && y > neckCutY - 0.07f && y < neckCutY + 0.02f && !adj[size_t(v)].empty()) band.push_back(v);
        }
        for (int it = 0; it < 14; ++it) {
            std::vector<float> next(band.size() * 3);
            for (size_t i = 0; i < band.size(); ++i) {
                const int v = band[i];
                float avg[3] = {0, 0, 0};
                for (int n : adj[size_t(v)]) for (int c = 0; c < 3; ++c) avg[c] += body.positions[size_t(n) * 3 + size_t(c)];
                const float inv = 1.0f / float(adj[size_t(v)].size());
                for (int c = 0; c < 3; ++c) {
                    const float cur = body.positions[size_t(v) * 3 + size_t(c)];
                    next[i * 3 + size_t(c)] = cur + 0.5f * (avg[c] * inv - cur);
                }
            }
            for (size_t i = 0; i < band.size(); ++i)
                for (int c = 0; c < 3; ++c) body.positions[size_t(band[i]) * 3 + size_t(c)] = next[i * 3 + size_t(c)];
        }
    }
    // ---- stitch the seam: a triangle strip between the body's neck hole and the head's neck rim ---------------------------
    // The strip belongs to the body mesh; it ends on COPIES of the head's rim vertices (same positions), so the two meshes meet
    // exactly and nothing is left to see through. The copies take the weights of the nearest body vertex on the hole's edge.
    if (haveRing && !p.bodyOnly) {
        std::unordered_map<EdgeKey, std::array<int, 2>> edgeUse;   // [kept faces, removed faces]
        for (size_t f = 0; f + 2 < a.mhrFaces.size(); f += 3) {
            int removed = 0;
            for (int k = 0; k < 3; ++k) removed += gone[size_t(a.mhrFaces[f + k])] ? 1 : 0;
            const bool kept = removed < 2;
            for (int k = 0; k < 3; ++k) {
                auto& use = edgeUse[edgeKey(uint32_t(a.mhrFaces[f + k]), uint32_t(a.mhrFaces[f + (k + 1) % 3]))];
                use[kept ? 0 : 1]++;
            }
        }
        std::vector<std::pair<uint32_t, uint32_t>> holeEdges;
        for (const auto& kv : edgeUse)
            if (kv.second[0] == 1 && kv.second[1] >= 1) holeEdges.push_back({uint32_t(kv.first >> 32), uint32_t(kv.first & 0xFFFFFFFFu)});
        auto holeLoops = chainLoops(holeEdges);
        // the head's rim: the lowest boundary loop of the kept skin polygons
        std::unordered_map<EdgeKey, int> ictEdge;
        for (int pi : skinPolys) {
            const uint32_t b0 = a.polyStart[size_t(pi)], n = uint32_t(a.polySize[size_t(pi)]);
            for (uint32_t k = 0; k < n; ++k) ictEdge[edgeKey(uint32_t(a.polyVertex[b0 + k]), uint32_t(a.polyVertex[b0 + (k + 1) % n]))]++;
        }
        std::vector<std::pair<uint32_t, uint32_t>> rimEdges;
        for (const auto& kv : ictEdge)
            if (kv.second == 1) rimEdges.push_back({uint32_t(kv.first >> 32), uint32_t(kv.first & 0xFFFFFFFFu)});
        auto rimLoops = chainLoops(rimEdges);
        auto lowest = [](const std::vector<std::vector<uint32_t>>& loops, const std::vector<float>& pos) -> const std::vector<uint32_t>* {
            const std::vector<uint32_t>* best = nullptr;
            float bestY = 1e30f;
            for (const auto& l : loops) {
                float y = 0;
                for (uint32_t v : l) y += pos[size_t(v) * 3 + 1];
                y /= float(l.size());
                if (y < bestY) { bestY = y; best = &l; }
            }
            return best;
        };
        // the hole loop: the body loop nearest the head rim (not a wrist or the bust cut)
        const std::vector<uint32_t>* hole = nullptr;
        const std::vector<uint32_t>* rim = lowest(rimLoops, headPos);
        if (rim) {
            float cy = 0;
            for (uint32_t v : *rim) cy += headPos[size_t(v) * 3 + 1];
            cy /= float(rim->size());
            float bestDy = 1e30f;
            for (const auto& l : holeLoops) {
                float y = 0, x = 0;
                for (uint32_t v : l) { y += body.positions[size_t(v) * 3 + 1]; x += std::fabs(body.positions[size_t(v) * 3]); }
                y /= float(l.size()); x /= float(l.size());
                if (x > 0.12f) continue;   // not around the neck
                const float dy = std::fabs(y - cy);
                if (dy < bestDy) { bestDy = dy; hole = &l; }
            }
        }
        if (hole && rim && hole->size() >= 3 && rim->size() >= 3) {
            std::vector<uint32_t> A = *hole, B = *rim;
            if ((loopArea(A, body.positions) < 0) != (loopArea(B, headPos) < 0)) std::reverse(B.begin(), B.end());
            // The hole's edge follows the mesh's vertex rows, so it zig-zags by a vertex spacing: left as is, the strip inherits a
            // sawtooth. Relax the loop along itself (tangential smoothing) so the seam is one clean curve.
            for (int it = 0; it < 14; ++it) {
                std::vector<float> next(A.size() * 3);
                for (size_t i = 0; i < A.size(); ++i) {
                    const size_t pv = (i + A.size() - 1) % A.size(), nx = (i + 1) % A.size();
                    for (int c = 0; c < 3; ++c) {
                        const float cur = body.positions[size_t(A[i]) * 3 + size_t(c)];
                        const float avg = 0.5f * (body.positions[size_t(A[pv]) * 3 + size_t(c)] + body.positions[size_t(A[nx]) * 3 + size_t(c)]);
                        next[i * 3 + size_t(c)] = cur + 0.5f * (avg - cur);
                    }
                }
                for (size_t i = 0; i < A.size(); ++i)
                    for (int c = 0; c < 3; ++c) body.positions[size_t(A[i]) * 3 + size_t(c)] = next[i * 3 + size_t(c)];
            }
            if (bodyRing.valid()) {
                // straight horizontal ring on both sides: the body's hole edge and the head's rim share the cut plane and outline
                for (uint32_t v : B) {
                    float* hp = &headPos[size_t(v) * 3];
                    const float dx = hp[0] - rh.cx, dz = hp[2] - rh.cz;
                    const float th = std::atan2(dz, dx), r = bodyRing.at(th);
                    hp[0] = rh.cx + r * std::cos(th);
                    hp[2] = rh.cz + r * std::sin(th);
                    hp[1] = neckCutY;
                }
            }
            std::vector<std::array<float, 3>> pa, pb;
            for (uint32_t v : A) pa.push_back({body.positions[size_t(v) * 3], body.positions[size_t(v) * 3 + 1], body.positions[size_t(v) * 3 + 2]});
            for (uint32_t v : B) pb.push_back({headPos[size_t(v) * 3], headPos[size_t(v) * 3 + 1], headPos[size_t(v) * 3 + 2]});
            // copies of the rim vertices inside the body mesh, weighted like the nearest hole-edge vertex
            std::vector<uint32_t> copyOf(B.size());
            for (size_t i = 0; i < B.size(); ++i) {
                copyOf[i] = uint32_t(body.positions.size() / 3);
                body.positions.insert(body.positions.end(), pb[i].begin(), pb[i].end());
                size_t nearest = 0;
                float bestD = 1e30f;
                for (size_t j = 0; j < A.size(); ++j) {
                    const float dx = pa[j][0] - pb[i][0], dy = pa[j][1] - pb[i][1], dz = pa[j][2] - pb[i][2];
                    const float d = dx * dx + dy * dy + dz * dz;
                    if (d < bestD) { bestD = d; nearest = j; }
                }
                body.skin.push_back(body.skin[A[nearest]]);
                bridgeUv.push_back(uvOfPosition[A[nearest]]);
            }
            const auto strip = zipLoops(pa, pb);
            // each strip triangle is oriented OUTWARD on its own (away from the neck axis, a little upward): where the head's rim
            // crosses the body's edge the band folds over, and one global orientation would leave those facets dark
            const float ccx = rh.cx, ccz = rh.cz;
            auto vtx = [&](bool isA, int i) { return isA ? A[size_t(i)] : copyOf[size_t(i)]; };
            auto posOf = [&](uint32_t v) { return std::array<float, 3>{body.positions[size_t(v) * 3], body.positions[size_t(v) * 3 + 1], body.positions[size_t(v) * 3 + 2]}; };
            std::vector<std::array<uint32_t, 3>> tris;
            for (const auto& t : strip) {
                std::array<uint32_t, 3> tri = t.aEdge ? std::array<uint32_t, 3>{vtx(true, t.ia0), vtx(true, t.ia1), vtx(false, t.ib0)}
                                                       : std::array<uint32_t, 3>{vtx(true, t.ia0), vtx(false, t.ib1), vtx(false, t.ib0)};
                if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
                const auto p0 = posOf(tri[0]), p1 = posOf(tri[1]), p2 = posOf(tri[2]);
                const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]}, e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
                const float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
                const float mx = (p0[0] + p1[0] + p2[0]) / 3 - ccx, mz = (p0[2] + p1[2] + p2[2]) / 3 - ccz;
                const float outward = n[0] * mx + n[2] * mz + 0.3f * std::sqrt(mx * mx + mz * mz) * n[1];
                if (outward < 0) std::swap(tri[1], tri[2]);
                tris.push_back(tri);
            }
            for (const auto& tri : tris) {
                std::vector<uint32_t> face;
                for (uint32_t v : tri) {
                    const uint64_t key = (uint64_t(v) << 32) | (v < uint32_t(V) ? uvOfPosition[v] : bridgeUv[v - uint32_t(V)]);
                    auto it = seenLayouts.find(key);
                    if (it == seenLayouts.end()) {
                        it = seenLayouts.emplace(key, uint32_t(body.layouts.size())).first;
                        body.layouts.push_back({v, uint32_t(key & 0xFFFFFFFFu), v});
                    }
                    face.push_back(it->second);
                }
                body.faces.push_back(std::move(face));
                seamFaces.push_back(body.faces.size() - 1);
            }
            rep.seamTriangles = int(tris.size());
        } else {
            rep.warnings << QStringLiteral("The neck seam could not be stitched (%1 hole loops, %2 rim loops); the head is left open.")
                                .arg(holeLoops.size()).arg(rimLoops.size());
        }
    }
    body.normals = positionNormals(body.positions, body.faces, body.layouts);
    // both sides of the stitching strip render (normals were taken from the outward side above)
    for (size_t fi : seamFaces) {
        std::vector<uint32_t> rev(body.faces[fi].rbegin(), body.faces[fi].rend());
        body.faces.push_back(std::move(rev));
    }
    rep.bodyVertices = int(body.positions.size() / 3);
    rep.bodyTriangles = int(body.faces.size());


    struct Group { const char* mesh; std::vector<const char*> parts; bool neck; };
    const std::vector<Group> groups = {
        {"head_skin", {"face", "head_neck", "mouth_socket", "eye_socket_l", "eye_socket_r"}, true},
        {"head_mouth", {"gums_tongue"}, false}, {"head_teeth", {"teeth"}, false},
        {"head_sclera", {"sclera_l", "sclera_r"}, false}, {"head_iris", {"iris_l", "iris_r"}, false},
        {"head_lashes", {"lashes_l", "lashes_r"}, false}};

    // body vertices near the neck cut, for the seam weights
    std::vector<int> seamBody;
    for (int v = 0; v < V; ++v) {
        const float y = body.positions[size_t(v) * 3 + 1];
        if (y < neckCutY + 0.03f && y > neckCutY - 0.08f) seamBody.push_back(v);
    }

    if (p.nativeHead && !p.bodyOnly) {
        // Seat each ICT eyeball in the MHR face: the identity shapes move the lids forwards or back by up to a centimetre, so
        // the eyeball is pushed along z until its front sits just ahead of the (opened) lid margin of THIS face.
        for (const char* side : {"l", "r"}) {
            const IctPart* scl = a.part(QStringLiteral("sclera_") + QLatin1String(side));
            const IctPart* iris = a.part(QStringLiteral("iris_") + QLatin1String(side));
            if (!scl || !iris) continue;
            float cx = 0, cy = 0, frontZ = -1e9f;
            int n = 0;
            for (int v = iris->v0; v <= iris->v1; ++v) { cx += headPos[size_t(v) * 3]; cy += headPos[size_t(v) * 3 + 1]; ++n; }
            if (n == 0) continue;
            cx /= float(n); cy /= float(n);
            const float dyLift = 0.0075f;   // the MHR lid groove sits about 6.5 mm above the ICT eyeball centre (the lids are opened there)
            for (const IctPart* q : {scl, iris})
                for (int v = q->v0; v <= q->v1; ++v) headPos[size_t(v) * 3 + 1] += dyLift;
            cy += dyLift;
            for (const IctPart* q : {scl, iris})
                for (int v = q->v0; v <= q->v1; ++v) frontZ = std::max(frontZ, headPos[size_t(v) * 3 + 2]);
            double zs = 0;
            int zn = 0;
            for (int v = 0; v < V; ++v) {
                const float dx = (body.positions[size_t(v) * 3] - cx) / 0.016f, dy = (body.positions[size_t(v) * 3 + 1] - cy) / 0.011f;
                if (dx * dx + dy * dy < 1.0f && body.positions[size_t(v) * 3 + 2] > frontZ - 0.03f) { zs += body.positions[size_t(v) * 3 + 2]; ++zn; }
            }
            if (zn < 8) continue;
            const float dz = float(zs / zn) + 0.0010f - frontZ;
            // a little smaller than ICT's (the MHR socket is narrower) and drawn slightly towards the nose
            const float shrink = 0.90f, medial = (cx > 0 ? -1.0f : 1.0f) * 0.0018f;
            float cz = 0;
            for (int v = iris->v0; v <= iris->v1; ++v) cz += headPos[size_t(v) * 3 + 2];
            cz = cz / float(n) + dz;
            for (const IctPart* q : {scl, iris})
                for (int v = q->v0; v <= q->v1; ++v) {
                    float* h = &headPos[size_t(v) * 3];
                    h[2] += dz;
                    h[0] = cx + (h[0] - cx) * shrink + medial;
                    h[1] = cy + (h[1] - cy) * shrink;
                    h[2] = cz + (h[2] - cz) * shrink;
                }
        }
    }
    for (const Group& g : groups) {
        if (p.bodyOnly) break;
        // the MHR head is the skin. Its eyes are closed lids with no socket behind them, so ICT's eyeballs would only poke through
        // them: only the mouth interior (gums, tongue, teeth) comes from ICT
        if (p.nativeHead && (g.neck || std::strcmp(g.mesh, "head_lashes") == 0)) continue;
        if (p.nativeHead && !p.openEyes && (std::strcmp(g.mesh, "head_sclera") == 0 || std::strcmp(g.mesh, "head_iris") == 0)) continue;   // hidden behind shut lids
        Dna::Mesh m;
        m.name = QString::fromLatin1(g.mesh);
        std::vector<int> local(size_t(a.ictVertices), -1);
        std::vector<int> globalOf;
        std::unordered_map<uint64_t, uint32_t> seen;
        m.uvs = a.ictTexcoords;
        for (const char* pn : g.parts) {
            const IctPart* part = a.part(QString::fromLatin1(pn));
            if (!part) { rep.warnings << QStringLiteral("ICT part '%1' is missing").arg(QString::fromLatin1(pn)); continue; }
            // ICT's sclera is a full shell in FRONT of the iris (meant to be translucent over it). With flat colours it would hide
            // the iris, so the front cap of each sclera is cut open (the iris dome shows through, a little wider than the cap).
            float capZ = 1e30f;
            if (part->name.startsWith(QLatin1String("sclera_"))) {
                const IctPart* iris = a.part(QStringLiteral("iris_") + part->name.mid(7));
                double cz = 0;
                int cnt = 0;
                for (const IctPart* q : {part, iris})
                    if (q)
                        for (int v = q->v0; v <= q->v1; ++v) { cz += a.ictNeutral[size_t(v) * 3 + 2]; ++cnt; }
                if (cnt > 0) capZ = float(cz / cnt) + 0.85f;   // cm, ICT space
            }
            for (int pi = part->p0; pi <= part->p1; ++pi) {
                const uint32_t b = a.polyStart[size_t(pi)], n = uint32_t(a.polySize[size_t(pi)]);
                if (capZ < 1e29f) {
                    float z = 0;
                    for (uint32_t k = 0; k < n; ++k) z += a.ictNeutral[size_t(a.polyVertex[b + k]) * 3 + 2];
                    if (z / float(n) > capZ) continue;
                }
                if (g.neck) {
                    bool below = false;
                    for (uint32_t k = 0; k < n; ++k)
                        if (headPos[size_t(a.polyVertex[b + k]) * 3 + 1] < trimY) { below = true; break; }
                    if (below) continue;
                }
                std::vector<uint32_t> face;
                for (uint32_t k = 0; k < n; ++k) {
                    const int gv = a.polyVertex[b + k];
                    if (local[size_t(gv)] < 0) {
                        local[size_t(gv)] = int(globalOf.size());
                        globalOf.push_back(gv);
                        for (int i = 0; i < 3; ++i) m.positions.push_back(headPos[size_t(gv) * 3 + i]);
                    }
                    const uint32_t li = uint32_t(local[size_t(gv)]), ti = uint32_t(a.polyTexcoord[b + k]);
                    const uint64_t key = (uint64_t(li) << 32) | ti;
                    auto it = seen.find(key);
                    if (it == seen.end()) {
                        it = seen.emplace(key, uint32_t(m.layouts.size())).first;
                        m.layouts.push_back({li, ti, li});
                    }
                    face.push_back(it->second);
                }
                m.faces.push_back(std::move(face));
            }
        }
        if (m.faces.empty()) continue;
        m.normals = positionNormals(m.positions, m.faces, m.layouts);

        // skin: rigid to the head joint; the neck rows blend to the nearest body vertex's weights so the seam follows the body
        const size_t pc = globalOf.size();
        m.skin.assign(pc, {});
        for (size_t i = 0; i < pc; ++i) {
            float t = 0.0f;
            const float y = m.positions[i * 3 + 1];
            if (g.neck) t = smoothstep((chinWy - 0.01f - y) / std::max(0.01f, chinWy - 0.01f - trimY));
            std::vector<Dna::SkinInfluence> inf;
            if (t > 0.0f && !seamBody.empty()) {
                int nearest = seamBody[0];
                float bestD = 1e9f;
                for (int bv : seamBody) {
                    const float dx = body.positions[size_t(bv) * 3] - m.positions[i * 3], dy = body.positions[size_t(bv) * 3 + 1] - y,
                                dz = body.positions[size_t(bv) * 3 + 2] - m.positions[i * 3 + 2];
                    const float d = dx * dx + dy * dy + dz * dz;
                    if (d < bestD) { bestD = d; nearest = bv; }
                }
                for (const auto& bi : body.skin[size_t(nearest)]) inf.push_back({bi.joint, bi.weight * t});
            }
            bool merged = false;
            for (auto& x : inf)
                if (x.joint == uint16_t(newIndex[size_t(cHead)])) { x.weight += 1.0f - t; merged = true; }
            if (!merged && t < 1.0f) inf.push_back({uint16_t(newIndex[size_t(cHead)]), 1.0f - t});
            m.skin[i] = std::move(inf);
        }

        // expression shapes -> sparse morph targets (ICT cm -> world m, scaled with the head)
        for (int e = 0; e < a.ictExpressions; ++e) {
            Dna::BlendShapeTarget bt;
            bt.channel = e;
            const float* d = a.ictExpression.data() + size_t(e) * a.ictNeutral.size();
            for (size_t i = 0; i < pc; ++i) {
                const float* dv = d + size_t(globalOf[i]) * 3;
                if (std::fabs(dv[0]) + std::fabs(dv[1]) + std::fabs(dv[2]) < 1e-3f) continue;   // < 10 micrometres
                bt.vertexIndices.push_back(uint32_t(i));
                for (int k = 0; k < 3; ++k) bt.deltas.push_back(dv[k] * s * kCmToM);
            }
            if (!bt.vertexIndices.empty()) m.blendShapeTargets.push_back(std::move(bt));
        }
        rep.headVertices += int(pc);
        rep.headTriangles += int(m.faces.size());
        rep.meshNames << m.name;
        rig.meshes.push_back(std::move(m));
    }
    if (p.nativeHead && !a.mhrArkit.empty() && a.mhrArkit.size() == size_t(a.ictExpressions) * size_t(V) * 3) {
        for (int e = 0; e < a.ictExpressions; ++e) {
            Dna::BlendShapeTarget bt;
            bt.channel = e;
            const float* d = a.mhrArkit.data() + size_t(e) * size_t(V) * 3;
            for (int v = 0; v < V; ++v) {
                const float* dv = d + size_t(v) * 3;
                if (std::fabs(dv[0]) + std::fabs(dv[1]) + std::fabs(dv[2]) < 2e-3f) continue;
                bt.vertexIndices.push_back(uint32_t(v));
                for (int k = 0; k < 3; ++k) bt.deltas.push_back(dv[k] * kCmToM);
            }
            if (!bt.vertexIndices.empty()) body.blendShapeTargets.push_back(std::move(bt));
        }
    } else if (p.nativeHead) {
        rep.warnings << QStringLiteral("This asset bundle has no MHR expression data (arkit_delta); rebuild the bundles with scripts/build-character-assets.py.");
    }
    rep.meshNames.prepend(body.name);
    rig.meshes.insert(rig.meshes.begin(), std::move(body));
    rig.blendShapeChannels = a.expressionNames;
    // Every part carries every channel. glTF puts all the parts of a node into ONE mesh whose primitives must share the same morph
    // targets in the same order (and takes the target names from the first primitive, which is the body): a part that skipped a
    // channel it does not deform would scramble the targets or lose the names on export. A channel a part does not move gets a
    // single zero delta.
    for (auto& m : rig.meshes) {
        std::vector<char> has(size_t(std::max(0, int(rig.blendShapeChannels.size()))), 0);
        for (const auto& bt : m.blendShapeTargets)
            if (bt.channel >= 0 && size_t(bt.channel) < has.size()) has[size_t(bt.channel)] = 1;
        for (size_t e = 0; e < has.size(); ++e) {
            if (has[e]) continue;
            Dna::BlendShapeTarget bt;
            bt.channel = int(e);
            bt.vertexIndices = {m.layouts.empty() ? 0u : m.layouts.front().position};   // a position the mesh really uses
            bt.deltas = {0.0f, 0.0f, 0.0f};
            m.blendShapeTargets.push_back(std::move(bt));
        }
        std::sort(m.blendShapeTargets.begin(), m.blendShapeTargets.end(),
                  [](const auto& x, const auto& y) { return x.channel < y.channel; });
    }
    rep.morphTargets = a.ictExpressions;
    if (rig.meshes.size() < 2) rep.warnings << QStringLiteral("The head produced no geometry.");

    if (!p.facePlusZ) Dna::yawRig(rig, 3.14159265358979323846);
    return rig;
}

}  // namespace CharacterGen
