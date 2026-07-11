#include "SkinMetrics.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

// Sparse squared L2 distance between two weight rows.
double rowDistanceSq(const SkinWeights::VertexWeights& a,
                     const SkinWeights::VertexWeights& b)
{
    double d = 0.0;
    // Terms where a has weight (paired with b's weight or 0).
    for (int i = 0; i < a.count; ++i) {
        double bw = 0.0;
        for (int j = 0; j < b.count; ++j) {
            if (b.boneIndices[j] == a.boneIndices[i]) {
                bw = b.weights[j];
                break;
            }
        }
        const double diff = a.weights[i] - bw;
        d += diff * diff;
    }
    // Bones only present in b.
    for (int j = 0; j < b.count; ++j) {
        bool inA = false;
        for (int i = 0; i < a.count; ++i) {
            if (a.boneIndices[i] == b.boneIndices[j]) { inA = true; break; }
        }
        if (!inA) d += b.weights[j] * b.weights[j];
    }
    return d;
}

} // namespace

SkinMetrics::InfluenceHistogram SkinMetrics::influenceHistogram(
    const std::vector<SkinWeights::VertexWeights>& weights)
{
    InfluenceHistogram h;
    if (weights.empty()) return h;
    long long total = 0;
    for (const auto& vw : weights) {
        const int c = std::clamp(vw.count, 0, 8);
        ++h.counts[c];
        total += c;
        h.maxInfluences = std::max(h.maxInfluences, c);
    }
    h.averageInfluences = double(total) / double(weights.size());
    return h;
}

double SkinMetrics::laplacianEnergy(
    const std::vector<SkinWeights::VertexWeights>& weights,
    const std::vector<std::vector<int>>& adjacency)
{
    if (weights.empty() || adjacency.size() != weights.size()) return -1.0;
    double sum = 0.0;
    long long edges = 0;
    for (std::size_t v = 0; v < adjacency.size(); ++v) {
        for (const int u : adjacency[v]) {
            if (u < 0 || std::size_t(u) >= weights.size()) continue;
            if (std::size_t(u) <= v) continue;   // each undirected edge once
            sum += rowDistanceSq(weights[v], weights[std::size_t(u)]);
            ++edges;
        }
    }
    if (edges == 0) return -1.0;
    return sum / double(edges);
}

SkinMetrics::Transform SkinMetrics::identityTransform()
{
    return { 1, 0, 0, 0,
             0, 1, 0, 0,
             0, 0, 1, 0 };
}

SkinMetrics::Transform SkinMetrics::rotationAbout(const double axis[3],
                                                  const double pivot[3],
                                                  double angleRad)
{
    // Normalize the axis.
    const double len = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1]
                                 + axis[2] * axis[2]);
    const double x = axis[0] / len, y = axis[1] / len, z = axis[2] / len;
    const double c = std::cos(angleRad), s = std::sin(angleRad);
    const double t = 1.0 - c;

    // Rodrigues rotation matrix.
    const double r[9] = {
        t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
        t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
        t * x * z - s * y, t * y * z + s * x, t * z * z + c,
    };
    // Translation = pivot − R·pivot so the pivot is a fixed point.
    const double tx = pivot[0] - (r[0] * pivot[0] + r[1] * pivot[1] + r[2] * pivot[2]);
    const double ty = pivot[1] - (r[3] * pivot[0] + r[4] * pivot[1] + r[5] * pivot[2]);
    const double tz = pivot[2] - (r[6] * pivot[0] + r[7] * pivot[1] + r[8] * pivot[2]);

    return { r[0], r[1], r[2], tx,
             r[3], r[4], r[5], ty,
             r[6], r[7], r[8], tz };
}

std::vector<float> SkinMetrics::deformLBS(
    const float* positions, int vertexCount,
    const std::vector<SkinWeights::VertexWeights>& weights,
    const std::vector<Transform>& boneTransforms)
{
    std::vector<float> out;
    if (!positions || vertexCount < 1) return out;
    out.assign(std::size_t(vertexCount) * 3, 0.0f);

    for (int v = 0; v < vertexCount; ++v) {
        const double px = positions[3 * v + 0];
        const double py = positions[3 * v + 1];
        const double pz = positions[3 * v + 2];
        double ox = 0, oy = 0, oz = 0, wsum = 0;
        if (std::size_t(v) < weights.size()) {
            const auto& vw = weights[v];
            for (int k = 0; k < vw.count; ++k) {
                const int b = vw.boneIndices[k];
                if (b < 0 || std::size_t(b) >= boneTransforms.size()) continue;
                const Transform& m = boneTransforms[b];
                const double w = vw.weights[k];
                ox += w * (m[0] * px + m[1] * py + m[2]  * pz + m[3]);
                oy += w * (m[4] * px + m[5] * py + m[6]  * pz + m[7]);
                oz += w * (m[8] * px + m[9] * py + m[10] * pz + m[11]);
                wsum += w;
            }
        }
        if (wsum < 1e-12) { ox = px; oy = py; oz = pz; }
        else if (std::abs(wsum - 1.0) > 1e-9) {
            ox /= wsum; oy /= wsum; oz /= wsum;
        }
        out[3 * v + 0] = float(ox);
        out[3 * v + 1] = float(oy);
        out[3 * v + 2] = float(oz);
    }
    return out;
}

double SkinMetrics::meshVolume(const float* positions, int vertexCount,
                               const std::uint32_t* indices,
                               std::size_t indexCount)
{
    if (!positions || !indices || indexCount < 3) return 0.0;
    double vol = 0.0;
    const std::size_t triCount = indexCount / 3;
    for (std::size_t t = 0; t < triCount; ++t) {
        const std::uint32_t i0 = indices[3 * t + 0];
        const std::uint32_t i1 = indices[3 * t + 1];
        const std::uint32_t i2 = indices[3 * t + 2];
        if (i0 >= std::uint32_t(vertexCount) || i1 >= std::uint32_t(vertexCount)
            || i2 >= std::uint32_t(vertexCount))
            continue;
        const float* a = positions + 3 * i0;
        const float* b = positions + 3 * i1;
        const float* c = positions + 3 * i2;
        // Signed volume of the origin tetrahedron: (a · (b × c)) / 6.
        const double crossX = double(b[1]) * c[2] - double(b[2]) * c[1];
        const double crossY = double(b[2]) * c[0] - double(b[0]) * c[2];
        const double crossZ = double(b[0]) * c[1] - double(b[1]) * c[0];
        vol += (a[0] * crossX + a[1] * crossY + a[2] * crossZ) / 6.0;
    }
    return std::abs(vol);
}
