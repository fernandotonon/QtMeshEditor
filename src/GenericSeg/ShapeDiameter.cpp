#include "ShapeDiameter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace GenericSeg {

namespace {

// Deterministic directions in a cone around +Z (golden-angle spiral), with
// the cosine of each direction's angle to the axis.
struct ConeDir { Vec3 d; float cosAngle; };

std::vector<ConeDir> coneDirections(int count, float coneDegrees)
{
    std::vector<ConeDir> dirs;
    const float halfAngle = 0.5f * coneDegrees * 3.14159265f / 180.0f;
    const float cosMax = std::cos(halfAngle);
    const float golden = 3.14159265f * (3.0f - std::sqrt(5.0f));
    for (int i = 0; i < count; ++i) {
        // Uniform over the spherical cap: cos(theta) uniform in [cosMax, 1].
        const float ct = 1.0f - (1.0f - cosMax) * (i + 0.5f) / count;
        const float st = std::sqrt(std::max(0.0f, 1.0f - ct * ct));
        const float phi = golden * i;
        dirs.push_back({Vec3(std::cos(phi) * st, std::sin(phi) * st, ct), ct});
    }
    return dirs;
}

float faceSdf(const SegMesh& mesh, const TriangleBvh& bvh, int f, const std::vector<ConeDir>& cone)
{
    const Vec3 n = mesh.faceNormals[f];
    if (length(n) < 0.5f) return -1.0f;
    const Vec3 axis = n * -1.0f;
    // Frame around the inward normal.
    const Vec3 t = normalized(std::fabs(axis.x) < 0.9f ? cross(axis, Vec3(1, 0, 0)) : cross(axis, Vec3(0, 1, 0)));
    const Vec3 b = cross(axis, t);
    const Vec3 origin = (mesh.positions[mesh.indices[3 * f]] + mesh.positions[mesh.indices[3 * f + 1]]
                         + mesh.positions[mesh.indices[3 * f + 2]]) * (1.0f / 3.0f);

    std::vector<std::pair<float, float>> hits;   // (distance, weight)
    hits.reserve(cone.size());
    for (const ConeDir& c : cone) {
        const Vec3 d = normalized(t * c.d.x + b * c.d.y + axis * c.d.z);
        TriangleBvh::Hit h;
        if (!bvh.intersect(origin, d, 1e-5f, 4.0f, h, f))
            continue;
        // Keep hits on the inside of an opposite wall: its outward normal
        // points along the ray. Hitting a wall from outside means the ray
        // left the volume through a hole or a crease.
        if (dot(bvh.faceNormal(h.face), d) <= 0.0f)
            continue;
        hits.push_back({h.t, c.cosAngle});
    }
    if (hits.empty()) return -1.0f;
    // Outlier rejection: keep values within one standard deviation of the median.
    std::vector<float> ds;
    for (const auto& h : hits) ds.push_back(h.first);
    std::nth_element(ds.begin(), ds.begin() + ds.size() / 2, ds.end());
    const float median = ds[ds.size() / 2];
    float mean = 0, var = 0;
    for (const auto& h : hits) mean += h.first;
    mean /= hits.size();
    for (const auto& h : hits) var += (h.first - mean) * (h.first - mean);
    const float sd = std::sqrt(var / hits.size());
    float sum = 0, wsum = 0;
    for (const auto& h : hits) {
        if (std::fabs(h.first - median) > sd + 1e-9f) continue;
        sum += h.first * h.second;
        wsum += h.second;
    }
    return wsum > 0 ? sum / wsum : median;
}

} // namespace

std::vector<float> shapeDiameter(const SegMesh& mesh, const TriangleBvh& bvh, const SdfOptions& opts)
{
    const int faceCount = mesh.faceCount();
    std::vector<float> sdf(faceCount, -1.0f);
    if (faceCount == 0) return sdf;
    const std::vector<ConeDir> cone = coneDirections(std::max(1, opts.rays), opts.coneDegrees);

    int threads = opts.threads > 0 ? opts.threads : int(std::thread::hardware_concurrency());
    threads = std::clamp(threads, 1, 64);
    // Workers pull faces from a shared counter, and the calling thread works
    // too: if a thread cannot be started, the ones that did (or this thread
    // alone) finish every face, and each started thread is joined.
    std::atomic<int> next{0};
    auto work = [&] {
        for (int f = next.fetch_add(1); f < faceCount; f = next.fetch_add(1))
            sdf[f] = faceSdf(mesh, bvh, f, cone);
    };
    std::vector<std::thread> pool;
    try {
        pool.reserve(size_t(threads - 1));
        for (int w = 1; w < threads; ++w) pool.emplace_back(work);
    } catch (const std::exception&) {
        // Fewer workers; the work still completes below.
    }
    work();
    for (std::thread& th : pool) th.join();

    // Fill faces with no valid hit from their neighbours (several sweeps).
    bool anyValid = std::any_of(sdf.begin(), sdf.end(), [](float v) { return v >= 0; });
    if (!anyValid) return std::vector<float>(faceCount, 0.0f);
    for (int pass = 0; pass < 64; ++pass) {
        bool changed = false, missing = false;
        std::vector<float> next = sdf;
        for (int f = 0; f < faceCount; ++f) {
            if (sdf[f] >= 0) continue;
            float s = 0; int c = 0;
            for (int g : mesh.faceNeighbors[f]) if (sdf[g] >= 0) { s += sdf[g]; ++c; }
            if (c > 0) { next[f] = s / c; changed = true; } else missing = true;
        }
        sdf.swap(next);
        if (!missing || !changed) break;
    }
    float mean = 0; int c = 0;
    for (float v : sdf) if (v >= 0) { mean += v; ++c; }
    mean = c ? mean / c : 0.0f;
    for (float& v : sdf) if (v < 0) v = mean;   // isolated pieces with no hit

    // Smooth (area-weighted) over the face graph.
    for (int it = 0; it < opts.smoothIterations; ++it) {
        std::vector<float> next(faceCount);
        for (int f = 0; f < faceCount; ++f) {
            float s = sdf[f] * mesh.faceAreas[f], w = mesh.faceAreas[f];
            for (int g : mesh.faceNeighbors[f]) { s += sdf[g] * mesh.faceAreas[g]; w += mesh.faceAreas[g]; }
            next[f] = w > 0 ? s / w : sdf[f];
        }
        sdf.swap(next);
    }
    return sdf;
}

std::vector<float> normalizeSdf(const std::vector<float>& raw)
{
    std::vector<float> out(raw.size(), 0.0f);
    if (raw.empty()) return out;
    // Range from the 1st..99th percentile, not min..max: a few faces with an
    // extreme diameter (a closed pocket, a long ray through a car body) would
    // otherwise squash everything else into one colour.
    std::vector<float> sorted(raw);
    const size_t n = sorted.size();
    const size_t iLo = size_t(std::floor(0.01 * double(n - 1)));
    const size_t iHi = size_t(std::ceil(0.99 * double(n - 1)));
    std::nth_element(sorted.begin(), sorted.begin() + iLo, sorted.end());
    const float lo = sorted[iLo];
    std::nth_element(sorted.begin(), sorted.begin() + iHi, sorted.end());
    const float hi = sorted[iHi];
    const float range = hi - lo;
    if (!(range > 0)) return out;
    const float a = 4.0f;
    for (size_t i = 0; i < n; ++i) {
        const float v = std::clamp((raw[i] - lo) / range, 0.0f, 1.0f);
        out[i] = std::log(1.0f + a * v) / std::log(1.0f + a);
    }
    return out;
}

} // namespace GenericSeg
