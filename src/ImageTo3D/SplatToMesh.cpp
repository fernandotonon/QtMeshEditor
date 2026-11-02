#include "SplatToMesh.h"

#include "MarchingCubes.h"

#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <cstring>
#include <unordered_map>

namespace SplatToMesh {

namespace {

// Dense binary volume, one byte per voxel, x fastest then y then z.
struct Grid {
    int n = 0;
    size_t idx(int x, int y, int z) const
    {
        return (static_cast<size_t>(z) * n + y) * n + x;
    }
    size_t size() const { return static_cast<size_t>(n) * n * n; }
};

// One 6-connected dilation step: dst = src grown by one voxel.
void dilate6(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, const Grid& g)
{
    const size_t sy = size_t(g.n), sz = size_t(g.n) * g.n;
    for (int z = 0; z < g.n; ++z)
        for (int y = 0; y < g.n; ++y) {
            const size_t row = g.idx(0, y, z);
            for (int x = 0; x < g.n; ++x) {
                const size_t i = row + x;
                dst[i] = src[i]
                    || (x > 0 && src[i - 1]) || (x + 1 < g.n && src[i + 1])
                    || (y > 0 && src[i - sy]) || (y + 1 < g.n && src[i + sy])
                    || (z > 0 && src[i - sz]) || (z + 1 < g.n && src[i + sz]);
            }
        }
}

// One 6-connected erosion step; voxels outside the grid count as empty.
void erode6(const std::vector<uint8_t>& src, std::vector<uint8_t>& dst, const Grid& g)
{
    const size_t sy = size_t(g.n), sz = size_t(g.n) * g.n;
    for (int z = 0; z < g.n; ++z)
        for (int y = 0; y < g.n; ++y) {
            const size_t row = g.idx(0, y, z);
            for (int x = 0; x < g.n; ++x) {
                const size_t i = row + x;
                dst[i] = src[i]
                    && x > 0 && src[i - 1] && x + 1 < g.n && src[i + 1]
                    && y > 0 && src[i - sy] && y + 1 < g.n && src[i + sy]
                    && z > 0 && src[i - sz] && z + 1 < g.n && src[i + sz];
            }
        }
}

// Mark (ext = 1) every empty voxel 6-connected to the grid border, without
// a work stack: alternating forward/backward sweeps propagate the label
// along monotone paths until nothing changes. A DFS stack can grow to a
// large fraction of the grid (hundreds of millions of entries at 1024^3);
// the sweeps need no memory, and a typical shape converges in a handful of
// passes (each pass pair handles any path that turns back once per axis).
void exteriorSweep(const std::vector<uint8_t>& wall, std::vector<uint8_t>& ext, const Grid& g)
{
    const int n = g.n;
    const size_t sy = size_t(n), sz = size_t(n) * n;
    std::fill(ext.begin(), ext.end(), 0);
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if ((x == 0 || y == 0 || z == 0 || x == n - 1 || y == n - 1 || z == n - 1)
                    && !wall[g.idx(x, y, z)])
                    ext[g.idx(x, y, z)] = 1;
    bool changed = true;
    for (int pass = 0; changed && pass < 4096; ++pass) {
        changed = false;
        const bool fwd = (pass % 2) == 0;
        for (int zi = 0; zi < n; ++zi) {
            const int z = fwd ? zi : n - 1 - zi;
            for (int yi = 0; yi < n; ++yi) {
                const int y = fwd ? yi : n - 1 - yi;
                for (int xi = 0; xi < n; ++xi) {
                    const int x = fwd ? xi : n - 1 - xi;
                    const size_t i = g.idx(x, y, z);
                    if (ext[i] || wall[i])
                        continue;
                    if ((x > 0 && ext[i - 1]) || (x + 1 < n && ext[i + 1])
                        || (y > 0 && ext[i - sy]) || (y + 1 < n && ext[i + sy])
                        || (z > 0 && ext[i - sz]) || (z + 1 < n && ext[i + sz])) {
                        ext[i] = 1;
                        changed = true;
                    }
                }
            }
        }
    }
}

std::vector<float> gaussianKernel(float sigma, int& radius)
{
    radius = std::max(1, static_cast<int>(4.0f * sigma + 0.5f));
    std::vector<float> k(2 * radius + 1);
    float sum = 0.0f;
    for (int i = -radius; i <= radius; ++i)
        sum += (k[i + radius] = std::exp(-0.5f * float(i * i) / (sigma * sigma)));
    for (float& v : k)
        v /= sum;
    return k;
}

int reflectIndex(int i, int n)
{
    while (i < 0 || i >= n)
        i = (i < 0) ? -i - 1 : 2 * n - i - 1;
    return i;
}

double signedVolume(const std::vector<float>& p, const std::vector<uint32_t>& t)
{
    double v = 0.0;
    for (size_t i = 0; i + 2 < t.size(); i += 3) {
        const float* a = &p[3 * t[i]];
        const float* b = &p[3 * t[i + 1]];
        const float* c = &p[3 * t[i + 2]];
        v += a[0] * (double(b[1]) * c[2] - double(b[2]) * c[1])
           - a[1] * (double(b[0]) * c[2] - double(b[2]) * c[0])
           + a[2] * (double(b[0]) * c[1] - double(b[1]) * c[0]);
    }
    return v / 6.0;
}

constexpr int kSlab = 64;   // z slices per slab (density and blur+MC passes)

} // namespace

Result extract(const Splats& s, const Options& opts)
{
    Result out;
    const size_t n = s.count();
    if (n == 0 || s.xyz.size() < n * 3 || s.scale.size() < n * 3) {
        out.error = QStringLiteral("SplatToMesh: no splats.");
        return out;
    }
    const int R = std::clamp(opts.resolution, 16, 1024);
    auto cancelled = [&](int done, int total) {
        if (opts.progress && !opts.progress(done, total)) {
            out.cancelled = true;
            out.error = QStringLiteral("cancelled");
            return true;
        }
        return false;
    };

    // ---- splats in grid coordinates, sorted by z -----------------------------
    std::vector<uint32_t> keep;
    keep.reserve(n);
    float lo[3] = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                   std::numeric_limits<float>::max()};
    float hi[3] = {-lo[0], -lo[0], -lo[0]};
    for (size_t i = 0; i < n; ++i) {
        if (!(s.opacity[i] > opts.minOpacity))
            continue;
        const float* p = &s.xyz[3 * i];
        if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]))
            continue;
        keep.push_back(static_cast<uint32_t>(i));
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::min(lo[a], p[a]);
            hi[a] = std::max(hi[a], p[a]);
        }
    }
    if (keep.empty()) {
        out.error = QStringLiteral("SplatToMesh: every splat is transparent.");
        return out;
    }
    const float ext = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], 1e-4f}) * 1.06f;
    const float vox = ext / float(R);
    float org[3];
    for (int a = 0; a < 3; ++a)
        org[a] = 0.5f * (lo[a] + hi[a]) - 0.5f * ext;

    // The kernel is set in VOXELS so a finer grid keeps finer detail. Pinholes
    // in the splat shell are closed by a morphological closing whose size is
    // kept constant in WORLD units (1 step per 256 voxels): measured on the
    // goblin, a 512^3 grid with a 1-step closing leaked the exterior fill into
    // the pinholes (dark pits all over the mesh); 2 steps removed them, and
    // inflating the kernel instead also removed them but blurred 512^3 back
    // to 256^3 detail.
    const float worldScale = float(R) / 256.0f;
    const float minRadius = opts.minRadiusVoxels;
    const float maxRadius = std::max(opts.maxRadiusVoxels * worldScale, minRadius);
    const int closeSteps = opts.closeSteps > 0
        ? opts.closeSteps : std::max(1, int(std::lround(worldScale)));

    struct GS { float p[3]; float r; float op; };
    std::vector<GS> gs(keep.size());
    for (size_t k = 0; k < keep.size(); ++k) {
        const uint32_t i = keep[k];
        const float* sc = &s.scale[3 * i];
        for (int a = 0; a < 3; ++a)
            gs[k].p[a] = (s.xyz[3 * i + a] - org[a]) / vox;
        gs[k].r = std::clamp((sc[0] + sc[1] + sc[2]) / (3.0f * vox), minRadius, maxRadius);
        gs[k].op = s.opacity[i];
    }
    std::sort(gs.begin(), gs.end(), [](const GS& a, const GS& b) { return a.p[2] < b.p[2]; });
    const int maxReach = int(std::ceil(2.5f * maxRadius));

    // ---- density -> shell mask, one z-slab at a time --------------------------
    Grid g{R};
    std::vector<uint8_t> shell(g.size(), 0);
    {
        const size_t plane = size_t(R) * R;
        std::vector<float> dens;
        for (int z0 = 0; z0 < R; z0 += kSlab) {
            const int z1 = std::min(R, z0 + kSlab);
            if (cancelled(z0, 2 * R))
                return out;
            dens.assign(plane * size_t(z1 - z0), 0.0f);
            auto first = std::lower_bound(gs.begin(), gs.end(), float(z0 - maxReach - 1),
                [](const GS& a, float z) { return a.p[2] < z; });
            for (auto it = first; it != gs.end() && it->p[2] < float(z1 + maxReach + 1); ++it) {
                const int reach = int(std::ceil(2.5f * it->r));
                const float inv2r2 = 0.5f / (it->r * it->r);
                const int bx = int(std::floor(it->p[0]));
                const int by = int(std::floor(it->p[1]));
                const int bz = int(std::floor(it->p[2]));
                for (int z = std::max(z0, bz - reach); z <= std::min(z1 - 1, bz + reach); ++z)
                    for (int y = std::max(0, by - reach); y <= std::min(R - 1, by + reach); ++y)
                        for (int x = std::max(0, bx - reach); x <= std::min(R - 1, bx + reach); ++x) {
                            const float ex = x + 0.5f - it->p[0];
                            const float ey = y + 0.5f - it->p[1];
                            const float ez = z + 0.5f - it->p[2];
                            const float w = it->op
                                * std::exp(-(ex * ex + ey * ey + ez * ez) * inv2r2);
                            if (w > 1e-3f)
                                dens[size_t(z - z0) * plane + size_t(y) * R + x] += w;
                        }
            }
            uint8_t* dst = shell.data() + size_t(z0) * plane;
            for (size_t i = 0; i < dens.size(); ++i)
                dst[i] = dens[i] > opts.shellThreshold ? 1 : 0;
        }
    }
    std::vector<GS>().swap(gs);

    // ---- closing + exterior fill -> solid -------------------------------------
    std::vector<uint8_t> a(shell), b(g.size());
    for (int k = 0; k < closeSteps; ++k) {
        dilate6(a, b, g);
        a.swap(b);
    }
    exteriorSweep(a, b, g);              // b = exterior
    for (size_t i = 0; i < b.size(); ++i)
        a[i] = b[i] ? 0 : 1;             // a = solid (closed)
    for (int k = 0; k < closeSteps; ++k) {
        erode6(a, b, g);
        a.swap(b);
    }
    std::vector<uint8_t>().swap(b);
    for (size_t i = 0; i < a.size(); ++i)
        a[i] = (a[i] || shell[i]) ? 1 : 0; // keep thin parts the erosion ate
    std::vector<uint8_t>().swap(shell);
    const std::vector<uint8_t>& solid = a;

    // ---- blur + marching cubes, one z-slab at a time -----------------------------
    // Positions come out in INDEX space (corner (x,y,z) at (x,y,z)), so the
    // plane two slabs share is computed bit-identically on both sides and its
    // vertices weld exactly; they are mapped to world space at the end.
    int br = 0;
    const std::vector<float> kern = gaussianKernel(std::max(opts.blurSigma, 1e-3f), br);
    const bool doBlur = opts.blurSigma > 0.0f;
    std::vector<float> positions;
    std::vector<uint32_t> indices;
    std::unordered_map<uint64_t, uint32_t> seam;   // shared-plane vertex -> index
    const size_t plane = size_t(R) * R;
    std::vector<float> f, tmp;
    for (int z0 = 0; z0 < R - 1; z0 += kSlab) {
        if (cancelled(R + z0, 2 * R))
            return out;
        const int z1 = std::min(R - 1, z0 + kSlab);   // slices z0..z1 inclusive
        const int h = doBlur ? br : 0;
        const int lo = z0 - h, hi = z1 + h;           // with blur halo
        const int nz = hi - lo + 1;
        f.assign(plane * size_t(nz), 0.0f);
        for (int z = lo; z <= hi; ++z) {
            const int sz = reflectIndex(z, R);
            const uint8_t* src = solid.data() + size_t(sz) * plane;
            float* dst = f.data() + size_t(z - lo) * plane;
            for (size_t i = 0; i < plane; ++i)
                dst[i] = src[i] ? 1.0f : 0.0f;
        }
        if (doBlur) {
            tmp.resize(f.size());
            for (int zz = 0; zz < nz; ++zz)              // x
                for (int y = 0; y < R; ++y) {
                    const float* row = f.data() + size_t(zz) * plane + size_t(y) * R;
                    float* o = tmp.data() + size_t(zz) * plane + size_t(y) * R;
                    for (int x = 0; x < R; ++x) {
                        float acc = 0.0f;
                        for (int k = -br; k <= br; ++k)
                            acc += kern[k + br] * row[reflectIndex(x + k, R)];
                        o[x] = acc;
                    }
                }
            for (int zz = 0; zz < nz; ++zz)              // y
                for (int y = 0; y < R; ++y) {
                    float* o = f.data() + size_t(zz) * plane + size_t(y) * R;
                    for (int x = 0; x < R; ++x) {
                        float acc = 0.0f;
                        for (int k = -br; k <= br; ++k)
                            acc += kern[k + br]
                                * tmp[size_t(zz) * plane + size_t(reflectIndex(y + k, R)) * R + x];
                        o[x] = acc;
                    }
                }
            // z: only the slab's own slices are needed; the halo feeds them.
            // Out-of-grid neighbours were filled by reflection above.
            for (int zz = h; zz < nz - h; ++zz)
                for (size_t i = 0; i < plane; ++i) {
                    float acc = 0.0f;
                    for (int k = -br; k <= br; ++k)
                        acc += kern[k + br] * f[size_t(zz + k) * plane + i];
                    tmp[size_t(zz) * plane + i] = acc;
                }
            f.swap(tmp);
        }
        const float* field = f.data() + size_t(h) * plane;
        const int slices = z1 - z0 + 1;
        MarchingCubes::Mesh m = MarchingCubes::extract(
            field, R, R, slices, 0.5f, {0.0f, 0.0f, float(z0)},
            {float(R - 1), float(R - 1), float(z1)}, plane * size_t(slices));
        std::vector<uint32_t> remap(size_t(m.vertexCount));
        for (int v = 0; v < m.vertexCount; ++v) {
            const float* p = &m.positions[3 * size_t(v)];
            const bool onSeam = p[2] == float(z0) || p[2] == float(z1);
            if (onSeam) {
                // Exact key on a 1/1024-voxel lattice. Positions on the
                // shared plane are NOT always bit-identical between the two
                // slabs (the cube on each side may interpolate an in-plane
                // edge from opposite ends), so exact-bit matching left open
                // edges along the seam — measured by the sphere test.
                const int64_t qx = std::llround(double(p[0]) * 1024.0);
                const int64_t qy = std::llround(double(p[1]) * 1024.0);
                const uint64_t key = (uint64_t(qx) << 32) ^ (uint64_t(qy) << 11)
                                   ^ uint64_t(int64_t(p[2]));
                auto it = seam.find(key);
                if (it != seam.end()) {
                    remap[size_t(v)] = it->second;
                    continue;
                }
                seam.emplace(key, uint32_t(positions.size() / 3));
            }
            remap[size_t(v)] = uint32_t(positions.size() / 3);
            positions.insert(positions.end(), p, p + 3);
        }
        for (uint32_t idx : m.indices)
            indices.push_back(remap[idx]);
    }
    if (indices.empty()) {
        out.error = QStringLiteral("SplatToMesh: no surface found.");
        return out;
    }
    // Index space -> world: voxel centre (i + 0.5) * vox + org.
    for (size_t i = 0; i < positions.size(); i += 3)
        for (int ax = 0; ax < 3; ++ax)
            positions[i + ax] = org[ax] + (positions[i + ax] + 0.5f) * vox;
    // Orient by signed volume rather than assuming the MC winding convention.
    if (signedVolume(positions, indices) < 0.0)
        for (size_t t = 0; t + 2 < indices.size(); t += 3)
            std::swap(indices[t + 1], indices[t + 2]);

    out.positions = std::move(positions);
    out.indices = std::move(indices);
    out.voxelSize = vox;
    out.splatsUsed = static_cast<int>(keep.size());
    out.ok = true;
    return out;
}


void toEditorFrame(std::vector<float>& xyz)
{
    for (size_t i = 0; i + 2 < xyz.size(); i += 3) {
        const float x = xyz[i], y = xyz[i + 1], z = xyz[i + 2];
        xyz[i] = y;
        xyz[i + 1] = z;
        xyz[i + 2] = x;
    }
}

// ---- ColorField --------------------------------------------------------------

ColorField::ColorField(const std::vector<float>& positions, const std::vector<float>& rgb,
                       const std::vector<float>& opacity, float cellSize, float minOpacity)
    : m_cell(std::max(cellSize, 1e-6f))
{
    const size_t n = opacity.size();
    m_min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
             std::numeric_limits<float>::max()};
    for (size_t i = 0; i < n; ++i) {
        if (!(opacity[i] > minOpacity))
            continue;
        m_pos.insert(m_pos.end(), &positions[3 * i], &positions[3 * i] + 3);
        m_rgb.insert(m_rgb.end(), &rgb[3 * i], &rgb[3 * i] + 3);
        m_op.push_back(opacity[i]);
        for (int a = 0; a < 3; ++a)
            m_min[a] = std::min(m_min[a], positions[3 * i + a]);
    }
    std::vector<long long> keys(m_op.size());
    for (size_t i = 0; i < m_op.size(); ++i)
        keys[i] = key(static_cast<int>(std::floor((m_pos[3 * i] - m_min[0]) / m_cell)),
                      static_cast<int>(std::floor((m_pos[3 * i + 1] - m_min[1]) / m_cell)),
                      static_cast<int>(std::floor((m_pos[3 * i + 2] - m_min[2]) / m_cell)));
    m_order.resize(m_op.size());
    std::iota(m_order.begin(), m_order.end(), 0u);
    std::sort(m_order.begin(), m_order.end(),
              [&](uint32_t a, uint32_t b) { return keys[a] < keys[b]; });
    m_keys.resize(m_order.size());
    for (size_t i = 0; i < m_order.size(); ++i)
        m_keys[i] = keys[m_order[i]];
}

long long ColorField::key(int x, int y, int z) const
{
    // 21 bits per axis (offset so small negatives from query points work).
    constexpr long long kOff = 1 << 20;
    return ((static_cast<long long>(x) + kOff) << 42)
         | ((static_cast<long long>(y) + kOff) << 21)
         | (static_cast<long long>(z) + kOff);
}

void ColorField::sample(const float* points, size_t count, float* rgb) const
{
    constexpr int kNearest = 8;
    struct Cand { float d2; uint32_t id; };
    std::vector<Cand> cand;
    for (size_t q = 0; q < count; ++q) {
        const float* p = points + 3 * q;
        const int c[3] = {static_cast<int>(std::floor((p[0] - m_min[0]) / m_cell)),
                          static_cast<int>(std::floor((p[1] - m_min[1]) / m_cell)),
                          static_cast<int>(std::floor((p[2] - m_min[2]) / m_cell))};
        cand.clear();
        for (int ring = 1; ring <= 4 && cand.size() < static_cast<size_t>(kNearest); ++ring) {
            cand.clear();
            for (int dz = -ring; dz <= ring; ++dz)
                for (int dy = -ring; dy <= ring; ++dy)
                    for (int dx = -ring; dx <= ring; ++dx) {
                        const long long k = key(c[0] + dx, c[1] + dy, c[2] + dz);
                        auto range = std::equal_range(m_keys.begin(), m_keys.end(), k);
                        for (auto it = range.first; it != range.second; ++it) {
                            const uint32_t id = m_order[static_cast<size_t>(it - m_keys.begin())];
                            const float ex = m_pos[3 * id] - p[0];
                            const float ey = m_pos[3 * id + 1] - p[1];
                            const float ez = m_pos[3 * id + 2] - p[2];
                            cand.push_back({ex * ex + ey * ey + ez * ez, id});
                        }
                    }
        }
        float* o = rgb + 3 * q;
        if (cand.empty()) {
            o[0] = o[1] = o[2] = 0.5f;
            continue;
        }
        const size_t k = std::min(cand.size(), static_cast<size_t>(kNearest));
        std::partial_sort(cand.begin(), cand.begin() + k, cand.end(),
                          [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        double acc[3] = {0, 0, 0}, wsum = 0;
        for (size_t i = 0; i < k; ++i) {
            const uint32_t id = cand[i].id;
            const double w = m_op[id] / (std::sqrt(double(cand[i].d2)) + 1e-4);
            for (int a = 0; a < 3; ++a)
                acc[a] += w * m_rgb[3 * id + a];
            wsum += w;
        }
        for (int a = 0; a < 3; ++a)
            o[a] = static_cast<float>(std::clamp(acc[a] / wsum, 0.0, 1.0));
    }
}

// ---- .ply writer ---------------------------------------------------------------

namespace {

void quatToMat(const float* q, double m[9])
{
    double w = q[0], x = q[1], y = q[2], z = q[3];
    const double len = std::sqrt(w * w + x * x + y * y + z * z);
    if (len < 1e-12) { w = 1; x = y = z = 0; }
    else { w /= len; x /= len; y /= len; z /= len; }
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y - w * z);     m[2] = 2 * (x * z + w * y);
    m[3] = 2 * (x * y + w * z);     m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z - w * x);
    m[6] = 2 * (x * z - w * y);     m[7] = 2 * (y * z + w * x);     m[8] = 1 - 2 * (x * x + y * y);
}

void matToQuat(const double m[9], float q[4])
{
    const double tr = m[0] + m[4] + m[8];
    double w, x, y, z;
    if (tr > 0) {
        const double s = std::sqrt(tr + 1.0) * 2;
        w = 0.25 * s; x = (m[7] - m[5]) / s; y = (m[2] - m[6]) / s; z = (m[3] - m[1]) / s;
    } else if (m[0] > m[4] && m[0] > m[8]) {
        const double s = std::sqrt(1.0 + m[0] - m[4] - m[8]) * 2;
        w = (m[7] - m[5]) / s; x = 0.25 * s; y = (m[1] + m[3]) / s; z = (m[2] + m[6]) / s;
    } else if (m[4] > m[8]) {
        const double s = std::sqrt(1.0 + m[4] - m[0] - m[8]) * 2;
        w = (m[2] - m[6]) / s; x = (m[1] + m[3]) / s; y = 0.25 * s; z = (m[5] + m[7]) / s;
    } else {
        const double s = std::sqrt(1.0 + m[8] - m[0] - m[4]) * 2;
        w = (m[3] - m[1]) / s; x = (m[2] + m[6]) / s; y = (m[5] + m[7]) / s; z = 0.25 * s;
    }
    const double len = std::sqrt(w * w + x * x + y * y + z * z);
    q[0] = float(w / len); q[1] = float(x / len); q[2] = float(y / len); q[3] = float(z / len);
}

} // namespace

bool writePly(const QString& path, const Splats& s, QString* error)
{
    const size_t n = s.count();
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    QByteArray header = "ply\nformat binary_little_endian 1.0\nelement vertex ";
    header += QByteArray::number(qulonglong(n));
    header += "\n";
    for (const char* a : {"x", "y", "z", "nx", "ny", "nz", "f_dc_0", "f_dc_1", "f_dc_2",
                          "opacity", "scale_0", "scale_1", "scale_2",
                          "rot_0", "rot_1", "rot_2", "rot_3"}) {
        header += "property float ";
        header += a;
        header += "\n";
    }
    header += "end_header\n";
    f.write(header);

    // Upstream's export transform T = [[1,0,0],[0,0,-1],[0,1,0]] (model
    // Z-up -> the 3DGS y-down viewer convention).
    static const double T[9] = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    std::vector<float> rec(17);
    QByteArray buf;
    buf.reserve(static_cast<int>(std::min<size_t>(n, 65536) * 17 * sizeof(float)));
    for (size_t i = 0; i < n; ++i) {
        const float* p = &s.xyz[3 * i];
        rec[0] = float(T[0] * p[0] + T[1] * p[1] + T[2] * p[2]);
        rec[1] = float(T[3] * p[0] + T[4] * p[1] + T[5] * p[2]);
        rec[2] = float(T[6] * p[0] + T[7] * p[1] + T[8] * p[2]);
        rec[3] = rec[4] = rec[5] = 0.0f;
        for (int a = 0; a < 3; ++a)
            rec[6 + a] = s.fdc[3 * i + a];
        const float o = std::clamp(s.opacity[i], 1e-6f, 1.0f - 1e-6f);
        rec[9] = std::log(o / (1.0f - o));
        for (int a = 0; a < 3; ++a)
            rec[10 + a] = std::log(std::max(s.scale[3 * i + a], 1e-8f));
        double r[9], tr[9];
        quatToMat(&s.rotation[4 * i], r);
        for (int row = 0; row < 3; ++row)
            for (int col = 0; col < 3; ++col)
                tr[row * 3 + col] = T[row * 3] * r[col] + T[row * 3 + 1] * r[3 + col]
                                  + T[row * 3 + 2] * r[6 + col];
        matToQuat(tr, &rec[13]);
        buf.append(reinterpret_cast<const char*>(rec.data()),
                   static_cast<int>(rec.size() * sizeof(float)));
        if (buf.size() > (1 << 22)) {
            f.write(buf);
            buf.clear();
        }
    }
    f.write(buf);
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

Splats readPly(const QString& path, QString* error)
{
    auto failed = [&](const QString& m) {
        if (error) *error = m;
        return Splats{};
    };
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return failed(f.errorString());
    // Header: only binary little-endian float properties are supported.
    QStringList props;
    qint64 count = -1;
    while (true) {
        const QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() && f.atEnd())
            return failed(QStringLiteral("truncated .ply header"));
        if (line == "end_header")
            break;
        const QList<QByteArray> tok = line.split(' ');
        if (tok.size() >= 3 && tok[0] == "format" && tok[1] != "binary_little_endian")
            return failed(QStringLiteral("only binary_little_endian .ply is supported"));
        if (tok.size() >= 3 && tok[0] == "element" && tok[1] == "vertex")
            count = tok[2].toLongLong();
        if (tok.size() >= 3 && tok[0] == "property") {
            if (tok[1] != "float" && tok[1] != "float32")
                return failed(QStringLiteral("non-float .ply property"));
            props << QString::fromLatin1(tok[2]);
        }
    }
    const int stride = int(props.size());
    auto col = [&](const char* name) { return int(props.indexOf(QLatin1String(name))); };
    const int ix = col("x"), iy = col("y"), iz = col("z"), iop = col("opacity");
    const int idc = col("f_dc_0"), isc = col("scale_0"), irot = col("rot_0");
    if (count <= 0 || ix < 0 || iy < 0 || iz < 0 || iop < 0 || idc < 0 || isc < 0 || irot < 0)
        return failed(QStringLiteral("not a 3D Gaussian .ply"));
    const QByteArray data = f.read(count * stride * qint64(sizeof(float)));
    if (data.size() != count * stride * qint64(sizeof(float)))
        return failed(QStringLiteral("truncated .ply data"));
    const float* d = reinterpret_cast<const float*>(data.constData());

    // Inverse of writePly's T (orthonormal: inverse = transpose).
    static const double T[9] = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    Splats s;
    const size_t n = size_t(count);
    s.xyz.resize(n * 3); s.fdc.resize(n * 3); s.opacity.resize(n);
    s.scale.resize(n * 3); s.rotation.resize(n * 4);
    for (size_t i = 0; i < n; ++i) {
        const float* r = d + i * stride;
        const double p[3] = {r[ix], r[iy], r[iz]};
        for (int a = 0; a < 3; ++a)
            s.xyz[3 * i + a] = float(T[a] * p[0] + T[3 + a] * p[1] + T[6 + a] * p[2]);
        for (int a = 0; a < 3; ++a) {
            s.fdc[3 * i + a] = r[idc + a];
            s.scale[3 * i + a] = std::exp(r[isc + a]);
        }
        s.opacity[i] = 1.0f / (1.0f + std::exp(-r[iop]));
        double m[9], tm[9];
        quatToMat(r + irot, m);
        for (int row = 0; row < 3; ++row)
            for (int c = 0; c < 3; ++c)
                tm[row * 3 + c] = T[row] * m[c] + T[3 + row] * m[3 + c] + T[6 + row] * m[6 + c];
        matToQuat(tm, &s.rotation[4 * i]);
    }
    return s;
}

} // namespace SplatToMesh
