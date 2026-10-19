#include "DnaRegister.h"

#include "FaceRig/NonRigidICP.h"
#include "QuadRemesh/QuadRemeshCore.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

namespace Dna {
namespace Fit {

namespace {

struct Bounds {
    std::array<float, 3> mn{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    std::array<float, 3> mx{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
    void add(const float* p) { for (int k = 0; k < 3; ++k) { mn[size_t(k)] = std::min(mn[size_t(k)], p[k]); mx[size_t(k)] = std::max(mx[size_t(k)], p[k]); } }
    double diag() const { double d = 0; for (int k = 0; k < 3; ++k) d += double(mx[size_t(k)] - mn[size_t(k)]) * double(mx[size_t(k)] - mn[size_t(k)]); return std::sqrt(d); }
};

}  // namespace

int bodyGroupOfJoint(const Rig& rig, int joint)
{
    for (int guard = 0; joint >= 0 && size_t(joint) < rig.joints.size() && guard < 256; ++guard) {
        const QString n = rig.joints[size_t(joint)].name;
        const bool left = n.endsWith(QStringLiteral("_l")), right = n.endsWith(QStringLiteral("_r"));
        if (left || right) {
            if (n.startsWith(QStringLiteral("thigh")) || n.startsWith(QStringLiteral("calf")) || n.startsWith(QStringLiteral("foot")) ||
                n.startsWith(QStringLiteral("ball")))
                return left ? GroupLeftLeg : GroupRightLeg;
            if (n.startsWith(QStringLiteral("upperarm")) || n.startsWith(QStringLiteral("lowerarm")) || n.startsWith(QStringLiteral("hand")))
                return left ? GroupLeftArm : GroupRightArm;
        }
        joint = rig.joints[size_t(joint)].parent;
    }
    return GroupTorso;
}

std::vector<int> targetBoneGroups(const std::vector<QString>& boneNames, const std::vector<int>& parent)
{
    const size_t n = boneNames.size();
    std::vector<int> group(n, -1);
    // bones named like a table joint
    const auto table = defaultJointNameMap();
    auto groupOfBodyJoint = [](const QString& j) {
        const bool left = j.endsWith(QStringLiteral("_l")), right = j.endsWith(QStringLiteral("_r"));
        if (left || right) {
            if (j.startsWith(QStringLiteral("thigh")) || j.startsWith(QStringLiteral("calf")) || j.startsWith(QStringLiteral("foot"))) return left ? int(GroupLeftLeg) : int(GroupRightLeg);
            if (j.startsWith(QStringLiteral("upperarm")) || j.startsWith(QStringLiteral("lowerarm")) || j.startsWith(QStringLiteral("hand"))) return left ? int(GroupLeftArm) : int(GroupRightArm);
        }
        return int(GroupTorso);
    };
    for (size_t b = 0; b < n; ++b)
        for (const auto& entry : table)
            for (const QString& cand : entry.second)
                if (boneNames[b].compare(cand, Qt::CaseInsensitive) == 0) group[b] = groupOfBodyJoint(entry.first);
    // everything else inherits from its parent chain (fingers/toes/twist bones), roots -> torso
    for (size_t b = 0; b < n; ++b) {
        if (group[b] >= 0) continue;
        int p = parent[b], guard = 0;
        while (p >= 0 && size_t(p) < n && group[size_t(p)] < 0 && guard++ < 256) p = parent[size_t(p)];
        group[b] = (p >= 0 && size_t(p) < n && group[size_t(p)] >= 0) ? group[size_t(p)] : int(GroupTorso);
    }
    return group;
}

namespace {

// Smooth coarse-to-fine ICP (see RegisterOptions::Method::Smooth). `X` holds the template positions
// (x,y,z per vertex) and is deformed in place. Returns false with `err` set on failure.
// Strongly weighted correspondences kept through the iterations (the skeleton's matched joints): `pos` is carried
// through every warp so it always names where the joint currently is.
struct IcpAnchor {
    std::array<double, 3> pos;
    std::array<double, 3> target;
};

bool smoothIcp(std::vector<float>& X, const std::vector<int>& tf, const std::vector<float>& targetV,
               const std::vector<int>& targetF, int roundsPerLevel, int detailRounds, QString* err, std::vector<IcpAnchor>* anchors = nullptr,
               const std::vector<int>* tmplGroup = nullptr, const std::vector<int>* tgtGroup = nullptr,
               const std::vector<char>* extraSkip = nullptr)
{
    std::vector<uint32_t> tidx(targetF.begin(), targetF.end());
    QuadRemesh::SurfaceLocator loc;
    loc.build(targetV, tidx);
    if (!loc.valid()) { *err = QStringLiteral("could not index the target surface"); return false; }
    const size_t nv = X.size() / 3;
    Bounds ub;
    for (size_t i = 0; i + 2 < targetV.size(); i += 3) ub.add(&targetV[i]);
    const double diag = ub.diag();

    // target triangle normals
    std::vector<std::array<double, 3>> tn(targetF.size() / 3);
    for (size_t t = 0; t < tn.size(); ++t) {
        const float *a = &targetV[size_t(targetF[t * 3]) * 3], *b = &targetV[size_t(targetF[t * 3 + 1]) * 3], *c = &targetV[size_t(targetF[t * 3 + 2]) * 3];
        const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
        std::array<double, 3> n{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
        const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 1e-20) for (double& k : n) k /= l;
        tn[t] = n;
    }
    // Per-body-part locators: a vertex only sees target triangles of its own part (see BodyGroup). The locators
    // keep pointers to their index vectors, so those live as long as this function.
    std::vector<std::vector<uint32_t>> groupIdx(BodyGroupCount);
    std::vector<std::vector<int>> groupTri(BodyGroupCount);      // group-local triangle -> global triangle
    std::vector<QuadRemesh::SurfaceLocator> groupLoc(BodyGroupCount);
    if (tmplGroup && tgtGroup && tgtGroup->size() * 3 == targetV.size()) {
        for (size_t t = 0; t < tn.size(); ++t) {
            const int g0 = (*tgtGroup)[size_t(targetF[t * 3])], g1 = (*tgtGroup)[size_t(targetF[t * 3 + 1])], g2 = (*tgtGroup)[size_t(targetF[t * 3 + 2])];
            const int g = (g1 == g2) ? g1 : g0;   // majority of three (two agreeing, else the first)
            if (g < 0 || g >= BodyGroupCount) continue;
            for (int k = 0; k < 3; ++k) groupIdx[size_t(g)].push_back(uint32_t(targetF[t * 3 + size_t(k)]));
            groupTri[size_t(g)].push_back(int(t));
        }
        for (int g = 0; g < BodyGroupCount; ++g)
            if (groupIdx[size_t(g)].size() >= 3) groupLoc[size_t(g)].build(targetV, groupIdx[size_t(g)]);
    }
    // Closest target point for template vertex `v` at `p`, restricted to its body part when known.
    auto closestFor = [&](size_t v, const float* p, int& tri, float bary[3], float q[3]) {
        if (tmplGroup && v < tmplGroup->size()) {
            const int g = (*tmplGroup)[v];
            if (g >= 0 && g < BodyGroupCount && groupLoc[size_t(g)].valid()) {
                int local;
                if (!groupLoc[size_t(g)].closest(p, local, bary, q)) return false;
                tri = groupTri[size_t(g)][size_t(local)];
                return true;
            }
        }
        return loc.closest(p, tri, bary, q);
    };
    auto vertexNormals = [&](const std::vector<float>& P) {
        std::vector<std::array<double, 3>> n(nv, {0, 0, 0});
        for (size_t t = 0; t + 2 < tf.size(); t += 3) {
            const float *a = &P[size_t(tf[t]) * 3], *b = &P[size_t(tf[t + 1]) * 3], *c = &P[size_t(tf[t + 2]) * 3];
            const double ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
            const std::array<double, 3> f{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};   // area-weighted
            for (int k = 0; k < 3; ++k) for (int c3 = 0; c3 < 3; ++c3) n[size_t(tf[t + size_t(k)])][size_t(c3)] += f[size_t(c3)];
        }
        for (auto& v : n) { const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-20) for (double& k : v) k /= l; }
        return n;
    };

    // Vertices on an OPEN boundary (neckline, wrists, ankles of a body that has no head or hands attached) have
    // no surface of their own to match: their closest target point is whatever geometry happens to be nearby
    // (the head and hat above a neckline, a boot below an ankle) and they drag the whole body toward it.
    // Exclude them, and the two rings around them, from the correspondences; the smooth warp carries them.
    std::vector<char> skipVertex(nv, 0);
    {
        std::map<std::pair<int, int>, int> edgeUse;
        for (size_t t = 0; t + 2 < tf.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                int a = tf[t + size_t(k)], b = tf[t + size_t((k + 1) % 3)];
                if (a > b) std::swap(a, b);
                ++edgeUse[{a, b}];
            }
        std::vector<std::vector<int>> adj(nv);
        for (const auto& e : edgeUse) {
            adj[size_t(e.first.first)].push_back(e.first.second);
            adj[size_t(e.first.second)].push_back(e.first.first);
            if (e.second == 1) { skipVertex[size_t(e.first.first)] = 1; skipVertex[size_t(e.first.second)] = 1; }
        }
        for (int ring = 0; ring < 2; ++ring) {
            std::vector<char> next = skipVertex;
            for (size_t v = 0; v < nv; ++v)
                if (skipVertex[v]) for (int n : adj[v]) next[size_t(n)] = 1;
            skipVertex = std::move(next);
        }
    }

    if (extraSkip) for (size_t v = 0; v < nv && v < extraSkip->size(); ++v) if ((*extraSkip)[v]) skipVertex[v] = 1;

    // Windings may disagree between the two meshes (different importers/tools): decide by the initial
    // correspondences which sign makes the normals agree, instead of assuming outward-facing for both.
    double normalSign = 1.0;
    {
        const std::vector<std::array<double, 3>> vn0 = vertexNormals(X);
        long pos = 0, neg = 0;
        for (size_t v = 0; v < nv; v += std::max<size_t>(1, nv / 4000)) {
            int tri; float bary[3], q[3];
            if (!closestFor(v, &X[v * 3], tri, bary, q)) continue;
            const double d = vn0[v][0] * tn[size_t(tri)][0] + vn0[v][1] * tn[size_t(tri)][1] + vn0[v][2] * tn[size_t(tri)][2];
            if (d > 0.5) ++pos; else if (d < -0.5) ++neg;
        }
        if (neg > pos) normalSign = -1.0;
    }

    // control points per level, coarse -> fine; a few ICP rounds each
    const int levels[] = {24, 48, 96, 192, 400, 900, 1500};
    const double ridge[] = {1e-3, 5e-4, 1e-4, 5e-5, 1e-5, 1e-6, 1e-7};
    const int nlev = 7;
    for (int L = 0; L < nlev; ++L) {
        for (int round = 0; round < roundsPerLevel; ++round) {
            const std::vector<std::array<double, 3>> vn = vertexNormals(X);
            std::vector<std::array<double, 3>> src, dst;
            src.reserve(nv); dst.reserve(nv);
            // adaptive outlier gate: pairs further than a multiple of the median are dropped
            std::vector<double> dists;
            dists.reserve(nv);
            struct Pair { size_t v; float q[3]; double d; };
            std::vector<Pair> pairs;
            pairs.reserve(nv);
            for (size_t v = 0; v < nv; ++v) {
                if (skipVertex[v]) continue;
                int tri; float bary[3], q[3];
                if (!closestFor(v, &X[v * 3], tri, bary, q)) continue;
                const auto& n = tn[size_t(tri)];
                const double ndot = normalSign * (vn[v][0] * n[0] + vn[v][1] * n[1] + vn[v][2] * n[2]);
                if (ndot < 0.5) continue;       // normals must agree (60 deg): rejects the wrong side of thin parts
                const double dx = X[v * 3] - q[0], dy = X[v * 3 + 1] - q[1], dz = X[v * 3 + 2] - q[2];
                const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                pairs.push_back({v, {q[0], q[1], q[2]}, d});
                dists.push_back(d);
            }
            if (pairs.size() < 20) { *err = QStringLiteral("too few compatible correspondences (the target does not resemble the body)"); return false; }
            std::sort(dists.begin(), dists.end());
            const double gate = std::max(3.0 * dists[dists.size() / 2], 0.002 * diag);
            for (const Pair& p : pairs) {
                if (p.d > gate) continue;
                src.push_back({X[p.v * 3], X[p.v * 3 + 1], X[p.v * 3 + 2]});
                dst.push_back({p.q[0], p.q[1], p.q[2]});
            }
            // The joint constraints: each anchor stands for `kAnchorWeight` coincident pairs, which the voxel
            // averaging in buildWarp turns into a weight (a lone pair would be averaged away by the hundreds of
            // surface pairs sharing its cell).
            if (anchors) {
                constexpr int kAnchorWeight = 150;
                for (const IcpAnchor& an : *anchors)
                    for (int m = 0; m < kAnchorWeight; ++m) { src.push_back(an.pos); dst.push_back(an.target); }
            }
            Options wo;
            wo.maxControlPoints = levels[L];
            wo.regularisation = ridge[L];
            QString werr;
            const Warp w = buildWarp(src, dst, wo, &werr);
            if (!w.valid) { *err = QStringLiteral("smooth warp failed: %1").arg(werr); return false; }
            for (size_t v = 0; v < nv; ++v) {
                const auto d = w.displacement({double(X[v * 3]), double(X[v * 3 + 1]), double(X[v * 3 + 2])});
                X[v * 3] += float(d[0]); X[v * 3 + 1] += float(d[1]); X[v * 3 + 2] += float(d[2]);
            }
            if (anchors)
                for (IcpAnchor& an : *anchors) {
                    const auto d = w.displacement(an.pos);
                    for (int k = 0; k < 3; ++k) an.pos[size_t(k)] += d[size_t(k)];
                }
        }
    }
    // Detail stage: the smooth warp cannot express anything finer than its control spacing (~7 cm at 1500
    // points over a body), which is what keeps a bust or a belly when the target has none. Move the VERTICES
    // toward their (normal-gated) closest target points, with the displacement field smoothed over the mesh
    // so neighbours move together (correspondences must not slide), and the anchors (the skeleton's joints)
    // pinned where they are.
    if (detailRounds > 0) {
        std::vector<std::vector<int>> adj(nv);
        {
            std::set<std::pair<int, int>> seen;
            for (size_t t = 0; t + 2 < tf.size(); t += 3)
                for (int k = 0; k < 3; ++k) {
                    int a = tf[t + size_t(k)], b = tf[t + size_t((k + 1) % 3)];
                    if (a > b) std::swap(a, b);
                    if (seen.insert({a, b}).second) { adj[size_t(a)].push_back(b); adj[size_t(b)].push_back(a); }
                }
        }
        for (int round = 0; round < detailRounds; ++round) {
            const std::vector<std::array<double, 3>> vn = vertexNormals(X);
            std::vector<std::array<double, 3>> D(nv, {0, 0, 0});
            std::vector<char> known(nv, 0);
            std::vector<double> dists;
            std::vector<size_t> ids;
            for (size_t v = 0; v < nv; ++v) {
                if (skipVertex[v]) continue;
                int tri; float bary[3], q[3];
                if (!closestFor(v, &X[v * 3], tri, bary, q)) continue;
                const auto& n = tn[size_t(tri)];
                if (normalSign * (vn[v][0] * n[0] + vn[v][1] * n[1] + vn[v][2] * n[2]) < 0.5) continue;
                D[v] = {double(q[0]) - X[v * 3], double(q[1]) - X[v * 3 + 1], double(q[2]) - X[v * 3 + 2]};
                known[v] = 1;
                dists.push_back(std::sqrt(D[v][0] * D[v][0] + D[v][1] * D[v][1] + D[v][2] * D[v][2]));
                ids.push_back(v);
            }
            if (dists.empty()) break;
            std::vector<double> sorted = dists;
            std::sort(sorted.begin(), sorted.end());
            const double gate = std::max(3.0 * sorted[sorted.size() / 2], 0.002 * diag);
            for (size_t k = 0; k < ids.size(); ++k)
                if (dists[k] > gate) { known[ids[k]] = 0; D[ids[k]] = {0, 0, 0}; }
            // smooth the displacement field over the mesh: unknown vertices take their neighbours' mean,
            // known ones blend halfway toward it
            for (int it = 0; it < 12; ++it) {
                std::vector<std::array<double, 3>> N = D;
                for (size_t v = 0; v < nv; ++v) {
                    if (adj[v].empty()) continue;
                    std::array<double, 3> m{0, 0, 0};
                    for (int nb : adj[v]) for (int k = 0; k < 3; ++k) m[size_t(k)] += D[size_t(nb)][size_t(k)];
                    for (int k = 0; k < 3; ++k) m[size_t(k)] /= double(adj[v].size());
                    const double s = known[v] ? 0.5 : 1.0;
                    for (int k = 0; k < 3; ++k) N[v][size_t(k)] = (1 - s) * D[v][size_t(k)] + s * m[size_t(k)];
                }
                D = std::move(N);
            }
            const double step = 0.6;
            for (size_t v = 0; v < nv; ++v)
                for (int k = 0; k < 3; ++k) X[v * 3 + size_t(k)] += float(step * D[v][size_t(k)]);
        }
    }
    return true;
}

}  // namespace

namespace {

// Every imported mesh's positions + triangles, concatenated (positions in rig frame). `offset[mi]` is where
// mesh `mi` starts. Returns the imported mesh indices (empty = nothing to register).
std::vector<size_t> buildTemplate(const Rig& rig, std::vector<float>& tv, std::vector<int>& tf, std::vector<size_t>& offset)
{
    const std::vector<size_t> imported = importedMeshIndices(rig);
    offset.assign(rig.meshes.size(), 0);
    for (size_t mi : imported) {
        const Mesh& m = rig.meshes[mi];
        offset[mi] = tv.size() / 3;
        tv.insert(tv.end(), m.positions.begin(), m.positions.end());
        for (const auto& face : m.faces) {
            if (face.size() < 3) continue;
            bool valid = true;
            for (uint32_t li : face) valid = valid && li < m.layouts.size() && m.layouts[li].position < m.positions.size() / 3;
            if (!valid) continue;   // a corrupt face must not take the registration down with it
            auto pos = [&](size_t k) { return int(offset[mi] + m.layouts[face[k]].position); };
            for (size_t k = 1; k + 1 < face.size(); ++k) { tf.push_back(pos(0)); tf.push_back(pos(k)); tf.push_back(pos(k + 1)); }
        }
    }
    return imported;
}

}  // namespace

RegisterResult registerToSurface(const Rig& rig, const std::vector<float>& targetV, const std::vector<int>& targetF,
                                 const RegisterOptions& options)
{
    RegisterResult r;
    if (targetV.size() < 9 || targetF.size() < 3) { r.error = QStringLiteral("the target surface is empty"); return r; }
    for (float v : targetV) if (!std::isfinite(v)) { r.error = QStringLiteral("the target surface has non-finite positions"); return r; }
    for (int i : targetF) if (i < 0 || size_t(i) * 3 + 2 >= targetV.size()) { r.error = QStringLiteral("the target surface has an out-of-range triangle index"); return r; }

    // ---- the template: every imported mesh's positions + triangles, concatenated ----
    std::vector<float> tv;
    std::vector<int> tf;
    std::vector<size_t> offset;
    const std::vector<size_t> imported = buildTemplate(rig, tv, tf, offset);
    if (imported.empty()) { r.error = QStringLiteral("the rig has no mesh to register"); return r; }
    r.templateVertices = int(tv.size() / 3);
    if (tf.empty()) { r.error = QStringLiteral("the rig mesh has no triangles"); return r; }

    // ---- pre-align: uniform scale by height, feet and centre matched (Y is up) ----
    Bounds tb, ub;
    for (size_t i = 0; i + 2 < tv.size(); i += 3) tb.add(&tv[i]);
    for (size_t i = 0; i + 2 < targetV.size(); i += 3) ub.add(&targetV[i]);
    const double th = double(tb.mx[1] - tb.mn[1]), uh = double(ub.mx[1] - ub.mn[1]);
    if (th <= 0 || uh <= 0) { r.error = QStringLiteral("a surface with no height cannot be aligned"); return r; }
    double s = 1.0;
    std::array<double, 3> shift{0, 0, 0};
    if (options.alignHeight) {
        s = uh / th;
        const double tcx = 0.5 * double(tb.mn[0] + tb.mx[0]), tcz = 0.5 * double(tb.mn[2] + tb.mx[2]);
        const double ucx = 0.5 * double(ub.mn[0] + ub.mx[0]), ucz = 0.5 * double(ub.mn[2] + ub.mx[2]);
        shift = {ucx - s * tcx, double(ub.mn[1]) - s * double(tb.mn[1]), ucz - s * tcz};
    }
    r.alignScale = s;
    std::vector<float> aligned(tv.size());
    for (size_t i = 0; i + 2 < tv.size(); i += 3)
        for (int k = 0; k < 3; ++k) aligned[i + size_t(k)] = float(s * double(tv[i + size_t(k)]) + shift[size_t(k)]);

    const std::vector<std::array<double, 3>> jointPos = jointWorldPositions(rig);
    // Options that only the NRICP method understands must not be dropped silently.
    if (options.method == RegisterOptions::Method::Smooth) {
        if (!options.stiffness.empty()) r.warnings << QStringLiteral("a stiffness schedule only applies to the NRICP method; the smooth wrap ignored it");
        if (!options.jointAnchors.empty()) r.warnings << QStringLiteral("joint anchors only apply to the NRICP method; the smooth wrap ignored them");
    }
    FaceRig::NricpOptions no;
    no.skipPrealign = options.alignHeight;   // our own feet/height/centre alignment is the start; do not undo it
    if (!options.stiffness.empty()) no.stiffness = options.stiffness;
    for (const auto& a : options.jointAnchors) {
        int ji = -1;
        for (size_t j = 0; j < rig.joints.size(); ++j) if (rig.joints[j].name == a.first) { ji = int(j); break; }
        if (ji < 0) { r.warnings << QStringLiteral("anchor joint '%1' not found").arg(a.first); continue; }
        const std::array<double, 3> w = jointPos[size_t(ji)];
        double best = 1e300; int bv = -1;
        for (size_t v = 0; v + 2 < aligned.size(); v += 3) {
            double d = 0;
            for (int k = 0; k < 3; ++k) { const double dd = double(aligned[v + size_t(k)]) - (s * w[size_t(k)] + shift[size_t(k)]); d += dd * dd; }
            if (d < best) { best = d; bv = int(v / 3); }
        }
        if (bv >= 0) no.landmarks.push_back({bv, a.second});
    }

    FaceRig::NricpResult nr;
    if (options.method == RegisterOptions::Method::Smooth) {
        std::vector<float> X = aligned;
        QString serr;
        if (!smoothIcp(X, tf, targetV, targetF, std::max(1, options.rounds), options.detailRounds, &serr)) { r.error = serr; return r; }
        // residual to the target surface, for the report
        std::vector<uint32_t> tidx(targetF.begin(), targetF.end());
        QuadRemesh::SurfaceLocator loc;
        loc.build(targetV, tidx);
        nr.fitted = X;
        nr.diag = ub.diag();
        double sum = 0;
        for (size_t v = 0; v * 3 + 2 < X.size(); ++v) {
            int tri; float bary[3], q[3];
            double d = 0;
            if (loc.closest(&X[v * 3], tri, bary, q)) d = std::sqrt(double(X[v*3]-q[0])*(X[v*3]-q[0]) + double(X[v*3+1]-q[1])*(X[v*3+1]-q[1]) + double(X[v*3+2]-q[2])*(X[v*3+2]-q[2]));
            nr.residual.push_back(float(d));
            sum += d;
            nr.maxResidual = std::max(nr.maxResidual, d);
        }
        nr.meanResidual = nr.residual.empty() ? 0 : sum / double(nr.residual.size());
        nr.ok = true;
    } else {
        nr = FaceRig::fit(aligned, tf, targetV, targetF, no);
    }
    if (!nr.ok || nr.fitted.size() != tv.size()) { r.error = QStringLiteral("the non-rigid registration did not converge"); return r; }

    r.positions.assign(rig.meshes.size(), {});
    for (size_t mi : imported) {
        const size_t np = rig.meshes[mi].positions.size() / 3;
        r.positions[mi].assign(nr.fitted.begin() + std::ptrdiff_t(offset[mi] * 3), nr.fitted.begin() + std::ptrdiff_t((offset[mi] + np) * 3));
    }
    std::vector<float> res = nr.residual;
    std::sort(res.begin(), res.end());
    const double diag = nr.diag > 0 ? nr.diag : ub.diag();
    r.meanResidualPct = 100.0 * nr.meanResidual / diag;
    r.maxResidualPct = 100.0 * nr.maxResidual / diag;
    r.p95ResidualPct = res.empty() ? 0 : 100.0 * double(res[size_t(double(res.size() - 1) * 0.95)]) / diag;
    r.ok = true;
    return r;
}

std::vector<std::pair<QString, QStringList>> defaultJointNameMap()
{
    auto side = [](const char* base, const QStringList& l, const QStringList& r, std::vector<std::pair<QString, QStringList>>& out) {
        out.push_back({QString::fromLatin1(base) + "_l", l});
        out.push_back({QString::fromLatin1(base) + "_r", r});
    };
    std::vector<std::pair<QString, QStringList>> m;
    m.push_back({"pelvis", {"Hips", "hips", "pelvis", "Pelvis", "mixamorig:Hips", "mixamorig_Hips"}});
    m.push_back({"spine_03", {"Spine1", "spine1", "Spine_1", "mixamorig:Spine1", "mixamorig_Spine1"}});
    m.push_back({"spine_05", {"Spine2", "spine2", "Chest", "UpperChest", "mixamorig:Spine2", "mixamorig_Spine2"}});
    m.push_back({"neck_01", {"Neck", "neck", "mixamorig:Neck", "mixamorig_Neck"}});
    m.push_back({"head", {"Head", "head", "mixamorig:Head", "mixamorig_Head"}});
    side("upperarm", {"LeftArm", "left_arm", "L_UpperArm", "UpperArm_L", "mixamorig:LeftArm", "mixamorig_LeftArm"},
         {"RightArm", "right_arm", "R_UpperArm", "UpperArm_R", "mixamorig:RightArm", "mixamorig_RightArm"}, m);
    side("lowerarm", {"LeftForeArm", "left_forearm", "L_LowerArm", "LowerArm_L", "mixamorig:LeftForeArm", "mixamorig_LeftForeArm"},
         {"RightForeArm", "right_forearm", "R_LowerArm", "LowerArm_R", "mixamorig:RightForeArm", "mixamorig_RightForeArm"}, m);
    side("hand", {"LeftHand", "left_hand", "L_Hand", "Hand_L", "mixamorig:LeftHand", "mixamorig_LeftHand"},
         {"RightHand", "right_hand", "R_Hand", "Hand_R", "mixamorig:RightHand", "mixamorig_RightHand"}, m);
    side("thigh", {"LeftUpLeg", "left_upleg", "L_UpperLeg", "UpperLeg_L", "mixamorig:LeftUpLeg", "mixamorig_LeftUpLeg"},
         {"RightUpLeg", "right_upleg", "R_UpperLeg", "UpperLeg_R", "mixamorig:RightUpLeg", "mixamorig_RightUpLeg"}, m);
    side("calf", {"LeftLeg", "left_leg", "L_LowerLeg", "LowerLeg_L", "mixamorig:LeftLeg", "mixamorig_LeftLeg"},
         {"RightLeg", "right_leg", "R_LowerLeg", "LowerLeg_R", "mixamorig:RightLeg", "mixamorig_RightLeg"}, m);
    side("foot", {"LeftFoot", "left_foot", "L_Foot", "Foot_L", "mixamorig:LeftFoot", "mixamorig_LeftFoot"},
         {"RightFoot", "right_foot", "R_Foot", "Foot_R", "mixamorig:RightFoot", "mixamorig_RightFoot"}, m);
    return m;
}

std::vector<JointAnchor> matchJointAnchors(const std::vector<std::pair<QString, std::array<double, 3>>>& targetBones,
                                           QStringList* unmatched)
{
    std::vector<JointAnchor> out;
    for (const auto& entry : defaultJointNameMap()) {
        bool found = false;
        for (const QString& cand : entry.second) {
            for (const auto& b : targetBones)
                if (b.first.compare(cand, Qt::CaseInsensitive) == 0) { out.push_back({entry.first, b.second}); found = true; break; }
            if (found) break;
        }
        if (!found && unmatched) *unmatched << entry.first;
    }
    return out;
}

JointRegisterResult registerByJoints(const Rig& rigIn, const std::vector<JointAnchor>& anchorsIn, bool alignLimbPose)
{
    JointRegisterResult r;
    std::vector<JointAnchor> anchors = anchorsIn;
    Rig rig = rigIn;
    r.alignedRig = rigIn;
    // The two characters may face different ways (a MetaHuman faces -Z; a glTF/Mixamo asset
    // usually +Z). Left/right pair correctly BY NAME in both, but the body must be turned to the
    // target's facing first or every direction step sees a ~180 degree twist. Facing is derived
    // from the skeleton: left-minus-right is the lateral axis and facing = lateral x up (Y).
    {
        auto facingOf = [](const std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>>& pairs) {
            std::array<double, 3> l{0, 0, 0};
            for (const auto& p : pairs) for (int k = 0; k < 3; ++k) l[size_t(k)] += p.first[size_t(k)] - p.second[size_t(k)];
            return std::array<double, 2>{-l[2], l[0]};   // (x, z) of lateral x Y
        };
        const std::vector<std::array<double, 3>> jp0 = jointWorldPositions(rigIn);
        auto rigPos = [&](const QString& n, std::array<double, 3>& p) { for (size_t j = 0; j < rigIn.joints.size(); ++j) if (rigIn.joints[j].name == n) { p = jp0[j]; return true; } return false; };
        auto tgtPos = [&](const QString& n, std::array<double, 3>& p) { for (const auto& a : anchors) if (a.bodyJoint == n) { p = a.target; return true; } return false; };
        std::vector<std::pair<std::array<double, 3>, std::array<double, 3>>> sp, tp;
        for (const char* base : {"upperarm", "hand", "thigh", "foot"}) {
            std::array<double, 3> sl, sr, tl, tr;
            if (!rigPos(QString::fromLatin1(base) + "_l", sl) || !rigPos(QString::fromLatin1(base) + "_r", sr) ||
                !tgtPos(QString::fromLatin1(base) + "_l", tl) || !tgtPos(QString::fromLatin1(base) + "_r", tr)) continue;
            sp.push_back({sl, sr});
            tp.push_back({tl, tr});
        }
        if (!sp.empty()) {
            const auto fs = facingOf(sp), ft = facingOf(tp);
            if (std::hypot(fs[0], fs[1]) > 1e-9 && std::hypot(ft[0], ft[1]) > 1e-9) {
                // yaw about +Y taking fs onto ft: R(theta) maps (x,z) -> (x c + z s, -x s + z c)
                const double theta = std::atan2(fs[0] * ft[1] - fs[1] * ft[0] , fs[0] * ft[0] + fs[1] * ft[1]);
                const double deg = std::abs(theta) * 180.0 / 3.14159265358979;
                if (deg > 30.0) {
                    yawRig(rig, -theta);
                    r.yawDegrees = -theta * 180.0 / 3.14159265358979;
                    r.alignedRig = rig;
                    r.warnings << QStringLiteral("the target faces %1 degrees away from the rig; the body was turned about Y to face it")
                                      .arg(std::abs(r.yawDegrees), 0, 'f', 0);
                }
            }
        }
    }
    if (alignLimbPose) {
        std::vector<std::pair<QString, std::array<double, 3>>> tj;
        for (const JointAnchor& a : anchors) tj.push_back({a.bodyJoint, a.target});
        LimbAlignment la = alignLimbs(rig, tj);
        r.limbChainsAligned = la.chainsAligned;
        r.limbAngles = la.applied;
        if (la.chainsAligned > 0) { rig = la.rig; r.alignedRig = la.rig; }
    }
    const std::vector<std::array<double, 3>> jp = jointWorldPositions(rig);
    std::vector<std::array<double, 3>> src, dst;
    for (const JointAnchor& a : anchors) {
        int ji = -1;
        for (size_t j = 0; j < rig.joints.size(); ++j) if (rig.joints[j].name == a.bodyJoint) { ji = int(j); break; }
        if (ji < 0) { r.warnings << QStringLiteral("rig joint '%1' not found").arg(a.bodyJoint); continue; }
        src.push_back(jp[size_t(ji)]);
        dst.push_back(a.target);
    }
    r.anchorsUsed = int(src.size());
    const size_t anchorCount = src.size();
    if (src.size() < 6) { r.error = QStringLiteral("only %1 joints could be paired with the target's skeleton (need at least 6)").arg(src.size()); return r; }
    // A T-posed character's joints are (nearly) coplanar, so a warp fitted to them alone
    // has no depth information and squashes the body flat. Constrain depth to the overall
    // similarity scale with ghost points offset along the depth axis (Z) of every anchor.
    // Only when the target's joints really ARE (nearly) planar: a skeleton with genuine depth offsets (Mixamo's
    // knees and spine) would be dragged back toward a uniform depth scale by the ghosts, and the lower body ended
    // up 2-3 cm behind the target's legs.
    double dstYRange = 0, dstZRange = 0;
    {
        double ylo = 1e300, yhi = -1e300, zlo = 1e300, zhi = -1e300;
        for (const auto& p : dst) { ylo = std::min(ylo, p[1]); yhi = std::max(yhi, p[1]); zlo = std::min(zlo, p[2]); zhi = std::max(zhi, p[2]); }
        dstYRange = yhi - ylo; dstZRange = zhi - zlo;
    }
    const bool planarTarget = dstZRange < 0.06 * std::max(dstYRange, 1e-9);
    if (planarTarget) {
        std::array<double, 3> sc{0, 0, 0}, dc{0, 0, 0};
        for (size_t i = 0; i < src.size(); ++i) for (int k = 0; k < 3; ++k) { sc[size_t(k)] += src[i][size_t(k)] / double(src.size()); dc[size_t(k)] += dst[i][size_t(k)] / double(src.size()); }
        double ss = 0, ds = 0, ymin = 1e300, ymax = -1e300;
        for (size_t i = 0; i < src.size(); ++i) {
            for (int k = 0; k < 3; ++k) { ss += (src[i][size_t(k)] - sc[size_t(k)]) * (src[i][size_t(k)] - sc[size_t(k)]); ds += (dst[i][size_t(k)] - dc[size_t(k)]) * (dst[i][size_t(k)] - dc[size_t(k)]); }
            ymin = std::min(ymin, src[i][1]); ymax = std::max(ymax, src[i][1]);
        }
        const double scale = ss > 0 ? std::sqrt(ds / ss) : 1.0;
        const double e = 0.12 * (ymax - ymin);
        const size_t n = src.size();
        for (size_t i = 0; i < n; ++i)
            for (int sgn = -1; sgn <= 1; sgn += 2) {
                auto a = src[i]; auto b = dst[i];
                a[2] += sgn * e; b[2] += sgn * e * scale;
                src.push_back(a); dst.push_back(b);
            }
    }
    Options o;
    o.maxControlPoints = 64;
    QString err;
    const Warp w = buildWarp(src, dst, o, &err);
    if (!w.valid) { r.error = QStringLiteral("could not build the joint warp: %1").arg(err); return r; }
    double worst = 0;
    for (size_t i = 0; i < anchorCount; ++i) {
        const auto d = w.displacement(src[i]);
        double e = 0;
        for (int k = 0; k < 3; ++k) { const double v = src[i][size_t(k)] + d[size_t(k)] - dst[i][size_t(k)]; e += v * v; }
        worst = std::max(worst, std::sqrt(e));
    }
    r.anchorResidualMm = worst * 1000.0;
    r.warpedJointPositions = jp;
    for (auto& p : r.warpedJointPositions) {
        const auto d = w.displacement(p);
        for (int k = 0; k < 3; ++k) p[size_t(k)] += d[size_t(k)];
    }
    r.positions.assign(rig.meshes.size(), {});
    for (size_t mi : importedMeshIndices(rig)) {
        const auto& P = rig.meshes[mi].positions;
        auto& out = r.positions[mi];
        out.resize(P.size());
        for (size_t i = 0; i + 2 < P.size(); i += 3) {
            const std::array<double, 3> p{double(P[i]), double(P[i + 1]), double(P[i + 2])};
            const auto d = w.displacement(p);
            for (int k = 0; k < 3; ++k) out[i + size_t(k)] = float(p[size_t(k)] + d[size_t(k)]);
        }
    }
    r.ok = true;
    return r;
}

JointRegisterResult registerJointsThenSurface(const Rig& rig, const std::vector<JointAnchor>& anchors,
                                              const std::vector<float>& targetV, const std::vector<int>& targetF,
                                              bool alignLimbPose, int rounds, int detailRounds,
                                              const std::vector<int>* targetVertexGroup)
{
    // 1. skeleton: facing, limb pose and proportions (a smooth warp from matched joints)
    JointRegisterResult r = registerByJoints(rig, anchors, alignLimbPose);
    if (!r.ok) return r;
    const bool symmetrize = qgetenv("QTMESH_WRAP_SYMMETRIZE") != "0";
    // 2. surface: refine the shape from that start. The template is the ALIGNED rig's meshes carried through
    //    the joint warp; the ICP then pulls it onto the surface.
    std::vector<float> X;
    std::vector<int> tf;
    std::vector<size_t> offset;
    const std::vector<size_t> imported = buildTemplate(r.alignedRig, X, tf, offset);
    if (imported.empty() || tf.empty()) { r.ok = false; r.error = QStringLiteral("the rig has no mesh to register"); return r; }
    if (targetV.size() < 9 || targetF.size() < 3) { r.ok = false; r.error = QStringLiteral("the target surface is empty"); return r; }
    for (float v : targetV) if (!std::isfinite(v)) { r.ok = false; r.error = QStringLiteral("the target surface has non-finite positions"); return r; }
    for (int i : targetF) if (i < 0 || size_t(i) * 3 + 2 >= targetV.size()) { r.ok = false; r.error = QStringLiteral("the target surface has an out-of-range triangle index"); return r; }
    // start from the joint-warped positions
    for (size_t mi : imported) {
        const auto& P = r.positions[mi];
        if (P.size() != r.alignedRig.meshes[mi].positions.size()) { r.ok = false; r.error = QStringLiteral("internal: joint warp / mesh size mismatch"); return r; }
        std::copy(P.begin(), P.end(), X.begin() + std::ptrdiff_t(offset[mi] * 3));
    }
    // The matched joints stay pinned to the target's bones through the refinement, so the surface can reshape
    // the body but not lift/shift it away from its skeleton (measured without this: the upper body ended
    // ~45 mm above the officer's bones, mean 40 mm joint drift).
    // body part of every template vertex (dominant skin joint's limb) so limbs cannot grab each other
    std::vector<int> tmplGroup(X.size() / 3, GroupTorso);
    for (size_t mi : imported) {
        const Mesh& m = r.alignedRig.meshes[mi];
        for (size_t v = 0; v < m.positions.size() / 3 && v < m.skin.size(); ++v) {
            uint16_t bj = 0; float bw = -1;
            for (const SkinInfluence& inf : m.skin[v]) if (inf.weight > bw) { bw = inf.weight; bj = inf.joint; }
            if (bw >= 0) tmplGroup[offset[mi] + v] = bodyGroupOfJoint(r.alignedRig, int(bj));
        }
    }
    // Hands and feet are not matched to the surface: a target's shoes and gloves are clothing, and pulling a bare
    // foot onto a boot bends and swells it. They follow the skeleton alignment (and the warp) instead.
    std::vector<char> extremity(X.size() / 3, 0);
    for (size_t mi : imported) {
        const Mesh& m = r.alignedRig.meshes[mi];
        for (size_t v = 0; v < m.positions.size() / 3 && v < m.skin.size(); ++v) {
            uint16_t bj = 0; float bw = -1;
            for (const SkinInfluence& inf : m.skin[v]) if (inf.weight > bw) { bw = inf.weight; bj = inf.joint; }
            if (bw < 0) continue;
            for (int j = int(bj), guard = 0; j >= 0 && size_t(j) < r.alignedRig.joints.size() && guard < 64; j = r.alignedRig.joints[size_t(j)].parent, ++guard) {
                const QString n = r.alignedRig.joints[size_t(j)].name;
                if (n.startsWith(QStringLiteral("hand")) || n.startsWith(QStringLiteral("foot")) || n.startsWith(QStringLiteral("ball")) || n == QStringLiteral("head") || n.startsWith(QStringLiteral("neck"))) { extremity[offset[mi] + v] = 1; break; }
            }
        }
    }
    std::vector<IcpAnchor> icpAnchors;
    for (const JointAnchor& an : anchors)
        for (size_t j = 0; j < r.alignedRig.joints.size() && j < r.warpedJointPositions.size(); ++j)
            if (r.alignedRig.joints[j].name == an.bodyJoint) { icpAnchors.push_back({r.warpedJointPositions[j], an.target}); break; }
    QString err;
    if (!smoothIcp(X, tf, targetV, targetF, std::max(1, rounds), detailRounds, &err, &icpAnchors, &tmplGroup, targetVertexGroup, &extremity)) {
        r.ok = false;
        r.error = QStringLiteral("surface refinement failed: %1").arg(err);
        return r;
    }
    // A body is left/right symmetric but a target's clothing is not (one trouser leg folds, one sleeve hangs
    // differently). Average each vertex's displacement with its mirror partner's (the template is symmetric, so the
    // partner is the vertex nearest to the mirrored start position), which keeps the target's overall shape and drops
    // the one-sided cloth wrinkles.
    if (symmetrize) {
        const size_t N = X.size() / 3;
        std::vector<std::array<float, 3>> st(N);
        for (size_t mi : imported) {
            const auto& P = r.positions[mi];
            for (size_t v = 0; v < P.size() / 3; ++v) st[offset[mi] + v] = {P[v * 3], P[v * 3 + 1], P[v * 3 + 2]};
        }
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (const auto& q : st) for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], q[size_t(a)]); hi[a] = std::max(hi[a], q[size_t(a)]); }
        const float diag = std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
        // the side axis is the one the left/right limbs differ along: the widest horizontal extent
        int side = (hi[0] - lo[0] >= hi[2] - lo[2]) ? 0 : 2;
        const float cx = 0.5f * (lo[size_t(side)] + hi[size_t(side)]);
        const float cell = std::max(1e-4f, diag * 0.01f);
        auto cellOf = [&](float v) { return (long long)std::floor(v / cell); };
        auto key = [](long long a, long long b, long long c) { return (a * 73856093LL) ^ (b * 19349663LL) ^ (c * 83492791LL); };
        std::unordered_map<long long, std::vector<size_t>> grid;
        for (size_t i = 0; i < N; ++i) grid[key(cellOf(st[i][0]), cellOf(st[i][1]), cellOf(st[i][2]))].push_back(i);
        std::vector<std::array<float, 3>> d(N);
        for (size_t i = 0; i < N; ++i) for (int a = 0; a < 3; ++a) d[i][size_t(a)] = X[i * 3 + size_t(a)] - st[i][size_t(a)];
        std::vector<float> maxDist(1, diag * 0.02f);
        // a whole-body shift is not asymmetry: symmetrise the deviation from the mean displacement only
        double md[3] = {0, 0, 0};
        for (size_t i = 0; i < N; ++i) for (int a = 0; a < 3; ++a) md[a] += d[i][size_t(a)];
        for (int a = 0; a < 3; ++a) md[a] /= double(std::max<size_t>(1, N));
        for (size_t i = 0; i < N; ++i) for (int a = 0; a < 3; ++a) d[i][size_t(a)] -= float(md[a]);
        for (size_t i = 0; i < N; ++i) {
            if (extremity[i]) continue;
            std::array<float, 3> m = st[i];
            m[size_t(side)] = 2 * cx - m[size_t(side)];
            const long long ca = cellOf(m[0]), cb = cellOf(m[1]), cc = cellOf(m[2]);
            double best = 1e30; size_t bj = i;
            for (long long da = -1; da <= 1; ++da) for (long long db = -1; db <= 1; ++db) for (long long dc = -1; dc <= 1; ++dc) {
                auto g = grid.find(key(ca + da, cb + db, cc + dc));
                if (g == grid.end()) continue;
                for (size_t j : g->second) {
                    const double dx = st[j][0] - m[0], dy = st[j][1] - m[1], dz = st[j][2] - m[2];
                    const double dd = dx * dx + dy * dy + dz * dz;
                    if (dd < best) { best = dd; bj = j; }
                }
            }
            if (bj == i || best > double(maxDist[0]) * double(maxDist[0])) continue;
            std::array<float, 3> dm = d[bj];
            dm[size_t(side)] = -dm[size_t(side)];
            for (int a = 0; a < 3; ++a) X[i * 3 + size_t(a)] = st[i][size_t(a)] + float(md[a]) + 0.5f * (d[i][size_t(a)] + dm[size_t(a)]);
        }
    }
    // The surface warp is global and smooth, so it also scaled the (unmatched) hands and feet along with the baggy
    // trousers/sleeves above them. Put each extremity back to its skeleton-aligned shape and carry it RIGIDLY by
    // the mean displacement of the 40 nearest matched vertices of the same limb, so it follows without growing.
    {
        std::vector<std::array<float, 3>> start(X.size() / 3);
        for (size_t mi : imported) {
            const auto& P = r.positions[mi];
            for (size_t v = 0; v < P.size() / 3; ++v) start[offset[mi] + v] = {P[v * 3], P[v * 3 + 1], P[v * 3 + 2]};
        }
        for (int g = 0; g < 5; ++g) {
            std::vector<size_t> ext, body;
            for (size_t i = 0; i < extremity.size(); ++i) {
                if (tmplGroup[i] != g) continue;
                (extremity[i] ? ext : body).push_back(i);
            }
            if (ext.empty() || body.empty()) continue;
            double c[3] = {0, 0, 0};
            for (size_t i : ext) for (int a = 0; a < 3; ++a) c[a] += start[i][size_t(a)];
            for (int a = 0; a < 3; ++a) c[a] /= double(ext.size());
            std::vector<std::pair<double, size_t>> d;
            d.reserve(body.size());
            for (size_t b : body) {
                const double dx = start[b][0] - c[0], dy = start[b][1] - c[1], dz = start[b][2] - c[2];
                d.push_back({dx * dx + dy * dy + dz * dz, b});
            }
            const size_t k = std::min<size_t>(40, d.size());
            std::partial_sort(d.begin(), d.begin() + std::ptrdiff_t(k), d.end());
            double t[3] = {0, 0, 0};
            for (size_t q = 0; q < k; ++q) for (int a = 0; a < 3; ++a) t[a] += X[d[q].second * 3 + size_t(a)] - start[d[q].second][size_t(a)];
            for (int a = 0; a < 3; ++a) t[a] /= double(k);
            // blend: at the joint with the matched limb the surface result is kept (no step where the calf grew), and
            // it fades to the rigid carry over the next few centimetres, so the foot keeps its own proportions
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t b : body) for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], start[b][size_t(a)]); hi[a] = std::max(hi[a], start[b][size_t(a)]); }
            const double diag = std::sqrt(double((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2])));
            const double d0 = 0.004 * diag, d1 = 0.045 * diag;
            std::vector<size_t> sub;   // body vertices nearest the extremity, to keep the distance search cheap
            {
                std::vector<std::pair<double, size_t>> bd;
                for (size_t b : body) {
                    const double dx = start[b][0] - c[0], dy = start[b][1] - c[1], dz = start[b][2] - c[2];
                    bd.push_back({dx * dx + dy * dy + dz * dz, b});
                }
                const size_t m = std::min<size_t>(1500, bd.size());
                std::partial_sort(bd.begin(), bd.begin() + std::ptrdiff_t(m), bd.end());
                for (size_t q = 0; q < m; ++q) sub.push_back(bd[q].second);
            }
            for (size_t i : ext) {
                double nd = 1e30;
                for (size_t b : sub) {
                    const double dx = start[b][0] - start[i][0], dy = start[b][1] - start[i][1], dz = start[b][2] - start[i][2];
                    nd = std::min(nd, dx * dx + dy * dy + dz * dz);
                }
                nd = std::sqrt(nd);
                double w = 1.0 - std::min(1.0, std::max(0.0, (nd - d0) / (d1 - d0)));
                w = w * w * (3 - 2 * w);
                for (int a = 0; a < 3; ++a) {
                    const float rigid = start[i][size_t(a)] + float(t[a]);
                    X[i * 3 + size_t(a)] = rigid + float(w) * (X[i * 3 + size_t(a)] - rigid);
                }
            }
        }
    }
    for (size_t mi : imported) {
        const size_t np = r.alignedRig.meshes[mi].positions.size() / 3;
        r.positions[mi].assign(X.begin() + std::ptrdiff_t(offset[mi] * 3), X.begin() + std::ptrdiff_t((offset[mi] + np) * 3));
    }
    r.surfaceRefined = true;
    r.warnings << QStringLiteral("shape refined onto the target surface after the skeleton alignment");
    return r;
}

}  // namespace Fit
}  // namespace Dna
