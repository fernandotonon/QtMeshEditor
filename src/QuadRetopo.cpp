#include "QuadRetopo.h"
#include "EditableMesh.h"

#include <Ogre.h>
#include <OgreEntity.h>
#include <OgreMesh.h>
#include <OgreSubMesh.h>
#include <OgreVector3.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

// Forward declarations from EditableMesh.cpp; the existing
// implementation already exposes these for use by other slices
// (decimation, UV unwrap, etc).
void writeNgonFacesToMesh(Ogre::Mesh* mesh,
                          const std::vector<EditableSubMesh>& subMeshes);

namespace {

// ─── Geometry helpers ───────────────────────────────────────────────────────

struct Vec3 {
    double x = 0, y = 0, z = 0;

    static Vec3 from(const float* p, unsigned int idx) {
        const float* v = p + idx * 3;
        return { v[0], v[1], v[2] };
    }

    Vec3 operator-(const Vec3& o) const { return { x - o.x, y - o.y, z - o.z }; }
    Vec3 operator+(const Vec3& o) const { return { x + o.x, y + o.y, z + o.z }; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const {
        return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x };
    }
    double lengthSq() const { return x * x + y * y + z * z; }
    double length() const { return std::sqrt(lengthSq()); }
    Vec3 normalized() const {
        const double L = length();
        return L > 1e-12 ? Vec3{ x / L, y / L, z / L } : Vec3{};
    }
};

double triNormal(const float* positions,
                 unsigned int a, unsigned int b, unsigned int c,
                 Vec3& outNormal)
{
    const Vec3 pa = Vec3::from(positions, a);
    const Vec3 pb = Vec3::from(positions, b);
    const Vec3 pc = Vec3::from(positions, c);
    const Vec3 e1 = pb - pa;
    const Vec3 e2 = pc - pa;
    const Vec3 n  = e1.cross(e2);
    const double L = n.length();
    outNormal = L > 1e-12 ? Vec3{ n.x / L, n.y / L, n.z / L } : Vec3{};
    return L;  // 2 * triangle area
}

// Return the interior angle at vertex `b` of the polygon edge a→b→c,
// in degrees. Always in (0, 180].
double interiorAngleDeg(const Vec3& a, const Vec3& b, const Vec3& c)
{
    const Vec3 ba = (a - b).normalized();
    const Vec3 bc = (c - b).normalized();
    const double d = std::clamp(ba.dot(bc), -1.0, 1.0);
    return std::acos(d) * 180.0 / Ogre::Math::PI;
}

// ─── Edge → adjacent triangle lookup ────────────────────────────────────────

// Undirected edge key (smaller index first).
struct EdgeKey {
    unsigned int a, b;

    static EdgeKey make(unsigned int u, unsigned int v) {
        return u < v ? EdgeKey{ u, v } : EdgeKey{ v, u };
    }

    bool operator==(const EdgeKey& o) const { return a == o.a && b == o.b; }
};

struct EdgeKeyHash {
    size_t operator()(const EdgeKey& e) const noexcept {
        return std::hash<uint64_t>{}((uint64_t(e.a) << 32) | uint64_t(e.b));
    }
};

// ─── Candidate quad scoring ─────────────────────────────────────────────────

struct CandidatePair {
    int    triA = -1;        // First triangle index (into `triangleCount` triangles)
    int    triB = -1;        // Second triangle index
    double score = 0.0;      // Higher = better merge candidate

    // The 4 vertex indices of the resulting quad, in winding order.
    unsigned int quad[4] = { 0, 0, 0, 0 };

    bool operator<(const CandidatePair& o) const { return score > o.score; }
};

// Given two triangles sharing an edge whose endpoints are `e0` and
// `e1` (undirected — `EdgeKey::make` returns `(min, max)`), find the
// non-shared "opposing" vertex of each triangle and emit a quad
// winding that preserves the source triangles' winding orientation.
//
// Convention: walk `tri0` in its own (CCW) order. If it goes through
// the shared edge in the direction `e0 → e1`, then walking the quad
// `[opposing0, e0, opposing1, e1]` (with `tri1` providing
// `opposing1`) winds CCW. Otherwise the directed edge is `e1 → e0`
// and the correct winding is `[opposing0, e1, opposing1, e0]`.
//
// This matters because the n-gon fan-triangulation in
// `triangulateFaces` builds `[v0, v_i, v_{i+1}]`; if the winding is
// flipped the resulting triangle normals are opposite the source
// triangles' normals — every retopologized quad would render with
// inverted normals (broken backface culling + lighting). Codex
// review caught this on the merged commit; see GitHub PR #697.
bool buildQuadWinding(const unsigned int* tri0,
                      const unsigned int* tri1,
                      unsigned int e0, unsigned int e1,
                      unsigned int outQuad[4])
{
    unsigned int opposing0 = ~0u, opposing1 = ~0u;
    for (int i = 0; i < 3; ++i) {
        if (tri0[i] != e0 && tri0[i] != e1) opposing0 = tri0[i];
        if (tri1[i] != e0 && tri1[i] != e1) opposing1 = tri1[i];
    }
    if (opposing0 == ~0u || opposing1 == ~0u) return false;

    // Determine tri0's direction over the shared edge. If `tri0`
    // contains the directed edge `e0 → e1` (i.e. `e1` immediately
    // follows `e0` in winding order), then the quad winding starting
    // from `opposing0` should pass through `e0` first, then
    // `opposing1`, then `e1`.
    bool sharedGoesE0toE1 = false;
    for (int i = 0; i < 3; ++i) {
        if (tri0[i] == e0 && tri0[(i + 1) % 3] == e1) {
            sharedGoesE0toE1 = true;
            break;
        }
    }
    outQuad[0] = opposing0;
    if (sharedGoesE0toE1) {
        outQuad[1] = e0;
        outQuad[2] = opposing1;
        outQuad[3] = e1;
    } else {
        outQuad[1] = e1;
        outQuad[2] = opposing1;
        outQuad[3] = e0;
    }
    return true;
}

// Score a candidate quad. Returns < 0 if the merge should be rejected
// outright (non-coplanar, concave, or out-of-tolerance shape).
double scoreCandidate(const float* positions,
                      const unsigned int quad[4],
                      const Vec3& n0,    // first triangle's normal
                      const Vec3& n1,    // second triangle's normal
                      const QuadRetopoOptions& opts)
{
    // 1. Coplanarity check via normal angle.
    const double cosNormals = std::clamp(n0.dot(n1), -1.0, 1.0);
    const double angleDeg = std::acos(cosNormals) * 180.0 / Ogre::Math::PI;
    if (angleDeg > opts.maxAngleDeg) return -1.0;

    const Vec3 p0 = Vec3::from(positions, quad[0]);
    const Vec3 p1 = Vec3::from(positions, quad[1]);
    const Vec3 p2 = Vec3::from(positions, quad[2]);
    const Vec3 p3 = Vec3::from(positions, quad[3]);

    // 2. Interior angles must be in [90 - tol, 90 + tol].
    const double a0 = interiorAngleDeg(p3, p0, p1);
    const double a1 = interiorAngleDeg(p0, p1, p2);
    const double a2 = interiorAngleDeg(p1, p2, p3);
    const double a3 = interiorAngleDeg(p2, p3, p0);
    const double tol = opts.shapeToleranceDeg;
    if (a0 < 90 - tol || a0 > 90 + tol) return -1.0;
    if (a1 < 90 - tol || a1 > 90 + tol) return -1.0;
    if (a2 < 90 - tol || a2 > 90 + tol) return -1.0;
    if (a3 < 90 - tol || a3 > 90 + tol) return -1.0;
    // Convexity check: interior angles of a convex quad sum to 360°
    // and each is < 180°. The tolerance guard above already ensures
    // each is < 90 + tol; with the default tol=65 (worst case 155°)
    // this still rejects non-convex quads.
    if (a0 + a1 + a2 + a3 > 360.5) return -1.0;

    // 3. Aspect ratio.
    const double e01 = (p1 - p0).length();
    const double e12 = (p2 - p1).length();
    const double e23 = (p3 - p2).length();
    const double e30 = (p0 - p3).length();
    const double longest  = std::max({ e01, e12, e23, e30 });
    const double shortest = std::min({ e01, e12, e23, e30 });
    if (shortest < 1e-9) return -1.0;
    const double aspect = longest / shortest;
    if (aspect > opts.maxAspectRatio) return -1.0;

    // Composite score: prefer near-coplanar (high cos), near-square
    // (low angle deviation), and low aspect ratio. We sum normalized
    // contributions in [0, 1] so the maximum score is 3.
    const double coplanarityScore = cosNormals;                          // [-1, 1]
    const double angleDevScore    = 1.0 - (std::abs(a0 - 90) +
                                            std::abs(a1 - 90) +
                                            std::abs(a2 - 90) +
                                            std::abs(a3 - 90)) / (4 * 90);
    const double aspectScore      = 1.0 / aspect;                        // (0, 1]

    return coplanarityScore + angleDevScore + aspectScore;
}

// ─── Per-submesh pairing ─────────────────────────────────────────────────────

void retopologizeSubmesh(EditableSubMesh& sub,
                         const QuadRetopoOptions& opts,
                         int submeshIndex,
                         QuadRetopoSubmeshReport& report,
                         int& globalRemainingTargetCount)
{
    report.submeshIndex = submeshIndex;
    report.trianglesBefore = static_cast<int>(sub.triangles.size());

    if (sub.triangles.empty() || sub.vertices.size() < 3) {
        report.facesAfter     = static_cast<int>(sub.triangles.size());
        report.trianglesAfter = static_cast<int>(sub.triangles.size());
        return;
    }

    // Flatten positions for QuadRetopo::retopologizeMesh.
    std::vector<float> positions(sub.vertices.size() * 3);
    for (size_t i = 0; i < sub.vertices.size(); ++i) {
        positions[3 * i + 0] = sub.vertices[i].position.x;
        positions[3 * i + 1] = sub.vertices[i].position.y;
        positions[3 * i + 2] = sub.vertices[i].position.z;
    }

    std::vector<unsigned int> indices;
    indices.reserve(sub.triangles.size() * 3);
    for (const auto& t : sub.triangles) {
        indices.push_back(t.indices[0]);
        indices.push_back(t.indices[1]);
        indices.push_back(t.indices[2]);
    }

    // Run the pure-data pairing. `globalRemainingTargetCount` is the
    // remaining *reduction budget* (number of pair operations we can
    // still spend across all submeshes), or -1 when unlimited.
    //
    // Convert that into a per-submesh `targetFaces` for the
    // `retopologizeMesh` pure-data call. With unlimited budget we
    // pass through `opts.targetFaces` unchanged (it'll be -1 too,
    // signalling "no limit"). With a constrained budget, the
    // per-submesh limit is `trianglesBefore - allowedReduction`,
    // floored at `ceil(trianglesBefore / 2)` (every tri paired).
    std::vector<std::vector<unsigned int>> faces;
    QuadRetopoOptions perSub = opts;
    if (globalRemainingTargetCount >= 0) {
        const int floorFaces = (report.trianglesBefore + 1) / 2;
        const int desiredFaces = report.trianglesBefore - globalRemainingTargetCount;
        perSub.targetFaces = std::max(floorFaces, desiredFaces);
    } else {
        // Unlimited budget — the caller didn't set a target.
        // `retopologizeMesh` treats `targetFaces <= 0` as "pair
        // every viable candidate", which is what we want here.
        perSub.targetFaces = -1;
    }

    QuadRetopo::retopologizeMesh(positions.data(),
                                 static_cast<int>(sub.vertices.size()),
                                 indices.data(),
                                 static_cast<int>(sub.triangles.size()),
                                 perSub, faces);

    // Decrement the global reduction budget by what we used. Each
    // pair op reduces face count by 1, so `(trianglesBefore -
    // facesNow)` units were consumed.
    if (globalRemainingTargetCount >= 0) {
        const int reductionHere = report.trianglesBefore - static_cast<int>(faces.size());
        globalRemainingTargetCount = std::max(0,
            globalRemainingTargetCount - reductionHere);
    }

    // Translate the faces back into EditableSubMesh::faces.
    sub.faces.clear();
    sub.faces.reserve(faces.size());
    for (const auto& f : faces) {
        EditableFace ef;
        ef.indices = f;
        if (ef.isValid())
            sub.faces.push_back(std::move(ef));
    }

    // Rebuild triangles so the GPU has a valid index buffer.
    triangulateFaces(sub);

    report.facesAfter     = static_cast<int>(sub.faces.size());
    report.quadsAfter     = 0;
    report.trianglesAfter = 0;
    for (const auto& f : sub.faces) {
        if (f.indices.size() == 4) ++report.quadsAfter;
        else if (f.indices.size() == 3) ++report.trianglesAfter;
    }
}

} // namespace

// ─── Public API ────────────────────────────────────────────────────────────

QuadRetopoReport QuadRetopo::retopologizeMesh(const float* positions,
                                              int vertexCount,
                                              const unsigned int* indices,
                                              int triangleCount,
                                              const QuadRetopoOptions& opts,
                                              std::vector<std::vector<unsigned int>>& outFaces)
{
    QuadRetopoReport report;
    outFaces.clear();

    if (!positions || !indices || vertexCount < 3 || triangleCount < 1) {
        report.error = QStringLiteral("empty or malformed input");
        return report;
    }

    report.totalTrianglesBefore = triangleCount;

    // 1. Compute per-triangle normals and area.
    std::vector<Vec3> triNormals(triangleCount);
    for (int t = 0; t < triangleCount; ++t) {
        const unsigned int* tri = indices + 3 * t;
        triNormal(positions, tri[0], tri[1], tri[2], triNormals[t]);
    }

    // 2. Build the edge → adjacent-triangles lookup. Each interior
    // edge appears in exactly two triangles; boundary edges appear
    // in one and are skipped.
    struct EdgeAdjacency {
        int t0 = -1;
        int t1 = -1;
    };
    std::unordered_map<EdgeKey, EdgeAdjacency, EdgeKeyHash> edgeMap;
    edgeMap.reserve(triangleCount * 3);
    for (int t = 0; t < triangleCount; ++t) {
        const unsigned int* tri = indices + 3 * t;
        for (int e = 0; e < 3; ++e) {
            const EdgeKey key = EdgeKey::make(tri[e], tri[(e + 1) % 3]);
            auto& adj = edgeMap[key];
            if (adj.t0 < 0)      adj.t0 = t;
            else if (adj.t1 < 0) adj.t1 = t;
            // Edges shared by >2 triangles (non-manifold) — keep first 2.
        }
    }

    // 3. Score every interior edge as a candidate quad merge.
    std::vector<CandidatePair> candidates;
    candidates.reserve(edgeMap.size());
    for (const auto& [key, adj] : edgeMap) {
        if (adj.t0 < 0 || adj.t1 < 0) continue;  // boundary
        const unsigned int* tri0 = indices + 3 * adj.t0;
        const unsigned int* tri1 = indices + 3 * adj.t1;
        CandidatePair c;
        c.triA = adj.t0;
        c.triB = adj.t1;
        if (!buildQuadWinding(tri0, tri1, key.a, key.b, c.quad)) continue;
        c.score = scoreCandidate(positions, c.quad,
                                  triNormals[adj.t0], triNormals[adj.t1], opts);
        if (c.score < 0) continue;
        candidates.push_back(c);
    }
    std::sort(candidates.begin(), candidates.end());

    // 4. Greedy pair-up: claim each triangle at most once. Stop early
    // if the caller specified a target face count and we've reached
    // it. Each winning pair stores its already-validated quad winding
    // directly so the emit pass below is O(pairs.size()), not
    // O(candidates × pairs).
    std::vector<char> claimed(triangleCount, 0);
    struct WinningPair {
        int triA, triB;
        unsigned int quad[4];
    };
    std::vector<WinningPair> pairs;
    pairs.reserve(candidates.size());
    int facesNow = triangleCount;
    for (const auto& c : candidates) {
        if (opts.targetFaces > 0 && facesNow <= opts.targetFaces) break;
        if (claimed[c.triA] || claimed[c.triB]) continue;
        claimed[c.triA] = 1;
        claimed[c.triB] = 1;
        WinningPair wp;
        wp.triA = c.triA;
        wp.triB = c.triB;
        wp.quad[0] = c.quad[0];
        wp.quad[1] = c.quad[1];
        wp.quad[2] = c.quad[2];
        wp.quad[3] = c.quad[3];
        pairs.push_back(wp);
        --facesNow;  // 2 tris → 1 quad
    }

    // 5. Emit winning quads (in score order — preserved by the order
    // we accepted them above).
    outFaces.reserve(triangleCount - pairs.size());
    for (const auto& p : pairs)
        outFaces.push_back({ p.quad[0], p.quad[1], p.quad[2], p.quad[3] });

    // 6. Emit unpaired triangles.
    for (int t = 0; t < triangleCount; ++t) {
        if (claimed[t]) continue;
        const unsigned int* tri = indices + 3 * t;
        outFaces.push_back({ tri[0], tri[1], tri[2] });
    }

    report.totalFacesAfter = static_cast<int>(outFaces.size());
    for (const auto& f : outFaces) {
        if (f.size() == 4) ++report.totalQuadsAfter;
        else if (f.size() == 3) ++report.totalTrianglesAfterRetopo;
    }
    report.applied = true;
    return report;
}

QuadRetopoReport QuadRetopo::retopologize(Ogre::Entity* entity,
                                          const QuadRetopoOptions& opts,
                                          Algorithm algo)
{
    QuadRetopoReport report;
    if (algo != Algorithm::TrianglePair) {
        report.error = QStringLiteral("only TrianglePair backend is implemented");
        return report;
    }
    if (!entity || !entity->getMesh()) {
        report.error = QStringLiteral("null entity / no mesh");
        return report;
    }

    Ogre::MeshPtr mesh = entity->getMesh();
    report.meshName = QString::fromStdString(mesh->getName());

    EditableMesh em;
    if (!em.loadFromEntity(entity)) {
        report.error = QStringLiteral("EditableMesh load failed");
        return report;
    }

    auto& subs = em.subMeshes();

    // `opts.targetFaces` is a *total* (across-all-submeshes) target.
    // Convert it into a global reduction budget: each pair op drops
    // the total face count by one (2 tris → 1 quad), so we have
    // (totalTris - targetFaces) pair operations to spend across all
    // submeshes. `retopologizeSubmesh` consumes from this counter and
    // also writes back the per-submesh face limit it actually used.
    int totalTris = 0;
    for (const auto& s : subs)
        totalTris += static_cast<int>(s.triangles.size());
    int remainingTarget = (opts.targetFaces > 0)
        ? std::max(0, totalTris - opts.targetFaces)
        : -1;  // -1 = unlimited budget (signalled by `opts.targetFaces <= 0`)

    for (size_t si = 0; si < subs.size(); ++si) {
        QuadRetopoSubmeshReport sub;
        retopologizeSubmesh(subs[si], opts,
                            static_cast<int>(si),
                            sub, remainingTarget);
        report.submeshes.push_back(sub);
        report.totalTrianglesBefore     += sub.trianglesBefore;
        report.totalFacesAfter          += sub.facesAfter;
        report.totalQuadsAfter          += sub.quadsAfter;
        report.totalTrianglesAfterRetopo += sub.trianglesAfter;
    }

    // Commit the edits back to the live Ogre::Mesh.
    if (!em.commitToEntity(entity)) {
        report.error = QStringLiteral("commitToEntity failed");
        return report;
    }

    // Write the n-gon binding so exporters and Edit Mode see the new
    // quad topology rather than the fan-triangulated tris.
    writeNgonFacesToMesh(mesh.get(), em.subMeshes());

    report.applied = true;
    return report;
}

QJsonObject QuadRetopo::reportToJson(const QuadRetopoReport& report)
{
    QJsonObject root;
    root["meshName"]                = report.meshName;
    root["applied"]                 = report.applied;
    root["totalTrianglesBefore"]    = report.totalTrianglesBefore;
    root["totalFacesAfter"]         = report.totalFacesAfter;
    root["totalQuadsAfter"]         = report.totalQuadsAfter;
    root["totalTrianglesAfterRetopo"] = report.totalTrianglesAfterRetopo;
    root["quadDominance"]           = report.quadDominance();
    if (!report.error.isEmpty()) root["error"] = report.error;

    QJsonArray subs;
    for (const auto& s : report.submeshes) {
        QJsonObject obj;
        obj["submeshIndex"]      = s.submeshIndex;
        obj["trianglesBefore"]   = s.trianglesBefore;
        obj["facesAfter"]        = s.facesAfter;
        obj["quadsAfter"]        = s.quadsAfter;
        obj["trianglesAfter"]    = s.trianglesAfter;
        subs.push_back(obj);
    }
    root["submeshes"] = subs;
    return root;
}

QString QuadRetopo::reportToText(const QuadRetopoReport& report)
{
    QString out;
    QTextStream s(&out);
    s << "Quad Retopology\n";
    s << "===============\n";
    s << "Mesh:          " << report.meshName << "\n";
    s << "Submeshes:     " << report.submeshes.size() << "\n";
    s << "Triangles in:  " << report.totalTrianglesBefore << "\n";
    s << "Faces out:     " << report.totalFacesAfter
      << "  (" << report.totalQuadsAfter << " quads, "
      << report.totalTrianglesAfterRetopo << " triangles)\n";
    s << "Quad dominance: "
      << QString::number(report.quadDominance() * 100.0, 'f', 1) << "%\n";
    if (!report.error.isEmpty()) s << "Error: " << report.error << "\n";
    return out;
}

QString QuadRetopo::algorithmToString(Algorithm algo)
{
    switch (algo) {
    case Algorithm::TrianglePair: return QStringLiteral("pair-tris");
    }
    return QStringLiteral("pair-tris");
}

QuadRetopo::Algorithm QuadRetopo::algorithmFromString(const QString& s)
{
    const QString lc = s.toLower();
    if (lc == "pair-tris" || lc == "pair") return Algorithm::TrianglePair;
    return Algorithm::TrianglePair;
}
