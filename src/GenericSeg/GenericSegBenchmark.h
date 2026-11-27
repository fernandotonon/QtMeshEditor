#pragma once
// Test-only helpers for the generic segmentation benchmark (#1179 slice 4).
// Header-only on purpose: none of this ships in the app.
//
// Ground truth comes from CC0 models built from coloured pieces (Quaternius,
// Kenney): a truth part is a connected region of faces that share one OBJ
// group + material. That is a proxy for "parts", not an annotation: two
// touching pieces with one material are one truth part, and a body painted
// in two colours is two.
//
// `loopSubdivide` makes a dense, smooth copy of such a low-poly model (the
// shape of a TRELLIS.2 asset: one fused surface, many triangles) and carries
// the truth labels to the new faces, so the normal + SDF path gets measured
// too, not only the matte path the low-poly originals take.

#include "GenericSeg/GenericSegmenter.h"
#include "GenericSeg/SegMesh.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <tuple>
#include <vector>

namespace GenericSegBench {

struct ObjMesh {
    std::vector<float> positions;     // xyz per vertex
    std::vector<uint32_t> indices;    // 3 per triangle
    std::vector<int> faceGroup;       // per triangle: index of (o/g name, usemtl)
    int groupCount = 0;
};

// Reads v / f / o / g / usemtl. Polygons are fanned. Returns false when the
// file has no faces.
inline bool loadObj(const QString& path, ObjMesh& out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    QHash<QString, int> groups;
    QString object, material;
    int group = -1;
    auto currentGroup = [&] {
        if (group >= 0) return group;
        const QString key = object + QLatin1Char('|') + material;
        auto it = groups.find(key);
        if (it == groups.end()) it = groups.insert(key, int(groups.size()));
        group = it.value();
        return group;
    };
    while (!f.atEnd()) {
        const QByteArray line = f.readLine().trimmed();
        if (line.startsWith("v ")) {
            const auto p = line.mid(2).simplified().split(' ');
            for (int k = 0; k < 3; ++k) out.positions.push_back(p.value(k).toFloat());
        } else if (line.startsWith("o ") || line.startsWith("g ")) {
            object = QString::fromUtf8(line.mid(2).trimmed());
            group = -1;
        } else if (line.startsWith("usemtl")) {
            material = QString::fromUtf8(line.mid(6).trimmed());
            group = -1;
        } else if (line.startsWith("f ")) {
            const auto p = line.mid(2).simplified().split(' ');
            std::vector<uint32_t> poly;
            for (const auto& t : p) {
                const int v = t.split('/').value(0).toInt();
                poly.push_back(uint32_t(v > 0 ? v - 1 : int(out.positions.size() / 3) + v));
            }
            const int g = currentGroup();
            for (size_t k = 1; k + 1 < poly.size(); ++k) {
                out.indices.insert(out.indices.end(), {poly[0], poly[k], poly[k + 1]});
                out.faceGroup.push_back(g);
            }
        }
    }
    out.groupCount = int(groups.size());
    return !out.indices.empty();
}

// Rig truth: `qtmesh segment <rigged file> --dump-training-data x.rig.json`
// writes the bind-pose points (centred, scaled by the largest half extent)
// with a body part per vertex from the dominant bone. The OBJ converted from
// the same file has the same shape, so each OBJ vertex takes the label of
// the nearest dumped point (after the same normalisation) and each face the
// majority of its corners. Returns per-face group ids, or empty on failure.
inline std::vector<int> rigFaceGroups(const ObjMesh& obj, const QString& rigJson)
{
    QFile f(rigJson);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray pts = root.value(QStringLiteral("points")).toArray();
    const QJsonArray labs = root.value(QStringLiteral("labels")).toArray();
    if (pts.isEmpty() || pts.size() != labs.size() * 3) return {};
    const size_t vCount = obj.positions.size() / 3;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < obj.positions.size(); ++i) {
        mn[i % 3] = std::min(mn[i % 3], obj.positions[i]);
        mx[i % 3] = std::max(mx[i % 3], obj.positions[i]);
    }
    float half = 0.0f;
    for (int a = 0; a < 3; ++a) half = std::max(half, 0.5f * (mx[a] - mn[a]));
    if (half <= 0.0f) return {};
    std::vector<int> vertexLabel(vCount, 0);
    const int n = int(labs.size());
    std::vector<float> P(size_t(n) * 3);
    for (int i = 0; i < n * 3; ++i) P[size_t(i)] = float(pts[i].toDouble());
    for (size_t v = 0; v < vCount; ++v) {
        float q[3];
        for (int a = 0; a < 3; ++a) q[a] = (obj.positions[v * 3 + a] - 0.5f * (mn[a] + mx[a])) / half;
        float best = 1e30f;
        for (int i = 0; i < n; ++i) {
            const float dx = P[size_t(i) * 3] - q[0], dy = P[size_t(i) * 3 + 1] - q[1], dz = P[size_t(i) * 3 + 2] - q[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < best) { best = d; vertexLabel[v] = labs[i].toInt(); }
        }
    }
    std::vector<int> groups(obj.indices.size() / 3);
    for (size_t t = 0; t < groups.size(); ++t) {
        const int a = vertexLabel[obj.indices[t * 3]], b = vertexLabel[obj.indices[t * 3 + 1]],
                  c = vertexLabel[obj.indices[t * 3 + 2]];
        groups[t] = (b == c) ? b : a;
    }
    return groups;
}

// Truth part per face (0-based): connected regions (edge-sharing after
// welding by position) of faces with the same group.
inline std::vector<int> truthRegions(const GenericSeg::SegMesh& mesh, const std::vector<int>& faceGroup)
{
    const int n = mesh.faceCount();
    std::vector<int> region(n, -1);
    int next = 0;
    for (int seed = 0; seed < n; ++seed) {
        if (region[seed] >= 0) continue;
        region[seed] = next;
        std::deque<int> queue{seed};
        while (!queue.empty()) {
            const int f = queue.front();
            queue.pop_front();
            for (int g : mesh.faceNeighbors[f])
                if (region[g] < 0 && faceGroup[g] == faceGroup[seed]) { region[g] = next; queue.push_back(g); }
        }
        ++next;
    }
    return region;
}

struct Score {
    double truthIoU = 0.0;   // mean over truth parts of the best IoU with a predicted part
    double predIoU = 0.0;    // mean over predicted parts of the best IoU with a truth part
    double f = 0.0;          // harmonic mean of the two
    int truthParts = 0;      // counted parts (>= minFraction of the surface)
    int predParts = 0;
};

// Area-based IoU between truth regions and predicted labels. Predicted label
// 0 (unknown) is not a part: its faces count against the truth parts they
// belong to. Parts smaller than `minFraction` of the surface are left out of
// the means (still part of every union).
inline Score evaluate(const std::vector<float>& faceArea, const std::vector<int>& truth,
                      const std::vector<int>& pred, double minFraction = 0.005)
{
    Score s;
    const size_t n = faceArea.size();
    if (truth.size() != n || pred.size() != n || n == 0) return s;
    double total = 0.0;
    std::map<int, double> ta, pa;
    std::map<std::pair<int, int>, double> inter;
    for (size_t f = 0; f < n; ++f) {
        const double a = faceArea[f];
        total += a;
        ta[truth[f]] += a;
        if (pred[f] > 0) { pa[pred[f]] += a; inter[{truth[f], pred[f]}] += a; }
    }
    if (total <= 0.0) return s;
    std::map<int, double> bestT, bestP;
    for (const auto& [key, i] : inter) {
        const double u = ta[key.first] + pa[key.second] - i;
        const double iou = u > 0.0 ? i / u : 0.0;
        bestT[key.first] = std::max(bestT[key.first], iou);
        bestP[key.second] = std::max(bestP[key.second], iou);
    }
    double sumT = 0.0, sumP = 0.0;
    for (const auto& [t, a] : ta)
        if (a >= minFraction * total) { ++s.truthParts; sumT += bestT.count(t) ? bestT[t] : 0.0; }
    for (const auto& [p, a] : pa)
        if (a >= minFraction * total) { ++s.predParts; sumP += bestP.count(p) ? bestP[p] : 0.0; }
    s.truthIoU = s.truthParts ? sumT / s.truthParts : 0.0;
    s.predIoU = s.predParts ? sumP / s.predParts : 0.0;
    s.f = s.truthIoU + s.predIoU > 0.0 ? 2.0 * s.truthIoU * s.predIoU / (s.truthIoU + s.predIoU) : 0.0;
    return s;
}

// One Loop subdivision step on a welded mesh (positions shared by index).
// Every face becomes four; `faceLabel` is carried to the children. Edges
// with other than two faces (boundary, non-manifold) use the boundary rules.
inline void loopSubdivide(std::vector<float>& positions, std::vector<uint32_t>& indices,
                          std::vector<int>& faceLabel)
{
    const size_t vCount = positions.size() / 3, fCount = indices.size() / 3;
    auto P = [&](size_t v, int k) { return double(positions[v * 3 + k]); };
    struct EdgeInfo { std::vector<uint32_t> opposite; uint32_t mid = 0; };
    std::map<std::pair<uint32_t, uint32_t>, EdgeInfo> edges;
    auto key = [](uint32_t a, uint32_t b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); };
    for (size_t f = 0; f < fCount; ++f)
        for (int k = 0; k < 3; ++k)
            edges[key(indices[f * 3 + k], indices[f * 3 + (k + 1) % 3])].opposite.push_back(indices[f * 3 + (k + 2) % 3]);

    std::vector<std::vector<uint32_t>> ring(vCount), boundaryRing(vCount);
    for (const auto& [e, info] : edges) {
        ring[e.first].push_back(e.second);
        ring[e.second].push_back(e.first);
        if (info.opposite.size() != 2) {
            boundaryRing[e.first].push_back(e.second);
            boundaryRing[e.second].push_back(e.first);
        }
    }
    std::vector<float> out(positions.size());
    for (size_t v = 0; v < vCount; ++v) {
        for (int k = 0; k < 3; ++k) out[v * 3 + k] = positions[v * 3 + k];
        if (!boundaryRing[v].empty()) {
            if (boundaryRing[v].size() == 2)
                for (int k = 0; k < 3; ++k)
                    out[v * 3 + k] = float(0.75 * P(v, k) + 0.125 * (P(boundaryRing[v][0], k) + P(boundaryRing[v][1], k)));
            continue;   // corners of non-manifold fans stay put
        }
        const size_t nb = ring[v].size();
        if (nb < 3) continue;
        const double beta = nb == 3 ? 3.0 / 16.0 : 3.0 / (8.0 * double(nb));
        for (int k = 0; k < 3; ++k) {
            double sum = 0.0;
            for (uint32_t u : ring[v]) sum += P(u, k);
            out[v * 3 + k] = float((1.0 - double(nb) * beta) * P(v, k) + beta * sum);
        }
    }
    for (auto& [e, info] : edges) {
        info.mid = uint32_t(out.size() / 3);
        for (int k = 0; k < 3; ++k) {
            double p = 0.5 * (P(e.first, k) + P(e.second, k));
            if (info.opposite.size() == 2)
                p = 0.375 * (P(e.first, k) + P(e.second, k)) + 0.125 * (P(info.opposite[0], k) + P(info.opposite[1], k));
            out.push_back(float(p));
        }
    }
    std::vector<uint32_t> idx;
    std::vector<int> labels;
    idx.reserve(fCount * 12);
    labels.reserve(fCount * 4);
    for (size_t f = 0; f < fCount; ++f) {
        const uint32_t a = indices[f * 3], b = indices[f * 3 + 1], c = indices[f * 3 + 2];
        const uint32_t ab = edges[key(a, b)].mid, bc = edges[key(b, c)].mid, ca = edges[key(c, a)].mid;
        idx.insert(idx.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
        labels.insert(labels.end(), 4, faceLabel[f]);
    }
    positions = std::move(out);
    indices = std::move(idx);
    faceLabel = std::move(labels);
}

// Welded copy: one vertex per distinct position (relative tolerance).
inline void weld(std::vector<float>& positions, std::vector<uint32_t>& indices)
{
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < positions.size(); ++i) {
        lo[i % 3] = std::min(lo[i % 3], positions[i]);
        hi[i % 3] = std::max(hi[i % 3], positions[i]);
    }
    const double cell = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-9f}) * 1e-5;
    std::map<std::tuple<long long, long long, long long>, uint32_t> ids;
    std::vector<uint32_t> remap(positions.size() / 3);
    std::vector<float> out;
    for (size_t v = 0; v < remap.size(); ++v) {
        const auto k = std::make_tuple(std::llround(positions[v * 3] / cell), std::llround(positions[v * 3 + 1] / cell),
                                       std::llround(positions[v * 3 + 2] / cell));
        auto it = ids.find(k);
        if (it == ids.end()) {
            it = ids.emplace(k, uint32_t(out.size() / 3)).first;
            out.insert(out.end(), {positions[v * 3], positions[v * 3 + 1], positions[v * 3 + 2]});
        }
        remap[v] = it->second;
    }
    for (uint32_t& i : indices) i = remap[i];
    positions = std::move(out);
}

// Applies "key=value,key=value" overrides to Options (benchmark knobs).
// Returns the keys it did not know.
inline QStringList applyOverrides(GenericSeg::Options& o, const QString& spec)
{
    QStringList unknown;
    for (const QString& kv : spec.split(',', Qt::SkipEmptyParts)) {
        const QString k = kv.section('=', 0, 0).trimmed();
        const QString v = kv.section('=', 1).trimmed();
        const double d = v.toDouble();
        if (k == QLatin1String("views")) o.views = int(d);
        else if (k == QLatin1String("resolution")) o.resolution = int(d);
        else if (k == QLatin1String("matteBelowFaces")) o.matteBelowFaces = int(d);
        else if (k == QLatin1String("normalImages")) o.normalImages = d != 0.0;
        else if (k == QLatin1String("sdfImages")) o.sdfImages = d != 0.0;
        else if (k == QLatin1String("leidenResolution")) o.leidenResolution = d;
        else if (k == QLatin1String("minPartFraction")) o.minPartFraction = float(d);
        else if (k == QLatin1String("minCoverage")) o.minCoverage = float(d);
        else if (k == QLatin1String("overlapThreshold")) o.overlapThreshold = float(d);
        else if (k == QLatin1String("minCommonFraction")) o.minCommonFraction = float(d);
        else if (k == QLatin1String("graphCut")) o.graphCut = d != 0.0;
        else if (k == QLatin1String("voteAreaExponent")) o.voteAreaExponent = float(d);
        else if (k == QLatin1String("voteSmoothing")) o.voteSmoothing = float(d);
        else if (k == QLatin1String("smoothness")) o.smoothness = float(d);
        else if (k == QLatin1String("creaseAngleDegrees")) o.creaseAngleDegrees = float(d);
        else if (k == QLatin1String("throughWeight")) o.throughWeight = float(d);
        else if (k == QLatin1String("throughThickness")) o.throughThickness = float(d);
        else if (k == QLatin1String("cleanup")) o.cleanup = d != 0.0;
        else if (k == QLatin1String("smoothIterations")) o.smoothIterations = int(d);
        else unknown << k;
    }
    return unknown;
}

} // namespace GenericSegBench
