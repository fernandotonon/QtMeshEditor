#include "GenericSegmenter.h"

#include "GraphCut.h"
#include "Leiden.h"
#include "../MeshSegmenter.h"

#include <QDebug>

#include <cstdio>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <numeric>
#include <unordered_map>

namespace GenericSeg {

QString imageKindName(ImageKind k)
{
    switch (k) {
    case ImageKind::Normal: return QStringLiteral("normal");
    case ImageKind::Sdf: return QStringLiteral("sdf");
    case ImageKind::Matte: return QStringLiteral("matte");
    }
    return QStringLiteral("normal");
}

QString granularityName(Granularity g)
{
    switch (g) {
    case Granularity::Coarse: return QStringLiteral("coarse");
    case Granularity::Medium: return QStringLiteral("medium");
    case Granularity::Fine: return QStringLiteral("fine");
    }
    return QStringLiteral("medium");
}

Granularity granularityFromName(const QString& name, bool* ok)
{
    const QString n = name.trimmed().toLower();
    if (ok) *ok = true;
    if (n == QLatin1String("coarse")) return Granularity::Coarse;
    if (n == QLatin1String("medium")) return Granularity::Medium;
    if (n == QLatin1String("fine")) return Granularity::Fine;
    if (ok) *ok = false;
    return Granularity::Medium;
}

// CPM resolution = the minimum edge density a group of masks needs to stay one
// part. Lower merges more.
double leidenResolutionFor(Granularity g)
{
    switch (g) {
    case Granularity::Coarse: return 0.02;
    case Granularity::Medium: return 0.05;
    case Granularity::Fine: return 0.15;
    }
    return 0.05;
}

bool usesMatteImages(int faceCount, const Options& opts)
{
    return faceCount < opts.matteBelowFaces;
}

int viewCountFor(int faceCount, const Options& opts)
{
    if (opts.views > 0) return opts.views;
    return usesMatteImages(faceCount, opts) ? 20 : 12;
}

int imagesPerViewFor(int faceCount, const Options& opts)
{
    if (usesMatteImages(faceCount, opts)) return 1;
    const int n = int(opts.normalImages) + int(opts.sdfImages);
    return n > 0 ? n : 1;
}

float minPartFractionFor(Granularity g)
{
    switch (g) {
    case Granularity::Coarse: return 0.03f;
    case Granularity::Medium: return 0.01f;
    case Granularity::Fine: return 0.003f;
    }
    return 0.01f;
}

namespace {

// SAMesh's per-view painting: masks are drawn largest first, so each pixel
// keeps only the SMALLEST mask covering it. SAM returns nested masks (the
// whole object, a limb, a finger); without this the whole-object mask, which
// every view produces, outvotes every part on every patch.
void paintSmallestFirst(std::vector<Mask>& masks, size_t pixelCount)
{
    std::vector<int> order;
    std::vector<size_t> area(masks.size(), 0);
    for (size_t i = 0; i < masks.size(); ++i) {
        if (masks[i].pixels.size() != pixelCount) continue;
        for (uint8_t v : masks[i].pixels) area[i] += v != 0;
        order.push_back(int(i));
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return area[a] > area[b]; });
    std::vector<int> owner(pixelCount, -1);
    for (int i : order)
        for (size_t p = 0; p < pixelCount; ++p)
            if (masks[i].pixels[p]) owner[p] = i;
    for (int i : order)
        for (size_t p = 0; p < pixelCount; ++p)
            masks[i].pixels[p] = owner[p] == i;
}


// One lifted mask: the surface patches it covers and how much of each.
struct Node {
    int image = 0;                 // (view, kind) id: masks of one image never connect
    int view = 0;
    float score = 1.0f;
    double area = 0.0;             // surface area of the covered patches
    std::vector<int> patches;      // sorted
    std::vector<float> coverage;   // share of the patch's visible pixels inside the mask
};

// Faces with label 0 take the label of the nearest labelled face (face-graph
// BFS, i.e. along the surface). Returns how many were filled.
int floodUnlabelled(const SegMesh& mesh, std::vector<int>& labels)
{
    std::deque<int> queue;
    for (int f = 0; f < int(labels.size()); ++f)
        if (labels[f] > 0) queue.push_back(f);
    int filled = 0;
    while (!queue.empty()) {
        const int f = queue.front();
        queue.pop_front();
        for (int g : mesh.faceNeighbors[f])
            if (labels[g] == 0) { labels[g] = labels[f]; ++filled; queue.push_back(g); }
    }
    return filled;
}

// Give every connected piece of a label its own label (two wheels are two parts).
void splitComponents(const SegMesh& mesh, std::vector<int>& labels)
{
    const int faceCount = int(labels.size());
    std::vector<int> out(faceCount, 0);
    int next = 0;
    for (int seed = 0; seed < faceCount; ++seed) {
        if (out[seed] != 0 || labels[seed] == 0) continue;
        out[seed] = ++next;
        std::deque<int> queue{seed};
        while (!queue.empty()) {
            const int f = queue.front();
            queue.pop_front();
            for (int g : mesh.faceNeighbors[f])
                if (out[g] == 0 && labels[g] == labels[seed]) { out[g] = next; queue.push_back(g); }
        }
    }
    labels.swap(out);
}

// Merge parts smaller than `minFraction` of the surface into the neighbour
// they share the longest border with. A small piece with no neighbour (a
// separate floating object) stays a part.
void mergeSmallParts(const SegMesh& mesh, std::vector<int>& labels, float minFraction)
{
    if (!(minFraction > 0)) return;
    const int faceCount = int(labels.size());
    const double total = std::accumulate(mesh.faceAreas.begin(), mesh.faceAreas.end(), 0.0);
    for (int pass = 0; pass < 4096; ++pass) {
        std::unordered_map<int, double> area;
        for (int f = 0; f < faceCount; ++f) area[labels[f]] += mesh.faceAreas[f];
        // Smallest part below the threshold that has a neighbour.
        std::vector<std::pair<double, int>> small;
        for (const auto& [l, a] : area)
            if (l > 0 && a < minFraction * total) small.push_back({a, l});
        std::sort(small.begin(), small.end());
        bool merged = false;
        for (const auto& [a, l] : small) {
            (void)a;
            std::unordered_map<int, int> border;
            for (int f = 0; f < faceCount; ++f) {
                if (labels[f] != l) continue;
                for (int g : mesh.faceNeighbors[f])
                    if (labels[g] != l) ++border[labels[g]];
            }
            int target = -1, best = 0;
            for (const auto& [nl, count] : border)
                if (count > best || (count == best && nl < target)) { best = count; target = nl; }
            if (target < 0) continue;
            for (int& x : labels) if (x == l) x = target;
            merged = true;
            break;
        }
        if (!merged) break;
    }
}

// Renumber 1..K by descending area; 0 stays 0.
int renumberByArea(const SegMesh& mesh, std::vector<int>& labels)
{
    std::unordered_map<int, double> area;
    for (int f = 0; f < int(labels.size()); ++f)
        if (labels[f] > 0) area[labels[f]] += mesh.faceAreas[f];
    std::vector<std::pair<double, int>> order;
    for (const auto& [l, a] : area) order.push_back({-a, l});
    std::sort(order.begin(), order.end());
    std::unordered_map<int, int> map;
    for (int i = 0; i < int(order.size()); ++i) map[order[i].second] = i + 1;
    for (int& l : labels) if (l > 0) l = map[l];
    return int(order.size());
}

// Graph-cut refinement (see Options::graphCut). `labels` are community + 1
// from the vote, every face labelled (after the flood).
// Returns false when skipped (over the memory budget).
bool refineWithGraphCut(const SegMesh& mesh, const TriangleBvh& bvh, const std::vector<int>& patchOf,
                        const std::vector<std::vector<std::pair<int, float>>>& patchVotes,
                        const Options& opts, std::vector<int>& labels)
{
    const int faceCount = mesh.faceCount();
    // Candidate labels: the ones that won somewhere.
    std::unordered_map<int, int> index;   // label -> 0..K-1
    std::vector<int> labelOf;
    for (int l : labels)
        if (l > 0 && index.emplace(l, int(labelOf.size())).second) labelOf.push_back(l);
    const int K = int(labelOf.size());
    if (K < 2) return true;
    if (static_cast<long long>(faceCount) * K > opts.graphCutMaxEntries) return false;

    // Unary: -log of the smoothed vote share, per unit area. Unvoted faces
    // cost the same for every label (the neighbours decide).
    const float a = std::max(1e-3f, opts.voteSmoothing);
    std::vector<float> unary(size_t(faceCount) * K, 0.0f);
    for (int f = 0; f < faceCount; ++f) {
        const auto& votes = patchVotes[patchOf[f]];
        if (votes.empty()) continue;
        double total = 0.0;
        std::vector<float> s(K, 0.0f);
        for (const auto& [c, sc] : votes) {
            const auto it = index.find(c + 1);
            if (it == index.end()) continue;
            s[it->second] += sc;
            total += sc;
        }
        const double denom = total + double(K) * a;
        for (int k = 0; k < K; ++k)
            unary[size_t(f) * K + k] = float(-std::log((s[k] + a) / denom) * mesh.faceAreas[f]);
    }

    std::vector<Vec3> centroid(faceCount);
    for (int f = 0; f < faceCount; ++f)
        centroid[f] = (mesh.positions[mesh.indices[3 * f]] + mesh.positions[mesh.indices[3 * f + 1]]
                       + mesh.positions[mesh.indices[3 * f + 2]]) * (1.0f / 3.0f);

    std::vector<GraphEdge> edges;
    // Surface edges: length * smoothness, cheap along concave creases.
    const float crease = std::max(1e-3f, opts.creaseAngleDegrees * 3.14159265f / 180.0f);
    for (int f = 0; f < faceCount; ++f) {
        for (int g : mesh.faceNeighbors[f]) {
            if (g <= f) continue;
            // Shared welded edge.
            int shared[2], n = 0;
            for (int i = 0; i < 3 && n < 2; ++i)
                for (int j = 0; j < 3; ++j)
                    if (mesh.weldedIndices[3 * f + i] == mesh.weldedIndices[3 * g + j]) {
                        shared[n++] = int(mesh.indices[3 * f + i]);
                        break;
                    }
            if (n < 2) continue;
            const float len = length(mesh.positions[shared[0]] - mesh.positions[shared[1]]);
            const Vec3 nf = mesh.faceNormals[f], ng = mesh.faceNormals[g];
            const float cosT = std::clamp(dot(nf, ng), -1.0f, 1.0f);
            const float theta = std::acos(cosT);
            const bool concave = dot(nf, centroid[g] - centroid[f]) > 0.0f;
            float w = 1.0f;
            if (concave) w = std::max(0.02f, 1.0f / (1.0f + (theta / crease) * (theta / crease)));
            if (len > 0) edges.push_back({f, g, opts.smoothness * len * w});
        }
    }
    // Through links: straight inward from each face to the wall it exits.
    if (opts.throughWeight > 0 && bvh.triangleCount() == faceCount) {
        const float tau = std::max(1e-4f, opts.throughThickness);
        for (int f = 0; f < faceCount; ++f) {
            const Vec3 nf = mesh.faceNormals[f];
            if (length(nf) < 0.5f) continue;
            const Vec3 d = nf * -1.0f;
            TriangleBvh::Hit h;
            if (!bvh.intersect(centroid[f], d, 1e-5f, 5.0f * tau, h, f)) continue;
            if (dot(bvh.faceNormal(h.face), d) < 0.3f) continue;   // entering another surface
            const float w = opts.throughWeight * std::min(mesh.faceAreas[f], mesh.faceAreas[h.face])
                            * std::exp(-h.t / tau);
            if (w > 0) edges.push_back({f, h.face, w});
        }
    }

    std::vector<int> init(faceCount);
    for (int f = 0; f < faceCount; ++f) init[f] = labels[f] > 0 ? index[labels[f]] : 0;
    // Each part's move is solved around the faces that voted for it (plus two
    // hops along surface and through links, which reach a thin part's other
    // side): on a 25k-face prop with 50 parts, whole-mesh moves took 20 s.
    std::vector<std::vector<int>> candidates(K);
    for (int f = 0; f < faceCount; ++f)
        for (const auto& [c, sc] : patchVotes[patchOf[f]]) {
            (void)sc;
            const auto it = index.find(c + 1);
            if (it != index.end()) candidates[it->second].push_back(f);
        }
    const std::vector<int> out = alphaExpansion(faceCount, K, unary, edges, init, 4, &candidates, 2);
    // Faces no view saw and the flood could not reach (floating pieces) stay unknown.
    for (int f = 0; f < faceCount; ++f)
        if (labels[f] > 0) labels[f] = labelOf[out[f]];
    return true;
}

} // namespace

Result segment(const float* positions, int vertexCount,
               const uint32_t* indices, int indexCount,
               MaskProvider& provider, const Options& opts)
{
    Result res;
    auto progress = [&](const QString& stage, int done, int total) {
        if (opts.progress && !opts.progress(stage, done, total)) {
            res.cancelled = true;
            res.error = QStringLiteral("cancelled");
            return false;
        }
        return true;
    };

    const SegMesh mesh = SegMesh::build(positions, vertexCount, indices, indexCount);
    const int faceCount = mesh.faceCount();
    if (faceCount == 0) {
        res.error = QStringLiteral("empty or invalid mesh");
        return res;
    }

    // ---- Images -----------------------------------------------------------
    const bool matte = usesMatteImages(faceCount, opts);
    std::vector<ImageKind> kinds;
    if (matte) {
        kinds.push_back(ImageKind::Matte);
    } else {
        if (opts.normalImages) kinds.push_back(ImageKind::Normal);
        if (opts.sdfImages) kinds.push_back(ImageKind::Sdf);
        if (kinds.empty()) kinds.push_back(ImageKind::Normal);
    }
    std::vector<float> sdf01;
    const bool wantSdf = std::find(kinds.begin(), kinds.end(), ImageKind::Sdf) != kinds.end();
    TriangleBvh bvh;
    if (wantSdf || opts.graphCut) {
        std::vector<float> flat(mesh.positions.size() * 3);
        for (size_t v = 0; v < mesh.positions.size(); ++v) {
            flat[3 * v] = mesh.positions[v].x;
            flat[3 * v + 1] = mesh.positions[v].y;
            flat[3 * v + 2] = mesh.positions[v].z;
        }
        bvh.build(flat.data(), int(mesh.positions.size()), mesh.indices.data(), int(mesh.indices.size()));
    }
    if (wantSdf) {
        if (!progress(QStringLiteral("shape diameter"), 0, 1)) return res;
        sdf01 = normalizeSdf(shapeDiameter(mesh, bvh, opts.sdf));
    }

    // ---- Surface patches ----------------------------------------------------
    // Masks are lifted onto small surface patches (faces grouped by a 3D grid
    // about `patchPixels` pixels wide), not single faces: on a dense mesh many
    // triangles are smaller than a pixel, so each view would keep a different
    // random subset of them and masks of one part from neighbouring views
    // would barely share faces.
    const int resolution = std::max(16, opts.resolution);
    const float pixelWorld = 2.0f * OrthoCamera().halfExtent / float(resolution);
    const float cell = std::max(1e-4f, opts.patchPixels * pixelWorld);
    std::vector<int> patchOf(faceCount);
    std::vector<double> patchArea;
    {
        std::unordered_map<uint64_t, int> ids;
        auto q = [cell](float v) { return uint64_t(int64_t(std::floor(v / cell)) & 0x1FFFFF); };
        for (int f = 0; f < faceCount; ++f) {
            const Vec3 c = (mesh.positions[mesh.indices[3 * f]] + mesh.positions[mesh.indices[3 * f + 1]]
                            + mesh.positions[mesh.indices[3 * f + 2]]) * (1.0f / 3.0f);
            const uint64_t key = (q(c.x) << 42) | (q(c.y) << 21) | q(c.z);
            const auto it = ids.emplace(key, int(ids.size())).first;
            patchOf[f] = it->second;
            if (it->second >= int(patchArea.size())) patchArea.push_back(0.0);
            patchArea[it->second] += mesh.faceAreas[f];
        }
    }
    const int patchCount = int(patchArea.size());
    res.stats.patches = patchCount;

    // ---- Views -> masks -> lifted nodes ------------------------------------
    const std::vector<Vec3> dirs = viewDirections(viewCountFor(faceCount, opts));
    std::vector<Node> nodes;
    std::vector<char> seen(faceCount, 0);
    std::vector<std::vector<char>> visibleIn(dirs.size());   // [view][patch]
    const int totalImages = int(dirs.size() * kinds.size());
    int imageId = 0;
    std::vector<int> inMask(patchCount, 0);
    std::vector<int> touched;
    for (int v = 0; v < int(dirs.size()); ++v) {
        const OrthoCamera cam = OrthoCamera::lookingFrom(dirs[v]);
        const Raster raster = rasterize(mesh, cam, resolution);
        const std::vector<int> visible = raster.pixelsPerFace(faceCount);
        std::vector<int> patchPixels(patchCount, 0);
        for (int f = 0; f < faceCount; ++f)
            if (visible[f] > 0) { seen[f] = 1; patchPixels[patchOf[f]] += visible[f]; }
        visibleIn[v].assign(patchCount, 0);
        for (int p = 0; p < patchCount; ++p)
            visibleIn[v][p] = patchPixels[p] >= opts.minPatchPixels ? 1 : 0;
        ++res.stats.views;
        for (ImageKind kind : kinds) {
            if (!progress(QStringLiteral("views"), imageId, totalImages)) return res;
            ViewImage vi;
            vi.view = v;
            vi.kind = kind;
            vi.raster = &raster;
            vi.camera = cam;
            switch (kind) {
            case ImageKind::Normal: vi.image = shadeNormals(raster, mesh); break;
            case ImageKind::Sdf: vi.image = shadeFaceScalar(raster, sdf01); break;
            case ImageKind::Matte: vi.image = shadeMatte(raster, mesh, cam); break;
            }
            std::vector<Mask> masks = provider.masksFor(vi);
            ++res.stats.images;
            res.stats.masks += int(masks.size());
            const size_t pixelCount = size_t(raster.size) * raster.size;
            if (opts.smallestMaskWins) paintSmallestFirst(masks, pixelCount);
            for (const Mask& m : masks) {
                if (m.pixels.size() != pixelCount) continue;
                touched.clear();
                for (size_t i = 0; i < pixelCount; ++i) {
                    if (!m.pixels[i]) continue;
                    const int f = raster.face[i];
                    if (f < 0) continue;
                    if (inMask[patchOf[f]]++ == 0) touched.push_back(patchOf[f]);
                }
                Node node;
                node.image = imageId;
                node.view = v;
                node.score = std::clamp(m.score, 0.01f, 1.0f);
                std::sort(touched.begin(), touched.end());
                for (int p : touched) {
                    const float cov = float(inMask[p]) / float(std::max(1, patchPixels[p]));
                    if (visibleIn[v][p] && cov >= opts.minCoverage) {
                        node.patches.push_back(p);
                        node.coverage.push_back(std::min(1.0f, cov));
                        node.area += patchArea[p];
                    }
                    inMask[p] = 0;
                }
                if (!node.patches.empty() && node.area > 0) nodes.push_back(std::move(node));
            }
            ++imageId;
        }
    }
    res.stats.nodes = int(nodes.size());
    res.stats.unseenFaces = int(std::count(seen.begin(), seen.end(), 0));
    if (nodes.empty()) {
        res.error = QStringLiteral("the 2D segmenter returned no usable masks");
        return res;
    }

    // ---- Graph: masks that agree across images -----------------------------
    if (!progress(QStringLiteral("merge"), 0, 1)) return res;
    std::vector<std::vector<int>> nodesOfPatch(patchCount);
    for (int n = 0; n < int(nodes.size()); ++n)
        for (int p : nodes[n].patches) nodesOfPatch[p].push_back(n);
    std::unordered_map<uint64_t, double> overlap;
    for (int p = 0; p < patchCount; ++p) {
        const auto& ns = nodesOfPatch[p];
        for (size_t i = 0; i < ns.size(); ++i)
            for (size_t j = i + 1; j < ns.size(); ++j) {
                const int a = ns[i], b = ns[j];
                if (nodes[a].image == nodes[b].image) continue;
                overlap[(uint64_t(std::min(a, b)) << 32) | uint32_t(std::max(a, b))] += patchArea[p];
            }
    }
    // Compare two masks only on the patches BOTH views see (masks of one part
    // from different views cover different patches but agree where both look),
    // by IoU: one part's masks score ~1, a mask spanning two parts scores about
    // the share of the part it shares, so it does not glue the parts together.
    // visArea[n][v] = area of node n's patches visible from view v.
    const int viewCount = int(dirs.size());
    std::vector<std::vector<double>> visArea(nodes.size(), std::vector<double>(viewCount, 0.0));
    for (int n = 0; n < int(nodes.size()); ++n)
        for (int p : nodes[n].patches)
            for (int v = 0; v < viewCount; ++v)
                if (visibleIn[v][p]) visArea[n][v] += patchArea[p];
    std::vector<GraphEdge> edges;
    for (const auto& [key, shared] : overlap) {
        const int a = int(key >> 32), b = int(key & 0xFFFFFFFFu);
        const double aSeenByB = visArea[a][nodes[b].view];
        const double bSeenByA = visArea[b][nodes[a].view];
        // Too little in common (nearly opposite views): no evidence either way.
        if (std::min(aSeenByB, bSeenByA) < opts.minCommonFraction * std::min(nodes[a].area, nodes[b].area))
            continue;
        const double uni = aSeenByB + bSeenByA - shared;
        if (!(uni > 0)) continue;
        const double iou = std::min(1.0, shared / uni);
        if (iou >= opts.overlapThreshold)
            edges.push_back({a, b, float(iou * std::min(nodes[a].score, nodes[b].score))});
    }
    std::sort(edges.begin(), edges.end(), [](const GraphEdge& x, const GraphEdge& y) {
        return x.a != y.a ? x.a < y.a : x.b < y.b;   // deterministic input order
    });
    res.stats.edges = int(edges.size());
    LeidenOptions lo;
    lo.quality = LeidenQuality::Cpm;
    lo.resolution = opts.leidenResolution >= 0 ? opts.leidenResolution : leidenResolutionFor(opts.granularity);
    lo.seed = opts.seed;
    const std::vector<int> community = leiden(int(nodes.size()), edges, lo);
    res.stats.communities = community.empty() ? 0 : *std::max_element(community.begin(), community.end()) + 1;
    if (qEnvironmentVariableIsSet("QTMESH_GENSEG_DEBUG")) {
        std::map<int, int> sizes;
        for (int c : community) ++sizes[c];
        QString s;
        for (const auto& [c, n] : sizes) s += QStringLiteral(" %1:%2").arg(c).arg(n);
        fprintf(stderr, "%s\n", qPrintable(QStringLiteral("[genseg] patches=%1 images=%2 masks=%3 nodes=%4 edges=%5 gamma=%6 communities=%7 sizes:%8")
                                 .arg(patchCount).arg(res.stats.images).arg(res.stats.masks).arg(nodes.size())
                                 .arg(edges.size()).arg(lo.resolution).arg(res.stats.communities).arg(s)));
    }

    // ---- Patch vote -> face labels -----------------------------------------
    if (!progress(QStringLiteral("labels"), 0, 1)) return res;
    std::vector<int> patchLabel(patchCount, 0);
    std::vector<std::vector<std::pair<int, float>>> patchVotes(patchCount);   // (community, score)
    {
        std::vector<float> specificity(nodes.size(), 1.0f);
        if (opts.voteAreaExponent > 0 && !nodes.empty()) {
            std::vector<double> areas;
            for (const Node& n : nodes) areas.push_back(n.area);
            std::nth_element(areas.begin(), areas.begin() + areas.size() / 2, areas.end());
            const double median = std::max(1e-12, areas[areas.size() / 2]);
            for (size_t n = 0; n < nodes.size(); ++n)
                specificity[n] = float(std::pow(median / std::max(1e-12, nodes[n].area), opts.voteAreaExponent));
        }
        std::unordered_map<int, float> score;
        for (int p = 0; p < patchCount; ++p) {
            if (nodesOfPatch[p].empty()) continue;
            score.clear();
            for (int n : nodesOfPatch[p]) {
                const Node& node = nodes[n];
                const auto it = std::lower_bound(node.patches.begin(), node.patches.end(), p);
                const float cov = (it != node.patches.end() && *it == p)
                                      ? node.coverage[size_t(it - node.patches.begin())] : 1.0f;
                // Only a mask holding most of the patch gets the specificity
                // boost: a small mask's partial cover at its silhouette edge
                // must not outvote the mask the patch really belongs to.
                score[community[n]] += node.score * cov * (cov >= 0.5f ? specificity[n] : 1.0f);
            }
            int best = -1;
            float bestScore = -1.0f;
            for (const auto& [c, sc] : score) {
                patchVotes[p].push_back({c, sc});
                if (sc > bestScore || (sc == bestScore && c < best)) { bestScore = sc; best = c; }
            }
            patchLabel[p] = best + 1;
        }
    }
    std::vector<int> labels(faceCount, 0);
    for (int f = 0; f < faceCount; ++f) labels[f] = patchLabel[patchOf[f]];
    floodUnlabelled(mesh, labels);
    if (opts.graphCut) {
        if (!progress(QStringLiteral("refine"), 0, 1)) return res;
        res.stats.graphCutSkipped = !refineWithGraphCut(mesh, bvh, patchOf, patchVotes, opts, labels);
    }

    // ---- Cleanup -----------------------------------------------------------
    const float minFraction = opts.minPartFraction >= 0 ? opts.minPartFraction
                                                        : minPartFractionFor(opts.granularity);
    splitComponents(mesh, labels);
    mergeSmallParts(mesh, labels, minFraction);
    if (opts.cleanup) {
        MeshSegmenter::smoothLabelBoundaries(labels, mesh.weldedIndices.data(),
                                             int(mesh.weldedIndices.size()), opts.smoothIterations);
        MeshSegmenter::cleanupLabelIslands(labels, mesh.weldedIndices.data(),
                                           int(mesh.weldedIndices.size()));
        splitComponents(mesh, labels);
        mergeSmallParts(mesh, labels, minFraction);
    }
    res.parts = renumberByArea(mesh, labels);
    if (qEnvironmentVariableIsSet("QTMESH_GENSEG_DEBUG")) {
        std::map<int, std::pair<double, int>> info;   // label -> (area, border faces)
        double total = 0;
        for (int f = 0; f < faceCount; ++f) {
            info[labels[f]].first += mesh.faceAreas[f];
            total += mesh.faceAreas[f];
            for (int g : mesh.faceNeighbors[f]) if (labels[g] != labels[f]) { ++info[labels[f]].second; break; }
        }
        QString s;
        for (const auto& [l, v] : info)
            s += QStringLiteral(" %1:%2%/%3").arg(l).arg(100.0 * v.first / total, 0, 'f', 2).arg(v.second);
        fprintf(stderr, "[genseg] parts (label:area/borderFaces):%s\n", qPrintable(s));
    }
    res.stats.parts = res.parts;

    res.faceLabels = std::move(labels);
    res.vertexLabels.assign(vertexCount, 0);
    MeshSegmenter::vertexLabelsFromFaces(res.vertexLabels, res.faceLabels, indices, indexCount);
    res.labelNames << QStringLiteral("unknown");
    for (int p = 1; p <= res.parts; ++p)
        res.labelNames << QStringLiteral("part_%1").arg(p, 2, 10, QLatin1Char('0'));
    res.ok = true;
    return res;
}

} // namespace GenericSeg
