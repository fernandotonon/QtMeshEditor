#include "HairFitting.h"

#include <QJsonObject>

#include <algorithm>
#include <cmath>

namespace CharacterGen {
namespace Hair {

namespace {

float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
float smooth01(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }

V3 rotateXYZ(const V3& v, const V3& deg)
{
    const float rx = deg.x * 3.14159265f / 180.f, ry = deg.y * 3.14159265f / 180.f, rz = deg.z * 3.14159265f / 180.f;
    V3 p = v;
    p = {p.x, p.y * std::cos(rx) - p.z * std::sin(rx), p.y * std::sin(rx) + p.z * std::cos(rx)};
    p = {p.x * std::cos(ry) + p.z * std::sin(ry), p.y, -p.x * std::sin(ry) + p.z * std::cos(ry)};
    p = {p.x * std::cos(rz) - p.y * std::sin(rz), p.x * std::sin(rz) + p.y * std::cos(rz), p.z};
    return p;
}

float percentile(std::vector<float> v, float q)
{
    if (v.empty()) return 0.0f;
    std::sort(v.begin(), v.end());
    return v[size_t(clampf(q, 0, 1) * float(v.size() - 1))];
}

std::vector<V3> recomputeNormals(const std::vector<V3>& pos, const std::vector<uint32_t>& idx)
{
    std::vector<V3> n(pos.size());
    for (size_t t = 0; t + 2 < idx.size(); t += 3) {
        if (idx[t] >= pos.size() || idx[t + 1] >= pos.size() || idx[t + 2] >= pos.size()) continue;
        const V3 f = cross(pos[idx[t + 1]] - pos[idx[t]], pos[idx[t + 2]] - pos[idx[t]]);
        n[idx[t]] += f; n[idx[t + 1]] += f; n[idx[t + 2]] += f;
    }
    for (V3& v : n) v = normalized(v);
    return n;
}

// Step 1: place the source on the head. Returns the transformed positions.
std::vector<V3> normalise(const ImportedHair& hair, const HeadFrame& head, const FitAdjust& adj, FitReport* rep)
{
    std::vector<V3> p = hair.pos;
    if (p.empty()) return p;
    V3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (const V3& v : p) {
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
    }
    V3 centre(0, 0, 0);
    float scale = 1.0f;
    float topY = hi.y;
    if (adj.autoFit) {
        // the crown: the upper 35 % of the style's height — a robust measure of the head it was made for, whatever the length
        const float cutY = lo.y + (hi.y - lo.y) * 0.65f;
        std::vector<float> xs, zs;
        for (const V3& v : p)
            if (v.y >= cutY) { xs.push_back(v.x); zs.push_back(v.z); }
        const float crownW = percentile(xs, 0.95f) - percentile(xs, 0.05f);
        centre = {0.5f * (percentile(xs, 0.95f) + percentile(xs, 0.05f)), 0, 0.5f * (percentile(zs, 0.95f) + percentile(zs, 0.05f))};
        // a style wraps the head with a little room: it is ~12 % wider than the skull
        scale = crownW > 1e-6f ? (2.0f * head.radii.x * 1.12f) / crownW : 1.0f;
    } else {
        centre = {};                       // as it is: no re-centring, no top alignment (see `placed` below)
        topY = 0.0f;
    }
    scale *= adj.scale;
    if (rep) rep->scale = scale;
    const float turn = adj.flipFront ? 3.14159265f : 0.0f;
    if (rep) rep->facingFlipped = adj.flipFront;
    // autoFit places the style on the head (centred in x/z, its top just above the crown); without it the source is used exactly as it is
    const V3 target = adj.autoFit ? V3(head.centre.x, 0, head.centre.z) : V3();
    const float targetTop = adj.autoFit ? head.top + 0.012f : 0.0f;
    for (V3& v : p) {
        V3 q((v.x - centre.x) * scale, (v.y - topY) * scale, (v.z - centre.z) * scale);
        if (adj.flipFront) q = {q.x * std::cos(turn) + q.z * std::sin(turn), q.y, -q.x * std::sin(turn) + q.z * std::cos(turn)};
        q = rotateXYZ(q, adj.rotateDeg);
        v = {q.x + target.x + adj.translate.x, q.y + targetTop + adj.translate.y, q.z + target.z + adj.translate.z};
    }
    return p;
}

// vertex adjacency for the displacement relaxation
std::vector<std::vector<uint32_t>> adjacency(size_t n, const std::vector<uint32_t>& idx)
{
    std::vector<std::vector<uint32_t>> adj(n);
    for (size_t t = 0; t + 2 < idx.size(); t += 3)
        for (int k = 0; k < 3; ++k) {
            const uint32_t a = idx[t + size_t(k)], b = idx[t + size_t((k + 1) % 3)];
            if (a >= n || b >= n) continue;
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    return adj;
}

// Welded vertices (same position, different UV) must move together or the card tears along its seams: group by position.
std::vector<uint32_t> weldGroups(const std::vector<V3>& p)
{
    std::vector<uint32_t> order(p.size()), group(p.size());
    for (size_t i = 0; i < p.size(); ++i) order[i] = uint32_t(i);
    auto key = [&](uint32_t i) { return std::array<long long, 3>{std::llround(p[i].x * 20000.0), std::llround(p[i].y * 20000.0), std::llround(p[i].z * 20000.0)}; };
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return key(a) < key(b); });
    uint32_t g = 0;
    for (size_t i = 0; i < order.size(); ++i) {
        if (i > 0 && key(order[i]) != key(order[i - 1])) ++g;
        group[order[i]] = g;
    }
    return group;
}

}  // namespace

QJsonObject toJson(const FitAdjust& a)
{
    return QJsonObject{{"tx", double(a.translate.x)}, {"ty", double(a.translate.y)}, {"tz", double(a.translate.z)},
                       {"rx", double(a.rotateDeg.x)}, {"ry", double(a.rotateDeg.y)}, {"rz", double(a.rotateDeg.z)},
                       {"scale", double(a.scale)}, {"snug", double(a.snug)}, {"clearance", double(a.clearance)},
                       {"flipFront", a.flipFront}, {"autoFit", a.autoFit}};
}

FitAdjust fitAdjustFromJson(const QJsonObject& o)
{
    FitAdjust a;
    auto f = [&](const char* k, float def) {
        const QJsonValue v = o.value(QLatin1String(k));
        const float x = v.isDouble() ? float(v.toDouble()) : def;
        return std::isfinite(x) ? x : def;
    };
    a.translate = {clampf(f("tx", 0), -0.3f, 0.3f), clampf(f("ty", 0), -0.3f, 0.3f), clampf(f("tz", 0), -0.3f, 0.3f)};
    a.rotateDeg = {clampf(f("rx", 0), -180, 180), clampf(f("ry", 0), -180, 180), clampf(f("rz", 0), -180, 180)};
    a.scale = clampf(f("scale", 1), 0.3f, 3.0f);
    a.snug = clampf(f("snug", 0.35f), 0, 1);
    a.clearance = clampf(f("clearance", 0.003f), 0, 0.03f);
    a.flipFront = o.value("flipFront").toBool(false);
    a.autoFit = o.value("autoFit").toBool(true);
    return a;
}

int countPenetrations(const std::vector<V3>& pos, const BodyCollider& collider, float tolerance)
{
    int n = 0;
    for (const V3& p : pos)
        if (collider.signedDistance(p) < -tolerance) ++n;
    return n;
}

void fitToHead(const ImportedHair& hair, const HeadFrame& head, const BodyCollider& collider, const FitAdjust& adj,
               std::vector<V3>* outPos, std::vector<V3>* outNrm, FitReport* report)
{
    FitReport local;
    FitReport& rep = report ? *report : local;
    if (!head.valid) {   // the head could not be measured: the style stays exactly as it came (normalise() would move it to a default head at the origin)
        rep.warnings << QStringLiteral("Hair fitting: the head could not be measured; the style was left unfitted.");
        if (outPos) *outPos = hair.pos;
        if (outNrm) *outNrm = recomputeNormals(hair.pos, hair.idx);
        return;
    }
    std::vector<V3> p = normalise(hair, head, adj, &rep);
    if (p.empty() || collider.empty()) {
        if (outPos) *outPos = p;
        if (outNrm) *outNrm = recomputeNormals(p, hair.idx);
        return;
    }
    // Step 2: displacement field = push-out of penetrating vertices + snug pull of the root band, attenuated by the distance to the
    // surface. Computed per welded position group so seams stay closed.
    const float band = 0.045f * (head.radii.x / 0.075f);
    const std::vector<uint32_t> group = weldGroups(p);
    std::vector<V3> disp(p.size());
    std::vector<float> weight(p.size(), 0.0f);
    for (size_t i = 0; i < p.size(); ++i) {
        float d = 0;
        const int v = collider.nearest(p[i], &d);
        if (v < 0) continue;
        const float s = dot(p[i] - collider.positions()[size_t(v)], collider.normals()[size_t(v)]);   // signed distance
        const V3 n = collider.normals()[size_t(v)];
        if (s < adj.clearance) {
            disp[i] = n * (adj.clearance - s);                              // out of the skin
            weight[i] = 1.0f;
            ++rep.penetrating;
            rep.maxPush = std::max(rep.maxPush, adj.clearance - s);
        } else if (s < band && adj.snug > 0.0f) {
            const float k = adj.snug * (1.0f - smooth01(s / band));         // fades to nothing at the band's edge
            disp[i] = n * ((adj.clearance - s) * k);                        // towards the scalp
            weight[i] = k;
            if (k > 0.05f) ++rep.snapped;
        }
    }
    // weld groups share their average displacement
    std::vector<V3> gd(size_t(*std::max_element(group.begin(), group.end())) + 1);
    std::vector<int> gc(gd.size(), 0);
    for (size_t i = 0; i < p.size(); ++i) { gd[group[i]] += disp[i]; ++gc[group[i]]; }
    for (size_t i = 0; i < p.size(); ++i) disp[i] = gd[group[i]] * (1.0f / float(std::max(1, gc[group[i]])));
    // relax the field over the mesh so the neighbours of a corrected vertex follow smoothly (no dents), keeping the corrected ones
    const auto adjacency_ = adjacency(p.size(), hair.idx);
    for (int it = 0; it < 4; ++it) {
        std::vector<V3> next = disp;
        for (size_t i = 0; i < p.size(); ++i) {
            if (weight[i] >= 1.0f || adjacency_[i].empty()) continue;
            V3 avg;
            for (uint32_t j : adjacency_[i]) avg += disp[j];
            avg = avg * (1.0f / float(adjacency_[i].size()));
            next[i] = disp[i] * 0.5f + avg * 0.5f * 0.85f;                   // slightly damped: the influence dies out with distance
        }
        // the copies of one welded position must keep moving together (each one averages its OWN triangle neighbours, so they drift apart and
        // a UV seam opens): put them back on the group average after every pass; held vertices (weight >= 1) stay as they are
        std::fill(gd.begin(), gd.end(), V3());
        std::fill(gc.begin(), gc.end(), 0);
        for (size_t i = 0; i < p.size(); ++i) { gd[group[i]] += next[i]; ++gc[group[i]]; }
        for (size_t i = 0; i < p.size(); ++i)
            if (weight[i] < 1.0f) next[i] = gd[group[i]] * (1.0f / float(std::max(1, gc[group[i]])));
        disp = std::move(next);
    }
    for (size_t i = 0; i < p.size(); ++i) p[i] += disp[i];
    // Step 3: a final hard guarantee for what the smoothing could not reach (thin gaps between a vertex and its neighbours)
    for (V3& v : p) v = collider.pushOut(v, adj.clearance * 0.5f);
    if (outPos) *outPos = p;
    if (outNrm) *outNrm = recomputeNormals(p, hair.idx);
}

Dna::Mesh importedToDnaMesh(const ImportedHair& hair, const std::vector<V3>& pos, const std::vector<V3>& nrm, const BodyCollider& col,
                            const std::vector<std::vector<Dna::SkinInfluence>>& bodySkin)
{
    CardMesh cm;
    cm.pos = pos;
    cm.nrm = nrm;
    cm.uv = hair.uv;
    cm.idx = hair.idx;
    if (cm.uv.size() < cm.pos.size() * 2) cm.uv.resize(cm.pos.size() * 2, 0.0f);
    return toDnaMesh(cm, col, bodySkin);
}

}  // namespace Hair
}  // namespace CharacterGen
