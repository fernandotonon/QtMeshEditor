#include "SkinWeightsPost.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <utility>

namespace {

// Sparse per-vertex weight row used during smoothing — VertexWeights
// caps at 8 slots, but a smoothing step can transiently spread a
// vertex across more bones than that before the prune pass cuts it
// back down.
using Row = std::vector<std::pair<int, double>>;   // (bone, weight), unsorted

Row toRow(const SkinWeights::VertexWeights& vw)
{
    Row r;
    r.reserve(vw.count);
    for (int i = 0; i < vw.count; ++i)
        r.emplace_back(vw.boneIndices[i], vw.weights[i]);
    return r;
}

void addScaled(Row& acc, const Row& src, double scale)
{
    for (const auto& [bone, w] : src) {
        bool found = false;
        for (auto& [b, aw] : acc) {
            if (b == bone) { aw += w * scale; found = true; break; }
        }
        if (!found) acc.emplace_back(bone, w * scale);
    }
}

void normalizeRow(Row& r)
{
    double sum = 0.0;
    for (const auto& [b, w] : r) sum += w;
    if (sum <= 0.0) return;
    for (auto& [b, w] : r) w /= sum;
}

// Collapse a sparse row back into the fixed-size VertexWeights,
// keeping the `maxK` largest entries, sorted descending, and
// renormalized — truncation of a >maxK row would otherwise leave
// the kept weights summing below one.
void toVertexWeights(Row r, int maxK, SkinWeights::VertexWeights& vw)
{
    std::sort(r.begin(), r.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (int(r.size()) > maxK) {
        r.resize(maxK);
        normalizeRow(r);
    }
    vw = {};
    for (const auto& [bone, w] : r) {
        vw.boneIndices[vw.count] = bone;
        vw.weights[vw.count]     = w;
        ++vw.count;
    }
}

} // namespace

std::vector<std::vector<int>> SkinWeightsPost::buildAdjacency(
    int vertexCount,
    const std::uint32_t* indices,
    std::size_t indexCount)
{
    std::vector<std::vector<int>> adj(std::max(0, vertexCount));
    if (!indices || indexCount < 3 || vertexCount <= 0) return adj;

    auto link = [&](std::uint32_t a, std::uint32_t b) {
        if (a >= std::uint32_t(vertexCount) || b >= std::uint32_t(vertexCount)
            || a == b)
            return;
        adj[a].push_back(int(b));
        adj[b].push_back(int(a));
    };
    const std::size_t triCount = indexCount / 3;
    for (std::size_t t = 0; t < triCount; ++t) {
        const std::uint32_t i0 = indices[3 * t + 0];
        const std::uint32_t i1 = indices[3 * t + 1];
        const std::uint32_t i2 = indices[3 * t + 2];
        link(i0, i1);
        link(i1, i2);
        link(i2, i0);
    }
    for (auto& n : adj) {
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
    }
    return adj;
}

void SkinWeightsPost::laplacianSmooth(
    std::vector<SkinWeights::VertexWeights>& weights,
    const std::vector<std::vector<int>>& adjacency,
    int iterations,
    const std::vector<std::uint8_t>& locked)
{
    if (iterations <= 0 || weights.empty()
        || adjacency.size() != weights.size())
        return;

    const int n = int(weights.size());
    auto isLocked = [&](int v) {
        return v < int(locked.size()) && locked[v] != 0;
    };

    // Work in the sparse representation for the whole run — one
    // conversion in, one out.
    std::vector<Row> rows(n);
    for (int v = 0; v < n; ++v) rows[v] = toRow(weights[v]);

    std::vector<Row> next(n);
    constexpr double kLambda = 0.5;   // blend factor toward the neighbour mean
    for (int it = 0; it < iterations; ++it) {
        for (int v = 0; v < n; ++v) {
            if (isLocked(v) || adjacency[v].empty()) {
                next[v] = rows[v];
                continue;
            }
            // avg = mean of neighbour rows; new = (1-λ)·self + λ·avg
            Row acc = rows[v];
            for (auto& [b, w] : acc) w *= (1.0 - kLambda);
            const double nb = kLambda / double(adjacency[v].size());
            for (const int u : adjacency[v]) addScaled(acc, rows[u], nb);
            normalizeRow(acc);
            next[v] = std::move(acc);
        }
        rows.swap(next);
    }

    // Collapse back. Preserve up to the hard cap of 8 here — the
    // prune stage applies the caller's maxInfluences afterwards.
    for (int v = 0; v < n; ++v) {
        if (isLocked(v)) continue;
        toVertexWeights(std::move(rows[v]), 8, weights[v]);
    }
}

void SkinWeightsPost::pruneAndRenormalize(
    std::vector<SkinWeights::VertexWeights>& weights,
    int maxInfluences,
    double threshold)
{
    const int maxK = std::clamp(maxInfluences, 1, 8);
    for (auto& vw : weights) {
        if (vw.count == 0) continue;
        Row r = toRow(vw);
        std::sort(r.begin(), r.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        Row kept;
        for (const auto& e : r) {
            if (int(kept.size()) >= maxK) break;
            if (e.second < threshold && !kept.empty()) break;  // sorted → done
            kept.push_back(e);
        }
        normalizeRow(kept);
        toVertexWeights(std::move(kept), maxK, vw);
    }
}

double SkinWeightsPost::bleedFraction(
    const std::vector<SkinWeights::VertexWeights>& weights,
    const std::vector<std::vector<int>>& allowedBones)
{
    if (weights.empty() || allowedBones.size() != weights.size()) return -1.0;
    long long total = 0, bleeding = 0;
    for (std::size_t v = 0; v < weights.size(); ++v) {
        const auto& allowed = allowedBones[v];
        if (allowed.empty()) continue;   // no geodesic data for this vertex
        const auto& vw = weights[v];
        for (int i = 0; i < vw.count; ++i) {
            ++total;
            if (std::find(allowed.begin(), allowed.end(), vw.boneIndices[i])
                == allowed.end())
                ++bleeding;
        }
    }
    if (total == 0) return -1.0;
    return double(bleeding) / double(total);
}

int SkinWeightsPost::unifyCoLocated(
    std::vector<SkinWeights::VertexWeights>& weights,
    const std::vector<float>& positions,
    float epsilon,
    const std::vector<std::uint8_t>& locked)
{
    const std::size_t n = weights.size();
    if (n == 0 || positions.size() < n * 3) return 0;

    if (epsilon <= 0.0f) {
        float mn[3] = { positions[0], positions[1], positions[2] };
        float mx[3] = { positions[0], positions[1], positions[2] };
        for (std::size_t v = 1; v < n; ++v)
            for (int a = 0; a < 3; ++a) {
                mn[a] = std::min(mn[a], positions[v * 3 + a]);
                mx[a] = std::max(mx[a], positions[v * 3 + a]);
            }
        const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
        epsilon = std::max(1e-9f,
                           1e-5f * std::sqrt(dx * dx + dy * dy + dz * dz));
    }

    // Quantized-position clustering (same scheme as MeshWeldOps).
    struct Key {
        long long x, y, z;
        bool operator<(const Key& o) const {
            if (x != o.x) return x < o.x;
            if (y != o.y) return y < o.y;
            return z < o.z;
        }
    };
    std::map<Key, std::vector<int>> clusters;
    for (std::size_t v = 0; v < n; ++v)
        clusters[{ (long long)std::llround(positions[v * 3 + 0] / epsilon),
                   (long long)std::llround(positions[v * 3 + 1] / epsilon),
                   (long long)std::llround(positions[v * 3 + 2] / epsilon) }]
            .push_back(int(v));

    auto isLocked = [&](int v) {
        return !locked.empty() && std::size_t(v) < locked.size() && locked[v];
    };
    auto sameWeights = [](const SkinWeights::VertexWeights& a,
                          const SkinWeights::VertexWeights& b) {
        if (a.count != b.count) return false;
        for (int i = 0; i < a.count; ++i)
            if (a.boneIndices[i] != b.boneIndices[i]
                || std::fabs(a.weights[i] - b.weights[i]) > 1e-6)
                return false;
        return true;
    };

    int changed = 0;
    for (auto& [key, verts] : clusters) {
        if (verts.size() < 2) continue;

        // A locked member dominates: everyone unlocked copies it.
        int lockedRep = -1;
        for (int v : verts)
            if (isLocked(v)) { lockedRep = v; break; }

        SkinWeights::VertexWeights target;
        if (lockedRep >= 0) {
            target = weights[lockedRep];
        } else {
            // Entry-wise average over the cluster, renormalized.
            std::map<int, double> acc;
            for (int v : verts)
                for (int i = 0; i < weights[v].count; ++i)
                    acc[weights[v].boneIndices[i]] +=
                        weights[v].weights[i] / double(verts.size());
            // Keep the top 8 by weight.
            std::vector<std::pair<int, double>> rows(acc.begin(), acc.end());
            std::sort(rows.begin(), rows.end(),
                      [](const auto& a, const auto& b) {
                          return a.second > b.second;
                      });
            if (rows.size() > 8) rows.resize(8);
            double sum = 0;
            for (const auto& r : rows) sum += r.second;
            target = {};
            for (const auto& r : rows) {
                target.boneIndices[target.count] = r.first;
                target.weights[target.count] = sum > 0 ? r.second / sum : 0.0;
                ++target.count;
            }
        }
        for (int v : verts) {
            if (isLocked(v)) continue;
            if (sameWeights(weights[v], target)) continue;
            weights[v] = target;
            ++changed;
        }
    }
    return changed;
}
